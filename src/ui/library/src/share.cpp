// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/ui/library/share.hpp"

#include "oa/data/replays/share.hpp"
#include "oa/ui/library/export.hpp"
#include "oa/ui/library/library.hpp"

#include <algorithm>
#include <string>

namespace oa::ui::library {
namespace {

void wipe(std::string& secret) {
    oa::data::replays::wipe_secret(secret);
}

/// Cuts a title to the bytes a share accepts, on a character boundary.
std::string cap_title(std::string title) {
    if (title.size() <= oa::data::replays::share_title_bytes)
        return title;
    title.resize(oa::data::replays::share_title_bytes);
    while (!title.empty() && (static_cast<unsigned char>(title.back()) & 0xC0U) == 0x80U)
        title.pop_back();
    return title;
}

} // namespace

void open_sign_in(Library& library) {
    if (!library.sign_in.secure)
        return;
    close_share_question(library);
    if (library.export_dialog.open)
        close_export(library);
    SignInDialog& dialog = library.sign_in;
    dialog.open = true;
    dialog.busy = false;
    dialog.note.clear();
    wipe(dialog.password);
    dialog.password_caret = 0;
    dialog.handle_caret = dialog.handle.size();
}

void close_sign_in(Library& library) {
    SignInDialog& dialog = library.sign_in;
    dialog.open = false;
    dialog.busy = false;
    wipe(dialog.password);
    dialog.password_caret = 0;
}

bool sign_in_can_submit(const SignInDialog& dialog) noexcept {
    return dialog.secure && !dialog.busy && !dialog.handle.empty() && !dialog.password.empty();
}

void open_share_question(Library& library, std::string path, std::string title) {
    close_sign_in(library);
    if (library.export_dialog.open)
        close_export(library);
    ShareQuestion& dialog = library.share_question;
    dialog.open = true;
    dialog.busy = false;
    dialog.note.clear();
    dialog.path = std::move(path);
    dialog.title = cap_title(std::move(title));
    dialog.title_caret = dialog.title.size();
    dialog.listed = true;
}

void close_share_question(Library& library) {
    ShareQuestion& dialog = library.share_question;
    dialog.open = false;
    dialog.busy = false;
    dialog.path.clear();
    dialog.title.clear();
    dialog.title_caret = 0;
    dialog.note.clear();
}

bool share_can_submit(const ShareQuestion& dialog) noexcept {
    return dialog.open && !dialog.busy && !dialog.path.empty();
}

void set_share_listed(Library& library, bool listed) {
    if (!library.share_question.open || library.share_question.busy)
        return;
    library.share_question.listed = listed;
}

} // namespace oa::ui::library
