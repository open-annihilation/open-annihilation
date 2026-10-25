// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// One ordered run of mutators. The list is checked, each plan is read in a
// sandbox, the files layer is mounted and documents are parsed from that
// layer. Each mutator then applies its rules and then its hooks, in list
// order. When the caller names a map, map rules run, then each maps entry.
// A pass is a transaction: a failure ends it, drops that mutator and anyone
// who requires it, and the run starts again from the store.
#include "oa/data/mutators/pipeline.hpp"

#include "clone.hpp"
#include "incompatible.hpp"
#include "oa/base/sha256.hpp"
#include "oa/data/defs/asset_files.hpp"
#include "oa/data/defs/files.hpp"
#include "oa/data/defs/unit_catalog.hpp"
#include "oa/data/field_schema.hpp"
#include "oa/data/mod_profile/overrides.hpp"
#include "oa/data/mutators/allow_list.hpp"
#include "oa/data/mutators/hold_range.hpp"
#include "oa/data/mutators/clashes.hpp"
#include "oa/data/mutators/loadout.hpp"
#include "oa/data/mutators/map_rules.hpp"
#include "oa/data/mutators/maps.hpp"
#include "oa/data/mutators/menus.hpp"
#include "oa/data/mutators/rules_lua.hpp"
#include "oa/data/mutators/weapon_ids.hpp"
#include "oa/data/mutation_rules/documents.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/formats/oamod.hpp"
#include "oa/formats/soft_mutator.hpp"
#include "oa/formats/tdf/edit.hpp"
#include "oa/formats/tdf/write.hpp"

#include "lua.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "collections_sides.hpp"

namespace oa::data::mutators {
namespace rules = oa::data::mutation_rules;
namespace host = oa::data::script_host;
namespace tdf = oa::formats::tdf;
namespace defs = oa::data::defs;
namespace fields = oa::data::field_schema;
namespace oamut = oa::formats::oamut;
namespace oamod = oa::formats::oamod;
namespace sm = oa::formats::soft_mutator;

namespace {

/// Base directory names, in DataDirectory order. A rule folder whose first
/// segment is one of these means the layout's folder of that kind.
/// Later tickets extend the plan-key and target-layer tables below, not this.
constexpr const char* k_base_directories[] = {
    "units",
    "weapons",
    "gamedata",
    "ai",
    "guis",
    "unitpics",
    "download",
};

static_assert(
    sizeof(k_base_directories) / sizeof(k_base_directories[0]) == defs::data_directory_count
);

/// How a supported plan key is read. D04, D06, D07, D08 and V18 add rows here.
enum class KeyJob : uint8_t {
    files,
    hacks,
    maps,
    rules,
    units_hook,
    weapons_hook,
    features_hook,
    sides_hook,
    tables_hook,
    menus_hook,
};

struct PlanKeyRow {
    const char* name{};
    KeyJob job{};
    oamut::Layer layer{};
};

constexpr PlanKeyRow k_plan_keys[] = {
    {"features", KeyJob::features_hook, oamut::Layer::features},
    {"files", KeyJob::files, oamut::Layer::files},
    {"hacks", KeyJob::hacks, oamut::Layer::hacks},
    {"maps", KeyJob::maps, oamut::Layer::maps},
    {"menus", KeyJob::menus_hook, oamut::Layer::menus},
    {"rules", KeyJob::rules, oamut::Layer::tables},
    {"sides", KeyJob::sides_hook, oamut::Layer::sides},
    {"tables", KeyJob::tables_hook, oamut::Layer::tables},
    {"units", KeyJob::units_hook, oamut::Layer::units},
    {"weapons", KeyJob::weapons_hook, oamut::Layer::weapons},
};

/// Which manifest layer a rule target uses. The same tickets add rows here.
struct TargetLayerRow {
    rules::TargetKind kind{};
    oamut::Layer layer{};
};

constexpr TargetLayerRow k_target_layers[] = {
    {rules::TargetKind::units, oamut::Layer::units},
    {rules::TargetKind::weapons, oamut::Layer::weapons},
    {rules::TargetKind::features, oamut::Layer::features},
    {rules::TargetKind::sidedata, oamut::Layer::sides},
    {rules::TargetKind::other, oamut::Layer::tables},
};

/// A rule folder that needs its own layer, ahead of the kind table.
struct FolderLayerRow {
    const char* folder{};
    oamut::Layer layer{};
};

constexpr FolderLayerRow k_folder_layers[] = {
    {"maps", oamut::Layer::maps},
};

/// Feature documents live under this folder. It is not one of the seven
/// directories a mod may rename.
constexpr const char* k_features_folder = "features";

/// One mutator's plan, kept after its sandbox has returned.
struct Plan {
    FilesPlan files;
    bool has_files{false};
    rules::MutatorRules rules;
    bool has_rules{false};
    /// `maps/*.ota` blocks, held until the maps phase.
    rules::MutatorRules map_rules;
    bool has_map_rules{false};
    std::vector<MutatorHack> hacks;
    bool has_maps{false};
    bool maps_function{false};
    int maps_ref{LUA_NOREF};
    int units_hook{LUA_NOREF};
    int weapons_hook{LUA_NOREF};
    int features_hook{LUA_NOREF};
    int sides_hook{LUA_NOREF};
    int tables_hook{LUA_NOREF};
    int menus_hook{LUA_NOREF};
};

/// Why reading a plan stopped, when it did.
struct PlanFault {
    bool failed{false};
    Cause cause{Cause::none};
    std::string message;
};

/// What the plan phase found wrong with one part of the plan.
enum class PartFault : uint8_t {
    none,
    wrong_type,     ///< files or rules is not a table, or a hook is not a function
    metatable,      ///< files or rules has a metatable
    mask_shape,     ///< files.mask is not a list of text
    mask_metatable, ///< files.mask has a metatable
    add_shape,      ///< files.add is not a list of text
    add_metatable,  ///< files.add has a metatable
    key_not_text,   ///< files holds a key that is not text
    unknown_key,    ///< files holds a key other than mask and add
};

/// The plan as the plan phase read it, by raw access only.
///
/// It holds registry references, flags and faults, and nothing that needs
/// destroying, so the read can raise. The text of a refusal is made after
/// the phase, from these. Every table a reference names was checked to be
/// plain, and every list to hold only text.
struct PlanShape {
    bool returned_table{false};
    bool metatable{false};
    bool key_not_text{false};
    /// The first unsupported plan key, byte by byte.
    int unknown_key{LUA_NOREF};
    bool has_files{false};
    PartFault files_fault{PartFault::none};
    /// The unsupported key in files, when it is text.
    int files_key{LUA_NOREF};
    int mask_list{LUA_NOREF};
    int add_list{LUA_NOREF};
    bool has_rules{false};
    PartFault rules_fault{PartFault::none};
    int rules_list{LUA_NOREF};
    bool has_hacks{false};
    PartFault hacks_fault{PartFault::none};
    int hacks_table{LUA_NOREF};
    bool has_maps{false};
    PartFault maps_fault{PartFault::none};
    bool maps_function{false};
    int maps_ref{LUA_NOREF};
    bool has_units{false};
    PartFault units_fault{PartFault::none};
    int units_hook{LUA_NOREF};
    bool has_weapons{false};
    PartFault weapons_fault{PartFault::none};
    int weapons_hook{LUA_NOREF};
    bool has_features{false};
    PartFault features_fault{PartFault::none};
    int features_hook{LUA_NOREF};
    bool has_sides{false};
    PartFault sides_fault{PartFault::none};
    int sides_hook{LUA_NOREF};
    bool has_tables{false};
    PartFault tables_fault{PartFault::none};
    int tables_hook{LUA_NOREF};
    bool has_menus{false};
    PartFault menus_fault{PartFault::none};
    int menus_hook{LUA_NOREF};
};

/// Pointers a Lua callback may use. The callback holds none of its own.
struct ContextView {
    const ModIdentity* mod{};
    const std::string* engine{};
    const std::vector<LoadoutOption>* options{};
    /// Set by `ctx.incompatible`. Null when the call has no stop flag.
    DeclaredStop* stop{};
};

/// What `mutate` returned, as the plan phase read it.
struct MutateBox {
    const ContextView* context{};
    PlanShape shape;
    int results{0};
    int missing{0};
};

/// Which collection a hook receives as its first argument.
///
/// One mutator's hooks run in this order, each only when the plan has it.
enum class HookWhich : uint8_t { units, weapons, features, sides, tables, menus };

/// One hook in the order a mutator's turn calls them.
struct HookStep {
    int Plan::* ref{};
    HookWhich which{HookWhich::units};
};

/// The hooks of one mutator, after its rules. V18 adds features, sides and tables.
constexpr HookStep k_hook_steps[] = {
    {&Plan::units_hook, HookWhich::units},
    {&Plan::weapons_hook, HookWhich::weapons},
    {&Plan::features_hook, HookWhich::features},
    {&Plan::sides_hook, HookWhich::sides},
    {&Plan::tables_hook, HookWhich::tables},
    {&Plan::menus_hook, HookWhich::menus},
};

/// Reports whether the plan returned any of the collection hooks.
bool has_data_hook(const Plan& plan) {
    for (const HookStep& step : k_hook_steps) {
        if (plan.*(step.ref) != LUA_NOREF)
            return true;
    }
    return false;
}

/// What one hook call needs. The callback holds none of its own objects.
struct HookBox {
    Collections* collections{};
    Menus* menus{};
    int function_ref{LUA_NOREF};
    HookWhich which{HookWhich::units};
    const ContextView* context{};
    const oa::AssetStore* store{};
    const defs::DataLayout* layout{};
    const std::string* variant{};
};

/// The package files one sandbox may read, sorted byte by byte.
struct Listing {
    std::filesystem::path root;
    std::vector<std::string> paths;
    std::string error;
};

/// Unmounts a layer this run mounted, unless the run published it.
struct MountGuard {
    oa::AssetStore& store;
    bool armed{false};

    ~MountGuard() {
        if (armed)
            store.unmount_mutator_layer();
    }

    void arm() { armed = true; }

    void disarm() { armed = false; }

    void release() {
        if (armed)
            store.unmount_mutator_layer();
        armed = false;
    }
};

/// Folds one byte to lower case.
char fold_ascii(char byte) {
    if (byte >= 'A' && byte <= 'Z')
        return static_cast<char>(byte - 'A' + 'a');
    return byte;
}

/// Copies a text folded to lower case.
std::string fold_copy(std::string_view text) {
    std::string out;
    out.resize(text.size());
    for (std::size_t offset = 0; offset < text.size(); ++offset)
        out[offset] = fold_ascii(text[offset]);
    return out;
}

/// Reports whether two texts match without regard to ASCII case.
bool same_text(std::string_view left, std::string_view right) {
    if (left.size() != right.size())
        return false;
    for (std::size_t offset = 0; offset < left.size(); ++offset) {
        if (fold_ascii(left[offset]) != fold_ascii(right[offset]))
            return false;
    }
    return true;
}

/// Folds a path byte: ASCII capitals become lower case, and '\\' becomes '/'.
char fold_path_byte(char byte) {
    if (byte == '\\')
        return '/';
    return fold_ascii(byte);
}

/// Reports whether `path` is a file inside `folder`, ignoring ASCII case.
bool path_in_folder(std::string_view path, std::string_view folder) {
    if (folder.empty() || path.size() <= folder.size())
        return false;
    if (path[folder.size()] != '/' && path[folder.size()] != '\\')
        return false;
    for (std::size_t offset = 0; offset < folder.size(); ++offset) {
        if (fold_path_byte(path[offset]) != fold_path_byte(folder[offset]))
            return false;
    }
    return true;
}

/// Reports whether two paths match, ignoring ASCII case and slash direction.
bool same_path(std::string_view left, std::string_view right) {
    if (left.size() != right.size())
        return false;
    for (std::size_t offset = 0; offset < left.size(); ++offset) {
        if (fold_path_byte(left[offset]) != fold_path_byte(right[offset]))
            return false;
    }
    return true;
}

/// Replaces a rule folder's first segment when it names a base directory.
std::string resolve_folder(std::string_view folder, const defs::DataLayout& layout) {
    const auto slash = folder.find('/');
    const std::string_view first =
        slash == std::string_view::npos ? folder : folder.substr(0, slash);
    const std::string_view rest =
        slash == std::string_view::npos ? std::string_view{} : folder.substr(slash);
    for (std::size_t directory_index = 0; directory_index < defs::data_directory_count;
         ++directory_index) {
        if (first == k_base_directories[directory_index]) {
            std::string resolved = layout.directories[directory_index];
            resolved.append(rest.data(), rest.size());
            return resolved;
        }
    }
    return std::string{folder};
}

/// Replaces the suffix `fbi` with the layout's unit extension.
std::string resolve_extension(std::string_view extension, const defs::DataLayout& layout) {
    if (extension == "fbi")
        return layout.unit_extension;
    return std::string{extension};
}

/// Classifies a resolved target the way the rules pass does.
rules::TargetKind kind_of(
    std::string_view rule_folder,
    std::string_view resolved_folder,
    std::string_view resolved_extension,
    const defs::DataLayout& layout
) {
    const std::size_t units = static_cast<std::size_t>(defs::DataDirectory::units);
    const std::size_t weapons = static_cast<std::size_t>(defs::DataDirectory::weapons);
    const std::size_t gamedata = static_cast<std::size_t>(defs::DataDirectory::gamedata);
    if (same_text(resolved_folder, layout.directories[units]) &&
        same_text(resolved_extension, layout.unit_extension))
        return rules::TargetKind::units;
    if (same_text(resolved_folder, layout.directories[weapons]))
        return rules::TargetKind::weapons;
    if (rule_folder == k_features_folder ||
        (rule_folder.size() > 9 && rule_folder.starts_with("features/")))
        return rules::TargetKind::features;
    if (same_text(resolved_folder, layout.directories[gamedata]) &&
        same_text(resolved_extension, "sidedata.tdf"))
        return rules::TargetKind::sidedata;
    return rules::TargetKind::other;
}

/// The manifest layer a target kind writes.
oamut::Layer layer_for_kind(rules::TargetKind kind) {
    for (const TargetLayerRow& row : k_target_layers) {
        if (row.kind == kind)
            return row.layer;
    }
    return oamut::Layer::tables;
}

/// The manifest layer a rule target writes. A `maps` folder needs `maps`.
oamut::Layer layer_for_target(std::string_view folder, rules::TargetKind kind) {
    for (const FolderLayerRow& row : k_folder_layers) {
        if (same_path(folder, row.folder))
            return row.layer;
    }
    return layer_for_kind(kind);
}

/// Reports whether the manifest lists `layer`.
bool declares(const oamut::Manifest& manifest, oamut::Layer layer) {
    for (const oamut::Layer have : manifest.layers) {
        if (have == layer)
            return true;
    }
    return false;
}

/// The layers one mutator may write during its turn.
WriteGrant grant_for(const oamut::Manifest& manifest) {
    WriteGrant grant;
    grant.units = declares(manifest, oamut::Layer::units);
    grant.weapons = declares(manifest, oamut::Layer::weapons);
    grant.features = declares(manifest, oamut::Layer::features);
    grant.sides = declares(manifest, oamut::Layer::sides);
    grant.tables = declares(manifest, oamut::Layer::tables);
    return grant;
}

/// Finds a plan key, or null when this engine does not support it.
const PlanKeyRow* plan_key(std::string_view name) {
    for (const PlanKeyRow& row : k_plan_keys) {
        if (name == row.name)
            return &row;
    }
    return nullptr;
}

/// The layout the input asked for, or the layout in force.
const defs::DataLayout& layout_of(const PipelineInput& input) {
    if (input.layout != nullptr)
        return *input.layout;
    return defs::data_layout();
}

/// Marks a mutator that was still in the run as failed.
///
/// `Cause::declared` is incompatibility: the script refused this mod, and
/// the pass is repeated without the mutator, so its staged work is dropped.
void fail_mutator(MutatorResult& result, Phase phase, Cause cause, std::string message) {
    result.hacks.clear();
    if (result.outcome != Outcome::applied)
        return;
    result.outcome = cause == Cause::declared ? Outcome::incompatible : Outcome::failed;
    result.phase = phase;
    result.cause = cause;
    result.message = std::move(message);
}

/// Marks a mutator excluded because something it requires did not apply.
/// The list decided it, so its phase is `loadout`.
void exclude_mutator(MutatorResult& result, std::string message) {
    result.hacks.clear();
    if (result.outcome != Outcome::applied)
        return;
    result.outcome = Outcome::excluded;
    result.phase = Phase::loadout;
    result.cause = Cause::prerequisite_failed;
    result.message = std::move(message);
}

/// Turns a script-host error into a cause. The message stays the host's text.
///
/// The allowances, the real-time limit and the string cap each have their
/// own cause. They stop the script whatever data it works on, so they are
/// never read as the script's own error.
Cause cause_of(host::ErrorKind kind) {
    if (kind == host::ErrorKind::instruction_budget)
        return Cause::instruction_budget;
    if (kind == host::ErrorKind::memory_budget)
        return Cause::memory_budget;
    if (kind == host::ErrorKind::time_limit)
        return Cause::time_limit;
    if (kind == host::ErrorKind::string_length)
        return Cause::string_length;
    return Cause::script_error;
}

/// The seed one mutator draws from: the first eight bytes of the SHA-256 of
/// the seed's eight little-endian bytes followed by the id, read little-endian.
uint64_t mutator_seed(uint64_t seed, std::string_view id) {
    std::vector<uint8_t> bytes(8 + id.size());
    for (int shift = 0; shift < 8; ++shift) {
        bytes[static_cast<std::size_t>(shift)] =
            static_cast<uint8_t>((seed >> (8 * shift)) & 0xffu);
    }
    for (std::size_t offset = 0; offset < id.size(); ++offset)
        bytes[8 + offset] = static_cast<uint8_t>(id[offset]);
    const oa::base::sha256::Digest digest = oa::base::sha256::digest_of(bytes);
    uint64_t mixed = 0;
    for (int shift = 0; shift < 8; ++shift) {
        mixed |= static_cast<uint64_t>(digest[static_cast<std::size_t>(shift)]) << (8 * shift);
    }
    return mixed;
}

/// Manifest defaults, with the player's values written over the ids they set.
std::vector<LoadoutOption> resolve_options(const PipelineMutator& mutator) {
    std::vector<LoadoutOption> values;
    values.reserve(mutator.manifest.options.size());
    for (const oamut::Option& option : mutator.manifest.options) {
        const LoadoutOption* given = nullptr;
        for (const LoadoutOption& candidate : mutator.options) {
            if (candidate.id == option.id) {
                given = &candidate;
                break;
            }
        }
        if (given != nullptr)
            values.push_back(*given);
        else
            values.push_back(default_option(option));
    }
    return values;
}

/// Lists every regular file under `root`, as '/' separated paths sorted
/// byte by byte. A link, or a file that is not regular, refuses the listing.
Listing list_package(const std::filesystem::path& root) {
    Listing listing;
    listing.root = root;
    try {
        std::error_code exists;
        if (!std::filesystem::is_directory(root, exists)) {
            listing.error = "the package folder is missing";
            return listing;
        }
        const std::filesystem::recursive_directory_iterator end;
        std::filesystem::recursive_directory_iterator cursor{root};
        for (; cursor != end; ++cursor) {
            const std::filesystem::directory_entry& entry = *cursor;
            if (entry.is_symlink()) {
                listing.error = "the package holds a link";
                listing.paths.clear();
                return listing;
            }
            if (entry.is_directory())
                continue;
            if (!entry.is_regular_file()) {
                listing.error = "the package holds a file that is not a regular file";
                listing.paths.clear();
                return listing;
            }
            const std::filesystem::path relative = std::filesystem::relative(entry.path(), root);
            const std::string generic = relative.generic_string();
            if (generic.empty() || generic == ".." || generic.starts_with("../") ||
                generic.find(':') != std::string::npos) {
                listing.error = "the package holds a path outside its folder";
                listing.paths.clear();
                return listing;
            }
            listing.paths.push_back(generic);
        }
    } catch (const std::exception& ex) {
        listing.paths.clear();
        listing.error = ex.what() != nullptr ? ex.what() : "the package folder could not be read";
        return listing;
    }
    std::sort(listing.paths.begin(), listing.paths.end());
    return listing;
}

/// Reads `path` only when it is exactly one listed path. Never throws.
bool read_listed(const Listing& listing, std::string_view path, std::vector<uint8_t>& bytes) {
    bytes.clear();
    try {
        const auto found = std::lower_bound(listing.paths.begin(), listing.paths.end(), path);
        if (found == listing.paths.end() || *found != path)
            return false;
        const std::filesystem::path on_disk = listing.root / std::filesystem::path{*found};
        std::ifstream in(on_disk, std::ios::binary);
        if (!in)
            return false;
        in.seekg(0, std::ios::end);
        const std::streamoff length = in.tellg();
        if (length < 0)
            return false;
        in.seekg(0, std::ios::beg);
        bytes.resize(static_cast<std::size_t>(length));
        if (length > 0) {
            in.read(reinterpret_cast<char*>(bytes.data()), length);
            if (!in)
                return false;
        }
        return true;
    } catch (...) {
        bytes.clear();
        return false;
    }
}

/// SourceHooks reader. The context is a Listing.
bool source_read(void* context, std::string_view path, std::vector<uint8_t>& bytes) {
    if (context == nullptr)
        return false;
    return read_listed(*static_cast<const Listing*>(context), path, bytes);
}

/// Pushes one option value. Called only from a Lua callback, so it keeps no
/// C++ object that would need destroying if the allocator raises.
void push_option(lua_State* state, int table, const LoadoutOption* option) {
    if (option->type == oamut::OptionType::toggle)
        lua_pushboolean(state, option->toggle ? 1 : 0);
    else if (option->type == oamut::OptionType::number || option->value.number)
        lua_pushnumber(state, oamod::number_to_double(option->value.number_value));
    else
        lua_pushlstring(state, option->value.text.data(), option->value.text.size());
    lua_setfield(state, table, option->id.c_str());
}

/// Pushes `ctx` with mod, engine, options and `incompatible`. Safe to long-jump out of.
void push_context(lua_State* state, const ContextView* shown) {
    lua_createtable(state, 0, 4);
    const int ctx = lua_gettop(state);
    lua_createtable(state, 0, 3);
    const int mod_table = lua_gettop(state);
    lua_pushlstring(state, shown->mod->id.data(), shown->mod->id.size());
    lua_setfield(state, mod_table, "id");
    lua_pushlstring(state, shown->mod->version.data(), shown->mod->version.size());
    lua_setfield(state, mod_table, "version");
    lua_pushlstring(state, shown->mod->game.data(), shown->mod->game.size());
    lua_setfield(state, mod_table, "game");
    lua_setfield(state, ctx, "mod");
    lua_createtable(state, 0, 1);
    const int engine_table = lua_gettop(state);
    lua_pushlstring(state, shown->engine->data(), shown->engine->size());
    lua_setfield(state, engine_table, "version");
    lua_setfield(state, ctx, "engine");
    const std::size_t count = shown->options->size();
    const int sized = count > 1000000 ? 1000000 : static_cast<int>(count);
    lua_createtable(state, 0, sized);
    const int options_table = lua_gettop(state);
    for (std::size_t option_index = 0; option_index < count; ++option_index)
        push_option(state, options_table, &(*shown->options)[option_index]);
    lua_setfield(state, ctx, "options");
    if (shown->stop != nullptr)
        set_incompatible(state, ctx, shown->stop);
}

int push_mutate(void* context, lua_State* state) {
    auto* box = static_cast<MutateBox*>(context);
    lua_getglobal(state, "mutate");
    if (!lua_isfunction(state, -1)) {
        box->missing = 1;
        lua_pop(state, 1);
        lua_pushnil(state);
        return 0;
    }
    push_context(state, box->context);
    return 1;
}

void read_plan_shape(lua_State* state, int index, PlanShape* shape);

void read_mutate(void* context, lua_State* state, int results) {
    auto* box = static_cast<MutateBox*>(context);
    box->results = results;
    if (results != 1 || !lua_istable(state, -1))
        return;
    read_plan_shape(state, lua_gettop(state), &box->shape);
}

void look_mutate(void* context, lua_State* state) {
    auto* found = static_cast<int*>(context);
    lua_getglobal(state, "mutate");
    *found = lua_isfunction(state, -1) ? 1 : 0;
    lua_pop(state, 1);
}

void install_forms(void*, lua_State* state) {
    install_rule_forms(state);
}

/// The context field that holds the collection `which` receives.
///
/// Units, weapons and menus are pushed again as the argument. Features,
/// sides and tables are the object `ctx` already holds: a second push would
/// be a different value, and the hook's argument is that same object.
const char* shared_hook_field(HookWhich which) {
    if (which == HookWhich::features)
        return "features";
    if (which == HookWhich::sides)
        return "sides";
    if (which == HookWhich::tables)
        return "tables";
    return nullptr;
}

int push_hook(void* context, lua_State* state) {
    auto* box = static_cast<HookBox*>(context);
    lua_rawgeti(state, LUA_REGISTRYINDEX, box->function_ref);
    const char* shared = shared_hook_field(box->which);
    if (shared == nullptr) {
        if (box->which == HookWhich::units)
            box->collections->push_units(state);
        else if (box->which == HookWhich::weapons)
            box->collections->push_weapons(state);
        else
            box->menus->push(state);
    }
    push_context(state, box->context);
    const int ctx = lua_gettop(state);
    box->collections->set_context_collections(state, ctx);
    if (box->menus != nullptr)
        box->menus->set_context(state, ctx);
    if (box->store != nullptr && box->layout != nullptr && box->variant != nullptr)
        set_context_sides(state, ctx, *box->collections, *box->store, *box->layout, *box->variant);
    if (shared != nullptr) {
        lua_getfield(state, ctx, shared);
        lua_insert(state, ctx);
    }
    return 2;
}

/// What the plan phase found in one list of text.
enum class ListFault : uint8_t {
    none,      ///< absent, or plain and holding only text
    shape,     ///< not a list of text
    metatable, ///< a table with a metatable
};

/// Reads one list of text inside the plan phase, by raw access only.
///
/// Nothing here needs destroying, so a refused allocation can raise. A list
/// it accepts is kept by a registry reference.
///
/// @param state the sandbox state
/// @param index the list's stack index
/// @param[out] ref the list's reference, when it is accepted and present
/// @return what was found
ListFault read_text_list(lua_State* state, int index, int* ref) {
    const int list = lua_absindex(state, index);
    if (lua_isnil(state, list))
        return ListFault::none;
    if (!lua_istable(state, list))
        return ListFault::shape;
    if (lua_getmetatable(state, list) != 0) {
        lua_pop(state, 1);
        return ListFault::metatable;
    }
    const lua_Unsigned count = lua_rawlen(state, list);
    for (lua_Unsigned slot = 1; slot <= count; ++slot) {
        const int kind = lua_rawgeti(state, list, static_cast<lua_Integer>(slot));
        lua_pop(state, 1);
        if (kind != LUA_TSTRING)
            return ListFault::shape;
    }
    lua_pushvalue(state, list);
    *ref = luaL_ref(state, LUA_REGISTRYINDEX);
    return ListFault::none;
}

/// Reads the files table inside the plan phase, by raw access only.
///
/// @param state the sandbox state
/// @param index the table's stack index
/// @param[out] shape where the lists and the fault go
void read_files_shape(lua_State* state, int index, PlanShape* shape) {
    const int files = lua_absindex(state, index);
    shape->has_files = true;
    if (!lua_istable(state, files)) {
        shape->files_fault = PartFault::wrong_type;
        return;
    }
    if (lua_getmetatable(state, files) != 0) {
        lua_pop(state, 1);
        shape->files_fault = PartFault::metatable;
        return;
    }
    lua_pushliteral(state, "mask");
    lua_rawget(state, files);
    const ListFault mask = read_text_list(state, -1, &shape->mask_list);
    lua_pop(state, 1);
    if (mask != ListFault::none) {
        shape->files_fault =
            mask == ListFault::metatable ? PartFault::mask_metatable : PartFault::mask_shape;
        return;
    }
    lua_pushliteral(state, "add");
    lua_rawget(state, files);
    const ListFault add = read_text_list(state, -1, &shape->add_list);
    lua_pop(state, 1);
    if (add != ListFault::none) {
        shape->files_fault =
            add == ListFault::metatable ? PartFault::add_metatable : PartFault::add_shape;
        return;
    }
    lua_pushnil(state);
    while (lua_next(state, files) != 0) {
        lua_pop(state, 1);
        if (lua_type(state, -1) != LUA_TSTRING) {
            lua_pop(state, 1);
            shape->files_fault = PartFault::key_not_text;
            return;
        }
        std::size_t length = 0;
        const char* text = lua_tolstring(state, -1, &length);
        const std::string_view key{text, length};
        if (key == "mask" || key == "add")
            continue;
        shape->files_fault = PartFault::unknown_key;
        shape->files_key = luaL_ref(state, LUA_REGISTRYINDEX);
        return;
    }
}

/// Reads the rules list inside the plan phase. D09 reads its entries later.
///
/// @param state the sandbox state
/// @param index the list's stack index
/// @param[out] shape where the list and the fault go
void read_rules_shape(lua_State* state, int index, PlanShape* shape) {
    const int list = lua_absindex(state, index);
    shape->has_rules = true;
    if (!lua_istable(state, list)) {
        shape->rules_fault = PartFault::wrong_type;
        return;
    }
    if (lua_getmetatable(state, list) != 0) {
        lua_pop(state, 1);
        shape->rules_fault = PartFault::metatable;
        return;
    }
    lua_pushvalue(state, list);
    shape->rules_list = luaL_ref(state, LUA_REGISTRYINDEX);
}

/// Reads one hook inside the plan phase and takes its reference there.
///
/// @param state the sandbox state
/// @param index the value's stack index
/// @param[out] present set: the plan names the hook
/// @param[out] fault set when the value is not a function
/// @param[out] ref the function's reference
void read_hook_shape(lua_State* state, int index, bool* present, PartFault* fault, int* ref) {
    const int value = lua_absindex(state, index);
    *present = true;
    if (!lua_isfunction(state, value)) {
        *fault = PartFault::wrong_type;
        return;
    }
    lua_pushvalue(state, value);
    *ref = luaL_ref(state, LUA_REGISTRYINDEX);
}

/// Reads the hacks table inside the plan phase and takes its reference there.
///
/// The values are copied after the phase, so nothing here builds a string.
///
/// @param state the sandbox state
/// @param index the value's stack index
/// @param[out] shape what was found
void read_hacks_shape(lua_State* state, int index, PlanShape* shape) {
    const int value = lua_absindex(state, index);
    shape->has_hacks = true;
    if (lua_istable(state, value) == 0) {
        shape->hacks_fault = PartFault::wrong_type;
        return;
    }
    if (lua_getmetatable(state, value) != 0) {
        lua_pop(state, 1);
        shape->hacks_fault = PartFault::metatable;
        return;
    }
    lua_pushvalue(state, value);
    shape->hacks_table = luaL_ref(state, LUA_REGISTRYINDEX);
}

/// Reads what `mutate` returned, inside the plan phase.
///
/// Raw access only, so no metamethod of the script runs. Nothing here needs
/// destroying, so a refused allocation can raise; the phase reports it with
/// the mutator's allowances and real-time limit in force. A table with a
/// metatable is refused rather than read around: its raw contents need not
/// be what the script meant.
///
/// @param state the sandbox state
/// @param index the plan's stack index
/// @param[out] shape what was found
void read_plan_shape(lua_State* state, int index, PlanShape* shape) {
    const int plan = lua_absindex(state, index);
    shape->returned_table = true;
    if (lua_getmetatable(state, plan) != 0) {
        lua_pop(state, 1);
        shape->metatable = true;
        return;
    }
    lua_pushnil(state);
    const int first_unknown = lua_gettop(state);
    lua_pushnil(state);
    while (lua_next(state, plan) != 0) {
        const int value = lua_gettop(state);
        const int key = value - 1;
        if (lua_type(state, key) != LUA_TSTRING) {
            shape->key_not_text = true;
            lua_settop(state, key);
            continue;
        }
        std::size_t length = 0;
        const char* text = lua_tolstring(state, key, &length);
        const std::string_view name{text, length};
        const PlanKeyRow* row = plan_key(name);
        if (row == nullptr) {
            bool earlier = lua_isnil(state, first_unknown);
            if (!earlier) {
                std::size_t kept_length = 0;
                const char* kept = lua_tolstring(state, first_unknown, &kept_length);
                earlier = name < std::string_view{kept, kept_length};
            }
            if (earlier) {
                lua_pushvalue(state, key);
                lua_replace(state, first_unknown);
            }
        } else if (row->job == KeyJob::files) {
            read_files_shape(state, value, shape);
        } else if (row->job == KeyJob::rules) {
            read_rules_shape(state, value, shape);
        } else if (row->job == KeyJob::hacks) {
            read_hacks_shape(state, value, shape);
        } else if (row->job == KeyJob::maps) {
            shape->has_maps = true;
            if (lua_isfunction(state, value)) {
                shape->maps_function = true;
                lua_pushvalue(state, value);
                shape->maps_ref = luaL_ref(state, LUA_REGISTRYINDEX);
            } else if (!lua_istable(state, value)) {
                shape->maps_fault = PartFault::wrong_type;
            } else if (lua_getmetatable(state, value) != 0) {
                lua_pop(state, 1);
                shape->maps_fault = PartFault::metatable;
            } else {
                lua_pushvalue(state, value);
                shape->maps_ref = luaL_ref(state, LUA_REGISTRYINDEX);
            }
        } else if (row->job == KeyJob::units_hook) {
            read_hook_shape(
                state, value, &shape->has_units, &shape->units_fault, &shape->units_hook
            );
        } else if (row->job == KeyJob::weapons_hook) {
            read_hook_shape(
                state, value, &shape->has_weapons, &shape->weapons_fault, &shape->weapons_hook
            );
        } else if (row->job == KeyJob::features_hook) {
            read_hook_shape(
                state, value, &shape->has_features, &shape->features_fault, &shape->features_hook
            );
        } else if (row->job == KeyJob::sides_hook) {
            read_hook_shape(
                state, value, &shape->has_sides, &shape->sides_fault, &shape->sides_hook
            );
        } else if (row->job == KeyJob::tables_hook) {
            read_hook_shape(
                state, value, &shape->has_tables, &shape->tables_fault, &shape->tables_hook
            );
        } else if (row->job == KeyJob::menus_hook) {
            read_hook_shape(
                state, value, &shape->has_menus, &shape->menus_fault, &shape->menus_hook
            );
        }
        lua_settop(state, key);
    }
    if (lua_isnil(state, first_unknown))
        lua_pop(state, 1);
    else
        shape->unknown_key = luaL_ref(state, LUA_REGISTRYINDEX);
}

/// A plan refusal.
PlanFault refuse_plan(Cause cause, std::string message) {
    PlanFault fault;
    fault.failed = true;
    fault.cause = cause;
    fault.message = std::move(message);
    return fault;
}

/// Copies the text a registry reference names.
///
/// The plan phase made the reference to a string, and no script runs after
/// it, so nothing here calls a script or allocates in the state.
std::string text_at(lua_State* state, int ref) {
    lua_rawgeti(state, LUA_REGISTRYINDEX, ref);
    std::size_t length = 0;
    const char* text = lua_tolstring(state, -1, &length);
    std::string copied = text != nullptr ? std::string(text, length) : std::string{};
    lua_pop(state, 1);
    return copied;
}

/// Copies a list the plan phase accepted. It is plain and holds only text,
/// so nothing here calls a script or allocates in the state.
void copy_text_list(lua_State* state, int ref, std::vector<std::string>& out) {
    if (ref == LUA_NOREF)
        return;
    lua_rawgeti(state, LUA_REGISTRYINDEX, ref);
    const int list = lua_gettop(state);
    const lua_Unsigned count = lua_rawlen(state, list);
    for (lua_Unsigned slot = 1; slot <= count; ++slot) {
        lua_rawgeti(state, list, static_cast<lua_Integer>(slot));
        std::size_t length = 0;
        const char* text = lua_tolstring(state, -1, &length);
        out.emplace_back(text, length);
        lua_pop(state, 1);
    }
    lua_pop(state, 1);
}

/// Turns the files part of a plan into F03's plan, or refuses it.
PlanFault read_files_part(
    lua_State* state, const PlanShape& shape, const oamut::Manifest& manifest, Plan& plan
) {
    const PartFault seen = shape.files_fault;
    if (seen == PartFault::wrong_type)
        return refuse_plan(Cause::invalid_plan, "files must be a table");
    if (seen == PartFault::metatable)
        return refuse_plan(
            Cause::invalid_plan, "files must be a plain table, not one with a metatable"
        );
    if (seen == PartFault::mask_shape)
        return refuse_plan(Cause::invalid_plan, "files.mask must be a list of text");
    if (seen == PartFault::mask_metatable)
        return refuse_plan(
            Cause::invalid_plan, "files.mask must be a plain list, not one with a metatable"
        );
    if (seen == PartFault::add_shape)
        return refuse_plan(Cause::invalid_plan, "files.add must be a list of text");
    if (seen == PartFault::add_metatable)
        return refuse_plan(
            Cause::invalid_plan, "files.add must be a plain list, not one with a metatable"
        );
    if (seen == PartFault::key_not_text)
        return refuse_plan(Cause::invalid_plan, "a key of files must be text");
    if (seen == PartFault::unknown_key)
        return refuse_plan(
            Cause::invalid_plan,
            "the plan key files." + text_at(state, shape.files_key) +
                " is not supported by this engine"
        );
    if (!declares(manifest, oamut::Layer::files))
        return refuse_plan(
            Cause::undeclared_layer,
            "returns a files plan but its manifest does not declare the files layer"
        );
    copy_text_list(state, shape.mask_list, plan.files.mask);
    copy_text_list(state, shape.add_list, plan.files.add);
    plan.has_files = !plan.files.mask.empty() || !plan.files.add.empty();
    return {};
}

/// Reads the rules list with D09 and checks each target's layer.
///
/// D09 reads the list by raw access and keeps C++ objects while it does,
/// so it runs after the plan phase, on a list that phase found plain.
PlanFault read_rules_part(
    lua_State* state,
    const PlanShape& shape,
    const Listing& listing,
    std::string_view entry,
    const oamut::Manifest& manifest,
    const defs::DataLayout& layout,
    Plan& plan
) {
    if (shape.rules_fault == PartFault::wrong_type)
        return refuse_plan(Cause::invalid_plan, "rules must be a list");
    if (shape.rules_fault == PartFault::metatable)
        return refuse_plan(
            Cause::invalid_plan, "rules must be a plain list, not one with a metatable"
        );
    const PackageReader reader = [&listing](std::string_view path, std::string* error) {
        std::vector<uint8_t> bytes;
        if (!read_listed(listing, path, bytes)) {
            if (error != nullptr)
                *error = "the package holds no such file";
            return std::optional<std::vector<uint8_t>>{};
        }
        return std::optional<std::vector<uint8_t>>{std::move(bytes)};
    };
    lua_rawgeti(state, LUA_REGISTRYINDEX, shape.rules_list);
    PlanRules parsed = read_plan_rules(state, lua_gettop(state), reader, entry);
    lua_pop(state, 1);
    if (!parsed.ok())
        return refuse_plan(Cause::unreadable_rules, std::move(parsed.error));
    plan.rules.sources = std::move(parsed.sources);
    plan.has_rules = !plan.rules.sources.empty();
    std::vector<rules::MutatorRules> one;
    one.push_back(plan.rules);
    const std::vector<sm::Target> targets = rules::targets_of(one);
    for (const sm::Target& target : targets) {
        const std::string folder = resolve_folder(target.folder, layout);
        const std::string extension = resolve_extension(target.extension, layout);
        const rules::TargetKind kind = kind_of(target.folder, folder, extension, layout);
        const oamut::Layer layer = layer_for_target(target.folder, kind);
        if (!declares(manifest, layer)) {
            const char* name = oamut::layer_name(layer);
            return refuse_plan(
                Cause::undeclared_layer,
                std::string{"returns a rule for the "} + name +
                    " layer but its manifest does not declare the " + name + " layer"
            );
        }
    }
    // Map OTA blocks wait for the maps phase. Anything else stays on the
    // rules the rules phase already runs, so a map path is never loaded.
    const SplitRules split = split_map_rules(one);
    if (!split.refusals.empty())
        return refuse_plan(Cause::invalid_plan, split.refusals.front());
    plan.rules.sources.clear();
    plan.has_rules = false;
    if (!split.others.empty()) {
        plan.rules.sources = std::move(split.others.front().sources);
        plan.has_rules = !plan.rules.sources.empty();
    }
    plan.map_rules = {};
    plan.has_map_rules = false;
    if (!split.maps.empty()) {
        plan.map_rules = std::move(split.maps.front());
        plan.has_map_rules = !plan.map_rules.sources.empty();
    }
    return {};
}

/// Turns the hacks table the plan phase found into records, or refuses it.
PlanFault read_hacks_part(
    lua_State* state, const PlanShape& shape, const oamut::Manifest& manifest, Plan& plan
) {
    if (shape.hacks_fault == PartFault::wrong_type)
        return refuse_plan(Cause::invalid_plan, "hacks: hacks must be a table");
    if (shape.hacks_fault == PartFault::metatable)
        return refuse_plan(
            Cause::invalid_plan, "hacks: hacks must be a plain table, not one with a metatable"
        );
    if (shape.hacks_table == LUA_NOREF)
        return {};
    lua_rawgeti(state, LUA_REGISTRYINDEX, shape.hacks_table);
    std::string problem = read_hacks(
        state,
        lua_gettop(state),
        plan.rules.mutator,
        declares(manifest, oamut::Layer::hacks),
        plan.hacks
    );
    lua_pop(state, 1);
    if (problem.empty())
        return {};
    // Every refusal of the table's contents, the missing hacks layer among
    // them, is that mutator's failure alone and never stops the game.
    return refuse_plan(Cause::hack_refused, std::move(problem));
}

/// Checks the maps entry and keeps its reference.
///
/// A function is kept as a hook. A table must be a list of finished
/// `where():change()` blocks whose change keys the map view can write.
PlanFault read_maps_part(
    lua_State* state, const PlanShape& shape, const oamut::Manifest& manifest, Plan& plan
) {
    if (shape.maps_fault == PartFault::wrong_type)
        return refuse_plan(
            Cause::invalid_plan, "maps must be a function or a list of where():change()"
        );
    if (shape.maps_fault == PartFault::metatable)
        return refuse_plan(
            Cause::invalid_plan, "maps must be a plain table, not one with a metatable"
        );
    if (!shape.maps_function) {
        lua_rawgeti(state, LUA_REGISTRYINDEX, shape.maps_ref);
        const std::string problem = maps_list_problem(state, lua_gettop(state));
        lua_pop(state, 1);
        if (!problem.empty())
            return refuse_plan(Cause::invalid_plan, problem);
    }
    if (!declares(manifest, oamut::Layer::maps))
        return refuse_plan(
            Cause::undeclared_layer,
            "returns a maps hook but its manifest does not declare the maps layer"
        );
    plan.has_maps = true;
    plan.maps_function = shape.maps_function;
    plan.maps_ref = shape.maps_ref;
    return {};
}

/// Checks one hook the plan phase found and keeps its reference.
PlanFault read_hook_part(
    PartFault seen,
    int found,
    const char* name,
    oamut::Layer layer,
    const oamut::Manifest& manifest,
    int& ref
) {
    if (seen == PartFault::wrong_type)
        return refuse_plan(Cause::invalid_plan, std::string{name} + " must be a function");
    if (!declares(manifest, layer))
        return refuse_plan(
            Cause::undeclared_layer,
            std::string{"returns a "} + name + " hook but its manifest does not declare the " +
                name + " layer"
        );
    ref = found;
    return {};
}

/// Turns the plan the phase read into a `Plan`, or refuses it.
///
/// The checks run in the order the keys sort in: features, files, hacks,
/// maps, menus, rules, sides, tables, units, then weapons, and the first
/// refusal is the one reported.
PlanFault read_returned_plan(
    lua_State* state,
    const PlanShape& shape,
    const Listing& listing,
    std::string_view entry,
    const oamut::Manifest& manifest,
    const defs::DataLayout& layout,
    Plan& plan
) {
    if (!shape.returned_table)
        return refuse_plan(Cause::invalid_plan, "mutate must return a table");
    if (shape.metatable)
        return refuse_plan(
            Cause::invalid_plan, "the plan must be a plain table, not one with a metatable"
        );
    if (shape.key_not_text)
        return refuse_plan(Cause::invalid_plan, "a plan key must be text");
    if (shape.unknown_key != LUA_NOREF)
        return refuse_plan(
            Cause::invalid_plan,
            "the plan key " + text_at(state, shape.unknown_key) + " is not supported by this engine"
        );
    PlanFault fault;
    if (shape.has_features) {
        fault = read_hook_part(
            shape.features_fault,
            shape.features_hook,
            "features",
            oamut::Layer::features,
            manifest,
            plan.features_hook
        );
        if (fault.failed)
            return fault;
    }
    if (shape.has_files) {
        fault = read_files_part(state, shape, manifest, plan);
        if (fault.failed)
            return fault;
    }
    if (shape.has_hacks) {
        fault = read_hacks_part(state, shape, manifest, plan);
        if (fault.failed)
            return fault;
    }
    if (shape.has_maps) {
        fault = read_maps_part(state, shape, manifest, plan);
        if (fault.failed)
            return fault;
    }
    if (shape.has_menus) {
        fault = read_hook_part(
            shape.menus_fault,
            shape.menus_hook,
            "menus",
            oamut::Layer::menus,
            manifest,
            plan.menus_hook
        );
        if (fault.failed)
            return fault;
    }
    if (shape.has_rules) {
        fault = read_rules_part(state, shape, listing, entry, manifest, layout, plan);
        if (fault.failed)
            return fault;
    }
    if (shape.has_sides) {
        fault = read_hook_part(
            shape.sides_fault,
            shape.sides_hook,
            "sides",
            oamut::Layer::sides,
            manifest,
            plan.sides_hook
        );
        if (fault.failed)
            return fault;
    }
    if (shape.has_tables) {
        fault = read_hook_part(
            shape.tables_fault,
            shape.tables_hook,
            "tables",
            oamut::Layer::tables,
            manifest,
            plan.tables_hook
        );
        if (fault.failed)
            return fault;
    }
    if (shape.has_units) {
        fault = read_hook_part(
            shape.units_fault,
            shape.units_hook,
            "units",
            oamut::Layer::units,
            manifest,
            plan.units_hook
        );
        if (fault.failed)
            return fault;
    }
    if (shape.has_weapons) {
        fault = read_hook_part(
            shape.weapons_fault,
            shape.weapons_hook,
            "weapons",
            oamut::Layer::weapons,
            manifest,
            plan.weapons_hook
        );
    }
    return fault;
}

/// The index of `id` in the input list, or nullopt when it is not there.
std::optional<uint16_t> index_of(const std::vector<std::string>& ids, std::string_view id) {
    for (std::size_t slot = 0; slot < ids.size(); ++slot) {
        if (ids[slot] == id)
            return static_cast<uint16_t>(slot);
    }
    return std::nullopt;
}

/// Appends a journal field name in lower case.
void set_field(JournalEntry& entry, std::string_view key) {
    entry.field = fold_copy(key);
}

/// Which collection a path belongs to, from the layout's directory names.
Collection collection_for_path(std::string_view path, const defs::DataLayout& layout) {
    std::string folded;
    folded.resize(path.size());
    for (std::size_t offset = 0; offset < path.size(); ++offset)
        folded[offset] = fold_path_byte(path[offset]);
    const auto slash = folded.find('/');
    const std::string_view first = slash == std::string::npos
                                       ? std::string_view{folded}
                                       : std::string_view{folded}.substr(0, slash);
    const std::string_view rest = slash == std::string::npos
                                      ? std::string_view{}
                                      : std::string_view{folded}.substr(slash + 1);
    if (first == k_features_folder)
        return Collection::features;
    const auto directory = [&](defs::DataDirectory which) {
        return same_text(first, layout.directories[static_cast<std::size_t>(which)]);
    };
    if (directory(defs::DataDirectory::units))
        return Collection::units;
    if (directory(defs::DataDirectory::weapons))
        return Collection::weapons;
    if (directory(defs::DataDirectory::guis))
        return Collection::menus;
    if (directory(defs::DataDirectory::download))
        return Collection::menus;
    // A language variant lives in "<gamedata>-<variant>/". The side loader
    // reads sidedata from that folder when the file is there, and the build
    // lists stay in the plain folder. Both are still game-data tables.
    const std::string gamedata_name =
        fold_copy(layout.directories[static_cast<std::size_t>(defs::DataDirectory::gamedata)]);
    const bool gamedata_variant = first.size() > gamedata_name.size() &&
                                  first.compare(0, gamedata_name.size(), gamedata_name) == 0 &&
                                  first[gamedata_name.size()] == '-';
    if (directory(defs::DataDirectory::gamedata) || gamedata_variant) {
        if (rest == "sidedata.tdf")
            return Collection::sides;
        return Collection::tables;
    }
    return Collection::other;
}

/// Finds a document by path. Returns size() when there is none.
std::size_t find_document(const rules::DocumentSet& documents, std::string_view path) {
    for (std::size_t slot = 0; slot < documents.size(); ++slot) {
        if (same_path(documents.at(slot).path, path))
            return slot;
    }
    return documents.size();
}

/// One change to a document: the mutator, and the phase it made it in.
struct Writer {
    uint16_t mutator{};
    Phase phase{};
};

/// Records that `mutator` changed the document at `path` in `phase`.
void note_writer(
    const rules::DocumentSet& documents,
    std::vector<std::vector<Writer>>& writers,
    std::vector<uint8_t>& menus,
    std::string_view path,
    uint16_t mutator,
    Phase phase,
    Collection collection
) {
    const std::size_t slot = find_document(documents, path);
    if (slot >= writers.size())
        return;
    writers[slot].push_back(Writer{mutator, phase});
    if (collection == Collection::menus)
        menus[slot] = 1;
}

/// The last mutator in list order that changed a document.
uint16_t owner_of(const std::vector<Writer>& changes) {
    uint16_t best = changes.front().mutator;
    for (const Writer& change : changes) {
        if (change.mutator > best)
            best = change.mutator;
    }
    return best;
}

/// The last phase in which `mutator` changed a document. Hooks come after rules.
Phase phase_of(const std::vector<Writer>& changes, uint16_t mutator) {
    Phase last = Phase::rules;
    for (const Writer& change : changes) {
        if (change.mutator == mutator &&
            static_cast<uint8_t>(change.phase) > static_cast<uint8_t>(last))
            last = change.phase;
    }
    return last;
}

/// One journal entry for a field write.
JournalEntry entry_from_write(
    const rules::FieldWrite& write,
    const std::vector<std::string>& ids,
    Phase phase,
    const defs::DataLayout& layout
) {
    JournalEntry entry;
    entry.phase = phase;
    entry.collection = collection_for_path(write.path, layout);
    entry.change = write.new_text.has_value() ? Change::set : Change::removed_field;
    entry.record = write.record;
    entry.block = write.block;
    set_field(entry, write.key);
    entry.old_text = write.old_text;
    entry.new_text = write.new_text;
    entry.path = write.path;
    for (const std::string& name : write.mutators) {
        const std::optional<uint16_t> slot = index_of(ids, name);
        if (slot.has_value())
            entry.mutators.push_back(*slot);
    }
    return entry;
}

/// A file added or hidden, in the phase that added or hid it.
JournalEntry file_entry(uint16_t mutator, std::string path, Change change, Phase phase) {
    JournalEntry entry;
    entry.mutators.push_back(mutator);
    entry.phase = phase;
    entry.collection = Collection::files;
    entry.change = change;
    entry.path = std::move(path);
    return entry;
}

/// The record kind a field check should use, or nullopt when the schema has none.
std::optional<fields::RecordKind>
record_kind(const rules::FieldRef& field, const defs::DataLayout& layout) {
    if (field.kind == rules::TargetKind::units)
        return fields::RecordKind::unit;
    if (field.kind == rules::TargetKind::weapons) {
        if (same_text(field.block, "damage"))
            return fields::RecordKind::weapon_damage;
        return fields::RecordKind::weapon;
    }
    const std::string folded = fold_copy(field.path);
    const auto slash = folded.rfind('/');
    const std::string_view file = slash == std::string::npos
                                      ? std::string_view{folded}
                                      : std::string_view{folded}.substr(slash + 1);
    if (file == "moveinfo.tdf") {
        const std::size_t gamedata = static_cast<std::size_t>(defs::DataDirectory::gamedata);
        const std::string folder = fold_copy(layout.directories[gamedata]);
        if (folded.starts_with(folder))
            return fields::RecordKind::move_class;
    }
    return std::nullopt;
}

/// True when `field` is the id of a weapon the game already numbered.
bool weapon_id_stays(const rules::FieldRef& field, const std::vector<std::string>& kept) {
    if (field.kind != rules::TargetKind::weapons || !field.block.empty())
        return false;
    if (!same_text(field.key, "id"))
        return false;
    const std::string folded = fold_copy(field.record);
    return std::binary_search(kept.begin(), kept.end(), folded);
}

/// Folded names of the weapon sections loaded before any mutator file runs.
std::vector<std::string>
kept_weapon_names(const rules::DocumentSet& documents, const defs::DataLayout& layout) {
    std::vector<std::string> names;
    const std::size_t weapons = static_cast<std::size_t>(defs::DataDirectory::weapons);
    const std::vector<std::size_t> docs =
        documents.documents_in(layout.directories[weapons], ".tdf", false);
    for (const std::size_t doc_index : docs) {
        const rules::LoadedDocument& loaded = documents.at(doc_index);
        const tdf::Block* root = loaded.document.get()->root;
        if (root == nullptr)
            continue;
        const uint32_t sections = tdf::child_count(root);
        for (uint32_t section_index = 0; section_index < sections; ++section_index) {
            const tdf::Block* section = tdf::child_at(root, section_index);
            if (section == nullptr || section->name == nullptr || section->name[0] == '\0')
                continue;
            names.push_back(fold_copy(section->name));
        }
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

/// A removal is accepted without a check. Known records go through the schema.
///
/// A write or a removal of `id` on a weapon the game already numbered is
/// refused. The weapon keeps the id it had when the documents were parsed.
/// A weapon `id` is held to `weapon_types`.
rules::CheckResult check_field(
    const rules::FieldRef& field,
    const std::optional<std::string>& text,
    const defs::DataLayout& layout,
    const std::vector<std::string>& kept_weapons,
    uint32_t weapon_types
) {
    rules::CheckResult result;
    if (weapon_id_stays(field, kept_weapons)) {
        result.outcome = rules::CheckOutcome::refuse;
        result.reason = "a base weapon keeps its id";
        return result;
    }
    if (!text.has_value()) {
        result.outcome = rules::CheckOutcome::ok;
        return result;
    }
    const std::optional<fields::RecordKind> kind = record_kind(field, layout);
    if (!kind.has_value()) {
        result.outcome = rules::CheckOutcome::ok;
        return result;
    }
    const fields::Check verdict = fields::check_text(*kind, field.key, *text, weapon_types);
    if (verdict.verdict == fields::Verdict::warn) {
        result.outcome = rules::CheckOutcome::warn;
        result.reason = verdict.reason;
        return result;
    }
    if (verdict.verdict == fields::Verdict::refuse) {
        result.outcome = rules::CheckOutcome::refuse;
        result.reason = verdict.reason;
        return result;
    }
    result.outcome = rules::CheckOutcome::ok;
    return result;
}

/// True when any sample of the target is a path a mutator may change.
bool target_allowed(
    const sm::Target& target,
    const defs::DataLayout& layout,
    const PathContext& paths,
    std::string& message
) {
    const std::string folder = resolve_folder(target.folder, layout);
    const std::string extension = resolve_extension(target.extension, layout);
    std::vector<std::string> candidates;
    if (extension.find('.') != std::string::npos)
        candidates.push_back(folder + "/" + extension);
    candidates.push_back(folder + "/item." + extension);
    message.clear();
    for (const std::string& candidate : candidates) {
        const PathVerdict verdict = check_mutable_path(candidate, paths);
        if (verdict.allowed())
            return true;
        if (message.empty())
            message = verdict.message;
    }
    return false;
}

/// Unit-file stems the store lists, folded, so a build picture can name them.
std::vector<std::string> unit_stems(const oa::AssetStore& store, const defs::DataLayout& layout) {
    const std::size_t units = static_cast<std::size_t>(defs::DataDirectory::units);
    std::string suffix = ".";
    suffix += fold_copy(layout.unit_extension);
    const std::vector<std::string> names =
        store.list_effective_in_mount_order(layout.directories[units], suffix, true);
    std::vector<std::string> stems;
    stems.reserve(names.size());
    for (const std::string& name : names) {
        const auto slash = name.rfind('/');
        const std::string_view file = slash == std::string::npos
                                          ? std::string_view{name}
                                          : std::string_view{name}.substr(slash + 1);
        if (file.size() <= suffix.size())
            continue;
        if (!same_text(file.substr(file.size() - suffix.size()), suffix))
            continue;
        stems.emplace_back(file.substr(0, file.size() - suffix.size()));
    }
    std::sort(stems.begin(), stems.end());
    stems.erase(std::unique(stems.begin(), stems.end()), stems.end());
    return stems;
}

/// Parses one document. A text that does not parse is a warning, not a
/// failure, and the warning names its path once however often it is listed.
void consider_document(
    rules::DocumentSet& documents,
    ConflictReport& report,
    const oa::AssetStore& store,
    std::string path
) {
    if (documents.find(path) != nullptr)
        return;
    for (const std::string& unread : report.unread) {
        if (same_path(unread, path))
            return;
    }
    try {
        const oa::AssetData data = store.read(path);
        const std::string text(data.bytes.begin(), data.bytes.end());
        std::string error;
        if (!documents.add(path, text, &error))
            report.unread.push_back(std::move(path));
    } catch (const std::exception&) {
        report.unread.push_back(std::move(path));
    }
}

/// Lists `directory` and parses each file whose name ends with `suffix`.
void consider_listed(
    rules::DocumentSet& documents,
    ConflictReport& report,
    const oa::AssetStore& store,
    std::string_view directory,
    std::string_view suffix,
    bool recursive
) {
    const std::vector<std::string> names =
        recursive ? store.list_effective_recursive(directory, suffix, true)
                  : store.list_effective_in_mount_order(directory, suffix, true);
    for (const std::string& name : names)
        consider_document(documents, report, store, name);
}

/// Reads sidedata and moveinfo through the variant path the game uses.
void consider_variant_file(
    rules::DocumentSet& documents,
    ConflictReport& report,
    oa::AssetStore& store,
    const defs::DataLayout& layout,
    const std::string& variant,
    const char* name
) {
    const defs::Files boundary = defs::asset_store_files(&store);
    std::array<char, defs::path_capacity> buffer{};
    const std::size_t gamedata = static_cast<std::size_t>(defs::DataDirectory::gamedata);
    const char* variant_word = variant.empty() ? nullptr : variant.c_str();
    defs::build_variant_path(
        &boundary,
        buffer.data(),
        buffer.size(),
        layout.directories[gamedata].c_str(),
        name,
        "tdf",
        variant_word
    );
    if (buffer[0] == '\0')
        return;
    consider_document(documents, report, store, std::string{buffer.data()});
}

/// Fills the document set from the mounted store, in listing order.
void parse_documents(
    rules::DocumentSet& documents,
    ConflictReport& report,
    oa::AssetStore& store,
    const defs::DataLayout& layout,
    const std::string& variant,
    const std::vector<rules::MutatorRules>& rule_sets
) {
    const std::size_t units = static_cast<std::size_t>(defs::DataDirectory::units);
    const std::size_t weapons = static_cast<std::size_t>(defs::DataDirectory::weapons);
    const std::size_t download = static_cast<std::size_t>(defs::DataDirectory::download);
    std::string unit_suffix = ".";
    unit_suffix += fold_copy(layout.unit_extension);
    consider_listed(documents, report, store, layout.directories[units], unit_suffix, false);
    consider_listed(documents, report, store, layout.directories[weapons], ".tdf", false);
    consider_listed(documents, report, store, k_features_folder, ".tdf", true);
    consider_listed(documents, report, store, layout.directories[download], ".tdf", false);
    consider_variant_file(documents, report, store, layout, variant, "sidedata");
    // Sides are read from the language variant when that file is present.
    // Build lists are read from the plain gamedata/sidedata.tdf, including
    // when a variant exists. Load the plain file as well so each write is
    // served at the path its reader opens. The second add is a no-op when
    // the variant file is absent, because that path is already the plain one.
    {
        const std::size_t gamedata = static_cast<std::size_t>(defs::DataDirectory::gamedata);
        consider_document(documents, report, store, layout.directories[gamedata] + "/sidedata.tdf");
    }
    consider_variant_file(documents, report, store, layout, variant, "moveinfo");
    const std::vector<sm::Target> targets = rules::targets_of(rule_sets);
    for (const sm::Target& target : targets) {
        const std::string folder = resolve_folder(target.folder, layout);
        const std::string extension = resolve_extension(target.extension, layout);
        std::string suffix = ".";
        suffix += fold_copy(extension);
        consider_listed(documents, report, store, folder, suffix, false);
    }
}

/// The folder a mask's files are listed from: its segments before the first
/// one that holds a wildcard, never including the last segment.
std::string mask_folder(std::string_view pattern) {
    std::string folder;
    std::size_t begin = 0;
    while (begin < pattern.size()) {
        const std::size_t slash = pattern.find_first_of("/\\", begin);
        if (slash == std::string_view::npos)
            break;
        const std::string_view segment = pattern.substr(begin, slash - begin);
        if (segment.find_first_of("*?") != std::string_view::npos)
            break;
        if (!folder.empty())
            folder.push_back('/');
        folder.append(segment.data(), segment.size());
        begin = slash + 1;
    }
    return folder;
}

/// One hidden entry for each file of the mod that a mask hides.
///
/// The store is read before the layer is mounted, so these are the mod's
/// own files. A file a mutator adds is not counted here: the layer serves
/// it over every mask, and a mask that hides another mutator's file is
/// journaled from the files layer's note. Each mutator names a path once.
std::vector<JournalEntry> mask_entries(
    const oa::AssetStore& store,
    const oa::MutatorLayerSpec& spec,
    const std::vector<std::string>& ids
) {
    std::vector<JournalEntry> entries;
    std::vector<std::string> folders;
    std::vector<std::vector<std::string>> listed;
    for (const oa::MutatorMask& mask : spec.masks) {
        const std::optional<uint16_t> owner = index_of(ids, mask.owner);
        if (!owner.has_value())
            continue;
        const std::string folder = mask_folder(mask.pattern);
        std::size_t known = 0;
        while (known < folders.size() && folders[known] != folder)
            ++known;
        if (known == folders.size()) {
            folders.push_back(folder);
            listed.push_back(store.list_effective_recursive(folder, "", true));
        }
        for (const std::string& name : listed[known]) {
            if (!oa::mutator_mask_matches(mask.pattern, name))
                continue;
            bool served = false;
            for (const oa::MutatorLayerEntry& entry : spec.entries) {
                if (same_path(entry.path, name)) {
                    served = true;
                    break;
                }
            }
            if (!served)
                entries.push_back(file_entry(*owner, name, Change::hidden, Phase::files));
        }
    }
    std::stable_sort(
        entries.begin(), entries.end(), [](const JournalEntry& left, const JournalEntry& right) {
            if (left.mutators.front() != right.mutators.front())
                return left.mutators.front() < right.mutators.front();
            return left.path < right.path;
        }
    );
    entries.erase(
        std::unique(
            entries.begin(),
            entries.end(),
            [](const JournalEntry& left, const JournalEntry& right) {
                return left.mutators.front() == right.mutators.front() && left.path == right.path;
            }
        ),
        entries.end()
    );
    return entries;
}

/// The position no failure has.
constexpr std::size_t k_no_slot = std::numeric_limits<std::size_t>::max();

/// The failure a pass keeps: the one nearest the top of the list.
///
/// A mutator's files, rules and hooks can depend on what the mutators above
/// it did, so only the first failure in list order is sure to be its own.
/// The pass keeps that one, and the next pass judges the rest without it.
struct FirstFailure {
    std::size_t slot{k_no_slot};
    Phase phase{};
    Cause cause{};
    std::string message;

    /// Keeps this failure when it is nearer the top than the one kept.
    void offer(std::size_t at, Phase in, Cause why, std::string text) {
        if (at >= slot)
            return;
        slot = at;
        phase = in;
        cause = why;
        message = std::move(text);
    }

    /// Reports whether the pass found a failure.
    [[nodiscard]] bool found() const noexcept { return slot != k_no_slot; }
};

/// Calls one hook. A script error, a spent allowance or a C++ exception fails
/// it, and the failure is offered as the pass's first.
bool call_hook(
    host::Sandbox& sandbox,
    Collections& collections,
    Menus& menus,
    int function_ref,
    HookWhich which,
    const ContextView& context,
    const oa::AssetStore& store,
    const defs::DataLayout& layout,
    const std::string& variant,
    std::size_t slot,
    FirstFailure& first
) {
    HookBox box;
    box.collections = &collections;
    box.menus = &menus;
    box.function_ref = function_ref;
    box.which = which;
    box.context = &context;
    box.store = &store;
    box.layout = &layout;
    box.variant = &variant;
    host::CallHooks hooks;
    hooks.context = &box;
    hooks.push = push_hook;
    host::ScriptError error;
    bool called = false;
    try {
        called = sandbox.call("hooks", hooks, error);
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& ex) {
        first.offer(
            slot,
            Phase::hooks,
            Cause::engine_error,
            ex.what() != nullptr ? ex.what() : "the hook stopped"
        );
        return false;
    }
    // The flag survives pcall, so a call that returned still ends the mutator.
    if (context.stop != nullptr && context.stop->raised) {
        first.offer(slot, Phase::hooks, Cause::declared, declared_reason(*context.stop));
        return false;
    }
    if (called)
        return true;
    first.offer(slot, Phase::hooks, cause_of(error.kind), error.message);
    return false;
}

/// Joins works-with ids for the incompatibility line.
std::string works_with_list(const oamut::Manifest& manifest) {
    std::string line;
    for (const oamut::Condition& condition : manifest.works_with) {
        if (!line.empty())
            line += ", ";
        line += condition.id;
    }
    if (line.empty())
        line = "nothing";
    return line;
}

/// True when "*" or a condition names this mod.
bool fits_mod(const oamut::Manifest& manifest, const ModIdentity& mod) {
    if (manifest.works_with.empty())
        return false;
    for (const oamut::Condition& condition : manifest.works_with) {
        if (condition.id == "*")
            return true;
        if (oamut::condition_met(condition, mod.id, mod.version))
            return true;
    }
    return false;
}

/// True when the engine requirement is empty or the engine meets it.
bool fits_engine(const oamut::Manifest& manifest, const std::string& engine_version) {
    if (manifest.requires_engine.empty())
        return true;
    const std::optional<oamut::VersionRange> range =
        oamut::parse_version_range(manifest.requires_engine, nullptr);
    const std::optional<oamut::Version> version = oamut::parse_version(engine_version);
    if (!range.has_value() || !version.has_value())
        return false;
    return oamut::version_in_range(*range, *version);
}

/// Drops mutators whose requirements are failed, incompatible or excluded.
void exclude_blocked(
    std::vector<MutatorResult>& results,
    const std::vector<PipelineMutator>& listed,
    const std::vector<std::string>& versions
) {
    bool changed = true;
    while (changed) {
        changed = false;
        for (std::size_t slot = 0; slot < listed.size(); ++slot) {
            if (results[slot].outcome != Outcome::applied)
                continue;
            for (const oamut::Condition& condition : listed[slot].manifest.requires_mutators) {
                for (std::size_t other = 0; other < listed.size(); ++other) {
                    if (other == slot)
                        continue;
                    if (!oamut::condition_met(condition, results[other].id, versions[other]))
                        continue;
                    if (results[other].outcome == Outcome::applied)
                        continue;
                    std::string message =
                        results[slot].id + " needs " + results[other].id + ", which ";
                    if (results[other].outcome == Outcome::incompatible)
                        message += "is incompatible";
                    else if (results[other].outcome == Outcome::failed)
                        message += "failed";
                    else
                        message += "did not apply";
                    exclude_mutator(results[slot], std::move(message));
                    changed = true;
                    break;
                }
                if (results[slot].outcome != Outcome::applied)
                    break;
            }
        }
    }
}

/// Refuses the whole list. Nothing is mounted and no script runs.
void refuse_list(PipelineResult& result, std::string refusal) {
    result.refusal = std::move(refusal);
    for (MutatorResult& mutator : result.mutators) {
        mutator.outcome = Outcome::excluded;
        mutator.phase = Phase::loadout;
        mutator.cause = Cause::none;
        mutator.message.clear();
        mutator.hacks.clear();
    }
}

/// Plans one mutator. A failure is recorded on `result` and the sandbox is dropped.
void plan_mutator(
    const PipelineInput& input,
    std::size_t slot,
    const defs::DataLayout& layout,
    Listing& listing,
    const std::vector<LoadoutOption>& options,
    std::optional<host::Sandbox>& sandbox,
    Plan& plan,
    MutatorResult& result
) {
    listing = list_package(input.mutators[slot].package);
    if (!listing.error.empty()) {
        const Cause cause = listing.error.find("link") != std::string::npos ? Cause::invalid_plan
                                                                            : Cause::engine_error;
        fail_mutator(result, Phase::plan, cause, listing.error);
        return;
    }
    host::Profile profile = host::mutator_profile();
    profile.budget = input.budget;
    host::OpenOptions options_for_open;
    options_for_open.package_id = result.id;
    options_for_open.profile = profile;
    options_for_open.sources.context = &listing;
    options_for_open.sources.read = source_read;
    options_for_open.random_seed = mutator_seed(input.seed, result.id);
    options_for_open.time_limit = input.time_limit;
    host::ScriptError error;
    std::optional<host::Sandbox> opened;
    try {
        opened = host::Sandbox::open(options_for_open, error);
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& ex) {
        fail_mutator(
            result,
            Phase::plan,
            Cause::engine_error,
            ex.what() != nullptr ? ex.what() : "open failed"
        );
        return;
    }
    if (!opened.has_value()) {
        fail_mutator(result, Phase::plan, cause_of(error.kind), error.message);
        return;
    }
    sandbox = std::move(*opened);
    if (!sandbox->run_host("plan", nullptr, install_forms, error)) {
        fail_mutator(result, Phase::plan, cause_of(error.kind), error.message);
        sandbox.reset();
        return;
    }
    const std::string& entry = input.mutators[slot].manifest.entry;
    if (!sandbox->run_file(entry, "plan", nullptr, error)) {
        fail_mutator(result, Phase::plan, cause_of(error.kind), error.message);
        sandbox.reset();
        return;
    }
    int found = 0;
    if (!sandbox->run_host("plan", &found, look_mutate, error)) {
        fail_mutator(result, Phase::plan, cause_of(error.kind), error.message);
        sandbox.reset();
        return;
    }
    if (found == 0) {
        fail_mutator(result, Phase::plan, Cause::invalid_plan, "the entry does not define mutate");
        sandbox.reset();
        return;
    }
    DeclaredStop stop;
    ContextView context;
    context.mod = &input.mod;
    context.engine = &input.engine_version;
    context.options = &options;
    context.stop = &stop;
    MutateBox box;
    box.context = &context;
    host::CallHooks hooks;
    hooks.context = &box;
    hooks.push = push_mutate;
    hooks.read = read_mutate;
    const bool called = sandbox->call("plan", hooks, error);
    if (stop.raised) {
        fail_mutator(result, Phase::plan, Cause::declared, declared_reason(stop));
        sandbox.reset();
        return;
    }
    if (!called) {
        fail_mutator(result, Phase::plan, cause_of(error.kind), error.message);
        sandbox.reset();
        return;
    }
    if (box.missing != 0) {
        fail_mutator(result, Phase::plan, Cause::invalid_plan, "mutate must return a table");
        sandbox.reset();
        return;
    }
    plan.rules.mutator = result.id;
    PlanFault fault;
    try {
        fault = read_returned_plan(
            sandbox->state(), box.shape, listing, entry, input.mutators[slot].manifest, layout, plan
        );
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& ex) {
        fail_mutator(
            result,
            Phase::plan,
            Cause::engine_error,
            ex.what() != nullptr ? ex.what() : "the plan could not be read"
        );
        sandbox.reset();
        return;
    }
    if (fault.failed) {
        fail_mutator(result, Phase::plan, fault.cause, std::move(fault.message));
        sandbox.reset();
        return;
    }
    result.hacks = plan.hacks;
}

/// True when `spec` already has an entry for `path`.
bool layer_has_path(const oa::MutatorLayerSpec& spec, std::string_view path) {
    for (const oa::MutatorLayerEntry& entry : spec.entries) {
        if (same_path(entry.path, path))
            return true;
    }
    return false;
}

/// True when the store, the documents or the layer already hold `path`.
bool served_path_exists(
    const oa::AssetStore& store,
    rules::DocumentSet& documents,
    const oa::MutatorLayerSpec& spec,
    std::string_view path
) {
    if (documents.find(path) != nullptr)
        return true;
    if (store.file_size(path) > 0)
        return true;
    return layer_has_path(spec, path);
}

/// A build page a unit clone may serve: `guis/<unit><digits>.gui` for a unit
/// cloned this pass, and only when that path is not already present, or
/// when `may_exist` says a Replace clone serves it in place of the replaced
/// unit's own page.
bool fresh_clone_page(
    std::string_view path,
    const Collections& collections,
    const defs::DataLayout& layout,
    const oa::AssetStore& store,
    rules::DocumentSet& documents,
    const oa::MutatorLayerSpec& spec,
    const PathVerdict& verdict,
    bool may_exist
) {
    if (verdict.rule != PathRule::menus_only)
        return false;
    const std::string& gui_folder =
        layout.directories[static_cast<std::size_t>(defs::DataDirectory::guis)];
    bool matches = false;
    for (const CloneRecord& record : collections.clones()) {
        if (record.weapon)
            continue;
        if (clone_build_page(path, gui_folder, record.name)) {
            matches = true;
            break;
        }
    }
    if (!matches) {
        for (const Collections::UnitFileShift& shift : collections.unit_file_shifts()) {
            if (clone_build_page(path, gui_folder, shift.stored)) {
                matches = true;
                break;
            }
        }
    }
    if (!matches)
        return false;
    return may_exist || !served_path_exists(store, documents, spec, path);
}

/// The step state's id for one hook write, folded the way a rule step folds it.
///
/// @param write the hook write
/// @return the id
rules::FieldId field_id_of(const rules::FieldWrite& write) {
    rules::FieldId id;
    id.path.resize(write.path.size());
    for (std::size_t offset = 0; offset < write.path.size(); ++offset)
        id.path[offset] = fold_path_byte(write.path[offset]);
    id.record = fold_copy(write.record);
    id.block = fold_copy(write.block);
    id.key = fold_copy(write.key);
    return id;
}

/// One replaced-value note: the field name and the two mutators.
struct ReplacedField {
    std::string field;
    uint16_t earlier{};
    uint16_t later{};

    /// Orders by the earlier mutator, then the later one, then the field.
    ///
    /// @param other the other note
    /// @return true when this note sorts first
    bool operator<(const ReplacedField& other) const {
        if (earlier != other.earlier)
            return earlier < other.earlier;
        if (later != other.later)
            return later < other.later;
        return field < other.field;
    }
};

/// Adds one rules note to the report.
///
/// A replaced value is kept once for each field name and pair of mutators.
/// Further records of that same replacement would flood a deep stack.
///
/// @param report the pass report
/// @param note the rules note
/// @param ids mutator ids, in list order
/// @param[in,out] replaced_fields the replacements already kept
void append_rule_note(
    ConflictReport& report,
    const rules::RuleNote& note,
    const std::vector<std::string>& ids,
    std::set<ReplacedField>& replaced_fields
) {
    Conflict conflict = conflict_from_rule(note, ids);
    if (conflict.kind == ConflictKind::value_replaced && conflict.mutators.size() >= 2) {
        ReplacedField key;
        key.field = conflict.field;
        key.earlier = conflict.mutators.front();
        key.later = conflict.mutators.back();
        if (!replaced_fields.insert(std::move(key)).second)
            return;
    }
    report.conflicts.push_back(std::move(conflict));
}

/// Adds one hook-replaced note, once per field name and pair of mutators.
///
/// @param report the pass report
/// @param ids mutator ids, in list order
/// @param[in,out] replaced the hook replacements already kept
/// @param earlier the mutator who last wrote the field, or who locked it
/// @param writer the hook's mutator
/// @param write the hook write
/// @param message the note's own sentence
void append_hook_replaced(
    ConflictReport& report,
    const std::vector<std::string>& ids,
    std::set<ReplacedField>& replaced,
    const std::string& earlier,
    const std::string& writer,
    const rules::FieldWrite& write,
    std::string message
) {
    const std::optional<uint16_t> earlier_slot = index_of(ids, earlier);
    const std::optional<uint16_t> later_slot = index_of(ids, writer);
    if (!earlier_slot.has_value() || !later_slot.has_value())
        return;
    ReplacedField key;
    key.field = fold_copy(write.key);
    key.earlier = *earlier_slot;
    key.later = *later_slot;
    if (!replaced.insert(std::move(key)).second)
        return;
    HookNote note;
    note.kind = HookNoteKind::hook_overwrote;
    note.mutators.push_back(earlier);
    note.mutators.push_back(writer);
    note.path = write.path;
    note.record = write.record;
    note.key = write.key;
    note.message = std::move(message);
    report.conflicts.push_back(conflict_from_hook(note, ids));
}

/// Records this mutator's new hook notes and writes.
///
/// The collections remember earlier hooks only. The note names the mutator
/// in the step state who last wrote the field, which may be a rule, and one
/// such note is kept for each field name and pair. A write over a lock keeps
/// the lock note, naming the locker, and does not add a second note for the
/// same pair. The step state remembers the hook as the last writer and keeps
/// the lock, so a later mutator's rules still leave the field alone.
///
/// @param collections the hooks' collections
/// @param[in,out] state lock marks and last writers
/// @param[in,out] report the pass report
/// @param ids mutator ids, in list order
/// @param slot this mutator's index
/// @param notes_before how many hook notes existed before this mutator
/// @param writes_before how many hook writes existed before this mutator
/// @param[in,out] hook_replaced the hook replacements already kept
void record_hook_turn(
    Collections& collections,
    rules::ApplyState& state,
    ConflictReport& report,
    const std::vector<std::string>& ids,
    std::size_t slot,
    std::size_t notes_before,
    std::size_t writes_before,
    std::set<ReplacedField>& hook_replaced
) {
    const std::span<const HookNote> notes = collections.notes();
    for (std::size_t note_index = notes_before; note_index < notes.size(); ++note_index) {
        // The collections' overwrite names the last hook, not a rule that
        // wrote the field afterwards. The write loop records that note.
        if (notes[note_index].kind == HookNoteKind::hook_overwrote)
            continue;
        report.conflicts.push_back(conflict_from_hook(notes[note_index], ids));
    }

    const std::span<const rules::FieldWrite> writes = collections.writes();
    for (std::size_t write_index = writes_before; write_index < writes.size(); ++write_index) {
        const rules::FieldWrite& write = writes[write_index];
        const rules::FieldId id = field_id_of(write);
        const std::string& writer = write.mutators.empty() ? ids[slot] : write.mutators.front();
        const auto found = state.writers.find(id);
        const bool other_wrote = found != state.writers.end() && found->second != writer;
        const std::string earlier = other_wrote ? found->second : std::string();
        state.writers.insert_or_assign(id, writer);
        const auto locker = state.locks.find(id);
        const bool locked_by_other = locker != state.locks.end() && locker->second != writer;
        const bool earlier_holds_lock = locked_by_other && earlier == locker->second;
        if (other_wrote && !earlier_holds_lock) {
            std::string message = writer;
            message += " replaced the value ";
            message += earlier;
            message += " wrote";
            append_hook_replaced(
                report, ids, hook_replaced, earlier, writer, write, std::move(message)
            );
        }
        if (!locked_by_other)
            continue;
        std::string message = writer;
        message += " wrote a field ";
        message += locker->second;
        message += " had locked";
        append_hook_replaced(
            report, ids, hook_replaced, locker->second, writer, write, std::move(message)
        );
    }
}

/// What a mutator's file did with one record.
enum class RecordFate : uint8_t {
    added,    ///< the name was new, and the record was written
    replaced, ///< the record replaced the one already there
    shared,   ///< the record matched the one already there, and was not written
    renamed,  ///< the record took a private name, and the existing one stayed
};

/// One record a mutator's file added, replaced, shared or renamed, so the
/// journal and the write-back can name the mutator that brought it.
struct RecordTouch {
    /// Document path the record lives in.
    std::string path;
    /// Index of the mutator whose file held it.
    uint16_t mutator{};
    /// Units, weapons or menus.
    Collection collection{};
    /// Whether the record was added, replaced, shared or renamed.
    RecordFate fate{RecordFate::added};
    /// Unit name, weapon section or download section, as it is stored.
    std::string record;
    /// The name the mutator wrote, when `fate` is `renamed`.
    std::string asked;
};

/// Warns that a record file did not parse, once per path.
void note_unread(ConflictReport& report, std::string path) {
    for (const std::string& unread : report.unread) {
        if (same_path(unread, path))
            return;
    }
    report.unread.push_back(std::move(path));
}

/// Rewrites '\\' as '/'.
std::string slashed(std::string_view text) {
    std::string out{text};
    for (char& character : out) {
        if (character == '\\')
            character = '/';
    }
    return out;
}

/// The stem of a unit file: the file name without the layout's unit extension.
std::string unit_stem_of(std::string_view resource, const defs::DataLayout& layout) {
    const auto slash = resource.find_last_of("/\\");
    const std::string_view file =
        slash == std::string_view::npos ? resource : resource.substr(slash + 1);
    const std::string suffix = "." + layout.unit_extension;
    if (file.size() <= suffix.size())
        return std::string{file};
    const std::string_view extension = file.substr(file.size() - suffix.size());
    if (!same_text(extension, suffix))
        return std::string{file};
    return std::string{file.substr(0, file.size() - suffix.size())};
}

/// UnitName, or the file stem when the section leaves the name out.
std::string
unit_name_of(const tdf::Block* section, std::string_view path, const defs::DataLayout& layout) {
    const char* written = section != nullptr ? tdf::find_value(section, "UnitName") : nullptr;
    if (written != nullptr && written[0] != '\0')
        return written;
    return unit_stem_of(path, layout);
}

/// Copies one block's keys onto another, in file order.
bool transfer_properties(
    tdf::Document* dest_doc, tdf::Block* dest, const tdf::Block* source, std::string& error
) {
    if (source == nullptr)
        return true;
    const uint32_t count = source->property_count;
    std::vector<uint32_t> places(count);
    for (uint32_t index = 0; index < count; ++index)
        places[index] = index;
    std::sort(places.begin(), places.end(), [&](uint32_t left, uint32_t right) {
        return source->properties[left].order < source->properties[right].order;
    });
    for (const uint32_t place : places) {
        const tdf::Property& property = source->properties[place];
        if (property.key == nullptr)
            continue;
        const char* value = property.value != nullptr ? property.value : "";
        const tdf::EditStatus status = tdf::set_value(dest_doc, dest, property.key, value);
        if (status != tdf::EditStatus::ok) {
            error = tdf::edit_status_message(status);
            return false;
        }
    }
    return true;
}

/// Copies one block's sections onto another, in file order.
bool transfer_children(
    tdf::Document* dest_doc, tdf::Block* dest, const tdf::Block* source, std::string& error
) {
    if (source == nullptr)
        return true;
    const uint32_t count = tdf::child_count(source);
    for (uint32_t index = 0; index < count; ++index) {
        const tdf::Block* child = tdf::child_at(source, index);
        if (child == nullptr || child->name == nullptr)
            continue;
        tdf::EditStatus status = tdf::EditStatus::ok;
        tdf::Block* made = tdf::add_child(dest_doc, dest, child->name, &status);
        if (made == nullptr) {
            error = tdf::edit_status_message(status);
            return false;
        }
        if (!transfer_properties(dest_doc, made, child, error) ||
            !transfer_children(dest_doc, made, child, error))
            return false;
    }
    return true;
}

/// Removes every key and child of a block. The block itself stays, so a
/// collection that already points at it still does.
bool empty_block(tdf::Document* document, tdf::Block* block, std::string& error) {
    if (block == nullptr)
        return true;
    const uint32_t children = tdf::child_count(block);
    for (uint32_t index = children; index > 0; --index) {
        const tdf::EditStatus status = tdf::remove_child(document, block, index - 1);
        if (status != tdf::EditStatus::ok) {
            error = tdf::edit_status_message(status);
            return false;
        }
    }
    std::vector<std::string> keys;
    const uint32_t props = tdf::property_count(block);
    keys.reserve(props);
    for (uint32_t index = 0; index < props; ++index) {
        const char* key = tdf::property_key_at(block, static_cast<int32_t>(index));
        if (key != nullptr)
            keys.emplace_back(key);
    }
    for (const std::string& key : keys) {
        const tdf::EditStatus status = tdf::remove_key(document, block, key);
        if (status != tdf::EditStatus::ok) {
            error = tdf::edit_status_message(status);
            return false;
        }
    }
    return true;
}

/// Puts `source` into `dest`, which is already the record of that name.
bool replace_block(
    tdf::Document* dest_doc, tdf::Block* dest, const tdf::Block* source, std::string& error
) {
    return empty_block(dest_doc, dest, error) &&
           transfer_properties(dest_doc, dest, source, error) &&
           transfer_children(dest_doc, dest, source, error);
}

/// The first UNITINFO of this unit name, and the path it lives in.
struct UnitAt {
    /// Document path. Empty when the unit is not loaded.
    std::string path;
    /// The UNITINFO block. Null when the unit is not loaded.
    tdf::Block* section{};
};

/// Finds a unit by its name, ignoring ASCII case.
UnitAt
find_unit(rules::DocumentSet& documents, const defs::DataLayout& layout, std::string_view name) {
    UnitAt found;
    const std::size_t units = static_cast<std::size_t>(defs::DataDirectory::units);
    const std::vector<std::size_t> docs =
        documents.documents_in(layout.directories[units], layout.unit_extension, false);
    for (const std::size_t doc_index : docs) {
        rules::LoadedDocument& loaded = documents.at(doc_index);
        tdf::Block* root = tdf::editable_root(loaded.document.get());
        if (root == nullptr)
            continue;
        const uint32_t sections = tdf::child_count(root);
        for (uint32_t section_index = 0; section_index < sections; ++section_index) {
            tdf::Block* block = tdf::editable_child(root, section_index);
            if (block == nullptr || block->name == nullptr)
                continue;
            if (tdf::compare_nocase(block->name, "UNITINFO") != 0)
                continue;
            const std::string unit_name = unit_name_of(block, loaded.path, layout);
            if (!same_text(unit_name, name))
                break;
            found.path = loaded.path;
            found.section = block;
            return found;
        }
    }
    return found;
}

/// The first weapon section of this name, and the path it lives in.
struct WeaponAt {
    /// Document path. Empty when the weapon is not loaded.
    std::string path;
    /// The section. Null when the weapon is not loaded.
    tdf::Block* section{};
};

/// Finds a weapon section by its name, ignoring ASCII case.
WeaponAt
find_weapon(rules::DocumentSet& documents, const defs::DataLayout& layout, std::string_view name) {
    WeaponAt found;
    const std::size_t weapons = static_cast<std::size_t>(defs::DataDirectory::weapons);
    const std::vector<std::size_t> docs =
        documents.documents_in(layout.directories[weapons], ".tdf", false);
    for (const std::size_t doc_index : docs) {
        rules::LoadedDocument& loaded = documents.at(doc_index);
        tdf::Block* root = tdf::editable_root(loaded.document.get());
        if (root == nullptr)
            continue;
        tdf::Block* section = tdf::editable_child(root, name);
        if (section == nullptr)
            continue;
        found.path = loaded.path;
        found.section = section;
        return found;
    }
    return found;
}

/// The index of a brought weapon, or brought.size() when the name is new.
std::size_t brought_index(const std::vector<BroughtWeapon>& brought, std::string_view name) {
    for (std::size_t index = 0; index < brought.size(); ++index) {
        if (same_text(brought[index].name, name))
            return index;
    }
    return brought.size();
}

/// Puts `text` into a document a hook had removed, and clears that mark.
///
/// The index forgets the document first, so the old sections are not left
/// pointing at a tree the parse frees. A unit file passes its own text. A
/// weapon or download file passes an empty document and then adds sections.
bool revive_document(
    rules::LoadedDocument& loaded,
    std::string_view text,
    Collections& collections,
    std::string& error
) {
    collections.release_document(loaded.path);
    tdf::ParseError parse_error;
    if (!loaded.document.parse(text, &parse_error)) {
        error = loaded.path;
        error.append(": ");
        error.append(tdf::describe(parse_error));
        return false;
    }
    loaded.removed = false;
    loaded.added = true;
    return true;
}

/// Opens `path` as an empty document when the set does not already hold it.
///
/// A document a hook removed is brought back empty, so a later file can
/// fill it. The path stays the one the set already holds.
rules::LoadedDocument* ensure_document(
    rules::DocumentSet& documents,
    const std::string& path,
    Collections& collections,
    std::string& error
) {
    if (rules::LoadedDocument* existing = documents.find(path)) {
        if (existing->removed && !revive_document(*existing, "\n", collections, error))
            return nullptr;
        return existing;
    }
    if (!documents.add(path, "\n", &error))
        return nullptr;
    rules::LoadedDocument* created = documents.find(path);
    if (created != nullptr)
        created->added = true;
    return created;
}

/// Remembers a record so the journal and the write-back name its mutator.
///
/// @param touches the pass's records
/// @param path the document path
/// @param mutator the mutator's index
/// @param collection units, weapons or menus
/// @param fate what the file did with the record
/// @param record the name as stored
/// @param asked the name the mutator wrote, when the record was renamed
void touch_record(
    std::vector<RecordTouch>& touches,
    std::string path,
    uint16_t mutator,
    Collection collection,
    RecordFate fate,
    std::string record,
    std::string asked = {}
) {
    RecordTouch touch;
    touch.path = std::move(path);
    touch.mutator = mutator;
    touch.collection = collection;
    touch.fate = fate;
    touch.record = std::move(record);
    touch.asked = std::move(asked);
    touches.push_back(std::move(touch));
}

/// Writes one field, or records why it could not.
bool write_field(
    tdf::Document* document,
    tdf::Block* block,
    std::string_view key,
    std::string_view value,
    std::string& error
) {
    const tdf::EditStatus status = tdf::set_value(document, block, key, value);
    if (status == tdf::EditStatus::ok)
        return true;
    error = tdf::edit_status_message(status);
    return false;
}

/// The first UNITINFO child, or null when the block has none.
tdf::Block* first_unit_info(tdf::Block* root) {
    if (root == nullptr)
        return nullptr;
    const uint32_t sections = tdf::child_count(root);
    for (uint32_t section_index = 0; section_index < sections; ++section_index) {
        tdf::Block* block = tdf::editable_child(root, section_index);
        if (block == nullptr || block->name == nullptr)
            continue;
        if (tdf::compare_nocase(block->name, "UNITINFO") != 0)
            continue;
        return block;
    }
    return nullptr;
}

/// The model a unit uses: Objectname, else UnitName, else the record name.
std::string unit_model_of(const tdf::Block* section, std::string_view record_name) {
    const char* object_name = section != nullptr ? tdf::find_value(section, "Objectname") : nullptr;
    if (object_name != nullptr && object_name[0] != '\0')
        return object_name;
    const char* unit_name = section != nullptr ? tdf::find_value(section, "UnitName") : nullptr;
    if (unit_name != nullptr && unit_name[0] != '\0')
        return unit_name;
    return std::string{record_name};
}

/// Sets UnitName on the first UNITINFO so a renamed file is not a second copy
/// of the name it was given.
bool set_stored_unit_name(
    rules::LoadedDocument& loaded, std::string_view unit_name, std::string& error
) {
    tdf::Block* info = first_unit_info(tdf::editable_root(loaded.document.get()));
    if (info == nullptr) {
        error = loaded.path + ": the unit has no UNITINFO";
        return false;
    }
    return write_field(loaded.document.get(), info, "UnitName", unit_name, error);
}

/// A path for a unit file. The incoming folder and extension stay when they
/// name the layout's unit directory, so `units/RGBOT.fbi` stores a renamed
/// unit as `units/RGBOT_2.fbi`.
std::string unit_store_path(
    std::string_view resource, std::string_view unit_name, const defs::DataLayout& layout
) {
    const std::string slashed_resource = slashed(resource);
    const std::size_t units = static_cast<std::size_t>(defs::DataDirectory::units);
    const auto slash = slashed_resource.find_last_of('/');
    const std::string_view folder = slash == std::string::npos
                                        ? std::string_view{}
                                        : std::string_view{slashed_resource}.substr(0, slash);
    const std::string_view file = slash == std::string::npos
                                      ? std::string_view{slashed_resource}
                                      : std::string_view{slashed_resource}.substr(slash + 1);
    const auto dot = file.find_last_of('.');
    const std::string_view extension =
        dot == std::string_view::npos ? std::string_view{} : file.substr(dot + 1);
    std::string path;
    if (same_text(folder, layout.directories[units]) &&
        same_text(extension, layout.unit_extension)) {
        path.assign(folder);
        path.push_back('/');
        path.append(unit_name);
        path.push_back('.');
        path.append(extension);
        return path;
    }
    path = layout.directories[units];
    path.push_back('/');
    path.append(unit_name);
    path.push_back('.');
    path.append(layout.unit_extension);
    return path;
}

/// Parses `text` into `path` and stores the unit under `unit_name`.
bool place_renamed_unit(
    rules::DocumentSet& documents,
    Collections& collections,
    const std::string& path,
    std::string_view text,
    std::string_view unit_name,
    std::string& error
) {
    if (rules::LoadedDocument* holding = documents.find(path)) {
        if (!holding->removed) {
            error = path + ": already holds a unit";
            return false;
        }
        if (!revive_document(*holding, text, collections, error))
            return false;
        return set_stored_unit_name(*holding, unit_name, error);
    }
    if (!documents.add(path, text, &error))
        return false;
    rules::LoadedDocument* created = documents.find(path);
    if (created == nullptr) {
        error = path + ": the unit could not be added";
        return false;
    }
    created->added = true;
    return set_stored_unit_name(*created, unit_name, error);
}

/// Points weapon fields on this block, and its children, at private names
/// this mutator assigned.
bool rewire_weapon_fields(
    tdf::Document* document, tdf::Block* block, Collections& collections, std::string& error
) {
    if (block == nullptr)
        return true;
    const uint32_t props = tdf::property_count(block);
    std::vector<std::pair<std::string, std::string>> fields;
    fields.reserve(props);
    for (uint32_t prop_index = 0; prop_index < props; ++prop_index) {
        const char* key = tdf::property_key_at(block, static_cast<int32_t>(prop_index));
        if (key == nullptr || !weapon_reference_key(key))
            continue;
        const char* value = tdf::find_value(block, key);
        if (value == nullptr)
            continue;
        const std::string mapped = collections.private_name_of(true, value);
        if (mapped.empty())
            continue;
        fields.emplace_back(key, mapped);
    }
    for (const std::pair<std::string, std::string>& field : fields) {
        if (!write_field(document, block, field.first, field.second, error))
            return false;
    }
    const uint32_t children = tdf::child_count(block);
    for (uint32_t child_index = 0; child_index < children; ++child_index) {
        if (!rewire_weapon_fields(
                document, tdf::editable_child(block, child_index), collections, error
            ))
            return false;
    }
    return true;
}

/// One field of a block, or an empty string when the key is absent.
std::string block_field(const tdf::Block* block, const char* key) {
    const char* value = block != nullptr ? tdf::find_value(block, key) : nullptr;
    if (value == nullptr)
        return {};
    return std::string{value};
}

/// A download entry found by its builder and unit.
struct MenuAt {
    /// Document path. Empty when no entry matches.
    std::string path;
    /// Section name. Empty when no entry matches.
    std::string name;
    /// The section. Null when no entry matches.
    tdf::Block* section{};
};

/// The first download entry whose builder and unit match, ignoring ASCII case.
///
/// An entry with an empty builder or an empty unit is not a button. A
/// document a hook removed is not searched.
MenuAt find_menu_button(
    rules::DocumentSet& documents,
    const defs::DataLayout& layout,
    std::string_view builder,
    std::string_view unit
) {
    MenuAt found;
    if (builder.empty() || unit.empty())
        return found;
    const std::size_t download = static_cast<std::size_t>(defs::DataDirectory::download);
    const std::vector<std::size_t> docs =
        documents.documents_in(layout.directories[download], ".tdf", false);
    for (const std::size_t doc_index : docs) {
        rules::LoadedDocument& loaded = documents.at(doc_index);
        tdf::Block* root = tdf::editable_root(loaded.document.get());
        if (root == nullptr)
            continue;
        const uint32_t sections = tdf::child_count(root);
        for (uint32_t section_index = 0; section_index < sections; ++section_index) {
            tdf::Block* block = tdf::editable_child(root, section_index);
            if (block == nullptr || block->name == nullptr || block->name[0] == '\0')
                continue;
            if (!same_text(block_field(block, "UNITMENU"), builder) ||
                !same_text(block_field(block, "UNITNAME"), unit))
                continue;
            found.path = loaded.path;
            found.name = block->name;
            found.section = block;
            return found;
        }
    }
    return found;
}

/// True when some download file's first section marks `unit` downloadable.
bool unit_is_downloadable(
    rules::DocumentSet& documents, const defs::DataLayout& layout, std::string_view unit
) {
    if (unit.empty())
        return false;
    const std::size_t download = static_cast<std::size_t>(defs::DataDirectory::download);
    const std::vector<std::size_t> docs =
        documents.documents_in(layout.directories[download], ".tdf", false);
    for (const std::size_t doc_index : docs) {
        const tdf::Block* root = tdf::editable_root(documents.at(doc_index).document.get());
        const tdf::Block* first = root != nullptr ? tdf::child_at(root, 0) : nullptr;
        if (first == nullptr)
            continue;
        if (!same_text(block_field(first, "UNITNAME"), unit))
            continue;
        if (block_field(first, "UNITMENU").empty())
            continue;
        return true;
    }
    return false;
}

/// `download/<unit>.tdf`, or `download/<unit>-<k>.tdf` with the first free k from 2.
std::string fresh_download_path(
    rules::DocumentSet& documents, const defs::DataLayout& layout, std::string_view unit
) {
    std::string base = layout.directories[static_cast<std::size_t>(defs::DataDirectory::download)];
    base.push_back('/');
    base.append(unit);
    for (int suffix = 1; suffix < 10000; ++suffix) {
        std::string candidate = base;
        if (suffix > 1) {
            candidate.push_back('-');
            candidate.append(std::to_string(suffix));
        }
        candidate.append(".tdf");
        const rules::LoadedDocument* holding = documents.find(candidate);
        if (holding != nullptr && !holding->removed)
            continue;
        return candidate;
    }
    return {};
}

/// A placeholder section. An empty UNITMENU marks nobody downloadable.
bool write_menu_placeholder(rules::LoadedDocument& loaded, tdf::Block* root, std::string& error) {
    std::string stored = "PLACEHOLDER";
    if (tdf::editable_child(root, stored) != nullptr) {
        const auto taken = [&](std::string_view candidate) {
            if (!record_name_ok(candidate))
                return true;
            return tdf::editable_child(root, std::string{candidate}) != nullptr;
        };
        const std::optional<std::string> renamed = private_record_name(stored, taken);
        if (!renamed.has_value()) {
            error = stored + ": no free name";
            return false;
        }
        stored = *renamed;
    }
    tdf::EditStatus status = tdf::EditStatus::ok;
    tdf::Block* made = tdf::add_child(loaded.document.get(), root, stored, &status);
    if (made == nullptr) {
        error = status == tdf::EditStatus::ok ? "the download entry could not be added"
                                              : tdf::edit_status_message(status);
        return false;
    }
    return write_field(loaded.document.get(), made, "UNITMENU", "", error) &&
           write_field(loaded.document.get(), made, "MENU", "0", error) &&
           write_field(loaded.document.get(), made, "BUTTON", "0", error) &&
           write_field(loaded.document.get(), made, "UNITNAME", "", error);
}

/// The download file a new button is appended to.
///
/// The loader reads five sections of a file. A file already at that cap
/// spills. A spill file this run opened for the same unit is reused while
/// it still has room. A new spill file starts with a placeholder when the
/// unit is not already downloadable, so the loader marks nobody.
rules::LoadedDocument* menu_destination(
    rules::DocumentSet& documents,
    const defs::DataLayout& layout,
    const std::string& path,
    std::string_view unit,
    Collections& collections,
    std::string& error
) {
    rules::LoadedDocument* loaded = ensure_document(documents, path, collections, error);
    if (loaded == nullptr)
        return nullptr;
    tdf::Block* root = tdf::editable_root(loaded->document.get());
    if (root == nullptr) {
        error = path + ": the download file has no root";
        return nullptr;
    }
    if (tdf::child_count(root) < defs::download_menu_entries)
        return loaded;
    for (const Collections::MenuSpill& spill : collections.menu_spills()) {
        if (!same_text(spill.unit, unit))
            continue;
        rules::LoadedDocument* earlier = documents.find(spill.path);
        if (earlier == nullptr || earlier->removed)
            continue;
        tdf::Block* spill_root = tdf::editable_root(earlier->document.get());
        if (spill_root == nullptr)
            continue;
        if (tdf::child_count(spill_root) < defs::download_menu_entries)
            return earlier;
    }
    const std::string spill_unit = unit.empty() ? std::string{"entry"} : std::string{unit};
    const std::string spill_path = fresh_download_path(documents, layout, spill_unit);
    if (spill_path.empty()) {
        error = spill_unit + ": no free download file";
        return nullptr;
    }
    const bool marked = unit_is_downloadable(documents, layout, unit);
    rules::LoadedDocument* created = ensure_document(documents, spill_path, collections, error);
    if (created == nullptr)
        return nullptr;
    tdf::Block* created_root = tdf::editable_root(created->document.get());
    if (created_root == nullptr) {
        error = spill_path + ": the download file has no root";
        return nullptr;
    }
    if (!marked && !write_menu_placeholder(*created, created_root, error))
        return nullptr;
    collections.note_menu_spill(spill_unit, created->path);
    return created;
}

/// Reads one unit FBI into the document set. No UNITINFO is left unread.
///
/// Replace writes over the unit of the same name. Add shares a record that
/// matches and stores a different one under a private name, leaving the
/// existing unit where it was. The display name is not changed.
bool apply_unit_file(
    rules::DocumentSet& documents,
    ConflictReport& report,
    const defs::DataLayout& layout,
    const RecordFile& record,
    std::string_view text,
    const tdf::Block* root,
    uint16_t mutator,
    std::vector<RecordTouch>& touches,
    Collections& collections,
    std::string& error
) {
    const tdf::Block* info = nullptr;
    const uint32_t sections = tdf::child_count(root);
    for (uint32_t index = 0; index < sections; ++index) {
        const tdf::Block* block = tdf::child_at(root, index);
        if (block == nullptr || block->name == nullptr)
            continue;
        if (tdf::compare_nocase(block->name, "UNITINFO") != 0)
            continue;
        info = block;
        break;
    }
    if (info == nullptr) {
        note_unread(report, slashed(record.path));
        return true;
    }
    const std::string name = unit_name_of(info, record.path, layout);
    if (name.empty()) {
        note_unread(report, slashed(record.path));
        return true;
    }
    const bool adding = collections.clash_mode() == oamut::ClashMode::add;
    const UnitAt existing = find_unit(documents, layout, name);
    const RecordFieldMap map_weapons = [&](std::string_view key, std::string_view value) {
        if (!weapon_reference_key(key))
            return std::string{value};
        const std::string mapped = collections.private_name_of(true, value);
        if (mapped.empty())
            return std::string{value};
        return mapped;
    };
    if (existing.section != nullptr) {
        if (adding && records_identical(existing.section, info, false, &map_weapons)) {
            touch_record(
                touches, existing.path, mutator, Collection::units, RecordFate::shared, name
            );
            return true;
        }
        if (adding) {
            const auto taken = [&](std::string_view candidate) {
                if (!record_name_ok(candidate))
                    return true;
                if (find_unit(documents, layout, candidate).section != nullptr)
                    return true;
                const std::string candidate_path = unit_store_path(record.path, candidate, layout);
                const rules::LoadedDocument* holding = documents.find(candidate_path);
                return holding != nullptr && !holding->removed;
            };
            const std::optional<std::string> renamed = private_record_name(name, taken);
            if (!renamed.has_value()) {
                error = name + ": no free name";
                return false;
            }
            const std::string path = unit_store_path(record.path, *renamed, layout);
            if (!place_renamed_unit(documents, collections, path, text, *renamed, error))
                return false;
            const rules::LoadedDocument* placed = documents.find(path);
            const std::string stored_path = placed != nullptr ? placed->path : path;
            const char* object_name = tdf::find_value(info, "Objectname");
            const bool omitted = object_name == nullptr || object_name[0] == '\0';
            const std::string model = unit_model_of(info, name);
            const bool move_model = same_text(model, name);
            if (move_model || omitted) {
                rules::LoadedDocument* placed_doc = documents.find(stored_path);
                tdf::Block* placed_info =
                    placed_doc != nullptr
                        ? first_unit_info(tdf::editable_root(placed_doc->document.get()))
                        : nullptr;
                const std::string written = move_model ? *renamed : model;
                if (placed_doc == nullptr || placed_info == nullptr ||
                    !write_field(
                        placed_doc->document.get(), placed_info, "Objectname", written, error
                    )) {
                    if (error.empty())
                        error = stored_path + ": the unit disappeared";
                    return false;
                }
            }
            Collections::UnitFileShift shift;
            shift.mutator = mutator;
            shift.asked = name;
            shift.stored = *renamed;
            shift.move_model = move_model;
            collections.note_unit_file_shift(std::move(shift));
            collections.note_rename(false, name, *renamed);
            touch_record(
                touches,
                stored_path,
                mutator,
                Collection::units,
                RecordFate::renamed,
                *renamed,
                name
            );
            return true;
        }
        rules::LoadedDocument* loaded = documents.find(existing.path);
        if (loaded == nullptr) {
            error = existing.path + ": the unit disappeared";
            return false;
        }
        if (!replace_block(loaded->document.get(), existing.section, info, error))
            return false;
        touch_record(touches, loaded->path, mutator, Collection::units, RecordFate::replaced, name);
        return true;
    }
    std::string path = slashed(record.path);
    if (!same_text(unit_stem_of(record.path, layout), name)) {
        path = layout.directories[static_cast<std::size_t>(defs::DataDirectory::units)];
        path.push_back('/');
        path.append(name);
        path.push_back('.');
        path.append(layout.unit_extension);
    }
    if (rules::LoadedDocument* holding = documents.find(path)) {
        if (!holding->removed) {
            error = path + ": already holds a unit";
            return false;
        }
        if (!revive_document(*holding, text, collections, error))
            return false;
        touch_record(touches, holding->path, mutator, Collection::units, RecordFate::added, name);
        return true;
    }
    if (!documents.add(path, text, &error))
        return false;
    rules::LoadedDocument* created = documents.find(path);
    if (created == nullptr) {
        error = path + ": the unit could not be added";
        return false;
    }
    created->added = true;
    touch_record(touches, created->path, mutator, Collection::units, RecordFate::added, name);
    return true;
}

/// Appends one weapon section to the mutator's weapon file and remembers it.
bool append_weapon_section(
    rules::DocumentSet& documents,
    Collections& collections,
    const std::string& path,
    std::string_view section_name,
    const tdf::Block* block,
    uint16_t mutator,
    std::vector<BroughtWeapon>& brought,
    std::vector<RecordTouch>& touches,
    RecordFate fate,
    std::string asked,
    std::string& error
) {
    rules::LoadedDocument* loaded = ensure_document(documents, path, collections, error);
    if (loaded == nullptr)
        return false;
    tdf::Block* dest_root = tdf::editable_root(loaded->document.get());
    tdf::EditStatus status = tdf::EditStatus::ok;
    tdf::Block* made =
        dest_root != nullptr
            ? tdf::add_child(loaded->document.get(), dest_root, section_name, &status)
            : nullptr;
    if (made == nullptr) {
        error = status == tdf::EditStatus::ok ? "the weapon could not be added"
                                              : tdf::edit_status_message(status);
        return false;
    }
    if (!transfer_properties(loaded->document.get(), made, block, error) ||
        !transfer_children(loaded->document.get(), made, block, error))
        return false;
    BroughtWeapon weapon;
    weapon.name = std::string{section_name};
    weapon.mutator = mutator;
    weapon.path = loaded->path;
    brought.push_back(std::move(weapon));
    touch_record(
        touches,
        loaded->path,
        mutator,
        Collection::weapons,
        fate,
        std::string{section_name},
        std::move(asked)
    );
    return true;
}

/// Reads each weapon section.
///
/// Replace writes over the section of the same name. A section the mod
/// already had keeps the mod's id. A section a mutator already brought is
/// replaced and the later mutator owns the request. Add shares a section
/// that matches and stores a different one under a private name on this
/// mutator's file. A new name is appended. The whole file is added only when
/// none of its names already exist.
bool apply_weapon_file(
    rules::DocumentSet& documents,
    const defs::DataLayout& layout,
    const RecordFile& record,
    std::string_view text,
    const tdf::Block* root,
    uint16_t mutator,
    std::vector<BroughtWeapon>& brought,
    std::vector<RecordTouch>& touches,
    Collections& collections,
    std::string& error
) {
    const uint32_t sections = tdf::child_count(root);
    bool clash = documents.find(record.path) != nullptr;
    std::vector<std::string> seen;
    for (uint32_t index = 0; index < sections; ++index) {
        const tdf::Block* block = tdf::child_at(root, index);
        if (block == nullptr || block->name == nullptr || block->name[0] == '\0')
            continue;
        for (const std::string& earlier : seen) {
            if (same_text(earlier, block->name))
                clash = true;
        }
        seen.emplace_back(block->name);
        if (find_weapon(documents, layout, block->name).section != nullptr)
            clash = true;
    }
    const std::string path = slashed(record.path);
    const bool adding = collections.clash_mode() == oamut::ClashMode::add;
    if (!clash) {
        if (!documents.add(path, text, &error))
            return false;
        rules::LoadedDocument* created = documents.find(path);
        if (created == nullptr) {
            error = path + ": the weapons could not be added";
            return false;
        }
        created->added = true;
        for (const std::string& name : seen) {
            BroughtWeapon weapon;
            weapon.name = name;
            weapon.mutator = mutator;
            weapon.path = created->path;
            brought.push_back(std::move(weapon));
            touch_record(
                touches, created->path, mutator, Collection::weapons, RecordFate::added, name
            );
        }
        return true;
    }

    for (uint32_t index = 0; index < sections; ++index) {
        const tdf::Block* block = tdf::child_at(root, index);
        if (block == nullptr || block->name == nullptr || block->name[0] == '\0')
            continue;
        const std::string name = block->name;
        const WeaponAt present = find_weapon(documents, layout, name);
        if (present.section != nullptr && adding) {
            if (records_identical(present.section, block, true)) {
                touch_record(
                    touches, present.path, mutator, Collection::weapons, RecordFate::shared, name
                );
                continue;
            }
            const auto taken = [&](std::string_view candidate) {
                if (!record_name_ok(candidate))
                    return true;
                return find_weapon(documents, layout, candidate).section != nullptr;
            };
            const std::optional<std::string> renamed = private_record_name(name, taken);
            if (!renamed.has_value()) {
                error = name + ": no free name";
                return false;
            }
            collections.note_rename(true, name, *renamed);
            if (!append_weapon_section(
                    documents,
                    collections,
                    path,
                    *renamed,
                    block,
                    mutator,
                    brought,
                    touches,
                    RecordFate::renamed,
                    name,
                    error
                ))
                return false;
            continue;
        }
        const std::size_t owned = brought_index(brought, name);
        if (owned < brought.size()) {
            rules::LoadedDocument* loaded = documents.find(brought[owned].path);
            tdf::Block* section =
                loaded != nullptr
                    ? tdf::editable_child(tdf::editable_root(loaded->document.get()), name)
                    : nullptr;
            if (loaded == nullptr || section == nullptr) {
                error = brought[owned].path + ": the weapon disappeared";
                return false;
            }
            if (!replace_block(loaded->document.get(), section, block, error))
                return false;
            brought[owned].mutator = mutator;
            brought[owned].path = loaded->path;
            brought[owned].name = name;
            touch_record(
                touches, loaded->path, mutator, Collection::weapons, RecordFate::replaced, name
            );
            continue;
        }
        const WeaponAt existing = find_weapon(documents, layout, name);
        if (existing.section != nullptr) {
            rules::LoadedDocument* loaded = documents.find(existing.path);
            if (loaded == nullptr) {
                error = existing.path + ": the weapon disappeared";
                return false;
            }
            const char* saved_id = tdf::find_value(existing.section, "ID");
            const bool had_id = saved_id != nullptr;
            const std::string kept_id = had_id ? std::string{saved_id} : std::string{};
            if (!replace_block(loaded->document.get(), existing.section, block, error))
                return false;
            if (had_id) {
                const char* now = tdf::find_value(existing.section, "ID");
                if (now == nullptr || std::string_view{now} != kept_id) {
                    const tdf::EditStatus status =
                        tdf::set_value(loaded->document.get(), existing.section, "ID", kept_id);
                    if (status != tdf::EditStatus::ok) {
                        error = tdf::edit_status_message(status);
                        return false;
                    }
                }
            } else if (
                tdf::remove_key(loaded->document.get(), existing.section, "ID") !=
                tdf::EditStatus::ok
            ) {
                error = "the weapon id could not be restored";
                return false;
            }
            touch_record(
                touches, loaded->path, mutator, Collection::weapons, RecordFate::replaced, name
            );
            continue;
        }
        if (!append_weapon_section(
                documents,
                collections,
                path,
                name,
                block,
                mutator,
                brought,
                touches,
                RecordFate::added,
                {},
                error
            ))
            return false;
    }
    return true;
}

/// Reads each download section. An entry is its builder and its unit, not its
/// section name. The same button is replaced in place and keeps the section
/// name already stored. A unit or builder this mutator renamed is written
/// under the private name before the button is matched, so it does not
/// replace the original unit's button or land on the original builder. A
/// new button whose section name is already used in that destination file
/// takes a private section name. A button past the file's fifth section
/// spills to another download file.
bool apply_download_file(
    rules::DocumentSet& documents,
    ConflictReport& report,
    const defs::DataLayout& layout,
    const RecordFile& record,
    const tdf::Block* root,
    uint16_t mutator,
    std::vector<RecordTouch>& touches,
    Collections& collections,
    std::string& error
) {
    (void)report;
    const std::string path = slashed(record.path);
    const uint32_t sections = tdf::child_count(root);
    for (uint32_t section_index = 0; section_index < sections; ++section_index) {
        const tdf::Block* block = tdf::child_at(root, section_index);
        if (block == nullptr || block->name == nullptr || block->name[0] == '\0')
            continue;
        std::string builder = block_field(block, "UNITMENU");
        const std::string mapped_builder = collections.private_name_of(false, builder);
        if (!mapped_builder.empty())
            builder = mapped_builder;
        std::string unit = block_field(block, "UNITNAME");
        const std::string mapped = collections.private_name_of(false, unit);
        if (!mapped.empty())
            unit = mapped;
        const MenuAt button = find_menu_button(documents, layout, builder, unit);
        if (button.section != nullptr) {
            rules::LoadedDocument* loaded = documents.find(button.path);
            if (loaded == nullptr) {
                error = button.path + ": the download file disappeared";
                return false;
            }
            if (!replace_block(loaded->document.get(), button.section, block, error))
                return false;
            if (!mapped_builder.empty() &&
                !write_field(
                    loaded->document.get(), button.section, "UNITMENU", mapped_builder, error
                ))
                return false;
            if (!mapped.empty() &&
                !write_field(loaded->document.get(), button.section, "UNITNAME", mapped, error))
                return false;
            touch_record(
                touches, loaded->path, mutator, Collection::menus, RecordFate::replaced, button.name
            );
            continue;
        }
        rules::LoadedDocument* loaded =
            menu_destination(documents, layout, path, unit, collections, error);
        if (loaded == nullptr)
            return false;
        tdf::Block* dest_root = tdf::editable_root(loaded->document.get());
        if (dest_root == nullptr) {
            error = loaded->path + ": the download file has no root";
            return false;
        }
        std::string stored = block->name;
        std::string asked;
        RecordFate fate = RecordFate::added;
        if (tdf::editable_child(dest_root, stored) != nullptr) {
            const auto taken = [&](std::string_view candidate) {
                if (!record_name_ok(candidate))
                    return true;
                return tdf::editable_child(dest_root, candidate) != nullptr;
            };
            const std::optional<std::string> renamed = private_record_name(stored, taken);
            if (!renamed.has_value()) {
                error = stored + ": no free name";
                return false;
            }
            asked = stored;
            stored = *renamed;
            fate = RecordFate::renamed;
        }
        tdf::EditStatus status = tdf::EditStatus::ok;
        tdf::Block* made = tdf::add_child(loaded->document.get(), dest_root, stored, &status);
        if (made == nullptr) {
            error = status == tdf::EditStatus::ok ? "the download entry could not be added"
                                                  : tdf::edit_status_message(status);
            return false;
        }
        if (!transfer_properties(loaded->document.get(), made, block, error) ||
            !transfer_children(loaded->document.get(), made, block, error))
            return false;
        if (!mapped_builder.empty() &&
            !write_field(loaded->document.get(), made, "UNITMENU", mapped_builder, error))
            return false;
        if (!mapped.empty() &&
            !write_field(loaded->document.get(), made, "UNITNAME", mapped, error))
            return false;
        touch_record(
            touches, loaded->path, mutator, Collection::menus, fate, stored, std::move(asked)
        );
    }
    return true;
}

/// Reads one record file. A text that does not parse is unread, not a failure.
bool apply_record_file(
    rules::DocumentSet& documents,
    ConflictReport& report,
    const defs::DataLayout& layout,
    const RecordFile& record,
    uint16_t mutator,
    std::vector<BroughtWeapon>& brought,
    std::vector<RecordTouch>& touches,
    Collections& collections,
    std::string& error
) {
    if (record.bytes == nullptr) {
        note_unread(report, slashed(record.path));
        return true;
    }
    const std::string text(record.bytes->begin(), record.bytes->end());
    tdf::OwnedDocument parsed;
    tdf::ParseError parse_error;
    if (!parsed.parse(text, &parse_error)) {
        note_unread(report, slashed(record.path));
        return true;
    }
    const tdf::Block* root = parsed.root();
    if (root == nullptr) {
        note_unread(report, slashed(record.path));
        return true;
    }
    const RecordFileKind kind = record_file_kind(record.path, layout);
    if (kind == RecordFileKind::unit)
        return apply_unit_file(
            documents, report, layout, record, text, root, mutator, touches, collections, error
        );
    if (kind == RecordFileKind::weapon)
        return apply_weapon_file(
            documents, layout, record, text, root, mutator, brought, touches, collections, error
        );
    if (kind == RecordFileKind::download)
        return apply_download_file(
            documents, report, layout, record, root, mutator, touches, collections, error
        );
    return true;
}

/// Applies the record files of one kind that belong to this mutator.
bool apply_kind(
    rules::DocumentSet& documents,
    ConflictReport& report,
    const defs::DataLayout& layout,
    const std::vector<RecordFile>& records,
    std::string_view mutator_id,
    uint16_t mutator,
    RecordFileKind kind,
    std::vector<BroughtWeapon>& brought,
    std::vector<RecordTouch>& touches,
    Collections& collections,
    std::string& error
) {
    for (const RecordFile& record : records) {
        if (record.mutator != mutator_id)
            continue;
        if (record_file_kind(record.path, layout) != kind)
            continue;
        if (!apply_record_file(
                documents, report, layout, record, mutator, brought, touches, collections, error
            ))
            return false;
    }
    return true;
}

/// Points weapon fields on units this mutator brought at weapons it renamed.
///
/// A shared unit is the record already there, so its fields stay.
bool rewire_brought_units(
    rules::DocumentSet& documents,
    const defs::DataLayout& layout,
    uint16_t mutator,
    const std::vector<RecordTouch>& touches,
    Collections& collections,
    std::string& error
) {
    for (const RecordTouch& touch : touches) {
        if (touch.mutator != mutator || touch.collection != Collection::units)
            continue;
        if (touch.fate == RecordFate::shared)
            continue;
        const UnitAt unit = find_unit(documents, layout, touch.record);
        if (unit.section == nullptr) {
            error = touch.path + ": " + touch.record + " disappeared";
            return false;
        }
        rules::LoadedDocument* loaded = documents.find(unit.path);
        if (loaded == nullptr) {
            error = unit.path + ": the unit disappeared";
            return false;
        }
        if (!rewire_weapon_fields(loaded->document.get(), unit.section, collections, error))
            return false;
    }
    return true;
}

/// Applies one mutator's record snapshots. Weapons land before units, so a
/// renamed weapon is known when that mutator's units are rewired. Download
/// entries are last, so a renamed unit's button uses the private name.
/// Other mutators' files wait.
bool apply_mutator_records(
    rules::DocumentSet& documents,
    ConflictReport& report,
    const defs::DataLayout& layout,
    const std::vector<RecordFile>& records,
    std::string_view mutator_id,
    uint16_t mutator,
    std::vector<BroughtWeapon>& brought,
    std::vector<RecordTouch>& touches,
    Collections& collections,
    std::string& error
) {
    const std::size_t before = touches.size();
    if (!apply_kind(
            documents,
            report,
            layout,
            records,
            mutator_id,
            mutator,
            RecordFileKind::weapon,
            brought,
            touches,
            collections,
            error
        ))
        return false;
    if (!apply_kind(
            documents,
            report,
            layout,
            records,
            mutator_id,
            mutator,
            RecordFileKind::unit,
            brought,
            touches,
            collections,
            error
        ))
        return false;
    std::vector<RecordTouch> own_units;
    own_units.reserve(touches.size() - before);
    for (std::size_t touch_index = before; touch_index < touches.size(); ++touch_index)
        own_units.push_back(touches[touch_index]);
    if (!rewire_brought_units(documents, layout, mutator, own_units, collections, error))
        return false;
    return apply_kind(
        documents,
        report,
        layout,
        records,
        mutator_id,
        mutator,
        RecordFileKind::download,
        brought,
        touches,
        collections,
        error
    );
}

/// One record the pass has already seen, so a later file can name who brought it.
struct SeenRecord {
    /// Units, weapons or menus. Menus also use the path.
    Collection collection{};
    /// Folded path. Empty except for a download entry, whose name is per file.
    std::string path;
    /// Folded record name.
    std::string name;

    /// Orders by collection, then path, then name.
    ///
    /// @param other the other record
    /// @return true when this record sorts first
    bool operator<(const SeenRecord& other) const {
        if (collection != other.collection)
            return static_cast<uint8_t>(collection) < static_cast<uint8_t>(other.collection);
        if (path != other.path)
            return path < other.path;
        return name < other.name;
    }
};

/// One pair of mutators whose records of one kind already produced a note.
struct RecordPair {
    /// The collection the note covers.
    Collection collection{};
    /// The earlier mutator.
    uint16_t earlier{};
    /// The later mutator.
    uint16_t later{};

    /// Orders by collection, then the earlier mutator, then the later one.
    ///
    /// @param other the other pair
    /// @return true when this pair sorts first
    bool operator<(const RecordPair& other) const {
        if (collection != other.collection)
            return static_cast<uint8_t>(collection) < static_cast<uint8_t>(other.collection);
        if (earlier != other.earlier)
            return earlier < other.earlier;
        return later < other.later;
    }
};

/// True when the run plays a mod rather than the base game.
///
/// The base game is named by its own id, or by a mod id equal to its game id.
bool plays_mod(const ModIdentity& mod) {
    return !mod.id.empty() && mod.id != oa::data::mod_profile::base_game_id &&
           !same_text(mod.id, mod.game);
}

/// Where a record that no mutator brought came from: "the mod" when the run
/// plays a mod and the mod's folder serves the record's file, else "the game".
///
/// A mod is the first of the store's folders when the store layers several,
/// and the whole store when it has one. A file from an archive is the mod's
/// when the archive sits in the mod's folder.
///
/// @param store the store, with the pass's layer mounted
/// @param mod the mod the run plays
/// @param path the record's document path
/// @return "the mod" or "the game"
std::string base_owner(const oa::AssetStore& store, const ModIdentity& mod, std::string_view path) {
    const std::string game{"the game"};
    if (!plays_mod(mod))
        return game;
    const std::span<const std::filesystem::path> roots = store.loose_roots();
    if (roots.size() <= 1)
        return "the mod";
    try {
        const oa::Provider provider = store.provider(path);
        if (provider.kind != oa::ProviderKind::loose && provider.kind != oa::ProviderKind::archive)
            return game;
        const std::filesystem::path relative =
            provider.path.lexically_normal().lexically_relative(roots.front().lexically_normal());
        if (!relative.empty() && *relative.begin() != "..")
            return "the mod";
    } catch (const std::exception&) {
    }
    return game;
}

/// The mutators that cloned each unit or weapon, in the order they cloned it.
///
/// The key is the record's folded name, and true for a weapon.
using CloneOwners =
    std::map<std::pair<bool, std::string>, std::vector<std::pair<uint16_t, std::size_t>>>;

/// Indexes the pass's clones by record, with their mutator and their place in the list.
CloneOwners clone_owners(std::span<const CloneRecord> clones, const std::vector<std::string>& ids) {
    CloneOwners owners;
    for (std::size_t index = 0; index < clones.size(); ++index) {
        const std::optional<uint16_t> mutator = index_of(ids, clones[index].mutator);
        if (!mutator.has_value())
            continue;
        owners[{clones[index].weapon, fold_copy(clones[index].name)}].emplace_back(*mutator, index);
    }
    return owners;
}

/// The latest clone of a record made before another change, or nothing.
///
/// @param owners the pass's clones by record
/// @param weapon true for a weapon, false for a unit
/// @param name the record
/// @param before_mutator only clones by a mutator above this one count
/// @param before_clone only clones earlier in the list than this count
/// @return the mutator that made it
std::optional<uint16_t> earlier_clone(
    const CloneOwners& owners,
    bool weapon,
    std::string_view name,
    std::size_t before_mutator,
    std::size_t before_clone
) {
    const auto found = owners.find({weapon, fold_copy(name)});
    if (found == owners.end())
        return std::nullopt;
    std::optional<uint16_t> latest;
    for (const std::pair<uint16_t, std::size_t>& clone : found->second) {
        if (clone.first < before_mutator && clone.second < before_clone)
            latest = clone.first;
    }
    return latest;
}

/// Journals every record a file added, replaced, shared or renamed.
///
/// A replacement keeps the record name in `old_text`. When the record is one
/// an earlier mutator brought, by a file or by a clone, one `value_replaced`
/// note is kept for that collection and that pair of mutators. A record no
/// mutator brought is noted as the game's or the mod's. A share records the
/// record and does not take it from whoever brought it, and it names no
/// writer because the document was not changed. A rename records the private
/// name and leaves the original record with the mutator that already owned it.
void journal_record_touches(
    Journal& journal,
    ConflictReport& report,
    const std::vector<RecordTouch>& touches,
    const rules::DocumentSet& documents,
    std::vector<std::vector<Writer>>& writers,
    std::vector<uint8_t>& menus,
    const CloneOwners& clones,
    const oa::AssetStore& store,
    const ModIdentity& mod
) {
    std::map<SeenRecord, uint16_t> owners;
    std::set<RecordPair> noted;
    for (const RecordTouch& touch : touches) {
        if (touch.fate != RecordFate::shared) {
            note_writer(
                documents, writers, menus, touch.path, touch.mutator, Phase::files, touch.collection
            );
        }
        JournalEntry entry;
        entry.mutators.push_back(touch.mutator);
        entry.phase = Phase::files;
        entry.collection = touch.collection;
        entry.change = Change::added;
        entry.record = touch.record;
        entry.path = touch.path;
        if (touch.fate == RecordFate::replaced)
            entry.old_text = touch.record;
        else if (touch.fate == RecordFate::shared) {
            entry.old_text = touch.record;
            entry.new_text = "shared";
        } else if (touch.fate == RecordFate::renamed) {
            entry.old_text = touch.asked;
            entry.new_text = touch.record;
        }
        journal.entries.push_back(std::move(entry));

        SeenRecord identity;
        identity.collection = touch.collection;
        identity.name = fold_copy(touch.record);
        if (touch.collection == Collection::menus)
            identity.path = fold_copy(touch.path);
        const auto found = owners.find(identity);
        // A record an earlier mutator's hook cloned is that mutator's, unless
        // a later mutator's file replaced it since. Records come before hooks
        // in a mutator's turn, so a clone by the same mutator counts as later.
        std::optional<uint16_t> owner;
        if (found != owners.end())
            owner = found->second;
        if (touch.collection != Collection::menus) {
            const std::optional<uint16_t> cloned = earlier_clone(
                clones,
                touch.collection == Collection::weapons,
                touch.record,
                touch.mutator,
                std::numeric_limits<std::size_t>::max()
            );
            if (cloned.has_value() && (!owner.has_value() || *cloned >= *owner))
                owner = cloned;
        }
        if (touch.fate == RecordFate::shared) {
            Conflict conflict;
            conflict.severity = Severity::note;
            conflict.kind = ConflictKind::record_shared;
            if (owner.has_value() && *owner != touch.mutator)
                conflict.mutators.push_back(*owner);
            conflict.mutators.push_back(touch.mutator);
            conflict.record = touch.record;
            conflict.path = touch.path;
            report.conflicts.push_back(std::move(conflict));
            continue;
        }
        if (touch.fate == RecordFate::renamed) {
            Conflict conflict;
            conflict.severity = Severity::note;
            conflict.kind = ConflictKind::record_renamed;
            conflict.mutators.push_back(touch.mutator);
            conflict.record = touch.asked;
            conflict.detail = touch.record;
            conflict.path = touch.path;
            report.conflicts.push_back(std::move(conflict));
        } else if (
            touch.fate == RecordFate::replaced && owner.has_value() && *owner != touch.mutator
        ) {
            RecordPair pair;
            pair.collection = touch.collection;
            pair.earlier = *owner;
            pair.later = touch.mutator;
            if (noted.insert(pair).second) {
                Conflict conflict;
                conflict.severity = Severity::note;
                conflict.kind = ConflictKind::value_replaced;
                conflict.mutators.push_back(pair.earlier);
                conflict.mutators.push_back(pair.later);
                conflict.record = touch.record;
                conflict.path = touch.path;
                report.conflicts.push_back(std::move(conflict));
            }
        } else if (touch.fate == RecordFate::replaced && !owner.has_value()) {
            Conflict conflict;
            conflict.severity = Severity::note;
            conflict.kind = ConflictKind::value_replaced;
            conflict.mutators.push_back(touch.mutator);
            conflict.record = touch.record;
            conflict.path = touch.path;
            conflict.detail = base_owner(store, mod, touch.path);
            report.conflicts.push_back(std::move(conflict));
        }
        owners.insert_or_assign(std::move(identity), touch.mutator);
    }
}

/// Drops record-file entries from the spec that will be mounted.
///
/// The base and mod files stay visible, and each mutator's records are
/// applied from the snapshots. A picture or a script stays a whole file.
void drop_record_entries(oa::MutatorLayerSpec& spec, const defs::DataLayout& layout) {
    std::vector<oa::MutatorLayerEntry> kept;
    kept.reserve(spec.entries.size());
    for (oa::MutatorLayerEntry& entry : spec.entries) {
        if (record_file_kind(entry.path, layout) != RecordFileKind::none)
            continue;
        kept.push_back(std::move(entry));
    }
    spec.entries.swap(kept);
}

/// Points a mutator's own rule selectors at records it renamed.
///
/// A named unit or weapon block, and `unitname = is("NAME")` on a unit
/// target, use the private name. The copy is this mutator's rules only.
void remap_rule_names(
    rules::MutatorRules& rules, const Collections& collections, const defs::DataLayout& layout
) {
    const std::string& units =
        layout.directories[static_cast<std::size_t>(defs::DataDirectory::units)];
    const std::string& weapons =
        layout.directories[static_cast<std::size_t>(defs::DataDirectory::weapons)];
    for (rules::RuleSource& source : rules.sources) {
        for (sm::Block& block : source.rules.blocks) {
            const bool unit_target = same_text(block.target.folder, units);
            const bool weapon_target = same_text(block.target.folder, weapons);
            if (block.selector == sm::Selector::name) {
                const std::string* mapped = nullptr;
                if (unit_target)
                    mapped = collections.renamed_to(false, block.name);
                else if (weapon_target)
                    mapped = collections.renamed_to(true, block.name);
                if (mapped != nullptr)
                    block.name = *mapped;
            }
            if (!unit_target)
                continue;
            for (sm::Need& need : block.needs) {
                if (need.kind != sm::NeedKind::equals)
                    continue;
                if (!same_text(need.field, "unitname"))
                    continue;
                const std::string* mapped = collections.renamed_to(false, need.text);
                if (mapped != nullptr)
                    need.text = *mapped;
            }
        }
    }
}

/// True when `remainder` is a known suffix of a file keyed by a unit name.
bool unit_file_remainder(
    std::string_view folder,
    std::string_view remainder,
    const defs::DataLayout& layout,
    bool move_model
) {
    const std::string& pictures =
        layout.directories[static_cast<std::size_t>(defs::DataDirectory::unitpics)];
    const std::string& pages =
        layout.directories[static_cast<std::size_t>(defs::DataDirectory::guis)];
    if (same_text(folder, "scripts") && same_text(remainder, ".cob"))
        return true;
    if (same_text(folder, pictures) && same_text(remainder, ".pcx"))
        return true;
    if (move_model && same_text(folder, "objects3d") && same_text(remainder, ".3do"))
        return true;
    if (same_text(folder, "anims") && same_text(remainder, "_gadget.gaf"))
        return true;
    const auto digits_then = [](std::string_view text, std::string_view extension) {
        if (text.size() <= extension.size() ||
            !same_text(text.substr(text.size() - extension.size()), extension))
            return false;
        const std::string_view digits = text.substr(0, text.size() - extension.size());
        if (digits.empty())
            return false;
        for (const char character : digits) {
            if (character < '0' || character > '9')
                return false;
        }
        return true;
    };
    if (same_text(folder, "anims") && digits_then(remainder, ".gaf"))
        return true;
    return same_text(folder, pages) && digits_then(remainder, ".gui");
}

/// The private path for a file keyed by `asked`, or empty when it is not one.
std::string private_unit_path(
    std::string_view path,
    std::string_view asked,
    std::string_view stored,
    const defs::DataLayout& layout,
    bool move_model
) {
    const std::string slashed_path = slashed(path);
    const std::size_t slash = slashed_path.find_last_of('/');
    if (slash == std::string::npos || asked.empty())
        return {};
    const std::string_view folder{slashed_path.data(), slash};
    const std::string_view file{slashed_path.data() + slash + 1, slashed_path.size() - slash - 1};
    if (file.size() < asked.size() || !same_text(file.substr(0, asked.size()), asked))
        return {};
    const std::string_view remainder = file.substr(asked.size());
    if (!unit_file_remainder(folder, remainder, layout, move_model))
        return {};
    std::string dest;
    dest.reserve(folder.size() + 1 + stored.size() + remainder.size());
    dest.assign(folder);
    dest.push_back('/');
    dest.append(stored);
    dest.append(remainder);
    return dest;
}

/// One file that held a path in the mutator layer.
///
/// The files that held one path are listed in the order the mutators
/// brought them: each file a later mutator displaced, then the file the
/// spec serves.
struct PathHolder {
    /// The path, folded, as the layer keys it.
    std::string key;
    /// The path, in the spelling that was read.
    std::string path;
    /// The mutator that brought the file.
    uint16_t mutator{};
    /// The displaced file, or null for the file the spec serves.
    const DisplacedFile* displaced{};
    /// Index in the spec of the file it serves; unused for a displaced file.
    std::size_t entry{};
    /// The renaming mutator's own file, served under the private name instead.
    bool moved{};
};

/// A path folded the way the layer compares paths.
std::string path_key(std::string_view path) {
    std::string key;
    key.reserve(path.size());
    for (const char byte : path)
        key.push_back(fold_path_byte(byte));
    return key;
}

/// One copy a renamed unit takes under its private name.
struct UnitFileCopy {
    /// The mutator that renamed the unit.
    uint16_t mutator{};
    /// The path keyed by the asked name.
    std::string source;
    /// The path keyed by the private name.
    std::string dest;
    /// The bytes to serve; null when there are none.
    std::shared_ptr<const std::vector<uint8_t>> bytes;
    /// The bytes are the game's or the mod's, read beneath the mutator layer.
    bool beneath{};
};

/// The bytes a holder's file serves.
std::shared_ptr<const std::vector<uint8_t>> holder_bytes(
    const PathHolder& holder, const oa::MutatorLayerSpec& spec, const oa::AssetStore& store
) {
    if (holder.displaced != nullptr)
        return displaced_bytes(*holder.displaced);
    const oa::MutatorLayerEntry& entry = spec.entries[holder.entry];
    if (entry.contents != nullptr && !entry.contents->empty())
        return entry.contents;
    std::optional<std::vector<uint8_t>> loaded = store.load_file_contents(entry.path);
    if (!loaded.has_value() || loaded->empty())
        return nullptr;
    return std::make_shared<const std::vector<uint8_t>>(std::move(*loaded));
}

/// The layer entry that serves a displaced file again.
///
/// A loose file is read from its host path. A file from an archive is
/// served from its bytes, since the layer may no longer hold that archive.
std::optional<oa::MutatorLayerEntry> restored_entry(const DisplacedFile& file) {
    oa::MutatorLayerEntry entry;
    entry.path = file.path;
    entry.owner = file.owner;
    if (file.kind == oa::MutatorSourceKind::loose) {
        entry.kind = oa::MutatorSourceKind::loose;
        entry.host = file.host;
        return entry;
    }
    entry.kind = oa::MutatorSourceKind::contents;
    entry.contents = displaced_bytes(file);
    if (entry.contents == nullptr)
        return std::nullopt;
    return entry;
}

/// Reads the game's and the mod's bytes of the paths beneath the mutator layer.
///
/// Each copy marked `beneath` is read with the layer swapped for one that
/// holds only the masks of the renaming mutator and those above it, as the
/// path stood at that mutator's turn. The layer `spec` is mounted again
/// afterwards. A copy whose read cannot be made keeps no bytes.
///
/// @param store the store, with `spec` mounted
/// @param spec the layer the pass mounted
/// @param ids mutator ids, in list order
/// @param[in,out] copies the planned copies
/// @param[out] error why the layer could not be mounted again
/// @return false when the layer could not be mounted again
bool read_beneath(
    oa::AssetStore& store,
    const oa::MutatorLayerSpec& spec,
    const std::vector<std::string>& ids,
    std::vector<UnitFileCopy>& copies,
    std::string& error
) {
    const bool mounted = store.mutator_layer_mounted();
    bool swapped = false;
    std::vector<uint16_t> done;
    for (const UnitFileCopy& lead : copies) {
        if (!lead.beneath || std::find(done.begin(), done.end(), lead.mutator) != done.end())
            continue;
        done.push_back(lead.mutator);
        if (mounted) {
            oa::MutatorLayerSpec beneath;
            beneath.label = spec.label.empty() ? std::string{"mutators"} : spec.label;
            for (const oa::MutatorMask& mask : spec.masks) {
                const std::optional<uint16_t> owner = index_of(ids, mask.owner);
                if (owner.has_value() && *owner <= lead.mutator)
                    beneath.masks.push_back(mask);
            }
            bool ready = true;
            if (beneath.masks.empty())
                store.unmount_mutator_layer();
            else
                ready = store.replace_mutator_layer(std::move(beneath));
            if (!ready)
                continue;
            swapped = true;
        }
        for (UnitFileCopy& copy : copies) {
            if (!copy.beneath || copy.mutator != lead.mutator)
                continue;
            std::optional<std::vector<uint8_t>> loaded = store.load_file_contents(copy.source);
            if (loaded.has_value() && !loaded->empty())
                copy.bytes = std::make_shared<const std::vector<uint8_t>>(std::move(*loaded));
        }
    }
    if (swapped && !store.replace_mutator_layer(spec, &error)) {
        if (error.empty())
            error = "the mutator layer could not be mounted again";
        return false;
    }
    return true;
}

/// Rewrites the notes that a mutator won `key`, after files moved off it.
///
/// A note that names a moved file's mutator goes. A file that displaced a
/// moved one is noted as winning the path over the file before the moved
/// one, when there is such a file, in the place of the first note that went.
///
/// @param[in,out] notes the files layer's notes
/// @param ids mutator ids, in list order
/// @param holders every file that held a path
/// @param held the indices in `holders` of the files that held `key`, in order
/// @param key the folded path
void renote_path(
    std::vector<FileNote>& notes,
    const std::vector<std::string>& ids,
    const std::vector<PathHolder>& holders,
    const std::vector<std::size_t>& held,
    const std::string& key
) {
    std::vector<FileNote> fresh;
    std::vector<std::string> moved;
    const PathHolder* before = nullptr;
    bool gap = false;
    for (const std::size_t index : held) {
        const PathHolder& holder = holders[index];
        if (holder.moved) {
            moved.push_back(ids[holder.mutator]);
            gap = true;
            continue;
        }
        if (before != nullptr && gap && before->mutator != holder.mutator) {
            FileNote note;
            note.kind = FileNoteKind::add_replaced;
            note.path = holder.path;
            note.mutator = ids[holder.mutator];
            note.other = ids[before->mutator];
            fresh.push_back(std::move(note));
        }
        before = &holder;
        gap = false;
    }
    const auto names_moved = [&moved](const FileNote& note) {
        return std::find(moved.begin(), moved.end(), note.mutator) != moved.end() ||
               std::find(moved.begin(), moved.end(), note.other) != moved.end();
    };
    std::vector<FileNote> kept;
    kept.reserve(notes.size() + fresh.size());
    bool placed = false;
    for (FileNote& note : notes) {
        if (note.kind != FileNoteKind::add_replaced || path_key(note.path) != key ||
            !names_moved(note)) {
            kept.push_back(std::move(note));
            continue;
        }
        if (placed)
            continue;
        placed = true;
        for (FileNote& one : fresh)
            kept.push_back(std::move(one));
    }
    notes.swap(kept);
}

/// Serves a renamed unit's name-keyed files under the private name.
///
/// A file the renaming mutator brought for the unit is served under the
/// private name. When that file still holds the asked path, it leaves it,
/// and the file it displaced there from a mutator above it is served
/// again. A file the mutator did not bring is copied as the path stood at
/// its turn: the latest file a mutator above it brought, else the game's
/// or the mod's, read beneath the layer. A later mutator's file at the
/// asked path is the original unit's, and stays there. The notes that a
/// mutator won such a path are rewritten to name the files that still
/// hold it.
///
/// @param[in,out] spec the layer the pass mounted, changed in place
/// @param store the store, with `spec` mounted
/// @param collections the pass's collections, which hold the renames and receive the copies
/// @param layout directory names
/// @param ids mutator ids, in list order
/// @param displaced the files a later mutator displaced, from the files layer
/// @param[in,out] notes the files layer's notes
/// @param[out] error why the layer could not be mounted again
/// @return false when the layer could not be mounted again
bool redirect_renamed_unit_files(
    oa::MutatorLayerSpec& spec,
    oa::AssetStore& store,
    Collections& collections,
    const defs::DataLayout& layout,
    const std::vector<std::string>& ids,
    std::span<const DisplacedFile> displaced,
    std::vector<FileNote>& notes,
    std::string& error
) {
    const std::span<const Collections::UnitFileShift> shifts = collections.unit_file_shifts();
    if (shifts.empty())
        return true;
    std::vector<PathHolder> holders;
    holders.reserve(displaced.size() + spec.entries.size());
    for (const DisplacedFile& file : displaced) {
        const std::optional<uint16_t> owner = index_of(ids, file.owner);
        if (!owner.has_value())
            continue;
        PathHolder holder;
        holder.key = path_key(file.path);
        holder.path = file.path;
        holder.mutator = *owner;
        holder.displaced = &file;
        holders.push_back(std::move(holder));
    }
    for (std::size_t index = 0; index < spec.entries.size(); ++index) {
        const oa::MutatorLayerEntry& entry = spec.entries[index];
        const std::optional<uint16_t> owner = index_of(ids, entry.owner);
        if (entry.kind == oa::MutatorSourceKind::hidden || !owner.has_value())
            continue;
        PathHolder holder;
        holder.key = path_key(entry.path);
        holder.path = entry.path;
        holder.mutator = *owner;
        holder.entry = index;
        holders.push_back(std::move(holder));
    }
    // Displaced files come first, in the order they were displaced, so each
    // path's list runs from the first file to the one the spec serves.
    std::map<std::string, std::vector<std::size_t>> held;
    for (std::size_t index = 0; index < holders.size(); ++index)
        held[holders[index].key].push_back(index);

    std::vector<UnitFileCopy> copies;
    std::vector<std::string> moved_keys;
    for (const Collections::UnitFileShift& shift : shifts) {
        if (shift.mutator >= ids.size())
            continue;
        std::vector<std::string> handled;
        const auto already = [&](std::string_view path) {
            const std::string key = path_key(path);
            return std::find(handled.begin(), handled.end(), key) != handled.end();
        };
        const auto plan = [&](const std::string& source,
                              std::shared_ptr<const std::vector<uint8_t>> bytes,
                              bool beneath) {
            const std::string dest =
                private_unit_path(source, shift.asked, shift.stored, layout, shift.move_model);
            if (dest.empty())
                return;
            UnitFileCopy copy;
            copy.mutator = shift.mutator;
            copy.source = source;
            copy.dest = dest;
            copy.bytes = std::move(bytes);
            copy.beneath = beneath;
            copies.push_back(std::move(copy));
            handled.push_back(path_key(source));
        };
        for (PathHolder& holder : holders) {
            if (holder.mutator != shift.mutator || holder.moved || already(holder.path))
                continue;
            if (private_unit_path(holder.path, shift.asked, shift.stored, layout, shift.move_model)
                    .empty())
                continue;
            plan(holder.path, holder_bytes(holder, spec, store), false);
            holder.moved = true;
            moved_keys.push_back(holder.key);
        }
        const auto consider = [&](const std::string& source) {
            if (already(source))
                return;
            const auto found = held.find(path_key(source));
            const PathHolder* earlier = nullptr;
            bool layer_serves = false;
            if (found != held.end()) {
                for (const std::size_t index : found->second) {
                    const PathHolder& holder = holders[index];
                    if (holder.displaced == nullptr)
                        layer_serves = true;
                    if (!holder.moved && holder.mutator < shift.mutator)
                        earlier = &holder;
                }
            }
            if (earlier != nullptr) {
                plan(source, holder_bytes(*earlier, spec, store), false);
                return;
            }
            // The store shows another mutator's file, or hides the path for a
            // mutator after this one: the game's bytes lie beneath the layer.
            for (const oa::MutatorMask& mask : spec.masks) {
                const std::optional<uint16_t> owner = index_of(ids, mask.owner);
                if (owner.has_value() && *owner > shift.mutator &&
                    oa::mutator_mask_matches(mask.pattern, source))
                    layer_serves = true;
            }
            if (layer_serves) {
                plan(source, nullptr, true);
                return;
            }
            std::optional<std::vector<uint8_t>> loaded = store.load_file_contents(source);
            if (!loaded.has_value() || loaded->empty())
                return;
            plan(source, std::make_shared<const std::vector<uint8_t>>(std::move(*loaded)), false);
        };
        const std::string& pictures =
            layout.directories[static_cast<std::size_t>(defs::DataDirectory::unitpics)];
        const std::string& pages =
            layout.directories[static_cast<std::size_t>(defs::DataDirectory::guis)];
        consider("scripts/" + shift.asked + ".COB");
        consider(pictures + "/" + shift.asked + ".PCX");
        if (shift.move_model)
            consider("objects3d/" + shift.asked + ".3DO");
        consider("anims/" + shift.asked + "_gadget.GAF");
        const auto probe_page = [&](const std::string& page) {
            const std::string gui = pages + "/" + shift.asked + page + ".GUI";
            const std::string anim = "anims/" + shift.asked + page + ".GAF";
            const bool present = store.file_size(gui) > 0 || store.file_size(anim) > 0;
            consider(gui);
            consider(anim);
            return present;
        };
        probe_page("0");
        for (uint32_t page = 1; page <= 32; ++page) {
            if (!probe_page(std::to_string(page)))
                break;
        }
    }
    if (!read_beneath(store, spec, ids, copies, error))
        return false;
    for (UnitFileCopy& copy : copies) {
        if (copy.bytes == nullptr || copy.bytes->empty())
            continue;
        ServedCopy served;
        served.mutator = ids[copy.mutator];
        served.path = copy.dest;
        served.source_path = copy.source;
        served.sentence = "added path ";
        served.sentence.append(copy.dest);
        served.sentence.append(", copy of ");
        served.sentence.append(copy.source);
        served.bytes = std::move(copy.bytes);
        collections.add_served_copy(std::move(served));
    }
    if (moved_keys.empty())
        return true;

    // A moved file that the spec served gives the path back to the latest
    // file it displaced from a mutator above it, when there is one.
    std::vector<std::optional<oa::MutatorLayerEntry>> replacement(spec.entries.size());
    std::vector<uint8_t> dropped(spec.entries.size(), 0);
    for (const PathHolder& holder : holders) {
        if (!holder.moved || holder.displaced != nullptr)
            continue;
        dropped[holder.entry] = 1;
        const PathHolder* earlier = nullptr;
        for (const std::size_t index : held[holder.key]) {
            const PathHolder& other = holders[index];
            if (other.displaced != nullptr && !other.moved && other.mutator < holder.mutator)
                earlier = &other;
        }
        if (earlier != nullptr)
            replacement[holder.entry] = restored_entry(*earlier->displaced);
    }
    std::vector<oa::MutatorLayerEntry> kept;
    kept.reserve(spec.entries.size());
    for (std::size_t index = 0; index < spec.entries.size(); ++index) {
        if (dropped[index] == 0)
            kept.push_back(std::move(spec.entries[index]));
        else if (replacement[index].has_value())
            kept.push_back(std::move(*replacement[index]));
    }
    spec.entries.swap(kept);
    std::sort(moved_keys.begin(), moved_keys.end());
    moved_keys.erase(std::unique(moved_keys.begin(), moved_keys.end()), moved_keys.end());
    for (const std::string& key : moved_keys)
        renote_path(notes, ids, holders, held[key], key);
    return true;
}

/// Fails every mutator still in the pass with an engine error.
void fail_every(
    std::vector<MutatorResult>& results,
    const std::vector<uint8_t>& active,
    Phase phase,
    const std::string& message
) {
    for (std::size_t slot = 0; slot < results.size() && slot < active.size(); ++slot) {
        if (active[slot] != 0)
            fail_mutator(results[slot], phase, Cause::engine_error, message);
    }
}

/// What applying the held map rules did. `error` empty means they ran.
struct HeldMapRules {
    std::string error;
    std::size_t slot{k_no_slot};
};

/// Applies held `maps/*.ota` rules to the chosen map, before any maps hook.
///
/// Writes join the journal as rules, with D07's record names. Notes join
/// the report. A changed OTA replaces `working` so the hooks read it back.
/// An unreadable map fails the first mutator that held map rules.
HeldMapRules apply_held_map_rules(
    const std::vector<Plan>& plans,
    const std::vector<uint8_t>& active,
    const std::vector<std::string>& ids,
    std::size_t limit,
    const ChosenMap& map,
    std::vector<uint8_t>& working,
    std::vector<JournalEntry>& map_journal,
    ConflictReport& report,
    std::set<ReplacedField>& replaced_fields,
    std::string& served_owner,
    std::shared_ptr<const std::vector<uint8_t>>& served_ota
) {
    HeldMapRules held;
    std::vector<rules::MutatorRules> held_rules;
    for (std::size_t slot = 0; slot < plans.size() && slot < active.size() && slot < limit;
         ++slot) {
        if (active[slot] == 0 || !plans[slot].has_map_rules)
            continue;
        if (held.slot == k_no_slot)
            held.slot = slot;
        held_rules.push_back(plans[slot].map_rules);
    }
    if (held_rules.empty())
        return held;
    rules::DocumentSet map_documents;
    rules::ApplyResult applied = apply_map_rules(held_rules, map, map_documents);
    for (const rules::RuleNote& note : applied.notes) {
        if (note.message == "the chosen map could not be read") {
            held.error = note.message;
            return held;
        }
    }
    for (const rules::RuleNote& note : applied.notes)
        append_rule_note(report, note, ids, replaced_fields);

    std::vector<JournalEntry> fresh;
    std::string owner;
    uint16_t highest = 0;
    bool have_owner = false;
    const std::string path = map.ota_path.empty() ? "maps/" + map.key + ".ota" : map.ota_path;
    for (const rules::FieldWrite& write : applied.writes) {
        JournalEntry entry;
        for (const std::string& name : write.mutators) {
            const std::optional<uint16_t> index = index_of(ids, name);
            if (!index.has_value())
                continue;
            entry.mutators.push_back(*index);
            if (!have_owner || *index >= highest) {
                highest = *index;
                have_owner = true;
                owner = ids[*index];
            }
        }
        entry.phase = Phase::rules;
        entry.collection = Collection::other;
        entry.change = write.new_text.has_value() ? Change::set : Change::removed_field;
        if (write.block.empty())
            entry.record = "map:" + map.key;
        else
            entry.record = "map:" + map.key + "/" + write.block;
        set_field(entry, write.key);
        entry.path = path;
        entry.old_text = write.old_text;
        entry.new_text = write.new_text;
        fresh.push_back(std::move(entry));
    }
    if (map_documents.size() == 0 || !map_documents.changed(0))
        return held;
    const std::optional<std::string> written = tdf::write_text(map_documents.at(0).document.get());
    if (!written.has_value()) {
        held.error = "the chosen map could not be written";
        return held;
    }
    auto bytes = std::make_shared<const std::vector<uint8_t>>(written->begin(), written->end());
    if (*bytes == working)
        return held;
    for (JournalEntry& entry : fresh)
        map_journal.push_back(std::move(entry));
    working.assign(bytes->begin(), bytes->end());
    served_ota = std::move(bytes);
    if (!owner.empty())
        served_owner = std::move(owner);
    return held;
}

/// Records one base document defines, so a mask of that file can be blamed
/// for a reference that named one of them. A mask is journaled as a hidden
/// path with an empty record.
struct BaseNames {
    std::string path;
    Collection collection{Collection::other};
    std::vector<std::string> records;
};

const tdf::Block* unit_info_block(const tdf::Block* root) {
    if (root == nullptr)
        return nullptr;
    const uint32_t count = tdf::child_count(root);
    for (uint32_t index = 0; index < count; ++index) {
        const tdf::Block* child = tdf::child_at(root, index);
        if (child != nullptr && child->name != nullptr &&
            tdf::compare_nocase(child->name, "UNITINFO") == 0)
            return child;
    }
    return nullptr;
}

void remember_names(
    std::vector<BaseNames>& defined, std::string path, Collection collection, std::string record
) {
    for (BaseNames& known : defined) {
        if (!same_path(known.path, path))
            continue;
        known.records.push_back(std::move(record));
        return;
    }
    BaseNames fresh;
    fresh.path = std::move(path);
    fresh.collection = collection;
    fresh.records.push_back(std::move(record));
    defined.push_back(std::move(fresh));
}

void remember_sections(
    std::vector<BaseNames>& defined, const rules::LoadedDocument& loaded, Collection collection
) {
    const tdf::Block* root = loaded.document.root();
    const uint32_t count = tdf::child_count(root);
    for (uint32_t index = 0; index < count; ++index) {
        const tdf::Block* section = tdf::child_at(root, index);
        if (section == nullptr || section->name == nullptr || section->name[0] == '\0')
            continue;
        remember_names(defined, loaded.path, collection, section->name);
    }
}

/// What each base document defines: a unit file's UnitName, else its stem;
/// each section of a weapons or features file; each class Name in moveinfo.
std::vector<BaseNames>
names_defined_by(const rules::DocumentSet& documents, const defs::DataLayout& layout) {
    std::vector<BaseNames> defined;
    const std::string unit_suffix = "." + layout.unit_extension;
    const std::vector<std::size_t> units = documents.documents_in(
        layout.directories[static_cast<std::size_t>(defs::DataDirectory::units)], unit_suffix, false
    );
    for (const std::size_t index : units) {
        const rules::LoadedDocument& loaded = documents.at(index);
        const tdf::Block* info = unit_info_block(loaded.document.root());
        remember_names(
            defined, loaded.path, Collection::units, unit_name_of(info, loaded.path, layout)
        );
    }
    const std::vector<std::size_t> weapons = documents.documents_in(
        layout.directories[static_cast<std::size_t>(defs::DataDirectory::weapons)], ".tdf", false
    );
    for (const std::size_t index : weapons)
        remember_sections(defined, documents.at(index), Collection::weapons);
    const std::vector<std::size_t> features = documents.documents_in("features", ".tdf", true);
    for (const std::size_t index : features)
        remember_sections(defined, documents.at(index), Collection::features);
    const std::vector<std::size_t> tables = documents.documents_in(
        layout.directories[static_cast<std::size_t>(defs::DataDirectory::gamedata)], ".tdf", true
    );
    for (const std::size_t index : tables) {
        const rules::LoadedDocument& loaded = documents.at(index);
        const std::size_t slash = loaded.path.find_last_of("/\\");
        const std::string_view file = slash == std::string::npos
                                          ? std::string_view{loaded.path}
                                          : std::string_view{loaded.path}.substr(slash + 1);
        if (!same_text(file, "moveinfo.tdf"))
            continue;
        const tdf::Block* root = loaded.document.root();
        const uint32_t count = tdf::child_count(root);
        for (uint32_t child = 0; child < count; ++child) {
            const tdf::Block* section = tdf::child_at(root, child);
            if (section == nullptr || section->name == nullptr)
                continue;
            const char* title = section->name;
            bool class_section = true;
            const char prefix[] = "class";
            for (std::size_t byte = 0; byte < 5; ++byte) {
                char folded = title[byte];
                if (folded == '\0') {
                    class_section = false;
                    break;
                }
                if (folded >= 'A' && folded <= 'Z')
                    folded = static_cast<char>(folded - 'A' + 'a');
                if (folded != prefix[byte]) {
                    class_section = false;
                    break;
                }
            }
            if (!class_section)
                continue;
            if (title[5] < '0' || title[5] > '9')
                continue;
            const char* name = tdf::find_value(section, "Name");
            if (name == nullptr || name[0] == '\0')
                continue;
            remember_names(defined, loaded.path, Collection::tables, name);
        }
    }
    return defined;
}

/// One removed entry per record a hidden base file defined, so blame can
/// name the mutator that masked the file.
void note_hidden_records(Journal& journal, std::span<const BaseNames> base_names) {
    std::vector<JournalEntry> removed_records;
    for (const JournalEntry& entry : journal.entries) {
        if (entry.change != Change::hidden)
            continue;
        for (const BaseNames& defined : base_names) {
            if (!same_path(defined.path, entry.path))
                continue;
            for (const std::string& record : defined.records) {
                JournalEntry removed;
                removed.mutators = entry.mutators;
                removed.phase = entry.phase;
                removed.collection = defined.collection;
                removed.change = Change::removed;
                removed.record = record;
                removed.path = entry.path;
                removed_records.push_back(std::move(removed));
            }
        }
    }
    for (JournalEntry& removed : removed_records)
        journal.entries.push_back(std::move(removed));
}

/// One pass over the mutators that are still applied.
///
/// A clean pass publishes its layer and returns true. A pass that finds a
/// failure records it, leaves the store unmounted and returns false. The
/// pass's documents are its own, parsed from the store, so nothing needs
/// rolling back: the next pass parses them again without what failed.
/// `base_issues` are the problems the store had before this run mounted a
/// layer. The pass compares its own documents with those and keeps only
/// the problems the mutators added. `base_names` are the records those base
/// documents defined, so a mask of one of them blames the mutator that hid it.
bool run_pass(
    oa::AssetStore& store,
    const PipelineInput& input,
    const defs::DataLayout& layout,
    const std::vector<std::string>& ids,
    std::vector<MutatorResult>& results,
    Journal& journal,
    ConflictReport& report,
    oa::MutatorLayerSpec& layer_spec,
    MountGuard& guard,
    std::span<const IntegrityIssue> base_issues,
    std::span<const BaseNames> base_names
) {
    const std::size_t count = input.mutators.size();
    // Destroyed in reverse order: every sandbox closes before the menus and
    // the collections that were pushed into it. The menus go before the
    // collections, the collections go before the documents, and the listings
    // the sandboxes read from are released with the sandboxes.
    rules::DocumentSet documents;
    std::optional<Collections> collections;
    std::optional<Menus> build_menus;
    std::vector<Listing> listings(count);
    std::vector<std::vector<LoadoutOption>> options(count);
    std::vector<std::optional<host::Sandbox>> sandboxes(count);
    std::vector<Plan> plans(count);
    std::vector<uint8_t> active(count, 0);
    for (std::size_t slot = 0; slot < count; ++slot)
        active[slot] = results[slot].outcome == Outcome::applied ? 1 : 0;

    // A plan sees only its own package and options, so each plan failure is
    // that mutator's own, and every one is kept.
    bool planned = true;
    for (std::size_t slot = 0; slot < count; ++slot) {
        if (active[slot] == 0)
            continue;
        options[slot] = resolve_options(input.mutators[slot]);
        plan_mutator(
            input,
            slot,
            layout,
            listings[slot],
            options[slot],
            sandboxes[slot],
            plans[slot],
            results[slot]
        );
        if (results[slot].outcome != Outcome::applied)
            planned = false;
    }
    if (!planned)
        return false;

    // Hacks are judged before any file, rule or hook, so a refusal drops
    // that mutator on the next pass and leaves nothing of it mounted.
    if (input.check_hacks) {
        std::vector<MutatorHack> planned_hacks;
        for (std::size_t slot = 0; slot < count; ++slot) {
            if (active[slot] == 0)
                continue;
            planned_hacks.insert(
                planned_hacks.end(), plans[slot].hacks.begin(), plans[slot].hacks.end()
            );
        }
        if (!planned_hacks.empty()) {
            bool refused = false;
            const std::vector<HackCheckError> errors = input.check_hacks(planned_hacks);
            for (const HackCheckError& error : errors) {
                const std::optional<uint16_t> slot = index_of(ids, error.mutator);
                if (!slot.has_value() || active[*slot] == 0)
                    continue;
                fail_mutator(results[*slot], Phase::plan, Cause::hack_refused, error.message);
                refused = true;
            }
            if (refused)
                return false;
        }
    }

    std::vector<std::string> stems = unit_stems(store, layout);
    PathContext paths;
    paths.layout = &layout;
    paths.is_unit_name = [&stems](std::string_view name) {
        const std::string folded = fold_copy(name);
        return std::binary_search(stems.begin(), stems.end(), folded);
    };

    // A rule target is judged by its own folder and the mod's unit names, so
    // these failures are each mutator's own too.
    bool targets_allowed = true;
    for (std::size_t slot = 0; slot < count; ++slot) {
        if (active[slot] == 0 || !plans[slot].has_rules)
            continue;
        std::vector<rules::MutatorRules> one;
        one.push_back(plans[slot].rules);
        const std::vector<sm::Target> targets = rules::targets_of(one);
        for (const sm::Target& target : targets) {
            std::string message;
            if (!target_allowed(target, layout, paths, message)) {
                fail_mutator(
                    results[slot], Phase::rules, Cause::protected_path, std::move(message)
                );
                targets_allowed = false;
                break;
            }
        }
    }
    if (!targets_allowed)
        return false;

    // From here a mutator's outcome can depend on the mutators above it: a
    // mask or a layer-wide limit in the files layer, or the documents its
    // hooks read. Only the first failure in list order is kept. A mutator the
    // files layer refused sits out the rest of the pass, so the mutators
    // above it are judged as they would be without it.
    FirstFailure first;
    std::vector<MutatorFiles> files;
    for (std::size_t slot = 0; slot < count; ++slot) {
        if (active[slot] == 0 || !plans[slot].has_files)
            continue;
        MutatorFiles one;
        one.id = ids[slot];
        one.package_root = input.mutators[slot].package;
        one.plan = plans[slot].files;
        files.push_back(std::move(one));
    }
    const FilesLayer built = build_files_layer(files, paths);
    for (const MutatorFailure& failure : built.failures) {
        const std::optional<uint16_t> slot = index_of(ids, failure.mutator);
        if (!slot.has_value())
            continue;
        first.offer(*slot, Phase::files, Cause::files_refused, failure.message);
        active[*slot] = 0;
    }
    std::vector<JournalEntry> masked = mask_entries(store, built.spec, ids);

    oa::MutatorLayerSpec spec = built.spec;
    drop_record_entries(spec, layout);
    if (!spec.entries.empty() || !spec.masks.empty() || !spec.archives.empty()) {
        std::string mount_error;
        if (!store.mount_mutator_layer(spec, &mount_error)) {
            fail_every(
                results,
                active,
                Phase::files,
                mount_error.empty() ? "the mutator layer could not be mounted" : mount_error
            );
            return false;
        }
        guard.arm();
    }

    std::vector<rules::MutatorRules> rule_sets;
    for (std::size_t slot = 0; slot < count; ++slot) {
        if (active[slot] != 0 && plans[slot].has_rules)
            rule_sets.push_back(plans[slot].rules);
    }
    parse_documents(documents, report, store, layout, input.variant, rule_sets);
    const std::vector<std::string> kept_weapons = kept_weapon_names(documents, layout);
    rules::ApplyOptions apply_options;
    apply_options.layout = &layout;
    apply_options.check = [&layout, &kept_weapons, weapon_types = input.limits.weapon_types](
                              const rules::FieldRef& field, const std::optional<std::string>& text
                          ) {
        return check_field(field, text, layout, kept_weapons, weapon_types);
    };
    rules::ApplyState apply_state;
    std::vector<rules::FieldWrite> rule_writes;
    std::set<ReplacedField> replaced_fields;
    std::set<ReplacedField> hook_replaced;

    // Each mutator applies the records in its own files, then its rules,
    // then its hooks, units, weapons, features, sides, tables and menus, on
    // the documents the mutators above it left. The maps entries run after
    // every one of those, once the pass's other hooks have succeeded and its
    // weapons are numbered. The first hook that fails ends the pass: no later
    // mutator's records, rules or hooks run on what it left. A mutator below
    // a failure the files layer found does not run either.
    collections.emplace(documents, &layout);
    collections->bind_store(&store);
    collections->set_weapon_types(input.limits.weapon_types);
    collections->keep_weapon_ids(kept_weapons);
    build_menus.emplace(*collections, store, &layout);
    std::vector<BroughtWeapon> brought;
    std::vector<RecordTouch> touches;
    bool steps_done = true;
    for (std::size_t slot = 0; slot < count && slot < first.slot; ++slot) {
        if (active[slot] == 0)
            continue;
        collections->clear_renames();
        collections->set_clash_mode(effective_clash(
            input.mutators[slot].clashes, input.mutators[slot].manifest.clashes, input.clash_default
        ));
        collections->set_clash_active(true);
        std::string record_error;
        if (!apply_mutator_records(
                documents,
                report,
                layout,
                built.records,
                ids[slot],
                static_cast<uint16_t>(slot),
                brought,
                touches,
                *collections,
                record_error
            )) {
            first.offer(slot, Phase::files, Cause::engine_error, ids[slot] + ": " + record_error);
            steps_done = false;
            break;
        }
        if (plans[slot].has_rules) {
            // Only a mutator that renamed a record needs its own copy of the
            // rules, with its selectors pointed at the private names.
            const rules::MutatorRules* step_rules = &plans[slot].rules;
            rules::MutatorRules mapped;
            if (collections->rename_count() > 0) {
                mapped = plans[slot].rules;
                remap_rule_names(mapped, *collections, layout);
                step_rules = &mapped;
            }
            rules::ApplyResult step =
                rules::apply_step(*step_rules, documents, apply_state, apply_options);
            const std::size_t wrote_from = rule_writes.size();
            for (rules::FieldWrite& write : step.writes)
                rule_writes.push_back(std::move(write));
            for (const rules::RuleNote& note : step.notes)
                append_rule_note(report, note, ids, replaced_fields);
            std::span<rules::FieldWrite> fresh;
            if (wrote_from < rule_writes.size()) {
                fresh = std::span<rules::FieldWrite>(
                    rule_writes.data() + wrote_from, rule_writes.size() - wrote_from
                );
            }
            std::string retarget_error;
            if (!collections->retarget_renamed_references(fresh, ids[slot], retarget_error)) {
                first.offer(
                    slot, Phase::rules, Cause::engine_error, ids[slot] + ": " + retarget_error
                );
                steps_done = false;
                break;
            }
        }
        // A mutator's renames end with its turn: a later mutator's names, and
        // the maps phase's, mean the records of those names.
        if (!has_data_hook(plans[slot])) {
            collections->clear_renames();
            continue;
        }
        if (!sandboxes[slot].has_value()) {
            collections->clear_renames();
            continue;
        }
        const std::size_t notes_before = collections->notes().size();
        const std::size_t writes_before = collections->writes().size();
        collections->begin_mutator(ids[slot], grant_for(input.mutators[slot].manifest));
        build_menus->begin_mutator(declares(input.mutators[slot].manifest, oamut::Layer::menus));
        DeclaredStop stop;
        ContextView context;
        context.mod = &input.mod;
        context.engine = &input.engine_version;
        context.options = &options[slot];
        context.stop = &stop;
        bool hook_ok = true;
        for (const HookStep& step : k_hook_steps) {
            if (!hook_ok)
                break;
            const int function_ref = plans[slot].*(step.ref);
            if (function_ref == LUA_NOREF)
                continue;
            stop.raised = false;
            stop.length = 0;
            stop.reason[0] = '\0';
            hook_ok = call_hook(
                *sandboxes[slot],
                *collections,
                *build_menus,
                function_ref,
                step.which,
                context,
                store,
                layout,
                input.variant,
                slot,
                first
            );
        }
        if (hook_ok && collections->write_limit_failed()) {
            // The script caught the error and returned. The limit still fails it.
            first.offer(
                slot, Phase::hooks, Cause::script_error, "this mutator has changed too many fields"
            );
            hook_ok = false;
        }
        if (!hook_ok) {
            steps_done = false;
            break;
        }
        std::string retarget_error;
        if (!collections->retarget_renamed_references(ids[slot], retarget_error)) {
            first.offer(slot, Phase::hooks, Cause::engine_error, ids[slot] + ": " + retarget_error);
            steps_done = false;
            break;
        }
        std::string menu_error;
        if (!build_menus->end_mutator(menu_error)) {
            first.offer(slot, Phase::hooks, Cause::engine_error, std::move(menu_error));
            steps_done = false;
            break;
        }
        collections->end_mutator();
        collections->clear_renames();
        record_hook_turn(
            *collections, apply_state, report, ids, slot, notes_before, writes_before, hook_replaced
        );
    }
    if (!steps_done) {
        sandboxes.clear();
        build_menus.reset();
        collections.reset();
        guard.release();
        fail_mutator(results[first.slot], first.phase, first.cause, std::move(first.message));
        return false;
    }

    if (collections.has_value()) {
        for (const CloneRecord& clone : collections->clones()) {
            if (!clone.weapon || clone.replaced ||
                brought_index(brought, clone.name) < brought.size())
                continue;
            const std::optional<uint16_t> owner = index_of(ids, clone.mutator);
            if (!owner.has_value())
                continue;
            BroughtWeapon weapon;
            weapon.name = clone.name;
            weapon.mutator = *owner;
            weapon.path = clone.path;
            brought.push_back(std::move(weapon));
        }
    }
    WeaponIdResult id_result;
    if (!assign_weapon_ids(documents, layout, brought, id_result, input.limits.weapon_types)) {
        std::string message;
        if (id_result.mutator < ids.size())
            message = ids[id_result.mutator];
        message.append(": no free weapon id for ");
        message.append(id_result.weapon);
        first.offer(id_result.mutator, Phase::integrity, Cause::weapon_ids, std::move(message));
    }

    // Map rules for maps/*.ota run first, on this map only. Then one
    // transaction per mutator that named maps. A turn that writes and then
    // errors keeps none of its bytes. The served file, when the bytes differ
    // from the map the caller passed, is published with the pass.
    std::vector<JournalEntry> map_journal;
    std::vector<Conflict> map_notes;
    std::shared_ptr<const std::vector<uint8_t>> served_ota;
    std::string served_owner;
    if (input.map.has_value()) {
        std::vector<uint8_t> working =
            input.map->ota != nullptr ? *input.map->ota : std::vector<uint8_t>{};
        const std::vector<uint8_t> original = working;
        bool maps_done = true;
        const HeldMapRules held = apply_held_map_rules(
            plans,
            active,
            ids,
            first.slot,
            *input.map,
            working,
            map_journal,
            report,
            replaced_fields,
            served_owner,
            served_ota
        );
        if (!held.error.empty()) {
            first.offer(held.slot, Phase::rules, Cause::engine_error, held.error);
            maps_done = false;
        }
        for (std::size_t slot = 0; slot < count && slot < first.slot && maps_done; ++slot) {
            if (active[slot] == 0 || !plans[slot].has_maps)
                continue;
            if (!sandboxes[slot].has_value() || !collections.has_value()) {
                first.offer(
                    slot, Phase::hooks, Cause::engine_error, "the maps hook has no sandbox"
                );
                maps_done = false;
                break;
            }
            MapScriptContext context;
            context.mod_id = input.mod.id;
            context.mod_version = input.mod.version;
            context.mod_game = input.mod.game;
            context.engine_version = input.engine_version;
            context.options = &options[slot];
            // Selects this mutator's tally; no record write is granted here.
            collections->begin_mutator(ids[slot], WriteGrant{});
            MapsTurn turn = apply_maps(
                *sandboxes[slot],
                plans[slot].maps_function,
                plans[slot].maps_ref,
                *input.map,
                working,
                store,
                *collections,
                layout,
                context,
                static_cast<uint16_t>(slot)
            );
            const bool over_writes = collections->write_limit_failed();
            collections->end_mutator();
            if (turn.declared) {
                first.offer(slot, Phase::hooks, Cause::declared, std::move(turn.message));
                maps_done = false;
                break;
            }
            if (!turn.ok) {
                const Cause cause = turn.from_script ? cause_of(turn.kind) : Cause::engine_error;
                first.offer(slot, Phase::hooks, cause, std::move(turn.message));
                maps_done = false;
                break;
            }
            if (over_writes) {
                first.offer(
                    slot,
                    Phase::hooks,
                    Cause::script_error,
                    "this mutator has changed too many fields"
                );
                maps_done = false;
                break;
            }
            for (JournalEntry& entry : turn.entries)
                map_journal.push_back(std::move(entry));
            if (turn.changed && turn.bytes != nullptr) {
                working.assign(turn.bytes->begin(), turn.bytes->end());
                served_ota = std::move(turn.bytes);
                served_owner = ids[slot];
            }
        }
        if (!maps_done) {
            sandboxes.clear();
            build_menus.reset();
            collections.reset();
            guard.release();
            fail_mutator(results[first.slot], first.phase, first.cause, std::move(first.message));
            return false;
        }
        if (served_ota != nullptr && *served_ota == original)
            served_ota.reset();
        if (working == original)
            served_ota.reset();
        map_notes = map_conflicts(map_journal);
    } else {
        for (std::size_t slot = 0; slot < count && slot < first.slot; ++slot) {
            if (active[slot] == 0 || !plans[slot].has_map_rules)
                continue;
            rules::RuleNote note;
            note.kind = rules::NoteKind::matched_nothing;
            note.mutators.push_back(ids[slot]);
            note.message = "applies to the chosen map only";
            append_rule_note(report, note, ids, replaced_fields);
        }
    }

    std::vector<std::vector<Writer>> writers(documents.size());
    std::vector<uint8_t> menus(documents.size(), 0);
    for (const rules::FieldWrite& write : rule_writes) {
        journal.entries.push_back(entry_from_write(write, ids, Phase::rules, layout));
        for (const uint16_t mutator : journal.entries.back().mutators) {
            note_writer(
                documents,
                writers,
                menus,
                write.path,
                mutator,
                Phase::rules,
                journal.entries.back().collection
            );
        }
    }
    for (const rules::FieldWrite& write : collections->writes()) {
        journal.entries.push_back(entry_from_write(write, ids, Phase::hooks, layout));
        for (const uint16_t mutator : journal.entries.back().mutators) {
            note_writer(
                documents,
                writers,
                menus,
                write.path,
                mutator,
                Phase::hooks,
                journal.entries.back().collection
            );
        }
    }
    for (const RecordRemoval& removal : collections->removals()) {
        JournalEntry entry;
        const std::optional<uint16_t> mutator = index_of(ids, removal.mutator);
        if (mutator.has_value())
            entry.mutators.push_back(*mutator);
        entry.phase = Phase::hooks;
        entry.change = Change::removed;
        entry.record = removal.name;
        entry.path = removal.path;
        if (removal.kind == RemovalKind::unit)
            entry.collection = Collection::units;
        else if (removal.kind == RemovalKind::weapon)
            entry.collection = Collection::weapons;
        else if (removal.kind == RemovalKind::feature)
            entry.collection = Collection::features;
        else if (removal.kind == RemovalKind::download_entry)
            entry.collection = Collection::menus;
        else
            entry.collection = collection_for_path(removal.path, layout);
        if (mutator.has_value()) {
            note_writer(
                documents, writers, menus, removal.path, *mutator, Phase::hooks, entry.collection
            );
        }
        journal.entries.push_back(std::move(entry));
    }
    const CloneOwners cloned = clone_owners(collections->clones(), ids);
    for (std::size_t clone_index = 0; clone_index < collections->clones().size(); ++clone_index) {
        const CloneRecord& clone = collections->clones()[clone_index];
        JournalEntry entry;
        const std::optional<uint16_t> mutator = index_of(ids, clone.mutator);
        if (mutator.has_value())
            entry.mutators.push_back(*mutator);
        entry.phase = Phase::hooks;
        entry.change = Change::added;
        entry.collection = clone.weapon ? Collection::weapons : Collection::units;
        entry.record = clone.name;
        entry.path = clone.path;
        if (clone.replaced)
            entry.old_text = clone.name;
        else if (!clone.asked.empty())
            entry.old_text = clone.asked;
        if (clone.weapon && !clone.replaced) {
            int assigned = clone.weapon_id;
            const std::size_t owned = brought_index(brought, clone.name);
            if (owned < brought.size() && brought[owned].assigned_id > 0)
                assigned = brought[owned].assigned_id;
            std::string sentence = "added weapon ";
            sentence.append(clone.name);
            sentence.append(" (copy of ");
            sentence.append(clone.source);
            sentence.append(") at id ");
            sentence.append(std::to_string(assigned));
            entry.new_text = std::move(sentence);
        } else {
            entry.new_text = clone.sentence;
        }
        const Collection stored_collection = entry.collection;
        if (mutator.has_value()) {
            note_writer(
                documents, writers, menus, clone.path, *mutator, Phase::hooks, stored_collection
            );
        }
        journal.entries.push_back(std::move(entry));
        if (!mutator.has_value())
            continue;
        if (!clone.asked.empty() && !clone.replaced) {
            Conflict renamed;
            renamed.severity = Severity::note;
            renamed.kind = ConflictKind::record_renamed;
            renamed.mutators.push_back(*mutator);
            renamed.record = clone.asked;
            renamed.detail = clone.name;
            renamed.path = clone.path;
            report.conflicts.push_back(std::move(renamed));
        }
        if (!clone.replaced)
            continue;
        // The record was the latest of: a file of this mutator or one above
        // it, which came before its hooks, and an earlier clone.
        std::optional<uint16_t> earlier;
        for (const RecordTouch& touch : touches) {
            if (touch.fate == RecordFate::shared || touch.collection != stored_collection)
                continue;
            if (touch.mutator > *mutator || !same_text(touch.record, clone.name))
                continue;
            earlier = touch.mutator;
        }
        const std::optional<uint16_t> earlier_cloned = earlier_clone(
            cloned, clone.weapon, clone.name, static_cast<std::size_t>(*mutator) + 1, clone_index
        );
        if (earlier_cloned.has_value() && (!earlier.has_value() || *earlier_cloned >= *earlier))
            earlier = earlier_cloned;
        if (earlier.has_value() && *earlier == *mutator)
            continue;
        Conflict replaced;
        replaced.severity = Severity::note;
        replaced.kind = ConflictKind::value_replaced;
        if (earlier.has_value())
            replaced.mutators.push_back(*earlier);
        replaced.mutators.push_back(*mutator);
        replaced.record = clone.name;
        replaced.path = clone.path;
        if (!earlier.has_value())
            replaced.detail = base_owner(store, input.mod, clone.path);
        report.conflicts.push_back(std::move(replaced));
    }
    // A download file the build menus created is a file the requesting mutator added.
    for (const MenuFile& file : build_menus->added_files()) {
        const std::optional<uint16_t> mutator = index_of(ids, file.mutator);
        if (mutator.has_value())
            journal.entries.push_back(file_entry(*mutator, file.path, Change::added, Phase::hooks));
    }
    journal_record_touches(
        journal, report, touches, documents, writers, menus, cloned, store, input.mod
    );
    for (JournalEntry& entry : map_journal)
        journal.entries.push_back(std::move(entry));
    for (Conflict& conflict : map_notes)
        report.conflicts.push_back(std::move(conflict));

    std::vector<FileNote> file_notes = built.notes;
    if (collections.has_value()) {
        std::string redirect_error;
        if (!redirect_renamed_unit_files(
                spec, store, *collections, layout, ids, built.displaced, file_notes, redirect_error
            )) {
            sandboxes.clear();
            build_menus.reset();
            collections.reset();
            guard.release();
            fail_every(results, active, Phase::files, redirect_error);
            return false;
        }
    }

    for (const oa::MutatorLayerEntry& entry : spec.entries) {
        const std::optional<uint16_t> mutator = index_of(ids, entry.owner);
        if (!mutator.has_value())
            continue;
        const Change change =
            entry.kind == oa::MutatorSourceKind::hidden ? Change::hidden : Change::added;
        journal.entries.push_back(file_entry(*mutator, entry.path, change, Phase::files));
    }
    for (JournalEntry& entry : masked)
        journal.entries.push_back(std::move(entry));
    for (const FileNote& note : file_notes) {
        if (note.kind == FileNoteKind::add_replaced &&
            record_file_kind(note.path, layout) != RecordFileKind::none)
            continue;
        report.conflicts.push_back(conflict_from_file(note, ids));
        if (note.kind != FileNoteKind::add_masked)
            continue;
        const std::optional<uint16_t> masker = index_of(ids, note.mutator);
        if (!masker.has_value())
            continue;
        JournalEntry hidden = file_entry(*masker, note.path, Change::hidden, Phase::files);
        const std::optional<uint16_t> adder = index_of(ids, note.other);
        if (adder.has_value())
            hidden.mutators.insert(hidden.mutators.begin(), *adder);
        journal.entries.push_back(std::move(hidden));
    }

    // The store still reads the files layer without the rules and hooks, so
    // a value the run left alone compares equal and keeps its own text.
    const BaseText base_text = [&store](const std::string& path) -> std::optional<std::string> {
        try {
            const oa::AssetData data = store.read(path);
            return std::string(data.bytes.begin(), data.bytes.end());
        } catch (const std::exception&) {
            return std::nullopt;
        }
    };
    const std::vector<HeldField> held = hold_narrow_fields(documents, layout, base_text);
    for (const HeldField& field : held) {
        Conflict note;
        note.severity = Severity::note;
        note.kind = ConflictKind::value_held;
        note.record = field.record;
        note.field = field.field;
        note.path = field.path;
        note.detail = field.from_text;
        note.detail.push_back('\n');
        note.detail += field.to_text;
        report.conflicts.push_back(std::move(note));
        JournalEntry* last = nullptr;
        for (JournalEntry& entry : journal.entries) {
            if (!same_path(entry.path, field.path) || !same_text(entry.field, field.field))
                continue;
            if (!field.record.empty() && !entry.record.empty() &&
                !same_text(entry.record, field.record))
                continue;
            last = &entry;
            if (entry.new_text.has_value() && *entry.new_text == field.from_text)
                entry.new_text = field.to_text;
        }
        if (last != nullptr && last->new_text.has_value() && *last->new_text != field.to_text)
            last->new_text = field.to_text;
    }

    for (std::size_t slot = 0; slot < documents.size(); ++slot) {
        if (!documents.changed(slot))
            continue;
        const rules::LoadedDocument& loaded = documents.at(slot);
        if (writers[slot].empty()) {
            for (std::size_t candidate = 0; candidate < count; ++candidate) {
                if (active[candidate] == 0)
                    continue;
                first.offer(
                    candidate, Phase::hooks, Cause::engine_error, "a changed document has no writer"
                );
                break;
            }
            continue;
        }
        const PathVerdict verdict = check_mutable_path(loaded.path, paths);
        const bool menus_pass = verdict.rule == PathRule::menus_only && menus[slot] != 0;
        if (!verdict.allowed() && !menus_pass) {
            for (const Writer& writer : writers[slot])
                first.offer(writer.mutator, writer.phase, Cause::protected_path, verdict.message);
            continue;
        }
        const uint16_t owner = owner_of(writers[slot]);
        const Phase owner_phase = phase_of(writers[slot], owner);
        oa::MutatorLayerEntry entry;
        entry.path = loaded.path;
        entry.owner = ids[owner];
        if (loaded.removed) {
            entry.kind = oa::MutatorSourceKind::hidden;
            journal.entries.push_back(file_entry(owner, loaded.path, Change::hidden, owner_phase));
        } else {
            const std::optional<std::string> text = tdf::write_text(loaded.document.get());
            if (!text.has_value()) {
                first.offer(
                    owner,
                    owner_phase,
                    Cause::engine_error,
                    "a changed document could not be written"
                );
                continue;
            }
            entry.kind = oa::MutatorSourceKind::contents;
            entry.contents =
                std::make_shared<const std::vector<uint8_t>>(text->begin(), text->end());
        }
        bool replaced = false;
        for (std::size_t entry_index = 0; entry_index < spec.entries.size(); ++entry_index) {
            if (!same_path(spec.entries[entry_index].path, loaded.path))
                continue;
            spec.entries[entry_index] = entry;
            replaced = true;
            break;
        }
        if (!replaced)
            spec.entries.push_back(std::move(entry));
    }
    // Each served path goes through check_mutable_path. A unit cloned this
    // pass counts as a unit name, so anims/<new><digits>.gaf is allowed.
    // guis/<new><digits>.gui stays menus-only and is served only when that
    // path does not already exist. An existing path refuses the clone and
    // names the path; the entry is not published.
    PathContext copy_paths;
    copy_paths.layout = &layout;
    copy_paths.is_unit_name = [&stems, &collections](std::string_view name) {
        const std::string folded = fold_copy(name);
        if (std::binary_search(stems.begin(), stems.end(), folded))
            return true;
        if (!collections.has_value())
            return false;
        for (const CloneRecord& record : collections->clones()) {
            if (!record.weapon && clone_names_unit(name, record.name))
                return true;
        }
        for (const Collections::UnitFileShift& shift : collections->unit_file_shifts()) {
            if (clone_names_unit(name, shift.stored))
                return true;
        }
        return false;
    };
    for (const ServedCopy& copy : collections->served_copies()) {
        const PathVerdict verdict = check_mutable_path(copy.path, copy_paths);
        // A Replace clone's copy takes the place of the replaced unit's own file.
        const bool exists = !copy.replaces && served_path_exists(store, documents, spec, copy.path);
        const bool fresh_page = fresh_clone_page(
            copy.path, *collections, layout, store, documents, spec, verdict, copy.replaces
        );
        if (exists || (!verdict.allowed() && !fresh_page)) {
            std::string message = verdict.message;
            if (exists) {
                message = copy.path;
                message.append(": the path already exists");
            }
            const std::optional<uint16_t> owner = index_of(ids, copy.mutator);
            const uint16_t slot = owner.has_value() ? *owner : static_cast<uint16_t>(0);
            first.offer(slot, Phase::hooks, Cause::protected_path, std::move(message));
            continue;
        }

        JournalEntry added;
        const std::optional<uint16_t> mutator = index_of(ids, copy.mutator);
        if (mutator.has_value())
            added.mutators.push_back(*mutator);
        added.phase = Phase::hooks;
        added.collection = Collection::files;
        added.change = Change::added;
        added.path = copy.path;
        added.new_text = copy.sentence;
        journal.entries.push_back(std::move(added));

        oa::MutatorLayerEntry served;
        served.path = copy.path;
        served.owner = copy.mutator;
        served.kind = oa::MutatorSourceKind::contents;
        served.contents = copy.bytes;
        bool replaced_copy = false;
        for (std::size_t entry_index = 0; entry_index < spec.entries.size(); ++entry_index) {
            if (!same_path(spec.entries[entry_index].path, served.path))
                continue;
            const std::string& held_by = spec.entries[entry_index].owner;
            if (!same_text(held_by, served.owner) && index_of(ids, held_by).has_value()) {
                FileNote note;
                note.kind = FileNoteKind::add_replaced;
                note.path = served.path;
                note.mutator = served.owner;
                note.other = held_by;
                report.conflicts.push_back(conflict_from_file(note, ids));
            }
            spec.entries[entry_index] = served;
            replaced_copy = true;
            break;
        }
        if (!replaced_copy)
            spec.entries.push_back(std::move(served));
    }
    sandboxes.clear();
    build_menus.reset();
    collections.reset();
    if (served_ota != nullptr && input.map.has_value()) {
        oa::MutatorLayerEntry served;
        served.path = input.map->ota_path;
        served.owner = served_owner;
        served.kind = oa::MutatorSourceKind::contents;
        served.contents = served_ota;
        bool replaced_ota = false;
        for (std::size_t entry_index = 0; entry_index < spec.entries.size(); ++entry_index) {
            if (!same_path(spec.entries[entry_index].path, served.path))
                continue;
            spec.entries[entry_index] = served;
            replaced_ota = true;
            break;
        }
        if (!replaced_ota)
            spec.entries.push_back(std::move(served));
    }

    // The journal is complete, and `spec` holds every file this pass would
    // serve, including a model copied for a renamed unit. Nothing has been
    // published yet: a failure below leaves the store unmounted.
    journal.order();
    const std::string& gui_folder =
        layout.directories[static_cast<std::size_t>(defs::DataDirectory::guis)];
    const auto holds = [&](std::string_view path) {
        // A changed build page is already a contents entry in this pass's
        // spec, at its own path, so the spec would always say the page is
        // held. The check asks whether the store held that page.
        if (path_in_folder(path, gui_folder))
            return store.file_size(path) > 0;
        bool staged = false;
        bool present = false;
        for (const oa::MutatorLayerEntry& entry : spec.entries) {
            if (!same_path(entry.path, path))
                continue;
            staged = true;
            if (entry.kind == oa::MutatorSourceKind::hidden)
                present = false;
            else if (entry.kind == oa::MutatorSourceKind::contents)
                present = entry.contents != nullptr && !entry.contents->empty();
            else
                present = true;
        }
        if (staged)
            return present;
        return store.file_size(path) > 0;
    };
    // A pass that has already failed is not published, and its documents
    // still hold the failed mutator's changes, so a problem found in them can
    // be that mutator's doing. The next pass runs without it and checks then.
    // A caller that turns the check off, such as the mutator diff, still
    // publishes the layer.
    std::vector<IntegrityIssue> fresh;
    if (!first.found() && input.check_integrity) {
        const std::vector<IntegrityIssue> found =
            integrity_issues(documents, layout, input.limits, holds);
        Journal blamed = journal;
        note_hidden_records(blamed, base_names);
        fresh = new_issues(base_issues, found, blamed, layout);
    }
    std::stable_sort(
        fresh.begin(), fresh.end(), [](const IntegrityIssue& left, const IntegrityIssue& right) {
            const uint32_t left_slot = left.mutator.has_value() ? *left.mutator : 0xffffu;
            const uint32_t right_slot = right.mutator.has_value() ? *right.mutator : 0xffffu;
            if (left_slot != right_slot)
                return left_slot < right_slot;
            return static_cast<uint8_t>(left.problem) < static_cast<uint8_t>(right.problem);
        }
    );
    uint16_t last_active = 0;
    bool any_active = false;
    for (std::size_t slot = 0; slot < count; ++slot) {
        if (active[slot] == 0)
            continue;
        last_active = static_cast<uint16_t>(slot);
        any_active = true;
    }
    for (const IntegrityIssue& issue : fresh) {
        if (issue.severity == Severity::warning) {
            Conflict conflict;
            conflict.severity = Severity::warning;
            conflict.kind = ConflictKind::integrity_warning;
            if (issue.mutator.has_value())
                conflict.mutators.push_back(*issue.mutator);
            conflict.record = issue.record;
            conflict.field = issue.field;
            conflict.path = issue.path;
            conflict.detail = issue.message;
            report.conflicts.push_back(std::move(conflict));
            continue;
        }
        if (issue.severity != Severity::failure)
            continue;
        uint16_t blamed = last_active;
        if (issue.mutator.has_value() && *issue.mutator < count && active[*issue.mutator] != 0)
            blamed = *issue.mutator;
        else if (!any_active)
            continue;
        first.offer(blamed, Phase::integrity, Cause::integrity, issue.message);
    }
    if (first.found()) {
        guard.release();
        fail_mutator(results[first.slot], first.phase, first.cause, std::move(first.message));
        return false;
    }

    spec.label = "mutators";
    for (std::size_t slot = 0; slot < count; ++slot) {
        if (results[slot].outcome != Outcome::applied)
            continue;
        spec.label.push_back(' ');
        spec.label += ids[slot];
        results[slot].phase = Phase::hooks;
        results[slot].cause = Cause::none;
        results[slot].message.clear();
    }
    journal.order();
    if (spec.entries.empty() && spec.masks.empty()) {
        guard.release();
        layer_spec = std::move(spec);
        return true;
    }
    std::string publish_error;
    const bool published = store.mutator_layer_mounted()
                               ? store.replace_mutator_layer(spec, &publish_error)
                               : store.mount_mutator_layer(spec, &publish_error);
    if (!published) {
        fail_every(
            results,
            active,
            Phase::files,
            publish_error.empty() ? "the mutator layer could not be mounted" : publish_error
        );
        guard.release();
        return false;
    }
    guard.disarm();
    layer_spec = std::move(spec);
    return true;
}

PipelineResult run_checked(oa::AssetStore& store, const PipelineInput& input) {
    PipelineResult result;
    const std::size_t count = input.mutators.size();
    result.mutators.resize(count);
    std::vector<std::string> ids(count);
    std::vector<std::string> versions(count);
    for (std::size_t slot = 0; slot < count; ++slot) {
        ids[slot] = input.mutators[slot].manifest.id;
        versions[slot] = oamut::version_text(input.mutators[slot].manifest.version);
        result.mutators[slot].id = ids[slot];
        result.mutators[slot].version = versions[slot];
    }
    if (store.mutator_layer_mounted()) {
        refuse_list(result, "a mutator layer is already mounted");
        return result;
    }
    const std::string refusal = list_refusal(input.mutators, versions);
    if (!refusal.empty()) {
        refuse_list(result, refusal);
        return result;
    }
    for (std::size_t slot = 0; slot < count; ++slot) {
        result.mutators[slot].outcome = Outcome::applied;
        result.mutators[slot].phase = Phase::manifest;
        result.mutators[slot].cause = Cause::none;
        const oamut::Manifest& manifest = input.mutators[slot].manifest;
        if (!fits_mod(manifest, input.mod)) {
            result.mutators[slot].outcome = Outcome::incompatible;
            result.mutators[slot].cause = Cause::works_with;
            result.mutators[slot].message =
                ids[slot] + " works with " + works_with_list(manifest) + ", not " + input.mod.id;
            continue;
        }
        if (!fits_engine(manifest, input.engine_version)) {
            result.mutators[slot].outcome = Outcome::incompatible;
            result.mutators[slot].cause = Cause::engine_version;
            result.mutators[slot].message = ids[slot] + " needs engine " +
                                            manifest.requires_engine + ", not " +
                                            input.engine_version;
        }
    }
    exclude_blocked(result.mutators, input.mutators, versions);
    const defs::DataLayout& layout = layout_of(input);
    // Once, before any pass mounts the files layer. A file a mutator adds is
    // not part of this parse, and a later pass reuses it. Rule targets are
    // not included: they are known only after a plan, inside the pass.
    // The mutator diff leaves the check off and skips this parse.
    std::vector<IntegrityIssue> base_issues;
    std::vector<BaseNames> base_names;
    if (input.check_integrity) {
        rules::DocumentSet base_documents;
        ConflictReport base_unread;
        parse_documents(base_documents, base_unread, store, layout, input.variant, {});
        base_issues =
            integrity_issues(base_documents, layout, input.limits, [&store](std::string_view path) {
                return store.file_size(path) > 0;
            });
        base_names = names_defined_by(base_documents, layout);
    }
    MountGuard guard{store, false};
    const std::size_t limit = count + 1;
    for (std::size_t pass = 0; pass < limit; ++pass) {
        bool any_applied = false;
        for (const MutatorResult& mutator : result.mutators) {
            if (mutator.outcome == Outcome::applied)
                any_applied = true;
        }
        if (!any_applied) {
            guard.release();
            result.layer = {};
            result.journal = {};
            result.report = {};
            break;
        }
        const std::size_t before = [&]() {
            std::size_t dropped = 0;
            for (const MutatorResult& mutator : result.mutators) {
                if (mutator.outcome != Outcome::applied)
                    ++dropped;
            }
            return dropped;
        }();
        Journal journal;
        ConflictReport report;
        oa::MutatorLayerSpec spec;
        const bool clean = run_pass(
            store,
            input,
            layout,
            ids,
            result.mutators,
            journal,
            report,
            spec,
            guard,
            base_issues,
            base_names
        );
        if (clean) {
            result.journal = std::move(journal);
            result.report = std::move(report);
            result.layer = std::move(spec);
            break;
        }
        result.journal = {};
        result.report = {};
        result.layer = {};
        exclude_blocked(result.mutators, input.mutators, versions);
        std::size_t after = 0;
        for (const MutatorResult& mutator : result.mutators) {
            if (mutator.outcome != Outcome::applied)
                ++after;
        }
        if (after == before) {
            for (MutatorResult& mutator : result.mutators) {
                fail_mutator(
                    mutator,
                    Phase::files,
                    Cause::engine_error,
                    "the pipeline could not drop a failed mutator"
                );
            }
        }
        guard.release();
    }
    return result;
}

/// What `hack_refusal_only` has found for one slot.
enum class Judged : uint8_t {
    unknown, ///< not asked yet
    judging, ///< being judged, further up the same chain of requirements
    yes,     ///< its failure is that mutator's alone
    no,      ///< its failure stops the start
};

/// True when this mutator failed because its hacks were refused, or was
/// excluded only because a mutator it requires failed that way, directly
/// or through another exclusion.
///
/// Each slot's answer is kept in `judged`, so a mutator that several others
/// require gives every one of them the same answer. A slot met again while
/// it is being judged would be a loop of requirements, which the list check
/// refuses before the pipeline runs; it counts as no.
bool hack_refusal_only(
    std::size_t slot,
    const std::vector<MutatorResult>& results,
    std::span<const PipelineMutator> listed,
    std::vector<Judged>& judged
) {
    if (slot >= results.size() || slot >= judged.size())
        return false;
    if (judged[slot] != Judged::unknown)
        return judged[slot] == Judged::yes;
    const MutatorResult& one = results[slot];
    if (one.outcome == Outcome::failed) {
        const bool alone = one.cause == Cause::hack_refused;
        judged[slot] = alone ? Judged::yes : Judged::no;
        return alone;
    }
    if (one.outcome != Outcome::excluded || one.cause != Cause::prerequisite_failed ||
        slot >= listed.size()) {
        judged[slot] = Judged::no;
        return false;
    }
    judged[slot] = Judged::judging;
    bool unmet = false;
    for (const oamut::Condition& condition : listed[slot].manifest.requires_mutators) {
        bool satisfied = false;
        std::vector<std::size_t> failed;
        for (std::size_t other = 0; other < results.size() && other < listed.size(); ++other) {
            if (other == slot)
                continue;
            const std::string version = oamut::version_text(listed[other].manifest.version);
            if (!oamut::condition_met(condition, results[other].id, version))
                continue;
            if (results[other].outcome == Outcome::applied) {
                satisfied = true;
                break;
            }
            failed.push_back(other);
        }
        if (satisfied)
            continue;
        if (failed.empty())
            continue;
        unmet = true;
        for (const std::size_t other : failed) {
            if (!hack_refusal_only(other, results, listed, judged)) {
                judged[slot] = Judged::no;
                return false;
            }
        }
    }
    judged[slot] = unmet ? Judged::yes : Judged::no;
    return unmet;
}

} // namespace

std::string
list_refusal(const std::vector<PipelineMutator>& listed, const std::vector<std::string>& versions) {
    if (listed.size() > max_mutators) {
        return "a loadout lists " + std::to_string(listed.size()) + " mutators, and " +
               std::to_string(max_mutators) + " is the most";
    }
    for (std::size_t slot = 0; slot < listed.size(); ++slot) {
        for (std::size_t earlier = 0; earlier < slot; ++earlier) {
            if (listed[slot].manifest.id == listed[earlier].manifest.id)
                return listed[slot].manifest.id + " is listed twice";
        }
    }
    for (std::size_t slot = 0; slot < listed.size(); ++slot) {
        for (const oamut::Condition& condition : listed[slot].manifest.requires_mutators) {
            bool met = false;
            for (std::size_t earlier = 0; earlier < slot; ++earlier) {
                if (oamut::condition_met(condition, listed[earlier].manifest.id, versions[earlier]))
                    met = true;
            }
            if (!met) {
                std::string message = listed[slot].manifest.id + " needs " + condition.id;
                if (!condition.range_text.empty()) {
                    message += " ";
                    message += condition.range_text;
                }
                message += " above it";
                return message;
            }
        }
    }
    for (std::size_t slot = 0; slot < listed.size(); ++slot) {
        for (const oamut::Condition& condition : listed[slot].manifest.conflicts_with) {
            for (std::size_t other = 0; other < listed.size(); ++other) {
                if (other == slot)
                    continue;
                if (oamut::condition_met(condition, listed[other].manifest.id, versions[other]))
                    return listed[slot].manifest.id + " conflicts with " +
                           listed[other].manifest.id;
            }
        }
    }
    return {};
}

bool PipelineResult::playable() const noexcept {
    if (!refusal.empty())
        return false;
    for (const MutatorResult& mutator : mutators) {
        if (mutator.outcome != Outcome::applied)
            return false;
    }
    return true;
}

std::optional<std::size_t>
first_blocking(const PipelineResult& result, std::span<const PipelineMutator> listed) {
    std::vector<Judged> judged(result.mutators.size(), Judged::unknown);
    for (std::size_t slot = 0; slot < result.mutators.size(); ++slot) {
        if (result.mutators[slot].outcome == Outcome::applied)
            continue;
        if (!hack_refusal_only(slot, result.mutators, listed, judged))
            return slot;
    }
    return std::nullopt;
}

PipelineResult run_pipeline(oa::AssetStore& store, const PipelineInput& input) {
    try {
        return run_checked(store, input);
    } catch (const std::bad_alloc&) {
        PipelineResult result;
        try {
            result.refusal = "the pipeline ran out of memory";
        } catch (const std::bad_alloc&) {
        }
        return result;
    } catch (const std::exception& ex) {
        PipelineResult result;
        try {
            result.refusal = ex.what() != nullptr ? ex.what() : "the pipeline stopped";
        } catch (const std::bad_alloc&) {
        }
        return result;
    } catch (...) {
        PipelineResult result;
        try {
            result.refusal = "the pipeline stopped";
        } catch (const std::bad_alloc&) {
        }
        return result;
    }
}

} // namespace oa::data::mutators
