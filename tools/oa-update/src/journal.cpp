// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/update_helper/journal.hpp"

#include "oa/formats/json.hpp"
#include "oa/update_helper/start.hpp"

#include <fstream>
#include <iterator>
#include <optional>
#include <utility>

#if defined(__APPLE__)
#include <copyfile.h>
#endif

namespace oa::update_helper {
namespace {

namespace json = oa::formats::json;
namespace fs = std::filesystem;

constexpr std::string_view journal_name = "journal.json";

struct Recorded {
    Operation::Kind kind{Operation::Kind::rename};
    std::string from;
    std::string to;
    std::string path;
    std::string held;
    std::string previous;
    bool created{};
    std::string program;
    std::vector<std::string> arguments;
};

void set_error(std::string* reason, std::string message) {
    if (reason != nullptr)
        *reason = std::move(message);
}

void note_line(const JournalHooks& hooks, std::string_view line) {
    if (hooks.note != nullptr)
        hooks.note(hooks.context, line);
}

const char* kind_name(Operation::Kind kind) noexcept {
    switch (kind) {
    case Operation::Kind::rename:
        return "rename";
    case Operation::Kind::copy_tree:
        return "copy_tree";
    case Operation::Kind::remove_tree:
        return "remove_tree";
    case Operation::Kind::make_link:
        return "make_link";
    case Operation::Kind::replace_link:
        return "replace_link";
    case Operation::Kind::write_file:
        return "write_file";
    case Operation::Kind::run_and_wait:
        return "run_and_wait";
    }
    return "";
}

std::optional<Operation::Kind> parse_kind(std::string_view text) {
    if (text == "rename")
        return Operation::Kind::rename;
    if (text == "copy_tree")
        return Operation::Kind::copy_tree;
    if (text == "remove_tree")
        return Operation::Kind::remove_tree;
    if (text == "make_link")
        return Operation::Kind::make_link;
    if (text == "replace_link")
        return Operation::Kind::replace_link;
    if (text == "write_file")
        return Operation::Kind::write_file;
    if (text == "run_and_wait")
        return Operation::Kind::run_and_wait;
    return std::nullopt;
}

fs::path journal_path(const fs::path& updates) {
    return updates / std::string(journal_name);
}

fs::path held_root(const fs::path& updates) {
    return updates / "journal-held";
}

bool replace_file(const fs::path& path, std::string_view bytes, std::string* reason) {
    const std::u8string temporary_name = path.filename().u8string() + std::u8string(u8".tmp");
    const fs::path beside = path.parent_path() / temporary_name;
    {
        std::ofstream out(beside, std::ios::binary | std::ios::trunc);
        if (!out) {
            set_error(reason, "a file could not be written");
            return false;
        }
        if (!bytes.empty())
            out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!out) {
            set_error(reason, "a file could not be written");
            std::error_code ignored;
            fs::remove(beside, ignored);
            return false;
        }
    }
    std::error_code io;
    fs::rename(beside, path, io);
#if defined(_WIN32)
    if (io) {
        std::error_code removed;
        fs::remove(path, removed);
        io.clear();
        fs::rename(beside, path, io);
    }
#endif
    if (io) {
        set_error(reason, "a file could not be replaced");
        std::error_code ignored;
        fs::remove(beside, ignored);
        return false;
    }
    return true;
}

std::string journal_text(const std::vector<Recorded>& steps) {
    json::JsonWriter writer;
    writer.begin_object();
    writer.key("journal");
    writer.integer(1);
    writer.key("steps");
    writer.begin_array();
    for (const Recorded& step : steps) {
        writer.begin_object();
        writer.key("op");
        writer.string(kind_name(step.kind));
        writer.key("from");
        writer.string(step.from);
        writer.key("to");
        writer.string(step.to);
        writer.key("path");
        writer.string(step.path);
        writer.key("held");
        writer.string(step.held);
        writer.key("previous");
        writer.string(step.previous);
        writer.key("created");
        writer.boolean(step.created);
        writer.key("program");
        writer.string(step.program);
        writer.key("arguments");
        writer.begin_array();
        for (const std::string& argument : step.arguments)
            writer.string(argument);
        writer.end_array();
        writer.end_object();
    }
    writer.end_array();
    writer.end_object();
    return writer.text();
}

bool write_journal(
    const fs::path& updates, const std::vector<Recorded>& steps, std::string* reason
) {
    return replace_file(journal_path(updates), journal_text(steps), reason);
}

bool read_text(const fs::path& path, std::string& out, std::string* reason) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        set_error(reason, "the journal could not be read");
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

std::optional<std::vector<Recorded>> read_journal(const fs::path& updates, std::string* reason) {
    const fs::path path = journal_path(updates);
    std::error_code io;
    if (!fs::exists(path, io) || io)
        return std::vector<Recorded>{};
    std::string text;
    if (!read_text(path, text, reason))
        return std::nullopt;
    json::JsonError json_error;
    const std::optional<json::Json> root = json::parse_json(text, json_error);
    if (!root || root->type() != json::JsonType::object) {
        set_error(reason, "the journal is not a record");
        return std::nullopt;
    }
    const json::Json* format = root->find("journal");
    if (format == nullptr || format->integer() != 1) {
        set_error(reason, "the journal is not a record");
        return std::nullopt;
    }
    const json::Json* steps = root->find("steps");
    if (steps == nullptr || steps->type() != json::JsonType::array) {
        set_error(reason, "the journal is not a record");
        return std::nullopt;
    }
    std::vector<Recorded> recorded;
    for (const json::Json& element : steps->elements()) {
        const std::string* op =
            element.find("op") != nullptr ? element.find("op")->string() : nullptr;
        if (op == nullptr) {
            set_error(reason, "the journal has a step this build does not know");
            return std::nullopt;
        }
        const std::optional<Operation::Kind> kind = parse_kind(*op);
        if (!kind) {
            set_error(reason, "the journal has a step this build does not know");
            return std::nullopt;
        }
        Recorded step;
        step.kind = *kind;
        auto take = [&](std::string_view name, std::string& dest) {
            const json::Json* value = element.find(name);
            if (value != nullptr && value->string() != nullptr)
                dest = *value->string();
        };
        take("from", step.from);
        take("to", step.to);
        take("path", step.path);
        take("held", step.held);
        take("previous", step.previous);
        take("program", step.program);
        const json::Json* created = element.find("created");
        if (created != nullptr && created->boolean())
            step.created = *created->boolean();
        const json::Json* arguments = element.find("arguments");
        if (arguments != nullptr && arguments->type() == json::JsonType::array) {
            for (const json::Json& argument : arguments->elements()) {
                if (argument.string() != nullptr)
                    step.arguments.push_back(*argument.string());
            }
        }
        recorded.push_back(std::move(step));
    }
    return recorded;
}

void clear_journal(const fs::path& updates) {
    std::error_code io;
    fs::remove(journal_path(updates), io);
    fs::remove_all(held_root(updates), io);
}

bool copy_one(const fs::path& from, const fs::path& to, std::string* reason) {
    std::error_code io;
    const fs::file_status status = fs::symlink_status(from, io);
    if (io || !fs::exists(status)) {
        set_error(reason, "cannot read " + utf8_of(from));
        return false;
    }
    if (fs::is_symlink(status)) {
        set_error(reason, "refusing a link");
        return false;
    }
    if (fs::exists(to, io)) {
        set_error(reason, "the destination already exists");
        return false;
    }
    if (fs::is_regular_file(status)) {
        fs::copy_file(from, to, io);
        if (io) {
            set_error(reason, "cannot copy " + utf8_of(from));
            return false;
        }
        return true;
    }
    if (!fs::is_directory(status)) {
        set_error(reason, "refusing a special file");
        return false;
    }
#if defined(__APPLE__)
    // A bundle keeps the flags and resource forks a rename would have kept.
    // An attribute that is already there, including a quarantine attribute,
    // is left as it is.
    if (copyfile(from.c_str(), to.c_str(), nullptr, COPYFILE_ALL | COPYFILE_RECURSIVE) != 0) {
        set_error(reason, "cannot copy " + utf8_of(from));
        std::error_code removed;
        fs::remove_all(to, removed);
        return false;
    }
    return true;
#else
    fs::create_directory(to, io);
    if (io) {
        set_error(reason, "cannot copy " + utf8_of(from));
        return false;
    }
    fs::directory_iterator it(from, io);
    while (!io && it != fs::directory_iterator()) {
        if (!copy_one(it->path(), to / it->path().filename(), reason)) {
            std::error_code removed;
            fs::remove_all(to, removed);
            return false;
        }
        it.increment(io);
    }
    if (io) {
        set_error(reason, "cannot read " + utf8_of(from));
        std::error_code removed;
        fs::remove_all(to, removed);
        return false;
    }
    return true;
#endif
}

bool run_step(
    const std::filesystem::path& program,
    const std::vector<std::string>& arguments,
    const JournalHooks& hooks,
    std::string* reason
) {
    if (hooks.run != nullptr)
        return hooks.run(hooks.context, program, arguments, reason);
    return run_and_wait(program, arguments, reason);
}

bool perform(
    const Operation& operation,
    const fs::path& updates,
    int step,
    Recorded& recorded,
    const JournalHooks& hooks,
    std::string* reason
) {
    std::error_code io;
    recorded.kind = operation.kind;
    switch (operation.kind) {
    case Operation::Kind::rename: {
        if (fs::exists(operation.to, io)) {
            set_error(reason, "the destination already exists");
            return false;
        }
        fs::rename(operation.from, operation.to, io);
        if (io) {
            set_error(reason, "cannot rename " + utf8_of(operation.from));
            return false;
        }
        recorded.from = utf8_of(operation.from);
        recorded.to = utf8_of(operation.to);
        note_line(hooks, "renamed " + recorded.from);
        return true;
    }
    case Operation::Kind::copy_tree: {
        if (!copy_one(operation.from, operation.to, reason))
            return false;
        recorded.path = utf8_of(operation.to);
        note_line(hooks, "copied " + recorded.path);
        return true;
    }
    case Operation::Kind::remove_tree: {
        fs::create_directories(held_root(updates), io);
        const fs::path held = held_root(updates) / std::to_string(step);
        if (fs::exists(held, io)) {
            set_error(reason, "the held file already exists");
            return false;
        }
        fs::rename(operation.path, held, io);
        if (io) {
            set_error(reason, "cannot move " + utf8_of(operation.path));
            return false;
        }
        recorded.path = utf8_of(operation.path);
        recorded.held = utf8_of(held);
        note_line(hooks, "moved aside " + recorded.path);
        return true;
    }
    case Operation::Kind::make_link: {
        fs::create_symlink(path_from(operation.link_target), operation.path, io);
        if (io) {
            set_error(reason, "cannot make a link");
            return false;
        }
        recorded.path = utf8_of(operation.path);
        note_line(hooks, "made a link " + recorded.path);
        return true;
    }
    case Operation::Kind::replace_link: {
        const fs::path previous = fs::read_symlink(operation.path, io);
        if (io) {
            set_error(reason, "cannot read a link");
            return false;
        }
        fs::remove(operation.path, io);
        fs::create_symlink(path_from(operation.link_target), operation.path, io);
        if (io) {
            std::error_code restored;
            fs::create_symlink(previous, operation.path, restored);
            set_error(reason, "cannot replace a link");
            return false;
        }
        recorded.path = utf8_of(operation.path);
        recorded.previous = utf8_of(previous);
        note_line(hooks, "replaced a link " + recorded.path);
        return true;
    }
    case Operation::Kind::write_file: {
        const bool exists = fs::exists(operation.path, io) && !io;
        if (exists) {
            const fs::path held = held_root(updates) / std::to_string(step) / "previous";
            fs::create_directories(held.parent_path(), io);
            fs::copy_file(operation.path, held, io);
            if (io) {
                set_error(reason, "cannot keep the previous file");
                return false;
            }
            recorded.held = utf8_of(held);
        }
        if (!replace_file(operation.path, operation.bytes, reason)) {
            if (!recorded.held.empty()) {
                std::error_code removed;
                fs::remove_all(held_root(updates) / std::to_string(step), removed);
            }
            return false;
        }
        recorded.path = utf8_of(operation.path);
        recorded.created = !exists;
        note_line(hooks, "wrote " + recorded.path);
        return true;
    }
    case Operation::Kind::run_and_wait: {
        if (!run_step(path_from(operation.program), operation.arguments, hooks, reason))
            return false;
        recorded.program = operation.inverse_program;
        recorded.arguments = operation.inverse_arguments;
        note_line(hooks, "ran " + operation.program);
        return true;
    }
    }
    set_error(reason, "a step is not known");
    return false;
}

bool undo_one(const Recorded& step, const JournalHooks& hooks, std::string* reason) {
    std::error_code io;
    switch (step.kind) {
    case Operation::Kind::rename: {
        const fs::path from = path_from(step.from);
        const fs::path to = path_from(step.to);
        if (fs::exists(from, io)) {
            set_error(reason, "cannot undo a rename");
            return false;
        }
        fs::rename(to, from, io);
        if (io) {
            set_error(reason, "cannot undo a rename");
            return false;
        }
        return true;
    }
    case Operation::Kind::copy_tree: {
        fs::remove_all(path_from(step.path), io);
        if (io) {
            set_error(reason, "cannot undo a copy");
            return false;
        }
        return true;
    }
    case Operation::Kind::remove_tree: {
        const fs::path path = path_from(step.path);
        if (fs::exists(path, io)) {
            set_error(reason, "cannot undo a move");
            return false;
        }
        fs::rename(path_from(step.held), path, io);
        if (io) {
            set_error(reason, "cannot undo a move");
            return false;
        }
        return true;
    }
    case Operation::Kind::make_link: {
        const fs::path path = path_from(step.path);
        if (!fs::is_symlink(fs::symlink_status(path, io))) {
            set_error(reason, "cannot undo a link");
            return false;
        }
        fs::remove(path, io);
        if (io) {
            set_error(reason, "cannot undo a link");
            return false;
        }
        return true;
    }
    case Operation::Kind::replace_link: {
        const fs::path path = path_from(step.path);
        fs::remove(path, io);
        fs::create_symlink(path_from(step.previous), path, io);
        if (io) {
            set_error(reason, "cannot undo a link");
            return false;
        }
        return true;
    }
    case Operation::Kind::write_file: {
        const fs::path path = path_from(step.path);
        if (step.created) {
            fs::remove(path, io);
            if (io) {
                set_error(reason, "cannot undo a write");
                return false;
            }
            return true;
        }
        std::string previous;
        if (!read_text(path_from(step.held), previous, reason))
            return false;
        return replace_file(path, previous, reason);
    }
    case Operation::Kind::run_and_wait: {
        if (step.program.empty())
            return true;
        return run_step(path_from(step.program), step.arguments, hooks, reason);
    }
    }
    set_error(reason, "a step is not known");
    return false;
}

/// Undoes from the end. On a failure, rewrites the journal with what remains.
bool undo_recorded(
    const fs::path& updates,
    std::vector<Recorded> steps,
    const JournalHooks& hooks,
    std::string* reason
) {
    while (!steps.empty()) {
        std::string undo_error;
        if (!undo_one(steps.back(), hooks, &undo_error)) {
            std::string write_error;
            if (!write_journal(updates, steps, &write_error))
                set_error(reason, undo_error + "; " + write_error);
            else
                set_error(reason, undo_error);
            return false;
        }
        steps.pop_back();
    }
    clear_journal(updates);
    return true;
}

} // namespace

JournalResult apply_journal(
    const fs::path& updates,
    const std::vector<Operation>& operations,
    const JournalHooks& hooks,
    std::string* error
) {
    std::vector<Recorded> done;
    for (std::size_t index = 0; index < operations.size(); ++index) {
        const int step = static_cast<int>(index + 1);
        if (hooks.fail_before_step != nullptr && hooks.fail_before_step(hooks.context, step)) {
            set_error(error, "a step was not carried out");
            if (!undo_recorded(updates, std::move(done), hooks, error))
                return JournalResult::failed;
            set_error(error, "a step was not carried out");
            return JournalResult::failed;
        }
        Recorded recorded;
        if (!perform(operations[index], updates, step, recorded, hooks, error)) {
            std::string perform_error = error != nullptr ? *error : std::string{};
            if (!undo_recorded(updates, std::move(done), hooks, error))
                return JournalResult::failed;
            set_error(error, std::move(perform_error));
            return JournalResult::failed;
        }
        done.push_back(std::move(recorded));
        if (!write_journal(updates, done, error)) {
            std::string write_error = error != nullptr ? *error : std::string{};
            if (!undo_recorded(updates, std::move(done), hooks, error))
                return JournalResult::failed;
            set_error(error, std::move(write_error));
            return JournalResult::failed;
        }
        if (hooks.stop_after_step != nullptr && hooks.stop_after_step(hooks.context, step))
            return JournalResult::stopped;
    }
    return JournalResult::finished;
}

bool undo_journal(const fs::path& updates, const JournalHooks& hooks, std::string* error) {
    const std::optional<std::vector<Recorded>> steps = read_journal(updates, error);
    if (!steps)
        return false;
    if (steps->empty()) {
        clear_journal(updates);
        return true;
    }
    return undo_recorded(updates, *steps, hooks, error);
}

} // namespace oa::update_helper
