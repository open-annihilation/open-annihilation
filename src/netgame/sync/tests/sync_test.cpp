// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The lobby's unit sync and map checks.
#include "oa/netgame/sync/map_hash.hpp"
#include "oa/netgame/sync/unit_checksum.hpp"
#include "oa/data/campaign/campaign_assets.hpp"
#include "oa/test/netgame_installs.hpp"
#include "oa/test/game_assets.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {

using namespace oa::data::campaign;
using namespace oa::netgame::sync;
using oa::data::unit_definitions::CatalogAssetReader;
using oa::data::unit_definitions::ErrorCode;
using oa::data::unit_definitions::Result;

int failures = 0;

void expect(bool condition, const char* what) {
    if (!condition) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}

void unit_checksum_tests() {
    // Hash lanes: sum, xor, sum(index ^ byte), xor(low8(index + byte)).
    expect(unit_file_checksum("A") == 0x41414141u, "one byte fills every lane");
    expect(unit_file_checksum("AB") == 0x02840383u, "two bytes mix the lanes");
    expect(unit_file_checksum(std::string("\0\xff", 2)) == 0x00feffffu, "zero and high bytes");
    expect(unit_file_checksum("") == 0u, "an empty file hashes to 0");

    class SyncAssets final : public CatalogAssetReader {
      public:

        mutable int reads = 0;
        mutable int lists = 0;
        bool fail_list = false;
        std::map<std::string, std::string> files;
        std::vector<std::string> guis;

        Result<std::vector<std::string>>
        list_effective(std::string_view directory, std::string_view extension) const override {
            ++lists;
            expect(
                directory == "guis" && extension == ".gui", "the pages are listed from guis/*.gui"
            );
            if (fail_list)
                return {{}, {ErrorCode::io, 0, "list failed"}};
            return {guis, {}};
        }

        Result<std::string> read(std::string_view path) const override {
            ++reads;
            const auto it = files.find(std::string(path));
            if (it == files.end())
                return {{}, {ErrorCode::io, 0, "missing"}};
            return {it->second, {}};
        }
    };

    SyncAssets cached;
    auto kept = mix_unit_file_checksum(cached, "ARMCOM", 7, 1);
    expect(
        kept && kept.value == 7u && cached.reads == 0 && cached.lists == 0,
        "a known checksum reads nothing"
    );

    SyncAssets cob_only;
    cob_only.files["scripts/ARMCOM.cob"] = "A";
    auto cob_hash = mix_unit_file_checksum(cob_only, "ARMCOM", 0, 0);
    expect(cob_hash && cob_hash.value == 0x41414141u, "the script alone");

    SyncAssets with_download;
    with_download.files["scripts/ARMCOM.cob"] = "A";
    with_download.files["download/ARMCOM.tdf"] = "B";
    auto both = mix_unit_file_checksum(with_download, "ARMCOM", 0, 0);
    expect(both && both.value == (0x41414141u ^ 0x42424242u), "the script and the download file");

    SyncAssets empty_download;
    empty_download.files["scripts/ARMCOM.cob"] = "A";
    empty_download.files["download/ARMCOM.tdf"] = "";
    auto skipped_empty = mix_unit_file_checksum(empty_download, "ARMCOM", 0, 0x01000000u);
    expect(
        skipped_empty && skipped_empty.value == (0x41414141u ^ 0x01000000u),
        "an empty download file is skipped"
    );

    SyncAssets pages;
    pages.files["scripts/ArmCom.cob"] = "A";
    pages.guis = {
        "guis/OTHER.GUI",
        "guis/.",
        "guis/..",
        "guis\\ARMCOM1.GUI",
        "guis/ARMCOMMANDER.GUI",
        "guis/ARMCOM9.GUI"
    };
    pages.files["guis/OTHER.GUI"] = "Z";
    pages.files["guis\\ARMCOM1.GUI"] = "B";
    pages.files["guis/ARMCOMMANDER.GUI"] = "C";
    // ARMCOM*.gui matches ARMCOM1 and ARMCOMMANDER, not OTHER. missing read is skipped.
    auto paged = mix_unit_file_checksum(pages, "ArmCom", 0, 0);
    expect(
        paged && paged.value == (0x41414141u ^ 0x42424242u ^ 0x43434343u), "the unit's build pages"
    );
    const auto listed_pages = list_unit_gui_pages(pages);
    const int lists_before = pages.lists;
    auto prelisted = mix_unit_file_checksum(pages, "ArmCom", 0, 0x01000000u, listed_pages.value);
    expect(
        listed_pages && prelisted && prelisted.value == (paged.value ^ 0x01000000u) &&
            pages.lists == lists_before,
        "pages listed once give the same checksum and are not listed again"
    );
    auto prelisted_known = mix_unit_file_checksum(pages, "ArmCom", 7, 1, listed_pages.value);
    expect(prelisted_known && prelisted_known.value == 7u, "a known checksum is kept");
    auto prelisted_bad = mix_unit_file_checksum(pages, "a\\b", 0, 0, listed_pages.value);
    expect(
        !prelisted_bad && prelisted_bad.error.code == ErrorCode::malformed,
        "a name with a separator is refused with pages listed"
    );

    SyncAssets question;
    question.guis = {"guis/AXM.gui", "guis/AM.gui"};
    question.files["guis/AXM.gui"] = "A";
    question.files["guis/AM.gui"] = "B";
    auto asked = mix_unit_file_checksum(question, "A?M", 0, 0);
    expect(asked && asked.value == 0x41414141u, "? matches one character");

    SyncAssets bad_name;
    auto separated = mix_unit_file_checksum(bad_name, "a/b", 0, 0);
    expect(
        !separated && separated.error.code == ErrorCode::malformed && bad_name.lists == 0,
        "a name with a separator is refused"
    );

    SyncAssets broken;
    broken.fail_list = true;
    auto listed = mix_unit_file_checksum(broken, "ARMCOM", 0, 0);
    expect(!listed && listed.error.code == ErrorCode::io, "a failed listing is an error");
}

std::string fold(std::string path) {
    for (auto& c : path)
        c = c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return path;
}

struct MemoryFiles {
    std::map<std::string, std::string> files;
    std::map<std::string, std::string> translations;
    // Text naming where the files come from; null answers -1.
    const char* source_name = nullptr;
    // Times the TNT was sized or read, so a cache hit can be told from a recompute.
    int opens = 0;
};

int32_t memory_size(void* context, const char* path) {
    auto* files = static_cast<MemoryFiles*>(context);
    ++files->opens;
    const auto found = files->files.find(fold(path));
    return found == files->files.end() ? -1 : static_cast<int32_t>(found->second.size());
}

int32_t memory_read(void* context, const char* path, char* buffer, uint32_t capacity) {
    auto* files = static_cast<MemoryFiles*>(context);
    ++files->opens;
    const auto found = files->files.find(fold(path));
    if (found == files->files.end())
        return -1;
    const auto count = std::min<std::size_t>(capacity, found->second.size());
    std::memcpy(buffer, found->second.data(), count);
    return static_cast<int32_t>(count);
}

void memory_list(
    void* context,
    const char* directory,
    const char* extension,
    void (*visit)(void*, const char*),
    void* visit_context
) {
    auto* files = static_cast<MemoryFiles*>(context);
    const auto prefix = fold(directory) + "/";
    std::string suffix = ".";
    suffix += fold(extension);
    for (const auto& [path, _] : files->files)
        if (path.rfind(prefix, 0) == 0 && path.size() > suffix.size() &&
            path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0) {
            std::string name = path.substr(prefix.size());
            name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
            visit(visit_context, name.c_str());
        }
}

int32_t memory_source(void* context, const char*, char* out, uint32_t capacity) {
    const auto* text = static_cast<MemoryFiles*>(context)->source_name;
    if (text == nullptr || out == nullptr || capacity == 0)
        return -1;
    uint32_t full = 0;
    while (text[full] != '\0')
        ++full;
    const uint32_t written = full < capacity ? full : capacity - 1;
    std::memcpy(out, text, written);
    out[written] = '\0';
    return static_cast<int32_t>(full);
}

const char* memory_translate(void* context, const char* text) {
    auto* files = static_cast<MemoryFiles*>(context);
    const auto found = files->translations.find(text);
    return found == files->translations.end() ? nullptr : found->second.c_str();
}

CampaignFiles services(MemoryFiles& files) {
    CampaignFiles services{};
    services.context = &files;
    services.size = memory_size;
    services.read = memory_read;
    services.list = memory_list;
    services.translate = memory_translate;
    return services;
}

std::string ota(const char* schema_types) {
    std::string text = "[GlobalHeader]\n{\nmissionname=Test;\n";
    int index = 0;
    for (const char* type = schema_types; *type != '\0'; type += std::strlen(type) + 1)
        text += "[Schema " + std::to_string(index++) + "]\n{\nType=" + type +
                ";\n[specials]\n{\n[special0]\n{\nspecialwhat=StartPos1;\nXPos=16;\nZPos=16;\n}\n}"
                "\n}\n";
    return text + "}\n";
}

void put_u32(std::string& bytes, std::size_t offset, uint32_t value) {
    for (int i = 0; i < 4; ++i)
        bytes[offset + i] = static_cast<char>((value >> (8 * i)) & 0xff);
}

// Version 0x2000, 2x2 cells with attributes 1..16 at 0x40, one feature
// record naming Rock01 at 0x50.
std::string terrain(uint32_t version) {
    std::string bytes(0x40 + 16 + 0x84, '\0');
    put_u32(bytes, 0x00, version);
    put_u32(bytes, 0x04, 2);
    put_u32(bytes, 0x08, 2);
    put_u32(bytes, 0x0c, 0x40);
    put_u32(bytes, 0x10, 0x40);
    put_u32(bytes, 0x1c, 1);
    put_u32(bytes, 0x20, 0x50);
    for (int i = 0; i < 16; ++i)
        bytes[0x40 + i] = static_cast<char>(i + 1);
    std::memcpy(&bytes[0x50], "Rock01", 6);
    return bytes;
}

// Hash terms of the synthetic terrain, from the buffer hash:
// header 0x71D571F5, attributes 0x501088, feature 0x2AA834F0.
constexpr uint32_t kTerrainHash = 0x5B2D558D;

struct SelectedMap {
    CampaignFile* file = new CampaignFile;

    SelectedMap() {
        campaign_file_init(file);
        file->kind = SessionKind::multiplayer;
    }

    ~SelectedMap() {
        campaign_file_free(file);
        delete file;
    }
};

void content_hash_tests() {
    MemoryFiles memory;
    memory.files["maps/alpha.ota"] = ota("Network 1\0");
    memory.files["maps/alpha.tnt"] = terrain(0x2000);
    memory.files["maps/old.ota"] = ota("Network 1\0");
    memory.files["maps/old.tnt"] = terrain(0x1020);
    const auto files = services(memory);
    const CampaignEnv env{&files, nullptr, 0, 0};
    MapHashCache cache{};

    SelectedMap first;
    expect(campaign_select_mission(first.file, &env, "Alpha"), "alpha selects");
    const auto hash = map_compute_content_hash(*first.file, files, cache);
    expect(
        (hash ^ first.file->header_hash) == kTerrainHash, "hash covers header, cells and features"
    );
    expect(first.file->content_hash == kTerrainHash, "content hash cached in the map context");
    expect(cache.count == 1, "content hash cached by path");

    // A second context resolves from the path cache without reading the TNT.
    SelectedMap second;
    expect(campaign_select_mission(second.file, &env, "ALPHA"), "alpha selects again");
    memory.files.erase("maps/alpha.tnt");
    expect(map_compute_content_hash(*second.file, files, cache) == hash, "cache hit is case-blind");
    expect(second.file->content_hash == kTerrainHash, "cache hit fills the context");

    // Reselecting clears the context's hash; the cache still answers.
    expect(campaign_select_mission(second.file, &env, "Alpha"), "reselect");
    expect(second.file->content_hash == 0, "reselect clears the hash");
    expect(map_compute_content_hash(*second.file, files, cache) == hash, "rehash from cache");

    SelectedMap legacy;
    expect(campaign_select_mission(legacy.file, &env, "Old"), "old selects");
    expect(map_compute_content_hash(*legacy.file, files, cache) == 0, "0x1020 terrain hashes to 0");
    expect(legacy.file->content_hash == 0 && cache.count == 1, "failed hash is not cached");

    memory.files["maps/short.ota"] = ota("Network 1\0");
    auto truncated = terrain(0x2000);
    truncated.resize(0x48);
    memory.files["maps/short.tnt"] = truncated;
    SelectedMap shortened;
    expect(campaign_select_mission(shortened.file, &env, "Short"), "short selects");
    expect(
        map_compute_content_hash(*shortened.file, files, cache) == 0, "truncated terrain rejected"
    );
    map_destroy_hash_cache(cache);
    expect(cache.entries == nullptr && cache.count == 0, "cache freed");
}

void bind_terrain(CampaignFile& file, const char* path) {
    std::memcpy(
        file.paths[static_cast<uint32_t>(CampaignPath::mission)], path, std::strlen(path) + 1
    );
}

// Two releases of one pack map share a TNT path and differ in the provider's
// serial. The second is hashed again; asking for the first again, after the
// file is gone, answers from the cache.
void provider_keyed_hash_tests() {
    MemoryFiles memory;
    memory.files["maps/isle@isles.tnt"] = terrain(0x2000);
    memory.source_name = "pack:isles#1";
    auto files = services(memory);
    files.source = memory_source;
    MapHashCache cache{};

    SelectedMap first;
    bind_terrain(*first.file, "maps/isle@isles.tnt");
    const auto first_hash = map_compute_content_hash(*first.file, files, cache);
    expect(first_hash != 0 && cache.count == 1, "the first release is hashed");

    auto revised = terrain(0x2000);
    revised[0x50] = 'S';
    memory.files["maps/isle@isles.tnt"] = revised;
    memory.source_name = "pack:isles#2";
    SelectedMap second;
    bind_terrain(*second.file, "maps/isle@isles.tnt");
    const auto second_hash = map_compute_content_hash(*second.file, files, cache);
    expect(second_hash != 0 && second_hash != first_hash, "a new serial is hashed again");
    expect(cache.count == 2, "each release keeps its own entry");

    memory.source_name = "pack:isles#1";
    memory.files.erase("maps/isle@isles.tnt");
    const int opens = memory.opens;
    SelectedMap again;
    bind_terrain(*again.file, "maps/isle@isles.tnt");
    expect(
        map_compute_content_hash(*again.file, files, cache) == first_hash,
        "the first release answers from the cache"
    );
    expect(memory.opens == opens, "the cached release is not read again");
    map_destroy_hash_cache(cache);
}

// Coast To Coast of the installed game, read through its store as the game
// reads it.
void installed_map_tests(const oa::AssetStore& assets) {
    const CampaignFiles files = oa::data::campaign::campaign_asset_files(assets);
    SelectedMap selected;
    const CampaignEnv env{&files, nullptr, 0, 0};
    MapHashCache cache{};
    expect(
        campaign_select_mission(selected.file, &env, "Coast To Coast"), "Coast To Coast selects"
    );
    expect(map_compute_content_hash(*selected.file, files, cache) != 0, "installed map hashes");
    map_destroy_hash_cache(cache);
}

} // namespace

int main(int argc, char** argv) {
    if (oa::test::game_data_requested(argc, argv)) {
        installed_map_tests(oa::test::require_game_assets("the installed map content hash"));
    } else {
        unit_checksum_tests();
        content_hash_tests();
        provider_keyed_hash_tests();
    }
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "lobby sync checks passed\n";
    return 0;
}
