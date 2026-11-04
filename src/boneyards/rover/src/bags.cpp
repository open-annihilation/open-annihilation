// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Property bags: keyed values kept as a list sorted by key, one entry per
// key, whose entries keep their addresses while the bag holds them and whose
// cursor walks them in key order; string keys and values follow
// RoverString's share rules.

#include "bags.h"

#include "heap.h"
#include "record.h"
#include "slots.h"
#include "wire_text.h"

#include "oa/rover/strings.h"

#include <new>
#include <string.h>

namespace {

bool is_valid(const RoverPropertyBag* bag) {
    return bag && bag->magic == kRoverBagMagic;
}

RoverBagNode* node_of(RoverBagLink* link) {
    return reinterpret_cast<RoverBagNode*>(link);
}

const RoverBagNode* node_of(const RoverBagLink* link) {
    return reinterpret_cast<const RoverBagNode*>(link);
}

// The text 3.1c hands a setter for an empty key or value.
constexpr char kNoText[] = "";

const char* text_or_empty(const RoverString& string) {
    return string.data ? string.data : kNoText;
}

/// Orders two keys by their bytes, a common prefix putting the shorter first.
///
/// @param key first key
/// @param other second key
/// @return -1, 0 or 1 as key orders before, equal to or after other
int32_t compare_keys(const RoverString* key, const RoverString* other) {
    return rover_string_compare(key, 0, key->length, rover_string_text(other), other->length);
}

/// Finds the first node whose key is not below key.
///
/// @param bag bag to search
/// @param key key to look for
/// @return that node's link, or the head when every key is lower
RoverBagLink* lower_bound(RoverPropertyBag* bag, const RoverString* key) {
    RoverBagLink* link = bag->head.next;
    while (link != &bag->head && compare_keys(&node_of(link)->key, key) < 0)
        link = link->next;
    return link;
}

/// Finds the node holding key.
///
/// @param bag bag to search
/// @param key key to look for
/// @return the node's link, or the head when there is none
RoverBagLink* find_node(RoverPropertyBag* bag, const RoverString* key) {
    RoverBagLink* link = lower_bound(bag, key);
    if (link != &bag->head && compare_keys(key, &node_of(link)->key) >= 0)
        return link;
    return &bag->head;
}

/// Finds the entry stored under key.
///
/// @param bag bag to search
/// @param key key to look for
/// @return the entry, or null when there is none
RoverBagEntry* find_entry(RoverPropertyBag* bag, const RoverString* key) {
    RoverBagLink* link = find_node(bag, key);
    return link == &bag->head ? nullptr : &node_of(link)->entry;
}

/// Returns the cursor's entry and moves the cursor to the next key.
///
/// @param[in,out] bag bag whose cursor advances
/// @return the entry, or null once the cursor has passed the last entry
RoverBagEntry* step_cursor(RoverPropertyBag* bag) {
    if (bag->cursor == &bag->head)
        return nullptr;
    RoverBagEntry* entry = &node_of(bag->cursor)->entry;
    bag->cursor = bag->cursor->next;
    return entry;
}

/// Finds key's entry, inserting an untyped one in key order when absent.
///
/// The key is staged in a temporary copy before it is stored, so a shareable key buffer ends with
/// the share count a 3.1c insert leaves; the untyped value is copied from a blank one into the
/// pair and then the node.
///
/// @param[in,out] bag bag to search and extend
/// @param key key to find or insert
/// @return the entry, or null when a new node could not be allocated
RoverBagEntry* find_or_insert(RoverPropertyBag* bag, const RoverString* key) {
    RoverBagEntry blank{};
    RoverString staged{};
    rover_string_assign(&staged, key);
    RoverBagEntry value{};
    rover_copy_field_record(&value, &blank);
    RoverBagLink* at = lower_bound(bag, &staged);
    RoverBagNode* node = nullptr;
    if (at != &bag->head && compare_keys(&staged, &node_of(at)->key) >= 0) {
        node = node_of(at);
    } else if (void* block = rover_heap_alloc(sizeof(RoverBagNode))) {
        node = new (block) RoverBagNode{};
        rover_string_assign(&node->key, &staged);
        rover_copy_field_record(&node->entry, &value);
        node->link.next = at;
        node->link.previous = at->previous;
        at->previous->next = &node->link;
        at->previous = &node->link;
        ++bag->count;
    }
    rover_record_field_release_strings(&value);
    rover_string_release(&staged);
    rover_record_field_release_strings(&blank);
    return node ? &node->entry : nullptr;
}

/// Destroys a node's key and value: the value's strings, then the key.
///
/// @param[in,out] node node whose strings are released
void release_node_value(RoverBagNode* node) {
    rover_record_field_release_strings(&node->entry);
    rover_string_release(&node->key);
}

} // namespace

void rover_copy_field_record(RoverBagEntry* entry, const RoverBagEntry* source) {
    entry->type = source->type;
    rover_string_tidy(&entry->name, false);
    rover_string_assign(&entry->name, &source->name);
    entry->int_value = source->int_value;
    entry->uint_value = source->uint_value;
    rover_string_tidy(&entry->string_value, false);
    rover_string_assign(&entry->string_value, &source->string_value);
}

void rover_record_field_release_strings(RoverBagEntry* entry) {
    rover_string_release(&entry->string_value);
    rover_string_release(&entry->name);
}

bool rover_walk_record_fields(const RoverPropertyBag* bag, RoverEntry& entry, int32_t text_limit) {
    for (const RoverBagLink* link = bag->head.next; link != &bag->head; link = link->next) {
        const RoverBagNode* node = node_of(link);
        const char* key = text_or_empty(node->key);
        switch (node->entry.type) {
        case OA_ROVER_ENTRY_INT:
            rover_entry_set_int(entry, key, node->entry.int_value);
            break;
        case OA_ROVER_ENTRY_UINT:
            rover_entry_set_uint(entry, key, node->entry.uint_value);
            break;
        case OA_ROVER_ENTRY_STRING: {
            const char* text = text_or_empty(node->entry.string_value);
            // Four UTF-8 bytes is the longest one wire byte stands for, and a report field is at
            // most 255 wire bytes.
            if (text_limit >= 0 && text_limit <= 255) {
                char kept[1024];
                int32_t length = rover_keep_for_wire(
                    kept,
                    static_cast<int32_t>(sizeof kept),
                    text,
                    static_cast<int32_t>(strlen(text)),
                    text_limit
                );
                if (length > 0)
                    rover_entry_set_string(entry, key, kept);
            } else {
                rover_entry_set_string(entry, key, text);
            }
            break;
        }
        default:
            break;
        }
    }
    return true;
}

RoverPropertyBag* ROVER_CALL rover_create_property_bag(void) {
    void* block = rover_heap_alloc(sizeof(RoverPropertyBag));
    if (!block)
        return nullptr;
    RoverPropertyBag* bag = new (block) RoverPropertyBag{};
    bag->head.next = &bag->head;
    bag->head.previous = &bag->head;
    bag->cursor = bag->head.next;
    bag->magic = kRoverBagMagic;
    return bag;
}

void ROVER_CALL rover_destroy_property_bag(RoverPropertyBag* bag) {
    if (!is_valid(bag))
        return;
    RoverBagLink* link = bag->head.next;
    while (link != &bag->head) {
        RoverBagNode* node = node_of(link);
        link = link->next;
        release_node_value(node);
        rover_heap_free(node);
    }
    // Cleared so a stale handle fails validation instead of being freed twice.
    bag->magic = 0;
    rover_heap_free(bag);
}

bool ROVER_CALL
rover_get_field_int(RoverPropertyBag* bag, const RoverString* key, int32_t* out_value) {
    if (!is_valid(bag))
        return false;
    const RoverBagEntry* entry = find_entry(bag, key);
    if (!entry || entry->type != OA_ROVER_ENTRY_INT)
        return false;
    *out_value = entry->int_value;
    return true;
}

bool ROVER_CALL
rover_get_field_uint(RoverPropertyBag* bag, const RoverString* key, uint32_t* out_value) {
    if (!is_valid(bag))
        return false;
    const RoverBagEntry* entry = find_entry(bag, key);
    if (!entry || entry->type != OA_ROVER_ENTRY_UINT)
        return false;
    *out_value = entry->uint_value;
    return true;
}

// Copies stored UTF-8 into out_value as the interface receives it.
static void present_string(RoverString* out_value, const RoverString& stored) {
    if (!rover_interface_windows1252()) {
        rover_string_assign(out_value, &stored);
        return;
    }
    const char* shown = rover_show(rover_string_text(&stored));
    rover_string_assign_text(out_value, shown, static_cast<uint32_t>(strlen(shown)));
}

// Stores value, reading Windows-1252 when that is the interface encoding.
static void take_bag_string(RoverString* stored, const RoverString* value) {
    if (!rover_interface_windows1252() || !value) {
        rover_string_assign(stored, value);
        return;
    }
    char taken[kRoverInterfaceTextBytes];
    const char* utf8 = rover_take_text(taken, kRoverInterfaceTextBytes, rover_string_text(value));
    rover_string_assign_text(stored, utf8, static_cast<uint32_t>(strlen(utf8)));
}

bool ROVER_CALL
rover_get_field_string(RoverPropertyBag* bag, const RoverString* key, RoverString* out_value) {
    if (!is_valid(bag))
        return false;
    const RoverBagEntry* entry = find_entry(bag, key);
    if (!entry || entry->type != OA_ROVER_ENTRY_STRING)
        return false;
    present_string(out_value, entry->string_value);
    return true;
}

bool ROVER_CALL rover_set_field_int(RoverPropertyBag* bag, const RoverString* key, int32_t value) {
    if (!is_valid(bag))
        return false;
    RoverBagEntry* entry = find_or_insert(bag, key);
    if (!entry)
        return false;
    entry->type = OA_ROVER_ENTRY_INT;
    entry->int_value = value;
    rover_string_assign(&entry->name, key);
    return true;
}

bool ROVER_CALL
rover_set_field_uint(RoverPropertyBag* bag, const RoverString* key, uint32_t value) {
    if (!is_valid(bag))
        return false;
    RoverBagEntry* entry = find_or_insert(bag, key);
    if (!entry)
        return false;
    entry->type = OA_ROVER_ENTRY_UINT;
    entry->uint_value = value;
    rover_string_assign(&entry->name, key);
    return true;
}

bool rover_bag_store_string(
    RoverPropertyBag* bag, const RoverString* key, const RoverString* value
) {
    if (!is_valid(bag))
        return false;
    RoverBagEntry* entry = find_or_insert(bag, key);
    if (!entry)
        return false;
    rover_string_assign(&entry->string_value, value);
    entry->type = OA_ROVER_ENTRY_STRING;
    rover_string_assign(&entry->name, key);
    return true;
}

bool ROVER_CALL
rover_set_bag_string(RoverPropertyBag* bag, const RoverString* key, const RoverString* value) {
    if (!is_valid(bag))
        return false;
    if (!rover_interface_windows1252() || !value)
        return rover_bag_store_string(bag, key, value);
    char taken[kRoverInterfaceTextBytes];
    const char* utf8 = rover_take_text(taken, kRoverInterfaceTextBytes, rover_string_text(value));
    RoverString converted{};
    rover_string_assign_text(&converted, utf8, static_cast<uint32_t>(strlen(utf8)));
    bool stored = rover_bag_store_string(bag, key, &converted);
    rover_string_release(&converted);
    return stored;
}

RoverBagEntry* ROVER_CALL rover_get_first_bag_entry(RoverPropertyBag* bag) {
    if (!is_valid(bag))
        return nullptr;
    bag->cursor = bag->head.next;
    return step_cursor(bag);
}

RoverBagEntry* ROVER_CALL rover_get_next_bag_entry(RoverPropertyBag* bag) {
    return is_valid(bag) ? step_cursor(bag) : nullptr;
}

RoverBagEntry* ROVER_CALL rover_find_bag_entry(RoverPropertyBag* bag, const RoverString* key) {
    return is_valid(bag) ? find_entry(bag, key) : nullptr;
}

uint32_t ROVER_CALL rover_get_entry_type(const RoverBagEntry* entry) {
    return entry ? entry->type : OA_ROVER_ENTRY_NONE;
}

bool ROVER_CALL rover_get_entry_name(const RoverBagEntry* entry, RoverString* out_name) {
    if (!entry)
        return false;
    rover_string_assign(out_name, &entry->name);
    return true;
}

bool ROVER_CALL rover_get_entry_int(const RoverBagEntry* entry, int32_t* out_value) {
    if (!entry || entry->type != OA_ROVER_ENTRY_INT)
        return false;
    *out_value = entry->int_value;
    return true;
}

bool ROVER_CALL rover_get_entry_uint(const RoverBagEntry* entry, uint32_t* out_value) {
    if (!entry || entry->type != OA_ROVER_ENTRY_UINT)
        return false;
    *out_value = entry->uint_value;
    return true;
}

bool ROVER_CALL rover_get_entry_string(const RoverBagEntry* entry, RoverString* out_value) {
    if (!entry || entry->type != OA_ROVER_ENTRY_STRING)
        return false;
    present_string(out_value, entry->string_value);
    return true;
}

bool ROVER_CALL rover_set_entry_int(RoverBagEntry* entry, int32_t value) {
    if (!entry)
        return false;
    entry->type = OA_ROVER_ENTRY_INT;
    entry->int_value = value;
    return true;
}

bool ROVER_CALL rover_set_entry_uint(RoverBagEntry* entry, uint32_t value) {
    if (!entry)
        return false;
    entry->type = OA_ROVER_ENTRY_UINT;
    entry->uint_value = value;
    return true;
}

bool ROVER_CALL rover_set_entry_string(RoverBagEntry* entry, const RoverString* value) {
    if (!entry)
        return false;
    take_bag_string(&entry->string_value, value);
    entry->type = OA_ROVER_ENTRY_STRING;
    return true;
}
