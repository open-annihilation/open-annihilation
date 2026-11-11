// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/data/languages/language_pack.hpp"

#include "oa/formats/oamod.hpp"
#include "oa/data/content_bundles/content_needs.hpp"
#include "oa/formats/oamod/package_keys.hpp"
#include "oa/formats/tdf.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace oa::data::languages {

namespace {

namespace tdf = oa::formats::tdf;
namespace oamod = oa::formats::oamod;

/// The most nested sections a picture's path names.
constexpr uint32_t most_picture_depth = 4;

/// Lowers the ASCII letters of a text.
///
/// @param text the text
/// @return the text, A to Z lowered
std::string lowered(std::string_view text) {
    std::string folded(text);
    for (char& letter : folded)
        if (letter >= 'A' && letter <= 'Z')
            letter = static_cast<char>(letter - 'A' + 'a');
    return folded;
}

/// Returns a mission file's name as missions.tdf is looked up by: lowered,
/// without a ".ota" extension.
///
/// @param mission_file the mission's file, as the campaign names it
/// @return the key
std::string mission_key(std::string_view mission_file) {
    std::string key = lowered(mission_file);
    constexpr std::string_view extension = ".ota";
    if (key.size() > extension.size() && key.ends_with(extension))
        key.resize(key.size() - extension.size());
    return key;
}

/// Parses a table, refusing one larger than most_pack_table_bytes.
///
/// @param file the file's bytes
/// @param[out] document the parsed file
/// @param[out] error why the file was refused; may be null
/// @return true when it parsed
bool parse_table(std::string_view file, tdf::OwnedDocument& document, std::string* error) {
    if (file.size() > most_pack_table_bytes) {
        if (error != nullptr)
            *error = "the table is larger than " + std::to_string(most_pack_table_bytes) + " bytes";
        return false;
    }
    tdf::ParseError parse_error{};
    if (!document.parse(file, &parse_error)) {
        if (error != nullptr)
            *error = tdf::describe(parse_error);
        return false;
    }
    return true;
}

/// Returns a section's value under a key, any case.
///
/// @param section the section
/// @param key the key
/// @return the value; empty without one
std::string_view value_of(const tdf::Block* section, const char* key) {
    const char* value = tdf::find_value(section, key);
    return value != nullptr ? std::string_view(value) : std::string_view{};
}

/// Adds a section's captions, and those of the sections nested in it.
///
/// @param section the section
/// @param path the path of the section's parent; empty at the top
/// @param depth how deep the section is, 1 at the top
/// @param[in,out] captions the captions, by lower-case path
void add_captions(
    const tdf::Block* section,
    const std::string& path,
    uint32_t depth,
    std::map<std::string, PictureCaptions::Keys, std::less<>>& captions
) {
    if (section == nullptr || section->name == nullptr || section->name[0] == '\0' ||
        depth > most_picture_depth)
        return;
    const std::string own =
        path.empty() ? lowered(section->name) : path + '/' + lowered(section->name);
    const uint32_t children = tdf::child_count(section);
    const uint32_t keys = tdf::property_count(section);
    // A section that only groups others is not a caption itself.
    if (keys != 0 || children == 0) {
        PictureCaptions::Keys values;
        for (uint32_t index = 0; index < keys; ++index) {
            const char* key = tdf::property_key_at(section, static_cast<int32_t>(index));
            const char* value = tdf::find_value(section, key);
            if (key != nullptr && value != nullptr)
                values[lowered(key)] = value;
        }
        captions[own] = std::move(values);
    }
    for (uint32_t index = 0; index < children; ++index)
        add_captions(tdf::child_at(section, index), own, depth + 1, captions);
}

/// Reads a manifest's text field.
///
/// @param node the field's node
/// @param[out] text its text
/// @return false when it is not a string
bool read_text(const oamod::Node& node, std::string& text) {
    if (node.kind != oamod::NodeKind::string)
        return false;
    text = node.text;
    return true;
}

/// Reads a manifest's list of tags.
///
/// @param node the field's node
/// @param[out] tags its strings
/// @return false when it is not a sequence of strings
bool read_tags(const oamod::Node& node, std::vector<std::string>& tags) {
    if (node.kind != oamod::NodeKind::sequence)
        return false;
    tags.clear();
    for (const oamod::Node& item : node.children) {
        if (item.kind != oamod::NodeKind::string || item.text.empty())
            return false;
        tags.push_back(item.text);
    }
    return true;
}

/// Reads a manifest's text: entry, { needs: game-fonts | modern-fonts | shaping }.
///
/// @param node the field's node
/// @param[out] needs what drawing the language needs
/// @param[out] error why it was refused
/// @return true when it was read
bool read_text_needs(const oamod::Node& node, TextNeeds& needs, std::string& error) {
    if (node.kind != oamod::NodeKind::mapping) {
        error = "text must be a mapping";
        return false;
    }
    for (const oamod::Node& entry : node.children) {
        if (entry.key.text != "needs") {
            error = "text has no key " + entry.key.text;
            return false;
        }
        if (entry.kind == oamod::NodeKind::string && entry.text == "game-fonts")
            needs = TextNeeds::game_fonts;
        else if (entry.kind == oamod::NodeKind::string && entry.text == "modern-fonts")
            needs = TextNeeds::modern_fonts;
        else if (entry.kind == oamod::NodeKind::string && entry.text == "shaping")
            needs = TextNeeds::text_shaping;
        else {
            error = "text needs must be game-fonts, modern-fonts or shaping";
            return false;
        }
    }
    return true;
}

/// Tells whether a name ends with a suffix, ignoring the case of ASCII letters.
///
/// @param text the name
/// @param suffix the suffix, in lower case
/// @return true when it ends so
bool ends_with_any_case(std::string_view text, std::string_view suffix) {
    if (text.size() < suffix.size())
        return false;
    const std::string_view tail = text.substr(text.size() - suffix.size());
    for (std::size_t index = 0; index < suffix.size(); ++index) {
        const char letter = tail[index];
        const char folded =
            letter >= 'A' && letter <= 'Z' ? static_cast<char>(letter - 'A' + 'a') : letter;
        if (folded != suffix[index])
            return false;
    }
    return true;
}

/// Tells whether a name is one file in a pack's own folder: 1 to 128 bytes of
/// ASCII letters, digits, '-' , '_' and '.', ending in one of the suffixes.
///
/// A slash, a backslash or any other character is refused, so the name cannot
/// leave the folder.
///
/// @param name the file name
/// @param first a suffix, in lower case, such as ".otf"
/// @param second another suffix; empty for none
/// @return true for such a name
bool pack_file_name(std::string_view name, std::string_view first, std::string_view second = {}) {
    if (name.empty() || name.size() > 128)
        return false;
    for (const unsigned char letter : name) {
        const bool allowed = (letter >= 'A' && letter <= 'Z') || (letter >= 'a' && letter <= 'z') ||
                             (letter >= '0' && letter <= '9') || letter == '-' || letter == '_' ||
                             letter == '.';
        if (!allowed)
            return false;
    }
    return ends_with_any_case(name, first) || (!second.empty() && ends_with_any_case(name, second));
}

/// Tells whether text is an ISO 8601 calendar date, YYYY-MM-DD, that exists.
///
/// @param text the date
/// @return true for a four-digit year, a month 01 to 12 and a day within that
///     month, February counting 29 days in Gregorian leap years
bool calendar_date(std::string_view text) {
    constexpr std::size_t year_digits = 4;
    constexpr std::size_t month_start = year_digits + 1;
    constexpr std::size_t day_start = month_start + 3;
    constexpr std::size_t date_length = day_start + 2;
    if (text.size() != date_length || text[month_start - 1] != '-' || text[day_start - 1] != '-')
        return false;
    const auto digits = [text](std::size_t start, std::size_t count, int& value) {
        value = 0;
        for (const char letter : text.substr(start, count)) {
            if (letter < '0' || letter > '9')
                return false;
            value = value * 10 + (letter - '0');
        }
        return true;
    };
    int year = 0;
    int month = 0;
    int day = 0;
    if (!digits(0, year_digits, year) || !digits(month_start, 2, month) ||
        !digits(day_start, 2, day))
        return false;
    if (month < 1 || month > 12 || day < 1)
        return false;
    constexpr std::array<int, 12> month_days{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    const int days = month == 2 && leap ? 29 : month_days[static_cast<std::size_t>(month - 1)];
    return day <= days;
}

/// Reads a manifest's fonts sequence.
///
/// @param node the field's node
/// @param[out] fonts the fonts
/// @param[out] error why it was refused
/// @return true when it was read
bool read_fonts(const oamod::Node& node, std::vector<PackFont>& fonts, std::string& error) {
    if (node.kind != oamod::NodeKind::sequence) {
        error = "the manifest's fonts is not of its kind";
        return false;
    }
    if (node.children.size() > most_pack_fonts) {
        error = "the manifest's fonts holds more than " + std::to_string(most_pack_fonts);
        return false;
    }
    fonts.clear();
    for (const oamod::Node& item : node.children) {
        if (item.kind != oamod::NodeKind::mapping) {
            error = "the manifest's fonts is not of its kind";
            return false;
        }
        bool saw_file = false;
        bool saw_role = false;
        PackFont font;
        for (const oamod::Node& entry : item.children) {
            if (entry.key.text == "file") {
                saw_file = true;
                if (entry.kind != oamod::NodeKind::string ||
                    !pack_file_name(entry.text, ".otf", ".ttf")) {
                    error = "the manifest's fonts file is not a font file";
                    return false;
                }
                font.file = entry.text;
            } else if (entry.key.text == "role") {
                saw_role = true;
                if (entry.kind == oamod::NodeKind::string && entry.text == "ideographs")
                    font.role = PackFontRole::ideographs;
                else if (entry.kind == oamod::NodeKind::string && entry.text == "letters")
                    font.role = PackFontRole::letters;
                else {
                    error = "the manifest's fonts role must be ideographs or letters";
                    return false;
                }
            } else {
                error = "the manifest's fonts has no key " + entry.key.text;
                return false;
            }
        }
        if (!saw_file || !saw_role) {
            error = "the manifest's fonts is not of its kind";
            return false;
        }
        for (const PackFont& earlier : fonts) {
            if (earlier.file == font.file) {
                error = "the manifest's fonts names " + font.file + " twice";
                return false;
            }
        }
        fonts.push_back(std::move(font));
    }
    return true;
}

/// Reads a manifest's warmup file name.
///
/// @param node the field's node
/// @param[out] warmup the file name
/// @param[out] error why it was refused
/// @return true when it was read
bool read_warmup(const oamod::Node& node, std::string& warmup, std::string& error) {
    if (node.kind != oamod::NodeKind::string || !pack_file_name(node.text, ".txt")) {
        error = "the manifest's warmup is not a text file";
        return false;
    }
    warmup = node.text;
    return true;
}

/// Reads a manifest's packaging block.
///
/// @param node the field's node
/// @param[out] packaging the block
/// @param[out] error why it was refused
/// @return true when it was read
bool read_packaging(
    const oamod::Node& node, std::optional<PackPackaging>& packaging, std::string& error
) {
    if (node.kind != oamod::NodeKind::mapping) {
        error = "the manifest's packaging is not of its kind";
        return false;
    }
    bool saw_revision = false;
    bool saw_date = false;
    bool saw_packager = false;
    PackPackaging read;
    for (const oamod::Node& entry : node.children) {
        if (entry.key.text == "revision") {
            saw_revision = true;
            int64_t number = 0;
            if (entry.kind != oamod::NodeKind::number ||
                !oamod::integer_value(entry.number, number) || number < 1 || number > 65535) {
                error = "the manifest's packaging revision must be an integer from 1 to 65535";
                return false;
            }
            read.revision = number;
        } else if (entry.key.text == "date") {
            saw_date = true;
            if (entry.kind != oamod::NodeKind::string || !calendar_date(entry.text)) {
                error = "the manifest's packaging date must be an ISO 8601 date, YYYY-MM-DD";
                return false;
            }
            read.date = entry.text;
        } else if (entry.key.text == "packager") {
            saw_packager = true;
            if (entry.kind != oamod::NodeKind::string || entry.text.empty() ||
                entry.text.size() > 128) {
                error = "the manifest's packaging packager must be a string of 1 to 128 bytes";
                return false;
            }
            read.packager = entry.text;
        } else {
            error = "the manifest's packaging has no key " + entry.key.text;
            return false;
        }
    }
    if (!saw_revision || !saw_date || !saw_packager) {
        error = "the manifest's packaging is not of its kind";
        return false;
    }
    packaging = std::move(read);
    return true;
}

} // namespace

bool read_manifest(std::span<const uint8_t> bytes, PackManifest& manifest, std::string* error) {
    oamod::Node root;
    oamod::ReadError read_error{};
    std::string failure;
    PackManifest read{};
    bool has_format = false;
    if (!oamod::read_document(bytes, root, read_error)) {
        failure = std::string(oamod::rule_message(read_error.rule)) + " at line " +
                  std::to_string(read_error.position.line) + ", column " +
                  std::to_string(read_error.position.column);
    } else {
        for (const oamod::Node& entry : root.children) {
            const std::string& key = entry.key.text;
            bool ok = true;
            if (key == "oalang") {
                ok = entry.kind == oamod::NodeKind::number &&
                     oamod::integer_value(entry.number, read.format);
                has_format = ok;
            } else if (key == "tag") {
                ok = read_text(entry, read.tag);
            } else if (key == "name") {
                ok = read_text(entry, read.name);
            } else if (key == "english-name") {
                ok = read_text(entry, read.english_name);
            } else if (key == "word") {
                ok = read_text(entry, read.word);
            } else if (key == "version") {
                ok = read_text(entry, read.version);
            } else if (key == "locales") {
                ok = read_tags(entry, read.locales);
            } else if (key == "fallbacks") {
                ok = read_tags(entry, read.fallbacks);
            } else if (key == "text") {
                if (!read_text_needs(entry, read.needs, failure))
                    break;
            } else if (key == "unicode") {
                ok = entry.kind == oamod::NodeKind::boolean;
                read.unicode = ok && entry.boolean;
            } else if (key == "fonts") {
                if (!read_fonts(entry, read.fonts, failure))
                    break;
            } else if (key == "warmup") {
                if (!read_warmup(entry, read.warmup, failure))
                    break;
            } else if (key == "packaging") {
                if (!read_packaging(entry, read.packaging, failure))
                    break;
            } else if (
                key == "homepage" || key == "tags" || key == "supportedGames" ||
                key == "requiredExpansions"
            ) {
                ok = true;
            } else if (key == "requires") {
                if (entry.kind != oamod::NodeKind::mapping) {
                    failure = "the manifest's requires is not a mapping";
                    break;
                }
                for (const oamod::Node& child : entry.children) {
                    if (child.key.kind != oamod::KeyKind::string ||
                        (child.key.text != "engine" && child.key.text != "base")) {
                        failure = "the manifest's requires takes engine and base";
                        break;
                    }
                    if (child.key.text == "base" &&
                        (child.kind != oamod::NodeKind::string || child.text != "ta-3.1c")) {
                        failure = "requires.base must be ta-3.1c";
                        break;
                    }
                }
                if (!failure.empty())
                    break;
            } else {
                failure = "the manifest has no key " + key;
                break;
            }
            if (!ok) {
                failure = "the manifest's " + key + " is not of its kind";
                break;
            }
        }
        if (failure.empty() && !has_format)
            failure = "the manifest names no oalang";
        else if (failure.empty() && read.format != pack_format_version)
            failure = "the manifest's oalang " + std::to_string(read.format) + " is not " +
                      std::to_string(pack_format_version);
        else if (failure.empty() && read.tag.empty())
            failure = "the manifest names no tag";
        else if (failure.empty() && read.word.empty())
            failure = "the manifest names no word";
        else if (failure.empty() && normalised_locale(read.tag) != read.tag)
            failure = "the manifest's tag " + read.tag + " is not a tag";
        else if (failure.empty()) {
            oamod::PackageKeys keys;
            std::vector<oamod::KeyProblem> problems;
            const oamod::Node* requires_block = oamod::find_entry(root, "requires");
            if (requires_block != nullptr && requires_block->kind != oamod::NodeKind::mapping)
                requires_block = nullptr;
            oamod::read_package_keys(root, requires_block, keys, problems);
            if (!problems.empty())
                failure = problems.front().path + ": " + problems.front().message;
            else {
                read.homepage = std::move(keys.homepage);
                read.tags = std::move(keys.tags);
                read.requires_engine = std::move(keys.requires_engine);
                oa::data::content_bundles::ContentNeeds needs;
                std::vector<oamod::KeyProblem> need_problems;
                oa::data::content_bundles::read_content_needs(
                    root,
                    requires_block,
                    oa::data::content_bundles::GamesWhenOmitted::every_game,
                    needs,
                    need_problems
                );
                if (!need_problems.empty())
                    failure = need_problems.front().path + ": " + need_problems.front().message;
                else {
                    read.supported_games = std::move(needs.supported_games);
                    read.required_expansions = std::move(needs.required_expansions);
                    if (needs.legacy_base)
                        read.requires_base = "ta-3.1c";
                }
            }
        }
    }
    if (!failure.empty()) {
        if (error != nullptr)
            *error = failure;
        return false;
    }
    manifest = std::move(read);
    return true;
}

LanguageEntry entry_of(const PackManifest& manifest) {
    LanguageEntry entry;
    entry.tag = manifest.tag;
    entry.endonym = manifest.name;
    entry.english_name = manifest.english_name;
    entry.word = manifest.word;
    entry.locales = manifest.locales;
    entry.fallbacks = manifest.fallbacks;
    entry.needs = manifest.needs;
    return entry;
}

std::string_view pack_table_file(PackTable table) noexcept {
    switch (table) {
    case PackTable::translate:
        return "translate.tdf";
    case PackTable::units:
        return "units.tdf";
    case PackTable::missions:
        return "missions.tdf";
    case PackTable::pictures:
        return "pictures.tdf";
    }
    return {};
}

bool PictureCaptions::add(std::string_view file, std::string* error) {
    tdf::OwnedDocument document;
    if (!parse_table(file, document, error))
        return false;
    const tdf::Block* root = document.root();
    std::map<std::string, Keys, std::less<>> added;
    for (uint32_t index = 0; index < tdf::child_count(root); ++index)
        add_captions(tdf::child_at(root, index), {}, 1, added);
    for (auto& [path, keys] : added)
        captions_[path] = std::move(keys);
    return true;
}

void PictureCaptions::add_missing(const PictureCaptions& lower) {
    for (const auto& [path, keys] : lower.captions_)
        captions_.try_emplace(path, keys);
}

const PictureCaptions::Keys* PictureCaptions::find(std::string_view path) const {
    const auto found = captions_.find(lowered(path));
    if (found == captions_.end() || found->second.empty())
        return nullptr;
    return &found->second;
}

bool translates(std::string_view from, std::string_view english, std::size_t field_bytes) noexcept {
    if (from.empty() || from == english)
        return true;
    // A field filled to its last byte may hold the English cut short.
    return field_bytes != 0 && english.size() + 1 >= field_bytes && from.starts_with(english);
}

bool LanguagePack::add(PackTable table, std::string_view file, std::string* error) {
    if (table == PackTable::pictures)
        return pictures_.add(file, error);
    tdf::OwnedDocument document;
    if (!parse_table(file, document, error))
        return false;
    const tdf::Block* root = document.root();
    for (uint32_t index = 0; index < tdf::child_count(root); ++index) {
        const tdf::Block* section = tdf::child_at(root, index);
        if (section == nullptr || section->name == nullptr || section->name[0] == '\0')
            continue;
        const std::string_view name = section->name;
        switch (table) {
        case PackTable::translate: {
            std::string_view text = value_of(section, manifest_.word.c_str());
            if (text.empty())
                text = value_of(section, manifest_.tag.c_str());
            if (!text.empty())
                translations_[std::string(name)] = std::string(text);
            break;
        }
        case PackTable::units: {
            PackUnitText& unit = units_[std::string(name)];
            const auto keep = [section](std::string& field, const char* key) {
                if (const std::string_view value = value_of(section, key); !value.empty())
                    field = std::string(value);
            };
            keep(unit.name, "name");
            keep(unit.name_from, "name-from");
            keep(unit.description, "description");
            keep(unit.description_from, "description-from");
            break;
        }
        case PackTable::missions: {
            auto& texts = missions_[mission_key(name)];
            for (const char* key : {"missionname", "missiondescription", "missionhint"})
                if (const std::string_view value = value_of(section, key); !value.empty())
                    texts[key] = std::string(value);
            break;
        }
        case PackTable::pictures:
            break;
        }
    }
    return true;
}

const std::string* LanguagePack::translation(std::string_view english) const {
    const auto found = translations_.find(english);
    return found != translations_.end() ? &found->second : nullptr;
}

const std::string*
LanguagePack::unit_name(std::string_view unit_name, std::string_view english) const {
    const auto found = units_.find(unit_name);
    if (found == units_.end() || found->second.name.empty() ||
        !translates(found->second.name_from, english, sizeof(UnitDef{}.name)))
        return nullptr;
    return &found->second.name;
}

const std::string*
LanguagePack::unit_description(std::string_view unit_name, std::string_view english) const {
    const auto found = units_.find(unit_name);
    if (found == units_.end() || found->second.description.empty() ||
        !translates(found->second.description_from, english, sizeof(UnitDef{}.description)))
        return nullptr;
    return &found->second.description;
}

const std::string*
LanguagePack::mission_text(std::string_view mission_file, std::string_view key) const {
    const auto found = missions_.find(mission_key(mission_file));
    if (found == missions_.end())
        return nullptr;
    const auto text = found->second.find(lowered(key));
    return text != found->second.end() ? &text->second : nullptr;
}

const PackLayer* layer_of(std::span<const PackLayer> layers, std::string_view word) {
    const UnitTexts::NoCaseLess less{};
    for (const PackLayer& layer : layers)
        if (!less(layer.word, word) && !less(word, layer.word))
            return &layer;
    return nullptr;
}

const char* layered_translation(
    std::span<const PackLayer> layers,
    std::span<const std::string> words,
    std::string_view english,
    const std::function<const char*(std::size_t)>& data
) {
    for (std::size_t index = 0; index < words.size(); ++index) {
        const PackLayer* layer = layer_of(layers, words[index]);
        if (layer != nullptr)
            for (const LanguagePack* pack : layer->before_data)
                if (const std::string* text = pack->translation(english))
                    return text->c_str();
        if (data)
            if (const char* text = data(index); text != nullptr && text[0] != '\0')
                return text;
        if (layer != nullptr)
            for (const LanguagePack* pack : layer->after_data)
                if (const std::string* text = pack->translation(english))
                    return text->c_str();
    }
    return nullptr;
}

const char* layered_mission_text(
    std::span<const PackLayer> layers,
    std::span<const std::string> words,
    std::string_view mission_file,
    std::string_view key,
    const char* data_text
) {
    for (std::size_t index = 0; index < words.size(); ++index) {
        const PackLayer* layer = layer_of(layers, words[index]);
        if (layer != nullptr)
            for (const LanguagePack* pack : layer->before_data)
                if (const std::string* text = pack->mission_text(mission_file, key))
                    return text->c_str();
        if (index == 0 && data_text != nullptr && data_text[0] != '\0')
            return data_text;
        if (layer != nullptr)
            for (const LanguagePack* pack : layer->after_data)
                if (const std::string* text = pack->mission_text(mission_file, key))
                    return text->c_str();
    }
    return data_text != nullptr && data_text[0] != '\0' ? data_text : nullptr;
}

PictureCaptions
layered_pictures(std::span<const PackLayer> layers, std::span<const std::string> words) {
    PictureCaptions captions;
    for (const std::string& word : words) {
        const PackLayer* layer = layer_of(layers, word);
        if (layer == nullptr)
            continue;
        for (const LanguagePack* pack : layer->before_data)
            captions.add_missing(pack->pictures());
        for (const LanguagePack* pack : layer->after_data)
            captions.add_missing(pack->pictures());
    }
    return captions;
}

} // namespace oa::data::languages
