// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The units, weapons and features a mutator hook reads and writes.
//
// A hook sees the pipeline's shared documents, one record at a time, and
// every change is kept in the journal below. The object must outlive every
// Lua state it is pushed into, and the document set must outlive the object:
// a script holds handles, not copies, and each access looks the record up
// again.
#pragma once

#include "oa/data/defs/layout.hpp"
#include "oa/data/mutation_rules/apply.hpp"
#include "oa/data/mutation_rules/documents.hpp"
#include "oa/data/mutators/clashes.hpp"
#include "oa/formats/soft_mutator.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct lua_State;

namespace oa {
class AssetStore;
}

namespace oa::data::mutators {

/// Reaches the records from Lua. The bindings are defined with the
/// implementation; a caller does not use this type.
struct CollectionsAccess;

/// Working storage for one collections object. Defined with the implementation.
struct CollectionsState;

/// The most field writes one mutator may journal on one turn.
///
/// The collections and the maps phase share one tally. A further write is
/// not stored, and it fails that mutator with
/// "this mutator has changed too many fields", even when the script catches
/// the error.
inline constexpr uint32_t mutator_write_limit = 65536;

/// The most notes one mutator may add on one collections object.
///
/// A further note is refused with "this mutator has made too many notes"; a
/// script that catches the error goes on.
inline constexpr uint32_t mutator_note_limit = 65536;

/// Which of these collections a mutator may change.
///
/// A flag is set when the mutator's manifest declares that layer. Reading is
/// allowed either way.
struct WriteGrant {
    bool units{};    ///< unit records and the download entries a removal drops
    bool weapons{};  ///< weapon records, including a weapon's damage block
    bool features{}; ///< feature records
    bool sides{};    ///< side data and the build lists
    bool tables{};   ///< other game-data tables
};

/// What a removal took out of the document set.
enum class RemovalKind : uint8_t {
    unit,           ///< a unit document
    weapon,         ///< one weapon section
    feature,        ///< one feature section
    download_entry, ///< one download section that named a removed unit
    document,       ///< a document left with no section
};

/// One record or download entry a hook removed.
struct RecordRemoval {
    std::string mutator; ///< mutator whose hook removed it
    RemovalKind kind{};  ///< which kind of removal
    std::string name;    ///< record or section name
    std::string path;    ///< document path
};

/// Why a hook left a note beside its writes.
enum class HookNoteKind : uint8_t {
    check_warned,                 ///< a typed write was outside the comfortable range and was kept
    hook_overwrote,               ///< a later hook replaced an earlier hook's value
    download_first_entry_changed, ///< removing the first download entry left a different unit first
    not_a_number,                 ///< a scale skipped a value that is not a number
    id_kept,   ///< a write of id on a weapon the game already numbered was left undone
    menu_note, ///< a moved button, a missing picture or a full AI list
};

/// One unit or weapon a hook cloned.
///
/// `sentence` is the journal line: "added unit NEW (copy of SRC)",
/// "replaced unit NAME (copy of SRC)", or the same for a weapon with
/// "at id N". `weapon_id` is that id, and 0 for a unit. `asked` is the
/// name the script wrote when Add stored the clone under a private name.
struct CloneRecord {
    std::string mutator;  ///< mutator whose hook cloned it
    bool weapon{};        ///< true for a weapon section, false for a unit
    std::string name;     ///< the stored name: the private name, or the name replaced
    std::string source;   ///< the record it was copied from, as the script wrote it
    int weapon_id{}; ///< free weapon id, from 1 to weapon_types - 1; 0 for a unit or a replacement
    std::string path;     ///< document path the copy was written to
    std::string sentence; ///< the journal line for this clone
    /// The name the script asked for when Add stored a different one. Empty
    /// when the asked name was stored as written, and when Replace wrote
    /// over the existing record.
    std::string asked;
    /// True when Replace put the clone in place of the record that was there.
    bool replaced{};
};

/// One binary file a clone serves under the new name.
///
/// `sentence` is "added path <new>, copy of <source>". The bytes are the
/// source file as the store held it when the clone ran.
struct ServedCopy {
    std::string mutator;                               ///< mutator whose clone served it
    std::string path;                                  ///< path the layer serves
    std::string source_path;                           ///< path the bytes were read from
    std::string sentence;                              ///< the journal line for this copy
    std::shared_ptr<const std::vector<uint8_t>> bytes; ///< the served bytes
    /// True when a Replace clone serves the copy in place of the replaced
    /// unit's own file, so the path may already exist.
    bool replaces{};
};

/// One note a hook produced, for the change journal.
struct HookNote {
    HookNoteKind kind{};               ///< which note
    std::vector<std::string> mutators; ///< mutators involved, earlier ones first
    std::string path;                  ///< document path
    std::string record;                ///< record name
    std::string key;                   ///< field name; empty when the note is not about one field
    std::string message;               ///< what happened, in plain words
};

/// Publishes `range`, `is`, `has`, `absent` and `not_` in a state.
///
/// The state is one the script host opened. The forms build the needs
/// `need_from_lua` reads, and their metatables cannot be read or replaced.
///
/// @param state state to publish into
void install_selection_forms(lua_State* state);

/// Reads one need from a Lua value.
///
/// A number `n` is `range(n, n)`. `true` is `range(1, 1)` and `false` is
/// `not_(range(1, 1))`. A string is `is` of that string. `absent()` is
/// `not_(has(""))`. `not_` of a `not_` is refused. Anything else is refused
/// with a message that names `field` when `field` is not empty.
///
/// @param state state that holds the value
/// @param index stack index of the value
/// @param field field the need applies to; empty when the caller has none yet
/// @param[out] need the need; unchanged when the call returns false
/// @param[out] error the reason; may be null
/// @return true when `need` was filled
[[nodiscard]] bool need_from_lua(
    lua_State* state,
    int index,
    std::string_view field,
    oa::formats::soft_mutator::Need& need,
    std::string* error
);

/// The units, weapons and features of one document set, as Lua collections.
///
/// The object must outlive every state `push_units`, `push_weapons`,
/// `push_features` or `set_context_collections` was called with. The document
/// set must outlive this object. A handle remembers the section it was given.
/// The name is kept for messages. Removing that section, or its document,
/// makes the handle fail. Removing a different section, or renaming the unit,
/// leaves the handle on its own section.
class Collections {
  public:

    /// Binds the collections to a document set.
    ///
    /// @param documents documents the hooks read and write; must outlive this object
    /// @param layout layout that names the folders; null uses the layout in force at each call
    Collections(
        oa::data::mutation_rules::DocumentSet& documents,
        const oa::data::defs::DataLayout* layout = nullptr
    );
    Collections(const Collections&) = delete;
    Collections& operator=(const Collections&) = delete;
    Collections(Collections&&) = delete;
    Collections& operator=(Collections&&) = delete;
    /// Drops the collections. Every state they were pushed into is already closed.
    ~Collections();

    /// Starts one mutator's turn.
    ///
    /// Writes and removals until `end_mutator` are journaled to `mutator` and
    /// refused unless `grant` allows that collection.
    ///
    /// @param mutator mutator name, as the player list spells it
    /// @param grant layers this mutator may write
    void begin_mutator(std::string mutator, WriteGrant grant);

    /// Ends the mutator's turn. Later writes are refused until the next begin.
    ///
    /// The write tally stays, so a later phase of the same mutator counts
    /// against the same limit.
    void end_mutator();

    /// True when this turn has passed `mutator_write_limit`.
    ///
    /// The write that crossed the limit sets the mark, and it stays for this
    /// mutator on this object. The pipeline fails the mutator after its hooks
    /// return, even when the script caught the error and returned normally.
    [[nodiscard]] bool write_limit_failed() const noexcept;

    /// True when this turn may journal one more field write.
    ///
    /// A refusal marks the mutator failed. A clone's fields and the maps
    /// phase ask here, so their rows share the collections' tally.
    [[nodiscard]] bool allow_field_write() noexcept;

    /// Counts one field write this turn has journaled.
    void count_field_write() noexcept;

    /// Names and paths the side and table bindings compute before they push.
    ///
    /// The object outlives the Lua callback. A callback fills it, returns,
    /// and only then pushes, so a Lua error cannot skip a destructor.
    struct SideScratch {
        std::vector<std::string> names;
        std::vector<std::string> raw_names;
        std::string text;
        std::string path;
        std::string record;
        std::string key;
        std::string builder;
        std::string side_path;
        std::string list_path;
        std::string side_error;
        std::string list_error;
        uint64_t visits = 0;
        uint64_t text_units = 0;
        uint64_t fields = 0;
        uint64_t comparisons = 0;
        bool scan = false;
        bool found = false;
        bool builder_unit = false;
        bool present = false;
        bool side_ready = false;
        bool list_ready = false;
        bool side_found = false;
        bool list_found = false;
        uint32_t count = 0;
        uint32_t slot = 0;
        std::size_t side_doc = 0;
        std::size_t list_doc = 0;
    };

    /// Pushes the units collection.
    ///
    /// @param state state to push into
    void push_units(lua_State* state);

    /// Pushes the weapons collection.
    ///
    /// @param state state to push into
    void push_weapons(lua_State* state);

    /// Pushes the features collection.
    ///
    /// @param state state to push into
    void push_features(lua_State* state);

    /// Sets `units`, `weapons` and `features` on a context table.
    ///
    /// @param state state that holds the table
    /// @param ctx_index stack index of the context table
    void set_context_collections(lua_State* state, int ctx_index);

    /// Returns the field writes so far, in the order the hooks made them.
    ///
    /// The span is invalidated by the next write.
    ///
    /// @return the writes
    [[nodiscard]] std::span<const oa::data::mutation_rules::FieldWrite> writes() const;

    /// Returns the removals so far, in the order the hooks made them.
    ///
    /// The span is invalidated by the next removal.
    ///
    /// @return the removals
    [[nodiscard]] std::span<const RecordRemoval> removals() const;

    /// Returns the notes so far, in the order the hooks made them.
    ///
    /// The span is invalidated by the next note.
    ///
    /// @return the notes
    [[nodiscard]] std::span<const HookNote> notes() const;

    /// Remembers the store a clone reads while a hook runs.
    ///
    /// The store must outlive the hooks. Null clears it.
    ///
    /// @param store the layered store, or null
    void bind_store(oa::AssetStore* store) { store_ = store; }

    /// Remembers how many weapon ids the mod numbers.
    ///
    /// Ids 0 to `count` - 1 take a slot. A clone takes a free id from 1 to
    /// `count` - 1, and a hook's write of a weapon `id` is held to that
    /// count. 256 unless the pipeline read the profile's `limits.weapon-types`.
    ///
    /// @param count weapon types the mod numbers
    void set_weapon_types(uint32_t count) noexcept { weapon_types_ = count; }

    /// How many weapon ids the mod numbers.
    ///
    /// @return the count last given to `set_weapon_types`, or 256
    [[nodiscard]] uint32_t weapon_types() const noexcept { return weapon_types_; }

    /// Remembers weapon sections whose ids the game already assigned.
    ///
    /// A later hook that writes `id` on one of these weapons does not change
    /// it, and the note names the mutator. A weapon a mutator brings has a
    /// new name and is not in this list, so a write of `id` on it is kept
    /// and the numbering pass reads it as that weapon's request.
    ///
    /// @param names section names, compared without ASCII case
    void keep_weapon_ids(std::span<const std::string> names);

    /// Reports whether a unit or a feature of this name is in the documents now.
    ///
    /// The maps hook checks a placed name here, on the data the mutators
    /// above it left: a unit an earlier hook cloned is there, and one a hook
    /// removed is not. The name is compared without ASCII case through the
    /// record index. With a state, the index visit and the matched name are
    /// charged as a lookup by name is. Call it with no local object still to
    /// destroy: the charge can raise.
    ///
    /// @param state the state to charge, or null to look without charging
    /// @param feature true for the features, false for the units
    /// @param sought the name the script wrote
    /// @return 1 when it is there, 0 when it is not, -1 when memory ran out
    [[nodiscard]] int has_record(lua_State* state, bool feature, std::string_view sought);

    /// Forgets a document the index has listed, so a later read sees it again.
    ///
    /// A hook that removed the document leaves the index holding that path.
    /// Bringing the file back clears that memory before the text is parsed
    /// again. A handle that still names a section of it then fails.
    ///
    /// @param path document path, '/' or '\\' separated
    void release_document(std::string_view path);

    /// Returns the clones made so far, in the order the hooks made them.
    ///
    /// The span is invalidated by the next clone.
    ///
    /// @return the clones
    [[nodiscard]] std::span<const CloneRecord> clones() const { return clones_; }

    /// Returns the binary copies served so far, in the order they were read.
    ///
    /// The span is invalidated by the next copy.
    ///
    /// @return the copies
    [[nodiscard]] std::span<const ServedCopy> served_copies() const { return served_; }

    /// Remembers one binary copy the pipeline will serve under a new path.
    ///
    /// The served index is updated, so a later clone sees the path as taken.
    ///
    /// @param copy the copy, including its bytes
    void add_served_copy(ServedCopy copy);

    /// Sets the clash mode the clone methods honour.
    ///
    /// While `set_clash_active` is set, Add gives a clone whose new name
    /// already exists a private name, and Replace puts that clone in place
    /// of the existing record. A collections object that has not been
    /// activated keeps the refusal "the new name already exists".
    ///
    /// @param mode the mode for this mutator
    void set_clash_mode(oa::formats::oamut::ClashMode mode) noexcept { clash_mode_ = mode; }

    /// Turns clash handling for clones on or off.
    ///
    /// The pipeline sets this for the mutator it is applying. A caller that
    /// never does leaves clones refusing an existing name.
    ///
    /// @param active true when this mutator's clash mode applies to clones
    void set_clash_active(bool active) noexcept { clash_active_ = active; }

    /// The clash mode last set. The default is replace.
    ///
    /// @return the mode
    [[nodiscard]] oa::formats::oamut::ClashMode clash_mode() const noexcept { return clash_mode_; }

    /// Reports whether clone clashes follow the mode last set.
    ///
    /// @return true when a clone may rename or replace an existing name
    [[nodiscard]] bool clash_active() const noexcept { return clash_active_; }

    /// Forgets the renames noted for the mutator that just ran.
    ///
    /// The pipeline calls it when each mutator's turn ends, so a name in the
    /// maps phase, or in a later mutator's turn, means the record of that name.
    void clear_renames() noexcept { renames_.clear(); }

    /// How many renames this mutator has noted. A name lookup walks them.
    ///
    /// @return the number of renames since the last `clear_renames`
    [[nodiscard]] std::size_t rename_count() const noexcept { return renames_.size(); }

    /// Remembers that this mutator stored `from` as `to`.
    ///
    /// @param weapon true for a weapon section, false for a unit
    /// @param from the name the mutator wrote
    /// @param to the private name
    void note_rename(bool weapon, std::string from, std::string to);

    /// The private name this mutator gave `name`, or empty when it kept it.
    ///
    /// The latest note wins when the same name was noted twice. A unit name
    /// does not see a weapon's rename.
    ///
    /// @param weapon true to look among weapon renames
    /// @param name the name the mutator wrote
    /// @return the private name, or an empty string
    [[nodiscard]] std::string private_name_of(bool weapon, std::string_view name) const;

    /// The private name this mutator stored `name` under, or null when it kept it.
    ///
    /// The pointer addresses this object's rename list and is invalidated by
    /// the next `note_rename` or `clear_renames`. The latest note wins. A
    /// unit name does not see a weapon's rename. The call allocates nothing,
    /// so it is safe to use before a lookup that can raise.
    ///
    /// @param weapon true to look among weapon renames
    /// @param name the name the mutator wrote
    /// @return the private name, or null
    [[nodiscard]] const std::string* renamed_to(bool weapon, std::string_view name) const noexcept;

    /// Points reference writes in `writes` at the names this mutator renamed.
    ///
    /// A write whose new text is a unit or weapon this mutator renamed, and
    /// whose field is a weapon reference, a side or AI list, or `unitname`,
    /// is written again with the private name. The journal text is updated
    /// to match. Other fields, and writes that do not name `mutator`, stay.
    ///
    /// @param writes the writes to retarget; a rule step's writes or the hooks'
    /// @param mutator the mutator, as `begin_mutator` spelled it
    /// @param[out] error the editor's message when a value cannot be written
    /// @return false when a value could not be written
    [[nodiscard]] bool retarget_renamed_references(
        std::span<oa::data::mutation_rules::FieldWrite> writes,
        std::string_view mutator,
        std::string& error
    );

    /// One unit Add stored under a private name, so its name-keyed files follow.
    ///
    /// Cleared only when the collections object is destroyed. `clear_renames`
    /// does not drop it: the files are copied after every mutator has run.
    struct UnitFileShift {
        uint16_t mutator{}; ///< slot of the mutator that renamed the unit
        std::string asked;  ///< the name the mutator wrote
        std::string stored; ///< the private name
        bool move_model{};  ///< the effective model was the asked name
    };

    /// Remembers that Add renamed a unit, so the pipeline can move its files.
    ///
    /// @param shift the rename
    void note_unit_file_shift(UnitFileShift shift);

    /// The unit-file renames noted this pass, in note order.
    ///
    /// @return the shifts
    [[nodiscard]] std::span<const UnitFileShift> unit_file_shifts() const { return unit_shifts_; }

    /// A download file this run opened so a button past the fifth section
    /// still has a file the loader reads.
    struct MenuSpill {
        std::string unit; ///< the unit the file was opened for
        std::string path; ///< the download file
    };

    /// Remembers a download file opened for `unit` during this pass.
    ///
    /// @param unit the unit the file serves
    /// @param path the download file
    void note_menu_spill(std::string unit, std::string path);

    /// The spill files noted this pass, in note order.
    ///
    /// @return the spills
    [[nodiscard]] std::span<const MenuSpill> menu_spills() const { return menu_spills_; }

    /// Points this mutator's reference writes at the names it renamed.
    ///
    /// A write whose new text is a unit or weapon this mutator renamed, and
    /// whose field is a weapon reference, a side or AI list, or `unitname`,
    /// is written again with the private name. The journal text is updated
    /// to match. Other fields, and other mutators' writes, stay as they are.
    ///
    /// @param mutator the mutator, as `begin_mutator` spelled it
    /// @param[out] error the editor's message when a value cannot be written
    /// @return false when a value could not be written
    [[nodiscard]] bool retarget_renamed_references(std::string_view mutator, std::string& error);

  private:

    friend struct CollectionsAccess;
    friend struct CloneAccess;
    /// The sides and tables collections journal through the same writes.
    friend struct SideCollectionsAccess;
    /// The build menus journal through the same writes and read the units index.
    friend struct MenusAccess;

    /// Keeps one field write, and an overwrite note when an earlier hook wrote it.
    ///
    /// @param write the write, naming this mutator
    /// @param[out] reason the limit it crossed; unchanged on success
    /// @return false when the mutator has journaled too much
    bool accept_write(oa::data::mutation_rules::FieldWrite write, std::string& reason);

    /// Keeps one hook note.
    ///
    /// @param note the note
    /// @param[out] reason the limit it crossed; unchanged on success
    /// @return false when the mutator has journaled too much
    bool accept_note(HookNote note, std::string& reason);

    /// Holds the next Lua error until `raise_held`.
    ///
    /// @param message the error
    void hold_error(std::string message);

    /// Raises the held error. Call it with no local objects still to destroy.
    ///
    /// @param state the state to raise in
    /// @return does not return
    int raise_held(lua_State* state);

    /// The side bindings' working storage. It lives as long as the collections.
    ///
    /// @return the storage
    SideScratch& side_scratch() noexcept;

    /// Resolves one unit through the units index.
    ///
    /// Charges the index check and the matched section. Copies the canonical
    /// name into `side_scratch().text` and the builder flag into
    /// `side_scratch().builder_unit`. Call it with no local object still to
    /// destroy: the charge can raise.
    ///
    /// @param state the state to charge, or null to build the index quietly
    /// @param sought the unit name the script wrote
    /// @return 1 when the unit is there, 0 when it is not, -1 on a held error
    int lookup_unit(lua_State* state, std::string_view sought);

    /// Finds a unit in the units index as the last `lookup_unit` left it.
    ///
    /// Neither charges nor looks for new documents, so it answers for the
    /// documents as they stood at that lookup. A caller charges the search.
    ///
    /// @param sought the unit name, compared without ASCII case
    /// @param[out] builder set true when the unit's FBI marks it a builder; may be null
    /// @return the unit's own spelling, valid until the next lookup; null when there is none
    [[nodiscard]] const std::string* indexed_unit(std::string_view sought, bool* builder) noexcept;

    oa::data::mutation_rules::DocumentSet* documents_;
    const oa::data::defs::DataLayout* layout_;
    bool active_{};
    std::string mutator_;
    WriteGrant grant_{};
    std::vector<oa::data::mutation_rules::FieldWrite> writes_;
    std::vector<RecordRemoval> removals_;
    std::vector<HookNote> notes_;
    std::unique_ptr<CollectionsState> state_;
    oa::AssetStore* store_{}; ///< store a clone reads; not owned
    /// Weapon ids the mod numbers, 0 to one less. 256 until `set_weapon_types`.
    uint32_t weapon_types_{256};
    std::vector<CloneRecord> clones_; ///< clones made during the hooks
    std::vector<ServedCopy> served_;  ///< binary copies those clones serve
    /// Folded weapon section names whose ids stay as the game assigned them.
    std::vector<std::string> kept_weapon_ids_;
    /// Clash mode the clone methods honour. Replace until a caller sets it.
    oa::formats::oamut::ClashMode clash_mode_{oa::formats::oamut::ClashMode::replace};
    /// True when an Add-mode clone may take a private name.
    bool clash_active_{};
    /// Renames this mutator has made. Cleared at the start of the next one.
    std::vector<ClashRename> renames_;
    /// Units Add renamed. Not cleared with `renames_`.
    std::vector<UnitFileShift> unit_shifts_;
    /// Download files opened so a sixth button is still loaded. Not cleared with `renames_`.
    std::vector<MenuSpill> menu_spills_;
    /// Folded served path to its index in `served_`. A page lookup does not scan the list.
    std::unordered_map<std::string, std::size_t> served_index_;
    SideScratch side_scratch_{};
};

} // namespace oa::data::mutators
