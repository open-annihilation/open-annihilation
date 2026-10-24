// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The installed brains. The list is read from the player's Brains folder
// on first use, and again after every brain package is installed or
// removed. The scripts in those folders are never opened.

#include "oa/app/runtime.hpp"

#include "oa/data/brain_library/library.hpp"

#include <memory>
#include <string>
#include <utility>

namespace oa::app {

struct Runtime::BrainLibraryState {
    oa::data::brain_library::Listing listing{};
};

void Runtime::destroy_brain_library_state(BrainLibraryState* state) noexcept {
    delete state;
}

const oa::data::brain_library::Listing& Runtime::installed_brains() const {
    if (!brain_library_state_) {
        std::unique_ptr<BrainLibraryState, void (*)(BrainLibraryState*) noexcept> made{
            new BrainLibraryState, destroy_brain_library_state
        };
        made->listing = oa::data::brain_library::list_installed(
            user_folder_ / std::string(oa::data::brain_library::brains_folder_name)
        );
        brain_library_state_ = std::move(made);
    }
    return brain_library_state_->listing;
}

void Runtime::reload_installed_brains() const {
    // The picker watches this count. A change before the first read still
    // moves it, and still leaves the listing unmade.
    ++installed_brains_generation_;
    if (brain_library_state_ == nullptr)
        return;
    brain_library_state_->listing = oa::data::brain_library::list_installed(
        user_folder_ / std::string(oa::data::brain_library::brains_folder_name)
    );
}

} // namespace oa::app
