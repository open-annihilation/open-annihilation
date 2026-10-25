// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/data/engine_feed/feed.hpp"

#include "oa/base/sha256.hpp"
#include "oa/formats/json.hpp"
#include "oa/formats/oamod/package_keys.hpp"
#include "oa/formats/url.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::data::engine_feed {
namespace {

namespace json = oa::formats::json;
namespace oamod = oa::formats::oamod;
namespace url = oa::formats::url;

/// Writes `level` pairs of spaces.
void indent(std::string& out, int level) {
    out.append(static_cast<std::size_t>(level) * 2, ' ');
}

/// A JSON string, quoted and escaped the way JsonWriter writes one.
std::string json_string(std::string_view text) {
    json::JsonWriter writer;
    writer.string(text);
    return writer.text();
}

/// A JSON whole number, as JsonWriter writes one.
std::string number(int64_t value) {
    json::JsonWriter writer;
    writer.integer(value);
    return writer.text();
}

/// A digest as 64 lower-case hex digits.
std::string hex_text(const base::sha256::Digest& digest) {
    const auto digits = base::sha256::to_hex(digest);
    return {digits.data(), digits.size()};
}

/// Writes file, size and sha256. The last line carries a comma when asked.
void write_file_members(std::string& out, int level, const PackageFile& file, bool last_comma) {
    indent(out, level);
    out += json_string("file");
    out += ": ";
    out += json_string(file.file);
    out += ",\n";
    indent(out, level);
    out += json_string("size");
    out += ": ";
    out += number(static_cast<int64_t>(file.size));
    out += ",\n";
    indent(out, level);
    out += json_string("sha256");
    out += ": ";
    out += json_string(hex_text(file.sha256));
    if (last_comma)
        out += ',';
    out += '\n';
}

/// Writes one package object. A comma follows when another package does.
void write_package(std::string& out, int level, const Package& package, bool trailing_comma) {
    indent(out, level);
    out += json_string(package.id);
    out += ": {\n";
    const int inner = level + 1;
    write_file_members(out, inner, package.zip, true);
    if (package.installer) {
        indent(out, inner);
        out += json_string("installer");
        out += ": {\n";
        write_file_members(out, inner + 1, *package.installer, false);
        indent(out, inner);
        out += "},\n";
    }
    indent(out, inner);
    out += json_string("mirrors");
    out += ": [\n";
    for (std::size_t index = 0; index < package.mirrors.size(); ++index) {
        indent(out, inner + 1);
        out += json_string(url::url_text(package.mirrors[index]));
        if (index + 1 < package.mirrors.size())
            out += ',';
        out += '\n';
    }
    indent(out, inner);
    out += "]\n";
    indent(out, level);
    out += '}';
    if (trailing_comma)
        out += ',';
    out += '\n';
}

/// Writes the notes object. A comma follows when asked.
void write_notes(
    std::string& out, int level, const std::vector<Notes>& notes, bool trailing_comma
) {
    indent(out, level);
    out += json_string("notes");
    out += ": {\n";
    for (std::size_t index = 0; index < notes.size(); ++index) {
        const Notes& notes_one = notes[index];
        indent(out, level + 1);
        out += json_string(notes_one.language);
        out += ": [\n";
        for (std::size_t line = 0; line < notes_one.lines.size(); ++line) {
            indent(out, level + 2);
            out += json_string(notes_one.lines[line]);
            if (line + 1 < notes_one.lines.size())
                out += ',';
            out += '\n';
        }
        indent(out, level + 1);
        out += ']';
        if (index + 1 < notes.size())
            out += ',';
        out += '\n';
    }
    indent(out, level);
    out += '}';
    if (trailing_comma)
        out += ',';
    out += '\n';
}

/// Writes one release object. A comma follows when another release does.
void write_release(std::string& out, const Release& release, bool trailing_comma) {
    indent(out, 2);
    out += "{\n";
    const std::string version = release.version_text.empty()
                                    ? oamod::engine_version_text(release.version)
                                    : release.version_text;
    auto key_line = [&](std::string_view key, std::string_view value, bool comma) {
        indent(out, 3);
        out += json_string(key);
        out += ": ";
        out += value;
        if (comma)
            out += ',';
        out += '\n';
    };
    key_line("release", number(release.release), true);
    key_line("version", json_string(version), true);
    key_line("date", json_string(release.date), true);
    key_line("from", json_string(oamod::engine_version_text(release.from)), true);
    key_line("rollout", number(release.rollout), true);
    if (!release.notes.empty())
        write_notes(out, 3, release.notes, true);
    if (!release.page.empty())
        key_line("page", json_string(release.page), true);
    indent(out, 3);
    out += json_string("packages");
    out += ": {\n";
    for (std::size_t index = 0; index < release.packages.size(); ++index)
        write_package(out, 4, release.packages[index], index + 1 < release.packages.size());
    indent(out, 3);
    out += "}\n";
    indent(out, 2);
    out += '}';
    if (trailing_comma)
        out += ',';
    out += '\n';
}

} // namespace

std::string feed_text(const Feed& feed) {
    std::string out = "{\n";
    indent(out, 1);
    out += json_string("feed");
    out += ": ";
    out += number(feed_format);
    out += ",\n";
    indent(out, 1);
    out += json_string("channel");
    out += ": ";
    out += json_string(feed.channel);
    out += ",\n";
    if (feed.online_min) {
        indent(out, 1);
        out += json_string("online_min");
        out += ": ";
        out += json_string(oamod::engine_version_text(*feed.online_min));
        out += ",\n";
    }
    indent(out, 1);
    out += json_string("releases");
    out += ": [\n";
    for (std::size_t index = 0; index < feed.releases.size(); ++index)
        write_release(out, feed.releases[index], index + 1 < feed.releases.size());
    indent(out, 1);
    out += "]\n}\n";
    return out;
}

} // namespace oa::data::engine_feed
