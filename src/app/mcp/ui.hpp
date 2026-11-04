// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The MCP tools that read and drive the screen (ui.cpp). A call that has to
// wait for the game returns held, and the frame hook polls it. Nothing here
// sleeps, and nothing here runs unless the process was started with --mcp.
#pragma once

#include "oa/app/mcp/session.hpp"

#include "input_events.hpp"

#include <memory>
#include <string_view>
#include <vector>

namespace oa::app {
struct AutomationHost;
struct CheckHost;
} // namespace oa::app

namespace oa::app::mcp {

struct Serve;

/// The screen tools' drive, the picture being taken and the wait in progress.
///
/// One act, one wait and one picture may be open. A second of the same kind
/// is refused. Cancelling the call drops it.
class UiState {
  public:

    /// An idle state, holding nothing.
    UiState();

    /// Frees the drive, the wait and the picture.
    ~UiState();

    /// Takes over another state's drive, wait and picture; that state is then idle.
    UiState(UiState&&) noexcept;

    /// Takes over another state's drive, wait and picture; that state is then idle.
    ///
    /// @return this state
    UiState& operator=(UiState&&) noexcept;
    UiState(const UiState&) = delete;
    UiState& operator=(const UiState&) = delete;

    /// Records what changed since the last pump, before any tool runs.
    ///
    /// @param check the check host
    /// @param automation the automation host
    void before_pump(const CheckHost& check, const AutomationHost& automation);

    /// Drops the open act and lets go of the keys and buttons it holds.
    ///
    /// @param check the check host; null when the game is not running
    /// @param automation the automation host; null when the game is not running
    void cancel_act(const CheckHost* check, const AutomationHost* automation);

    /// Drops the open wait.
    void cancel_wait() noexcept;

    /// Drops the picture being taken.
    ///
    /// @param automation the automation host; null when the game is not running
    void cancel_shot(const AutomationHost* automation);

    /// Answers ui.screen.
    ///
    /// @param serve the running game
    /// @param call the arguments and the poll count
    /// @return the screen, as the automation endpoint's screen answer gives it
    [[nodiscard]] ToolReply on_screen(Serve& serve, const ToolCall& call);

    /// Answers ui.tree.
    ///
    /// @param serve the running game
    /// @param call the arguments and the poll count
    /// @return the control tree
    [[nodiscard]] ToolReply on_tree(Serve& serve, const ToolCall& call);

    /// Answers ui.click. The click is pushed at poll 0 and answered once the
    /// game has taken it.
    ///
    /// @param serve the running game
    /// @param call the arguments and the poll count
    /// @return held until the game takes the click
    [[nodiscard]] ToolReply on_click(Serve& serve, const ToolCall& call);

    /// Answers ui.select. A row off the page is scrolled on, then clicked.
    ///
    /// @param serve the running game
    /// @param call the arguments and the poll count
    /// @return held until the row has been clicked and taken
    [[nodiscard]] ToolReply on_select(Serve& serve, const ToolCall& call);

    /// Answers ui.type.
    ///
    /// @param serve the running game
    /// @param call the arguments and the poll count
    /// @return held until the game takes the text
    [[nodiscard]] ToolReply on_type(Serve& serve, const ToolCall& call);

    /// Answers ui.key.
    ///
    /// @param serve the running game
    /// @param call the arguments and the poll count
    /// @return held until the game takes the keys
    [[nodiscard]] ToolReply on_key(Serve& serve, const ToolCall& call);

    /// Answers ui.pointer.
    ///
    /// @param serve the running game
    /// @param call the arguments and the poll count
    /// @return held until the game takes the move
    [[nodiscard]] ToolReply on_pointer(Serve& serve, const ToolCall& call);

    /// Answers ui.scroll.
    ///
    /// @param serve the running game
    /// @param call the arguments and the poll count
    /// @return held until the game takes the wheel
    [[nodiscard]] ToolReply on_scroll(Serve& serve, const ToolCall& call);

    /// Answers ui.drag.
    ///
    /// @param serve the running game
    /// @param call the arguments and the poll count
    /// @return held until the game takes the drag
    [[nodiscard]] ToolReply on_drag(Serve& serve, const ToolCall& call);

    /// Answers ui.wait. The check runs on each poll, including the first.
    ///
    /// @param serve the running game
    /// @param call the arguments and the poll count
    /// @return held until the condition holds or the timeout passes
    [[nodiscard]] ToolReply on_wait(Serve& serve, const ToolCall& call);

    /// Answers capture.screenshot. A presented picture is started at the pump
    /// and encoded once the frame has been presented.
    ///
    /// @param serve the running game
    /// @param call the arguments and the poll count
    /// @return held until the PNG is ready
    [[nodiscard]] ToolReply on_shot(Serve& serve, const ToolCall& call);

  private:

    struct State;
    std::unique_ptr<State> state_;

    /// Answers an act whose events have already been pushed, once the game
    /// has taken them. A select that still has to click its row pushes the
    /// next step instead.
    ///
    /// @param serve the running game
    /// @param call the arguments and the poll count
    /// @return held, or the act's result
    [[nodiscard]] ToolReply poll_act(Serve& serve, const ToolCall& call);

    /// Pushes events for the open act and holds the call.
    ///
    /// @param serve the running game
    /// @param check the check host
    /// @param automation the automation host
    /// @param events the events, in order
    /// @param expect_screen the screen they are for; empty skips the check
    /// @return held, or the reason they were not pushed
    [[nodiscard]] ToolReply push_act(
        Serve& serve,
        const CheckHost& check,
        const AutomationHost& automation,
        const std::vector<oa::app::automation::InputEvent>& events,
        std::string_view expect_screen
    );

    /// Answers the open act from the screen shown now.
    ///
    /// @param check the check host
    /// @param automation the automation host
    /// @return the result
    [[nodiscard]] ToolReply finish_act(const CheckHost& check, const AutomationHost& automation);

    /// Clicks the select's row, or the scroll button that brings it on.
    ///
    /// @param serve the running game
    /// @param check the check host
    /// @param automation the automation host
    /// @return held, or why the row cannot be chosen
    [[nodiscard]] ToolReply
    aim_row(Serve& serve, const CheckHost& check, const AutomationHost& automation);

    /// Checks the open wait.
    ///
    /// @param serve the running game
    /// @return held, the screen, or the timeout
    [[nodiscard]] ToolReply poll_wait(Serve& serve);

    /// Moves the open picture on by one stage.
    ///
    /// @param serve the running game
    /// @return held, the PNG, or why it failed
    [[nodiscard]] ToolReply poll_shot(Serve& serve);
};

/// Adds the screen tools. Act tools are hidden when the session only lists
/// tools that read.
///
/// @param[in,out] session the session
/// @param[in,out] serve the game the tools read; it outlives the session's calls
void add_ui_tools(Session& session, Serve& serve);

/// Records the screen's changes for this pump, before the session is served.
///
/// @param[in,out] serve the game of this frame
void ui_before_pump(Serve& serve);

} // namespace oa::app::mcp
