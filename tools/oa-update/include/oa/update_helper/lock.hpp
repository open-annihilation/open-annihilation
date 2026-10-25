// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// One update at a time. The lock is held until the helper finishes.
#pragma once

#include <filesystem>
#include <string>

namespace oa::update_helper {

/// An exclusive hold on `<updates>/oa-update.lock`.
class UpdateLock {
  public:

    /// Holds nothing.
    UpdateLock() = default;

    /// Releases the hold.
    ~UpdateLock();

    UpdateLock(const UpdateLock&) = delete;
    UpdateLock& operator=(const UpdateLock&) = delete;

    /// Takes the hold without waiting.
    ///
    /// A hold already taken is a failure and changes nothing.
    ///
    /// @param updates the data folder's updates directory; it must exist
    /// @param[out] error why the hold was not taken; may be null
    /// @return true when this helper holds the lock
    [[nodiscard]] bool try_lock(const std::filesystem::path& updates, std::string* error);

    /// Releases the hold. A second call does nothing.
    void release();

  private:

#if defined(_WIN32)
    void* handle_{};
#else
    int fd_{-1};
#endif
};

} // namespace oa::update_helper
