// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The brain picker's drawing: the kit's window face, the display list,
// and the kit's window edge over it. Nothing else draws.
#include "oa/ui/brain_picker/screen.hpp"

#include "oa/ui/kit/chrome.hpp"
#include "oa/ui/kit/components.hpp"

namespace oa::ui::brain_picker {

void draw_picker(const kit::Canvas& canvas, const kit::DisplayList& list, kit::Point size) {
    const kit::Rect whole{0, 0, size.x, size.y};
    kit::draw_window_face(canvas, whole);
    kit::paint(canvas, list);
    kit::draw_window_edge(canvas, whole);
}

} // namespace oa::ui::brain_picker
