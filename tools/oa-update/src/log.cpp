// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/update_helper/log.hpp"

#include <cstdint>
#include <fstream>
#include <utility>

namespace oa::update_helper {
namespace {

constexpr uint64_t max_log_bytes = 1024 * 1024;

void set_error(std::string* reason, std::string message) {
    if (reason != nullptr)
        *reason = std::move(message);
}

} // namespace

bool Log::open(const std::filesystem::path& updates, std::string* error) {
    path_ = updates / "oa-update.log";
    std::ofstream out(path_, std::ios::binary | std::ios::app);
    if (!out) {
        set_error(error, "the log could not be opened");
        open_ = false;
        return false;
    }
    open_ = true;
    return true;
}

void Log::line(std::string_view text) {
    if (!open_)
        return;
    trim();
    std::string cleaned;
    cleaned.reserve(text.size());
    for (const char byte : text) {
        if (byte == '\n' || byte == '\r')
            cleaned.push_back(' ');
        else
            cleaned.push_back(byte);
    }
    std::ofstream out(path_, std::ios::binary | std::ios::app);
    if (!out)
        return;
    out.write(cleaned.data(), static_cast<std::streamsize>(cleaned.size()));
    out.put('\n');
    trim();
}

void Log::trim() {
    std::error_code io;
    const auto size = std::filesystem::file_size(path_, io);
    if (io || size <= max_log_bytes)
        return;
    std::ifstream in(path_, std::ios::binary);
    if (!in)
        return;
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (text.size() <= max_log_bytes)
        return;
    const std::size_t half = text.size() / 2;
    std::size_t cut = text.find('\n', half);
    if (cut == std::string::npos)
        cut = half;
    else
        ++cut;
    const std::string kept = text.substr(cut);
    std::ofstream out(path_, std::ios::binary | std::ios::trunc);
    if (!out)
        return;
    out.write(kept.data(), static_cast<std::streamsize>(kept.size()));
}

} // namespace oa::update_helper
