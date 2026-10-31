// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// File boundary. Engine code opens, reads, positions and closes files through
// this table of function pointers; the default implementation uses the host C
// library, whose streams already serialise concurrent access.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace oa::platform {

struct FileHandle;

enum class SeekOrigin : int32_t { begin = 0, current = 1, end = 2 };

struct Files {
    void* context{};
    // mode uses C stdio spelling ("rb", "wb"...). Null on failure.
    FileHandle* (*open)(void* context, const char* path, const char* mode){};
    // Returns whole elements read, like fread.
    std::size_t (*read)(
        void* context, FileHandle* file, void* buffer, std::size_t size, std::size_t count
    ){};
    std::size_t (*write)(
        void* context, FileHandle* file, const void* buffer, std::size_t size, std::size_t count
    ){};
    // Current offset, or -1 on failure.
    int64_t (*tell)(void* context, FileHandle* file){};
    // 0 on success, nonzero on failure.
    int32_t (*seek)(void* context, FileHandle* file, int64_t offset, SeekOrigin origin){};
    void (*close)(void* context, FileHandle* file){};
};

/// Opens a file as a C stream, as fopen does.
///
/// Other streams and programs may open the same file at the same time, for
/// reading and for writing, on every system. On Windows the file is opened by
/// its path's wide spelling, so that a path longer than 259 characters opens
/// where long paths are turned on.
///
/// @param path the file's path, in the encoding the host C library reads
///        narrow paths in
/// @param mode the mode in fopen's spelling, such as "rb" or "ab"
/// @return the stream, or null when the file cannot be opened
[[nodiscard]] std::FILE* open_file(const char* path, const char* mode) noexcept;

/// Opens a file as a C stream, as fopen does, by a path of any spelling.
///
/// On Windows the file is opened by its path's wide spelling, so that a name
/// outside the system's code page opens too. Other streams and programs may
/// open the same file at the same time, for reading and for writing.
///
/// @param path the file's path
/// @param mode the mode in fopen's spelling, such as "rb" or "ab"
/// @return the stream, or null when the file cannot be opened
[[nodiscard]] std::FILE* open_file(const std::filesystem::path& path, const char* mode) noexcept;

/// Returns a path's UTF-8 spelling.
///
/// On Windows the path's UTF-16 is converted to UTF-8 directly, never
/// through the system's code page, so that every name converts whatever
/// characters it holds and whatever the code page; a lone surrogate, which
/// UTF-8 cannot hold, becomes U+FFFD. Elsewhere the path's own bytes are
/// returned as they are. Use it, never std::filesystem::path::string(),
/// generic_string() or operator<<, which convert through the code page on
/// Windows and throw on a character it lacks.
///
/// @param path the path
/// @return its UTF-8 spelling
[[nodiscard]] std::string path_to_utf8(const std::filesystem::path& path);

/// Returns a path's UTF-8 spelling with forward slashes between its names.
///
/// path_to_utf8() with each of Windows' backslashes written as a slash, as
/// std::filesystem::path::generic_string() spells it; elsewhere it is the
/// same as path_to_utf8().
///
/// @param path the path
/// @return its UTF-8 spelling, with forward slashes
[[nodiscard]] std::string path_to_generic_utf8(const std::filesystem::path& path);

/// Returns the path a UTF-8 spelling names.
///
/// On Windows the text is converted to UTF-16 directly, never through the
/// system's code page. A byte that does not begin a well-formed UTF-8
/// sequence stands for the character of the same value (Latin-1), so that
/// no text is refused. Elsewhere the bytes are the path's as they are. Use
/// it, never a path made from a std::string or a const char*, which Windows
/// reads in the code page.
///
/// @param text the path, UTF-8
/// @return the path
[[nodiscard]] std::filesystem::path path_from_utf8(std::string_view text);

/// Converts UTF-16 to UTF-8, as path_to_utf8() does on Windows.
///
/// @param text the UTF-16; a lone surrogate becomes U+FFFD
/// @return the UTF-8
[[nodiscard]] std::string utf8_from_utf16(std::u16string_view text);

/// Converts UTF-8 to UTF-16, as path_from_utf8() does on Windows.
///
/// @param text the UTF-8; a byte that does not begin a well-formed sequence
///        stands for the character of the same value
/// @return the UTF-16
[[nodiscard]] std::u16string utf16_from_utf8(std::string_view text);

/// Returns the most characters a path may have for the system to open it.
///
/// That is 1,023 on macOS and 4,095 on Linux. On Windows it is 259, unless
/// the system lets this program open longer paths (Windows 10, version 1607,
/// or later, with long paths turned on), when it is 32,767.
///
/// @return the length, counted in the path's own characters: bytes, or
///         UTF-16 units on Windows
[[nodiscard]] std::size_t longest_path() noexcept;

/// Reports whether the system opens paths longer than 259 characters only
/// once long paths are turned on in it, as Windows does.
///
/// @return true on Windows while long paths are not turned on, or the system
///         is older than Windows 10, version 1607
[[nodiscard]] bool long_paths_turned_off() noexcept;

/// Returns the file boundary backed by the host C library.
///
/// @return the boundary; its context is null
[[nodiscard]] Files stdio_files() noexcept;

/// Writes a serialised diagnostic line to standard error.
///
/// @param format printf-style format
/// @return the number of characters written, or a negative value on failure
int log_message(const char* format, ...) noexcept
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

/// Replaces a file by writing a temporary file beside it and renaming it over.
///
/// The temporary name is the file's name, then .tmp-, the process and a
/// count. The folder is made when it is missing. The temporary file is
/// flushed to the disk, renamed over the file, and the folder is synced
/// where the system allows. On any failure the temporary file is removed,
/// the old file is left as it was, and the function says why.
///
/// @param file the file to replace
/// @param bytes the bytes the file should hold
/// @param[out] error why the file was not replaced; may be null
/// @return true when the file holds these bytes
[[nodiscard]] bool replace_file(
    const std::filesystem::path& file, std::span<const uint8_t> bytes, std::string* error
) noexcept;

} // namespace oa::platform
