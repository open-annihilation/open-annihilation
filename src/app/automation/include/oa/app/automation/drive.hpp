// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Input pushed for the MCP server, beside the automation endpoint's own
// hold. The two do not share keys or a pending request, so --mcp and
// --fark can both be on. The events are the same ones an input request
// carries, and they are pushed the same way: onto SDL's queue at this
// frame's pump, taken by the game before the next pump.
#pragma once

#include "input_events.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app {
struct AutomationHost;
struct CheckHost;
} // namespace oa::app

namespace oa::app::automation {

/// Why a drive did not push its events.
struct DriveFailure {
    std::string code;    ///< busy, screen_changed, unsupported or internal
    std::string message; ///< what a caller can show
};

/// The keys, buttons and pointer one MCP session drives.
///
/// One drive pushes one batch at a time. The batch stays pending until a
/// later pump, when the game has taken the events.
class DriveInput {
  public:

    /// An idle drive, holding nothing.
    DriveInput();

    /// Lets the held devices go only when release is called. Destroying a
    /// drive that is still holding keys does not push the ups: call release
    /// first, while the game is still running.
    ~DriveInput();

    /// Takes over another drive's held devices; that drive then holds nothing.
    DriveInput(DriveInput&&) noexcept;

    /// Takes over another drive's held devices; that drive then holds nothing.
    ///
    /// @return this drive
    DriveInput& operator=(DriveInput&&) noexcept;
    DriveInput(const DriveInput&) = delete;
    DriveInput& operator=(const DriveInput&) = delete;

    /// Pushes events at this frame's pump.
    ///
    /// Refuses while an earlier batch is still pending. An expect_screen
    /// that is not the screen shown refuses and pushes nothing.
    ///
    /// @param check the check host
    /// @param automation the automation host, which holds keys and buttons
    /// @param events the events, in order
    /// @param expect_screen the screen they are for; empty skips the check
    /// @param frame the frame whose pump this is
    /// @return why they were not pushed; empty when they were
    [[nodiscard]] std::optional<DriveFailure> push(
        const CheckHost& check,
        const AutomationHost& automation,
        const std::vector<InputEvent>& events,
        std::string_view expect_screen,
        uint64_t frame
    );

    /// Tells whether a batch is waiting for the game to take it.
    ///
    /// @return true while one is pending
    [[nodiscard]] bool pending() const noexcept;

    /// Tells whether the game has taken the pending batch.
    ///
    /// True at a pump whose frame is later than the one that pushed it.
    ///
    /// @param frame the frame being served
    /// @return true when the batch has been taken
    [[nodiscard]] bool taken(uint64_t frame) const noexcept;

    /// Drops the pending mark. Keys and buttons the batch holds stay held.
    void finish() noexcept;

    /// Lets go of every key, button and finger this drive holds, and drops
    /// any pending mark.
    ///
    /// @param check the check host, for the window
    /// @param automation the automation host, which lets the keys go
    void release(const CheckHost& check, const AutomationHost& automation);

  private:

    struct State;
    std::unique_ptr<State> state_;
};

} // namespace oa::app::automation
