// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/update_helper/lock.hpp"

#include <string>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace oa::update_helper {
namespace {

void set_error(std::string* reason, std::string message) {
    if (reason != nullptr)
        *reason = std::move(message);
}

} // namespace

UpdateLock::~UpdateLock() {
    release();
}

bool UpdateLock::try_lock(const std::filesystem::path& updates, std::string* error) {
    release();
    const std::filesystem::path path = updates / "oa-update.lock";
#if defined(_WIN32)
    HANDLE handle = CreateFileW(
        path.wstring().c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (handle == INVALID_HANDLE_VALUE && GetLastError() == ERROR_CALL_NOT_IMPLEMENTED) {
        const std::string narrow = path.string();
        handle = CreateFileA(
            narrow.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );
    }
    if (handle == INVALID_HANDLE_VALUE) {
        set_error(error, "the update lock could not be taken");
        return false;
    }
    OVERLAPPED overlapped{};
    if (!LockFileEx(
            handle,
            LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
            0,
            MAXDWORD,
            MAXDWORD,
            &overlapped
        )) {
        const DWORD lock_error = GetLastError();
        if (lock_error != ERROR_CALL_NOT_IMPLEMENTED) {
            CloseHandle(handle);
            set_error(error, "another update is running");
            return false;
        }
        // Windows 9x has LockFile, not LockFileEx.
        if (!LockFile(handle, 0, 0, MAXDWORD, MAXDWORD)) {
            const DWORD simple = GetLastError();
            CloseHandle(handle);
            if (simple == ERROR_LOCK_VIOLATION)
                set_error(error, "another update is running");
            else
                set_error(error, "the update lock could not be taken");
            return false;
        }
    }
    handle_ = handle;
    return true;
#else
    const int opened = ::open(path.c_str(), O_CREAT | O_RDWR, 0644);
    if (opened < 0) {
        set_error(error, "the update lock could not be taken");
        return false;
    }
    if (::flock(opened, LOCK_EX | LOCK_NB) != 0) {
        const int saved = errno;
        ::close(opened);
        if (saved == EWOULDBLOCK || saved == EAGAIN)
            set_error(error, "another update is running");
        else
            set_error(error, "the update lock could not be taken");
        return false;
    }
    fd_ = opened;
    return true;
#endif
}

void UpdateLock::release() {
#if defined(_WIN32)
    if (handle_ == nullptr)
        return;
    HANDLE handle = static_cast<HANDLE>(handle_);
    OVERLAPPED overlapped{};
    if (!UnlockFileEx(handle, 0, MAXDWORD, MAXDWORD, &overlapped) &&
        GetLastError() == ERROR_CALL_NOT_IMPLEMENTED) {
        UnlockFile(handle, 0, 0, MAXDWORD, MAXDWORD);
    }
    CloseHandle(handle);
    handle_ = nullptr;
#else
    if (fd_ < 0)
        return;
    ::flock(fd_, LOCK_UN);
    ::close(fd_);
    fd_ = -1;
#endif
}

} // namespace oa::update_helper
