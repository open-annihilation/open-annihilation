// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// What the automation endpoint answers: one table of the protocol's
// operations, each with the handler that answers it, and one of the work
// the endpoint does each time it is served, after it has taken requests,
// such as answering a request a handler holds. A new operation adds a row
// to the first table; work that follows a request over later frames adds a
// row to the second.
#pragma once

#include "endpoint.hpp"

#include "oa/app/automation/screen.hpp"

#include <span>
#include <string_view>

namespace oa::app::automation {

/// Answers one request: adds the answer's members, refuses the request
/// (Answer::refuse) or holds it to answer later (Answer::held).
///
/// @param[in,out] endpoint the endpoint, which gives the running game
/// @param request the request
/// @param[in,out] answer the answer, begun
using RequestHandler = void (*)(Endpoint& endpoint, const Request& request, Answer& answer);

/// One operation of the protocol.
struct RequestKind {
    std::string_view op;      ///< the operation, as a request's op names it
    RequestHandler handler{}; ///< what answers it
};

/// Does the endpoint's work of one stage of a frame, after it has taken requests.
///
/// @param[in,out] endpoint the endpoint, which gives the running game
/// @param stage the stage
using FrameWork = void (*)(Endpoint& endpoint, FrameStage stage);

/// Returns every operation the endpoint answers.
///
/// @return the table
[[nodiscard]] std::span<const RequestKind> request_kinds() noexcept;

/// Finds an operation the endpoint answers.
///
/// @param op the operation
/// @return its row, or null when the endpoint does not answer it
[[nodiscard]] const RequestKind* find_request(std::string_view op) noexcept;

/// Returns the work the endpoint does each time it is served.
///
/// @return the table, in the order the work is done
[[nodiscard]] std::span<const FrameWork> frame_work() noexcept;

/// Answers hello: the protocol's version, the engine's, the window's size,
/// where the game's canvas lies in it, the tick rate and whether the clock
/// is fixed.
///
/// @param[in,out] endpoint the endpoint
/// @param request the request
/// @param[in,out] answer the answer
void answer_hello(Endpoint& endpoint, const Request& request, Answer& answer);

/// Answers screen: the screen shown by name, the frontend's state, the
/// dialogs over it, the focused control, the pointer on the canvas, and in
/// a match where its camera looks (null elsewhere).
///
/// @param[in,out] endpoint the endpoint
/// @param request the request
/// @param[in,out] answer the answer
void answer_screen(Endpoint& endpoint, const Request& request, Answer& answer);

/// Answers prefs: the preferences as the game holds them now, all of them
/// or those the request's names list, and the file they live in.
///
/// @param[in,out] endpoint the endpoint
/// @param request the request
/// @param[in,out] answer the answer
void answer_prefs(Endpoint& endpoint, const Request& request, Answer& answer);

/// Answers quit, then has the game quit as a player closing its window does.
///
/// @param[in,out] endpoint the endpoint
/// @param request the request
/// @param[in,out] answer the answer
void answer_quit(Endpoint& endpoint, const Request& request, Answer& answer);

} // namespace oa::app::automation
