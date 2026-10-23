// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The default computer player. The preference names an installed brain, or
// the built-in player when it names none. Once the Library's worker has
// searched, the id is taken from that listing, so this does not open a
// manifest on the Library's thread. Before that search the Brains folder
// is read: a match can start while the worker has not listed anything.

#include "library_host.hpp"

#include "oa/app/runtime.hpp"
#include "oa/base/threads.hpp"
#include "oa/data/brain_library/library.hpp"
#include "oa/ui/engine_settings.hpp"

#include <optional>
#include <string>
#include <utility>

namespace oa::app {

namespace settings = oa::ui::engine_settings;

std::string Runtime::default_brain() const {
    const auto found = preference_values_.find(std::string(settings::key::brain_default));
    if (found == preference_values_.end() || found->second.empty())
        return {};
    const std::string id = found->second;
    if (LibraryHost* host = library_.get()) {
        // Copy the listing out. A pointer into the worker's list would not
        // outlive the lock, and the lock is not held across any other read.
        std::optional<oa::data::brain_library::Listing> copied;
        {
            base::threads::LockGuard guard(host->work.mutex);
            if (host->work.searches > 0)
                copied = host->work.brain_listing;
        }
        // No search yet: the listing is empty, not a refusal of this id.
        // The folder read below is the same one a closed Library uses.
        if (copied) {
            if (oa::data::brain_library::find(*copied, id) == nullptr)
                return {};
            return id;
        }
    }
    if (oa::data::brain_library::find(installed_brains(), id) == nullptr)
        return {};
    return id;
}

} // namespace oa::app
