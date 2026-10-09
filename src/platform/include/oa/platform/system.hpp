// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// Host clock, sleep, thread start, processor count, environment and
// fatal-message services.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace oa::platform {

/// Returns the host millisecond tick count.
///
/// @return milliseconds since an arbitrary process-local origin, wrapping at
///         32 bits
[[nodiscard]] uint32_t tick_ms() noexcept;

/// Suspends the calling thread.
///
/// @param milliseconds time to sleep
void sleep_ms(uint32_t milliseconds) noexcept;

using ThreadEntry = void (*)(void* argument);

/// Starts a detached thread.
///
/// @param entry function the thread runs
/// @param stack_size requested stack size in bytes; advisory
/// @param argument value passed to entry
/// @return false when the thread could not be created
[[nodiscard]] bool start_thread(ThreadEntry entry, std::size_t stack_size, void* argument) noexcept;

/// Returns how many threads the machine runs at once.
///
/// @return the number of logical processors the system reports, at least 1
[[nodiscard]] uint32_t processor_count() noexcept;

/// Returns the value of an environment variable of the process.
///
/// @param name the variable's name
/// @return its value, which may be empty, or nothing when it is not set
[[nodiscard]] std::optional<std::string> environment_value(const char* name);

using ErrorSink = void (*)(const char* message);

/// Routes show_error_message to a front end, for example a message box.
///
/// @param sink receiver of error messages; null restores the default, which
///        writes to standard error
void set_error_sink(ErrorSink sink) noexcept;

/// Reports a fatal or user-visible error under the "Error" caption.
///
/// @param message NUL-terminated text passed to the error sink
void show_error_message(const char* message) noexcept;

// Out-of-memory report text, appended to the error log beside the application.
inline constexpr char out_of_memory_message[] = "Out of memory!\r\nYour hard disk may be full\r\n";
inline constexpr char error_log_file_name[] = "ErrorLog.txt";

/// Appends text to ErrorLog.txt.
///
/// @param directory path ending in a separator, UTF-8 as SDL gives the
///        application's folder; null or empty for the working directory
/// @param text NUL-terminated text appended as is
/// @return false when the log cannot be opened
bool append_error_log(const char* directory, const char* text) noexcept;

/// Returns the folder beside the application, where ErrorLog.txt goes.
///
/// The application is the executable, or on macOS the application bundle
/// that holds it, whose own folders stay as they were installed.
///
/// @param base_path the folder SDL_GetBasePath() names, ending in a
///        separator: the executable's folder, or a bundle's
///        Contents/Resources/ folder; null for none
/// @return base_path, or the folder that holds the bundle when base_path is
///         the Contents/Resources/ folder of a bundle named *.app; empty
///         when base_path is null
[[nodiscard]] std::string error_log_directory(const char* base_path);

/// Returns the folder this program's own files sit beside, ending in a
/// separator.
///
/// The executable's folder, or the Resources folder of the bundle that holds
/// it on macOS — the same folder SDL_GetBasePath() names. It is answered here
/// from the system rather than from SDL because SDL answers with nothing on a
/// Windows whose wide entry points are stubs, which is a Windows this program
/// runs on, and the folders it holds — the log, the fonts, the catalogues —
/// are wanted there as much as anywhere.
///
/// @return the folder, ending in a separator; empty when the system will not
///         say where the program is
[[nodiscard]] std::string program_directory();

} // namespace oa::platform
