// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The built-in bundles. The file table is bundle_table.inc, generated from
// the names, sizes and SHA-256s of the 3.1c install and the three Commander
// Pack installers. Further rows are printed by tools/bundle_table_entries.py
// from an install; only names, sizes and SHA-256s are kept. The demo's
// unpacked archive uses the size and SHA-256 that demo_1997 already checks;
// the installer's own size, hash and resource stay in demo_installer.hpp.
#include "oa/data/content_bundles.hpp"

#include <stddef.h>

namespace oa::data::content_bundles {
namespace {

/// Folds a path byte: ASCII capitals to lower case, and `\` to `/`.
///
/// @param byte a byte of a path
/// @return the folded byte
[[nodiscard]] constexpr char fold_byte(char byte) noexcept {
    if (byte == '\\')
        return '/';
    if (byte >= 'A' && byte <= 'Z')
        return static_cast<char>(byte - 'A' + 'a');
    return byte;
}

/// Reports whether two paths are the same without regard to ASCII case or slash direction.
///
/// @param left a path
/// @param right another path
/// @return true when they fold to the same bytes
[[nodiscard]] constexpr bool same_path(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size())
        return false;
    for (size_t index = 0; index < left.size(); ++index) {
        if (fold_byte(left[index]) != fold_byte(right[index]))
            return false;
    }
    return true;
}

/// Returns the last name of a path.
///
/// @param path a path, with `/` or `\`
/// @return the part after the last separator, or the whole path
[[nodiscard]] constexpr std::string_view last_name(std::string_view path) noexcept {
    size_t start = 0;
    for (size_t index = 0; index < path.size(); ++index) {
        if (path[index] == '/' || path[index] == '\\')
            start = index + 1;
    }
    return path.substr(start);
}

/// Reports whether a path names a directory.
///
/// @param path a path
/// @return true when it holds `/` or `\`
[[nodiscard]] constexpr bool has_directory(std::string_view path) noexcept {
    for (char byte : path) {
        if (byte == '/' || byte == '\\')
            return true;
    }
    return false;
}

#include "bundle_table.inc"

/// Returns the table index of a bundle id.
///
/// @param id a bundle id
/// @return the index, or -1 when the id is not in the table
[[nodiscard]] constexpr int bundle_index(std::string_view id) noexcept {
    for (size_t index = 0; index < k_bundle_count; ++index) {
        if (k_bundles[index].id == id)
            return static_cast<int>(index);
    }
    return -1;
}

/// Returns how many bundles one bundle needs directly.
///
/// @param bundle a bundle
/// @return the count of games it extends plus the expansions it requires
[[nodiscard]] constexpr size_t direct_need_count(const Bundle& bundle) noexcept {
    return bundle.extends.size() + bundle.needs.size();
}

/// Returns one direct need of a bundle.
///
/// The games it extends come first, then the expansions it requires.
///
/// @param bundle a bundle
/// @param slot which need, below direct_need_count
/// @return that bundle's id
[[nodiscard]] constexpr std::string_view direct_need(const Bundle& bundle, size_t slot) noexcept {
    if (slot < bundle.extends.size())
        return bundle.extends[slot];
    return bundle.needs[slot - bundle.extends.size()];
}

/// Follows direct needs and reports a cycle.
///
/// @param index a bundle's table index
/// @param colour 0 unseen, 1 on the path, 2 finished, one per bundle
/// @param depth how many needs have been followed
/// @return true when a need reaches a bundle already on the path
[[nodiscard]] constexpr bool needs_cycle(int index, uint8_t* colour, int depth) noexcept {
    if (depth > static_cast<int>(k_bundle_count))
        return true;
    if (colour[index] == 1)
        return true;
    if (colour[index] == 2)
        return false;
    colour[index] = 1;
    const Bundle& bundle = k_bundles[index];
    for (size_t slot = 0; slot < direct_need_count(bundle); ++slot) {
        const int next = bundle_index(direct_need(bundle, slot));
        if (next >= 0 && needs_cycle(next, colour, depth + 1))
            return true;
    }
    colour[index] = 2;
    return false;
}

/// Reports whether a file name is one the bundles leave out.
///
/// @param name a copy name or a source name
/// @return true for the installer archive and the sample unit file
[[nodiscard]] constexpr bool left_out_name(std::string_view name) noexcept {
    const std::string_view last = last_name(name);
    return same_path(last, "totala3.hpi") || same_path(last, "Example.ufo");
}

/// Returns one of a file's names. Slot 0 is the copy name; later slots are source names.
///
/// @param file a file
/// @param slot which name, below one plus the source-name count
/// @return that name
[[nodiscard]] constexpr std::string_view
file_name_at(const ContentFile& file, size_t slot) noexcept {
    if (slot == 0)
        return file.copy_name;
    return file.source_names[slot - 1];
}

/// Reports whether two files would match the same path and size.
///
/// @param left a file
/// @param right another file
/// @return true when some name of each folds to the same path, or the same last name, and the sizes match
[[nodiscard]] constexpr bool
files_collide(const ContentFile& left, const ContentFile& right) noexcept {
    if (left.size != right.size)
        return false;
    const size_t left_count = 1 + left.source_names.size();
    const size_t right_count = 1 + right.source_names.size();
    for (size_t left_slot = 0; left_slot < left_count; ++left_slot) {
        const std::string_view left_name = file_name_at(left, left_slot);
        for (size_t right_slot = 0; right_slot < right_count; ++right_slot) {
            const std::string_view right_name = file_name_at(right, right_slot);
            if (same_path(left_name, right_name) ||
                same_path(last_name(left_name), last_name(right_name)))
                return true;
        }
    }
    return false;
}

/// Checks one file's names against the files already accepted.
///
/// @param file the file
/// @param earlier_bundles bundles before this file's bundle
/// @param earlier_files files before this one in its bundle
/// @return empty, or the first fault
[[nodiscard]] constexpr std::string_view
file_names_fault(const ContentFile& file, size_t earlier_bundles, size_t earlier_files) noexcept {
    if (file.copy_name.empty() || file.source_names.empty() || file.sha256.empty() ||
        file.size == 0)
        return "a file is missing its name, size or hash";
    if (left_out_name(file.copy_name))
        return "a bundle holds a file that is not copied";
    for (std::string_view source : file.source_names) {
        if (source.empty() || left_out_name(source))
            return "a file's source name is empty or is not copied";
    }
    for (size_t bundle_slot = 0; bundle_slot < earlier_bundles; ++bundle_slot) {
        for (const ContentFile& other : k_bundles[bundle_slot].files) {
            if (files_collide(file, other))
                return "two files share a name and a size";
        }
    }
    for (size_t file_slot = 0; file_slot < earlier_files; ++file_slot) {
        if (files_collide(file, k_bundles[earlier_bundles].files[file_slot]))
            return "two files share a name and a size";
    }
    return {};
}

/// Checks the built-in table.
///
/// @return empty when it holds; otherwise the first fault
[[nodiscard]] constexpr std::string_view table_fault_of() noexcept {
    if (k_bundle_count == 0)
        return "the table is empty";
    for (size_t index = 0; index < k_bundle_count; ++index) {
        const Bundle& bundle = k_bundles[index];
        if (bundle.id.empty() || bundle.english_name.empty())
            return "a bundle has no id or no name";
        for (size_t other = index + 1; other < k_bundle_count; ++other) {
            if (k_bundles[other].id == bundle.id)
                return "two bundles share an id";
        }
        if (bundle.kind == Kind::base_game) {
            if (!bundle.extends.empty() || !bundle.needs.empty())
                return "a base game extends or requires a bundle";
        } else if (bundle.extends.empty()) {
            return "an expansion extends no base game";
        }
        for (std::string_view extended : bundle.extends) {
            const int found = bundle_index(extended);
            if (found < 0)
                return "an expansion extends an unknown bundle";
            if (k_bundles[static_cast<size_t>(found)].kind != Kind::base_game)
                return "an expansion extends something other than a base game";
            if (extended == "totala-demo")
                return "an expansion extends the demo";
        }
        for (std::string_view required : bundle.needs) {
            const int found = bundle_index(required);
            if (found < 0)
                return "a bundle requires an unknown bundle";
            if (k_bundles[static_cast<size_t>(found)].kind != Kind::expansion)
                return "a bundle requires something other than an expansion";
            if (required == bundle.id)
                return "a bundle requires itself";
        }
        if (bundle.files.empty())
            return "a bundle has no files";
        for (size_t file_slot = 0; file_slot < bundle.files.size(); ++file_slot) {
            if (const std::string_view fault =
                    file_names_fault(bundle.files[file_slot], index, file_slot);
                !fault.empty())
                return fault;
        }
    }
    uint8_t colour[k_bundle_count]{};
    for (size_t index = 0; index < k_bundle_count; ++index) {
        if (needs_cycle(static_cast<int>(index), colour, 0))
            return "a bundle's needs form a cycle";
    }
    if (bundle_index("totala") < 0 || bundle_index("totala-demo") < 0)
        return "a base game is missing";
    if (k_installer_count == 0)
        return "no installer is known";
    for (size_t index = 0; index < k_installer_count; ++index) {
        const Installer& installer = k_installers[index];
        if (installer.file_name.empty() || installer.size == 0)
            return "an installer has no name or no size";
        for (size_t other = index + 1; other < k_installer_count; ++other) {
            if (k_installers[other].size == installer.size ||
                same_path(k_installers[other].file_name, installer.file_name))
                return "two installers share a name or a size";
        }
    }
    return {};
}

static_assert(table_fault_of().empty(), "the content bundle table is inconsistent");

/// Writes the marked bundles in table order.
///
/// @param marked one flag per bundle
/// @return the marked ids
[[nodiscard]] std::vector<std::string_view> marked_ids(const bool* marked) {
    std::vector<std::string_view> ids;
    for (size_t index = 0; index < k_bundle_count; ++index) {
        if (marked[index])
            ids.push_back(k_bundles[index].id);
    }
    return ids;
}

/// Marks `ids` that name a bundle.
///
/// @param ids bundle ids
/// @param marked one flag per bundle, set for each id the table holds
void mark_ids(std::span<const std::string_view> ids, bool* marked) noexcept {
    for (std::string_view id : ids) {
        const int index = bundle_index(id);
        if (index >= 0)
            marked[index] = true;
    }
}

} // namespace

std::span<const Bundle> bundles() noexcept {
    return k_bundles;
}

const Bundle* find_bundle(std::string_view id) noexcept {
    const int index = bundle_index(id);
    return index < 0 ? nullptr : &k_bundles[static_cast<size_t>(index)];
}

std::vector<std::string_view> requirement_closure(std::span<const std::string_view> ids) {
    bool marked[k_bundle_count]{};
    mark_ids(ids, marked);
    bool grew = true;
    while (grew) {
        grew = false;
        for (size_t index = 0; index < k_bundle_count; ++index) {
            if (!marked[index])
                continue;
            const Bundle& bundle = k_bundles[index];
            for (size_t slot = 0; slot < direct_need_count(bundle); ++slot) {
                const int next = bundle_index(direct_need(bundle, slot));
                if (next >= 0 && !marked[static_cast<size_t>(next)]) {
                    marked[static_cast<size_t>(next)] = true;
                    grew = true;
                }
            }
        }
    }
    return marked_ids(marked);
}

std::vector<std::string_view> removal_closure(std::span<const std::string_view> ids) {
    bool marked[k_bundle_count]{};
    mark_ids(ids, marked);
    bool grew = true;
    while (grew) {
        grew = false;
        for (size_t index = 0; index < k_bundle_count; ++index) {
            if (marked[index])
                continue;
            const Bundle& bundle = k_bundles[index];
            for (size_t slot = 0; slot < direct_need_count(bundle); ++slot) {
                const int needed = bundle_index(direct_need(bundle, slot));
                if (needed >= 0 && marked[static_cast<size_t>(needed)]) {
                    marked[index] = true;
                    grew = true;
                    break;
                }
            }
        }
    }
    return marked_ids(marked);
}

std::vector<std::string_view>
after_turning_on(std::span<const std::string_view> enabled, std::string_view id) {
    bool was_on[k_bundle_count]{};
    mark_ids(enabled, was_on);
    bool now_on[k_bundle_count]{};
    for (size_t index = 0; index < k_bundle_count; ++index)
        now_on[index] = was_on[index];
    const int turned = bundle_index(id);
    if (turned < 0)
        return marked_ids(now_on);
    const std::string_view one[]{id};
    for (std::string_view needed : requirement_closure(one)) {
        const int index = bundle_index(needed);
        if (index >= 0)
            now_on[static_cast<size_t>(index)] = true;
    }
    for (size_t index = 0; index < k_bundle_count; ++index) {
        if (now_on[index] || k_bundles[index].kind != Kind::expansion)
            continue;
        int missing = -1;
        size_t missing_count = 0;
        const Bundle& bundle = k_bundles[index];
        for (size_t slot = 0; slot < direct_need_count(bundle); ++slot) {
            const int needed = bundle_index(direct_need(bundle, slot));
            if (needed < 0 || !was_on[static_cast<size_t>(needed)]) {
                missing = needed;
                ++missing_count;
            }
        }
        if (missing_count == 1 && missing == turned)
            now_on[index] = true;
    }
    return marked_ids(now_on);
}

std::vector<std::string_view>
after_turning_off(std::span<const std::string_view> enabled, std::string_view id) {
    bool on[k_bundle_count]{};
    mark_ids(enabled, on);
    const int turned = bundle_index(id);
    if (turned < 0 || !on[static_cast<size_t>(turned)])
        return marked_ids(on);
    const std::string_view one[]{id};
    for (std::string_view gone : removal_closure(one)) {
        const int index = bundle_index(gone);
        if (index >= 0)
            on[static_cast<size_t>(index)] = false;
    }
    return marked_ids(on);
}

FileMatch match_file(std::string_view source_name, uint64_t size) noexcept {
    const bool bare = !has_directory(source_name);
    for (const Bundle& bundle : k_bundles) {
        for (const ContentFile& file : bundle.files) {
            if (file.size != size)
                continue;
            if (same_path(file.copy_name, source_name))
                return {&bundle, &file};
            for (std::string_view source : file.source_names) {
                if (same_path(source, source_name))
                    return {&bundle, &file};
            }
            if (!bare)
                continue;
            if (same_path(last_name(file.copy_name), source_name))
                return {&bundle, &file};
            for (std::string_view source : file.source_names) {
                if (same_path(last_name(source), source_name))
                    return {&bundle, &file};
            }
        }
    }
    return {};
}

bool known_hash(const ContentFile& file, const base::sha256::Digest& digest) noexcept {
    for (const base::sha256::Digest& known : file.sha256) {
        if (known == digest)
            return true;
    }
    return false;
}

std::span<const Installer> installers() noexcept {
    return k_installers;
}

const Installer* find_installer(uint64_t size) noexcept {
    for (const Installer& installer : k_installers) {
        if (installer.size == size)
            return &installer;
    }
    return nullptr;
}

bool known_installer_hash(const Installer& installer, const base::sha256::Digest& digest) noexcept {
    return installer.sha256 == digest;
}

std::string_view table_fault() noexcept {
    return table_fault_of();
}

} // namespace oa::data::content_bundles
