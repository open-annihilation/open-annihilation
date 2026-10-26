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

#include "oa/app/runtime.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/ui/engine_settings/notice.hpp"
#include "oa/ui/engine_settings/prompt.hpp"
#include "oa/ui/frontend_dialogs.hpp"
#include "oa/ui/key_bindings/chord.hpp"
#include "oa/ui/key_bindings/input.hpp"
#include "oa/ui/key_editor/screen.hpp"
#include "oa/ui/kit/chrome.hpp"

#include <SDL3/SDL.h>
#include "oa/ui/kit/components.hpp"
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

namespace {

namespace kit = oa::ui::kit;
namespace editor = oa::ui::key_editor;
namespace keys = oa::ui::key_bindings;
namespace settings = oa::ui::engine_settings;
namespace display = oa::ui::display_layout;
namespace pad = oa::ui::pad_controls;

/// The sound opening the editor plays, as Settings does.
constexpr std::string_view kOpenSound = "Options";
/// A question centred in the match dialog. "prompt" is QuestionScreen's name.
constexpr std::string_view kMatchPrompt = "key-editor-prompt";
/// A notice centred in the match dialog. "notice" is NoticeScreen's name.
constexpr std::string_view kMatchNotice = "key-editor-notice";
/// The layout's name, a text field. kit::Question has no field of its own.
constexpr std::string_view kNameScreen = "key-editor-name";
/// Settings' name on the layer.
constexpr std::string_view kSettingsScreen = "settings";
/// The Library's name on the layer. EDIT KEYS… covers it.
constexpr std::string_view kLibraryScreen = "library";
/// The name field's buttons are wider than the theme's OK, which CANCEL outgrows.
constexpr int32_t kNameButtonWidth = 96;
/// The name field.
constexpr kit::ControlId kNameField = 1;
/// The name screen's Cancel.
constexpr kit::ControlId kNameCancel = 2;
/// The name screen's accept. Its caption is OK; the control stays .save.
constexpr kit::ControlId kNameSave = 3;

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

/// Returns the name screen's height at a size class.
///
/// @param size_class the class
/// @return the height, in points
int32_t name_height(kit::SizeClass size_class) noexcept {
    const kit::Metrics& metrics = kit::metrics_of(size_class);
    return metrics.header_height + metrics.padding * 2 + metrics.field_height +
           metrics.footer_height;
}

/// Tells whether a question, a notice or the name screen is already up.
///
/// @param layer the layer
/// @return true while one shows
bool question_up(const OaLayer& layer) {
    return layer.find(kit::question_word) != nullptr || layer.find(kMatchPrompt) != nullptr ||
           layer.find(kit::notice_word) != nullptr || layer.find(kMatchNotice) != nullptr ||
           layer.find(kNameScreen) != nullptr;
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

/// The question that asks before unsaved changes are left.
///
/// @param model the editor
/// @return the question, named prompt
kit::Question leave_question(const editor::Editor& model) {
    const editor::TextHooks& hooks = model.text();
    const std::string count = std::to_string(model.unsaved());
    const std::string_view pattern =
        model.unsaved() == 1 ? editor::words::leave_question_one : editor::words::leave_question;
    kit::Question question;
    question.title = editor::show(hooks, editor::words::keys);
    kit::Paragraph body;
    body.text = editor::fill(editor::show(hooks, pattern), {{"n", count}});
    question.paragraphs.push_back(std::move(body));
    question.buttons.push_back(question_button(hooks, editor::words::leave, true, "leave"));
    question.buttons.push_back(
        question_button(hooks, editor::words::keep_editing, false, "keep-editing")
    );
    question.primary_button = 0;
    question.cancel_button = 1;
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

/// The layout's name. A kit question has buttons and no text field, so the
/// name is a field of its own, on the menus and in a match.
class NameScreen final : public LayerScreen {
  public:

    /// What a name does. False keeps the screen open: an empty name, or a
    /// name the editor refused.
    using Apply = std::function<bool(std::string_view name)>;

    /// Makes the screen, the field focused.
    ///
    /// @param layer the layer
    /// @param hooks how the captions are looked up
    /// @param apply what the accepted name does
    /// @param in_match the editor was opened over a match
    /// @param close_when tells, once a frame, whether the screen must close
    NameScreen(
        OaLayer& layer,
        editor::TextHooks hooks,
        Apply apply,
        bool in_match,
        std::function<bool()> close_when
    )
        : layer_(layer), hooks_(hooks), apply_(std::move(apply)), in_match_(in_match),
          close_when_(std::move(close_when)) {
        interaction_.focused = kNameField;
        interaction_.focus_shown = true;
    }

    /// Returns "key-editor-name".
    ///
    /// @return the name
    [[nodiscard]] std::string_view name() const override { return kNameScreen; }

    /// Returns where the screen shows: the class's notice width, on the menus
    /// centred in the window and in a match centred in the match dialog.
    ///
    /// @param placed_on what the screen is placed on
    /// @return its place; empty off the screen it was opened on
    [[nodiscard]] LayerPlacement placement(const LayerView& placed_on) const override {
        if (layer_.screen_fonts() == nullptr)
            return {};
        if (in_match_) {
            const MatchDialogPlace dialog = match_place(placed_on);
            if (!placed_on.match || placed_on.match_layout == nullptr)
                return {};
            const int32_t width = kit::metrics_of(dialog.size_class).notice_width;
            return place_in_dialog(dialog, width, name_height(dialog.size_class));
        }
        if (placed_on.match)
            return {};
        const kit::Metrics& metrics = kit::metrics_of(placed_on.frame.size_class);
        return centred_placement(
            placed_on, metrics.notice_width, name_height(placed_on.frame.size_class)
        );
    }

    /// Tells that the screen takes every input.
    ///
    /// @return true
    [[nodiscard]] bool modal() const override { return true; }

    /// Tells that what lies under the screen darkens.
    ///
    /// @return true
    [[nodiscard]] bool backdrop() const override { return true; }

    /// Lays the screen out at the class it shows at.
    ///
    /// @param placed_on what the screen is placed on
    void lay_out(const LayerView& placed_on) override {
        const kit::SizeClass next = size_class_of(placed_on);
        if (laid_ && next == size_class_)
            return;
        size_class_ = next;
        laid_ = true;
        rebuild();
    }

    /// Draws the window, the title, the field and the buttons.
    ///
    /// @param canvas where it draws
    void draw(const kit::Canvas& canvas) const override {
        if (!kit::can_draw(canvas))
            return;
        const kit::Point size = window_points();
        const kit::Rect whole{0, 0, size.x, size.y};
        kit::draw_window_face(canvas, whole);
        kit::paint(canvas, list_);
        kit::draw_window_edge(canvas, whole);
    }

    /// Takes a pointer. A press on the field focuses it. Cancel closes.
    /// The accept asks the editor, and stays open when the name is refused.
    ///
    /// @param kind pointer_move, pointer_down or pointer_up
    /// @param button the pointer's button
    /// @param at the pointer, in the screen's points
    /// @param finger_reach a finger's reach; 0 for a mouse
    /// @return what the screen did
    LayerAnswer
    pointer(ScreenInputKind kind, uint8_t button, kit::Point at, int32_t finger_reach) override {
        kit::PointerOutcome outcome{};
        if (kind == ScreenInputKind::pointer_move) {
            outcome = kit::pointer_move(interaction_, list_, at);
        } else if (kind == ScreenInputKind::pointer_down) {
            if (button != kLeftButton)
                return LayerAnswer::none;
            outcome = finger_reach > 0 ? kit::finger_down(interaction_, list_, at, finger_reach)
                                       : kit::pointer_down(interaction_, list_, at);
            if (outcome.control == kNameField)
                interaction_.focused = kNameField;
        } else if (kind == ScreenInputKind::pointer_up) {
            if (button != kLeftButton)
                return LayerAnswer::none;
            outcome = kit::pointer_up(interaction_, list_, at);
            if (outcome.result == kit::PointerResult::activated) {
                if (outcome.control == kNameCancel)
                    return LayerAnswer::close;
                if (outcome.control == kNameSave)
                    return accept();
                if (outcome.control == kNameField)
                    interaction_.focused = kNameField;
            }
        } else {
            return LayerAnswer::none;
        }
        if (outcome.result == kit::PointerResult::none)
            return LayerAnswer::none;
        rebuild();
        return LayerAnswer::redraw;
    }

    /// Takes a key. Escape closes without the name. Enter accepts it. Tab
    /// follows the field, Cancel and the accept.
    ///
    /// @param pressed the key
    /// @return what the screen did
    LayerAnswer key(kit::Key pressed, uint32_t /*sdl_key*/) override {
        if (pressed == kit::Key::escape)
            return LayerAnswer::close;
        if (pressed == kit::Key::enter)
            return accept();
        if (pressed == kit::Key::tab || pressed == kit::Key::back_tab) {
            interaction_.focused =
                kit::next_in_tab_order(list_, interaction_.focused, pressed == kit::Key::tab);
            interaction_.focus_shown = true;
            rebuild();
            return LayerAnswer::redraw;
        }
        if (pressed == kit::Key::space) {
            if (interaction_.focused == kNameCancel)
                return LayerAnswer::close;
            if (interaction_.focused == kNameSave)
                return accept();
        }
        if (interaction_.focused == kNameField && kit::edit_text(field_, pressed)) {
            rebuild();
            return LayerAnswer::redraw;
        }
        return LayerAnswer::none;
    }

    /// Y focuses the field. B, Menu and the shoulders become keys.
    ///
    /// @param action the gamepad action
    /// @return what the screen did; pass for every other action
    LayerAnswer pad_action(pad::Action action) override {
        if (action != pad::Action::library_search)
            return LayerAnswer::pass;
        interaction_.focused = kNameField;
        interaction_.focus_shown = true;
        rebuild();
        return LayerAnswer::redraw;
    }

    /// The name screen does not scroll.
    ///
    /// @return none
    LayerAnswer wheel(kit::Point /*at*/, float /*notches*/) override { return LayerAnswer::none; }

    /// Takes text typed into the field.
    ///
    /// @param utf8 the text
    /// @return what the screen did
    LayerAnswer text(std::string_view utf8) override {
        if (interaction_.focused != kNameField || !kit::insert_text(field_, utf8))
            return LayerAnswer::none;
        rebuild();
        return LayerAnswer::redraw;
    }

    /// Returns the field while it has the focus.
    ///
    /// @return its rectangle; none otherwise
    [[nodiscard]] std::optional<kit::Rect> text_field() const override {
        if (interaction_.focused != kNameField)
            return std::nullopt;
        const kit::Control* field = kit::control_of(list_, kNameField);
        if (field == nullptr)
            return std::nullopt;
        return field->rect;
    }

    /// Returns the title, the field and the buttons.
    ///
    /// @return the display list
    [[nodiscard]] kit::DisplayList display_list() const override { return list_; }

    /// Returns the hover, the press and the focus.
    ///
    /// @return the interaction
    [[nodiscard]] kit::Interaction interaction() const override { return interaction_; }

    /// Closes when the editor is gone.
    ///
    /// @return close when it must; otherwise none
    LayerAnswer tick() override {
        return close_when_ && close_when_() ? LayerAnswer::close : LayerAnswer::none;
    }

    /// Returns the screen's revision.
    ///
    /// @return the revision
    [[nodiscard]] uint64_t revision() const override { return revision_; }

    /// Has nothing of its own to close.
    void close(bool /*by_key*/) override {}

  private:

    /// Returns the class the screen is laid out at.
    ///
    /// @param placed_on what the screen is placed on
    /// @return the class; the one it has while it is off its screen
    [[nodiscard]] kit::SizeClass size_class_of(const LayerView& placed_on) const noexcept {
        if (in_match_) {
            if (!placed_on.match || placed_on.match_layout == nullptr)
                return size_class_;
            return match_place(placed_on).size_class;
        }
        if (placed_on.match)
            return size_class_;
        return placed_on.frame.size_class;
    }

    /// Returns the window's size at the class it is laid out at.
    ///
    /// @return the width and the height, in points
    [[nodiscard]] kit::Point window_points() const noexcept {
        return {kit::metrics_of(size_class_).notice_width, name_height(size_class_)};
    }

    /// Accepts the field. A refused name leaves the screen open.
    ///
    /// @return close when the name was taken; redraw when it was refused
    LayerAnswer accept() {
        if (apply_ && !apply_(field_.text)) {
            rebuild();
            return LayerAnswer::redraw;
        }
        return LayerAnswer::close;
    }

    /// Builds the title, the field and the buttons for the class.
    void rebuild() {
        ++revision_;
        list_ = {};
        const kit::Fonts* fonts = layer_.screen_fonts();
        const kit::Metrics& metrics = kit::metrics_of(size_class_);
        const int32_t width = metrics.notice_width;
        const int32_t height = name_height(size_class_);
        if (fonts == nullptr || width <= 0 || height <= 0)
            return;

        const std::string title = editor::show(hooks_, editor::words::name_layout);
        kit::Item heading;
        heading.role = kit::Role::text;
        heading.rect = {metrics.padding, 0, width - 2 * metrics.padding, metrics.header_height};
        heading.text = kit::cut_to_width(*fonts, kit::FontRole::regular, title, heading.rect.width);
        heading.font = kit::FontRole::regular;
        heading.align = kit::Align::left;
        heading.colour = kit::colour::text;
        heading.opacity = 256;
        list_.items.push_back(std::move(heading));

        kit::SearchLook field_look;
        field_look.field = field_;
        field_look.focused = interaction_.focused == kNameField;
        field_look.hovered = interaction_.hovered == kNameField;
        field_look.magnifier = false;
        const kit::Rect field_rect{
            metrics.padding,
            metrics.header_height + metrics.padding,
            width - 2 * metrics.padding,
            metrics.field_height,
        };
        kit::add_search(list_, field_rect, field_look, kNameField, "key-editor.name.field");

        const int32_t button_y =
            height - metrics.footer_height + (metrics.footer_height - metrics.button_height) / 2;
        const int32_t save_x = width - metrics.padding - kNameButtonWidth;
        const int32_t cancel_x = save_x - metrics.button_gap - kNameButtonWidth;
        kit::ButtonLook cancel_look;
        cancel_look.caption = editor::show(hooks_, editor::words::cancel);
        cancel_look.style = kit::ButtonStyle::plain;
        cancel_look.hovered = interaction_.hovered == kNameCancel;
        cancel_look.held =
            interaction_.pressed == kNameCancel && interaction_.hovered == kNameCancel;
        kit::add_button(
            list_,
            {cancel_x, button_y, kNameButtonWidth, metrics.button_height},
            cancel_look,
            kNameCancel,
            "key-editor.name.cancel"
        );
        kit::ButtonLook save_look;
        save_look.caption = editor::show(hooks_, editor::words::ok);
        save_look.style = kit::ButtonStyle::accent;
        save_look.hovered = interaction_.hovered == kNameSave;
        save_look.held = interaction_.pressed == kNameSave && interaction_.hovered == kNameSave;
        kit::add_button(
            list_,
            {save_x, button_y, kNameButtonWidth, metrics.button_height},
            save_look,
            kNameSave,
            "key-editor.name.save"
        );
        list_.tab_order = {kNameField, kNameCancel, kNameSave};
        if (interaction_.focus_shown) {
            if (const kit::Control* focused = kit::control_of(list_, interaction_.focused))
                kit::add_focus_ring(list_, focused->rect, true);
        }
    }

    OaLayer& layer_;                                     ///< the layer
    editor::TextHooks hooks_;                            ///< how the captions are looked up
    Apply apply_;                                        ///< what an accepted name does
    bool in_match_{false};                               ///< the editor was opened over a match
    std::function<bool()> close_when_;                   ///< whether the screen must close
    kit::SizeClass size_class_{kit::SizeClass::compact}; ///< the class it is laid out at
    bool laid_{false};                                   ///< lay_out has built the list
    kit::TextField field_{};                             ///< the name and its caret
    kit::Interaction interaction_{};                     ///< the hover, the press and the focus
    kit::DisplayList list_{};                            ///< what it draws
    uint64_t revision_{};                                ///< counts the changes to its look
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
        if (!request.select.empty())
            state_.selected = std::move(request.select);
        else if (state_.filter == editor::Filter::clashes && state_.selected.empty()) {
            const std::vector<editor::Row> clashing = editor_.rows({}, editor::Filter::clashes, "");
            if (!clashing.empty())
                state_.selected = clashing.front().id;
        }
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
        if (laid_ && next.area.width == frame_.area.width &&
            next.area.height == frame_.area.height && next.size_class == frame_.size_class &&
            next.scale_percent == frame_.scale_percent &&
            next.reduced_motion == frame_.reduced_motion)
            return;
        frame_ = next;
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

    /// While a capture is waiting, every key is the chord's, including
    /// Escape. Once the chord is finished the prompt is an ordinary dialog.
    /// An empty, focused search takes a chord with a modifier, or a key that
    /// types nothing, and lists what that chord does.
    ///
    /// @param sdl_key the key code
    /// @param modifiers the SDL modifier bits
    /// @return what the editor did; pass when the key is ordinary text
    LayerAnswer shortcut(uint32_t sdl_key, uint16_t modifiers) override {
        if (state_.prompt == editor::Prompt::capture &&
            state_.capture.state == editor::CaptureState::waiting) {
            const keys::InputEvent event =
                capture_key_event(sdl_key, modifiers, true, false, capture_scancode(sdl_key));
            return take(editor::editor_capture_input(editor_, state_, event));
        }
        if (state_.prompt != editor::Prompt::none)
            return LayerAnswer::pass;
        if (!editor::focused_field(state_, list_) || !state_.search.text.empty())
            return LayerAnswer::pass;
        const keys::InputEvent event =
            capture_key_event(sdl_key, modifiers, true, false, capture_scancode(sdl_key));
        const std::optional<keys::Chord> found = editor::chord_to_find(editor_, event);
        if (!found)
            return LayerAnswer::pass;
        state_.search_chord = *found;
        state_.search.text.clear();
        state_.search.caret = 0;
        state_.list_scroll = 0;
        relayout();
        return LayerAnswer::redraw;
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
        if (state_.prompt == editor::Prompt::capture)
            return take(editor::editor_capture_tick(editor_, state_, SDL_GetTicks()));
        return LayerAnswer::none;
    }

    /// True while the capture prompt or the clash prompt covers the list.
    ///
    /// @return true while a prompt is open
    [[nodiscard]] bool capturing() const { return state_.prompt != editor::Prompt::none; }

    /// True when this key is a chord the empty, focused search would list.
    ///
    /// A key-up and a repeat are asked as a fresh press: the search reads the
    /// press, and the feed must skip the release and the repeat as well.
    ///
    /// @param event the event dispatch_event is given
    /// @return true when the feed should not give the chord to the dispatcher
    [[nodiscard]] bool find_swallows(const SDL_Event& event) const {
        if (state_.prompt != editor::Prompt::none)
            return false;
        if (!editor::focused_field(state_, list_) || !state_.search.text.empty())
            return false;
        if (event.type != SDL_EVENT_KEY_DOWN && event.type != SDL_EVENT_KEY_UP)
            return false;
        const keys::InputEvent probe = capture_key_event(
            static_cast<uint32_t>(event.key.key),
            static_cast<uint16_t>(static_cast<uint32_t>(event.key.mod)),
            true,
            false,
            capture_scancode(static_cast<uint32_t>(event.key.key))
        );
        return editor::chord_to_find(editor_, probe).has_value();
    }

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
        case editor::EditorAction::ask_leave:
            ask_leave();
            break;
        case editor::EditorAction::ask_name:
            ask_name(result.id);
            break;
        case editor::EditorAction::none:
            break;
        }
        if (result.action != editor::EditorAction::none || result.redraw) {
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

    /// Asks before unsaved changes are left. Leaving closes the editor.
    void ask_leave() {
        if (question_up(runtime_.oa_layer()))
            return;
        Runtime* runtime = &runtime_;
        auto answer = [runtime](int32_t button) {
            LayerScreen* found = runtime->oa_layer().find(editor::screen_name);
            if (found == nullptr)
                return true;
            if (button == 0)
                runtime->oa_layer().close(found);
            return true;
        };
        auto close_when = [runtime]() { return editor_gone(runtime->oa_layer()); };
        push_question(leave_question(editor_), std::move(answer), std::move(close_when));
    }

    /// Asks the layout's name. An empty name leaves the screen open.
    ///
    /// @param which "duplicate" or "rename"
    void ask_name(const std::string& which) {
        if (question_up(runtime_.oa_layer()))
            return;
        const bool renaming = which == "rename";
        Runtime* runtime = &runtime_;
        auto apply = [runtime, renaming](std::string_view typed) {
            LayerScreen* found = runtime->oa_layer().find(editor::screen_name);
            if (found == nullptr)
                return true;
            auto* screen = static_cast<KeyEditorScreen*>(found);
            const bool made =
                renaming ? screen->editor().rename(typed) : screen->editor().duplicate(typed);
            if (!made)
                return false;
            screen->changed();
            return true;
        };
        auto close_when = [runtime]() { return editor_gone(runtime->oa_layer()); };
        runtime_.oa_layer().push(
            std::make_unique<NameScreen>(
                runtime_.oa_layer(),
                editor_.text(),
                std::move(apply),
                state_.in_match,
                std::move(close_when)
            )
        );
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

bool Runtime::key_editor_find_swallows(const SDL_Event& event) {
    LayerScreen* found = oa_layer().find(editor::screen_name);
    if (found == nullptr)
        return false;
    return static_cast<KeyEditorScreen*>(found)->find_swallows(event);
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

} // namespace oa::app
