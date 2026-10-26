// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Campaign/mission-info object: campaign TDF loading, mission selection and
// the per-mission OTA header, briefing and placement tables.
#include "oa/data/campaign/campaign_file.hpp"
#include "oa/data/defs/layout.hpp"
#include "oa/data/map_pack/map_name.hpp"

#include "oa/core/game_state.h"
#include "oa/base/text.hpp"
#include "oa/data/languages/translation.hpp"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string_view>

namespace oa::data::campaign {
namespace {

constexpr const char* kCampsDirectory = "camps";
constexpr const char* kMapsDirectory = "Maps";
constexpr const char* kBriefsDirectory = "camps/briefs";
constexpr const char* kHintsDirectory = "camps/hints";
constexpr const char* kUseOnlyDirectory = "camps/useonly";
constexpr const char* kGlobalHeader = "GlobalHeader";
constexpr const char* kStartPos = "StartPos";
constexpr const char* kUnnamedMission = "Error -- Unnamed Mission";
constexpr const char* kAllSides = "ALL";
constexpr std::size_t kStartPosLength = 8;
constexpr int32_t kDefaultUnitsPerPlayer = 200;
constexpr uint32_t kUnitStringLimit = 0x400;
constexpr uint32_t kPoolGranule = 0x400;
constexpr int32_t kMaxSchemas = 64;

// Copies at most capacity - 1 characters of text into out and fills the rest
// of out with zero bytes, so out always ends in one.
void copy_bounded(char* out, std::size_t capacity, const char* text) noexcept {
    if (capacity == 0)
        return;
    if (text == nullptr)
        text = "";
    std::size_t length = 0;
    while (length + 1 < capacity && text[length] != '\0')
        ++length;
    std::memcpy(out, text, length);
    std::memset(out + length, 0, capacity - length);
}

/// Copies a text as copy_bounded does, cutting it between whole UTF-8
/// characters (oa::base::text::whole_characters).
///
/// @param[out] out the field
/// @param capacity its bytes, its NUL included
/// @param text the text
void copy_whole_characters(char* out, std::size_t capacity, std::string_view text) noexcept {
    if (capacity == 0)
        return;
    const std::size_t kept = oa::base::text::whole_characters(text, capacity - 1);
    std::memcpy(out, text.data(), kept);
    std::memset(out + kept, 0, capacity - kept);
}

bool prefix_nocase(const char* text, const char* prefix, std::size_t length) noexcept {
    for (std::size_t i = 0; i < length; ++i) {
        const int a = std::tolower(static_cast<unsigned char>(text[i]));
        const int b = std::tolower(static_cast<unsigned char>(prefix[i]));
        if (a != b)
            return false;
        if (a == 0)
            return true;
    }
    return true;
}

void message(const CampaignFiles* files, const char* text) {
    if (files != nullptr && files->message != nullptr)
        files->message(files->context, text);
}

const char* translate(const CampaignFiles* files, const char* text) {
    if (files == nullptr || files->translate == nullptr)
        return text;
    const char* translated = files->translate(files->context, text);
    return translated != nullptr ? translated : text;
}

int32_t file_size(const CampaignFiles* files, const char* path) {
    if (files == nullptr || files->size == nullptr || path == nullptr || path[0] == '\0')
        return -1;
    return files->size(files->context, path);
}

// Strips a trailing extension from the last path component.
void remove_extension(char* path) noexcept {
    char* dot = std::strrchr(path, '.');
    const char* slash = std::strrchr(path, '/');
    if (dot != nullptr && (slash == nullptr || dot > slash))
        *dot = '\0';
}

// A non-empty extension replaces the one the name carries.
void replace_extension(char* path, std::size_t capacity, const char* extension) noexcept {
    if (extension == nullptr || extension[0] == '\0')
        return;
    remove_extension(path);
    oa::base::text::append_terminated(std::span(path, capacity), ".");
    oa::base::text::append_terminated(std::span(path, capacity), extension);
}

} // namespace

void build_variant_path(
    const CampaignFiles* files,
    char* out,
    std::size_t capacity,
    const char* directory,
    const char* name,
    const char* extension
) {
    out[0] = '\0';
    if (name == nullptr || name[0] == '\0')
        return;
    const char* separator = directory[0] != '\0' ? "/" : "";
    if (files != nullptr && files->language != nullptr && files->language[0] != '\0' &&
        directory[0] != '\0') {
        std::snprintf(out, capacity, "%s-%s/%s", directory, files->language, name);
        replace_extension(out, capacity, extension);
        if (file_size(files, out) >= 0)
            return;
    }
    std::snprintf(out, capacity, "%s%s%s", directory, separator, name);
    replace_extension(out, capacity, extension);
}

bool load_tdf(
    const CampaignFiles* files,
    oa::formats::tdf::Document* document,
    const char* path,
    oa::formats::tdf::ParseError* error
) {
    oa::formats::tdf::document_free(document);
    oa::formats::tdf::document_init(document);
    if (error != nullptr)
        *error = {};
    const int32_t size = file_size(files, path);
    if (size >= 0 && static_cast<uint32_t>(size) > oa::formats::tdf::max_input_bytes) {
        if (error != nullptr)
            error->status = oa::formats::tdf::ParseStatus::too_large;
        return false;
    }
    if (size < 0 || files->read == nullptr)
        return false;
    char* text = static_cast<char*>(std::malloc(static_cast<std::size_t>(size) + 1));
    if (text == nullptr)
        return false;
    const int32_t read = files->read(files->context, path, text, static_cast<uint32_t>(size));
    bool ok = false;
    if (read >= 0) {
        text[read] = '\0';
        ok =
            oa::formats::tdf::parse_text(document, text, static_cast<uint32_t>(read), false, error);
    }
    std::free(text);
    return ok;
}

namespace {

bool select_mission_section(oa::formats::tdf::Document* document, int32_t index) noexcept {
    char key[32];
    std::snprintf(key, sizeof(key), "MISSION%d", index);
    oa::formats::tdf::reset_cursor(document);
    return oa::formats::tdf::select_section(document, key);
}

/// Copies a path into one slot of the resolved-path table.
///
/// @param[in,out] file campaign object
/// @param files file services, for the map file size
/// @param slot path slot; the mission slot also records the file's size
/// @param path path to store; empty clears the slot
void set_path(CampaignFile* file, const CampaignFiles* files, CampaignPath slot, const char* path) {
    const auto index = static_cast<uint32_t>(slot);
    copy_bounded(file->paths[index], kCampaignPathBytes, path);
    if (slot == CampaignPath::mission) {
        const int32_t size = path[0] == '\0' ? 0 : file_size(files, path);
        file->mission_file_size = size < 0 ? 0 : size;
    }
}

void resolve_path(
    CampaignFile* file,
    const CampaignFiles* files,
    CampaignPath slot,
    const char* directory,
    const char* name,
    const char* extension
) {
    char path[kCampaignPathBytes];
    build_variant_path(files, path, sizeof(path), directory, name, extension);
    set_path(file, files, slot, path);
}

/// Frees the old briefing text and reads the briefing: a mod's language
/// pack's in the language, else the game data's in the language, else the
/// player's or the engine's pack's in it, else the file the briefing path
/// names (oa/data/languages/translation.hpp).
///
/// @param[in,out] file campaign object
/// @param files file services
/// @param name the briefing's name, as the mission names it
void load_briefing_text(CampaignFile* file, const CampaignFiles* files, const char* name) {
    std::free(file->briefing_text);
    file->briefing_text = nullptr;
    const char* path = campaign_path(file, CampaignPath::briefing);
    if (path == nullptr)
        return;
    char language_path[kCampaignPathBytes]{};
    if (files != nullptr && files->language != nullptr && files->language[0] != '\0' &&
        name != nullptr && name[0] != '\0') {
        // A path too long for the buffer names no language's briefing.
        const int written = std::snprintf(
            language_path, sizeof language_path, "%s-%s/%s", kBriefsDirectory, files->language, name
        );
        if (written > 0 && static_cast<std::size_t>(written) < sizeof language_path)
            replace_extension(language_path, sizeof language_path, "TXT");
        else
            language_path[0] = '\0';
    }
    std::string_view pack_text{};
    if (language_path[0] != '\0') {
        pack_text = oa::data::languages::installed_language_file(language_path, true);
        if (pack_text.data() == nullptr && std::strcmp(path, language_path) != 0)
            pack_text = oa::data::languages::installed_language_file(language_path, false);
    }
    if (pack_text.data() != nullptr) {
        auto* text = static_cast<char*>(std::malloc(pack_text.size() + 1));
        if (text == nullptr)
            return;
        std::memcpy(text, pack_text.data(), pack_text.size());
        text[pack_text.size()] = '\0';
        file->briefing_text = text;
        return;
    }
    const int32_t size = file_size(files, path);
    if (size <= 0 || static_cast<uint32_t>(size) > oa::formats::tdf::max_input_bytes ||
        files->read == nullptr)
        return;
    auto* text = static_cast<char*>(std::malloc(static_cast<std::size_t>(size) + 1));
    if (text == nullptr)
        return;
    const int32_t read = files->read(files->context, path, text, static_cast<uint32_t>(size));
    text[read > 0 ? read : 0] = '\0';
    file->briefing_text = text;
}

int32_t count_start_positions(const oa::formats::tdf::Block* specials) {
    int32_t count = 0;
    for (uint32_t i = 0; i < oa::formats::tdf::child_count(specials); ++i) {
        char what[0x10];
        if (oa::formats::tdf::get_string(
                oa::formats::tdf::child_at(specials, i), "specialwhat", what, sizeof(what), ""
            ) &&
            prefix_nocase(what, kStartPos, kStartPosLength))
            ++count;
    }
    return count;
}

} // namespace

bool find_matching_schema(
    SessionKind kind,
    oa::formats::tdf::Document* ota,
    int32_t difficulty,
    int32_t players,
    char* schema_name,
    std::size_t capacity,
    const match_rules::AiDifficultyNames& names
) {
    static constexpr const char* kDifficultyTypes[] = {"Easy", "Medium", "Hard"};
    const char* const kTypes[] = {
        kDifficultyTypes[static_cast<std::size_t>(names.names[0])],
        kDifficultyTypes[static_cast<std::size_t>(names.names[1])],
        kDifficultyTypes[static_cast<std::size_t>(names.names[2])],
        "Network 1",
        "Network 2",
        "Network 3",
        "Network 4"
    };
    int32_t order[4] = {-1, -1, -1, -1};
    if (kind == SessionKind::campaign) {
        if (difficulty == 0) {
            order[0] = 0, order[1] = 1, order[2] = 2;
        } else if (difficulty == 1) {
            order[0] = 1, order[1] = 0, order[2] = 2;
        } else if (difficulty == 2) {
            order[0] = 2, order[1] = 1, order[2] = 0;
        } else {
            return false;
        }
    } else if (kind == SessionKind::skirmish || kind == SessionKind::multiplayer) {
        order[0] = 3, order[1] = 4, order[2] = 5, order[3] = 6;
    } else {
        return false;
    }
    bool found = false;
    int32_t best_count = 0;
    const oa::formats::tdf::Block* best_block = nullptr;
    for (int32_t pass = 0; pass < 4 && order[pass] != -1; ++pass) {
        for (int32_t index = 0; index < kMaxSchemas; ++index) {
            oa::formats::tdf::reset_cursor(ota);
            if (!oa::formats::tdf::select_section(ota, kGlobalHeader))
                return false;
            char section[0x20];
            std::snprintf(section, sizeof(section), "Schema %i", index);
            if (!oa::formats::tdf::select_section(ota, section))
                break;
            char type[0x20];
            if (!oa::formats::tdf::get_string(
                    oa::formats::tdf::cursor(ota), "type", type, sizeof(type), ""
                ) ||
                oa::formats::tdf::compare_nocase(type, kTypes[order[pass]]) != 0)
                continue;
            if (kind == SessionKind::campaign) {
                copy_bounded(schema_name, capacity, section);
                return true;
            }
            if (schema_name == nullptr)
                return true;
            const oa::formats::tdf::Block* schema = oa::formats::tdf::cursor(ota);
            if (!oa::formats::tdf::select_section(ota, "specials"))
                continue;
            const int32_t count = count_start_positions(oa::formats::tdf::cursor(ota));
            if (count != 0 && (count == players || players == 0 ||
                               (best_count < count && best_count != players))) {
                best_count = count;
                best_block = schema;
                found = true;
                copy_bounded(schema_name, capacity, section);
            }
        }
    }
    if (best_block != nullptr)
        oa::formats::tdf::set_cursor(ota, best_block);
    return found;
}

namespace {

/// Reads `key` into `out` from the cursor block, or `fallback` when the key is absent.
///
/// @param document campaign TDF document whose cursor names the block
/// @param key key to read
/// @param[out] out the value, cut to `capacity - 1` characters; empty when both
///     the key and `fallback` are absent
/// @param capacity size of `out` in bytes
/// @param fallback text taken when the key is absent, or null
void cursor_string(
    oa::formats::tdf::Document* document,
    const char* key,
    char* out,
    std::size_t capacity,
    const char* fallback
) {
    out[0] = '\0';
    (void)oa::formats::tdf::get_string(
        oa::formats::tdf::cursor(document), key, out, capacity, fallback
    );
}

/// Reads a key in the game's language first, as 3.1c does for a mission's
/// name, briefing, narration and hint: "<language><key>", which wins when
/// present, even empty, then the key itself.
///
/// @param block the block read
/// @param language the language's word; null or empty reads the key alone
/// @param key key to read
/// @param[out] out the value, cut to `capacity - 1` characters
/// @param capacity size of `out` in bytes
/// @param fallback text taken when neither key is there, or null
/// @return true when either key was there
bool language_string(
    const oa::formats::tdf::Block* block,
    const char* language,
    const char* key,
    char* out,
    std::size_t capacity,
    const char* fallback
) {
    if (capacity == 0)
        return false;
    out[0] = '\0';
    if (language != nullptr && language[0] != '\0') {
        char prefixed[0x100];
        if (std::snprintf(prefixed, sizeof prefixed, "%s%s", language, key) <
                static_cast<int>(sizeof prefixed) &&
            oa::formats::tdf::get_string(block, prefixed, out, capacity, nullptr))
            return true;
    }
    return oa::formats::tdf::get_string(block, key, out, capacity, fallback);
}

/// Returns file services that read the game's rules from their own
/// folders: the language's folders hold only what players read and hear,
/// so that the language never changes a mission.
///
/// @param files the file services
/// @return a copy without the language's folders
CampaignFiles rule_files(const CampaignFiles* files) noexcept {
    CampaignFiles plain = files != nullptr ? *files : CampaignFiles{};
    plain.language = nullptr;
    return plain;
}

} // namespace

void campaign_file_init(CampaignFile* file) noexcept {
    std::memset(static_cast<void*>(file), 0, sizeof(*file));
    oa::formats::tdf::document_init(&file->campaign);
    file->tidal_strength = -1.0f;
}

void campaign_file_construct(CampaignFile* file, SessionKind kind, const CampaignEnv* env) {
    campaign_file_init(file);
    file->kind = kind;
    campaign_load_file(file, env, "");
}

void campaign_file_free(CampaignFile* file) noexcept {
    campaign_free_owned_blocks(file);
    oa::formats::tdf::document_free(&file->campaign);
    oa::formats::tdf::document_init(&file->campaign);
}

void campaign_session_record(const CampaignFile* file, int32_t record[4]) noexcept {
    record[1] = file->mapping;
    record[2] = file->line_of_sight;
    record[3] = kCampaignSessionEnabled;
    record[0] = kCampaignCommanderRule;
}

bool campaign_load_file(CampaignFile* file, const CampaignEnv* env, const char* name) {
    const CampaignFiles* files = env->files;
    oa::formats::tdf::document_free(&file->campaign);
    oa::formats::tdf::document_init(&file->campaign);
    copy_bounded(file->campaign_name, kCampaignNameBytes, name);
    for (uint32_t slot = 0; slot < kCampaignPathCount; ++slot)
        set_path(file, files, static_cast<CampaignPath>(slot), "");
    if (file->campaign_name[0] == '\0')
        return true;
    const CampaignFiles rules = rule_files(files);
    resolve_path(file, &rules, CampaignPath::campaign, kCampsDirectory, file->campaign_name, "TDF");
    const char* path = campaign_path(file, CampaignPath::campaign);
    if (!load_tdf(files, &file->campaign, path)) {
        char text[kCampaignPathBytes + 64];
        std::snprintf(
            text,
            sizeof(text),
            "The requested campaign file, %s, does not exist.",
            path != nullptr ? path : "(null)"
        );
        message(files, text);
        campaign_load_file(file, env, "");
        return false;
    }
    file->content_hash = 0;
    file->mission_index = 0;
    (void)campaign_load_mission_info(file, env, nullptr);
    return true;
}

const char* campaign_name_if_loaded(const CampaignFile* file) noexcept {
    return file->campaign_name[0] != '\0' ? file->campaign_name : nullptr;
}

const char* campaign_path(const CampaignFile* file, CampaignPath path) noexcept {
    const char* text = file->paths[static_cast<uint32_t>(path)];
    return text[0] != '\0' ? text : nullptr;
}

const char* campaign_briefing_text(const CampaignFile* file) noexcept {
    return file->briefing_text;
}

bool campaign_has_mission_name(const CampaignFile* file) noexcept {
    return file->mission_name[0] != '\0';
}

SessionKind campaign_kind(const CampaignFile* file) noexcept {
    return file->kind;
}

int32_t campaign_mission_file_size(const CampaignFile* file) noexcept {
    return file->mission_file_size;
}

const char* campaign_localized_name(const CampaignFile* file) noexcept {
    return file->localized_name;
}

const char* campaign_mission_name(const CampaignFile* file) noexcept {
    return file->mission_name;
}

int32_t campaign_mission_index(const CampaignFile* file) noexcept {
    return file->mission_index;
}

const char* campaign_planet(const CampaignFile* file) noexcept {
    return file->planet;
}

int32_t campaign_count_missions(CampaignFile* file) noexcept {
    if (file->campaign_name[0] == '\0')
        return 0;
    int32_t count = 0;
    while (count < kMaxCampaignMissions && select_mission_section(&file->campaign, count))
        ++count;
    return count;
}

int32_t campaign_load_mission_list(
    CampaignFile* file, char (*names)[kCampaignNameBytes], int32_t capacity
) noexcept {
    if (file->campaign_name[0] == '\0')
        return 0;
    const int32_t count = campaign_count_missions(file);
    for (int32_t index = 0; index < count; ++index) {
        if (!select_mission_section(&file->campaign, index))
            return 0;
        char name[kCampaignNameBytes];
        const bool named = oa::formats::tdf::get_string(
            oa::formats::tdf::cursor(&file->campaign), "missionname", name, sizeof(name), nullptr
        );
        if (names != nullptr && index < capacity)
            copy_bounded(names[index], kCampaignNameBytes, named ? name : kUnnamedMission);
    }
    return count;
}

bool campaign_mission_title(
    CampaignFile* file, int32_t index, const char* language, char* out, std::size_t capacity
) noexcept {
    if (capacity != 0)
        out[0] = '\0';
    if (capacity == 0 || file->campaign_name[0] == '\0' ||
        !select_mission_section(&file->campaign, index))
        return false;
    const oa::formats::tdf::Block* mission = oa::formats::tdf::cursor(&file->campaign);
    // The game data's own name in the language, then the language packs'
    // around it (oa/data/languages/translation.hpp).
    char own[kCampaignNameBytes]{};
    char prefixed[0x100];
    const bool has_own =
        language != nullptr && language[0] != '\0' &&
        std::snprintf(prefixed, sizeof prefixed, "%smissionname", language) <
            static_cast<int>(sizeof prefixed) &&
        oa::formats::tdf::get_string(mission, prefixed, own, sizeof own, nullptr) && own[0] != '\0';
    char mission_file[kCampaignPathBytes]{};
    if (language != nullptr && language[0] != '\0' &&
        oa::formats::tdf::get_string(
            mission, "missionfile", mission_file, sizeof mission_file, ""
        ) &&
        mission_file[0] != '\0')
        if (const char* text = oa::data::languages::installed_mission_text(
                mission_file, "missionname", has_own ? own : nullptr
            );
            text != nullptr && text[0] != '\0') {
            copy_whole_characters(out, capacity, text);
            return true;
        }
    (void)language_string(mission, language, "missionname", out, capacity, kUnnamedMission);
    return true;
}

int32_t campaign_load_mission_titles(
    CampaignFile* file, const char* language, char (*names)[kCampaignNameBytes], int32_t capacity
) noexcept {
    const int32_t count = campaign_load_mission_list(file, nullptr, 0);
    for (int32_t index = 0; index < count && index < capacity && names != nullptr; ++index)
        if (!campaign_mission_title(file, index, language, names[index], kCampaignNameBytes))
            return 0;
    return count;
}

bool campaign_mission_file(
    CampaignFile* file, int32_t index, char* out, std::size_t capacity
) noexcept {
    if (capacity != 0)
        out[0] = '\0';
    if (file->campaign_name[0] == '\0' || !select_mission_section(&file->campaign, index))
        return false;
    return oa::formats::tdf::get_string(
               oa::formats::tdf::cursor(&file->campaign), "missionfile", out, capacity, ""
           ) &&
           out[0] != '\0';
}

bool campaign_has_next_mission(CampaignFile* file, int32_t index) noexcept {
    return index < campaign_count_missions(file);
}

bool campaign_advance_next_mission(CampaignFile* file, const CampaignEnv* env) {
    const bool more = file->mission_index + 1 < campaign_count_missions(file);
    if (more) {
        file->content_hash = 0;
        ++file->mission_index;
        (void)campaign_load_mission_info(file, env, nullptr);
    }
    return more;
}

bool campaign_bind_mission(CampaignFile* file, const CampaignEnv* env, int32_t index) {
    file->content_hash = 0;
    file->mission_index = index;
    return campaign_load_mission_info(file, env, nullptr);
}

bool campaign_select_mission(CampaignFile* file, const CampaignEnv* env, const char* name) {
    file->content_hash = 0;
    if (file->kind == SessionKind::campaign) {
        auto* names = static_cast<char (*)[kCampaignNameBytes]>(
            std::calloc(kMaxCampaignMissions, kCampaignNameBytes)
        );
        if (names == nullptr)
            return false;
        const int32_t count = campaign_load_mission_list(file, names, kMaxCampaignMissions);
        for (int32_t index = 0; index < count && index < kMaxCampaignMissions; ++index) {
            if (oa::formats::tdf::compare_nocase(names[index], name) == 0) {
                std::free(names);
                file->content_hash = 0;
                file->mission_index = index;
                return campaign_load_mission_info(file, env, nullptr);
            }
        }
        std::free(names);
        return false;
    }
    if (file->kind != SessionKind::skirmish && file->kind != SessionKind::multiplayer)
        return false;
    const bool loaded = campaign_load_mission_info(file, env, name);
    // A pack map's files keep the stem. The mounted layer shows the OTA at
    // the name the battle room carries, stem@id. Players see the name the
    // pack's index gives that map, or the OTA's title when the index has no
    // entry. A map of the base game keeps its file name.
    if (name != nullptr && oa::data::map_pack::split_pack_map_name(name)) {
        if (file->localized_name[0] == '\0')
            copy_bounded(file->localized_name, kCampaignNameBytes, name);
        return loaded;
    }
    const char* language = env->files != nullptr ? env->files->language : nullptr;
    if (loaded && language != nullptr && language[0] != '\0' &&
        oa::formats::tdf::compare_nocase(language, "english") != 0) {
        char lowered[200];
        copy_bounded(lowered, sizeof(lowered), name);
        for (char* p = lowered; *p != '\0'; ++p)
            *p = static_cast<char>(std::tolower(static_cast<unsigned char>(*p)));
        copy_bounded(file->localized_name, 0xff, translate(env->files, lowered));
        if (oa::formats::tdf::compare_nocase(file->localized_name, name) != 0)
            return loaded;
    }
    copy_bounded(file->localized_name, kCampaignNameBytes, name);
    return loaded;
}

bool campaign_load_mission_info(CampaignFile* file, const CampaignEnv* env, const char* map_name) {
    const CampaignFiles* files = env->files;
    const CampaignFiles rules = rule_files(files);
    file->planet[0] = '\0';
    file->description[0] = '\0';
    file->surface_metal = -1;
    file->min_wind = -1;
    file->max_wind = -1;
    file->gravity = -1;
    file->tidal_strength = -1.0f;
    file->lava_world = 0;
    file->no_sea_level_trigger = 0;
    campaign_free_owned_blocks(file);

    oa::formats::tdf::Document ota;
    oa::formats::tdf::document_init(&ota);
    char text[kCampaignPathBytes * 2];
    char path[kCampaignPathBytes];
    char name[kCampaignPathBytes];
    bool ok = false;

    if (file->kind == SessionKind::campaign) {
        if (!select_mission_section(&file->campaign, file->mission_index)) {
            char section[32];
            std::snprintf(section, sizeof(section), "MISSION%d", file->mission_index);
            std::snprintf(
                text, sizeof(text), "The requested mission file, %s, does not exist.", section
            );
            message(files, text);
            oa::formats::tdf::document_free(&ota);
            return false;
        }
        const oa::formats::tdf::Block* mission = oa::formats::tdf::cursor(&file->campaign);
        (void)oa::formats::tdf::get_string(
            mission, "missionname", file->mission_name, kCampaignNameBytes, nullptr
        );
        if (!oa::formats::tdf::get_string(mission, "missionfile", name, sizeof(name), "")) {
            message(files, "Old TED format no longer supported!");
            oa::formats::tdf::document_free(&ota);
            return false;
        }
        // The two messages below name the mission as the campaign file
        // writes it, not the path searched.
        build_variant_path(&rules, path, sizeof(path), kMapsDirectory, name, "OTA");
        if (!load_tdf(files, &ota, path)) {
            std::snprintf(
                text,
                sizeof(text),
                "Hey, joker!  There is no mission defintion for this mission: %s",
                name
            );
            message(files, text);
            oa::formats::tdf::document_free(&ota);
            return false;
        }
        oa::formats::tdf::reset_cursor(&ota);
        if (!oa::formats::tdf::select_section(&ota, kGlobalHeader)) {
            std::snprintf(
                text,
                sizeof(text),
                "Hey, joker!  Mission file %s is corrupt (no header found).",
                name
            );
            message(files, text);
            oa::formats::tdf::document_free(&ota);
            return false;
        }
        file->units_per_player = oa::formats::tdf::get_int(
            oa::formats::tdf::cursor(&ota), "maxunits", kDefaultUnitsPerPlayer
        );
        if (env->game != nullptr)
            env->game->units_per_player = static_cast<uint16_t>(file->units_per_player);
        oa::formats::tdf::reset_cursor(&ota);
    } else if (file->kind == SessionKind::skirmish || file->kind == SessionKind::multiplayer) {
        file->mission_file_size = 0;
        file->localized_name[0] = '\0';
        copy_bounded(file->mission_name, kCampaignNameBytes, map_name);
        copy_bounded(name, sizeof(name), map_name);
        build_variant_path(&rules, path, sizeof(path), kMapsDirectory, name, "OTA");
        if (!load_tdf(files, &ota, path)) {
            oa::formats::tdf::document_free(&ota);
            return false;
        }
    } else {
        oa::formats::tdf::document_free(&ota);
        return false;
    }

    resolve_path(file, &rules, CampaignPath::mission, kMapsDirectory, name, "TNT");
    if (!oa::formats::tdf::select_section(&ota, kGlobalHeader)) {
        message(files, "No GlobalHeader block in mission file!");
        oa::formats::tdf::document_free(&ota);
        return false;
    }
    const oa::formats::tdf::Block* header = oa::formats::tdf::cursor(&ota);
    file->header_hash = header->body_hash;
    // What players read and hear comes from the language's folders and keys
    // first, as 3.1c reads it.
    const char* language = files != nullptr ? files->language : nullptr;
    // The title a pack map shows. The file name stays mission_name, which is
    // how the map is found again. Another language's own key wins, as a
    // briefing's does. Otherwise players see the name the installed pack's
    // index gives the map, and the OTA's missionname only when that index
    // has no entry. A missing title leaves the display name empty so the
    // caller can keep the name the battle room carries. The index is asked
    // only once the file has loaded, so a player who does not have the map
    // still sees that name.
    if ((file->kind == SessionKind::skirmish || file->kind == SessionKind::multiplayer) &&
        map_name != nullptr && oa::data::map_pack::split_pack_map_name(map_name)) {
        bool own_key = false;
        if (language != nullptr && language[0] != '\0') {
            char prefixed[0x100];
            if (std::snprintf(prefixed, sizeof prefixed, "%smissionname", language) <
                    static_cast<int>(sizeof prefixed) &&
                oa::formats::tdf::get_string(
                    header, prefixed, file->localized_name, kCampaignNameBytes, nullptr
                ))
                own_key = true;
        }
        if (!own_key) {
            const char* index_name = nullptr;
            if (env->pack_index_name != nullptr)
                index_name = env->pack_index_name(env->pack_index_context, map_name);
            if (index_name != nullptr && index_name[0] != '\0')
                copy_bounded(file->localized_name, kCampaignNameBytes, index_name);
            else
                (void)language_string(
                    header, nullptr, "missionname", file->localized_name, kCampaignNameBytes, ""
                );
        }
    }
    char value[kCampaignPathBytes];
    language_string(header, language, "brief", value, sizeof(value), "");
    resolve_path(file, files, CampaignPath::briefing, kBriefsDirectory, value, "TXT");
    load_briefing_text(file, files, value);
    language_string(header, language, "narration", value, sizeof(value), "");
    resolve_path(file, files, CampaignPath::narration, kBriefsDirectory, value, "WAV");
    language_string(header, language, "missionhint", value, sizeof(value), "");
    resolve_path(file, files, CampaignPath::hint, kHintsDirectory, value, "TXT");
    cursor_string(&ota, "glamour", value, sizeof(value), "");
    resolve_path(file, files, CampaignPath::glamour, "", value, "PCX");
    cursor_string(&ota, "glamoursound", value, sizeof(value), "");
    resolve_path(file, files, CampaignPath::glamour_sound, kBriefsDirectory, value, "WAV");
    cursor_string(&ota, "UseOnlyUnits", value, sizeof(value), "");
    resolve_path(file, &rules, CampaignPath::use_only, kUseOnlyDirectory, value, "TDF");

    file->mapping = oa::formats::tdf::get_int(header, "mapping", 0);
    file->line_of_sight = oa::formats::tdf::get_int(header, "lineofsight", 0);
    if (env->game != nullptr) {
        int32_t record[4]{};
        campaign_session_record(file, record);
        std::memcpy(env->game->session_record, record, sizeof record);
    }
    cursor_string(&ota, "memory", file->memory, kCampaignShortTextBytes, "");
    cursor_string(&ota, "numplayers", file->num_players, kCampaignShortTextBytes, "");
    cursor_string(&ota, "Planet", file->planet, kCampaignShortTextBytes, "");
    file->no_movie = oa::formats::tdf::get_int(header, "nomovie", 0);
    if (env->game != nullptr)
        env->game->no_movie = file->no_movie;

    char description[kCampaignShortTextBytes];
    cursor_string(
        &ota, "missiondescription", description, sizeof(description), "No description available"
    );
    char lowered[kCampaignShortTextBytes];
    copy_bounded(lowered, sizeof(lowered), description);
    for (char* p = lowered; *p != '\0'; ++p)
        *p = static_cast<char>(std::tolower(static_cast<unsigned char>(*p)));
    copy_bounded(file->description, kCampaignShortTextBytes, translate(files, lowered));
    if (oa::formats::tdf::compare_nocase(file->description, lowered) == 0)
        copy_bounded(file->description, kCampaignShortTextBytes, description);

    file->min_wind = oa::formats::tdf::get_int(header, "minwindspeed", 0);
    file->max_wind = oa::formats::tdf::get_int(header, "maxwindspeed", 0);
    file->gravity = oa::formats::tdf::get_int(header, "gravity", 0);
    file->tidal_strength =
        static_cast<float>(oa::formats::tdf::get_double(header, "tidalstrength", 0.0));
    file->lava_world = oa::formats::tdf::get_int(header, "lavaworld", 0);
    file->no_sea_level_trigger = oa::formats::tdf::get_int(header, "nosealeveltrigger", 0);
    file->water_does_damage = oa::formats::tdf::get_int(header, "waterdoesdamage", 0);
    file->water_damage = oa::formats::tdf::get_int(header, "waterdamage", 0);
    file->kill_multiplier =
        static_cast<float>(oa::formats::tdf::get_double(header, "killmul", 0.0));
    file->time_multiplier =
        static_cast<float>(oa::formats::tdf::get_double(header, "timemul", 0.0));

    const int32_t players = env->player_count;
    if (!find_matching_schema(
            file->kind,
            &ota,
            env->difficulty,
            players,
            file->schema,
            sizeof(file->schema),
            env->difficulty_names
        )) {
        message(files, "No suitable schema type in mission file!");
        oa::formats::tdf::document_free(&ota);
        return false;
    }
    const oa::formats::tdf::Block* schema = oa::formats::tdf::cursor(&ota);
    file->metal[0] = static_cast<float>(oa::formats::tdf::get_int(schema, "HumanMetal", 0));
    file->energy[0] = static_cast<float>(oa::formats::tdf::get_int(schema, "HumanEnergy", 0));
    file->metal[1] = static_cast<float>(oa::formats::tdf::get_int(schema, "ComputerMetal", 0));
    file->energy[1] = static_cast<float>(oa::formats::tdf::get_int(schema, "ComputerEnergy", 0));
    file->surface_metal = oa::formats::tdf::get_int(schema, "SurfaceMetal", 0);
    cursor_string(&ota, "aiprofile", value, sizeof(value), "");
    const char* ai_directory = oa::data::defs::directory_name(oa::data::defs::DataDirectory::ai);
    resolve_path(file, &rules, CampaignPath::ai_profile, ai_directory, value, "txt");
    if (campaign_path(file, CampaignPath::ai_profile) == nullptr)
        resolve_path(file, &rules, CampaignPath::ai_profile, ai_directory, "Default", "txt");
    ok = campaign_parse_mission_data(file, file->schema, &ota);
    oa::formats::tdf::document_free(&ota);
    return ok;
}

bool campaign_parse_mission_data(
    CampaignFile* file, const char* schema_name, oa::formats::tdf::Document* ota
) noexcept {
    oa::formats::tdf::reset_cursor(ota);
    if (!oa::formats::tdf::select_section(ota, "globalheader") ||
        !oa::formats::tdf::select_section(ota, schema_name))
        return true;
    const oa::formats::tdf::Block* schema = oa::formats::tdf::cursor(ota);
    static constexpr const char* kUnitStrings[] = {"Unitname", "Ident", "InitialMission"};

    const oa::formats::tdf::Block* units =
        oa::formats::tdf::find_child(schema, oa::data::defs::map_units_section());
    const uint32_t unit_count = oa::formats::tdf::child_count(units);
    uint32_t pool = 0;
    char scratch[kUnitStringLimit];
    for (uint32_t i = 0; i < unit_count; ++i)
        for (const char* key : kUnitStrings)
            if (oa::formats::tdf::get_string(
                    oa::formats::tdf::child_at(units, i), key, scratch, sizeof(scratch), ""
                ))
                pool += static_cast<uint32_t>(std::strlen(scratch)) + 1;
    file->unit_count = static_cast<int32_t>(unit_count);
    const std::size_t table = unit_count * sizeof(MissionUnit);
    auto* block =
        static_cast<uint8_t*>(std::calloc(1, (pool / kPoolGranule + 2) * kPoolGranule + table));
    if (block == nullptr) {
        file->unit_count = 0;
        return false;
    }
    file->units = reinterpret_cast<MissionUnit*>(block);
    char* strings = reinterpret_cast<char*>(block + table);
    for (uint32_t i = 0; i < unit_count; ++i) {
        MissionUnit& unit = file->units[i];
        const oa::formats::tdf::Block* entry = oa::formats::tdf::child_at(units, i);
        const char** targets[] = {&unit.unit_name, &unit.ident, &unit.initial_mission};
        for (std::size_t k = 0; k < 3; ++k) {
            if (oa::formats::tdf::get_string(
                    entry, kUnitStrings[k], scratch, sizeof(scratch), ""
                )) {
                const std::size_t length = std::strlen(scratch);
                std::memcpy(strings, scratch, length + 1);
                *targets[k] = strings;
                strings += length + 1;
            } else {
                *targets[k] = nullptr;
            }
        }
        unit.x = static_cast<int32_t>(
            static_cast<uint32_t>(oa::formats::tdf::get_int(entry, "XPos", 0)) << 16
        );
        unit.y = static_cast<int32_t>(
            static_cast<uint32_t>(oa::formats::tdf::get_int(entry, "YPos", 0)) << 16
        );
        unit.z = static_cast<int32_t>(
            static_cast<uint32_t>(oa::formats::tdf::get_int(entry, "ZPos", 0)) << 16
        );
        const auto angle = static_cast<int32_t>(
            static_cast<uint32_t>(oa::formats::tdf::get_int(entry, "Angle", 0)) << 16
        );
        unit.angle = static_cast<int16_t>(angle / 360);
        unit.player = static_cast<uint8_t>(oa::formats::tdf::get_int(entry, "Player", 0));
        if (unit.player == 0)
            unit.player = 1;
        unit.health_percent =
            static_cast<int16_t>(oa::formats::tdf::get_int(entry, "HealthPercentage", 100));
        unit.build_priority =
            static_cast<int16_t>(oa::formats::tdf::get_int(entry, "BuildPriority", 0));
        unit.creation_countdown = oa::formats::tdf::get_int(entry, "CreationCountdown", 0);
        uint8_t flags = 0;
        if (oa::formats::tdf::get_int(entry, "MissionCriticalUnit", 0) & 1)
            flags |= mission_unit_flag::mission_critical;
        if (oa::formats::tdf::get_int(entry, "AiIgnore", 0) & 1)
            flags |= mission_unit_flag::ai_ignore;
        if (oa::formats::tdf::get_int(entry, "AiPriorityTarget", 0) & 1)
            flags |= mission_unit_flag::ai_priority_target;
        flags |= static_cast<uint8_t>(oa::formats::tdf::get_int(entry, "InitialGroup", 0)) &
                 mission_unit_flag::initial_group_mask;
        if (oa::formats::tdf::get_int(entry, "Immunity", 0) & 1)
            flags |= mission_unit_flag::immunity;
        unit.flags = flags;
    }

    const oa::formats::tdf::Block* specials = oa::formats::tdf::find_child(schema, "specials");
    const uint32_t rule_count = oa::formats::tdf::child_count(specials);
    file->rule_count = static_cast<int32_t>(rule_count);
    file->rules = static_cast<MissionRule*>(
        std::calloc(rule_count != 0 ? rule_count : 1, sizeof(MissionRule))
    );
    int32_t unnumbered = 0;
    for (uint32_t i = 0; file->rules != nullptr && i < rule_count; ++i) {
        MissionRule& rule = file->rules[i];
        rule.type = MissionRuleType::none;
        const oa::formats::tdf::Block* entry = oa::formats::tdf::child_at(specials, i);
        char what[0x100];
        if (!oa::formats::tdf::get_string(entry, "specialwhat", what, sizeof(what), ""))
            continue;
        if (!prefix_nocase(what, kStartPos, kStartPosLength))
            continue;
        rule.type = MissionRuleType::start_position;
        rule.x = static_cast<int16_t>(oa::formats::tdf::get_int(entry, "XPos", 0));
        rule.z = static_cast<int16_t>(oa::formats::tdf::get_int(entry, "ZPos", 0));
        const char* suffix = what + kStartPosLength;
        const int32_t number =
            std::isdigit(static_cast<unsigned char>(*suffix)) ? std::atoi(suffix) : ++unnumbered;
        rule.index = number > 0 ? number - 1 : number;
    }

    const oa::formats::tdf::Block* features = oa::formats::tdf::find_child(schema, "features");
    const uint32_t feature_count = oa::formats::tdf::child_count(features);
    file->feature_count = static_cast<int32_t>(feature_count);
    std::free(file->features);
    file->features = static_cast<MissionFeature*>(
        std::calloc(feature_count != 0 ? feature_count : 1, sizeof(MissionFeature))
    );
    for (uint32_t i = 0; file->features != nullptr && i < feature_count; ++i) {
        MissionFeature& feature = file->features[i];
        const oa::formats::tdf::Block* entry = oa::formats::tdf::child_at(features, i);
        if (!oa::formats::tdf::get_string(
                entry, "Featurename", feature.name, sizeof(feature.name), ""
            ))
            feature.name[0] = '\0';
        feature.x = oa::formats::tdf::get_int(entry, "XPos", -1);
        feature.z = oa::formats::tdf::get_int(entry, "ZPos", -1);
        if (feature.x < 0 || feature.z < 0)
            feature.name[0] = '\0';
    }
    return true;
}

void campaign_free_owned_blocks(CampaignFile* file) noexcept {
    std::free(file->units);
    file->units = nullptr;
    file->unit_count = 0;
    std::free(file->rules);
    file->rules = nullptr;
    file->rule_count = 0;
    std::free(file->features);
    file->feature_count = 0;
    file->features = nullptr;
    std::free(file->briefing_text);
    file->briefing_text = nullptr;
}

bool select_difficulty_schema(
    oa::formats::tdf::Document* ota, int32_t difficulty, char* schema_name, std::size_t capacity
) noexcept {
    return find_matching_schema(SessionKind::campaign, ota, difficulty, 0, schema_name, capacity);
}

int32_t campaign_count_files(const CampaignFiles* files) {
    if (files == nullptr || files->count == nullptr)
        return 0;
    char pattern[kCampaignPathBytes];
    build_variant_path(files, pattern, sizeof(pattern), kCampsDirectory, "*", "TDF");
    return files->count(files->context, pattern);
}

namespace {

// True when a name before `name` in the packed list of campaign files is the
// same file. Names compare without case, the way a layered folder treats one
// path: a lower layer's copy reads the top layer's file, which the first copy
// has already listed or left out.
bool campaign_name_already_tried(const char* names, const char* name) {
    for (const char* at = names; at < name; at += std::strlen(at) + 1)
        if (oa::formats::tdf::compare_nocase(at, name) == 0)
            return true;
    return false;
}

// Reports a campaign file that does not load, which the list leaves out.
void report_unlisted_campaign(
    const CampaignFiles* files, const char* path, const oa::formats::tdf::ParseError& error
) {
    char reason[160];
    if (error.status == oa::formats::tdf::ParseStatus::ok)
        std::snprintf(reason, sizeof(reason), "it cannot be read");
    else if (error.status == oa::formats::tdf::ParseStatus::too_large)
        std::snprintf(
            reason, sizeof(reason), "it is over %u bytes", oa::formats::tdf::max_input_bytes
        );
    else
        std::snprintf(
            reason,
            sizeof(reason),
            "%s at byte %u%s%s%s",
            oa::formats::tdf::parse_status_message(error.status),
            error.offset,
            error.block_name[0] != '\0' ? " in [" : "",
            error.block_name,
            error.block_name[0] != '\0' ? "]" : ""
        );
    char text[kCampaignPathBytes + sizeof(reason) + 64];
    std::snprintf(
        text, sizeof(text), "The campaign file %s is left out of the list: %s.", path, reason
    );
    message(files, text);
}

} // namespace

int32_t
campaign_load_names(const CampaignFiles* files, const char* side, char* out, std::size_t capacity) {
    if (capacity != 0)
        out[0] = '\0';
    if (files == nullptr || capacity == 0)
        return 0;
    char pattern[kCampaignPathBytes];
    build_variant_path(files, pattern, sizeof(pattern), kCampsDirectory, "*", "TDF");
    const int32_t count = files->count != nullptr ? files->count(files->context, pattern) : 0;
    if (count <= 0)
        return 0;
    const std::size_t names_bytes = static_cast<std::size_t>(count) * kCampaignPathBytes;
    auto* names = static_cast<char*>(std::calloc(1, names_bytes));
    if (names == nullptr)
        return 0;
    const DirectoryFind find{files->context, files->find, nullptr};
    (void)list_directory_entries(
        find, pattern, names, names_bytes, nullptr, 0, false, true, ListSort::by_name
    );
    int32_t kept = 0;
    std::size_t used = 0;
    const char* name = names;
    oa::formats::tdf::Document document;
    oa::formats::tdf::document_init(&document);
    for (int32_t left = count; left > 0; --left) {
        // Loose files come first, then each mount, and equal names keep that
        // order, so the first copy of a file is the top layer. A lower layer's
        // copy of the same path is the same campaign.
        if (campaign_name_already_tried(names, name)) {
            name += std::strlen(name) + 1;
            continue;
        }
        char path[kCampaignPathBytes];
        const CampaignFiles rules = rule_files(files);
        build_variant_path(&rules, path, sizeof(path), kCampsDirectory, name, "tdf");
        oa::formats::tdf::ParseError error{};
        if (!load_tdf(files, &document, path, &error)) {
            report_unlisted_campaign(files, path, error);
            name += std::strlen(name) + 1;
            continue;
        }
        oa::formats::tdf::reset_cursor(&document);
        if (oa::formats::tdf::select_section(&document, "HEADER")) {
            char campaign_side[0x40];
            (void)oa::formats::tdf::get_string(
                oa::formats::tdf::cursor(&document),
                "campaignside",
                campaign_side,
                sizeof(campaign_side),
                ""
            );
            if (std::strcmp(side, campaign_side) == 0 ||
                std::strcmp(kAllSides, campaign_side) == 0) {
                const std::size_t length = std::strlen(name);
                if (used + length + 1 < capacity) {
                    std::memcpy(out + used, name, length + 1);
                    used += length + 1;
                    ++kept;
                }
            }
        }
        name += std::strlen(name) + 1;
    }
    if (used < capacity)
        out[used] = '\0';
    oa::formats::tdf::document_free(&document);
    std::free(names);
    return kept;
}

void campaign_sides_present(const CampaignFiles* files, bool* arm, bool* core) {
    if (arm != nullptr)
        *arm = false;
    if (core != nullptr)
        *core = false;
    if (files == nullptr || (arm == nullptr && core == nullptr))
        return;
    char pattern[kCampaignPathBytes];
    build_variant_path(files, pattern, sizeof(pattern), kCampsDirectory, "*", "TDF");
    const int32_t count = files->count != nullptr ? files->count(files->context, pattern) : 0;
    if (count <= 0)
        return;
    const std::size_t names_bytes = static_cast<std::size_t>(count) * kCampaignPathBytes;
    auto* names = static_cast<char*>(std::calloc(1, names_bytes));
    if (names == nullptr)
        return;
    const DirectoryFind find{files->context, files->find, nullptr};
    (void)list_directory_entries(
        find, pattern, names, names_bytes, nullptr, 0, false, true, ListSort::by_name
    );
    const char* name = names;
    oa::formats::tdf::Document document;
    oa::formats::tdf::document_init(&document);
    bool arm_found = false;
    bool core_found = false;
    for (int32_t left = count; left > 0; --left) {
        if (campaign_name_already_tried(names, name)) {
            name += std::strlen(name) + 1;
            continue;
        }
        char path[kCampaignPathBytes];
        const CampaignFiles rules = rule_files(files);
        build_variant_path(&rules, path, sizeof(path), kCampsDirectory, name, "tdf");
        oa::formats::tdf::ParseError error{};
        if (!load_tdf(files, &document, path, &error)) {
            report_unlisted_campaign(files, path, error);
            name += std::strlen(name) + 1;
            continue;
        }
        oa::formats::tdf::reset_cursor(&document);
        if (oa::formats::tdf::select_section(&document, "HEADER")) {
            char campaign_side[0x40];
            (void)oa::formats::tdf::get_string(
                oa::formats::tdf::cursor(&document),
                "campaignside",
                campaign_side,
                sizeof(campaign_side),
                ""
            );
            const bool all_sides = std::strcmp(kAllSides, campaign_side) == 0;
            if (all_sides || std::strcmp("ARM", campaign_side) == 0)
                arm_found = true;
            if (all_sides || std::strcmp("CORE", campaign_side) == 0)
                core_found = true;
        }
        name += std::strlen(name) + 1;
        const bool arm_done = arm == nullptr || arm_found;
        const bool core_done = core == nullptr || core_found;
        if (arm_done && core_done)
            break;
    }
    oa::formats::tdf::document_free(&document);
    std::free(names);
    if (arm != nullptr)
        *arm = arm_found;
    if (core != nullptr)
        *core = core_found;
}

} // namespace oa::data::campaign
