// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The sign-in question and the share question over the Library. They draw
// nothing and send nothing: the app reads what the player typed and forgets
// the password once the request is built.
#pragma once

#include <cstddef>
#include <string>

namespace oa::ui::library {

struct Library;

/// The sign-in question. The password stays here until the request is built,
/// and the field the player sees is a mask, not this text.
struct SignInDialog {
    /// The question is open over the Library.
    bool open{};
    /// The Core Prime handle.
    std::string handle{};
    /// The caret in `handle`, in bytes, on a character boundary.
    std::size_t handle_caret{};
    /// The password, for this question only.
    std::string password{};
    /// The caret in `password`, in bytes, on a character boundary.
    std::size_t password_caret{};
    /// A sign-in is being sent. The question takes no other press.
    bool busy{};
    /// A sentence under the fields. Empty shows the forgotten-password line.
    std::string note{};
    /// This system can send the password over TLS. Off, the question does not open.
    bool secure{true};
};

/// The share question. The title may be empty, and the player leaves it out.
struct ShareQuestion {
    /// The question is open over the Library.
    bool open{};
    /// The replay below Recordings.
    std::string path{};
    /// The title the question proposes. Empty is left off the request.
    std::string title{};
    /// The caret in `title`, in bytes, on a character boundary.
    std::size_t title_caret{};
    /// Who can see it. True is public, the one offered first.
    bool listed{true};
    /// A share is being sent. The question takes no other press.
    bool busy{};
    /// A sentence under the fields, when one is waiting.
    std::string note{};
};

/// Opens the sign-in question and closes the share question.
///
/// A system without TLS does not open it. The password starts empty.
///
/// @param[in,out] library the model
void open_sign_in(Library& library);

/// Closes the sign-in question and drops the password.
///
/// @param[in,out] library the model
void close_sign_in(Library& library);

/// Tells whether Sign in may be pressed.
///
/// The handle and the password are both filled, the system has TLS, and no
/// sign-in is already being sent.
///
/// @param dialog the question
/// @return true when Sign in may be pressed
[[nodiscard]] bool sign_in_can_submit(const SignInDialog& dialog) noexcept;

/// Opens the share question for one replay and closes the sign-in question.
///
/// The title is the one proposed, cut to the bytes a share accepts, on a
/// character boundary. Public is offered first.
///
/// @param[in,out] library the model
/// @param path the replay below Recordings
/// @param title the title the Library proposes
void open_share_question(Library& library, std::string path, std::string title);

/// Closes the share question.
///
/// @param[in,out] library the model
void close_share_question(Library& library);

/// Tells whether Share may be pressed.
///
/// A share that is already being sent may not. An empty title may.
///
/// @param dialog the question
/// @return true when Share may be pressed
[[nodiscard]] bool share_can_submit(const ShareQuestion& dialog) noexcept;

/// Chooses who can see the share.
///
/// @param[in,out] library the model
/// @param listed true for public, false for unlisted
void set_share_listed(Library& library, bool listed);

} // namespace oa::ui::library
