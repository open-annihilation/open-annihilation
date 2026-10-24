// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The state, its allocator, the instruction hook and the one protected entry
// every phase uses. A spent allowance, or a phase past its real-time limit,
// makes xpcall return the error unchanged, so a script message handler cannot
// keep running after the phase has stopped.

#include "internal.hpp"

#include <chrono>
#include <clocale>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <utility>

namespace oa::data::script_host {
namespace {

// File-local. Other files re-arm it through rearm_count_hook.
extern "C" {
static void script_count_hook(lua_State* state, lua_Debug* /*activation*/) {
    HostDetail::Control* control = control_from(state);
    // At the clock interval, nine calls in ten only read the clock. The tenth
    // counts the allowance as one call per budget_check_interval did, so the
    // count, the charges and where a spent allowance stops the phase are the
    // same. Scripts have no coroutines, so every instruction runs on this one
    // state. Once armed at one instruction, every call takes the full check.
    if (lua_gethookcount(state) == clock_check_interval) {
        ++control->clock_checks;
        if (control->clock_checks < clock_checks_per_budget_check) {
            if (phase_past_time_limit(control)) {
                raise_time_limit(state);
            }
            return;
        }
        control->clock_checks = 0;
    }
    if (control->string_refused) {
        lua_sethook(state, script_count_hook, LUA_MASKCOUNT, 1);
        luaL_error(state, "%s", control->string_message);
    }
    if (control->time_expired) {
        raise_time_limit(state);
    }
    if (control->allowance_spent) {
        lua_sethook(state, script_count_hook, LUA_MASKCOUNT, 1);
        luaL_error(state, "%s", control->spent_allowance_message);
    }
    control->instructions_used += static_cast<uint64_t>(budget_check_interval);
    if (control->instructions_used >= control->instruction_allowance) {
        control->allowance_spent = true;
        lua_sethook(state, script_count_hook, LUA_MASKCOUNT, 1);
        luaL_error(state, "%s", control->spent_allowance_message);
    }
    if (phase_past_time_limit(control)) {
        raise_time_limit(state);
    }
}
} // extern "C"

using Control = HostDetail::Control;

void write_budget_messages(Control& control) {
    char number[32]{};
    append_u64(number, number + sizeof number, control.instruction_allowance);
    char* cursor = control.instruction_message;
    char* end = control.instruction_message + sizeof control.instruction_message;
    cursor = append_text(cursor, end, "the script ran past its budget of ");
    cursor = append_text(cursor, end, number);
    append_text(cursor, end, " instructions");

    char mebibytes[32]{};
    append_u64(
        mebibytes, mebibytes + sizeof mebibytes, static_cast<uint64_t>(control.memory_cap >> 20)
    );
    cursor = control.memory_message;
    end = control.memory_message + sizeof control.memory_message;
    cursor = append_text(cursor, end, "the script used more than its ");
    cursor = append_text(cursor, end, mebibytes);
    append_text(cursor, end, " MiB of memory");
}

// Names the phase and the limit it was given. Whole seconds use the default
// sentence's unit; any other limit is named in milliseconds.
void write_time_message(Control& control) {
    const int64_t milliseconds = control.time_limit.count();
    const bool whole_seconds = milliseconds > 0 && milliseconds % 1000 == 0;
    uint64_t number = 0;
    const char* unit = "milliseconds";
    if (milliseconds > 0)
        number = static_cast<uint64_t>(milliseconds);
    if (whole_seconds) {
        number = static_cast<uint64_t>(milliseconds / 1000);
        unit = number == 1 ? "second" : "seconds";
    } else if (number == 1) {
        unit = "millisecond";
    }
    char digits[32]{};
    append_u64(digits, digits + sizeof digits, number);
    char* cursor = control.time_message;
    char* end = control.time_message + sizeof control.time_message;
    cursor = append_text(cursor, end, control.phase.c_str());
    cursor = append_text(cursor, end, " ran for more than ");
    cursor = append_text(cursor, end, digits);
    cursor = append_text(cursor, end, " ");
    cursor = append_text(cursor, end, unit);
    if (control.banked_call) {
        append_text(
            cursor,
            end,
            " on this computer; the stop is for the computer's speed, not the script's "
            "instructions"
        );
        return;
    }
    append_text(cursor, end, "; its work is too large or never ends");
}

/// Copies a profile's bank. A zero `per_tick` stores no bank. The balance opens at the limit.
void copy_profile_bank(Control& control, const Bank& bank) {
    control.bank_per_tick = bank.per_tick;
    control.bank_call_time_limit = bank.call_time_limit;
    if (bank.per_tick == 0) {
        control.bank_limit = 0;
        control.bank_balance = 0;
        return;
    }
    const uint32_t ticks = bank.ticks < 1 ? 1 : bank.ticks;
    if (bank.per_tick > UINT64_MAX / static_cast<uint64_t>(ticks))
        control.bank_limit = UINT64_MAX;
    else
        control.bank_limit = bank.per_tick * static_cast<uint64_t>(ticks);
    control.bank_balance = control.bank_limit;
}

/// Writes the sentence a banked call raises when it spends `balance`.
void write_banked_instruction_message(Control& control, uint64_t balance) {
    char balance_text[32]{};
    char per_tick_text[32]{};
    char limit_text[32]{};
    append_u64(balance_text, balance_text + sizeof balance_text, balance);
    append_u64(per_tick_text, per_tick_text + sizeof per_tick_text, control.bank_per_tick);
    append_u64(limit_text, limit_text + sizeof limit_text, control.bank_limit);
    char* cursor = control.banked_instruction_message;
    char* end = control.banked_instruction_message + sizeof control.banked_instruction_message;
    cursor = append_text(cursor, end, "the script ran past its allowance of ");
    cursor = append_text(cursor, end, balance_text);
    cursor = append_text(cursor, end, " instructions (");
    cursor = append_text(cursor, end, per_tick_text);
    cursor = append_text(cursor, end, " a tick, banked up to ");
    cursor = append_text(cursor, end, limit_text);
    append_text(cursor, end, ")");
}

/// Puts the profile allowance back and takes `balance`'s used count off the bank.
void restore_banked_call(Control& control, uint64_t balance) noexcept {
    const uint64_t used = control.instructions_used;
    const uint64_t spent = used < balance ? used : balance;
    control.bank_balance -= spent;
    control.instruction_allowance = control.profile_instruction_allowance;
    control.time_limit = control.profile_time_limit;
    control.banked_call = false;
    control.spent_allowance_message = control.instruction_message;
}

// Strips one leading "chunk:line: " prefix. The chunk is the text before the
// first colon, and the line is the digits between the colons.
bool take_prefix(
    const char* text,
    std::size_t length,
    char* chunk,
    std::size_t chunk_capacity,
    int32_t* line,
    char* message,
    std::size_t message_capacity
) {
    if (text == nullptr)
        return false;
    std::size_t colon = 0;
    while (colon < length && text[colon] != ':')
        ++colon;
    if (colon == 0 || colon >= length)
        return false;
    std::size_t index = colon + 1;
    if (index >= length || text[index] < '0' || text[index] > '9')
        return false;
    int32_t value = 0;
    while (index < length && text[index] >= '0' && text[index] <= '9') {
        const int digit = text[index] - '0';
        if (value > (INT32_MAX - digit) / 10)
            return false;
        value = static_cast<int32_t>(value * 10 + digit);
        ++index;
    }
    if (index + 1 >= length || text[index] != ':' || text[index + 1] != ' ')
        return false;
    copy_bounded(chunk, chunk_capacity, text, colon);
    *line = value;
    const std::size_t rest = index + 2;
    copy_bounded(message, message_capacity, text + rest, length - rest);
    return true;
}

const char* source_name(const char* source) {
    if (source == nullptr || source[0] == '\0')
        return "?";
    if (source[0] == '@')
        return source + 1;
    return source;
}

const char* frame_kind(const char* what) {
    if (what != nullptr && what[0] == 'm')
        return "main chunk";
    if (what != nullptr && what[0] == 't')
        return "tail call";
    return "function";
}

void append_traceback_row(Control* control, const char* chunk, int line, const char* kind) {
    char row[192];
    if (line > 0)
        std::snprintf(row, sizeof row, "\n\t%s:%d: in %s", chunk, line, kind);
    else
        std::snprintf(row, sizeof row, "\n\t%s: in %s", chunk, kind);
    if (control->traceback_text[0] == '\0')
        copy_bounded(
            control->traceback_text, sizeof control->traceback_text, "stack traceback:", 16
        );
    const std::size_t have = std::strlen(control->traceback_text);
    const std::size_t need = std::strlen(row);
    if (have + need + 1 > sizeof control->traceback_text)
        return;
    std::memcpy(control->traceback_text + have, row, need + 1);
}

void record_frames(Control* control, lua_State* state) {
    lua_Debug activation;
    bool found_line = false;
    bool found_frame = false;
    char fallback[chunk_buffer_bytes]{};
    int32_t fallback_line = 0;
    for (int level = 0; lua_getstack(state, level, &activation) != 0; ++level) {
        lua_getinfo(state, "Sl", &activation);
        if (activation.what != nullptr && activation.what[0] == 'C')
            continue;
        const char* chunk = source_name(activation.source);
        const int line = activation.currentline > 0 ? activation.currentline : 0;
        append_traceback_row(control, chunk, line, frame_kind(activation.what));
        if (!found_frame) {
            copy_bounded(fallback, sizeof fallback, chunk, std::strlen(chunk));
            fallback_line = static_cast<int32_t>(line);
            found_frame = true;
        }
        if (!found_line && line > 0) {
            copy_bounded(
                control->error_chunk, sizeof control->error_chunk, chunk, std::strlen(chunk)
            );
            control->error_line = static_cast<int32_t>(line);
            found_line = true;
        }
    }
    if (!found_line && found_frame) {
        copy_bounded(
            control->error_chunk, sizeof control->error_chunk, fallback, std::strlen(fallback)
        );
        control->error_line = fallback_line;
    }
}

bool raised_by_require(lua_State* state, lua_CFunction require_function) {
    if (require_function == nullptr)
        return false;
    lua_Debug activation;
    for (int level = 0; level < 4; ++level) {
        if (lua_getstack(state, level, &activation) == 0)
            return false;
        lua_getinfo(state, "Sf", &activation);
        const bool is_require =
            lua_iscfunction(state, -1) != 0 && lua_tocfunction(state, -1) == require_function;
        const bool is_c = activation.what != nullptr && activation.what[0] == 'C';
        lua_pop(state, 1);
        if (is_require)
            return true;
        if (!is_c)
            return false;
    }
    return false;
}

void note_error_object(Control* control, lua_State* state) {
    if (lua_type(state, 1) != LUA_TSTRING) {
        const char* type_name = lua_typename(state, lua_type(state, 1));
        char* cursor = control->error_message;
        char* end = control->error_message + sizeof control->error_message;
        cursor = append_text(cursor, end, "(error object is a ");
        cursor = append_text(cursor, end, type_name != nullptr ? type_name : "value");
        append_text(cursor, end, " value)");
        record_frames(control, state);
        return;
    }
    std::size_t length = 0;
    const char* text = lua_tolstring(state, 1, &length);
    char parsed_chunk[chunk_buffer_bytes]{};
    int32_t parsed_line = 0;
    const bool parsed = take_prefix(
        text,
        length,
        parsed_chunk,
        sizeof parsed_chunk,
        &parsed_line,
        control->error_message,
        sizeof control->error_message
    );
    if (!parsed)
        copy_bounded(control->error_message, sizeof control->error_message, text, length);
    record_frames(control, state);
    if (control->error_chunk[0] == '\0' && parsed) {
        copy_bounded(
            control->error_chunk,
            sizeof control->error_chunk,
            parsed_chunk,
            std::strlen(parsed_chunk)
        );
        control->error_line = parsed_line;
    }
}

extern "C" {
static void* allocate(void* user, void* pointer, size_t old_size, size_t new_size) {
    auto* control = static_cast<Control*>(user);
    if (new_size == 0) {
        if (pointer != nullptr) {
            if (old_size <= control->memory_in_use)
                control->memory_in_use -= old_size;
            else
                control->memory_in_use = 0;
            std::free(pointer);
        }
        return nullptr;
    }
    // A new string object arrives as a null block tagged LUA_TSTRING. The
    // size is the header plus the bytes plus one. The header was measured
    // at open, so the length compared with the cap is the same on every build.
    // The charge is that length, not the block: the header differs by build,
    // and a table's block differs from run to run. A string of at most
    // short_string_limit bytes is shared, and arrives here only when no live
    // copy exists, which depends on when the collector ran: it is not charged.
    bool charge_new_string = false;
    std::size_t new_string_length = 0;
    if (pointer == nullptr && old_size == LUA_TSTRING) {
        if (control->measuring_string_header) {
            if (new_size >= 201) {
                control->string_header = new_size - 201;
                charge_new_string = true;
                new_string_length = 200;
            }
        } else if (control->string_refused) {
            return nullptr;
        } else if (control->string_header != 0 && new_size >= control->string_header + 1) {
            const std::size_t length = new_size - control->string_header - 1;
            if (length > string_length_limit) {
                record_string_limit(control, static_cast<uint64_t>(length), nullptr);
                return nullptr;
            }
            charge_new_string = length > short_string_limit;
            new_string_length = length;
        }
    }
    const std::size_t previous = pointer == nullptr ? 0 : old_size;
    if (new_size > previous) {
        const std::size_t growth = new_size - previous;
        if (control->memory_in_use > control->memory_cap ||
            growth > control->memory_cap - control->memory_in_use) {
            control->allocation_refused = true;
            return nullptr;
        }
    }
    void* block = std::realloc(pointer, new_size);
    if (block == nullptr) {
        // A shrink is not allowed to fail. The old block is still the old size.
        if (new_size <= previous)
            return pointer;
        return nullptr;
    }
    if (previous > control->memory_in_use)
        control->memory_in_use = new_size;
    else
        control->memory_in_use = control->memory_in_use - previous + new_size;
    if (charge_new_string)
        add_charge(control, units_for_bytes(new_string_length));
    return block;
}

static int message_handler(lua_State* state) {
    Control* control = control_from(state);
    if (control->string_refused || control->location_locked)
        return 1;
    if (raised_by_require(state, control->require_function) &&
        control->raised_kind != ErrorKind::none)
        control->pending_kind = control->raised_kind;
    else
        control->pending_kind = ErrorKind::runtime;
    control->raised_kind = ErrorKind::none;
    note_error_object(control, state);
    return 1;
}

static int trampoline(lua_State* state) {
    Control* control = control_from(state);
    control->instructions_used = 0;
    control->allowance_spent = false;
    control->clock_checks = 0;
    control->time_expired = false;
    // The phase's start. Later reads of the clock are in the count hook and
    // in charge(), and only those reads can stop the phase.
    control->phase_started = std::chrono::steady_clock::now();
    write_time_message(*control);
    control->string_refused = false;
    control->string_refused_length = 0;
    control->string_message[0] = '\0';
    control->loading_chunk = false;
    control->pending_kind = ErrorKind::none;
    control->raised_kind = ErrorKind::none;
    control->location_locked = false;
    control->error_line = 0;
    control->error_message[0] = '\0';
    control->error_chunk[0] = '\0';
    control->traceback_text[0] = '\0';
    control->loading_count = 0;
    lua_sethook(state, script_count_hook, LUA_MASKCOUNT, clock_check_interval);

    switch (control->mode) {
    case PhaseMode::open_libraries: {
        // One long string, past the short-string limit, so the allocator sees
        // a fresh LUA_TSTRING and can record the header. The cap stays off until then.
        control->main_state = state;
        control->measuring_string_header = true;
        char sample[200];
        std::memset(sample, 'a', sizeof sample);
        lua_pushlstring(state, sample, sizeof sample);
        lua_pop(state, 1);
        control->measuring_string_header = false;
        open_libraries(state);
        if (control->install != nullptr)
            control->install(control->install_context, state);
        break;
    }
    case PhaseMode::run_chunk:
    case PhaseMode::compile: {
        const char* bytes =
            control->chunk_size == 0 ? "" : reinterpret_cast<const char*>(control->chunk_data);
        control->active_chunk_name = control->chunk_name_storage.c_str();
        control->loading_chunk = true;
        const int loaded =
            luaL_loadbufferx(state, bytes, control->chunk_size, control->active_chunk_name, "t");
        control->loading_chunk = false;
        if (loaded == LUA_ERRMEM) {
            if (!control->string_refused)
                control->allocation_refused = true;
            return lua_error(state);
        }
        if (loaded != LUA_OK) {
            remember_load_error(control, state);
            return lua_error(state);
        }
        if (control->mode == PhaseMode::compile)
            break;
        lua_call(state, 0, LUA_MULTRET);
        if (control->has_read && control->call_hooks.read != nullptr)
            control->call_hooks.read(control->call_hooks.context, state, lua_gettop(state));
        break;
    }
    case PhaseMode::call: {
        int arguments = 0;
        if (control->call_hooks.push != nullptr)
            arguments = control->call_hooks.push(control->call_hooks.context, state);
        lua_call(state, arguments, LUA_MULTRET);
        if (control->has_read && control->call_hooks.read != nullptr)
            control->call_hooks.read(control->call_hooks.context, state, lua_gettop(state));
        break;
    }
    case PhaseMode::host:
        if (control->host_work != nullptr)
            control->host_work(control->host_context, state);
        break;
    }
    return 0;
}
} // extern "C"

void keep_string_place(HostDetail::Control* control, const char* source, int line) {
    const char* chunk = source;
    if (chunk == nullptr || chunk[0] == '\0')
        chunk = "";
    else if (chunk[0] == '@')
        ++chunk;
    copy_bounded(control->error_chunk, sizeof control->error_chunk, chunk, std::strlen(chunk));
    control->error_line = line > 0 ? static_cast<int32_t>(line) : 0;
    control->location_locked = true;
}

void write_string_sentence(HostDetail::Control* control, uint64_t length) {
    char number[32];
    append_u64(number, number + sizeof number, length);
    char* cursor = control->string_message;
    char* end = control->string_message + sizeof control->string_message;
    cursor = append_text(cursor, end, "the script made a string of ");
    cursor = append_text(cursor, end, number);
    append_text(cursor, end, " bytes; the most is 65536");
}

void note_string_place(HostDetail::Control* control) {
    if (control->loading_chunk) {
        const char* path = control->active_chunk_name != nullptr ? control->active_chunk_name : "";
        keep_string_place(control, path, 0);
        return;
    }
    lua_State* state = control->main_state;
    if (state == nullptr) {
        control->error_chunk[0] = '\0';
        control->error_line = 0;
        control->location_locked = true;
        return;
    }
    lua_Debug activation;
    for (int level = 0; lua_getstack(state, level, &activation) != 0; ++level) {
        lua_getinfo(state, "Sl", &activation);
        if (activation.what != nullptr && activation.what[0] == 'C')
            continue;
        const int line = activation.currentline > 0 ? activation.currentline : 0;
        keep_string_place(control, activation.source, line);
        return;
    }
    control->error_chunk[0] = '\0';
    control->error_line = 0;
    control->location_locked = true;
}

// A locale category whose setting changes what a script computes, and the
// words that name it in the refusal.
struct LocaleNeed {
    int category{};
    const char* words{};
};

const LocaleNeed locale_needs[] = {
    {LC_COLLATE, "collation"},
    {LC_NUMERIC, "numeric"},
    {LC_CTYPE, "character-type"},
};

bool locale_is_c(const char* name) {
    return name != nullptr && (std::strcmp(name, "C") == 0 || std::strcmp(name, "POSIX") == 0);
}

} // namespace

void rearm_count_hook(lua_State* state) {
    lua_sethook(state, script_count_hook, LUA_MASKCOUNT, 1);
}

void charge_work(lua_State* state, uint64_t units) {
    charge(state, units);
}

void record_string_limit(HostDetail::Control* control, uint64_t length, const char* message) {
    if (control == nullptr || control->string_refused)
        return;
    control->string_refused = true;
    control->string_refused_length = length;
    if (message != nullptr) {
        copy_bounded(
            control->string_message, sizeof control->string_message, message, std::strlen(message)
        );
    } else {
        write_string_sentence(control, length);
    }
    note_string_place(control);
}

void raise_string_limit(lua_State* state, uint64_t length, const char* message) {
    HostDetail::Control* control = control_from(state);
    record_string_limit(control, length, message);
    luaL_error(state, "%s", control->string_message);
}

void remember_load_error(HostDetail::Control* control, lua_State* state) {
    control->pending_kind = ErrorKind::syntax;
    control->raised_kind = ErrorKind::syntax;
    control->location_locked = true;
    const char* path = control->active_chunk_name != nullptr ? control->active_chunk_name : "";
    if (path[0] == '@')
        ++path;
    copy_bounded(control->error_chunk, sizeof control->error_chunk, path, std::strlen(path));
    control->error_line = 0;
    control->error_message[0] = '\0';
    if (lua_type(state, -1) != LUA_TSTRING)
        return;
    std::size_t length = 0;
    const char* text = lua_tolstring(state, -1, &length);
    char parsed_chunk[chunk_buffer_bytes]{};
    int32_t parsed_line = 0;
    if (take_prefix(
            text,
            length,
            parsed_chunk,
            sizeof parsed_chunk,
            &parsed_line,
            control->error_message,
            sizeof control->error_message
        ))
        control->error_line = parsed_line;
    else
        copy_bounded(control->error_message, sizeof control->error_message, text, length);
}

Sandbox::Sandbox(std::unique_ptr<Control> control, lua_State* state) noexcept
    : control_(std::move(control)), state_(state) {
}

void Sandbox::close() noexcept {
    if (state_ == nullptr)
        return;
    if (control_ != nullptr) {
        control_->allowance_spent = true;
        control_->sandbox_failed = true;
        rearm_count_hook(state_);
    }
    lua_close(state_);
    state_ = nullptr;
}

Sandbox::~Sandbox() {
    close();
}

Sandbox::Sandbox(Sandbox&& other) noexcept
    : control_(std::move(other.control_)), state_(other.state_), failed_(other.failed_),
      failure_(std::move(other.failure_)) {
    other.state_ = nullptr;
    other.failed_ = false;
    other.failure_ = ScriptError{};
}

Sandbox& Sandbox::operator=(Sandbox&& other) noexcept {
    if (this != &other) {
        close();
        control_ = std::move(other.control_);
        state_ = other.state_;
        failed_ = other.failed_;
        failure_ = std::move(other.failure_);
        other.state_ = nullptr;
        other.failed_ = false;
        other.failure_ = ScriptError{};
    }
    return *this;
}

void Sandbox::fail(const ScriptError& error) {
    failed_ = true;
    if (control_ != nullptr)
        control_->sandbox_failed = true;
    failure_ = error;
}

bool Sandbox::fail_allocation(ScriptError& error) {
    failed_ = true;
    if (control_ != nullptr)
        control_->sandbox_failed = true;
    try {
        error = {};
        error.kind = ErrorKind::memory_budget;
        error.message = "the script used more than its memory allowance";
        if (control_ != nullptr) {
            error.package_id = control_->package_id;
            if (!control_->phase.empty())
                error.phase = control_->phase;
        }
        failure_ = error;
    } catch (const std::bad_alloc&) {
        error.kind = ErrorKind::memory_budget;
    }
    return false;
}

bool Sandbox::prepare(ScriptError& error) {
    if (state_ == nullptr || control_ == nullptr) {
        error = {};
        error.kind = ErrorKind::runtime;
        error.message = "the sandbox has been moved from";
        return false;
    }
    if (failed_) {
        error = failure_;
        return false;
    }
    return true;
}

bool Sandbox::enter(std::string_view phase, ScriptError& error) {
    try {
        if (!prepare(error))
            return false;
        control_->phase.assign(phase.begin(), phase.end());
        // The handler and the trampoline stay in the first two slots. Dropping an
        // error object from the previous phase removes a string or a plain value.
        if (lua_gettop(state_) > 2)
            lua_settop(state_, 2);
        // A C function with no upvalue is a light value and does not allocate.
        // Copying it does not allocate either. This is the only protected entry.
        lua_pushvalue(state_, 2);
        const int status = lua_pcall(state_, 0, 0, 1);
        if (status != LUA_OK && control_->error_message[0] == '\0' &&
            lua_type(state_, -1) == LUA_TSTRING) {
            std::size_t length = 0;
            const char* text = lua_tolstring(state_, -1, &length);
            char parsed_chunk[chunk_buffer_bytes]{};
            int32_t parsed_line = 0;
            if (!take_prefix(
                    text,
                    length,
                    parsed_chunk,
                    sizeof parsed_chunk,
                    &parsed_line,
                    control_->error_message,
                    sizeof control_->error_message
                ))
                copy_bounded(control_->error_message, sizeof control_->error_message, text, length);
            else if (control_->error_chunk[0] == '\0') {
                copy_bounded(
                    control_->error_chunk,
                    sizeof control_->error_chunk,
                    parsed_chunk,
                    std::strlen(parsed_chunk)
                );
                control_->error_line = parsed_line;
            }
        }
        if (lua_gettop(state_) > 2)
            lua_settop(state_, 2);
        lua_sethook(state_, nullptr, 0, 0);

        const bool too_long = control_->string_refused;
        const bool memory = control_->allocation_refused || status == LUA_ERRMEM;
        const bool instructions = control_->allowance_spent;
        const bool timed_out = control_->time_expired;
        if (status == LUA_OK && !too_long && !memory && !instructions && !timed_out) {
            error = {};
            return true;
        }

        ScriptError result;
        result.package_id = control_->package_id;
        result.phase = control_->phase;
        result.chunk = control_->error_chunk;
        result.line = control_->error_line;
        result.traceback = control_->traceback_text;
        result.message = control_->error_message;
        if (too_long) {
            result.kind = ErrorKind::string_length;
            result.message = control_->string_message;
        } else if (memory) {
            result.kind = ErrorKind::memory_budget;
            result.message = control_->memory_message;
        } else if (instructions) {
            result.kind = ErrorKind::instruction_budget;
            result.message = control_->spent_allowance_message;
        } else if (timed_out) {
            result.kind = ErrorKind::time_limit;
            result.message = control_->time_message;
        } else if (control_->pending_kind != ErrorKind::none) {
            result.kind = control_->pending_kind;
        } else if (status == LUA_ERRSYNTAX) {
            result.kind = ErrorKind::syntax;
        } else {
            result.kind = ErrorKind::runtime;
        }
        if (result.message.empty())
            result.message = "the script failed";
        fail(result);
        error = failure_;
        return false;
    } catch (const std::bad_alloc&) {
        return fail_allocation(error);
    }
}

std::optional<Sandbox> Sandbox::open(const OpenOptions& options, ScriptError& error) {
    try {
        error = {};
        // String order follows LC_COLLATE, reading and writing number text
        // LC_NUMERIC, and upper, lower and the pattern classes LC_CTYPE. The
        // process starts in C, and nothing in the engine changes that. Another
        // locale would give a script different results on each machine.
        for (const LocaleNeed& need : locale_needs) {
            const char* current = std::setlocale(need.category, nullptr);
            if (locale_is_c(current))
                continue;
            const char* name = current != nullptr && current[0] != '\0' ? current : "unknown";
            error.kind = ErrorKind::runtime;
            error.phase = "open";
            error.package_id = options.package_id;
            error.message = std::string("the C library's ") + need.words + " locale is " + name +
                            "; scripts need C";
            return std::nullopt;
        }

        std::unique_ptr<Control> control;
        try {
            control = std::make_unique<Control>();
            control->memory_cap = options.profile.budget.memory_bytes;
            control->instruction_allowance = options.profile.budget.instructions;
            control->time_limit = options.time_limit;
            control->profile_instruction_allowance = control->instruction_allowance;
            control->profile_time_limit = control->time_limit;
            copy_profile_bank(*control, options.profile.bank);
            control->sources = options.sources;
            control->package_id = options.package_id;
            control->random_seed = options.random_seed;
            control->install = options.profile.install;
            control->install_context = options.profile.context;
            write_budget_messages(*control);
            control->spent_allowance_message = control->instruction_message;
        } catch (const std::bad_alloc&) {
            error.kind = ErrorKind::memory_budget;
            error.phase = "open";
            error.package_id = options.package_id;
            error.message = "the script used more than its memory allowance";
            return std::nullopt;
        }

        lua_State* state = lua_newstate(allocate, control.get());
        if (state == nullptr) {
            error.kind = ErrorKind::memory_budget;
            error.phase = "open";
            error.package_id = options.package_id;
            error.message = control->memory_message;
            return std::nullopt;
        }
        control->main_state = state;
        // Both pushes are light C functions. The new stack has room, and neither
        // call allocates, so neither can raise.
        lua_pushcfunction(state, message_handler);
        lua_pushcfunction(state, trampoline);

        Sandbox sandbox(std::move(control), state);
        sandbox.control_->mode = PhaseMode::open_libraries;
        ScriptError open_error;
        if (!sandbox.enter("open", open_error)) {
            error = std::move(open_error);
            return std::nullopt;
        }
        return sandbox;
    } catch (const std::bad_alloc&) {
        error = {};
        error.kind = ErrorKind::memory_budget;
        error.phase = "open";
        error.message = "the script used more than its memory allowance";
        try {
            error.package_id = options.package_id;
        } catch (const std::bad_alloc&) {
        }
        return std::nullopt;
    }
}

bool Sandbox::run_host(
    std::string_view phase,
    void* context,
    void (*work)(void* context, lua_State* state),
    ScriptError& error
) {
    try {
        if (!prepare(error))
            return false;
        control_->host_context = context;
        control_->host_work = work;
        control_->mode = PhaseMode::host;
        return enter(phase, error);
    } catch (const std::bad_alloc&) {
        return fail_allocation(error);
    }
}

bool Sandbox::run_file(
    std::string_view path, std::string_view phase, const CallHooks* results, ScriptError& error
) {
    try {
        if (!prepare(error))
            return false;
        control_->phase.assign(phase.begin(), phase.end());
        control_->read_buffer.clear();
        bool found = false;
        bool threw = false;
        try {
            if (control_->sources.read != nullptr)
                found =
                    control_->sources.read(control_->sources.context, path, control_->read_buffer);
        } catch (...) {
            threw = true;
        }

        auto reject = [&](ErrorKind kind, std::string message) {
            ScriptError result;
            result.kind = kind;
            result.package_id = control_->package_id;
            result.phase = control_->phase;
            result.chunk.assign(path.begin(), path.end());
            result.message = std::move(message);
            fail(result);
            error = failure_;
            return false;
        };
        if (threw)
            return reject(
                ErrorKind::runtime, std::string(path) + ": the package file could not be read"
            );
        if (!found)
            return reject(
                ErrorKind::runtime, std::string(path) + ": the package holds no such file"
            );
        if (control_->read_buffer.size() > max_source_bytes) {
            return reject(
                ErrorKind::source_too_large,
                std::string(path) + " is " + std::to_string(control_->read_buffer.size()) +
                    " bytes of source; the most is 1048576"
            );
        }
        if (!control_->read_buffer.empty() && control_->read_buffer[0] == 0x1b) {
            return reject(
                ErrorKind::precompiled,
                std::string(path) + " is precompiled Lua; packages hold Lua source only"
            );
        }

        control_->chunk_name_storage.clear();
        control_->chunk_name_storage.push_back('@');
        control_->chunk_name_storage.append(path.begin(), path.end());
        control_->chunk_data =
            control_->read_buffer.empty() ? nullptr : control_->read_buffer.data();
        control_->chunk_size = control_->read_buffer.size();
        control_->call_hooks = results != nullptr ? *results : CallHooks{};
        control_->has_read = results != nullptr && results->read != nullptr;
        control_->mode = PhaseMode::run_chunk;
        return enter(phase, error);
    } catch (const std::bad_alloc&) {
        return fail_allocation(error);
    }
}

bool Sandbox::call(std::string_view phase, const CallHooks& hooks, ScriptError& error) {
    try {
        if (!prepare(error))
            return false;
        control_->call_hooks = hooks;
        control_->has_read = hooks.read != nullptr;
        control_->mode = PhaseMode::call;
        return enter(phase, error);
    } catch (const std::bad_alloc&) {
        return fail_allocation(error);
    }
}

void Sandbox::refill_bank() noexcept {
    if (control_ == nullptr || control_->bank_per_tick == 0)
        return;
    if (control_->bank_balance >= control_->bank_limit) {
        control_->bank_balance = control_->bank_limit;
        return;
    }
    const uint64_t room = control_->bank_limit - control_->bank_balance;
    if (control_->bank_per_tick > room)
        control_->bank_balance = control_->bank_limit;
    else
        control_->bank_balance += control_->bank_per_tick;
}

bool Sandbox::call_banked(std::string_view phase, const CallHooks& hooks, ScriptError& error) {
    try {
        if (!prepare(error))
            return false;
        if (control_->bank_per_tick == 0) {
            error = {};
            error.kind = ErrorKind::runtime;
            error.package_id = control_->package_id;
            error.phase.assign(phase.begin(), phase.end());
            error.message = "this sandbox's profile has no bank";
            return false;
        }
        if (control_->bank_balance == 0) {
            write_banked_instruction_message(*control_, 0);
            ScriptError result;
            result.kind = ErrorKind::instruction_budget;
            result.package_id = control_->package_id;
            result.phase.assign(phase.begin(), phase.end());
            result.message = control_->banked_instruction_message;
            fail(result);
            error = failure_;
            return false;
        }

        // The allowance is the balance this call starts with. The count it
        // uses comes off that balance afterwards, success or failure.
        const uint64_t balance = control_->bank_balance;
        control_->instruction_allowance = balance;
        control_->time_limit = control_->bank_call_time_limit;
        control_->banked_call = true;
        write_banked_instruction_message(*control_, balance);
        control_->spent_allowance_message = control_->banked_instruction_message;
        control_->instructions_used = 0;
        control_->call_hooks = hooks;
        control_->has_read = hooks.read != nullptr;
        control_->mode = PhaseMode::call;
        const bool ran = enter(phase, error);
        restore_banked_call(*control_, balance);
        return ran;
    } catch (const std::bad_alloc&) {
        return fail_allocation(error);
    }
}

uint64_t Sandbox::bank_balance() const noexcept {
    if (control_ == nullptr || control_->bank_per_tick == 0)
        return 0;
    return control_->bank_balance;
}

uint64_t Sandbox::bank_limit() const noexcept {
    if (control_ == nullptr || control_->bank_per_tick == 0)
        return 0;
    return control_->bank_limit;
}

lua_State* Sandbox::state() const noexcept {
    return state_;
}

bool Sandbox::failed() const noexcept {
    return failed_;
}

const ScriptError& Sandbox::failure() const noexcept {
    return failure_;
}

uint64_t Sandbox::instructions_used() const noexcept {
    return control_ == nullptr ? 0 : control_->instructions_used;
}

std::size_t Sandbox::memory_in_use() const noexcept {
    return control_ == nullptr ? 0 : control_->memory_in_use;
}

std::size_t Sandbox::string_header_bytes() const noexcept {
    return control_ == nullptr ? 0 : control_->string_header;
}

std::span<const std::string> Sandbox::printed() const noexcept {
    if (control_ == nullptr)
        return {};
    return control_->printed;
}

void Sandbox::drain_printed(
    void* context, void (*line)(void* context, std::string_view text)
) noexcept {
    if (control_ == nullptr)
        return;
    if (line != nullptr) {
        for (const std::string& kept : control_->printed)
            line(context, kept);
    }
    control_->printed.clear();
    control_->printed_bytes = 0;
    control_->lines_left_out = 0;
    control_->print_closed = false;
    control_->omission_stored = false;
}

std::string describe(const ScriptError& error) {
    std::string text;
    text += error.package_id;
    text += ": ";
    text += error.phase;
    text += ": ";
    if (!error.chunk.empty()) {
        text += error.chunk;
        text += ':';
        text += std::to_string(error.line);
        text += ": ";
    }
    text += error.message;
    return text;
}

Profile mutator_profile() noexcept {
    return Profile{"mutator", Budget{}, nullptr, nullptr};
}

bool compile_check(std::span<const uint8_t> source, std::string_view path, ScriptError& error) {
    try {
        error = {};
        if (source.size() > max_source_bytes) {
            error.kind = ErrorKind::source_too_large;
            error.phase = "compile";
            error.chunk.assign(path.begin(), path.end());
            error.message = error.chunk + " is " + std::to_string(source.size()) +
                            " bytes of source; the most is 1048576";
            return false;
        }
        if (!source.empty() && source.front() == 0x1b) {
            error.kind = ErrorKind::precompiled;
            error.phase = "compile";
            error.chunk.assign(path.begin(), path.end());
            error.message = error.chunk + " is precompiled Lua; packages hold Lua source only";
            return false;
        }

        OpenOptions options;
        options.profile = mutator_profile();
        ScriptError open_error;
        std::optional<Sandbox> sandbox = Sandbox::open(options, open_error);
        if (!sandbox) {
            error = std::move(open_error);
            error.phase = "compile";
            return false;
        }
        sandbox->control_->chunk_name_storage.clear();
        sandbox->control_->chunk_name_storage.push_back('@');
        sandbox->control_->chunk_name_storage.append(path.begin(), path.end());
        sandbox->control_->chunk_data = source.empty() ? nullptr : source.data();
        sandbox->control_->chunk_size = source.size();
        sandbox->control_->has_read = false;
        sandbox->control_->mode = PhaseMode::compile;
        return sandbox->enter("compile", error);
    } catch (const std::bad_alloc&) {
        error = {};
        error.kind = ErrorKind::memory_budget;
        error.phase = "compile";
        error.message = "the script used more than its memory allowance";
        return false;
    }
}

} // namespace oa::data::script_host
