// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/platform/system.hpp"

#include "oa/base/threads.hpp"
#include "oa/platform/files.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <vector>
#endif

namespace oa::platform {
namespace {
std::atomic<ErrorSink> error_sink{nullptr};

const std::chrono::steady_clock::time_point process_origin = std::chrono::steady_clock::now();
} // namespace

uint32_t tick_ms() noexcept {
    const auto elapsed = std::chrono::steady_clock::now() - process_origin;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    return static_cast<uint32_t>(static_cast<uint64_t>(ms));
}

void sleep_ms(uint32_t milliseconds) noexcept {
    base::threads::sleep_ms(milliseconds);
}

bool start_thread(ThreadEntry entry, std::size_t, void* argument) noexcept {
    return base::threads::start_detached_thread(entry, argument);
}

uint32_t processor_count() noexcept {
    return base::threads::processor_count();
}

std::optional<std::string> environment_value(const char* name) {
#if defined(_MSC_VER)
    // Visual Studio's C library marks getenv unsafe; its own copy is the
    // same value.
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0 || value == nullptr)
        return std::nullopt;

    struct Release {
        void operator()(char* copy) const noexcept { std::free(copy); }
    };

    const std::unique_ptr<char, Release> owned(value);
    return std::string(owned.get());
#else
    const char* value = std::getenv(name);
    if (value == nullptr)
        return std::nullopt;
    return std::string(value);
#endif
}

void set_error_sink(ErrorSink sink) noexcept {
    error_sink.store(sink);
}

void show_error_message(const char* message) noexcept {
    if (const ErrorSink sink = error_sink.load()) {
        sink(message);
        return;
    }
    std::fprintf(stderr, "Error: %s\n", message ? message : "");
}

bool append_error_log(const char* directory, const char* text) noexcept {
    std::FILE* log = nullptr;
    try {
        std::string path = directory != nullptr ? directory : "";
        path += error_log_file_name;
        log = open_file(std::filesystem::path(std::u8string(path.begin(), path.end())), "ab");
    } catch (const std::exception&) {
        return false;
    }
    if (log == nullptr)
        return false;
    const std::size_t length = text != nullptr ? std::strlen(text) : 0;
    const bool written = std::fwrite(text, 1, length, log) == length;
    return std::fclose(log) == 0 && written;
}

namespace {

/// A path as the folder holding it, ending in a separator, which is how the
/// folder SDL names is written.
std::string as_folder(const std::filesystem::path& path) {
    if (path.empty())
        return {};
    std::string text = path.string();
    if (!text.empty() && text.back() != '/' && text.back() != '\\')
        text += '/';
    return text;
}

} // namespace

std::string program_directory() {
#if defined(_WIN32)
    // The wide form first, because only it carries a name outside the system
    // character set on a Windows that has both. It answers nothing at all on a
    // Windows whose wide entry points are stubs — Windows 95's are — so the
    // system-character-set form is asked then.
    std::wstring wide(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length =
            GetModuleFileNameW(nullptr, wide.data(), static_cast<DWORD>(wide.size()));
        if (length == 0) {
            std::string narrow(MAX_PATH, '\0');
            const DWORD narrow_length =
                GetModuleFileNameA(nullptr, narrow.data(), static_cast<DWORD>(narrow.size()));
            if (narrow_length == 0 || narrow_length >= narrow.size())
                return {};
            narrow.resize(narrow_length);
            return as_folder(std::filesystem::path(narrow));
        }
        if (length < wide.size()) {
            wide.resize(length);
            return as_folder(std::filesystem::path(wide));
        }
        if (wide.size() >= 32768)
            return {};
        wide.resize(wide.size() * 2);
    }
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(size + 1U, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0)
        return {};
    // The program's own files live in the bundle's Resources folder, which is
    // the folder SDL names, rather than beside the executable inside MacOS.
    const std::filesystem::path parent = std::filesystem::path(buffer.data()).parent_path();
    if (parent.filename() == "MacOS" && parent.parent_path().filename() == "Contents")
        return as_folder(parent.parent_path() / "Resources");
    return as_folder(parent);
#else
    std::error_code error;
    const std::filesystem::path resolved = std::filesystem::read_symlink("/proc/self/exe", error);
    return error ? std::string() : as_folder(resolved.parent_path());
#endif
}

std::string error_log_directory(const char* base_path) {
    // SDL names the Resources folder of a bundle as the executable's.
    constexpr std::string_view bundle_resources = "/Contents/Resources/";
    constexpr std::string_view bundle_extension = ".app";
    const std::string_view folder = base_path != nullptr ? base_path : "";
    if (folder.ends_with(bundle_resources)) {
        const std::string_view bundle = folder.substr(0, folder.size() - bundle_resources.size());
        const std::size_t slash = bundle.find_last_of('/');
        if (bundle.ends_with(bundle_extension) && slash != std::string_view::npos)
            return std::string(bundle.substr(0, slash + 1));
    }
    return std::string(folder);
}

} // namespace oa::platform
