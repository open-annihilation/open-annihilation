// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Lua collections over one document set. A handle remembers the section it
// was given and looks that section up again on every access. The name stored
// in the handle is for messages. Iteration follows the folded name, then the
// document path, then the ordinal of that name in its document. It does not
// follow Lua's table order. Name lookup uses an index built once per document,
// in linear time, and kept up to date when a section is added or removed. A
// change to UnitName moves only that unit's entry. Each index entry carries
// its section's handle slot and the section's index in its document. A
// mutator that passes the write or note limit fails.
#include "oa/data/mutators/collections.hpp"
#include "oa/data/mutators/hold_range.hpp"
#include "oa/data/mutators/maps.hpp"

#include "clone.hpp"
#include "oa/data/field_schema.hpp"
#include "oa/data/script_host/fixed.hpp"
#include "oa/data/script_host/script_host.hpp"
#include "oa/data/script_host/sorted.hpp"
#include "oa/formats/tdf/edit.hpp"
#include "oa/formats/tdf/number.hpp"

#include "lua.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace oa::data::mutators {

/// Indexes, section identities and the storage used while a hook runs.
struct CollectionsState {
    /// Which collection a view or a record belongs to.
    enum class Kind : uint8_t { units, weapons, features };

    /// A value a script asked to store.
    enum class StoredKind : uint8_t { nil_value, boolean, number, text, fixed_value };

    /// One section a handle can name: its document, its place in that document, and its block.
    struct Identity {
        uint32_t doc_index = 0;
        uint32_t section_index = 0;
        oa::formats::tdf::Block* section = nullptr;
    };

    /// Orders identities for the slot lookup. Nothing walks this order.
    struct IdentityLess {
        bool operator()(const Identity& left, const Identity& right) const noexcept {
            if (left.doc_index != right.doc_index)
                return left.doc_index < right.doc_index;
            return std::less<const oa::formats::tdf::Block*>{}(left.section, right.section);
        }
    };

    /// The section each slot names. A slot is never reused.
    std::vector<Identity> identities;

    /// The slot of each section in `identities`, found without a scan.
    std::map<Identity, uint32_t, IdentityLess> slots;

    struct Rec {
        uint32_t doc_index = 0;
        oa::formats::tdf::Block* section = nullptr;
        uint32_t ordinal = 0;
        uint32_t slot = 0; ///< the section's slot in `identities`
        std::string name;
        std::string path;
    };

    /// The first section of one lower-cased name in one document, and how many
    /// sections of that name have been indexed. Later duplicates keep higher
    /// ordinals, so removing one does not reshuffle the others.
    struct NamedSection {
        oa::formats::tdf::Block* section = nullptr;
        uint32_t next_ordinal = 0;
    };

    struct Index {
        bool ready = false;
        std::vector<Rec> records;
        /// Next top-level child not yet indexed, per document.
        std::unordered_map<uint32_t, uint32_t> listed_children;
        /// Lower-cased record name to `NamedSection`, per document.
        std::unordered_map<uint32_t, std::unordered_map<std::string, NamedSection>> names;
    };

    Index indexes[3]{};

    struct WriterKey {
        std::string path;
        std::string record;
        std::string block;
        std::string key;

        bool operator<(const WriterKey& other) const {
            if (path != other.path)
                return path < other.path;
            if (record != other.record)
                return record < other.record;
            if (block != other.block)
                return block < other.block;
            return key < other.key;
        }
    };

    std::map<WriterKey, std::vector<std::string>> writers;

    struct Tally {
        std::string mutator;
        uint32_t writes = 0;
        uint32_t notes = 0;
        /// Set when a write passed the limit. It stays set if the script catches the error.
        bool over_writes = false;
    };

    std::vector<Tally> tallies;

    struct Live {
        Kind kind = Kind::units;
        bool damage = false;
        uint32_t doc_index = 0;
        uint32_t section_index = 0;
        std::string name;
        std::string path;
        oa::formats::tdf::Document* document = nullptr;
        oa::formats::tdf::Block* section = nullptr;
        oa::formats::tdf::Block* block = nullptr;
    };

    struct Handle {
        Kind kind = Kind::units;
        bool damage = false;
        uint32_t slot = 0;
        std::string name;
    };

    struct Stored {
        StoredKind kind = StoredKind::nil_value;
        bool flag = false;
        double number = 0;
        int64_t count = 0;
        std::string text;
    };

    struct Assignment {
        std::string key;
        Stored value;
    };

    struct Factor {
        std::string key;
        double number = 0;
    };

    struct Prepared {
        oa::data::field_schema::Value::Kind kind = oa::data::field_schema::Value::Kind::absent;
        int64_t integer = 0;
        double number = 0;
        bool flag = false;
        std::string text;
    };

    std::vector<oa::formats::soft_mutator::Need> needs;
    oa::formats::soft_mutator::Need staging;
    std::vector<uint32_t> matched;
    std::vector<Handle> handles;
    std::vector<Assignment> assignments;
    std::vector<Factor> factors;
    std::vector<std::string> keys;
    std::vector<Live> lives;
    Live live;
    Stored stored;
    Prepared prepared;

    /// The next Lua error, held until the trampoline can raise it.
    std::string pending_error;
};

/// The bindings reach the journal through here. Callers do not use this type.
struct CollectionsAccess {
    static oa::data::mutation_rules::DocumentSet& documents(Collections& self) noexcept {
        return *self.documents_;
    }

    static const oa::data::defs::DataLayout* layout_ptr(const Collections& self) noexcept {
        return self.layout_;
    }

    static bool active(const Collections& self) noexcept { return self.active_; }

    static const std::string& mutator(const Collections& self) noexcept { return self.mutator_; }

    static const WriteGrant& grant(const Collections& self) noexcept { return self.grant_; }

    static const std::vector<oa::data::mutation_rules::FieldWrite>&
    writes(const Collections& self) noexcept {
        return self.writes_;
    }

    static std::vector<oa::data::mutation_rules::FieldWrite>& writes(Collections& self) noexcept {
        return self.writes_;
    }

    static std::vector<RecordRemoval>& removals(Collections& self) noexcept {
        return self.removals_;
    }

    static std::vector<HookNote>& notes(Collections& self) noexcept { return self.notes_; }

    static const std::vector<std::string>& kept_weapon_ids(const Collections& self) noexcept {
        return self.kept_weapon_ids_;
    }

    static CollectionsState& work(Collections& self) noexcept { return *self.state_; }
};

namespace {

namespace tdf = oa::formats::tdf;
namespace fields = oa::data::field_schema;
namespace rules = oa::data::mutation_rules;
namespace sm = oa::formats::soft_mutator;
namespace host = oa::data::script_host;
namespace defs = oa::data::defs;

using Kind = CollectionsState::Kind;
using StoredKind = CollectionsState::StoredKind;

constexpr const char* kNeedMeta = "oa.mutators.need";
constexpr const char* kCollectionMeta = "oa.mutators.collection";
constexpr const char* kSetMeta = "oa.mutators.set";
constexpr const char* kRecordMeta = "oa.mutators.record";

/// Numbers and flags of a need. The compared text is uservalue 1, so the
/// userdata has nothing to destroy.
struct NeedBits {
    uint8_t kind = 0;
    uint8_t negated = 0;
    double low = 0;
    double high = 0;
};

static_assert(std::is_trivially_copyable_v<NeedBits>);
static_assert(std::is_trivially_destructible_v<NeedBits>);

/// A collection or a set. A set keeps its records in uservalue 1.
struct ViewBox {
    Collections* owner = nullptr;
    Kind kind = Kind::units;
};

/// A record handle. The name's bytes follow the struct in the same userdata.
struct RecordBox {
    Collections* owner = nullptr;
    Kind kind = Kind::units;
    uint8_t damage = 0;
    uint32_t slot = 0;
    uint32_t name_size = 0;
};

static_assert(std::is_trivially_copyable_v<RecordBox>);
static_assert(std::is_standard_layout_v<RecordBox>);

using Rec = CollectionsState::Rec;
using Live = CollectionsState::Live;
using Handle = CollectionsState::Handle;
using Stored = CollectionsState::Stored;
using Assignment = CollectionsState::Assignment;
using Factor = CollectionsState::Factor;
using Prepared = CollectionsState::Prepared;
using Identity = CollectionsState::Identity;
using WriterKey = CollectionsState::WriterKey;

/// The key and text currently stored for one field.
struct FieldNow {
    bool present = false;
    std::string key;
    std::optional<std::string> text;
};

/// One section a set will remove, in snapshot order.
struct SectionDrop {
    uint32_t doc_index = 0;
    uint32_t section_index = 0;
    bool journal_document = false;
    std::string name;
    std::string path;
};

CollectionsState& working(Collections& self) noexcept {
    return CollectionsAccess::work(self);
}

/// The error the next raise on this collections object reports.
std::string& pending_of(Collections& self) noexcept {
    return working(self).pending_error;
}

/// Reports whether a byte is a space, a tab or a line break.
bool is_separator(unsigned char byte) noexcept {
    return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r';
}

/// Trims spaces, tabs and line breaks from both ends.
std::string_view trim_view(std::string_view text) noexcept {
    while (!text.empty() && is_separator(static_cast<unsigned char>(text.front())))
        text.remove_prefix(1);
    while (!text.empty() && is_separator(static_cast<unsigned char>(text.back())))
        text.remove_suffix(1);
    return text;
}

/// Folds one ASCII letter to lower case.
unsigned char fold_byte(unsigned char byte) noexcept {
    if (byte >= 'A' && byte <= 'Z')
        return static_cast<unsigned char>(byte - 'A' + 'a');
    return byte;
}

/// Copies text with ASCII letters folded. Other bytes stay as they are.
std::string fold_copy(std::string_view text) {
    std::string folded(text);
    for (char& byte : folded)
        byte = static_cast<char>(fold_byte(static_cast<unsigned char>(byte)));
    return folded;
}

/// Compares two texts after ASCII lower-casing. A shorter prefix comes first.
int compare_folded(std::string_view left, std::string_view right) noexcept {
    const std::size_t count = std::min(left.size(), right.size());
    for (std::size_t index = 0; index < count; ++index) {
        const unsigned char left_byte = fold_byte(static_cast<unsigned char>(left[index]));
        const unsigned char right_byte = fold_byte(static_cast<unsigned char>(right[index]));
        if (left_byte < right_byte)
            return -1;
        if (left_byte > right_byte)
            return 1;
    }
    if (left.size() < right.size())
        return -1;
    if (left.size() > right.size())
        return 1;
    return 0;
}

/// Reports whether two texts match without regard to ASCII case.
bool same_text(std::string_view left, std::string_view right) noexcept {
    return compare_folded(left, right) == 0;
}

/// Orders records by folded name, then folded path, then ordinal.
bool rec_before(const Rec& left, const Rec& right) noexcept {
    const int names = compare_folded(left.name, right.name);
    if (names != 0)
        return names < 0;
    const int paths = compare_folded(left.path, right.path);
    if (paths != 0)
        return paths < 0;
    return left.ordinal < right.ordinal;
}

/// The collection's layer name, as a grant error spells it.
const char* layer_name(Kind kind) noexcept {
    if (kind == Kind::units)
        return "units";
    if (kind == Kind::weapons)
        return "weapons";
    return "features";
}

/// The schema a need on this collection uses.
rules::TargetKind target_kind(Kind kind) noexcept {
    if (kind == Kind::units)
        return rules::TargetKind::units;
    if (kind == Kind::weapons)
        return rules::TargetKind::weapons;
    return rules::TargetKind::features;
}

/// The typed schema of a record, or nothing for a feature.
std::optional<fields::RecordKind> schema_of(const Live& live) noexcept {
    if (live.kind == Kind::features)
        return std::nullopt;
    if (live.damage)
        return fields::RecordKind::weapon_damage;
    if (live.kind == Kind::weapons)
        return fields::RecordKind::weapon;
    return fields::RecordKind::unit;
}

/// The file name at the end of a path.
std::string file_name(std::string_view path) {
    const std::size_t slash = path.rfind('/');
    if (slash == std::string_view::npos)
        return std::string(path);
    return std::string(path.substr(slash + 1));
}

/// The file name without its last extension.
std::string file_stem(std::string_view path) {
    const std::string filename = file_name(path);
    const std::size_t dot = filename.rfind('.');
    if (dot == std::string::npos || dot == 0)
        return filename;
    return filename.substr(0, dot);
}

/// "<name> was removed".
std::string removed_message(std::string_view name) {
    std::string text;
    text.reserve(name.size() + 12);
    text.append(name);
    text.append(" was removed");
    return text;
}

/// A script error that names the collection, the record and the field.
std::string
prefixed(Kind kind, std::string_view record, std::string_view key, std::string_view detail) {
    std::string text;
    text.reserve(detail.size() + record.size() + key.size() + 16);
    text.append(layer_name(kind));
    text.push_back(' ');
    text.append(record);
    text.push_back(' ');
    text.append(key);
    text.append(": ");
    text.append(detail);
    return text;
}

/// Reports whether field text starts as a TDF number.
///
/// After spaces and tabs, an optional sign and at least one digit. A leading
/// point is not a number here.
bool starts_as_number(const char* text) noexcept {
    if (text == nullptr)
        return false;
    const auto* cursor = reinterpret_cast<const unsigned char*>(text);
    while (*cursor == ' ' || *cursor == '\t')
        ++cursor;
    if (*cursor == '+' || *cursor == '-')
        ++cursor;
    return *cursor >= '0' && *cursor <= '9';
}

/// The name stored after a record handle.
std::string_view name_of(const RecordBox* box) noexcept {
    const auto* bytes = reinterpret_cast<const char*>(box) + sizeof(RecordBox);
    return std::string_view(bytes, box->name_size);
}

/// Charges one key-collecting walk. May raise. No C++ destructor is alive.
void charge_scan(lua_State* state) {
    host::charge_work(state, host::scan_call_charge);
}

/// Charges `visits` elements in one step. May raise. No C++ destructor is alive.
void charge_visits(lua_State* state, uint64_t visits) {
    if (host::element_charge != 0 && visits > UINT64_MAX / host::element_charge)
        visits = UINT64_MAX / host::element_charge;
    host::charge_work(state, visits * host::element_charge);
}

/// Elements charged for one field read or write of a record, for the lookup
/// and the field check around it.
constexpr uint64_t record_access_charge = 8;

/// Adds `more` to a bill of units, saturating at UINT64_MAX. Never raises.
void add_saturated(uint64_t& units, uint64_t more) noexcept {
    units = more > UINT64_MAX - units ? UINT64_MAX : units + more;
}

/// Charges the bytes of one key or name. May raise. No C++ destructor is alive.
void charge_text(lua_State* state, std::size_t bytes) {
    host::charge_work(state, host::units_for_bytes(bytes));
}

/// Bytes a stored value writes. The spelled number, when there is one, dies here.
std::size_t stored_body_bytes(const Stored& stored) {
    if (stored.kind == StoredKind::text)
        return trim_view(stored.text).size();
    if (stored.kind == StoredKind::boolean)
        return 1;
    if (stored.kind == StoredKind::fixed_value) {
        char spelled[host::fixed_text_bytes];
        if (!host::fixed_text(stored.count, spelled, sizeof spelled))
            return 0;
        return std::strlen(spelled);
    }
    if (stored.kind == StoredKind::number && std::isfinite(stored.number)) {
        const std::optional<std::string> spelled = tdf::format_number(stored.number);
        return spelled.has_value() ? spelled->size() : 0;
    }
    return 0;
}

/// `count` copies of `bytes`, saturated.
uint64_t repeated_units(std::size_t bytes, std::size_t count) {
    const uint64_t one = host::units_for_bytes(bytes);
    if (one == 0 || count == 0)
        return 0;
    if (static_cast<uint64_t>(count) > UINT64_MAX / one)
        return UINT64_MAX;
    return one * static_cast<uint64_t>(count);
}

/// `left * right`, or the largest count when the product does not fit.
uint64_t saturating_product(std::size_t left, std::size_t right) {
    const uint64_t wide_left = static_cast<uint64_t>(left);
    const uint64_t wide_right = static_cast<uint64_t>(right);
    if (wide_left != 0 && wide_right > UINT64_MAX / wide_left)
        return UINT64_MAX;
    return wide_left * wide_right;
}

/// The layout the collections read, or the layout in force.
const defs::DataLayout& layout_of(const Collections& self) noexcept {
    if (CollectionsAccess::layout_ptr(self) != nullptr)
        return *CollectionsAccess::layout_ptr(self);
    return defs::data_layout();
}

/// True when the current mutator may change this collection.
bool ensure_grant(const Collections& self, Kind kind, std::string& reason) {
    if (!CollectionsAccess::active(self)) {
        reason = "no mutator has begun";
        return false;
    }
    const WriteGrant& grant = CollectionsAccess::grant(self);
    const bool allowed = kind == Kind::units     ? grant.units
                         : kind == Kind::weapons ? grant.weapons
                                                 : grant.features;
    if (allowed)
        return true;
    reason = "this mutator does not declare the ";
    reason.append(layer_name(kind));
    reason.append(" layer");
    return false;
}

/// Installs a protected metatable. An existing one is left as it is.
void ensure_meta(
    lua_State* state,
    const char* name,
    lua_CFunction index,
    lua_CFunction newindex,
    lua_CFunction length,
    lua_CFunction pairs,
    lua_CFunction collect
) {
    if (luaL_newmetatable(state, name) == 0) {
        lua_pop(state, 1);
        return;
    }
    if (index != nullptr) {
        lua_pushcfunction(state, index);
        lua_setfield(state, -2, "__index");
    }
    if (newindex != nullptr) {
        lua_pushcfunction(state, newindex);
        lua_setfield(state, -2, "__newindex");
    }
    if (length != nullptr) {
        lua_pushcfunction(state, length);
        lua_setfield(state, -2, "__len");
    }
    if (pairs != nullptr) {
        lua_pushcfunction(state, pairs);
        lua_setfield(state, -2, "__pairs");
    }
    if (collect != nullptr) {
        lua_pushcfunction(state, collect);
        lua_setfield(state, -2, "__gc");
    }
    lua_pushboolean(state, 0);
    lua_setfield(state, -2, "__metatable");
    lua_pop(state, 1);
}

/// Raises the owner's pending error. Call it only after C++ objects in the
/// frame are gone, and never from inside a catch block.
int raise_pending(lua_State* state, Collections& owner) {
    std::string& text = pending_of(owner);
    lua_pushlstring(state, text.data(), text.size());
    text.clear();
    return lua_error(state);
}

/// The unit name a section has now: UnitName, or the file stem.
std::string unit_name_of(const tdf::Block* section, std::string_view path) {
    const char* unit_name = section != nullptr ? tdf::find_value(section, "UnitName") : nullptr;
    if (unit_name != nullptr && unit_name[0] != '\0')
        return unit_name;
    return file_stem(path);
}

/// Where a file's stem sits in `path`.
struct StemSpan {
    std::size_t offset = 0;
    std::size_t length = 0;
};

StemSpan file_stem_span(std::string_view path) noexcept {
    const std::size_t slash = path.rfind('/');
    const std::size_t start = slash == std::string_view::npos ? 0 : slash + 1;
    const std::string_view filename = path.substr(start);
    const std::size_t dot = filename.rfind('.');
    StemSpan span;
    span.offset = start;
    if (dot == std::string_view::npos || dot == 0)
        span.length = filename.size();
    else
        span.length = dot;
    return span;
}

/// Remembers a section and returns its slot. The same block keeps its slot.
///
/// The lookup is a map search, so a walk costs the same however many
/// sections were remembered before it. `section_index` is stored on the
/// slot, not in the map key.
uint32_t
remember(Collections& self, uint32_t doc_index, tdf::Block* section, uint32_t section_index) {
    CollectionsState& scratch = working(self);
    const Identity identity{doc_index, section_index, section};
    const auto known = scratch.slots.find(identity);
    if (known != scratch.slots.end()) {
        Identity& kept = scratch.identities[known->second];
        if (kept.section == section)
            kept.section_index = section_index;
        return known->second;
    }
    if (scratch.identities.size() >= static_cast<std::size_t>(std::numeric_limits<uint32_t>::max()))
        throw std::bad_alloc();
    const auto slot = static_cast<uint32_t>(scratch.identities.size());
    const auto placed = scratch.slots.emplace(identity, slot).first;
    try {
        scratch.identities.push_back(identity);
    } catch (const std::bad_alloc&) {
        scratch.slots.erase(placed);
        throw;
    }
    return slot;
}

/// One section a document gained since the index listed it.
struct NewRecord {
    uint32_t doc_index = 0;
    uint32_t section_index = 0;
    tdf::Block* section = nullptr;
    std::string_view path;
    const char* name = nullptr;
    std::size_t name_length = 0;
};

/// Calls `on_record` for each new record and `on_grown` for each document
/// whose top-level child list grew. Neither callback may call `charge_work`:
/// this function holds the folder strings.
template <typename RecordFn, typename GrownFn>
void each_new_record(
    rules::DocumentSet& documents,
    const defs::DataLayout& layout,
    Kind kind,
    const CollectionsState::Index& index,
    RecordFn on_record,
    GrownFn on_grown
) {
    std::string folder;
    std::string suffix;
    bool recursive = false;
    if (kind == Kind::units) {
        folder = layout.directories[static_cast<std::size_t>(defs::DataDirectory::units)];
        suffix = layout.unit_extension;
    } else if (kind == Kind::weapons) {
        folder = layout.directories[static_cast<std::size_t>(defs::DataDirectory::weapons)];
        suffix = "tdf";
    } else {
        folder = "features";
        suffix = "tdf";
        recursive = true;
    }
    const std::vector<std::size_t> docs = documents.documents_in(folder, suffix, recursive);
    for (const std::size_t doc_index : docs) {
        if (doc_index > static_cast<std::size_t>(std::numeric_limits<uint32_t>::max()))
            continue;
        const auto narrow_doc = static_cast<uint32_t>(doc_index);
        rules::LoadedDocument& loaded = documents.at(doc_index);
        tdf::Document* document = loaded.document.get();
        tdf::Block* root = tdf::editable_root(document);
        if (root == nullptr)
            continue;
        const uint32_t sections = tdf::child_count(root);
        const auto listed = index.listed_children.find(narrow_doc);
        const uint32_t start = listed == index.listed_children.end() ? 0 : listed->second;
        if (start >= sections)
            continue;
        bool unit_taken = false;
        if (kind == Kind::units)
            unit_taken = index.names.find(narrow_doc) != index.names.end();
        for (uint32_t section = start; section < sections; ++section) {
            tdf::Block* block = tdf::editable_child(root, section);
            if (block == nullptr || block->name == nullptr)
                continue;
            NewRecord fresh;
            fresh.doc_index = narrow_doc;
            fresh.section_index = section;
            fresh.section = block;
            fresh.path = loaded.path;
            if (kind == Kind::units) {
                if (unit_taken || tdf::compare_nocase(block->name, "UNITINFO") != 0)
                    continue;
                unit_taken = true;
                const char* unit_name = tdf::find_value(block, "UnitName");
                if (unit_name != nullptr && unit_name[0] != '\0') {
                    fresh.name = unit_name;
                    fresh.name_length = std::strlen(unit_name);
                } else {
                    const StemSpan stem = file_stem_span(loaded.path);
                    fresh.name = loaded.path.data() + stem.offset;
                    fresh.name_length = stem.length;
                }
            } else {
                fresh.name = block->name;
                fresh.name_length = std::strlen(block->name);
            }
            on_record(fresh);
        }
        on_grown(narrow_doc, sections);
    }
}

/// Sections and name bytes an index build would add. No allocation of names.
struct IndexBill {
    uint64_t sections = 0;
    uint64_t name_units = 0;
    bool grew = false;
};

IndexBill measure_index(
    rules::DocumentSet& documents,
    const defs::DataLayout& layout,
    Kind kind,
    const CollectionsState::Index& index
) {
    IndexBill bill;
    each_new_record(
        documents,
        layout,
        kind,
        index,
        [&bill](const NewRecord& fresh) {
            ++bill.sections;
            const uint64_t units = host::units_for_bytes(fresh.name_length);
            if (bill.name_units > UINT64_MAX - units)
                bill.name_units = UINT64_MAX;
            else
                bill.name_units += units;
        },
        [&bill](uint32_t, uint32_t) { bill.grew = true; }
    );
    return bill;
}

/// The records of one collection, each with its slot.
///
/// The first visit of a document indexes it in linear time and charges one
/// unit per new section plus the name bytes. Every visit checks each document
/// for growth and charges one unit per document for that check. A later visit
/// indexes only a section list that grew. `state` null builds without
/// charging. May raise when `state` is set, and only before the new records
/// are allocated.
const std::vector<Rec>& ensure_index(Collections& self, Kind kind, lua_State* state) {
    CollectionsState::Index& index = working(self).indexes[static_cast<std::size_t>(kind)];
    rules::DocumentSet& documents = CollectionsAccess::documents(self);
    const defs::DataLayout& layout = layout_of(self);
    if (!index.ready) {
        index.records.clear();
        index.listed_children.clear();
        index.names.clear();
    }
    if (state != nullptr)
        charge_visits(state, static_cast<uint64_t>(documents.size()));
    const IndexBill bill = measure_index(documents, layout, kind, index);
    if (!bill.grew) {
        index.ready = true;
        return index.records;
    }
    if (state != nullptr) {
        charge_visits(state, bill.sections);
        if (bill.name_units != 0)
            host::charge_work(state, bill.name_units);
    }
    std::vector<Rec> added;
    std::vector<uint32_t> section_indexes;
    auto names = index.names;
    auto listed = index.listed_children;
    each_new_record(
        documents,
        layout,
        kind,
        index,
        [&](const NewRecord& fresh) {
            Rec rec;
            rec.doc_index = fresh.doc_index;
            rec.section = fresh.section;
            rec.path.assign(fresh.path);
            rec.name.assign(fresh.name != nullptr ? fresh.name : "", fresh.name_length);
            const std::string folded = fold_copy(rec.name);
            CollectionsState::NamedSection& named = names[fresh.doc_index][folded];
            rec.ordinal = named.next_ordinal;
            if (named.section == nullptr)
                named.section = fresh.section;
            if (named.next_ordinal != std::numeric_limits<uint32_t>::max())
                ++named.next_ordinal;
            added.push_back(std::move(rec));
            section_indexes.push_back(fresh.section_index);
        },
        [&](uint32_t doc_index, uint32_t sections) { listed[doc_index] = sections; }
    );
    for (std::size_t rec_index = 0; rec_index < added.size(); ++rec_index) {
        added[rec_index].slot = remember(
            self, added[rec_index].doc_index, added[rec_index].section, section_indexes[rec_index]
        );
    }
    index.records.insert(
        index.records.end(),
        std::make_move_iterator(added.begin()),
        std::make_move_iterator(added.end())
    );
    index.names.swap(names);
    index.listed_children.swap(listed);
    std::sort(index.records.begin(), index.records.end(), rec_before);
    index.ready = true;
    return index.records;
}

/// The first record of this name, in collection order, or null.
///
/// A unit or weapon name this mutator renamed is looked up as the private
/// name, and the walk of the rename list is charged. The private name is a
/// pointer into the rename list, taken before the index build, so nothing
/// new is alive if that build raises. Features are not renamed.
const Rec* find_named(Collections& self, Kind kind, std::string_view key, lua_State* state) {
    const std::string* renamed = nullptr;
    if (kind == Kind::units)
        renamed = self.renamed_to(false, key);
    else if (kind == Kind::weapons)
        renamed = self.renamed_to(true, key);
    if (state != nullptr && kind != Kind::features && self.rename_count() > 0)
        charge_visits(state, self.rename_count());
    const std::string_view sought = renamed != nullptr ? std::string_view{*renamed} : key;
    const std::vector<Rec>& records = ensure_index(self, kind, state);
    const auto found = std::lower_bound(
        records.begin(), records.end(), sought, [](const Rec& rec, std::string_view needle) {
            return compare_folded(rec.name, needle) < 0;
        }
    );
    if (found == records.end() || !same_text(found->name, sought))
        return nullptr;
    return &(*found);
}

/// Pushes a record handle. The name is copied into the userdata.
void push_record(
    lua_State* state,
    Collections* owner,
    Kind kind,
    bool damage,
    uint32_t slot,
    std::string_view name
) {
    charge_text(state, name.size());
    const std::size_t bytes = sizeof(RecordBox) + name.size();
    void* memory = lua_newuserdatauv(state, bytes, 0);
    auto* box = static_cast<RecordBox*>(memory);
    box->owner = owner;
    box->kind = kind;
    box->damage = damage ? static_cast<uint8_t>(1) : static_cast<uint8_t>(0);
    box->slot = slot;
    box->name_size = static_cast<uint32_t>(name.size());
    if (!name.empty())
        std::memcpy(reinterpret_cast<char*>(memory) + sizeof(RecordBox), name.data(), name.size());
    luaL_setmetatable(state, kRecordMeta);
}

/// Pushes a value prepared on the collections object.
void push_prepared(lua_State* state, const Prepared& value) {
    switch (value.kind) {
    case fields::Value::Kind::absent:
        lua_pushnil(state);
        return;
    case fields::Value::Kind::integer:
        if (value.integer >= static_cast<int64_t>(std::numeric_limits<lua_Integer>::min()) &&
            value.integer <= static_cast<int64_t>(std::numeric_limits<lua_Integer>::max()))
            lua_pushinteger(state, static_cast<lua_Integer>(value.integer));
        else
            lua_pushnumber(state, static_cast<double>(value.integer));
        return;
    case fields::Value::Kind::number:
        lua_pushnumber(state, value.number);
        return;
    case fields::Value::Kind::boolean:
        lua_pushboolean(state, value.flag ? 1 : 0);
        return;
    case fields::Value::Kind::text:
        lua_pushlstring(state, value.text.data(), value.text.size());
        return;
    }
}

/// Reads a Lua value into `stored`.
bool read_stored(lua_State* state, int index, Stored& stored, std::string& reason) {
    const int type = lua_type(state, index);
    if (type == LUA_TNIL) {
        stored.kind = StoredKind::nil_value;
        return true;
    }
    if (type == LUA_TBOOLEAN) {
        stored.kind = StoredKind::boolean;
        stored.flag = lua_toboolean(state, index) != 0;
        return true;
    }
    if (type == LUA_TNUMBER) {
        stored.kind = StoredKind::number;
        stored.number = lua_tonumber(state, index);
        return true;
    }
    if (type == LUA_TSTRING) {
        std::size_t length = 0;
        const char* text = lua_tolstring(state, index, &length);
        stored.kind = StoredKind::text;
        stored.text.assign(text != nullptr ? text : "", length);
        return true;
    }
    if (type == LUA_TUSERDATA) {
        int64_t count = 0;
        if (host::to_fixed(state, index, &count)) {
            stored.kind = StoredKind::fixed_value;
            stored.count = count;
            return true;
        }
    }
    reason = "a ";
    const char* type_name = lua_typename(state, type);
    reason.append(type_name != nullptr ? type_name : "value");
    reason.append(" cannot be stored");
    return false;
}

/// Finds the handle's section again. A missing section sets `reason`.
bool resolve(
    Collections& self,
    const RecordBox& box,
    std::string_view message_name,
    Live& live,
    std::string& reason
) {
    CollectionsState& scratch = working(self);
    if (box.slot >= scratch.identities.size()) {
        reason = removed_message(message_name);
        return false;
    }
    const Identity& identity = scratch.identities[box.slot];
    rules::DocumentSet& documents = CollectionsAccess::documents(self);
    if (identity.section == nullptr ||
        identity.doc_index >= static_cast<uint32_t>(documents.size())) {
        reason = removed_message(message_name);
        return false;
    }
    rules::LoadedDocument& loaded = documents.at(identity.doc_index);
    if (loaded.removed) {
        reason = removed_message(message_name);
        return false;
    }
    tdf::Document* document = loaded.document.get();
    tdf::Block* root = tdf::editable_root(document);
    if (root == nullptr) {
        reason = removed_message(message_name);
        return false;
    }
    const uint32_t sections = tdf::child_count(root);
    if (identity.section_index >= sections ||
        tdf::editable_child(root, identity.section_index) != identity.section) {
        reason = removed_message(message_name);
        return false;
    }
    live.kind = box.kind;
    live.damage = box.damage != 0;
    live.doc_index = identity.doc_index;
    live.section_index = identity.section_index;
    live.path = loaded.path;
    live.document = document;
    live.section = identity.section;
    live.block = identity.section;
    if (live.kind == Kind::units)
        live.name = unit_name_of(identity.section, loaded.path);
    else if (identity.section->name != nullptr)
        live.name = identity.section->name;
    else
        live.name.clear();
    if (live.damage)
        live.block = tdf::editable_child(identity.section, "DAMAGE");
    return true;
}

/// The stored spelling and text of one key, or the trimmed spelling.
FieldNow field_now(const tdf::Block* block, std::string_view key) {
    const std::string_view trimmed = trim_view(key);
    FieldNow now;
    now.key.assign(trimmed);
    if (block == nullptr)
        return now;
    const uint32_t count = tdf::property_count(block);
    for (uint32_t index = 0; index < count; ++index) {
        const auto place = static_cast<int32_t>(index);
        const char* stored = tdf::property_key_at(block, place);
        if (stored == nullptr || !same_text(stored, trimmed))
            continue;
        now.present = true;
        now.key = stored;
        const char* value = tdf::property_value_at(block, place);
        now.text =
            value != nullptr ? std::optional<std::string>(value) : std::optional<std::string>("");
        break;
    }
    return now;
}

/// The tally for this mutator name, counted across the collections object's life.
std::size_t tally_index(Collections& self) {
    CollectionsState& scratch = working(self);
    const std::string& mutator = CollectionsAccess::mutator(self);
    for (std::size_t index = 0; index < scratch.tallies.size(); ++index) {
        if (scratch.tallies[index].mutator == mutator)
            return index;
    }
    scratch.tallies.push_back(CollectionsState::Tally{mutator, 0, 0});
    return scratch.tallies.size() - 1;
}

/// Refuses a write or a note that would pass the per-mutator limit.
bool room_for(Collections& self, uint32_t more_writes, uint32_t more_notes, std::string& reason) {
    CollectionsState::Tally& tally = working(self).tallies[tally_index(self)];
    if (tally.over_writes ||
        (more_writes > 0 && tally.writes > mutator_write_limit - more_writes)) {
        tally.over_writes = true;
        reason = "this mutator has changed too many fields";
        return false;
    }
    if (more_notes > 0 && tally.notes > mutator_note_limit - more_notes) {
        reason = "this mutator has made too many notes";
        return false;
    }
    return true;
}

/// Counts a write and its notes against the mutator's limit.
void commit_tally(Collections& self, uint32_t more_writes, uint32_t more_notes) {
    CollectionsState::Tally& tally = working(self).tallies[tally_index(self)];
    tally.writes += more_writes;
    tally.notes += more_notes;
}

/// Names every other mutator that already wrote this field, in first-seen order.
std::vector<std::string> earlier_writers(
    Collections& self,
    std::string_view path,
    std::string_view record,
    std::string_view block,
    std::string_view key
) {
    WriterKey writer_key;
    writer_key.path = fold_copy(path);
    writer_key.record = fold_copy(record);
    writer_key.block = fold_copy(block);
    writer_key.key = fold_copy(key);
    std::vector<std::string> earlier;
    const auto found = working(self).writers.find(writer_key);
    if (found == working(self).writers.end())
        return earlier;
    const std::string& current = CollectionsAccess::mutator(self);
    for (const std::string& name : found->second) {
        if (name != current)
            earlier.push_back(name);
    }
    return earlier;
}

/// Records that this mutator wrote the field, keeping the first-seen order.
void remember_writer(
    Collections& self,
    std::string_view path,
    std::string_view record,
    std::string_view block,
    std::string_view key
) {
    WriterKey writer_key;
    writer_key.path = fold_copy(path);
    writer_key.record = fold_copy(record);
    writer_key.block = fold_copy(block);
    writer_key.key = fold_copy(key);
    std::vector<std::string>& names = working(self).writers[writer_key];
    const std::string& current = CollectionsAccess::mutator(self);
    for (const std::string& name : names) {
        if (name == current)
            return;
    }
    names.push_back(current);
}

/// Records one write, then a warning or an overwrite when either applies.
void journal_write(
    Collections& self,
    const Live& live,
    const std::string& key,
    std::optional<std::string> old_text,
    std::optional<std::string> new_text,
    const fields::Check& check,
    const std::vector<std::string>& earlier
) {
    const std::string block = live.damage ? "DAMAGE" : "";
    rules::FieldWrite write;
    write.mutators.push_back(CollectionsAccess::mutator(self));
    write.path = live.path;
    write.record = live.name;
    write.block = block;
    write.key = key;
    write.old_text = std::move(old_text);
    write.new_text = std::move(new_text);
    CollectionsAccess::writes(self).push_back(std::move(write));
    uint32_t notes = 0;
    if (check.verdict == fields::Verdict::warn) {
        HookNote note;
        note.kind = HookNoteKind::check_warned;
        note.mutators.push_back(CollectionsAccess::mutator(self));
        note.path = live.path;
        note.record = live.name;
        note.key = key;
        note.message = check.reason;
        CollectionsAccess::notes(self).push_back(std::move(note));
        ++notes;
    }
    if (!earlier.empty()) {
        HookNote note;
        note.kind = HookNoteKind::hook_overwrote;
        note.mutators = earlier;
        note.mutators.push_back(CollectionsAccess::mutator(self));
        note.path = live.path;
        note.record = live.name;
        note.key = key;
        note.message = CollectionsAccess::mutator(self);
        note.message.append(" replaced the value written by ");
        note.message.append(earlier[0]);
        for (std::size_t index = 1; index < earlier.size(); ++index) {
            note.message.append(" and ");
            note.message.append(earlier[index]);
        }
        CollectionsAccess::notes(self).push_back(std::move(note));
        ++notes;
    }
    remember_writer(self, live.path, live.name, block, key);
    commit_tally(self, 1, notes);
}

/// Moves a renamed unit's entry to its place under the new name.
///
/// `live` was resolved before the write, so `live.name` is the name the
/// index holds. The other entries keep their order, and the result is the
/// order a rebuild would give. An entry that cannot be found, or a failure
/// part way, leaves the index to be built again on the next lookup.
void rekey_if_renamed(Collections& self, const Live& live, std::string_view key) {
    if (live.kind != Kind::units || live.damage || !same_text(key, "UnitName"))
        return;
    CollectionsState::Index& index = working(self).indexes[static_cast<std::size_t>(Kind::units)];
    if (!index.ready)
        return;
    index.ready = false;
    std::string renamed;
    try {
        renamed = unit_name_of(live.section, live.path);
    } catch (const std::bad_alloc&) {
        return;
    }
    std::vector<Rec>& records = index.records;
    auto entry = std::lower_bound(
        records.begin(),
        records.end(),
        std::string_view(live.name),
        [](const Rec& rec, std::string_view sought) { return compare_folded(rec.name, sought) < 0; }
    );
    while (entry != records.end() && same_text(entry->name, live.name) &&
           (entry->doc_index != live.doc_index || entry->section != live.section))
        ++entry;
    if (entry == records.end() || !same_text(entry->name, live.name))
        return;
    entry->name.swap(renamed);
    const auto doc_names = index.names.find(live.doc_index);
    if (doc_names != index.names.end()) {
        const std::string old_folded = fold_copy(renamed);
        const std::string new_folded = fold_copy(entry->name);
        const auto old_place = doc_names->second.find(old_folded);
        if (old_place != doc_names->second.end()) {
            CollectionsState::NamedSection moved = old_place->second;
            doc_names->second.erase(old_place);
            const auto existing = doc_names->second.find(new_folded);
            if (existing == doc_names->second.end()) {
                doc_names->second.emplace(new_folded, moved);
            } else {
                entry->ordinal = existing->second.next_ordinal;
                if (existing->second.section == nullptr)
                    existing->second.section = entry->section;
                if (existing->second.next_ordinal != std::numeric_limits<uint32_t>::max())
                    ++existing->second.next_ordinal;
            }
        }
    }
    if (entry != records.begin() && rec_before(*entry, *(entry - 1))) {
        const auto place = std::upper_bound(records.begin(), entry, *entry, rec_before);
        std::rotate(place, entry, entry + 1);
    } else {
        const auto place = std::lower_bound(entry + 1, records.end(), *entry, rec_before);
        std::rotate(entry, entry + 1, place);
    }
    index.ready = true;
}

/// True when `key` is the id of a weapon the game already numbered.
bool weapon_id_stays(const Collections& self, const Live& live, std::string_view key) {
    if (live.kind != Kind::weapons || live.damage || !same_text(key, "id"))
        return false;
    const std::string folded = fold_copy(live.name);
    const std::vector<std::string>& names = CollectionsAccess::kept_weapon_ids(self);
    return std::binary_search(names.begin(), names.end(), folded);
}

/// Leaves a base or mod weapon id as it was, and notes the mutator.
bool note_kept_weapon_id(Collections& self, const Live& live, std::string& reason) {
    if (!room_for(self, 0, 1, reason))
        return false;
    HookNote note;
    note.kind = HookNoteKind::id_kept;
    note.mutators.push_back(CollectionsAccess::mutator(self));
    note.path = live.path;
    note.record = live.name;
    note.key = "id";
    note.message = "a base weapon keeps its id";
    CollectionsAccess::notes(self).push_back(std::move(note));
    commit_tally(self, 0, 1);
    return true;
}

/// Writes one stored value. The grant has already been checked.
bool write_stored(
    Collections& self, Live& live, std::string_view key, const Stored& stored, std::string& reason
) {
    const std::string trimmed(trim_view(key));
    if (weapon_id_stays(self, live, trimmed))
        return note_kept_weapon_id(self, live, reason);
    const FieldNow before = field_now(live.block, trimmed);
    const std::string block_name = live.damage ? "DAMAGE" : "";
    if (stored.kind == StoredKind::nil_value) {
        if (!before.present || live.block == nullptr)
            return true;
        const std::vector<std::string> earlier =
            earlier_writers(self, live.path, live.name, block_name, before.key);
        const uint32_t notes = earlier.empty() ? 0 : 1;
        if (!room_for(self, 1, notes, reason))
            return false;
        const tdf::EditStatus status = tdf::remove_key(live.document, live.block, before.key);
        if (status != tdf::EditStatus::ok) {
            reason = prefixed(live.kind, live.name, trimmed, tdf::edit_status_message(status));
            return false;
        }
        journal_write(self, live, before.key, before.text, std::nullopt, {}, earlier);
        rekey_if_renamed(self, live, trimmed);
        return true;
    }

    fields::Check check;
    std::string text;
    const std::optional<fields::RecordKind> schema = schema_of(live);
    if (stored.kind == StoredKind::number) {
        if (!std::isfinite(stored.number)) {
            if (schema.has_value())
                check = fields::check_number(*schema, trimmed, stored.number, self.weapon_types());
            if (check.verdict == fields::Verdict::refuse)
                reason = prefixed(live.kind, live.name, trimmed, check.reason);
            else {
                const char* word = std::isnan(stored.number) ? "NaN" : "infinity";
                reason = prefixed(
                    live.kind,
                    live.name,
                    trimmed,
                    std::string(word) +
                        " is not a finite number; a value that is not finite cannot be stored"
                );
            }
            return false;
        }
        if (schema.has_value())
            check = fields::check_number(*schema, trimmed, stored.number, self.weapon_types());
        if (check.verdict == fields::Verdict::refuse) {
            reason = prefixed(live.kind, live.name, trimmed, check.reason);
            return false;
        }
        const std::optional<std::string> spelled = tdf::format_number(stored.number);
        if (!spelled.has_value()) {
            reason = prefixed(
                live.kind, live.name, trimmed, "a value that is not finite cannot be stored"
            );
            return false;
        }
        text = *spelled;
    } else if (stored.kind == StoredKind::boolean) {
        const double number = stored.flag ? 1.0 : 0.0;
        if (schema.has_value())
            check = fields::check_number(*schema, trimmed, number, self.weapon_types());
        if (check.verdict == fields::Verdict::refuse) {
            reason = prefixed(live.kind, live.name, trimmed, check.reason);
            return false;
        }
        text = stored.flag ? "1" : "0";
    } else if (stored.kind == StoredKind::fixed_value) {
        const double as_number = static_cast<double>(stored.count) / 65536.0;
        if (schema.has_value())
            check = fields::check_number(*schema, trimmed, as_number, self.weapon_types());
        if (check.verdict == fields::Verdict::refuse) {
            reason = prefixed(live.kind, live.name, trimmed, check.reason);
            return false;
        }
        char spelled[host::fixed_text_bytes];
        if (!host::fixed_text(stored.count, spelled, sizeof spelled)) {
            reason = prefixed(
                live.kind, live.name, trimmed, "a value that is not finite cannot be stored"
            );
            return false;
        }
        text.assign(spelled);
    } else {
        const std::string_view body = trim_view(stored.text);
        if (schema.has_value())
            check = fields::check_text(*schema, trimmed, body, self.weapon_types());
        if (check.verdict == fields::Verdict::refuse) {
            reason = prefixed(live.kind, live.name, trimmed, check.reason);
            return false;
        }
        text.assign(body);
    }

    if (before.present && before.text.has_value() && *before.text == text)
        return true;

    const std::string& journal_lookup = before.present ? before.key : trimmed;
    const std::vector<std::string> earlier =
        earlier_writers(self, live.path, live.name, block_name, journal_lookup);
    uint32_t notes = earlier.empty() ? 0 : 1;
    if (check.verdict == fields::Verdict::warn)
        ++notes;
    if (!room_for(self, 1, notes, reason))
        return false;

    bool created = false;
    if (live.damage && live.block == nullptr) {
        tdf::EditStatus created_status = tdf::EditStatus::ok;
        live.block = tdf::add_child(live.document, live.section, "DAMAGE", &created_status);
        if (live.block == nullptr) {
            reason =
                prefixed(live.kind, live.name, trimmed, tdf::edit_status_message(created_status));
            return false;
        }
        created = true;
    }
    const std::string write_key = before.present ? before.key : trimmed;
    const tdf::EditStatus status = tdf::set_value(live.document, live.block, write_key, text);
    if (status != tdf::EditStatus::ok) {
        if (created) {
            const uint32_t count = tdf::child_count(live.section);
            if (count > 0 &&
                tdf::remove_child(live.document, live.section, count - 1) == tdf::EditStatus::ok)
                live.block = nullptr;
        }
        reason = prefixed(live.kind, live.name, trimmed, tdf::edit_status_message(status));
        return false;
    }
    const FieldNow after = field_now(live.block, trimmed);
    const std::string journal_key = after.present ? after.key : write_key;
    const std::optional<std::string> new_text =
        after.present ? after.text : std::optional<std::string>(text);
    journal_write(
        self,
        live,
        journal_key,
        before.present ? before.text : std::nullopt,
        new_text,
        check,
        earlier
    );
    rekey_if_renamed(self, live, trimmed);
    return true;
}

/// Drops download sections that name `unit_name`, and journals each one.
bool remove_download(Collections& self, std::string_view unit_name, std::string& reason) {
    const defs::DataLayout& layout = layout_of(self);
    const std::string& folder =
        layout.directories[static_cast<std::size_t>(defs::DataDirectory::download)];
    const std::vector<std::size_t> docs =
        CollectionsAccess::documents(self).documents_in(folder, "tdf", false);
    for (const std::size_t doc_index : docs) {
        rules::LoadedDocument& loaded = CollectionsAccess::documents(self).at(doc_index);
        tdf::Document* document = loaded.document.get();
        tdf::Block* root = tdf::editable_root(document);
        if (root == nullptr)
            continue;
        const uint32_t count = tdf::child_count(root);
        std::vector<uint32_t> matched;
        std::vector<std::string> names;
        bool first_matched = false;
        for (uint32_t index = 0; index < count; ++index) {
            tdf::Block* section = tdf::editable_child(root, index);
            if (section == nullptr)
                continue;
            const char* unit = tdf::find_value(section, "UNITNAME");
            const char* menu = tdf::find_value(section, "UNITMENU");
            const bool names_unit = (unit != nullptr && same_text(unit, unit_name)) ||
                                    (menu != nullptr && same_text(menu, unit_name));
            if (!names_unit)
                continue;
            matched.push_back(index);
            names.emplace_back(section->name != nullptr ? section->name : "");
            if (index == 0)
                first_matched = true;
        }
        for (std::size_t index = matched.size(); index > 0; --index) {
            const tdf::EditStatus status = tdf::remove_child(document, root, matched[index - 1]);
            if (status != tdf::EditStatus::ok) {
                reason =
                    prefixed(Kind::units, unit_name, "UNITNAME", tdf::edit_status_message(status));
                return false;
            }
        }
        for (const std::string& section_name : names) {
            RecordRemoval removal;
            removal.mutator = CollectionsAccess::mutator(self);
            removal.kind = RemovalKind::download_entry;
            removal.name = section_name;
            removal.path = loaded.path;
            CollectionsAccess::removals(self).push_back(std::move(removal));
        }
        if (tdf::child_count(root) == 0) {
            loaded.removed = true;
            RecordRemoval removal;
            removal.mutator = CollectionsAccess::mutator(self);
            removal.kind = RemovalKind::document;
            removal.name = file_name(loaded.path);
            removal.path = loaded.path;
            CollectionsAccess::removals(self).push_back(std::move(removal));
        } else if (first_matched) {
            if (!room_for(self, 0, 1, reason))
                return false;
            const tdf::Block* first = tdf::child_at(root, 0);
            const char* now = first != nullptr ? tdf::find_value(first, "UNITNAME") : nullptr;
            HookNote note;
            note.kind = HookNoteKind::download_first_entry_changed;
            note.mutators.push_back(CollectionsAccess::mutator(self));
            note.path = loaded.path;
            note.record = std::string(unit_name);
            note.key = "UNITNAME";
            note.message = "removing the first entry leaves ";
            if (now != nullptr && now[0] != '\0')
                note.message.append(now);
            else
                note.message.append("an entry with no UNITNAME");
            note.message.append(" as the unit this file marks");
            CollectionsAccess::notes(self).push_back(std::move(note));
            commit_tally(self, 0, 1);
        }
    }
    return true;
}

/// Another live section of `name` in the same document, or null.
tdf::Block* survivor_section(
    const std::vector<Rec>& records, uint32_t doc_index, std::string_view name, tdf::Block* removed
) {
    const auto start = std::lower_bound(
        records.begin(), records.end(), name, [](const Rec& rec, std::string_view sought) {
            return compare_folded(rec.name, sought) < 0;
        }
    );
    for (auto cursor = start; cursor != records.end() && same_text(cursor->name, name); ++cursor) {
        if (cursor->doc_index == doc_index && cursor->section != nullptr &&
            cursor->section != removed)
            return cursor->section;
    }
    return nullptr;
}

/// Drops one section from the index. `shift` moves later siblings down one place.
void forget_section(
    Collections& self,
    Kind kind,
    uint32_t doc_index,
    tdf::Block* section,
    std::string_view name,
    bool shift,
    uint32_t section_index
) {
    CollectionsState::Index& index = working(self).indexes[static_cast<std::size_t>(kind)];
    tdf::Block* survivor = survivor_section(index.records, doc_index, name, section);
    index.records.erase(
        std::remove_if(
            index.records.begin(),
            index.records.end(),
            [&](const Rec& rec) { return rec.doc_index == doc_index && rec.section == section; }
        ),
        index.records.end()
    );
    const auto doc_names = index.names.find(doc_index);
    if (doc_names != index.names.end()) {
        const auto named = doc_names->second.find(fold_copy(name));
        if (named != doc_names->second.end() && named->second.section == section)
            named->second.section = survivor;
    }
    for (Identity& id : working(self).identities) {
        if (id.doc_index != doc_index)
            continue;
        if (id.section == section)
            id.section = nullptr;
        else if (shift && id.section != nullptr && id.section_index > section_index)
            --id.section_index;
    }
    if (!shift)
        return;
    const auto listed = index.listed_children.find(doc_index);
    if (listed != index.listed_children.end() && section_index < listed->second)
        --listed->second;
}

/// Removes one unit document and the download entries that name it.
bool remove_unit(Collections& self, const Live& live, std::string& reason) {
    if (live.doc_index >= CollectionsAccess::documents(self).size()) {
        reason = removed_message(live.name);
        return false;
    }
    rules::LoadedDocument& loaded = CollectionsAccess::documents(self).at(live.doc_index);
    if (loaded.removed) {
        reason = removed_message(live.name);
        return false;
    }
    loaded.removed = true;
    forget_section(self, Kind::units, live.doc_index, live.section, live.name, false, 0);
    RecordRemoval removal;
    removal.mutator = CollectionsAccess::mutator(self);
    removal.kind = RemovalKind::unit;
    removal.name = live.name;
    removal.path = live.path;
    CollectionsAccess::removals(self).push_back(std::move(removal));
    return remove_download(self, live.name, reason);
}

/// Removes weapon or feature sections. Indices are taken before any removal.
bool remove_sections(
    Collections& self, Kind kind, const std::vector<Live>& lives, std::string& reason
) {
    std::vector<SectionDrop> drops;
    drops.reserve(lives.size());
    for (const Live& live : lives) {
        SectionDrop drop;
        drop.doc_index = live.doc_index;
        drop.section_index = live.section_index;
        drop.name = live.name;
        drop.path = live.path;
        drops.push_back(std::move(drop));
    }
    std::vector<uint32_t> doc_ids;
    for (const SectionDrop& drop : drops) {
        bool seen = false;
        for (const uint32_t doc_index : doc_ids) {
            if (doc_index == drop.doc_index)
                seen = true;
        }
        if (!seen)
            doc_ids.push_back(drop.doc_index);
    }
    for (const uint32_t doc_index : doc_ids) {
        if (doc_index >= CollectionsAccess::documents(self).size())
            continue;
        const uint32_t children =
            tdf::child_count(CollectionsAccess::documents(self).at(doc_index).document.root());
        uint32_t removing = 0;
        std::size_t last = 0;
        bool any = false;
        for (std::size_t index = 0; index < drops.size(); ++index) {
            if (drops[index].doc_index != doc_index)
                continue;
            ++removing;
            last = index;
            any = true;
        }
        if (any && removing == children)
            drops[last].journal_document = true;
    }
    std::vector<std::size_t> order(drops.size());
    for (std::size_t index = 0; index < order.size(); ++index)
        order[index] = index;
    std::sort(order.begin(), order.end(), [&](std::size_t left, std::size_t right) {
        const SectionDrop& a = drops[left];
        const SectionDrop& b = drops[right];
        if (a.doc_index != b.doc_index)
            return a.doc_index < b.doc_index;
        return a.section_index > b.section_index;
    });
    for (const std::size_t index : order) {
        const SectionDrop& drop = drops[index];
        if (drop.doc_index >= CollectionsAccess::documents(self).size()) {
            reason = removed_message(drop.name);
            return false;
        }
        rules::LoadedDocument& loaded = CollectionsAccess::documents(self).at(drop.doc_index);
        tdf::Block* root = tdf::editable_root(loaded.document.get());
        tdf::Block* section =
            root != nullptr ? tdf::editable_child(root, drop.section_index) : nullptr;
        const tdf::EditStatus status =
            tdf::remove_child(loaded.document.get(), root, drop.section_index);
        if (status != tdf::EditStatus::ok) {
            reason = prefixed(kind, drop.name, "", tdf::edit_status_message(status));
            return false;
        }
        if (section != nullptr)
            forget_section(
                self, kind, drop.doc_index, section, drop.name, true, drop.section_index
            );
    }
    for (const uint32_t doc_index : doc_ids) {
        if (doc_index >= CollectionsAccess::documents(self).size())
            continue;
        rules::LoadedDocument& loaded = CollectionsAccess::documents(self).at(doc_index);
        if (tdf::child_count(loaded.document.root()) == 0)
            loaded.removed = true;
    }
    for (const SectionDrop& drop : drops) {
        RecordRemoval removal;
        removal.mutator = CollectionsAccess::mutator(self);
        removal.kind = kind == Kind::weapons ? RemovalKind::weapon : RemovalKind::feature;
        removal.name = drop.name;
        removal.path = drop.path;
        CollectionsAccess::removals(self).push_back(std::move(removal));
        if (!drop.journal_document)
            continue;
        RecordRemoval document;
        document.mutator = CollectionsAccess::mutator(self);
        document.kind = RemovalKind::document;
        document.name = file_name(drop.path);
        document.path = drop.path;
        CollectionsAccess::removals(self).push_back(std::move(document));
    }
    return true;
}

/// Removes every live record. The grant has already been checked.
bool remove_lives(
    Collections& self, Kind kind, const std::vector<Live>& lives, std::string& reason
) {
    if (kind == Kind::units) {
        for (const Live& live : lives) {
            if (!remove_unit(self, live, reason))
                return false;
        }
        return true;
    }
    return remove_sections(self, kind, lives, reason);
}

/// Copies a record userdata into a handle the removal path can keep.
Handle handle_from(const RecordBox& box) {
    Handle handle;
    handle.kind = box.kind;
    handle.damage = box.damage != 0;
    handle.slot = box.slot;
    handle.name = std::string(name_of(&box));
    return handle;
}

/// The record userdata at `index`, or null.
const RecordBox* record_at(lua_State* state, int index) {
    return static_cast<const RecordBox*>(luaL_testudata(state, index, kRecordMeta));
}

const ViewBox* view_at(lua_State* state, int index, const char* meta) {
    return static_cast<const ViewBox*>(luaL_testudata(state, index, meta));
}

/// True when `key` is one of the five record methods, spelled exactly.
bool record_method(std::string_view key) noexcept {
    return key == "remove" || key == "get" || key == "set" || key == "keys" || key == "fixed";
}

/// Copies the records of a set, or the one record a method was called on.
bool collect_handles(lua_State* state, Collections& owner, int index) {
    CollectionsState& scratch = working(owner);
    scratch.handles.clear();
    if (const RecordBox* box = record_at(state, index)) {
        try {
            scratch.handles.push_back(handle_from(*box));
        } catch (const std::bad_alloc&) {
            scratch.pending_error = "out of memory";
            return false;
        }
        return true;
    }
    if (view_at(state, index, kSetMeta) == nullptr) {
        scratch.pending_error = "remove expects a record or a set";
        return false;
    }
    if (lua_getiuservalue(state, index, 1) != LUA_TTABLE) {
        lua_pop(state, 1);
        scratch.pending_error = "the set has no records";
        return false;
    }
    const int sequence = lua_absindex(state, -1);
    const lua_Unsigned length = lua_rawlen(state, sequence);
    charge_scan(state);
    for (lua_Unsigned slot = 1; slot <= length; ++slot) {
        charge_visits(state, 1);
        lua_rawgeti(state, sequence, static_cast<lua_Integer>(slot));
        const RecordBox* box = record_at(state, -1);
        if (box == nullptr) {
            lua_pop(state, 2);
            scratch.pending_error = "the set holds a value that is not a record";
            return false;
        }
        try {
            scratch.handles.push_back(handle_from(*box));
        } catch (const std::bad_alloc&) {
            lua_pop(state, 2);
            scratch.pending_error = "out of memory";
            return false;
        }
        lua_pop(state, 1);
    }
    lua_pop(state, 1);
    return true;
}

/// Reads the host's key sequence into the collections object. Keys stay raw.
bool read_keys(lua_State* state, Collections& self, int key_count, const char* type_error) {
    CollectionsState& scratch = working(self);
    scratch.keys.clear();
    const int sequence = lua_absindex(state, -1);
    charge_scan(state);
    for (int slot = 1; slot <= key_count; ++slot) {
        charge_visits(state, 1);
        lua_rawgeti(state, sequence, slot);
        if (lua_type(state, -1) != LUA_TSTRING) {
            lua_pop(state, 1);
            scratch.pending_error = type_error;
            return false;
        }
        std::size_t length = 0;
        const char* text = lua_tolstring(state, -1, &length);
        charge_text(state, length);
        try {
            scratch.keys.emplace_back(text != nullptr ? text : "", length);
        } catch (const std::bad_alloc&) {
            lua_pop(state, 1);
            scratch.pending_error = "out of memory";
            return false;
        }
        lua_pop(state, 1);
    }
    return true;
}

/// The owner of the collection or set at index 1, or null.
Collections* view_owner(lua_State* state, const char* meta) {
    const ViewBox* box = view_at(state, 1, meta);
    if (box == nullptr)
        return nullptr;
    return box->owner;
}

/// The owner of the collection or the set at index 1, or null.
Collections* any_view_owner(lua_State* state) {
    if (Collections* owner = view_owner(state, kSetMeta))
        return owner;
    return view_owner(state, kCollectionMeta);
}

/// Builds a set from the field-name sequence `where` pushed. The collection
/// is at index 1 and the needs table at index 2.
int where_from_keys(lua_State* state, Collections& owner, Kind kind, int key_count) {
    if (!read_keys(state, owner, key_count, "where: a field name must be a string"))
        return -1;
    CollectionsState& scratch = working(owner);
    scratch.needs.clear();
    const int table = 2;
    for (std::size_t index = 0; index < scratch.keys.size(); ++index) {
        charge_visits(state, 1);
        const std::string& field = scratch.keys[index];
        charge_text(state, field.size());
        lua_pushlstring(state, field.data(), field.size());
        lua_rawget(state, table);
        if (!need_from_lua(state, -1, trim_view(field), scratch.staging, &scratch.pending_error)) {
            lua_pop(state, 1);
            return -1;
        }
        try {
            scratch.needs.push_back(std::move(scratch.staging));
        } catch (const std::bad_alloc&) {
            lua_pop(state, 1);
            scratch.pending_error = "out of memory";
            return -1;
        }
        lua_pop(state, 1);
    }
    const std::vector<Rec>* indexed = nullptr;
    try {
        indexed = &ensure_index(owner, kind, state);
        scratch.matched.clear();
    } catch (const std::bad_alloc&) {
        scratch.pending_error = "out of memory";
        return -1;
    }
    if (indexed == nullptr) {
        scratch.pending_error = "out of memory";
        return -1;
    }
    const std::vector<Rec>& records = *indexed;
    // The match loops are C++. Charge them before any string is built, so a
    // spent allowance raises with no destructor alive.
    charge_scan(state);
    charge_visits(state, static_cast<uint64_t>(records.size()));
    charge_visits(state, saturating_product(records.size(), scratch.needs.size()));
    uint64_t compared_units = 0;
    for (const Rec& rec : records) {
        for (const sm::Need& need : scratch.needs) {
            const char* written = nullptr;
            if (rec.section != nullptr)
                written = tdf::find_value(rec.section, need.field.c_str());
            if (written == nullptr)
                continue;
            const uint64_t units = host::units_for_bytes(std::strlen(written));
            if (compared_units > UINT64_MAX - units)
                compared_units = UINT64_MAX;
            else
                compared_units += units;
        }
    }
    if (compared_units != 0)
        host::charge_work(state, compared_units);
    try {
        for (std::size_t index = 0; index < records.size(); ++index) {
            bool keep = true;
            for (const sm::Need& need : scratch.needs) {
                if (!rules::need_met(records[index].section, target_kind(kind), need)) {
                    keep = false;
                    break;
                }
            }
            if (!keep)
                continue;
            if (index > static_cast<std::size_t>(std::numeric_limits<uint32_t>::max())) {
                scratch.pending_error = "too many records";
                return -1;
            }
            if (records[index].name.size() >
                static_cast<std::size_t>(std::numeric_limits<uint32_t>::max())) {
                scratch.pending_error = "a record name is too long";
                return -1;
            }
            scratch.matched.push_back(static_cast<uint32_t>(index));
        }
    } catch (const std::bad_alloc&) {
        scratch.pending_error = "out of memory";
        return -1;
    }
    if (scratch.matched.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        scratch.pending_error = "too many records";
        return -1;
    }
    lua_createtable(state, static_cast<int>(scratch.matched.size()), 0);
    for (std::size_t index = 0; index < scratch.matched.size(); ++index) {
        const Rec& rec = records[scratch.matched[index]];
        push_record(state, &owner, kind, false, rec.slot, rec.name);
        lua_rawseti(state, -2, static_cast<lua_Integer>(index) + 1);
    }
    auto* box = static_cast<ViewBox*>(lua_newuserdatauv(state, sizeof(ViewBox), 1));
    box->owner = &owner;
    box->kind = kind;
    luaL_setmetatable(state, kSetMeta);
    lua_pushvalue(state, -2);
    lua_setiuservalue(state, -2, 1);
    return 1;
}

/// Pushes the records of the collection at index 1, or the snapshot the set
/// at index 1 already holds.
int push_members(lua_State* state, Collections& owner) {
    CollectionsState& scratch = working(owner);
    if (view_at(state, 1, kSetMeta) != nullptr) {
        if (lua_getiuservalue(state, 1, 1) != LUA_TTABLE) {
            lua_pop(state, 1);
            scratch.pending_error = "the set has no records";
            return -1;
        }
        return 1;
    }
    const ViewBox* collection = view_at(state, 1, kCollectionMeta);
    if (collection == nullptr) {
        scratch.pending_error = "expected a collection";
        return -1;
    }
    const Kind kind = collection->kind;
    try {
        const std::vector<Rec>& records = ensure_index(owner, kind, state);
        for (const Rec& rec : records) {
            if (rec.name.size() > static_cast<std::size_t>(std::numeric_limits<uint32_t>::max())) {
                scratch.pending_error = "a record name is too long";
                return -1;
            }
        }
    } catch (const std::bad_alloc&) {
        scratch.pending_error = "out of memory";
        return -1;
    }
    const std::vector<Rec>& records = ensure_index(owner, kind, state);
    if (records.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        scratch.pending_error = "too many records";
        return -1;
    }
    charge_scan(state);
    charge_visits(state, static_cast<uint64_t>(records.size()));
    lua_createtable(state, static_cast<int>(records.size()), 0);
    for (std::size_t index = 0; index < records.size(); ++index) {
        push_record(state, &owner, kind, false, records[index].slot, records[index].name);
        lua_rawseti(state, -2, static_cast<lua_Integer>(index) + 1);
    }
    return 1;
}

/// Resolves a handle without a userdata name tail. The message uses `handle.name`.
bool resolve_handle(Collections& self, const Handle& handle, Live& live, std::string& reason);

/// Applies the prepared assignments. Nothing here calls into Lua.
bool apply_assignments(lua_State* state, std::string& reason) {
    const ViewBox* set = view_at(state, 1, kSetMeta);
    if (set == nullptr || set->owner == nullptr) {
        reason = "set expects a set";
        return false;
    }
    if (!ensure_grant(*set->owner, set->kind, reason))
        return false;
    CollectionsState& scratch = working(*set->owner);
    for (const Handle& handle : scratch.handles) {
        Live live;
        if (!resolve_handle(*set->owner, handle, live, reason))
            return false;
    }
    for (const Assignment& assignment : scratch.assignments) {
        for (const Handle& handle : scratch.handles) {
            Live live;
            if (!resolve_handle(*set->owner, handle, live, reason))
                return false;
            if (!write_stored(*set->owner, live, assignment.key, assignment.value, reason))
                return false;
        }
    }
    return true;
}

/// Resolves a handle without a userdata name tail. The message uses `handle.name`.
bool resolve_handle(Collections& self, const Handle& handle, Live& live, std::string& reason) {
    RecordBox box{};
    box.owner = &self;
    box.kind = handle.kind;
    box.damage = handle.damage ? static_cast<uint8_t>(1) : static_cast<uint8_t>(0);
    box.slot = handle.slot;
    box.name_size = static_cast<uint32_t>(handle.name.size());
    return resolve(self, box, handle.name, live, reason);
}

/// Scales the present numeric fields of the set at index 1.
bool apply_scale(lua_State* state, std::string& reason) {
    const ViewBox* set = view_at(state, 1, kSetMeta);
    if (set == nullptr || set->owner == nullptr) {
        reason = "scale expects a set";
        return false;
    }
    if (!ensure_grant(*set->owner, set->kind, reason))
        return false;
    CollectionsState& scratch = working(*set->owner);
    for (const Handle& handle : scratch.handles) {
        Live live;
        if (!resolve_handle(*set->owner, handle, live, reason))
            return false;
    }
    for (const Factor& factor : scratch.factors) {
        for (const Handle& handle : scratch.handles) {
            Live live;
            if (!resolve_handle(*set->owner, handle, live, reason))
                return false;
            const FieldNow before = field_now(live.block, factor.key);
            if (!before.present || !before.text.has_value())
                continue;
            if (!starts_as_number(before.text->c_str())) {
                if (!room_for(*set->owner, 0, 1, reason))
                    return false;
                HookNote note;
                note.kind = HookNoteKind::not_a_number;
                note.mutators.push_back(CollectionsAccess::mutator(*set->owner));
                note.path = live.path;
                note.record = live.name;
                note.key = before.key;
                note.message = "the field text is not a number";
                CollectionsAccess::notes(*set->owner).push_back(std::move(note));
                commit_tally(*set->owner, 0, 1);
                continue;
            }
            const double current = tdf::parse_double(before.text->c_str());
            Stored stored;
            stored.kind = StoredKind::number;
            stored.number = current * factor.number;
            if (!write_stored(*set->owner, live, factor.key, stored, reason))
                return false;
        }
    }
    return true;
}

/// Removes the handles collected for the value at index 1.
bool remove_collected(Collections& self, Kind kind, std::string& reason) {
    if (!ensure_grant(self, kind, reason))
        return false;
    CollectionsState& scratch = working(self);
    for (const Handle& handle : scratch.handles) {
        if (handle.damage) {
            reason = "a damage block cannot be removed";
            return false;
        }
    }
    scratch.lives.clear();
    for (const Handle& handle : scratch.handles) {
        Live live;
        if (!resolve_handle(self, handle, live, reason))
            return false;
        if (live.damage) {
            reason = "a damage block cannot be removed";
            return false;
        }
        scratch.lives.push_back(std::move(live));
    }
    return remove_lives(self, kind, scratch.lives, reason);
}

/// Copies at most `size - 1` bytes of `text` and writes a NUL.
void copy_capped(const char* text, char* out, std::size_t size) {
    if (size == 0)
        return;
    if (text == nullptr) {
        out[0] = '\0';
        return;
    }
    const std::size_t length = std::strlen(text);
    const std::size_t keep = length + 1 <= size ? length : size - 1;
    if (keep > 0)
        std::memcpy(out, text, keep);
    out[keep] = '\0';
}

/// True when the exponent is not the all-ones pattern of a NaN or an infinity.
bool finite_bits(double value) noexcept {
    static_assert(sizeof(uint64_t) == sizeof(double));
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof bits);
    return ((bits >> 52) & 0x7ffu) != 0x7ffu;
}

/// The sign bit of `value`.
bool negative_bits(double value) noexcept {
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof bits);
    return (bits >> 63) != 0;
}

/// True when `value` is a NaN.
bool nan_bits(double value) noexcept {
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof bits);
    const auto exponent = static_cast<uint32_t>((bits >> 52) & 0x7ffu);
    const uint64_t fraction = bits & ((uint64_t{1} << 52) - 1u);
    return exponent == 0x7ffu && fraction != 0;
}

constexpr char k_fixed_range[] = "(-2147483648 to 2147483647.99998)";

/// Stores the fixed-library message for a value that cannot be a count.
void assign_fixed_error(Collections& owner, const char* shown, bool finite) {
    std::string message = "fixed: ";
    message += shown != nullptr ? shown : "";
    if (finite) {
        message += " is outside the range of fixed numbers ";
        message += k_fixed_range;
    } else {
        message += " is not a finite number";
    }
    pending_of(owner) = std::move(message);
}

/// The count for one field. 1 when `count` is set, 0 when the key is absent, -1 on error.
///
/// Present text is read with `fixed_from_text`. A key the block leaves out uses
/// `default_value`, then `fixed_from_number` (or `fixed_from_text` for text).
int prepare_fixed(lua_State* state, const RecordBox* box, std::string_view key, int64_t* count) {
    charge_visits(state, record_access_charge);
    const char* text = nullptr;
    std::optional<fields::RecordKind> schema;
    const tdf::Block* block = nullptr;
    {
        std::string reason;
        Live live;
        if (!resolve(*box->owner, *box, name_of(box), live, reason)) {
            pending_of(*box->owner) = std::move(reason);
            return -1;
        }
        schema = schema_of(live);
        block = live.block;
        const std::string owned(key);
        if (block != nullptr)
            text = tdf::find_value(block, owned.c_str());
    }
    charge_text(state, text != nullptr ? std::strlen(text) : 0);
    if (text != nullptr) {
        const std::optional<int64_t> got = host::fixed_from_text(text);
        if (!got.has_value()) {
            char shown[160];
            copy_capped(text, shown, sizeof shown);
            assign_fixed_error(*box->owner, shown, finite_bits(tdf::parse_double(text)));
            return -1;
        }
        *count = *got;
        return 1;
    }
    if (!schema.has_value())
        return 0;
    const fields::Value fallback = fields::default_value(*schema, key, block);
    if (fallback.kind == fields::Value::Kind::absent)
        return 0;
    std::optional<int64_t> got;
    bool finite = true;
    char shown[160];
    shown[0] = '\0';
    if (fallback.kind == fields::Value::Kind::text) {
        got = host::fixed_from_text(fallback.text.c_str());
        copy_capped(fallback.text.c_str(), shown, sizeof shown);
        finite = finite_bits(tdf::parse_double(fallback.text.c_str()));
    } else {
        double number = fallback.number;
        if (fallback.kind == fields::Value::Kind::integer)
            number = static_cast<double>(fallback.integer);
        else if (fallback.kind == fields::Value::Kind::boolean)
            number = fallback.boolean ? 1.0 : 0.0;
        got = host::fixed_from_number(number);
        finite = finite_bits(number);
        if (!finite) {
            const char* word = nan_bits(number) ? "nan" : (negative_bits(number) ? "-inf" : "inf");
            copy_capped(word, shown, sizeof shown);
        } else {
            const std::optional<std::string> spelled = tdf::format_number(number);
            if (spelled.has_value())
                copy_capped(spelled->c_str(), shown, sizeof shown);
        }
    }
    if (!got.has_value()) {
        assign_fixed_error(*box->owner, shown, finite);
        return -1;
    }
    *count = *got;
    return 1;
}

/// Fills the prepared read. The trampoline pushes it after this returns.
int prepare_read(lua_State* state, const RecordBox* box, std::string_view key) {
    charge_visits(state, record_access_charge);
    const char* text = nullptr;
    const fields::FieldSpec* spec = nullptr;
    {
        std::string reason;
        Live live;
        if (!resolve(*box->owner, *box, name_of(box), live, reason)) {
            pending_of(*box->owner) = std::move(reason);
            return -1;
        }
        const std::optional<fields::RecordKind> schema = schema_of(live);
        if (schema.has_value())
            spec = fields::find_field(*schema, key);
        const std::string owned(key);
        if (live.block != nullptr)
            text = tdf::find_value(live.block, owned.c_str());
    }
    charge_text(state, text != nullptr ? std::strlen(text) : 0);
    fields::Value value = fields::read_value(spec, text);
    // A whole number reads as written, not cast to its field's width, as
    // rules and scale read it: a value an earlier mutator stacked past the
    // range is not seen wrapped, and the run holds it once, at the end.
    if (value.kind == fields::Value::Kind::integer) {
        const std::optional<int64_t> whole = written_whole(text);
        if (whole.has_value())
            value.integer = *whole;
    }
    Prepared& prepared = working(*box->owner).prepared;
    prepared.kind = value.kind;
    prepared.integer = value.integer;
    prepared.number = value.number;
    prepared.flag = value.boolean;
    prepared.text = std::move(value.text);
    return 1;
}

/// Assigns one field. The value is already on the stack at index 3.
int assign_record(lua_State* state, const RecordBox* box, std::string_view key) {
    charge_visits(state, record_access_charge);
    {
        std::string reason;
        if (!ensure_grant(*box->owner, box->kind, reason)) {
            pending_of(*box->owner) = std::move(reason);
            return -1;
        }
        if (!read_stored(state, 3, working(*box->owner).stored, reason)) {
            pending_of(*box->owner) = prefixed(box->kind, name_of(box), key, reason);
            return -1;
        }
    }
    charge_text(state, stored_body_bytes(working(*box->owner).stored));
    std::string reason;
    Live live;
    if (!resolve(*box->owner, *box, name_of(box), live, reason)) {
        pending_of(*box->owner) = std::move(reason);
        return -1;
    }
    if (!write_stored(*box->owner, live, key, working(*box->owner).stored, reason)) {
        pending_of(*box->owner) = std::move(reason);
        return -1;
    }
    return 0;
}

/// Collects a record's keys into the collections object, in name order.
///
/// The charges sit outside every `try`: a spent allowance raises, and a
/// raise leaves a destructor unrun.
int prepare_keys(lua_State* state, const RecordBox* box) {
    const tdf::Block* block = nullptr;
    try {
        std::string reason;
        Live live;
        if (!resolve(*box->owner, *box, name_of(box), live, reason)) {
            pending_of(*box->owner) = std::move(reason);
            return -1;
        }
        block = live.block;
    } catch (const std::bad_alloc&) {
        pending_of(*box->owner) = "out of memory";
        return -1;
    }
    CollectionsState& scratch = working(*box->owner);
    scratch.keys.clear();
    const uint32_t property_count = block != nullptr ? tdf::property_count(block) : 0;
    charge_scan(state);
    charge_visits(state, property_count);
    for (uint32_t scan_index = 0; scan_index < property_count; ++scan_index) {
        const char* stored = tdf::property_key_at(block, static_cast<int32_t>(scan_index));
        if (stored == nullptr)
            continue;
        charge_text(state, std::strlen(stored));
    }
    try {
        for (uint32_t copy_index = 0; copy_index < property_count; ++copy_index) {
            const char* stored = tdf::property_key_at(block, static_cast<int32_t>(copy_index));
            if (stored == nullptr)
                continue;
            scratch.keys.emplace_back(stored);
        }
    } catch (const std::bad_alloc&) {
        pending_of(*box->owner) = "out of memory";
        return -1;
    }
    host::charge_work(state, host::comparison_unit * static_cast<uint64_t>(scratch.keys.size()));
    try {
        std::stable_sort(scratch.keys.begin(), scratch.keys.end(), host::name_less);
    } catch (const std::bad_alloc&) {
        pending_of(*box->owner) = "out of memory";
        return -1;
    }
    return 1;
}

/// Reports whether the record userdata still names a live section.
bool still_there(const RecordBox* box) {
    if (box == nullptr || box->owner == nullptr)
        return false;
    Live live;
    std::string reason;
    return resolve(*box->owner, *box, name_of(box), live, reason);
}

/// Writes the reason a need was refused. Returns false.
bool refuse_need(std::string* error, std::string_view field) {
    if (error == nullptr)
        return false;
    try {
        if (field.empty())
            *error = "a need must be a number, a boolean, a string or a form";
        else
            *error = std::string(field) + " is not a number, a boolean, a string or a form";
    } catch (const std::bad_alloc&) {
        try {
            *error = "out of memory";
        } catch (const std::bad_alloc&) {
        }
    }
    return false;
}

// Trampolines. A raising call happens here, after every C++ object in the frame has gone.
extern "C" {

static int form_range(lua_State* state);
static int form_is(lua_State* state);
static int form_has(lua_State* state);
static int form_absent(lua_State* state);
static int form_not(lua_State* state);
static int pairs_next(lua_State* state);
static int collection_where(lua_State* state);
static int collection_each(lua_State* state);
static int set_assign(lua_State* state);
static int set_scale(lua_State* state);
static int set_remove(lua_State* state);
static int record_call_remove(lua_State* state);
static int record_call_get(lua_State* state);
static int record_call_set(lua_State* state);
static int record_call_keys(lua_State* state);
static int record_call_fixed(lua_State* state);
static int collection_index(lua_State* state);
static int collection_call_clone(lua_State* state);
static int collection_newindex(lua_State* state);
static int collection_len(lua_State* state);
static int view_pairs(lua_State* state);
static int set_index(lua_State* state);
static int set_newindex(lua_State* state);
static int set_len(lua_State* state);
static int record_index(lua_State* state);
static int record_newindex(lua_State* state);

static int form_range(lua_State* state) {
    if (lua_gettop(state) < 2 || lua_type(state, 1) != LUA_TNUMBER ||
        lua_type(state, 2) != LUA_TNUMBER)
        return luaL_error(state, "range expects two numbers");
    void* memory = lua_newuserdatauv(state, sizeof(NeedBits), 1);
    auto* bits = static_cast<NeedBits*>(memory);
    bits->kind = static_cast<uint8_t>(sm::NeedKind::range);
    bits->negated = 0;
    bits->low = lua_tonumber(state, 1);
    bits->high = lua_tonumber(state, 2);
    lua_pushliteral(state, "");
    lua_setiuservalue(state, -2, 1);
    luaL_setmetatable(state, kNeedMeta);
    return 1;
}

static int form_is(lua_State* state) {
    if (lua_gettop(state) < 1 || lua_type(state, 1) != LUA_TSTRING)
        return luaL_error(state, "is expects a string");
    void* memory = lua_newuserdatauv(state, sizeof(NeedBits), 1);
    auto* bits = static_cast<NeedBits*>(memory);
    bits->kind = static_cast<uint8_t>(sm::NeedKind::equals);
    bits->negated = 0;
    bits->low = 0;
    bits->high = 0;
    lua_pushvalue(state, 1);
    lua_setiuservalue(state, -2, 1);
    luaL_setmetatable(state, kNeedMeta);
    return 1;
}

static int form_has(lua_State* state) {
    if (lua_gettop(state) < 1 || lua_type(state, 1) != LUA_TSTRING)
        return luaL_error(state, "has expects a string");
    void* memory = lua_newuserdatauv(state, sizeof(NeedBits), 1);
    auto* bits = static_cast<NeedBits*>(memory);
    bits->kind = static_cast<uint8_t>(sm::NeedKind::contains);
    bits->negated = 0;
    bits->low = 0;
    bits->high = 0;
    lua_pushvalue(state, 1);
    lua_setiuservalue(state, -2, 1);
    luaL_setmetatable(state, kNeedMeta);
    return 1;
}

static int form_absent(lua_State* state) {
    void* memory = lua_newuserdatauv(state, sizeof(NeedBits), 1);
    auto* bits = static_cast<NeedBits*>(memory);
    bits->kind = static_cast<uint8_t>(sm::NeedKind::contains);
    bits->negated = 1;
    bits->low = 0;
    bits->high = 0;
    lua_pushliteral(state, "");
    lua_setiuservalue(state, -2, 1);
    luaL_setmetatable(state, kNeedMeta);
    return 1;
}

static int form_not(lua_State* state) {
    const auto* existing = static_cast<const NeedBits*>(luaL_testudata(state, 1, kNeedMeta));
    if (existing == nullptr)
        return luaL_error(state, "not_ expects a need");
    if (existing->negated != 0)
        return luaL_error(state, "not_ of a not_");
    if (lua_getiuservalue(state, 1, 1) != LUA_TSTRING) {
        lua_pop(state, 1);
        lua_pushliteral(state, "");
    }
    void* memory = lua_newuserdatauv(state, sizeof(NeedBits), 1);
    auto* bits = static_cast<NeedBits*>(memory);
    bits->kind = existing->kind;
    bits->negated = 1;
    bits->low = existing->low;
    bits->high = existing->high;
    lua_pushvalue(state, -2);
    lua_setiuservalue(state, -2, 1);
    luaL_setmetatable(state, kNeedMeta);
    lua_replace(state, -2);
    return 1;
}

static int pairs_next(lua_State* state) {
    const bool skip = lua_toboolean(state, lua_upvalueindex(3)) != 0;
    for (;;) {
        charge_visits(state, 1);
        lua_settop(state, 0);
        const lua_Integer slot = lua_tointeger(state, lua_upvalueindex(2)) + 1;
        lua_pushinteger(state, slot);
        lua_replace(state, lua_upvalueindex(2));
        if (lua_rawgeti(state, lua_upvalueindex(1), slot) != LUA_TUSERDATA)
            return 1;
        const auto* box = static_cast<const RecordBox*>(lua_touserdata(state, -1));
        if (box == nullptr || box->owner == nullptr)
            return luaL_error(state, "pairs: the record is missing");
        if (skip) {
            bool alive = false;
            bool failed = false;
            try {
                alive = still_there(box);
            } catch (const std::bad_alloc&) {
                pending_of(*box->owner) = "out of memory";
                failed = true;
            }
            if (failed)
                return raise_pending(state, *box->owner);
            if (!alive) {
                lua_pop(state, 1);
                continue;
            }
        }
        if (box->name_size == 0)
            lua_pushliteral(state, "");
        else {
            const char* bytes = reinterpret_cast<const char*>(box) + sizeof(RecordBox);
            lua_pushlstring(state, bytes, box->name_size);
        }
        lua_insert(state, -2);
        return 2;
    }
}

static int collection_index(lua_State* state) {
    const ViewBox* collection = view_at(state, 1, kCollectionMeta);
    if (collection == nullptr || collection->owner == nullptr)
        return luaL_error(state, "expected a collection");
    if (lua_type(state, 2) != LUA_TSTRING) {
        lua_pushnil(state);
        return 1;
    }
    std::size_t length = 0;
    const char* text = lua_tolstring(state, 2, &length);
    const std::string_view key(text != nullptr ? text : "", length);
    if (key == "where") {
        lua_pushcfunction(state, collection_where);
        return 1;
    }
    if (key == "each") {
        lua_pushcfunction(state, collection_each);
        return 1;
    }
    if (key == "clone" && collection->kind != Kind::features) {
        lua_pushcfunction(state, collection_call_clone);
        return 1;
    }
    Collections& owner = *collection->owner;
    const Rec* found = nullptr;
    bool failed = false;
    try {
        found = find_named(owner, collection->kind, key, state);
        if (found != nullptr &&
            found->name.size() > static_cast<std::size_t>(std::numeric_limits<uint32_t>::max())) {
            pending_of(owner) = "a record name is too long";
            failed = true;
        }
    } catch (const std::bad_alloc&) {
        pending_of(owner) = "out of memory";
        failed = true;
    }
    if (failed)
        return raise_pending(state, owner);
    if (found == nullptr) {
        lua_pushnil(state);
        return 1;
    }
    push_record(state, &owner, collection->kind, false, found->slot, found->name);
    return 1;
}

static int collection_call_clone(lua_State* state) {
    const ViewBox* collection = view_at(state, 1, kCollectionMeta);
    if (collection == nullptr || collection->owner == nullptr)
        return luaL_error(state, "expected a collection");
    Collections& owner = *collection->owner;
    // Index and charge the collection before any C++ object exists, so the
    // clone's own lookups build nothing and charge nothing.
    bool failed = false;
    try {
        ensure_index(owner, collection->kind, state);
    } catch (const std::bad_alloc&) {
        pending_of(owner) = "out of memory";
        failed = true;
    }
    if (failed)
        return raise_pending(state, owner);
    uint64_t units = 0;
    int result = -1;
    {
        // Nothing in this block may raise: the clone bills its work in `units`
        // and a refusal waits in the pending error.
        std::string error;
        try {
            result =
                clone_collection(state, owner, collection->kind == Kind::weapons, error, units);
        } catch (const std::bad_alloc&) {
            pending_of(owner) = "out of memory";
            failed = true;
        }
        if (!failed && result < 0)
            pending_of(owner) = std::move(error);
    }
    // The block's objects are gone, so the charge may raise. A refused clone pays too.
    host::charge_work(state, units);
    if (failed || result < 0)
        return raise_pending(state, owner);
    // The new document is already in the set. Index the sections it added and
    // charge them as an add. A ready index is not cleared, so this is not a rebuild.
    try {
        ensure_index(owner, collection->kind, state);
    } catch (const std::bad_alloc&) {
        pending_of(owner) = "out of memory";
        failed = true;
    }
    if (failed)
        return raise_pending(state, owner);
    lua_pushboolean(state, 1);
    return 1;
}

static int collection_newindex(lua_State* state) {
    (void)state;
    return luaL_error(state, "a collection cannot be assigned");
}

static int collection_len(lua_State* state) {
    const ViewBox* collection = view_at(state, 1, kCollectionMeta);
    if (collection == nullptr || collection->owner == nullptr)
        return luaL_error(state, "expected a collection");
    std::size_t count = 0;
    bool failed = false;
    try {
        count = ensure_index(*collection->owner, collection->kind, state).size();
    } catch (const std::bad_alloc&) {
        pending_of(*collection->owner) = "out of memory";
        failed = true;
    }
    if (failed)
        return raise_pending(state, *collection->owner);
    if (count > static_cast<std::size_t>(std::numeric_limits<lua_Integer>::max()))
        return luaL_error(state, "too many records");
    lua_pushinteger(state, static_cast<lua_Integer>(count));
    return 1;
}

static int view_pairs(lua_State* state) {
    Collections* owner = any_view_owner(state);
    if (owner == nullptr)
        return luaL_error(state, "expected a collection");
    const bool skip = view_at(state, 1, kCollectionMeta) != nullptr;
    const int built = push_members(state, *owner);
    if (built < 0)
        return raise_pending(state, *owner);
    lua_pushinteger(state, 0);
    lua_pushboolean(state, skip ? 1 : 0);
    lua_pushcclosure(state, pairs_next, 3);
    lua_pushnil(state);
    lua_pushnil(state);
    return 3;
}

static int collection_each(lua_State* state) {
    if (!lua_isfunction(state, 2))
        return luaL_error(state, "each expects a function");
    Collections* owner = any_view_owner(state);
    if (owner == nullptr)
        return luaL_error(state, "expected a collection");
    const bool skip = view_at(state, 1, kCollectionMeta) != nullptr;
    const int built = push_members(state, *owner);
    if (built < 0)
        return raise_pending(state, *owner);
    const int sequence = lua_gettop(state);
    const lua_Unsigned length = lua_rawlen(state, sequence);
    if (length > static_cast<lua_Unsigned>(std::numeric_limits<lua_Integer>::max()))
        return luaL_error(state, "each: too many records");
    const auto last = static_cast<lua_Integer>(length);
    charge_scan(state);
    charge_visits(state, static_cast<uint64_t>(last));
    for (lua_Integer index = 1; index <= last; ++index) {
        if (skip) {
            lua_rawgeti(state, sequence, index);
            const auto* box = static_cast<const RecordBox*>(lua_touserdata(state, -1));
            bool alive = false;
            bool failed = false;
            try {
                alive = still_there(box);
            } catch (const std::bad_alloc&) {
                pending_of(*owner) = "out of memory";
                failed = true;
            }
            if (failed)
                return raise_pending(state, *owner);
            lua_pop(state, 1);
            if (!alive)
                continue;
        }
        lua_pushvalue(state, 2);
        lua_rawgeti(state, sequence, index);
        lua_call(state, 1, 0);
    }
    return 0;
}

static int collection_where(lua_State* state) {
    const ViewBox* collection = view_at(state, 1, kCollectionMeta);
    if (collection == nullptr || collection->owner == nullptr)
        return luaL_error(state, "where expects a collection");
    if (!lua_istable(state, 2))
        return luaL_error(state, "where expects a table");
    Collections& owner = *collection->owner;
    const Kind kind = collection->kind;
    const int key_count = host::push_sorted_keys(state, 2);
    const int result = where_from_keys(state, owner, kind, key_count);
    if (result < 0)
        return raise_pending(state, owner);
    return result;
}

static int set_index(lua_State* state) {
    if (view_at(state, 1, kSetMeta) == nullptr)
        return luaL_error(state, "expected a set");
    if (lua_type(state, 2) != LUA_TSTRING) {
        lua_pushnil(state);
        return 1;
    }
    std::size_t length = 0;
    const char* text = lua_tolstring(state, 2, &length);
    const std::string_view key(text != nullptr ? text : "", length);
    if (key == "each") {
        lua_pushcfunction(state, collection_each);
        return 1;
    }
    if (key == "set") {
        lua_pushcfunction(state, set_assign);
        return 1;
    }
    if (key == "scale") {
        lua_pushcfunction(state, set_scale);
        return 1;
    }
    if (key == "remove") {
        lua_pushcfunction(state, set_remove);
        return 1;
    }
    lua_pushnil(state);
    return 1;
}

static int set_newindex(lua_State* state) {
    (void)state;
    return luaL_error(state, "a set cannot be assigned");
}

static int set_len(lua_State* state) {
    if (view_at(state, 1, kSetMeta) == nullptr)
        return luaL_error(state, "expected a set");
    if (lua_getiuservalue(state, 1, 1) != LUA_TTABLE) {
        lua_pop(state, 1);
        lua_pushinteger(state, 0);
        return 1;
    }
    const lua_Unsigned length = lua_rawlen(state, -1);
    lua_pop(state, 1);
    lua_pushinteger(state, static_cast<lua_Integer>(length));
    return 1;
}

static int set_assign(lua_State* state) {
    if (view_at(state, 1, kSetMeta) == nullptr)
        return luaL_error(state, "set expects a set");
    if (!lua_istable(state, 2))
        return luaL_error(state, "set expects a table");
    Collections* owner = view_owner(state, kSetMeta);
    if (owner == nullptr)
        return luaL_error(state, "set expects a set");
    const int key_count = host::push_sorted_keys(state, 2);
    if (!read_keys(state, *owner, key_count, "a field name must be a string"))
        return raise_pending(state, *owner);
    if (!collect_handles(state, *owner, 1))
        return raise_pending(state, *owner);
    CollectionsState& scratch = working(*owner);
    scratch.assignments.clear();
    const ViewBox* set = view_at(state, 1, kSetMeta);
    const Kind kind = set != nullptr ? set->kind : Kind::units;
    const int table = 2;
    for (std::size_t index = 0; index < scratch.keys.size(); ++index) {
        charge_visits(state, 1);
        const std::string& raw = scratch.keys[index];
        charge_text(state, raw.size());
        lua_pushlstring(state, raw.data(), raw.size());
        lua_rawget(state, table);
        bool stored_ok = false;
        {
            std::string reason;
            Assignment assignment;
            try {
                stored_ok = read_stored(state, -1, assignment.value, reason);
                if (stored_ok) {
                    assignment.key.assign(trim_view(raw));
                    scratch.assignments.push_back(std::move(assignment));
                } else {
                    const std::string_view record = scratch.handles.empty()
                                                        ? std::string_view("record")
                                                        : std::string_view(scratch.handles[0].name);
                    pending_of(*owner) = prefixed(kind, record, trim_view(raw), reason);
                }
            } catch (const std::bad_alloc&) {
                pending_of(*owner) = "out of memory";
                stored_ok = false;
            }
        }
        lua_pop(state, 1);
        if (!stored_ok)
            return raise_pending(state, *owner);
    }
    charge_visits(state, static_cast<uint64_t>(scratch.handles.size()));
    charge_visits(state, saturating_product(scratch.handles.size(), scratch.assignments.size()));
    uint64_t body_units = 0;
    for (const Assignment& assignment : scratch.assignments) {
        const uint64_t part =
            repeated_units(stored_body_bytes(assignment.value), scratch.handles.size());
        if (body_units > UINT64_MAX - part)
            body_units = UINT64_MAX;
        else
            body_units += part;
    }
    if (body_units != 0)
        host::charge_work(state, body_units);
    bool ok = false;
    try {
        std::string reason;
        ok = apply_assignments(state, reason);
        if (!ok)
            pending_of(*owner) = std::move(reason);
    } catch (const std::bad_alloc&) {
        pending_of(*owner) = "out of memory";
        ok = false;
    }
    if (!ok)
        return raise_pending(state, *owner);
    return 0;
}

static int set_scale(lua_State* state) {
    if (view_at(state, 1, kSetMeta) == nullptr)
        return luaL_error(state, "scale expects a set");
    if (!lua_istable(state, 2))
        return luaL_error(state, "scale expects a table");
    Collections* owner = view_owner(state, kSetMeta);
    if (owner == nullptr)
        return luaL_error(state, "scale expects a set");
    const int key_count = host::push_sorted_keys(state, 2);
    if (!read_keys(state, *owner, key_count, "a field name must be a string"))
        return raise_pending(state, *owner);
    if (!collect_handles(state, *owner, 1))
        return raise_pending(state, *owner);
    CollectionsState& scratch = working(*owner);
    scratch.factors.clear();
    const ViewBox* set = view_at(state, 1, kSetMeta);
    const Kind kind = set != nullptr ? set->kind : Kind::units;
    const int table = 2;
    const std::string_view record = scratch.handles.empty()
                                        ? std::string_view("record")
                                        : std::string_view(scratch.handles[0].name);
    for (std::size_t index = 0; index < scratch.keys.size(); ++index) {
        charge_visits(state, 1);
        const std::string& raw = scratch.keys[index];
        charge_text(state, raw.size());
        lua_pushlstring(state, raw.data(), raw.size());
        lua_rawget(state, table);
        const bool number = lua_type(state, -1) == LUA_TNUMBER;
        const double factor = number ? lua_tonumber(state, -1) : 0;
        lua_pop(state, 1);
        const std::string_view key = trim_view(raw);
        if (!number) {
            try {
                pending_of(*owner) = prefixed(kind, record, key, "scale expects a number");
            } catch (const std::bad_alloc&) {
                pending_of(*owner) = "out of memory";
            }
            return raise_pending(state, *owner);
        }
        bool failed = false;
        try {
            Factor stored;
            stored.key.assign(key);
            stored.number = factor;
            scratch.factors.push_back(std::move(stored));
        } catch (const std::bad_alloc&) {
            pending_of(*owner) = "out of memory";
            failed = true;
        }
        if (failed)
            return raise_pending(state, *owner);
    }
    charge_visits(state, static_cast<uint64_t>(scratch.handles.size()));
    charge_visits(state, saturating_product(scratch.handles.size(), scratch.factors.size()));
    uint64_t scaled_units = 0;
    for (const Factor& factor : scratch.factors) {
        for (const Handle& handle : scratch.handles) {
            const char* text = nullptr;
            {
                Live live;
                std::string reason;
                if (!resolve_handle(*owner, handle, live, reason))
                    continue;
                if (live.block != nullptr)
                    text = tdf::find_value(live.block, factor.key.c_str());
            }
            if (text == nullptr)
                continue;
            const uint64_t units = host::units_for_bytes(std::strlen(text));
            if (scaled_units > UINT64_MAX - units)
                scaled_units = UINT64_MAX;
            else
                scaled_units += units;
        }
    }
    if (scaled_units != 0)
        host::charge_work(state, scaled_units);
    bool ok = false;
    try {
        std::string reason;
        ok = apply_scale(state, reason);
        if (!ok)
            pending_of(*owner) = std::move(reason);
    } catch (const std::bad_alloc&) {
        pending_of(*owner) = "out of memory";
        ok = false;
    }
    if (!ok)
        return raise_pending(state, *owner);
    return 0;
}

static int set_remove(lua_State* state) {
    const ViewBox* set = view_at(state, 1, kSetMeta);
    const RecordBox* record = record_at(state, 1);
    if (set == nullptr && record == nullptr)
        return luaL_error(state, "remove expects a record or a set");
    Collections* owner = set != nullptr ? set->owner : record->owner;
    const Kind kind = set != nullptr ? set->kind : record->kind;
    if (owner == nullptr)
        return luaL_error(state, "remove expects a record or a set");
    if (!collect_handles(state, *owner, 1))
        return raise_pending(state, *owner);
    // `remove_collected` walks the handles twice. Charge that before the
    // caller's reason string exists.
    charge_visits(state, saturating_product(working(*owner).handles.size(), 2));
    bool ok = false;
    try {
        std::string reason;
        ok = remove_collected(*owner, kind, reason);
        if (!ok)
            pending_of(*owner) = std::move(reason);
    } catch (const std::bad_alloc&) {
        pending_of(*owner) = "out of memory";
        ok = false;
    }
    if (!ok)
        return raise_pending(state, *owner);
    return 0;
}

static int record_index(lua_State* state) {
    const RecordBox* box = record_at(state, 1);
    if (box == nullptr || box->owner == nullptr)
        return luaL_error(state, "expected a record");
    if (lua_type(state, 2) != LUA_TSTRING) {
        lua_pushnil(state);
        return 1;
    }
    std::size_t length = 0;
    const char* text = lua_tolstring(state, 2, &length);
    const std::string_view key = trim_view(std::string_view(text != nullptr ? text : "", length));
    if (key == "remove") {
        lua_pushcfunction(state, record_call_remove);
        return 1;
    }
    if (key == "get") {
        lua_pushcfunction(state, record_call_get);
        return 1;
    }
    if (key == "set") {
        lua_pushcfunction(state, record_call_set);
        return 1;
    }
    if (key == "keys") {
        lua_pushcfunction(state, record_call_keys);
        return 1;
    }
    if (key == "fixed") {
        lua_pushcfunction(state, record_call_fixed);
        return 1;
    }
    if (box->kind == Kind::weapons && box->damage == 0 && same_text(key, "damage")) {
        if (box->name_size > static_cast<std::size_t>(std::numeric_limits<uint32_t>::max()))
            return luaL_error(state, "a record name is too long");
        push_record(state, box->owner, box->kind, true, box->slot, name_of(box));
        return 1;
    }
    int result = 0;
    try {
        result = prepare_read(state, box, key);
    } catch (const std::bad_alloc&) {
        pending_of(*box->owner) = "out of memory";
        result = -1;
    }
    if (result < 0)
        return raise_pending(state, *box->owner);
    push_prepared(state, working(*box->owner).prepared);
    return 1;
}

static int record_newindex(lua_State* state) {
    const RecordBox* box = record_at(state, 1);
    if (box == nullptr || box->owner == nullptr)
        return luaL_error(state, "expected a record");
    if (lua_type(state, 2) != LUA_TSTRING)
        return luaL_error(state, "a field name must be a string");
    std::size_t length = 0;
    const char* text = lua_tolstring(state, 2, &length);
    const std::string_view key = trim_view(std::string_view(text != nullptr ? text : "", length));
    if (record_method(key))
        return luaL_error(state, "a record method cannot be assigned");
    int result = 0;
    try {
        result = assign_record(state, box, key);
    } catch (const std::bad_alloc&) {
        pending_of(*box->owner) = "out of memory";
        result = -1;
    }
    if (result < 0)
        return raise_pending(state, *box->owner);
    return 0;
}

static int record_call_remove(lua_State* state) {
    return set_remove(state);
}

static int record_call_get(lua_State* state) {
    const RecordBox* box = record_at(state, 1);
    if (box == nullptr || box->owner == nullptr)
        return luaL_error(state, "get expects a record");
    if (lua_type(state, 2) != LUA_TSTRING)
        return luaL_error(state, "get expects a field name");
    std::size_t length = 0;
    const char* text = lua_tolstring(state, 2, &length);
    const std::string_view key = trim_view(std::string_view(text != nullptr ? text : "", length));
    int result = 0;
    try {
        result = prepare_read(state, box, key);
    } catch (const std::bad_alloc&) {
        pending_of(*box->owner) = "out of memory";
        result = -1;
    }
    if (result < 0)
        return raise_pending(state, *box->owner);
    push_prepared(state, working(*box->owner).prepared);
    return 1;
}

static int record_call_set(lua_State* state) {
    const RecordBox* box = record_at(state, 1);
    if (box == nullptr || box->owner == nullptr)
        return luaL_error(state, "set expects a record");
    if (lua_type(state, 2) != LUA_TSTRING)
        return luaL_error(state, "set expects a field name");
    if (lua_gettop(state) < 3 || lua_type(state, 3) == LUA_TNONE)
        return luaL_error(state, "set expects a value");
    std::size_t length = 0;
    const char* text = lua_tolstring(state, 2, &length);
    const std::string_view key = trim_view(std::string_view(text != nullptr ? text : "", length));
    int result = 0;
    try {
        result = assign_record(state, box, key);
    } catch (const std::bad_alloc&) {
        pending_of(*box->owner) = "out of memory";
        result = -1;
    }
    if (result < 0)
        return raise_pending(state, *box->owner);
    return 0;
}

static int record_call_fixed(lua_State* state) {
    const RecordBox* box = record_at(state, 1);
    if (box == nullptr || box->owner == nullptr)
        return luaL_error(state, "fixed expects a record");
    if (lua_type(state, 2) != LUA_TSTRING)
        return luaL_error(state, "fixed expects a field name");
    std::size_t length = 0;
    const char* text = lua_tolstring(state, 2, &length);
    const std::string_view key = trim_view(std::string_view(text != nullptr ? text : "", length));
    int64_t count = 0;
    int result = 0;
    try {
        result = prepare_fixed(state, box, key, &count);
    } catch (const std::bad_alloc&) {
        pending_of(*box->owner) = "out of memory";
        result = -1;
    }
    if (result < 0)
        return raise_pending(state, *box->owner);
    if (result == 0) {
        lua_pushnil(state);
        return 1;
    }
    host::push_fixed(state, count);
    return 1;
}

static int record_call_keys(lua_State* state) {
    const RecordBox* box = record_at(state, 1);
    if (box == nullptr || box->owner == nullptr)
        return luaL_error(state, "keys expects a record");
    const int result = prepare_keys(state, box);
    if (result < 0)
        return raise_pending(state, *box->owner);
    const std::vector<std::string>& keys = working(*box->owner).keys;
    if (keys.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        pending_of(*box->owner) = "too many keys";
        return raise_pending(state, *box->owner);
    }
    lua_createtable(state, static_cast<int>(keys.size()), 0);
    for (std::size_t index = 0; index < keys.size(); ++index) {
        lua_pushlstring(state, keys[index].data(), keys[index].size());
        lua_rawseti(state, -2, static_cast<lua_Integer>(index) + 1);
    }
    return 1;
}

} // extern "C"

/// Installs the four protected metatables.
void ensure_all(lua_State* state) {
    ensure_meta(state, kNeedMeta, nullptr, nullptr, nullptr, nullptr, nullptr);
    ensure_meta(
        state,
        kCollectionMeta,
        collection_index,
        collection_newindex,
        collection_len,
        view_pairs,
        nullptr
    );
    ensure_meta(state, kSetMeta, set_index, set_newindex, set_len, view_pairs, nullptr);
    ensure_meta(state, kRecordMeta, record_index, record_newindex, nullptr, nullptr, nullptr);
}

/// Pushes one collection view.
void push_view(lua_State* state, Collections* owner, Kind kind) {
    ensure_all(state);
    auto* box = static_cast<ViewBox*>(lua_newuserdatauv(state, sizeof(ViewBox), 0));
    box->owner = owner;
    box->kind = kind;
    luaL_setmetatable(state, kCollectionMeta);
}

} // namespace

bool clone_find(
    Collections& collections, bool weapons, std::string_view name, CloneHit& hit, uint64_t& units
) {
    const Kind kind = weapons ? Kind::weapons : Kind::units;
    // The caller indexed and charged the collection first, so a null state
    // finds the record without building, charging or raising. The lookup
    // still checks every document for growth, as `units.NAME` pays for.
    const Rec* found = find_named(collections, kind, name, nullptr);
    uint64_t visits = static_cast<uint64_t>(CollectionsAccess::documents(collections).size());
    if (host::element_charge != 0 && visits > UINT64_MAX / host::element_charge)
        visits = UINT64_MAX / host::element_charge;
    add_saturated(units, visits * host::element_charge);
    add_saturated(units, record_access_charge * host::element_charge);
    add_saturated(units, host::units_for_bytes(name.size()));
    if (found == nullptr)
        return false;
    hit.doc_index = found->doc_index;
    hit.section = found->section;
    hit.name = found->name;
    hit.path = found->path;
    return true;
}

bool need_from_lua(
    lua_State* state, int index, std::string_view field, sm::Need& need, std::string* error
) {
    const int type = lua_type(state, index);
    const NeedBits* bits = nullptr;
    const char* text_ptr = nullptr;
    std::size_t text_length = 0;
    bool have_text = false;
    bool pop_text = false;
    sm::NeedKind kind = sm::NeedKind::range;
    bool negated = false;
    double low = 0;
    double high = 0;
    if (type == LUA_TNUMBER) {
        kind = sm::NeedKind::range;
        low = lua_tonumber(state, index);
        high = low;
    } else if (type == LUA_TBOOLEAN) {
        kind = sm::NeedKind::range;
        low = 1;
        high = 1;
        negated = lua_toboolean(state, index) == 0;
    } else if (type == LUA_TSTRING) {
        kind = sm::NeedKind::equals;
        text_ptr = lua_tolstring(state, index, &text_length);
        have_text = true;
    } else if (type == LUA_TUSERDATA) {
        if (map_match_value(state, index)) {
            if (error != nullptr) {
                try {
                    *error = std::string(map_match_refusal);
                } catch (const std::bad_alloc&) {
                }
            }
            return false;
        }
        bits = static_cast<const NeedBits*>(luaL_testudata(state, index, kNeedMeta));
        if (bits == nullptr)
            return refuse_need(error, field);
        const int value_type = lua_getiuservalue(state, index, 1);
        pop_text = true;
        if (value_type == LUA_TSTRING) {
            text_ptr = lua_tolstring(state, -1, &text_length);
            have_text = true;
        }
        kind = static_cast<sm::NeedKind>(bits->kind);
        negated = bits->negated != 0;
        low = bits->low;
        high = bits->high;
    } else {
        return refuse_need(error, field);
    }
    try {
        sm::Need built;
        built.field.assign(field);
        built.kind = kind;
        built.negated = negated;
        built.low = low;
        built.high = high;
        if (have_text && text_ptr != nullptr)
            built.text.assign(text_ptr, text_length);
        if (pop_text)
            lua_pop(state, 1);
        need = std::move(built);
        return true;
    } catch (const std::bad_alloc&) {
        if (pop_text)
            lua_pop(state, 1);
        if (error != nullptr) {
            try {
                *error = "out of memory";
            } catch (const std::bad_alloc&) {
            }
        }
        return false;
    }
}

void install_selection_forms(lua_State* state) {
    ensure_all(state);
    install_map_match_forms(state);
    lua_pushcfunction(state, form_range);
    lua_setglobal(state, "range");
    lua_pushcfunction(state, form_is);
    lua_setglobal(state, "is");
    lua_pushcfunction(state, form_has);
    lua_setglobal(state, "has");
    lua_pushcfunction(state, form_absent);
    lua_setglobal(state, "absent");
    lua_pushcfunction(state, form_not);
    lua_setglobal(state, "not_");
}

Collections::Collections(rules::DocumentSet& documents, const defs::DataLayout* layout)
    : documents_(&documents), layout_(layout), state_(std::make_unique<CollectionsState>()) {
}

Collections::~Collections() = default;

void Collections::begin_mutator(std::string mutator, WriteGrant grant) {
    mutator_ = std::move(mutator);
    grant_ = grant;
    active_ = true;
    side_scratch_ = SideScratch{};
    // The row outlives the turn, so a later phase keeps the same count.
    (void)tally_index(*this);
}

bool Collections::write_limit_failed() const noexcept {
    if (mutator_.empty() || state_ == nullptr)
        return false;
    for (const CollectionsState::Tally& tally : state_->tallies) {
        if (tally.mutator == mutator_)
            return tally.over_writes;
    }
    return false;
}

bool Collections::allow_field_write() noexcept {
    if (mutator_.empty() || state_ == nullptr)
        return false;
    for (CollectionsState::Tally& tally : state_->tallies) {
        if (tally.mutator != mutator_)
            continue;
        if (tally.over_writes || tally.writes > mutator_write_limit - 1) {
            tally.over_writes = true;
            return false;
        }
        return true;
    }
    return false;
}

void Collections::count_field_write() noexcept {
    if (mutator_.empty() || state_ == nullptr)
        return;
    for (CollectionsState::Tally& tally : state_->tallies) {
        if (tally.mutator != mutator_)
            continue;
        if (tally.writes < mutator_write_limit)
            ++tally.writes;
        return;
    }
}

Collections::SideScratch& Collections::side_scratch() noexcept {
    return side_scratch_;
}

int Collections::lookup_unit(lua_State* state, std::string_view sought) {
    const CollectionsState::Rec* found = nullptr;
    try {
        found = find_named(*this, CollectionsState::Kind::units, sought, state);
    } catch (const std::bad_alloc&) {
        hold_error("out of memory");
        return -1;
    }
    SideScratch& scratch = side_scratch_;
    if (found == nullptr || found->section == nullptr) {
        scratch.found = false;
        scratch.builder_unit = false;
        return 0;
    }
    const uint32_t properties = oa::formats::tdf::property_count(found->section);
    if (state != nullptr) {
        charge_visits(state, properties == 0 ? uint64_t{1} : properties);
        charge_text(state, found->name.size());
    }
    scratch.found = true;
    scratch.builder_unit = oa::formats::tdf::get_int(found->section, "builder", 0) != 0;
    try {
        scratch.text.assign(found->name);
    } catch (const std::bad_alloc&) {
        hold_error("out of memory");
        return -1;
    }
    return 1;
}

const std::string* Collections::indexed_unit(std::string_view sought, bool* builder) noexcept {
    if (builder != nullptr)
        *builder = false;
    const CollectionsState::Index& index =
        state_->indexes[static_cast<std::size_t>(CollectionsState::Kind::units)];
    if (!index.ready)
        return nullptr;
    const auto found = std::lower_bound(
        index.records.begin(),
        index.records.end(),
        sought,
        [](const CollectionsState::Rec& rec, std::string_view key) {
            return compare_folded(rec.name, key) < 0;
        }
    );
    if (found == index.records.end() || !same_text(found->name, sought) ||
        found->section == nullptr)
        return nullptr;
    if (builder != nullptr)
        *builder = oa::formats::tdf::get_int(found->section, "builder", 0) != 0;
    return &found->name;
}

int Collections::has_record(lua_State* state, bool feature, std::string_view sought) {
    const CollectionsState::Rec* found = nullptr;
    try {
        found = find_named(
            *this,
            feature ? CollectionsState::Kind::features : CollectionsState::Kind::units,
            sought,
            state
        );
    } catch (const std::bad_alloc&) {
        return -1;
    }
    if (found == nullptr || found->section == nullptr)
        return 0;
    if (state != nullptr)
        charge_text(state, found->name.size());
    return 1;
}

void Collections::end_mutator() {
    active_ = false;
    mutator_.clear();
    grant_ = {};
}

void Collections::note_rename(bool weapon, std::string from, std::string to) {
    ClashRename rename;
    rename.weapon = weapon;
    rename.from = std::move(from);
    rename.to = std::move(to);
    renames_.push_back(std::move(rename));
}

const std::string* Collections::renamed_to(bool weapon, std::string_view name) const noexcept {
    const std::string* found = nullptr;
    for (const ClashRename& rename : renames_) {
        if (rename.weapon != weapon)
            continue;
        if (!same_text(rename.from, name))
            continue;
        found = &rename.to;
    }
    return found;
}

std::string Collections::private_name_of(bool weapon, std::string_view name) const {
    const std::string* found = renamed_to(weapon, name);
    if (found == nullptr)
        return {};
    return *found;
}

void Collections::add_served_copy(ServedCopy copy) {
    std::string key;
    key.reserve(copy.path.size());
    for (const char character : copy.path) {
        unsigned char byte = static_cast<unsigned char>(character);
        if (byte == '\\')
            byte = '/';
        key.push_back(static_cast<char>(fold_byte(byte)));
    }
    const std::size_t index = served_.size();
    served_.push_back(std::move(copy));
    served_index_[std::move(key)] = index;
}

void Collections::note_unit_file_shift(UnitFileShift shift) {
    unit_shifts_.push_back(std::move(shift));
}

void Collections::note_menu_spill(std::string unit, std::string path) {
    MenuSpill spill;
    spill.unit = std::move(unit);
    spill.path = std::move(path);
    menu_spills_.push_back(std::move(spill));
}

namespace {

/// True when `record` is `CANBUILD.` and a builder name.
///
/// @param record the journal record
/// @param[out] builder the builder name
/// @return true when the record names a build-list block
bool canbuild_record(std::string_view record, std::string& builder) {
    constexpr std::string_view prefix = "canbuild.";
    if (record.size() < prefix.size())
        return false;
    for (std::size_t index = 0; index < prefix.size(); ++index) {
        unsigned char byte = static_cast<unsigned char>(record[index]);
        if (byte >= 'A' && byte <= 'Z')
            byte = static_cast<unsigned char>(byte - 'A' + 'a');
        if (byte != static_cast<unsigned char>(prefix[index]))
            return false;
    }
    builder.assign(record.substr(prefix.size()));
    return true;
}

/// The block a field write names, or null when it is gone.
///
/// @param loaded the document
/// @param write the write
/// @return the block
oa::formats::tdf::Block*
block_of_write(rules::LoadedDocument& loaded, const rules::FieldWrite& write) {
    namespace tdf = oa::formats::tdf;
    tdf::Block* root = tdf::editable_root(loaded.document.get());
    if (root == nullptr)
        return nullptr;
    std::string builder;
    if (canbuild_record(write.record, builder)) {
        tdf::Block* section = tdf::editable_child(root, "CANBUILD");
        tdf::Block* block =
            section != nullptr ? tdf::editable_child(section, builder.c_str()) : nullptr;
        if (block != nullptr && !write.block.empty())
            block = tdf::editable_child(block, write.block.c_str());
        return block;
    }
    tdf::Block* section = tdf::editable_child(root, write.record.c_str());
    if (section == nullptr) {
        const uint32_t children = tdf::child_count(root);
        for (uint32_t child_index = 0; child_index < children; ++child_index) {
            tdf::Block* child = tdf::editable_child(root, child_index);
            if (child == nullptr || child->name == nullptr)
                continue;
            if (tdf::compare_nocase(child->name, "UNITINFO") != 0)
                continue;
            const char* unit_name = tdf::find_value(child, "UnitName");
            if (unit_name != nullptr && tdf::compare_nocase(unit_name, write.record.c_str()) == 0) {
                section = child;
                break;
            }
        }
    }
    if (section != nullptr && !write.block.empty())
        section = tdf::editable_child(section, write.block.c_str());
    return section;
}

} // namespace

bool Collections::retarget_renamed_references(std::string_view mutator, std::string& error) {
    return retarget_renamed_references(writes_, mutator, error);
}

bool Collections::retarget_renamed_references(
    std::span<rules::FieldWrite> writes, std::string_view mutator, std::string& error
) {
    namespace tdf = oa::formats::tdf;
    if (renames_.empty() || documents_ == nullptr)
        return true;
    for (rules::FieldWrite& write : writes) {
        bool owned = false;
        for (const std::string& id : write.mutators) {
            if (id == mutator)
                owned = true;
        }
        if (!owned || !write.new_text.has_value())
            continue;
        const bool weapon_field = weapon_reference_key(write.key);
        const bool unit_field =
            unit_list_key(write.key) || tdf::compare_nocase(write.key.c_str(), "unitname") == 0;
        if (weapon_field == unit_field)
            continue;
        const std::string mapped = private_name_of(weapon_field, *write.new_text);
        if (mapped.empty() || tdf::compare_nocase(mapped.c_str(), write.new_text->c_str()) == 0)
            continue;
        rules::LoadedDocument* loaded = documents_->find(write.path);
        if (loaded == nullptr) {
            error = write.path + ": the document disappeared";
            return false;
        }
        tdf::Block* block = block_of_write(*loaded, write);
        if (block == nullptr) {
            error = write.path + ": " + write.record + " disappeared";
            return false;
        }
        const tdf::EditStatus status =
            tdf::set_value(loaded->document.get(), block, write.key, mapped);
        if (status != tdf::EditStatus::ok) {
            error = tdf::edit_status_message(status);
            return false;
        }
        write.new_text = mapped;
    }
    return true;
}

void Collections::keep_weapon_ids(std::span<const std::string> names) {
    kept_weapon_ids_.clear();
    kept_weapon_ids_.reserve(names.size());
    for (const std::string& name : names)
        kept_weapon_ids_.push_back(fold_copy(name));
    std::sort(kept_weapon_ids_.begin(), kept_weapon_ids_.end());
    kept_weapon_ids_.erase(
        std::unique(kept_weapon_ids_.begin(), kept_weapon_ids_.end()), kept_weapon_ids_.end()
    );
}

void Collections::release_document(std::string_view path) {
    if (documents_ == nullptr || state_ == nullptr)
        return;
    rules::LoadedDocument* loaded = documents_->find(path);
    if (loaded == nullptr)
        return;
    std::size_t doc_place = documents_->size();
    for (std::size_t cursor = 0; cursor < documents_->size(); ++cursor) {
        if (&documents_->at(cursor) == loaded) {
            doc_place = cursor;
            break;
        }
    }
    if (doc_place >= documents_->size())
        return;
    if (doc_place > static_cast<std::size_t>(std::numeric_limits<uint32_t>::max()))
        return;
    const auto narrow = static_cast<uint32_t>(doc_place);
    CollectionsState& scratch = *state_;
    for (CollectionsState::Index& index : scratch.indexes) {
        index.records.erase(
            std::remove_if(
                index.records.begin(),
                index.records.end(),
                [narrow](const CollectionsState::Rec& rec) { return rec.doc_index == narrow; }
            ),
            index.records.end()
        );
        index.names.erase(narrow);
        index.listed_children.erase(narrow);
    }
    for (auto slot = scratch.slots.begin(); slot != scratch.slots.end();) {
        if (slot->first.doc_index == narrow)
            slot = scratch.slots.erase(slot);
        else
            ++slot;
    }
    for (CollectionsState::Identity& identity : scratch.identities) {
        if (identity.doc_index == narrow)
            identity.section = nullptr;
    }
}

void Collections::push_units(lua_State* state) {
    push_view(state, this, Kind::units);
}

void Collections::push_weapons(lua_State* state) {
    push_view(state, this, Kind::weapons);
}

void Collections::push_features(lua_State* state) {
    push_view(state, this, Kind::features);
}

void Collections::set_context_collections(lua_State* state, int ctx_index) {
    const int absolute = lua_absindex(state, ctx_index);
    push_units(state);
    lua_setfield(state, absolute, "units");
    push_weapons(state);
    lua_setfield(state, absolute, "weapons");
    push_features(state);
    lua_setfield(state, absolute, "features");
}

std::span<const rules::FieldWrite> Collections::writes() const {
    return writes_;
}

std::span<const RecordRemoval> Collections::removals() const {
    return removals_;
}

std::span<const HookNote> Collections::notes() const {
    return notes_;
}

bool Collections::accept_write(rules::FieldWrite write, std::string& reason) {
    const std::string path = write.path;
    const std::string record = write.record;
    const std::string block = write.block;
    const std::string key = write.key;
    const std::vector<std::string> earlier = earlier_writers(*this, path, record, block, key);
    const uint32_t notes = earlier.empty() ? 0u : 1u;
    if (!room_for(*this, 1u, notes, reason))
        return false;
    writes_.push_back(std::move(write));
    if (!earlier.empty()) {
        HookNote note;
        note.kind = HookNoteKind::hook_overwrote;
        note.mutators = earlier;
        note.mutators.push_back(mutator_);
        note.path = path;
        note.record = record;
        note.key = key;
        note.message = mutator_;
        note.message.append(" replaced the value written by ");
        note.message.append(earlier[0]);
        for (std::size_t index = 1; index < earlier.size(); ++index) {
            note.message.append(" and ");
            note.message.append(earlier[index]);
        }
        notes_.push_back(std::move(note));
    }
    remember_writer(*this, path, record, block, key);
    commit_tally(*this, 1u, notes);
    return true;
}

bool Collections::accept_note(HookNote note, std::string& reason) {
    if (!room_for(*this, 0u, 1u, reason))
        return false;
    notes_.push_back(std::move(note));
    commit_tally(*this, 0u, 1u);
    return true;
}

void Collections::hold_error(std::string message) {
    std::string& pending = working(*this).pending_error;
    if (pending.empty())
        pending = std::move(message);
}

int Collections::raise_held(lua_State* state) {
    std::string& text = working(*this).pending_error;
    if (text.empty())
        text = "the hook stopped";
    lua_pushlstring(state, text.data(), text.size());
    text.clear();
    return lua_error(state);
}

} // namespace oa::data::mutators
