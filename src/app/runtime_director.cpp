// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The director's runs (docs/director.md): --generate-script replays a
// recording to its end without drawing, records its timeline and plans a
// director script from it; --render-script replays a script's recording
// tick by tick in director mode, draws its shots at the script's size and
// frame rate, mixes the match's sounds from the director's camera and
// writes the video's chunks. Both reach the recording only through the
// extension that replays it (Extension::open_recording), on the fixed clock
// and seed; a script that names a stage in place of a recording renders the
// headless skirmish that stage sets up and plays out. --check-director-render
// renders a small script over the headless skirmish instead of a recording.
#include "oa/app/runtime.hpp"
#include "renderer_effect_events.hpp"
#include "director_state.hpp"
#include "match_models.hpp"

#include "oa/app/director_output.hpp"
#include "oa/app/game_directory.hpp"
#include "oa/app/hook_call.hpp"
#include "oa/audio/mixer.hpp"
#include "oa/audio/offline_mix.hpp"
#include "oa/base/sha256.hpp"
#include "oa/formats/oascript.hpp"
#include "oa/formats/zip.hpp"
#include "oa/media/director.hpp"
#include "oa/media/director/clock.hpp"
#include "oa/media/director/planner.hpp"
#include "oa/media/director/timeline_recorder.hpp"
#include "oa/media/director/transition.hpp"
#include "oa/sim/match_runtime.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#ifndef OA_ENGINE_VERSION
#define OA_ENGINE_VERSION "unknown"
#endif

namespace oa::app {

namespace director = oa::media::director;
namespace oascript = oa::formats::oascript;
namespace offline_mix = oa::audio::offline_mix;
namespace sha256 = oa::base::sha256;
namespace zip = oa::formats::zip;

// Media and audio may not include each other: their sound formats are tied
// here, where both are seen.
static_assert(director::audio_sample_rate == offline_mix::sample_rate);
static_assert(director::audio_channels == offline_mix::channels);
static_assert(director::audio_sample_rate == director_sample_rate);

struct Runtime::DirectorRender {
    // What is rendered.
    const oascript::Script* script{};
    const director::ShotList* shots{};
    DirectorOutputSettings output{};
    uint32_t first_chunk{}; ///< the first chunk drawn and written
    uint32_t last_chunk{};  ///< the last, included; below the video's chunk count
    RunDescription run{};   ///< what the run manifest says besides the chunks
    bool mute{};            ///< the sound is silence: no sound reaches the mix
    bool quiet{};           ///< print no line for each chunk
    /// The frames also written as stills (write_still), in increasing order;
    /// each lies in a chunk drawn.
    std::vector<uint64_t> stills{};

    // What the render did.
    std::vector<ChunkSummary> chunks{};
    sha256::Digest pcm_digest{}; ///< of every written chunk's PCM, in order
    uint64_t frames_drawn{};
    uint64_t sounds_started{};
    std::vector<std::string> clip_errors{};
    RecordingStatus last_status{}; ///< the replay's, after the last tick stepped
    std::string last_error{};      ///< its last error's text
    uint64_t world_digest{};       ///< director_world_digest after the last tick stepped
    uint64_t point_sounds{};       ///< the match's point sounds that reached the hooks
    uint64_t announcements{};      ///< unit announcements played through the hooks
};

namespace {

/// The frame size a generated script plans for unless --resolution says.
constexpr uint32_t kGeneratedWidth = oascript::default_width;
constexpr uint32_t kGeneratedHeight = oascript::default_height;
/// The frame and tick rates a generated script plans for.
constexpr oascript::Decimal kGeneratedFramerate = oascript::default_framerate;
constexpr oascript::Decimal kGeneratedTickrate = oascript::default_tickrate;
/// Ticks a replay may run past the end its extension expects before the
/// generator gives up on it: ten minutes of game time.
constexpr uint32_t kEndTickSlack = 10 * 60 * 30;
/// The most ticks a replay whose end is not known may run before the
/// generator gives up on it: a day of game time at the normal speed.
constexpr uint32_t kMaxReplayTicks = 24 * 60 * 60 * 30;
/// The fewest ticks a replay must run cleanly for a script to be made of
/// it: a minute of game time.
constexpr uint32_t kMinUsableTicks = 60 * 30;
/// Steps in a row a replay may take without its match's tick moving on
/// before the run stops.
constexpr uint32_t kMaxStalledSteps = 30;
/// Bytes of one pixel of a frame: red, green and blue.
constexpr size_t kPixelBytes = 3;
/// The extensions of a director script and a director bundle.
constexpr std::string_view kScriptExtension = ".oascript";
constexpr std::string_view kBundleExtension = ".oamovie";
/// The name the generator's temporary output gets while it is written.
constexpr std::string_view kPartialSuffix = ".part";
/// Milliseconds a second, for the lines that report how long a run took.
constexpr double kMillisecondsPerSecond = 1000.0;

/// Undoes what a run set up when the run leaves its scope, normally or by
/// an exception.
///
/// @tparam Action a callable taking nothing, which must not throw
template <typename Action>
class ScopeExit {
  public:

    /// Arms the action.
    ///
    /// @param action what to do when the scope is left
    explicit ScopeExit(Action action) : action_(std::move(action)) {}

    /// Runs the action unless it was released.
    ~ScopeExit() {
        if (armed_)
            action_();
    }

    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;

    /// Runs the action now, once.
    void run_now() noexcept {
        if (armed_)
            action_();
        armed_ = false;
    }

  private:

    Action action_;
    bool armed_{true};
};

/// Returns seconds of wall time since a moment, for the lines that report
/// how long a run took; nothing it returns reaches a file.
///
/// @param start the moment
/// @return seconds
[[nodiscard]] double seconds_since(std::chrono::steady_clock::time_point start) {
    const auto spent = std::chrono::steady_clock::now() - start;
    return static_cast<double>(
               std::chrono::duration_cast<std::chrono::milliseconds>(spent).count()
           ) /
           kMillisecondsPerSecond;
}

/// Returns a path's extension in lower case.
///
/// @param path the path
/// @return such as ".oascript"; empty when it has none
[[nodiscard]] std::string lower_extension(const fs::path& path) {
    std::string text = path_to_utf8(path.extension());
    std::transform(text.begin(), text.end(), text.begin(), [](char letter) {
        return letter >= 'A' && letter <= 'Z' ? static_cast<char>(letter - 'A' + 'a') : letter;
    });
    return text;
}

/// Reads a whole file of at most some bytes.
///
/// Throws std::runtime_error naming the file when it cannot be read or is
/// larger.
///
/// @param path the file
/// @param limit the most bytes it may hold
/// @param what what the file is, for the messages
/// @return its bytes
[[nodiscard]] std::vector<uint8_t>
read_bounded(const fs::path& path, uint64_t limit, std::string_view what) {
    std::error_code error;
    const uint64_t size = fs::file_size(path, error);
    if (error)
        throw std::runtime_error(
            "cannot read the " + std::string(what) + " " + path_to_utf8(path) + ": " +
            error.message()
        );
    if (size > limit)
        throw std::runtime_error(
            "the " + std::string(what) + " " + path_to_utf8(path) + " is larger than " +
            std::to_string(limit) + " bytes"
        );
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    std::ifstream file(path, std::ios::binary);
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file || static_cast<uint64_t>(file.gcount()) != size)
        throw std::runtime_error("cannot read the " + std::string(what) + " " + path_to_utf8(path));
    return bytes;
}

/// Writes a file whole: to a temporary name beside it first, then renamed
/// over it.
///
/// Throws std::runtime_error naming the file when it cannot be written.
///
/// @param path the file
/// @param bytes its contents
void write_whole_file(const fs::path& path, std::span<const uint8_t> bytes) {
    std::error_code error;
    if (path.has_parent_path())
        fs::create_directories(path.parent_path(), error);
    fs::path partial = path;
    partial += fs::path(std::u8string(kPartialSuffix.begin(), kPartialSuffix.end()));
    {
        std::ofstream file(partial, std::ios::binary | std::ios::trunc);
        file.write(
            reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())
        );
        file.close();
        if (!file)
            throw std::runtime_error("cannot write " + path_to_utf8(partial));
    }
    fs::rename(partial, path, error);
    if (error) {
        fs::remove(partial, error);
        throw std::runtime_error("cannot write " + path_to_utf8(path));
    }
}

/// Prints a warning of a director run on standard error.
///
/// @param text the warning
void warn(const std::string& text) {
    std::cerr << "open-annihilation: director: warning: " << text << '\n';
}

/// Reports that undoing a run's set-up failed: closing the replay, leaving
/// director mode or leaving the match. The run then ends with status 1.
///
/// @param what the error's message
void report_teardown_failure(const char* what) noexcept {
    std::fprintf(stderr, "open-annihilation: director: teardown: %s\n", what);
}

/// Returns a script decoder's message as one line.
///
/// @param source the script's name
/// @param diagnostic the message
/// @return "<source>: <key path> (line L, column C): <message>"
[[nodiscard]] std::string
diagnostic_text(const std::string& source, const oascript::Diagnostic& diagnostic) {
    std::string text = source + ": ";
    if (!diagnostic.path.empty())
        text += diagnostic.path + ' ';
    if (diagnostic.position.line != 0)
        text += "(line " + std::to_string(diagnostic.position.line) + ", column " +
                std::to_string(diagnostic.position.column) + ") ";
    return text + diagnostic.message;
}

/// Asks the extensions for the one that replays a recording, and has it
/// start the recording's match.
///
/// Throws std::runtime_error when none replays it; an extension that
/// recognises the recording and cannot replay it throws its own.
///
/// @param extension the combined extension table
/// @param[in,out] runtime the running app, with no match running
/// @param name the recording's name, for the extension and the messages
/// @param bytes the recording
/// @param[out] replay the replay's hooks
/// @param[out] info what the recording holds
void open_recording(
    const Extension& extension,
    Runtime& runtime,
    const std::string& name,
    std::span<const uint8_t> bytes,
    ReplayHooks& replay,
    RecordingInfo& info
) {
    replay = {};
    info = {};
    const RecordingInput input{name.c_str(), bytes.data(), bytes.size(), true};
    if (!call_hook_or_raise<&Extension::open_recording>(extension, runtime, input, replay, info))
        throw std::runtime_error("no extension of this build replays " + name);
    if (replay.step == nullptr || replay.status == nullptr || replay.close == nullptr)
        throw std::logic_error("the extension that replays " + name + " left a replay hook null");
}

/// Returns where a replay stands.
///
/// A status hook that throws, which it must not, is reported and leaves the
/// status all zero.
///
/// @param replay the replay
/// @param report takes the hook's name and the message, when the hook threw
/// @return its status
template <typename Report>
[[nodiscard]] RecordingStatus replay_status(const ReplayHooks& replay, Report&& report) {
    RecordingStatus status{};
    HookError error;
    call_hook<&ReplayHooks::status>(replay, error, status);
    if (error.caught) {
        status = {};
        pass_hook_error(report, HookTraits<&ReplayHooks::status>::entry.name, error);
    }
    return status;
}

/// Returns an engine camera the renderer can place: a view that would
/// start left of or above the map, as on a map smaller than the smallest
/// view, starts at its edge instead.
///
/// @param view the engine camera engine_view gave
/// @return the camera to set
[[nodiscard]] director::EngineView placed_view(director::EngineView view) noexcept {
    if (view.left < 0) {
        view.left = 0;
        view.offset_x = 0;
    }
    if (view.top < 0) {
        view.top = 0;
        view.offset_y = 0;
    }
    return view;
}

/// Returns the view that shows the whole map at an output size.
///
/// @param bounds the map's bounds
/// @param output the output size
/// @return the view, clamped
[[nodiscard]] director::View
whole_map_view(director::MapBounds bounds, director::OutputSize output) {
    const director::View view{
        static_cast<double>(bounds.width) / 2.0,
        static_cast<double>(bounds.height) / 2.0,
        director::fit_height(bounds, output)
    };
    return director::clamp_view(view, bounds, output);
}

/// Where the director's sounds go while a script renders: the offline
/// mix, each starting on the first sample of the first frame that shows
/// the tick being run.
struct SoundGlue {
    offline_mix::OfflineMix* mix{};
    uint64_t start_sample{}; ///< the first sample of the frame that first shows the tick being run
    bool audible{};          ///< the tick being run is one the video shows
    uint64_t started{};      ///< sounds started in the mix

    /// Starts a sound of the match in the mix (DirectorSoundHooks::play).
    ///
    /// @param context the SoundGlue
    /// @param sound the sound
    static void play(void* context, const DirectorSound& sound) {
        auto& glue = *static_cast<SoundGlue*>(context);
        if (!glue.audible || glue.mix == nullptr || sound.resource == nullptr)
            return;
        const oa::audio::VoicePosition position{sound.x, sound.y, sound.z};
        constexpr int32_t spatial_on = 1;
        glue.mix->start(
            {sound.resource,
             sound.volume,
             oa::audio::voice_spatial(
                 spatial_on,
                 sound.min_distance,
                 sound.max_distance,
                 sound.placed ? &position : nullptr
             ),
             glue.start_sample}
        );
        ++glue.started;
    }
};

/// Reads a sound file of the game's archives for the offline mix
/// (ClipFileHooks::load).
///
/// @param context the AssetStore
/// @param resource the file's path in the archives
/// @param[out] bytes the file
/// @return false when no archive holds it
bool load_clip_file(void* context, const char* resource, std::vector<uint8_t>* bytes) {
    try {
        *bytes = static_cast<const oa::AssetStore*>(context)->read(resource).bytes;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

/// Which units the recording's viewer sees, for the timeline recorder.
struct VisibilityGlue {
    const oa::sim::match_runtime::Match* match{};
    uint8_t viewer{};

    /// Tells whether the viewer's view draws a unit
    /// (UnitVisibilityHooks::visible): Match::unit_shown for the viewer;
    /// a unit the match cannot answer for counts as drawn.
    ///
    /// @param context the VisibilityGlue
    /// @param unit the unit's slot
    /// @return true when it is drawn
    static bool visible(void* context, uint16_t unit) {
        const auto& glue = *static_cast<const VisibilityGlue*>(context);
        const auto& slots = glue.match->world().slots;
        if (unit >= slots.size() || slots[unit].unit == nullptr)
            return false;
        try {
            return glue.match->unit_shown(glue.viewer, unit);
        } catch (const std::exception&) {
            return true;
        }
    }
};

} // namespace

struct Runtime::MatchReplay {
    Runtime* runtime{};
    uint32_t errors{};        ///< ticks that failed
    std::string last_error{}; ///< the last failure's text
};

ReplayHooks Runtime::match_replay_hooks(MatchReplay& replay) {
    return {
        &replay,
        [](void* context) {
            auto& self = *static_cast<MatchReplay*>(context);
            try {
                self.runtime->step_match_simulation();
            } catch (const std::exception& error) {
                ++self.errors;
                self.last_error = error.what();
                self.runtime->report_match_tick_error(error.what());
            }
            return self.runtime->match_ != nullptr;
        },
        [](void* context, RecordingStatus& status) {
            const auto& self = *static_cast<const MatchReplay*>(context);
            status.tick = self.runtime->match_ ? self.runtime->match_->state().game.tick : 0;
            status.clean = self.errors == 0;
            status.paced = true;
            status.errors = self.errors;
            status.last_error = self.last_error.empty() ? nullptr : self.last_error.c_str();
        },
        [](void*) {}
    };
}

uint64_t Runtime::director_world_digest() {
    const auto kept_x = match_camera_x_;
    const auto kept_z = match_camera_z_;
    match_camera_x_ = 0;
    match_camera_z_ = 0;
    const auto digest = match_world_digest();
    match_camera_x_ = kept_x;
    match_camera_z_ = kept_z;
    return digest;
}

void Runtime::render_director_frames(DirectorRender& render, const ReplayHooks& replay) {
    const auto& shots = *render.shots;
    const auto& clock = shots.clock;
    const auto output_size = shots.output;
    const uint32_t chunk_total = director::chunk_count(clock, shots.chunks, shots.frame_count);
    if (render.first_chunk > render.last_chunk || render.last_chunk >= chunk_total)
        throw std::logic_error("a render draws chunks the video has");
    offline_mix::OfflineMix mix({&assets_, load_clip_file});
    SoundGlue glue{&mix, 0, false, 0};
    const DirectorSoundHooks sounds =
        render.mute ? DirectorSoundHooks{} : DirectorSoundHooks{&glue, SoundGlue::play};
    enter_director_mode(
        {output_size.width, output_size.height, render.script->output.show_ux, true}, sounds
    );
    ScopeExit leave{[this]() noexcept { leave_director_mode(); }};
    DirectorOutput output(render.output);
    director::CameraRig rig(shots);
    const size_t frame_bytes =
        static_cast<size_t>(output_size.width) * output_size.height * kPixelBytes;
    std::vector<uint8_t> incoming(frame_bytes);
    std::vector<uint8_t> outgoing;
    std::vector<uint8_t> blended;
    std::vector<int16_t> samples;
    const uint64_t end_frame = std::min(
        shots.frame_count, director::chunk_first_frame(clock, shots.chunks, render.last_chunk + 1)
    );
    uint32_t errors_seen = replay_status(replay, hook_error_report()).errors;
    if (match_->state().game.tick > clock.first_tick)
        warn(
            "the replay starts on tick " + std::to_string(match_->state().game.tick) +
            ", after the first shot's tick " + std::to_string(clock.first_tick)
        );
    bool chunk_open = false;
    auto chunk_start = std::chrono::steady_clock::now();
    director::FrameCameras cameras{};
    // Every draw leaves the frames after it to be drawn at a whole tick.
    ScopeExit whole_ticks{[this]() noexcept { set_presentation_alpha(1.0F); }};
    while (rig.position() < end_frame && rig.next(cameras)) {
        const uint64_t frame = cameras.frame;
        const auto view = placed_view(director::engine_view(cameras.camera, output_size));
        // A frame whose time lies between two ticks is drawn from the later
        // one, part of the way from the earlier (frame_place), so that the
        // video moves at its own frame rate rather than the tick rate. A frame
        // that would need the tick no frame shows is drawn from the tick it
        // shows.
        const auto place = director::frame_place(clock, frame);
        const bool between = place.fraction < director::tick_parts && place.tick < shots.end_tick;
        const uint32_t drawn_tick = between ? place.tick : cameras.tick;
        // Every tick runs with the camera of the first frame that shows it
        // whole bound, which places its sounds, and its sounds start with
        // that frame, whether or not an earlier frame shows part of the way
        // to it.
        const auto view_of_tick = [&](uint32_t tick) {
            const uint64_t first = director::first_frame_of_tick(clock, tick);
            if (first <= frame)
                return view;
            auto ahead = rig;
            director::FrameCameras later{};
            while (later.frame < first && ahead.next(later)) {
            }
            return later.frame == first
                       ? placed_view(director::engine_view(later.camera, output_size))
                       : view;
        };
        uint32_t stalls = 0;
        while (match_ && match_->state().game.tick < drawn_tick) {
            const uint32_t before = match_->state().game.tick;
            set_director_view(view_of_tick(before + 1));
            bind_director_view();
            glue.start_sample =
                director::frame_samples(clock, director::first_frame_of_tick(clock, before + 1))
                    .first;
            glue.audible = before + 1 >= clock.first_tick;
            const auto effect_capture = capture_match_effect_events();
            if (!call_hook_or_report<&ReplayHooks::step>(replay, hook_error_report()))
                throw std::runtime_error(
                    "the replay ended on tick " + std::to_string(before) + ", before tick " +
                    std::to_string(cameras.tick) + " of the video"
                );
            if (!match_)
                throw std::runtime_error(
                    "the replay's match ended on tick " + std::to_string(before)
                );
            if (match_->state().game.tick == before) {
                if (++stalls > kMaxStalledSteps)
                    throw std::runtime_error(
                        "the replay stopped moving on tick " + std::to_string(before)
                    );
                continue;
            }
            stalls = 0;
            // The units of players this machine does not simulate are played
            // out after every tick, as the main loop's clock does.
            observe_unit_playout();
            // The presentation notes every tick, drawn or not, so that a
            // drawn frame always has the tick before at hand.
            observe_match_tick(match_models(), *match_);
            observe_match_effect_tick();
            present_director_announcements();
            const auto status = replay_status(replay, hook_error_report());
            if (status.errors > errors_seen) {
                warn(
                    "the replay failed on tick " + std::to_string(status.tick) +
                    (status.last_error != nullptr ? std::string(": ") + status.last_error : "")
                );
                errors_seen = status.errors;
            }
        }
        const uint32_t chunk = director::chunk_of_frame(clock, shots.chunks, frame);
        const bool drawn = chunk >= render.first_chunk && chunk <= render.last_chunk;
        if (drawn) {
            if (!chunk_open) {
                output.begin_chunk(chunk, frame);
                chunk_open = true;
                chunk_start = std::chrono::steady_clock::now();
            }
            set_director_view(view);
            set_presentation_alpha(
                between ? static_cast<float>(place.fraction) / director::tick_parts : 1.0F
            );
            draw_director_frame(incoming);
            std::span<const uint8_t> picture = incoming;
            if (cameras.transition) {
                outgoing.resize(frame_bytes);
                blended.resize(frame_bytes);
                set_director_view(
                    placed_view(director::engine_view(cameras.outgoing, output_size))
                );
                draw_director_frame(outgoing);
                set_presentation_alpha(1.0F);
                if (!director::blend_transition(
                        cameras.transition_kind,
                        cameras.transition_step,
                        cameras.transition_steps,
                        outgoing,
                        incoming,
                        output_size.width,
                        output_size.height,
                        blended
                    ))
                    throw std::logic_error("a transition's frames are the output's size");
                picture = blended;
            }
            set_presentation_alpha(1.0F);
            output.add_frame(frame, cameras.tick, picture);
            ++render.frames_drawn;
            if (std::binary_search(render.stills.begin(), render.stills.end(), frame)) {
                const auto still = write_still(
                    render.output.paths, frame, picture, output_size.width, output_size.height
                );
                std::printf(
                    "director: still of frame %llu, tick %u: %s\n",
                    static_cast<unsigned long long>(frame),
                    cameras.tick,
                    path_to_utf8(still).c_str()
                );
                std::fflush(stdout);
            }
        }
        // The mix runs on through frames that are not drawn, so that the
        // sound of a chunk does not depend on the chunks drawn before it.
        const auto range = director::frame_samples(clock, frame);
        samples.resize(static_cast<size_t>(range.end - range.first) * offline_mix::channels);
        mix.render(samples);
        if (!drawn)
            continue;
        output.add_samples(samples);
        if (frame + 1 == shots.frame_count ||
            director::chunk_of_frame(clock, shots.chunks, frame + 1) != chunk) {
            const auto& summary = output.end_chunk();
            chunk_open = false;
            if (!render.quiet)
                std::printf(
                    "director: chunk %u of %u: frames %llu-%llu, ticks %u-%u, %.1f s\n",
                    summary.chunk,
                    chunk_total,
                    static_cast<unsigned long long>(summary.first_frame),
                    static_cast<unsigned long long>(summary.first_frame + summary.frame_count - 1),
                    summary.first_tick,
                    summary.last_tick,
                    seconds_since(chunk_start)
                );
            std::fflush(stdout);
        }
    }
    render.world_digest = director_world_digest();
    render.point_sounds = director_->tally.point_sounds;
    render.announcements = director_->tally.announcements_played;
    render.last_status = replay_status(replay, hook_error_report());
    render.last_error =
        render.last_status.last_error != nullptr ? render.last_status.last_error : "";
    render.last_status.last_error = nullptr;
    render.chunks = output.chunks();
    render.pcm_digest = output.pcm_digest();
    render.sounds_started = glue.started;
    render.clip_errors = mix.errors();
    leave.run_now();
    output.finish(render.run);
}

int Runtime::run_render_script() {
    const auto started = std::chrono::steady_clock::now();
    // The match's sounds reach the mix through the director's hooks alone:
    // the match's own sound paths stay quiet whatever --mute says, so that
    // --mute changes nothing but the mix.
    const bool silent = options_.mute;
    options_.mute = true;
    ReplayHooks replay{};
    // The match's own ticks, when the script plays a stage.
    MatchReplay played{this, 0, {}};
    bool replay_open = false;
    bool teardown_failed = false;
    // The replay closes before its match is torn down, and director mode is
    // left before either (render_director_frames leaves it itself).
    ScopeExit teardown{[&]() noexcept {
        try {
            if (replay_open)
                call_hook_or_report<&ReplayHooks::close>(replay, hook_error_report());
            replay_open = false;
            leave_director_mode();
            if (match_)
                leave_match();
        } catch (const std::exception& error) {
            teardown_failed = true;
            report_teardown_failure(error.what());
        } catch (...) {
            teardown_failed = true;
            report_teardown_failure("an error of unknown type");
        }
    }};
    try {
        const fs::path file = options_.render_script;
        const std::string extension = lower_extension(file);
        const bool bundled = extension == kBundleExtension;
        if (!bundled && extension != kScriptExtension)
            throw std::runtime_error(
                "--render-script reads a director script (.oascript) or bundle (.oamovie), not " +
                path_to_utf8(file)
            );
        std::vector<uint8_t> archive;
        Bundle bundle{};
        std::vector<uint8_t> script_bytes;
        std::string script_name;
        if (bundled) {
            archive = read_bounded(file, zip::max_archive_bytes, "bundle");
            bundle = read_bundle(archive);
            script_bytes = std::move(bundle.script_bytes);
            script_name = bundle.script_name;
        } else {
            script_bytes = read_bounded(file, oascript::max_input_bytes, "director script");
            script_name = path_to_utf8(file.filename());
        }
        oascript::Script script{};
        oascript::DecodeReport report{};
        const bool read = oascript::read_script(script_bytes, script, report);
        for (const auto& warning : report.warnings)
            warn(diagnostic_text(script_name, warning));
        if (!read) {
            for (size_t error = 1; error < report.errors.size(); ++error)
                std::cerr << "open-annihilation: director: "
                          << diagnostic_text(script_name, report.errors[error]) << '\n';
            throw std::runtime_error(
                report.errors.empty() ? script_name + ": not a director script"
                                      : diagnostic_text(script_name, report.errors.front())
            );
        }
        // A script plays a recording or, in its place, a stage, which has no
        // end of its own.
        const bool staged = !script.input.stage.empty();
        if (staged && bundled)
            throw std::runtime_error(script_name + ": a bundle carries a recording, not a stage");
        if (staged && !script.director.end_tick)
            throw std::runtime_error(
                script_name + ": a script that plays a stage ends at director.endTick"
            );
        const std::string& recording_name = staged ? script.input.stage : script.input.recording;
        const fs::path input_path = file.parent_path() / path_from_utf8(recording_name);
        const std::vector<uint8_t> recording =
            bundled
                ? read_bundle_entry(archive, bundle, recording_name)
                : read_bounded(input_path, zip::max_entry_bytes, staged ? "stage" : "recording");
        archive = {};
        const auto encoder = encoder_settings_from_environment();
        if (encoder.enabled)
            run_encoder({director_encoder, "-hide_banner", "-loglevel", "error", "-version"});

        RecordingInfo info{};
        if (staged) {
            // The headless skirmish, which the stage sets up and plays out.
            options_.stage_file = input_path;
            prepare_headless_match();
            replay = match_replay_hooks(played);
        } else {
            open_recording(extension_, *this, recording_name, recording, replay, info);
        }
        replay_open = true;
        const auto& game = match_->state().game;
        const director::MapBounds bounds{game.map_pixel_width, game.map_pixel_height};
        const director::OutputSize output_size{script.output.width, script.output.height};
        uint32_t recording_end = info.expected_end_tick;
        if (!script.director.end_tick && recording_end == 0) {
            // The recording's end is not known: an undrawn pass finds it,
            // stepping each tick as the render does, then the recording is
            // opened again.
            std::printf("director: replaying %s to find its end\n", recording_name.c_str());
            std::fflush(stdout);
            enter_director_mode({output_size.width, output_size.height, false, true}, {});
            set_director_view(
                placed_view(director::engine_view(whole_map_view(bounds, output_size), output_size))
            );
            uint32_t stalls = 0;
            for (auto status = replay_status(replay, hook_error_report()); !status.finished;
                 status = replay_status(replay, hook_error_report())) {
                const uint32_t before = match_->state().game.tick;
                if (before >= kMaxReplayTicks)
                    throw std::runtime_error(
                        "the replay did not finish by tick " + std::to_string(before)
                    );
                bind_director_view();
                const auto effect_capture = capture_match_effect_events();
                if (!call_hook_or_report<&ReplayHooks::step>(replay, hook_error_report()) ||
                    !match_)
                    throw std::runtime_error(
                        "the replay ended on tick " + std::to_string(before) + " before it finished"
                    );
                if (match_->state().game.tick == before) {
                    if (++stalls > kMaxStalledSteps)
                        throw std::runtime_error(
                            "the replay stopped moving on tick " + std::to_string(before)
                        );
                    continue;
                }
                stalls = 0;
                // The units of players this machine does not simulate are played
                // out after every tick, as the main loop's clock does.
                observe_unit_playout();
            }
            recording_end = match_->state().game.tick + 1;
            call_hook_or_report<&ReplayHooks::close>(replay, hook_error_report());
            replay_open = false;
            leave_director_mode();
            leave_match();
            load(Screen::main_menu);
            open_recording(extension_, *this, recording_name, recording, replay, info);
            replay_open = true;
        }
        director::ShotList shots{};
        director::CompileMessages messages{};
        const bool compiled =
            director::compile_shots(script, bounds, recording_end, shots, messages);
        for (const auto& warning : messages.warnings)
            warn(script_name + ": " + warning);
        if (!compiled) {
            for (size_t error = 1; error < messages.errors.size(); ++error)
                std::cerr << "open-annihilation: director: " << script_name << ": "
                          << messages.errors[error] << '\n';
            throw std::runtime_error(
                script_name + ": " +
                (messages.errors.empty() ? std::string("the shots do not compile")
                                         : messages.errors.front())
            );
        }
        const uint32_t chunk_total =
            director::chunk_count(shots.clock, shots.chunks, shots.frame_count);
        uint32_t first_chunk = 0;
        uint32_t last_chunk = chunk_total - 1;
        if (options_.director_chunks) {
            first_chunk = options_.director_chunks->first;
            last_chunk = options_.director_chunks->second;
            if (last_chunk >= chunk_total)
                throw std::runtime_error(
                    "--chunks " + std::to_string(first_chunk) + '-' + std::to_string(last_chunk) +
                    ": the video has " + std::to_string(chunk_total) + " chunks, 0 to " +
                    std::to_string(chunk_total - 1)
                );
        }

        // Each still lies in the video and in a chunk drawn.
        for (const uint64_t still : options_.director_stills) {
            if (still >= shots.frame_count)
                throw std::runtime_error(
                    "--stills " + std::to_string(still) + ": the video has " +
                    std::to_string(shots.frame_count) + " frames, 0 to " +
                    std::to_string(shots.frame_count - 1)
                );
            const uint32_t chunk = director::chunk_of_frame(shots.clock, shots.chunks, still);
            if (chunk < first_chunk || chunk > last_chunk)
                throw std::runtime_error(
                    "--stills " + std::to_string(still) + ": the frame is in chunk " +
                    std::to_string(chunk) + ", which the render does not draw"
                );
        }

        DirectorRender render{};
        render.script = &script;
        render.shots = &shots;
        render.stills = options_.director_stills;
        fs::path directory = options_.director_output;
        if (directory.empty())
            directory = file.parent_path() / file.stem();
        render.output.paths = {directory, path_to_utf8(file.stem())};
        render.output.width = output_size.width;
        render.output.height = output_size.height;
        render.output.framerate = script.output.framerate;
        render.output.encoder = encoder;
        render.output.joined = first_chunk == 0 && last_chunk + 1 == chunk_total;
        render.first_chunk = first_chunk;
        render.last_chunk = last_chunk;
        render.mute = silent;
        render.run.engine_version = OA_ENGINE_VERSION;
        render.run.script_name = script_name;
        render.run.recording_name = recording_name;
        render.run.script_digest = sha256::digest_of(script_bytes);
        render.run.recording_digest = sha256::digest_of(recording);
        render.run.width = output_size.width;
        render.run.height = output_size.height;
        render.run.framerate = script.output.framerate;
        render.run.tickrate = script.input.tickrate;
        render.run.first_tick = shots.clock.first_tick;
        render.run.end_tick = shots.end_tick;
        render.run.frame_count = shots.frame_count;
        render.run.chunk_count = chunk_total;
        std::printf(
            "director: rendering %s: %ux%u, %s frames a second, ticks %u to %u, %llu frames in "
            "%u chunks; chunks %u to %u to %s%s\n",
            script_name.c_str(),
            output_size.width,
            output_size.height,
            oascript::decimal_text(script.output.framerate).c_str(),
            shots.clock.first_tick,
            shots.end_tick,
            static_cast<unsigned long long>(shots.frame_count),
            chunk_total,
            first_chunk,
            last_chunk,
            path_to_utf8(directory).c_str(),
            encoder.enabled ? "" : " (encoding off)"
        );
        std::fflush(stdout);
        render_director_frames(render, replay);
        teardown.run_now();
        for (const auto& clip : render.clip_errors)
            warn("a sound could not be played: " + clip);
        std::printf(
            "director: rendered %llu frames with %llu sounds (%llu at points, %llu unit "
            "announcements) in %.1f s; the replay stood at tick %u (world %016llx), %s, %u "
            "errors%s%s\n",
            static_cast<unsigned long long>(render.frames_drawn),
            static_cast<unsigned long long>(render.sounds_started),
            static_cast<unsigned long long>(render.point_sounds),
            static_cast<unsigned long long>(render.announcements),
            seconds_since(started),
            render.last_status.tick,
            static_cast<unsigned long long>(render.world_digest),
            render.last_status.finished ? "finished" : "not finished",
            render.last_status.errors,
            render.last_error.empty() ? "" : "; last error: ",
            render.last_error.c_str()
        );
        if (render.output.joined)
            std::printf("director: sound %s\n", digest_text(render.pcm_digest).c_str());
        std::fflush(stdout);
        return teardown_failed ? 1 : 0;
    } catch (const std::exception& error) {
        teardown.run_now();
        std::cerr << "open-annihilation: director: " << error.what() << '\n';
        return 1;
    }
}

int Runtime::run_generate_script() {
    const auto started = std::chrono::steady_clock::now();
    // The analysis hears nothing, and the match starts as a render's does.
    options_.mute = true;
    ReplayHooks replay{};
    bool replay_open = false;
    bool teardown_failed = false;
    ScopeExit teardown{[&]() noexcept {
        try {
            if (match_)
                match_->event_hooks = {};
            if (replay_open)
                call_hook_or_report<&ReplayHooks::close>(replay, hook_error_report());
            replay_open = false;
            leave_director_mode();
            if (match_)
                leave_match();
        } catch (const std::exception& error) {
            teardown_failed = true;
            report_teardown_failure(error.what());
        } catch (...) {
            teardown_failed = true;
            report_teardown_failure("an error of unknown type");
        }
    }};
    try {
        const fs::path recording_path = options_.generate_script;
        fs::path out = options_.director_output;
        if (out.empty()) {
            out = recording_path;
            out.replace_extension(
                fs::path(std::u8string(kScriptExtension.begin(), kScriptExtension.end()))
            );
        }
        const std::string out_extension = lower_extension(out);
        const bool bundled = out_extension == kBundleExtension;
        if (!bundled && out_extension != kScriptExtension)
            throw std::runtime_error(
                "--generate-script writes a director script (.oascript) or bundle (.oamovie), "
                "not " +
                path_to_utf8(out)
            );
        const uint32_t width = options_.window_resolution
                                   ? static_cast<uint32_t>(options_.match_width)
                                   : kGeneratedWidth;
        const uint32_t height = options_.window_resolution
                                    ? static_cast<uint32_t>(options_.match_height)
                                    : kGeneratedHeight;
        if (width % 2 != 0 || height % 2 != 0 || width < oascript::min_dimension ||
            height < oascript::min_dimension || width > oascript::max_dimension ||
            height > oascript::max_dimension)
            throw std::runtime_error(
                "a director script's frames are an even number of pixels from " +
                std::to_string(oascript::min_dimension) + " to " +
                std::to_string(oascript::max_dimension) + " a side, not " + std::to_string(width) +
                'x' + std::to_string(height)
            );
        const auto recording = read_bounded(recording_path, zip::max_entry_bytes, "recording");
        const std::string recording_name = path_to_utf8(recording_path.filename());
        // The script names its recording by its path from the script's
        // folder when it lies there or below, else by its absolute path; a
        // bundle names it by its entry name.
        const std::string demo_key =
            bundled ? recording_name : script_recording_key(recording_path, out);

        RecordingInfo info{};
        open_recording(extension_, *this, recording_name, recording, replay, info);
        replay_open = true;
        const auto& world = match_->state();
        const director::MapBounds bounds{world.game.map_pixel_width, world.game.map_pixel_height};
        const director::OutputSize output_size{width, height};
        enter_director_mode({width, height, false, true}, {});
        set_director_view(
            placed_view(director::engine_view(whole_map_view(bounds, output_size), output_size))
        );
        director::TimelineRecorder recorder;
        recorder.begin(world, info.viewer_player, selected_map_name_runtime_);
        match_->event_hooks = recorder.event_hooks();
        VisibilityGlue visibility{match_.get(), info.viewer_player};
        const director::UnitVisibilityHooks visibility_hooks{&visibility, VisibilityGlue::visible};
        const uint32_t first_tick = world.game.tick;
        const uint32_t limit =
            info.expected_end_tick != 0 ? info.expected_end_tick + kEndTickSlack : kMaxReplayTicks;
        std::printf(
            "director: replaying %s from tick %u to plan a %ux%u script\n",
            recording_name.c_str(),
            first_tick,
            width,
            height
        );
        std::fflush(stdout);
        auto status = replay_status(replay, hook_error_report());
        uint32_t errors_seen = status.errors;
        uint32_t first_error_tick = errors_seen != 0 ? first_tick : 0;
        uint32_t stalls = 0;
        while (!status.finished && match_->state().game.tick < limit) {
            const uint32_t before = match_->state().game.tick;
            bind_director_view();
            const auto effect_capture = capture_match_effect_events();
            if (!call_hook_or_report<&ReplayHooks::step>(replay, hook_error_report()) || !match_)
                break;
            if (match_->state().game.tick == before) {
                if (++stalls > kMaxStalledSteps)
                    throw std::runtime_error(
                        "the replay stopped moving on tick " + std::to_string(before)
                    );
                continue;
            }
            stalls = 0;
            // The units of players this machine does not simulate are played
            // out after every tick, as the main loop's clock does.
            observe_unit_playout();
            recorder.after_tick(match_->state(), visibility_hooks);
            status = replay_status(replay, hook_error_report());
            if (status.errors > errors_seen && first_error_tick == 0)
                first_error_tick = match_->state().game.tick;
            errors_seen = status.errors;
        }
        if (!match_)
            throw std::runtime_error("the replay's match ended before the recording did");
        const uint32_t last_tick = match_->state().game.tick;
        director::ReplayVerdict verdict{};
        verdict.finished = status.finished;
        verdict.clean = status.clean;
        verdict.paced = status.paced;
        verdict.content_differs = info.content_differs;
        verdict.errors = status.errors;
        verdict.first_error_tick = first_error_tick;
        verdict.last_error = status.last_error != nullptr ? status.last_error : "";
        if (!verdict.finished)
            throw std::runtime_error(
                "the replay of " + recording_name + " did not finish; it stopped on tick " +
                std::to_string(last_tick)
            );
        if (first_error_tick != 0) {
            if (first_error_tick < first_tick + kMinUsableTicks)
                throw std::runtime_error(
                    "the replay of " + recording_name + " failed on tick " +
                    std::to_string(first_error_tick) + ", less than a minute in" +
                    (verdict.last_error.empty() ? "" : ": " + verdict.last_error)
                );
            warn(
                "the replay failed on tick " + std::to_string(first_error_tick) +
                "; the script ends there" +
                (verdict.last_error.empty() ? "" : ": " + verdict.last_error)
            );
        }
        if (!verdict.paced)
            warn("the recording's periodic records did not come at the spacing it states");
        recorder.finish(match_->state(), verdict);
        const uint64_t world_digest = director_world_digest();
        teardown.run_now();
        const director::Timeline timeline = recorder.take();
        std::printf(
            "director: replayed ticks %u to %u (world %016llx; %s, %u errors) in %.1f s; %zu "
            "events, %zu samples\n",
            first_tick,
            last_tick,
            static_cast<unsigned long long>(world_digest),
            verdict.clean ? "clean" : "not clean",
            verdict.errors,
            seconds_since(started),
            timeline.events.size(),
            timeline.samples.size()
        );

        director::PlannerSettings settings{};
        settings.recording = demo_key;
        settings.width = width;
        settings.height = height;
        settings.framerate = kGeneratedFramerate;
        settings.tickrate = kGeneratedTickrate;
        const auto plan = director::plan_script(timeline, settings);
        for (const auto& note : plan.notes)
            std::printf("director: %s\n", note.c_str());
        // The plan must read back and compile: a failure is the engine's.
        oascript::Script decoded{};
        oascript::DecodeReport report{};
        if (!oascript::decode_script(oascript::encode_script(plan.script), decoded, report))
            throw std::logic_error(
                "the planned script does not read back: " +
                (report.errors.empty() ? std::string()
                                       : diagnostic_text("plan", report.errors.front()))
            );
        director::ShotList shots{};
        director::CompileMessages messages{};
        if (!director::compile_shots(
                decoded,
                {timeline.header.map_width, timeline.header.map_height},
                timeline.usable_end_tick,
                shots,
                messages
            ))
            throw std::logic_error(
                "the planned script does not compile: " +
                (messages.errors.empty() ? std::string() : messages.errors.front())
            );
        for (const auto& warning : messages.warnings)
            warn("the planned script: " + warning);
        const std::string text = oascript::write_script(plan.script, oascript::DocumentForm::yaml);
        if (bundled) {
            fs::path script_entry = out.filename();
            script_entry.replace_extension(
                fs::path(std::u8string(kScriptExtension.begin(), kScriptExtension.end()))
            );
            const auto bytes =
                write_bundle(path_to_utf8(script_entry), text, recording_name, recording);
            write_whole_file(out, bytes);
        } else {
            write_whole_file(out, {reinterpret_cast<const uint8_t*>(text.data()), text.size()});
        }
        std::printf(
            "director: wrote %s: %zu shots, ticks %u to %u, %llu frames, in %.1f s\n",
            path_to_utf8(out).c_str(),
            plan.script.director.shots.size(),
            shots.clock.first_tick,
            shots.end_tick,
            static_cast<unsigned long long>(shots.frame_count),
            seconds_since(started)
        );
        std::fflush(stdout);
        return teardown_failed ? 1 : 0;
    } catch (const std::exception& error) {
        teardown.run_now();
        std::cerr << "open-annihilation: director: " << error.what() << '\n';
        return 1;
    }
}

namespace {

/// The director render check's frame size: a small 16:9 frame.
constexpr uint32_t kRenderCheckWidth = 320;
constexpr uint32_t kRenderCheckHeight = 180;
/// Units a side in the check's fight, whose weapons make the sound.
constexpr size_t kRenderCheckArmy = 8;
/// Ticks the check's script shows, and each of its two chunks.
constexpr uint32_t kRenderCheckTicks = 120;
constexpr uint32_t kRenderCheckChunkTicks = 60;
/// Ticks into the script its second shot starts, with a dissolve.
constexpr uint32_t kRenderCheckCutTicks = 70;
/// Frames and ticks a second of the check's video, and its chunks' frames.
constexpr uint64_t kRenderCheckFramerate = 60;
constexpr uint64_t kRenderCheckTickrate = 30;
constexpr uint64_t kRenderCheckChunkFrames =
    kRenderCheckChunkTicks * kRenderCheckFramerate / kRenderCheckTickrate;

/// Returns the check's script: two shots over the fight, a spring and a
/// cut with a dissolve, in two chunks.
///
/// @param first_tick the tick the script starts on
/// @param centre_x the fight's centre, map pixels across
/// @param centre_z its map-image row
/// @return the script's YAML text
[[nodiscard]] std::string
render_check_script(uint32_t first_tick, int32_t centre_x, int32_t centre_z) {
    const auto number = [](int64_t value) { return std::to_string(value); };
    return "oascript: 1\n"
           "input:\n"
           "  demo: skirmish.rec\n"
           "output:\n"
           "  resolution: { width: " +
           number(kRenderCheckWidth) + ", height: " + number(kRenderCheckHeight) +
           " }\n"
           "  framerate: 60\n"
           "  chunking: { mode: ticks, length: " +
           number(kRenderCheckChunkTicks) +
           " }\n"
           "director:\n"
           "  shots:\n"
           "    - tick: " +
           number(first_tick) +
           "\n"
           "      cameraStart: { position: { x: " +
           number(centre_x - 120) + ", y: 480, z: " + number(centre_z) +
           " } }\n"
           "      cameraEnd: { position: { x: " +
           number(centre_x) + ", y: 300.5, z: " + number(centre_z) +
           " } }\n"
           "      motion: { spring: { frequency: 1.5, dampingRatio: 1 } }\n"
           "    - tick: " +
           number(first_tick + kRenderCheckCutTicks) +
           "\n"
           "      cameraStart: { position: { x: " +
           number(centre_x + 40) + ", y: 270, z: " + number(centre_z - 20) +
           " } }\n"
           "      cameraEnd: { position: { x: " +
           number(centre_x - 40) + ", y: 360, z: " + number(centre_z + 20) +
           " } }\n"
           "      transition: { type: dissolve, duration: 0.25 }\n"
           "  endTick: " +
           number(first_tick + kRenderCheckTicks) + "\n";
}

/// Reads a whole file for the check.
///
/// @param path the file
/// @return its bytes; empty when it cannot be read
[[nodiscard]] std::vector<uint8_t> read_check_file(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

} // namespace

void Runtime::check_director_render() {
    // The match's sounds reach the mix through the director's hooks alone,
    // as in a render.
    options_.mute = true;
    const auto require = [](bool ok, const std::string& what) {
        if (!ok)
            throw std::runtime_error("director render check: " + what);
    };
    // The headless skirmish with the fight --combat sets up; returns the
    // map-image point the fight is centred on.
    const auto start_fight = [this] {
        start_benchmark_skirmish();
        match_layout_ = lay_out_match(options_.match_width, options_.match_height);
        match_zoom_ = std::clamp(options_.match_zoom, kMinBattlefieldZoom, kMaxBattlefieldZoom);
        match_zoom_target_ = match_zoom_;
        spawn_combat_armies(kRenderCheckArmy);
        const int32_t centre_x = match_camera_x_ + visible_map_width() / 2;
        const int32_t centre_z = match_camera_z_ + visible_map_height() / 2;
        const int32_t ground = match_->map_height(
            static_cast<uint32_t>(centre_x) << 16, static_cast<uint32_t>(centre_z) << 16
        );
        return std::pair{centre_x, centre_z - ground / 2};
    };
    const auto restart = [this] {
        leave_match();
        load(Screen::main_menu);
    };
    MatchReplay skirmish{this, 0, {}};
    const ReplayHooks replay = match_replay_hooks(skirmish);

    // Where the check writes: beside --snapshot when given, else a
    // temporary folder removed afterwards.
    fs::path directory;
    if (!options_.snapshot.empty()) {
        directory = options_.snapshot;
        directory.replace_extension();
        directory += "-director-render";
    } else {
        directory = fs::temp_directory_path() /
                    ("oa-director-render-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    }
    std::error_code removed;
    fs::remove_all(directory, removed);

    // Renders the check's script over a fresh start of the fight; returns
    // the render and the world it reached.
    const auto render_pass = [&](uint32_t first_chunk, uint32_t last_chunk, const char* name) {
        const auto centre = start_fight();
        const uint32_t first_tick = match_->state().game.tick;
        const auto text = render_check_script(first_tick, centre.first, centre.second);
        oascript::Script script{};
        oascript::DecodeReport report{};
        require(
            oascript::read_script(
                {reinterpret_cast<const uint8_t*>(text.data()), text.size()}, script, report
            ) && report.warnings.empty(),
            "the check's script does not read: " +
                (report.errors.empty()
                     ? std::string()
                     : report.errors.front().path + ": " + report.errors.front().message)
        );
        const auto& game = match_->state().game;
        director::ShotList shots{};
        director::CompileMessages messages{};
        require(
            director::compile_shots(
                script, {game.map_pixel_width, game.map_pixel_height}, 0, shots, messages
            ),
            "the check's script does not compile: " +
                (messages.errors.empty() ? std::string() : messages.errors.front())
        );
        require(
            director::chunk_count(shots.clock, shots.chunks, shots.frame_count) == 2,
            "the check's script is not two chunks"
        );
        auto render = std::make_unique<DirectorRender>();
        render->script = &script;
        render->shots = &shots;
        render->output.paths = {directory / name, "check"};
        render->output.width = kRenderCheckWidth;
        render->output.height = kRenderCheckHeight;
        render->output.framerate = script.output.framerate;
        render->output.encoder.enabled = false;
        render->output.joined = first_chunk == 0 && last_chunk == 1;
        render->first_chunk = first_chunk;
        render->last_chunk = last_chunk;
        render->quiet = true;
        render->run.engine_version = OA_ENGINE_VERSION;
        render->run.script_name = "check.oascript";
        render->run.recording_name = "skirmish";
        render->run.script_digest =
            sha256::digest_of({reinterpret_cast<const uint8_t*>(text.data()), text.size()});
        render->run.width = kRenderCheckWidth;
        render->run.height = kRenderCheckHeight;
        render->run.framerate = script.output.framerate;
        render->run.tickrate = script.input.tickrate;
        render->run.first_tick = shots.clock.first_tick;
        render->run.end_tick = shots.end_tick;
        render->run.frame_count = shots.frame_count;
        render->run.chunk_count = 2;
        skirmish = MatchReplay{this, 0, {}};
        render_director_frames(*render, replay);
        require(!director_mode(), "the render left the match in director mode");
        require(skirmish.errors == 0, "a tick failed: " + skirmish.last_error);
        const auto digest = director_world_digest();
        const auto tick = match_->state().game.tick;
        restart();
        return std::tuple{std::move(render), digest, tick};
    };

    // Every chunk.
    auto [full, full_digest, full_tick] = render_pass(0, 1, "full");
    const DirectorPaths full_paths{directory / "full", "check"};
    require(full->chunks.size() == 2, "the render did not write two chunks");
    require(
        full->frames_drawn == kRenderCheckTicks * kRenderCheckFramerate / kRenderCheckTickrate &&
            full->chunks[0].frame_count == kRenderCheckChunkFrames &&
            full->chunks[1].frame_count == kRenderCheckChunkFrames &&
            full->chunks[1].first_frame == kRenderCheckChunkFrames,
        "the chunks do not hold a minute of ticks each at 60 frames a second"
    );
    const uint64_t chunk_samples =
        uint64_t{kRenderCheckChunkTicks} * director::audio_sample_rate / kRenderCheckTickrate;
    std::string frame_manifests;
    for (const auto& chunk : full->chunks) {
        require(chunk.sample_frames == chunk_samples, "a chunk's sound is not its frames' length");
        const auto wave = read_check_file(chunk_audio_path(full_paths, chunk.chunk));
        const auto header = offline_mix::wave_header(chunk_samples);
        require(
            wave.size() == header.size() + chunk_samples * director::audio_channels * 2 &&
                std::equal(header.begin(), header.end(), wave.begin()),
            "a chunk's WAVE file is not its sound"
        );
        const auto manifest = read_check_file(chunk_manifest_path(full_paths, chunk.chunk));
        require(
            static_cast<uint64_t>(std::count(manifest.begin(), manifest.end(), '\n')) ==
                    chunk.frame_count &&
                sha256::digest_of(manifest) == chunk.manifest_digest,
            "a chunk's frame manifest is not a line a frame"
        );
        frame_manifests.append(manifest.begin(), manifest.end());
    }
    const auto run_manifest = read_check_file(run_manifest_path(full_paths));
    require(
        std::string(run_manifest.begin(), run_manifest.end())
            .starts_with("open-annihilation director render\n"),
        "the run manifest was not written"
    );
    require(full->sounds_started != 0, "no sound of the fight reached the mix");
    require(full->clip_errors.empty(), "a sound could not be played");
    const auto second_wave = read_check_file(chunk_audio_path(full_paths, 1));
    require(
        std::any_of(
            second_wave.begin() + offline_mix::wave_header_bytes,
            second_wave.end(),
            [](uint8_t byte) { return byte != 0; }
        ),
        "the fight's second chunk is silent"
    );

    // The second chunk alone: its frames drawn without the first's, its
    // sound mixed on through the first all the same.
    auto [partial, partial_digest, partial_tick] = render_pass(1, 1, "partial");
    const DirectorPaths partial_paths{directory / "partial", "check"};
    require(
        partial->chunks.size() == 1 && partial->chunks[0].chunk == 1 &&
            !fs::exists(chunk_audio_path(partial_paths, 0)) &&
            !fs::exists(run_manifest_path(partial_paths)),
        "the partial render wrote other chunks' files"
    );
    require(
        read_check_file(chunk_audio_path(partial_paths, 1)) == second_wave,
        "the second chunk's sound depends on the chunks drawn before it"
    );
    require(
        full_tick == partial_tick && full_digest == partial_digest,
        "drawing the first chunk changed the world"
    );
    const bool frames_alike = partial->chunks[0].manifest_digest == full->chunks[1].manifest_digest;

    // The same ticks as the generator replays them: in director mode, the
    // whole map's view bound, debris particles started once a tick, nothing
    // drawn.
    // The view is bound to the whole map, so the fight's centre is not
    // needed.
    start_fight();
    const auto& game = match_->state().game;
    const director::MapBounds bounds{game.map_pixel_width, game.map_pixel_height};
    const director::OutputSize check_size{kRenderCheckWidth, kRenderCheckHeight};
    enter_director_mode({kRenderCheckWidth, kRenderCheckHeight, false, true}, {});
    set_director_view(
        placed_view(director::engine_view(whole_map_view(bounds, check_size), check_size))
    );
    const uint32_t start_tick = match_->state().game.tick;
    while (match_->state().game.tick < start_tick + kRenderCheckTicks - 1) {
        bind_director_view();
        try {
            step_match_simulation();
        } catch (const std::exception& error) {
            report_match_tick_error(error.what());
        }
    }
    leave_director_mode();
    require(
        match_->state().game.tick == full_tick && director_world_digest() == full_digest,
        "the render reached another world than the generator's undrawn replay"
    );

    const auto frames_hex = digest_text(
        sha256::digest_of(
            {reinterpret_cast<const uint8_t*>(frame_manifests.data()), frame_manifests.size()}
        )
    );
    std::printf(
        "director render: frames %s sound %s\n",
        frames_hex.c_str(),
        digest_text(full->pcm_digest).c_str()
    );
    std::printf(
        "director render: %llu sounds; the second chunk alone draws %s frames\n",
        static_cast<unsigned long long>(full->sounds_started),
        frames_alike ? "the same" : "other"
    );
    std::fflush(stdout);
    if (options_.snapshot.empty())
        fs::remove_all(directory, removed);
}

} // namespace oa::app
