// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Reading and writing the module's small JSON files: one object whose
// members are each named once, paths that must be absolute, versions, and a
// whole-file replace. The plan, the state file and the receipt share these.
#pragma once

#include "oa/app/update/plan.hpp"
#include "oa/formats/json.hpp"
#include "oa/formats/oamod/package_keys.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace oa::app::update::fields {

namespace json = oa::formats::json;
namespace fs = std::filesystem;

/// Records `message` when the caller asked for a reason.
inline void set_error(std::string* reason, std::string message) {
    if (reason != nullptr)
        *reason = std::move(message);
}

/// The bytes as text. An empty span is an empty view.
inline std::string_view bytes_as_text(std::span<const uint8_t> bytes) {
    if (bytes.empty())
        return {};
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

/// A path from UTF-8.
inline fs::path path_from(std::string_view text) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

/// True when `text` is an absolute path.
inline bool absolute_path(std::string_view text) {
    if (text.empty())
        return false;
    return path_from(text).is_absolute();
}

/// True when every member of the object is named once.
inline bool names_unique(const json::Json& value, std::string* reason) {
    if (value.type() != json::JsonType::object)
        return true;
    const auto member_names = value.names();
    for (std::size_t index = 0; index < member_names.size(); ++index) {
        for (std::size_t earlier = 0; earlier < index; ++earlier) {
            if (member_names[earlier] == member_names[index]) {
                set_error(reason, "a member is named twice");
                return false;
            }
        }
    }
    return true;
}

/// The object's member, or null when it is missing.
inline const json::Json* member_of(const json::Json& object, std::string_view name) {
    return object.find(name);
}

/// A required member.
inline const json::Json* require_member(
    const json::Json& object, std::string_view name, std::string_view what, std::string* reason
) {
    const json::Json* value = member_of(object, name);
    if (value == nullptr)
        set_error(reason, std::string(what) + " has no " + std::string(name));
    return value;
}

/// A required string.
inline bool require_string(
    const json::Json& object,
    std::string_view name,
    std::string_view what,
    std::string& out,
    std::string* reason
) {
    const json::Json* value = require_member(object, name, what, reason);
    if (value == nullptr)
        return false;
    const std::string* text = value->string();
    if (text == nullptr) {
        set_error(reason, std::string(name) + " is not text");
        return false;
    }
    out = *text;
    return true;
}

/// A required absolute path. An empty string is reported on its own.
inline bool require_absolute(
    const json::Json& object,
    std::string_view name,
    std::string_view what,
    std::string& out,
    std::string* reason
) {
    if (!require_string(object, name, what, out, reason))
        return false;
    if (out.empty()) {
        set_error(reason, std::string(name) + " is empty");
        return false;
    }
    if (!absolute_path(out)) {
        set_error(reason, std::string(name) + " is not an absolute path");
        return false;
    }
    return true;
}

/// A required whole number.
inline bool require_integer(
    const json::Json& object,
    std::string_view name,
    std::string_view what,
    int64_t& out,
    std::string* reason
) {
    const json::Json* value = require_member(object, name, what, reason);
    if (value == nullptr)
        return false;
    const std::optional<int64_t> number = value->integer();
    if (!number) {
        set_error(reason, std::string(name) + " is not a whole number");
        return false;
    }
    out = *number;
    return true;
}

/// A required boolean.
inline bool require_bool(
    const json::Json& object,
    std::string_view name,
    std::string_view what,
    bool& out,
    std::string* reason
) {
    const json::Json* value = require_member(object, name, what, reason);
    if (value == nullptr)
        return false;
    const std::optional<bool> flag = value->boolean();
    if (!flag) {
        set_error(reason, std::string(name) + " is not true or false");
        return false;
    }
    out = *flag;
    return true;
}

/// A version as MAJOR.MINOR.PATCH, kept as the text.
inline bool require_version(
    const json::Json& object,
    std::string_view name,
    std::string_view what,
    std::string& out,
    std::string* reason
) {
    if (!require_string(object, name, what, out, reason))
        return false;
    if (!oa::formats::oamod::parse_engine_version(out)) {
        set_error(reason, std::string(name) + " is not a version");
        return false;
    }
    return true;
}

/// The window line: text, at most max_window_line_bytes.
inline bool require_line(
    const json::Json& object,
    std::string_view name,
    std::string_view what,
    std::string& out,
    std::string* reason
) {
    if (!require_string(object, name, what, out, reason))
        return false;
    if (out.size() > max_window_line_bytes) {
        set_error(reason, std::string(name) + " is too long");
        return false;
    }
    return true;
}

/// One JSON value, with unique member names.
inline std::optional<json::Json> parse_object(std::span<const uint8_t> bytes, std::string* reason) {
    if (bytes.size() > max_plan_bytes) {
        set_error(reason, "the file is too large");
        return std::nullopt;
    }
    json::JsonError json_error;
    std::optional<json::Json> root = json::parse_json(bytes_as_text(bytes), json_error);
    if (!root) {
        set_error(reason, json_error.message.empty() ? "the file is not JSON" : json_error.message);
        return std::nullopt;
    }
    if (root->type() != json::JsonType::object) {
        set_error(reason, "the file is not an object");
        return std::nullopt;
    }
    if (!names_unique(*root, reason))
        return std::nullopt;
    return root;
}

/// Replaces `path` with `bytes`. The temporary file sits beside it.
///
/// @param path the file
/// @param bytes its new bytes
/// @param what the file as a failure names it, such as `the state file`
/// @param[out] reason why it was not replaced; may be null
/// @return true when the file was replaced
inline bool replace_file(
    const fs::path& path, std::string_view bytes, std::string_view what, std::string* reason
) {
    const std::u8string temporary_name = path.filename().u8string() + std::u8string(u8".tmp");
    const fs::path beside = path.parent_path() / temporary_name;
    {
        std::ofstream out(beside, std::ios::binary | std::ios::trunc);
        if (!out) {
            set_error(reason, std::string(what) + " could not be written");
            return false;
        }
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        out.close();
        if (!out) {
            set_error(reason, std::string(what) + " could not be written");
            std::error_code ignored;
            fs::remove(beside, ignored);
            return false;
        }
    }
#if defined(_WIN32)
    // Rename leaves an existing file where it is and fails. One replace keeps
    // the previous state when the new file cannot take its place.
    if (MoveFileExW(
            beside.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH
        ) == 0) {
        set_error(reason, std::string(what) + " could not be replaced");
        std::error_code ignored;
        fs::remove(beside, ignored);
        return false;
    }
#else
    std::error_code io;
    fs::rename(beside, path, io);
    if (io) {
        set_error(reason, std::string(what) + " could not be replaced");
        std::error_code ignored;
        fs::remove(beside, ignored);
        return false;
    }
#endif
    return true;
}

} // namespace oa::app::update::fields
