// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/boneyards/screens/registration.hpp"

#include "text.hpp"

#include <cstdio>
#include <cstring>

namespace oa::boneyards::screens {

namespace {

WebAddressEntry g_entry{};

void copy_host(char* out, std::size_t capacity, const char* address) {
    const char* start = address != nullptr ? address : "";
    const char* scheme = std::strstr(start, "://");
    if (scheme != nullptr)
        start = scheme + 3;
    std::size_t length = 0;
    while (start[length] != '\0' && start[length] != '/' && start[length] != '?' &&
           start[length] != '#')
        ++length;
    if (capacity == 0)
        return;
    if (length >= capacity)
        length = capacity - 1;
    if (length > 0)
        std::memcpy(out, start, length);
    out[length] = '\0';
}

} // namespace

void registration_set_web_entry(WebAddressEntry entry) {
    g_entry = entry;
}

bool registration_browser_available() {
    return g_entry.available != nullptr && g_entry.available();
}

void registration_init(
    RegistrationConsole* console,
    const char* address,
    int32_t browser_available,
    int32_t in_game_route
) {
    if (console == nullptr)
        return;
    const char* chosen = address != nullptr && address[0] != '\0' ? address : kRegistrationAddress;
    copy_bounded(console->address, sizeof console->address, chosen);
    copy_host(console->host, sizeof console->host, console->address);
    console->browser_available = browser_available != 0 ? 1 : 0;
    console->in_game_route = in_game_route != 0 ? 1 : 0;
    console->route = 0;
    console->back = 0;
    console->opened_address[0] = '\0';
    console->open_count = 0;
}

void registration_select(RegistrationConsole* console, int32_t route) {
    if (console == nullptr)
        return;
    if (route != 0 && console->in_game_route == 0)
        return;
    console->route = route == 0 ? 0 : 1;
}

void registration_back(RegistrationConsole* console) {
    if (console != nullptr)
        console->back = 1;
}

void registration_open_browser(RegistrationConsole* console) {
    if (console == nullptr || console->browser_available == 0 || g_entry.open == nullptr)
        return;
    copy_bounded(console->opened_address, sizeof console->opened_address, console->address);
    ++console->open_count;
    g_entry.open(console->address);
}

void registration_open_address(const char* address) {
    if (address == nullptr || address[0] == '\0' || !registration_browser_available() ||
        g_entry.open == nullptr)
        return;
    g_entry.open(address);
}

namespace {

constexpr char kNameHint[] = "Choose a name of 3 to 15 characters.";
constexpr char kPasswordHint[] = "Your password travels as a digest, never in clear.";
constexpr char kReady[] = "Ready to enlist.";
constexpr char kEmptyName[] = "Enter a name.";
constexpr char kPasswordMismatch[] = "The passwords do not match.";
constexpr char kPasswordMark[] = "A password cannot contain an apostrophe.";
constexpr char kPasswordEmpty[] = "Enter a password.";
constexpr char kBadEmail[] = "Enter an e-mail address.";
constexpr char kRules[] = "Accept the Rules of Conduct.";
constexpr char kUnavailable[] =
    "This server does not take\\nregistrations from the game yet.\\nPlease use the web page.";
constexpr char kNameTaken[] = "That name is taken.\\nPlease choose another.";

bool blank_name(const char* text) {
    if (text == nullptr || text[0] == '\0')
        return true;
    for (const char* p = text; *p != '\0'; ++p) {
        if (*p != ' ')
            return false;
    }
    return true;
}

int32_t utf8_characters(const char* text) {
    int32_t count = 0;
    if (text == nullptr)
        return 0;
    const auto* p = reinterpret_cast<const unsigned char*>(text);
    while (*p != 0) {
        int32_t bytes = 1;
        if (*p >= 0xF0 && *p < 0xF8)
            bytes = 4;
        else if (*p >= 0xE0)
            bytes = 3;
        else if (*p >= 0xC0)
            bytes = 2;
        for (int32_t i = 1; i < bytes; ++i) {
            if (p[i] == 0 || (p[i] & 0xC0) != 0x80) {
                bytes = 1;
                break;
            }
        }
        p += bytes;
        ++count;
    }
    return count;
}

bool has_apostrophe(const char* text) {
    return text != nullptr && std::strchr(text, '\'') != nullptr;
}

bool email_plausible(const char* email) {
    if (email == nullptr || email[0] == '\0')
        return false;
    const char* at = std::strchr(email, '@');
    if (at == nullptr || at == email || std::strchr(at + 1, '@') != nullptr)
        return false;
    const char* dot = std::strrchr(at + 1, '.');
    if (dot == nullptr || dot == at + 1 || dot[1] == '\0' || dot[2] == '\0')
        return false;
    for (const char* p = email; *p != '\0'; ++p) {
        if (*p == ' ' || *p == '\t')
            return false;
    }
    return true;
}

int32_t judge(const RegistrationForm* form) {
    if (blank_name(form->name))
        return registration_check::empty_name;
    const int32_t count = utf8_characters(form->name);
    if (count > kRegistrationNameMax)
        return registration_check::name_too_long;
    if (count < kRegistrationNameMin)
        return registration_check::name_too_short;
    if (has_apostrophe(form->password) || has_apostrophe(form->confirm))
        return registration_check::password_mark;
    if (std::strcmp(form->password, form->confirm) != 0)
        return registration_check::password_mismatch;
    if (form->password[0] == '\0')
        return registration_check::password_empty;
    if (!email_plausible(form->email))
        return registration_check::bad_email;
    if (form->rules == 0)
        return registration_check::rules;
    return registration_check::ok;
}

const char* verdict(int32_t check) {
    switch (check) {
    case registration_check::empty_name:
        return kEmptyName;
    case registration_check::name_too_long:
    case registration_check::name_too_short:
        return kNameHint;
    case registration_check::password_mismatch:
        return kPasswordMismatch;
    case registration_check::password_mark:
        return kPasswordMark;
    case registration_check::password_empty:
        return kPasswordEmpty;
    case registration_check::bad_email:
        return kBadEmail;
    case registration_check::rules:
        return kRules;
    default:
        return kReady;
    }
}

void set_line(RegistrationForm* form, int32_t index, const char* text) {
    if (index < 0 || index >= kRegistrationStatusLines)
        return;
    copy_bounded(form->status[index], kRegistrationStatusBytes, text != nullptr ? text : "");
}

void set_idle(RegistrationForm* form, int32_t check) {
    set_line(form, 0, kNameHint);
    set_line(form, 1, kPasswordHint);
    set_line(form, 2, verdict(check));
    set_line(form, 3, "");
    form->status_count = 3;
    form->status_bright = -1;
    form->check = check;
}

void set_sending(RegistrationForm* form) {
    set_line(form, 0, "Contacting Central Command");
    set_line(form, 1, "Contact Established");
    char checking[kRegistrationStatusBytes];
    std::snprintf(checking, sizeof checking, "Checking name %s", form->name);
    set_line(form, 2, checking);
    set_line(form, 3, "Creating dossier...");
    form->status_count = 4;
    form->status_bright = 3;
    form->check = registration_check::ok;
    form->sending = 1;
    form->reply = registration_reply::none;
}

} // namespace

void registration_form_init(RegistrationForm* form) {
    if (form == nullptr)
        return;
    std::memset(form, 0, sizeof *form);
    form->side = registration_side::core;
    form->status_bright = -1;
    set_idle(form, registration_check::empty_name);
}

void registration_form_review(RegistrationForm* form) {
    if (form == nullptr || form->sending != 0)
        return;
    set_idle(form, judge(form));
}

int32_t registration_form_submit(RegistrationForm* form) {
    if (form == nullptr)
        return registration_check::bad_email;
    if (form->sending != 0)
        return form->check;
    const int32_t check = judge(form);
    if (check != registration_check::ok) {
        set_idle(form, check);
        return check;
    }
    set_sending(form);
    return registration_check::ok;
}

void registration_form_answer(RegistrationForm* form, int32_t reply) {
    if (form == nullptr || form->sending == 0)
        return;
    form->reply = reply;
}

void registration_form_receive(RegistrationForm* form) {
    registration_form_answer(form, registration_reply::unavailable);
}

void registration_form_abandon(RegistrationForm* form) {
    if (form == nullptr)
        return;
    form->sending = 0;
    form->reply = registration_reply::none;
    set_idle(form, judge(form));
}

void registration_reply_text(int32_t reply, const char* name, char* out, std::size_t capacity) {
    if (out == nullptr || capacity == 0)
        return;
    out[0] = '\0';
    if (reply == registration_reply::unavailable) {
        copy_bounded(out, capacity, kUnavailable);
        return;
    }
    if (reply == registration_reply::name_taken) {
        copy_bounded(out, capacity, kNameTaken);
        return;
    }
    if (reply == registration_reply::registered) {
        std::snprintf(
            out,
            capacity,
            "Welcome, %s!\\nYour dossier is ready.\\nLog in to begin.",
            name != nullptr ? name : ""
        );
    }
}

} // namespace oa::boneyards::screens
