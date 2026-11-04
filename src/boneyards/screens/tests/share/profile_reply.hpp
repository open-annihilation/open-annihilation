// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Synthetic profile replies for the screen tests: a reply's fields held as
// text (the integer readers parse them) and the fields written back kept.
#pragma once

#include "oa/boneyards/screens/profile.hpp"

#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <map>
#include <string>
#include <utility>

namespace by_test {

struct ProfileReply {
    std::map<std::string, std::string> values;

    ProfileReply() = default;

    ProfileReply(std::initializer_list<std::pair<const std::string, std::string>> fields)
        : values(fields) {}

    oa::boneyards::screens::ProfileFields fields() {
        return {this, get_string, get_int, get_uint, set_string};
    }

    static const std::string* find(void* user, const char* key) {
        const auto& values = static_cast<ProfileReply*>(user)->values;
        const auto it = values.find(key);
        return it == values.end() ? nullptr : &it->second;
    }

    static bool get_string(void* user, const char* key, char* out, std::size_t capacity) {
        const std::string* value = find(user, key);
        if (value == nullptr)
            return false;
        std::snprintf(out, capacity, "%s", value->c_str());
        return true;
    }

    static bool get_int(void* user, const char* key, int32_t* out) {
        const std::string* value = find(user, key);
        if (value == nullptr)
            return false;
        *out = static_cast<int32_t>(std::strtol(value->c_str(), nullptr, 10));
        return true;
    }

    static bool get_uint(void* user, const char* key, uint32_t* out) {
        const std::string* value = find(user, key);
        if (value == nullptr)
            return false;
        *out = static_cast<uint32_t>(std::strtoll(value->c_str(), nullptr, 10));
        return true;
    }

    static void set_string(void* user, const char* key, const char* value) {
        static_cast<ProfileReply*>(user)->values[key] = value;
    }
};

// One profile request a test service took.
struct ProfileRequestSeen {
    uint32_t user_id;
    oa::boneyards::ProfileCallback on_profile;
    oa::boneyards::LadderCallback on_ladder;
    void* context;
};

} // namespace by_test
