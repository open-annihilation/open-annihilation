// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A layout manifest as the bindings in use see it. Chords are parsed here,
// where the chord reader and the action registry are both in reach.
#pragma once

#include "oa/formats/oakeys/manifest.hpp"
#include "oa/ui/key_bindings/layout.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace oa::ui::key_bindings {

/// One problem found while turning a manifest into a layout.
struct LayoutProblem {
    uint32_t line{};       ///< the 1-based line
    uint32_t column{};     ///< the 1-based byte column
    std::string key{};     ///< the path, such as bindings.order.attack[0]
    std::string message{}; ///< the sentence
    bool warning{};        ///< a warning; the binding is kept
};

/// Turns a manifest into a layout.
///
/// Every chord is parsed. A chord that does not parse, and a modifier on its
/// own on an action whose kind is not a modifier, are errors at that chord.
/// An action this build does not know, a binding that leaves Escape out of a
/// fixed action or gives Escape to another action in a context that meets
/// it, a second binding of one chord, and a character the keyboard does not
/// type, are warnings. The layout is left unchanged when any chord is an
/// error. A warning keeps the binding it names.
///
/// @param manifest the manifest
/// @param keyboard the keyboard a character is checked against, or null to
///        skip that check
/// @param[out] layout the layout; left unchanged when any chord is an error
/// @param[in,out] problems one entry appended for each error and each warning
/// @return true when every chord parsed
[[nodiscard]] bool layout_from_manifest(
    const oa::formats::oakeys::Manifest& manifest,
    const KeyboardMap* keyboard,
    Layout& layout,
    std::vector<LayoutProblem>& problems
);

/// Builds a manifest from a layout.
///
/// `rest` supplies every key the layout does not hold. The id, the base and
/// the bindings come from the layout. Each chord is written in its canonical
/// text, and an action this build does not know is written back.
///
/// @param layout the layout
/// @param rest the other keys
/// @return the manifest
[[nodiscard]] oa::formats::oakeys::Manifest
manifest_from_layout(const Layout& layout, const oa::formats::oakeys::Manifest& rest);

} // namespace oa::ui::key_bindings
