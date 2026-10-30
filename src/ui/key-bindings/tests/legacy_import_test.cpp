// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Windows virtual-key numbers become key places. tdraw's [Settings] keys and
// cnc-ddraw's hotkey lines are read; nothing else is, and nothing is written.

#include "oa/ui/key_bindings/legacy_import.hpp"

#include "oa/test/check.hpp"
#include "oa/ui/key_bindings/chord.hpp"
#include "oa/ui/key_bindings/key_codes.hpp"

#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace keys = oa::ui::key_bindings;

/// Views a text's bytes.
///
/// @param text the text
/// @return its bytes
std::span<const uint8_t> as_bytes(std::string_view text) {
    return {reinterpret_cast<const uint8_t*>(text.data()), text.size()};
}

/// Prints a line when a check is about to fail, then checks it.
///
/// @param condition the check
/// @param detail what failed
void expect(bool condition, const std::string& detail) {
    if (!condition)
        std::fprintf(stderr, "%s\n", detail.c_str());
    OA_CHECK(condition);
}

/// The canonical text of one action's first chord, or empty.
///
/// @param read the file
/// @param action the action id
/// @return the chord text
std::string first_text(const keys::LegacyRead& read, std::string_view action) {
    for (const keys::LegacyKey& key : read.keys) {
        if (key.action == action && !key.chords.empty())
            return keys::chord_text(key.chords.front());
    }
    return {};
}

/// True when a problem names this key.
///
/// @param read the file
/// @param key the name
/// @return true when it was reported
bool reported(const keys::LegacyRead& read, std::string_view key) {
    for (const keys::LayoutProblem& problem : read.problems) {
        if (problem.key == key)
            return true;
    }
    return false;
}

/// A [Settings] file of the shape a later tdraw build stores.
void tdraw_sample() {
    constexpr std::string_view text = "[Game]\n"
                                      "PlayMode=1\n"
                                      "[Settings]\n"
                                      "WhiteboardKey=114\n"
                                      "megamapkey=0x73\n"
                                      "BuildLinesRingsKey=88\n"
                                      "ClickSnapOverrideKey=0\n"
                                      "NotAKey=112\n"
                                      "RotateBuildKey=9999\n";
    const keys::LegacyRead read = keys::read_tdraw_settings(as_bytes(text));
    expect(first_text(read, "whiteboard.draw") == "F3", "WhiteboardKey=114 is not F3");
    expect(first_text(read, "megamap.toggle") == "F4", "megamapkey=0x73 is not F4");
    expect(first_text(read, "build.line") == "@KeyX", "BuildLinesRingsKey=88 is not @KeyX");
    expect(first_text(read, "build.snap-override").empty(), "0 was read as a chord");
    expect(reported(read, "NotAKey"), "an unknown key name was not reported");
    expect(reported(read, "RotateBuildKey"), "an unknown Windows number was not reported");
    for (const keys::LegacyKey& key : read.keys)
        expect(key.action != "build.snap-override", "a zero value became a binding");
    expect(read.keys.size() == 3, "the sample did not keep three keys");
}

/// [ddraw] hotkeys, with [TotalA] turning one off, and a key OA does not use.
void ddraw_sample() {
    constexpr std::string_view text = "[ddraw]\n"
                                      "keytogglefullscreen=0x0D\n"
                                      "keytogglemaximize=0x22\n"
                                      "keyunlockcursor1=0x09\n"
                                      "keyunlockcursor2=0xA3\n"
                                      "keyscreenshot=0x2C\n"
                                      "[other]\n"
                                      "keytogglefullscreen=0x70\n"
                                      "[TotalA]\n"
                                      "keytogglefullscreen=0x00\n";
    const keys::LegacyRead read = keys::read_cnc_ddraw(as_bytes(text));
    expect(first_text(read, "ui.full-screen").empty(), "TotalA did not turn full screen off");
    bool unused = false;
    bool noted = false;
    std::size_t release = 0;
    for (const keys::LegacyKey& key : read.keys) {
        if (key.action.empty())
            unused = true;
        if (key.action == "ui.release-pointer") {
            release = key.chords.size();
            if (key.note.find("Left and right") != std::string::npos &&
                key.note.find("Ctrl") != std::string::npos)
                noted = true;
        }
    }
    expect(unused, "keytogglemaximize was not listed");
    expect(release == 2, "the two unlock chords did not join");
    expect(noted, "the right-hand Ctrl did not say the hands are one key");
    expect(first_text(read, "game.screenshot") == "PrintScreen", "screenshot is not PrintScreen");
    const std::string release_first = first_text(read, "ui.release-pointer");
    expect(release_first == "Ctrl+Tab", "unlock is not Ctrl+Tab, it is " + release_first);
}

/// Every number of the table is a code K04 knows. An unknown number is not.
void every_code() {
    constexpr uint32_t numbers[] = {
        0x08, 0x09, 0x0D, 0x10, 0x11, 0x12, 0x13, 0x1B, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26,
        0x27, 0x28, 0x2C, 0x2D, 0x2E, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
        0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F,
        0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x5D, 0x60, 0x61, 0x62,
        0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x6B, 0x6D, 0x6E, 0x6F, 0x70, 0x71, 0x72,
        0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x7B, 0x7C, 0x7D, 0x7E, 0x7F, 0x80, 0x81,
        0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xBA, 0xBB, 0xBC,
        0xBD, 0xBE, 0xBF, 0xC0, 0xDB, 0xDC, 0xDD, 0xDE, 0xE2,
    };
    for (const uint32_t number : numbers) {
        const std::optional<std::string_view> code = keys::code_of_virtual_key(number);
        expect(code.has_value(), "a table number has no code");
        if (!code)
            continue;
        expect(
            keys::scancode_of(*code).has_value(), std::string{*code} + " is not a code K04 knows"
        );
    }
    expect(!keys::code_of_virtual_key(0x01), "an unknown number was given a code");
    expect(keys::modifier_of_virtual_key(0xA3) == keys::ctrl, "right Ctrl is not Ctrl");
    expect(!keys::modifier_of_virtual_key(0x41), "A was read as a modifier");
}

/// A file past 64 KiB is reported, and the keys inside the limit are still read.
void past_the_limit() {
    std::string text = "[Settings]\nWhiteboardKey=114\n";
    text.append(64u * 1024u, ' ');
    text += "\nMegaMapKey=115\n";
    const keys::LegacyRead read = keys::read_tdraw_settings(as_bytes(text));
    expect(reported(read, "file"), "a file past 64 KiB was not reported");
    expect(first_text(read, "whiteboard.draw") == "F3", "the key inside the limit was dropped");
}

} // namespace

int main() {
    tdraw_sample();
    ddraw_sample();
    every_code();
    past_the_limit();
    return 0;
}
