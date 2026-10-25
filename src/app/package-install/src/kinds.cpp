// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The kinds of package the game installs, and the names and folder reads
// each kind shares: oamod, oamap, oalang, expansion, oamut, oabrain and oakeys.

#include "files.hpp"

#include "oa/app/content_copy.hpp"
#include "oa/app/package_install.hpp"
#include "oa/app/package_install/oalang.hpp"
#include "oa/app/package_install/oamap.hpp"
#include "oa/app/package_install/oamod.hpp"
#include "oa/app/package_install/oamut.hpp"
#include "oa/app/package_install/oabrain.hpp"
#include "oa/app/package_install/oaexp.hpp"
#include "oa/app/package_install/oakeys.hpp"
#include "oa/data/brain_library/library.hpp"
#include "oa/data/key_library/library.hpp"
#include "oa/data/mutator_library/library.hpp"
#include "oa/app/package_install/origin.hpp"
#include "oa/app/package_install/prompts.hpp"
#include "oa/app/user_folder.hpp"
#include "oa/base/threads.hpp"
#include "oa/formats/oamod.hpp"

#include <list>
#include <string>
#include <string_view>
#include <utility>

namespace oa::app::package_install {

static_assert(oamut::mutators_folder_name == oa::data::mutator_library::mutators_folder_name);
static_assert(oabrain::brains_folder_name == oa::data::brain_library::brains_folder_name);
static_assert(oakeys::keys_folder_name == oa::data::key_library::keys_folder_name);

namespace fs = std::filesystem;

namespace {

/// A kind remembered by its prefix, so a folder it left can be settled after
/// the kind's own object is gone. The list keeps each node where it is, and
/// the kind's prefix view points at the node's text.
struct Noted {
    std::string prefix{};
    PackageKind kind{};
};

base::threads::Mutex& noted_mutex() {
    static base::threads::Mutex mutex;
    return mutex;
}

std::list<Noted>& noted_kinds() {
    static std::list<Noted> kinds;
    return kinds;
}

constexpr KindPrompts kModPrompts{
    .installing = &oamod::installing_prompt,
    .question = &oamod::question_prompt,
    .installed = &oamod::installed_prompt,
    .updated = &oamod::updated_prompt,
    .refusal_text = &oamod::refusal_text,
    .refused = &oamod::refused_prompt,
};

constexpr KindPrompts kMapPrompts{
    .installing = &oamap::installing_prompt,
    .question = &oamap::question_prompt,
    .installed = &oamap::installed_prompt,
    .updated = &oamap::updated_prompt,
    .refusal_text = &oamap::refusal_text,
    .refused = &oamap::refused_prompt,
};

constexpr KindPrompts kLanguagePrompts{
    .installing = &oalang::installing_prompt,
    .question = &oalang::question_prompt,
    .installed = &oalang::installed_prompt,
    .updated = &oalang::updated_prompt,
    .refusal_text = &oalang::refusal_text,
    .refused = &oalang::refused_prompt,
};

constexpr KindPrompts kExpansionPrompts{
    .installing = &oaexp::installing_prompt,
    .question = &oaexp::question_prompt,
    .installed = &oaexp::installed_prompt,
    .updated = &oaexp::updated_prompt,
    .refusal_text = &oaexp::refusal_text,
    .refused = &oaexp::refused_prompt,
};

constexpr KindPrompts kMutatorPrompts{
    .installing = &oamut::installing_prompt,
    .question = &oamut::question_prompt,
    .installed = &oamut::installed_prompt,
    .updated = &oamut::updated_prompt,
    .refusal_text = &oamut::refusal_text,
    .refused = &oamut::refused_prompt,
};

constexpr KindPrompts kBrainPrompts{
    .installing = &oabrain::installing_prompt,
    .question = &oabrain::question_prompt,
    .installed = &oabrain::installed_prompt,
    .updated = &oabrain::updated_prompt,
    .refusal_text = &oabrain::refusal_text,
    .refused = &oabrain::refused_prompt,
};

constexpr KindPrompts kOakeysPrompts{
    .installing = &oakeys::installing_prompt,
    .question = &oakeys::question_prompt,
    .installed = &oakeys::installed_prompt,
    .updated = &oakeys::updated_prompt,
    .refusal_text = &oakeys::refusal_text,
    .refused = &oakeys::refused_prompt,
};

constexpr PackageKind kKinds[] = {
    {
        .name = "oamod",
        .extension = ".oamod",
        .manifest = "oamod.yaml",
        .manifest_most_bytes = oa::formats::oamod::max_input_bytes,
        .root_folder = user_mods_folder_name,
        .prefix = ".oamod-",
        .read_manifest = &oamod::read_manifest,
        .read_installed = &oamod::read_installed_mod,
        .read_backup = &oamod::read_backup,
        .plan = &oamod::plan_install,
        .check_staged = nullptr,
        .prompts = &kModPrompts,
    },
    {
        .name = "oamap",
        .extension = oamap::oamap_extension,
        .manifest = "oamap.yaml",
        .manifest_most_bytes = oa::formats::oamod::max_input_bytes,
        .root_folder = oamap::maps_folder_name,
        .prefix = ".oamap-",
        .read_manifest = &oamap::read_manifest,
        .read_installed = &oamap::read_installed,
        .read_backup = &oamap::read_backup,
        .plan = &oamap::plan_install,
        .check_staged = &oamap::check_staged,
        .prompts = &kMapPrompts,
    },
    {
        .name = "oalang",
        .extension = oalang::oalang_extension,
        .manifest = "language.yaml",
        .manifest_most_bytes = oa::formats::oamod::max_input_bytes,
        .root_folder = oalang::languages_folder_name,
        .prefix = ".oalang-",
        .read_manifest = &oalang::read_manifest,
        .read_installed = &oalang::read_installed,
        .read_backup = &oalang::read_backup,
        .plan = &oalang::plan_install,
        .check_staged = &oalang::check_staged,
        .prompts = &kLanguagePrompts,
    },
    {
        .name = "expansion",
        .extension = oaexp::oaexp_extension,
        .manifest = oaexp::manifest_file_name,
        .manifest_most_bytes = oa::formats::oamod::max_input_bytes,
        .root_folder = managed_expansions_folder,
        .prefix = ".oaexp-",
        .read_manifest = &oaexp::read_manifest,
        .read_installed = &oaexp::read_installed,
        .read_backup = &oaexp::read_backup,
        .plan = &oaexp::plan_install,
        .check_staged = nullptr,
        .prompts = &kExpansionPrompts,
    },
    {
        .name = "oamut",
        .extension = oamut::oamut_extension,
        .manifest = "oamut.yaml",
        .manifest_most_bytes = oa::formats::oamod::max_input_bytes,
        .root_folder = oamut::mutators_folder_name,
        .prefix = ".oamut-",
        .read_manifest = &oamut::read_manifest,
        .read_installed = &oamut::read_installed,
        .read_backup = &oamut::read_backup,
        .plan = &oamut::plan_install,
        .check_staged = nullptr,
        .prompts = &kMutatorPrompts,
    },
    {
        .name = "oabrain",
        .extension = oabrain::oabrain_extension,
        .manifest = "oabrain.yaml",
        .manifest_most_bytes = oa::formats::oamod::max_input_bytes,
        .root_folder = oabrain::brains_folder_name,
        .prefix = ".oabrain-",
        .read_manifest = &oabrain::read_manifest,
        .read_installed = &oabrain::read_installed,
        .read_backup = &oabrain::read_backup,
        .plan = &oabrain::plan_install,
        .check_staged = &oabrain::check_staged,
        .prompts = &kBrainPrompts,
    },
    {
        .name = "oakeys",
        .extension = oakeys::oakeys_extension,
        .manifest = "oakeys.yaml",
        .manifest_most_bytes = oa::formats::oamod::max_input_bytes,
        .root_folder = oakeys::keys_folder_name,
        .prefix = ".oakeys-",
        .read_manifest = &oakeys::read_manifest,
        .read_installed = &oakeys::read_installed,
        .read_backup = &oakeys::read_backup,
        .plan = &oakeys::plan_install,
        .check_staged = nullptr,
        .prompts = &kOakeysPrompts,
    },
};

/// The root a folder_hooks reads, kept with the hooks.
struct Binding {
    const PackageKind* kind{};
    fs::path root{};
};

} // namespace

namespace detail {

void note_kind(const PackageKind& kind) {
    const base::threads::LockGuard guard(noted_mutex());
    for (const Noted& noted : noted_kinds())
        if (noted.prefix == kind.prefix)
            return;
    Noted& noted = noted_kinds().emplace_back();
    noted.prefix = std::string(kind.prefix);
    noted.kind = kind;
    noted.kind.prefix = noted.prefix;
}

const PackageKind* kind_of_reserved(std::string_view name) {
    const PackageKind* found = nullptr;
    const auto consider = [&](const PackageKind& kind) {
        if (!kind.prefix.empty() && name.starts_with(kind.prefix) &&
            (found == nullptr || kind.prefix.size() > found->prefix.size()))
            found = &kind;
    };
    for (const PackageKind& kind : kKinds)
        consider(kind);
    const base::threads::LockGuard guard(noted_mutex());
    for (const Noted& noted : noted_kinds())
        consider(noted.kind);
    return found;
}

} // namespace detail

std::span<const PackageKind> package_kinds() noexcept {
    return kKinds;
}

const PackageKind* kind_for_file(const fs::path& file) noexcept {
    const std::string name = detail::folded(detail::utf8_of(file.filename()));
    const PackageKind* found = nullptr;
    for (const PackageKind& kind : kKinds) {
        const std::string extension = detail::folded(kind.extension);
        if (extension.empty() || name.size() <= extension.size() || !name.ends_with(extension))
            continue;
        if (found == nullptr || kind.extension.size() > found->extension.size())
            found = &kind;
    }
    return found;
}

const PackageKind* find_kind(std::string_view name) noexcept {
    for (const PackageKind& kind : kKinds)
        if (kind.name == name)
            return &kind;
    return nullptr;
}

const PackageKind& mod_kind() noexcept {
    return kKinds[0];
}

FolderNames folder_names(const PackageKind& kind) {
    const std::string prefix(kind.prefix);
    FolderNames names{};
    names.reserved = prefix;
    names.staging = prefix + "staging-";
    names.old = prefix + "old-";
    names.replaced = prefix + "replaced-";
    names.restore = prefix + "restore-";
    names.discard = prefix + "discard-";
    names.lock = prefix + "lock";
    return names;
}

FolderHooks folder_hooks(const PackageKind& kind, const fs::path& root) {
    const auto binding = std::make_shared<Binding>();
    binding->kind = &kind;
    binding->root = root;
    FolderHooks hooks{};
    hooks.owned = binding;
    hooks.context = binding.get();
    hooks.look = [](void* context, std::string_view folder) {
        const auto& bound = *static_cast<const Binding*>(context);
        if (bound.kind == nullptr || bound.kind->read_installed == nullptr)
            return InstalledPackage{};
        const fs::path path = bound.root / detail::path_of(folder);
        InstalledPackage held = bound.kind->read_installed(path);
        if (held.kind == FolderKind::package)
            held.origin = read_origin(path);
        return held;
    };
    hooks.backup_of = [](void* context, std::string_view folder) {
        const auto& bound = *static_cast<const Binding*>(context);
        if (bound.kind == nullptr || bound.kind->read_backup == nullptr)
            return std::optional<InstalledPackage>{};
        const fs::path path = bound.root / detail::path_of(folder);
        std::optional<InstalledPackage> held = bound.kind->read_backup(path);
        if (held && held->kind == FolderKind::package) {
            if (const auto kept = oa::app::entry_without_case(path, backup_folder_name))
                held->origin = read_origin(*kept);
        }
        return held;
    };
    return hooks;
}

} // namespace oa::app::package_install
