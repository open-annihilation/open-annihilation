// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Keys an older tool stored, read as places. Windows virtual-key numbers
// become UI Events codes. The reader looks only at the named lines and
// writes nothing back.
#pragma once

#include "oa/ui/key_bindings/chord.hpp"
#include "oa/ui/key_bindings/layout_file.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::ui::key_bindings {

/// The UI Events code of one Windows virtual-key number.
///
/// Letters, digits, the function keys, the named editing keys and the
/// punctuation the table names are places. Shift, Ctrl and Alt, including
/// the left-hand and right-hand numbers, are that modifier's key code.
/// Anything else is not in the table.
///
/// @param code the virtual-key number
/// @return the UI Events code, or nothing when the number is not in the table
[[nodiscard]] std::optional<std::string_view> code_of_virtual_key(uint32_t code);

/// The modifier bit of a Windows virtual-key number that is Shift, Ctrl or Alt.
///
/// The generic number and both hands are the same bit. A number that is not
/// one of those nine is not a modifier.
///
/// @param code the virtual-key number
/// @return Shift, Ctrl or Alt, or nothing
[[nodiscard]] std::optional<uint8_t> modifier_of_virtual_key(uint32_t code);

/// One key an older tool stored.
struct LegacyKey {
    std::string action{};        ///< the action id; empty when Open Annihilation has none
    std::vector<Chord> chords{}; ///< the chords, as places
    std::string source_line{};   ///< the line as the file wrote it
    uint32_t line{};             ///< the 1-based line
    std::string note{};          ///< a sentence the row shows; empty when it has none
};

/// The keys a legacy file held, and the lines that could not be read.
struct LegacyRead {
    std::vector<LegacyKey> keys{};         ///< the keys, in file order
    std::vector<LayoutProblem> problems{}; ///< unknown names and numbers, and a file past the limit
};

/// Reads tdraw's `[Settings]` keys.
///
/// Only that section is read. Names match ignoring case: `WhiteboardKey` is
/// `whiteboard.draw`, `MegaMapKey` and `MegamapKey` are `megamap.toggle`,
/// `BuildLinesRingsKey` and `KeyCode` are `build.line`, `ClickSnapOverrideKey`
/// is `build.snap-override`, and `RotateBuildKey` is `build.rotate`. A value
/// is decimal or `0x` hex. `0` is off. An unknown name or number is reported
/// and skipped. At most 64 KiB are read.
///
/// @param bytes the file
/// @return the keys and the problems
[[nodiscard]] LegacyRead read_tdraw_settings(std::span<const uint8_t> bytes);

/// Reads cnc-ddraw's hotkey lines.
///
/// `[ddraw]` is read, then `[TotalA]` replaces any of the same names. `0x00`
/// is off. `keytogglefullscreen` is `ui.full-screen` as Alt and the key,
/// `keyunlockcursor1` is `ui.release-pointer` as Ctrl and the key,
/// `keyunlockcursor2` is that action's second chord as Alt and the key,
/// and `keyscreenshot` is `game.screenshot`. `keytogglemaximize` has no
/// action. A left-hand or right-hand modifier says that Open Annihilation
/// joins the two hands. Anything else in the file is not read.
///
/// @param bytes the file
/// @return the keys and the problems
[[nodiscard]] LegacyRead read_cnc_ddraw(std::span<const uint8_t> bytes);

} // namespace oa::ui::key_bindings
