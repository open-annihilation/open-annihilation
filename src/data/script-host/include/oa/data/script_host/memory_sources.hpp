// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A package's files held in memory and served to a sandbox. Paths match
// byte for byte: `Lua/util.lua` is not `lua/util.lua`, and a trailing slash
// is a different path.
#pragma once

#include "oa/data/script_host/script_host.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::data::script_host {

/// One package's files, sorted by path, for a sandbox to read.
///
/// Movable, not copyable. `hooks` points at this object, so it must not be
/// moved or destroyed while a sandbox is using those hooks.
class MemorySources {
  public:

    MemorySources() = default;
    MemorySources(const MemorySources&) = delete;
    MemorySources& operator=(const MemorySources&) = delete;
    /// Moves the files and leaves the other object empty.
    MemorySources(MemorySources&& other) noexcept;
    /// Moves the files and drops the files held here.
    MemorySources& operator=(MemorySources&& other) noexcept;
    /// Drops the files.
    ~MemorySources() = default;

    /// Copies `bytes` under `path`, replacing a file already stored there.
    ///
    /// An empty path is refused. The copy is the only one kept: the caller
    /// may free `bytes` when this returns.
    ///
    /// @param path package path, matched byte for byte; empty is refused
    /// @param bytes the file contents to copy
    /// @return false when `path` is empty or the copy cannot be allocated
    [[nodiscard]] bool add(std::string_view path, std::span<const uint8_t> bytes) noexcept;

    /// Returns the bytes stored at `path`.
    ///
    /// The span is valid until the next `add` on this object or until it is
    /// destroyed. An empty file and a missing path both yield an empty span;
    /// `hooks` can still tell them apart.
    ///
    /// @param path package path, matched byte for byte
    /// @return the stored bytes, or an empty span when `path` is absent
    [[nodiscard]] std::span<const uint8_t> find(std::string_view path) const noexcept;

    /// Returns how many files are stored.
    ///
    /// @return the number of paths, or 0 after a move
    [[nodiscard]] std::size_t size() const noexcept;

    /// Returns the sum of the stored file sizes.
    ///
    /// @return that sum in bytes, or 0 after a move
    [[nodiscard]] std::size_t total_bytes() const noexcept;

    /// Returns a reader over this object's files.
    ///
    /// The hooks' context is this object. Do not move or destroy it while a
    /// sandbox is using the hooks. The reader copies the exact path's bytes,
    /// or returns false when the path is absent.
    ///
    /// @return hooks that read this object
    [[nodiscard]] SourceHooks hooks() const noexcept;

  private:

    /// One stored file. Paths stay in byte order.
    struct Stored {
        std::string path;
        std::vector<uint8_t> bytes;
    };

    /// SourceHooks reader. The context is a `MemorySources`.
    ///
    /// @param context the `MemorySources` to read; null returns false
    /// @param path package path, matched byte for byte
    /// @param[out] bytes the file contents; cleared when the path is absent
    /// @return true when `path` is stored, including when the file is empty
    static bool read_file(void* context, std::string_view path, std::vector<uint8_t>& bytes);

    /// True when `stored` comes before `path` in byte order.
    ///
    /// @param stored a file already held
    /// @param path the path being sought
    /// @return true when `stored.path` is less than `path`
    [[nodiscard]] static bool path_before(const Stored& stored, std::string_view path);

    /// Finds `path`, or the end when it is absent.
    ///
    /// @param path package path
    /// @return the stored file, or the end when `path` is absent
    [[nodiscard]] std::vector<Stored>::const_iterator locate(std::string_view path) const;

    std::vector<Stored> files_;
    std::size_t total_bytes_{};
};

} // namespace oa::data::script_host
