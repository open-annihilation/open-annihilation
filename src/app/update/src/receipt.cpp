// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/app/update/receipt.hpp"

#include "json_fields.hpp"

#include <cstdint>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace oa::app::update {

using namespace fields;

namespace {

/// A detail cut to most_step_detail_bytes, on a whole UTF-8 character.
///
/// @param detail the detail
/// @return the detail, cut when it is too long
std::string cut_detail(std::string_view detail) {
    if (detail.size() <= most_step_detail_bytes)
        return std::string(detail);
    std::size_t end = most_step_detail_bytes;
    // A byte of the form 10xxxxxx continues a character; the cut moves back to its start.
    while (end > 0 && (static_cast<unsigned char>(detail[end]) & 0xC0U) == 0x80U)
        --end;
    return std::string(detail.substr(0, end));
}

/// An optional version: empty, or MAJOR.MINOR.PATCH.
///
/// @param object the receipt
/// @param name the member
/// @param[out] out the text
/// @param[out] reason why it was refused; may be null
/// @return false when the member is missing or not a version
bool require_optional_version(
    const json::Json& object, std::string_view name, std::string& out, std::string* reason
) {
    if (!require_string(object, name, "the receipt", out, reason))
        return false;
    if (!out.empty() && !oa::formats::oamod::parse_engine_version(out)) {
        set_error(reason, std::string(name) + " is not a version");
        return false;
    }
    return true;
}

/// Reads one step.
///
/// @param value the step's object
/// @param[out] reason why it was refused; may be null
/// @return the step, or nothing when it is not one
std::optional<ReceiptStep> read_step(const json::Json& value, std::string* reason) {
    if (value.type() != json::JsonType::object) {
        set_error(reason, "a step is not an object");
        return std::nullopt;
    }
    if (!names_unique(value, reason))
        return std::nullopt;
    ReceiptStep step;
    std::string name;
    if (!require_string(value, "step", "a step", name, reason))
        return std::nullopt;
    const std::optional<Step> parsed = parse_step(name);
    if (!parsed) {
        set_error(reason, "a step is not known");
        return std::nullopt;
    }
    step.step = *parsed;
    std::string result;
    if (!require_string(value, "result", "a step", result, reason))
        return std::nullopt;
    if (result == "done") {
        step.result = StepResult::done;
    } else if (result == "failed") {
        step.result = StepResult::failed;
    } else {
        set_error(reason, "a step's result is not known");
        return std::nullopt;
    }
    if (!require_string(value, "detail", "a step", step.detail, reason))
        return std::nullopt;
    if (step.detail.size() > most_step_detail_bytes) {
        set_error(reason, "a step's detail is too long");
        return std::nullopt;
    }
    return step;
}

} // namespace

const char* outcome_name(Outcome outcome) noexcept {
    switch (outcome) {
    case Outcome::running:
        return "running";
    case Outcome::installed:
        return "installed";
    case Outcome::installs_next_start:
        return "installs-next-start";
    case Outcome::opened_installer:
        return "opened-installer";
    case Outcome::restart_windows:
        return "restart-windows";
    case Outcome::not_installed:
        return "not-installed";
    case Outcome::cancelled:
        return "cancelled";
    case Outcome::failed_restored:
        return "failed-restored";
    case Outcome::recovery_incomplete:
        return "recovery-incomplete";
    case Outcome::rolled_back:
        return "rolled-back";
    case Outcome::roll_back_failed:
        return "roll-back-failed";
    case Outcome::roll_back_incomplete:
        return "roll-back-incomplete";
    }
    return "";
}

std::optional<Outcome> parse_outcome(std::string_view text) noexcept {
    constexpr Outcome every[] = {
        Outcome::running,
        Outcome::installed,
        Outcome::installs_next_start,
        Outcome::opened_installer,
        Outcome::restart_windows,
        Outcome::not_installed,
        Outcome::cancelled,
        Outcome::failed_restored,
        Outcome::recovery_incomplete,
        Outcome::rolled_back,
        Outcome::roll_back_failed,
        Outcome::roll_back_incomplete,
    };
    for (const Outcome outcome : every) {
        if (text == outcome_name(outcome))
            return outcome;
    }
    return std::nullopt;
}

const char* step_name(Step step) noexcept {
    switch (step) {
    case Step::wait:
        return "wait";
    case Step::verify:
        return "verify";
    case Step::unpack:
        return "unpack";
    case Step::swap:
        return "swap";
    case Step::installer:
        return "installer";
    case Step::start:
        return "start";
    case Step::watch:
        return "watch";
    case Step::undo:
        return "undo";
    case Step::restore:
        return "restore";
    }
    return "";
}

std::optional<Step> parse_step(std::string_view text) noexcept {
    constexpr Step every[] = {
        Step::wait,
        Step::verify,
        Step::unpack,
        Step::swap,
        Step::installer,
        Step::start,
        Step::watch,
        Step::undo,
        Step::restore,
    };
    for (const Step step : every) {
        if (text == step_name(step))
            return step;
    }
    return std::nullopt;
}

bool failed_outcome(Outcome outcome) noexcept {
    switch (outcome) {
    case Outcome::running:
    case Outcome::not_installed:
    case Outcome::cancelled:
    case Outcome::failed_restored:
    case Outcome::recovery_incomplete:
    case Outcome::roll_back_failed:
    case Outcome::roll_back_incomplete:
        return true;
    case Outcome::installed:
    case Outcome::installs_next_start:
    case Outcome::opened_installer:
    case Outcome::restart_windows:
    case Outcome::rolled_back:
        return false;
    }
    return false;
}

bool holds_release(Outcome outcome) noexcept {
    return outcome == Outcome::running || outcome == Outcome::not_installed ||
           outcome == Outcome::cancelled || outcome == Outcome::failed_restored ||
           outcome == Outcome::recovery_incomplete;
}

Outcome failure_outcome(Action action, bool files_changed, bool undo_finished) noexcept {
    const bool roll_back = action == Action::roll_back;
    if (!files_changed)
        return roll_back ? Outcome::roll_back_failed : Outcome::not_installed;
    if (!undo_finished)
        return roll_back ? Outcome::roll_back_incomplete : Outcome::recovery_incomplete;
    return roll_back ? Outcome::roll_back_failed : Outcome::failed_restored;
}

std::optional<Receipt> read_receipt(std::span<const uint8_t> bytes, std::string* error) {
    const std::optional<json::Json> root = parse_object(bytes, error);
    if (!root)
        return std::nullopt;
    Receipt receipt;
    int64_t format = 0;
    if (!require_integer(*root, "receipt", "the receipt", format, error))
        return std::nullopt;
    if (format != receipt_format) {
        set_error(error, "the receipt is not receipt 1");
        return std::nullopt;
    }
    receipt.receipt = format;

    std::string action;
    if (!require_string(*root, "action", "the receipt", action, error))
        return std::nullopt;
    const std::optional<Action> parsed_action = parse_action(action);
    if (!parsed_action) {
        set_error(error, "the action is not known");
        return std::nullopt;
    }
    receipt.action = *parsed_action;
    if (const json::Json* reason = member_of(*root, "reason")) {
        const std::string* text = reason->string();
        if (text == nullptr) {
            set_error(error, "reason is not text");
            return std::nullopt;
        }
        const std::optional<RollBackReason> parsed_reason = parse_roll_back_reason(*text);
        if (!parsed_reason) {
            set_error(error, "the roll-back reason is not known");
            return std::nullopt;
        }
        receipt.reason = *parsed_reason;
    }

    std::string outcome;
    if (!require_string(*root, "outcome", "the receipt", outcome, error))
        return std::nullopt;
    const std::optional<Outcome> parsed_outcome = parse_outcome(outcome);
    if (!parsed_outcome) {
        set_error(error, "the outcome is not known");
        return std::nullopt;
    }
    receipt.outcome = *parsed_outcome;

    if (!require_optional_version(*root, "from_version", receipt.from_version, error) ||
        !require_optional_version(*root, "to_version", receipt.to_version, error) ||
        !require_integer(*root, "release", "the receipt", receipt.release, error) ||
        !require_integer(*root, "started_at", "the receipt", receipt.started_at, error) ||
        !require_integer(*root, "finished_at", "the receipt", receipt.finished_at, error))
        return std::nullopt;
    if (receipt.release < 0) {
        set_error(error, "release is not a release number");
        return std::nullopt;
    }

    const json::Json* steps = require_member(*root, "steps", "the receipt", error);
    if (steps == nullptr)
        return std::nullopt;
    if (steps->type() != json::JsonType::array) {
        set_error(error, "steps is not a list");
        return std::nullopt;
    }
    if (steps->elements().size() > most_receipt_steps) {
        set_error(error, "steps has too many entries");
        return std::nullopt;
    }
    for (const json::Json& element : steps->elements()) {
        std::optional<ReceiptStep> step = read_step(element, error);
        if (!step)
            return std::nullopt;
        receipt.steps.push_back(std::move(*step));
    }
    if (!require_bool(*root, "shown", "the receipt", receipt.shown, error))
        return std::nullopt;
    return receipt;
}

std::string receipt_text(const Receipt& receipt) {
    json::JsonWriter writer;
    writer.begin_object();
    writer.key("receipt");
    writer.integer(receipt.receipt);
    writer.key("action");
    writer.string(action_name(receipt.action));
    if (receipt.reason != RollBackReason::none) {
        writer.key("reason");
        writer.string(roll_back_reason_name(receipt.reason));
    }
    writer.key("outcome");
    writer.string(outcome_name(receipt.outcome));
    writer.key("from_version");
    writer.string(receipt.from_version);
    writer.key("to_version");
    writer.string(receipt.to_version);
    writer.key("release");
    writer.integer(receipt.release);
    writer.key("started_at");
    writer.integer(receipt.started_at);
    writer.key("finished_at");
    writer.integer(receipt.finished_at);
    writer.key("steps");
    writer.begin_array();
    std::size_t written = 0;
    for (const ReceiptStep& step : receipt.steps) {
        if (written == most_receipt_steps)
            break;
        writer.begin_object();
        writer.key("step");
        writer.string(step_name(step.step));
        writer.key("result");
        writer.string(step.result == StepResult::done ? "done" : "failed");
        writer.key("detail");
        writer.string(cut_detail(step.detail));
        writer.end_object();
        ++written;
    }
    writer.end_array();
    writer.key("shown");
    writer.boolean(receipt.shown);
    writer.end_object();
    return writer.text() + "\n";
}

bool write_receipt(const std::filesystem::path& path, const Receipt& receipt, std::string* error) {
    return replace_file(path, receipt_text(receipt), "the receipt", error);
}

std::optional<Receipt> load_receipt(const std::filesystem::path& path) {
    std::error_code io;
    const uintmax_t size = std::filesystem::file_size(path, io);
    if (io || size == 0 || size > max_plan_bytes)
        return std::nullopt;
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return std::nullopt;
    const std::vector<uint8_t> bytes(
        (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>()
    );
    std::string error;
    return read_receipt(bytes, &error);
}

} // namespace oa::app::update
