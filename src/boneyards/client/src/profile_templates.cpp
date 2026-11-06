// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Profile.def's tables: ladder page templates, category texts and the two
// player-page templates, filed record by record from Rover's parser.
#include "oa/boneyards/profile_templates.hpp"

#include "oa/boneyards/rover_calls.hpp"
#include "oa/boneyards/rover_session.hpp"
#include "oa/boneyards/rover_string.hpp"

#include <cstdlib>
#include <cstring>
#include <vector>

namespace oa::boneyards {
namespace {

constexpr const char* kLadderTemplate = "ladder_template";
constexpr const char* kCategoryInfo = "profile_category_info";
constexpr const char* kByinfoTemplate = "profile_byinfo_template";
constexpr const char* kPopupTemplate = "profile_popup_template";

constexpr const char* kTypeKey = "type";
constexpr const char* kSubtypeKey = "subtype";
constexpr const char* kDescriptionKey = "description";

// A text-mode read ends the file at this byte.
constexpr char kTextEnd = 0x1a;

ProfileTemplates g_templates{};

// A whole file read in text mode ("r"): CR LF becomes LF and a Ctrl-Z ends
// the text. The path is the one the game names, read through the session
// (the player's copy, then the game's files, then the install). Null when
// the file cannot be opened.
char* read_text_file(const char* path) {
    std::vector<uint8_t> bytes;
    if (!rover_session_read_file(path, &bytes))
        return nullptr;
    char* text = static_cast<char*>(std::malloc(bytes.size() + 1));
    if (text == nullptr)
        return nullptr;
    std::size_t kept = 0;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        const char c = static_cast<char>(bytes[i]);
        if (c == kTextEnd)
            break;
        if (c == '\r' && i + 1 < bytes.size() && bytes[i + 1] == '\n')
            continue;
        text[kept++] = c;
    }
    text[kept] = '\0';
    return text;
}

char* copy_text(const char* text) {
    const std::size_t length = std::strlen(text);
    char* copy = static_cast<char*>(std::malloc(length + 1));
    if (copy != nullptr)
        std::memcpy(copy, text, length + 1);
    return copy;
}

void replace_text(char** slot, char* text) {
    std::free(*slot);
    *slot = text;
}

void add_ladder(ProfileTemplates* t, LadderTemplate* ladder) {
    if (t->ladder_count == t->ladder_capacity) {
        const int32_t capacity = t->ladder_capacity > 0 ? t->ladder_capacity * 2 : 4;
        auto* grown = static_cast<LadderTemplate**>(
            std::realloc(t->ladders, sizeof(LadderTemplate*) * static_cast<std::size_t>(capacity))
        );
        if (grown == nullptr) {
            std::free(ladder->page);
            std::free(ladder);
            return;
        }
        t->ladders = grown;
        t->ladder_capacity = capacity;
    }
    t->ladders[t->ladder_count++] = ladder;
}

} // namespace

LadderTemplate* ladder_template_init(
    LadderTemplate* ladder, int32_t type, int32_t subtype, int32_t sort, const char* file
) {
    ladder->page = nullptr;
    ladder->type = type;
    ladder->subtype = subtype;
    ladder->sort = sort;
    ladder->page = read_text_file(file);
    return ladder;
}

bool ROVER_CALL profile_template_record(
    const char* command,
    const char* id,
    const char* name,
    const char* sort,
    const char*,
    const char*,
    const char* file,
    RoverPropertyBag* props,
    void* user
) {
    auto* t = static_cast<ProfileTemplates*>(user);
    if (std::strcmp(command, kLadderTemplate) == 0) {
        int32_t type = 0;
        rover_call_get_field_int(props, kTypeKey, &type);
        int32_t subtype = 0;
        rover_call_get_field_int(props, kSubtypeKey, &subtype);
        auto* ladder = static_cast<LadderTemplate*>(std::malloc(sizeof(LadderTemplate)));
        if (ladder == nullptr)
            return true;
        ladder_template_init(ladder, type, subtype, std::atoi(sort), file);
        if (ladder->page != nullptr)
            add_ladder(t, ladder);
        else
            std::free(ladder);
    } else if (std::strcmp(command, kCategoryInfo) == 0) {
        // A record whose id is outside 0..6 is dropped.
        const int32_t index = std::atoi(id);
        if (index < 0 || index >= static_cast<int32_t>(kProfileCategoryCount))
            return true;
        replace_text(&t->category_names[index], copy_text(name));
        RoverString description{};
        rover_call_get_field_string(props, kDescriptionKey, &description);
        replace_text(&t->category_descriptions[index], copy_text(rover_string_text(&description)));
        rover_string_release(&description, true);
    } else if (std::strcmp(command, kByinfoTemplate) == 0) {
        replace_text(&t->byinfo_page, read_text_file(file));
    } else if (std::strcmp(command, kPopupTemplate) == 0) {
        replace_text(&t->popup_page, read_text_file(file));
    }
    return true;
}

const ProfileTemplates* profile_templates_load() {
    ProfileTemplates* t = &g_templates;
    if (!t->loaded) {
        t->loaded = true;
        rover_call_parse_record_file(kProfileDefinitionsPath, profile_template_record, t);
    }
    return t;
}

void profile_templates_release() {
    ProfileTemplates* t = &g_templates;
    for (int32_t i = 0; i < t->ladder_count; ++i) {
        std::free(t->ladders[i]->page);
        std::free(t->ladders[i]);
    }
    std::free(t->ladders);
    for (std::size_t i = 0; i < kProfileCategoryCount; ++i) {
        std::free(t->category_names[i]);
        std::free(t->category_descriptions[i]);
    }
    std::free(t->byinfo_page);
    std::free(t->popup_page);
    *t = ProfileTemplates{};
}

} // namespace oa::boneyards
