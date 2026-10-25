// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Sides and the other game-data tables on a hook's ctx.
//
// Sides are read from the language variant of sidedata when that file is
// present, because load_side_data reads the variant. Build lists are read
// from the plain gamedata/sidedata.tdf, because the build-list loader
// always passes an empty variant. When those paths differ, each document
// is served at the path its own reader opens.
//
// Every walk is charged against the hook's instruction allowance, at the
// same rates as the unit collections. A callback computes into the
// collections' working storage and returns before it pushes into Lua, so a
// raising charge or a Lua allocation cannot skip a destructor.
#include "collections_sides.hpp"

#include "oa/data/defs/asset_files.hpp"
#include "oa/data/mutators/allow_list.hpp"
#include "oa/data/script_host/script_host.hpp"
#include "oa/formats/tdf/edit.hpp"

#include "lua.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace oa::data::mutators {
namespace rules = oa::data::mutation_rules;
namespace tdf = oa::formats::tdf;
namespace defs = oa::data::defs;
namespace host = oa::data::script_host;

/// The sides and tables collections reach the journal through here.
struct SideCollectionsAccess {
    static rules::DocumentSet& documents(Collections& self) noexcept { return *self.documents_; }

    static bool active(const Collections& self) noexcept { return self.active_; }

    static const std::string& mutator(const Collections& self) noexcept { return self.mutator_; }

    static const WriteGrant& grant(const Collections& self) noexcept { return self.grant_; }

    static bool accept_write(Collections& self, rules::FieldWrite write, std::string& reason) {
        return self.accept_write(std::move(write), reason);
    }

    static bool accept_note(Collections& self, HookNote note, std::string& reason) {
        return self.accept_note(std::move(note), reason);
    }

    static void hold_error(Collections& self, std::string message) {
        self.hold_error(std::move(message));
    }

    static int raise(Collections& self, lua_State* state) { return self.raise_held(state); }

    static Collections::SideScratch& scratch(Collections& self) noexcept {
        return self.side_scratch();
    }

    static int lookup(Collections& self, lua_State* state, std::string_view sought) {
        return self.lookup_unit(state, sought);
    }
};

namespace {

constexpr uint32_t k_list_kept = 30;
constexpr uint32_t k_list_cap = 1024;
constexpr std::size_t k_name_limit = 29;
constexpr std::size_t k_prefix_limit = 3;
constexpr std::size_t k_commander_limit = 31;
constexpr std::size_t k_intgaf_limit = 29;
constexpr std::size_t k_font_limit = 255;
constexpr std::size_t k_text_limit = tdf::max_edited_value_bytes;
constexpr std::size_t k_no_doc = std::numeric_limits<std::size_t>::max();
/// One field read or write, matching the unit collections' record access.
constexpr uint64_t k_field_charge = 8;
constexpr const char* k_list_note =
    "the AI keeps only the profile's limits.build-list-entries count (30 by default)";
constexpr const char* k_table_refuse = " is not a file directly in gamedata";

constexpr const char* k_sides_meta = "oa.mutators.sides";
constexpr const char* k_side_meta = "oa.mutators.side";
constexpr const char* k_tables_meta = "oa.mutators.tables";
constexpr const char* k_table_meta = "oa.mutators.table";
constexpr const char* k_section_meta = "oa.mutators.section";

enum class BoxKind : uint8_t { sides, side, tables, table, section };

/// Pointers a callback may use. None of them is owned here.
struct Box {
    Collections* collections{};
    const oa::AssetStore* store{};
    const defs::DataLayout* layout{};
    const std::string* variant{};
    BoxKind kind{BoxKind::sides};
    uint32_t side_slot{};
    std::size_t doc_index{k_no_doc};
    tdf::Block* block{};
    tdf::Block* parent{};
};

static_assert(std::is_trivially_copyable_v<Box>);
static_assert(std::is_trivially_destructible_v<Box>);

/// What a trampoline pushes after the prepare call has returned.
enum class EmitKind : int {
    error = -1,
    none = 0,
    nil,
    count,
    text,
    names,
    side,
    section,
    table,
    method_each,
    method_canbuild,
    method_set,
    method_add,
    method_remove,
    method_keys,
    method_sections,
    method_add_section,
    method_remove_section,
    method_table_sections
};

struct Emit {
    EmitKind kind{EmitKind::none};
    uint32_t slot{};
    uint32_t count{};
    std::size_t doc{k_no_doc};
    tdf::Block* block{};
    tdf::Block* parent{};
};

static_assert(std::is_trivially_copyable_v<Emit>);
static_assert(std::is_trivially_destructible_v<Emit>);

using SideScratch = Collections::SideScratch;

enum class TableKind : uint8_t { open, sidedata, refused };

char fold_ascii(char byte) {
    if (byte >= 'A' && byte <= 'Z')
        return static_cast<char>(byte - 'A' + 'a');
    return byte;
}

bool same_text(std::string_view earlier, std::string_view later) {
    if (earlier.size() != later.size())
        return false;
    for (std::size_t offset = 0; offset < earlier.size(); ++offset) {
        if (fold_ascii(earlier[offset]) != fold_ascii(later[offset]))
            return false;
    }
    return true;
}

bool same_path(std::string_view earlier, std::string_view later) {
    if (earlier.size() != later.size())
        return false;
    for (std::size_t offset = 0; offset < earlier.size(); ++offset) {
        const char first_byte = earlier[offset] == '\\' ? '/' : fold_ascii(earlier[offset]);
        const char second_byte = later[offset] == '\\' ? '/' : fold_ascii(later[offset]);
        if (first_byte != second_byte)
            return false;
    }
    return true;
}

std::string slash_copy(std::string path) {
    for (char& byte : path) {
        if (byte == '\\')
            byte = '/';
    }
    return path;
}

std::string_view trim_ends(std::string_view text) {
    std::size_t begin = 0;
    while (begin < text.size()) {
        const char byte = text[begin];
        if (byte != ' ' && byte != '\t' && byte != '\n' && byte != '\r')
            break;
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin) {
        const char byte = text[end - 1];
        if (byte != ' ' && byte != '\t' && byte != '\n' && byte != '\r')
            break;
        --end;
    }
    return text.substr(begin, end - begin);
}

bool cutting_character(std::string_view text, std::string& reason) {
    for (const char byte : text) {
        if (byte == ';' || byte == '{' || byte == '}' || byte == '[' || byte == ']' ||
            byte == '\n' || byte == '\r' || byte == '\0') {
            reason = "a value cannot hold ';', braces, brackets or a line break";
            return true;
        }
    }
    for (std::size_t offset = 0; offset + 1 < text.size(); ++offset) {
        if (text[offset] == '/' && (text[offset + 1] == '/' || text[offset + 1] == '*')) {
            reason = "a value cannot hold a comment mark";
            return true;
        }
    }
    return false;
}

bool has_slash(std::string_view text) {
    return text.find('/') != std::string_view::npos || text.find('\\') != std::string_view::npos;
}

SideScratch& scratch_of(Collections& collections) noexcept {
    return SideCollectionsAccess::scratch(collections);
}

void hold(Collections& collections, std::string message) {
    SideCollectionsAccess::hold_error(collections, std::move(message));
}

void note_visit(SideScratch& scratch, uint64_t count) {
    if (count == 0)
        return;
    if (scratch.visits > std::numeric_limits<uint64_t>::max() - count)
        scratch.visits = std::numeric_limits<uint64_t>::max();
    else
        scratch.visits += count;
}

void note_text(SideScratch& scratch, std::size_t bytes) {
    const uint64_t units = host::units_for_bytes(bytes);
    if (units == 0)
        return;
    if (scratch.text_units > std::numeric_limits<uint64_t>::max() - units)
        scratch.text_units = std::numeric_limits<uint64_t>::max();
    else
        scratch.text_units += units;
}

void note_field(SideScratch& scratch) {
    if (scratch.fields != std::numeric_limits<uint64_t>::max())
        ++scratch.fields;
}

/// Charges one key-collecting walk. May raise. No C++ destructor is alive.
void charge_scan(lua_State* state) {
    host::charge_work(state, host::scan_call_charge);
}

/// `count` added `unit` times, stopping at the top of uint64.
uint64_t scaled_units(uint64_t count, uint64_t unit) {
    if (count == 0 || unit == 0)
        return 0;
    uint64_t scaled = 0;
    for (uint64_t step = 0; step < unit; ++step) {
        if (scaled > std::numeric_limits<uint64_t>::max() - count)
            return std::numeric_limits<uint64_t>::max();
        scaled += count;
    }
    return scaled;
}

/// Charges `visits` elements in one step. May raise. No C++ destructor is alive.
void charge_visits(lua_State* state, uint64_t visits) {
    host::charge_work(state, scaled_units(visits, host::element_charge));
}

/// Raises when the allowance is spent. Call it with no local object still to destroy.
void settle(lua_State* state, Collections& collections) {
    SideScratch& scratch = scratch_of(collections);
    const bool scan = scratch.scan;
    const uint64_t fields = scratch.fields;
    const uint64_t visits = scratch.visits;
    const uint64_t text_units = scratch.text_units;
    const uint64_t comparisons = scratch.comparisons;
    scratch.scan = false;
    scratch.fields = 0;
    scratch.visits = 0;
    scratch.text_units = 0;
    scratch.comparisons = 0;
    if (scan)
        charge_scan(state);
    if (fields != 0)
        charge_visits(state, scaled_units(fields, k_field_charge));
    if (visits != 0)
        charge_visits(state, visits);
    if (text_units != 0)
        host::charge_work(state, text_units);
    if (comparisons != 0)
        host::charge_work(state, scaled_units(comparisons, host::comparison_unit));
}

Emit error_emit() {
    Emit emit;
    emit.kind = EmitKind::error;
    return emit;
}

bool layer_ok(Collections& collections, bool allowed, const char* layer, std::string& reason) {
    if (!SideCollectionsAccess::active(collections)) {
        reason = "no mutator has begun";
        return false;
    }
    if (allowed)
        return true;
    reason = "this mutator does not declare the ";
    reason.append(layer);
    reason.append(" layer");
    return false;
}

bool commit_field(
    Collections& collections,
    std::string path,
    std::string record,
    std::string key,
    std::optional<std::string> old_text,
    std::optional<std::string> new_text,
    std::string& reason
) {
    rules::FieldWrite write;
    write.mutators.push_back(SideCollectionsAccess::mutator(collections));
    write.path = std::move(path);
    write.record = std::move(record);
    write.key = std::move(key);
    write.old_text = std::move(old_text);
    write.new_text = std::move(new_text);
    return SideCollectionsAccess::accept_write(collections, std::move(write), reason);
}

bool commit_note(
    Collections& collections,
    std::string path,
    std::string record,
    std::string message,
    std::string& reason
) {
    HookNote note;
    note.kind = HookNoteKind::check_warned;
    note.mutators.push_back(SideCollectionsAccess::mutator(collections));
    note.path = std::move(path);
    note.record = std::move(record);
    note.message = std::move(message);
    return SideCollectionsAccess::accept_note(collections, std::move(note), reason);
}

std::string directory_name(const defs::DataLayout& layout, defs::DataDirectory which) {
    return layout.directories[static_cast<std::size_t>(which)];
}

/// The side file load_side_data would open: the variant when it exists.
std::string side_file_path(const Box& box) {
    SideScratch& scratch = scratch_of(*box.collections);
    note_visit(scratch, 1);
    const defs::Files boundary = defs::asset_store_files(box.store);
    std::array<char, defs::path_capacity> buffer{};
    const char* word =
        box.variant != nullptr && !box.variant->empty() ? box.variant->c_str() : nullptr;
    defs::build_variant_path(
        &boundary,
        buffer.data(),
        buffer.size(),
        directory_name(*box.layout, defs::DataDirectory::gamedata).c_str(),
        "sidedata",
        "tdf",
        word
    );
    return slash_copy(buffer.data());
}

/// The file the build-list loader opens. It never asks for a variant.
std::string list_file_path(const Box& box) {
    return directory_name(*box.layout, defs::DataDirectory::gamedata) + "/sidedata.tdf";
}

std::size_t document_index(
    Collections& collections,
    const oa::AssetStore& store,
    const std::string& path,
    std::string& error,
    bool required
) {
    SideScratch& scratch = scratch_of(collections);
    rules::DocumentSet& documents = SideCollectionsAccess::documents(collections);
    for (std::size_t slot = 0; slot < documents.size(); ++slot) {
        note_visit(scratch, 1);
        rules::LoadedDocument& loaded = documents.at(slot);
        note_text(scratch, loaded.path.size());
        note_text(scratch, path.size());
        if (!same_path(loaded.path, path))
            continue;
        if (loaded.removed) {
            error = path + " was removed";
            return k_no_doc;
        }
        if (loaded.document.root() == nullptr) {
            error = path + ": the table could not be read";
            return k_no_doc;
        }
        return slot;
    }
    std::size_t nbytes = 0;
    std::string text;
    try {
        const oa::AssetData data = store.read(path);
        nbytes = data.bytes.size();
        text.assign(data.bytes.begin(), data.bytes.end());
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception&) {
        if (required)
            error = path + " is missing";
        return k_no_doc;
    }
    note_text(scratch, nbytes);
    std::string add_error;
    if (!documents.add(path, text, &add_error)) {
        for (std::size_t slot = 0; slot < documents.size(); ++slot) {
            note_visit(scratch, 1);
            if (same_path(documents.at(slot).path, path))
                return slot;
        }
        error = path;
        error.append(": ");
        error.append(add_error);
        return k_no_doc;
    }
    for (std::size_t slot = 0; slot < documents.size(); ++slot) {
        note_visit(scratch, 1);
        if (same_path(documents.at(slot).path, path))
            return slot;
    }
    error = path + ": the table could not be read";
    return k_no_doc;
}

/// Finds the side document once per mutator. Later reads reuse the index.
bool ensure_side(const Box& box, bool required, std::string& error) {
    SideScratch& scratch = scratch_of(*box.collections);
    if (scratch.side_ready) {
        if (!scratch.side_error.empty()) {
            error = scratch.side_error;
            return false;
        }
        if (!scratch.side_found) {
            if (required)
                error = scratch.side_path + " is missing";
            return false;
        }
        return true;
    }
    const std::string path = side_file_path(box);
    std::string local_error;
    const std::size_t doc =
        path.empty() ? k_no_doc
                     : document_index(*box.collections, *box.store, path, local_error, required);
    scratch.side_path = path.empty() ? std::string("gamedata/sidedata.tdf") : path;
    scratch.side_error = local_error;
    scratch.side_found = doc != k_no_doc && local_error.empty();
    scratch.side_doc = doc;
    scratch.side_ready = true;
    if (scratch.side_found) {
        rules::LoadedDocument& loaded = SideCollectionsAccess::documents(*box.collections).at(doc);
        scratch.side_path = loaded.path;
    }
    if (!local_error.empty()) {
        error = local_error;
        return false;
    }
    if (!scratch.side_found) {
        if (required)
            error = scratch.side_path + " is missing";
        return false;
    }
    return true;
}

/// Finds the build-list document once per mutator.
bool ensure_list(const Box& box, std::string& error) {
    SideScratch& scratch = scratch_of(*box.collections);
    if (scratch.list_ready) {
        if (!scratch.list_error.empty()) {
            error = scratch.list_error;
            return false;
        }
        if (!scratch.list_found) {
            error = scratch.list_path + " is missing";
            return false;
        }
        return true;
    }
    const std::string path = list_file_path(box);
    std::string local_error;
    const std::size_t doc = document_index(*box.collections, *box.store, path, local_error, true);
    scratch.list_path = path;
    scratch.list_error = local_error;
    scratch.list_found = doc != k_no_doc && local_error.empty();
    scratch.list_doc = doc;
    scratch.list_ready = true;
    if (scratch.list_found) {
        rules::LoadedDocument& loaded = SideCollectionsAccess::documents(*box.collections).at(doc);
        scratch.list_path = loaded.path;
    }
    if (!scratch.list_found) {
        error = local_error.empty() ? scratch.list_path + " is missing" : local_error;
        return false;
    }
    return true;
}

tdf::Block* side_block(Collections& collections, rules::LoadedDocument& loaded, uint32_t slot) {
    tdf::Block* root = tdf::editable_root(loaded.document.get());
    if (root == nullptr)
        return nullptr;
    note_visit(scratch_of(collections), tdf::child_count(root));
    const std::string name = std::string("SIDE") + std::to_string(slot);
    note_text(scratch_of(collections), name.size());
    return tdf::editable_child(root, name);
}

uint32_t count_loaded_sides(Collections& collections, rules::LoadedDocument& loaded) {
    const uint32_t limit = collections.side_limit();
    uint32_t count = 0;
    while (count < limit && side_block(collections, loaded, count) != nullptr)
        ++count;
    if (count < limit)
        note_visit(scratch_of(collections), 1);
    return count;
}

uint32_t count_sides(const Box& box, std::string& error) {
    if (!ensure_side(box, false, error))
        return 0;
    SideScratch& scratch = scratch_of(*box.collections);
    rules::LoadedDocument& loaded =
        SideCollectionsAccess::documents(*box.collections).at(scratch.side_doc);
    return count_loaded_sides(*box.collections, loaded);
}

std::optional<uint32_t> side_named(const Box& box, std::string_view sought, std::string& error) {
    if (!ensure_side(box, false, error))
        return std::nullopt;
    SideScratch& scratch = scratch_of(*box.collections);
    rules::LoadedDocument& loaded =
        SideCollectionsAccess::documents(*box.collections).at(scratch.side_doc);
    const uint32_t count = count_loaded_sides(*box.collections, loaded);
    for (uint32_t index = 0; index < count; ++index) {
        tdf::Block* block = side_block(*box.collections, loaded, index);
        note_field(scratch);
        const char* name = block != nullptr ? tdf::find_value(block, "name") : nullptr;
        if (name != nullptr) {
            note_text(scratch, std::strlen(name));
            note_text(scratch, sought.size());
        }
        if (name != nullptr && same_text(name, sought))
            return index;
    }
    return std::nullopt;
}

bool load_side(
    const Box& box,
    uint32_t slot,
    std::size_t& doc,
    tdf::Block*& block,
    std::string& path,
    std::string& error
) {
    if (!ensure_side(box, true, error))
        return false;
    SideScratch& scratch = scratch_of(*box.collections);
    doc = scratch.side_doc;
    rules::LoadedDocument& loaded = SideCollectionsAccess::documents(*box.collections).at(doc);
    path = loaded.path;
    const uint32_t count = count_loaded_sides(*box.collections, loaded);
    if (slot >= count) {
        error = "a mutator cannot add a side";
        return false;
    }
    block = side_block(*box.collections, loaded, slot);
    if (block == nullptr) {
        error = "a mutator cannot add a side";
        return false;
    }
    return true;
}

bool load_list_document(const Box& box, std::size_t& doc, std::string& path, std::string& error) {
    if (!ensure_list(box, error))
        return false;
    SideScratch& scratch = scratch_of(*box.collections);
    doc = scratch.list_doc;
    path = scratch.list_path;
    return true;
}

/// True when the store provides the path. The file's bytes are not read.
bool resource_present(const oa::AssetStore& store, std::string_view path) {
    try {
        return store.provider(path).kind != oa::ProviderKind::none;
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::runtime_error&) {
        return false;
    }
}

bool art_exists(
    const Box& box, const char* directory, const std::string& name, const char* extension
) {
    SideScratch& scratch = scratch_of(*box.collections);
    const std::string plain = std::string(directory) + "/" + name + "." + extension;
    note_visit(scratch, 1);
    note_text(scratch, plain.size());
    if (box.variant != nullptr && !box.variant->empty()) {
        const std::string varied =
            std::string(directory) + "-" + *box.variant + "/" + name + "." + extension;
        note_visit(scratch, 1);
        note_text(scratch, varied.size());
        if (resource_present(*box.store, varied))
            return true;
    }
    return resource_present(*box.store, plain);
}

/// Copies one field. The stored key spelling is kept for the journal.
void take_field(SideScratch& scratch, const tdf::Block* block, std::string_view key) {
    scratch.present = false;
    scratch.key.assign(key.begin(), key.end());
    scratch.text.clear();
    note_field(scratch);
    note_text(scratch, key.size());
    if (block == nullptr)
        return;
    const uint32_t count = tdf::property_count(block);
    for (uint32_t index = 0; index < count; ++index) {
        note_visit(scratch, 1);
        const char* stored = tdf::property_key_at(block, static_cast<int32_t>(index));
        if (stored == nullptr)
            continue;
        note_text(scratch, std::strlen(stored));
        if (!same_text(stored, key))
            continue;
        const char* value = tdf::property_value_at(block, static_cast<int32_t>(index));
        scratch.present = true;
        scratch.key.assign(stored);
        if (value != nullptr) {
            note_text(scratch, std::strlen(value));
            scratch.text.assign(value);
        }
        return;
    }
}

bool child_named(SideScratch& scratch, const tdf::Block* block, std::string_view key) {
    if (block == nullptr)
        return false;
    const uint32_t count = tdf::child_count(block);
    note_visit(scratch, count);
    note_text(scratch, key.size());
    for (uint32_t index = 0; index < count; ++index) {
        const tdf::Block* child = tdf::child_at(block, index);
        if (child == nullptr || child->name == nullptr)
            continue;
        note_text(scratch, std::strlen(child->name));
        if (same_text(child->name, key))
            return true;
    }
    return false;
}

bool put_text(
    tdf::Document* document,
    tdf::Block* block,
    const std::string& path,
    const std::string& record,
    const std::string& key,
    const std::string& value,
    Collections& collections,
    std::string& reason
) {
    SideScratch& scratch = scratch_of(collections);
    take_field(scratch, block, key);
    if (scratch.present && scratch.text == value)
        return true;
    const std::string stored_key = scratch.present ? scratch.key : key;
    const bool had = scratch.present;
    const std::string old_text = scratch.text;
    const tdf::EditStatus status = tdf::set_value(document, block, stored_key, value);
    if (status != tdf::EditStatus::ok) {
        reason = tdf::edit_status_message(status);
        return false;
    }
    std::optional<std::string> previous;
    if (had)
        previous = old_text;
    return commit_field(collections, path, record, stored_key, std::move(previous), value, reason);
}

bool prefix_pages(
    const Box& box, std::string_view present, std::string_view updated, std::string& reason
) {
    if (present.empty() || same_text(present, updated))
        return true;
    const char* pages[] = {"GEN.GUI", "MAIN2.GUI", "DL.GUI"};
    const std::string folder = directory_name(*box.layout, defs::DataDirectory::guis);
    for (const char* page : pages) {
        const std::string had = folder + "/" + std::string(present) + page;
        const std::string next = folder + "/" + std::string(updated) + page;
        note_visit(scratch_of(*box.collections), 2);
        note_text(scratch_of(*box.collections), had.size());
        note_text(scratch_of(*box.collections), next.size());
        if (resource_present(*box.store, had) && !resource_present(*box.store, next)) {
            reason = "the prefix has no ";
            reason.append(next);
            reason.append(" page");
            return false;
        }
    }
    return true;
}

bool name_free(const Box& box, uint32_t self_slot, std::string_view updated, std::string& reason) {
    if (!ensure_side(box, true, reason))
        return false;
    SideScratch& scratch = scratch_of(*box.collections);
    rules::LoadedDocument& loaded =
        SideCollectionsAccess::documents(*box.collections).at(scratch.side_doc);
    const uint32_t count = count_loaded_sides(*box.collections, loaded);
    for (uint32_t index = 0; index < count; ++index) {
        if (index == self_slot)
            continue;
        tdf::Block* block = side_block(*box.collections, loaded, index);
        const char* name = block != nullptr ? tdf::find_value(block, "name") : nullptr;
        if (name != nullptr) {
            note_text(scratch, std::strlen(name));
            note_visit(scratch, 1);
        }
        if (name != nullptr && same_text(name, updated)) {
            reason = "a side already has that name";
            return false;
        }
    }
    reason.clear();
    return true;
}

/// Checks a side value. A commander is length-checked here and resolved by the caller.
bool checked_text(
    const Box& box,
    uint32_t self_slot,
    tdf::Block* block,
    std::string_view key,
    std::string_view raw,
    std::string& stored,
    bool& needs_unit,
    std::string& reason
) {
    needs_unit = false;
    if (cutting_character(raw, reason))
        return false;
    const std::string_view trimmed = trim_ends(raw);
    stored.assign(trimmed.begin(), trimmed.end());
    if (same_text(key, "name")) {
        if (stored.empty() || stored.size() > k_name_limit) {
            reason = "a side name is 1 to 29 characters";
            return false;
        }
        return name_free(box, self_slot, stored, reason);
    }
    if (same_text(key, "nameprefix")) {
        if (has_slash(raw)) {
            reason = "a prefix cannot hold a slash";
            return false;
        }
        if (stored.empty() || stored.size() > k_prefix_limit) {
            reason = "a prefix is 1 to 3 letters or digits";
            return false;
        }
        for (const char byte : stored) {
            const bool digit = byte >= '0' && byte <= '9';
            const bool letter = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z');
            if (!digit && !letter) {
                reason = "a prefix is 1 to 3 letters or digits";
                return false;
            }
        }
        take_field(scratch_of(*box.collections), block, "nameprefix");
        const std::string had = scratch_of(*box.collections).present
                                    ? scratch_of(*box.collections).text
                                    : std::string();
        return prefix_pages(box, had, stored, reason);
    }
    if (same_text(key, "commander")) {
        if (stored.empty() || stored.size() > k_commander_limit) {
            reason = "a commander name is 1 to 31 characters";
            return false;
        }
        needs_unit = true;
        return true;
    }
    if (same_text(key, "intgaf")) {
        if (has_slash(raw)) {
            reason = "an intgaf name cannot hold a slash";
            return false;
        }
        if (stored.empty() || stored.size() > k_intgaf_limit) {
            reason = "an intgaf name is 1 to 29 characters";
            return false;
        }
        if (!art_exists(box, "anims", stored, "GAF")) {
            reason = "an intgaf file is missing: anims/";
            reason.append(stored);
            reason.append(".GAF");
            return false;
        }
        return true;
    }
    if (same_text(key, "font")) {
        if (has_slash(raw)) {
            reason = "a font name cannot hold a slash";
            return false;
        }
        if (stored.empty() || stored.size() > k_font_limit) {
            reason = "a font name is 1 to 255 characters";
            return false;
        }
        if (!art_exists(box, "fonts", stored, "FNT")) {
            reason = "a font file is missing: fonts/";
            reason.append(stored);
            reason.append(".FNT");
            return false;
        }
        return true;
    }
    if (stored.size() > k_text_limit) {
        reason = "a value is longer than the field can hold";
        return false;
    }
    return true;
}

bool parse_canbuild_key(std::string_view key, uint32_t& number) {
    constexpr std::string_view prefix = "canbuild";
    if (key.size() <= prefix.size() || !same_text(key.substr(0, prefix.size()), prefix))
        return false;
    uint32_t value = 0;
    for (std::size_t offset = prefix.size(); offset < key.size(); ++offset) {
        const char digit = key[offset];
        if (digit < '0' || digit > '9')
            return false;
        const auto piece = static_cast<uint32_t>(digit - '0');
        constexpr uint32_t k_ten_limit = 429496729u;
        if (value > k_ten_limit || (value == k_ten_limit && piece > 5u))
            return false;
        value = value * 10u + piece;
    }
    number = value;
    return true;
}

void read_list(SideScratch& scratch, const tdf::Block* block, std::vector<std::string>& names) {
    names.clear();
    if (block == nullptr) {
        note_visit(scratch, 1);
        return;
    }
    for (uint32_t number = 1; number <= k_list_cap; ++number) {
        note_visit(scratch, 1);
        const std::string key = std::string("canbuild") + std::to_string(number);
        note_text(scratch, key.size());
        const char* value = tdf::find_value(block, key.c_str());
        if (value == nullptr)
            break;
        note_text(scratch, std::strlen(value));
        names.emplace_back(value);
    }
}

bool list_matches(
    SideScratch& scratch, const tdf::Block* block, const std::vector<std::string>& names
) {
    if (block == nullptr)
        return names.empty();
    std::vector<std::string> current;
    read_list(scratch, block, current);
    if (current.size() != names.size())
        return false;
    for (std::size_t slot = 0; slot < names.size(); ++slot) {
        note_visit(scratch, 1);
        note_text(scratch, current[slot].size());
        note_text(scratch, names[slot].size());
        if (current[slot] != names[slot])
            return false;
    }
    const uint32_t count = tdf::property_count(block);
    for (uint32_t key_index = 0; key_index < count; ++key_index) {
        note_visit(scratch, 1);
        const char* key = tdf::property_key_at(block, static_cast<int32_t>(key_index));
        if (key == nullptr)
            continue;
        note_text(scratch, std::strlen(key));
        uint32_t number = 0;
        if (!parse_canbuild_key(key, number))
            continue;
        const std::string canonical = std::string("canbuild") + std::to_string(number);
        if (number < 1 || static_cast<std::size_t>(number) > names.size() ||
            !same_text(key, canonical))
            return false;
    }
    return true;
}

bool write_list(
    tdf::Document* document,
    tdf::Block* unit_block,
    const std::vector<std::string>& names,
    const std::string& path,
    Collections& collections,
    std::string& reason
) {
    SideScratch& scratch = scratch_of(collections);
    if (list_matches(scratch, unit_block, names))
        return true;
    const std::string record =
        std::string("CANBUILD.") + (unit_block->name != nullptr ? unit_block->name : "");
    for (std::size_t slot = 0; slot < names.size(); ++slot) {
        note_visit(scratch, 1);
        const std::string key = std::string("canbuild") + std::to_string(slot + 1);
        if (!put_text(document, unit_block, path, record, key, names[slot], collections, reason))
            return false;
    }
    std::vector<std::string> drop;
    const uint32_t count = tdf::property_count(unit_block);
    for (uint32_t key_index = 0; key_index < count; ++key_index) {
        note_visit(scratch, 1);
        const char* key = tdf::property_key_at(unit_block, static_cast<int32_t>(key_index));
        if (key == nullptr)
            continue;
        note_text(scratch, std::strlen(key));
        uint32_t number = 0;
        if (!parse_canbuild_key(key, number))
            continue;
        const std::string canonical = std::string("canbuild") + std::to_string(number);
        if (number < 1 || static_cast<std::size_t>(number) > names.size() ||
            !same_text(key, canonical))
            drop.emplace_back(key);
    }
    for (const std::string& key : drop) {
        take_field(scratch, unit_block, key);
        if (!scratch.present)
            continue;
        const std::string dropped_key = scratch.key;
        const std::string dropped_text = scratch.text;
        const tdf::EditStatus status = tdf::remove_key(document, unit_block, dropped_key);
        if (status != tdf::EditStatus::ok) {
            reason = tdf::edit_status_message(status);
            return false;
        }
        if (!commit_field(
                collections, path, record, dropped_key, dropped_text, std::nullopt, reason
            ))
            return false;
    }
    if (names.size() > k_list_kept) {
        if (!commit_note(collections, path, record, k_list_note, reason))
            return false;
    }
    return true;
}

tdf::Block* ensure_canbuild(
    Collections& collections,
    tdf::Document* document,
    tdf::Block* root,
    bool create,
    std::string& reason
) {
    if (root != nullptr)
        note_visit(scratch_of(collections), tdf::child_count(root));
    tdf::Block* section = root != nullptr ? tdf::editable_child(root, "CANBUILD") : nullptr;
    if (section != nullptr || !create)
        return section;
    tdf::EditStatus status = tdf::EditStatus::ok;
    section = tdf::add_child(document, root, "CANBUILD", &status);
    if (section == nullptr)
        reason = tdf::edit_status_message(status);
    return section;
}

tdf::Block* ensure_builder(
    Collections& collections,
    tdf::Document* document,
    tdf::Block* section,
    const std::string& name,
    bool create,
    std::string& reason
) {
    if (section != nullptr)
        note_visit(scratch_of(collections), tdf::child_count(section));
    note_text(scratch_of(collections), name.size());
    tdf::Block* block = section != nullptr ? tdf::editable_child(section, name) : nullptr;
    if (block != nullptr || !create)
        return block;
    tdf::EditStatus status = tdf::EditStatus::ok;
    block = tdf::add_child(document, section, name, &status);
    if (block == nullptr)
        reason = tdf::edit_status_message(status);
    return block;
}

/// Resolves the builder and every listed unit.
///
/// Lookups charge, so this holds its own messages and keeps no local string
/// across a lookup. The caller must not have a local string alive either.
bool resolve_names(lua_State* state, Collections& collections, bool need_builder) {
    SideScratch& scratch = scratch_of(collections);
    const int builder = SideCollectionsAccess::lookup(collections, state, scratch.builder);
    if (builder < 0)
        return false;
    if (builder == 0) {
        std::string reason = "there is no unit ";
        reason.append(scratch.builder);
        hold(collections, std::move(reason));
        return false;
    }
    const bool builds = scratch.builder_unit;
    scratch.builder = scratch.text;
    if (need_builder && !builds) {
        hold(collections, "the unit is not a builder");
        return false;
    }
    scratch.names.clear();
    for (std::size_t index = 0; index < scratch.raw_names.size(); ++index) {
        const int listed =
            SideCollectionsAccess::lookup(collections, state, scratch.raw_names[index]);
        if (listed < 0)
            return false;
        if (listed == 0) {
            std::string reason = "there is no unit ";
            reason.append(scratch.raw_names[index]);
            hold(collections, std::move(reason));
            return false;
        }
        {
            std::string reason;
            if (cutting_character(scratch.text, reason)) {
                hold(collections, std::move(reason));
                return false;
            }
        }
        scratch.names.push_back(scratch.text);
    }
    return true;
}

bool contains_block(const tdf::Block* node, const tdf::Block* sought, uint64_t& visits) {
    if (node == nullptr || sought == nullptr)
        return false;
    if (visits != std::numeric_limits<uint64_t>::max())
        ++visits;
    if (node == sought)
        return true;
    for (uint32_t index = 0; index < node->child_count; ++index) {
        if (contains_block(node->children[index], sought, visits))
            return true;
    }
    return false;
}

tdf::Block* root_of(const Box& box) {
    rules::LoadedDocument& loaded =
        SideCollectionsAccess::documents(*box.collections).at(box.doc_index);
    return tdf::editable_root(loaded.document.get());
}

bool section_live(const Box& box, std::string& reason) {
    const rules::DocumentSet& documents = SideCollectionsAccess::documents(*box.collections);
    if (box.doc_index >= documents.size() || box.block == nullptr) {
        reason = "that section was removed";
        return false;
    }
    const rules::LoadedDocument& loaded = documents.at(box.doc_index);
    uint64_t walked = 0;
    const bool inside =
        !loaded.removed && contains_block(loaded.document.root(), box.block, walked);
    note_visit(scratch_of(*box.collections), walked);
    if (!inside) {
        reason = "that section was removed";
        return false;
    }
    return true;
}

std::string parent_record(const Box& box, const tdf::Block* parent) {
    const tdf::Block* root = root_of(box);
    if (parent == nullptr || parent == root) {
        const std::string& path =
            SideCollectionsAccess::documents(*box.collections).at(box.doc_index).path;
        const auto slash = path.rfind('/');
        std::string_view file = slash == std::string::npos
                                    ? std::string_view(path)
                                    : std::string_view(path).substr(slash + 1);
        const auto dot = file.rfind('.');
        if (dot != std::string_view::npos)
            file = file.substr(0, dot);
        return std::string(file);
    }
    return parent->name != nullptr ? parent->name : std::string();
}

std::string lua_string(lua_State* state, int index, bool& ok) {
    ok = lua_type(state, index) == LUA_TSTRING;
    if (!ok)
        return {};
    std::size_t length = 0;
    const char* text = lua_tolstring(state, index, &length);
    if (text == nullptr) {
        ok = false;
        return {};
    }
    return std::string(text, length);
}

bool read_name_array(
    lua_State* state,
    Collections& collections,
    int index,
    std::vector<std::string>& names,
    std::string& reason
) {
    if (lua_type(state, index) != LUA_TTABLE) {
        reason = "a build list is an array of unit names";
        return false;
    }
    const lua_Unsigned length = lua_rawlen(state, index);
    if (length > k_list_cap) {
        reason = "a build list keeps at most 1024 names";
        return false;
    }
    names.clear();
    names.reserve(static_cast<std::size_t>(length));
    for (lua_Unsigned slot = 1; slot <= length; ++slot) {
        note_visit(scratch_of(collections), 1);
        lua_rawgeti(state, index, static_cast<lua_Integer>(slot));
        bool ok = false;
        std::string name = lua_string(state, -1, ok);
        lua_pop(state, 1);
        if (!ok) {
            reason = "a build list name is text";
            return false;
        }
        note_text(scratch_of(collections), name.size());
        names.push_back(std::move(name));
    }
    return true;
}

/// A `.tdf` directly in the layout's gamedata folder. Variant folders are refused.
TableKind
classify_table(const defs::DataLayout& layout, std::string_view requested, std::string& canonical) {
    std::string text(requested);
    for (char& byte : text) {
        if (byte == '\\')
            byte = '/';
    }
    std::size_t begin = 0;
    while (begin < text.size() && text[begin] == '/')
        ++begin;
    std::size_t end = text.size();
    while (end > begin && text[end - 1] == '/')
        --end;
    if (begin >= end)
        return TableKind::refused;
    const std::string folder = directory_name(layout, defs::DataDirectory::gamedata);
    std::string_view first;
    std::string_view second;
    int parts = 0;
    std::size_t cursor = begin;
    while (cursor < end) {
        const std::size_t slash = text.find('/', cursor);
        const std::size_t stop = slash == std::string::npos || slash > end ? end : slash;
        const std::string_view part(text.data() + cursor, stop - cursor);
        if (part.empty() || part == "." || part == "..")
            return TableKind::refused;
        if (parts == 0)
            first = part;
        else if (parts == 1)
            second = part;
        ++parts;
        if (parts > 2)
            return TableKind::refused;
        if (stop >= end)
            break;
        cursor = stop + 1;
    }
    std::string file;
    if (parts == 1)
        file.assign(first.begin(), first.end());
    else if (!same_text(first, folder))
        return TableKind::refused;
    else
        file.assign(second.begin(), second.end());
    constexpr std::string_view suffix = ".tdf";
    std::string stem = file;
    if (file.size() >= suffix.size() &&
        same_text(std::string_view(file).substr(file.size() - suffix.size()), suffix))
        stem.resize(stem.size() - suffix.size());
    else
        file.append(suffix);
    if (same_text(stem, "sidedata"))
        return TableKind::sidedata;
    canonical = folder + "/" + file;
    return TableKind::open;
}

void push_box(lua_State* state, const Box& source, const char* meta) {
    auto* created = static_cast<Box*>(lua_newuserdatauv(state, sizeof(Box), 0));
    *created = source;
    luaL_setmetatable(state, meta);
}

Box* box_at(lua_State* state, int index, BoxKind kind) {
    const char* meta = k_sides_meta;
    if (kind == BoxKind::side)
        meta = k_side_meta;
    else if (kind == BoxKind::tables)
        meta = k_tables_meta;
    else if (kind == BoxKind::table)
        meta = k_table_meta;
    else if (kind == BoxKind::section)
        meta = k_section_meta;
    auto* box = static_cast<Box*>(luaL_testudata(state, index, meta));
    if (box == nullptr || box->kind != kind)
        return nullptr;
    return box;
}

void push_side(lua_State* state, const Box& source, uint32_t slot) {
    Box copy = source;
    copy.kind = BoxKind::side;
    copy.side_slot = slot;
    copy.block = nullptr;
    copy.parent = nullptr;
    push_box(state, copy, k_side_meta);
}

void push_section(
    lua_State* state, const Box& source, std::size_t doc, tdf::Block* parent, tdf::Block* block
) {
    Box copy = source;
    copy.kind = BoxKind::section;
    copy.doc_index = doc;
    copy.parent = parent;
    copy.block = block;
    push_box(state, copy, k_section_meta);
}

int publish(lua_State* state, const Box& box, const Emit& emit) {
    SideScratch& scratch = scratch_of(*box.collections);
    switch (emit.kind) {
    case EmitKind::error:
        return SideCollectionsAccess::raise(*box.collections, state);
    case EmitKind::none:
        return 0;
    case EmitKind::nil:
        lua_pushnil(state);
        return 1;
    case EmitKind::count:
        lua_pushinteger(state, static_cast<lua_Integer>(emit.count));
        return 1;
    case EmitKind::text:
        lua_pushlstring(state, scratch.text.data(), scratch.text.size());
        return 1;
    case EmitKind::names:
        if (scratch.names.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            hold(*box.collections, "too many names");
            return SideCollectionsAccess::raise(*box.collections, state);
        }
        lua_createtable(state, static_cast<int>(scratch.names.size()), 0);
        for (std::size_t slot = 0; slot < scratch.names.size(); ++slot) {
            lua_pushlstring(state, scratch.names[slot].data(), scratch.names[slot].size());
            lua_rawseti(state, -2, static_cast<lua_Integer>(slot + 1));
        }
        return 1;
    case EmitKind::side:
        push_side(state, box, emit.slot);
        return 1;
    case EmitKind::section:
        push_section(state, box, emit.doc, emit.parent, emit.block);
        return 1;
    case EmitKind::table: {
        Box copy = box;
        copy.kind = BoxKind::table;
        copy.doc_index = emit.doc;
        copy.block = nullptr;
        copy.parent = nullptr;
        push_box(state, copy, k_table_meta);
        return 1;
    }
    case EmitKind::method_each:
    case EmitKind::method_canbuild:
    case EmitKind::method_set:
    case EmitKind::method_add:
    case EmitKind::method_remove:
    case EmitKind::method_keys:
    case EmitKind::method_sections:
    case EmitKind::method_add_section:
    case EmitKind::method_remove_section:
    case EmitKind::method_table_sections:
        return 0;
    }
    return 0;
}

Emit prepare_side_count(lua_State* state, const Box& box, bool scan) {
    bool failed = false;
    uint32_t count = 0;
    {
        std::string error;
        count = count_sides(box, error);
        if (!error.empty()) {
            hold(*box.collections, std::move(error));
            failed = true;
        }
    }
    if (scan)
        scratch_of(*box.collections).scan = true;
    settle(state, *box.collections);
    if (failed)
        return error_emit();
    Emit emit;
    emit.kind = EmitKind::count;
    emit.count = count;
    return emit;
}

Emit prepare_canbuild(lua_State* state, const Box& box) {
    bool failed = false;
    {
        bool ok = false;
        const std::string builder = lua_string(state, 2, ok);
        if (!ok) {
            hold(*box.collections, "a build list name is text");
            failed = true;
        } else {
            scratch_of(*box.collections).builder = builder;
        }
    }
    if (failed) {
        settle(state, *box.collections);
        return error_emit();
    }
    scratch_of(*box.collections).raw_names.clear();
    if (!resolve_names(state, *box.collections, false)) {
        settle(state, *box.collections);
        return error_emit();
    }
    {
        std::string error;
        std::size_t doc = k_no_doc;
        std::string path;
        if (!load_list_document(box, doc, path, error)) {
            hold(*box.collections, std::move(error));
            failed = true;
        } else {
            rules::LoadedDocument& loaded =
                SideCollectionsAccess::documents(*box.collections).at(doc);
            tdf::Block* root = tdf::editable_root(loaded.document.get());
            tdf::Block* section =
                ensure_canbuild(*box.collections, loaded.document.get(), root, false, error);
            tdf::Block* listed = section != nullptr ? ensure_builder(
                                                          *box.collections,
                                                          loaded.document.get(),
                                                          section,
                                                          scratch_of(*box.collections).builder,
                                                          false,
                                                          error
                                                      )
                                                    : nullptr;
            read_list(scratch_of(*box.collections), listed, scratch_of(*box.collections).names);
            scratch_of(*box.collections).scan = true;
        }
    }
    settle(state, *box.collections);
    if (failed)
        return error_emit();
    Emit emit;
    emit.kind = EmitKind::names;
    return emit;
}

Emit prepare_set_canbuild(lua_State* state, const Box& box) {
    bool failed = false;
    {
        std::string reason;
        if (!layer_ok(
                *box.collections,
                SideCollectionsAccess::grant(*box.collections).sides,
                "sides",
                reason
            )) {
            hold(*box.collections, std::move(reason));
            failed = true;
        } else {
            bool ok = false;
            std::string builder = lua_string(state, 2, ok);
            if (!ok) {
                hold(*box.collections, "a build list name is text");
                failed = true;
            } else if (!read_name_array(
                           state,
                           *box.collections,
                           3,
                           scratch_of(*box.collections).raw_names,
                           reason
                       )) {
                hold(*box.collections, std::move(reason));
                failed = true;
            } else {
                scratch_of(*box.collections).builder = std::move(builder);
            }
        }
    }
    if (failed) {
        settle(state, *box.collections);
        return error_emit();
    }
    if (!resolve_names(state, *box.collections, true)) {
        settle(state, *box.collections);
        return error_emit();
    }
    {
        std::string reason;
        std::size_t doc = k_no_doc;
        std::string path;
        if (!load_list_document(box, doc, path, reason)) {
            hold(*box.collections, std::move(reason));
            failed = true;
        } else {
            rules::LoadedDocument& loaded =
                SideCollectionsAccess::documents(*box.collections).at(doc);
            tdf::Document* document = loaded.document.get();
            tdf::Block* root = tdf::editable_root(document);
            if (root == nullptr) {
                hold(*box.collections, path + ": the table could not be read");
                failed = true;
            } else {
                const bool create = !scratch_of(*box.collections).names.empty();
                tdf::Block* section =
                    ensure_canbuild(*box.collections, document, root, create, reason);
                if (create && section == nullptr) {
                    hold(*box.collections, std::move(reason));
                    failed = true;
                } else {
                    tdf::Block* listed = section != nullptr
                                             ? ensure_builder(
                                                   *box.collections,
                                                   document,
                                                   section,
                                                   scratch_of(*box.collections).builder,
                                                   create,
                                                   reason
                                               )
                                             : nullptr;
                    if (create && listed == nullptr) {
                        hold(*box.collections, std::move(reason));
                        failed = true;
                    } else if (
                        listed != nullptr && !write_list(
                                                 document,
                                                 listed,
                                                 scratch_of(*box.collections).names,
                                                 path,
                                                 *box.collections,
                                                 reason
                                             )
                    ) {
                        hold(*box.collections, std::move(reason));
                        failed = true;
                    }
                }
            }
        }
    }
    settle(state, *box.collections);
    return failed ? error_emit() : Emit{};
}

Emit prepare_add_canbuild(lua_State* state, const Box& box) {
    bool failed = false;
    {
        std::string reason;
        if (!layer_ok(
                *box.collections,
                SideCollectionsAccess::grant(*box.collections).sides,
                "sides",
                reason
            )) {
            hold(*box.collections, std::move(reason));
            failed = true;
        } else {
            bool builder_ok = false;
            bool unit_ok = false;
            std::string builder = lua_string(state, 2, builder_ok);
            std::string unit = lua_string(state, 3, unit_ok);
            if (!builder_ok || !unit_ok) {
                hold(*box.collections, "a build list name is text");
                failed = true;
            } else {
                scratch_of(*box.collections).builder = std::move(builder);
                scratch_of(*box.collections).raw_names.clear();
                scratch_of(*box.collections).raw_names.push_back(std::move(unit));
            }
        }
    }
    if (failed) {
        settle(state, *box.collections);
        return error_emit();
    }
    if (!resolve_names(state, *box.collections, true)) {
        settle(state, *box.collections);
        return error_emit();
    }
    {
        std::string reason;
        std::size_t doc = k_no_doc;
        std::string path;
        if (!load_list_document(box, doc, path, reason)) {
            hold(*box.collections, std::move(reason));
            failed = true;
        } else {
            rules::LoadedDocument& loaded =
                SideCollectionsAccess::documents(*box.collections).at(doc);
            tdf::Document* document = loaded.document.get();
            tdf::Block* root = tdf::editable_root(document);
            if (root == nullptr) {
                hold(*box.collections, path + ": the table could not be read");
                failed = true;
            } else {
                tdf::Block* section =
                    ensure_canbuild(*box.collections, document, root, true, reason);
                if (section == nullptr) {
                    hold(*box.collections, std::move(reason));
                    failed = true;
                } else {
                    tdf::Block* existing = ensure_builder(
                        *box.collections,
                        document,
                        section,
                        scratch_of(*box.collections).builder,
                        false,
                        reason
                    );
                    std::vector<std::string> listed;
                    read_list(scratch_of(*box.collections), existing, listed);
                    bool already = false;
                    for (const std::string& entry : listed) {
                        if (same_text(entry, scratch_of(*box.collections).names[0]))
                            already = true;
                    }
                    if (!already) {
                        listed.push_back(scratch_of(*box.collections).names[0]);
                        if (listed.size() > k_list_cap) {
                            hold(*box.collections, "a build list keeps at most 1024 names");
                            failed = true;
                        } else {
                            tdf::Block* block = ensure_builder(
                                *box.collections,
                                document,
                                section,
                                scratch_of(*box.collections).builder,
                                true,
                                reason
                            );
                            if (block == nullptr) {
                                hold(*box.collections, std::move(reason));
                                failed = true;
                            } else if (!write_list(
                                           document, block, listed, path, *box.collections, reason
                                       )) {
                                hold(*box.collections, std::move(reason));
                                failed = true;
                            }
                        }
                    }
                }
            }
        }
    }
    settle(state, *box.collections);
    return failed ? error_emit() : Emit{};
}

Emit prepare_remove_canbuild(lua_State* state, const Box& box) {
    const bool one_list = lua_type(state, 3) != LUA_TNONE && lua_type(state, 3) != LUA_TNIL;
    bool failed = false;
    {
        std::string reason;
        if (!layer_ok(
                *box.collections,
                SideCollectionsAccess::grant(*box.collections).sides,
                "sides",
                reason
            )) {
            hold(*box.collections, std::move(reason));
            failed = true;
        } else {
            bool first_ok = false;
            bool second_ok = false;
            std::string first = lua_string(state, 2, first_ok);
            std::string second = one_list ? lua_string(state, 3, second_ok) : std::string();
            if (!first_ok || (one_list && !second_ok)) {
                hold(*box.collections, "a build list name is text");
                failed = true;
            } else {
                scratch_of(*box.collections).builder = one_list ? std::move(first) : first;
                scratch_of(*box.collections).raw_names.clear();
                scratch_of(*box.collections)
                    .raw_names.push_back(one_list ? std::move(second) : std::move(first));
            }
        }
    }
    if (failed) {
        settle(state, *box.collections);
        return error_emit();
    }
    if (!resolve_names(state, *box.collections, one_list)) {
        settle(state, *box.collections);
        return error_emit();
    }
    {
        std::string reason;
        std::size_t doc = k_no_doc;
        std::string path;
        if (!load_list_document(box, doc, path, reason)) {
            hold(*box.collections, std::move(reason));
            failed = true;
        } else {
            rules::LoadedDocument& loaded =
                SideCollectionsAccess::documents(*box.collections).at(doc);
            tdf::Document* document = loaded.document.get();
            tdf::Block* root = tdf::editable_root(document);
            tdf::Block* section = ensure_canbuild(*box.collections, document, root, false, reason);
            if (section != nullptr) {
                const uint32_t children = tdf::child_count(section);
                note_visit(scratch_of(*box.collections), children);
                for (uint32_t child_index = 0; child_index < children; ++child_index) {
                    tdf::Block* listed = tdf::editable_child(section, child_index);
                    if (listed == nullptr || listed->name == nullptr)
                        continue;
                    if (one_list && !same_text(listed->name, scratch_of(*box.collections).builder))
                        continue;
                    std::vector<std::string> kept;
                    read_list(scratch_of(*box.collections), listed, kept);
                    const std::size_t before = kept.size();
                    const std::string removing = scratch_of(*box.collections).names[0];
                    kept.erase(
                        std::remove_if(
                            kept.begin(),
                            kept.end(),
                            [&](const std::string& entry) { return same_text(entry, removing); }
                        ),
                        kept.end()
                    );
                    if (kept.size() == before)
                        continue;
                    if (!write_list(document, listed, kept, path, *box.collections, reason)) {
                        hold(*box.collections, std::move(reason));
                        failed = true;
                        break;
                    }
                }
            }
        }
    }
    settle(state, *box.collections);
    return failed ? error_emit() : Emit{};
}

Emit prepare_sides_index(lua_State* state, const Box& box) {
    if (lua_isinteger(state, 2)) {
        const lua_Integer slot = lua_tointeger(state, 2);
        bool failed = false;
        uint32_t count = 0;
        {
            std::string error;
            count = count_sides(box, error);
            if (!error.empty()) {
                hold(*box.collections, std::move(error));
                failed = true;
            }
        }
        note_field(scratch_of(*box.collections));
        settle(state, *box.collections);
        if (failed)
            return error_emit();
        if (slot < 1 || static_cast<uint64_t>(slot) > count) {
            Emit missing;
            missing.kind = EmitKind::nil;
            return missing;
        }
        Emit emit;
        emit.kind = EmitKind::side;
        emit.slot = static_cast<uint32_t>(slot - 1);
        return emit;
    }
    EmitKind method = EmitKind::nil;
    bool failed = false;
    uint32_t found_slot = 0;
    bool have_slot = false;
    {
        bool ok = false;
        const std::string key = lua_string(state, 2, ok);
        if (ok)
            note_text(scratch_of(*box.collections), key.size());
        if (!ok) {
            method = EmitKind::nil;
        } else if (same_text(key, "each")) {
            method = EmitKind::method_each;
        } else if (same_text(key, "canbuild")) {
            method = EmitKind::method_canbuild;
        } else if (same_text(key, "set_canbuild")) {
            method = EmitKind::method_set;
        } else if (same_text(key, "add_canbuild")) {
            method = EmitKind::method_add;
        } else if (same_text(key, "remove_canbuild")) {
            method = EmitKind::method_remove;
        } else {
            std::string error;
            const std::optional<uint32_t> slot = side_named(box, key, error);
            if (!error.empty()) {
                hold(*box.collections, std::move(error));
                failed = true;
            } else if (!slot.has_value()) {
                method = EmitKind::nil;
            } else {
                have_slot = true;
                found_slot = *slot;
                method = EmitKind::side;
            }
        }
    }
    note_field(scratch_of(*box.collections));
    settle(state, *box.collections);
    if (failed)
        return error_emit();
    Emit emit;
    emit.kind = method;
    emit.slot = found_slot;
    if (!have_slot && method == EmitKind::side)
        emit.kind = EmitKind::nil;
    return emit;
}

Emit prepare_sides_newindex(lua_State* state, const Box& box) {
    const bool clearing = lua_type(state, 3) == LUA_TNIL;
    bool known = false;
    bool failed = false;
    {
        if (lua_isinteger(state, 2)) {
            const lua_Integer slot = lua_tointeger(state, 2);
            std::string error;
            const uint32_t count = count_sides(box, error);
            if (!error.empty()) {
                hold(*box.collections, std::move(error));
                failed = true;
            }
            known = slot >= 1 && static_cast<uint64_t>(slot) <= count;
        } else {
            bool ok = false;
            const std::string key = lua_string(state, 2, ok);
            if (ok && (same_text(key, "each") || same_text(key, "canbuild") ||
                       same_text(key, "set_canbuild") || same_text(key, "add_canbuild") ||
                       same_text(key, "remove_canbuild"))) {
                hold(*box.collections, "that name is a method");
                failed = true;
            } else if (ok) {
                std::string error;
                known = side_named(box, key, error).has_value();
                if (!error.empty()) {
                    hold(*box.collections, std::move(error));
                    failed = true;
                }
            }
        }
    }
    if (!failed)
        hold(
            *box.collections,
            known && clearing ? "a mutator cannot remove a side" : "a mutator cannot add a side"
        );
    settle(state, *box.collections);
    return error_emit();
}

Emit prepare_side_index(lua_State* state, const Box& box) {
    bool failed = false;
    bool absent = false;
    {
        bool ok = false;
        const std::string key = lua_string(state, 2, ok);
        if (!ok) {
            absent = true;
        } else {
            note_text(scratch_of(*box.collections), key.size());
            std::string error;
            std::size_t doc = k_no_doc;
            tdf::Block* block = nullptr;
            std::string path;
            if (!load_side(box, box.side_slot, doc, block, path, error)) {
                hold(*box.collections, std::move(error));
                failed = true;
            } else if (child_named(scratch_of(*box.collections), block, key)) {
                note_field(scratch_of(*box.collections));
                absent = true;
            } else {
                take_field(scratch_of(*box.collections), block, key);
                absent = !scratch_of(*box.collections).present;
            }
        }
    }
    settle(state, *box.collections);
    if (failed)
        return error_emit();
    Emit emit;
    emit.kind = absent ? EmitKind::nil : EmitKind::text;
    return emit;
}

Emit prepare_side_newindex(lua_State* state, const Box& box) {
    bool failed = false;
    bool needs_unit = false;
    {
        bool ok = false;
        const std::string key = lua_string(state, 2, ok);
        if (!ok) {
            hold(*box.collections, "a side field name is text");
            failed = true;
        } else if (lua_type(state, 3) != LUA_TSTRING) {
            hold(*box.collections, "a side value is text");
            failed = true;
        } else {
            std::string reason;
            if (!layer_ok(
                    *box.collections,
                    SideCollectionsAccess::grant(*box.collections).sides,
                    "sides",
                    reason
                )) {
                hold(*box.collections, std::move(reason));
                failed = true;
            } else {
                const std::string raw = lua_string(state, 3, ok);
                std::size_t doc = k_no_doc;
                tdf::Block* block = nullptr;
                std::string path;
                if (!load_side(box, box.side_slot, doc, block, path, reason)) {
                    hold(*box.collections, std::move(reason));
                    failed = true;
                } else if (child_named(scratch_of(*box.collections), block, key)) {
                    hold(*box.collections, "a side's panels are not changed");
                    failed = true;
                } else {
                    std::string stored;
                    if (!checked_text(
                            box, box.side_slot, block, key, raw, stored, needs_unit, reason
                        )) {
                        hold(*box.collections, std::move(reason));
                        failed = true;
                    } else {
                        scratch_of(*box.collections).key = key;
                        scratch_of(*box.collections).text = stored;
                        scratch_of(*box.collections).path = path;
                        rules::LoadedDocument& loaded =
                            SideCollectionsAccess::documents(*box.collections).at(doc);
                        scratch_of(*box.collections).record =
                            block->name != nullptr ? block->name : "";
                        scratch_of(*box.collections).path = loaded.path;
                        (void)loaded;
                    }
                }
            }
        }
    }
    if (failed) {
        settle(state, *box.collections);
        return error_emit();
    }
    if (needs_unit) {
        scratch_of(*box.collections).builder = scratch_of(*box.collections).text;
        const int found = SideCollectionsAccess::lookup(
            *box.collections, state, scratch_of(*box.collections).builder
        );
        if (found < 0) {
            settle(state, *box.collections);
            return error_emit();
        }
        if (found == 0) {
            std::string reason = "there is no unit ";
            reason.append(scratch_of(*box.collections).builder);
            hold(*box.collections, std::move(reason));
            settle(state, *box.collections);
            return error_emit();
        }
        {
            std::string reason;
            if (cutting_character(scratch_of(*box.collections).text, reason)) {
                hold(*box.collections, std::move(reason));
                settle(state, *box.collections);
                return error_emit();
            }
        }
    }
    {
        std::string reason;
        SideScratch& scratch = scratch_of(*box.collections);
        const std::string path = scratch.path;
        const std::string record = scratch.record;
        const std::string key = scratch.key;
        const std::string value = scratch.text;
        std::size_t doc = k_no_doc;
        tdf::Block* block = nullptr;
        std::string loaded_path;
        if (!load_side(box, box.side_slot, doc, block, loaded_path, reason)) {
            hold(*box.collections, std::move(reason));
            failed = true;
        } else {
            rules::LoadedDocument& loaded =
                SideCollectionsAccess::documents(*box.collections).at(doc);
            if (!put_text(
                    loaded.document.get(), block, path, record, key, value, *box.collections, reason
                )) {
                hold(*box.collections, std::move(reason));
                failed = true;
            }
        }
    }
    settle(state, *box.collections);
    return failed ? error_emit() : Emit{};
}

void collect_children(SideScratch& scratch, const tdf::Block* parent) {
    scratch.names.clear();
    const uint32_t count = parent != nullptr ? tdf::child_count(parent) : 0;
    note_visit(scratch, count == 0 ? uint64_t{1} : count);
    scratch.names.reserve(count);
    for (uint32_t index = 0; index < count; ++index) {
        const tdf::Block* child = tdf::child_at(parent, index);
        const char* name = child != nullptr && child->name != nullptr ? child->name : "";
        note_text(scratch, std::strlen(name));
        scratch.names.emplace_back(name);
    }
    scratch.scan = true;
}

/// The first child of this name. The walk is noted, not charged, here.
tdf::Block* find_child(SideScratch& scratch, tdf::Block* parent, std::string_view key) {
    if (parent == nullptr)
        return nullptr;
    const uint32_t count = tdf::child_count(parent);
    note_visit(scratch, count == 0 ? uint64_t{1} : count);
    note_text(scratch, key.size());
    for (uint32_t index = 0; index < count; ++index) {
        tdf::Block* child = tdf::editable_child(parent, index);
        if (child == nullptr || child->name == nullptr)
            continue;
        note_text(scratch, std::strlen(child->name));
        if (same_text(child->name, key))
            return child;
    }
    return nullptr;
}

Emit prepare_section_keys(lua_State* state, const Box& box) {
    bool failed = false;
    {
        std::string reason;
        if (!section_live(box, reason)) {
            hold(*box.collections, std::move(reason));
            failed = true;
        } else {
            const tdf::Block* block = box.block;
            const uint32_t count = tdf::property_count(block);
            std::vector<uint32_t> places(count);
            for (uint32_t place = 0; place < count; ++place)
                places[place] = place;
            if (block != nullptr && block->properties != nullptr) {
                std::stable_sort(
                    places.begin(), places.end(), [&](uint32_t earlier, uint32_t later) {
                        return block->properties[earlier].order < block->properties[later].order;
                    }
                );
            }
            SideScratch& scratch = scratch_of(*box.collections);
            scratch.names.clear();
            scratch.names.reserve(count);
            for (const uint32_t place : places) {
                note_visit(scratch, 1);
                const char* key = block != nullptr && block->properties != nullptr
                                      ? block->properties[place].key
                                      : nullptr;
                const char* text = key != nullptr ? key : "";
                note_text(scratch, std::strlen(text));
                scratch.names.emplace_back(text);
            }
            if (scratch.comparisons > std::numeric_limits<uint64_t>::max() - count)
                scratch.comparisons = std::numeric_limits<uint64_t>::max();
            else
                scratch.comparisons += count;
            scratch.scan = true;
        }
    }
    settle(state, *box.collections);
    if (failed)
        return error_emit();
    Emit emit;
    emit.kind = EmitKind::names;
    return emit;
}

Emit prepare_section_sections(lua_State* state, const Box& box) {
    bool failed = false;
    {
        std::string reason;
        if (!section_live(box, reason)) {
            hold(*box.collections, std::move(reason));
            failed = true;
        } else {
            collect_children(scratch_of(*box.collections), box.block);
        }
    }
    settle(state, *box.collections);
    if (failed)
        return error_emit();
    Emit emit;
    emit.kind = EmitKind::names;
    return emit;
}

Emit prepare_table_sections(lua_State* state, const Box& box) {
    bool failed = false;
    {
        const rules::DocumentSet& documents = SideCollectionsAccess::documents(*box.collections);
        if (box.doc_index >= documents.size()) {
            hold(*box.collections, "that section was removed");
            failed = true;
        } else {
            collect_children(scratch_of(*box.collections), root_of(box));
        }
    }
    settle(state, *box.collections);
    if (failed)
        return error_emit();
    Emit emit;
    emit.kind = EmitKind::names;
    return emit;
}

Emit prepare_mutation(lua_State* state, const Box& box, bool on_section, bool removing) {
    bool failed = false;
    {
        std::string reason;
        if (!layer_ok(
                *box.collections,
                SideCollectionsAccess::grant(*box.collections).tables,
                "tables",
                reason
            )) {
            hold(*box.collections, std::move(reason));
            failed = true;
        } else if (on_section && !section_live(box, reason)) {
            hold(*box.collections, std::move(reason));
            failed = true;
        } else {
            bool ok = false;
            const std::string raw = lua_string(state, 2, ok);
            if (!ok) {
                hold(*box.collections, "a section name is text");
                failed = true;
            } else if (!removing && cutting_character(raw, reason)) {
                hold(*box.collections, std::move(reason));
                failed = true;
            } else {
                const std::string_view trimmed = trim_ends(raw);
                if (!removing && trimmed.empty()) {
                    hold(*box.collections, "a section name is empty");
                    failed = true;
                } else {
                    const std::string name = removing ? raw : std::string(trimmed);
                    rules::LoadedDocument& loaded =
                        SideCollectionsAccess::documents(*box.collections).at(box.doc_index);
                    tdf::Document* document = loaded.document.get();
                    tdf::Block* parent = on_section ? box.block : tdf::editable_root(document);
                    if (parent == nullptr) {
                        hold(*box.collections, "that section was removed");
                        failed = true;
                    } else if (removing) {
                        SideScratch& scratch = scratch_of(*box.collections);
                        const uint32_t count = tdf::child_count(parent);
                        note_visit(scratch, count == 0 ? uint64_t{1} : count);
                        tdf::Block* child = nullptr;
                        uint32_t child_index = 0;
                        for (uint32_t index = 0; index < count; ++index) {
                            tdf::Block* candidate = tdf::editable_child(parent, index);
                            if (candidate == nullptr || candidate->name == nullptr)
                                continue;
                            note_text(scratch, std::strlen(candidate->name));
                            if (!same_text(candidate->name, name))
                                continue;
                            child = candidate;
                            child_index = index;
                            break;
                        }
                        if (child != nullptr) {
                            const std::string record = parent_record(box, parent);
                            const std::string stored = child->name != nullptr ? child->name : name;
                            const tdf::EditStatus status =
                                tdf::remove_child(document, parent, child_index);
                            if (status != tdf::EditStatus::ok) {
                                hold(*box.collections, tdf::edit_status_message(status));
                                failed = true;
                            } else if (!commit_field(
                                           *box.collections,
                                           loaded.path,
                                           record,
                                           stored,
                                           stored,
                                           std::nullopt,
                                           reason
                                       )) {
                                hold(*box.collections, std::move(reason));
                                failed = true;
                            }
                        }
                    } else {
                        tdf::EditStatus status = tdf::EditStatus::ok;
                        tdf::Block* created = tdf::add_child(document, parent, name, &status);
                        if (created == nullptr) {
                            hold(*box.collections, tdf::edit_status_message(status));
                            failed = true;
                        } else {
                            const std::string record = parent_record(box, parent);
                            const std::string stored =
                                created->name != nullptr ? created->name : name;
                            if (!commit_field(
                                    *box.collections,
                                    loaded.path,
                                    record,
                                    stored,
                                    std::nullopt,
                                    stored,
                                    reason
                                )) {
                                hold(*box.collections, std::move(reason));
                                failed = true;
                            }
                        }
                    }
                }
            }
        }
    }
    settle(state, *box.collections);
    return failed ? error_emit() : Emit{};
}

Emit prepare_section_index(lua_State* state, const Box& box) {
    Emit emit;
    emit.kind = EmitKind::nil;
    bool failed = false;
    {
        bool ok = false;
        const std::string key = lua_string(state, 2, ok);
        if (!ok) {
            note_field(scratch_of(*box.collections));
        } else {
            note_text(scratch_of(*box.collections), key.size());
            std::string reason;
            if (!section_live(box, reason)) {
                hold(*box.collections, std::move(reason));
                failed = true;
            } else if (same_text(key, "keys")) {
                note_field(scratch_of(*box.collections));
                emit.kind = EmitKind::method_keys;
            } else if (same_text(key, "sections")) {
                note_field(scratch_of(*box.collections));
                emit.kind = EmitKind::method_sections;
            } else if (same_text(key, "add_section")) {
                note_field(scratch_of(*box.collections));
                emit.kind = EmitKind::method_add_section;
            } else if (same_text(key, "remove_section")) {
                note_field(scratch_of(*box.collections));
                emit.kind = EmitKind::method_remove_section;
            } else {
                tdf::Block* child = find_child(scratch_of(*box.collections), box.block, key);
                if (child != nullptr) {
                    note_field(scratch_of(*box.collections));
                    emit.kind = EmitKind::section;
                    emit.doc = box.doc_index;
                    emit.parent = box.block;
                    emit.block = child;
                } else {
                    take_field(scratch_of(*box.collections), box.block, key);
                    emit.kind =
                        scratch_of(*box.collections).present ? EmitKind::text : EmitKind::nil;
                }
            }
        }
    }
    settle(state, *box.collections);
    if (failed)
        return error_emit();
    return emit;
}

Emit prepare_section_newindex(lua_State* state, const Box& box) {
    bool failed = false;
    {
        bool ok = false;
        const std::string key = lua_string(state, 2, ok);
        if (!ok) {
            hold(*box.collections, "a table key is text");
            failed = true;
        } else {
            std::string reason;
            if (!layer_ok(
                    *box.collections,
                    SideCollectionsAccess::grant(*box.collections).tables,
                    "tables",
                    reason
                )) {
                hold(*box.collections, std::move(reason));
                failed = true;
            } else if (!section_live(box, reason)) {
                hold(*box.collections, std::move(reason));
                failed = true;
            } else if (
                same_text(key, "keys") || same_text(key, "sections") ||
                same_text(key, "add_section") || same_text(key, "remove_section")
            ) {
                hold(*box.collections, "that name is a method");
                failed = true;
            } else if (child_named(scratch_of(*box.collections), box.block, key)) {
                hold(*box.collections, "a section is not assigned");
                failed = true;
            } else if (lua_type(state, 3) == LUA_TNIL) {
                hold(*box.collections, "nil does not remove a table key");
                failed = true;
            } else if (lua_type(state, 3) != LUA_TSTRING) {
                hold(*box.collections, "a table value is text");
                failed = true;
            } else {
                const std::string raw = lua_string(state, 3, ok);
                if (cutting_character(raw, reason)) {
                    hold(*box.collections, std::move(reason));
                    failed = true;
                } else {
                    const std::string_view trimmed = trim_ends(raw);
                    if (trimmed.size() > k_text_limit) {
                        hold(*box.collections, "a value is longer than the field can hold");
                        failed = true;
                    } else {
                        const std::string stored(trimmed);
                        rules::LoadedDocument& loaded =
                            SideCollectionsAccess::documents(*box.collections).at(box.doc_index);
                        const std::string path = loaded.path;
                        const std::string record =
                            box.block->name != nullptr ? box.block->name : std::string();
                        if (!put_text(
                                loaded.document.get(),
                                box.block,
                                path,
                                record,
                                key,
                                stored,
                                *box.collections,
                                reason
                            )) {
                            hold(*box.collections, std::move(reason));
                            failed = true;
                        }
                    }
                }
            }
        }
    }
    settle(state, *box.collections);
    return failed ? error_emit() : Emit{};
}

Emit prepare_table_index(lua_State* state, const Box& box) {
    Emit emit;
    emit.kind = EmitKind::nil;
    {
        bool ok = false;
        const std::string key = lua_string(state, 2, ok);
        if (!ok) {
            note_field(scratch_of(*box.collections));
        } else {
            note_text(scratch_of(*box.collections), key.size());
            if (same_text(key, "sections")) {
                note_field(scratch_of(*box.collections));
                emit.kind = EmitKind::method_table_sections;
            } else if (same_text(key, "add_section")) {
                note_field(scratch_of(*box.collections));
                emit.kind = EmitKind::method_add_section;
            } else if (same_text(key, "remove_section")) {
                note_field(scratch_of(*box.collections));
                emit.kind = EmitKind::method_remove_section;
            } else {
                tdf::Block* root = root_of(box);
                tdf::Block* child = find_child(scratch_of(*box.collections), root, key);
                note_field(scratch_of(*box.collections));
                if (child != nullptr) {
                    emit.kind = EmitKind::section;
                    emit.doc = box.doc_index;
                    emit.parent = root;
                    emit.block = child;
                }
            }
        }
    }
    settle(state, *box.collections);
    return emit;
}

Emit prepare_table_newindex(lua_State* state, const Box& box) {
    {
        bool ok = false;
        const std::string key = lua_string(state, 2, ok);
        if (ok && (same_text(key, "sections") || same_text(key, "add_section") ||
                   same_text(key, "remove_section")))
            hold(*box.collections, "that name is a method");
        else
            hold(*box.collections, "a section is not assigned");
    }
    settle(state, *box.collections);
    return error_emit();
}

Emit prepare_tables_index(lua_State* state, const Box& box) {
    Emit emit;
    emit.kind = EmitKind::nil;
    bool failed = false;
    {
        bool ok = false;
        const std::string requested = lua_string(state, 2, ok);
        if (!ok) {
            emit.kind = EmitKind::nil;
        } else {
            note_text(scratch_of(*box.collections), requested.size());
            std::string canonical;
            const TableKind kind = classify_table(*box.layout, requested, canonical);
            if (kind == TableKind::sidedata) {
                hold(*box.collections, "use ctx.sides");
                failed = true;
            } else if (kind == TableKind::refused) {
                std::string message = requested;
                message.append(k_table_refuse);
                hold(*box.collections, std::move(message));
                failed = true;
            } else {
                PathContext context;
                context.layout = box.layout;
                const PathVerdict verdict = check_mutable_path(canonical, context);
                if (!verdict.allowed()) {
                    hold(*box.collections, verdict.message);
                    failed = true;
                } else {
                    std::string error;
                    const std::size_t doc =
                        document_index(*box.collections, *box.store, canonical, error, false);
                    if (!error.empty()) {
                        hold(*box.collections, std::move(error));
                        failed = true;
                    } else if (doc == k_no_doc) {
                        emit.kind = EmitKind::nil;
                    } else {
                        emit.kind = EmitKind::table;
                        emit.doc = doc;
                    }
                }
            }
        }
    }
    note_field(scratch_of(*box.collections));
    settle(state, *box.collections);
    if (failed)
        return error_emit();
    return emit;
}

Emit prepare_side_count_scan(lua_State* state, const Box& box) {
    return prepare_side_count(state, box, true);
}

Emit prepare_side_count_quiet(lua_State* state, const Box& box) {
    return prepare_side_count(state, box, false);
}

using PrepareFn = Emit (*)(lua_State*, const Box&);

Emit call_prepare(lua_State* state, const Box& box, PrepareFn prepare) {
    try {
        return prepare(state, box);
    } catch (const std::bad_alloc&) {
        hold(*box.collections, "out of memory");
    } catch (const std::exception& ex) {
        const char* what = ex.what();
        hold(*box.collections, what != nullptr && what[0] != '\0' ? what : "the hook stopped");
    }
    return error_emit();
}

int raise_box(const Box& box, lua_State* state) {
    return SideCollectionsAccess::raise(*box.collections, state);
}

void ensure_meta(
    lua_State* state,
    const char* name,
    lua_CFunction index,
    lua_CFunction newindex,
    lua_CFunction length,
    lua_CFunction pairs
) {
    if (luaL_newmetatable(state, name) == 0) {
        lua_pop(state, 1);
        return;
    }
    lua_pushcfunction(state, index);
    lua_setfield(state, -2, "__index");
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
    lua_pushboolean(state, 0);
    lua_setfield(state, -2, "__metatable");
    lua_pop(state, 1);
}

extern "C" {

static int sides_each(lua_State* state);
static int sides_next(lua_State* state);
static int sides_pairs(lua_State* state);
static int sides_len(lua_State* state);
static int sides_index(lua_State* state);
static int sides_newindex(lua_State* state);
static int side_index(lua_State* state);
static int side_newindex(lua_State* state);
static int read_canbuild(lua_State* state);
static int set_canbuild(lua_State* state);
static int add_canbuild(lua_State* state);
static int remove_canbuild(lua_State* state);
static int section_keys(lua_State* state);
static int section_sections(lua_State* state);
static int section_index(lua_State* state);
static int section_newindex(lua_State* state);
static int add_section(lua_State* state);
static int remove_section(lua_State* state);
static int table_sections(lua_State* state);
static int table_index(lua_State* state);
static int table_newindex(lua_State* state);
static int tables_index(lua_State* state);
static int tables_newindex(lua_State* state);

static int push_method(lua_State* state, EmitKind kind) {
    lua_CFunction function = nullptr;
    if (kind == EmitKind::method_each)
        function = sides_each;
    else if (kind == EmitKind::method_canbuild)
        function = read_canbuild;
    else if (kind == EmitKind::method_set)
        function = set_canbuild;
    else if (kind == EmitKind::method_add)
        function = add_canbuild;
    else if (kind == EmitKind::method_remove)
        function = remove_canbuild;
    else if (kind == EmitKind::method_keys)
        function = section_keys;
    else if (kind == EmitKind::method_sections)
        function = section_sections;
    else if (kind == EmitKind::method_add_section)
        function = add_section;
    else if (kind == EmitKind::method_remove_section)
        function = remove_section;
    else if (kind == EmitKind::method_table_sections)
        function = table_sections;
    if (function == nullptr)
        return 0;
    lua_pushcfunction(state, function);
    return 1;
}

static int finish_value(lua_State* state, const Box& box, PrepareFn prepare) {
    const Emit emit = call_prepare(state, box, prepare);
    if (emit.kind == EmitKind::error)
        return raise_box(box, state);
    const int method = push_method(state, emit.kind);
    if (method != 0)
        return method;
    return publish(state, box, emit);
}

static int sides_each(lua_State* state) {
    Box* box = box_at(state, 1, BoxKind::sides);
    if (box == nullptr) {
        lua_pushliteral(state, "expected the sides collection");
        return lua_error(state);
    }
    if (!lua_isfunction(state, 2)) {
        hold(*box->collections, "each expects a function");
        return raise_box(*box, state);
    }
    const Emit emit = call_prepare(state, *box, prepare_side_count_scan);
    if (emit.kind == EmitKind::error)
        return raise_box(*box, state);
    const uint32_t count = emit.count;
    for (uint32_t index = 0; index < count; ++index) {
        lua_pushvalue(state, 2);
        push_side(state, *box, index);
        lua_call(state, 1, 0);
    }
    return 0;
}

static int sides_next(lua_State* state) {
    Box* box = box_at(state, 1, BoxKind::sides);
    if (box == nullptr)
        return 0;
    const Emit emit = call_prepare(state, *box, prepare_side_count_quiet);
    if (emit.kind == EmitKind::error)
        return raise_box(*box, state);
    lua_Integer slot = 0;
    if (lua_isinteger(state, 2))
        slot = lua_tointeger(state, 2);
    if (slot < 0 || static_cast<uint64_t>(slot) >= emit.count)
        return 0;
    lua_pushinteger(state, slot + 1);
    push_side(state, *box, static_cast<uint32_t>(slot));
    return 2;
}

static int sides_pairs(lua_State* state) {
    Box* box = box_at(state, 1, BoxKind::sides);
    if (box == nullptr) {
        lua_pushliteral(state, "expected the sides collection");
        return lua_error(state);
    }
    scratch_of(*box->collections).scan = true;
    settle(state, *box->collections);
    lua_pushcfunction(state, sides_next);
    lua_pushvalue(state, 1);
    lua_pushinteger(state, 0);
    return 3;
}

static int sides_len(lua_State* state) {
    Box* box = box_at(state, 1, BoxKind::sides);
    if (box == nullptr) {
        lua_pushliteral(state, "expected the sides collection");
        return lua_error(state);
    }
    return finish_value(state, *box, prepare_side_count_quiet);
}

static int sides_index(lua_State* state) {
    Box* box = box_at(state, 1, BoxKind::sides);
    if (box == nullptr) {
        lua_pushliteral(state, "expected the sides collection");
        return lua_error(state);
    }
    return finish_value(state, *box, prepare_sides_index);
}

static int sides_newindex(lua_State* state) {
    Box* box = box_at(state, 1, BoxKind::sides);
    if (box == nullptr) {
        lua_pushliteral(state, "expected the sides collection");
        return lua_error(state);
    }
    const Emit emit = call_prepare(state, *box, prepare_sides_newindex);
    if (emit.kind == EmitKind::error)
        return raise_box(*box, state);
    return 0;
}

static int side_index(lua_State* state) {
    Box* box = box_at(state, 1, BoxKind::side);
    if (box == nullptr) {
        lua_pushliteral(state, "expected a side");
        return lua_error(state);
    }
    return finish_value(state, *box, prepare_side_index);
}

static int side_newindex(lua_State* state) {
    Box* box = box_at(state, 1, BoxKind::side);
    if (box == nullptr) {
        lua_pushliteral(state, "expected a side");
        return lua_error(state);
    }
    const Emit emit = call_prepare(state, *box, prepare_side_newindex);
    if (emit.kind == EmitKind::error)
        return raise_box(*box, state);
    return 0;
}

static int read_canbuild(lua_State* state) {
    Box* box = box_at(state, 1, BoxKind::sides);
    if (box == nullptr) {
        lua_pushliteral(state, "expected the sides collection");
        return lua_error(state);
    }
    return finish_value(state, *box, prepare_canbuild);
}

static int set_canbuild(lua_State* state) {
    Box* box = box_at(state, 1, BoxKind::sides);
    if (box == nullptr) {
        lua_pushliteral(state, "expected the sides collection");
        return lua_error(state);
    }
    const Emit emit = call_prepare(state, *box, prepare_set_canbuild);
    if (emit.kind == EmitKind::error)
        return raise_box(*box, state);
    return 0;
}

static int add_canbuild(lua_State* state) {
    Box* box = box_at(state, 1, BoxKind::sides);
    if (box == nullptr) {
        lua_pushliteral(state, "expected the sides collection");
        return lua_error(state);
    }
    const Emit emit = call_prepare(state, *box, prepare_add_canbuild);
    if (emit.kind == EmitKind::error)
        return raise_box(*box, state);
    return 0;
}

static int remove_canbuild(lua_State* state) {
    Box* box = box_at(state, 1, BoxKind::sides);
    if (box == nullptr) {
        lua_pushliteral(state, "expected the sides collection");
        return lua_error(state);
    }
    const Emit emit = call_prepare(state, *box, prepare_remove_canbuild);
    if (emit.kind == EmitKind::error)
        return raise_box(*box, state);
    return 0;
}

static int section_keys(lua_State* state) {
    Box* box = box_at(state, 1, BoxKind::section);
    if (box == nullptr) {
        lua_pushliteral(state, "expected a section");
        return lua_error(state);
    }
    return finish_value(state, *box, prepare_section_keys);
}

static int section_sections(lua_State* state) {
    Box* box = box_at(state, 1, BoxKind::section);
    if (box == nullptr) {
        lua_pushliteral(state, "expected a section");
        return lua_error(state);
    }
    return finish_value(state, *box, prepare_section_sections);
}

static int section_index(lua_State* state) {
    Box* box = box_at(state, 1, BoxKind::section);
    if (box == nullptr) {
        lua_pushliteral(state, "expected a section");
        return lua_error(state);
    }
    return finish_value(state, *box, prepare_section_index);
}

static int section_newindex(lua_State* state) {
    Box* box = box_at(state, 1, BoxKind::section);
    if (box == nullptr) {
        lua_pushliteral(state, "expected a section");
        return lua_error(state);
    }
    const Emit emit = call_prepare(state, *box, prepare_section_newindex);
    if (emit.kind == EmitKind::error)
        return raise_box(*box, state);
    return 0;
}

static int mutate_section(lua_State* state, bool removing) {
    Box* box = box_at(state, 1, BoxKind::section);
    const bool on_section = box != nullptr;
    if (!on_section)
        box = box_at(state, 1, BoxKind::table);
    if (box == nullptr) {
        lua_pushliteral(state, "expected a table");
        return lua_error(state);
    }
    Emit emit;
    try {
        emit = prepare_mutation(state, *box, on_section, removing);
    } catch (const std::bad_alloc&) {
        hold(*box->collections, "out of memory");
        emit = error_emit();
    } catch (const std::exception& ex) {
        const char* what = ex.what();
        hold(*box->collections, what != nullptr && what[0] != '\0' ? what : "the hook stopped");
        emit = error_emit();
    }
    if (emit.kind == EmitKind::error)
        return raise_box(*box, state);
    return 0;
}

static int add_section(lua_State* state) {
    return mutate_section(state, false);
}

static int remove_section(lua_State* state) {
    return mutate_section(state, true);
}

static int table_sections(lua_State* state) {
    Box* box = box_at(state, 1, BoxKind::table);
    if (box == nullptr) {
        lua_pushliteral(state, "expected a table");
        return lua_error(state);
    }
    return finish_value(state, *box, prepare_table_sections);
}

static int table_index(lua_State* state) {
    Box* box = box_at(state, 1, BoxKind::table);
    if (box == nullptr) {
        lua_pushliteral(state, "expected a table");
        return lua_error(state);
    }
    return finish_value(state, *box, prepare_table_index);
}

static int table_newindex(lua_State* state) {
    Box* box = box_at(state, 1, BoxKind::table);
    if (box == nullptr) {
        lua_pushliteral(state, "expected a table");
        return lua_error(state);
    }
    const Emit emit = call_prepare(state, *box, prepare_table_newindex);
    if (emit.kind == EmitKind::error)
        return raise_box(*box, state);
    return 0;
}

static int tables_index(lua_State* state) {
    Box* box = box_at(state, 1, BoxKind::tables);
    if (box == nullptr) {
        lua_pushliteral(state, "expected the tables collection");
        return lua_error(state);
    }
    return finish_value(state, *box, prepare_tables_index);
}

static int tables_newindex(lua_State* state) {
    Box* box = box_at(state, 1, BoxKind::tables);
    if (box == nullptr) {
        lua_pushliteral(state, "expected the tables collection");
        return lua_error(state);
    }
    hold(*box->collections, "a table is opened by name, not assigned");
    return raise_box(*box, state);
}

} // extern "C"

} // namespace

void set_context_sides(
    lua_State* state,
    int ctx_index,
    Collections& collections,
    const oa::AssetStore& store,
    const defs::DataLayout& layout,
    const std::string& variant
) {
    ensure_meta(state, k_sides_meta, sides_index, sides_newindex, sides_len, sides_pairs);
    ensure_meta(state, k_side_meta, side_index, side_newindex, nullptr, nullptr);
    ensure_meta(state, k_tables_meta, tables_index, tables_newindex, nullptr, nullptr);
    ensure_meta(state, k_table_meta, table_index, table_newindex, nullptr, nullptr);
    ensure_meta(state, k_section_meta, section_index, section_newindex, nullptr, nullptr);
    const int ctx = lua_absindex(state, ctx_index);
    Box box;
    box.collections = &collections;
    box.store = &store;
    box.layout = &layout;
    box.variant = &variant;
    box.kind = BoxKind::sides;
    push_box(state, box, k_sides_meta);
    lua_setfield(state, ctx, "sides");
    box.kind = BoxKind::tables;
    push_box(state, box, k_tables_meta);
    lua_setfield(state, ctx, "tables");
}

} // namespace oa::data::mutators
