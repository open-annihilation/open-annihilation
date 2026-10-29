// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A panel inside the game's window, framed as the game's dialogs are, that
// shows a web site's sign-in page, and closes itself once the page goes to an address that starts as the
// caller says, keeping that address. A site that ends its sign-in on a page
// of its own, as GOG does, so hands its code to the game without the player
// copying it. On Windows the panel is the system's WebView2; where a system
// has no such browser, available is false and nothing opens.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace oa::platform::web_sign_in {

/// Tells whether this system can show a sign-in window.
///
/// @return true on Windows with the WebView2 runtime installed, when the
///         game was built with it
[[nodiscard]] bool available() noexcept;

/// The colours of the panel's frame, each 0xRRGGBB.
struct Look {
    uint32_t face{};       ///< behind the page while it loads
    uint32_t band{};       ///< the title band
    uint32_t rule{};       ///< the hairline under the band
    uint32_t edge_light{}; ///< the raised edge's top and left
    uint32_t edge_dark{};  ///< the raised edge's bottom and right
    uint32_t text{};       ///< the title, and the close button's cross under the pointer
    uint32_t quiet{};      ///< the close button's cross
    uint32_t hover{};      ///< the close button's face under the pointer
    uint32_t held{};       ///< the close button's face while a press on it is held
};

/// What a sign-in window shows.
struct Request {
    std::string address{}; ///< the sign-in page it opens on, an https address
    /// The window closes when the page goes to an address starting with this,
    /// before that page loads; it is kept as Window::landed_address.
    std::string landing{};
    /// The title in the frame's band, in UTF-8.
    std::string title{};
    Look look{}; ///< the frame's colours
    /// The game's window the panel shows inside, as the system knows it: a
    /// window handle (HWND) on Windows. With none it is a window of its own.
    void* parent{};
};

/// Where a sign-in window is.
enum class State : uint8_t {
    idle,    ///< it has not been opened
    opening, ///< the window and its browser are starting
    open,    ///< the page is showing
    landed,  ///< the page went to the landing address; the window has closed
    closed,  ///< the player closed it, or close was called
    failed,  ///< the browser could not start; the window has closed
};

/// One sign-in panel. It lives on the thread that opens it, whose event loop
/// (SDL's) delivers its messages and the browser's answers; every member is
/// called from that thread. Destroying it closes the panel.
class Window {
  public:
    Window();
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    /// Opens the panel in the middle of the parent's window, in front, and
    /// loads the page. Its frame has a band with the game's icon, the title
    /// and a close button, which closes the panel and every page in it. A
    /// panel still open is closed first. A page's own window, such as a
    /// sign-in with Google opens, takes the first page's place in the panel
    /// until it closes. Escape closes the page it is pressed in, and the
    /// first page closes the panel. The browser keeps its files
    /// in the player's local application data, and forgets its cookies as the
    /// page loads and as it lands, so no sign-in carries over to a later window.
    ///
    /// @param request what it shows
    /// @return false when this system cannot show one
    bool open(const Request& request);

    /// Closes the window; its state turns closed unless it had landed.
    void close();

    /// Returns where the window is.
    ///
    /// @return its state
    [[nodiscard]] State state() const;

    /// Returns the address the page went to.
    ///
    /// @return the whole address, when the state is landed; else empty
    [[nodiscard]] std::string landed_address() const;

  private:
    struct Shared;
    std::shared_ptr<Shared> shared_{};
};

} // namespace oa::platform::web_sign_in
