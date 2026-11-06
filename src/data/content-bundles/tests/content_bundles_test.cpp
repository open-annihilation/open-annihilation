// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Lookups, both dependency closures, turning a bundle on and off, matching
// a file by name and size, and the table's own consistency. The demo
// archive's size and hash are the ones demo_1997 already checks.

#include "oa/app/demo_installer.hpp"
#include "oa/data/content_bundles.hpp"
#include "oa/data/content_bundles/content_choice.hpp"
#include "oa/test/check.hpp"
#include "oa/test/scratch_directory.hpp"

#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <span>
#include <stddef.h>
#include <stdint.h>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using oa::data::content_bundles::Bundle;
using oa::data::content_bundles::ContentFile;
using oa::data::content_bundles::FileMatch;
using oa::data::content_bundles::Installer;
using oa::data::content_bundles::InstallerKind;
using oa::data::content_bundles::Kind;
using oa::data::content_bundles::after_turning_off;
using oa::data::content_bundles::after_turning_on;
using oa::data::content_bundles::bundles;
using oa::data::content_bundles::find_bundle;
using oa::data::content_bundles::find_installer;
using oa::data::content_bundles::installers;
using oa::data::content_bundles::known_hash;
using oa::data::content_bundles::known_installer_hash;
using oa::data::content_bundles::SourceFile;
using oa::data::content_bundles::match_file;
using oa::data::content_bundles::present_bundles;
using oa::data::content_bundles::removal_closure;
using oa::data::content_bundles::requirement_closure;
using oa::data::content_bundles::table_fault;

/// Reports whether `got` is `expected`, in that order.
///
/// @param got the ids a function returned
/// @param expected the ids the case wants
/// @return true when the two lists are the same
bool same_ids(std::span<const std::string_view> got, std::initializer_list<const char*> expected) {
    if (got.size() != expected.size())
        return false;
    size_t slot = 0;
    for (const char* id : expected) {
        if (got[slot] != id)
            return false;
        ++slot;
    }
    return true;
}

void test_find_bundle() {
    const std::span<const Bundle> table = bundles();
    OA_CHECK(table.size() == 9);
    OA_CHECK(table[0].id == "totala");
    OA_CHECK(table[0].english_name == "Total Annihilation 3.1c");
    OA_CHECK(table[0].kind == Kind::base_game);
    OA_CHECK(table[0].files.size() == 6);
    const Bundle* totala = find_bundle("totala");
    OA_CHECK(totala == &table[0]);
    const Bundle* units = find_bundle("cavedog-units");
    OA_CHECK(units != nullptr && units->kind == Kind::expansion);
    OA_CHECK(units->english_name == "Cavedog units");
    OA_CHECK(units->files.size() == 6);
    OA_CHECK(find_bundle("campaign")->files.size() == 3);
    OA_CHECK(find_bundle("core-contingency")->files.size() == 3);
    OA_CHECK(find_bundle("battle-tactics")->files.size() == 10);
    OA_CHECK(find_bundle("music")->files.size() == 18);
    OA_CHECK(find_bundle("cavedog-tilesets")->files.size() == 1);
    const Bundle* lobby = find_bundle("boneyards");
    OA_CHECK(lobby != nullptr && lobby->kind == Kind::expansion);
    OA_CHECK(lobby->english_name == "The Boneyards");
    OA_CHECK(lobby->extends.size() == 1 && lobby->extends[0] == "totala");
    OA_CHECK(lobby->needs.empty());
    OA_CHECK(lobby->files.size() == 110);
    OA_CHECK(find_bundle("Totala") == nullptr);
    OA_CHECK(find_bundle("ridge") == nullptr);
}

void test_closures() {
    const std::string_view units_id[]{"cavedog-units"};
    const auto units_need = requirement_closure(units_id);
    OA_CHECK(same_ids(units_need, {"totala", "campaign", "core-contingency", "cavedog-units"}));

    const std::string_view campaign_id[]{"campaign"};
    const auto campaign_off = removal_closure(campaign_id);
    OA_CHECK(
        same_ids(campaign_off, {"campaign", "core-contingency", "battle-tactics", "cavedog-units"})
    );

    const std::string_view totala_id[]{"totala"};
    const auto totala_off = removal_closure(totala_id);
    OA_CHECK(same_ids(
        totala_off,
        {"totala",
         "campaign",
         "core-contingency",
         "battle-tactics",
         "music",
         "cavedog-units",
         "cavedog-tilesets",
         "boneyards"}
    ));

    const std::string_view demo_id[]{"totala-demo"};
    OA_CHECK(same_ids(requirement_closure(demo_id), {"totala-demo"}));
    OA_CHECK(same_ids(removal_closure(demo_id), {"totala-demo"}));

    const std::string_view both[]{"music", "cavedog-units"};
    const auto both_need = requirement_closure(both);
    OA_CHECK(
        same_ids(both_need, {"totala", "campaign", "core-contingency", "music", "cavedog-units"})
    );

    const std::string_view unknown[]{"ridge"};
    OA_CHECK(requirement_closure(unknown).empty());
    OA_CHECK(removal_closure(unknown).empty());
    OA_CHECK(table_fault().empty());
}

void test_turning_on_and_off() {
    const auto from_empty = after_turning_on({}, "campaign");
    OA_CHECK(same_ids(from_empty, {"totala", "campaign"}));

    const std::string_view base_on[]{"totala"};
    const auto waiting = after_turning_on(base_on, "campaign");
    OA_CHECK(same_ids(waiting, {"totala", "campaign", "core-contingency", "battle-tactics"}));

    const std::string_view all[]{
        "totala",
        "campaign",
        "core-contingency",
        "battle-tactics",
        "music",
        "cavedog-units",
        "cavedog-tilesets",
        "ridge"
    };
    const auto campaign_gone = after_turning_off(all, "campaign");
    OA_CHECK(same_ids(campaign_gone, {"totala", "music", "cavedog-tilesets"}));

    const auto unchanged = after_turning_off(base_on, "campaign");
    OA_CHECK(same_ids(unchanged, {"totala"}));
    OA_CHECK(same_ids(after_turning_on(base_on, "ridge"), {"totala"}));

    const auto units_on = after_turning_on({}, "cavedog-units");
    OA_CHECK(same_ids(units_on, {"totala", "campaign", "core-contingency", "cavedog-units"}));
}

void test_match_and_hash() {
    const FileMatch archive = match_file("TOTALA1.HPI", 32311035);
    OA_CHECK(archive);
    OA_CHECK(archive.bundle != nullptr && archive.bundle->id == "totala");
    OA_CHECK(archive.file->copy_name == "totala1.hpi");
    OA_CHECK(known_hash(*archive.file, archive.file->sha256[0]));
    oa::base::sha256::Digest other = archive.file->sha256[0];
    other[0] = static_cast<uint8_t>(other[0] ^ 0x01);
    OA_CHECK(!known_hash(*archive.file, other));

    const FileMatch movie = match_file("data\\1.ZRB", 6919100);
    OA_CHECK(movie && movie.file->copy_name == "Data/1.zrb");
    OA_CHECK(movie.file->source_names.size() == 1);
    OA_CHECK(movie.file->source_names[0] == "Data/1.ZRB");

    const FileMatch by_last = match_file("2.zrb", 41229000);
    OA_CHECK(by_last && by_last.file->copy_name == "Data/2.zrb");
    OA_CHECK(!match_file("foo/totala1.hpi", 32311035));
    OA_CHECK(!match_file("totala1.hpi", 1));
    OA_CHECK(!match_file("totala3.hpi", 1));
    OA_CHECK(!match_file("Example.ufo", 1));

    const FileMatch track_one = match_file("music/1.mp3", 2849041);
    const FileMatch track_ten = match_file("music/10.mp3", 2849041);
    OA_CHECK(track_one && track_ten);
    OA_CHECK(track_one.file != track_ten.file);
    OA_CHECK(track_one.file->sha256[0] == track_ten.file->sha256[0]);
    const FileMatch track_zero = match_file("music/0.mp3", 2576323);
    const FileMatch track_nine = match_file("music/9.mp3", 2576323);
    OA_CHECK(track_zero && track_nine && track_zero.file != track_nine.file);
    OA_CHECK(track_zero.file->sha256[0] == track_nine.file->sha256[0]);
}

void test_demo_archive_matches_todays_recognition() {
    const Bundle* demo = find_bundle("totala-demo");
    OA_CHECK(demo != nullptr);
    OA_CHECK(demo->kind == Kind::base_game);
    OA_CHECK(demo->extends.empty() && demo->needs.empty());
    OA_CHECK(demo->files.size() == 1);
    const ContentFile& unpacked = demo->files[0];
    OA_CHECK(unpacked.copy_name == oa::app::demo_1997.archive_name);
    OA_CHECK(unpacked.size == oa::app::demo_1997.archive_size);
    OA_CHECK(unpacked.sha256.size() == 1);
    OA_CHECK(unpacked.sha256[0] == oa::app::demo_1997.archive_sha256);
    OA_CHECK(known_hash(unpacked, oa::app::demo_1997.archive_sha256));
    OA_CHECK(unpacked.size != oa::app::demo_1997.installer_size);
}

/// Names, sizes and hash prefixes of the expansion, and a folder of tiny
/// files with those names. The bytes are the test's own.
struct ListedFile {
    const char* name{};
    uint64_t size{};
    const char* prefix{};
};

/// Reports whether `ids` holds `id`.
///
/// @param ids the ids
/// @param id the one to find
/// @return true when it is there
bool listed(std::span<const std::string_view> ids, std::string_view id) {
    for (const std::string_view one : ids)
        if (one == id)
            return true;
    return false;
}

void test_lobby_files() {
    const Bundle* bundle = find_bundle("boneyards");
    OA_CHECK(bundle != nullptr);
    if (bundle == nullptr)
        return;
    static const ListedFile rows[] = {
        {"by.ccx", 1199000, "075b36626e917f4e"},
        {"Boneyards/BYPacific_map.gwm", 17231, "08411f077c9071ba"},
        {"Boneyards/Help/Help.def", 3831, "0fea3da685aca642"},
        {"Boneyards/Help/Help112.htm", 986, "ed42b3927f86913f"},
        {"Boneyards/Help/Help114.htm", 819, "a374dfb51b01642b"},
        {"Boneyards/Help/Help115.htm", 424, "a9746eb3ff81354b"},
        {"Boneyards/Help/Help123.htm", 3526, "6be2b793a36c4668"},
        {"Boneyards/Help/Help124.htm", 831, "6b64f966eb8c1131"},
        {"Boneyards/Help/Help125.htm", 783, "1085de4dfc8c3004"},
        {"Boneyards/Help/Help128.htm", 964, "cef393c37c59246c"},
        {"Boneyards/Help/Help168.htm", 664, "fa2b56d85fb41fbc"},
        {"Boneyards/Help/Help201.htm", 2358, "2dbfec7af9a79843"},
        {"Boneyards/Help/Help226.htm", 565, "acf12e7bc199a496"},
        {"Boneyards/Help/Help227.htm", 697, "10381d25b0c9c609"},
        {"Boneyards/Help/Help38.htm", 3329, "b32ccce93142eebe"},
        {"Boneyards/Help/Help39.htm", 222, "3288808277167ec6"},
        {"Boneyards/Help/Help40.htm", 961, "45c376861bbb4ea6"},
        {"Boneyards/Help/Help41.htm", 3429, "5b85cbe4b681e431"},
        {"Boneyards/Help/Help42.htm", 3594, "61ec47427ef8bc5c"},
        {"Boneyards/Help/Help45.htm", 785, "f3f21ca855fb9d10"},
        {"Boneyards/Help/Help46.htm", 1809, "5e916fc99a4b744b"},
        {"Boneyards/Help/Help47.htm", 2246, "4e8f54a69e3e70a6"},
        {"Boneyards/Help/Help48.htm", 4759, "603a2c37043d2b2b"},
        {"Boneyards/Help/Help50.htm", 941, "a15df5c9fd7940cf"},
        {"Boneyards/Help/Help53.htm", 1270, "e08290d809f42ce3"},
        {"Boneyards/Help/Help54.htm", 842, "19beb5f5f1008993"},
        {"Boneyards/Help/Help57.htm", 933, "d1bc40e6b81ff159"},
        {"Boneyards/Help/Help59.htm", 922, "8ed5901b5fff9711"},
        {"Boneyards/Help/Help60.htm", 2108, "56d8c0d196b5cb56"},
        {"Boneyards/Help/Help66.htm", 1043, "1f79e45bd58ccce8"},
        {"Boneyards/Help/Help67.htm", 1170, "a21aea9ca5551373"},
        {"Boneyards/Help/Help68.htm", 1442, "1942ab74543ef610"},
        {"Boneyards/Help/Help69.htm", 1186, "53c6d2f5095aafa6"},
        {"Boneyards/Help/Help70.htm", 1149, "3e9a619757caff6c"},
        {"Boneyards/Help/Help71.htm", 1959, "1a85198369f2d94f"},
        {"Boneyards/Help/Help72.htm", 2112, "526132a8bdc3b793"},
        {"Boneyards/Help/Help73.htm", 2298, "395440d317b74b19"},
        {"Boneyards/Help/Help74.htm", 2550, "db65eb996932be20"},
        {"Boneyards/Help/Help75.htm", 1981, "da945c22980fcf76"},
        {"Boneyards/Ladders/Ladders.def", 1147, "c1704475c0ffe901"},
        {"Boneyards/Profile/BYInfoTemplate.htm", 320, "72e936c84ab494b8"},
        {"Boneyards/Profile/Duel_LadderPU.htm", 693, "e7a2f7c6167764aa"},
        {"Boneyards/Profile/GWarArm_LadderPU.htm", 1697, "acd185d09810f6cc"},
        {"Boneyards/Profile/GWarCore_LadderPU.htm", 1700, "bd59962e4efadac7"},
        {"Boneyards/Profile/GWarTrainingArm_LadderPU.htm", 382, "7c25a44a895cb9da"},
        {"Boneyards/Profile/GWarTrainingCore_LadderPU.htm", 383, "103fd1844c617713"},
        {"Boneyards/Profile/PopupInfoTemplate.htm", 1109, "d70fae6685bcb0f9"},
        {"Boneyards/Profile/Profile.def", 1581, "e5385f3d514d5722"},
        {"Boneyards/Profile/WTA_LadderPU.htm", 632, "a24499f7ae2ebb1d"},
        {"Boneyards/images/GameForming.png", 988, "25d375a7928fd614"},
        {"Boneyards/images/GameProgress.png", 993, "5e2171ab5e5f45bb"},
        {"Boneyards/images/I-battle.png", 1826, "cc83a4200cdae97a"},
        {"Boneyards/images/I-chat.png", 1844, "3876dc1040b001bb"},
        {"Boneyards/images/I-help.png", 1805, "3ddad4cecd27f1ca"},
        {"Boneyards/images/I-ladders.png", 1859, "36fa64e3776710a9"},
        {"Boneyards/images/I-locator.png", 1850, "023f74a14b09990b"},
        {"Boneyards/images/I-news.png", 1880, "c950902358fc3927"},
        {"Boneyards/images/I-profile.png", 1816, "8d5ddccaa15eb364"},
        {"Boneyards/images/I-quickplay.png", 1677, "45a4d50ddd2c76dc"},
        {"Boneyards/images/I-recon.png", 1786, "378a47d1cfa824f5"},
        {"Boneyards/images/RANK_ARM0.png", 998, "6b095f90b0841981"},
        {"Boneyards/images/RANK_ARM1.png", 996, "2eebb0fb8f86277c"},
        {"Boneyards/images/RANK_ARM2.png", 1001, "acf9005b5aad93b7"},
        {"Boneyards/images/RANK_ARM3.png", 997, "6fc3f2c809324dfe"},
        {"Boneyards/images/RANK_ARM4.png", 988, "39e96b4f7ceb2d74"},
        {"Boneyards/images/RANK_ARM5.png", 983, "8459eeb30f75dae1"},
        {"Boneyards/images/RANK_ARM6.png", 979, "8d32c6180cf922b6"},
        {"Boneyards/images/RANK_ARM7.png", 980, "a427243ec8bcc162"},
        {"Boneyards/images/RANK_ARM8.png", 976, "42aa35728f74bc96"},
        {"Boneyards/images/RANK_CORE0.png", 998, "8697814a9a9417ca"},
        {"Boneyards/images/RANK_CORE1.png", 996, "75055e695b734537"},
        {"Boneyards/images/RANK_CORE2.png", 1001, "d17d06939c3a675e"},
        {"Boneyards/images/RANK_CORE3.png", 997, "eedc5cb3724b3f62"},
        {"Boneyards/images/RANK_CORE4.png", 988, "e292752c596c8a9a"},
        {"Boneyards/images/RANK_CORE5.png", 983, "dae600c50148eafd"},
        {"Boneyards/images/RANK_CORE6.png", 979, "712e1699aee251b1"},
        {"Boneyards/images/RANK_CORE7.png", 980, "883cfc0e2256b5a1"},
        {"Boneyards/images/RANK_CORE8.png", 976, "77bc4d42a361c09f"},
        {"Boneyards/images/amen_logo.png", 2358, "b7cd396b9e0d53ed"},
        {"Boneyards/images/anon.png", 983, "c91934702b651cec"},
        {"Boneyards/images/battles.png", 18086, "49169bad40edfc13"},
        {"Boneyards/images/chat.png", 19313, "981f0a80ea56dac4"},
        {"Boneyards/images/help.png", 22947, "228531ddd7b57488"},
        {"Boneyards/images/king_logo.png", 5086, "3584fb4ea077f039"},
        {"Boneyards/images/ladder.png", 19647, "34df4f5fc0ec0289"},
        {"Boneyards/images/latBad.png", 958, "4abf24cdd68addf8"},
        {"Boneyards/images/latGood.png", 958, "6df9344510eb18a4"},
        {"Boneyards/images/latOK.png", 958, "7de9419f3e239a4f"},
        {"Boneyards/images/locator.png", 23814, "601f9fd5a3802b9b"},
        {"Boneyards/images/login.png", 22558, "56be82667315d96e"},
        {"Boneyards/images/map.png", 23882, "ab3db12edbf11d85"},
        {"Boneyards/images/news.png", 19811, "a18a9f37ea0535c7"},
        {"Boneyards/images/profile.png", 23641, "966e4e9a1823350a"},
        {"Boneyards/images/r1.png", 976, "42aa35728f74bc96"},
        {"Boneyards/images/r2.png", 980, "a427243ec8bcc162"},
        {"Boneyards/images/r3.png", 979, "8d32c6180cf922b6"},
        {"Boneyards/images/r4.png", 983, "8459eeb30f75dae1"},
        {"Boneyards/images/r5.png", 988, "39e96b4f7ceb2d74"},
        {"Boneyards/images/r6.png", 997, "6fc3f2c809324dfe"},
        {"Boneyards/images/r7.png", 1001, "acf9005b5aad93b7"},
        {"Boneyards/images/r8.png", 996, "2eebb0fb8f86277c"},
        {"Boneyards/images/r9.png", 998, "6b095f90b0841981"},
        {"Boneyards/images/recon.png", 24508, "6ecf7d7809ef7d55"},
        {"Boneyards/images/reg.png", 969, "2610f8ac259b8314"},
        {"Boneyards/images/s-tabtbadge.png", 7706, "3a16d51b6f2c265a"},
        {"Boneyards/images/s-taccbadge.png", 8597, "0bba7b9a903e90cc"},
        {"Boneyards/images/sysop.png", 1002, "b6c61bc325607f8f"},
        {"Boneyards/images/tacommpack.png", 7423, "b5c94165d366cd1e"},
        {"Boneyards/images/update.png", 24413, "5d55d3d7e28ca2b1"},
        {"Boneyards/server.byd", 46, "cb8fa443baa72625"},
    };
    OA_CHECK(bundle->files.size() == sizeof rows / sizeof rows[0]);
    const size_t count = bundle->files.size();
    std::vector<SourceFile> at_size;
    std::vector<SourceFile> wrong_size;
    at_size.reserve(count);
    wrong_size.reserve(count);
    for (size_t at = 0; at < count && at < sizeof rows / sizeof rows[0]; ++at) {
        const ContentFile& file = bundle->files[at];
        const ListedFile& row = rows[at];
        OA_CHECK(file.copy_name == row.name);
        OA_CHECK(file.size == row.size);
        OA_CHECK(file.sha256.size() == 1);
        if (!file.sha256.empty()) {
            const auto hex = oa::base::sha256::to_hex(file.sha256[0]);
            OA_CHECK(std::string_view(hex.data(), 16) == row.prefix);
        }
        at_size.push_back(SourceFile{file.copy_name, file.size});
        wrong_size.push_back(SourceFile{file.copy_name, uint64_t{1}});
    }
    OA_CHECK(same_ids(present_bundles(at_size), {"boneyards"}));
    OA_CHECK(!listed(present_bundles(wrong_size), "boneyards"));

    namespace fs = std::filesystem;
    const fs::path folder = oa::test::make_scratch_directory("X01-expansion");
    const fs::path archive = folder / "by.ccx";
    {
        std::ofstream out(archive, std::ios::binary);
        const char byte = 'x';
        out.write(&byte, 1);
    }
    std::ifstream in(archive, std::ios::binary);
    std::vector<uint8_t> bytes(1);
    in.read(reinterpret_cast<char*>(bytes.data()), 1);
    OA_CHECK(static_cast<bool>(in));
    OA_CHECK(fs::file_size(archive) == 1);
    const ContentFile& first = bundle->files[0];
    OA_CHECK(first.copy_name == "by.ccx");
    OA_CHECK(!known_hash(first, oa::base::sha256::digest_of(bytes)));
    const SourceFile alone[]{SourceFile{first.copy_name, uint64_t{1}}};
    OA_CHECK(!listed(present_bundles(alone), "boneyards"));
    for (const ListedFile& row : rows) {
        fs::path at = folder;
        std::string_view rest = row.name;
        while (true) {
            const size_t slash = rest.find('/');
            if (slash == std::string_view::npos) {
                std::ofstream out(at / std::string(rest), std::ios::binary);
                const char byte = 'x';
                out.write(&byte, 1);
                break;
            }
            at /= std::string(rest.substr(0, slash));
            std::filesystem::create_directories(at);
            rest.remove_prefix(slash + 1);
        }
        OA_CHECK(fs::file_size(folder / row.name) == 1);
    }
    std::error_code error;
    fs::remove_all(folder, error);
}

void test_installers() {
    const std::span<const Installer> known = installers();
    OA_CHECK(known.size() == 3);
    const Installer* windows = find_installer(1093885504);
    OA_CHECK(windows != nullptr && windows->kind == InstallerKind::windows);
    OA_CHECK(windows == &known[0]);
    OA_CHECK(known_installer_hash(*windows, windows->sha256));
    oa::base::sha256::Digest other = windows->sha256;
    other[0] = static_cast<uint8_t>(other[0] ^ 0x01);
    OA_CHECK(!known_installer_hash(*windows, other));
    const Installer* macos = find_installer(2559350261);
    OA_CHECK(macos != nullptr && macos->kind == InstallerKind::macos);
    const Installer* older = find_installer(1144736288);
    OA_CHECK(older != nullptr && older->kind == InstallerKind::older_mac);
    OA_CHECK(find_installer(oa::app::demo_1997.installer_size) == nullptr);
    OA_CHECK(find_installer(1) == nullptr);
}

} // namespace

int main() {
    test_find_bundle();
    test_closures();
    test_turning_on_and_off();
    test_match_and_hash();
    test_demo_archive_matches_todays_recognition();
    test_lobby_files();
    test_installers();
    return oa::test::check_exit_status();
}
