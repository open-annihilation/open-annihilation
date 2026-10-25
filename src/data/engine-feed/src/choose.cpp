// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/data/engine_feed/feed.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace oa::data::engine_feed {
namespace {

/// Compares two texts, folding ASCII letters and nothing else.
bool same_ascii(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        unsigned char one = static_cast<unsigned char>(left[index]);
        unsigned char two = static_cast<unsigned char>(right[index]);
        if (one >= 'A' && one <= 'Z')
            one = static_cast<unsigned char>(one - 'A' + 'a');
        if (two >= 'A' && two <= 'Z')
            two = static_cast<unsigned char>(two - 'A' + 'a');
        if (one != two)
            return false;
    }
    return true;
}

/// The notes whose language matches `tag`, or null.
const Notes* find_language(const Release& release, std::string_view tag) noexcept {
    for (const Notes& notes : release.notes) {
        if (same_ascii(notes.language, tag))
            return &notes;
    }
    return nullptr;
}

} // namespace

std::optional<int64_t> release_number(formats::oamod::EngineVersion version) noexcept {
    if (version.minor > 99 || version.patch > 99)
        return std::nullopt;
    return static_cast<int64_t>(version.major) * 10000 + static_cast<int64_t>(version.minor) * 100 +
           static_cast<int64_t>(version.patch);
}

Offer choose(const Feed& feed, const Build& build) noexcept {
    const Release* best = nullptr;
    const Package* best_package = nullptr;
    for (const Release& candidate : feed.releases) {
        if (candidate.release <= build.release)
            continue;
        if (best != nullptr && candidate.release <= best->release)
            continue;
        const Package* found = nullptr;
        for (const Package& package : candidate.packages) {
            if (package.id == build.package) {
                found = &package;
                break;
            }
        }
        if (found == nullptr)
            continue;
        best = &candidate;
        best_package = found;
    }
    Offer offer;
    if (best == nullptr)
        return offer;
    offer.release = best;
    offer.package = best_package;
    offer.way = build.version >= best->from ? Way::in_place : Way::page_only;
    return offer;
}

const Notes* notes_for(const Release& release, std::string_view language_tag) noexcept {
    if (const Notes* exact = find_language(release, language_tag))
        return exact;
    const std::size_t hyphen = language_tag.find('-');
    if (hyphen != std::string_view::npos && hyphen > 0) {
        if (const Notes* primary = find_language(release, language_tag.substr(0, hyphen)))
            return primary;
    }
    return find_language(release, "en");
}

} // namespace oa::data::engine_feed
