// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The people lists under boneyards/peoplelists: reading a list file on
// first use, the player's copy ahead of the game folder's, repeats and
// erasure, writing CRLF lines into the player's folder, release writing
// every list, and the Rover session's service binding that the chat
// console's muzzle goes through.
#include "oa/boneyards/people_lists.hpp"
#include "oa/boneyards/rover_session.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

using namespace oa::boneyards;
namespace fs = std::filesystem;

int g_failures = 0;

void check(bool ok, const char* what, int line) {
    if (!ok) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
        ++g_failures;
    }
}

#define CHECK(expr) check((expr), #expr, __LINE__)

// A game folder, whose Boneyards directory is capitalised as shipped, and the
// player's folder beside it. Writes go to the player's folder when `player`
// is set.
struct Folders {
    fs::path install;
    fs::path player;
};

Folders fresh_folders(const char* name, bool with_boneyards, bool player) {
    const fs::path root = fs::temp_directory_path() / name;
    fs::remove_all(root);
    Folders folders;
    folders.install = root / "install";
    folders.player = root / "player";
    fs::create_directories(folders.install);
    fs::create_directories(folders.player);
    if (with_boneyards)
        fs::create_directories(folders.install / "Boneyards");
    rover_session_configure(folders.install.string().c_str(), "");
    if (player)
        rover_session_set_player_folder(folders.player.string().c_str());
    return folders;
}

void write_file(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// A load reads "%d" until the first text that is not a number.
void test_load_reads_ids_until_non_number() {
    const Folders folders = fresh_folders("oa-peoplelists-load", true, true);
    write_file(
        folders.install / "Boneyards" / "peoplelists" / "Silenced.lst", "12\r\n-7\n34 x 56\n"
    );
    PeopleLists lists{};
    people_lists_init(&lists);
    CHECK(people_list_contains(&lists, kSilencedList, 12));
    CHECK(people_list_contains(&lists, kSilencedList, -7));
    CHECK(people_list_contains(&lists, kSilencedList, 34));
    CHECK(!people_list_contains(&lists, kSilencedList, 56));
    const PeopleList* list = people_lists_find_or_add(&lists, kSilencedList);
    CHECK(list != nullptr && list->loaded == 1 && list->count == 3);
    CHECK(lists.count == 1);
    people_lists_release(&lists);
}

// A missing file still marks the list read, so it is not tried again; the
// insert keeps repeats and the erase removes every copy.
void test_missing_file_repeats_and_erase() {
    fresh_folders("oa-peoplelists-missing", true, true);
    PeopleLists lists{};
    people_lists_init(&lists);
    CHECK(!people_list_load(&lists, kSilencedList));
    CHECK(people_lists_find_or_add(&lists, kSilencedList)->loaded == 1);
    CHECK(!people_list_contains(&lists, kSilencedList, 5));
    people_list_insert(&lists, kSilencedList, 5);
    people_list_insert(&lists, kSilencedList, 9);
    people_list_insert(&lists, kSilencedList, 5);
    const PeopleList* list = people_lists_find_or_add(&lists, kSilencedList);
    CHECK(list->count == 3 && list->ids[0] == 5 && list->ids[1] == 9 && list->ids[2] == 5);
    CHECK(people_list_erase(&lists, kSilencedList, 5));
    CHECK(list->count == 1 && list->ids[0] == 9);
    CHECK(!people_list_contains(&lists, kSilencedList, 5));
    CHECK(!people_list_erase(&lists, kSilencedList, 5));
    people_lists_release(&lists);
}

// A save writes "%d\r\n" under the player's boneyards/peoplelists, making
// those directories, and leaves the game folder untouched.
void test_save_writes_crlf_lines_and_makes_directory() {
    const Folders folders = fresh_folders("oa-peoplelists-save", true, true);
    PeopleLists lists{};
    people_lists_init(&lists);
    people_list_insert(&lists, kSilencedList, 42);
    people_list_insert(&lists, kSilencedList, 7);
    CHECK(people_list_save(&lists, kSilencedList));
    CHECK(
        read_file(folders.player / "Boneyards" / "peoplelists" / "Silenced.lst") == "42\r\n7\r\n"
    );
    CHECK(!fs::exists(folders.install / "Boneyards" / "peoplelists"));
    // A list never read is written empty and reported unsaved.
    CHECK(!people_list_save(&lists, "Unread"));
    CHECK(fs::exists(folders.player / "Boneyards" / "peoplelists" / "Unread.lst"));
    CHECK(read_file(folders.player / "Boneyards" / "peoplelists" / "Unread.lst").empty());
    people_lists_release(&lists);
}

// With no player folder chosen, a save writes nothing into the game folder.
void test_save_needs_a_player_folder() {
    const Folders folders = fresh_folders("oa-peoplelists-nodir", false, false);
    PeopleLists lists{};
    people_lists_init(&lists);
    people_list_insert(&lists, kSilencedList, 1);
    CHECK(!people_list_save(&lists, kSilencedList));
    CHECK(!fs::exists(folders.install / "boneyards"));
    CHECK(!fs::exists(folders.install / "Boneyards"));
    CHECK(!fs::exists(folders.player / "boneyards"));
    CHECK(!fs::exists(folders.player / "Boneyards"));
    people_lists_release(&lists);
}

// The player's copy is read when there is one, and the game folder's when
// there is not.
void test_player_copy_preferred() {
    const Folders folders = fresh_folders("oa-peoplelists-prefer", true, true);
    write_file(folders.install / "Boneyards" / "peoplelists" / "Silenced.lst", "1\r\n");
    PeopleLists from_install{};
    people_lists_init(&from_install);
    CHECK(people_list_contains(&from_install, kSilencedList, 1));
    CHECK(!people_list_contains(&from_install, kSilencedList, 2));
    people_list_insert(&from_install, kSilencedList, 9);
    people_lists_release(&from_install);
    CHECK(read_file(folders.install / "Boneyards" / "peoplelists" / "Silenced.lst") == "1\r\n");
    CHECK(read_file(folders.player / "Boneyards" / "peoplelists" / "Silenced.lst") == "1\r\n9\r\n");

    write_file(folders.player / "Boneyards" / "peoplelists" / "Silenced.lst", "2\r\n");
    PeopleLists from_player{};
    people_lists_init(&from_player);
    CHECK(!people_list_contains(&from_player, kSilencedList, 1));
    CHECK(people_list_contains(&from_player, kSilencedList, 2));
    people_lists_release(&from_player);
    CHECK(read_file(folders.install / "Boneyards" / "peoplelists" / "Silenced.lst") == "1\r\n");
    CHECK(read_file(folders.player / "Boneyards" / "peoplelists" / "Silenced.lst") == "2\r\n");
}

// The release writes every list into the player's folder before freeing them.
void test_release_writes_every_list() {
    const Folders folders = fresh_folders("oa-peoplelists-release", true, true);
    PeopleLists lists{};
    people_lists_init(&lists);
    people_list_insert(&lists, kSilencedList, 3);
    people_list_insert(&lists, "Friends", 4);
    people_lists_release(&lists);
    CHECK(lists.count == 0);
    CHECK(read_file(folders.player / "Boneyards" / "peoplelists" / "Silenced.lst") == "3\r\n");
    CHECK(read_file(folders.player / "Boneyards" / "peoplelists" / "Friends.lst") == "4\r\n");
    CHECK(!fs::exists(folders.install / "Boneyards" / "peoplelists"));
}

void test_find_or_add_bounds() {
    fresh_folders("oa-peoplelists-bounds", true, true);
    PeopleLists lists{};
    people_lists_init(&lists);
    char long_name[kPeopleListNameCapacity + 1];
    std::memset(long_name, 'n', kPeopleListNameCapacity);
    long_name[kPeopleListNameCapacity] = '\0';
    CHECK(people_lists_find_or_add(&lists, long_name) == nullptr);
    char name[8];
    for (uint32_t i = 0; i < kMaxPeopleLists; ++i) {
        std::snprintf(name, sizeof name, "L%u", static_cast<unsigned>(i));
        CHECK(people_lists_find_or_add(&lists, name) != nullptr);
    }
    CHECK(people_lists_find_or_add(&lists, "One more") == nullptr);
    CHECK(people_lists_find_or_add(&lists, "L3") == &lists.lists[3]);
    CHECK(!people_list_contains(&lists, "One more", 1));
    lists = PeopleLists{};
}

// The Rover session creates the lists on first use and the release writes
// them back; the next use reads the file again.
void test_session_service_binding() {
    const Folders folders = fresh_folders("oa-peoplelists-service", true, true);
    RoverSession* session = rover_session();
    CHECK(session->people_lists == nullptr);
    const BoneyardsService service = rover_session_service();
    CHECK(!service.peoplelist_contains(service.user, kSilencedList, 77));
    CHECK(session->people_lists != nullptr);
    service.peoplelist_insert(service.user, kSilencedList, 77);
    service.peoplelist_insert(service.user, kSilencedList, 78);
    service.peoplelist_erase(service.user, kSilencedList, 78);
    CHECK(service.peoplelist_contains(service.user, kSilencedList, 77));
    service.peoplelist_release(service.user);
    CHECK(session->people_lists == nullptr);
    CHECK(read_file(folders.player / "Boneyards" / "peoplelists" / "Silenced.lst") == "77\r\n");
    CHECK(!fs::exists(folders.install / "Boneyards" / "peoplelists"));
    CHECK(service.peoplelist_contains(service.user, kSilencedList, 77));
    CHECK(!service.peoplelist_contains(service.user, kSilencedList, 78));
    rover_session_release_people_lists(session);
    rover_session_release_people_lists(session);
    CHECK(session->people_lists == nullptr);
}

} // namespace

int main() {
    test_load_reads_ids_until_non_number();
    test_missing_file_repeats_and_erase();
    test_save_writes_crlf_lines_and_makes_directory();
    test_save_needs_a_player_folder();
    test_player_copy_preferred();
    test_release_writes_every_list();
    test_find_or_add_bounds();
    test_session_service_binding();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("boneyards people lists: ok");
    return 0;
}
