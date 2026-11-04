// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The keyed-value container behind the property-bag slots (44-62). Entries
// stay in ascending key order (byte compare, shorter prefix first), each key
// at most once; an entry's address is stable until the bag is destroyed.
#ifndef OA_ROVER_BAGS_H
#define OA_ROVER_BAGS_H

#include "oa/rover/interface.h"

#include <stddef.h>
#include <stdint.h>

// A live bag carries this at the handle's validation field; every by-bag slot
// rejects a handle without it. Its bytes, least significant first, spell
// "VNko".
constexpr uint32_t kRoverBagMagic = 0x6f6b4e56;

// One entry as the host sees it through slots 52-62: 3.1c's 0x2c-byte
// value record. Every setter retags the entry; values of other types stay.
struct RoverBagEntry {
    uint32_t type{}; // OA_ROVER_ENTRY_*
    RoverString name{};
    int32_t int_value{};
    uint32_t uint_value{};
    RoverString string_value{};
};

struct RoverBagLink {
    RoverBagLink* next{};
    RoverBagLink* previous{};
};

struct RoverBagNode {
    RoverBagLink link{};
    RoverString key{};
    RoverBagEntry entry{};
};

struct RoverPropertyBag {
    RoverBagLink head{}; // sentinel; head.next is the lowest key
    uint32_t count{};
    RoverBagLink* cursor{}; // node slot 53 returns next; &head once exhausted
    uint32_t magic{};
};

struct RoverEntry;

/// Copy-constructs a value record: type, key name, both numbers and the string value.
///
/// Each string of entry is emptied without freeing and then assigned from source, so buffers are
/// shared under the usual rules.
///
/// @param[out] entry value record to fill; strings it held before are not released
/// @param source value record to copy
void rover_copy_field_record(RoverBagEntry* entry, const RoverBagEntry* source);

/// Releases a value record's strings: the string value first, then the key name.
///
/// @param[in,out] entry value record whose strings are released
void rover_record_field_release_strings(RoverBagEntry* entry);

/// Adds each typed entry of the bag to a request entry, in key order.
///
/// Each field is keyed by the bag's key rather than the entry's stored name. Untyped entries are
/// skipped, and a setter that refuses its field does not stop the walk.
///
/// @param bag bag to read
/// @param[in,out] entry request entry receiving the fields
/// @param text_limit most wire bytes of each string, cut on a character boundary; negative leaves
///     the string whole
/// @return true
/// @quirk Reports true even when a setter refused a field.
bool rover_walk_record_fields(
    const RoverPropertyBag* bag, RoverEntry& entry, int32_t text_limit = -1
);

/// Stores a string value already in Rover's UTF-8, without an interface conversion.
///
/// The slot that the game calls converts Windows-1252 first. A value read off the wire is already
/// UTF-8 and is stored here.
///
/// @param bag bag to update
/// @param key entry key
/// @param value string to store
/// @return true when the entry was stored
bool rover_bag_store_string(
    RoverPropertyBag* bag, const RoverString* key, const RoverString* value
);

static_assert(offsetof(RoverBagNode, link) == 0, "a link is its node's address");
#if OA_ROVER_ABI32
static_assert(sizeof(RoverBagEntry) == 0x2c, "3.1c's value record");
static_assert(offsetof(RoverBagEntry, int_value) == 0x14, "RoverBagEntry.int_value offset");
static_assert(offsetof(RoverBagEntry, uint_value) == 0x18, "RoverBagEntry.uint_value offset");
static_assert(offsetof(RoverBagEntry, string_value) == 0x1c, "RoverBagEntry.string_value offset");
#endif

#endif
