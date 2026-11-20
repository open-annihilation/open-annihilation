// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The coverage maps a thread keeps from one frame to the next: the kit's
// rounded shapes and icons, each worked out once at its size and blended
// from the kept bytes whenever it is drawn again.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace oa::ui::kit {

/// The coverage maps a thread asked for most recently, each with the key it
/// was worked out from: at most a number of maps holding at most a number
/// of bytes together, the map asked for longest ago making room for a new
/// one. A map larger than all the bytes is worked out each time it is asked
/// for. A screen draws the same shapes on every frame it draws.
///
/// @tparam Key what a map is worked out from, compared with ==; equal keys
///     give equal maps
template <typename Key>
class KeptCoverage {
  public:

    /// Keeps nothing yet.
    ///
    /// @param most_maps the most maps kept
    /// @param most_bytes the most bytes the kept maps hold together
    KeptCoverage(std::size_t most_maps, std::size_t most_bytes) noexcept
        : most_maps_(most_maps), most_bytes_(most_bytes) {}

    /// Returns a key's map: the kept one, or the one work_out returns, kept
    /// when it fits.
    ///
    /// @param key what the map is worked out from
    /// @param bytes the map's bytes, as work_out returns it
    /// @param work_out returns the map, a std::vector<uint8_t>
    /// @return the map, until the next ask
    template <typename WorkOut>
    const std::vector<uint8_t>& coverage(const Key& key, std::size_t bytes, WorkOut&& work_out) {
        const uint64_t asked = ++asks_;
        for (Kept& kept : maps_)
            if (kept.key == key) {
                kept.used = asked;
                return kept.coverage;
            }
        if (bytes > most_bytes_) {
            unkept_ = work_out();
            return unkept_;
        }
        while (!maps_.empty() && (maps_.size() >= most_maps_ || bytes_ + bytes > most_bytes_)) {
            const auto oldest = std::min_element(
                maps_.begin(), maps_.end(), [](const Kept& one, const Kept& other) {
                    return one.used < other.used;
                }
            );
            bytes_ -= oldest->coverage.size();
            if (oldest != maps_.end() - 1)
                *oldest = std::move(maps_.back());
            maps_.pop_back();
        }
        Kept& added = maps_.emplace_back();
        added.key = key;
        added.used = asked;
        added.coverage = work_out();
        bytes_ += added.coverage.size();
        return added.coverage;
    }

  private:

    /// A map with what it was worked out from.
    struct Kept {
        Key key{};                       ///< what it was worked out from
        uint64_t used{};                 ///< when it was last asked for, in asks
        std::vector<uint8_t> coverage{}; ///< its bytes
    };

    std::size_t most_maps_;         ///< the most maps kept
    std::size_t most_bytes_;        ///< the most bytes they hold together
    std::vector<Kept> maps_{};      ///< the kept maps, at most most_maps_
    std::size_t bytes_{};           ///< their bytes, at most most_bytes_
    uint64_t asks_{};               ///< how many times a map was asked for
    std::vector<uint8_t> unkept_{}; ///< the last map too large to keep
};

} // namespace oa::ui::kit
