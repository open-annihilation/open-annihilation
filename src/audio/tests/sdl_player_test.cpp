// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The WAV player's one stream on SDL's dummy device, which plays in real
// time: it waits out its delay, plays once, stops at once, and a new stream
// takes the place of the one playing; stop_all, which silences every
// effect, the loop and the stream at once; and the effects' voice policy.
// On a mixer the test runs by hand, a placed start weighs its two sides by
// the placement's levels, and a held loop is silent until the hold ends.
#include "audio_test_support.hpp"
#include "oa/audio/sdl_audio.hpp"
#include "oa/audio/software_mixer.hpp"
#include "oa/audio/sound_output.hpp"
#include "oa/audio/spatial_gain.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/test/scratch_directory.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using oa::audio::game_audio::SdlWavPlayer;
using audio_test::require;

namespace {

constexpr uint32_t kRate = 22050;
constexpr uint32_t kHeaderBytes = 8; // a RIFF chunk's tag and size

// Writes a 16-bit mono square wave of `milliseconds`.
void write_tone(const std::filesystem::path& path, uint32_t milliseconds) {
    std::vector<uint8_t> samples;
    for (uint32_t i = 0; i < kRate * milliseconds / 1000; ++i)
        audio_test::put16(samples, static_cast<uint16_t>((i % 50) < 25 ? 3000 : -3000));
    auto riff = audio_test::make_riff(kRate, 16, 1, samples);
    const auto riff_size = static_cast<uint32_t>(riff.size()) - kHeaderBytes;
    for (uint32_t byte = 0; byte < 4; ++byte)
        riff[4 + byte] = static_cast<uint8_t>(riff_size >> (8 * byte));
    std::ofstream(path, std::ios::binary)
        .write(
            reinterpret_cast<const char*>(riff.data()), static_cast<std::streamsize>(riff.size())
        );
}

// Writes an 8-bit mono WAV of `count` samples at `rate`.
void write_slow(const std::filesystem::path& path, uint32_t rate, uint32_t count) {
    auto riff = audio_test::make_riff(rate, 8, 1, std::vector<uint8_t>(count, 0x90));
    const auto riff_size = static_cast<uint32_t>(riff.size()) - kHeaderBytes;
    for (uint32_t byte = 0; byte < 4; ++byte)
        riff[4 + byte] = static_cast<uint8_t>(riff_size >> (8 * byte));
    std::ofstream(path, std::ios::binary)
        .write(
            reinterpret_cast<const char*>(riff.data()), static_cast<std::streamsize>(riff.size())
        );
}

// A sound output whose mixer the test runs by hand.
class HandMixedOutput final : public oa::audio::SoundOutput {
  public:

    bool start(std::string&) override { return true; }

    void stop() override {}

    bool started() const override { return true; }

    std::unique_ptr<oa::audio::OutputStream> open_stream(
        const oa::audio::StreamFormat& format,
        oa::audio::StreamFeed feed,
        void* context,
        std::string& error
    ) override {
        return mixer.open_stream(format, feed, context, error);
    }

    std::string driver_name() const override { return "by hand"; }

    std::string last_error() const override { return ""; }

    // Mixes the next frames.
    std::vector<int16_t> mix(uint32_t frames) {
        std::vector<int16_t> out(static_cast<std::size_t>(frames) * 2);
        mixer.mix(out.data(), frames);
        return out;
    }

    oa::audio::SoftwareMixer mixer{oa::audio::MixerLock{}};
};

// A placed start plays each side at the placement's level: against the same
// start unplaced, within a step of 16 bits for the rounding of the gains.
void placed_sides(const std::filesystem::path& root) {
    HandMixedOutput output;
    oa::audio::set_sound_output(&output);
    {
        const oa::AssetStore assets(root);
        SdlWavPlayer player(assets);
        std::string error;
        constexpr uint32_t frames = 4096;
        require(player.play_resource("sounds/short.wav", error), "an unplaced start plays");
        const auto centred = output.mix(frames);
        player.stop_all();
        oa::audio::Spatial placed{};
        placed.mode = oa::audio::SpatialMode::normal;
        placed.x = 300.0F;
        placed.z = 200.0F;
        placed.min_distance = 100.0F;
        placed.max_distance = 2000.0F;
        const auto sides = oa::audio::spatial_stereo_gain(placed);
        require(sides.left < sides.right - 0.1F, "the placement weighs the sides apart");
        require(
            player.play_placed("sounds/short.wav", oa::audio::volume_near, placed, error),
            "a placed start plays"
        );
        const auto weighed = output.mix(frames);
        player.stop_all();
        bool sounding = false;
        for (std::size_t i = 0; i < centred.size(); i += 2) {
            sounding = sounding || centred[i] != 0;
            const auto left = static_cast<int32_t>(std::lround(centred[i] * sides.left));
            const auto right = static_cast<int32_t>(std::lround(centred[i + 1] * sides.right));
            require(std::abs(weighed[i] - left) <= 1, "the left side plays at its level");
            require(std::abs(weighed[i + 1] - right) <= 1, "the right side plays at its level");
        }
        require(sounding, "the start sounds");
    }
    oa::audio::set_sound_output(nullptr);
}

// A held loop is silent, and once the hold ends it plays on from where it
// stopped; a loop started while the hold lasts starts silent.
void held_loop(const std::filesystem::path& root) {
    HandMixedOutput output;
    oa::audio::set_sound_output(&output);
    {
        const oa::AssetStore assets(root);
        SdlWavPlayer player(assets);
        std::string error;
        constexpr uint32_t frames = 256;
        const auto sounds = [](const std::vector<int16_t>& mix) {
            return std::any_of(mix.begin(), mix.end(), [](int16_t sample) { return sample != 0; });
        };
        require(player.start_loop_resource("sounds/short.wav", error), "a loop starts");
        const auto first = output.mix(frames);
        const auto following = output.mix(frames);
        require(sounds(first) && sounds(following), "the loop sounds");
        // The loop playing from its start again does not pass for it playing on.
        require(first != following, "the loop's next frames differ from its first");

        require(player.start_loop_resource("sounds/short.wav", error), "the loop starts again");
        require(output.mix(frames) == first, "a loop starts from its beginning");
        player.hold_loop(true);
        require(!sounds(output.mix(frames)), "a held loop is silent");
        player.hold_loop(true);
        require(!sounds(output.mix(frames)), "holding a held loop keeps it silent");
        player.hold_loop(false);
        require(output.mix(frames) == following, "a loop plays on from where the hold stopped it");

        player.hold_loop(true);
        player.stop_loop();
        require(
            player.start_loop_resource("sounds/short.wav", error),
            "a loop starts while the hold lasts"
        );
        require(!sounds(output.mix(frames)), "a loop started while held is silent");
        player.hold_loop(false);
        require(output.mix(frames) == first, "the hold ends and the loop plays from its beginning");
        player.stop_all();
    }
    oa::audio::set_sound_output(nullptr);
}

// Waits until `done` holds or `timeout_ms` passes.
template <typename Done>
bool wait_for(Done done, uint64_t timeout_ms) {
    const uint64_t start = SDL_GetTicks();
    while (SDL_GetTicks() - start < timeout_ms) {
        if (done())
            return true;
        SDL_Delay(5);
    }
    return done();
}

} // namespace

int main() {
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    require(SDL_Init(SDL_INIT_AUDIO), "SDL audio init");
    const auto root = oa::test::make_scratch_directory("oa-sdl-player-test");
    std::filesystem::create_directories(root / "sounds");
    write_tone(root / "sounds" / "long.wav", 5000);
    write_tone(root / "sounds" / "short.wav", 100);
    for (int other = 0; other < 6; ++other)
        write_tone(root / "sounds" / ("other" + std::to_string(other) + ".wav"), 5000);
    // At 1000 Hz, 200,000 samples convert to 17.6 MB, past the cache's
    // 8 MiB, and 400,000 to 35.3 MB, past a stream's 32 MiB; an 11025 Hz
    // effect of a second converts to 88 KB.
    write_slow(root / "sounds" / "slow.wav", 1000, 200000);
    write_slow(root / "sounds" / "slower.wav", 1000, 400000);
    write_slow(root / "sounds" / "effect.wav", 11025, 11025);
    {
        const oa::AssetStore assets(root);
        SdlWavPlayer player(assets);
        std::string error;
        require(!player.stream_busy(), "no stream plays before one starts");

        require(player.play_stream("sounds/long.wav", 0, error), "a stream starts");
        require(player.stream_busy(), "a started stream plays");
        player.stop_stream();
        require(!player.stream_busy(), "a stopped stream is silent at once");

        require(player.play_stream("sounds/short.wav", 0, error), "a short stream starts");
        require(
            wait_for([&] { return !player.stream_busy(); }, 3000), "a stream plays once and ends"
        );

        // The delay is far longer than the test could stall, so the stream is
        // still waiting it out at both checks, however slowly the test runs.
        require(player.play_stream("sounds/short.wav", 60000, error), "a delayed stream starts");
        require(player.stream_busy(), "a stream waiting out its delay is busy");
        SDL_Delay(50);
        require(player.stream_busy(), "a stream is still busy while its delay lasts");
        player.stop_stream();
        require(!player.stream_busy(), "a stream stopped during its delay never plays");

        // The long sound is replaced: the short one plays out in its place.
        require(player.play_stream("sounds/long.wav", 0, error), "the long stream starts");
        require(player.play_stream("sounds/short.wav", 0, error), "the short stream replaces it");
        require(
            wait_for([&] { return !player.stream_busy(); }, 3000),
            "a new stream takes the place of the one playing"
        );

        // The stream is not the looping sound.
        require(player.play_stream("sounds/long.wav", 0, error), "the stream starts again");
        player.stop_loop();
        require(player.stream_busy(), "stopping the looping sound leaves the stream playing");
        player.collect_finished();
        require(player.stream_busy(), "a playing stream is not collected");
        player.stop_stream();

        require(
            !player.play_stream("sounds/missing.wav", 0, error) && !error.empty(),
            "a missing sound starts no stream and says why"
        );
        require(!player.stream_busy(), "a failed stream is not busy");

        // stop_all silences an effect, the loop and the stream together.
        require(!player.playing(), "nothing plays before the effects start");
        require(player.play_resource("sounds/long.wav", error), "an effect starts");
        require(player.playing(), "a started effect plays");
        player.stop_all();
        require(!player.playing(), "stop_all ends a playing effect");
        require(player.start_loop_resource("sounds/short.wav", error), "a loop starts");
        require(player.play_stream("sounds/long.wav", 0, error), "a stream starts beside it");
        require(player.play_resource("sounds/long.wav", error), "an effect starts beside them");
        require(player.playing() && player.stream_busy(), "the effect, loop and stream play");
        player.stop_all();
        require(!player.playing(), "stop_all ends the effect and the loop");
        require(!player.stream_busy(), "stop_all ends the stream");
        player.stop_all();
        require(!player.playing(), "stop_all with nothing playing changes nothing");
        // The player plays on afterwards; the sounds outlast the checks.
        require(player.play_resource("sounds/long.wav", error), "an effect starts after stop_all");
        require(player.playing(), "an effect after stop_all plays");
        require(player.play_stream("sounds/long.wav", 0, error), "a stream starts after stop_all");
        require(player.stream_busy(), "a stream after stop_all plays");
        player.stop_all();
        require(player.effect_voices() == 0, "stop_all frees every voice");

        // One sound plays on four voices at most; a fifth start restarts one,
        // which then holds two voices.
        for (int start = 1; start <= 4; ++start) {
            require(player.play_resource("sounds/long.wav", error), "a sound starts again");
            require(player.effect_voices() == start, "each start of a sound takes a voice");
        }
        require(player.play_resource("sounds/long.wav", error), "a fifth start restarts one");
        require(player.effect_voices() == 5, "a restarted sound holds two voices");
        // Eight voices at most: further sounds stop the oldest voices.
        for (int other = 0; other < 6; ++other)
            require(
                player.play_resource("sounds/other" + std::to_string(other) + ".wav", error),
                "another sound starts"
            );
        require(player.effect_voices() == 8, "no more than eight voices play");
        require(player.playing(), "the newest sounds play on");
        player.stop_all();
        require(player.effect_voices() == 0 && !player.playing(), "stop_all ends them all");

        // A sound that converts to more than the cache holds is refused
        // before it is converted, as an effect and as a loop; as a stream it
        // plays up to a stream's own bound.
        error.clear();
        require(
            !player.play_resource("sounds/slow.wav", error) &&
                error.find("too long") != std::string::npos,
            "an effect past the cache is refused"
        );
        require(
            !player.start_loop_resource("sounds/slow.wav", error),
            "a loop past the cache is refused"
        );
        require(player.play_stream("sounds/slow.wav", 0, error), "a stream past the cache plays");
        player.stop_stream();
        require(
            !player.play_stream("sounds/slower.wav", 0, error), "a stream past its bound is refused"
        );
        require(player.play_resource("sounds/effect.wav", error), "an 11025 Hz effect plays");
        player.stop_all();
    }
    placed_sides(root);
    held_loop(root);
    std::filesystem::remove_all(root);
    SDL_Quit();
    return 0;
}
