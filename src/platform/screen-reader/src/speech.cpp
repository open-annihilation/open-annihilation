// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The sentence a focus change speaks, and, on Windows, the system's own
// speech when the screen-reader bridge cannot run.
#include "oa/platform/screen_reader/speech.hpp"

#include "oa/platform/screen_reader.hpp"

#include <deque>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
// sapi.h is the system's speech (mingw-w64 or the Windows SDK). The class
// id is written out below, so this program does not link the speech library
// and does not import it.
#include <sapi.h>

#include <climits>
#endif

namespace oa::platform::screen_reader {
namespace {

std::string last_identity;
std::string last_label;
std::string last_role;

/// Appends one part of a spoken sentence.
///
/// @param[in,out] sentence the sentence so far
/// @param part the next part; empty adds nothing
void append_part(std::string& sentence, std::string_view part) {
    if (part.empty())
        return;
    if (!sentence.empty())
        sentence += ", ";
    sentence.append(part);
}

#if defined(_WIN32)
// {96749377-3391-11D2-9EE3-00C04F797396}, the system's voice. Written here
// so the program imports nothing from the speech library.
const CLSID speech_class = {
    0x96749377, 0x3391, 0x11d2, {0x9e, 0xe3, 0x00, 0xc0, 0x4f, 0x79, 0x73, 0x96}
};
const IID speech_interface = {
    0x6c44df74, 0x72b9, 0x4992, {0xa1, 0xec, 0xef, 0x99, 0x6e, 0x04, 0x22, 0xd4}
};

/// The voice, once it has been asked for. A failed ask stays failed, and
/// nothing is shown.
struct Voice {
    ISpVoice* voice{nullptr};
    bool asked{false};
};

/// UTF-8 text as the wide text the voice speaks. Invalid sequences are kept
/// as the system's replacement, so a line is still spoken.
///
/// @param text the sentence
/// @return the wide text, or empty when it cannot be converted
std::wstring wide_text(std::string_view text) {
    if (text.empty() || text.size() > static_cast<std::size_t>(INT_MAX))
        return {};
    const int bytes = static_cast<int>(text.size());
    const int needed = MultiByteToWideChar(CP_UTF8, 0, text.data(), bytes, nullptr, 0);
    if (needed <= 0)
        return {};
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    const int wrote = MultiByteToWideChar(CP_UTF8, 0, text.data(), bytes, wide.data(), needed);
    if (wrote <= 0)
        return {};
    wide.resize(static_cast<std::size_t>(wrote));
    return wide;
}

/// The system's voice, or null when this machine has none.
///
/// The first failed creation is remembered. No dialog is shown, and nothing
/// is written, whether the voice is missing or its library is not there.
/// The voice is kept for the rest of the run. Releasing it as the process
/// ends would run after the system has already shut the voice down.
///
/// @return the voice, or null
ISpVoice* voice_or_quiet() {
    static Voice held;
    if (held.asked)
        return held.voice;
    held.asked = true;
    const UINT previous = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
#if defined(OA_WINDOWS_95)
    const HRESULT started = CoInitialize(nullptr);
#else
    const HRESULT started = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
#endif
    const bool ready = SUCCEEDED(started) || started == RPC_E_CHANGED_MODE;
    if (ready) {
        ISpVoice* created = nullptr;
        const HRESULT made = CoCreateInstance(
            speech_class,
            nullptr,
            CLSCTX_INPROC_SERVER,
            speech_interface,
            reinterpret_cast<void**>(&created)
        );
        if (SUCCEEDED(made))
            held.voice = created;
    }
    SetErrorMode(previous);
    return held.voice;
}

/// Lines handed to the voice and not yet finished. The voice reads the
/// buffer until it has said it, so a line stays here. A line that stops the
/// voice ends the earlier lines, and once the voice has said everything the
/// buffers can go.
std::deque<std::wstring> spoken_lines;

/// When the last announcement was handed to the voice, from `GetTickCount`.
DWORD announced_at = 0;
/// Whether an announcement has been handed to the voice yet.
bool announced = false;

/// Forgets the held lines once the voice has said all of them.
///
/// @param voice the voice
void drop_said_lines(ISpVoice* voice) {
    SPVOICESTATUS status{};
    LPWSTR bookmark = nullptr;
    const HRESULT asked = voice->GetStatus(&status, &bookmark);
    CoTaskMemFree(bookmark);
    if (SUCCEEDED(asked) && (status.dwRunningState & SPRS_DONE) != 0)
        spoken_lines.clear();
}

/// Speaks one line. An empty focus line that may cut off the voice stops
/// the previous line and says nothing. A missing voice does nothing.
///
/// @param text the sentence
/// @param line what it is, which decides whether it stops the voice first
void speak_now(std::string_view text, SpokenLine line) {
    if (!speech_where_bridge_cannot())
        return;
    if (text.empty() && line != SpokenLine::focus)
        return;
    ISpVoice* voice = voice_or_quiet();
    if (voice == nullptr)
        return;
    const DWORD now = GetTickCount();
    const int64_t since =
        announced ? static_cast<int64_t>(static_cast<DWORD>(now - announced_at)) : -1;
    const bool cut = line_cuts_off(line, since);
    if (line != SpokenLine::focus && !text.empty()) {
        announced = true;
        announced_at = now;
    }
    drop_said_lines(voice);
    // A chat line can hold a "<". It is spoken as words, not as markup.
    DWORD flags = SPF_ASYNC | SPF_IS_NOT_XML;
    if (cut)
        flags |= SPF_PURGEBEFORESPEAK;
    const std::wstring wide = wide_text(text);
    if (wide.empty()) {
        if (cut) {
            voice->Speak(L"", flags, nullptr);
            spoken_lines.clear();
        }
        return;
    }
    // Enough held lines that an earlier one has been said, or a focus
    // change has stopped it. Past this, stop the backlog and keep the new
    // line only, so a burst cannot grow without limit.
    constexpr std::size_t kept_lines = 32;
    if (spoken_lines.size() >= kept_lines)
        flags |= SPF_PURGEBEFORESPEAK;
    spoken_lines.push_back(wide);
    voice->Speak(spoken_lines.back().c_str(), flags, nullptr);
    if ((flags & SPF_PURGEBEFORESPEAK) != 0 && spoken_lines.size() > 1)
        spoken_lines.erase(spoken_lines.begin(), spoken_lines.end() - 1);
}
#endif

} // namespace

std::string_view role_words(SpokenRole role) {
    switch (role) {
    case SpokenRole::none:
        return {};
    case SpokenRole::button:
        return "button";
    case SpokenRole::check_box:
        return "check box";
    case SpokenRole::switch_control:
        return "switch";
    case SpokenRole::radio_group:
        return "radio group";
    case SpokenRole::radio:
        return "radio button";
    case SpokenRole::tab_list:
        return "tab list";
    case SpokenRole::tab:
        return "tab";
    case SpokenRole::list:
        return "list";
    case SpokenRole::list_item:
        return "list item";
    case SpokenRole::tree:
        return "tree";
    case SpokenRole::tree_item:
        return "tree item";
    case SpokenRole::text_field:
        return "text field";
    case SpokenRole::password:
        return "password";
    case SpokenRole::search:
        return "search field";
    case SpokenRole::slider:
        return "slider";
    case SpokenRole::log:
        return "log";
    case SpokenRole::dialog:
        return "dialog";
    case SpokenRole::alert_dialog:
        return "alert";
    case SpokenRole::group:
        return "group";
    case SpokenRole::image:
        return "image";
    case SpokenRole::link:
        return "link";
    case SpokenRole::label:
        return "text";
    case SpokenRole::window:
        return "window";
    }
    return {};
}

bool line_cuts_off(SpokenLine line, int64_t since_announcement_ms) {
    if (line == SpokenLine::polite)
        return false;
    return since_announcement_ms < 0 || since_announcement_ms >= announcement_kept_ms;
}

std::string focus_sentence(std::string_view label, std::string_view role, std::string_view value) {
    std::string sentence;
    sentence.reserve(label.size() + role.size() + value.size() + 4);
    append_part(sentence, label);
    append_part(sentence, role);
    append_part(sentence, value);
    return sentence;
}

void forget_spoken_focus() {
    last_identity.clear();
    last_label.clear();
    last_role.clear();
}

void speak_focus(
    std::string_view identity, std::string_view label, std::string_view role, std::string_view value
) {
    if (!speech_where_bridge_cannot())
        return;
    if (identity == last_identity && label == last_label && role == last_role)
        return;
    last_identity.assign(identity);
    last_label.assign(label);
    last_role.assign(role);
#if defined(_WIN32)
    speak_now(focus_sentence(label, role, value), SpokenLine::focus);
#else
    (void)value;
#endif
}

void speak_announcement(std::string_view text, Politeness politeness) {
#if defined(_WIN32)
    speak_now(
        text, politeness == Politeness::assertive ? SpokenLine::assertive : SpokenLine::polite
    );
#else
    (void)text;
    (void)politeness;
#endif
}

} // namespace oa::platform::screen_reader
