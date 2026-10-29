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

#include <exception>
#include <iostream>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>

namespace oa::app {

namespace settings = oa::ui::engine_settings;

std::string Runtime::default_brain() const {
    const auto found = preference_values_.find(std::string(settings::key::brain_default));
    if (found == preference_values_.end() || found->second.empty())
        return {};
    const std::string id = found->second;
    if (LibraryHost* host = library_.get()) {
        // The id is copied out under the lock. find only scans, so the lock
        // is not held across any other read, and the listing is not copied.
        base::threads::LockGuard guard(host->work.mutex);
        if (host->work.searches > 0) {
            if (oa::data::brain_library::find(host->work.brain_listing, id) == nullptr)
                return {};
            return id;
        }
    }
    // No search yet: the worker's listing is empty, not a refusal of this id.
    // The folder read is the same one a closed Library uses.
    if (oa::data::brain_library::find(installed_brains(), id) == nullptr)
        return {};
    return id;
}

void Runtime::set_default_brain(std::string_view id) {
    const std::string key{settings::key::brain_default};
    if (id.empty()) {
        if (preference_values_.erase(key) == 0)
            return;
    } else {
        preference_values_[key] = std::string(id);
    }
    try {
        preferences_dirty_ = true;
        save_preferences();
    } catch (const std::exception& error) {
        if (brain_default_failure_logged_)
            return;
        brain_default_failure_logged_ = true;
        std::cerr << "open-annihilation: brains: the default could not be stored: " << error.what()
                  << '\n';
    } catch (...) {
        if (brain_default_failure_logged_)
            return;
        brain_default_failure_logged_ = true;
        std::cerr << "open-annihilation: brains: the default could not be stored\n";
    }
}

} // namespace oa::app
