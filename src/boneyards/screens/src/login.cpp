// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/boneyards/screens/login.hpp"

#include "oa/boneyards/release.hpp"
#include "oa/boneyards/shown_text.hpp"
#include "text.hpp"

#include <cstring>
#include <type_traits>

namespace oa::boneyards::screens {

void login_init(
    LoginDialog* d,
    const BoneyardsService* service,
    void* ui_user,
    void (*message_box)(void* user, const char* text)
) {
    // The dialog is trivially copyable; it is cleared in place.
    static_assert(std::is_trivially_copyable_v<LoginDialog>);
    std::memset(static_cast<void*>(d), 0, sizeof *d);
    d->service = service;
    d->ui_user = ui_user;
    d->message_box = message_box;
    d->member_value = 1;
    d->guest_value = kGuestsOffered ? 2 : 0;
    d->password_field_value = 1;
}

void login_set_status(LoginDialog* d, const char* text) {
    // A wrong password keeps the session's sentence and names the reset page
    // in the same status list. The catalogue translates that whole line.
    const char* english = text;
    if (text != nullptr && std::strcmp(text, kWrongPasswordStatus) == 0)
        english = kWrongPasswordResetStatus;
    const char* face = shown_text(english);
    copy_bounded(d->status, sizeof d->status, face);
    if (d->append_status != nullptr)
        d->append_status(d->status_user, face);
}

void login_on_control(LoginDialog* d, LoginControl control) {
    switch (control) {
    case LoginControl::login_button: {
        login_set_status(d, "Contacting Central Command");
        d->login_value = 0;
        copy_chars(d->session_name, sizeof d->session_name, d->name, kPlayerNameCharacters);
        const char* password = d->member_value == 1 ? d->password : "";
        copy_chars(
            d->session_password, sizeof d->session_password, password, kPlayerNameCharacters
        );
        d->state = LoginState::connecting;
        const LoginRequest request{d->session_name, d->session_password, false, d->server};
        const ServiceResult result = d->service->login(d->service->user, &request);
        if (result != ServiceResult::ok && result != ServiceResult::pending)
            login_on_result(d, false, false);
        break;
    }
    case LoginControl::cancel_button:
        login_set_status(d, "Transimision aborted by request.");
        d->service->cancel_login(d->service->user);
        d->state = LoginState::idle;
        d->login_value = 2;
        d->closed = false;
        login_set_status(d, "");
        break;
    case LoginControl::guest_radio:
        // Checks call this while the radio is drawn disabled. A player cannot.
        d->password[0] = '\0';
        d->password_field_value = 0;
        d->guest_value = 1;
        d->member_value = 2;
        break;
    case LoginControl::member_radio:
        d->password_field_value = 1;
        d->member_value = 1;
        d->guest_value = kGuestsOffered ? 2 : 0;
        break;
    case LoginControl::register_button:
        // The attached console shows the web page, and the in-game route beside
        // it when registration_form is set. With no console, REGISTER points at
        // that console.
        if (d->open_registration != nullptr) {
            d->open_registration(d->registration_user);
            break;
        }
        if (d->message_box != nullptr)
            d->message_box(d->ui_user, "To register please visit the Registration console");
        break;
    }
}

void login_set_registration_access(LoginDialog* d, int32_t beta_login_ux) {
    const int32_t released = release_flag(ReleaseFeature::registration_form);
    d->registration_form =
        feature_visible(ReleaseFeature::registration_form, 0, released, beta_login_ux) ? 1 : 0;
}

void login_update_button(LoginDialog* d) {
    const bool ready = d->name[0] != '\0' && (d->guest_value == 1 || d->password[0] != '\0');
    if (!ready)
        d->login_value = 0;
    else if (d->login_value == 0)
        d->login_value = 2;
}

bool login_on_enter(LoginDialog* d, bool password_focused) {
    if (!password_focused || d->login_value == 0)
        return false;
    login_on_control(d, LoginControl::login_button);
    return true;
}

void login_on_result(LoginDialog* d, bool connected, bool cancelled) {
    if (connected) {
        d->state = LoginState::planetary;
        d->closed = true;
        return;
    }
    login_set_status(d, cancelled ? "Transimision aborted by request." : "Server Not responding");
    login_on_reset(d);
}

void login_on_reset(LoginDialog* d) {
    d->state = LoginState::idle;
    d->login_value = 2;
}

} // namespace oa::boneyards::screens
