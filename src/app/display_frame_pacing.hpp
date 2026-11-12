// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "oa/app/frame_pacing.hpp"

namespace oa::app::frame_pacing {

/// Whether a late frame restarts the schedule or retains its nearby display deadline.
enum class FrameCadence : uint8_t { restart_after_late, keep_display_phase };

/// Chooses display pacing only for an enhanced match waiting for the display.
/// @param enhanced the match has Enhanced Mode enabled
/// @param vertical_sync the renderer is waiting for the display
/// @return the schedule used by the frame and the display rate cap
[[nodiscard]] constexpr FrameCadence
display_frame_cadence(bool enhanced, bool vertical_sync) noexcept {
    return enhanced && vertical_sync ? FrameCadence::keep_display_phase
                                     : FrameCadence::restart_after_late;
}

/// Schedules the next frame, retaining display phase after a short overrun.
/// A frame less than one period late is followed immediately by the next;
/// its remaining work can finish before the following absolute deadline.
/// Longer stalls restart the schedule. The ordinary cadence, no limit and
/// the tick-rate schedule retain end_paced_frame's original behaviour.
/// @param[in,out] pacer the loop's schedule
/// @param now_ns the frame's end, nanoseconds on the steady clock
/// @param frames_per_second the requested rate; zero waits for nothing
/// @param cadence whether short overruns retain the display phase
/// @return nanoseconds to wait before the next frame starts
[[nodiscard]] uint64_t end_paced_frame(
    FramePacer& pacer, uint64_t now_ns, uint32_t frames_per_second, FrameCadence cadence
) noexcept;

/// Returns the cap used while waiting for the display.
/// Display cadence permits the display's full reported rate, rounded up to
/// a whole frame per second; the renderer's vertical sync bounds presentation.
/// The ordinary cadence retains the cap just below that rate.
/// @param display_hz the reported display refresh rate
/// @param cadence whether frames keep the display's phase
/// @return the rate cap; zero when the display reports no positive rate
[[nodiscard]] uint32_t vsync_frame_cap(float display_hz, FrameCadence cadence) noexcept;

} // namespace oa::app::frame_pacing
