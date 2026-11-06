// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Property bags through the interface table (slots 44-62): handle checks,
// typed access by key and by entry, retyping, key order, the shared cursor of
// the first/next pair, which strings share the caller's buffers, and the walk
// that adds a bag's entries to an outgoing request.

#include "../src/bags.h"
#include "../src/record.h"
#include "oa/rover/interface.h"
#include "oa/rover/strings.h"

#include <stdio.h>
#include <string.h>

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

// A host-built key or value, released at the end of its scope.
struct Text {
    RoverString string{};

    explicit Text(const char* text) {
        rover_string_assign_text(&string, text, static_cast<uint32_t>(strlen(text)));
    }

    ~Text() { rover_string_release(&string); }

    Text(const Text&) = delete;
    Text& operator=(const Text&) = delete;

    const RoverString* get() const { return &string; }
};

bool reads(const RoverString& string, const char* text) {
    return strcmp(rover_string_text(&string), text) == 0 && string.length == strlen(text);
}

void check_handles(const RoverInterface* rover) {
    Text key("k");
    int32_t value = 7;
    RoverString out{};
    check(
        !rover->get_field_int(nullptr, key.get(), &value) && value == 7, "null bag fails the getter"
    );
    check(!rover->set_field_int(nullptr, key.get(), 1), "null bag fails the setter");
    check(
        !rover->set_bag_string(nullptr, key.get(), key.get()), "null bag fails the string setter"
    );
    check(rover->get_first_bag_entry(nullptr) == nullptr, "null bag has no first entry");
    check(rover->get_next_bag_entry(nullptr) == nullptr, "null bag has no next entry");
    check(rover->find_bag_entry(nullptr, key.get()) == nullptr, "null bag finds nothing");
    rover->destroy_property_bag(nullptr);

    // A block that is not a bag fails every check that validates the handle.
    alignas(8) unsigned char not_a_bag[64] = {};
    auto* fake = reinterpret_cast<RoverPropertyBag*>(not_a_bag);
    check(!rover->set_field_uint(fake, key.get(), 1), "a handle without the magic is rejected");
    check(
        !rover->get_field_string(fake, key.get(), &out),
        "a handle without the magic fails the getter"
    );
    rover->destroy_property_bag(fake);

    check(rover->get_entry_type(nullptr) == OA_ROVER_ENTRY_NONE, "null entry has no type");
    check(!rover->get_entry_name(nullptr, &out), "null entry has no name");
    check(!rover->get_entry_int(nullptr, &value), "null entry has no int");
    check(!rover->set_entry_uint(nullptr, 1), "null entry takes no value");
    check(!rover->set_entry_string(nullptr, key.get()), "null entry takes no string");
}

void check_typed_fields(const RoverInterface* rover) {
    RoverPropertyBag* bag = rover->create_property_bag();
    check(bag != nullptr, "create_property_bag");
    if (!bag)
        return;
    check(rover->get_first_bag_entry(bag) == nullptr, "a new bag is empty");

    Text user_status("USER_STATUS");
    Text user_id("user_id");
    Text name("LOGINNAME");
    Text login("Commander");
    check(rover->set_field_int(bag, user_status.get(), -5), "set_field_int");
    check(rover->set_field_uint(bag, user_id.get(), 0x80000001u), "set_field_uint");
    check(rover->set_bag_string(bag, name.get(), login.get()), "set_bag_string");

    int32_t signed_value = 0;
    uint32_t unsigned_value = 0;
    check(
        rover->get_field_int(bag, user_status.get(), &signed_value) && signed_value == -5,
        "get_field_int"
    );
    check(
        rover->get_field_uint(bag, user_id.get(), &unsigned_value) && unsigned_value == 0x80000001u,
        "get_field_uint"
    );
    RoverString out{};
    check(
        rover->get_field_string(bag, name.get(), &out) && reads(out, "Commander"),
        "get_field_string"
    );
    rover_string_release(&out);

    signed_value = 99;
    check(
        !rover->get_field_int(bag, user_id.get(), &signed_value) && signed_value == 99,
        "a getter of another type fails and leaves the output"
    );
    Text missing("missing");
    check(!rover->get_field_uint(bag, missing.get(), &unsigned_value), "a missing key fails");
    Text other_case("loginname");
    check(
        !rover->get_field_string(bag, other_case.get(), &out) && out.data == nullptr,
        "keys are case-sensitive"
    );

    // Retyping keeps the stale value of the old type in the entry.
    check(rover->set_field_uint(bag, user_status.get(), 12), "retype an existing key");
    check(!rover->get_field_int(bag, user_status.get(), &signed_value), "old type no longer reads");
    RoverBagEntry* entry = rover->find_bag_entry(bag, user_status.get());
    check(
        entry != nullptr && rover->get_entry_type(entry) == OA_ROVER_ENTRY_UINT, "find_bag_entry"
    );
    check(
        rover->set_entry_int(entry, 3) && rover->get_entry_int(entry, &signed_value) &&
            signed_value == 3,
        "set_entry_int retypes"
    );
    check(
        rover->set_entry_uint(entry, 4) && rover->get_entry_uint(entry, &unsigned_value) &&
            unsigned_value == 4,
        "set_entry_uint retypes"
    );
    check(
        rover->get_field_uint(bag, user_status.get(), &unsigned_value) && unsigned_value == 4,
        "an entry setter is visible by key"
    );
    Text text("text");
    check(
        rover->set_entry_string(entry, text.get()) &&
            rover->get_entry_type(entry) == OA_ROVER_ENTRY_STRING,
        "set_entry_string retypes"
    );
    check(rover->get_entry_string(entry, &out) && reads(out, "text"), "get_entry_string");
    check(rover->get_entry_name(entry, &out) && reads(out, "USER_STATUS"), "get_entry_name");
    check(!rover->get_entry_int(entry, &signed_value), "a string entry has no int");
    rover_string_release(&out);

    rover->destroy_property_bag(bag);
}

void check_order_and_cursor(const RoverInterface* rover) {
    RoverPropertyBag* bag = rover->create_property_bag();
    const char* keys[] = {"b", "a", "ab", "B", "", "a"};
    for (int32_t i = 0; i < 6; ++i) {
        Text key(keys[i]);
        rover->set_field_int(bag, key.get(), i);
    }
    // Byte order, shorter prefix first, duplicate "a" overwritten in place.
    const char* expected[] = {"", "B", "a", "ab", "b"};
    const int32_t values[] = {4, 3, 5, 2, 0};
    RoverString name{};
    int32_t i = 0;
    for (RoverBagEntry* entry = rover->get_first_bag_entry(bag); entry;
         entry = rover->get_next_bag_entry(bag), ++i) {
        int32_t value = -1;
        char what[64];
        snprintf(what, sizeof what, "entry %d in key order", i);
        check(
            i < 5 && rover->get_entry_name(entry, &name) && reads(name, expected[i]) &&
                rover->get_entry_int(entry, &value) && value == values[i],
            what
        );
    }
    check(i == 5, "each key once");
    check(rover->get_next_bag_entry(bag) == nullptr, "the cursor stays at the end");

    // The cursor is shared state: first restarts it, next continues it, and a
    // key inserted mid-walk is visited only when it sorts after the cursor.
    RoverBagEntry* first = rover->get_first_bag_entry(bag);
    rover->get_next_bag_entry(bag);
    Text behind("Z");
    Text ahead("aa");
    rover->set_field_int(bag, behind.get(), 6);
    rover->set_field_int(bag, ahead.get(), 7);
    const char* rest[] = {"a", "aa", "ab", "b"};
    bool walked = true;
    for (const char* key : rest) {
        RoverBagEntry* entry = rover->get_next_bag_entry(bag);
        walked = walked && entry && rover->get_entry_name(entry, &name) && reads(name, key);
    }
    check(
        walked && rover->get_next_bag_entry(bag) == nullptr,
        "keys inserted behind the cursor are skipped, ahead of it visited"
    );
    check(rover->get_first_bag_entry(bag) == first, "entry addresses are stable");
    rover_string_release(&name);
    rover->destroy_property_bag(bag);
}

// The share counts 3.1c's assignments leave on the host's buffers.
void check_string_sharing(const RoverInterface* rover) {
    RoverPropertyBag* bag = rover->create_property_bag();
    RoverString key{};
    RoverString value{};
    rover_string_assign_text(&key, "EMAIL", 5);
    rover_string_assign_text(&value, "cmdr@example.net", 16);
    rover->set_bag_string(bag, &key, &value);
    uint8_t key_count = reinterpret_cast<uint8_t*>(key.data)[-1];
    uint8_t value_count = reinterpret_cast<uint8_t*>(value.data)[-1];
    check(key_count == 2, "the node key and the entry name share the caller's key");
    check(value_count == 1, "the entry value shares the caller's value");

    RoverString out{};
    rover->get_field_string(bag, &key, &out);
    check(
        out.data == value.data && reinterpret_cast<uint8_t*>(value.data)[-1] == 2,
        "get_field_string hands out the shared buffer"
    );

    // Releasing the host's strings leaves the bag the owner; destroying the
    // bag then leaves only the string the getter filled.
    rover_string_release(&key);
    rover_string_release(&value);
    rover->destroy_property_bag(bag);
    check(
        reinterpret_cast<uint8_t*>(out.data)[-1] == 0 && reads(out, "cmdr@example.net"),
        "the last owner keeps the text"
    );
    rover_string_release(&out);

    // A key buffer already shared 0xfd times: staging it reaches 0xfe, so the
    // node copies the key and only the entry name takes a share.
    bag = rover->create_property_bag();
    rover_string_assign_text(&key, "busy", 4);
    reinterpret_cast<uint8_t*>(key.data)[-1] = 0xfd;
    rover->set_field_int(bag, &key, 1);
    check(
        reinterpret_cast<uint8_t*>(key.data)[-1] == 0xfe,
        "the staged copy decides whether the node shares the key"
    );
    rover->destroy_property_bag(bag);
    check(reinterpret_cast<uint8_t*>(key.data)[-1] == 0xfd, "destroying gives the share back");
    reinterpret_cast<uint8_t*>(key.data)[-1] = 0;
    rover_string_release(&key);
}

// The value record's copy constructor and destructor: every member copied, the
// strings sharing the source's buffer, and releasing each drops one share.
void check_value_record_copy() {
    Text name("VISITCOUNT");
    Text value("seventy-eight");
    RoverBagEntry source{};
    source.type = OA_ROVER_ENTRY_STRING;
    rover_string_assign(&source.name, name.get());
    source.int_value = -78;
    source.uint_value = 0x4e;
    rover_string_assign(&source.string_value, value.get());

    RoverBagEntry copy{};
    rover_copy_field_record(&copy, &source);
    check(
        copy.type == OA_ROVER_ENTRY_STRING && copy.int_value == -78 && copy.uint_value == 0x4e,
        "the copy carries the type and both numbers"
    );
    check(
        reads(copy.name, "VISITCOUNT") && reads(copy.string_value, "seventy-eight"),
        "the copy carries both strings"
    );
    check(
        copy.name.data == name.string.data && copy.string_value.data == value.string.data,
        "the copy shares the source's string buffers"
    );
    rover_record_field_release_strings(&copy);
    check(
        copy.name.data == nullptr && copy.string_value.data == nullptr && copy.name.length == 0 &&
            copy.string_value.length == 0,
        "releasing a record empties both strings"
    );
    rover_record_field_release_strings(&source);
    check(reads(value.string, "seventy-eight"), "the host's buffer outlives every share");
}

// The walk adds entries under the bag's keys in key order, typed as stored;
// a field the record refuses does not stop it, and it always reports true.
void check_walk(const RoverInterface* rover) {
    RoverRecord* record = rover_record_create();
    RoverEntry* entry = record ? rover_record_add_entry(*record, true, "UserManager") : nullptr;
    check(entry != nullptr, "a request entry to walk into");
    if (!entry) {
        rover_record_destroy(&record);
        return;
    }
    rover_entry_set_uint(*entry, "user_id", 2);

    RoverPropertyBag* empty = rover->create_property_bag();
    check(rover_walk_record_fields(empty, *entry), "walking an empty bag reports true");
    rover->destroy_property_bag(empty);

    RoverPropertyBag* bag = rover->create_property_bag();
    Text visits("VISITCOUNT");
    Text allegiance("ALLEGIANCE");
    Text arm("ARM");
    Text stamp("zeta");
    Text taken("user_id");
    Text bad_key("a=b");
    Text blank("blank");
    Text nothing("");
    rover->set_field_int(bag, visits.get(), -78);
    rover->set_bag_string(bag, allegiance.get(), arm.get());
    rover->set_field_uint(bag, stamp.get(), 0x1f);
    rover->set_field_int(bag, taken.get(), 5);
    rover->set_field_int(bag, bad_key.get(), 1);
    rover->set_bag_string(bag, blank.get(), nothing.get());
    check(rover_walk_record_fields(bag, *entry), "the walk reports true despite refused fields");
    rover->destroy_property_bag(bag);

    char text[256]{};
    int32_t length = 0;
    rover_record_format(*record, text, sizeof text - 1, &length);
    check(
        strcmp(
            text, "{[UserManager<S:ALLEGIANCE=ARM><I:VISITCOUNT=-78><U:user_id=2><U:zeta=1f>]}"
        ) == 0,
        "each typed entry lands under its key; a taken, reserved or empty one is refused"
    );
    rover_record_destroy(&record);
}

} // namespace

int main() {
    const RoverInterface* rover = nullptr;
    rover_get_interface(&rover);
    check_handles(rover);
    check_typed_fields(rover);
    check_order_and_cursor(rover);
    check_string_sharing(rover);
    check_value_record_copy();
    check_walk(rover);
    if (failures)
        return 1;
    puts("rover bags: ok");
    return 0;
}
