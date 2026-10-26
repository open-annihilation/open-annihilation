// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// This build's package id. A steam-deck directory is tried in a scratch
// folder. Nothing is fetched.

#include "oa/app/update/build.hpp"

#include "oa/app/version_line.hpp"
#include "oa/data/engine_feed/feed.hpp"
#include "oa/formats/oamod/package_keys.hpp"
#include "oa/test/check.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace update = oa::app::update;

#if defined(OA_WINDOWS_95)
constexpr bool kWindows95 = true;
#else
constexpr bool kWindows95 = false;
#endif

/// A scratch folder removed when the case ends.
struct Scratch {
    fs::path path;

    explicit Scratch(std::string_view name) {
        const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() /
               (std::string("oa-v03-") + std::string(name) + "-" + std::to_string(ticks));
        fs::create_directories(path);
    }

    ~Scratch() {
        std::error_code error;
        fs::remove_all(path, error);
    }
};

void package_ids() {
    const std::string_view platforms[] = {
        "macos-universal",
        "windows-x64",
        "windows-x86",
        "windows-arm64",
        "linux-x86_64",
        "linux-arm64",
        "linux-armhf",
        "ios",
        "android",
        "unknown",
    };
    for (const std::string_view platform : platforms) {
        OA_CHECK(update::package_id(platform, false, false) == platform);
        OA_CHECK(update::package_id(platform, true, true) == "windows-95");
    }
    OA_CHECK(update::package_id("linux-x86_64", false, true) == "steam-deck");
    OA_CHECK(update::package_id("linux-arm64", false, true) == "linux-arm64");
    OA_CHECK(update::package_id("macos-universal", false, true) == "macos-universal");
    OA_CHECK(update::package_id("ios", false, false) == "ios");
}

void this_build_matches_the_compiled_version() {
    const update::ThisBuild built = update::this_build({});
#if defined(OA_ENGINE_VERSION)
    const std::string_view compiled = OA_ENGINE_VERSION;
    const std::optional<oa::formats::oamod::EngineVersion> parsed =
        oa::formats::oamod::parse_engine_version(compiled);
    OA_CHECK(parsed.has_value());
    if (parsed) {
        OA_CHECK(built.version.major == parsed->major);
        OA_CHECK(built.version.minor == parsed->minor);
        OA_CHECK(built.version.patch == parsed->patch);
        OA_CHECK(built.version_text == oa::formats::oamod::engine_version_text(*parsed));
        const std::optional<int64_t> number = oa::data::engine_feed::release_number(*parsed);
        OA_CHECK(built.release == (number ? *number : 0));
    }
#else
    OA_CHECK(built.version_text.empty());
    OA_CHECK(built.release == 0);
#endif
#if defined(OA_ENGINE_VERSION_LABEL)
    OA_CHECK(built.development == (std::string_view(OA_ENGINE_VERSION_LABEL).size() != 0));
#else
    OA_CHECK(!built.development);
#endif
    OA_CHECK(
        built.package == update::package_id(oa::app::release_platform_name(), kWindows95, false)
    );
}

void steam_deck_folder_follows_the_platform() {
    Scratch scratch("deck");
    const fs::path folder = scratch.path / "steam-deck";
    fs::create_directory(folder);
    const update::ThisBuild deck = update::this_build(scratch.path);
    OA_CHECK(
        deck.package == update::package_id(oa::app::release_platform_name(), kWindows95, true)
    );
    fs::remove(folder);
    std::ofstream file(scratch.path / "steam-deck", std::ios::binary);
    file << "not a folder";
    file.close();
    const update::ThisBuild file_marker = update::this_build(scratch.path);
    OA_CHECK(
        file_marker.package ==
        update::package_id(oa::app::release_platform_name(), kWindows95, false)
    );
}

void steam_deck_folder_is_beside_the_versions() {
    Scratch scratch("deck-root");
    const fs::path version = scratch.path / "versions" / "0.8.6";
    fs::create_directories(version / "steam-deck");
    const update::ThisBuild inside = update::this_build(version);
    OA_CHECK(
        inside.package == update::package_id(oa::app::release_platform_name(), kWindows95, false)
    );
    fs::create_directory(scratch.path / "steam-deck");
    const update::ThisBuild beside = update::this_build(version);
    OA_CHECK(
        beside.package == update::package_id(oa::app::release_platform_name(), kWindows95, true)
    );
}

/// The words a notice looked up, and the marked translation it was given.
struct LookedUp {
    std::vector<std::string> words;
    std::string held;
};

/// Wraps the English in «» and records it. The command must never arrive.
///
/// @param context the LookedUp
/// @param english the word
/// @return the marked word; valid until the next call
std::string_view mark_word(void* context, std::string_view english) {
    auto* seen = static_cast<LookedUp*>(context);
    seen->words.emplace_back(english);
    seen->held = "«";
    seen->held += english;
    seen->held += "»";
    return seen->held;
}

/// True when the recorded words include this English text.
///
/// @param seen the lookup
/// @param english the word
/// @return true when it was looked up
bool looked_for(const LookedUp& seen, std::string_view english) {
    for (const std::string& word : seen.words) {
        if (word == english)
            return true;
    }
    return false;
}

void this_build_replaces_itself() {
    OA_CHECK(update::self_update_built);
    OA_CHECK(update::replaces_itself());
    OA_CHECK(update::packager_name().empty());
    OA_CHECK(update::packager_update_command().empty());
}

void packager_notice_names_a_command() {
    LookedUp seen;
    const update::PackagerNotice notice =
        update::packager_notice("0.8.7", "Ridge", "flatpak update", mark_word, &seen);
    OA_CHECK(notice.title == "«0.8.7 is out»");
    OA_CHECK(notice.paragraphs.size() == 3);
    OA_CHECK(notice.paragraphs[0] == "«This copy came from Ridge, which updates it.»");
    OA_CHECK(notice.paragraphs[1] == "«Open your software centre, or run:»");
    OA_CHECK(notice.paragraphs[2] == "flatpak update");
    OA_CHECK(!notice.open_the_page);
    OA_CHECK(looked_for(seen, update::version_is_out));
    OA_CHECK(looked_for(seen, update::copy_from_packager));
    OA_CHECK(looked_for(seen, update::open_centre_or_run));
    OA_CHECK(!looked_for(seen, "flatpak update"));
    OA_CHECK(!looked_for(seen, "Ridge"));

    LookedUp command_is_ok;
    const update::PackagerNotice same_as_button =
        update::packager_notice("0.8.7", "Ridge", "OK", mark_word, &command_is_ok);
    OA_CHECK(same_as_button.paragraphs.size() == 3);
    OA_CHECK(same_as_button.paragraphs[2] == "OK");
    OA_CHECK(!looked_for(command_is_ok, "OK"));
}

void packager_notice_names_no_command() {
    LookedUp seen;
    const update::PackagerNotice notice =
        update::packager_notice("0.8.7", "Nix", {}, mark_word, &seen);
    OA_CHECK(notice.title == "«0.8.7 is out»");
    OA_CHECK(notice.paragraphs.size() == 2);
    OA_CHECK(notice.paragraphs[0] == "«This copy came from Nix, which updates it.»");
    OA_CHECK(notice.paragraphs[1] == "«Open your software centre to update it.»");
    OA_CHECK(!notice.open_the_page);
    OA_CHECK(looked_for(seen, update::open_centre));
    OA_CHECK(!looked_for(seen, update::open_centre_or_run));
}

void packager_notice_names_nobody() {
    LookedUp seen;
    const update::PackagerNotice notice =
        update::packager_notice("0.8.7", {}, "flatpak update", mark_word, &seen);
    OA_CHECK(notice.title == "«0.8.7 is out»");
    OA_CHECK(notice.paragraphs.size() == 1);
    OA_CHECK(
        notice.paragraphs[0] ==
        "«This copy cannot replace itself. Get the new release from its page.»"
    );
    OA_CHECK(notice.open_the_page);
    OA_CHECK(looked_for(seen, update::cannot_replace));
    OA_CHECK(!looked_for(seen, "flatpak update"));
    OA_CHECK(!looked_for(seen, update::copy_from_packager));

    const update::PackagerNotice plain =
        update::packager_notice("0.8.7", "Ridge", "flatpak update", nullptr, nullptr);
    OA_CHECK(plain.title == "0.8.7 is out");
    OA_CHECK(plain.paragraphs.size() == 3);
    OA_CHECK(plain.paragraphs[2] == "flatpak update");
}

} // namespace

int main() {
    package_ids();
    this_build_matches_the_compiled_version();
    steam_deck_folder_follows_the_platform();
    steam_deck_folder_is_beside_the_versions();
    this_build_replaces_itself();
    packager_notice_names_a_command();
    packager_notice_names_no_command();
    packager_notice_names_nobody();
    return oa::test::check_exit_status();
}
