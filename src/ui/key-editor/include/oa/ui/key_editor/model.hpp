// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The key editor's model. It resolves every row from the base, the chosen
// layout, the mod's data and the player's draft, and it changes nothing
// outside that draft. A host writes the draft when the player saves.
#pragma once

#include "oa/formats/oakeys/manifest.hpp"
#include "oa/ui/key_bindings/layout.hpp"
#include "oa/ui/key_editor/text.hpp"
#include "oa/ui/kit/looks.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace oa::ui::key_editor {

namespace kit = oa::ui::kit;

/// One installed layout the editor can switch to, beside OA default.
struct LayoutSummary {
    std::string id{};                              ///< the layout's id
    std::string name{};                            ///< the name players see
    std::string author{};                          ///< who wrote it
    std::string version{};                         ///< the version, as shown
    oa::ui::key_bindings::Layout layout{};         ///< its bindings
    oa::formats::oakeys::Helpers helpers{};        ///< the helpers it sets
};

/// Everything the editor is allowed to read. The draft starts as `own`.
struct EditorInputs {
    oa::ui::key_bindings::Base base{oa::ui::key_bindings::Base::oa_default}; ///< the base
    bool has_chosen{false};                   ///< a layout other than OA default is in use
    LayoutSummary chosen{};                   ///< that layout, when has_chosen
    oa::ui::key_bindings::ModData mod{};      ///< the played mod's letters
    std::vector<std::string> hacks_on{};      ///< the hacks that are on
    oa::ui::key_bindings::PointerScheme scheme{
        oa::ui::key_bindings::PointerScheme::right_click}; ///< the pointer scheme
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
    std::string id{};            ///< the category; a heading's starts with "heading."
    std::string title{};         ///< the words, already looked up
    kit::KeyTint tint{kit::KeyTint::none}; ///< the tint square; none where the kit has none
    std::string letter{};        ///< the letter on the square; empty where this list has none
    std::size_t count{0};        ///< the actions it holds, before a filter or a search
    bool read_only{false};       ///< a heading, or actions this version does not know
};

/// One action in the list.
struct Row {
    std::string id{};            ///< the action id
    std::string name{};          ///< the name, already looked up
    std::string aside{};         ///< the quiet aside, already looked up
    kit::KeyMark mark{kit::KeyMark::none}; ///< clash, unbound, changed, fixed, or none
    std::vector<oa::ui::key_bindings::Chord> chords{}; ///< the chords in use, at most four
    bool read_only{false};       ///< an action this version does not know
    std::string category{};      ///< the category id the row belongs to
};

/// One chord in the details, and why it is bound.
struct ChordDetail {
    oa::ui::key_bindings::Chord chord{}; ///< the chord
    std::string explanation{};           ///< what it is, already looked up
    oa::ui::key_bindings::Step step{oa::ui::key_bindings::Step::base}; ///< which step bound it
    bool fixed{false};                   ///< Escape's fixed chord: no remove
    bool removable{true};                ///< a press on ✕ takes it off
};

/// The chosen action's details.
struct Details {
    std::string id{};            ///< the action id; empty when none is chosen
    std::string name{};          ///< the name, already looked up
    std::string where{};         ///< the area and where it works
    std::string kind{};          ///< how it fires
    std::string base{};          ///< the base chords and where they come from
    std::string description{};   ///< what it does
    std::string works_with{};    ///< the conditions; empty when it names none
    std::vector<ChordDetail> chords{}; ///< the chords in use
    bool latch_offered{false};   ///< the action can latch
    bool latch_on{false};        ///< Latch is on
    bool confirm_offered{false}; ///< the action can require a hold
    bool confirm_on{false};      ///< Hold to confirm is on
    bool confirm_setting_off{false}; ///< Accessibility's hold is off: the line shows
    std::string reset{};         ///< RESET TO <layout>, already looked up
    bool read_only{false};       ///< nothing here can be changed
    kit::KeyTint tint{kit::KeyTint::none}; ///< the category tint
    std::string letter{};        ///< the category letter
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
    [[nodiscard]] std::vector<Row> rows(std::string_view category, Filter filter, std::string_view query) const;

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
    /// @return the count
    [[nodiscard]] std::size_t clashes() const;

    /// True when the bindings in use have no clash, so SAVE may write them.
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

    /// True when the editor was opened over a match. The host sets the screen
    /// state; this is the mod's name for the header.
    ///
    /// @return the played mod's name; empty with no mod
    [[nodiscard]] const std::string& mod_name() const noexcept;

  private:

    struct Snapshot {
        std::string layout_id{};
        bool number_keys{false};
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

    [[nodiscard]] oa::ui::key_bindings::Resolved resolved() const;
    [[nodiscard]] oa::ui::key_bindings::ResolveInput resolve_input() const;
    [[nodiscard]] oa::formats::oakeys::Helpers effective_helpers() const;
};

} // namespace oa::ui::key_editor
