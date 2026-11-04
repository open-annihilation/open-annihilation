// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Platform scaffolding: the markup text service over the fonts bundled with
// the game, standing in for the Windows faces the game's pages name.
#pragma once

#include "oa/boneyards/html_fonts.hpp"

namespace oa::platform {
struct FontLibrary;
}

namespace oa::boneyards::html {

/// Returns a markup text service over a platform font library; text is
/// measured and drawn unsmoothed, as the pages draw it onto the 8-bit canvas.
///
/// Each face keeps its own glyphs. A character the face does not have is
/// drawn from DejaVu Sans, then the engine's text faces for Chinese,
/// Japanese and Korean, then Noto Emoji, the chain page_script_fonts opens.
/// No missing-glyph box is drawn: a character no face has, and a control
/// character such as a carriage return or a tab, draws nothing and takes no
/// room.
///
/// @param library font library to open faces from; must outlive every font
///     the service opens; null gives no service (every slot null)
/// @return the service
FontService host_font_service(oa::platform::FontLibrary* library);

} // namespace oa::boneyards::html
