// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// HTMLView: a Boneyards window that shows one markup document (help pages,
// news, profile reports) with an HTMLSlider scrolling it.
#pragma once

#include <cstdint>
#include <vector>

#include "oa/boneyards/html.hpp"
#include "oa/boneyards/html_fnt.hpp"
#include "oa/boneyards/html_fonts.hpp"
#include "oa/boneyards/html_scripts.hpp"
#include "oa/boneyards/toolkit.hpp"
#include "oa/formats/fnt.hpp"
#include "oa/present/surface.h"

namespace oa::boneyards::console {

struct StatusWidget;

// What an HTML view holds besides its window: the document, the names of
// its missing images and the fonts its text is set in (the host's faces,
// with the FNT fonts where it has none).
struct HtmlViewState {
    html::Document doc{};
    std::vector<const char*> pending;
    oa::formats::fnt::Font regular{};
    oa::formats::fnt::Font large{};
    html::FntFonts fonts{};
    bool has_fonts{};
    html::ServiceFonts text_fonts{};
    // Characters the FNT fonts lack, drawn from the page chain. `fonts.scripts`
    // points here when the chain opened.
    html::ScriptFonts scripts{};
};

// Code an HTMLView sends its parent when a link is clicked (the value the
// toolkit also uses for a released scroll button).
inline constexpr NotifyCode k_html_link_notify = static_cast<NotifyCode>(200);
// Links past this many are not keyboard stops. The page still shows them.
inline constexpr int32_t k_html_link_stop_cap = 64;
// Palette index the view's canvas leaves transparent when drawn.
inline constexpr uint8_t k_html_view_key = 9;

// A window showing one HTML page.
struct HtmlView {
    Window base;
    HtmlViewState* state; // the document, names of images still missing
    Slider* slider;
    Surface region; // part of the canvas the view shows; drawn once it has pixels
    // Target of the last clicked link.
    const char* link;
    StatusWidget* owner; // image store and host tables
    // One hotspot per link, in document order. The coordinates are the
    // document's, so a scroll can move the hotspot without rebuilding it.
    // The hrefs point into the document buffer and die with the next page.
    Window* link_stops[k_html_link_stop_cap]{};
    const char* link_hrefs[k_html_link_stop_cap]{};
    int32_t link_doc_x[k_html_link_stop_cap]{};
    int32_t link_doc_top[k_html_link_stop_cap]{};
    int32_t link_doc_width[k_html_link_stop_cap]{};
    int32_t link_doc_height[k_html_link_stop_cap]{};
    int32_t link_stop_count{};
    // Where the keyboard goes when a followed link takes its stop away: the
    // console's list. Null leaves it on the page.
    Window* keyboard_home{};
};

/// Creates a hidden HTML view inside its parent with its slider.
///
/// Loads the FNT fonts used without a host text service and lays the
/// document out 0x17 pixels narrower than the view.
///
/// @param owner status widget whose files, palette and text fonts the view uses
/// @param[in,out] parent window the view is placed in
/// @param dx left edge relative to the parent, in pixels
/// @param dy top edge relative to the parent, in pixels
/// @param width view width in pixels
/// @param height view height in pixels
/// @return the view
HtmlView* html_view_create(
    StatusWidget* owner, Window* parent, int32_t dx, int32_t dy, int32_t width, int32_t height
);

/// Shows a markup document in the view.
///
/// Loads and paints it, collects its missing images and sizes the slider to
/// the part that does not fit, starting at the top. Without usable fonts
/// nothing is shown.
///
/// @param[in,out] view HTML view
/// @param text markup text
void html_view_set_text(HtmlView* view, const char* text);

/// Reloads the images named `name` from a file and repaints the view.
///
/// @param[in,out] view HTML view
/// @param name image name as the document references it
/// @param path file holding the image
void html_view_set_pair(HtmlView* view, const char* name, const char* path);

/// Creates a vertical HTMLSlider; its buttons are made when it is placed.
///
/// @return the slider
Slider* html_slider_create();

/// Removes and returns the last name of an image still missing.
///
/// @param[in,out] view HTML view
/// @return the name, or null when none is missing
const char* html_view_pop_pending(HtmlView* view);

} // namespace oa::boneyards::console
