// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The sign-in panel on Windows: a child window of the game's own, framed as
// the game's dialogs are, with a title band, a close button and a raised
// edge, and below the band a pane with the system's WebView2 browser.
// It lives on the game's thread, whose event loop delivers its messages and
// the browser's answers, so the thread must be a COM single-threaded
// apartment, as SDL's is. A navigation to the landing address is cancelled
// before it loads, and its address kept. A page's own window, such as the
// one a sign-in with Google or another account opens, is a second pane in
// the first one's place with a browser of its own that the page can still
// talk to; the first is hidden until the second closes.

#include "oa/platform/web_sign_in.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <knownfolders.h>
#include <objbase.h>
#include <shlobj.h>

#include <WebView2.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace oa::platform::web_sign_in {

namespace {

/// The window class of the framed panel, and of each browser's pane in it.
constexpr wchar_t panel_class[] = L"OpenAnnihilationSignIn";
constexpr wchar_t pane_class[] = L"OpenAnnihilationSignInPane";
/// The browser's size at 96 dots per inch, before the game's window limits
/// the panel.
constexpr int window_width = 560;
constexpr int window_height = 720;
/// The room left between the panel and the edges of the game's window, at
/// 96 dots per inch.
constexpr int window_margin = 16;
/// The frame's measures at 96 dots per inch: the raised edge, the title
/// band, the room before the game's icon, the icon, the room between it and
/// the title, the title's letters and the close button's width.
constexpr int frame_edge = 1;
constexpr int band_height = 32;
constexpr int band_padding = 12;
constexpr int mark_side = 20;
constexpr int mark_gap = 8;
constexpr int title_height = 15;
constexpr int close_width = 44;
/// The close button's cross, a glyph of the system's icon font, and its size
/// at 96 dots per inch.
constexpr wchar_t close_glyph[] = L"\xE8BB";
constexpr wchar_t glyph_font[] = L"Segoe MDL2 Assets";
constexpr int glyph_height = 10;
/// The timer that fits the panel to the game's window as it changes size,
/// and how often it runs, in milliseconds.
constexpr UINT_PTR fit_timer = 1;
constexpr UINT fit_every_ms = 250;
/// Where the browser keeps its cookies, under the player's local
/// application data.
constexpr wchar_t profile_folder[] = L"\\Open Annihilation\\Sign-in browser";
/// The resource number of the game's icon (open-annihilation.rc.in).
constexpr int icon_resource = 1;

/// UTF-8 text as Windows' wide characters.
std::wstring wide(const std::string& text) {
    if (text.empty())
        return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0);
    std::wstring out(static_cast<std::size_t>(std::max(size, 0)), L'\0');
    if (size > 0)
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(),
                            size);
    return out;
}

/// Windows' wide characters as UTF-8.
std::string utf8(const wchar_t* text) {
    if (text == nullptr || *text == L'\0')
        return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1)
        return {};
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
    out.resize(static_cast<std::size_t>(size - 1));
    return out;
}

/// A 0xRRGGBB colour as Windows draws it.
COLORREF colour_of(uint32_t rgb) {
    return RGB((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
}

/// Fills a rectangle with a 0xRRGGBB colour.
void fill(HDC context, const RECT& area, uint32_t rgb) {
    HBRUSH brush = CreateSolidBrush(colour_of(rgb));
    FillRect(context, &area, brush);
    DeleteObject(brush);
}

/// A COM handler that calls a function; `Arguments` are its Invoke's.
template <class Interface, class... Arguments>
class Handler final : public Interface {
  public:
    explicit Handler(std::function<HRESULT(Arguments...)> call) : call_(std::move(call)) {}

    ULONG STDMETHODCALLTYPE AddRef() override { return ++count_; }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = --count_;
        if (left == 0)
            delete this;
        return left;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (out == nullptr)
            return E_POINTER;
        if (id == __uuidof(IUnknown) || id == __uuidof(Interface)) {
            *out = static_cast<Interface*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE Invoke(Arguments... arguments) override {
        return call_(arguments...);
    }

  private:
    ~Handler() = default;

    std::atomic<ULONG> count_{1};
    std::function<HRESULT(Arguments...)> call_;
};

/// Makes a handler; the caller passes it on and then releases it.
template <class Interface, class... Arguments>
Handler<Interface, Arguments...>* handler(std::function<HRESULT(Arguments...)> call) {
    return new Handler<Interface, Arguments...>(std::move(call));
}

/// The folder the browser keeps its files in; empty for the default one.
std::wstring profile_path() {
    PWSTR local = nullptr;
    std::wstring path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)) &&
        local != nullptr)
        path = std::wstring(local) + profile_folder;
    CoTaskMemFree(local);
    return path;
}

/// Forgets every cookie the browser holds, so no sign-in outlasts a panel.
void forget_cookies(ICoreWebView2* view) {
    ICoreWebView2_2* view_2 = nullptr;
    if (FAILED(view->QueryInterface(__uuidof(ICoreWebView2_2), reinterpret_cast<void**>(&view_2))) ||
        view_2 == nullptr)
        return;
    ICoreWebView2CookieManager* cookies = nullptr;
    if (SUCCEEDED(view_2->get_CookieManager(&cookies)) && cookies != nullptr) {
        cookies->DeleteAllCookies();
        cookies->Release();
    }
    view_2->Release();
}

struct Session;

/// One browser and the pane it fills.
struct Pane {
    HWND window{};
    ICoreWebView2Controller* controller{};
    ICoreWebView2* view{};
    /// The sign-in this pane belongs to.
    Session* owner{};
    /// True for a page's own window, whose end leaves the sign-in open.
    bool page_window{};
};

/// Lets go of a pane's browser.
void release(Pane& pane) {
    if (pane.view != nullptr) {
        pane.view->Release();
        pane.view = nullptr;
    }
    if (pane.controller != nullptr) {
        pane.controller->Close();
        pane.controller->Release();
        pane.controller = nullptr;
    }
}

/// Fits the browser to its pane.
void fit_browser(const Pane& pane) {
    if (pane.controller == nullptr || pane.window == nullptr)
        return;
    RECT bounds{};
    GetClientRect(pane.window, &bounds);
    pane.controller->put_Bounds(bounds);
}

/// A measure at 96 dots per inch, scaled to `dpi`.
int scaled(int measure, UINT dpi) {
    return MulDiv(measure, static_cast<int>(dpi), 96);
}

/// The panel's rectangle in the client area of `parent`: the browser's size
/// and the frame's, scaled to the window's dots per inch, within the client
/// area less a margin, and centred on it.
RECT placement(HWND parent, DWORD style) {
    RECT client{};
    GetClientRect(parent, &client);
    const UINT dpi = std::max(GetDpiForWindow(parent), 96U);
    RECT outer{0, 0, scaled(window_width + 2 * frame_edge, dpi),
               scaled(window_height + band_height + 1 + 2 * frame_edge, dpi)};
    AdjustWindowRectExForDpi(&outer, style, FALSE, 0, dpi);
    const int margin = scaled(window_margin, dpi);
    const int room_width = std::max<int>(client.right - 2 * margin, 1);
    const int room_height = std::max<int>(client.bottom - 2 * margin, 1);
    const int width = std::min<int>(outer.right - outer.left, room_width);
    const int height = std::min<int>(outer.bottom - outer.top, room_height);
    const int left = (client.right - width) / 2;
    const int top = (client.bottom - height) / 2;
    return RECT{left, top, left + width, top + height};
}

LRESULT CALLBACK panel_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
LRESULT CALLBACK pane_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam);

/// Registers the window classes once.
bool register_classes(HINSTANCE instance) {
    static const bool registered = [instance] {
        auto add = [instance](const wchar_t* name, WNDPROC procedure) {
            WNDCLASSEXW type{};
            type.cbSize = sizeof(type);
            type.lpfnWndProc = procedure;
            type.hInstance = instance;
            type.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(icon_resource));
            type.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
            type.lpszClassName = name;
            return RegisterClassExW(&type) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
        };
        return add(panel_class, panel_procedure) && add(pane_class, pane_procedure);
    }();
    return registered;
}

/// One sign-in: its panel, its panes and their browsers, and where it is.
struct Session {
    State state{State::idle};
    std::string landed{};
    /// The game's window the panel is inside; none for a window of its own.
    HWND parent{};
    /// The framed panel the panes are in.
    HWND panel{};
    /// The parent's client size the panel was last fitted to.
    SIZE fitted{};
    /// The panel's style: a child's within a parent, else a window's own.
    DWORD style{};
    std::wstring title{};
    std::wstring landing{};
    Look look{};
    /// The fonts of the title and of the close button's cross, and the dots
    /// per inch they were made for.
    HFONT title_font{};
    HFONT glyph{};
    UINT dpi{};
    /// The close button is under the pointer, or a press on it is held.
    bool close_hovered{};
    bool close_held{};
    /// Counts the sign-ins; an answer from the browser for an earlier one is
    /// let go.
    unsigned session{};
    /// True when open initialised COM on this thread, so close ends it.
    bool com{};
    Pane pane{};
    /// The page's own windows.
    std::vector<std::unique_ptr<Pane>> page_windows{};
    ICoreWebView2Environment* browser{};
    ICoreWebView2NavigationStartingEventHandler* starting{};
    ICoreWebView2NewWindowRequestedEventHandler* new_window{};
    ICoreWebView2AcceleratorKeyPressedEventHandler* key{};

    /// Moves to `next`, unless the sign-in has already ended.
    void settle(State next) {
        if (state == State::landed || state == State::failed || state == State::closed)
            return;
        state = next;
    }

    /// The pane showing: the last page's own window, else the first pane.
    Pane& showing() { return page_windows.empty() ? pane : *page_windows.back(); }

    /// The panel's dots per inch.
    UINT panel_dpi() const { return std::max(GetDpiForWindow(panel), 96U); }

    /// Makes the fonts for the panel's dots per inch, when they changed.
    void make_fonts() {
        const UINT now = panel_dpi();
        if (now == dpi && title_font != nullptr)
            return;
        drop_fonts();
        dpi = now;
        title_font = CreateFontW(-scaled(title_height, dpi), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE,
                                 FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        glyph = CreateFontW(-scaled(glyph_height, dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH, glyph_font);
    }

    void drop_fonts() {
        for (HFONT font : {title_font, glyph}) {
            if (font != nullptr)
                DeleteObject(font);
        }
        title_font = nullptr;
        glyph = nullptr;
    }

    /// The title band, across the panel inside its edge.
    RECT band() const {
        RECT client{};
        GetClientRect(panel, &client);
        const UINT at = panel_dpi();
        const int edge = scaled(frame_edge, at);
        return RECT{edge, edge, client.right - edge, edge + scaled(band_height, at)};
    }

    /// The close button, at the band's right.
    RECT close_button() const {
        RECT button = band();
        button.left = std::max<LONG>(button.right - scaled(close_width, panel_dpi()), button.left);
        return button;
    }

    /// Where the panes go: below the band and its rule, inside the edge.
    RECT content() const {
        RECT client{};
        GetClientRect(panel, &client);
        const int edge = scaled(frame_edge, panel_dpi());
        const RECT over = band();
        return RECT{edge, over.bottom + 1, client.right - edge,
                    std::max<LONG>(client.bottom - edge, over.bottom + 1)};
    }

    /// Draws the frame: the face, the band with the game's icon, the title
    /// and the close button, the rule under the band and the raised edge.
    void paint(HDC context) {
        make_fonts();
        RECT client{};
        GetClientRect(panel, &client);
        fill(context, client, look.face);
        const RECT over = band();
        fill(context, over, look.band);
        fill(context, RECT{over.left, over.bottom, over.right, over.bottom + 1}, look.rule);
        const int padding = scaled(band_padding, dpi);
        const int side = scaled(mark_side, dpi);
        const int mark_left = over.left + padding;
        if (HICON icon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),
                                                       MAKEINTRESOURCEW(icon_resource), IMAGE_ICON,
                                                       side, side, LR_DEFAULTCOLOR))) {
            DrawIconEx(context, mark_left, over.top + (over.bottom - over.top - side) / 2, icon,
                       side, side, 0, nullptr, DI_NORMAL);
            DestroyIcon(icon);
        }
        const RECT button = close_button();
        SetBkMode(context, TRANSPARENT);
        RECT words{mark_left + side + scaled(mark_gap, dpi), over.top, button.left - padding,
                   over.bottom};
        HGDIOBJ was = SelectObject(context, title_font);
        SetTextColor(context, colour_of(look.text));
        DrawTextW(context, title.c_str(), static_cast<int>(title.size()), &words,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        if (close_hovered || close_held)
            fill(context, button, close_held ? look.held : look.hover);
        RECT cross = button;
        SelectObject(context, glyph);
        SetTextColor(context, colour_of(close_hovered || close_held ? look.text : look.quiet));
        DrawTextW(context, close_glyph, -1, &cross,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(context, was);
        const int edge = scaled(frame_edge, dpi);
        fill(context, RECT{0, 0, client.right, edge}, look.edge_light);
        fill(context, RECT{0, 0, edge, client.bottom}, look.edge_light);
        fill(context, RECT{0, client.bottom - edge, client.right, client.bottom}, look.edge_dark);
        fill(context, RECT{client.right - edge, 0, client.right, client.bottom}, look.edge_dark);
    }

    /// Marks the close button under the pointer, or not.
    void hover_close(bool over) {
        if (over == close_hovered)
            return;
        close_hovered = over;
        const RECT button = close_button();
        InvalidateRect(panel, &button, FALSE);
    }

    /// Makes the panel in the middle and shows it on top.
    bool make_panel() {
        HINSTANCE instance = GetModuleHandleW(nullptr);
        if (!register_classes(instance))
            return false;
        RECT place{};
        if (parent != nullptr) {
            place = placement(parent, style);
        } else {
            RECT work{};
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
            const int width = window_width + 2 * frame_edge;
            const int height = window_height + band_height + 1 + 2 * frame_edge;
            place = RECT{work.left, work.top, work.left + width, work.top + height};
            OffsetRect(&place, (work.right - work.left - width) / 2,
                       (work.bottom - work.top - height) / 2);
        }
        const DWORD extended = parent != nullptr ? WS_EX_NOPARENTNOTIFY : 0;
        panel = CreateWindowExW(extended, panel_class, title.c_str(), style, place.left, place.top,
                                place.right - place.left, place.bottom - place.top, parent,
                                nullptr, instance, this);
        if (panel == nullptr)
            return false;
        ShowWindow(panel, SW_SHOWNORMAL);
        SetWindowPos(panel, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        if (parent == nullptr)
            SetForegroundWindow(panel);
        return true;
    }

    /// Makes a pane for `made` in the panel's content and shows it.
    HWND make_pane(Pane& made) {
        const RECT place = content();
        made.window = CreateWindowExW(WS_EX_NOPARENTNOTIFY, pane_class, L"",
                                      WS_CHILD | WS_CLIPSIBLINGS, place.left, place.top,
                                      place.right - place.left, place.bottom - place.top, panel,
                                      nullptr, GetModuleHandleW(nullptr), &made);
        if (made.window != nullptr)
            ShowWindow(made.window, SW_SHOWNORMAL);
        return made.window;
    }

    /// Fits every pane to the panel's content.
    void lay_panes() {
        const RECT place = content();
        auto move = [&place](const Pane& held) {
            if (held.window != nullptr)
                SetWindowPos(held.window, nullptr, place.left, place.top,
                             place.right - place.left, place.bottom - place.top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
        };
        move(pane);
        for (const auto& page : page_windows)
            move(*page);
    }

    /// Keeps the panel in the middle of the game's window as it changes size.
    void fit_to_parent() {
        if (parent == nullptr || !IsWindow(parent) || panel == nullptr)
            return;
        RECT client{};
        GetClientRect(parent, &client);
        if (client.right == fitted.cx && client.bottom == fitted.cy)
            return;
        fitted = SIZE{client.right, client.bottom};
        const RECT place = placement(parent, style);
        SetWindowPos(panel, nullptr, place.left, place.top, place.right - place.left,
                     place.bottom - place.top, SWP_NOZORDER | SWP_NOACTIVATE);
    }

    /// Closes a page's own window and shows the pane it was in place of.
    void close_page_window(Pane* page) {
        auto found = std::find_if(page_windows.begin(), page_windows.end(),
                                  [page](const auto& held) { return held.get() == page; });
        if (found == page_windows.end())
            return;
        std::unique_ptr<Pane> ending = std::move(*found);
        page_windows.erase(found);
        release(*ending);
        if (ending->window != nullptr && IsWindow(ending->window))
            DestroyWindow(ending->window);
        HWND back = showing().window;
        if (back != nullptr) {
            ShowWindow(back, SW_SHOWNORMAL);
            SetFocus(back);
        }
    }

    /// Ends the sign-in: the panel closes, the browsers and the handlers are
    /// let go, and the state turns `next` unless the sign-in had already ended.
    void finish(State next) {
        settle(next);
        ++session;
        if (parent != nullptr && IsWindow(parent) && panel != nullptr)
            SetFocus(parent);
        for (auto& page : page_windows)
            release(*page);
        release(pane);
        if (panel != nullptr) {
            HWND ending = panel;
            panel = nullptr;
            KillTimer(ending, fit_timer);
            // The panes go with the panel.
            if (IsWindow(ending))
                DestroyWindow(ending);
        }
        page_windows.clear();
        pane.window = nullptr;
        drop_fonts();
        dpi = 0;
        close_hovered = false;
        close_held = false;
        for (IUnknown* held : {static_cast<IUnknown*>(starting), static_cast<IUnknown*>(new_window),
                               static_cast<IUnknown*>(key), static_cast<IUnknown*>(browser)}) {
            if (held != nullptr)
                held->Release();
        }
        starting = nullptr;
        new_window = nullptr;
        key = nullptr;
        browser = nullptr;
        if (com) {
            com = false;
            CoUninitialize();
        }
    }

    /// Gives a new browser in `held` the sign-in's handlers.
    void attach(Pane& held, ICoreWebView2Controller* controller) {
        controller->AddRef();
        held.controller = controller;
        fit_browser(held);
        controller->put_IsVisible(TRUE);
        EventRegistrationToken token{};
        held.view->add_NavigationStarting(starting, &token);
        held.view->add_NewWindowRequested(new_window, &token);
        controller->add_AcceleratorKeyPressed(key, &token);
    }

    bool start(const Request& request);
};

LRESULT CALLBACK panel_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* owner = reinterpret_cast<Session*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (owner == nullptr || owner->panel != window) {
        if (message == WM_CLOSE) {
            DestroyWindow(window);
            return 0;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }
    const POINT at{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
    switch (message) {
    case WM_SIZE:
        owner->lay_panes();
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT painting{};
        HDC context = BeginPaint(window, &painting);
        owner->paint(context);
        EndPaint(window, &painting);
        return 0;
    }
    case WM_SETFOCUS:
        if (HWND showing = owner->showing().window)
            SetFocus(showing);
        return 0;
    case WM_TIMER:
        if (wparam == fit_timer)
            owner->fit_to_parent();
        return 0;
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
        TrackMouseEvent(&tracking);
        const RECT button = owner->close_button();
        owner->hover_close(PtInRect(&button, at) != FALSE);
        return 0;
    }
    case WM_MOUSELEAVE:
        owner->hover_close(false);
        return 0;
    case WM_LBUTTONDOWN: {
        const RECT button = owner->close_button();
        if (PtInRect(&button, at)) {
            owner->close_held = true;
            SetCapture(window);
            InvalidateRect(window, &button, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (!owner->close_held)
            return 0;
        owner->close_held = false;
        ReleaseCapture();
        const RECT button = owner->close_button();
        InvalidateRect(window, &button, FALSE);
        // The close button closes every browser the sign-in has open.
        if (PtInRect(&button, at))
            PostMessageW(window, WM_CLOSE, 0, 0);
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (owner->close_held) {
            owner->close_held = false;
            const RECT button = owner->close_button();
            InvalidateRect(window, &button, FALSE);
        }
        return 0;
    case WM_CLOSE:
        owner->finish(State::closed);
        return 0;
    case WM_DESTROY:
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        return 0;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

LRESULT CALLBACK pane_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* pane = reinterpret_cast<Pane*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_SIZE:
        if (pane != nullptr)
            fit_browser(*pane);
        return 0;
    case WM_ERASEBKGND: {
        if (pane == nullptr || pane->owner == nullptr)
            return DefWindowProcW(window, message, wparam, lparam);
        RECT client{};
        GetClientRect(window, &client);
        fill(reinterpret_cast<HDC>(wparam), client, pane->owner->look.face);
        return 1;
    }
    case WM_SETFOCUS:
        if (pane != nullptr && pane->controller != nullptr)
            pane->controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        return 0;
    case WM_CLOSE:
        if (pane == nullptr || pane->owner == nullptr)
            DestroyWindow(window);
        else if (pane->page_window)
            pane->owner->close_page_window(pane);
        else
            pane->owner->finish(State::closed);
        return 0;
    case WM_DESTROY:
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        return 0;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

bool Session::start(const Request& request) {
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(apartment))
        return false;
    com = true;
    parent = static_cast<HWND>(request.parent);
    if (parent != nullptr && !IsWindow(parent))
        parent = nullptr;
    style = parent != nullptr ? WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN
                              : WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
    title = wide(request.title);
    landing = wide(request.landing);
    look = request.look;
    fitted = SIZE{};
    pane = Pane{};
    pane.owner = this;
    if (!make_panel() || make_pane(pane) == nullptr)
        return false;
    if (parent != nullptr) {
        RECT client{};
        GetClientRect(parent, &client);
        fitted = SIZE{client.right, client.bottom};
        SetTimer(panel, fit_timer, fit_every_ms, nullptr);
    }
    SetFocus(pane.window);

    const unsigned current = session;
    // A navigation to the landing address ends the sign-in, once the
    // browser that saw it has finished with it.
    starting = handler<ICoreWebView2NavigationStartingEventHandler, ICoreWebView2*,
                       ICoreWebView2NavigationStartingEventArgs*>(
        [this, current](ICoreWebView2* sender, ICoreWebView2NavigationStartingEventArgs* args) {
            if (session != current)
                return S_OK;
            LPWSTR uri = nullptr;
            if (FAILED(args->get_Uri(&uri)) || uri == nullptr)
                return S_OK;
            const std::wstring_view to(uri);
            if (!landing.empty() && to.starts_with(landing)) {
                args->put_Cancel(TRUE);
                forget_cookies(sender);
                if (state == State::opening || state == State::open) {
                    state = State::landed;
                    landed = utf8(uri);
                }
                if (panel != nullptr)
                    PostMessageW(panel, WM_CLOSE, 0, 0);
            }
            CoTaskMemFree(uri);
            return S_OK;
        });
    // Escape closes the pane it is pressed in, as it closes the game's
    // question; in the first pane it closes the panel.
    key = handler<ICoreWebView2AcceleratorKeyPressedEventHandler, ICoreWebView2Controller*,
                  ICoreWebView2AcceleratorKeyPressedEventArgs*>(
        [this, current](ICoreWebView2Controller* sender,
                        ICoreWebView2AcceleratorKeyPressedEventArgs* args) {
            if (session != current)
                return S_OK;
            COREWEBVIEW2_KEY_EVENT_KIND kind{};
            UINT pressed = 0;
            if (FAILED(args->get_KeyEventKind(&kind)) || FAILED(args->get_VirtualKey(&pressed)) ||
                kind != COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN || pressed != VK_ESCAPE)
                return S_OK;
            args->put_Handled(TRUE);
            HWND closing = pane.window;
            for (const auto& page : page_windows) {
                if (page->controller == sender)
                    closing = page->window;
            }
            if (closing != nullptr)
                PostMessageW(closing, WM_CLOSE, 0, 0);
            return S_OK;
        });
    // A page asking for a window of its own gets a pane in place of the one
    // showing, which is hidden until it closes. It stays its page's: the
    // page that opened it hears from it, and may close it.
    new_window = handler<ICoreWebView2NewWindowRequestedEventHandler, ICoreWebView2*,
                         ICoreWebView2NewWindowRequestedEventArgs*>(
        [this, current](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) {
            if (session != current || browser == nullptr || panel == nullptr)
                return S_OK;
            auto made_pane = std::make_unique<Pane>();
            made_pane->owner = this;
            made_pane->page_window = true;
            HWND before = showing().window;
            if (make_pane(*made_pane) == nullptr)
                return S_OK;
            ICoreWebView2Deferral* deferral = nullptr;
            if (FAILED(args->GetDeferral(&deferral)) || deferral == nullptr) {
                DestroyWindow(made_pane->window);
                return S_OK;
            }
            Pane* held = made_pane.get();
            page_windows.push_back(std::move(made_pane));
            if (before != nullptr)
                ShowWindow(before, SW_HIDE);
            args->AddRef();
            auto* made = handler<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler,
                                 HRESULT, ICoreWebView2Controller*>(
                [this, current, held, args,
                 deferral](HRESULT result, ICoreWebView2Controller* controller) {
                    const bool kept =
                        session == current &&
                        std::any_of(page_windows.begin(), page_windows.end(),
                                    [held](const auto& page) { return page.get() == held; });
                    if (kept && SUCCEEDED(result) && controller != nullptr &&
                        SUCCEEDED(controller->get_CoreWebView2(&held->view)) &&
                        held->view != nullptr) {
                        attach(*held, controller);
                        auto* close_asked = handler<ICoreWebView2WindowCloseRequestedEventHandler,
                                                    ICoreWebView2*, IUnknown*>(
                            [page = held->window](ICoreWebView2*, IUnknown*) {
                                PostMessageW(page, WM_CLOSE, 0, 0);
                                return S_OK;
                            });
                        EventRegistrationToken token{};
                        held->view->add_WindowCloseRequested(close_asked, &token);
                        close_asked->Release();
                        args->put_NewWindow(held->view);
                        args->put_Handled(TRUE);
                        SetFocus(held->window);
                    } else {
                        if (controller != nullptr && !kept)
                            controller->Close();
                        if (kept)
                            close_page_window(held);
                    }
                    deferral->Complete();
                    deferral->Release();
                    args->Release();
                    return S_OK;
                });
            if (FAILED(browser->CreateCoreWebView2Controller(held->window, made))) {
                close_page_window(held);
                deferral->Complete();
                deferral->Release();
                args->Release();
            }
            made->Release();
            return S_OK;
        });
    auto* controller_made = handler<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler,
                                    HRESULT, ICoreWebView2Controller*>(
        [this, current, address = wide(request.address)](HRESULT result,
                                                         ICoreWebView2Controller* controller) {
            if (session != current) {
                if (controller != nullptr)
                    controller->Close();
                return S_OK;
            }
            if (FAILED(result) || controller == nullptr ||
                FAILED(controller->get_CoreWebView2(&pane.view)) || pane.view == nullptr) {
                finish(State::failed);
                return S_OK;
            }
            attach(pane, controller);
            forget_cookies(pane.view);
            pane.view->Navigate(address.c_str());
            if (GetFocus() == pane.window)
                controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
            settle(State::open);
            return S_OK;
        });
    auto* environment_made = handler<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,
                                     HRESULT, ICoreWebView2Environment*>(
        [this, current, controller_made](HRESULT result, ICoreWebView2Environment* environment) {
            if (session != current)
                return S_OK;
            if (SUCCEEDED(result) && environment != nullptr) {
                environment->AddRef();
                browser = environment;
            }
            if (FAILED(result) || environment == nullptr ||
                FAILED(environment->CreateCoreWebView2Controller(pane.window, controller_made)))
                finish(State::failed);
            return S_OK;
        });
    const std::wstring profile = profile_path();
    const HRESULT created = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, profile.empty() ? nullptr : profile.c_str(), nullptr, environment_made
    );
    // The browser holds the completions it will call for as long as it needs them.
    environment_made->Release();
    controller_made->Release();
    return SUCCEEDED(created);
}

} // namespace

struct Window::Shared : Session {};

bool available() noexcept {
    LPWSTR version = nullptr;
    const bool found =
        SUCCEEDED(GetAvailableCoreWebView2BrowserVersionString(nullptr, &version)) &&
        version != nullptr;
    CoTaskMemFree(version);
    return found;
}

Window::Window() : shared_(std::make_shared<Shared>()) {}

Window::~Window() {
    close();
}

bool Window::open(const Request& request) {
    close();
    if (!available())
        return false;
    shared_->state = State::opening;
    shared_->landed.clear();
    if (!shared_->start(request)) {
        shared_->finish(State::failed);
        return false;
    }
    return true;
}

void Window::close() {
    if (shared_->panel != nullptr || shared_->com)
        shared_->finish(State::closed);
}

State Window::state() const {
    return shared_->state;
}

std::string Window::landed_address() const {
    return shared_->state == State::landed ? shared_->landed : std::string();
}

} // namespace oa::platform::web_sign_in
