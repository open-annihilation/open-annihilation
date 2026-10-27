// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The UI kit gallery: every component of the kit, working on the gallery's
// own model and in each fixed state, laid out in sections that flow in one,
// two or three columns by the frame's size class. Only the kit's add_*
// functions, its looks and its generic roles build it, and the kit's paint
// draws it.

#include "oa/ui/kit/gallery.hpp"

#include "oa/ui/kit/chrome.hpp"
#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/components_more.hpp"
#include "oa/ui/kit/controls.hpp"
#include "oa/ui/kit/first_run.hpp"
#include "oa/ui/kit/keys.hpp"
#include "oa/ui/kit/theme.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::ui::kit {

namespace {

/// The sizes the gallery's samples keep in every size class, as the
/// components keep them.
constexpr const Metrics& kMetrics = compact_metrics;

// ---------------------------------------------------------------------------
// The working samples' controls, numbered in the order Tab visits them.

constexpr std::size_t kTabCount = 3;     ///< the working tabs
constexpr std::size_t kChipCount = 3;    ///< the working filter chips
constexpr std::size_t kNavCount = 4;     ///< the working nav list's entries
constexpr std::size_t kRowCount = 3;     ///< the working list rows
constexpr std::size_t kListRowCount = 4; ///< the scroll list's rows
constexpr std::size_t kCardCount = 3;    ///< the working cards
constexpr std::size_t kLevelCount = 3;   ///< the working strip's levels

constexpr ControlId kFirstTab = 1;
constexpr ControlId kButton = kFirstTab + static_cast<ControlId>(kTabCount);
constexpr ControlId kLink = kButton + 1;
constexpr ControlId kToggle = kLink + 1;
constexpr ControlId kLevels = kToggle + 1;
constexpr ControlId kChoice = kLevels + 1;
constexpr ControlId kSlider = kChoice + 1;
constexpr ControlId kSearch = kSlider + 1;
constexpr ControlId kFirstChip = kSearch + 1;
constexpr ControlId kFirstNav = kFirstChip + static_cast<ControlId>(kChipCount);
constexpr ControlId kFirstRow = kFirstNav + static_cast<ControlId>(kNavCount);
constexpr ControlId kFirstListRow = kFirstRow + static_cast<ControlId>(kRowCount);
constexpr ControlId kFirstCard = kFirstListRow + static_cast<ControlId>(kListRowCount);
constexpr ControlId kOaButton = kFirstCard + static_cast<ControlId>(kCardCount);
constexpr ControlId kClose = kOaButton + 1;

constexpr std::size_t kCarouselCount = 5; ///< the working carousel's cards
constexpr std::size_t kChoiceCount = 2;   ///< the working choice cards
constexpr ControlId kCarouselPrevious = kClose + 1;
constexpr ControlId kFirstCarousel = kCarouselPrevious + 1;
constexpr ControlId kCarouselNext = kFirstCarousel + static_cast<ControlId>(kCarouselCount);
constexpr ControlId kFirstChoice = kCarouselNext + 1;
constexpr ControlId kSetupSkip = kFirstChoice + static_cast<ControlId>(kChoiceCount);
constexpr ControlId kSetupBack = kSetupSkip + 1;
constexpr ControlId kSetupNext = kSetupBack + 1;
constexpr ControlId kCallToAction = kSetupNext + 1;
constexpr ControlId kLanguageCard = kCallToAction + 1;
constexpr ControlId kKeycap = kLanguageCard + 1;

static_assert(
    std::tuple_size_v<decltype(GalleryModel::chips)> == kChipCount,
    "the model holds one value for each working filter chip"
);

/// The scroll list's rows' scroll area: the only one the gallery numbers.
constexpr int32_t kRowsGroup = 0;

// ---------------------------------------------------------------------------
// Sizes, in points.

constexpr int32_t kHeadingHeight = kMetrics.regular_line; ///< a section's heading
constexpr int32_t kAfterRule = 6;                         ///< under the rule under a heading
constexpr int32_t kSectionGap = 14;                       ///< between two sections of a column
constexpr int32_t kFirstRunTop = 0;                       ///< First run starts flush with the view
constexpr int32_t kContentBottom = 10;                    ///< under the last section of a column
constexpr int32_t kLineGap = 8;                           ///< between two lines of samples
constexpr int32_t kCellGap = 8;                           ///< between two samples of a line
constexpr int32_t kCaptionGap = 2;                        ///< between a sample and its caption
constexpr int32_t kLeastCell = 56;                        ///< the least width of a sample's cell
constexpr int32_t kButtonWidth = 76;                      ///< a sample button's width
constexpr int32_t kFieldWidth = 150;                      ///< a sample drop-down's field
constexpr int32_t kLevelWidth = 40;      ///< a sample strip's level, inside its border
constexpr int32_t kSliderWidth = 110;    ///< a sample slider
constexpr int32_t kSearchWidth = 140;    ///< a sample search field
constexpr int32_t kNavWidth = 160;       ///< the working nav list's face
constexpr int32_t kNavEntryHeight = 20;  ///< a nav entry
constexpr int32_t kNavInset = 6;         ///< a nav entry's left in its face
constexpr int32_t kNavTop = 8;           ///< the first nav entry's top in its face
constexpr int32_t kNavGap = 1;           ///< between two nav entries
constexpr int32_t kNavDividerMargin = 4; ///< round the nav list's divider
constexpr int32_t kNavSampleWidth = 140; ///< a sample nav entry
constexpr int32_t kRowGap = 4;           ///< between two working list rows
constexpr int32_t kShortRowWidth = 180;  ///< the least width of a sample short row
constexpr int32_t kListRowGap = 2;       ///< between two scroll list rows
constexpr int32_t kListView = 74;        ///< the scroll list's view's height
constexpr int32_t kScrollRoom = 12;      ///< the scroll list's room for its bar
constexpr int32_t kCardWidth = 120;      ///< a working card's most width
constexpr int32_t kSampleCardWidth = 84; ///< a sample card
constexpr int32_t kAnchorSide = 24;      ///< the hover card's anchor
constexpr int32_t kOaSide = 24;          ///< a sample OA button's side
constexpr int32_t kMarkSide = 32;        ///< the OA mark's and the icon's side
constexpr int32_t kMarkGap = 16;         ///< between the OA mark's samples
constexpr int32_t kWindowSample = 300;   ///< the window chrome's sample's most width
constexpr int32_t kWindowHeight = 80;    ///< the window chrome's sample's height
constexpr int32_t kWindowMargin = 8;     ///< round it, over the darkened screen
constexpr int32_t kWheelStep = 2 * kMetrics.small_line; ///< what a notch of the wheel scrolls

// ---------------------------------------------------------------------------
// The sample texts. None names a real mod, map, person or service, or the
// build's version.

constexpr std::string_view kTitle = "Open Annihilation";
constexpr std::string_view kSecond = "UI kit gallery";
constexpr std::string_view kCloseCaption = "Close";
constexpr std::array<std::string_view, kTabCount> kTabCaptions{"Mods", "Maps", "Updates"};
constexpr std::array<int32_t, kTabCount> kTabCounts{0, 0, 2};
constexpr std::array<std::string_view, kLevelCount> kLevelCaptions{"Off", "Basic", "Full"};
constexpr std::array<std::string_view, kLevelCount> kLevelWords{"off", "basic", "full"};
constexpr std::array<std::string_view, 4> kLanguageWords{
    "english", "deutsch", "espanol", "francais"
};
constexpr std::array<std::string_view, kNavCount> kNavCaptions{
    "Mods", "Language", "Graphics", "Downloads"
};

/// One package of the samples: a row's, a card's and a menu's words.
struct Package {
    std::string_view name{};    ///< its name
    std::string_view letters{}; ///< its badge's letters
    std::string_view line{};    ///< the line under its name
    std::string_view version{}; ///< its version
    std::string_view size{};    ///< its size
    ChipState state{};          ///< the chip it shows
    std::string_view chip{};    ///< the chip's words
};

constexpr std::array<Package, 4> kPackages{{
    {"Sample Mod",
     "SM",
     "A sample row: a title, a byline and a line.",
     "4.8 · rev 3",
     "41.0 MB",
     ChipState::update,
     "Update"},
    {"Example Maps",
     "EM",
     "Six sample maps.",
     "1.2 · 2 maps",
     "12.0 MB",
     ChipState::installed,
     "Installed"},
    {"Another Mod", "AM", "A row that can be fetched.", "2.0", "8.5 MB", ChipState::get, "Get"},
    {"Third Mod", "TM", "The last of the samples.", "1.0", "3.1 MB", ChipState::playing, "Playing"},
}};

constexpr std::string_view kByline = "by the sample team";

// ---------------------------------------------------------------------------
// Fixed states.

/// A fixed state a sample shows.
enum class Sample : uint8_t {
    idle,     ///< at rest
    hovered,  ///< the pointer over it
    pressed,  ///< a press held on it
    focused,  ///< the keyboard focus on it
    disabled, ///< taking nothing
    locked,   ///< showing a value it cannot change
    selected, ///< the chosen one of its group
};

/// The five states every component that takes input shows.
constexpr std::array<Sample, 5> kFiveStates{
    Sample::idle, Sample::hovered, Sample::pressed, Sample::focused, Sample::disabled
};
/// The five states and locked, for a component with a lock.
constexpr std::array<Sample, 6> kLockedStates{
    Sample::idle,
    Sample::hovered,
    Sample::pressed,
    Sample::focused,
    Sample::disabled,
    Sample::locked
};
/// The five states and selected, for a component chosen among others.
constexpr std::array<Sample, 6> kSelectableStates{
    Sample::idle,
    Sample::hovered,
    Sample::pressed,
    Sample::focused,
    Sample::disabled,
    Sample::selected
};

/// Returns the caption under a sample in a state.
///
/// @param shown the state
/// @return its caption
std::string_view caption_of(Sample shown) noexcept {
    switch (shown) {
    case Sample::idle:
        return "Idle";
    case Sample::hovered:
        return "Hovered";
    case Sample::pressed:
        return "Pressed";
    case Sample::focused:
        return "Focused";
    case Sample::disabled:
        return "Disabled";
    case Sample::locked:
        return "Locked";
    case Sample::selected:
        return "Selected";
    }
    return {};
}

/// Tells whether a sample in a state looks hovered: hovered, or pressed,
/// which a press held on it is with the pointer over it.
///
/// @param shown the state
/// @return true when it does
constexpr bool looks_hovered(Sample shown) noexcept {
    return shown == Sample::hovered || shown == Sample::pressed;
}

// ---------------------------------------------------------------------------
// Building.

/// What the sections are built into.
struct Builder {
    const GalleryModel* model{}; ///< the model
    const Fonts* fonts{};        ///< the fonts
    DisplayList list{};          ///< the gallery's parts and controls
    std::vector<GalleryPanel> panels{};
    std::vector<ControlId> order{};    ///< the declared Tab order
    Rect view{};                       ///< the sections' view
    ScrollArea rows{};                 ///< the scroll list's
    Rect menu{};                       ///< the working drop-down's open menu
    ChoiceMenuLook menu_look{};        ///< what it shows
    std::optional<Item> menu_part{};   ///< its part, drawn over the sections
    const Metrics* metrics{&kMetrics}; ///< the frame's size class
};

/// Appends a part of a role at a rectangle.
///
/// @param[in,out] list the display list
/// @param role what it draws
/// @param rect where, in points
/// @return the part, valid until the list next grows
Item& add_part(DisplayList& list, Role role, const Rect& rect) {
    Item& part = list.items.emplace_back();
    part.role = role;
    part.rect = rect;
    return part;
}

/// Appends a line of text.
///
/// @param[in,out] list the display list
/// @param rect its box, in points
/// @param words the text
/// @param font its font
/// @param align where it sits in its box
/// @param ink its colour
void add_words(
    DisplayList& list,
    const Rect& rect,
    std::string_view words,
    FontRole font,
    Align align,
    Colour ink
) {
    Item& part = add_part(list, Role::text, rect);
    part.text = std::string(words);
    part.font = font;
    part.align = align;
    part.colour = ink;
}

/// Returns a working sample's automation name: gallery.<section>.<n>.
///
/// @param section the section's word
/// @param place the sample's place in its section, from 1
/// @return the name
std::string named(std::string_view section, std::size_t place) {
    return "gallery." + std::string(section) + "." + std::to_string(place);
}

/// Tells whether a working control has the focus shown.
///
/// @param model the model
/// @param id the control
/// @return true when it does
bool focus_on(const GalleryModel& model, ControlId id) noexcept {
    return model.interaction.focus_shown && model.interaction.focused == id;
}

/// Tells whether the pointer is over a working control, or holds it.
///
/// @param model the model
/// @param id the control
/// @return true when it is
bool pointer_on(const GalleryModel& model, ControlId id) noexcept {
    return model.interaction.hovered == id || model.interaction.pressed == id;
}

/// Gives a working sample's part the control it draws and the pointer's and
/// the focus's state of it, and the focus ring round it when it has the focus.
///
/// @param[in,out] b what is built
/// @param part_at the part's place in the list
/// @param id the control
/// @param ring the rectangle the focus ring goes round; empty for none
void show_working(Builder& b, std::size_t part_at, ControlId id, const Rect& ring) {
    Item& part = b.list.items[part_at];
    part.control = id;
    part.state.hovered = part.state.hovered || b.model->interaction.hovered == id;
    part.state.pressed = b.model->interaction.pressed == id;
    part.state.focused = focus_on(*b.model, id);
    if (part.state.focused && ring.width > 0 && ring.height > 0)
        add_focus_ring(b.list, ring, true);
}

/// Adds a component in a fixed state: what add adds, its controls taken off
/// so that it takes no input, its first part given the state's flags, then
/// the focus ring round ring when it is focused, and the locked fade over it
/// when it is disabled and has no disabled look of its own.
///
/// @param[in,out] list the display list
/// @param rect the sample
/// @param ring the rectangle the focus ring goes round; empty for none
/// @param shown the state
/// @param fades a disabled sample is faded as a locked row is
/// @param add adds the component's parts
template <class Add>
void add_fixed(
    DisplayList& list, const Rect& rect, const Rect& ring, Sample shown, bool fades, Add&& add
) {
    const std::size_t first_part = list.items.size();
    const std::size_t first_control = list.controls.size();
    add();
    list.controls.erase(
        list.controls.begin() + static_cast<std::ptrdiff_t>(first_control), list.controls.end()
    );
    if (first_part < list.items.size()) {
        Item& part = list.items[first_part];
        part.control = no_control;
        part.state.hovered = part.state.hovered || looks_hovered(shown);
        part.state.pressed = shown == Sample::pressed;
        part.state.focused = shown == Sample::focused;
        part.state.disabled = part.state.disabled || shown == Sample::disabled;
        part.state.locked = part.state.locked || shown == Sample::locked;
        part.state.selected = part.state.selected || shown == Sample::selected;
    }
    if (shown == Sample::focused && ring.width > 0 && ring.height > 0)
        add_focus_ring(list, ring, true);
    if (shown == Sample::disabled && fades)
        add_part(list, Role::locked_fade, rect);
}

/// Lays samples of one size in lines across a width, each over its caption
/// in the small font, and returns the height they take.
///
/// @param[in,out] list the display list
/// @param at the first sample's top left, in points
/// @param width the width they lay across
/// @param size each sample's size
/// @param captions each sample's caption, in order
/// @param add adds the sample at a rectangle, given its place in captions
/// @return the height, in points
template <class Add>
int32_t add_cells(
    DisplayList& list,
    Point at,
    int32_t width,
    Point size,
    std::span<const std::string_view> captions,
    Add&& add
) {
    if (captions.empty())
        return 0;
    const int32_t cell = std::max(size.x, kLeastCell);
    const int32_t across = std::max(int32_t{1}, (width + kCellGap) / (cell + kCellGap));
    const int32_t line_height = size.y + kCaptionGap + kMetrics.small_line;
    for (std::size_t place = 0; place < captions.size(); ++place) {
        const auto index = static_cast<int32_t>(place);
        const Rect sample{
            at.x + (index % across) * (cell + kCellGap),
            at.y + (index / across) * (line_height + kLineGap),
            size.x,
            size.y,
        };
        add(sample, place);
        add_words(
            list,
            {sample.x, sample.y + size.y + kCaptionGap, std::min(cell, width), kMetrics.small_line},
            captions[place],
            FontRole::small,
            Align::left,
            colour::hint
        );
    }
    const auto count = static_cast<int32_t>(captions.size());
    const int32_t lines = (count + across - 1) / across;
    return lines * line_height + (lines - 1) * kLineGap;
}

/// Lays samples in fixed states, as add_cells does, each captioned with its state.
///
/// @param[in,out] list the display list
/// @param at the first sample's top left, in points
/// @param width the width they lay across
/// @param size each sample's size
/// @param states the states, in order
/// @param add adds the sample at a rectangle in a state
/// @return the height, in points
template <class Add>
int32_t add_states(
    DisplayList& list,
    Point at,
    int32_t width,
    Point size,
    std::span<const Sample> states,
    Add&& add
) {
    std::vector<std::string_view> captions;
    captions.reserve(states.size());
    for (const Sample shown : states)
        captions.push_back(caption_of(shown));
    return add_cells(list, at, width, size, captions, [&](const Rect& sample, std::size_t place) {
        add(sample, states[place]);
    });
}

/// Returns a button's look.
///
/// @param caption its words
/// @param style its look
/// @param hovered the pointer is over it
/// @param held a press is held on it with the pointer over it
/// @param enabled it takes a press
/// @return the look
ButtonLook button_look(
    std::string_view caption, ButtonStyle style, bool hovered, bool held, bool enabled = true
) {
    return {std::string(caption), style, hovered, held, enabled};
}

/// Returns the strings of a list of words.
///
/// @param words the words
/// @return the strings
template <std::size_t N>
std::vector<std::string> strings_of(const std::array<std::string_view, N>& words) {
    return {words.begin(), words.end()};
}

/// Returns a list row's look of a sample package.
///
/// @param package the package
/// @param full the full form: the byline, the line, the version and the size
/// @return the look
ListRowLook row_look(const Package& package, bool full) {
    ListRowLook look;
    look.badge_text = std::string(package.letters);
    look.badge_colour = colour::list_selected;
    look.title = std::string(package.name);
    look.aside.emplace_back(package.version);
    if (full) {
        look.by = std::string(kByline);
        look.lines.emplace_back(package.line);
        look.aside.emplace_back(package.size);
        look.chip = ChipLook{std::string(package.chip), package.state};
    }
    return look;
}

/// Returns a card's look of a sample package.
///
/// @param package the package
/// @return the look
CardLook card_look(const Package& package) {
    CardLook look;
    look.placeholder = "Preview";
    look.title = std::string(package.name);
    look.subtitle = std::string(package.version);
    look.chips.push_back({std::string(package.chip), package.state});
    return look;
}

/// Returns a strip of the sample levels.
///
/// @return the look, its middle level chosen
LevelsLook levels_look() {
    LevelsLook look;
    look.captions = strings_of(kLevelCaptions);
    look.level_width = kLevelWidth;
    look.chosen = 1;
    look.offered = kLevelCount;
    return look;
}

/// Returns the sample tabs.
///
/// @return the look, the first tab open
TabsLook tabs_look() {
    TabsLook look;
    look.captions = strings_of(kTabCaptions);
    look.counts.assign(kTabCounts.begin(), kTabCounts.end());
    return look;
}

/// Returns the sample languages' menu.
///
/// @param chosen the chosen language
/// @param marked the marked language
/// @param focus_shown the keyboard focus outlines the marked item
/// @param aside the last language shows an aside, as one to fetch does
/// @return the look
ChoiceMenuLook languages_menu(int32_t chosen, int32_t marked, bool focus_shown, bool aside) {
    ChoiceMenuLook look;
    look.shown = strings_of(gallery_languages);
    for (std::size_t place = 0; place < gallery_languages.size(); ++place) {
        ChoiceMenuRow menu_row;
        menu_row.id = std::string(kLanguageWords[place]);
        if (aside && place + 1 == gallery_languages.size())
            menu_row.aside = "4.0 MB";
        look.items.push_back(std::move(menu_row));
    }
    look.first = 0;
    look.total = static_cast<int32_t>(gallery_languages.size());
    look.chosen = chosen;
    look.marked = marked;
    look.focus_shown = focus_shown;
    return look;
}

/// Returns where a drop-down's menu of the sample languages lies: under its field.
///
/// @param field the field
/// @return the menu
Rect menu_under(const Rect& field) noexcept {
    return choice_menu(field, gallery_languages.size(), std::numeric_limits<int32_t>::max());
}

// ---------------------------------------------------------------------------
// The sections. Each adds its samples from a top left corner across a width
// and returns their height.

/// Window chrome: an OA window, its face, header, footer band and edge, over
/// the screen under it darkened as a modal screen's backdrop darkens it.
int32_t window_chrome(Builder& b, Point at, int32_t width, std::string_view /*word*/) {
    const Rect area{at.x, at.y, width, kWindowHeight + 2 * kWindowMargin};
    add_part(b.list, Role::fill, area).colour = colour::control_border;
    Item& veil = add_part(b.list, Role::blend, area);
    veil.colour = colour::backdrop;
    veil.opacity = menu_backdrop_opacity;
    const int32_t window_width = std::max(std::min(width - 2 * kWindowMargin, kWindowSample), 1);
    GalleryPanel panel;
    panel.origin = {at.x + kWindowMargin, at.y + kWindowMargin};
    panel.place = {panel.origin.x, panel.origin.y, window_width, kWindowHeight};
    const Rect whole{0, 0, window_width, kWindowHeight};
    add_part(panel.list, Role::fill, whole).colour = colour::panel;
    HeaderLook header;
    header.width = window_width;
    header.title = std::string(kTitle);
    header.tracking = kMetrics.heading_tracking;
    header.title_width = tracked_width(*b.fonts, FontRole::regular, kTitle, header.tracking);
    header.second = "Settings";
    header.second_width =
        tracked_width(*b.fonts, FontRole::regular, header.second, header.tracking);
    header.version = std::string(gallery_version);
    header.version_width = text_width(*b.fonts, FontRole::small, gallery_version);
    Item& header_part = add_part(
        panel.list, Role::header, {0, 0, window_width, kMetrics.edge + kMetrics.header_height + 1}
    );
    header_part.text = header.title;
    header_part.look = std::move(header);
    const int32_t rule_row = kWindowHeight - kMetrics.edge - kMetrics.footer_height - 1;
    add_part(panel.list, Role::footer_band, {0, rule_row, window_width, kMetrics.footer_height + 1})
        .look = FooterBandLook{window_width, rule_row};
    add_part(panel.list, Role::bevel, whole).colour = colour::edge_light;
    b.panels.push_back(std::move(panel));
    return area.height;
}

/// Tabs: three tabs with a count on the last, working, then in each state.
int32_t tabs(Builder& b, Point at, int32_t width, std::string_view word) {
    const GalleryModel& model = *b.model;
    TabsLook look = tabs_look();
    look.selected = std::min(model.tab, kTabCount - 1);
    std::vector<ControlId> ids;
    std::vector<std::string> names;
    for (std::size_t place = 0; place < kTabCount; ++place) {
        const ControlId id = kFirstTab + static_cast<ControlId>(place);
        ids.push_back(id);
        names.push_back(named(word, place + 1));
        if (pointer_on(model, id))
            look.hovered = place;
    }
    const Rect strip{at.x, at.y, width, kMetrics.tab_height};
    const std::vector<Rect> places = tab_rects(*b.fonts, strip, look);
    const std::size_t part_at = b.list.items.size();
    add_tabs(b.list, *b.fonts, strip, look, ids, names);
    // The strip draws the open tab's control; the focus ring goes round the
    // focused tab.
    show_working(b, part_at, ids[look.selected], {});
    for (std::size_t place = 0; place < kTabCount && place < places.size(); ++place)
        if (focus_on(model, ids[place]))
            add_focus_ring(b.list, places[place], true);
    b.order.insert(b.order.end(), ids.begin(), ids.end());

    const TabsLook sample = tabs_look();
    const std::vector<Rect> sample_tabs =
        tab_rects(*b.fonts, {0, 0, width, kMetrics.tab_height}, sample);
    const int32_t sample_width =
        sample_tabs.empty() ? width : sample_tabs.back().x + sample_tabs.back().width;
    const int32_t top = at.y + kMetrics.tab_height + kLineGap;
    return top - at.y +
           add_states(
               b.list,
               {at.x, top},
               width,
               {sample_width, kMetrics.tab_height},
               kFiveStates,
               [&](const Rect& rect, Sample shown) {
                   TabsLook shown_tabs = sample;
                   if (looks_hovered(shown))
                       shown_tabs.hovered = 1;
                   const std::vector<Rect> shown_places = tab_rects(*b.fonts, rect, shown_tabs);
                   const Rect ring = shown_places.empty() ? rect : shown_places[0];
                   add_fixed(b.list, rect, ring, shown, true, [&] {
                       add_tabs(b.list, *b.fonts, rect, shown_tabs, {}, {});
                   });
               }
           );
}

/// Buttons: a working button and link, the six looks, then a button and a
/// link in each state, at Kit 2's Compact control height.
int32_t buttons_section(Builder& b, Point at, int32_t width, std::string_view word) {
    const GalleryModel& model = *b.model;
    int32_t top = at.y;
    {
        const Rect button{at.x, top, kButtonWidth, kMetrics.control_height};
        const bool over = model.interaction.hovered == kButton;
        const std::size_t part_at = b.list.items.size();
        add_button(
            b.list,
            button,
            button_look(
                "Press", ButtonStyle::accent, over, over && model.interaction.pressed == kButton
            ),
            kButton,
            named(word, 1)
        );
        show_working(b, part_at, kButton, button);
        b.order.push_back(kButton);
        const Rect count{button.x + button.width + kCellGap, top, 96, kMetrics.control_height};
        add_words(
            b.list,
            count,
            "Pressed " + std::to_string(model.presses) + (model.presses == 1 ? " time" : " times"),
            FontRole::small,
            Align::left,
            colour::hint
        );
        LinkLook link;
        link.caption = "Homepage";
        link.hovered = pointer_on(model, kLink);
        const Rect link_rect{
            count.x + count.width + kCellGap,
            top + (kMetrics.control_height - kMetrics.small_line) / 2,
            link_width(*b.fonts, link),
            kMetrics.small_line
        };
        const std::size_t link_at = b.list.items.size();
        add_link(b.list, link_rect, link, kLink, named(word, 2));
        show_working(b, link_at, kLink, link_rect);
        b.order.push_back(kLink);
        top += kMetrics.control_height + kLineGap;
    }
    // The six looks, at rest.
    constexpr std::array<std::string_view, 6> kLooks{
        "Accent", "Plain", "Quiet", "Inset", "Filled", "Danger"
    };
    constexpr std::array<std::string_view, 6> kLookCaptions{
        "OK", "Cancel", "Open folder", "Roll back", "Mutators", "Uninstall"
    };
    constexpr std::array<ButtonStyle, 6> kStyles{
        ButtonStyle::accent,
        ButtonStyle::plain,
        ButtonStyle::quiet,
        ButtonStyle::inset,
        ButtonStyle::filled,
        ButtonStyle::danger
    };
    top += add_cells(
               b.list,
               {at.x, top},
               width,
               {kButtonWidth, kMetrics.control_height},
               kLooks,
               [&](const Rect& rect, std::size_t place) {
                   add_fixed(b.list, rect, {}, Sample::idle, false, [&] {
                       ButtonLook look =
                           button_look(kLookCaptions[place], kStyles[place], false, false);
                       // The filled look wears the OA badge, as Mutators does.
                       look.oa_mark = kStyles[place] == ButtonStyle::filled;
                       add_button(b.list, rect, look, no_control, {});
                   });
               }
           ) +
           kLineGap;
    // A plain button in each state.
    top += add_states(
               b.list,
               {at.x, top},
               width,
               {kButtonWidth, kMetrics.control_height},
               kFiveStates,
               [&](const Rect& rect, Sample shown) {
                   add_fixed(b.list, rect, rect, shown, false, [&] {
                       add_button(
                           b.list,
                           rect,
                           button_look(
                               "Cancel",
                               ButtonStyle::plain,
                               looks_hovered(shown),
                               shown == Sample::pressed,
                               shown != Sample::disabled
                           ),
                           no_control,
                           {}
                       );
                   });
               }
           ) +
           kLineGap;
    // A close mark in each state: the same square face as a refresh mark,
    // with an × in the text colour.
    top += add_states(
               b.list,
               {at.x, top},
               width,
               {kMetrics.control_height, kMetrics.control_height},
               kFiveStates,
               [&](const Rect& rect, Sample shown) {
                   add_fixed(b.list, rect, rect, shown, false, [&] {
                       ButtonLook look = button_look(
                           "Close",
                           ButtonStyle::quiet,
                           looks_hovered(shown),
                           shown == Sample::pressed,
                           shown != Sample::disabled
                       );
                       look.mark = ButtonMark::close;
                       add_button(b.list, rect, look, no_control, {});
                   });
               }
           ) +
           kLineGap;
    // A link in each state.
    LinkLook sample_link;
    sample_link.caption = "Homepage";
    const int32_t link_columns = link_width(*b.fonts, sample_link);
    top += add_states(
        b.list,
        {at.x, top},
        width,
        {link_columns, kMetrics.small_line},
        kFiveStates,
        [&](const Rect& rect, Sample shown) {
            LinkLook link = sample_link;
            link.hovered = looks_hovered(shown);
            link.enabled = shown != Sample::disabled;
            add_fixed(b.list, rect, rect, shown, false, [&] {
                add_link(b.list, rect, link, no_control, {});
            });
        }
    );
    return top - at.y;
}

/// Toggle: a working switch beside its label, then a switch in each state
/// and locked.
int32_t toggle_section(Builder& b, Point at, int32_t width, std::string_view word) {
    const GalleryModel& model = *b.model;
    const Rect field{at.x, at.y, kMetrics.switch_width, kMetrics.switch_height};
    SwitchLook look;
    look.on = model.toggle_on;
    look.hovered = pointer_on(model, kToggle);
    look.off_caption = "Off";
    look.on_caption = "On";
    const std::size_t part_at = b.list.items.size();
    add_switch(b.list, field, look, kToggle, named(word, 1));
    show_working(b, part_at, kToggle, field);
    b.order.push_back(kToggle);
    add_words(
        b.list,
        {field.x + field.width + kCellGap,
         at.y,
         std::max(width - field.width - kCellGap, 0),
         kMetrics.switch_height},
        "Unicode chat",
        FontRole::regular,
        Align::left,
        colour::text
    );
    const int32_t top = at.y + kMetrics.switch_height + kLineGap;
    return top - at.y +
           add_states(
               b.list,
               {at.x, top},
               width,
               {kMetrics.switch_width, kMetrics.switch_height},
               kLockedStates,
               [&](const Rect& rect, Sample shown) {
                   SwitchLook sample = look;
                   sample.on = true;
                   sample.hovered = looks_hovered(shown);
                   sample.locked = shown == Sample::locked;
                   add_fixed(b.list, rect, rect, shown, true, [&] {
                       add_switch(b.list, rect, sample, no_control, {});
                   });
               }
           );
}

/// Level strip: a working strip of three levels, then one in each state and
/// locked.
int32_t level_strip(Builder& b, Point at, int32_t width, std::string_view word) {
    const GalleryModel& model = *b.model;
    const int32_t strip_width = static_cast<int32_t>(kLevelCount) * kLevelWidth + 2;
    const Rect strip{at.x, at.y, strip_width, kMetrics.regular_line};
    LevelsLook look = levels_look();
    look.chosen = std::min(model.level, kLevelCount - 1);
    look.hovered = pointer_on(model, kLevels);
    const std::size_t part_at = b.list.items.size();
    add_levels(b.list, strip, look, kLevels, named(word, 1));
    // Automation names the levels by their words.
    b.list.controls.back().parts = strings_of(kLevelWords);
    show_working(b, part_at, kLevels, strip);
    b.order.push_back(kLevels);
    const int32_t top = at.y + kMetrics.regular_line + kLineGap;
    return top - at.y +
           add_states(
               b.list,
               {at.x, top},
               width,
               {strip_width, kMetrics.regular_line},
               kLockedStates,
               [&](const Rect& rect, Sample shown) {
                   LevelsLook sample = levels_look();
                   sample.hovered = looks_hovered(shown);
                   sample.locked = shown == Sample::locked;
                   add_fixed(b.list, rect, rect, shown, true, [&] {
                       add_levels(b.list, rect, sample, no_control, {});
                   });
               }
           );
}

/// Drop-down: a working drop-down of four languages with room for its menu
/// under it, one shown open with an aside on its last item, then one in
/// each state.
int32_t drop_down(Builder& b, Point at, int32_t width, std::string_view word) {
    const GalleryModel& model = *b.model;
    const int32_t chosen =
        static_cast<int32_t>(std::min(model.language, gallery_languages.size() - 1));
    const Rect field{at.x, at.y, kFieldWidth, kMetrics.regular_line};
    ChoiceLook look;
    look.text = std::string(gallery_languages[static_cast<std::size_t>(chosen)]);
    look.hovered = pointer_on(model, kChoice);
    look.open = model.menu_open;
    const std::size_t part_at = b.list.items.size();
    add_choice(b.list, field, look, kChoice, named(word, 1));
    b.list.controls.back().parts = strings_of(kLanguageWords);
    show_working(b, part_at, kChoice, field);
    b.order.push_back(kChoice);
    const Rect menu = menu_under(field);
    if (model.menu_open) {
        b.menu = menu;
        b.menu_look = languages_menu(
            chosen,
            std::clamp(
                model.menu_marked, int32_t{0}, static_cast<int32_t>(gallery_languages.size()) - 1
            ),
            model.interaction.focus_shown,
            false
        );
        Item part;
        part.role = Role::choice_menu;
        part.rect = menu;
        part.look = b.menu_look;
        b.menu_part = std::move(part);
    }
    // The open sample beside the working one where the width holds both,
    // else under its menu's room.
    const int32_t room = menu.y + menu.height - at.y;
    Point open_at{field.x + field.width + 3 * kCellGap, at.y};
    int32_t top = at.y + room + kCaptionGap;
    if (open_at.x + kFieldWidth > at.x + width) {
        open_at = {at.x, top};
        top = open_at.y + room + kCaptionGap;
    }
    const Rect open_field{open_at.x, open_at.y, kFieldWidth, kMetrics.regular_line};
    add_fixed(b.list, open_field, {}, Sample::idle, false, [&] {
        ChoiceLook open_look;
        open_look.text = "Français";
        open_look.open = true;
        add_choice(b.list, open_field, open_look, no_control, {});
        Item& open_menu = add_part(b.list, Role::choice_menu, menu_under(open_field));
        open_menu.look = languages_menu(3, 1, false, true);
    });
    add_words(
        b.list,
        {open_field.x, open_field.y + room + kCaptionGap, kFieldWidth, kMetrics.small_line},
        "Open",
        FontRole::small,
        Align::left,
        colour::hint
    );
    top += kMetrics.small_line + kLineGap;
    return top - at.y +
           add_states(
               b.list,
               {at.x, top},
               width,
               {kFieldWidth, kMetrics.regular_line},
               kFiveStates,
               [&](const Rect& rect, Sample shown) {
                   ChoiceLook sample;
                   sample.text = std::string(gallery_languages[0]);
                   sample.hovered = looks_hovered(shown);
                   add_fixed(b.list, rect, rect, shown, true, [&] {
                       add_choice(b.list, rect, sample, no_control, {});
                   });
               }
           );
}

/// Slider: a working slider of eleven stops with its value, then one in
/// each state and locked.
int32_t slider_section(Builder& b, Point at, int32_t width, std::string_view word) {
    const GalleryModel& model = *b.model;
    constexpr int32_t kValueWidth = 40;
    const Rect track{
        at.x,
        at.y,
        std::max(std::min(width - kValueWidth - kCellGap, 2 * kSliderWidth), kSliderWidth),
        kMetrics.slider_line_height
    };
    SliderLook look;
    look.stops = gallery_slider_stops;
    look.stop = std::clamp(model.slider_stop, int32_t{0}, gallery_slider_stops - 1);
    look.hovered = pointer_on(model, kSlider);
    const std::size_t part_at = b.list.items.size();
    add_slider(b.list, track, look, kSlider, named(word, 1));
    show_working(b, part_at, kSlider, track);
    b.order.push_back(kSlider);
    add_words(
        b.list,
        {track.x + track.width + kCellGap, at.y, kValueWidth, kMetrics.slider_line_height},
        std::to_string(look.stop * 10) + "%",
        FontRole::regular,
        Align::left,
        colour::text
    );
    const int32_t top = at.y + kMetrics.slider_line_height + kLineGap;
    return top - at.y +
           add_states(
               b.list,
               {at.x, top},
               width,
               {kSliderWidth, kMetrics.slider_line_height},
               kLockedStates,
               [&](const Rect& rect, Sample shown) {
                   SliderLook sample;
                   sample.stops = gallery_slider_stops;
                   sample.stop = 6;
                   sample.hovered = looks_hovered(shown);
                   sample.locked = shown == Sample::locked;
                   add_fixed(b.list, rect, rect, shown, true, [&] {
                       add_slider(b.list, rect, sample, no_control, {});
                   });
               }
           );
}

/// Search field: a working search field that takes typing, then one in
/// each state and a plain text field.
int32_t search_field(Builder& b, Point at, int32_t width, std::string_view word) {
    const GalleryModel& model = *b.model;
    const Rect field{at.x, at.y, std::min(width, 2 * kSearchWidth), kMetrics.field_height};
    SearchLook look;
    look.placeholder = "Search mods";
    look.field = model.search;
    look.focused = focus_on(model, kSearch);
    look.hovered = pointer_on(model, kSearch);
    const std::size_t part_at = b.list.items.size();
    add_search(b.list, field, look, kSearch, named(word, 1));
    show_working(b, part_at, kSearch, field);
    b.order.push_back(kSearch);
    int32_t top = at.y + kMetrics.field_height + kLineGap;
    top += add_states(
               b.list,
               {at.x, top},
               width,
               {kSearchWidth, kMetrics.field_height},
               kFiveStates,
               [&](const Rect& rect, Sample shown) {
                   SearchLook sample;
                   sample.placeholder = "Search mods";
                   sample.hovered = looks_hovered(shown);
                   if (shown == Sample::focused) {
                       sample.field.text = "Sample";
                       sample.field.caret = sample.field.text.size();
                       sample.focused = true;
                   }
                   add_fixed(b.list, rect, rect, shown, true, [&] {
                       add_search(b.list, rect, sample, no_control, {});
                   });
               }
           ) +
           kLineGap;
    constexpr std::array<std::string_view, 1> kPlain{"Text field"};
    top += add_cells(
        b.list,
        {at.x, top},
        width,
        {kSearchWidth, kMetrics.field_height},
        kPlain,
        [&](const Rect& rect, std::size_t) {
            SearchLook plain;
            plain.placeholder = "Your name";
            plain.magnifier = false;
            add_fixed(b.list, rect, {}, Sample::idle, false, [&] {
                add_search(b.list, rect, plain, no_control, {});
            });
        }
    );
    return top - at.y;
}

/// Lays chips from left to right across a width, a new line where one
/// would pass its right, and returns the height they take.
///
/// @param[in,out] b what is built
/// @param at the first chip's top left
/// @param width the width
/// @param looks the chips
/// @param add adds a chip at a rectangle, given its place
/// @return the height, in points
template <class Add>
int32_t
chip_lines(Builder& b, Point at, int32_t width, std::span<const ChipLook> looks, Add&& add) {
    int32_t x = at.x;
    int32_t y = at.y;
    for (std::size_t place = 0; place < looks.size(); ++place) {
        const int32_t chip_columns = chip_width(*b.fonts, looks[place]);
        if (x > at.x && x + chip_columns > at.x + width) {
            x = at.x;
            y += kMetrics.chip_height + kMetrics.chip_gap;
        }
        add(Rect{x, y, chip_columns, kMetrics.chip_height}, place);
        x += chip_columns + kMetrics.chip_gap;
    }
    return looks.empty() ? 0 : y + kMetrics.chip_height - at.y;
}

/// Chips: one of each state, three working filters, then a filter in each
/// state.
int32_t chips_section(Builder& b, Point at, int32_t width, std::string_view word) {
    const GalleryModel& model = *b.model;
    const std::array<ChipLook, 7> kinds{{
        {"Get", ChipState::get},
        {"Installed", ChipState::installed},
        {"Update", ChipState::update},
        {"Playing", ChipState::playing},
        {"Online", ChipState::online},
        {"Problem", ChipState::problem},
        {"Filter", ChipState::filter, true},
    }};
    int32_t top = at.y;
    top += chip_lines(
               b,
               {at.x, top},
               width,
               kinds,
               [&](const Rect& rect, std::size_t place) {
                   add_chip(b.list, rect, kinds[place], no_control, {});
               }
           ) +
           kLineGap;
    constexpr std::array<std::string_view, kChipCount> kFilterChips{"Balance", "Units", "Maps"};
    std::array<ChipLook, kChipCount> filters{};
    for (std::size_t place = 0; place < kChipCount; ++place) {
        const ControlId id = kFirstChip + static_cast<ControlId>(place);
        filters[place].text = std::string(kFilterChips[place]);
        filters[place].state = ChipState::filter;
        filters[place].selected = model.chips[place];
        filters[place].hovered = pointer_on(model, id);
    }
    top += chip_lines(
               b,
               {at.x, top},
               width,
               filters,
               [&](const Rect& rect, std::size_t place) {
                   const ControlId id = kFirstChip + static_cast<ControlId>(place);
                   const std::size_t part_at = b.list.items.size();
                   add_chip(b.list, rect, filters[place], id, named(word, place + 1));
                   show_working(b, part_at, id, rect);
                   b.order.push_back(id);
               }
           ) +
           kLineGap;
    ChipLook filter{"Filter", ChipState::filter};
    top += add_states(
        b.list,
        {at.x, top},
        width,
        {chip_width(*b.fonts, filter), kMetrics.chip_height},
        kFiveStates,
        [&](const Rect& rect, Sample shown) {
            ChipLook sample = filter;
            sample.hovered = looks_hovered(shown);
            sample.disabled = shown == Sample::disabled;
            add_fixed(b.list, rect, rect, shown, false, [&] {
                add_chip(b.list, rect, sample, no_control, {});
            });
        }
    );
    // The two maker's marks, at the state icon's size, with a caption each.
    // They are not controls: a press does nothing and they add no name.
    top += kLineGap;
    const int32_t side = kMetrics.state_icon_side;
    constexpr int32_t kCaption = 48;
    ChipLook oa_mark;
    oa_mark.icon = StateIcon::oa;
    oa_mark.text = "OA";
    add_chip(b.list, {at.x, top, side, side}, oa_mark, no_control, {});
    add_words(
        b.list,
        {at.x + side + kMetrics.chip_gap, top, kCaption, side},
        "OA",
        FontRole::small,
        Align::left,
        colour::text
    );
    const int32_t bone_x = at.x + side + kMetrics.chip_gap + kCaption + kMetrics.chip_gap;
    ChipLook bone_mark;
    bone_mark.icon = StateIcon::bone;
    bone_mark.text = "Bone";
    add_chip(b.list, {bone_x, top, side, side}, bone_mark, no_control, {});
    add_words(
        b.list,
        {bone_x + side + kMetrics.chip_gap, top, kCaption, side},
        "Bone",
        FontRole::small,
        Align::left,
        colour::text
    );
    top += side;
    return top - at.y;
}

/// Pills: one of each state, display only, then a selectable pill in each
/// state. Playing and active are filled; neutral and inactive are the
/// selected surface; installed, update, problem, information and brand are
/// their colour's tint. Hover and disabled are drawn on the pill, and focus
/// is the pill's own ring, so a sample has no separate ring.
int32_t pills_section(Builder& b, Point at, int32_t width, std::string_view /*word*/) {
    const std::array<PillLook, 9> kinds{{
        {"Playing", PillState::playing},
        {"Built in", PillState::neutral},
        {"Installed", PillState::installed},
        {"Update", PillState::update},
        {"Problem", PillState::problem},
        {"Active", PillState::active},
        {"Inactive", PillState::inactive},
        {"Update 0.8.8", PillState::information},
        {"Core Prime", PillState::brand},
    }};
    int32_t x = at.x;
    int32_t y = at.y;
    for (const PillLook& look : kinds) {
        const int32_t across = pill_width(*b.fonts, look);
        if (x > at.x && x + across > at.x + width) {
            x = at.x;
            y += pill_height() + kMetrics.chip_gap;
        }
        add_pill(b.list, {x, y, across, pill_height()}, look, no_control, {});
        x += across + kMetrics.chip_gap;
    }
    PillLook sample{"Filter", PillState::inactive};
    const int32_t states = add_states(
        b.list,
        {at.x, y + pill_height() + kLineGap},
        width,
        {pill_width(*b.fonts, sample), pill_height()},
        kSelectableStates,
        [&](const Rect& rect, Sample shown) {
            PillLook drawn = sample;
            drawn.hovered = looks_hovered(shown);
            drawn.focused = shown == Sample::focused;
            drawn.disabled = shown == Sample::disabled;
            if (shown == Sample::selected)
                drawn.state = PillState::active;
            add_fixed(b.list, rect, {}, shown, false, [&] {
                add_pill(b.list, rect, drawn, no_control, {});
            });
        }
    );
    return y + pill_height() + kLineGap + states - at.y;
}

/// Progress: a bar waiting, part done, paused part done and done, each
/// with its share.
int32_t progress_section(Builder& b, Point at, int32_t width, std::string_view /*word*/) {
    constexpr int32_t kShareWidth = 40;
    constexpr std::array<int32_t, 4> kDone{0, 400, 400, 1000};
    const int32_t bar_width = std::max(width - kShareWidth - kCellGap, 1);
    int32_t top = at.y;
    for (std::size_t place = 0; place < kDone.size(); ++place) {
        const int32_t done = kDone[place];
        add_progress(
            b.list,
            {at.x,
             top + (kMetrics.small_line - kMetrics.progress_bar_height) / 2,
             bar_width,
             kMetrics.progress_bar_height},
            done,
            question_progress_whole,
            place == 2
        );
        add_words(
            b.list,
            {at.x + bar_width + kCellGap, top, kShareWidth, kMetrics.small_line},
            std::to_string(done / 10) + "%",
            FontRole::small,
            Align::right,
            colour::hint
        );
        top += kMetrics.small_line + kMetrics.chip_gap;
    }
    return top - kMetrics.chip_gap - at.y;
}

/// Returns a nav entry's rectangle in the working nav list's face.
///
/// @param face the face
/// @param place the entry's place, from 0
/// @return the entry
Rect nav_entry(const Rect& face, std::size_t place) noexcept {
    const auto index = static_cast<int32_t>(place);
    int32_t top = face.y + kNavTop + index * (kNavEntryHeight + kNavGap);
    // The last entry stands under the divider.
    if (place + 1 == kNavCount)
        top += 2 * kNavDividerMargin + 1 - kNavGap;
    return {face.x + kNavInset, top, face.width - 2 * kNavInset, kNavEntryHeight};
}

/// Nav list: four working entries with a divider before the last, then an
/// entry in each state and chosen.
int32_t nav_list(Builder& b, Point at, int32_t width, std::string_view word) {
    const GalleryModel& model = *b.model;
    const Rect last_entry = nav_entry({0, 0, kNavWidth, 0}, kNavCount - 1);
    const Rect face{
        at.x, at.y, std::min(kNavWidth, width), last_entry.y + kNavEntryHeight + kNavTop
    };
    NavLook look;
    look.area = face;
    look.rule_column = face.x + face.width - 1;
    const Rect before_last = nav_entry(face, kNavCount - 2);
    look.divider = Rect{
        face.x + 2 * kNavInset,
        before_last.y + kNavEntryHeight + kNavDividerMargin,
        face.width - 4 * kNavInset,
        1
    };
    std::vector<ControlId> ids;
    std::vector<std::string> names;
    for (std::size_t place = 0; place < kNavCount; ++place) {
        const ControlId id = kFirstNav + static_cast<ControlId>(place);
        NavEntry entry;
        entry.rect = nav_entry(face, place);
        entry.caption = std::string(kNavCaptions[place]);
        entry.selected = model.nav == place;
        entry.hovered = pointer_on(model, id);
        entry.focused = focus_on(model, id);
        look.entries.push_back(std::move(entry));
        ids.push_back(id);
        names.push_back(named(word, place + 1));
    }
    const std::size_t first_part = b.list.items.size();
    add_nav(b.list, look, ids, names);
    // The list draws each entry's own focus outline.
    for (std::size_t place = 0; place < kNavCount; ++place)
        show_working(b, first_part + place, ids[place], {});
    b.order.insert(b.order.end(), ids.begin(), ids.end());
    const int32_t top = face.y + face.height + kLineGap;
    return top - at.y +
           add_states(
               b.list,
               {at.x, top},
               width,
               {kNavSampleWidth, kNavEntryHeight},
               kSelectableStates,
               [&](const Rect& rect, Sample shown) {
                   NavLook sample;
                   sample.area = rect;
                   sample.rule_column = rect.x + rect.width - 1;
                   NavEntry entry;
                   entry.rect = {rect.x, rect.y, rect.width - 1, rect.height};
                   entry.caption = "Graphics";
                   entry.selected = shown == Sample::selected;
                   entry.hovered = looks_hovered(shown);
                   entry.focused = shown == Sample::focused;
                   sample.entries.push_back(std::move(entry));
                   add_fixed(b.list, rect, {}, shown, true, [&] {
                       add_nav(b.list, sample, {}, {});
                   });
               }
           );
}

/// List rows: three working rows in the full form, then a short row in each
/// state and chosen.
int32_t list_rows(Builder& b, Point at, int32_t width, std::string_view word) {
    const GalleryModel& model = *b.model;
    int32_t top = at.y;
    for (std::size_t place = 0; place < kRowCount; ++place) {
        const ControlId id = kFirstRow + static_cast<ControlId>(place);
        const Rect rect{at.x, top, width, kMetrics.list_row_height};
        ListRowLook look = row_look(kPackages[place], true);
        look.selected = model.row == place;
        look.hovered = pointer_on(model, id);
        const std::size_t part_at = b.list.items.size();
        add_list_row(b.list, rect, look, id, named(word, place + 1));
        show_working(b, part_at, id, rect);
        b.order.push_back(id);
        top += kMetrics.list_row_height + kRowGap;
    }
    top += kLineGap - kRowGap;
    const int32_t sample_width =
        width >= 2 * kShortRowWidth + kCellGap ? (width - kCellGap) / 2 : width;
    top += add_states(
        b.list,
        {at.x, top},
        width,
        {sample_width, kMetrics.short_list_row_height},
        kSelectableStates,
        [&](const Rect& rect, Sample shown) {
            ListRowLook sample = row_look(kPackages[3], false);
            sample.hovered = looks_hovered(shown);
            sample.selected = shown == Sample::selected;
            add_fixed(b.list, rect, rect, shown, true, [&] {
                add_list_row(b.list, rect, sample, no_control, {});
            });
        }
    );
    return top - at.y;
}

/// Scroll list: four short rows in a view that shows two and a half, with
/// its scroll bar; the rows scroll under the wheel and follow the focus.
int32_t scroll_list(Builder& b, Point at, int32_t width, std::string_view word) {
    const GalleryModel& model = *b.model;
    const int32_t row_width = std::max(width - kScrollRoom, 1);
    const int32_t row_step = kMetrics.short_list_row_height + kListRowGap;
    ScrollArea area;
    area.view = {at.x, at.y, row_width, kListView};
    area.well = {
        at.x + width - (kMetrics.scroll_thumb_width + 2),
        at.y,
        kMetrics.scroll_thumb_width + 2,
        kListView
    };
    area.hit = area.well;
    area.page_step = std::max(kListView - row_step, row_step);
    area.content_height = static_cast<int32_t>(kListRowCount) * row_step - kListRowGap;
    area.limit = ScrollArea::limit_for(area.content_height, kListView);
    area.offset = std::clamp(model.list_scroll, int32_t{0}, area.limit);
    for (std::size_t place = 0; place < kListRowCount; ++place) {
        const ControlId id = kFirstListRow + static_cast<ControlId>(place);
        const Rect rect = area.placed(
            {at.x,
             at.y + static_cast<int32_t>(place) * row_step,
             row_width,
             kMetrics.short_list_row_height}
        );
        ListRowLook look = row_look(kPackages[place], false);
        look.selected = model.list_row == place;
        look.hovered = pointer_on(model, id);
        const std::size_t part_at = b.list.items.size();
        add_list_row(b.list, rect, look, id, named(word, place + 1));
        Control& added = b.list.controls.back();
        added.clip = area.view;
        added.group = kRowsGroup;
        show_working(b, part_at, id, rect);
        // The rows and their focus ring are clipped to the view.
        for (std::size_t drawn = part_at; drawn < b.list.items.size(); ++drawn)
            b.list.items[drawn].clip = area.view;
        b.order.push_back(id);
    }
    Item& bar = add_part(b.list, Role::scroll_bar, area.well);
    bar.look = ScrollBarLook{area.thumb(kMetrics.least_thumb_height), false};
    b.rows = area;
    return kListView;
}

/// Card grid: three working cards, then a card in each state and chosen.
int32_t card_grid(Builder& b, Point at, int32_t width, std::string_view word) {
    const GalleryModel& model = *b.model;
    const int32_t grid_width = std::min(
        width,
        static_cast<int32_t>(kCardCount) * (kCardWidth + kMetrics.card_gap) - kMetrics.card_gap
    );
    const std::vector<Rect> cells = card_cells({at.x, at.y, grid_width, 0}, kCardCount, kCardCount);
    int32_t top = at.y;
    for (std::size_t place = 0; place < kCardCount && place < cells.size(); ++place) {
        const ControlId id = kFirstCard + static_cast<ControlId>(place);
        CardLook look = card_look(kPackages[place]);
        look.selected = model.card == place;
        look.hovered = pointer_on(model, id);
        const std::size_t part_at = b.list.items.size();
        add_card(b.list, cells[place], look, id, named(word, place + 1));
        show_working(b, part_at, id, cells[place]);
        b.order.push_back(id);
        top = std::max(top, cells[place].y + cells[place].height);
    }
    top += kLineGap;
    top += add_states(
        b.list,
        {at.x, top},
        width,
        {kSampleCardWidth, card_height(kSampleCardWidth)},
        kSelectableStates,
        [&](const Rect& rect, Sample shown) {
            CardLook sample = card_look(kPackages[3]);
            sample.hovered = looks_hovered(shown);
            sample.selected = shown == Sample::selected;
            sample.greyed = shown == Sample::disabled;
            add_fixed(b.list, rect, rect, shown, false, [&] {
                add_card(b.list, rect, sample, no_control, {});
            });
        }
    );
    return top - at.y;
}

/// Hover card: a card beside what it describes, an outlined box holding the
/// OA mark, its arrow pointing at the box.
int32_t hover_card(Builder& b, Point at, int32_t width, std::string_view /*word*/) {
    HoverCardLook look;
    look.mark = true;
    look.title = std::string(kPackages[0].name);
    look.aside = std::string(kPackages[0].version);
    look.rows = {
        {"By", "the sample team", colour::text},
        {"Size", std::string(kPackages[0].size), colour::text},
        {"Rules", "Differ from the host's", colour::lock},
    };
    look.block = {"Another Mod and Third Mod", "need it."};
    look.arrow = Side::left;
    const int32_t left = at.x + kAnchorSide + kMetrics.hover_card_arrow + kCellGap / 2;
    Point size = hover_card_size(*b.fonts, look);
    if (size.x > at.x + width - left) {
        look.width = std::max(at.x + width - left, 1);
        size = hover_card_size(*b.fonts, look);
    }
    const Rect card{left, at.y, size.x, size.y};
    const Rect anchor{
        at.x, at.y + std::max((size.y - kAnchorSide) / 2, 0), kAnchorSide, kAnchorSide
    };
    add_part(b.list, Role::outline, anchor).colour = colour::control_border;
    const Point mark = mark_size(Mark::oa_small);
    add_mark(
        b.list,
        {anchor.x + (anchor.width - mark.x) / 2, anchor.y + (anchor.height - mark.y) / 2},
        Mark::oa_small,
        colour::accent
    );
    look.arrow_at = anchor.y + anchor.height / 2 - card.y;
    add_hover_card(b.list, card, look);
    return std::max(card.height, anchor.y + anchor.height - at.y);
}

/// Section, label and hint: a section's heading over two rows, the second
/// with a line drawn as a notice.
int32_t section_label_hint(Builder& b, Point at, int32_t width, std::string_view /*word*/) {
    Item& heading = add_part(b.list, Role::heading, {at.x, at.y, width, kHeadingHeight});
    heading.text = "LANGUAGE";
    heading.look = HeadingLook{"LANGUAGE"};
    int32_t top = at.y + kHeadingHeight + 2;
    const auto add_row = [&](std::string_view label,
                             std::span<const std::string_view> hints,
                             std::size_t notices_from) {
        RowFrame frame;
        frame.top = top;
        frame.left = at.x;
        frame.width = width;
        const int32_t label_top = top + 1 + kMetrics.row_padding / 2;
        frame.label = {at.x, label_top, width, kMetrics.regular_line};
        frame.label_text = std::string(label);
        int32_t line_top = label_top + kMetrics.regular_line + kMetrics.hint_gap;
        for (std::size_t line = 0; line < hints.size(); ++line) {
            frame.hints.push_back({at.x, line_top, width, kMetrics.small_line});
            frame.hint_texts.emplace_back(hints[line]);
            frame.hint_notices.push_back(line >= notices_from);
            line_top += kMetrics.small_line;
        }
        const int32_t row_height = line_top + kMetrics.row_padding / 2 - top;
        Item& part = add_part(b.list, Role::row_frame, {at.x, top, width, row_height});
        part.text = std::string(label);
        part.look = std::move(frame);
        top += row_height;
    };
    constexpr std::array<std::string_view, 1> kLanguageHint{
        "The game's own text and unit names, where its data has them."
    };
    constexpr std::array<std::string_view, 2> kSizeHint{
        "How large the game's text is drawn.", "Needs modern fonts."
    };
    add_row("Language", kLanguageHint, kLanguageHint.size());
    add_row("Text size", kSizeHint, 1);
    return top - at.y;
}

/// Lock: two labels, each with the lock that says why it cannot change.
int32_t lock_section(Builder& b, Point at, int32_t width, std::string_view /*word*/) {
    constexpr std::array<std::string_view, 2> kLabels{"Unicode chat", "Text size"};
    constexpr std::array<std::string_view, 2> kLocks{"Set by the language", "Needs modern fonts"};
    int32_t top = at.y;
    for (std::size_t place = 0; place < kLabels.size(); ++place) {
        const Rect line{at.x, top, width, kMetrics.regular_line};
        add_words(b.list, line, kLabels[place], FontRole::regular, Align::left, colour::text);
        add_lock(b.list, line, LockLook{std::string(kLocks[place])});
        top += kMetrics.regular_line + kMetrics.chip_gap;
    }
    return top - kMetrics.chip_gap - at.y;
}

/// Takes the controls off a component's list and the controls off its
/// parts: a panel takes no input.
///
/// @param[in,out] list the list
void without_input(DisplayList& list) {
    list.controls.clear();
    list.tab_order.clear();
    for (Item& part : list.items)
        part.control = no_control;
}

/// Notice: a notice at Compact, with a folder's path, its open button and OK.
int32_t notice_section(Builder& b, Point at, int32_t /*width*/, std::string_view /*word*/) {
    Notice notice;
    notice.title = "SAMPLE MOD INSTALLED";
    notice.paragraphs = {
        {"Sample Mod 4.8 · rev 3 is ready to play, in your Mods folder:", false},
        {"Mods/sample-mod", true},
    };
    notice.open_caption = "Open folder";
    GalleryPanel panel;
    panel.origin = at;
    const int32_t height = place_notice(notice, b.fonts).height;
    panel.place = {at.x, at.y, notice_width, height};
    panel.list = notice_list(notice, b.fonts);
    without_input(panel.list);
    b.panels.push_back(std::move(panel));
    return height;
}

/// Question: a question at Compact with two buttons and a progress bar.
int32_t question_section(Builder& b, Point at, int32_t /*width*/, std::string_view /*word*/) {
    Question question;
    question.title = "UPDATE SAMPLE MOD?";
    question.paragraphs = {
        {"Sample Mod 4.8 · rev 3 replaces 4.7. Another Mod and Third Mod use it.", false},
    };
    question.buttons = {{"Cancel", false, "cancel"}, {"Update", true, "update"}};
    question.cancel_button = 0;
    question.primary_button = 1;
    question.progress = 400;
    question.marked = 1;
    GalleryPanel panel;
    panel.origin = at;
    const int32_t height = place_question(question, b.fonts).height;
    panel.place = {at.x, at.y, notice_width, height};
    panel.list = question_list(question, b.fonts);
    without_input(panel.list);
    b.panels.push_back(std::move(panel));
    return height;
}

/// OA mark: a working OA button, the OA mark alone, the icon as a picture
/// and the kit's one-bit marks, then an OA button in each state.
int32_t oa_mark(Builder& b, Point at, int32_t width, std::string_view word) {
    const GalleryModel& model = *b.model;
    const int32_t middle = at.y + kMarkSide / 2;
    const Rect button{at.x, middle - kOaSide / 2, kOaSide, kOaSide};
    {
        Item& part = add_part(b.list, Role::oa_button, button);
        OaButtonState state = OaButtonState::idle;
        if (model.interaction.pressed == kOaButton && model.interaction.hovered == kOaButton)
            state = OaButtonState::pressed;
        else if (pointer_on(model, kOaButton))
            state = OaButtonState::hovered;
        part.look = OaButtonLook{kOaSide, state, false};
        Control control;
        control.id = kOaButton;
        control.rect = button;
        control.name = named(word, 1);
        control.kind = ControlKind::button;
        control.text = "OA";
        b.list.controls.push_back(std::move(control));
        show_working(b, b.list.items.size() - 1, kOaButton, button);
        b.order.push_back(kOaButton);
    }
    int32_t x = button.x + button.width + kMarkGap;
    add_part(b.list, Role::oa_mark, {x, at.y, kMarkSide, kMarkSide});
    x += kMarkSide + kMarkGap;
    add_part(b.list, Role::picture, {x, at.y, kMarkSide, kMarkSide});
    x += kMarkSide + kMarkGap;
    constexpr std::array<Mark, 7> kMarks{
        Mark::oa_small,
        Mark::oa_large,
        Mark::padlock,
        Mark::arrow_closed,
        Mark::arrow_open,
        Mark::choice_arrow,
        Mark::magnifier,
    };
    int32_t line_bottom = at.y + kMarkSide;
    int32_t y = middle;
    for (const Mark mark : kMarks) {
        const Point size = mark_size(mark);
        if (x + size.x > at.x + width) {
            x = at.x;
            y = line_bottom + kLineGap + kMetrics.small_line / 2;
            line_bottom = y + kMetrics.small_line / 2;
        }
        add_mark(
            b.list, {x, y - size.y / 2}, mark, mark == Mark::padlock ? colour::lock : colour::accent
        );
        x += size.x + kCellGap;
    }
    int32_t top = line_bottom + kLineGap;
    top += add_states(
               b.list,
               {at.x, top},
               width,
               {kOaSide, kOaSide},
               kFiveStates,
               [&](const Rect& rect, Sample shown) {
                   add_fixed(b.list, rect, rect, shown, true, [&] {
                       OaButtonState state = OaButtonState::idle;
                       if (shown == Sample::hovered)
                           state = OaButtonState::hovered;
                       else if (shown == Sample::pressed)
                           state = OaButtonState::pressed;
                       add_part(b.list, Role::oa_button, rect).look =
                           OaButtonLook{kOaSide, state, false};
                   });
               }
           ) +
           kLineGap;
    // At rest wearing the attention dot, as the main menu's does while
    // updates wait.
    constexpr std::array<std::string_view, 1> kDot{"With its dot"};
    top += add_cells(
        b.list, {at.x, top}, width, {kOaSide, kOaSide}, kDot, [&](const Rect& rect, std::size_t) {
            add_fixed(b.list, rect, {}, Sample::idle, false, [&] {
                add_part(b.list, Role::oa_button, rect).look =
                    OaButtonLook{kOaSide, OaButtonState::idle, true};
            });
        }
    );
    return top - at.y;
}

/// A language the carousel and the language-card row show.
struct Tongue {
    std::string_view letters{}; ///< the badge, when the pack has no picture
    std::string_view own{};     ///< the language's own name
    std::string_view english{}; ///< its English name
    std::string_view facts{};   ///< the facts line
    std::string_view pill{};    ///< the pill's words
    std::string_view tag{};     ///< the carousel card's automation word
    LanguageAvailability availability{LanguageAvailability::built_in};
};

/// The carousel's five cards. The centre is the third, so two show on each side.
constexpr std::array<Tongue, kCarouselCount> kTongues{{
    {"EN",
     "English",
     "English",
     "Built into the game",
     "Built in",
     "english",
     LanguageAvailability::built_in},
    {"DE",
     "Deutsch",
     "German",
     "Built into the game",
     "Built in",
     "deutsch",
     LanguageAvailability::built_in},
    {"ES",
     "Español",
     "Spanish",
     "Built into the game",
     "Built in",
     "espanol",
     LanguageAvailability::built_in},
    {"FR",
     "Français",
     "French",
     "Built into the game",
     "Built in",
     "francais",
     LanguageAvailability::built_in},
    {"JA",
     "日本語",
     "Japanese",
     "From Ridge · Coverage 100%",
     "Get · 3.4 MB",
     "nihongo",
     LanguageAvailability::get},
}};

/// Tells whether a press is held on a control with the pointer still over it.
///
/// @param model the model
/// @param id the control
/// @return true when it is
bool held_on(const GalleryModel& model, ControlId id) noexcept {
    return model.interaction.hovered == id && model.interaction.pressed == id;
}

/// Fills a language card's look from a tongue.
///
/// @param tongue the tongue
/// @param hovered the pointer is over the card
/// @param held a press is held on it
/// @param disabled it takes nothing
/// @return the look
LanguageCardLook tongue_look(const Tongue& tongue, bool hovered, bool held, bool disabled) {
    LanguageCardLook look;
    look.letters = std::string(tongue.letters);
    look.own_name = std::string(tongue.own);
    look.english_name = std::string(tongue.english);
    look.facts = std::string(tongue.facts);
    look.pill = std::string(tongue.pill);
    look.availability = tongue.availability;
    look.hovered = hovered;
    look.held = held;
    look.disabled = disabled;
    return look;
}

/// Tells whether the centre card and two turned cards on each side lie inside
/// a band. The outer cards shrink, so the band can be narrower than four
/// steps plus a full card.
///
/// @param geometry the card and the step
/// @param room the band's width, in points
/// @return true when every shown face lies in the band
bool turned_faces_fit(const CarouselGeometry& geometry, int32_t room) {
    if (room <= 0 || geometry.card_width <= 0 || geometry.card_height <= 0 || geometry.step <= 0)
        return false;
    const Rect area{0, 0, room, geometry.card_height};
    const std::vector<CarouselSlot> slots =
        carousel_places(area, geometry, static_cast<int32_t>(kCarouselCount), 2 * 1000, false);
    if (slots.size() != kCarouselCount)
        return false;
    for (const CarouselSlot& slot : slots) {
        if (slot.face.x < area.x || slot.face.y < area.y)
            return false;
        if (slot.face.x + slot.face.width > area.x + area.width)
            return false;
        if (slot.face.y + slot.face.height > area.y + area.height)
            return false;
    }
    return true;
}

/// First run, across the whole gallery, at the language card's real size.
/// The carousel shows the centre card and two turned cards on each side.
/// Compact draws the sample cards at the carousel's fitted size, and
/// smaller still when that would put a choice card over CLOSE. The card
/// scales with that rectangle. What does not fit scrolls with the section.
int32_t first_run(Builder& b, Point at, int32_t width, std::string_view /*word*/) {
    const GalleryModel& model = *b.model;
    const Metrics& metrics = b.metrics != nullptr ? *b.metrics : kMetrics;
    constexpr int32_t kArrowWidth = 22;
    constexpr int32_t kArrowGap = 4;
    constexpr int32_t kGap = 4;
    constexpr int32_t kSetupHeight = 62;
    const int32_t room = std::max(width - 2 * (kArrowWidth + kArrowGap), int32_t{1});
    const int32_t card_w = std::max(metrics.language_card_width, int32_t{1});
    const int32_t card_h = std::max(metrics.language_card_height, int32_t{1});
    CarouselGeometry geometry{card_w, card_h, std::max(metrics.carousel_step, int32_t{1})};
    if (!turned_faces_fit(geometry, room))
        geometry = carousel_fit(room, metrics);

    const int32_t row_height = geometry.card_height + carousel_pager_band;
    const int32_t centre =
        std::clamp(model.carousel_index, int32_t{0}, static_cast<int32_t>(kCarouselCount) - 1);
    const Rect previous{
        at.x,
        at.y + (geometry.card_height - kMetrics.button_height) / 2,
        kArrowWidth,
        kMetrics.button_height
    };
    const Rect band{previous.x + previous.width + kArrowGap, at.y, room, row_height};
    const Rect next{
        band.x + band.width + kArrowGap, previous.y, kArrowWidth, kMetrics.button_height
    };

    CarouselLook carousel;
    carousel.card_width = geometry.card_width;
    carousel.card_height = geometry.card_height;
    carousel.step = geometry.step;
    carousel.centre_milli = centre * 1000;
    carousel.cards.reserve(kTongues.size());
    for (std::size_t place = 0; place < kTongues.size(); ++place) {
        const ControlId id = kFirstCarousel + static_cast<ControlId>(place);
        const bool over = model.interaction.hovered == id;
        CarouselCard card;
        card.letters = std::string(kTongues[place].letters);
        card.own_name = std::string(kTongues[place].own);
        card.english_name = std::string(kTongues[place].english);
        card.facts = std::string(kTongues[place].facts);
        card.pill = std::string(kTongues[place].pill);
        card.availability = kTongues[place].availability;
        card.hovered = over;
        card.held = held_on(model, id);
        carousel.cards.push_back(std::move(card));
    }
    add_carousel(b.list, band, carousel);

    const std::vector<CarouselSlot> slots = carousel_places(
        carousel_band(band),
        geometry,
        static_cast<int32_t>(kCarouselCount),
        carousel.centre_milli,
        false
    );
    std::array<Rect, kCarouselCount> faces{};
    std::array<bool, kCarouselCount> face_shown{};
    for (const CarouselSlot& slot : slots) {
        if (slot.index < 0 || slot.index >= static_cast<int32_t>(kCarouselCount))
            continue;
        faces[static_cast<std::size_t>(slot.index)] = slot.face;
        face_shown[static_cast<std::size_t>(slot.index)] =
            slot.opacity > 0 && slot.face.width > 0 && slot.face.height > 0;
    }
    {
        const bool over = model.interaction.hovered == kCarouselPrevious;
        const std::size_t part_at = b.list.items.size();
        add_button(
            b.list,
            previous,
            button_look("<", ButtonStyle::plain, over, held_on(model, kCarouselPrevious)),
            kCarouselPrevious,
            "gallery.carousel.previous"
        );
        b.list.controls.back().steps = true;
        show_working(b, part_at, kCarouselPrevious, previous);
        b.order.push_back(kCarouselPrevious);
    }
    for (std::size_t place = 0; place < kCarouselCount; ++place) {
        if (!face_shown[place])
            continue;
        const ControlId id = kFirstCarousel + static_cast<ControlId>(place);
        add_carousel_control(
            b.list,
            faces[place],
            id,
            "gallery.carousel.card." + std::string(kTongues[place].tag),
            std::string(kTongues[place].own),
            true
        );
        if (focus_on(model, id))
            add_focus_ring(b.list, faces[place], true);
        b.order.push_back(id);
    }
    {
        const bool over = model.interaction.hovered == kCarouselNext;
        const std::size_t part_at = b.list.items.size();
        add_button(
            b.list,
            next,
            button_look(">", ButtonStyle::plain, over, held_on(model, kCarouselNext)),
            kCarouselNext,
            "gallery.carousel.next"
        );
        b.list.controls.back().steps = true;
        show_working(b, part_at, kCarouselNext, next);
        b.order.push_back(kCarouselNext);
    }

    struct LanguageSample {
        Tongue tongue;
        bool working;
        Sample shown;
    };

    const std::array<LanguageSample, 7> language_samples{{
        {kTongues[0], true, Sample::idle},
        {{"DE",
          "Deutsch",
          "German",
          "Built into the game",
          "Get · 3.4 MB",
          "deutsch",
          LanguageAvailability::get},
         false,
         Sample::hovered},
        {{"ES",
          "Español",
          "Spanish",
          "Built into the game",
          "Downloading…",
          "espanol",
          LanguageAvailability::downloading},
         false,
         Sample::pressed},
        {{"FR",
          "Français",
          "French",
          "Built into the game",
          "Active",
          "francais",
          LanguageAvailability::active},
         false,
         Sample::focused},
        {{"JA",
          "日本語",
          "Japanese",
          "From Ridge · Coverage 100%",
          "Offline",
          "nihongo",
          LanguageAvailability::offline},
         false,
         Sample::disabled},
        {{"EN",
          "English",
          "English",
          "Built into the game",
          "Checking…",
          "english",
          LanguageAvailability::checking},
         false,
         Sample::idle},
        {{"RD",
          "Ridge",
          "Ridge",
          "From Ridge · Coverage 100%",
          "Couldn't download",
          "ridge",
          LanguageAvailability::failed},
         false,
         Sample::idle},
    }};

    auto add_language = [&](std::size_t place, const Rect& card) {
        const LanguageSample& sample = language_samples[place];
        if (sample.working) {
            const bool over = model.interaction.hovered == kLanguageCard;
            const std::size_t part_at = b.list.items.size();
            add_language_card(
                b.list,
                card,
                tongue_look(sample.tongue, over, held_on(model, kLanguageCard), false),
                kLanguageCard,
                "gallery.language-card.1"
            );
            show_working(b, part_at, kLanguageCard, card);
            b.order.push_back(kLanguageCard);
            return;
        }
        const LanguageCardLook look = tongue_look(
            sample.tongue,
            looks_hovered(sample.shown),
            sample.shown == Sample::pressed,
            sample.shown == Sample::disabled
        );
        add_fixed(b.list, card, card, sample.shown, false, [&] {
            add_language_card(b.list, card, look, no_control, {});
        });
    };

    constexpr std::array<std::string_view, kChoiceCount> kChoiceTitles{"Ridge pack", "Not now"};
    constexpr std::array<std::string_view, kChoiceCount> kChoiceHints{
        "From the sample catalogue.", "Set this up later."
    };
    auto add_working_choice = [&](std::size_t place, const Rect& card) {
        const ControlId id = kFirstChoice + static_cast<ControlId>(place);
        ChoiceCardLook look;
        look.title = std::string(kChoiceTitles[place]);
        look.hint = std::string(kChoiceHints[place]);
        look.chosen = place == model.choice_index;
        look.hovered = model.interaction.hovered == id;
        look.held = held_on(model, id);
        const std::size_t part_at = b.list.items.size();
        add_choice_card(b.list, card, look, id, named("choice-card", place + 1));
        show_working(b, part_at, id, card);
        b.order.push_back(id);
    };
    constexpr std::array<Sample, 4> kChoiceStates{
        Sample::hovered, Sample::pressed, Sample::focused, Sample::disabled
    };
    auto add_choice_state = [&](const Rect& card, Sample shown) {
        add_fixed(b.list, card, card, shown, false, [&] {
            ChoiceCardLook look;
            look.title = "Ridge pack";
            look.hint = "A choice.";
            look.chosen = shown == Sample::focused;
            look.hovered = looks_hovered(shown);
            look.held = shown == Sample::pressed;
            look.disabled = shown == Sample::disabled;
            add_choice_card(b.list, card, look, no_control, {});
        });
    };
    auto add_chrome = [&](const Rect& panel_rect) {
        SetupChromeLook chrome;
        chrome.width = panel_rect.width;
        chrome.height = panel_rect.height;
        chrome.page = "Language";
        chrome.step_name = "LANGUAGE";
        chrome.step_index = 1;
        chrome.step_count = 3;
        chrome.skip_hovered = model.interaction.hovered == kSetupSkip;
        chrome.skip_held = held_on(model, kSetupSkip);
        chrome.back_hovered = model.interaction.hovered == kSetupBack;
        chrome.back_held = held_on(model, kSetupBack);
        chrome.next_hovered = model.interaction.hovered == kSetupNext;
        chrome.next_held = held_on(model, kSetupNext);
        GalleryPanel panel;
        panel.origin = {panel_rect.x, panel_rect.y};
        panel.place = panel_rect;
        add_setup_chrome(panel.list, {0, 0}, chrome);
        b.panels.push_back(std::move(panel));
        const SetupButtons buttons = setup_buttons(b.fonts, chrome);
        const Point origin{panel_rect.x, panel_rect.y};
        add_setup_controls(
            b.list,
            origin,
            buttons,
            chrome,
            kSetupSkip,
            "gallery.setup.skip",
            kSetupBack,
            "gallery.setup.back",
            kSetupNext,
            "gallery.setup.next"
        );
        const Rect skip{
            buttons.skip.x + origin.x,
            buttons.skip.y + origin.y,
            buttons.skip.width,
            buttons.skip.height
        };
        const Rect back{
            buttons.back.x + origin.x,
            buttons.back.y + origin.y,
            buttons.back.width,
            buttons.back.height
        };
        const Rect next_button{
            buttons.next.x + origin.x,
            buttons.next.y + origin.y,
            buttons.next.width,
            buttons.next.height
        };
        const std::array<std::pair<ControlId, Rect>, 3> setup_controls{{
            {kSetupSkip, skip},
            {kSetupBack, back},
            {kSetupNext, next_button},
        }};
        for (const std::pair<ControlId, Rect>& setup_control : setup_controls) {
            if (focus_on(model, setup_control.first))
                add_focus_ring(b.list, setup_control.second, true);
            b.order.push_back(setup_control.first);
        }
    };
    auto add_picture = [&](const Rect& well) {
        PictureWellLook picture;
        picture.caption = "Library, Active";
        picture.stand_in = true;
        add_picture_well(b.list, well, picture);
    };
    auto add_coach = [&](const Rect& layer) {
        const int32_t mark = std::min(int32_t{28}, std::max(layer.height - 8, int32_t{16}));
        Rect target;
        Rect coach_card;
        // The compact column is only as wide as the row leaves. The card stays
        // wide enough for its body; the spotlight target shrinks to fit beside it.
        if (layer.height <= kSetupHeight) {
            constexpr int32_t kCard = 100;
            constexpr int32_t kTarget = 20;
            constexpr int32_t kLeft = 2;
            constexpr int32_t kGapHere = 4;
            constexpr int32_t kRight = 2;
            int32_t target_w = kTarget;
            int32_t card_w = layer.width - kLeft - target_w - kGapHere - kRight;
            if (card_w < kCard) {
                target_w = std::max(layer.width - kLeft - kGapHere - kRight - kCard, int32_t{12});
                card_w = std::max(layer.width - kLeft - target_w - kGapHere - kRight, int32_t{40});
            }
            const int32_t target_h = std::min(mark, std::max(layer.height - 8, int32_t{16}));
            target = {
                layer.x + kLeft,
                layer.y + std::max((layer.height - target_h) / 2, int32_t{0}),
                target_w,
                target_h,
            };
            coach_card = {
                target.x + target.width + kGapHere,
                layer.y + 2,
                card_w,
                std::max(layer.height - 4, int32_t{40}),
            };
        } else {
            int32_t target_w = std::min(mark, std::max(layer.width / 5, int32_t{16}));
            // The title "Library" needs a card about 140 points wide. A
            // narrower leftover shrinks the spotlight instead of the words.
            constexpr int32_t kCardLeast = 140;
            const int32_t gaps = coach_gap + 8;
            if (layer.width - gaps - target_w < kCardLeast)
                target_w = std::max(layer.width - gaps - kCardLeast, int32_t{8});
            target = {
                layer.x + 4,
                layer.y + std::max((layer.height - mark) / 2, int32_t{0}),
                target_w,
                mark,
            };
            const int32_t wanted_w =
                std::max(layer.width - (target.width + coach_gap + 8), int32_t{40});
            const int32_t wanted_h = std::max(layer.height - 8, int32_t{40});
            coach_card = place_coach_mark(layer, target, wanted_w, wanted_h);
            const Rect overlap = intersect(coach_card, target);
            if (coach_card.width < 48 || coach_card.height < 40 ||
                (overlap.width > 0 && overlap.height > 0)) {
                // Stay inside the column. A narrow column cannot hold a
                // 40-point card beside the target, and forcing that width
                // runs the card out of the section.
                const int32_t right = layer.x + layer.width - 4;
                int32_t card_x = target.x + target.width + coach_gap;
                int32_t card_w = right - card_x;
                if (card_w < 40) {
                    card_w = std::min(std::max(layer.width - 8, int32_t{1}), int32_t{48});
                    card_x = right - card_w;
                }
                if (card_x < layer.x + 2)
                    card_x = layer.x + 2;
                if (card_x + card_w > right)
                    card_w = std::max(right - card_x, int32_t{1});
                coach_card = {
                    card_x,
                    layer.y + 4,
                    card_w,
                    std::max(layer.height - 8, int32_t{1}),
                };
            }
        }
        add_spotlight(b.list, layer, SpotlightLook{target});
        CoachMarkLook coach_mark;
        coach_mark.number = 1;
        coach_mark.title = "Library";
        coach_mark.body = "Open it from the main menu.";
        coach_mark.target = target;
        add_coach_mark(b.list, coach_card, coach_mark);
    };
    auto add_step = [&](const Rect& step_rect) {
        StepLook step;
        step.name = "LANGUAGE";
        step.index = 1;
        step.count = 3;
        add_step_indicator(b.list, step_rect, step);
    };
    auto add_calls = [&](const Rect& area, bool stacked, bool captions) {
        constexpr int32_t kCount = 5;
        constexpr std::array<Sample, kCount> kStates{
            Sample::idle,
            Sample::hovered,
            Sample::pressed,
            Sample::focused,
            Sample::disabled,
        };
        const int32_t button_h = kMetrics.button_height;
        int32_t button_w = 1;
        int32_t origin_x = area.x;
        int32_t origin_y = area.y;
        int32_t pitch_x = 0;
        int32_t pitch_y = 0;
        if (stacked) {
            constexpr int32_t kCallGap = 2;
            const int32_t stack = kCount * button_h + (kCount - 1) * kCallGap;
            origin_y = area.y + std::max((area.height - stack) / 2, int32_t{0});
            button_w = std::max(area.width - 12, int32_t{1});
            origin_x = area.x + std::max((area.width - button_w) / 2, int32_t{0});
            pitch_y = button_h + kCallGap;
        } else {
            constexpr int32_t kCallGap = 8;
            const int32_t gaps = (kCount - 1) * kCallGap;
            button_w = std::max((area.width - gaps) / kCount, int32_t{1});
            const int32_t used = kCount * button_w + gaps;
            origin_x = area.x + std::max((area.width - used) / 2, int32_t{0});
            pitch_x = button_w + kCallGap;
        }
        for (int32_t place = 0; place < kCount; ++place) {
            const Rect button{
                origin_x + place * pitch_x,
                origin_y + place * pitch_y,
                button_w,
                button_h,
            };
            const Sample shown = kStates[static_cast<std::size_t>(place)];
            if (place == 0) {
                const bool over = model.interaction.hovered == kCallToAction;
                CallToActionLook look;
                look.caption = "Start playing";
                look.hovered = over;
                look.held = held_on(model, kCallToAction);
                look.enabled = true;
                look.time_ms = 400;
                const std::size_t part_at = b.list.items.size();
                add_call_to_action(b.list, button, look, kCallToAction, "gallery.call-to-action.1");
                show_working(b, part_at, kCallToAction, button);
                b.order.push_back(kCallToAction);
            } else {
                add_fixed(b.list, button, button, shown, false, [&] {
                    CallToActionLook look;
                    look.caption = "Start playing";
                    look.hovered = looks_hovered(shown);
                    look.held = shown == Sample::pressed;
                    look.enabled = shown != Sample::disabled;
                    look.time_ms = 400;
                    look.reduced_motion = shown == Sample::focused;
                    add_call_to_action(b.list, button, look, no_control, {});
                });
            }
            if (!captions)
                continue;
            add_words(
                b.list,
                {button.x, button.y + button_h + kCaptionGap + 2, button_w, kMetrics.small_line},
                caption_of(shown),
                FontRole::small,
                Align::left,
                colour::hint
            );
        }
    };

    const bool compact = card_w <= compact_metrics.language_card_width;
    const int32_t choice_h = choice_card_height();
    // Sample cards follow the carousel's fitted card, so a narrow window
    // keeps the design's shape. On Compact the two working choice cards
    // share the second row and must end inside the scroll view: a card
    // that runs into the footer covers CLOSE.
    int32_t sample_w = compact ? geometry.card_width : card_w;
    int32_t sample_h = compact ? geometry.card_height : card_h;
    if (compact) {
        const int32_t view_bottom = b.view.y + b.view.height;
        const int32_t room_for_card = view_bottom - at.y - row_height - 2 * choice_h;
        if (room_for_card > 0 && sample_h > room_for_card) {
            sample_w = std::max(sample_w * room_for_card / sample_h, int32_t{1});
            sample_h = room_for_card;
        }
    }
    int32_t bottom = at.y + row_height;
    if (compact) {
        const int32_t row1 = at.y + row_height;
        const int32_t row2 = row1 + sample_h;
        const int32_t row3 = row2 + sample_h;
        const int32_t row4 = row3 + kSetupHeight;
        const int32_t used_four = 4 * sample_w + 3 * kGap;
        const Rect calls{
            at.x + used_four + kGap, row1, std::max(width - used_four - kGap, int32_t{1}), sample_h
        };
        for (int32_t index = 0; index < 4; ++index)
            add_language(
                static_cast<std::size_t>(index),
                {at.x + index * (sample_w + kGap), row1, sample_w, sample_h}
            );
        const int32_t used_three = 3 * sample_w + 2 * kGap;
        const int32_t pocket_x = at.x + used_three + kGap;
        const int32_t pocket_w = std::max(at.x + width - pocket_x, int32_t{1});
        for (int32_t index = 0; index < 3; ++index)
            add_language(
                static_cast<std::size_t>(index + 4),
                {at.x + index * (sample_w + kGap), row2, sample_w, sample_h}
            );
        add_working_choice(0, {pocket_x, row2, pocket_w, choice_h});
        add_working_choice(1, {pocket_x, row2 + choice_h, pocket_w, choice_h});
        const int32_t step_h = step_indicator_height();
        add_step({pocket_x, row2 + sample_h - step_h, pocket_w, step_h});

        // The setup header holds "Open Annihilation", the page word and the
        // step indicator on one line. The picture stays wide enough for its
        // caption. The coach column keeps a 100-point card beside its target.
        constexpr int32_t kCoachWidth = 128;
        constexpr int32_t kPictureWidth = 88;
        const int32_t setup_w =
            std::max(width - kCoachWidth - kPictureWidth - 2 * kGap, int32_t{1});
        add_chrome({at.x, row3, setup_w, kSetupHeight});
        add_picture({at.x + setup_w + kGap, row3, kPictureWidth, kSetupHeight});
        add_coach({
            at.x + setup_w + kGap + kPictureWidth + kGap,
            row3,
            kCoachWidth,
            kSetupHeight,
        });

        const int32_t strip_w = std::max(width - kButtonWidth - kGap, int32_t{1});
        const int32_t short_w = std::max((strip_w - 3 * kGap) / 4, int32_t{1});
        for (std::size_t state_at = 0; state_at < kChoiceStates.size(); ++state_at)
            add_choice_state(
                {at.x + static_cast<int32_t>(state_at) * (short_w + kGap), row4, short_w, choice_h},
                kChoiceStates[state_at]
            );
        add_calls(calls, true, false);
        bottom = row4 + choice_h;
    } else {
        const int32_t vgap = card_w >= large_metrics.language_card_width ? 8 : 2;
        const int32_t row1 = at.y + row_height + vgap;
        const int32_t row2 = row1 + card_h + vgap;
        const int32_t choice_y = row2 + card_h + vgap;
        const int32_t four_x = at.x + 4 * card_w + 4 * kGap;
        const int32_t three_x = at.x + 3 * card_w + 3 * kGap;
        const int32_t pocket1_w = std::max(at.x + width - four_x, int32_t{1});
        const int32_t pocket2_w = std::max(at.x + width - three_x, int32_t{1});
        for (int32_t index = 0; index < 4; ++index)
            add_language(
                static_cast<std::size_t>(index),
                {at.x + index * (card_w + kGap), row1, card_w, card_h}
            );
        for (int32_t index = 0; index < 3; ++index)
            add_language(
                static_cast<std::size_t>(index + 4),
                {at.x + index * (card_w + kGap), row2, card_w, card_h}
            );

        int32_t long_w = std::min(card_w + 50, std::max((width - 5 * kGap) / 3, int32_t{1}));
        const int32_t short_w = std::max((width - 2 * long_w - 5 * kGap) / 4, int32_t{1});
        int32_t choice_x = at.x;
        for (std::size_t place = 0; place < kChoiceCount; ++place) {
            add_working_choice(place, {choice_x, choice_y, long_w, choice_h});
            choice_x += long_w + kGap;
        }
        for (std::size_t state_at = 0; state_at < kChoiceStates.size(); ++state_at) {
            add_choice_state({choice_x, choice_y, short_w, choice_h}, kChoiceStates[state_at]);
            choice_x += short_w + kGap;
        }

        add_chrome({three_x, row2, pocket2_w, kSetupHeight});
        // Beside four language cards the leftover is a third of a Regular
        // window, too narrow for "Library". When that column cannot hold the
        // coach, the picture sits above it and the coach takes the pocket's
        // whole width. Large already leaves a wide enough column beside the
        // picture.
        const int32_t picture_w = card_w >= large_metrics.language_card_width ? 168 : 112;
        constexpr int32_t kCoachLeast = 140;
        const int32_t beside = std::max(pocket1_w - picture_w - kGap, int32_t{0});
        if (beside >= kCoachLeast) {
            add_picture({four_x, row1, picture_w, card_h});
            add_coach({four_x + picture_w + kGap, row1, beside, card_h});
        } else {
            const int32_t picture_h = std::max(std::min(card_h / 3, int32_t{96}), int32_t{40});
            const int32_t coach_y = row1 + picture_h + kGap;
            add_picture({four_x, row1, std::max(pocket1_w, int32_t{1}), picture_h});
            add_coach({
                four_x,
                coach_y,
                std::max(pocket1_w, int32_t{1}),
                std::max(row1 + card_h - coach_y, int32_t{40}),
            });
        }
        const int32_t under = row2 + kSetupHeight + 4;
        add_step({
            three_x,
            under + kMetrics.button_height + kMetrics.small_line + 8,
            pocket2_w,
            step_indicator_height(),
        });
        add_calls({three_x, under, pocket2_w, kMetrics.button_height}, false, true);
        bottom = choice_y + choice_h;
    }
    return bottom - at.y;
}

/// A 16 by 16 checker kept for the gallery's life, in two kit colours.
///
/// @return the picture
oa::ui::frontend_renderer::RgbaPicture badge_checker() {
    static std::vector<uint8_t> pixels;
    if (pixels.empty()) {
        constexpr uint32_t side = 16;
        pixels.assign(
            static_cast<std::size_t>(side) * side * oa::ui::frontend_renderer::picture_pixel_bytes,
            0
        );
        for (uint32_t y = 0; y < side; ++y) {
            for (uint32_t x = 0; x < side; ++x) {
                const Colour ink = ((x / 8) ^ (y / 8)) == 0 ? colour::hint : colour::text;
                const std::size_t at = (static_cast<std::size_t>(y) * side + x) *
                                       oa::ui::frontend_renderer::picture_pixel_bytes;
                pixels[at] = ink.r;
                pixels[at + 1] = ink.g;
                pixels[at + 2] = ink.b;
                pixels[at + 3] = 255;
            }
        }
    }
    return {16, 16, pixels};
}

/// Badges and tags: the eight tag icons, a picture, letters and a blank
/// badge, a pill and a nav entry with a tag, a drag handle in each state,
/// and a question listing two made-up mutators.
int32_t badges_section(Builder& b, Point at, int32_t width, std::string_view /*word*/) {
    constexpr int32_t side = 20;
    constexpr int32_t gap = 6;
    int32_t x = at.x;
    int32_t y = at.y;
    const auto place_badge = [&](const BadgeLook& look) {
        if (x > at.x && x + side > at.x + width) {
            x = at.x;
            y += side + gap;
        }
        add_badge(b.list, {x, y, side, side}, look);
        x += side + gap;
    };
    for (const TagIcon icon : all_tag_icons) {
        BadgeLook look;
        look.tag = icon;
        look.face = colour::hint;
        place_badge(look);
    }
    BadgeLook picture;
    picture.picture = badge_checker();
    place_badge(picture);
    BadgeLook letters;
    letters.letters = "GA";
    letters.face = colour::hint;
    place_badge(letters);
    place_badge({});
    y += side + gap;

    PillLook pill;
    pill.text = "Economy";
    pill.tag = TagIcon::economy;
    const int32_t pill_across = pill_width(*b.fonts, pill);
    add_pill(b.list, {at.x, y, pill_across, pill_height()}, pill, no_control, {});

    const int32_t nav_x = at.x + pill_across + gap;
    const int32_t nav_across =
        std::min(kNavSampleWidth, std::max(width - (nav_x - at.x), int32_t{0}));
    NavLook nav;
    nav.area = {nav_x, y, nav_across, kNavEntryHeight};
    nav.rule_column = nav_x + nav_across;
    NavEntry entry;
    entry.rect = nav.area;
    entry.caption = "Maps";
    entry.tag = TagIcon::maps;
    nav.entries.push_back(entry);
    Item& nav_part = add_part(b.list, Role::nav, entry.rect);
    nav_part.text = entry.caption;
    nav_part.look = std::move(nav);
    y += std::max(pill_height(), kNavEntryHeight) + gap;

    const GripLook grips[] = {
        {},
        {.hovered = true},
        {.held = true},
        {.disabled = true},
    };
    int32_t grip_x = at.x;
    for (const GripLook& grip : grips) {
        add_grip(b.list, {grip_x, y, grip_width, grip_height}, grip, no_control, {});
        grip_x += grip_width + gap;
    }
    y += grip_height + gap;

    Question question;
    question.title = "GET GATEHOUSE?";
    question.paragraphs = {{"Install these mutators.", false}};
    question.buttons = {{"Cancel", false, "cancel"}, {"Get", true, "get"}};
    question.cancel_button = 0;
    question.primary_button = 1;
    QuestionItem gatehouse;
    gatehouse.badge.tag = TagIcon::economy;
    gatehouse.badge.face = colour::hint;
    gatehouse.text = "Gatehouse 4.1";
    gatehouse.aside = " · from Core Prime";
    QuestionItem keep;
    keep.badge.tag = TagIcon::maps;
    keep.badge.face = colour::hint;
    keep.text = "Gatehouse Keep 2.0";
    question.items = {std::move(gatehouse), std::move(keep)};
    GalleryPanel panel;
    panel.origin = {at.x, y};
    const int32_t height = place_question(question, b.fonts).height;
    panel.place = {at.x, y, notice_width, height};
    panel.list = question_list(question, b.fonts);
    without_input(panel.list);
    b.panels.push_back(std::move(panel));
    return y + height - at.y;
}

/// Keys: a working keycap, every state and tint, a modifier, two small
/// badges, chord chips, the three keyboards, a mouse and hold rings.
///
/// The keyboard's and the mouse's keys are controls so automation can name
/// them, and they are not focusable. Tab through every key would walk the
/// gallery hundreds of times. The working keycap is the section's focus stop.
int32_t keys_section(Builder& b, Point at, int32_t width, std::string_view word) {
    const GalleryModel& model = *b.model;
    int32_t y = at.y;
    const bool over = pointer_on(model, kKeycap);
    KeycapLook working;
    working.label = "A";
    working.sub = "Go";
    working.tint = KeyTint::orders;
    working.letter = "O";
    working.hovered = over;
    working.held = over && model.interaction.pressed == kKeycap;
    working.focused = focus_on(model, kKeycap);
    const int32_t working_across =
        std::min(std::max(keycap_width(*b.fonts, working), int32_t{28}), width);
    const Rect working_rect{at.x, y, working_across, keycap_height};
    const std::size_t part_at = b.list.items.size();
    add_keycap(b.list, working_rect, working, kKeycap, named(word, 1));
    show_working(b, part_at, kKeycap, working_rect);
    b.order.push_back(kKeycap);
    y += keycap_height + kLineGap;

    y += add_states(
             b.list,
             {at.x, y},
             width,
             {28, keycap_height},
             kFiveStates,
             [&](const Rect& rect, Sample shown) {
                 add_fixed(b.list, rect, rect, shown, false, [&] {
                     KeycapLook look;
                     look.label = "A";
                     look.letter = "O";
                     look.tint = KeyTint::orders;
                     look.hovered = looks_hovered(shown);
                     look.held = shown == Sample::pressed;
                     look.focused = shown == Sample::focused;
                     look.disabled = shown == Sample::disabled;
                     add_keycap(b.list, rect, look, no_control, {});
                 });
             }
         ) +
         kLineGap;

    struct TintCap {
        KeyTint tint;
        std::string_view letter;
    };

    constexpr std::array<TintCap, 12> kTints{{
        {KeyTint::orders, "O"},
        {KeyTint::pointer, "P"},
        {KeyTint::selection, "S"},
        {KeyTint::groups, "G"},
        {KeyTint::camera, "C"},
        {KeyTint::building, "B"},
        {KeyTint::game, "M"},
        {KeyTint::chat, "T"},
        {KeyTint::window, "W"},
        {KeyTint::hacks, "L"},
        {KeyTint::mod, "D"},
        {KeyTint::developer, "V"},
    }};
    constexpr std::array<std::string_view, 12> kTintCaptions{
        "O", "P", "S", "G", "C", "B", "M", "T", "W", "L", "D", "V"
    };
    y += add_cells(
             b.list,
             {at.x, y},
             width,
             {28, keycap_height},
             kTintCaptions,
             [&](const Rect& rect, std::size_t place) {
                 KeycapLook look;
                 look.label = "A";
                 look.tint = kTints[place].tint;
                 look.letter = std::string(kTints[place].letter);
                 add_keycap(b.list, rect, look, no_control, {});
             }
         ) +
         kLineGap;

    constexpr std::array<KeyMark, 4> kMarks{
        KeyMark::changed, KeyMark::clash, KeyMark::unbound, KeyMark::fixed
    };
    constexpr std::array<std::string_view, 4> kMarkCaptions{"Changed", "Clash", "Unbound", "Fixed"};
    y += add_cells(
             b.list,
             {at.x, y},
             width,
             {28, keycap_height},
             kMarkCaptions,
             [&](const Rect& rect, std::size_t place) {
                 KeycapLook look;
                 look.label = "A";
                 look.mark = kMarks[place];
                 add_keycap(b.list, rect, look, no_control, {});
             }
         ) +
         kLineGap;

    KeycapLook modifier;
    modifier.label = "Shift";
    modifier.modifier = true;
    const int32_t modifier_across =
        std::min(std::max(keycap_width(*b.fonts, modifier), keycap_least_width), width);
    add_keycap(b.list, {at.x, y, modifier_across, keycap_height}, modifier, no_control, {});
    int32_t badge_x = at.x + modifier_across + kCellGap;
    for (const std::string_view badge : {"A", "B"}) {
        if (badge_x + 9 > at.x + width)
            break;
        KeycapLook small;
        small.label = std::string(badge);
        small.small = true;
        add_keycap(b.list, {badge_x, y, 9, 9}, small, no_control, {});
        badge_x += 9 + kCellGap;
    }
    y += keycap_height + kLineGap;

    const auto place_chord = [&](ChordChipLook look) {
        const int32_t want = chord_chip_width(*b.fonts, look);
        const int32_t across = std::min(std::max(want, keycap_least_width), width);
        add_chord_chip(b.list, {at.x, y, across, keycap_height}, std::move(look), no_control, {});
        y += keycap_height + kLineGap;
    };
    ChordChipLook ctrl_b;
    ctrl_b.keys = {"Ctrl", "B"};
    place_chord(std::move(ctrl_b));
    ChordChipLook ctrl_d;
    ctrl_d.keys = {"Ctrl", "D"};
    ctrl_d.gesture = "hold";
    place_chord(std::move(ctrl_d));
    ChordChipLook mouse5;
    mouse5.keys = {"Mouse 5"};
    mouse5.selected = true;
    place_chord(std::move(mouse5));
    ChordChipLook clash;
    clash.keys = {"Ctrl", "K"};
    clash.mark = KeyMark::clash;
    place_chord(std::move(clash));
    ChordChipLook changed;
    changed.keys = {"Ctrl", "G"};
    changed.mark = KeyMark::changed;
    place_chord(std::move(changed));
    ChordChipLook add;
    add.add = true;
    add.none_text = "Pick a key";
    place_chord(std::move(add));
    ChordChipLook unbound;
    unbound.none_text = "Escape does it as Back";
    place_chord(std::move(unbound));

    ControlId next_id = kKeycap + 1;
    const auto place_keyboard = [&](KeyboardLook look, std::string name) {
        const int32_t across = std::min(width, int32_t{91 * 4});
        const int32_t quarter = std::max(across / 91, int32_t{1});
        const int32_t high = 25 * quarter;
        const std::size_t first_control = b.list.controls.size();
        add_keyboard(b.list, {at.x, y, across, high}, look, next_id, std::move(name));
        next_id += static_cast<ControlId>(keyboard_places(look.shape).size());
        for (std::size_t index = first_control; index < b.list.controls.size(); ++index)
            b.list.controls[index].focusable = false;
        y += high + kLineGap;
    };
    KeyboardLook ansi;
    ansi.shape = KeyboardShape::ansi;
    KeyboardKey orders_key;
    orders_key.code = "KeyA";
    orders_key.cap.tint = KeyTint::orders;
    orders_key.cap.letter = "O";
    orders_key.cap.sub = "Go";
    ansi.keys.push_back(orders_key);
    place_keyboard(ansi, "gallery.keys.ansi");
    KeyboardLook iso;
    iso.shape = KeyboardShape::iso;
    place_keyboard(std::move(iso), "gallery.keys.iso");
    KeyboardLook jis;
    jis.shape = KeyboardShape::jis;
    place_keyboard(std::move(jis), "gallery.keys.jis");

    constexpr int32_t kMouseAcross = 80;
    constexpr int32_t kMouseDown = 128;
    const int32_t mouse_across = std::min(width, kMouseAcross);
    const int32_t mouse_down = mouse_across >= 40 ? kMouseDown : 0;
    if (mouse_down > 0) {
        MouseLook mouse;
        mouse.extra_buttons = true;
        mouse.buttons[static_cast<std::size_t>(MouseButton::left)].tint = KeyTint::pointer;
        mouse.buttons[static_cast<std::size_t>(MouseButton::left)].letter = "P";
        const std::size_t first_control = b.list.controls.size();
        add_mouse(
            b.list, {at.x, y, mouse_across, mouse_down}, mouse, next_id, "gallery.keys.mouse"
        );
        for (std::size_t index = first_control; index < b.list.controls.size(); ++index)
            b.list.controls[index].focusable = false;
        y += mouse_down + kLineGap;
    }

    constexpr std::array<HoldRingLook, 8> kRings{{
        {0, 3, false},
        {1, 3, false},
        {2, 3, false},
        {3, 3, false},
        {0, 3, true},
        {1, 3, true},
        {2, 3, true},
        {3, 3, true},
    }};
    constexpr int32_t kRingSide = 32;
    int32_t ring_x = at.x;
    int32_t ring_y = y;
    int32_t ring_bottom = y;
    for (const HoldRingLook& ring : kRings) {
        if (ring_x > at.x && ring_x + kRingSide > at.x + width) {
            ring_x = at.x;
            ring_y += kRingSide + kCellGap;
        }
        add_hold_ring(b.list, {ring_x, ring_y, kRingSide, kRingSide}, ring);
        ring_x += kRingSide + kCellGap;
        ring_bottom = ring_y + kRingSide;
    }
    return ring_bottom - at.y;
}

/// One section of the gallery.
struct Section {
    std::string_view heading{}; ///< its heading, in capitals
    std::string_view word{};    ///< its word in its samples' names
    /// Adds its samples from a top left corner across a width and returns
    /// their height.
    int32_t (*add)(Builder&, Point, int32_t, std::string_view){};
};

/// The sections, in the order they show.
constexpr auto kSections = std::to_array<Section>({
    {"FIRST RUN", "first-run", &first_run},
    {"WINDOW CHROME", "window-chrome", &window_chrome},
    {"TABS", "tabs", &tabs},
    {"BUTTONS", "buttons", &buttons_section},
    {"TOGGLE", "toggle", &toggle_section},
    {"LEVEL STRIP", "level-strip", &level_strip},
    {"DROP-DOWN", "drop-down", &drop_down},
    {"SLIDER", "slider", &slider_section},
    {"SEARCH FIELD", "search-field", &search_field},
    {"CHIPS", "chips", &chips_section},
    {"PILLS", "pills", &pills_section},
    {"PROGRESS", "progress", &progress_section},
    {"NAV LIST", "nav-list", &nav_list},
    {"LIST ROWS", "list-rows", &list_rows},
    {"SCROLL LIST", "scroll-list", &scroll_list},
    {"CARD GRID", "card-grid", &card_grid},
    {"HOVER CARD", "hover-card", &hover_card},
    {"SECTION, LABEL AND HINT", "section", &section_label_hint},
    {"LOCK", "lock", &lock_section},
    {"NOTICE", "notice", &notice_section},
    {"QUESTION", "question", &question_section},
    {"OA MARK", "oa-mark", &oa_mark},
    {"BADGES AND TAGS", "badges", &badges_section},
    {"KEYS", "keys", &keys_section},
});

/// Adds a section: its heading, the rule under it and its samples.
///
/// @param[in,out] b what is built
/// @param section the section
/// @param at its top left corner, in points
/// @param width its width
/// @return its height
int32_t add_section(Builder& b, const Section& section, Point at, int32_t width) {
    Item& heading = add_part(b.list, Role::heading, {at.x, at.y, width, kHeadingHeight});
    heading.text = std::string(section.heading);
    heading.look = HeadingLook{std::string(section.heading)};
    add_part(b.list, Role::rule, {at.x, at.y + kHeadingHeight, width, 1}).colour = colour::rule;
    const int32_t top = at.y + kHeadingHeight + 1 + kAfterRule;
    return top - at.y + section.add(b, {at.x, top}, width, section.word);
}

/// Returns the columns the sections flow in at a size class.
///
/// @param size_class the class
/// @return 1, 2 or 3
std::size_t columns_of(SizeClass size_class) noexcept {
    switch (size_class) {
    case SizeClass::regular:
        return 2;
    case SizeClass::large:
        return 3;
    case SizeClass::compact:
        break;
    }
    return 1;
}

/// Returns the size class's name and the scale, as the header's note shows them.
///
/// @param frame the frame
/// @return such as "Large · 1x" or "Compact · 1.5x"
std::string scale_note(const Frame& frame) {
    std::string note;
    switch (frame.size_class) {
    case SizeClass::compact:
        note = "Compact";
        break;
    case SizeClass::regular:
        note = "Regular";
        break;
    case SizeClass::large:
        note = "Large";
        break;
    }
    const int32_t percent = std::max(frame.scale_percent, int32_t{1});
    note += " · " + std::to_string(percent / 100);
    if (percent % 100 != 0) {
        std::string fraction = std::to_string(percent % 100 + 100).substr(1);
        while (!fraction.empty() && fraction.back() == '0')
            fraction.pop_back();
        note += "." + fraction;
    }
    return note + "x";
}

// ---------------------------------------------------------------------------
// Events.

/// Returns a working control's index in a run of controls from a first.
///
/// @param id the control
/// @param first the run's first control
/// @param count the run's controls
/// @return its index; none when it is not in the run
std::optional<std::size_t> in_run(ControlId id, ControlId first, std::size_t count) noexcept {
    if (id < first || id >= first + static_cast<ControlId>(count))
        return std::nullopt;
    return static_cast<std::size_t>(id - first);
}

/// Moves the working slider's knob to the stop nearest a column.
///
/// @param[in,out] model the model
/// @param laid the gallery as laid out from it
/// @param column the column, in points
/// @return true when the stop changed
bool slide_to(GalleryModel& model, const GalleryLayout& laid, int32_t column) {
    const Control* control = control_of(laid.list, kSlider);
    if (control == nullptr)
        return false;
    const int32_t stop = stop_at(control->rect, column, gallery_slider_stops);
    if (stop == model.slider_stop)
        return false;
    model.slider_stop = stop;
    return true;
}

/// Presses a working control at a point: what a release over it, Space or
/// Enter asks.
///
/// @param[in,out] model the model
/// @param laid the gallery as laid out from it
/// @param id the control
/// @param at where the press lands, in points
/// @return what the host does
GalleryResult press_at(GalleryModel& model, const GalleryLayout& laid, ControlId id, Point at) {
    const Control* control = control_of(laid.list, id);
    if (control == nullptr)
        return GalleryResult::none;
    if (id == kClose)
        return GalleryResult::close;
    if (const auto tab = in_run(id, kFirstTab, kTabCount)) {
        model.tab = *tab;
    } else if (id == kButton || id == kLink || id == kOaButton) {
        ++model.presses;
    } else if (id == kToggle) {
        // The half it lands on: Off on the left, On on the right.
        const int32_t half = (control->rect.width - 2) / 2;
        model.toggle_on = at.x >= control->rect.x + 1 + half;
    } else if (id == kLevels) {
        model.level = level_at(control->rect, kLevelCount, kLevelWidth, at.x);
    } else if (id == kChoice) {
        model.menu_open = true;
        model.menu_marked = static_cast<int32_t>(model.language);
    } else if (id == kSlider) {
        slide_to(model, laid, at.x);
    } else if (id == kSearch) {
        model.interaction.focused = kSearch;
        model.interaction.focus_shown = true;
    } else if (const auto chip = in_run(id, kFirstChip, kChipCount)) {
        model.chips[*chip] = !model.chips[*chip];
    } else if (const auto entry = in_run(id, kFirstNav, kNavCount)) {
        model.nav = *entry;
    } else if (const auto row_at = in_run(id, kFirstRow, kRowCount)) {
        model.row = *row_at;
    } else if (const auto list_at = in_run(id, kFirstListRow, kListRowCount)) {
        model.list_row = *list_at;
    } else if (const auto card = in_run(id, kFirstCard, kCardCount)) {
        model.card = *card;
    } else if (const auto carousel_card = in_run(id, kFirstCarousel, kCarouselCount)) {
        model.carousel_index = static_cast<int32_t>(*carousel_card);
    } else if (id == kCarouselPrevious) {
        model.carousel_index = std::max(model.carousel_index - 1, int32_t{0});
    } else if (id == kCarouselNext) {
        model.carousel_index =
            std::min(model.carousel_index + 1, static_cast<int32_t>(kCarouselCount) - 1);
    } else if (const auto choice = in_run(id, kFirstChoice, kChoiceCount)) {
        model.choice_index = *choice;
    }
    return GalleryResult::redraw;
}

/// Returns a control's middle.
///
/// @param control the control
/// @return the point
Point middle_of(const Control& control) noexcept {
    return {control.rect.x + control.rect.width / 2, control.rect.y + control.rect.height / 2};
}

/// Scrolls the sections, and the scroll list for one of its rows, until the
/// focused control and its focus ring show.
///
/// @param[in,out] model the model
/// @param frame the frame
/// @param fonts the fonts
void show_focus(GalleryModel& model, const Frame& frame, const Fonts& fonts) {
    const ControlId id = model.interaction.focused;
    {
        const GalleryLayout laid = gallery_layout(model, frame, fonts);
        const Control* control = control_of(laid.list, id);
        if (control == nullptr)
            return;
        if (control->group == kRowsGroup) {
            model.list_scroll = laid.rows.offset_showing(
                control->rect.y + laid.rows.offset,
                control->rect.height,
                id + 1 == kFirstListRow + static_cast<ControlId>(kListRowCount)
            );
        }
    }
    const GalleryLayout laid = gallery_layout(model, frame, fonts);
    const Control* control = control_of(laid.list, id);
    if (control == nullptr)
        return;
    const Rect ring = grown(control->rect, kMetrics.focus_inset);
    model.scroll = laid.sections.offset_showing(ring.y + laid.sections.offset, ring.height, false);
}

/// Takes a key the focused working control takes.
///
/// @param[in,out] model the model
/// @param laid the gallery as laid out from it
/// @param id the control
/// @param pressed the key
/// @return what the host does
GalleryResult key_on(GalleryModel& model, const GalleryLayout& laid, ControlId id, Key pressed) {
    if (id == kSearch)
        return edit_text(model.search, pressed) ? GalleryResult::redraw : GalleryResult::none;
    if (pressed == Key::left || pressed == Key::right) {
        const int32_t by = pressed == Key::right ? 1 : -1;
        if (id == kToggle) {
            const bool on = pressed == Key::right;
            if (model.toggle_on == on)
                return GalleryResult::none;
            model.toggle_on = on;
        } else if (id == kLevels) {
            const auto level = static_cast<int32_t>(model.level) + by;
            if (level < 0 || level >= static_cast<int32_t>(kLevelCount))
                return GalleryResult::none;
            model.level = static_cast<std::size_t>(level);
        } else if (id == kSlider) {
            const int32_t stop =
                std::clamp(model.slider_stop + by, int32_t{0}, gallery_slider_stops - 1);
            if (stop == model.slider_stop)
                return GalleryResult::none;
            model.slider_stop = stop;
        } else if (id == kChoice) {
            const auto language = static_cast<int32_t>(model.language) + by;
            if (language < 0 || language >= static_cast<int32_t>(gallery_languages.size()))
                return GalleryResult::none;
            model.language = static_cast<std::size_t>(language);
        } else if (
            id == kCarouselPrevious || id == kCarouselNext ||
            in_run(id, kFirstCarousel, kCarouselCount)
        ) {
            const int32_t next_index = std::clamp(
                model.carousel_index + by, int32_t{0}, static_cast<int32_t>(kCarouselCount) - 1
            );
            if (next_index == model.carousel_index)
                return GalleryResult::none;
            model.carousel_index = next_index;
        } else {
            return GalleryResult::none;
        }
        return GalleryResult::redraw;
    }
    if (id == kToggle) {
        model.toggle_on = !model.toggle_on;
        return GalleryResult::redraw;
    }
    if (id == kLevels) {
        model.level = (model.level + 1) % kLevelCount;
        return GalleryResult::redraw;
    }
    if (id == kSlider)
        return GalleryResult::none;
    const Control* control = control_of(laid.list, id);
    if (control == nullptr)
        return GalleryResult::none;
    return press_at(model, laid, id, middle_of(*control));
}

/// Takes a key while the working drop-down's menu is open.
///
/// @param[in,out] model the model
/// @param laid the gallery as laid out from it
/// @param pressed the key
/// @return what the host does; none for a key the menu leaves to the gallery
std::optional<GalleryResult> menu_key(GalleryModel& model, const GalleryLayout& laid, Key pressed) {
    const auto total = static_cast<int32_t>(gallery_languages.size());
    const int32_t page = shown_choices(gallery_languages.size());
    switch (pressed) {
    case Key::escape:
        model.menu_open = false;
        return GalleryResult::redraw;
    case Key::up:
    case Key::down:
        model.menu_marked =
            choice_mark_toward(laid.menu_look, model.menu_marked, pressed == Key::down ? 1 : -1);
        return GalleryResult::redraw;
    case Key::page_up:
    case Key::page_down:
        model.menu_marked = choice_mark_toward(
            laid.menu_look, model.menu_marked, pressed == Key::page_down ? page : -page
        );
        return GalleryResult::redraw;
    case Key::home:
        model.menu_marked = 0;
        return GalleryResult::redraw;
    case Key::end:
        model.menu_marked = total - 1;
        return GalleryResult::redraw;
    case Key::enter:
    case Key::space:
        model.language =
            static_cast<std::size_t>(std::clamp(model.menu_marked, int32_t{0}, total - 1));
        model.menu_open = false;
        return GalleryResult::redraw;
    default:
        // Any other key closes the menu and then does what it does.
        model.menu_open = false;
        return std::nullopt;
    }
}

/// Returns the gallery laid out whole: its frame as tall as shows every
/// section unscrolled, at the same size class, so that the arrows judge where
/// a control lies whether the sections' view shows it or not, and the footer
/// lies under every section.
///
/// @param model the model
/// @param frame the frame
/// @param fonts the fonts
/// @param laid the gallery as laid out for the frame
/// @return the layout
GalleryLayout laid_whole(
    const GalleryModel& model, const Frame& frame, const Fonts& fonts, const GalleryLayout& laid
) {
    Frame tall = frame;
    tall.area.height +=
        std::max(laid.sections.content_height - laid.sections.view.height, int32_t{0});
    GalleryModel unscrolled = model;
    unscrolled.scroll = 0;
    return gallery_layout(unscrolled, tall, fonts);
}

/// Takes a key.
///
/// @param[in,out] model the model
/// @param frame the frame
/// @param fonts the fonts
/// @param laid the gallery as laid out from the model
/// @param pressed the key
/// @return what the host does
GalleryResult take_key(
    GalleryModel& model,
    const Frame& frame,
    const Fonts& fonts,
    const GalleryLayout& laid,
    Key pressed
) {
    bool closed_menu = false;
    if (model.menu_open) {
        if (const auto taken = menu_key(model, laid, pressed))
            return *taken;
        closed_menu = true;
    }
    // The focus moves among the controls of the gallery laid out whole.
    const GalleryLayout whole = laid_whole(model, frame, fonts, laid);
    const KeyOutcome outcome = key(model.interaction, whole.list, pressed);
    switch (outcome.result) {
    case KeyResult::redraw:
        show_focus(model, frame, fonts);
        return GalleryResult::redraw;
    case KeyResult::to_control: {
        const GalleryResult result = key_on(model, laid, outcome.control, pressed);
        return closed_menu && result == GalleryResult::none ? GalleryResult::redraw : result;
    }
    case KeyResult::scroll: {
        const ScrollArea& area = laid.sections;
        int32_t offset = area.offset;
        if (pressed == Key::page_up)
            offset -= area.page_step;
        else if (pressed == Key::page_down)
            offset += area.page_step;
        else if (pressed == Key::home)
            offset = 0;
        else if (pressed == Key::end)
            offset = area.limit;
        offset = std::clamp(offset, int32_t{0}, area.limit);
        if (offset == area.offset)
            return closed_menu ? GalleryResult::redraw : GalleryResult::none;
        model.scroll = offset;
        return GalleryResult::redraw;
    }
    case KeyResult::cancel:
        return GalleryResult::close;
    case KeyResult::accept:
    case KeyResult::none:
        break;
    }
    return closed_menu ? GalleryResult::redraw : GalleryResult::none;
}

} // namespace

GalleryLayout gallery_layout(const GalleryModel& model, const Frame& frame, const Fonts& fonts) {
    const int32_t width = std::max(frame.area.width, int32_t{1});
    const int32_t height = std::max(frame.area.height, int32_t{1});
    const Metrics& sized = metrics_of(frame.size_class);
    const int32_t pad = sized.padding;
    const int32_t header_bottom = kMetrics.edge + kMetrics.header_height + 1;
    const int32_t footer_rule = height - kMetrics.edge - kMetrics.footer_height - 1;

    GalleryLayout laid;
    laid.columns = columns_of(frame.size_class);
    Builder b;
    b.model = &model;
    b.fonts = &fonts;
    b.metrics = &sized;
    b.view = {kMetrics.edge, header_bottom, width - 2 * kMetrics.edge, footer_rule - header_bottom};
    add_part(b.list, Role::fill, {0, 0, width, height}).colour = colour::panel;

    const std::size_t first_part = b.list.items.size();
    // Compact's choice-card states hang into the footer so the cards keep
    // their real height. They are drawn after the footer, still in the frame.
    std::vector<Item> over_footer;
    const bool roomy = b.view.width > 0 && b.view.height > 0;
    if (roomy) {
        const auto columns = static_cast<int32_t>(laid.columns);
        const int32_t content_width = width - 2 * pad;
        const int32_t column_width = std::max((content_width - (columns - 1) * pad) / columns, 1);
        // First run spans the content width. The other sections flow into the
        // shortest column under it.
        std::array<Point, kSections.size()> places{};
        std::array<int32_t, kSections.size()> spans{};
        Builder measured;
        measured.model = &model;
        measured.fonts = &fonts;
        measured.view = b.view;
        measured.metrics = &sized;
        const int32_t first_height = add_section(measured, kSections[0], {0, 0}, content_width);
        places[0] = {pad, b.view.y + kFirstRunTop};
        spans[0] = content_width;
        std::vector<int32_t> bottoms(laid.columns, places[0].y + first_height + kSectionGap);
        for (std::size_t index = 1; index < kSections.size(); ++index) {
            Builder measure;
            measure.model = &model;
            measure.fonts = &fonts;
            measure.view = b.view;
            measure.metrics = &sized;
            const int32_t section_height =
                add_section(measure, kSections[index], {0, 0}, column_width);
            const auto shortest = static_cast<std::size_t>(
                std::min_element(bottoms.begin(), bottoms.end()) - bottoms.begin()
            );
            places[index] = {
                pad + static_cast<int32_t>(shortest) * (column_width + pad), bottoms[shortest]
            };
            spans[index] = column_width;
            bottoms[shortest] += section_height + kSectionGap;
        }
        const int32_t content_bottom =
            *std::max_element(bottoms.begin(), bottoms.end()) - kSectionGap + kContentBottom;
        ScrollArea& area = laid.sections;
        area.view = b.view;
        area.well = {
            width - pad + kMetrics.focus_inset + 1,
            b.view.y + kMetrics.focus_inset,
            kMetrics.scroll_thumb_width + 2,
            std::max(b.view.height - 2 * kMetrics.focus_inset, 0)
        };
        area.hit = {width - pad, b.view.y, pad - kMetrics.edge, b.view.height};
        area.page_step = std::max(b.view.height - 3 * kMetrics.small_line, kMetrics.small_line);
        area.content_height = content_bottom - b.view.y;
        area.limit = ScrollArea::limit_for(area.content_height, b.view.height);
        area.offset = std::clamp(model.scroll, int32_t{0}, area.limit);
        for (std::size_t index = 0; index < kSections.size(); ++index)
            add_section(
                b, kSections[index], {places[index].x, places[index].y - area.offset}, spans[index]
            );
        if (b.menu_part)
            b.list.items.push_back(std::move(*b.menu_part));
        // Every part and control of the sections is clipped to the view; the
        // scroll list's rows to its own view within it. Rows whose view the
        // sections' view shows none of are not drawn and take no press.
        const Rect below_header{
            b.view.x, b.view.y, b.view.width, std::max(height - b.view.y, int32_t{0})
        };
        std::vector<Item> kept;
        kept.reserve(b.list.items.size());
        for (std::size_t index = 0; index < b.list.items.size(); ++index) {
            Item& part = b.list.items[index];
            if (index >= first_part) {
                const bool own = part.clip.width > 0 && part.clip.height > 0;
                if (own) {
                    part.clip = intersect(part.clip, b.view);
                    if (part.clip.width <= 0 || part.clip.height <= 0)
                        continue;
                } else if (wholly_in(part.rect, b.view)) {
                    part.clip = b.view;
                } else if (wholly_in(part.rect, below_header)) {
                    part.clip = below_header;
                    over_footer.push_back(std::move(part));
                    continue;
                } else {
                    part.clip = b.view;
                    if (part.clip.width <= 0 || part.clip.height <= 0)
                        continue;
                }
            }
            kept.push_back(std::move(part));
        }
        b.list.items = std::move(kept);
        for (Control& control : b.list.controls) {
            const bool own = control.clip.width > 0 && control.clip.height > 0;
            if (own)
                control.clip = intersect(control.clip, b.view);
            else if (!wholly_in(control.rect, b.view) && wholly_in(control.rect, below_header))
                control.clip = below_header;
            else
                control.clip = b.view;
            if (control.clip.width <= 0 || control.clip.height <= 0)
                control.enabled = false;
        }
        if (area.limit > 0) {
            Item& bar = add_part(b.list, Role::scroll_bar, area.well);
            bar.look = ScrollBarLook{area.thumb(kMetrics.least_thumb_height), false};
        }
        laid.rows = b.rows;
        laid.menu = b.menu;
        laid.menu_look = b.menu_look;
        for (GalleryPanel& panel : b.panels)
            panel.clip = intersect(panel.place, b.view);
        laid.panels = std::move(b.panels);
    }

    // The window's header, footer band, CLOSE and edge.
    HeaderLook header;
    header.width = width;
    header.title = std::string(kTitle);
    header.tracking = kMetrics.heading_tracking;
    header.title_width = tracked_width(fonts, FontRole::regular, kTitle, header.tracking);
    header.second = std::string(kSecond);
    header.second_width = tracked_width(fonts, FontRole::regular, kSecond, header.tracking);
    header.note = scale_note(frame);
    Item& header_part = add_part(b.list, Role::header, {0, 0, width, header_bottom});
    header_part.text = header.title;
    header_part.look = std::move(header);
    add_part(b.list, Role::footer_band, {0, footer_rule, width, kMetrics.footer_height + 1}).look =
        FooterBandLook{width, footer_rule};
    const Rect close{
        width - pad - kButtonWidth,
        footer_rule + 1 + (kMetrics.footer_height - kMetrics.button_height) / 2,
        kButtonWidth,
        kMetrics.button_height
    };
    const bool over_close = model.interaction.hovered == kClose;
    const std::size_t close_at = b.list.items.size();
    add_button(
        b.list,
        close,
        button_look(
            kCloseCaption,
            ButtonStyle::accent,
            over_close,
            over_close && model.interaction.pressed == kClose
        ),
        kClose,
        "gallery.close"
    );
    show_working(b, close_at, kClose, close);
    b.order.push_back(kClose);
    for (Item& part : over_footer)
        b.list.items.push_back(std::move(part));
    add_part(b.list, Role::bevel, {0, 0, width, height}).colour = colour::edge_light;

    b.list.tab_order = std::move(b.order);
    laid.list = std::move(b.list);
    return laid;
}

void draw_gallery(const Canvas& canvas, const GalleryLayout& layout) {
    paint(canvas, layout.list);
    for (const GalleryPanel& panel : layout.panels) {
        if (panel.clip.width <= 0 || panel.clip.height <= 0)
            continue;
        paint(placed_at(clipped(canvas, panel.clip), panel.origin), panel.list);
    }
}

GalleryResult gallery_act(
    GalleryModel& model, const Frame& frame, const Fonts& fonts, const GalleryEvent& event
) {
    const GalleryLayout laid = gallery_layout(model, frame, fonts);
    Interaction& seen = model.interaction;
    switch (event.kind) {
    case GalleryEventKind::pointer_move: {
        if (model.menu_open && contains(laid.menu, event.at)) {
            bool changed = seen.hovered != no_control;
            seen.hovered = no_control;
            const int32_t place = choice_item_at(laid.menu, event.at.x, event.at.y);
            if (place >= 0 && place != model.menu_marked) {
                model.menu_marked = place;
                changed = true;
            }
            return changed ? GalleryResult::redraw : GalleryResult::none;
        }
        const ControlId held = seen.pressed;
        const PointerOutcome outcome = pointer_move(seen, laid.list, event.at);
        bool changed = outcome.result != PointerResult::none;
        // A held slider follows the pointer.
        if (held == kSlider)
            changed = slide_to(model, laid, outcome.at.x) || changed;
        return changed ? GalleryResult::redraw : GalleryResult::none;
    }
    case GalleryEventKind::pointer_down: {
        if (model.menu_open) {
            const int32_t place = contains(laid.menu, event.at)
                                      ? choice_item_at(laid.menu, event.at.x, event.at.y)
                                      : -1;
            if (place >= 0) {
                model.menu_held = true;
                model.menu_marked = place;
                return GalleryResult::redraw;
            }
            // A press anywhere else only closes the menu.
            model.menu_open = false;
            model.menu_held = false;
            return GalleryResult::redraw;
        }
        const PointerOutcome outcome = event.reach > 0
                                           ? finger_down(seen, laid.list, event.at, event.reach)
                                           : pointer_down(seen, laid.list, event.at);
        if (outcome.control == kSlider)
            slide_to(model, laid, outcome.at.x);
        if (outcome.control == kSearch) {
            seen.focused = kSearch;
            seen.focus_shown = true;
        }
        return outcome.result == PointerResult::none ? GalleryResult::none : GalleryResult::redraw;
    }
    case GalleryEventKind::pointer_up: {
        if (model.menu_held) {
            model.menu_held = false;
            const int32_t place = model.menu_open && contains(laid.menu, event.at)
                                      ? choice_item_at(laid.menu, event.at.x, event.at.y)
                                      : -1;
            if (place >= 0) {
                model.language = static_cast<std::size_t>(laid.menu_look.first + place);
                model.menu_open = false;
            }
            return GalleryResult::redraw;
        }
        const PointerOutcome outcome = pointer_up(seen, laid.list, event.at);
        if (outcome.result == PointerResult::activated)
            return press_at(model, laid, outcome.control, outcome.at);
        return outcome.result == PointerResult::none ? GalleryResult::none : GalleryResult::redraw;
    }
    case GalleryEventKind::key:
        return take_key(model, frame, fonts, laid, event.key);
    case GalleryEventKind::text:
        if (seen.focus_shown && seen.focused == kSearch && insert_text(model.search, event.text))
            return GalleryResult::redraw;
        return GalleryResult::none;
    case GalleryEventKind::wheel: {
        const Rect list_view = intersect(laid.rows.view, laid.sections.view);
        if (laid.rows.limit > 0 && contains(list_view, event.at)) {
            const int32_t offset = wheel_offset(
                model.list_wheel, event.notches, kWheelStep, laid.rows.offset, laid.rows.limit
            );
            if (offset == laid.rows.offset)
                return GalleryResult::none;
            model.list_scroll = offset;
            return GalleryResult::redraw;
        }
        const int32_t offset = wheel_offset(
            model.wheel, event.notches, kWheelStep, laid.sections.offset, laid.sections.limit
        );
        if (offset == laid.sections.offset)
            return GalleryResult::none;
        model.scroll = offset;
        return GalleryResult::redraw;
    }
    }
    return GalleryResult::none;
}

std::optional<Rect> gallery_text_field(const GalleryModel& model, const GalleryLayout& layout) {
    if (!focus_on(model, kSearch))
        return std::nullopt;
    const Control* control = control_of(layout.list, kSearch);
    if (control == nullptr || !control->enabled)
        return std::nullopt;
    return intersect(control->rect, control->clip);
}

} // namespace oa::ui::kit
