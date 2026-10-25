// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The director render's files: their names, the ffmpeg arguments that
// encode, join and stitch the chunks, the frame manifests, the chunks' sound
// and the run manifest written with encoding off, into a directory named
// relative to the working directory too, the stills of frames, the encoder
// settings the environment gives, and the bundle's reader and writer,
// malformed bundles included.
#include "oa/app/director_output.hpp"

#include "oa/audio/offline_mix.hpp"
#include "oa/base/sha256.hpp"
#include "oa/formats/png.hpp"
#include "oa/formats/zip.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* expression, const char* file, int line) {
    if (!condition) {
        std::fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expression);
        ++failures;
    }
}

#define CHECK(condition) check((condition), #condition, __FILE__, __LINE__)

using namespace oa::app;
namespace fs = std::filesystem;
namespace sha256 = oa::base::sha256;
using oa::formats::oascript::Decimal;

/// The frame size of the test renders, pixels.
constexpr uint32_t kWidth = 4;
constexpr uint32_t kHeight = 2;
constexpr size_t kFrameBytes = size_t{kWidth} * kHeight * 3;

/// Returns the value that follows an option in some arguments.
///
/// @param arguments the arguments
/// @param option the option
/// @return its value, or "" when it has none
std::string after(const std::vector<std::string>& arguments, std::string_view option) {
    const auto found = std::find(arguments.begin(), arguments.end(), option);
    return found == arguments.end() || found + 1 == arguments.end() ? std::string() : *(found + 1);
}

/// Tells whether some arguments hold one.
///
/// @param arguments the arguments
/// @param argument the one looked for
/// @return true when it is there
bool holds(const std::vector<std::string>& arguments, std::string_view argument) {
    return std::find(arguments.begin(), arguments.end(), argument) != arguments.end();
}

/// Returns a new directory's path under the temporary directory.
///
/// @param name the directory's name, before a unique suffix
/// @return the path; the directory is not made
fs::path fresh_temporary(std::string_view name) {
    return fs::temp_directory_path() /
           (std::string(name) + "-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
}

/// Reads a whole file.
///
/// @param path the file
/// @return its bytes; empty when it cannot be read
std::vector<uint8_t> read_file(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

/// Returns text as bytes.
///
/// @param text the text
/// @return its bytes
std::vector<uint8_t> bytes_of(std::string_view text) {
    return {text.begin(), text.end()};
}

/// Sets or clears an environment variable.
///
/// @param name the variable
/// @param value its value; null clears it
void set_environment(const char* name, const char* value) {
#ifdef _WIN32
    (void)_putenv_s(name, value != nullptr ? value : "");
#else
    if (value != nullptr)
        (void)setenv(name, value, 1);
    else
        (void)unsetenv(name);
#endif
}

/// Returns the message a call throws as std::runtime_error, or "" when it
/// throws nothing.
///
/// @param call the call
/// @return the message
template <typename Call>
std::string runtime_error_of(Call call) {
    try {
        call();
    } catch (const std::runtime_error& error) {
        return error.what();
    }
    return {};
}

/// Tells whether a call throws std::logic_error.
///
/// @param call the call
/// @return true when it does
template <typename Call>
bool logic_error_of(Call call) {
    try {
        call();
    } catch (const std::logic_error&) {
        return true;
    }
    return false;
}

void test_paths() {
    const DirectorPaths paths{fs::path("out"), "game"};
    CHECK(chunk_stem(paths, 0) == "game-000");
    CHECK(chunk_stem(paths, 7) == "game-007");
    CHECK(chunk_stem(paths, 1234) == "game-1234");
    CHECK(chunk_video_path(paths, 1) == fs::path("out") / "game-001.mp4");
    CHECK(chunk_video_only_path(paths, 1) == fs::path("out") / "game-001.video.mp4");
    CHECK(chunk_audio_path(paths, 2) == fs::path("out") / "game-002.wav");
    CHECK(chunk_manifest_path(paths, 3) == fs::path("out") / "game-003.frames");
    CHECK(joined_audio_path(paths) == fs::path("out") / "game.wav");
    CHECK(joined_video_path(paths) == fs::path("out") / "game.mp4");
    CHECK(run_manifest_path(paths) == fs::path("out") / "game.manifest");
    CHECK(concat_list_path(paths) == fs::path("out") / "game.concat.txt");
    CHECK(still_path(paths, 0) == fs::path("out") / "game-still-000000.png");
    CHECK(still_path(paths, 120) == fs::path("out") / "game-still-000120.png");
    CHECK(still_path(paths, 1234567) == fs::path("out") / "game-still-1234567.png");
}

/// Ignores a warning or an error of the PNG reader; the test reads the
/// result instead.
void ignore_png_message(void*, const char*) {
}

// A still is the frame as drawn, an 8-bit RGB PNG that reads back to the
// same bytes, in a directory made for it; a frame of the wrong size is
// refused.
void test_stills() {
    const fs::path root{fresh_temporary("oa-director-stills")};
    const DirectorPaths paths{root / "out", "game"};
    std::vector<uint8_t> frame(kFrameBytes);
    for (size_t index = 0; index < frame.size(); ++index)
        frame[index] = static_cast<uint8_t>(index * 37 + 11);
    const fs::path written = write_still(paths, 42, frame, kWidth, kHeight);
    CHECK(written == still_path(paths, 42));
    const auto file = read_file(written);
    namespace png = oa::formats::png;
    const png::Messages messages{nullptr, ignore_png_message, ignore_png_message};
    png::Info info{};
    CHECK(png::has_signature(file) && png::read_info(file, messages, &info));
    CHECK(info.header.width == kWidth && info.header.height == kHeight);
    CHECK(info.header.bit_depth == 8 && info.header.color_type == png::ColorType::rgb);
    std::vector<uint8_t> rows(png::row_bytes(info.header, {}) * kHeight);
    CHECK(png::read_image(file, info, {}, messages, rows) == png::Progress::end);
    CHECK(rows == frame);
    CHECK(logic_error_of([&] {
        (void)write_still(paths, 43, std::span<const uint8_t>(frame).first(3), kWidth, kHeight);
    }));
    CHECK(!fs::exists(still_path(paths, 43)));
    std::error_code removed;
    fs::remove_all(root, removed);
}

// A script names a recording in its folder or below it by its path from
// there, and any other by its absolute path.
void test_recording_key() {
    const fs::path root{fresh_temporary("oa-director-key")};
    std::error_code error;
    fs::create_directories(root / "games" / "old", error);
    CHECK(!error);
    for (const fs::path& file : {root / "game.rec", root / "games" / "old" / "game.rec"})
        std::ofstream(file, std::ios::binary) << "recording";
    const fs::path canonical_root{fs::weakly_canonical(root)};
    const auto absolute{[&](const fs::path& path) {
        const std::u8string text{(canonical_root / path).lexically_normal().generic_u8string()};
        return std::string(text.begin(), text.end());
    }};
    CHECK(script_recording_key(root / "game.rec", root / "game.oascript") == "game.rec");
    CHECK(
        script_recording_key(root / "games" / "old" / "game.rec", root / "game.oascript") ==
        "games/old/game.rec"
    );
    CHECK(
        script_recording_key(root / "games" / ".." / "game.rec", root / "new" / "game.oascript") ==
        absolute("game.rec")
    );
    CHECK(
        script_recording_key(root / "game.rec", root / "games" / "old" / "game.oascript") ==
        absolute("game.rec")
    );
    CHECK(
        script_recording_key(root / "games" / "old" / "game.rec", root / "games" / "x.oascript") ==
        "old/game.rec"
    );
    fs::remove_all(root, error);
}

void test_encoder_arguments() {
    CHECK(half_second_frames(Decimal{60, 0}) == 30);
    CHECK(half_second_frames(Decimal{29970, 3}) == 14);
    CHECK(half_second_frames(Decimal{24, 0}) == 12);
    CHECK(half_second_frames(Decimal{1, 0}) == 1);

    const auto encode =
        chunk_encoder_arguments("game-000.video.mp4", 3840, 2160, Decimal{29970, 3}, "veryfast");
    CHECK(encode.front() == director_encoder);
    CHECK(after(encode, "-f") == "rawvideo" && after(encode, "-pix_fmt") == "rgb24");
    CHECK(after(encode, "-video_size") == "3840x2160");
    CHECK(after(encode, "-framerate") == "2997/100");
    CHECK(after(encode, "-i") == "-");
    CHECK(after(encode, "-c:v") == "libx264" && after(encode, "-preset") == "veryfast");
    CHECK(after(encode, "-crf") == "18" && after(encode, "-profile:v") == "high");
    CHECK(after(encode, "-colorspace") == "bt709" && after(encode, "-color_primaries") == "bt709");
    CHECK(after(encode, "-color_trc") == "bt709");
    CHECK(after(encode, "-g") == "14" && after(encode, "-flags") == "+cgop");
    CHECK(after(encode, "-movflags") == "+faststart");
    CHECK(encode.back() == "game-000.video.mp4");
    CHECK(
        after(chunk_encoder_arguments("a.mp4", 2, 2, Decimal{60, 0}, "medium"), "-framerate") ==
        "60/1"
    );

    const auto mux = chunk_mux_arguments("game-000.video.mp4", "game-000.wav", "game-000.mp4");
    CHECK(mux.front() == director_encoder && mux.back() == "game-000.mp4");
    CHECK(after(mux, "-c:v") == "copy" && after(mux, "-c:a") == "aac");
    CHECK(after(mux, "-b:a") == "384k" && after(mux, "-ar") == "48000");
    CHECK(after(mux, "-movflags") == "+faststart");
    // The sound holds exactly the chunk's samples: nothing is cut to the
    // shorter stream, which AAC's priming could make the frames.
    CHECK(!holds(mux, "-shortest") && !holds(mux, "-t"));

    const auto stitch = stitch_arguments("game.concat.txt", "game.wav", "game.mp4");
    CHECK(after(stitch, "-f") == "concat" && after(stitch, "-safe") == "0");
    CHECK(after(stitch, "-i") == "game.concat.txt" && holds(stitch, "game.wav"));
    CHECK(after(stitch, "-c:v") == "copy" && after(stitch, "-c:a") == "aac");
    CHECK(after(stitch, "-b:a") == "384k" && stitch.back() == "game.mp4");
    CHECK(!holds(stitch, "-shortest"));

    const std::vector<fs::path> files{"game-000.video.mp4", "it's-001.video.mp4"};
    CHECK(concat_list_text(files) == "file 'game-000.video.mp4'\nfile 'it'\\''s-001.video.mp4'\n");
}

void test_manifest_lines() {
    const auto digest = sha256::digest_of(bytes_of("abc"));
    CHECK(
        manifest_line(12, 6, digest) ==
        "12 6 ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\n"
    );
    CHECK(digest_text(digest).size() == sha256::hex_size);
}

void test_card_manifest() {
    RunDescription run{};
    run.engine_version = "1.2.3";
    run.script_name = "game.oascript";
    run.recording_name = "game.rec";
    run.width = kWidth;
    run.height = kHeight;
    run.framerate = Decimal{60, 0};
    run.tickrate = Decimal{30, 0};
    run.first_tick = 10;
    run.end_tick = 12;
    run.frame_count = 3;
    const auto digest = sha256::digest_of(bytes_of("pcm"));
    const std::string processor = run_manifest_text(run, {}, digest);
    CHECK(processor.find("drawn") == std::string::npos);
    // A render the graphics card drew says so after the output's line, and
    // is otherwise the same.
    run.graphics_card = "some-driver";
    const std::string card = run_manifest_text(run, {}, digest);
    const auto ticks = processor.find("ticks ");
    CHECK(ticks != std::string::npos);
    CHECK(
        card == processor.substr(0, ticks) +
                    "drawn graphics-card some-driver full-tier: frames differ from one driver to "
                    "the next and are not pinned\n" +
                    processor.substr(ticks)
    );
}

void test_environment() {
    set_environment(director_encoder_variable, nullptr);
    set_environment(director_preset_variable, nullptr);
    auto settings = encoder_settings_from_environment();
    CHECK(settings.enabled && settings.preset == director_default_preset);
    set_environment(director_encoder_variable, director_encoder_off);
    set_environment(director_preset_variable, "veryfast");
    settings = encoder_settings_from_environment();
    CHECK(!settings.enabled && settings.preset == "veryfast");
    set_environment(director_encoder_variable, "ffmpeg");
    CHECK(!runtime_error_of([] { (void)encoder_settings_from_environment(); }).empty());
    set_environment(director_encoder_variable, nullptr);
    set_environment(director_preset_variable, "very fast; rm");
    CHECK(!runtime_error_of([] { (void)encoder_settings_from_environment(); }).empty());
    set_environment(director_preset_variable, nullptr);
}

void test_missing_encoder() {
    const std::vector<std::string> arguments{"oa-no-such-encoder-program", "-version"};
    CHECK(runtime_error_of([&] {
              run_encoder(arguments);
          }).find("oa-no-such-encoder-program") != std::string::npos);
    CHECK(!runtime_error_of([&] { EncoderPipe pipe(arguments); }).empty());
}

/// Returns a test frame: every byte the frame's number plus its index.
///
/// @param frame the frame
/// @return its bytes
std::vector<uint8_t> test_frame(uint64_t frame) {
    std::vector<uint8_t> rgb(kFrameBytes);
    for (size_t index = 0; index < rgb.size(); ++index)
        rgb[index] = static_cast<uint8_t>(frame + index);
    return rgb;
}

void test_render_without_encoder() {
    const auto directory = fresh_temporary("oa-director-output");
    const DirectorPaths paths{directory / "render", "game"};
    EncoderSettings encoder{};
    encoder.enabled = false;
    const std::vector<int16_t> sound{0, -1, 256, -32768, 32767, 1};
    {
        DirectorOutput output({paths, kWidth, kHeight, Decimal{60, 0}, encoder, true});
        CHECK(fs::is_directory(paths.directory));
        CHECK(logic_error_of([&] { output.add_frame(0, 0, test_frame(0)); }));
        output.begin_chunk(0, 0);
        CHECK(logic_error_of([&] { output.begin_chunk(1, 2); }));
        output.add_frame(0, 10, test_frame(0));
        CHECK(logic_error_of([&] { output.add_frame(2, 11, test_frame(2)); }));
        CHECK(logic_error_of([&] { output.add_frame(1, 11, std::vector<uint8_t>(3)); }));
        CHECK(logic_error_of([&] { output.add_samples(std::vector<int16_t>(3)); }));
        output.add_samples({sound.data(), 4});
        output.add_frame(1, 10, test_frame(1));
        output.add_samples({sound.data() + 4, 2});
        const auto& first = output.end_chunk();
        CHECK(first.chunk == 0 && first.first_frame == 0 && first.frame_count == 2);
        CHECK(first.first_tick == 10 && first.last_tick == 10 && first.sample_frames == 3);
        CHECK(logic_error_of([&] { output.begin_chunk(0, 2); }));
        output.begin_chunk(1, 2);
        output.add_frame(2, 11, test_frame(2));
        output.add_samples({sound.data(), 2});
        CHECK(logic_error_of([&] { output.finish({}); }));
        (void)output.end_chunk();
        RunDescription run{};
        run.engine_version = "1.2.3";
        run.script_name = "game.oascript";
        run.recording_name = "game.rec";
        run.script_digest = sha256::digest_of(bytes_of("script"));
        run.recording_digest = sha256::digest_of(bytes_of("recording"));
        run.width = kWidth;
        run.height = kHeight;
        run.framerate = Decimal{60, 0};
        run.tickrate = Decimal{30, 0};
        run.first_tick = 10;
        run.end_tick = 12;
        run.frame_count = 3;
        run.chunk_count = 2;
        output.finish(run);
        CHECK(output.chunks().size() == 2);
    }

    // The frame manifests: a line a frame.
    const auto first_manifest = read_file(chunk_manifest_path(paths, 0));
    const std::string first_text(first_manifest.begin(), first_manifest.end());
    CHECK(
        first_text == manifest_line(0, 10, sha256::digest_of(test_frame(0))) +
                          manifest_line(1, 10, sha256::digest_of(test_frame(1)))
    );
    const auto second_manifest = read_file(chunk_manifest_path(paths, 1));
    CHECK(
        std::string(second_manifest.begin(), second_manifest.end()) ==
        manifest_line(2, 11, sha256::digest_of(test_frame(2)))
    );

    // Each chunk's sound: the WAVE header, then little-endian samples.
    const auto first_wave = read_file(chunk_audio_path(paths, 0));
    const auto header = oa::audio::offline_mix::wave_header(3);
    CHECK(first_wave.size() == header.size() + 12);
    CHECK(std::equal(header.begin(), header.end(), first_wave.begin()));
    const std::vector<uint8_t> first_pcm{
        0x00, 0x00, 0xff, 0xff, 0x00, 0x01, 0x00, 0x80, 0xff, 0x7f, 0x01, 0x00
    };
    CHECK(std::equal(first_pcm.begin(), first_pcm.end(), first_wave.begin() + header.size()));
    const auto second_wave = read_file(chunk_audio_path(paths, 1));
    CHECK(second_wave.size() == header.size() + 4);

    // With encoding off no video is made, and nothing kept for one stays.
    CHECK(!fs::exists(chunk_video_path(paths, 0)) && !fs::exists(joined_video_path(paths)));
    CHECK(!fs::exists(joined_audio_path(paths)) && !fs::exists(concat_list_path(paths)));

    // The run manifest names every chunk and the whole sound's hash.
    std::vector<uint8_t> all_pcm = first_pcm;
    all_pcm.insert(all_pcm.end(), {0x00, 0x00, 0xff, 0xff});
    const auto manifest = read_file(run_manifest_path(paths));
    const std::string manifest_text(manifest.begin(), manifest.end());
    const std::string expected =
        "open-annihilation director render\n"
        "engine 1.2.3\n"
        "script " +
        digest_text(sha256::digest_of(bytes_of("script"))) +
        " game.oascript\n"
        "recording " +
        digest_text(sha256::digest_of(bytes_of("recording"))) +
        " game.rec\n"
        "output 4x2 framerate 60 tickrate 30\n"
        "ticks 10 to 12 (exclusive)\n"
        "frames 3\n"
        "chunks 2\n"
        "chunk 0 frames 0-1 ticks 10-10 samples 3 frames-sha256 " +
        digest_text(sha256::digest_of(first_manifest)) + " pcm-sha256 " +
        digest_text(sha256::digest_of(first_pcm)) +
        "\n"
        "chunk 1 frames 2-2 ticks 11-11 samples 1 frames-sha256 " +
        digest_text(sha256::digest_of(second_manifest)) + " pcm-sha256 " +
        digest_text(sha256::digest_of(std::vector<uint8_t>{0x00, 0x00, 0xff, 0xff})) +
        "\n"
        "pcm-sha256 " +
        digest_text(sha256::digest_of(all_pcm)) + "\n";
    CHECK(manifest_text == expected);

    // A render of some chunks writes no run manifest.
    const DirectorPaths partial{directory / "partial", "game"};
    {
        DirectorOutput output({partial, kWidth, kHeight, Decimal{60, 0}, encoder, false});
        output.begin_chunk(3, 90);
        output.add_frame(90, 45, test_frame(90));
        (void)output.end_chunk();
        output.finish({});
    }
    CHECK(fs::exists(chunk_manifest_path(partial, 3)) && fs::exists(chunk_audio_path(partial, 3)));
    CHECK(!fs::exists(run_manifest_path(partial)));
    CHECK(
        read_file(chunk_audio_path(partial, 3)).size() == oa::audio::offline_mix::wave_header_bytes
    );

    // Odd sizes are refused.
    CHECK(!runtime_error_of([&] {
               DirectorOutput output({partial, 3, 2, Decimal{60, 0}, encoder, false});
           }).empty());
    std::error_code error;
    fs::remove_all(directory, error);
}

// Paths named relative to the working directory, without a folder, as
// `--generate-script game.rec --output game.oascript` and `--render-script
// game.oascript --output 4k` name them: the script names its recording by
// its file name, and the render makes its directory there and writes every
// file into it.
void test_relative_paths() {
    const fs::path root{fresh_temporary("oa-director-relative")};
    std::error_code error;
    fs::create_directories(root, error);
    CHECK(!error);
    std::ofstream(root / "game.rec", std::ios::binary) << "recording";
    const fs::path working{fs::current_path()};
    fs::current_path(root);
    try {
        CHECK(script_recording_key("game.rec", "game.oascript") == "game.rec");
        CHECK(script_recording_key("game.rec", fs::path("4k") / "game.oascript") != "game.rec");
        const DirectorPaths paths{fs::path("4k"), "game"};
        EncoderSettings encoder{};
        encoder.enabled = false;
        {
            DirectorOutput output({paths, kWidth, kHeight, Decimal{60, 0}, encoder, true});
            output.begin_chunk(0, 0);
            output.add_frame(0, 10, test_frame(0));
            (void)output.end_chunk();
            RunDescription run{};
            run.script_name = "game.oascript";
            run.recording_name = "game.rec";
            run.width = kWidth;
            run.height = kHeight;
            run.framerate = Decimal{60, 0};
            run.tickrate = Decimal{30, 0};
            run.first_tick = 10;
            run.end_tick = 11;
            run.frame_count = 1;
            run.chunk_count = 1;
            output.finish(run);
        }
    } catch (const std::exception& caught) {
        std::fprintf(stderr, "relative paths: %s\n", caught.what());
        CHECK(false);
    }
    fs::current_path(working);
    for (const char* name : {"game-000.frames", "game-000.wav", "game.manifest"})
        CHECK(fs::is_regular_file(root / "4k" / name));
    fs::remove_all(root, error);
}

void test_bundles() {
    const auto script = std::string("oascript: 1\n");
    const auto recording = bytes_of("recorded game");
    const auto bundle_bytes = write_bundle("game.oascript", script, "game.rec", recording);
    CHECK(bundle_bytes == write_bundle("game.oascript", script, "game.rec", recording));
    const auto bundle = read_bundle(bundle_bytes);
    CHECK(bundle.script_name == "game.oascript");
    CHECK(bundle.script_bytes == bytes_of(script));
    CHECK(read_bundle_entry(bundle_bytes, bundle, "game.rec") == recording);
    CHECK(!runtime_error_of([&] {
               (void)read_bundle_entry(bundle_bytes, bundle, "other.rec");
           }).empty());
    CHECK(!runtime_error_of([&] {
               (void)read_bundle_entry(bundle_bytes, bundle, "../game.rec");
           }).empty());
    CHECK(!runtime_error_of([&] {
               (void)read_bundle_entry(bundle_bytes, bundle, "C:\\game.rec");
           }).empty());

    // The writer refuses what the reader would not read.
    CHECK(!runtime_error_of([&] {
               (void)write_bundle("game.txt", script, "game.rec", recording);
           }).empty());
    CHECK(!runtime_error_of([&] {
               (void)write_bundle("a/game.oascript", script, "game.rec", recording);
           }).empty());
    CHECK(!runtime_error_of([&] {
               (void)write_bundle("game.oascript", script, "/game.rec", recording);
           }).empty());
    CHECK(!runtime_error_of([&] {
               (void)write_bundle("game.oascript", script, "game.oascript", recording);
           }).empty());

    // A bundle holds exactly one script at its root; one in a folder is not it.
    namespace zip = oa::formats::zip;
    const auto archive_of =
        [](std::initializer_list<std::pair<std::string_view, std::string_view>> entries) {
            std::vector<zip::NewEntry> list;
            for (const auto& [name, text] : entries)
                list.push_back(
                    {name, {reinterpret_cast<const uint8_t*>(text.data()), text.size()}}
                );
            std::vector<uint8_t> archive;
            zip::ZipError error{};
            CHECK(zip::write_archive(list, archive, error));
            return archive;
        };
    const auto two = archive_of({{"a.oascript", "a"}, {"B.OASCRIPT", "b"}, {"game.rec", "r"}});
    CHECK(runtime_error_of([&] {
              (void)read_bundle(two);
          }).find("this one 2") != std::string::npos);
    const auto none = archive_of({{"scripts/a.oascript", "a"}, {"game.rec", "r"}});
    CHECK(runtime_error_of([&] {
              (void)read_bundle(none);
          }).find("this one 0") != std::string::npos);
    const auto upper = archive_of({{"GAME.OASCRIPT", "text"}, {"game.rec", "r"}});
    CHECK(read_bundle(upper).script_name == "GAME.OASCRIPT");

    // Malformed bundles: every truncation and a flipped byte in every
    // position are refused or read, never more.
    for (size_t length = 0; length < bundle_bytes.size(); ++length) {
        const std::span<const uint8_t> truncated{bundle_bytes.data(), length};
        CHECK(!runtime_error_of([&] { (void)read_bundle(truncated); }).empty());
    }
    for (size_t index = 0; index < bundle_bytes.size(); ++index) {
        auto flipped = bundle_bytes;
        flipped[index] = static_cast<uint8_t>(flipped[index] ^ 0x5a);
        try {
            const auto read = read_bundle(flipped);
            (void)read_bundle_entry(flipped, read, "game.rec");
        } catch (const std::runtime_error&) {
        }
    }
    CHECK(!runtime_error_of([] {
               (void)read_bundle(bytes_of("not a zip archive at all"));
           }).empty());
}

} // namespace

int main() {
    test_paths();
    test_stills();
    test_recording_key();
    test_encoder_arguments();
    test_manifest_lines();
    test_card_manifest();
    test_environment();
    test_missing_encoder();
    test_render_without_encoder();
    test_relative_paths();
    test_bundles();
    if (failures != 0) {
        std::fprintf(stderr, "%d director output checks failed\n", failures);
        return 1;
    }
    std::printf("director output: all checks passed\n");
    return 0;
}
