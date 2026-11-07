// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The controls of the screen shown, as the automation endpoint reports
// them: the built-in screens' and the match's through the automation host
// (automation_host.hpp), the multiplayer screens' from their own panel
// (oa/ui/frontend_multiplayer/screens.hpp). The screen answer's dialogs and
// focused control come from them too.
#pragma once

#include "endpoint.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace oa::app::automation {

/// Collects the controls of the windows extensions show, then of the screen
/// shown and of the dialog over it, the dialog's first among the screen's.
///
/// @param check the check host, which names the screen
/// @param host the automation host, which lists the controls
/// @param[out] controls replaced by the controls
void collect_controls(
    const oa::app::CheckHost& check,
    const oa::app::AutomationHost& host,
    std::vector<AutomationControl>& controls
);

/// Collects the controls the endpoint's hosts see.
///
/// @param endpoint the endpoint, served
/// @param[out] controls replaced by the controls
void collect_controls(const Endpoint& endpoint, std::vector<AutomationControl>& controls);

/// Returns the name the protocol gives a kind of control.
///
/// @param kind the kind
/// @return button, check box, list, text field, slider, label, area or image
[[nodiscard]] std::string_view control_kind_name(AutomationControlKind kind) noexcept;

/// Writes the fields a screen reader reads, after the control's existing fields.
///
/// `role` and `label` are always written. A list keeps its integer `selected`
/// and does not gain the boolean of the same name. A masked field's label is
/// never the text it holds.
///
/// @param[in,out] json the control's object, open
/// @param control the control
void write_control_access(JsonWriter& json, const AutomationControl& control);

/// Returns the dialogs the controls belong to, by their GUI file's name, in
/// the controls' order, each once.
///
/// @param controls the screen's controls (collect_controls)
/// @return the dialogs; empty when no dialog is over the screen
[[nodiscard]] std::vector<std::string> dialog_names(const std::vector<AutomationControl>& controls);

/// Writes the screen answer's dialogs and focused control: the dialogs the
/// controls belong to, in their order, and the name of the control that
/// holds the keyboard focus.
///
/// @param[in,out] json the answer's object, open
/// @param controls the screen's controls (collect_controls)
void write_dialogs_and_focus(JsonWriter& json, const std::vector<AutomationControl>& controls);

/// Answers controls: the screen's name and every control of it and of the
/// dialog over it, each with its name, kind, dialog, rectangle on the
/// canvas and in the window's pixels, state and text, and a list's rows.
///
/// Refuses screen_changed when the request names another screen than the
/// one shown.
///
/// @param[in,out] endpoint the endpoint
/// @param request the request
/// @param[in,out] answer the answer
void answer_controls(Endpoint& endpoint, const Request& request, Answer& answer);

} // namespace oa::app::automation
