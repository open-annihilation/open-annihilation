// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// What a seat may see: sight, the seen list, its own radar picture and, for a
// full-awareness brain, the search over every unit. install_sight is called
// from install_libraries and works when the seat has no match.
#pragma once

#include <cstdint>

struct lua_State;

namespace oa::sim::brains {

struct Seat;

/// One enemy contact kept for the radar events, in unit slot order.
///
/// `diff_sighted` matches on `unit` and `generation` only. `sonar` and `radar`
/// are the picture bits that published the contact.
struct ContactEntry {
    /// The unit's slot.
    uint16_t unit{};
    /// The generation that tells this unit from the next to take the slot.
    uint32_t generation{};
    /// Set when the publishing picture marked the slot as sonar.
    bool sonar{};
    /// Set when the publishing picture marked the slot as radar.
    bool radar{};
};

/// Opens sight, radar and, when the seat has full awareness, the wider search.
///
/// Works when `seat` has no match. In a match, asks the match to keep the
/// seat's radar picture for the rest of the match. May raise.
///
/// @param state sandbox state
/// @param seat the seat being opened
void install_sight(lua_State* state, Seat& seat);

/// Charges one element per picture slot and fills the seat's work list.
///
/// The delivered list and the work list are allocated once, to the picture's
/// span. A seat with no match, or a picture nobody kept, fills nothing.
/// Enemy contacts only: a live unit the seat is not allied with, whose picture
/// byte has radar or sonar, in slot order.
///
/// @param state sandbox state, charged for the read
/// @param seat the seat
/// @return how many contacts were written into the work list
uint32_t collect_enemy_contacts(lua_State* state, Seat& seat);

/// Pushes the one contact object of `entry`.
///
/// The seat keeps that object by slot. The same generation is the same object,
/// and its sonar and radar bits become the ones `entry` carries. A new
/// generation is a new object. May raise.
///
/// @param state sandbox state
/// @param seat the seat whose registry holds the contact
/// @param entry the contact to publish
void push_contact(lua_State* state, Seat& seat, const ContactEntry& entry);

/// Refreshes the sonar and radar bits of the contacts the seat has delivered.
///
/// A contact the latest list no longer holds keeps the bits it was published
/// with. May raise.
///
/// @param state sandbox state
/// @param seat the seat, after its delivered list is the new picture
void remember_contacts(lua_State* state, Seat& seat);

} // namespace oa::sim::brains
