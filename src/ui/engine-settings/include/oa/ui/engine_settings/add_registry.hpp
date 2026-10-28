// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Add registry window: a kit window in a notice's chrome that shows what
// the player is about to trust before they trust it. A registry is added by
// its address, from an .oareg file, or as a mirror of one the player added;
// all three lead to the same window. The host checks the registry and makes
// the change; the window holds plain strings and only says which press asked
// for what. Nothing here reads a registry, a file or the network.
#pragma once

#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/text.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace oa::ui::kit {
struct Canvas;
}

namespace oa::ui::engine_settings {

/// The address field, named add-registry.url.
inline constexpr oa::ui::kit::ControlId add_registry_url_control = 1;
/// CHECK, which asks the host to check the address typed, named add-registry.check.
inline constexpr oa::ui::kit::ControlId add_registry_check_control = 2;
/// ADD, or ADD MIRROR for a mirror, named add-registry.add.
inline constexpr oa::ui::kit::ControlId add_registry_add_control = 3;
/// CANCEL, named add-registry.cancel.
inline constexpr oa::ui::kit::ControlId add_registry_cancel_control = 4;
/// The registry's name: a label (an area neither enabled nor focusable),
/// named add-registry.name.
inline constexpr oa::ui::kit::ControlId add_registry_name_control = 5;
/// The address line: a label named add-registry.address.
inline constexpr oa::ui::kit::ControlId add_registry_address_control = 6;
/// The key lines, or the line an unsigned registry shows in their place: a
/// label named add-registry.key.
inline constexpr oa::ui::kit::ControlId add_registry_key_control = 7;
/// The refusal: a label named add-registry.refusal.
inline constexpr oa::ui::kit::ControlId add_registry_refusal_control = 8;
/// The signing-key field, under the address, for an address add. Named
/// add-registry.key-field. An .oareg file and a mirror show no field.
inline constexpr oa::ui::kit::ControlId add_registry_key_field_control = 9;

/// The most key fingerprints the window lists; the rest are counted.
inline constexpr std::size_t shown_fingerprints = 3;

/// What the window shows of a check: the plain strings the host makes from
/// the content service's answer.
struct ProbeView {
    std::string name;       ///< the registry's name; empty when no descriptor was read
    std::string address;    ///< the address the player is shown: a host, and a port that is not 80
    std::size_t packages{}; ///< the packages its catalogue lists
    /// The kinds of package the catalogue holds, each as the window names
    /// it: "mods", "maps", "languages", "expansions", "mutators" or "brains".
    std::vector<std::string> kinds;
    /// The keys' fingerprints, as a player compares them. For a mirror, the
    /// keys the player already trusts for the registry.
    std::vector<std::string> fingerprints;
    /// How packages are downloaded: "direct", "with download keys", or
    /// "with download keys · needs an install ID".
    std::string downloads;
    bool is_signed{true}; ///< false for a registry that names no key
    std::string refusal;  ///< why it cannot be added, one sentence; empty when it can
    /// The registry's icon, once the picture cache has handed it over.
    /// Empty while it is missing or on its way. A display list's picture
    /// points into these pixels, which this view keeps.
    std::vector<uint8_t> icon_rgba{};
    uint32_t icon_width{};  ///< in pixels; 0 when there is no icon yet
    uint32_t icon_height{}; ///< in pixels; 0 when there is no icon yet
    /// The picture's address, resolved against the catalogue that was read.
    /// Empty when the catalogue names no icon. Never the logo the game ships.
    std::string icon_url{};
    std::string icon_registry{}; ///< the registry id the cache is asked under
};

/// One Add registry window, as the host last told it and the player left it.
struct AddRegistry {
    /// How the registry came to the window.
    enum class Mode : uint8_t {
        by_url,    ///< the player types its address
        from_file, ///< an .oareg file the game was opened with
        mirror,    ///< the player types a mirror's address for a registry they added
    };
    /// Where the window stands.
    enum class Phase : uint8_t {
        editing,  ///< the address field takes typing; CHECK or Enter checks it
        checking, ///< the host is checking; only CANCEL
        ready,    ///< the check holds: what would be trusted, and ADD
        refused,  ///< the check or the change was refused, and why
    };

    Mode mode{Mode::by_url}; ///< how the registry came
    std::string url;         ///< the address field's text: the player's own, never looked up
    std::size_t caret{};     ///< the caret, in bytes from the address field's start
    /// The signing key the player typed, for an address add. Empty keeps the
    /// descriptor's keys. A full public key is `ed25519:` and 43 base64
    /// characters and `=`.
    std::string signing_key;
    std::size_t key_caret{};     ///< the caret, in bytes from the signing-key field's start
    std::string file_name;       ///< the .oareg file's name, for Mode::from_file
    std::string mirror_of;       ///< the name of the registry a mirror is for, for Mode::mirror
    Phase phase{Phase::editing}; ///< where it stands
    ProbeView shown;             ///< what the check found, and why it refused
    /// The address the host is checking, which "Checking {address}…" names.
    std::string checking;
    /// The size class it is laid out at. Its width and its height limits are
    /// that class's notice width and heights.
    oa::ui::kit::SizeClass size_class{oa::ui::kit::SizeClass::compact};
    oa::ui::kit::Interaction interaction{}; ///< the hover, the press and the focus
};

/// Which label a line of the window belongs to. Automation reads each
/// label's lines as one.
enum class AddRegistryLabel : uint8_t {
    none,    ///< no label
    name,    ///< add-registry.name
    address, ///< add-registry.address
    key,     ///< add-registry.key
    refusal, ///< add-registry.refusal
};

/// What a line of the window is coloured as.
enum class AddRegistryInk : uint8_t {
    text,    ///< plain text
    hint,    ///< a fact's label, a field's caption
    warning, ///< what to heed: amber
};

/// The room left above a line of the window.
enum class AddRegistryGap : uint8_t {
    none,      ///< none
    name,      ///< the little room under the registry's name
    paragraph, ///< a paragraph's room
};

/// One line of what the window's phase shows, before it is placed.
struct AddRegistryLine {
    /// How the line is laid out.
    enum class Kind : uint8_t {
        heading,   ///< the registry's name, its icon before it while there is one
        line,      ///< one line, cut short where it is too long
        paragraph, ///< wrapped onto as many lines as it takes
        fact,      ///< its label at the left and its value after it
    };
    Kind kind{Kind::line};                          ///< how it is laid out
    std::string label{};                            ///< a fact's label; empty for a fact's further line
    std::string text{};                             ///< its words, or a fact's value
    AddRegistryInk ink{AddRegistryInk::text};       ///< its colour
    AddRegistryLabel belongs{AddRegistryLabel::none}; ///< the label it belongs to
    AddRegistryGap gap{AddRegistryGap::none};       ///< the room above it
};

/// One button of the window's footer, before it is placed.
struct AddRegistryButton {
    oa::ui::kit::ControlId id{oa::ui::kit::no_control}; ///< its number
    std::string caption{};                              ///< its words
    std::string name{};                                 ///< its automation name
    bool accent{};                                      ///< drawn as the accent button
};

/// What the window shows, in the language shown, before any look places it.
struct AddRegistryContent {
    std::string title{};               ///< "Add registry", or "Add mirror" for a mirror
    std::string heading{};             ///< "Mirror of {name}" for a mirror; empty otherwise
    std::string file{};                ///< "From {file}" for a file; empty otherwise
    bool address_field{};              ///< the address field shows: an address or a mirror
    std::string address_placeholder{}; ///< what the empty address field shows
    bool key_field{};                  ///< the signing-key field shows: an address add
    std::string key_label{};           ///< the line over the signing-key field
    std::string key_placeholder{};     ///< what the empty signing-key field shows
    std::vector<AddRegistryLine> lines{};     ///< what its phase shows, top to bottom
    std::vector<AddRegistryButton> buttons{}; ///< the footer's buttons, left to right
};

/// Returns what the window shows, as add_registry_list tells, for a look to
/// place: its title, the mirror's heading or the file's line, its fields,
/// its phase's lines and its buttons.
///
/// @param window the window
/// @return what it shows
[[nodiscard]] AddRegistryContent add_registry_content(const AddRegistry& window);

/// What an event on the window asks of its host.
enum class AddRegistryAction : uint8_t {
    none,   ///< nothing
    redraw, ///< only the look changed: a hover, a press, the focus, the field
    /// CHECK or Enter: check AddRegistry::url (AddRegistry::checking says
    /// nothing yet; the host sets it)
    check,
    add,    ///< ADD or ADD MIRROR: make the change the check allowed
    cancel, ///< CANCEL or Escape: close the window and drop any check
    /// Space on the field, a pad's A, or a press on it: start the system's
    /// text input over the field, so that an on-screen keyboard appears
    start_typing,
};

/// Returns the window's width at a size class: that class's notice width.
///
/// @param size_class the class
/// @return the width, in points
[[nodiscard]] int32_t add_registry_width(oa::ui::kit::SizeClass size_class) noexcept;

/// Returns the window's height: its header, what its phase shows and its
/// footer, from its class's least notice height to its greatest.
///
/// @param window the window
/// @param fonts the fonts the text is measured in; null estimates each character
/// @return the height, in points
[[nodiscard]] int32_t
add_registry_height(const AddRegistry& window, const oa::ui::kit::Fonts* fonts = nullptr);

/// Returns what the window draws and where its controls are.
///
/// The header's title is "Add registry", or "Add mirror" for a mirror, whose
/// heading reads "Mirror of {name}". The address field (add-registry.url),
/// with "http://…" while it is empty, shows for an address or a mirror and
/// takes typing while editing or refused; a file shows "From {file}" in its
/// place. An address add also shows a "Signing key" line and a field
/// (add-registry.key-field), with "ed25519:…" while it is empty, under the
/// address. A file and a mirror show no signing-key field. Editing offers
/// CANCEL and CHECK. Checking says "Checking
/// {address}…" over CANCEL. Ready to add a registry: its name in the
/// heading role, then Address, Packages ("{n} · {kinds}"), Key (each
/// fingerprint, at most three, then "+N more"), or for an unsigned registry
/// the amber "Unsigned · accepted in Developer mode only" in Key's place,
/// Downloads, and the amber warning, over CANCEL and ADD. Ready to add a
/// mirror: Address, "Checked against the keys you trust for {name}" and
/// those keys, over CANCEL and ADD MIRROR. Refused: the refusal in amber,
/// over CANCEL, and CHECK while the field takes typing. The name, the
/// address, the keys and the refusal are labels.
///
/// @param window the window and the pointer's place on it
/// @param fonts the fonts the text is measured in; null estimates each character
/// @return the display list, in points from the window's top left corner
[[nodiscard]] oa::ui::kit::DisplayList
add_registry_list(const AddRegistry& window, const oa::ui::kit::Fonts* fonts = nullptr);

/// Puts the focus where its phase starts it, and shows it: on the address
/// field while it takes typing, otherwise on CANCEL, so that trusting a
/// registry is never what Enter or a pad's A does first.
///
/// @param[in,out] window the window
void add_registry_settle(AddRegistry& window) noexcept;

/// Tells whether the field takes typing: an address or a mirror, while
/// editing or refused.
///
/// @param window the window
/// @return true while it does
[[nodiscard]] bool add_registry_editable(const AddRegistry& window) noexcept;

/// Returns the focused field while it takes typing and the focus is shown,
/// over which the host keeps the system's text input started. That is the
/// signing-key field when it has the focus, otherwise the address field.
///
/// @param window the window
/// @return the field's rectangle, in points; none otherwise
[[nodiscard]] std::optional<oa::ui::kit::Rect>
add_registry_field(const AddRegistry& window) noexcept;

/// Takes typed or pasted text into the focused field at its caret: the
/// signing-key field when it has the focus, otherwise the address. Only the
/// text before its first line break is kept. The address holds at most
/// oa::formats::url::max_url_bytes and the signing key at most 128 bytes,
/// cut at a character's start. A refused address goes back to editing once
/// the player changes either field.
///
/// @param[in,out] window the window
/// @param utf8 the text, in UTF-8
/// @return redraw when the field changed; none while it takes no typing
[[nodiscard]] AddRegistryAction add_registry_text(AddRegistry& window, std::string_view utf8);

/// Takes a key. Escape is CANCEL. Enter is CHECK while a field takes
/// typing. On the address field or the signing-key field, Space starts
/// typing, and Backspace, Delete, Left, Right, Home and End edit that
/// field. Enter and Space press a focused button. Tab and the arrows move
/// the focus.
///
/// @param[in,out] window the window
/// @param pressed the key
/// @param fonts the fonts the text is measured in; null estimates each character
/// @return what the host does
[[nodiscard]] AddRegistryAction add_registry_key(
    AddRegistry& window, oa::ui::kit::Key pressed, const oa::ui::kit::Fonts* fonts = nullptr
);

/// Moves the pointer: the control under it lights.
///
/// @param[in,out] window the window
/// @param at the pointer, in points from the window's top left corner
/// @param fonts the fonts the text is measured in; null estimates each character
/// @return redraw when the look changed, otherwise none
[[nodiscard]] AddRegistryAction add_registry_pointer_move(
    AddRegistry& window, oa::ui::kit::Point at, const oa::ui::kit::Fonts* fonts = nullptr
);

/// Presses the pointer's button: a press on a control holds it.
///
/// @param[in,out] window the window
/// @param at the pointer, in points from the window's top left corner
/// @param fonts the fonts the text is measured in; null estimates each character
/// @return redraw when a control is held, otherwise none
[[nodiscard]] AddRegistryAction add_registry_pointer_down(
    AddRegistry& window, oa::ui::kit::Point at, const oa::ui::kit::Fonts* fonts = nullptr
);

/// Presses where a finger landed: the control within reach takes it, and
/// the later release is moved by the same distance.
///
/// @param[in,out] window the window
/// @param at the finger, in points from the window's top left corner
/// @param reach how far a control may lie from the finger, in points
/// @param fonts the fonts the text is measured in; null estimates each character
/// @return redraw when a control is held, otherwise none
[[nodiscard]] AddRegistryAction add_registry_finger_down(
    AddRegistry& window,
    oa::ui::kit::Point at,
    int32_t reach,
    const oa::ui::kit::Fonts* fonts = nullptr
);

/// Releases the pointer. A release over the control the press held presses
/// it; a release over the field focuses it and starts typing.
///
/// @param[in,out] window the window
/// @param at the pointer, in points from the window's top left corner
/// @param fonts the fonts the text is measured in; null estimates each character
/// @return the control's action, redraw when the release missed it, otherwise none
[[nodiscard]] AddRegistryAction add_registry_pointer_up(
    AddRegistry& window, oa::ui::kit::Point at, const oa::ui::kit::Fonts* fonts = nullptr
);

/// Takes a key, as add_registry_key does, over a look's own display list.
///
/// @param[in,out] window the window
/// @param pressed the key
/// @param list the look's display list, its controls those add_registry_list names
/// @return what the host does
[[nodiscard]] AddRegistryAction add_registry_key(
    AddRegistry& window, oa::ui::kit::Key pressed, const oa::ui::kit::DisplayList& list
);

/// Takes a pointer's move, press or release, as add_registry_pointer_move,
/// add_registry_pointer_down and add_registry_pointer_up do, over a look's
/// own display list.
///
/// @param[in,out] window the window
/// @param outcome what the kit's pointer did on the look's list
/// @return the control's action, redraw when the look changed, otherwise none
[[nodiscard]] AddRegistryAction
add_registry_pointer(AddRegistry& window, const oa::ui::kit::PointerOutcome& outcome);

/// Draws the window at the canvas's origin, in the canvas's fonts.
///
/// @param canvas where it is drawn
/// @param window the window
void draw_add_registry(const oa::ui::kit::Canvas& canvas, const AddRegistry& window);

} // namespace oa::ui::engine_settings
