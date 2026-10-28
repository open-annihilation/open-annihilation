// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The key editor on the OA layer. open_key_editor pushes it, modal over a
// backdrop: centred at its size class's dialog on the menus, and beside the
// side column in a match, the pause menu shown first as Settings shows it.
// SAVE writes the player's own layout and reloads the bindings. A question
// on the menus is a QuestionScreen. QuestionScreen::placement and
// NoticeScreen::placement are empty in a match, so a question, a notice or
// the name field there is a screen of this file, centred in the match dialog.

#include "oa_layer.hpp"

#include "oa/app/game_directory.hpp"
#include "oa/app/runtime.hpp"
#include "oa/app/user_folder.hpp"
#include "oa/app/view_rules.hpp"
#include "oa/data/input_actions/actions.hpp"
#include "oa/data/key_library/library.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/formats/oakeys/manifest.hpp"
#include "oa/ui/key_bindings/legacy.hpp"
#include "oa/ui/key_bindings/legacy_import.hpp"
#include "oa/ui/engine_settings/notice.hpp"
#include "oa/ui/engine_settings/prompt.hpp"
#include "oa/sim/gameplay_input/order_cursor.hpp"
#include "oa/ui/frontend_dialogs.hpp"
#include "oa/ui/key_bindings/chord.hpp"
#include "oa/ui/key_bindings/input.hpp"
#include "oa/ui/key_editor/screen.hpp"
#include "oa/ui/kit/chrome.hpp"
#include "oa/ui/kit/looks.hpp"

#include <SDL3/SDL.h>
#include "oa/ui/kit/components.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>
#include "oa/ui/kit/components_more.hpp"
#include "oa/ui/kit/controls.hpp"
#include "oa/ui/kit/input.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace oa::app {

/// The keyboard shape the system reports. On macOS that is Carbon's layout
/// type. Elsewhere it is a guess from the keys that print.
///
/// @param map the player's keyboard
/// @return ANSI, ISO or JIS
[[nodiscard]] oa::ui::kit::KeyboardShape
keyboard_shape(const oa::ui::key_bindings::KeyboardMap& map);

namespace {

/// The preference that remembers the keyboard picture's shape.
constexpr std::string_view kKeyboardShapePreference = "open-annihilation.keys.keyboard";

namespace fs = std::filesystem;
namespace kit = oa::ui::kit;
namespace editor = oa::ui::key_editor;
namespace keys = oa::ui::key_bindings;
namespace settings = oa::ui::engine_settings;
namespace display = oa::ui::display_layout;
namespace pad = oa::ui::pad_controls;
namespace key_library = oa::data::key_library;
namespace oakeys = oa::formats::oakeys;
namespace actions = oa::data::input_actions;

/// The most bytes PASTE… reads from the clipboard.
constexpr std::size_t kClipboardCap = 256 * 1024;
/// One byte past the legacy reader's limit, so a longer file is reported.
constexpr std::size_t kLegacyReadCap = 64 * 1024 + 1;

/// True for Primary+Z, the editor's Undo: the command key on macOS, Ctrl elsewhere.
///
/// @param sdl_key the key code
/// @param modifiers the SDL modifier bits
/// @return true for that chord
bool undo_key(uint32_t sdl_key, uint16_t modifiers) noexcept {
    if (sdl_key != SDLK_Z || (modifiers & SDL_KMOD_SHIFT) != 0)
        return false;
#ifdef SDL_PLATFORM_MACOS
    return (modifiers & SDL_KMOD_GUI) != 0;
#else
    return (modifiers & SDL_KMOD_CTRL) != 0;
#endif
}

/// True when this run must not open a file dialog.
///
/// @param unattended the command line asked for no questions
/// @param headless a check is running
/// @return true for a check, an unattended run, or a driver nobody watches
bool skip_file_dialog(bool unattended, bool headless) {
    if (unattended || headless)
        return true;
    const char* driver = SDL_GetCurrentVideoDriver();
    return driver != nullptr && unattended_environment({}, driver);
}

/// Packs one manifest as a key-layout package.
///
/// @param manifest the layout
/// @param[out] problem why it was not packed
/// @return the package, or nothing
std::optional<std::vector<uint8_t>>
pack_share(const oakeys::Manifest& manifest, std::string& problem) {
    const std::string yaml = oakeys::write_manifest(manifest);
    key_library::PackageFile held;
    held.name = std::string(oakeys::manifest_file);
    held.bytes.assign(yaml.begin(), yaml.end());
    const std::array<key_library::PackageFile, 1> files{std::move(held)};
    return key_library::package_bytes(files, &problem);
}

/// Reads at most `cap` bytes of a file.
///
/// @param path the file
/// @param cap the most bytes
/// @return the bytes; empty when the file cannot be opened
std::vector<uint8_t> read_capped(const fs::path& path, std::size_t cap) {
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> bytes;
    if (!in)
        return bytes;
    bytes.resize(cap);
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(cap));
    const std::streamsize got = in.gcount();
    if (got <= 0)
        bytes.clear();
    else
        bytes.resize(static_cast<std::size_t>(got));
    return bytes;
}

/// A file the system's dialog named.
struct PickedPath {
    bool cancelled{false}; ///< the player closed the dialog
    bool failed{false};    ///< the dialog could not be shown
    fs::path path{};       ///< the file
    std::string error{};   ///< why it failed
};

#if OA_NATIVE_FOLDER_DIALOG
/// SDL answers inside the call on macOS and from event pumping elsewhere.
struct DialogWait {
    std::atomic<bool> done{false};
    bool failed{false};
    std::string path{};
    std::string error{};
};

void SDLCALL dialog_chosen(void* userdata, const char* const* filelist, int) {
    auto* wait = static_cast<DialogWait*>(userdata);
    if (filelist == nullptr) {
        wait->failed = true;
        const char* reason = SDL_GetError();
        if (reason != nullptr)
            wait->error = reason;
    } else if (filelist[0] != nullptr && filelist[0][0] != '\0') {
        wait->path = filelist[0];
    }
    wait->done.store(true, std::memory_order_release);
}

/// Asks for one file, the same way the installer pick waits.
///
/// @param window the window the dialog belongs to
/// @param save true for SAVE FILE…
/// @param start the folder the dialog opens in
/// @param title the dialog's title
/// @return the file, a cancel, or why the dialog failed
PickedPath pick_key_file(SDL_Window* window, bool save, const fs::path& start, std::string title) {
    PickedPath picked;
    DialogWait wait;
    SDL_DialogFileFilter filters[] = {
        {save ? "Key layout" : "Settings", save ? "oakeys" : "ini"},
        {"All files", "*"},
    };
    const SDL_PropertiesID properties = SDL_CreateProperties();
    if (properties == 0) {
        const char* reason = SDL_GetError();
        if (reason != nullptr)
            picked.error = reason;
        picked.failed = true;
        return picked;
    }
    (void)SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_TITLE_STRING, title.c_str());
    (void)SDL_SetPointerProperty(properties, SDL_PROP_FILE_DIALOG_FILTERS_POINTER, filters);
    (void)SDL_SetNumberProperty(properties, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER, 2);
    if (window != nullptr)
        (void)SDL_SetPointerProperty(properties, SDL_PROP_FILE_DIALOG_WINDOW_POINTER, window);
    (void)SDL_SetBooleanProperty(properties, SDL_PROP_FILE_DIALOG_MANY_BOOLEAN, false);
    const std::string location = dialog_location(start);
    if (!location.empty())
        (void)SDL_SetStringProperty(
            properties, SDL_PROP_FILE_DIALOG_LOCATION_STRING, location.c_str()
        );
    SDL_ShowFileDialogWithProperties(
        save ? SDL_FILEDIALOG_SAVEFILE : SDL_FILEDIALOG_OPENFILE, dialog_chosen, &wait, properties
    );
    SDL_DestroyProperties(properties);
    while (!wait.done.load(std::memory_order_acquire)) {
        SDL_PumpEvents();
        SDL_Delay(15);
    }
    if (wait.failed) {
        picked.failed = true;
        picked.error = std::move(wait.error);
        return picked;
    }
    if (wait.path.empty()) {
        picked.cancelled = true;
        return picked;
    }
    picked.path = path_from_utf8(wait.path);
    return picked;
}
#endif

/// The sound opening the editor plays, as Settings does.
constexpr std::string_view kOpenSound = "Options";
/// A question centred in the match dialog. "prompt" is QuestionScreen's name.
constexpr std::string_view kMatchPrompt = "key-editor-prompt";
/// A notice centred in the match dialog. "notice" is NoticeScreen's name.
constexpr std::string_view kMatchNotice = "key-editor-notice";
/// Settings' name on the layer.
constexpr std::string_view kSettingsScreen = "settings";
/// The Library's name on the layer. EDIT KEYS… covers it.
constexpr std::string_view kLibraryScreen = "library";

/// The left pointer button, as the layer numbers it.
constexpr uint8_t kLeftButton = 1;

/// Returns where a box of a point size sits in the match dialog.
///
/// A phone's scale of 0 keeps the dialog's fitted rectangle and the box's
/// own point size, so the layer maps that rectangle onto the points. Any
/// other scale centres the box, drawn at the dialog's scale, in the dialog.
///
/// @param dialog where the match dialog shows
/// @param width the box's width, in points
/// @param height the box's height, in points
/// @return the box's place
LayerPlacement place_in_dialog(const MatchDialogPlace& dialog, int32_t width, int32_t height) {
    display::Rect shown = dialog.shown;
    if (dialog.scale_percent != 0) {
        const kit::Rect drawn =
            kit::to_canvas(kit::Rect{0, 0, width, height}, dialog.scale_percent);
        shown.x = dialog.shown.x + (dialog.shown.width - static_cast<int>(drawn.width)) / 2;
        shown.y = dialog.shown.y + (dialog.shown.height - static_cast<int>(drawn.height)) / 2;
        shown.width = static_cast<int>(drawn.width);
        shown.height = static_cast<int>(drawn.height);
    }
    return {shown, width, height, dialog.scale_percent};
}

/// Returns the match dialog's place, or an empty one off a match.
///
/// @param placed_on what the screen is placed on
/// @return the dialog's place
MatchDialogPlace match_place(const LayerView& placed_on) {
    if (!placed_on.match || placed_on.match_layout == nullptr)
        return {};
    return OaLayer::match_dialog(
        *placed_on.match_layout, placed_on.interface_percent, placed_on.fit_safe_area
    );
}

/// Tells whether a question or a notice is already up.
///
/// @param layer the layer
/// @return true while one shows
bool question_up(const OaLayer& layer) {
    return layer.find(kit::question_word) != nullptr || layer.find(kMatchPrompt) != nullptr ||
           layer.find(kit::notice_word) != nullptr || layer.find(kMatchNotice) != nullptr;
}

/// Returns the name a keep question uses for a layout.
///
/// @param model the editor
/// @param id the layout the player is choosing
/// @return its name, looked up for OA default
std::string target_layout_name(const editor::Editor& model, std::string_view id) {
    if (id.empty() || id == "oa-default")
        return editor::show(model.text(), editor::words::oa_default);
    for (const editor::LayoutSummary& layout : model.installed()) {
        if (layout.id == id && !layout.name.empty())
            return layout.name;
    }
    return std::string(id);
}

/// One button of a question, its caption looked up.
///
/// @param hooks the lookup
/// @param word the English caption
/// @param accent the accent face
/// @param id the automation id
/// @return the button
kit::QuestionButton question_button(
    const editor::TextHooks& hooks, std::string_view word, bool accent, std::string id
) {
    kit::QuestionButton button;
    button.caption = editor::show(hooks, word);
    button.accent = accent;
    button.id = std::move(id);
    return button;
}

/// The question that asks before another layout is chosen.
///
/// @param model the editor
/// @param id the layout the player is choosing
/// @return the question, named prompt
kit::Question keep_question(const editor::Editor& model, std::string_view id) {
    const editor::TextHooks& hooks = model.text();
    const std::string count = std::to_string(model.unsaved());
    const std::string layout = target_layout_name(model, id);
    const std::string_view pattern =
        model.unsaved() == 1 ? editor::words::keep_question_one : editor::words::keep_question;
    kit::Question question;
    question.title = editor::show(hooks, editor::words::keys);
    kit::Paragraph body;
    body.text = editor::fill(editor::show(hooks, pattern), {{"n", count}, {"layout", layout}});
    question.paragraphs.push_back(std::move(body));
    question.buttons.push_back(question_button(hooks, editor::words::keep_them, true, "keep-them"));
    question.buttons.push_back(
        question_button(hooks, editor::words::set_aside, false, "set-aside")
    );
    question.buttons.push_back(question_button(hooks, editor::words::cancel, false, "cancel"));
    question.primary_button = 0;
    question.cancel_button = 2;
    question.marked = 0;
    return question;
}

/// The notice a failed write shows.
///
/// @param hooks the lookup
/// @param reason the sentence the writer gave
/// @return the notice, named notice
kit::Notice write_notice(const editor::TextHooks& hooks, std::string_view reason) {
    kit::Notice notice;
    notice.title = editor::show(hooks, editor::words::keys);
    kit::Paragraph body;
    body.text =
        editor::fill(editor::show(hooks, editor::words::write_failed), {{"reason", reason}});
    notice.paragraphs.push_back(std::move(body));
    notice.ok_caption = editor::show(hooks, editor::words::ok);
    return notice;
}

/// The parts of a question a pointer moves.
///
/// @param question the question
/// @return the hover, the press, the mark and a finger's shift
auto question_look(const kit::Question& question) noexcept {
    return std::make_tuple(
        question.hovered,
        question.pressed,
        question.marked,
        question.finger_shift_x,
        question.finger_shift_y
    );
}

/// The parts of a notice a pointer moves.
///
/// @param notice the notice
/// @return the hover, the press, the mark and a finger's shift
auto notice_look(const kit::Notice& notice) noexcept {
    return std::make_tuple(
        notice.hovered, notice.pressed, notice.marked, notice.finger_shift_x, notice.finger_shift_y
    );
}

/// Counts a look an input moved, so a hover is drawn.
///
/// @param answer what the screen answered
/// @param moved the input moved the look
/// @param[in,out] revision the screen's revision
/// @return the answer; redraw in place of none when the look moved
LayerAnswer note_look(LayerAnswer answer, bool moved, uint64_t& revision) noexcept {
    if (!moved)
        return answer;
    ++revision;
    return answer == LayerAnswer::none ? LayerAnswer::redraw : answer;
}

/// A question centred in the match dialog. QuestionScreen draws nothing there.
class MatchPrompt final : public LayerScreen {
  public:

    /// What an answer does. True closes the question.
    using Answer = std::function<bool(int32_t button)>;

    /// Makes the question.
    ///
    /// @param layer the layer
    /// @param asked the question
    /// @param answer what a button does
    /// @param close_when tells, once a frame, whether the question must close
    MatchPrompt(
        OaLayer& layer, kit::Question asked, Answer answer, std::function<bool()> close_when
    )
        : layer_(layer), question_(std::move(asked)), answer_(std::move(answer)),
          close_when_(std::move(close_when)) {}

    /// Returns "key-editor-prompt".
    ///
    /// @return the name
    [[nodiscard]] std::string_view name() const override { return kMatchPrompt; }

    /// Returns where the question shows: centred in the match dialog, as tall
    /// as its text at the dialog's class. Empty off a match.
    ///
    /// @param placed_on what the screen is placed on
    /// @return its place
    [[nodiscard]] LayerPlacement placement(const LayerView& placed_on) const override {
        const kit::Fonts* fonts = layer_.screen_fonts();
        const MatchDialogPlace dialog = match_place(placed_on);
        if (fonts == nullptr || !placed_on.match || placed_on.match_layout == nullptr)
            return {};
        kit::Question sized = question_;
        sized.size_class = dialog.size_class;
        const int32_t height = settings::prompt_height(sized, fonts);
        const int32_t width = kit::metrics_of(dialog.size_class).notice_width;
        return place_in_dialog(dialog, width, height);
    }

    /// Tells that the question takes every input.
    ///
    /// @return true
    [[nodiscard]] bool modal() const override { return true; }

    /// Tells that what lies under the question darkens.
    ///
    /// @return true
    [[nodiscard]] bool backdrop() const override { return true; }

    /// Lays the question out at the match dialog's class.
    ///
    /// @param placed_on what the screen is placed on
    void lay_out(const LayerView& placed_on) override {
        const MatchDialogPlace dialog = match_place(placed_on);
        if (!placed_on.match || question_.size_class == dialog.size_class)
            return;
        question_.size_class = dialog.size_class;
        ++revision_;
    }

    /// Draws the question.
    ///
    /// @param canvas where it draws
    void draw(const kit::Canvas& canvas) const override {
        if (!kit::can_draw(canvas) || canvas.fonts == nullptr)
            return;
        settings::draw_prompt(canvas, question_);
    }

    /// Takes a pointer.
    ///
    /// @param kind pointer_move, pointer_down or pointer_up
    /// @param button the pointer's button
    /// @param at the pointer, in the question's points
    /// @param finger_reach a finger's reach; 0 for a mouse
    /// @return what the question did
    LayerAnswer
    pointer(ScreenInputKind kind, uint8_t button, kit::Point at, int32_t finger_reach) override {
        const auto before = question_look(question_);
        const int32_t tall = settings::prompt_height(question_, layer_.screen_fonts());
        const kit::Fonts* fonts = layer_.screen_fonts();
        kit::QuestionAnswer outcome{};
        switch (kind) {
        case ScreenInputKind::pointer_move:
            outcome = kit::question_pointer_move(question_, at, tall, fonts);
            break;
        case ScreenInputKind::pointer_down:
            if (button == kLeftButton)
                outcome = finger_reach > 0
                              ? kit::question_finger_down(question_, at, tall, finger_reach, fonts)
                              : kit::question_pointer_down(question_, at, tall, fonts);
            break;
        case ScreenInputKind::pointer_up:
            if (button == kLeftButton)
                outcome = kit::question_pointer_up(question_, at, tall, fonts);
            break;
        default:
            break;
        }
        return note_look(take_answer(outcome), question_look(question_) != before, revision_);
    }

    /// Takes a key.
    ///
    /// @param pressed the key
    /// @return what the question did
    LayerAnswer key(kit::Key pressed, uint32_t /*sdl_key*/) override {
        const auto before = question_look(question_);
        const LayerAnswer answer = take_answer(kit::question_key(question_, pressed));
        return note_look(answer, question_look(question_) != before, revision_);
    }

    /// Lets the controller's keys through: B is Escape, Menu is Enter, and
    /// the shoulders are Tab, which the question already answers.
    ///
    /// @return pass
    LayerAnswer pad_action(pad::Action /*action*/) override { return LayerAnswer::pass; }

    /// A question does not scroll.
    ///
    /// @return none
    LayerAnswer wheel(kit::Point /*at*/, float /*notches*/) override { return LayerAnswer::none; }

    /// Returns the question's controls.
    ///
    /// @return the display list
    [[nodiscard]] kit::DisplayList display_list() const override {
        return kit::question_list(question_, layer_.screen_fonts());
    }

    /// Returns the question's hover, press and mark.
    ///
    /// @return the interaction
    [[nodiscard]] kit::Interaction interaction() const override {
        kit::Interaction shown{};
        shown.hovered = question_.hovered;
        shown.pressed = question_.pressed;
        shown.focused = question_.marked;
        shown.focus_shown = question_.marked != kit::no_control;
        shown.finger_shift = {question_.finger_shift_x, question_.finger_shift_y};
        return shown;
    }

    /// Closes when the editor is gone.
    ///
    /// @return close when it must; otherwise none
    LayerAnswer tick() override {
        return close_when_ && close_when_() ? LayerAnswer::close : LayerAnswer::none;
    }

    /// Returns the question's revision.
    ///
    /// @return the revision
    [[nodiscard]] uint64_t revision() const override { return revision_; }

    /// Has nothing of its own to close.
    void close(bool /*by_key*/) override {}

  private:

    /// Hands an answer to the host.
    ///
    /// @param outcome what the question did
    /// @return what the screen does
    LayerAnswer take_answer(kit::QuestionAnswer outcome) {
        switch (outcome.action) {
        case kit::QuestionAction::answered: {
            ++revision_;
            const bool last = !answer_ || answer_(outcome.button);
            return last ? LayerAnswer::close : LayerAnswer::redraw;
        }
        case kit::QuestionAction::redraw:
            ++revision_;
            return LayerAnswer::redraw;
        case kit::QuestionAction::none:
            break;
        }
        return LayerAnswer::none;
    }

    OaLayer& layer_;                   ///< the layer
    kit::Question question_;           ///< the question
    Answer answer_;                    ///< what a button does
    std::function<bool()> close_when_; ///< whether the question must close
    uint64_t revision_{};              ///< counts the changes to its look
};

/// A notice centred in the match dialog. NoticeScreen draws nothing there.
class MatchNotice final : public LayerScreen {
  public:

    /// Makes the notice.
    ///
    /// @param layer the layer
    /// @param told the notice
    /// @param close_when tells, once a frame, whether the notice must close
    MatchNotice(OaLayer& layer, kit::Notice told, std::function<bool()> close_when)
        : layer_(layer), notice_(std::move(told)), close_when_(std::move(close_when)) {}

    /// Returns "key-editor-notice".
    ///
    /// @return the name
    [[nodiscard]] std::string_view name() const override { return kMatchNotice; }

    /// Returns where the notice shows: centred in the match dialog. Empty off a match.
    ///
    /// @param placed_on what the screen is placed on
    /// @return its place
    [[nodiscard]] LayerPlacement placement(const LayerView& placed_on) const override {
        const kit::Fonts* fonts = layer_.screen_fonts();
        const MatchDialogPlace dialog = match_place(placed_on);
        if (fonts == nullptr || !placed_on.match || placed_on.match_layout == nullptr)
            return {};
        kit::Notice sized = notice_;
        sized.size_class = dialog.size_class;
        const int32_t height = settings::notice_height(sized, fonts);
        const int32_t width = kit::metrics_of(dialog.size_class).notice_width;
        return place_in_dialog(dialog, width, height);
    }

    /// Tells that the notice takes every input.
    ///
    /// @return true
    [[nodiscard]] bool modal() const override { return true; }

    /// Tells that what lies under the notice darkens.
    ///
    /// @return true
    [[nodiscard]] bool backdrop() const override { return true; }

    /// Lays the notice out at the match dialog's class.
    ///
    /// @param placed_on what the screen is placed on
    void lay_out(const LayerView& placed_on) override {
        const MatchDialogPlace dialog = match_place(placed_on);
        if (!placed_on.match || notice_.size_class == dialog.size_class)
            return;
        notice_.size_class = dialog.size_class;
        ++revision_;
    }

    /// Draws the notice.
    ///
    /// @param canvas where it draws
    void draw(const kit::Canvas& canvas) const override {
        if (!kit::can_draw(canvas) || canvas.fonts == nullptr)
            return;
        settings::draw_notice(canvas, notice_);
    }

    /// Takes a pointer.
    ///
    /// @param kind pointer_move, pointer_down or pointer_up
    /// @param button the pointer's button
    /// @param at the pointer, in the notice's points
    /// @param finger_reach a finger's reach; 0 for a mouse
    /// @return what the notice did
    LayerAnswer
    pointer(ScreenInputKind kind, uint8_t button, kit::Point at, int32_t finger_reach) override {
        const auto before = notice_look(notice_);
        const int32_t tall = settings::notice_height(notice_, layer_.screen_fonts());
        kit::NoticeAction action = kit::NoticeAction::none;
        switch (kind) {
        case ScreenInputKind::pointer_move:
            action = kit::notice_pointer_move(notice_, at, tall);
            break;
        case ScreenInputKind::pointer_down:
            if (button == kLeftButton)
                action = finger_reach > 0 ? kit::notice_finger_down(notice_, at, tall, finger_reach)
                                          : kit::notice_pointer_down(notice_, at, tall);
            break;
        case ScreenInputKind::pointer_up:
            if (button == kLeftButton)
                action = kit::notice_pointer_up(notice_, at, tall);
            break;
        default:
            break;
        }
        return note_look(take_action(action), notice_look(notice_) != before, revision_);
    }

    /// Takes a key.
    ///
    /// @param pressed the key
    /// @return what the notice did
    LayerAnswer key(kit::Key pressed, uint32_t /*sdl_key*/) override {
        const auto before = notice_look(notice_);
        const LayerAnswer answer = take_action(kit::notice_key(notice_, pressed));
        return note_look(answer, notice_look(notice_) != before, revision_);
    }

    /// Lets the controller's keys through, as a question does.
    ///
    /// @return pass
    LayerAnswer pad_action(pad::Action /*action*/) override { return LayerAnswer::pass; }

    /// A notice does not scroll.
    ///
    /// @return none
    LayerAnswer wheel(kit::Point /*at*/, float /*notches*/) override { return LayerAnswer::none; }

    /// Returns the notice's controls.
    ///
    /// @return the display list
    [[nodiscard]] kit::DisplayList display_list() const override {
        return kit::notice_list(
            notice_, layer_.screen_fonts(), oa::data::languages::interface_text
        );
    }

    /// Returns the notice's hover, press and mark.
    ///
    /// @return the interaction
    [[nodiscard]] kit::Interaction interaction() const override {
        kit::Interaction shown{};
        shown.hovered = notice_.hovered;
        shown.pressed = notice_.pressed;
        shown.focused = notice_.marked;
        shown.focus_shown = notice_.marked != kit::no_control;
        shown.finger_shift = {notice_.finger_shift_x, notice_.finger_shift_y};
        return shown;
    }

    /// Closes when the editor is gone.
    ///
    /// @return close when it must; otherwise none
    LayerAnswer tick() override {
        return close_when_ && close_when_() ? LayerAnswer::close : LayerAnswer::none;
    }

    /// Returns the notice's revision.
    ///
    /// @return the revision
    [[nodiscard]] uint64_t revision() const override { return revision_; }

    /// Has nothing of its own to close.
    void close(bool /*by_key*/) override {}

  private:

    /// Closes on OK. The notice has no folder to open.
    ///
    /// @param action what the notice did
    /// @return what the screen does
    LayerAnswer take_action(kit::NoticeAction action) {
        switch (action) {
        case kit::NoticeAction::closed:
            ++revision_;
            return LayerAnswer::close;
        case kit::NoticeAction::redraw:
        case kit::NoticeAction::open_folder:
            ++revision_;
            return LayerAnswer::redraw;
        case kit::NoticeAction::none:
            break;
        }
        return LayerAnswer::none;
    }

    OaLayer& layer_;                   ///< the layer
    kit::Notice notice_;               ///< the notice
    std::function<bool()> close_when_; ///< whether the notice must close
    uint64_t revision_{};              ///< counts the changes to its look
};

/// Tells whether the editor's screen is still on the layer.
///
/// @param layer the layer
/// @return true when it has gone
bool editor_gone(const OaLayer& layer) {
    return layer.find(editor::screen_name) == nullptr;
}

/// The physical modifier bits an event carries. Never `primary`.
///
/// @param mods the SDL modifier bits
/// @return the binding bits
uint8_t capture_modifiers(uint16_t mods) {
    uint8_t bits = 0;
    if ((mods & SDL_KMOD_CTRL) != 0)
        bits = static_cast<uint8_t>(bits | keys::ctrl);
    if ((mods & SDL_KMOD_SHIFT) != 0)
        bits = static_cast<uint8_t>(bits | keys::shift);
    if ((mods & SDL_KMOD_ALT) != 0)
        bits = static_cast<uint8_t>(bits | keys::alt);
    if ((mods & SDL_KMOD_GUI) != 0)
        bits = static_cast<uint8_t>(bits | keys::meta);
    return bits;
}

/// One key as the capture reads it. The time is this process's clock, and
/// never 0, so a held Escape can tell that its timer has started.
///
/// @param sdl_key the key code
/// @param modifiers the SDL modifier bits
/// @param down the key went down
/// @param repeat the platform repeated a key that is still held
/// @param scancode the key's position
/// @return the input event
keys::InputEvent
capture_key_event(uint32_t sdl_key, uint16_t modifiers, bool down, bool repeat, uint16_t scancode) {
    keys::InputEvent input;
    input.kind = keys::InputKind::key;
    input.down = down;
    input.repeat = repeat;
    input.key_code = sdl_key;
    input.scancode = scancode;
    input.modifiers = capture_modifiers(modifiers);
    const uint64_t now = SDL_GetTicks();
    input.time_ms = now == 0 ? uint64_t{1} : now;
    return input;
}

/// The position number of a key code, or 0 when it is past the map.
///
/// @param sdl_key the key code
/// @return the position
uint16_t capture_scancode(uint32_t sdl_key) {
    const SDL_Scancode scan = SDL_GetScancodeFromKey(static_cast<SDL_Keycode>(sdl_key), nullptr);
    const auto value = static_cast<uint32_t>(scan);
    if (value == 0 || value >= keys::scancode_limit)
        return 0;
    return static_cast<uint16_t>(value);
}

} // namespace

/// The key editor as a screen of the OA layer.
struct Runtime::KeyEditorScreen final : LayerScreen {
    /// Makes the editor from the inputs the runtime filled, on the list the
    /// request names.
    ///
    /// @param runtime the runtime
    /// @param inputs the layouts, the mod and the settings
    /// @param request the list it opens on, and whether Settings is under it
    /// @param in_match the editor was opened over a match
    KeyEditorScreen(
        Runtime& runtime, editor::EditorInputs inputs, KeyEditorRequest request, bool in_match
    )
        : runtime_(runtime), editor_(std::move(inputs)), over_settings_(request.over_settings) {
        state_.in_match = in_match;
        if (request.filter)
            state_.filter = *request.filter;
        else if (editor_.draft_clashes() != 0 || !editor_.draft_takes().empty())
            state_.filter = editor::Filter::clashes;
        if (state_.filter != editor::Filter::all)
            state_.category = "all";
        if (!request.select.empty())
            state_.selected = std::move(request.select);
        else if (state_.filter == editor::Filter::clashes && state_.selected.empty()) {
            const std::vector<editor::Row> clashing = editor_.rows({}, editor::Filter::clashes, "");
            if (!clashing.empty())
                state_.selected = clashing.front().id;
        }
        apply_keyboard_shape();
    }

    /// Returns "key-editor".
    ///
    /// @return the name
    [[nodiscard]] std::string_view name() const override { return editor::screen_name; }

    /// Returns where the editor shows. On the menus it is centred at the
    /// view's class. In a match it takes the match dialog's rectangle, class
    /// and scale, beside the side column.
    ///
    /// @param placed_on what the screen is placed on
    /// @return its place; empty off the screen it was opened on
    [[nodiscard]] LayerPlacement placement(const LayerView& placed_on) const override {
        if (state_.in_match) {
            if (!placed_on.match || placed_on.match_layout == nullptr)
                return {};
            const MatchDialogPlace place = OaLayer::match_dialog(
                *placed_on.match_layout, placed_on.interface_percent, placed_on.fit_safe_area
            );
            const kit::Metrics& sized = kit::metrics_of(place.size_class);
            return {place.shown, sized.dialog_width, sized.dialog_height, place.scale_percent};
        }
        if (placed_on.match)
            return {};
        const kit::Point size = editor::window_size(placed_on.frame);
        return centred_placement(placed_on, size.x, size.y);
    }

    /// Tells that the editor takes every input.
    ///
    /// @return true
    [[nodiscard]] bool modal() const override { return true; }

    /// Tells that what lies under the editor darkens.
    ///
    /// @return true
    [[nodiscard]] bool backdrop() const override { return true; }

    /// Lays the editor out for the view. In a match the frame is the match
    /// dialog's class and point size, not the window's class.
    ///
    /// @param placed_on what the screen is placed on
    void lay_out(const LayerView& placed_on) override {
        const kit::Frame next = frame_for(placed_on);
        const bool extra = runtime_.mouse_extra_buttons_;
        if (laid_ && next.area.width == frame_.area.width &&
            next.area.height == frame_.area.height && next.size_class == frame_.size_class &&
            next.scale_percent == frame_.scale_percent &&
            next.reduced_motion == frame_.reduced_motion && extra == state_.extra_buttons)
            return;
        frame_ = next;
        state_.extra_buttons = extra;
        laid_ = true;
        relayout();
    }

    /// Draws the editor.
    ///
    /// @param canvas where it draws
    void draw(const kit::Canvas& canvas) const override {
        editor::draw_editor(canvas, list_, editor::window_size(frame_));
    }

    /// Takes a pointer.
    ///
    /// @param kind pointer_move, pointer_down or pointer_up
    /// @param button the pointer's button
    /// @param at the pointer, in the editor's points
    /// @param finger_reach a finger's reach; 0 for a mouse
    /// @return what the editor did
    LayerAnswer
    pointer(ScreenInputKind kind, uint8_t button, kit::Point at, int32_t finger_reach) override {
        editor::PointerKind moved = editor::PointerKind::move;
        if (kind == ScreenInputKind::pointer_down)
            moved = editor::PointerKind::down;
        else if (kind == ScreenInputKind::pointer_up)
            moved = editor::PointerKind::up;
        else if (kind != ScreenInputKind::pointer_move)
            return LayerAnswer::none;
        return take(
            editor::editor_pointer(editor_, state_, list_, moved, button, at, finger_reach)
        );
    }

    /// Takes a key, as the kit names it. Escape's fixed job is the editor's.
    ///
    /// @param pressed the key
    /// @return what the editor did
    LayerAnswer key(kit::Key pressed, uint32_t /*sdl_key*/) override {
        return take(editor::editor_key(editor_, state_, list_, pressed));
    }

    /// While a capture or Find by key is listening, every key is the
    /// chord's, including Escape. Once the chord is finished the prompt is an
    /// ordinary dialog.
    ///
    /// @param sdl_key the key code
    /// @param modifiers the SDL modifier bits
    /// @return what the editor did; pass when the key is ordinary text
    LayerAnswer shortcut(uint32_t sdl_key, uint16_t modifiers) override {
        const bool listening = (state_.prompt == editor::Prompt::capture &&
                                state_.capture.state == editor::CaptureState::waiting) ||
                               (state_.prompt == editor::Prompt::find && !state_.finding.chord);
        if (listening) {
            const keys::InputEvent event =
                capture_key_event(sdl_key, modifiers, true, false, capture_scancode(sdl_key));
            return take(editor::editor_capture_input(editor_, state_, event));
        }
        if (state_.prompt != editor::Prompt::none)
            return LayerAnswer::pass;
        // Primary+V on the list reads the clipboard, and Primary+Z takes the
        // last change back. A focused search keeps the layer's own keys.
        if (layer_paste(sdl_key, modifiers)) {
            if (editor::focused_field(state_, list_))
                return LayerAnswer::pass;
            return take({editor::EditorAction::read_clipboard, {}, true});
        }
        if (undo_key(sdl_key, modifiers) && !editor::focused_field(state_, list_))
            return take(
                editor::editor_command(editor_, state_, list_, editor::EditorCommand::undo)
            );
        return LayerAnswer::pass;
    }

    /// Y finds, B goes back, Menu saves, X adds a key, and the shoulders are Tab.
    ///
    /// @param action the gamepad action
    /// @return what the editor did; pass for every other action
    LayerAnswer pad_action(pad::Action action) override {
        switch (action) {
        case pad::Action::library_search:
            return take(
                editor::editor_command(editor_, state_, list_, editor::EditorCommand::find)
            );
        case pad::Action::back:
            return take(
                editor::editor_command(editor_, state_, list_, editor::EditorCommand::back)
            );
        case pad::Action::default_button:
            return take(
                editor::editor_command(editor_, state_, list_, editor::EditorCommand::save)
            );
        case pad::Action::focus_previous:
            return take(editor::editor_key(editor_, state_, list_, kit::Key::back_tab));
        case pad::Action::focus_next:
            return take(editor::editor_key(editor_, state_, list_, kit::Key::tab));
        case pad::Action::library_action:
            return take(
                editor::editor_command(editor_, state_, list_, editor::EditorCommand::add_key)
            );
        default:
            return LayerAnswer::pass;
        }
    }

    /// Takes the controller's right stick: it scrolls the focused pane.
    ///
    /// @param notches the stick's turn
    /// @return what the editor did
    LayerAnswer pad_stick(float notches) override {
        return take(editor::editor_stick(editor_, state_, list_, notches));
    }

    /// Takes a turn of the wheel.
    ///
    /// @param at the pointer
    /// @param notches how far the wheel turned
    /// @return what the editor did
    LayerAnswer wheel(kit::Point at, float notches) override {
        return take(editor::editor_wheel(editor_, state_, list_, at, notches));
    }

    /// Takes text typed into the search.
    ///
    /// @param utf8 the text
    /// @return what the editor did
    LayerAnswer text(std::string_view utf8) override {
        return take(editor::editor_text(editor_, state_, list_, utf8));
    }

    /// Returns the search while it has the focus.
    ///
    /// @return its rectangle; none otherwise
    [[nodiscard]] std::optional<kit::Rect> text_field() const override {
        return editor::focused_field(state_, list_);
    }

    /// Returns what the editor draws.
    ///
    /// @return the display list
    [[nodiscard]] kit::DisplayList display_list() const override { return list_; }

    /// Returns the hover, the press and the focus.
    ///
    /// @return the interaction
    [[nodiscard]] kit::Interaction interaction() const override { return state_.interaction; }

    /// Closes when the match it was opened over ends, when the match screen
    /// comes up under an editor opened on the menus, and when Settings was
    /// under it and has gone. A match object still held after the menus
    /// return is not a match on screen, so the editor stays.
    ///
    /// @return close when the editor closes; otherwise none
    LayerAnswer tick() override {
        if (state_.in_match) {
            if (runtime_.screen_ != Screen::match || runtime_.match_ == nullptr ||
                runtime_.match_finished_)
                return LayerAnswer::close;
        } else if (runtime_.screen_ == Screen::match) {
            return LayerAnswer::close;
        } else if (over_settings_ && runtime_.oa_layer().find(kSettingsScreen) == nullptr) {
            return LayerAnswer::close;
        }
        if (state_.prompt == editor::Prompt::capture || state_.prompt == editor::Prompt::find)
            return take(editor::editor_capture_tick(editor_, state_, SDL_GetTicks()));
        return LayerAnswer::none;
    }

    /// True while a prompt covers the list: the capture, the clash question,
    /// Find by key, or Share, Paste or Import.
    ///
    /// @return true while a prompt is open
    [[nodiscard]] bool capturing() const { return state_.prompt != editor::Prompt::none; }

    /// Gives a key-up to the capture. The layer does not call the screen for
    /// a key-up, and a held Escape cancels when it is let go before a second.
    ///
    /// @param event the key
    void capture_event(const keys::InputEvent& event) {
        take(editor::editor_capture_input(editor_, state_, event));
    }

    /// Returns the editor's revision.
    ///
    /// @return the revision
    [[nodiscard]] uint64_t revision() const override { return revision_; }

    /// Tells that the editor draws its last layout alone, and each layout
    /// moves its revision, so a frame in which nothing changed keeps its
    /// drawing.
    ///
    /// @return true
    [[nodiscard]] bool revision_covers_drawing() const override { return true; }

    /// Has nothing of its own to close. The draft is kept only by SAVE.
    void close(bool /*by_key*/) override {}

    /// The editor, so a question's answer can change the draft.
    ///
    /// @return the editor
    [[nodiscard]] editor::Editor& editor() noexcept { return editor_; }

    /// Lays the editor out again after a question changed the draft.
    void changed() { relayout(); }

    /// Chooses a layout from the keep question and lays the list out again.
    ///
    /// @param id the layout
    /// @param keep whether the draft stays on top
    void apply_choice(std::string_view id, editor::KeepChanges keep) {
        editor_.choose_layout(id, keep);
        state_.list_scroll = 0;
        state_.details_scroll = 0;
        relayout();
    }

  private:

    /// Returns the frame the editor is laid out on.
    ///
    /// @param placed_on what the screen is placed on
    /// @return the frame; an empty one off the screen it was opened on
    [[nodiscard]] kit::Frame frame_for(const LayerView& placed_on) const {
        kit::Frame given{};
        if (state_.in_match) {
            if (!placed_on.match || placed_on.match_layout == nullptr)
                return given;
            const MatchDialogPlace place = OaLayer::match_dialog(
                *placed_on.match_layout, placed_on.interface_percent, placed_on.fit_safe_area
            );
            const kit::Metrics& metrics = kit::metrics_of(place.size_class);
            given.size_class = place.size_class;
            given.scale_percent = place.scale_percent == 0 ? 100 : place.scale_percent;
            given.area = {0, 0, metrics.dialog_width, metrics.dialog_height};
            given.reduced_motion = placed_on.frame.reduced_motion;
            return given;
        }
        if (placed_on.match)
            return given;
        return placed_on.frame;
    }

    /// Reads the keyboard picture's shape from the preference, or the system.
    void apply_keyboard_shape() {
        const kit::KeyboardShape detected = keyboard_shape(editor_.keyboard());
        state_.system_shape = detected;
        const auto found = runtime_.preference_values_.find(std::string(kKeyboardShapePreference));
        const std::string stored =
            found == runtime_.preference_values_.end() ? std::string() : found->second;
        if (stored == "iso")
            state_.shape = kit::KeyboardShape::iso;
        else if (stored == "jis")
            state_.shape = kit::KeyboardShape::jis;
        else if (stored == "ansi")
            state_.shape = kit::KeyboardShape::ansi;
        else
            state_.shape = detected;
        state_.shape_from_system = stored != "ansi" && stored != "iso" && stored != "jis";
    }

    /// Writes the shape the player chose. Absent stays absent while it follows the system.
    void remember_keyboard_shape() {
        std::string want = "system";
        if (!state_.shape_from_system) {
            if (state_.shape == kit::KeyboardShape::iso)
                want = "iso";
            else if (state_.shape == kit::KeyboardShape::jis)
                want = "jis";
            else
                want = "ansi";
        }
        const std::string key{kKeyboardShapePreference};
        const auto found = runtime_.preference_values_.find(key);
        if (want == "system" && found == runtime_.preference_values_.end())
            return;
        if (found != runtime_.preference_values_.end() && found->second == want)
            return;
        runtime_.preference_values_[key] = std::move(want);
        runtime_.preferences_dirty_ = true;
        runtime_.save_preferences();
    }

    /// Lays the editor out again.
    void relayout() {
        ++revision_;
        const kit::Fonts* fonts = runtime_.oa_layer().screen_fonts();
        if (fonts == nullptr || frame_.area.width <= 0 || frame_.area.height <= 0) {
            list_ = {};
            return;
        }
        list_ = editor::editor_layout(editor_, state_, frame_, *fonts);
    }

    /// Does what one input asked, and lays the editor out when it changed.
    ///
    /// @param result what the input asked
    /// @return what the layer does
    LayerAnswer take(editor::EditorResult result) {
        switch (result.action) {
        case editor::EditorAction::cancel:
            return LayerAnswer::close;
        case editor::EditorAction::save:
            return save_draft();
        case editor::EditorAction::ask_keep_changes:
            ask_keep(result.id);
            break;
        case editor::EditorAction::copy_text:
            copy_share_text();
            break;
        case editor::EditorAction::save_share:
            save_share_file(result.id);
            break;
        case editor::EditorAction::open_share_folder:
            open_saved_folder();
            break;
        case editor::EditorAction::read_clipboard:
            read_pasted_text();
            break;
        case editor::EditorAction::prepare_import:
            fill_import_offers();
            break;
        case editor::EditorAction::read_import:
            read_import_source(result.id);
            break;
        case editor::EditorAction::none:
            break;
        }
        if (result.action != editor::EditorAction::none || result.redraw) {
            remember_keyboard_shape();
            relayout();
            return LayerAnswer::redraw;
        }
        return LayerAnswer::none;
    }

    /// Writes the draft. A clash stays. A failed write shows a notice and
    /// leaves the editor open.
    ///
    /// @return close when the draft was written; redraw when it was not
    LayerAnswer save_draft() {
        if (!editor_.can_save()) {
            relayout();
            return LayerAnswer::redraw;
        }
        const int32_t face = editor_.scheme() == keys::PointerScheme::left_click
                                 ? oa::sim::gameplay_input::interface_left_click
                                 : oa::sim::gameplay_input::interface_right_click;
        if (runtime_.preferences_.interface_type != face) {
            runtime_.preferences_.interface_type = face;
            runtime_.preferences_dirty_ = true;
        }
        if (runtime_.match_ != nullptr)
            runtime_.match_->state().game.interface_type = face;
        std::string problem;
        if (!runtime_.save_key_editor(editor_.result(), problem)) {
            tell_write_failed(problem);
            relayout();
            return LayerAnswer::redraw;
        }
        return LayerAnswer::close;
    }

    /// Asks whether the draft stays on top of another layout.
    ///
    /// @param layout_id the layout the player is choosing
    void ask_keep(const std::string& layout_id) {
        if (question_up(runtime_.oa_layer()))
            return;
        const std::string id = layout_id;
        Runtime* runtime = &runtime_;
        auto answer = [runtime, id](int32_t button) {
            LayerScreen* found = runtime->oa_layer().find(editor::screen_name);
            if (found == nullptr)
                return true;
            auto* screen = static_cast<KeyEditorScreen*>(found);
            if (button == 0)
                screen->apply_choice(id, editor::KeepChanges::keep);
            else if (button == 1)
                screen->apply_choice(id, editor::KeepChanges::aside);
            else
                screen->changed();
            return true;
        };
        auto close_when = [runtime]() { return editor_gone(runtime->oa_layer()); };
        push_question(keep_question(editor_, id), std::move(answer), std::move(close_when));
    }

    /// Pushes a question: a QuestionScreen on the menus, a match prompt in a match.
    ///
    /// @param question the question
    /// @param answer what a button does
    /// @param close_when whether it must close
    void push_question(
        kit::Question question,
        std::function<bool(int32_t)> answer,
        std::function<bool()> close_when
    ) {
        if (state_.in_match) {
            runtime_.oa_layer().push(
                std::make_unique<MatchPrompt>(
                    runtime_.oa_layer(),
                    std::move(question),
                    std::move(answer),
                    std::move(close_when)
                )
            );
            return;
        }
        QuestionScreen::Host host;
        host.answer = std::move(answer);
        host.tick = std::move(close_when);
        auto screen = std::make_unique<QuestionScreen>(
            runtime_.oa_layer(), runtime_.screen_, std::move(question), std::move(host)
        );
        screen->show_on_any_menu();
        runtime_.oa_layer().push(std::move(screen));
    }

    /// Shows why a write failed, and leaves the editor open.
    ///
    /// @param reason the sentence the writer gave
    void tell_write_failed(std::string_view reason) {
        if (question_up(runtime_.oa_layer()))
            return;
        Runtime* runtime = &runtime_;
        auto close_when = [runtime]() { return editor_gone(runtime->oa_layer()); };
        kit::Notice notice = write_notice(editor_.text(), reason);
        if (state_.in_match) {
            runtime_.oa_layer().push(
                std::make_unique<MatchNotice>(
                    runtime_.oa_layer(), std::move(notice), std::move(close_when)
                )
            );
            return;
        }
        NoticeScreen::Host host;
        host.tick = std::move(close_when);
        auto screen = std::make_unique<NoticeScreen>(
            runtime_.oa_layer(), runtime_.screen_, std::move(notice), std::move(host)
        );
        screen->show_on_any_menu();
        runtime_.oa_layer().push(std::move(screen));
    }

    /// The date imported-from records. A check uses a fixed day.
    ///
    /// @return the date
    std::string import_stamp() {
        if (!runtime_.key_import_folder_.empty() || runtime_.options_.check_key_share)
            return "2026-10-27";
        const std::chrono::year_month_day ymd{
            std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now())
        };
        const int year = static_cast<int>(ymd.year());
        const unsigned month = static_cast<unsigned>(ymd.month());
        const unsigned day = static_cast<unsigned>(ymd.day());
        std::ostringstream out;
        out << year << '-';
        if (month < 10)
            out << '0';
        out << month << '-';
        if (day < 10)
            out << '0';
        out << day;
        return out.str();
    }

    /// True when the played mod stores one of the three Ctrl+F2 keys.
    ///
    /// @return true when a stored number is present
    bool eye_is_stored() {
        static constexpr std::array<std::string_view, 3> ids{
            "build.snap-override", "build.line", "build.rotate"
        };
        for (const std::string_view id : ids) {
            bool stored = false;
            std::ignore = view_rules::imported_key(id, [&](std::string_view name) {
                const std::optional<uint32_t> value =
                    runtime_.read_number(view_rules::view_settings_section, name);
                if (value)
                    stored = true;
                return value;
            });
            if (stored)
                return true;
        }
        return false;
    }

    /// Adds one file the import prompt can read, once.
    ///
    /// @param path the file
    /// @param tool tdraw or cnc-ddraw
    void add_offer(const fs::path& path, std::string tool) {
        std::error_code error;
        if (!fs::is_regular_file(path, error) || error)
            return;
        const std::string place = path.string();
        for (const editor::ImportOffer& offer : state_.import_offers)
            if (offer.path == place)
                return;
        editor::ImportOffer offer;
        offer.id = path.filename().string();
        offer.path = place;
        offer.tool = std::move(tool);
        offer.file = path.filename().string();
        offer.caption = offer.file;
        state_.import_offers.push_back(std::move(offer));
    }

    /// Fills the import offers from the check's folder, or the played mod.
    void fill_import_offers() {
        state_.import_offers.clear();
        state_.import_date = import_stamp();
        state_.import_eye = eye_is_stored();
        state_.import_chosen = -1;
        const fs::path folder = runtime_.key_import_folder_.empty() ? runtime_.played_mod_folder()
                                                                    : runtime_.key_import_folder_;
        if (runtime_.key_import_folder_.empty()) {
            std::string settings_name = "totala.ini";
            if (const oa::data::mod_profile::ModProfile* profile = runtime_.mod_profile()) {
                if (!profile->identity.settings_file.empty())
                    settings_name = profile->identity.settings_file;
            }
            add_offer(folder / settings_name, "tdraw");
        }
        add_offer(folder / "Settings.ini", "tdraw");
        add_offer(folder / "ddraw.ini", "cnc-ddraw");
    }

    /// The file a source button reads.
    ///
    /// @param which "tdraw" or "ddraw"
    /// @return the file
    [[nodiscard]] fs::path import_file(std::string_view which) const {
        const bool ddraw = which == "ddraw";
        const char* leaf = ddraw ? "ddraw.ini" : "Settings.ini";
        if (!runtime_.key_import_folder_.empty())
            return runtime_.key_import_folder_ / leaf;
        for (const editor::ImportOffer& offer : state_.import_offers) {
            const bool match = ddraw ? offer.tool == "cnc-ddraw" : offer.tool == "tdraw";
            if (match && !offer.path.empty())
                return fs::path(offer.path);
        }
        return runtime_.played_mod_folder() / leaf;
    }

    /// The mod's stored Ctrl+F2 keys, as a legacy read.
    ///
    /// @return the three slider actions that map to a chord
    keys::LegacyRead eye_read() {
        keys::LegacyRead read;
        static constexpr std::array<std::string_view, 3> ids{
            "build.snap-override", "build.line", "build.rotate"
        };
        uint32_t line = 1;
        for (const std::string_view id : ids) {
            const std::optional<view_rules::ImportedKey> imported =
                view_rules::imported_key(id, [&](std::string_view name) {
                    return runtime_.read_number(view_rules::view_settings_section, name);
                });
            if (!imported)
                continue;
            const actions::Action* action = actions::find(id);
            if (action == nullptr)
                continue;
            const std::optional<keys::Chord> chord = keys::legacy_key_chord(
                imported->code, capture_scancode(imported->code), action->by
            );
            if (!chord)
                continue;
            keys::LegacyKey key;
            key.action = std::string(id);
            key.chords = keys::imported_list(*action, *chord);
            key.source_line = std::string(imported->setting);
            key.line = line;
            ++line;
            read.keys.push_back(std::move(key));
        }
        return read;
    }

    /// Shows an import preview.
    ///
    /// @param read the keys
    /// @param tool the tool name
    /// @param file the file name
    void show_import(keys::LegacyRead read, std::string tool, std::string file) {
        state_.importing = editor::import_preview(editor_, read, tool, file);
        state_.import_chosen = 0;
        state_.exchange_scroll = 0;
        state_.prompt = editor::Prompt::import;
    }

    /// Reads the source the import prompt named.
    ///
    /// @param which "eye", "tdraw", "ddraw" or "choose"
    void read_import_source(const std::string& which) {
        if (which == "eye") {
            show_import(eye_read(), "eye", "Ctrl+F2");
            return;
        }
        if (which == "choose" &&
            !skip_file_dialog(runtime_.options_.unattended, runtime_.options_.check_key_share)) {
#if OA_NATIVE_FOLDER_DIALOG
            const std::string title =
                std::string(oa::data::languages::interface_text("Choose another file…"));
            const PickedPath picked =
                pick_key_file(runtime_.sdl_.window, false, runtime_.played_mod_folder(), title);
            if (picked.cancelled)
                return;
            if (picked.failed) {
                tell_write_failed(picked.error);
                return;
            }
            const std::string leaf = picked.path.filename().string();
            std::string lowered = leaf;
            for (char& character : lowered) {
                if (character >= 'A' && character <= 'Z')
                    character = static_cast<char>(character - 'A' + 'a');
            }
            const bool ddraw_file = lowered == "ddraw.ini";
            const std::vector<uint8_t> bytes = read_capped(picked.path, kLegacyReadCap);
            show_import(
                ddraw_file ? keys::read_cnc_ddraw(bytes) : keys::read_tdraw_settings(bytes),
                ddraw_file ? "cnc-ddraw" : "tdraw",
                leaf
            );
            return;
#endif
        }
        const bool ddraw = which == "ddraw";
        const fs::path path = import_file(ddraw ? "ddraw" : "tdraw");
        const std::vector<uint8_t> bytes = read_capped(path, kLegacyReadCap);
        show_import(
            ddraw ? keys::read_cnc_ddraw(bytes) : keys::read_tdraw_settings(bytes),
            ddraw ? "cnc-ddraw" : "tdraw",
            path.filename().string()
        );
    }

    /// Puts the share text on the clipboard.
    void copy_share_text() {
        const editor::ShareSummary summary = editor::share_summary(editor_);
        if (!SDL_SetClipboardText(summary.text.c_str())) {
            const char* reason = SDL_GetError();
            tell_write_failed(reason == nullptr ? "The clipboard was not set." : reason);
        }
    }

    /// Writes the shared package. A check names the path; otherwise the
    /// player does, or the file lands in Keys/Shared.
    ///
    /// @param layout_id the folded id
    void save_share_file(const std::string& layout_id) {
        const oakeys::Manifest manifest = editor::share_manifest(editor_);
        if (manifest.id.empty() || manifest.id != layout_id)
            return;
        std::string problem;
        const std::optional<std::vector<uint8_t>> packed = pack_share(manifest, problem);
        if (!packed) {
            tell_write_failed(problem);
            return;
        }
        fs::path out = runtime_.key_share_save_path_;
        const fs::path shared = runtime_.user_folder_ / std::string(key_library::keys_folder_name) /
                                "Shared" / (layout_id + ".oakeys");
        if (out.empty()) {
            if (skip_file_dialog(runtime_.options_.unattended, runtime_.options_.check_key_share)) {
                out = shared;
            } else {
#if OA_NATIVE_FOLDER_DIALOG
                const std::string title =
                    std::string(oa::data::languages::interface_text("Save file…"));
                const PickedPath picked =
                    pick_key_file(runtime_.sdl_.window, true, shared.parent_path(), title);
                if (picked.cancelled)
                    return;
                if (picked.failed) {
                    tell_write_failed(picked.error);
                    return;
                }
                out = picked.path.empty() ? shared : picked.path;
#else
                out = shared;
#endif
            }
        }
        std::error_code made;
        if (!out.parent_path().empty())
            fs::create_directories(out.parent_path(), made);
        if (made || !key_library::write_package(out, *packed, &problem)) {
            if (problem.empty())
                problem = "The file was not written.";
            tell_write_failed(problem);
            return;
        }
        state_.share_place = path_to_utf8(out);
    }

    /// Opens the folder the shared package was written to.
    void open_saved_folder() {
        if (state_.share_place.empty())
            return;
        const fs::path file = path_from_utf8(state_.share_place);
        const fs::path folder = file.has_parent_path() ? file.parent_path() : file;
        std::ignore = runtime_.open_player_folder(folder);
    }

    /// Reads the clipboard, at most 256 KiB, and opens the paste prompt.
    void read_pasted_text() {
        char* clipboard = SDL_GetClipboardText();
        std::vector<uint8_t> bytes;
        if (clipboard != nullptr) {
            const std::size_t length = std::strlen(clipboard);
            const std::size_t used = length < kClipboardCap ? length : kClipboardCap;
            bytes.assign(
                reinterpret_cast<const uint8_t*>(clipboard),
                reinterpret_cast<const uint8_t*>(clipboard) + used
            );
            SDL_free(clipboard);
        }
        state_.paste = editor::paste_preview(editor_, bytes);
        state_.exchange_scroll = 0;
        state_.prompt = editor::Prompt::paste;
    }

    Runtime& runtime_;            ///< the runtime
    editor::Editor editor_;       ///< the draft
    editor::ScreenState state_{}; ///< the list, the focus and the search
    bool over_settings_{false};   ///< Settings was under the editor when it opened
    kit::Frame frame_{};          ///< the frame it is laid out on
    bool laid_{false};            ///< lay_out has built the list
    kit::DisplayList list_{};     ///< what it draws
    uint64_t revision_{};         ///< counts the changes to what it draws
};

bool Runtime::key_editor_capturing() {
    LayerScreen* found = oa_layer().find(editor::screen_name);
    if (found == nullptr)
        return false;
    return static_cast<KeyEditorScreen*>(found)->capturing();
}

void Runtime::key_editor_capture_event(const SDL_Event& event) {
    if (event.type != SDL_EVENT_KEY_UP)
        return;
    LayerScreen* found = oa_layer().find(editor::screen_name);
    if (found == nullptr)
        return;
    const auto scan = static_cast<uint32_t>(event.key.scancode);
    const uint16_t scancode =
        scan > 0 && scan < keys::scancode_limit ? static_cast<uint16_t>(scan) : uint16_t{0};
    const keys::InputEvent input = capture_key_event(
        static_cast<uint32_t>(event.key.key),
        static_cast<uint16_t>(static_cast<uint32_t>(event.key.mod)),
        false,
        event.key.repeat,
        scancode
    );
    static_cast<KeyEditorScreen*>(found)->capture_event(input);
}

bool Runtime::open_key_editor(KeyEditorRequest request) {
    if (oa_layer().screen_fonts() == nullptr)
        return false;
    if (oa_layer().find(editor::screen_name) != nullptr)
        return false;

    const bool in_match = screen_ == Screen::match;
    if (in_match) {
        if (match_ == nullptr || match_finished_)
            return false;
        if (!ingame_menu_column_shown()) {
            if (pause_menu_shown() || oa::ui::frontend_dialogs::dialog_count() != 0)
                return false;
            show_match_pause_menu();
            if (!ingame_menu_column_shown())
                return false;
        }
    } else if (oa::ui::frontend_dialogs::dialog_count() != 0) {
        return false;
    }

    const LayerScreen* top = oa_layer().top();
    const bool library_top = top != nullptr && top->name() == kLibraryScreen;
    // The Library is modal. EDIT KEYS… covers it the way the editor covers Settings.
    if (!library_top) {
        const bool settings_up = oa_layer().find(kSettingsScreen) != nullptr;
        if (settings_up && !request.over_settings)
            return false;
        if (request.over_settings) {
            if (top == nullptr || top->name() != kSettingsScreen)
                return false;
        } else if (oa_layer().modal_shown(in_match)) {
            return false;
        }
    }

    editor::EditorInputs inputs;
    fill_key_editor(inputs);
    play_ui_sound(kOpenSound, 0);
    oa_layer().push(
        std::make_unique<KeyEditorScreen>(*this, std::move(inputs), std::move(request), in_match)
    );
    return true;
}

bool Runtime::stage_clipboard_text(std::string_view text) {
    const std::string owned(text);
    return SDL_SetClipboardText(owned.c_str());
}

} // namespace oa::app
