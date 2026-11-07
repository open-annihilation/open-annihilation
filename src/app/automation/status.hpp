// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Status lines for a launcher: the events the automation endpoint sends,
// other than focus, one JSON object a line, with a hello line first and an
// exit line last. The lines go to the standard output the process was started
// with,
// or to a file, or both. They are not a second vocabulary: hello and exit
// frame the stream, and every other line is an event the producers in
// events.cpp already build.
#pragma once

#include "events.hpp"
#include "options.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace oa::app::automation {

/// Where one status stream writes a line, including its newline.
///
/// The function flushes before it returns, so a launcher never waits on a
/// buffer for the line.
///
/// @param context the pointer set with the function
/// @param line the line, its newline included
using StatusWriter = void (*)(void* context, std::string_view line);

/// One status stream: hello, the events, and one exit line.
class StatusStream {
  public:

    StatusStream() = default;

    /// Closes the file and the output the process was started with.
    ~StatusStream();
    StatusStream(const StatusStream&) = delete;
    StatusStream& operator=(const StatusStream&) = delete;

    /// Sets the function that receives each line.
    ///
    /// @param context passed to the function
    /// @param writer the function; null writes nowhere through it
    void set_writer(void* context, StatusWriter writer) noexcept;

    /// Opens `path` and sends later lines there too.
    ///
    /// @param path the file, replaced
    /// @return false when the file cannot be opened
    [[nodiscard]] bool open_file(const std::filesystem::path& path);

    /// Sends later lines to an output handle taken before a log redirect.
    ///
    /// On Windows `handle` is a HANDLE from DuplicateHandle of the standard
    /// output the process was started with. Elsewhere it is a file
    /// descriptor from dup. -1 sends nowhere.
    ///
    /// @param handle the handle
    void use_inherited_output(intptr_t handle) noexcept;

    /// Tells whether a line has somewhere to go.
    ///
    /// @return true when a writer, a file or a descriptor is set
    [[nodiscard]] bool has_destination() const noexcept;

    /// The output handle use_inherited_output gave the stream.
    ///
    /// @return the handle, or -1 when none was given
    [[nodiscard]] intptr_t inherited_output() const noexcept;

    /// Writes the hello line once.
    ///
    /// @param version the line --version prints, with no newline
    /// @param pid this process
    void hello(std::string_view version, uint64_t pid);

    /// Remembers that the game ended, and asks to leave when that is the
    /// option's moment.
    void note_game_over();

    /// Remembers that the player left before the game ended.
    ///
    /// @param connection_lost the player lost the connection
    void note_departure(bool connection_lost);

    /// Asks to leave once the score screen has been left, when the option
    /// waits for that.
    void note_score_left();

    /// Remembers that the game refused its launch: the exit line's reason is
    /// refused, whatever came before.
    void note_refused();

    /// Sets when the run leaves.
    ///
    /// @param quit the option
    void set_quit(QuitAfter quit) noexcept;

    /// Takes the request to leave, once.
    ///
    /// @return true the first time the option's moment has been reached
    [[nodiscard]] bool take_quit() noexcept;

    /// The code the process uses for this stream.
    ///
    /// With --quit-after-game a game that ended is 0 and a player who left
    /// or lost the connection is 3. Otherwise, and for a refused launch,
    /// `played` is kept.
    ///
    /// @param played the code the run would return
    /// @return the code
    [[nodiscard]] int resolved_code(int played) const noexcept;

    /// Writes the exit line once, after hello.
    ///
    /// @param played the code the run would return
    void finish(int played);

    /// Tells whether hello was written.
    ///
    /// @return true after hello
    [[nodiscard]] bool opened() const noexcept;

    /// The watch the events are compared with.
    ///
    /// @return the watch
    [[nodiscard]] EventWatch& watch() noexcept;

    /// Sets the frame and tick the next events name.
    ///
    /// @param frame the frame
    /// @param tick the match's tick, or 0 outside one
    void set_clock(uint64_t frame, int64_t tick) noexcept;

    /// Moves the frame on by one.
    void advance_frame() noexcept;

    /// The frame the next events name.
    ///
    /// @return the frame
    [[nodiscard]] uint64_t frame() const noexcept;

    /// The tick the next events name.
    ///
    /// @return the tick
    [[nodiscard]] int64_t tick() const noexcept;

    /// Hooks that write each event as one line.
    ///
    /// @return the hooks; their context is this stream
    [[nodiscard]] EventHooks hooks();

  private:

    struct HookState {
        StatusStream* stream{};
        std::string kind;
    };

    /// Writes one object as a line, with its newline, and flushes it.
    ///
    /// A stream with nowhere to write drops the line.
    ///
    /// @param json the object, without a newline
    void write_line(std::string_view json);

    /// Closes the output the process was started with.
    void close_inherited() noexcept;

    void* writer_context_{};
    StatusWriter writer_{};
    std::FILE* file_{};
    intptr_t inherited_{-1};
    HookState hook_{};
    EventWatch watch_{};
    uint64_t seq_{};
    uint64_t frame_{};
    int64_t tick_{};
    bool hello_written_{};
    bool exit_written_{};
    bool saw_game_over_{};
    bool quit_pending_{};
    QuitAfter quit_{QuitAfter::none};
    std::string reason_{"quit"};
};

/// Tells whether the command line asked for status lines.
///
/// @return true for --status-stdout or --status-file
[[nodiscard]] bool status_lines_wanted() noexcept;

/// Tells whether the command line asked for lines on standard output.
///
/// @return true for --status-stdout
[[nodiscard]] bool status_stdout_wanted() noexcept;

/// Tells whether status lines or --quit-after-game are on.
///
/// @return true when the frame hook follows the match for them
[[nodiscard]] bool status_following() noexcept;

/// Keeps the standard output the process was started with, before a log
/// redirect replaces it.
void capture_status_stdout();

/// Opens the file, when one was asked for, and writes hello.
///
/// @param version the line --version prints
/// @throws std::runtime_error when the file cannot be opened
void begin_status_lines(std::string_view version);

/// Writes the exit line when hello was written.
///
/// @param played the code the run would return
void finish_status_lines(int played);

/// Writes the exit line of a launch the game refused, when hello was written.
///
/// The line's reason is refused and its code is `code`, with or without
/// --quit-after-game.
///
/// @param code the code the process returns for the refusal
void finish_refused_status_lines(int code);

/// Writes one line to the standard output the process was started with.
///
/// That is the output capture_status_stdout kept, or the status stream's
/// once the stream took it, so a log that took the streams does not take
/// the line. Without either, the line goes to std::cout. The line is
/// flushed.
///
/// @param line the line, without a newline
void write_started_output(std::string_view line);

/// The code main returns when --quit-after-game is on.
///
/// @param played the code the run would return
/// @return 0 or 3 for that option, otherwise `played`
[[nodiscard]] int status_exit_code(int played) noexcept;

/// Sends one observed moment to the status stream.
///
/// @param now what the game holds
void status_on_observed(const Observed& now);

/// Sends a loading update to the status stream.
///
/// @param match the match being built; null before it exists
/// @param rows the loading screen's rows
void status_on_loading(const oa::World* match, std::span<const uint8_t> rows);

/// Sends the match's end to the status stream.
void status_on_match_gone();

/// Notes that the player left the running match.
///
/// @param connection_lost the player lost the connection
void status_note_departure(bool connection_lost);

/// Notes that the score screen was left.
void status_note_score_left();

/// Takes the request to leave, once.
///
/// @return true when --quit-after-game's moment has been reached
[[nodiscard]] bool status_take_quit() noexcept;

/// Tells whether the next status events name the screen, so its dialogs
/// are read only then.
///
/// @param screen the screen shown, by the endpoint's name for it
/// @return true when the screen is announced or differs from the last seen
[[nodiscard]] bool status_screen_due(std::string_view screen) noexcept;

} // namespace oa::app::automation
