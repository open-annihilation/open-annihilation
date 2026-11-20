// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The replays the player keeps: a small file of their own under `.replays`,
// by each recording's identity, written whole by a rename. Nothing the
// index's cache holds is needed to read it, so the index can be deleted or
// damaged without a mark being lost.

#include "oa/data/replays/replays.hpp"

#include "oa/formats/json.hpp"
#include "oa/platform/files.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace oa::data::replays {
namespace {

namespace fs = std::filesystem;
namespace json = oa::formats::json;

/// The member that names the kept marks' version.
constexpr std::string_view version_member = "replays_kept";

/// The member that holds the kept marks.
constexpr std::string_view marks_member = "kept";

/// The member that names the exported marks' version.
constexpr std::string_view exported_version_member = "replays_exported";

/// The member that holds the exported marks.
constexpr std::string_view exported_marks_member = "exported";

/// Which JSON object a mark file is.
struct MarkKeys {
    std::string_view version_key{}; ///< the version member
    std::string_view list_key{};    ///< the array member
    int version{};                  ///< the version this module writes
};

/// The largest kept file read, in bytes. A larger one is damaged.
constexpr uint64_t most_kept_bytes = 64ULL * 1024ULL * 1024ULL;

/// Returns a text with its ASCII capitals lower-cased.
///
/// @param text the text
/// @return the lower-case text
std::string lowered(std::string_view text) {
    std::string out(text);
    for (char& letter : out)
        if (letter >= 'A' && letter <= 'Z')
            letter = static_cast<char>(letter - 'A' + 'a');
    return out;
}

/// Tells whether a mark comes before another: by identity, then path.
///
/// @param left a mark
/// @param right another
/// @return true when `left` sorts first
bool mark_before(const KeptMark& left, const KeptMark& right) {
    if (left.sha256 != right.sha256)
        return left.sha256 < right.sha256;
    return left.path < right.path;
}

/// Finds the mark that holds a replay, by its place in the list.
///
/// @param marks the marks
/// @param replay the replay
/// @return the place of its mark; nothing when it has none
std::optional<std::size_t> mark_slot(const std::vector<KeptMark>& marks, const Replay& replay) {
    const auto first = std::lower_bound(
        marks.begin(),
        marks.end(),
        replay.sha256,
        [](const KeptMark& mark, const std::string& sha) { return mark.sha256 < sha; }
    );
    if (!replay.sha256.empty()) {
        if (first != marks.end() && first->sha256 == replay.sha256)
            return static_cast<std::size_t>(first - marks.begin());
        return std::nullopt;
    }
    // No identity: among the marks that have none, the one of the replay's path.
    for (auto at = first; at != marks.end() && at->sha256.empty(); ++at)
        if (at->path == replay.path)
            return static_cast<std::size_t>(at - marks.begin());
    return std::nullopt;
}

/// Finds the mark that holds a replay.
///
/// @param kept the marks
/// @param replay the replay
/// @return the place of its mark, or the end
std::vector<KeptMark>::const_iterator find_mark(const KeptMarks& kept, const Replay& replay) {
    const std::optional<std::size_t> slot = mark_slot(kept.marks, replay);
    if (!slot)
        return kept.marks.end();
    return kept.marks.begin() + static_cast<std::ptrdiff_t>(*slot);
}

/// Tells whether the marks already hold a mark.
///
/// An identity is held once. A mark with none is held once per path.
///
/// @param marks the marks
/// @param mark the mark
/// @return true when `marks` already holds it
bool holds_mark(const std::vector<KeptMark>& marks, const KeptMark& mark) {
    if (!mark.sha256.empty()) {
        for (const KeptMark& held : marks)
            if (held.sha256 == mark.sha256)
                return true;
        return false;
    }
    for (const KeptMark& held : marks)
        if (held.sha256.empty() && held.path == mark.path)
            return true;
    return false;
}

/// Inserts a mark in order, when the marks do not already hold it.
///
/// @param[in,out] marks the marks
/// @param mark the mark
/// @return true when it was inserted
bool insert_mark(std::vector<KeptMark>& marks, KeptMark mark) {
    if (mark.sha256.empty() && mark.path.empty())
        return false;
    if (holds_mark(marks, mark))
        return false;
    marks.insert(std::upper_bound(marks.begin(), marks.end(), mark, mark_before), std::move(mark));
    return true;
}

/// Sets one flag of each replay from the marks.
///
/// A file a mark names by path stays marked even when another file still has
/// the mark's identity. That file gains a mark of its new identity, or of its
/// path alone when it no longer reads. A path whose file is gone moves to a
/// file that still has the identity.
///
/// @param[in,out] marks the marks; a path may change and a mark may be added
/// @param[in,out] replays the replays
/// @param flag which flag the marks set
/// @return true when the marks changed
bool apply_marks(std::vector<KeptMark>& marks, std::vector<Replay>& replays, bool Replay::* flag) {
    std::unordered_map<std::string, std::string> first_path;
    std::unordered_map<std::string, std::size_t> by_path;
    for (std::size_t place = 0; place < replays.size(); ++place) {
        const Replay& replay = replays[place];
        by_path.emplace(replay.path, place);
        if (!replay.sha256.empty())
            first_path.emplace(replay.sha256, replay.path);
    }
    std::unordered_set<std::string> marked_sha;
    std::unordered_set<std::string> path_only;
    for (const KeptMark& mark : marks) {
        if (!mark.sha256.empty())
            marked_sha.insert(mark.sha256);
        else if (!mark.path.empty())
            path_only.insert(mark.path);
    }
    std::vector<char> on(replays.size(), 0);
    for (std::size_t place = 0; place < replays.size(); ++place) {
        const Replay& replay = replays[place];
        if (!replay.sha256.empty() && marked_sha.count(replay.sha256) != 0)
            on[place] = 1;
        else if (replay.sha256.empty() && path_only.count(replay.path) != 0)
            on[place] = 1;
    }
    bool changed = false;
    std::vector<KeptMark> extra;
    for (KeptMark& mark : marks) {
        if (mark.path.empty()) {
            const auto now = first_path.find(mark.sha256);
            if (!mark.sha256.empty() && now != first_path.end()) {
                mark.path = now->second;
                changed = true;
            }
            continue;
        }
        const auto at = by_path.find(mark.path);
        if (at == by_path.end()) {
            const auto now = first_path.find(mark.sha256);
            if (mark.sha256.empty() || now == first_path.end() || mark.path == now->second)
                continue;
            mark.path = now->second;
            changed = true;
            continue;
        }
        // The named file stays marked, whoever else still has the identity.
        on[at->second] = 1;
        const Replay& named = replays[at->second];
        if (!mark.sha256.empty() && named.sha256 == mark.sha256)
            continue;
        KeptMark added;
        added.sha256 = named.sha256;
        added.path = named.path;
        if (!holds_mark(marks, added) && !holds_mark(extra, added))
            extra.push_back(std::move(added));
    }
    for (KeptMark& added : extra)
        changed = insert_mark(marks, std::move(added)) || changed;
    for (std::size_t place = 0; place < replays.size(); ++place)
        replays[place].*flag = on[place] != 0;
    return changed;
}

/// Reads a string member, when the object has it.
///
/// @param object the object
/// @param name the member's name
/// @param[out] out the text; unchanged when the member is missing
/// @return false when the member is there and is not a string
bool take_text(const json::Json& object, std::string_view name, std::string& out) {
    const json::Json* value = object.find(name);
    if (value == nullptr)
        return true;
    const std::string* text = value->string();
    if (text == nullptr)
        return false;
    out = *text;
    return true;
}

} // namespace

bool is_kept(const KeptMarks& kept, const Replay& replay) {
    if (find_mark(kept, replay) != kept.marks.end())
        return true;
    if (replay.path.empty())
        return false;
    for (const KeptMark& mark : kept.marks)
        if (mark.path == replay.path)
            return true;
    return false;
}

bool set_kept(KeptMarks& kept, const Replay& replay, bool keep) {
    if (!keep) {
        bool changed = false;
        for (std::size_t place = 0; place < kept.marks.size();) {
            KeptMark& mark = kept.marks[place];
            const bool identity = !replay.sha256.empty() && mark.sha256 == replay.sha256;
            const bool path_only =
                mark.sha256.empty() && !replay.path.empty() && mark.path == replay.path;
            if (identity || path_only) {
                kept.marks.erase(kept.marks.begin() + static_cast<std::ptrdiff_t>(place));
                changed = true;
                continue;
            }
            // A mark of another identity that names this file keeps that
            // identity, and stops naming this file.
            if (!mark.sha256.empty() && !replay.path.empty() && mark.path == replay.path) {
                mark.path.clear();
                changed = true;
            }
            ++place;
        }
        return changed;
    }
    const auto found = find_mark(kept, replay);
    if (found != kept.marks.end())
        return false;
    KeptMark mark;
    mark.sha256 = replay.sha256;
    mark.path = replay.path;
    kept.marks.insert(
        std::upper_bound(kept.marks.begin(), kept.marks.end(), mark, mark_before), std::move(mark)
    );
    return true;
}

bool apply_kept(KeptMarks& kept, std::vector<Replay>& replays) {
    return apply_marks(kept.marks, replays, &Replay::kept);
}

bool release_kept(KeptMarks& kept, const std::vector<Replay>& listed, const Replay& deleted) {
    const auto holder_of = [&](const std::string& sha) -> const Replay* {
        if (sha.empty())
            return nullptr;
        for (const Replay& other : listed) {
            if (other.path == deleted.path)
                continue;
            if (other.sha256 == sha)
                return &other;
        }
        return nullptr;
    };
    const Replay* stay = holder_of(deleted.sha256);
    bool changed = false;
    for (std::size_t place = 0; place < kept.marks.size();) {
        KeptMark& mark = kept.marks[place];
        const bool same_sha = !deleted.sha256.empty() && mark.sha256 == deleted.sha256;
        const bool names_file = !deleted.path.empty() && mark.path == deleted.path;
        if (!same_sha && !names_file) {
            ++place;
            continue;
        }
        if (same_sha && stay != nullptr) {
            if (mark.path != stay->path) {
                mark.path = stay->path;
                changed = true;
            }
            ++place;
            continue;
        }
        if (names_file && !mark.sha256.empty() && mark.sha256 != deleted.sha256) {
            if (const Replay* holder = holder_of(mark.sha256)) {
                if (mark.path != holder->path) {
                    mark.path = holder->path;
                    changed = true;
                }
                ++place;
                continue;
            }
        }
        kept.marks.erase(kept.marks.begin() + static_cast<std::ptrdiff_t>(place));
        changed = true;
    }
    return changed;
}

/// Writes one mark file's JSON.
///
/// @param marks the marks
/// @param keys which object it is
/// @return the JSON text
std::string marks_text(const KeptMarks& marks, MarkKeys keys) {
    json::JsonWriter writer;
    writer.begin_object();
    writer.key(keys.version_key);
    writer.integer(keys.version);
    writer.key(keys.list_key);
    writer.begin_array();
    for (const KeptMark& mark : marks.marks) {
        writer.begin_object();
        writer.key("sha256");
        writer.string(mark.sha256);
        writer.key("path");
        writer.string(mark.path);
        writer.end_object();
    }
    writer.end_array();
    writer.end_object();
    return writer.text();
}

/// Reads one mark file's JSON.
///
/// @param text the JSON text
/// @param keys which object it is
/// @return the marks, or nothing when the text is not that object
std::optional<KeptMarks> parse_marks(std::string_view text, MarkKeys keys) {
    json::JsonError problem;
    const std::optional<json::Json> root = json::parse_json(text, problem);
    if (!root.has_value() || root->type() != json::JsonType::object)
        return std::nullopt;
    const json::Json* version = root->find(keys.version_key);
    if (version == nullptr || version->integer() != keys.version)
        return std::nullopt;
    const json::Json* entries = root->find(keys.list_key);
    if (entries == nullptr || entries->type() != json::JsonType::array)
        return std::nullopt;
    KeptMarks parsed;
    for (const json::Json& element : entries->elements()) {
        if (element.type() != json::JsonType::object)
            continue;
        KeptMark mark;
        if (!take_text(element, "sha256", mark.sha256) || !take_text(element, "path", mark.path))
            continue;
        mark.sha256 = lowered(mark.sha256);
        if (mark.sha256.empty() && mark.path.empty())
            continue;
        parsed.marks.push_back(std::move(mark));
    }
    std::stable_sort(parsed.marks.begin(), parsed.marks.end(), mark_before);
    // A later mark of an identity, or of a path with no identity, is dropped.
    const auto distinct_end = std::unique(
        parsed.marks.begin(), parsed.marks.end(), [](const KeptMark& left, const KeptMark& right) {
            return left.sha256 == right.sha256 && (!left.sha256.empty() || left.path == right.path);
        }
    );
    parsed.marks.erase(distinct_end, parsed.marks.end());
    return parsed;
}

std::string kept_text(const KeptMarks& kept) {
    return marks_text(kept, {version_member, marks_member, kept_version});
}

std::optional<KeptMarks> parse_kept(std::string_view text) {
    return parse_marks(text, {version_member, marks_member, kept_version});
}

/// Takes a mark from one JSON object, when it is one.
///
/// @param text one object, braces included
/// @param[in,out] marks the marks
void take_salvaged(std::string_view text, KeptMarks& marks) {
    json::JsonError problem;
    const std::optional<json::Json> object = json::parse_json(text, problem);
    if (!object.has_value() || object->type() != json::JsonType::object)
        return;
    KeptMark mark;
    if (!take_text(*object, "sha256", mark.sha256) || !take_text(*object, "path", mark.path))
        return;
    mark.sha256 = lowered(mark.sha256);
    if (mark.sha256.empty() && mark.path.empty())
        return;
    static_cast<void>(insert_mark(marks.marks, std::move(mark)));
}

KeptMarks salvage_kept(std::string_view text) {
    KeptMarks salvaged;
    bool in_string = false;
    bool escape = false;
    std::vector<std::size_t> starts;
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char letter = text[index];
        if (in_string) {
            if (escape)
                escape = false;
            else if (letter == '\\')
                escape = true;
            else if (letter == '"')
                in_string = false;
            continue;
        }
        if (letter == '"') {
            in_string = true;
            continue;
        }
        if (letter == '{') {
            starts.push_back(index);
            continue;
        }
        if (letter != '}' || starts.empty())
            continue;
        const std::size_t start = starts.back();
        starts.pop_back();
        take_salvaged(text.substr(start, index - start + 1), salvaged);
    }
    std::stable_sort(salvaged.marks.begin(), salvaged.marks.end(), mark_before);
    return salvaged;
}

bool spared_by_kept_bytes(
    std::string_view bytes, std::string_view sha256, std::string_view path
) noexcept {
    if (!sha256.empty() && bytes.find(sha256) != std::string_view::npos)
        return true;
    return !path.empty() && bytes.find(path) != std::string_view::npos;
}

fs::path kept_path(const fs::path& recordings) {
    return recordings / oa::platform::path_from_utf8(replays_folder_name) /
           oa::platform::path_from_utf8(kept_name);
}

KeptRead read_kept(const fs::path& recordings) {
    KeptRead read;
    try {
        const fs::path file = kept_path(recordings);
        std::error_code error;
        const fs::file_status status = fs::status(file, error);
        if (fs::status_known(status) && !fs::exists(status))
            return read;
        // From here on the file is there: whatever stops it being read is damage.
        read.damaged = true;
        const uint64_t size = fs::file_size(file, error);
        if (error || size > most_kept_bytes)
            return read;
        std::ifstream in(file, std::ios::binary);
        if (!in)
            return read;
        std::string text(static_cast<std::size_t>(size), '\0');
        in.read(text.data(), static_cast<std::streamsize>(text.size()));
        if (in.gcount() != static_cast<std::streamsize>(text.size()))
            return read;
        std::optional<KeptMarks> parsed = parse_kept(text);
        if (!parsed.has_value())
            return read;
        read.kept = std::move(*parsed);
        read.damaged = false;
        return read;
    } catch (const std::exception&) {
        read.kept = {};
        read.damaged = true;
        return read;
    }
}

bool save_kept(const fs::path& recordings, const KeptMarks& kept) {
    try {
        std::error_code error;
        fs::create_directories(
            recordings / oa::platform::path_from_utf8(replays_folder_name), error
        );
        if (error)
            return false;
        const std::string text = kept_text(kept);
        const std::span<const uint8_t> bytes{
            reinterpret_cast<const uint8_t*>(text.data()), text.size()
        };
        return oa::platform::replace_file(kept_path(recordings), bytes, nullptr);
    } catch (const std::exception&) {
        return false;
    }
}

bool set_exported(KeptMarks& exported, const Replay& replay, bool mark) {
    return set_kept(exported, replay, mark);
}

bool apply_exported(KeptMarks& exported, std::vector<Replay>& replays) {
    return apply_marks(exported.marks, replays, &Replay::exported);
}

std::string exported_text(const KeptMarks& exported) {
    return marks_text(exported, {exported_version_member, exported_marks_member, exported_version});
}

std::optional<KeptMarks> parse_exported(std::string_view text) {
    return parse_marks(text, {exported_version_member, exported_marks_member, exported_version});
}

fs::path exported_path(const fs::path& recordings) {
    return recordings / oa::platform::path_from_utf8(replays_folder_name) /
           oa::platform::path_from_utf8(exported_name);
}

ExportedRead read_exported(const fs::path& recordings) {
    ExportedRead read;
    const KeptRead loaded = [&] {
        KeptRead out;
        try {
            const fs::path file = exported_path(recordings);
            std::error_code error;
            const fs::file_status status = fs::status(file, error);
            if (fs::status_known(status) && !fs::exists(status))
                return out;
            out.damaged = true;
            const uint64_t size = fs::file_size(file, error);
            if (error || size > most_kept_bytes)
                return out;
            std::ifstream in(file, std::ios::binary);
            if (!in)
                return out;
            std::string text(static_cast<std::size_t>(size), '\0');
            in.read(text.data(), static_cast<std::streamsize>(text.size()));
            if (in.gcount() != static_cast<std::streamsize>(text.size()))
                return out;
            std::optional<KeptMarks> parsed = parse_exported(text);
            if (!parsed.has_value())
                return out;
            out.kept = std::move(*parsed);
            out.damaged = false;
            return out;
        } catch (const std::exception&) {
            out.kept = {};
            out.damaged = true;
            return out;
        }
    }();
    read.exported = loaded.kept;
    read.damaged = loaded.damaged;
    return read;
}

bool save_exported(const fs::path& recordings, const KeptMarks& exported) {
    try {
        std::error_code error;
        fs::create_directories(
            recordings / oa::platform::path_from_utf8(replays_folder_name), error
        );
        if (error)
            return false;
        const std::string text = exported_text(exported);
        const std::span<const uint8_t> bytes{
            reinterpret_cast<const uint8_t*>(text.data()), text.size()
        };
        return oa::platform::replace_file(exported_path(recordings), bytes, nullptr);
    } catch (const std::exception&) {
        return false;
    }
}

} // namespace oa::data::replays
