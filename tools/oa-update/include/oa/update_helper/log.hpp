// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The helper's log: one line a step, appended, kept under a mebibyte by
// dropping the first half.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace oa::update_helper {

/// An append-only log under the updates directory.
class Log {
  public:

    /// Opens `<updates>/oa-update.log`, creating it when it is missing.
    ///
    /// @param updates the data folder's updates directory
    /// @param[out] error why the log could not be opened; may be null
    /// @return true when lines can be written
    [[nodiscard]] bool open(const std::filesystem::path& updates, std::string* error);

    /// Appends one line. A newline in `text` becomes a space.
    ///
    /// @param text the line, without a line end
    void line(std::string_view text);

  private:

    /// Drops the first half once the file is over a mebibyte.
    void trim();

    std::filesystem::path path_{};
    bool open_{};
};

} // namespace oa::update_helper
