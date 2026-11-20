// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/platform/file_copy.hpp"

#include <array>
#include <cstddef>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace oa::platform {
namespace {

namespace fs = std::filesystem;

/// How many bytes are read and written at a time.
constexpr std::size_t chunk_bytes = 32 * 1024;

/// The permissions a new file is asked for: read and write for everyone, of which the process's
/// umask takes its share, as for the files the engine's streams make.
constexpr unsigned int new_file_permissions = 0666;

/// Turns a code the system left into an error code.
///
/// On Windows the codes that name what the standard library's own errors name are given their
/// standard values, so that a caller compares them with a std::errc.
///
/// @param code errno, or GetLastError() on Windows, captured at the failure
/// @return the error code
std::error_code error_from(int code) noexcept {
#if defined(_WIN32)
    switch (code) {
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:
        return std::make_error_code(std::errc::file_exists);
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        return std::make_error_code(std::errc::no_such_file_or_directory);
    case ERROR_ACCESS_DENIED:
        return std::make_error_code(std::errc::permission_denied);
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
        return std::make_error_code(std::errc::no_space_on_device);
    default:
        return {code, std::system_category()};
    }
#else
    return {code, std::generic_category()};
#endif
}

/// Removes the file a failed copy made, where it is a file the copy can have made.
///
/// @param to the destination
void remove_partial(const fs::path& to) noexcept {
    std::error_code failure;
    if (fs::is_regular_file(to, failure))
        fs::remove(to, failure);
}

#if defined(_WIN32)

/// The code the last file call left.
///
/// @return GetLastError()
int last_code() noexcept {
    return static_cast<int>(GetLastError());
}

/// Opens the source and the destination, and copies the bytes between them.
///
/// @param from the file to copy
/// @param to the file to make
/// @param onto what to do where `to` names a file
/// @param[out] made true once `to` has been opened, so that a failure leaves a file to remove
/// @return the reason it failed, or no error
std::error_code
copy_bytes(const fs::path& from, const fs::path& to, CopyOnto onto, bool& made) noexcept {
    // Other programs may read and write the source while it is copied, as they may for any stream
    // the engine opens.
    constexpr DWORD share_all = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    const HANDLE in = CreateFileW(
        from.c_str(),
        GENERIC_READ,
        share_all,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (in == INVALID_HANDLE_VALUE)
        return error_from(last_code());
    const HANDLE out = CreateFileW(
        to.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        onto == CopyOnto::must_be_new ? CREATE_NEW : CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (out == INVALID_HANDLE_VALUE) {
        const std::error_code refused = error_from(last_code());
        CloseHandle(in);
        return refused;
    }
    made = true;
    std::error_code failure;
    std::array<char, chunk_bytes> chunk;
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(in, chunk.data(), static_cast<DWORD>(chunk.size()), &got, nullptr)) {
            failure = error_from(last_code());
            break;
        }
        if (got == 0)
            break;
        DWORD done = 0;
        while (done < got) {
            DWORD put = 0;
            if (!WriteFile(out, chunk.data() + done, got - done, &put, nullptr) || put == 0) {
                failure = error_from(last_code());
                if (!failure)
                    failure = std::make_error_code(std::errc::io_error);
                break;
            }
            done += put;
        }
        if (failure)
            break;
    }
    CloseHandle(in);
    // A close that fails is a write that did not reach the file.
    if (CloseHandle(out) == 0 && !failure)
        failure = error_from(last_code());
    return failure;
}

#else

/// Opens the source and the destination, and copies the bytes between them.
///
/// @param from the file to copy
/// @param to the file to make
/// @param onto what to do where `to` names a file
/// @param[out] made true once `to` has been opened, so that a failure leaves a file to remove
/// @return the reason it failed, or no error
std::error_code
copy_bytes(const fs::path& from, const fs::path& to, CopyOnto onto, bool& made) noexcept {
    int in = -1;
    do {
        in = ::open(from.c_str(), O_RDONLY | O_CLOEXEC);
    } while (in < 0 && errno == EINTR);
    if (in < 0)
        return error_from(errno);
    const int creation =
        O_WRONLY | O_CREAT | O_CLOEXEC | (onto == CopyOnto::must_be_new ? O_EXCL : O_TRUNC);
    int out = -1;
    do {
        out = ::open(to.c_str(), creation, new_file_permissions);
    } while (out < 0 && errno == EINTR);
    if (out < 0) {
        const std::error_code refused = error_from(errno);
        ::close(in);
        return refused;
    }
    made = true;
    std::error_code failure;
    std::array<char, chunk_bytes> chunk;
    for (;;) {
        const ssize_t got = ::read(in, chunk.data(), chunk.size());
        if (got < 0 && errno == EINTR)
            continue;
        if (got < 0) {
            failure = error_from(errno);
            break;
        }
        if (got == 0)
            break;
        std::size_t done = 0;
        while (done < static_cast<std::size_t>(got)) {
            const ssize_t put =
                ::write(out, chunk.data() + done, static_cast<std::size_t>(got) - done);
            if (put < 0 && errno == EINTR)
                continue;
            if (put <= 0) {
                failure = put < 0 ? error_from(errno) : std::make_error_code(std::errc::io_error);
                break;
            }
            done += static_cast<std::size_t>(put);
        }
        if (failure)
            break;
    }
    ::close(in);
    // A close that fails is a write that did not reach the file: a network share reports a write
    // it could not keep when the file is closed.
    if (::close(out) != 0 && !failure)
        failure = error_from(errno);
    return failure;
}

#endif

} // namespace

std::error_code
copy_file_contents(const fs::path& from, const fs::path& to, CopyOnto onto) noexcept {
    std::error_code failure;
    if (onto == CopyOnto::replace) {
        // Opening the destination for writing empties it, and it must not be the source.
        const bool same = fs::equivalent(from, to, failure);
        if (!failure && same)
            return std::make_error_code(std::errc::file_exists);
        failure.clear();
    }
    bool made = false;
    failure = copy_bytes(from, to, onto, made);
    if (failure && made)
        remove_partial(to);
    return failure;
}

} // namespace oa::platform
