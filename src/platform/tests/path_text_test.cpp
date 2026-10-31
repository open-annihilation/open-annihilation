// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A path's UTF-8 spelling and the path a UTF-8 spelling names: the
// conversions between UTF-16 and UTF-8 that Windows paths take, text that is
// not well-formed UTF-8, and a folder whose names no one code page holds,
// listed, opened and replaced by name.

#include "oa/platform/files.hpp"
#include "oa/test/check.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace platform = oa::platform;

/// Names that no one code page holds: Cyrillic, Chinese and Japanese, Greek,
/// an accented Latin letter, and a character beyond the Basic Multilingual
/// Plane, which UTF-16 writes as a surrogate pair.
constexpr std::string_view mixed_scripts =
    "\xd0\x9a\xd0\xb0\xd1\x80\xd1\x82\xd0\xb0 \xe5\x9c\xb0\xe5\x9b\xb3 "
    "\xce\xa7\xce\xac\xcf\x81\xcf\x84\xce\xb7\xcf\x82 caf\xc3\xa9 \xf0\x9f\x97\xba";
/// The same text in UTF-16.
constexpr std::u16string_view mixed_scripts_utf16 =
    u"\u041a\u0430\u0440\u0442\u0430 \u5730\u56f3 \u03a7\u03ac\u03c1\u03c4\u03b7\u03c2 "
    u"caf\u00e9 \U0001f5fa";

/// A folder removed when the test ends.
struct Scratch {
    fs::path path{};

    Scratch() {
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / ("oa-path-text-" + std::to_string(tick));
        fs::create_directories(path);
    }

    ~Scratch() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};

/// Reads a file whole by its path.
///
/// @param file the file
/// @return what it holds; empty when it cannot be opened
std::string read_text(const fs::path& file) {
    std::FILE* stream = platform::open_file(file, "rb");
    if (stream == nullptr)
        return {};
    std::string text;
    char buffer[256];
    std::size_t got = 0;
    while ((got = std::fread(buffer, 1, sizeof buffer, stream)) != 0)
        text.append(buffer, got);
    std::fclose(stream);
    return text;
}

/// Whole characters go from UTF-16 to UTF-8 and back unchanged.
void converts_whole_characters() {
    OA_CHECK(platform::utf8_from_utf16(u"") == "");
    OA_CHECK(platform::utf8_from_utf16(u"totala1.hpi") == "totala1.hpi");
    OA_CHECK(platform::utf8_from_utf16(mixed_scripts_utf16) == mixed_scripts);
    OA_CHECK(platform::utf16_from_utf8(mixed_scripts) == mixed_scripts_utf16);
    // The last character of each UTF-8 length, and the last of all.
    OA_CHECK(platform::utf8_from_utf16(u"\u007f\u07ff\uffff") == "\x7f\xdf\xbf\xef\xbf\xbf");
    OA_CHECK(platform::utf8_from_utf16(u"\U0010ffff") == "\xf4\x8f\xbf\xbf");
    OA_CHECK(platform::utf16_from_utf8("\xf4\x8f\xbf\xbf") == u"\U0010ffff");
}

/// UTF-16 that UTF-8 cannot carry, a surrogate without its other half,
/// becomes U+FFFD; the characters around it are kept.
void replaces_lone_surrogates() {
    const std::u16string high_alone{u'a', char16_t(0xd83d), u'b'};
    OA_CHECK(
        platform::utf8_from_utf16(high_alone) == "a\xef\xbf\xbd"
                                                 "b"
    );
    const std::u16string low_alone{u'a', char16_t(0xdc00), u'b'};
    OA_CHECK(
        platform::utf8_from_utf16(low_alone) == "a\xef\xbf\xbd"
                                                "b"
    );
    const std::u16string high_last{u'a', char16_t(0xd83d)};
    OA_CHECK(platform::utf8_from_utf16(high_last) == "a\xef\xbf\xbd");
    const std::u16string swapped{char16_t(0xdc00), char16_t(0xd83d)};
    OA_CHECK(platform::utf8_from_utf16(swapped) == "\xef\xbf\xbd\xef\xbf\xbd");
}

/// A byte that does not begin well-formed UTF-8 stands for the character of
/// its own value, so that no text is refused and Latin-1 text reads as it was
/// meant.
void reads_bytes_that_are_not_utf8() {
    // A Latin-1 name: "Carte \xe9t\xe9".
    OA_CHECK(platform::utf16_from_utf8("Carte \xe9t\xe9") == u"Carte \u00e9t\u00e9");
    // A continuation byte with no lead, an overlong '/', an encoded
    // surrogate, a cut-off sequence and one past U+10FFFF.
    OA_CHECK(platform::utf16_from_utf8("\x80") == u"\u0080");
    OA_CHECK(platform::utf16_from_utf8("\xc0\xaf") == u"\u00c0\u00af");
    OA_CHECK(platform::utf16_from_utf8("\xed\xa0\x80") == u"\u00ed\u00a0\u0080");
    OA_CHECK(platform::utf16_from_utf8("\xe2\x82") == u"\u00e2\u0082");
    OA_CHECK(platform::utf16_from_utf8("\xf4\x90\x80\x80") == u"\u00f4\u0090\u0080\u0080");
    // A bad byte does not swallow the good character after it.
    OA_CHECK(platform::utf16_from_utf8("\xe9\xc3\xa9") == u"\u00e9\u00e9");
}

/// A UTF-8 spelling names the path that spells back to it, on every system.
void spells_paths_in_utf8() {
    const fs::path name = platform::path_from_utf8(mixed_scripts);
    OA_CHECK(platform::path_to_utf8(name) == mixed_scripts);
    OA_CHECK(name == fs::path(std::u8string(mixed_scripts.begin(), mixed_scripts.end())));
    const fs::path nested = platform::path_from_utf8("Maps") / name / "maps";
    OA_CHECK(platform::path_to_utf8(nested.filename()) == "maps");
    OA_CHECK(platform::path_to_utf8(nested.parent_path().filename()) == mixed_scripts);
    OA_CHECK(
        platform::path_to_generic_utf8(nested) == "Maps/" + std::string(mixed_scripts) + "/maps"
    );
    OA_CHECK(platform::path_to_utf8(fs::path{}).empty());
    OA_CHECK(platform::path_from_utf8("").empty());
#if defined(_WIN32)
    // Windows writes its own separator; the generic spelling writes slashes.
    OA_CHECK(platform::path_to_utf8(nested) == "Maps\\" + std::string(mixed_scripts) + "\\maps");
    OA_CHECK(platform::path_from_utf8("C:\\Games\\TA").native() == L"C:\\Games\\TA");
    OA_CHECK(
        platform::path_to_generic_utf8(platform::path_from_utf8("C:\\Games\\TA")) == "C:/Games/TA"
    );
#else
    // Elsewhere a path is its bytes, UTF-8 or not.
    OA_CHECK(platform::path_from_utf8("Carte \xe9t\xe9").native() == "Carte \xe9t\xe9");
    OA_CHECK(platform::path_to_utf8(fs::path("Carte \xe9t\xe9")) == "Carte \xe9t\xe9");
#endif
}

/// A folder holding names no one code page holds is listed with each name's
/// UTF-8 spelling, and each file opens and is replaced by the path its
/// spelling names.
void lists_a_folder_of_names_outside_any_code_page() {
    const Scratch scratch;
    const fs::path maps = scratch.path / platform::path_from_utf8(mixed_scripts);
    const std::vector<std::string> names{
        std::string(mixed_scripts) + ".tnt",
        "\xe6\x88\xa6\xe5\xa0\xb4.ota",                                 // Chinese and Japanese
        "\xd0\x9f\xd1\x83\xd1\x81\xd1\x82\xd1\x8b\xd0\xbd\xd1\x8f.ufo", // Cyrillic
        "plain.hpi",
    };
    for (const std::string& name : names) {
        const fs::path file = maps / platform::path_from_utf8(name);
        std::error_code made;
        fs::create_directories(file.parent_path(), made);
        std::FILE* stream = platform::open_file(file, "wb");
        OA_CHECK(stream != nullptr);
        if (stream == nullptr)
            continue;
        std::fwrite(name.data(), 1, name.size(), stream);
        std::fclose(stream);
    }

#if defined(_WIN32)
    // Says whether this system's code page holds the names, which decides
    // whether a narrow spelling of them would have been refused.
    bool narrow_refused = false;
    try {
        static_cast<void>(maps.string());
    } catch (const std::exception&) {
        narrow_refused = true;
    }
    std::printf(
        "the code page %s these names; their UTF-8 spellings are read\n",
        narrow_refused ? "cannot write" : "writes"
    );
#endif

    std::vector<std::string> listed;
    std::error_code failure;
    for (fs::directory_iterator entry(maps, failure), end; !failure && entry != end;
         entry.increment(failure)) {
        const std::string name = platform::path_to_utf8(entry->path().filename());
        listed.push_back(name);
        OA_CHECK(entry->path() == maps / platform::path_from_utf8(name));
        OA_CHECK(read_text(maps / platform::path_from_utf8(name)) == name);
    }
    OA_CHECK(!failure);
    std::sort(listed.begin(), listed.end());
    std::vector<std::string> expected = names;
    std::sort(expected.begin(), expected.end());
    OA_CHECK(listed == expected);

    const fs::path replaced = maps / platform::path_from_utf8(names[1]);
    const std::string bytes = "replaced";
    std::string error;
    OA_CHECK(
        platform::replace_file(
            replaced,
            std::span(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()),
            &error
        )
    );
    OA_CHECK(error.empty());
    OA_CHECK(read_text(replaced) == bytes);

    // A folder in the file's place is not replaced, and the refusal names it
    // in UTF-8.
    const std::string folder_name = "\xce\xb1\xce\xb2 \xe7\x9b\xae\xe5\xbd\x95";
    const fs::path folder = maps / platform::path_from_utf8(folder_name);
    fs::create_directories(folder / "inside");
    OA_CHECK(!platform::replace_file(
        folder, std::span(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()), &error
    ));
    OA_CHECK(error.find(folder_name) != std::string::npos);
    OA_CHECK(fs::is_directory(folder / "inside"));
}

} // namespace

int main() {
    converts_whole_characters();
    replaces_lone_surrogates();
    reads_bytes_that_are_not_utf8();
    spells_paths_in_utf8();
    try {
        lists_a_folder_of_names_outside_any_code_page();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "check failed: %s\n", error.what());
        ++oa::test::failed_checks();
    }
    return oa::test::check_exit_status();
}
