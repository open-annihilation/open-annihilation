// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The key editor's model. It resolves every row from the base, the chosen
// layout, the mod's data and the player's draft, and it changes nothing
// outside that draft. A host writes the draft when the player saves.
#pragma once

#include "oa/data/input_actions/actions.hpp"
#include "oa/formats/oakeys/manifest.hpp"
#include "oa/ui/key_bindings/layout.hpp"
#include "oa/ui/key_editor/text.hpp"
#include "oa/ui/kit/looks.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace oa::ui::key_editor {

namespace kit = oa::ui::kit;

/// One installed layout the editor can switch to, beside OA default.
struct LayoutSummary {
    std::string id{};                       ///< the layout's id
    std::string name{};                     ///< the name players see
    std::string author{};                   ///< who wrote it
    std::string version{};                  ///< the version, as shown
    oa::ui::key_bindings::Layout layout{};  ///< its bindings
    oa::formats::oakeys::Helpers helpers{}; ///< the helpers it sets
};

/// Everything the editor is allowed to read. The draft starts as `own`.
struct EditorInputs {
    oa::ui::key_bindings::Base base{oa::ui::key_bindings::Base::oa_default}; ///< the base
    bool has_chosen{false};              ///< a layout other than OA default is in use
    LayoutSummary chosen{};              ///< that layout, when has_chosen
    oa::ui::key_bindings::ModData mod{}; ///< the played mod's letters
    std::vector<std::string> hacks_on{}; ///< the hacks that are on
    oa::ui::key_bindings::PointerScheme scheme{
        oa::ui::key_bindings::PointerScheme::right_click
    }; ///< the pointer scheme
    oa::ui::key_bindings::KeyboardMap keyboard{oa::ui::key_bindings::KeyboardMap::us()}; ///< keys
    oa::ui::key_bindings::Platform platform{}; ///< which key Primary means
    oa::ui::key_bindings::Layout own{};        ///< the player's own changes, the draft's start
    oa::formats::oakeys::Helpers helpers{};    ///< the draft's helpers at the start
    bool number_keys_select_groups{false};     ///< 3.1c's SwitchAlt, as it is saved
    bool developer_mode{false};                ///< Developer keys are listed while this is set
    bool hold_to_confirm_setting{true};        ///< Settings › Accessibility, for the details' line
    std::string mod_name{};                    ///< the played mod's name; empty with no mod
    std::vector<LayoutSummary> installed{};    ///< layouts installed beside OA default
    TextHooks text{};                          ///< how words are looked up
};

/// Which rows a list keeps.
enum class Filter : uint8_t {
    all,       ///< every action in the category
    changed,   ///< actions the draft names
    clashes,   ///< actions in a pair find_clashes reports
    unbound,   ///< actions with no chord
    latchable, ///< actions the registry marks latchable
};

/// One category in the nav or the drop-down.
struct CategoryEntry {
    std::string id{};                      ///< the category; a heading's starts with "heading."
    std::string title{};                   ///< the words, already looked up
    kit::KeyTint tint{kit::KeyTint::none}; ///< the tint square; none where the kit has none
    std::string letter{};  ///< the letter on the square; empty where this list has none
    std::size_t count{0};  ///< the actions it holds, before a filter or a search
    bool read_only{false}; ///< a heading, or actions this version does not know
};

/// A chord the draft took, so resolution dropped it from the other action.
///
/// The pair is no longer in the resolved chords: the draft's action holds the
/// key and the other action lost it. The card, the search and a capture still
/// name both. SAVE does not wait on this pair. It waits only on a clash that
/// resolution kept, and that the draft takes part in.
struct DraftTake {
    std::string winner{};                ///< the action that kept the chord
    std::string loser{};                 ///< the action that lost it
    oa::ui::key_bindings::Chord chord{}; ///< the chord, as the loser had bound it
};

/// One action in the list.
struct Row {
    std::string id{};                      ///< the action id
    std::string name{};                    ///< the name, already looked up
    std::string aside{};                   ///< the quiet aside, already looked up
    std::string where{};                   ///< the area and where it works, for a group heading
    kit::KeyMark mark{kit::KeyMark::none}; ///< clash, unbound, changed, fixed, or none
    std::vector<oa::ui::key_bindings::Chord> chords{}; ///< the chords in use, at most four
    bool read_only{false};                             ///< an action this version does not know
    std::string category{};                            ///< the category id the row belongs to
};

/// One chord in the details, and why it is bound.
struct ChordDetail {
    oa::ui::key_bindings::Chord chord{}; ///< the chord
    std::string explanation{};           ///< what it is, already looked up
    oa::ui::key_bindings::Step step{oa::ui::key_bindings::Step::base}; ///< which step bound it
    bool fixed{false};    ///< Escape's fixed chord: no remove
    bool removable{true}; ///< a press on ✕ takes it off
};

/// The chosen action's details.
struct Details {
    std::string id{};                      ///< the action id; empty when none is chosen
    std::string name{};                    ///< the name, already looked up
    std::string where{};                   ///< the area and where it works
    std::string kind{};                    ///< how it fires
    std::string base{};                    ///< the base chords and where they come from
    std::string description{};             ///< what it does
    std::string works_with{};              ///< the conditions; empty when it names none
    std::vector<ChordDetail> chords{};     ///< the chords in use
    bool latch_offered{false};             ///< the action can latch
    bool latch_on{false};                  ///< Latch is on
    bool confirm_offered{false};           ///< the action can require a hold
    bool confirm_on{false};                ///< Hold to confirm is on
    bool confirm_setting_off{false};       ///< Accessibility's hold is off: the line shows
    std::string reset{};                   ///< RESET TO <layout>, already looked up
    bool read_only{false};                 ///< nothing here can be changed
    kit::KeyTint tint{kit::KeyTint::none}; ///< the category tint
    std::string letter{};                  ///< the category letter
};

/// How many actions each filter keeps, across every category.
struct FilterCounts {
    std::size_t all{0};       ///< every listed action
    std::size_t changed{0};   ///< named by the draft
    std::size_t clashes{0};   ///< in a clash
    std::size_t unbound{0};   ///< no chord
    std::size_t latchable{0}; ///< the registry's flag
};

/// Whether the draft stays when the player chooses another layout.
enum class KeepChanges : uint8_t {
    keep,  ///< the draft stays on top of the new layout
    aside, ///< the draft is cleared and the old own folder is left as it is
};

/// What SAVE writes: the own manifest, SwitchAlt, and any layout just made.
struct SaveResult {
    oa::formats::oakeys::Manifest manifest{}; ///< the player's own changes
    bool number_keys{false};                  ///< SwitchAlt
    std::string layout_id{"oa-default"};      ///< the layout the changes sit on
    /// Installed layouts the editor created or renamed. The host writes each
    /// beside the own manifest.
    std::vector<oa::formats::oakeys::Manifest> layouts{};
};

/// Which picture the editor is showing.
enum class View : uint8_t {
    list,     ///< the list of actions
    keyboard, ///< the player's keyboard
    mouse,    ///< the mouse and its roles
};

/// What the keyboard picture is asked to draw.
struct KeyboardViewInputs {
    kit::KeyboardShape shape{kit::KeyboardShape::ansi}; ///< ANSI, ISO or JIS
    bool shape_from_system{true};                       ///< the shape follows the system
    /// 0 is Nothing. Otherwise one modifier bit: Shift, Ctrl, Alt or Meta.
    uint8_t layer{0};
};

/// A named key's word, already looked up. The code is the W3C code.
struct NamedLabel {
    std::string code{}; ///< the W3C code, such as Space
    std::string word{}; ///< the word the key shows
};

/// The words the keyboard picture uses where a key does not print.
struct Labels {
    std::vector<NamedLabel> keys{}; ///< one word per named key
};

/// One line of a key's card: where it is read, the action, and a note.
struct CardLine {
    std::string context{}; ///< where it is read, already looked up
    std::string action{};  ///< the action's short name, already looked up
    std::string note{};    ///< the source, the gesture or the player's change
    std::string id{};      ///< the action id
    bool fixed{false};     ///< Escape's fixed line on the chat line or the screens
};

/// One row of the mouse table, one role in one place.
struct RoleLine {
    std::string place{};    ///< the place, already looked up
    std::string place_id{}; ///< playing, minimap, build-button or whiteboard
    std::string button{};   ///< the button, already looked up; empty when the place is free
    std::string role{};     ///< the action's short name, or Free
    std::string action{};   ///< the action id; empty when the place is free
    bool free{false};       ///< the place has no role
};

/// The editor. Rows are what resolution would put in use. Nothing is written
/// until a host saves `result()`.
class Editor {
  public:

    /// Starts from the inputs. The draft is the own layout and its helpers.
    ///
    /// @param inputs the base, the layouts, the mod and the settings
    explicit Editor(EditorInputs inputs);

    /// The categories, in the order the nav shows them. A heading's id starts
    /// with "heading." and holds no rows of its own.
    ///
    /// @return the categories
    [[nodiscard]] std::vector<CategoryEntry> categories() const;

    /// The rows of one category, after a filter and a search.
    ///
    /// An empty category, or "all", is every category. The search is folded
    /// as the Library folds text: ASCII letters to lower case, other bytes
    /// unchanged. A row matches when every word of the search occurs in its
    /// name, its description, its id or its category's title.
    ///
    /// @param category the category id
    /// @param filter which rows to keep
    /// @param query the search; empty keeps every row the filter keeps
    /// @return the rows, in list order
    [[nodiscard]] std::vector<Row>
    rows(std::string_view category, Filter filter, std::string_view query) const;

    /// The details of one action. Empty id and name when the list has no such action.
    ///
    /// @param id the action id
    /// @return the details
    [[nodiscard]] Details details(std::string_view id) const;

    /// How many actions each filter keeps, with no search.
    ///
    /// @return the five counts
    [[nodiscard]] FilterCounts counts() const;

    /// Takes one chord off an action. The draft then names that action with
    /// the chords that remain. Escape's fixed chord on `chat.cancel` and
    /// `ui.screen-cancel` is refused, and so is any change to an action this
    /// version does not know.
    ///
    /// @param id the action id
    /// @param index the chord, counting from 0 in the order `details` lists them
    /// @return true when the draft changed
    bool remove_chord(std::string_view id, std::size_t index);

    /// Drops the action from the draft, so it returns to the layout under it.
    ///
    /// @param id the action id
    /// @return true when the draft changed
    bool reset_action(std::string_view id);

    /// Clears the draft's known bindings and its helpers. Actions this version
    /// does not know stay, so a save writes them back. Number keys return to
    /// the saved SwitchAlt.
    void restore_defaults();

    /// Sets Latch for an action that can latch. Turning one on while the
    /// helper was unset writes a list of just that action.
    ///
    /// @param id the action id
    /// @param on the switch
    /// @return true when the draft changed
    bool set_latch(std::string_view id, bool on);

    /// Sets Hold to confirm. Turning one off while the helper was unset writes
    /// every confirmable action except that one.
    ///
    /// @param id the action id
    /// @param on the switch
    /// @return true when the draft changed
    bool set_confirm(std::string_view id, bool on);

    /// Sets Number keys select groups. It is saved with the layout and is not
    /// one of the own changes the summary counts.
    ///
    /// @param on the switch
    void set_number_keys(bool on);

    /// Chooses a layout. "oa-default" is the built-in. Setting the changes
    /// aside clears the draft and leaves the previous own folder for the host
    /// to leave untouched.
    ///
    /// @param id the layout id
    /// @param keep whether the draft stays on top
    /// @return true when the layout or the draft changed
    bool choose_layout(std::string_view id, KeepChanges keep);

    /// Makes a new layout from the current one, under this name, and chooses it.
    /// The draft's bindings and helpers become that layout's. An empty name
    /// does nothing.
    ///
    /// @param name the name the player typed
    /// @return true when the layout was made
    bool duplicate(std::string_view name);

    /// Renames the chosen layout. OA default is not renamed.
    ///
    /// @param name the name the player typed
    /// @return true when the name changed
    bool rename(std::string_view name);

    /// How many of the player's own changes sit on the layout: the draft's
    /// bindings, its latch list and, when set, its hold list. Number keys are
    /// not counted.
    ///
    /// @return the count the summary shows
    [[nodiscard]] std::size_t changes() const;

    /// How many changes are not saved yet, including a layout that was chosen,
    /// duplicated or renamed and the number-keys switch. A clean open is 0.
    ///
    /// @return the count the leave question names
    [[nodiscard]] std::size_t unsaved() const;

    /// How many clashing pairs the bindings in use have.
    ///
    /// A clash that lives only in the mod's data is included. SAVE does not
    /// wait on that pair: the layout SAVE writes does not name it.
    ///
    /// @return the count
    [[nodiscard]] std::size_t clashes() const;

    /// How many clashing pairs name an action the draft binds.
    ///
    /// SAVE waits while this is not zero. A clash in the mod's data, or in
    /// the chosen layout, is still listed and does not count here.
    ///
    /// @return the count the footer names
    [[nodiscard]] std::size_t draft_clashes() const;

    /// True when the draft takes part in no clash, so SAVE may write it.
    ///
    /// A clash in the mod's data alone leaves this true.
    ///
    /// @return true when a save is allowed
    [[nodiscard]] bool can_save() const;

    /// The manifest to write, SwitchAlt, and any layout just made or renamed.
    ///
    /// @return what a host writes on SAVE
    [[nodiscard]] SaveResult result() const;

    /// Number keys select groups, as the switch shows it.
    ///
    /// @return the switch
    [[nodiscard]] bool number_keys() const noexcept;

    /// The layout in use.
    ///
    /// @return "oa-default", or the chosen layout's id
    [[nodiscard]] std::string layout_id() const;

    /// The layout's name, already looked up for OA default.
    ///
    /// @return the name
    [[nodiscard]] std::string layout_name() const;

    /// The layout's author, already looked up for OA default.
    ///
    /// @return the author
    [[nodiscard]] std::string layout_author() const;

    /// The layout's version, as shown.
    ///
    /// @return the version
    [[nodiscard]] std::string layout_version() const;

    /// The installed layouts, not including OA default.
    ///
    /// @return the layouts, in the order they were given and then made
    [[nodiscard]] const std::vector<LayoutSummary>& installed() const noexcept;

    /// The word lookup the editor was given.
    ///
    /// @return the hooks
    [[nodiscard]] const TextHooks& text() const noexcept;

    /// The keyboard the chords are drawn with.
    ///
    /// @return the map the editor was given
    [[nodiscard]] const oa::ui::key_bindings::KeyboardMap& keyboard() const noexcept;

    /// Which key Primary means, for the chords the screen draws.
    ///
    /// @return the platform the editor was given
    [[nodiscard]] oa::ui::key_bindings::Platform platform() const noexcept;

    /// The pointer scheme the draft is showing. The list and the mouse view
    /// draw their roles from it. SAVE is what writes Interface Type.
    ///
    /// @return the scheme
    [[nodiscard]] oa::ui::key_bindings::PointerScheme scheme() const noexcept;

    /// Changes the draft's pointer scheme. The layout's bindings stay as they
    /// are. Interface Type is not written.
    ///
    /// @param scheme the scheme the views draw
    void set_scheme(oa::ui::key_bindings::PointerScheme scheme);

    /// True when the editor was opened over a match. The host sets the screen
    /// state; this is the mod's name for the header.
    ///
    /// @return the played mod's name; empty with no mod
    [[nodiscard]] const std::string& mod_name() const noexcept;

    /// Replaces the chords an action uses. An empty list writes an unbound
    /// draft, which overrides the layout under it. More than four chords are
    /// refused, and so is dropping Escape from a fixed action, or any change
    /// to an action this version does not know.
    ///
    /// @param id the action id
    /// @param chords the chords that remain, in list order
    /// @return true when the draft changed
    bool set_chords(std::string_view id, std::vector<oa::ui::key_bindings::Chord> chords);

    /// Drops the action's draft binding, so the layout under it returns.
    /// Escape on a fixed action returns with that layout. An action this
    /// version does not know is refused.
    ///
    /// @param id the action id
    /// @return true when a draft binding was removed
    bool clear_chords(std::string_view id);

    /// What binding this chord on an action would clash with.
    ///
    /// A character's position is ignored, and the chord's paired Primary form
    /// is asked as well. Each other action is named once. `a` is this action
    /// and `b` is the other. An action that lost the chord to the draft is
    /// named too.
    ///
    /// @param id the action the chord would be bound to
    /// @param chord the chord
    /// @return the clashes
    [[nodiscard]] std::vector<oa::ui::key_bindings::Clash>
    clashes_for(std::string_view id, const oa::ui::key_bindings::Chord& chord) const;

    /// The draft clash this action is in, with `a` set to this action.
    ///
    /// A clash that the draft does not name is not returned. A chord the draft
    /// took, so the other action lost it, is returned: the card can give it
    /// back. SAVE does not wait on that pair.
    ///
    /// @param id the action id
    /// @return the pair, or nothing when this action is not in a draft clash
    [[nodiscard]] std::optional<oa::ui::key_bindings::Clash> draft_clash(std::string_view id) const;

    /// Chords the draft took from another action.
    ///
    /// Resolution has already dropped the loser's chord. The list marks both
    /// actions, and the card settles the pair.
    ///
    /// @return one entry per chord the draft took
    [[nodiscard]] std::vector<DraftTake> draft_takes() const;

  private:

    struct Snapshot {
        std::string layout_id{};
        bool number_keys{false};
        oa::ui::key_bindings::PointerScheme scheme{
            oa::ui::key_bindings::PointerScheme::right_click
        };
        oa::ui::key_bindings::Layout own{};
        oa::formats::oakeys::Helpers helpers{};
        std::vector<std::string> names{}; ///< each installed id, a tab, and its name
    };

    EditorInputs inputs_{};
    oa::ui::key_bindings::Layout own_{};
    oa::formats::oakeys::Helpers helpers_{};
    bool number_keys_{false};
    bool has_chosen_{false};
    LayoutSummary chosen_{};
    std::vector<LayoutSummary> installed_{};
    std::vector<std::string> written_{}; ///< ids created or renamed, for result()
    Snapshot opened_{};

    /// Resolves the base, the chosen layout, the mod's data and the draft.
    ///
    /// @return the bindings SAVE would put in use
    [[nodiscard]] oa::ui::key_bindings::Resolved resolved() const;

    /// The resolution's inputs, pointing into this editor.
    ///
    /// @return the input; it lives no longer than the editor
    [[nodiscard]] oa::ui::key_bindings::ResolveInput resolve_input() const;

    /// The chosen layout's helpers with the draft's latch and confirm lists over them.
    ///
    /// @return the helpers in use
    [[nodiscard]] oa::formats::oakeys::Helpers effective_helpers() const;

    /// The keyboard picture for one shape and one modifier layer.
    friend kit::KeyboardLook
    keyboard_look(const Editor& editor, const KeyboardViewInputs& inputs, const Labels& labels);
    /// Every action on one key in one layer.
    friend std::vector<CardLine>
    key_card(const Editor& editor, std::string_view code, uint8_t layer);
    /// The mouse picture for one pointer scheme.
    friend kit::MouseLook
    mouse_look(const Editor& editor, oa::ui::key_bindings::PointerScheme scheme);
    /// The mouse table for one pointer scheme, by place.
    friend std::vector<RoleLine>
    mouse_table(const Editor& editor, oa::ui::key_bindings::PointerScheme scheme);
    /// The keys one action is bound to.
    friend std::vector<std::string> keys_of(const Editor& editor, std::string_view action);
};

/// The keyboard picture for one shape and one modifier layer.
///
/// Each key is labelled as the player's keyboard prints it, in capitals, or
/// with the named key's word. The short name is the plain press of exactly
/// that layer's modifiers that is read while playing. A key that only meets
/// play, such as Back on Escape, is used when nothing is read in play itself.
/// Nothing is drawn dashed as fixed. A clash is marked, and a key the draft
/// changed is marked when it does not clash. The layer's own modifier keys
/// are held.
///
/// @param editor the bindings, the keyboard and the platform
/// @param inputs the shape and the layer
/// @param labels the named keys' words
/// @return the picture K30 draws
[[nodiscard]] kit::KeyboardLook
keyboard_look(const Editor& editor, const KeyboardViewInputs& inputs, const Labels& labels);

/// Every action on one key in one layer, in every context, gestures included.
///
/// Escape under Nothing adds the chat line's and the screens' fixed lines.
/// Settings is an ordinary bound key: its line does not say always.
///
/// @param editor the bindings and the words
/// @param code the W3C code
/// @param layer 0, or one modifier bit
/// @return the lines, in list order
[[nodiscard]] std::vector<CardLine>
key_card(const Editor& editor, std::string_view code, uint8_t layer);

/// The mouse picture for one pointer scheme.
///
/// The scheme is the one passed in, not the draft's, so a caller can draw
/// either mode. The layout's bindings are not changed. A button's short name
/// is what it does while playing. Select and Command may share the left
/// button in the left-click scheme, and that is not a clash. Buttons 6 to 8
/// are not asked for here: the screen shows them once they have been pressed.
///
/// @param editor the bindings and the words
/// @param scheme the pointer scheme to draw
/// @return the picture K30 draws
[[nodiscard]] kit::MouseLook
mouse_look(const Editor& editor, oa::ui::key_bindings::PointerScheme scheme);

/// The mouse table for one pointer scheme, by place.
///
/// The places are while playing, the minimap, a build button and the
/// whiteboard. A place with no role has one Free row. The scheme is the one
/// passed in. The layout's bindings are not changed.
///
/// @param editor the bindings and the words
/// @param scheme the pointer scheme to draw
/// @return the rows, place by place
[[nodiscard]] std::vector<RoleLine>
mouse_table(const Editor& editor, oa::ui::key_bindings::PointerScheme scheme);

/// The keys an action is bound to, as W3C codes, for FIND ON KEYBOARD.
///
/// A letter is the key that prints it. A place is that key. A named key is
/// its key. The same code is listed once.
///
/// @param editor the bindings and the keyboard
/// @param action the action id
/// @return the codes, in key order
[[nodiscard]] std::vector<std::string> keys_of(const Editor& editor, std::string_view action);

/// The named keys' words: Space, Enter and Tab.
///
/// @param hooks how words are looked up
/// @return the labels
[[nodiscard]] Labels key_labels(const TextHooks& hooks);

/// Where a capture sits.
enum class CaptureState : uint8_t {
    waiting,  ///< modifiers may be held; the chord is not finished
    complete, ///< the chord is free
    clash,    ///< the chord is taken, and OK asks what to do
    refused,  ///< the chord is a fixed action's key
};

/// What one capture event did.
enum class CaptureEvent : uint8_t {
    kept,      ///< the capture did not change
    changed,   ///< the capture changed
    cancelled, ///< Escape went up before its second, so the prompt closes
};

/// How OK settles a chord that is taken.
enum class ClashChoice : uint8_t {
    replace, ///< the other action loses the chord
    swap,    ///< the other action takes the chord this one gave up
};

/// Which side of a draft clash keeps the chord.
enum class Settle : uint8_t {
    keep_here,  ///< the action the card was opened on keeps it
    keep_there, ///< the other action keeps it
};

/// One capture, from + ADD KEY or from Enter on a chord.
struct Capture {
    std::string action{};                   ///< the action being bound
    std::optional<std::size_t> replacing{}; ///< the chord being changed; none for + ADD KEY
    uint8_t held_modifiers{};               ///< modifiers held, in stored bits
    std::optional<oa::ui::key_bindings::Chord> chord{};                     ///< the finished chord
    oa::data::input_actions::By means{oa::data::input_actions::By::letter}; ///< letter or place
    oa::ui::key_bindings::Gesture starts{oa::ui::key_bindings::Gesture::press}; ///< how it finishes
    CaptureState state{CaptureState::waiting}; ///< free, taken, refused, or still waiting
    uint64_t escape_down_ms{}; ///< when bare Escape went down; 0 when it is not held
};

/// The line under the well, and whether OK can be pressed.
struct CaptureStatus {
    std::string line{};     ///< the free, clash or refused line; empty while waiting
    bool ok_enabled{false}; ///< OK asks, or binds a free chord
    bool refused{false};    ///< a fixed chord; OK stays disabled
    std::optional<oa::ui::key_bindings::Clash>
        clash{}; ///< the first clash, when the chord is taken
};

/// What applying a capture changed.
struct ApplyResult {
    bool applied{false};  ///< the draft changed
    std::string select{}; ///< the row to choose afterwards
};

/// One action a chord is bound to, for the search.
struct ChordUse {
    std::string id{};      ///< the action id
    std::string name{};    ///< the name, already looked up
    std::string context{}; ///< where it is read, already looked up
};

/// The modifier bits a chord stores for the physical modifiers an event holds.
///
/// Primary is the platform's command key. On a platform where Primary is Ctrl,
/// a physical Ctrl is stored as Primary. On a platform where Primary is the
/// command key, a physical Ctrl stays Ctrl and the command key is stored as
/// Primary. Alt and Shift pass through.
///
/// @param physical the event's modifier bits, never including Primary
/// @param platform which key Primary means
/// @return the bits a chord stores
[[nodiscard]] uint8_t
stored_modifiers(uint8_t physical, oa::ui::key_bindings::Platform platform) noexcept;

/// True when two chords are the key a player pressed.
///
/// A character's position is ignored. Two named keys with the same id match
/// even when one of them stored no position. Primary and the platform's
/// physical primary key count as one key, in either direction.
///
/// @param bound a chord in use
/// @param wanted the chord being asked
/// @param keyboard the map a letter and a place are read on
/// @param platform which key Primary means
/// @return true when a player would call them the same key
[[nodiscard]] bool chords_match(
    const oa::ui::key_bindings::Chord& bound,
    const oa::ui::key_bindings::Chord& wanted,
    const oa::ui::key_bindings::KeyboardMap& keyboard,
    oa::ui::key_bindings::Platform platform
);

/// The same chord with Primary and the platform's physical primary key exchanged.
///
/// Nothing when neither bit is set, or when both are. `same_key` does not
/// treat those two bits as one key, so a search and a clash ask both forms.
///
/// @param chord the chord
/// @param platform which key Primary means
/// @return the paired chord, or nothing
[[nodiscard]] std::optional<oa::ui::key_bindings::Chord>
paired_chord(const oa::ui::key_bindings::Chord& chord, oa::ui::key_bindings::Platform platform);

/// Where an action is read, already looked up: "while playing", or the tail
/// of the details' place.
///
/// @param editor the editor
/// @param action the action id
/// @return the phrase
[[nodiscard]] std::string context_phrase(const Editor& editor, std::string_view action);

/// Takes a key while the prompt is waiting.
///
/// A modifier key shows as held. A lone modifier finishes only for a modifier
/// action. The first other key finishes the chord. Bare Escape starts a
/// second; its release before that second cancels the prompt. A repeated key
/// changes nothing. Escape's second is `capture_tick`.
///
/// @param[in,out] capture the capture
/// @param editor the action, the keyboard and the platform
/// @param event the key
/// @return whether it changed, or the prompt was cancelled
CaptureEvent
capture_key(Capture& capture, const Editor& editor, const oa::ui::key_bindings::InputEvent& event);

/// Takes a mouse button in the well. The button number is the chord's, 1 to 8.
///
/// @param[in,out] capture the capture
/// @param editor the platform
/// @param event the button, already numbered as a chord stores it
void capture_button(
    Capture& capture, const Editor& editor, const oa::ui::key_bindings::InputEvent& event
);

/// Takes a wheel turn in the well. Positive y is up.
///
/// @param[in,out] capture the capture
/// @param editor the platform
/// @param event the wheel
void capture_wheel(
    Capture& capture, const Editor& editor, const oa::ui::key_bindings::InputEvent& event
);

/// Finishes Escape when it has been held for a second.
///
/// @param[in,out] capture the capture
/// @param editor the action and the keyboard
/// @param now_ms the time, from the same clock as the key event
void capture_tick(Capture& capture, const Editor& editor, uint64_t now_ms);

/// Sets letter or place. A named key, a button and a wheel stay as they are.
/// A letter no key on this keyboard types stays a place.
///
/// @param[in,out] capture the capture
/// @param editor the keyboard
/// @param means letter or place
void set_means(Capture& capture, const Editor& editor, oa::data::input_actions::By means);

/// Sets the gesture. A wheel turn and a modifier alone take none.
///
/// @param[in,out] capture the capture
/// @param editor the clashes are read again, because the gesture is part of the key
/// @param gesture a press, a double press or a hold
void set_gesture(Capture& capture, const Editor& editor, oa::ui::key_bindings::Gesture gesture);

/// The line under the well, and the clash or the refusal.
///
/// Waiting has an empty line and OK disabled. A fixed Escape in a context
/// that meets this action's is refused. Anything else taken is a clash, and
/// the line names every other action.
///
/// @param editor the draft and the words
/// @param capture the capture
/// @return the line, whether OK is enabled, and the clash or the refusal
[[nodiscard]] CaptureStatus capture_status(const Editor& editor, const Capture& capture);

/// Writes a finished capture. SWAP with no chord being changed writes nothing.
/// REPLACE drops the chord from every action it clashes with and chooses the
/// first of those. SWAP gives the first of those the chord this action gave up.
/// A fifth chord is refused. CANCEL does not call this.
///
/// @param[in,out] editor the draft
/// @param capture the finished capture
/// @param choice replace or swap
/// @return whether the draft changed, and which row to choose
[[nodiscard]] ApplyResult apply_capture(Editor& editor, const Capture& capture, ClashChoice choice);

/// Settles one draft clash. The action that loses the chord is unbound when
/// that was its only own chord, so the layout under it returns; a base chord
/// is overridden with what remains, which may be nothing.
///
/// @param[in,out] editor the draft
/// @param clash the pair, with `a` the action the card calls "here"
/// @param which which side keeps the chord
/// @return true when the draft changed
bool settle(Editor& editor, const oa::ui::key_bindings::Clash& clash, Settle which);

/// Every action bound to this chord, in list order, in every context.
///
/// The paired Primary form counts as the same chord. An action that lost the
/// chord to the draft is listed as well.
///
/// @param editor the editor
/// @param chord the chord
/// @return one row per action
[[nodiscard]] std::vector<ChordUse>
find_by_chord(const Editor& editor, const oa::ui::key_bindings::Chord& chord);

/// The chord a focused, empty search should list, or nothing when the key types.
///
/// A chord with Ctrl, Alt, Meta or Primary, a function key, Print Screen,
/// Pause, Insert, Page Up, Page Down, Home, End, a numpad key, or a mouse
/// side button is a search. A plain letter, digit or punctuation mark is not,
/// and neither is a lock or a modifier key on its own.
///
/// @param editor the platform and the keyboard
/// @param event the key or the button
/// @return the chord, or nothing when the search should take the key as text
[[nodiscard]] std::optional<oa::ui::key_bindings::Chord>
chord_to_find(const Editor& editor, const oa::ui::key_bindings::InputEvent& event);

} // namespace oa::ui::key_editor
