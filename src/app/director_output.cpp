// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The files of a director render and the ffmpeg runs that encode them
// (director_output.hpp), and the director bundle's reader and writer.
#include "oa/app/director_output.hpp"

#include "oa/audio/offline_mix.hpp"
#include "oa/base/threads.hpp"
#include "oa/formats/png.hpp"
#include "oa/platform/system.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <numeric>
#include <stdexcept>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#endif

#ifndef OA_PROCESS_SPAWNING
#error "OA_PROCESS_SPAWNING (0 or 1) says whether the game may start other programs"
#endif

namespace oa::app {

namespace fs = std::filesystem;
namespace oascript = oa::formats::oascript;
namespace sha256 = oa::base::sha256;
namespace threads = oa::base::threads;
namespace zip = oa::formats::zip;

static_assert(director_sample_rate == oa::audio::offline_mix::sample_rate);
static_assert(director_channels == oa::audio::offline_mix::channels);

namespace {

/// The encoder's constant quality: lower is better, 18 is close to lossless
/// to the eye.
constexpr const char* kQuality = "18";
/// The sound's AAC bit rate.
constexpr const char* kAudioBitRate = "384k";
/// Frames the encoder holds while it catches up.
constexpr const char* kFrameQueue = "32";
/// Bytes of one pixel of a frame: red, green and blue.
constexpr size_t kPixelBytes = 3;
/// Bits of each of a still's samples: red, green and blue.
constexpr uint8_t kStillBitDepth = 8;
/// Bytes of one 16-bit sample.
constexpr size_t kSampleBytes = 2;
/// The extension of a director script.
constexpr std::string_view kScriptExtension = ".oascript";
/// The base of decimal numbers.
constexpr uint64_t kDecimalBase = 10;
/// A byte's bits, and the mask of a value's low byte.
constexpr uint32_t kByteBits = 8;
constexpr uint16_t kLowByte = 0xff;
/// The most threads that hash frames, and the share of the machine's
/// threads they take: the encoder and the drawing need the rest.
constexpr unsigned kMaxHashThreads = 8;
constexpr unsigned kHashThreadShare = 3;
/// Frames in flight beyond one a hashing thread, so that the drawing does
/// not wait on the slowest hash.
constexpr size_t kExtraFramesInFlight = 2;

/// Returns a path's UTF-8 spelling.
///
/// @param path the path
/// @return its UTF-8 text
[[nodiscard]] std::string path_text(const fs::path& path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}

/// Returns a path of UTF-8 text.
///
/// @param text the UTF-8 text
/// @return the path
[[nodiscard]] fs::path utf8_path(std::string_view text) {
    return fs::path(std::u8string(text.begin(), text.end()));
}

/// Returns 10 to a power.
///
/// @param places the power, at most max_decimal_places
/// @return 10^places
[[nodiscard]] uint64_t power_of_ten(uint32_t places) noexcept {
    uint64_t value = 1;
    for (uint32_t place = 0; place < places; ++place)
        value *= kDecimalBase;
    return value;
}

/// Returns a rate as the encoder writes a fraction: numerator/denominator in
/// lowest terms.
///
/// @param rate the rate, above zero
/// @return such as 60/1 or 2997/100
[[nodiscard]] std::string fraction_text(oascript::Decimal rate) {
    const auto numerator = static_cast<uint64_t>(rate.mantissa);
    const uint64_t denominator = power_of_ten(rate.places);
    const uint64_t divisor = std::gcd(numerator, denominator);
    return std::to_string(numerator / divisor) + '/' + std::to_string(denominator / divisor);
}

/// Throws the render's error for a file that cannot be written.
///
/// @param path the file
/// @param what what failed
[[noreturn]] void fail_file(const fs::path& path, const char* what) {
    throw std::runtime_error(std::string(what) + ' ' + path_text(path));
}

/// Writes bytes to a file.
///
/// Throws std::runtime_error naming the file when they cannot be written.
///
/// @param[in,out] file the open file
/// @param path its path, for the message
/// @param bytes the bytes
void write_bytes(std::ofstream& file, const fs::path& path, std::span<const uint8_t> bytes) {
    file.write(
        reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())
    );
    if (!file)
        fail_file(path, "cannot write");
}

/// Opens a file for writing, emptied.
///
/// Throws std::runtime_error naming the file when it cannot be opened.
///
/// @param[out] file the file
/// @param path its path
void open_file(std::ofstream& file, const fs::path& path) {
    file.open(path, std::ios::binary | std::ios::trunc);
    if (!file)
        fail_file(path, "cannot create");
}

/// Writes the WAVE header of some sample frames over a WAVE file's first
/// bytes and closes the file.
///
/// Throws std::runtime_error naming the file when it cannot be written.
///
/// @param[in,out] file the open file, its header's place and data written
/// @param path its path, for the message
/// @param sample_frames the sample frames it holds
void finish_wave(std::ofstream& file, const fs::path& path, uint64_t sample_frames) {
    const auto header = oa::audio::offline_mix::wave_header(sample_frames);
    file.seekp(0);
    write_bytes(file, path, header);
    file.close();
    if (!file)
        fail_file(path, "cannot write");
}

/// Removes a file the render kept only for a later step; one already gone
/// is fine, and one that cannot be removed stays beside the output, which is
/// whole without it.
///
/// @param path the file
void remove_file(const fs::path& path) {
    std::error_code error;
    std::ignore = fs::remove(path, error);
}

/// Starts the encoder.
///
/// Throws std::runtime_error when it cannot be started, as in a build that
/// starts no other programs (the OA_PROCESS_SPAWNING build option), which
/// never tries.
///
/// @param arguments the arguments, the program's name first
/// @param piped true: its standard input is a pipe from this process
/// @return the running encoder
[[nodiscard]] SDL_Process* start_encoder(const std::vector<std::string>& arguments, bool piped) {
    if (arguments.empty())
        throw std::logic_error("the encoder runs with its program's name");
#if !OA_PROCESS_SPAWNING
    std::ignore = piped;
    throw std::runtime_error(
        "cannot start " + arguments.front() + ": this build starts no other programs (" +
        director_encoder_variable + "=" + director_encoder_off + " renders without it)"
    );
#else
    std::vector<const char*> argv;
    argv.reserve(arguments.size() + 1);
    for (const auto& argument : arguments)
        argv.push_back(argument.c_str());
    argv.push_back(nullptr);
    const SDL_PropertiesID properties = SDL_CreateProperties();
    if (properties == 0)
        throw std::runtime_error(
            std::string("cannot start ") + arguments.front() + ": " + SDL_GetError()
        );
    SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argv.data());
    SDL_SetNumberProperty(
        properties,
        SDL_PROP_PROCESS_CREATE_STDIN_NUMBER,
        piped ? SDL_PROCESS_STDIO_APP : SDL_PROCESS_STDIO_NULL
    );
    SDL_SetNumberProperty(
        properties, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_NULL
    );
    SDL_SetNumberProperty(
        properties, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_INHERITED
    );
    SDL_Process* process = SDL_CreateProcessWithProperties(properties);
    SDL_DestroyProperties(properties);
    if (process == nullptr)
        throw std::runtime_error(
            "cannot start " + arguments.front() + ", which rendering needs on PATH (" +
            director_encoder_variable + "=" + director_encoder_off +
            " renders without it): " + SDL_GetError()
        );
    return process;
#endif
}

/// Makes writes to a process's input wait for the process: SDL opens the
/// pipe without blocking, and frames are handed over whole.
///
/// Throws std::runtime_error when the pipe cannot be changed.
///
/// @param input the process's input
void wait_on_input(SDL_IOStream* input) {
    const SDL_PropertiesID properties = SDL_GetIOProperties(input);
#ifdef _WIN32
    auto* pipe = static_cast<HANDLE>(
        SDL_GetPointerProperty(properties, SDL_PROP_IOSTREAM_WINDOWS_HANDLE_POINTER, nullptr)
    );
    DWORD mode = PIPE_WAIT;
    if (pipe == nullptr || !SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr))
        throw std::runtime_error("cannot make the encoder's input wait");
#else
    const auto descriptor = static_cast<int>(
        SDL_GetNumberProperty(properties, SDL_PROP_IOSTREAM_FILE_DESCRIPTOR_NUMBER, -1)
    );
    const int flags = descriptor < 0 ? -1 : fcntl(descriptor, F_GETFL);
    if (flags < 0 || fcntl(descriptor, F_SETFL, flags & ~O_NONBLOCK) < 0)
        throw std::runtime_error("cannot make the encoder's input wait");
#endif
}

/// Throws the encoder's failure.
///
/// @param program the encoder's program
/// @param status its exit status; -1 when it did not exit
[[noreturn]] void fail_encoder(const std::string& program, int status) {
    throw std::runtime_error(
        program + " failed (exit status " + std::to_string(status) + "); its messages are above"
    );
}

/// Tells whether a name ends in the script extension, in any case.
///
/// @param name the name
/// @return true for NAME.oascript
[[nodiscard]] bool names_script(std::string_view name) noexcept {
    if (name.size() <= kScriptExtension.size())
        return false;
    const auto tail = name.substr(name.size() - kScriptExtension.size());
    return std::equal(tail.begin(), tail.end(), kScriptExtension.begin(), [](char a, char b) {
        return (a >= 'A' && a <= 'Z' ? static_cast<char>(a - 'A' + 'a') : a) == b;
    });
}

/// Returns a zip error as the bundle's message.
///
/// @param error the error
/// @return such as "a name name_is_safe refuses at byte 22 (entry ../x)"
[[nodiscard]] std::string zip_message(const zip::ZipError& error) {
    std::string text = std::string(zip::zip_status_message(error.status)) + " at byte " +
                       std::to_string(error.offset);
    if (!error.entry.empty())
        text += " (entry " + error.entry + ")";
    return text;
}

} // namespace

std::string chunk_stem(const DirectorPaths& paths, uint32_t chunk) {
    std::string number = std::to_string(chunk);
    if (number.size() < chunk_number_digits)
        number.insert(0, chunk_number_digits - number.size(), '0');
    return paths.stem + '-' + number;
}

fs::path chunk_video_only_path(const DirectorPaths& paths, uint32_t chunk) {
    return paths.directory / utf8_path(chunk_stem(paths, chunk) + ".video.mp4");
}

fs::path chunk_video_path(const DirectorPaths& paths, uint32_t chunk) {
    return paths.directory / utf8_path(chunk_stem(paths, chunk) + ".mp4");
}

fs::path chunk_audio_path(const DirectorPaths& paths, uint32_t chunk) {
    return paths.directory / utf8_path(chunk_stem(paths, chunk) + ".wav");
}

fs::path chunk_manifest_path(const DirectorPaths& paths, uint32_t chunk) {
    return paths.directory / utf8_path(chunk_stem(paths, chunk) + ".frames");
}

fs::path joined_audio_path(const DirectorPaths& paths) {
    return paths.directory / utf8_path(paths.stem + ".wav");
}

fs::path joined_video_path(const DirectorPaths& paths) {
    return paths.directory / utf8_path(paths.stem + ".mp4");
}

fs::path run_manifest_path(const DirectorPaths& paths) {
    return paths.directory / utf8_path(paths.stem + ".manifest");
}

fs::path concat_list_path(const DirectorPaths& paths) {
    return paths.directory / utf8_path(paths.stem + ".concat.txt");
}

fs::path still_path(const DirectorPaths& paths, uint64_t frame) {
    std::string number = std::to_string(frame);
    if (number.size() < still_number_digits)
        number.insert(0, still_number_digits - number.size(), '0');
    return paths.directory / utf8_path(paths.stem + "-still-" + number + ".png");
}

fs::path write_still(
    const DirectorPaths& paths,
    uint64_t frame,
    std::span<const uint8_t> rgb,
    uint32_t width,
    uint32_t height
) {
    namespace png = oa::formats::png;
    if (rgb.size() != size_t{width} * height * kPixelBytes)
        throw std::logic_error("a still is its frame's width * height * 3 bytes");
    png::Header header{};
    header.width = width;
    header.height = height;
    header.bit_depth = kStillBitDepth;
    header.color_type = png::ColorType::rgb;
    header.interlace = png::Interlace::none;
    std::vector<uint8_t> file;
    if (!png::write(png::Image{header, {}, rgb}, &file))
        throw std::logic_error("a still's frame does not encode");
    const fs::path path = still_path(paths, frame);
    std::error_code made;
    if (!paths.directory.empty())
        fs::create_directories(paths.directory, made);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(
        reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size())
    );
    out.close();
    if (!out)
        throw std::runtime_error("cannot write the still " + path_text(path));
    return path;
}

uint64_t half_second_frames(oascript::Decimal framerate) noexcept {
    const auto numerator = static_cast<uint64_t>(std::max<int64_t>(framerate.mantissa, 0));
    return std::max<uint64_t>(1, numerator / (2 * power_of_ten(framerate.places)));
}

std::vector<std::string> chunk_encoder_arguments(
    const fs::path& video_only,
    uint32_t width,
    uint32_t height,
    oascript::Decimal framerate,
    std::string_view preset
) {
    return {
        director_encoder,
        "-hide_banner",
        "-loglevel",
        "error",
        "-y",
        // Raw frames on standard input.
        "-f",
        "rawvideo",
        "-pix_fmt",
        "rgb24",
        "-video_size",
        std::to_string(width) + 'x' + std::to_string(height),
        "-framerate",
        fraction_text(framerate),
        "-thread_queue_size",
        kFrameQueue,
        "-i",
        "-",
        // 4:2:0 in BT.709's limited range, and the frames tagged so.
        "-vf",
        "scale=out_color_matrix=bt709:out_range=tv,format=yuv420p,"
        "setparams=color_primaries=bt709:color_trc=bt709:colorspace=bt709:range=tv",
        "-c:v",
        "libx264",
        "-preset",
        std::string(preset),
        "-profile:v",
        "high",
        "-crf",
        kQuality,
        "-pix_fmt",
        "yuv420p",
        "-colorspace",
        "bt709",
        "-color_primaries",
        "bt709",
        "-color_trc",
        "bt709",
        "-g",
        std::to_string(half_second_frames(framerate)),
        "-flags",
        "+cgop",
        "-movflags",
        "+faststart",
        path_text(video_only),
    };
}

std::vector<std::string>
chunk_mux_arguments(const fs::path& video_only, const fs::path& wav, const fs::path& out) {
    return {
        director_encoder,
        "-hide_banner",
        "-loglevel",
        "error",
        "-y",
        "-i",
        path_text(video_only),
        "-i",
        path_text(wav),
        "-map",
        "0:v:0",
        "-map",
        "1:a:0",
        "-c:v",
        "copy",
        "-c:a",
        "aac",
        "-b:a",
        kAudioBitRate,
        "-ar",
        std::to_string(director_sample_rate),
        "-movflags",
        "+faststart",
        path_text(out),
    };
}

std::vector<std::string>
stitch_arguments(const fs::path& concat_list, const fs::path& joined_wav, const fs::path& out) {
    return {
        director_encoder,
        "-hide_banner",
        "-loglevel",
        "error",
        "-y",
        "-f",
        "concat",
        "-safe",
        "0",
        "-i",
        path_text(concat_list),
        "-i",
        path_text(joined_wav),
        "-map",
        "0:v:0",
        "-map",
        "1:a:0",
        "-c:v",
        "copy",
        "-c:a",
        "aac",
        "-b:a",
        kAudioBitRate,
        "-ar",
        std::to_string(director_sample_rate),
        "-movflags",
        "+faststart",
        path_text(out),
    };
}

std::string concat_list_text(std::span<const fs::path> files) {
    std::string text;
    for (const auto& file : files) {
        text += "file '";
        for (const char letter : path_text(file)) {
            if (letter == '\'')
                text += "'\\''";
            else
                text += letter;
        }
        text += "'\n";
    }
    return text;
}

std::string digest_text(const sha256::Digest& digest) {
    const auto hex = sha256::to_hex(digest);
    return {hex.begin(), hex.end()};
}

std::string manifest_line(uint64_t frame, uint32_t tick, const sha256::Digest& digest) {
    return std::to_string(frame) + ' ' + std::to_string(tick) + ' ' + digest_text(digest) + '\n';
}

EncoderSettings encoder_settings_from_environment() {
    EncoderSettings settings{};
    if (const auto encoder = oa::platform::environment_value(director_encoder_variable);
        encoder && !encoder->empty()) {
        if (*encoder != director_encoder_off)
            throw std::runtime_error(
                std::string(director_encoder_variable) + " is " + director_encoder_off +
                " or unset, not " + *encoder
            );
        settings.enabled = false;
    }
    if (const auto preset = oa::platform::environment_value(director_preset_variable);
        preset && !preset->empty()) {
        const std::string_view text(*preset);
        if (!std::all_of(text.begin(), text.end(), [](char letter) {
                return letter >= 'a' && letter <= 'z';
            }))
            throw std::runtime_error(
                std::string(director_preset_variable) +
                " names a libx264 preset, such as veryfast, not " + *preset
            );
        settings.preset = text;
    }
    return settings;
}

void run_encoder(const std::vector<std::string>& arguments) {
    SDL_Process* process = start_encoder(arguments, false);
    int status = -1;
    const bool exited = SDL_WaitProcess(process, true, &status);
    SDL_DestroyProcess(process);
    if (!exited || status != 0)
        fail_encoder(arguments.front(), exited ? status : -1);
}

EncoderPipe::EncoderPipe(const std::vector<std::string>& arguments)
    : process_(start_encoder(arguments, true)) {
    try {
        input_ = SDL_GetProcessInput(process_);
        if (input_ == nullptr)
            throw std::runtime_error(
                "cannot write to " + arguments.front() + ": " + SDL_GetError()
            );
        wait_on_input(input_);
    } catch (...) {
        // The error being thrown says what went wrong; the encoder's status
        // adds nothing to it.
        std::ignore = release();
        throw;
    }
}

EncoderPipe::~EncoderPipe() {
    // finish() has read the status of an encoder that was not abandoned.
    std::ignore = release();
}

int EncoderPipe::release() noexcept {
    // Closing the input ends the frames; the encoder then finishes them.
    if (input_ != nullptr)
        SDL_CloseIO(input_);
    input_ = nullptr;
    int status = -1;
    if (process_ != nullptr) {
        if (!SDL_WaitProcess(process_, true, &status))
            status = -1;
        SDL_DestroyProcess(process_);
    }
    process_ = nullptr;
    return status;
}

void EncoderPipe::write(std::span<const uint8_t> bytes) {
    if (input_ == nullptr)
        throw std::logic_error("the encoder's frames have ended");
    const uint8_t* data = bytes.data();
    size_t left = bytes.size();
    while (left > 0) {
        const size_t written = SDL_WriteIO(input_, data, left);
        if (written == 0)
            throw std::runtime_error(
                std::string(director_encoder) + " stopped taking frames: " + SDL_GetError()
            );
        data += written;
        left -= written;
    }
}

void EncoderPipe::finish() {
    const int status = release();
    if (status != 0)
        fail_encoder(director_encoder, status);
}

std::string run_manifest_text(
    const RunDescription& run,
    std::span<const ChunkSummary> chunks,
    const sha256::Digest& pcm_digest
) {
    std::string text = "open-annihilation director render\n";
    text += "engine " + run.engine_version + '\n';
    text += "script " + digest_text(run.script_digest) + ' ' + run.script_name + '\n';
    text += "recording " + digest_text(run.recording_digest) + ' ' + run.recording_name + '\n';
    text += "output " + std::to_string(run.width) + 'x' + std::to_string(run.height) +
            " framerate " + oascript::decimal_text(run.framerate) + " tickrate " +
            oascript::decimal_text(run.tickrate) + '\n';
    if (!run.graphics_card.empty())
        text += "drawn graphics-card " + run.graphics_card +
                " full-tier: frames differ from one driver to the next and are not pinned\n";
    text += "ticks " + std::to_string(run.first_tick) + " to " + std::to_string(run.end_tick) +
            " (exclusive)\n";
    text += "frames " + std::to_string(run.frame_count) + '\n';
    text += "chunks " + std::to_string(run.chunk_count) + '\n';
    for (const auto& chunk : chunks) {
        const uint64_t last_frame =
            chunk.frame_count == 0 ? chunk.first_frame : chunk.first_frame + chunk.frame_count - 1;
        text += "chunk " + std::to_string(chunk.chunk) + " frames " +
                std::to_string(chunk.first_frame) + '-' + std::to_string(last_frame) + " ticks " +
                std::to_string(chunk.first_tick) + '-' + std::to_string(chunk.last_tick) +
                " samples " + std::to_string(chunk.sample_frames) + " frames-sha256 " +
                digest_text(chunk.manifest_digest) + " pcm-sha256 " +
                digest_text(chunk.pcm_digest) + '\n';
    }
    text += "pcm-sha256 " + digest_text(pcm_digest) + '\n';
    return text;
}

struct DirectorOutput::FrameHashing {
    /// One frame being hashed.
    struct Frame {
        uint64_t frame{};
        uint32_t tick{};
        std::vector<uint8_t> rgb{};
        sha256::Digest digest{};
        bool done{}; ///< the digest is ready; guarded by the mutex
    };

    threads::Mutex mutex{};
    threads::ConditionVariable work{};     ///< a job came, or the threads stop
    threads::ConditionVariable finished{}; ///< a job is done
    std::deque<Frame*> jobs{};             ///< frames no thread took yet; guarded by the mutex
    bool stopping{};                       ///< guarded by the mutex
    // The render's thread alone uses these.
    std::deque<std::unique_ptr<Frame>> in_flight{}; ///< in frame order
    std::vector<std::unique_ptr<Frame>> spare{};    ///< frames whose memory is kept for reuse
    size_t capacity{};                              ///< the most frames in flight
    std::vector<threads::Thread> workers{};

    /// Starts the hashing threads.
    ///
    /// @param count threads, at least 1
    explicit FrameHashing(unsigned count) : capacity(count + kExtraFramesInFlight), workers(count) {
        for (auto& worker : workers) {
            if (!threads::start_thread(worker, run_worker, this)) {
                stop();
                throw std::runtime_error("cannot start the threads that hash a render's frames");
            }
        }
    }

    /// Lets the threads finish the frames they were given, and joins them.
    ~FrameHashing() { stop(); }

    FrameHashing(const FrameHashing&) = delete;
    FrameHashing& operator=(const FrameHashing&) = delete;

    /// Stops and joins the threads once no frame waits for one.
    void stop() noexcept {
        {
            const threads::LockGuard lock(mutex);
            stopping = true;
        }
        work.notify_all();
        for (auto& worker : workers)
            threads::join_thread(worker);
        workers.clear();
    }

    /// Runs one hashing thread.
    ///
    /// @param hashing the FrameHashing whose frames the thread hashes
    static void run_worker(void* hashing) { static_cast<FrameHashing*>(hashing)->run(); }

    /// Hashes frames until the threads stop and no frame waits.
    void run() noexcept {
        for (;;) {
            Frame* job = nullptr;
            {
                const threads::LockGuard lock(mutex);
                work.wait(mutex, [this] { return stopping || !jobs.empty(); });
                if (jobs.empty())
                    return;
                job = jobs.front();
                jobs.pop_front();
            }
            const auto digest = sha256::digest_of(job->rgb);
            {
                const threads::LockGuard lock(mutex);
                job->digest = digest;
                job->done = true;
            }
            finished.notify_all();
        }
    }
};

DirectorOutput::DirectorOutput(DirectorOutputSettings settings) : settings_(std::move(settings)) {
    if (settings_.width == 0 || settings_.height == 0 || settings_.width % 2 != 0 ||
        settings_.height % 2 != 0)
        throw std::runtime_error(
            "a render's frames are an even number of pixels a side, not " +
            std::to_string(settings_.width) + 'x' + std::to_string(settings_.height)
        );
    std::error_code error;
    fs::create_directories(settings_.paths.directory, error);
    if (error || !fs::is_directory(settings_.paths.directory))
        fail_file(settings_.paths.directory, "cannot make the output directory");
    const unsigned machine_threads = std::max(1U, threads::processor_count());
    hashing_ = std::make_unique<FrameHashing>(
        std::clamp(machine_threads / kHashThreadShare, 1U, kMaxHashThreads)
    );
    if (settings_.joined && settings_.encoder.enabled) {
        const auto path = joined_audio_path(settings_.paths);
        open_file(joined_wave_, path);
        const std::array<uint8_t, oa::audio::offline_mix::wave_header_bytes> placeholder{};
        write_bytes(joined_wave_, path, placeholder);
    }
}

DirectorOutput::~DirectorOutput() = default;

void DirectorOutput::begin_chunk(uint32_t chunk, uint64_t first_frame) {
    if (open_ || finished_)
        throw std::logic_error("a render's chunk begins after the one before ends");
    if (!chunks_.empty() && chunk <= chunks_.back().chunk)
        throw std::logic_error("a render's chunks come in order");
    current_ = ChunkSummary{};
    current_.chunk = chunk;
    current_.first_frame = first_frame;
    next_frame_ = first_frame;
    manifest_hash_ = {};
    pcm_hash_ = {};
    open_file(manifest_, chunk_manifest_path(settings_.paths, chunk));
    const auto wave_path = chunk_audio_path(settings_.paths, chunk);
    open_file(wave_, wave_path);
    const std::array<uint8_t, oa::audio::offline_mix::wave_header_bytes> placeholder{};
    write_bytes(wave_, wave_path, placeholder);
    if (settings_.encoder.enabled)
        encoder_ = std::make_unique<EncoderPipe>(chunk_encoder_arguments(
            chunk_video_only_path(settings_.paths, chunk),
            settings_.width,
            settings_.height,
            settings_.framerate,
            settings_.encoder.preset
        ));
    open_ = true;
}

void DirectorOutput::add_frame(uint64_t frame, uint32_t tick, std::span<const uint8_t> rgb) {
    if (!open_)
        throw std::logic_error("a frame goes into an open chunk");
    if (frame != next_frame_)
        throw std::logic_error("a chunk's frames come in order");
    if (rgb.size() != static_cast<size_t>(settings_.width) * settings_.height * kPixelBytes)
        throw std::logic_error("a frame is width * height * 3 bytes");
    if (encoder_ != nullptr)
        encoder_->write(rgb);
    // The frame is hashed on a thread of its own while the next is drawn.
    auto& hashing = *hashing_;
    retire_frames(hashing.capacity - 1);
    std::unique_ptr<FrameHashing::Frame> slot;
    if (hashing.spare.empty()) {
        slot = std::make_unique<FrameHashing::Frame>();
    } else {
        slot = std::move(hashing.spare.back());
        hashing.spare.pop_back();
    }
    slot->frame = frame;
    slot->tick = tick;
    slot->rgb.assign(rgb.begin(), rgb.end());
    slot->done = false;
    FrameHashing::Frame* job = slot.get();
    hashing.in_flight.push_back(std::move(slot));
    {
        const threads::LockGuard lock(hashing.mutex);
        hashing.jobs.push_back(job);
    }
    hashing.work.notify_one();
    if (current_.frame_count == 0)
        current_.first_tick = tick;
    current_.last_tick = tick;
    ++current_.frame_count;
    ++next_frame_;
}

void DirectorOutput::add_samples(std::span<const int16_t> samples) {
    if (!open_)
        throw std::logic_error("sound goes into an open chunk");
    if (samples.size() % director_channels != 0)
        throw std::logic_error("a chunk's sound is whole sample frames");
    little_endian_.resize(samples.size() * kSampleBytes);
    for (size_t index = 0; index < samples.size(); ++index) {
        const auto value = static_cast<uint16_t>(samples[index]);
        little_endian_[index * kSampleBytes] = static_cast<uint8_t>(value & kLowByte);
        little_endian_[index * kSampleBytes + 1] = static_cast<uint8_t>(value >> kByteBits);
    }
    write_bytes(wave_, chunk_audio_path(settings_.paths, current_.chunk), little_endian_);
    sha256::update(pcm_hash_, little_endian_);
    sha256::update(joined_pcm_hash_, little_endian_);
    if (joined_wave_.is_open())
        write_bytes(joined_wave_, joined_audio_path(settings_.paths), little_endian_);
    const uint64_t frames = samples.size() / director_channels;
    current_.sample_frames += frames;
    joined_sample_frames_ += frames;
}

void DirectorOutput::retire_frames(size_t in_flight) {
    auto& hashing = *hashing_;
    while (!hashing.in_flight.empty()) {
        const auto& oldest = *hashing.in_flight.front();
        {
            const threads::LockGuard lock(hashing.mutex);
            if (!oldest.done) {
                if (hashing.in_flight.size() <= in_flight)
                    return;
                hashing.finished.wait(hashing.mutex, [&oldest] { return oldest.done; });
            }
        }
        const auto line = manifest_line(oldest.frame, oldest.tick, oldest.digest);
        const std::span<const uint8_t> line_bytes{
            reinterpret_cast<const uint8_t*>(line.data()), line.size()
        };
        write_bytes(manifest_, chunk_manifest_path(settings_.paths, current_.chunk), line_bytes);
        sha256::update(manifest_hash_, line_bytes);
        hashing.spare.push_back(std::move(hashing.in_flight.front()));
        hashing.in_flight.pop_front();
    }
}

const ChunkSummary& DirectorOutput::end_chunk() {
    if (!open_)
        throw std::logic_error("only an open chunk ends");
    retire_frames(0);
    open_ = false;
    const auto chunk = current_.chunk;
    const auto manifest_path = chunk_manifest_path(settings_.paths, chunk);
    manifest_.close();
    if (!manifest_)
        fail_file(manifest_path, "cannot write");
    const auto wave_path = chunk_audio_path(settings_.paths, chunk);
    finish_wave(wave_, wave_path, current_.sample_frames);
    current_.manifest_digest = sha256::finish(manifest_hash_);
    current_.pcm_digest = sha256::finish(pcm_hash_);
    if (encoder_ != nullptr) {
        encoder_->finish();
        encoder_.reset();
        const auto video_only = chunk_video_only_path(settings_.paths, chunk);
        run_encoder(
            chunk_mux_arguments(video_only, wave_path, chunk_video_path(settings_.paths, chunk))
        );
        // The joined video is made from the chunks' frames as they are; a
        // render of some chunks makes none.
        if (!settings_.joined)
            remove_file(video_only);
    }
    chunks_.push_back(current_);
    return chunks_.back();
}

void DirectorOutput::finish(const RunDescription& run) {
    if (open_)
        throw std::logic_error("a render ends after its last chunk");
    if (finished_)
        return;
    finished_ = true;
    if (!settings_.joined)
        return;
    if (settings_.encoder.enabled) {
        const auto joined_wave = joined_audio_path(settings_.paths);
        finish_wave(joined_wave_, joined_wave, joined_sample_frames_);
        std::vector<fs::path> files;
        files.reserve(chunks_.size());
        for (const auto& chunk : chunks_)
            files.push_back(chunk_video_only_path(settings_.paths, chunk.chunk).filename());
        const auto list = concat_list_path(settings_.paths);
        {
            std::ofstream file;
            open_file(file, list);
            const auto text = concat_list_text(files);
            write_bytes(file, list, {reinterpret_cast<const uint8_t*>(text.data()), text.size()});
        }
        run_encoder(stitch_arguments(list, joined_wave, joined_video_path(settings_.paths)));
        remove_file(list);
        remove_file(joined_wave);
        for (const auto& chunk : chunks_)
            remove_file(chunk_video_only_path(settings_.paths, chunk.chunk));
    }
    const auto text = run_manifest_text(run, chunks_, pcm_digest());
    const auto path = run_manifest_path(settings_.paths);
    std::ofstream file;
    open_file(file, path);
    write_bytes(file, path, {reinterpret_cast<const uint8_t*>(text.data()), text.size()});
    file.close();
    if (!file)
        fail_file(path, "cannot write");
}

const std::vector<ChunkSummary>& DirectorOutput::chunks() const noexcept {
    return chunks_;
}

sha256::Digest DirectorOutput::pcm_digest() const noexcept {
    return sha256::finish(joined_pcm_hash_);
}

Bundle read_bundle(std::span<const uint8_t> archive) {
    Bundle bundle{};
    zip::ZipError error{};
    if (!zip::read_directory(archive, bundle.directory, error))
        throw std::runtime_error(
            "the bundle is not a zip archive this build reads: " + zip_message(error)
        );
    const zip::Entry* script = nullptr;
    size_t scripts = 0;
    for (const auto& entry : bundle.directory.entries) {
        if (entry.directory || entry.name.find('/') != std::string::npos ||
            !names_script(entry.name))
            continue;
        ++scripts;
        script = &entry;
    }
    if (scripts != 1)
        throw std::runtime_error(
            "a bundle holds one director script (.oascript) at its root, this one " +
            std::to_string(scripts)
        );
    if (script->bytes > oascript::max_input_bytes)
        throw std::runtime_error(
            "the bundle's script " + script->name + " is larger than " +
            std::to_string(oascript::max_input_bytes) + " bytes"
        );
    if (!zip::read_entry(archive, *script, bundle.script_bytes, error))
        throw std::runtime_error("the bundle's script cannot be read: " + zip_message(error));
    bundle.script_name = script->name;
    return bundle;
}

std::string script_recording_key(const fs::path& recording, const fs::path& script) {
    const auto resolved{[](const fs::path& path) {
        std::error_code error;
        fs::path absolute{fs::absolute(path, error)};
        if (error)
            absolute = path;
        fs::path canonical{fs::weakly_canonical(absolute, error)};
        return (error ? absolute : canonical).lexically_normal();
    }};
    const fs::path file{resolved(recording)};
    const fs::path folder{resolved(script).parent_path()};
    const fs::path relative{file.lexically_relative(folder)};
    const bool below{
        !relative.empty() && !relative.is_absolute() && *relative.begin() != fs::path("..") &&
        relative != fs::path(".")
    };
    const std::u8string text{(below ? relative : file).generic_u8string()};
    return {text.begin(), text.end()};
}

std::vector<uint8_t>
read_bundle_entry(std::span<const uint8_t> archive, const Bundle& bundle, std::string_view name) {
    if (!zip::name_is_safe(name))
        throw std::runtime_error(
            "the script's demo key names no entry a bundle may hold: " + std::string(name)
        );
    const zip::Entry* entry = zip::find_entry(bundle.directory, name);
    if (entry == nullptr || entry->directory)
        throw std::runtime_error("the bundle holds no recording named " + std::string(name));
    std::vector<uint8_t> bytes;
    zip::ZipError error{};
    if (!zip::read_entry(archive, *entry, bytes, error))
        throw std::runtime_error("the bundle's recording cannot be read: " + zip_message(error));
    return bytes;
}

std::vector<uint8_t> write_bundle(
    std::string_view script_name,
    std::string_view script_text,
    std::string_view recording_name,
    std::span<const uint8_t> recording_bytes
) {
    if (!names_script(script_name) || script_name.find('/') != std::string_view::npos)
        throw std::runtime_error(
            "a bundle's script is a .oascript at its root, not " + std::string(script_name)
        );
    if (script_name == recording_name)
        throw std::runtime_error("a bundle's script and recording have different names");
    const std::array<zip::NewEntry, 2> entries{
        zip::NewEntry{
            script_name, {reinterpret_cast<const uint8_t*>(script_text.data()), script_text.size()}
        },
        zip::NewEntry{recording_name, recording_bytes},
    };
    std::vector<uint8_t> archive;
    zip::ZipError error{};
    if (!zip::write_archive(entries, archive, error))
        throw std::runtime_error("the bundle cannot be written: " + zip_message(error));
    return archive;
}

} // namespace oa::app
