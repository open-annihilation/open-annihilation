// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The kinds of game extensions record, one list for the process.

#include "oa/data/replays/replays.hpp"

#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace oa::data::replays {
namespace {

/// The process's kinds, and the lock that guards them.
struct KindTable {
    /// Guards `kinds`.
    std::mutex mutex{};
    /// The kinds, in the order each id was first registered.
    std::vector<Kind> kinds{};
};

/// Returns the process's kinds, made on first use.
///
/// @return the table
KindTable& kind_table() {
    static KindTable table;
    return table;
}

} // namespace

void register_kind(Kind kind) {
    if (kind.id.empty())
        return;
    KindTable& table = kind_table();
    const std::lock_guard<std::mutex> lock(table.mutex);
    for (Kind& listed : table.kinds) {
        if (listed.id == kind.id) {
            listed = std::move(kind);
            return;
        }
    }
    table.kinds.push_back(std::move(kind));
}

std::vector<Kind> registered_kinds() {
    KindTable& table = kind_table();
    const std::lock_guard<std::mutex> lock(table.mutex);
    return table.kinds;
}

void forget_kinds() noexcept {
    KindTable& table = kind_table();
    std::vector<Kind> forgotten;
    {
        const std::lock_guard<std::mutex> lock(table.mutex);
        forgotten.swap(table.kinds);
    }
}

} // namespace oa::data::replays
