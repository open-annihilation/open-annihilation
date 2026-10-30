// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/update_helper/log.hpp"

#include <chrono>
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

/// Seconds since 1970.
int64_t wall_seconds() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::seconds>(now).count();
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
    last_line_ = cleaned;
    std::ofstream out(path_, std::ios::binary | std::ios::app);
    if (!out)
        return;
    out.write(cleaned.data(), static_cast<std::streamsize>(cleaned.size()));
    out.put('\n');
    trim();
}

void Log::begin_receipt(const std::filesystem::path& updates, oa::app::update::Receipt receipt) {
    receipt_path_ = updates / std::string(oa::app::update::receipt_file_name);
    receipt.outcome = oa::app::update::Outcome::running;
    receipt.started_at = wall_seconds();
    receipt.finished_at = 0;
    receipt.shown = false;
    receipt.steps.clear();
    receipt_ = std::move(receipt);
    write_receipt();
}

void Log::step(
    oa::app::update::Step step, oa::app::update::StepResult result, std::string_view detail
) {
    if (!receipt_)
        return;
    if (receipt_->steps.size() < oa::app::update::most_receipt_steps)
        receipt_->steps.push_back({step, result, std::string(detail)});
    write_receipt();
}

void Log::set_to_version(std::string_view version) {
    if (!receipt_ || receipt_->to_version == version)
        return;
    receipt_->to_version = std::string(version);
    write_receipt();
}

void Log::finish(oa::app::update::Outcome outcome) {
    if (!receipt_)
        return;
    receipt_->outcome = outcome;
    receipt_->finished_at = wall_seconds();
    write_receipt();
}

bool Log::receipt_running() const noexcept {
    return receipt_.has_value() && receipt_->outcome == oa::app::update::Outcome::running;
}

oa::app::update::Action Log::receipt_action() const noexcept {
    return receipt_ ? receipt_->action : oa::app::update::Action::install;
}

void Log::write_receipt() {
    if (!receipt_)
        return;
    std::string error;
    if (oa::app::update::write_receipt(receipt_path_, *receipt_, &error))
        return;
    if (!receipt_failed_) {
        receipt_failed_ = true;
        line(error.empty() ? "the receipt could not be written" : error);
    }
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
