// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Every action the game answers, as the action registry states it. Nothing
// in the game reads this table yet. Each default stays the text the registry
// wrote; a later change parses that text.
#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace oa::data::input_actions {

/// How an action fires.
enum class Kind {
    press,    ///< once, when the chord goes down
    repeat,   ///< again while the chord is held
    hold,     ///< on while the chord is held
    modifier, ///< held during another action
    pointer,  ///< a mouse gesture
    wheel,    ///< a wheel turn
};

/// Whether a character or a key position follows the letter or the place.
enum class By {
    none,   ///< the defaults are not letters or positions
    letter, ///< the character follows the player's layout
    place,  ///< the key keeps its position
};

/// Where a pointer or wheel action lands. `none` is every other kind.
enum class Target {
    none,
    battlefield,
    minimap,
    build_button,
    queued_site,
    marker,
    resource_row,
    megamap,
};

/// The parent index of the root context.
inline constexpr uint8_t no_parent = 255;

/// One context, in the order of an action's context bits.
struct Context {
    std::string_view name{};  ///< the id
    uint8_t parent{};         ///< the parent's index, or no_parent at the root
    bool overlay{};           ///< sits above its parent while it is open
    std::string_view title{}; ///< the English title
};

/// What one hack does to an action while that hack is on.
struct HackChange {
    std::string_view hack{};                      ///< the hack id
    bool sets_default{};                          ///< replaces the action's defaults
    std::span<const std::string_view> defaults{}; ///< the replacement chords
    bool sets_when{};                             ///< replaces the action's conditions
    uint32_t when{};                              ///< the replacement condition bits
};

/// The English texts of an action. They are the words themselves: there are no
/// word ids.
struct Words {
    std::string_view name{};        ///< the short name
    std::string_view description{}; ///< what the action does
};

/// One catalogue action.
struct Action {
    std::string_view id{};                        ///< stable id
    std::string_view area{};                      ///< editor category
    uint16_t order{};                             ///< order within the area, from 1
    uint32_t context{};                           ///< bits over contexts()
    uint32_t when{};                              ///< bits over conditions()
    Kind kind{};                                  ///< how it fires
    By by{};                                      ///< letter, place, or neither
    std::span<const std::string_view> source{};   ///< where the action comes from
    std::span<const std::string_view> defaults{}; ///< the OA default chords, as text
    Words words{};                                ///< the English name and description
    std::span<const std::string_view> aliases{};  ///< old ids kept after a rename
    std::string_view replaces{};                  ///< the base action, or empty
    bool fixed{};                                 ///< the one chord a layout cannot take
    bool latchable{};                             ///< a hold or modifier that may latch
    bool confirmable{};                           ///< may require a hold to confirm
    bool repeatable{};                            ///< a repeat action the helper may apply
    std::span<const HackChange> hacks{};          ///< changes while a hack is on
    std::string_view left_click_default{};        ///< the left-click scheme's chord, or empty
    Target target{};                              ///< where a pointer or wheel action lands
    std::string_view gadget{};                    ///< the order-panel gadget, or empty
    std::string_view pad{};                       ///< a pad Action enumerator, or empty
    std::string_view touch{};                     ///< a touch action, or empty
};

/// A family of actions made when a mod loads, not listed as actions.
struct Family {
    std::string_view id{};                      ///< `gui`, `select.category` or `select.tag`
    std::string_view area{};                    ///< the editor category
    uint32_t context{};                         ///< bits over contexts()
    Kind kind{};                                ///< how each made action fires
    By by{};                                    ///< letter or place
    std::span<const std::string_view> source{}; ///< where the made actions come from
    std::string_view default_template{};        ///< a chord template kept as text, or empty
    std::string_view order_panel_area{};        ///< area of an order-panel gadget, or empty
    uint32_t order_panel_context{};             ///< context bits of that gadget, or 0
    /// Name and description templates. `{word}` is the data's spelling.
    Words words{};
};

/// Returns every action, sorted by id.
///
/// @return the action table
[[nodiscard]] std::span<const Action> actions() noexcept;

/// Finds an action by its id or by an old id it still answers to.
///
/// @param id_or_alias the id or the alias
/// @return the action, or null when the table has neither
[[nodiscard]] const Action* find(std::string_view id_or_alias) noexcept;

/// Returns the contexts, in bit order. The root's parent is no_parent.
///
/// @return the context table
[[nodiscard]] std::span<const Context> contexts() noexcept;

/// Returns the condition names, in bit order.
///
/// @return the condition table
[[nodiscard]] std::span<const std::string_view> conditions() noexcept;

/// Returns the load-time families.
///
/// @return the family table
[[nodiscard]] std::span<const Family> families() noexcept;

/// Returns the hack ids the registry names, sorted.
///
/// @return the hack ids
[[nodiscard]] std::span<const std::string_view> hack_ids() noexcept;

/// Reports whether two condition masks require opposite sides of an exclusive
/// pair.
///
/// @param when_a one action's conditions
/// @param when_b the other action's conditions
/// @return true when a pair is split across the two masks
[[nodiscard]] bool exclusive(uint32_t when_a, uint32_t when_b) noexcept;

} // namespace oa::data::input_actions
