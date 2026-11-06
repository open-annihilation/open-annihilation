// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/boneyards/people_lists.hpp"

#include "oa/boneyards/rover_session.hpp"
#include "oa/platform/files.hpp"

#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <system_error>

namespace oa::boneyards {
namespace {

// Written text-mode on Windows, so each line ends CRLF on disk.
constexpr const char* kIdLineFormat = "%d\r\n";
constexpr uint32_t kFirstIdCapacity = 16;

/// Reads one decimal id, as a `%d` conversion does: whitespace, an optional
/// sign, then digits. Returns 1 and stores the id, 0 when the next token is
/// not an id, or EOF when nothing remains.
int read_user_id(std::FILE* file, int32_t* user_id) {
    int ch = std::fgetc(file);
    while (ch != EOF && std::isspace(static_cast<unsigned char>(ch)))
        ch = std::fgetc(file);
    if (ch == EOF)
        return EOF;
    char token[16];
    std::size_t length = 0;
    if (ch == '+' || ch == '-') {
        token[length++] = static_cast<char>(ch);
        ch = std::fgetc(file);
    }
    if (ch == EOF || !std::isdigit(static_cast<unsigned char>(ch)))
        return 0;
    while (ch != EOF && std::isdigit(static_cast<unsigned char>(ch))) {
        if (length + 1 >= sizeof token)
            return 0;
        token[length++] = static_cast<char>(ch);
        ch = std::fgetc(file);
    }
    if (ch != EOF)
        std::ungetc(ch, file);
    const char* begin = token[0] == '+' ? token + 1 : token;
    int32_t value = 0;
    const auto parsed = std::from_chars(begin, token + length, value);
    if (parsed.ec != std::errc{} || parsed.ptr != token + length)
        return 0;
    *user_id = value;
    return 1;
}

std::string list_relative(const char* name) {
    std::string relative = kPeopleListDirectory;
    relative += '/';
    relative += name;
    relative += kPeopleListExtension;
    return relative;
}

void append_id(PeopleList* list, int32_t user_id) {
    if (list->count == list->capacity) {
        if (list->capacity >= kMaxPeopleListIds)
            return;
        const uint32_t capacity = list->capacity == 0 ? kFirstIdCapacity : list->capacity * 2;
        auto* ids = static_cast<int32_t*>(std::realloc(list->ids, capacity * sizeof(int32_t)));
        if (ids == nullptr)
            return;
        list->ids = ids;
        list->capacity = capacity;
    }
    list->ids[list->count++] = user_id;
}

PeopleList* read_list(PeopleLists* lists, const char* name) {
    PeopleList* list = people_lists_find_or_add(lists, name);
    if (list != nullptr && list->loaded == 0)
        (void)people_list_load(lists, name);
    return list;
}

} // namespace

void people_lists_init(PeopleLists* lists) {
    *lists = PeopleLists{};
}

void people_lists_release(PeopleLists* lists) {
    (void)people_lists_save_all(lists);
    for (uint32_t i = 0; i < lists->count; ++i)
        std::free(lists->lists[i].ids);
    *lists = PeopleLists{};
}

PeopleList* people_lists_find_or_add(PeopleLists* lists, const char* name) {
    for (uint32_t i = 0; i < lists->count; ++i)
        if (std::strcmp(lists->lists[i].name, name) == 0)
            return &lists->lists[i];
    if (lists->count == kMaxPeopleLists || std::strlen(name) >= kPeopleListNameCapacity)
        return nullptr;
    PeopleList* list = &lists->lists[lists->count++];
    *list = PeopleList{};
    std::snprintf(list->name, sizeof list->name, "%s", name);
    return list;
}

bool people_list_contains(PeopleLists* lists, const char* name, int32_t user_id) {
    const PeopleList* list = read_list(lists, name);
    if (list == nullptr)
        return false;
    for (uint32_t i = 0; i < list->count; ++i)
        if (list->ids[i] == user_id)
            return true;
    return false;
}

void people_list_insert(PeopleLists* lists, const char* name, int32_t user_id) {
    if (PeopleList* list = read_list(lists, name))
        append_id(list, user_id);
}

bool people_list_erase(PeopleLists* lists, const char* name, int32_t user_id) {
    if (!people_list_contains(lists, name, user_id))
        return false;
    PeopleList* list = people_lists_find_or_add(lists, name);
    uint32_t kept = 0;
    for (uint32_t i = 0; i < list->count; ++i)
        if (list->ids[i] != user_id)
            list->ids[kept++] = list->ids[i];
    list->count = kept;
    return true;
}

bool people_list_load(PeopleLists* lists, const char* name) {
    PeopleList* list = people_lists_find_or_add(lists, name);
    if (list == nullptr)
        return false;
    list->loaded = 1;
    std::FILE* file = oa::platform::open_file(
        rover_session_placed_path(list_relative(name).c_str()).c_str(), "rb"
    );
    if (file == nullptr)
        return false;
    int32_t user_id = 0;
    for (;;) {
        const int read = read_user_id(file, &user_id);
        if (read == 0 || read == EOF)
            break;
        append_id(list, user_id);
    }
    std::fclose(file);
    return true;
}

bool people_list_save(PeopleLists* lists, const char* name) {
    const RoverPlatform* platform = rover_session_platform();
    if (platform == nullptr || platform->file_create == nullptr ||
        platform->file_write == nullptr || platform->file_close == nullptr)
        return false;
    RoverFile* file = platform->file_create(list_relative(name).c_str());
    if (file == nullptr)
        return false;
    bool written = false;
    const PeopleList* list = people_lists_find_or_add(lists, name);
    if (list != nullptr && list->loaded != 0) {
        for (uint32_t i = 0; i < list->count; ++i) {
            char line[32];
            const int length = std::snprintf(line, sizeof line, kIdLineFormat, list->ids[i]);
            if (length < 0 || platform->file_write(file, line, length) != length) {
                platform->file_close(file);
                return false;
            }
        }
        written = true;
    }
    platform->file_close(file);
    return written;
}

bool people_lists_save_all(PeopleLists* lists) {
    for (uint32_t i = 0; i < lists->count; ++i)
        (void)people_list_save(lists, lists->lists[i].name);
    return true;
}

} // namespace oa::boneyards
