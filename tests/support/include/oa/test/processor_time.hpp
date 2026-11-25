// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The processor time a test's own work takes, for a bound that a busy
// machine does not stretch the way it stretches the real time. Defined by
// oa-test-processor-time, which reads the Windows clock in its own source
// file, so that no test sees the Windows headers' macros.
#pragma once

#include <chrono>
#include <optional>

namespace oa::test {

/// Reads the processor time this program has spent so far: the user and
/// kernel time of all its threads.
///
/// On Windows std::clock counts real time since the program started, so a
/// busy machine would stretch it; the process's own user and kernel times
/// are read there instead. Elsewhere std::clock gives the processor time.
///
/// @return seconds of processor time, or nothing when the system cannot say
[[nodiscard]] std::optional<double> processor_seconds() noexcept;

/// What a piece of work took: this program's processor time and the real
/// time.
struct WorkTime {
    /// Seconds of processor time; nothing when the system could not say at
    /// the start or at the end.
    std::optional<double> processor_seconds{};
    std::chrono::milliseconds real{}; ///< the real time
};

/// Times a piece of work from the moment it is made, in this program's
/// processor time and in real time.
class WorkTimer {
  public:

    /// Starts timing.
    WorkTimer() noexcept;

    /// Returns what the work took from the start until now.
    ///
    /// @return the processor time and the real time
    [[nodiscard]] WorkTime elapsed() const noexcept;

  private:

    std::optional<double> processor_started_{};
    std::chrono::steady_clock::time_point real_started_{};
};

} // namespace oa::test
