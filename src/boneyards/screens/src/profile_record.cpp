// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Profile record readers: the profile_info and ladder_profile replies read
// into a player's record, and the ladder, awards, pop-up and Boneyards-info
// pages built from them and Profile.def's templates.
#include "oa/boneyards/screens/profile.hpp"

#include "text.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

namespace oa::boneyards::screens {
namespace {

constexpr const char* kPageOpen = "<html><BODY>";
constexpr const char* kPageClose = "<P></BODY></html>";
constexpr const char* kAwardOpen = "<font size=+1>";
constexpr const char* kAwardClose = "</font><BR><P>";

constexpr const char* kUserIdKey = "user_id";
constexpr const char* kLoginNameKey = "LOGINNAME";
constexpr const char* kIconKey = "ICON";
constexpr const char* kAllegianceKey = "ALLEGIANCE";
constexpr const char* kAllegianceLockedKey = "ALLEGIANCE_LOCKED";
constexpr const char* kClanKey = "CLAN";
constexpr const char* kUserStatusKey = "USER_STATUS";
constexpr const char* kEmailKey = "EMAIL";
constexpr const char* kTagKey = "TAG";
constexpr const char* kCoreContingencyKey = "HAS_CC";
constexpr const char* kBattleTacticsKey = "HAS_BT";
constexpr const char* kForumAliasKey = "TAFORUM_ALIAS";
constexpr const char* kEmailAliasKey = "EMAIL_ALIAS";
constexpr const char* kIcqKey = "ICQ_ALIAS";
constexpr const char* kPlayersKey = "NUMPLAYERS";
constexpr const char* kPlayersTextKey = "NUMPLAYERS_TEXT";
constexpr const char* kSkillKey = "PLAYERSKILL";
constexpr const char* kSkillTextKey = "PLAYERSKILL_TEXT";
constexpr const char* kDeathKey = "ONCOMDEATH";
constexpr const char* kDeathTextKey = "ONCMDDEATH_TEXT";
constexpr const char* kFavoriteMapKey = "FAVORITEMAP";
constexpr const char* kAwardKeyFormat = "AWARD_%d";

constexpr const char* kYes = "Yes";
// A reply field holding a single space stands for an empty one.
constexpr const char* kBlankField = " ";
constexpr const char* kPlayersUnset = "1";
constexpr const char* kDoesntMatter = "Doesn't Matter";

// PLAYERSKILL values.
constexpr int32_t kSkillNovice = 1;
constexpr int32_t kSkillSeasoned = 2;
constexpr int32_t kSkillExpert = 3;

// ONCOMDEATH values (first character).
constexpr char kDeathContinues = '0';
constexpr char kDeathEnds = '1';
constexpr char kDeathmatch = '2';

char* copy_text(const char* text) {
    const std::size_t length = std::strlen(text);
    char* copy = static_cast<char*>(std::malloc(length + 1));
    if (copy != nullptr)
        std::memcpy(copy, text, length + 1);
    return copy;
}

void free_text(const char** slot) {
    std::free(const_cast<char*>(*slot));
    *slot = nullptr;
}

void store_text(const char** slot, const char* text) {
    free_text(slot);
    *slot = copy_text(text);
}

// Reads a text field; `out` (kProfileTextCapacity bytes) keeps its contents
// when the field is absent.
bool read_text(const ProfileFields* f, const char* key, char* out) {
    char text[kProfileTextCapacity];
    if (f->get_string == nullptr || !f->get_string(f->user, key, text, sizeof text))
        return false;
    copy_bounded(out, kProfileTextCapacity, text);
    return true;
}

// An integer field, or `fallback` when it is absent.
int32_t read_int(const ProfileFields* f, const char* key, int32_t fallback) {
    int32_t value = 0;
    if (f->get_int == nullptr || !f->get_int(f->user, key, &value))
        return fallback;
    return value;
}

void write_text(const ProfileFields* f, const char* key, const char* value) {
    if (f->set_string != nullptr)
        f->set_string(f->user, key, value);
}

/// Reads a text field into a record string; a single space reads as empty and is written back
/// empty, so the page templates show it empty too.
///
/// @param f the reply's fields
/// @param key field name
/// @param[in,out] slot record string replaced; left as it is when the field is absent
void read_blankable_text(const ProfileFields* f, const char* key, const char** slot) {
    char text[kProfileTextCapacity];
    if (!read_text(f, key, text))
        return;
    if (std::strcmp(text, kBlankField) == 0) {
        text[0] = '\0';
        write_text(f, key, "");
    }
    store_text(slot, text);
}

char side_code(const char* allegiance) {
    switch (allegiance[0]) {
    case 'A':
    case 'a':
        return 'A';
    case 'C':
    case 'c':
        return 'C';
    default:
        return 'U';
    }
}

const char* skill_text(int32_t skill) {
    switch (skill) {
    case kSkillNovice:
        return "Novice";
    case kSkillSeasoned:
        return "Seasoned";
    case kSkillExpert:
        return "Expert";
    default:
        return "Don't Know";
    }
}

const char* death_text(char death) {
    switch (death) {
    case kDeathContinues:
        return "Continues";
    case kDeathEnds:
        return "Ends";
    case kDeathmatch:
        return "Deathmatch";
    default:
        return kDoesntMatter;
    }
}

// Expands <$KEY|default$> tags from the reply's fields.
struct FieldLookup {
    const ProfileFields* fields;
    char value[kProfileTextCapacity];
};

bool lookup_field(void* context, const char* tag, const char** value) {
    auto* lookup = static_cast<FieldLookup*>(context);
    const ProfileFields* f = lookup->fields;
    if (f->get_string == nullptr ||
        !f->get_string(f->user, tag, lookup->value, sizeof lookup->value))
        return false;
    *value = lookup->value;
    return true;
}

// A template expanded with the fields into a heap page: measured, allocated,
// then written; null when the template is null or the expansion fails.
char* expand_page(const char* page, const ProfileFields* f) {
    if (page == nullptr)
        return nullptr;
    FieldLookup lookup{};
    lookup.fields = f;
    std::size_t size = 0;
    if (!expand_template_tags(nullptr, &size, page, lookup_field, &lookup))
        return nullptr;
    char* out = static_cast<char*>(std::malloc(size));
    if (out == nullptr)
        return nullptr;
    if (!expand_template_tags(out, &size, page, lookup_field, &lookup)) {
        std::free(out);
        return nullptr;
    }
    return out;
}

/// Starts a ladder page from the first ladder template of a type and subtype.
///
/// Takes the template's sort order and expands its page with the ladder's
/// fields.
///
/// @param[out] page page to fill; no match leaves sort 0 and no text
/// @param t Profile.def's tables; may be null
/// @param type ladder type
/// @param subtype ladder subtype
/// @param f the ladder standing's fields
/// @return `page`
LadderPage* ladder_page_init(
    LadderPage* page,
    const ProfileTemplates* t,
    int32_t type,
    int32_t subtype,
    const ProfileFields* f
) {
    page->sort = 0;
    page->html = nullptr;
    if (t == nullptr)
        return page;
    for (int32_t i = 0; i < t->ladder_count; ++i) {
        const LadderTemplate* ladder = t->ladders[i];
        if (ladder->type != type || ladder->subtype != subtype)
            continue;
        page->sort = ladder->sort;
        page->html = expand_page(ladder->page, f);
        break;
    }
    return page;
}

/// Frees a ladder page's text.
///
/// @param[in,out] page ladder page
void ladder_page_release(LadderPage* page) {
    std::free(page->html);
    page->html = nullptr;
}

bool add_ladder_page(ProfileRecord* r, const LadderPage* page) {
    if (r->ladder_page_count == r->ladder_page_capacity) {
        const int32_t capacity = r->ladder_page_capacity > 0 ? r->ladder_page_capacity * 2 : 4;
        auto* grown = static_cast<LadderPage*>(
            std::realloc(r->ladder_pages, sizeof(LadderPage) * static_cast<std::size_t>(capacity))
        );
        if (grown == nullptr)
            return false;
        r->ladder_pages = grown;
        r->ladder_page_capacity = capacity;
    }
    r->ladder_pages[r->ladder_page_count++] = *page;
    return true;
}

/// Orders a record's ladder pages by their templates' sort order, keeping arrival order among equal
/// ones.
///
/// @param[in,out] r profile record
void sort_ladder_pages(ProfileRecord* r) {
    for (int32_t i = 1; i < r->ladder_page_count; ++i) {
        const LadderPage page = r->ladder_pages[i];
        int32_t at = i;
        for (; at > 0 && page.sort < r->ladder_pages[at - 1].sort; --at)
            r->ladder_pages[at] = r->ladder_pages[at - 1];
        r->ladder_pages[at] = page;
    }
}

// The ladder page: every ladder page's text in sort order.
char* join_ladder_pages(ProfileRecord* r) {
    std::size_t size = std::strlen(kPageOpen) + std::strlen(kPageClose) + 1;
    for (int32_t i = 0; i < r->ladder_page_count; ++i)
        size += std::strlen(r->ladder_pages[i].html);
    char* page = static_cast<char*>(std::malloc(size));
    if (page == nullptr)
        return nullptr;
    std::size_t used = static_cast<std::size_t>(std::snprintf(page, size, "%s", kPageOpen));
    sort_ladder_pages(r);
    for (int32_t i = 0; i < r->ladder_page_count; ++i) {
        used += static_cast<std::size_t>(
            std::snprintf(page + used, size - used, "%s", r->ladder_pages[i].html)
        );
    }
    std::snprintf(page + used, size - used, "%s", kPageClose);
    return page;
}

// The AWARD_1, AWARD_2, ... texts up to the first one absent.
struct AwardList {
    char** texts;
    int32_t count;
    int32_t capacity;
};

/// Appends a copy of an award text to the list.
///
/// @param[in,out] list award list
/// @param text award text
/// @return false when memory runs out
bool add_award(AwardList* list, const char* text) {
    if (list->count == list->capacity) {
        const int32_t capacity = list->capacity > 0 ? list->capacity * 2 : 8;
        auto* grown = static_cast<char**>(
            std::realloc(list->texts, sizeof(char*) * static_cast<std::size_t>(capacity))
        );
        if (grown == nullptr)
            return false;
        list->texts = grown;
        list->capacity = capacity;
    }
    char* copy = copy_text(text);
    if (copy == nullptr)
        return false;
    list->texts[list->count++] = copy;
    return true;
}

/// Frees the award texts and empties the list.
///
/// @param[in,out] list award list
void release_awards(AwardList* list) {
    for (int32_t i = 0; i < list->count; ++i)
        std::free(list->texts[i]);
    std::free(list->texts);
    *list = AwardList{};
}

void read_awards(AwardList* list, const ProfileFields* f) {
    char key[0x20];
    char text[kProfileTextCapacity];
    for (int32_t i = 1;; ++i) {
        std::snprintf(key, sizeof key, kAwardKeyFormat, i);
        text[0] = '\0';
        if (!read_text(f, key, text))
            return;
        if (!add_award(list, text))
            return;
    }
}

// The awards page: each award in the large font on a paragraph of its own.
char* awards_page(const AwardList* list) {
    const std::size_t item = std::strlen(kAwardOpen) + std::strlen(kAwardClose);
    std::size_t size = std::strlen(kPageOpen) + std::strlen(kPageClose) + 1;
    for (int32_t i = 0; i < list->count; ++i)
        size += std::strlen(list->texts[i]) + item;
    char* page = static_cast<char*>(std::malloc(size));
    if (page == nullptr)
        return nullptr;
    std::size_t used = static_cast<std::size_t>(std::snprintf(page, size, "%s", kPageOpen));
    for (int32_t i = 0; i < list->count; ++i) {
        used += static_cast<std::size_t>(std::snprintf(page + used, size - used, "%s", kAwardOpen));
        used +=
            static_cast<std::size_t>(std::snprintf(page + used, size - used, "%s", list->texts[i]));
        used +=
            static_cast<std::size_t>(std::snprintf(page + used, size - used, "%s", kAwardClose));
    }
    std::snprintf(page + used, size - used, "%s", kPageClose);
    return page;
}

void read_expansion(const ProfileFields* f, const char* key, bool* is_yes, bool* is_set) {
    char text[kProfileTextCapacity] = "";
    read_text(f, key, text);
    *is_yes = std::strcmp(text, kYes) == 0;
    *is_set = text[0] != '\0';
}

// NUMPLAYERS as the preferred player count; "1" (also when absent) reads as
// no preference. A given value is stored back as NUMPLAYERS_TEXT.
void read_players(ProfileRecord* r, const ProfileFields* f) {
    char text[kProfileTextCapacity];
    copy_bounded(text, sizeof text, kPlayersUnset);
    const bool given = read_text(f, kPlayersKey, text);
    if (std::strcmp(text, kPlayersUnset) == 0)
        copy_bounded(text, sizeof text, kDoesntMatter);
    store_text(&r->preferred_players, text);
    r->players_given = given;
    if (given)
        write_text(f, kPlayersTextKey, text);
}

void read_skill(ProfileRecord* r, const ProfileFields* f) {
    char text[kProfileTextCapacity] = "";
    const bool given = read_text(f, kSkillKey, text);
    store_text(&r->preferred_skill, skill_text(std::atoi(text)));
    r->skill_given = given;
    if (given)
        write_text(f, kSkillTextKey, r->preferred_skill);
}

void read_death(ProfileRecord* r, const ProfileFields* f) {
    char text[kProfileTextCapacity] = "";
    const bool given = read_text(f, kDeathKey, text);
    store_text(&r->preferred_death, death_text(text[0]));
    r->death_given = given;
    if (given)
        write_text(f, kDeathTextKey, r->preferred_death);
}

void replace_page(const char** slot, char* page) {
    free_text(slot);
    *slot = page;
}

} // namespace

void profile_record_release(ProfileRecord* r) {
    for (int32_t i = 0; i < r->ladder_page_count; ++i)
        ladder_page_release(&r->ladder_pages[i]);
    std::free(r->ladder_pages);
    for (const char** slot :
         {&r->ladder_html,
          &r->awards_html,
          &r->general_html,
          &r->info_html,
          &r->clan,
          &r->email,
          &r->tag_line,
          &r->forum_alias,
          &r->email_alias,
          &r->icq,
          &r->preferred_skill,
          &r->preferred_players,
          &r->preferred_death,
          &r->favorite_map})
        free_text(slot);
    profile_record_init(r);
}

void profile_from_fields(
    ProfileRecord* r, const ProfileTemplates* templates, const ProfileFields* f
) {
    uint32_t user_id = 0;
    if (f->get_uint != nullptr && f->get_uint(f->user, kUserIdKey, &user_id))
        r->user_id = static_cast<int32_t>(user_id);

    char text[kProfileTextCapacity] = "";
    read_text(f, kLoginNameKey, text);
    copy_chars(r->name, sizeof r->name, text, kProfileNameCharacters);
    r->user_icon = read_int(f, kIconKey, 0);
    text[0] = '\0';
    read_text(f, kAllegianceKey, text);
    r->allegiance = side_code(text);
    r->allegiance_locked = -1;
    if (f->get_int != nullptr) {
        int32_t locked = 0;
        if (f->get_int(f->user, kAllegianceLockedKey, &locked))
            r->allegiance_locked = locked != 0 ? 1 : 0;
    }
    read_blankable_text(f, kClanKey, &r->clan);
    r->status = read_int(f, kUserStatusKey, 0);
    if (read_text(f, kEmailKey, text))
        store_text(&r->email, text);
    read_blankable_text(f, kTagKey, &r->tag_line);
    read_expansion(f, kCoreContingencyKey, &r->has_core_contingency, &r->core_contingency_set);
    read_expansion(f, kBattleTacticsKey, &r->has_battle_tactics, &r->battle_tactics_set);
    read_blankable_text(f, kForumAliasKey, &r->forum_alias);
    read_blankable_text(f, kEmailAliasKey, &r->email_alias);
    read_blankable_text(f, kIcqKey, &r->icq);
    read_players(r, f);
    read_skill(r, f);
    read_death(r, f);
    read_blankable_text(f, kFavoriteMapKey, &r->favorite_map);

    free_text(&r->ladder_html);
    if (r->status != static_cast<int32_t>(ProfileStatus::admin) && r->ladder_page_count != 0)
        r->ladder_html = join_ladder_pages(r);

    AwardList awards{};
    read_awards(&awards, f);
    replace_page(&r->awards_html, awards.count != 0 ? awards_page(&awards) : nullptr);
    release_awards(&awards);

    replace_page(
        &r->general_html, expand_page(templates != nullptr ? templates->popup_page : nullptr, f)
    );
    replace_page(
        &r->info_html, expand_page(templates != nullptr ? templates->byinfo_page : nullptr, f)
    );
}

bool ladder_profile_apply(
    ProfileRecord* r,
    const ProfileTemplates* templates,
    int32_t ladder_id,
    int32_t ladder_subid,
    const ProfileFields* f
) {
    LadderPage page{};
    ladder_page_init(&page, templates, ladder_id, ladder_subid, f);
    if (page.html == nullptr)
        return false;
    if (!add_ladder_page(r, &page)) {
        ladder_page_release(&page);
        return false;
    }
    return true;
}

} // namespace oa::boneyards::screens
