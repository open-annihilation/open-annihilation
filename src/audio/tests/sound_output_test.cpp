// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The integer resampler, the software mixer and the buffered output on a
// device the test plays by hand; with --wave-out on Windows, the wave-out
// mixer on the system's device.

#include "oa/audio/buffered_output.hpp"
#include "oa/base/threads.hpp"
#include "oa/audio/software_mixer.hpp"
#include "oa/audio/sound_output.hpp"
#include "oa/audio/sound_output_backends.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace oa::audio;

namespace {

[[noreturn]] void fail(const char* file, int line, const char* expression) {
    std::fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expression);
    std::exit(1);
}

#define CHECK(expression)                                                                          \
    do {                                                                                           \
        if (!(expression))                                                                         \
            fail(__FILE__, __LINE__, #expression);                                                 \
    } while (false)

constexpr int skipped = 77;

std::recursive_mutex mixer_mutex;
int lock_depth = 0;

MixerLock test_lock() {
    MixerLock lock{};
    lock.lock = [](void*) {
        mixer_mutex.lock();
        ++lock_depth;
    };
    lock.unlock = [](void*) {
        --lock_depth;
        mixer_mutex.unlock();
    };
    return lock;
}

template <typename Sample>
std::vector<uint8_t> bytes_of(const std::vector<Sample>& samples) {
    std::vector<uint8_t> bytes(samples.size() * sizeof(Sample));
    std::memcpy(bytes.data(), samples.data(), bytes.size());
    return bytes;
}

std::vector<int16_t> mix(SoftwareMixer& mixer, uint32_t frames) {
    std::vector<int16_t> out(static_cast<std::size_t>(frames) * mixer_output_channels, 123);
    mixer.mix(out.data(), frames);
    return out;
}

void output_choice() {
    // Windows before Vista plays through the wave-out mixer first, every
    // other system through SDL's; the environment variable chooses either.
    CHECK(choose_sound_output(std::nullopt, false) == SoundOutputKind::sdl);
    CHECK(choose_sound_output(std::nullopt, true) == SoundOutputKind::wave_out);
    CHECK(choose_sound_output("waveout", false) == SoundOutputKind::wave_out);
    CHECK(choose_sound_output("sdl", true) == SoundOutputKind::sdl);
    CHECK(choose_sound_output("other", true) == SoundOutputKind::wave_out);
    CHECK(choose_sound_output("", false) == SoundOutputKind::sdl);
    CHECK(std::string_view(sound_output_variable) == "OA_SOUND_OUTPUT");
}

void formats() {
    CHECK(sample_bytes(SampleFormat::u8) == 1 && sample_bytes(SampleFormat::s16) == 2);
    CHECK(sample_bytes(SampleFormat::s32) == 4 && sample_bytes(SampleFormat::f32) == 4);
    CHECK(frame_bytes(StreamFormat{SampleFormat::s16, 2, 44100}) == 4);
    CHECK(silence_byte(SampleFormat::u8) == 0x80 && silence_byte(SampleFormat::s16) == 0);
}

// A tone of amplitude 8192 at a frequency, sampled at a frame of a rate.
double tone(double frequency, double frame, double rate) {
    constexpr double amplitude = 8192.0;
    return amplitude * std::sin(2.0 * std::numbers::pi * frequency * frame / rate);
}

void pcm_resampler_limits() {
    PcmResampler resampler;
    CHECK(!resampler.configure(resampler_min_rate - 1, 44100, 2));
    CHECK(!resampler.configure(44100, resampler_max_rate + 1, 2));
    CHECK(!resampler.configure(44100, 48000, 0));
    CHECK(!resampler.configure(44100, 48000, resampler_max_channels + 1));
    // Equal rates copy, owing nothing.
    CHECK(resampler.configure(44100, 44100, 2) && resampler.passthrough());
    const std::vector<int16_t> input{16384, -32768, 7, 32767};
    std::vector<int32_t> output;
    resampler.process(input, output);
    CHECK(resampler.owed_frames() == 0);
    resampler.finish(output);
    CHECK(output == std::vector<int32_t>(input.begin(), input.end()));
}

void pcm_resampler_lengths_and_chunks() {
    // ceil(frames * out / in) frames, whatever the split of the input; the
    // frames still owed shrink to none as the input ends.
    const uint32_t rates[][2] = {
        {11025, 44100},
        {22050, 44100},
        {22254, 44100},
        {48000, 44100},
        {8000, 44100},
        {96000, 44100},
        {44101, 44100},
    };
    for (const auto& rate : rates) {
        for (const uint32_t frames : {1U, 5U, 999U, 4096U}) {
            std::vector<int16_t> input(frames * 2);
            for (std::size_t i = 0; i < input.size(); ++i)
                input[i] = static_cast<int16_t>(20000.0 * std::sin(0.01 * static_cast<double>(i)));
            PcmResampler whole;
            CHECK(whole.configure(rate[0], rate[1], 2));
            std::vector<int32_t> once;
            whole.process(input, once);
            const uint64_t owed = (uint64_t{frames} * rate[1] + rate[0] - 1) / rate[0];
            CHECK(once.size() / 2 + whole.owed_frames() == owed);
            whole.finish(once);
            CHECK(once.size() == owed * 2 && whole.owed_frames() == 0);
            PcmResampler pieces;
            CHECK(pieces.configure(rate[0], rate[1], 2));
            std::vector<int32_t> split;
            std::size_t at = 0;
            for (std::size_t step = 1; at < frames; step = step * 3 + 1) {
                const std::size_t take = std::min<std::size_t>(step, frames - at);
                pieces.process(std::span<const int16_t>(input).subspan(at * 2, take * 2), split);
                at += take;
            }
            pieces.finish(split);
            CHECK(split == once);
        }
    }
}

void pcm_resampler_quality() {
    // A constant passes unchanged away from the ends, where the silence
    // around the input comes in.
    PcmResampler resampler;
    CHECK(resampler.configure(11025, 44100, 1));
    for (const int16_t level : {int16_t{8192}, int16_t{-32768}, int16_t{32767}}) {
        resampler.reset();
        const std::vector<int16_t> constant(500, level);
        std::vector<int32_t> output;
        resampler.process(constant, output);
        resampler.finish(output);
        CHECK(output.size() == 2000);
        for (std::size_t frame = 64; frame + 64 < output.size(); ++frame)
            CHECK(output[frame] == level);
    }

    // A tone well inside the band comes out within the error of 16-bit
    // samples and 14-bit taps, from every rate the game's sounds take.
    for (const uint32_t rate : {11025U, 22050U, 22254U, 32000U, 48000U}) {
        std::vector<int16_t> input(rate / 2);
        for (std::size_t i = 0; i < input.size(); ++i)
            input[i] =
                static_cast<int16_t>(std::lround(tone(1000.0, static_cast<double>(i), rate)));
        CHECK(resampler.configure(rate, 44100, 1));
        std::vector<int32_t> output;
        resampler.process(input, output);
        resampler.finish(output);
        double signal = 0.0;
        double error = 0.0;
        for (std::size_t frame = 64; frame + 64 < output.size(); ++frame) {
            const double want = tone(1000.0, static_cast<double>(frame), 44100.0);
            signal += want * want;
            error += (output[frame] - want) * (output[frame] - want);
        }
        CHECK(10.0 * std::log10(signal / error) > 70.0);
    }

    // No delay: an impulse at the first frame peaks at the first output
    // frame, the kernel's centre.
    CHECK(resampler.configure(11025, 44100, 1));
    std::vector<int16_t> impulse(64, 0);
    impulse[0] = 16384;
    std::vector<int32_t> output;
    resampler.process(impulse, output);
    resampler.finish(output);
    CHECK(output.size() == 256 && output[0] > 15000);
    for (std::size_t frame = 1; frame < output.size(); ++frame)
        CHECK(std::abs(output[frame]) < output[0]);

    // The overshoot of a band-limited full-scale square wave is kept past
    // 16 bits.
    std::vector<int16_t> square(400);
    for (std::size_t i = 0; i < square.size(); ++i)
        square[i] = (i / 8) % 2 == 0 ? int16_t{32767} : int16_t{-32768};
    output.clear();
    resampler.process(square, output);
    resampler.finish(output);
    CHECK(*std::max_element(output.begin(), output.end()) > 32767);
    CHECK(*std::min_element(output.begin(), output.end()) < -32768);
}

void conversion_and_gain() {
    SoftwareMixer mixer(test_lock());
    std::string error;
    CHECK(mixer.open_stream({SampleFormat::s16, 0, 44100}, nullptr, nullptr, error) == nullptr);
    CHECK(mixer.open_stream({SampleFormat::s16, 1, 10}, nullptr, nullptr, error) == nullptr);

    // A stream opens paused and adds nothing.
    auto mono = mixer.open_stream({SampleFormat::s16, 1, 44100}, nullptr, nullptr, error);
    CHECK(mono != nullptr && mixer.stream_count() == 1);
    const auto samples = bytes_of(std::vector<int16_t>{16384, -16384, 32767, -32768});
    CHECK(mono->put(samples.data(), static_cast<int32_t>(samples.size())));
    CHECK(mono->queued_bytes() == 8);
    for (const int16_t sample : mix(mixer, 4))
        CHECK(sample == 0);

    // Mono plays on both sides; at a gain of one, the samples are unchanged.
    CHECK(mono->resume());
    auto out = mix(mixer, 4);
    const std::vector<int16_t> both{16384, 16384, -16384, -16384, 32767, 32767, -32768, -32768};
    CHECK(out == both);
    CHECK(mono->queued_bytes() == 0 && mono->available_bytes() == 0);
    // Nothing left: silence, not the last samples again.
    for (const int16_t sample : mix(mixer, 4))
        CHECK(sample == 0);

    // Each format becomes 16-bit; the gain multiplies.
    struct Case {
        SampleFormat sample;
        std::vector<uint8_t> frame; // one stereo frame
        int16_t left;
        int16_t right;
    };

    float quarter = 0.25F;
    std::vector<uint8_t> float_frame(8);
    std::memcpy(float_frame.data(), &quarter, 4);
    quarter = -1.5F;
    std::memcpy(float_frame.data() + 4, &quarter, 4);
    const Case cases[] = {
        {SampleFormat::u8, {0xc0, 0x40}, 16384, -16384},
        {SampleFormat::s16, {0x00, 0x20, 0x00, 0xe0}, 8192, -8192},
        {SampleFormat::s32, {0, 0, 0, 0x40, 0, 0, 0, 0xc0}, 16384, -16384},
        {SampleFormat::f32, float_frame, 8192, -32768}, // -1.5 is held to -32768
    };
    for (const auto& c : cases) {
        auto stream = mixer.open_stream({c.sample, 2, 44100}, nullptr, nullptr, error);
        CHECK(
            stream != nullptr && stream->put(c.frame.data(), static_cast<int32_t>(c.frame.size()))
        );
        CHECK(stream->resume());
        out = mix(mixer, 1);
        CHECK(out[0] == c.left && out[1] == c.right);
    }
    auto stereo = mixer.open_stream({SampleFormat::s16, 2, 44100}, nullptr, nullptr, error);
    const auto pair = bytes_of(std::vector<int16_t>{16384, 16384});
    CHECK(stereo->set_gain(0.5F) && stereo->put(pair.data(), 4) && stereo->resume());
    out = mix(mixer, 1);
    CHECK(out[0] == 8192 && out[1] == 8192);

    // The sides take gains of their own on top of the stream's: one
    // channel plays on both at those levels, and two each at its own.
    CHECK(mono->put(samples.data(), 4) && mono->set_side_gains(0.5F, 0.25F));
    out = mix(mixer, 2);
    CHECK(out == std::vector<int16_t>({8192, 4096, -8192, -4096}));
    CHECK(stereo->set_side_gains(0.25F, 2.0F) && stereo->put(pair.data(), 4));
    out = mix(mixer, 1);
    CHECK(out[0] == 2048 && out[1] == 16384); // at the stream's gain of 0.5
    CHECK(stereo->set_side_gains(1.0F, 1.0F));
    // A float that is not a number is silence.
    const float not_a_number = std::numeric_limits<float>::quiet_NaN();
    std::vector<uint8_t> nan_frame(8);
    std::memcpy(nan_frame.data(), &not_a_number, 4);
    std::memcpy(nan_frame.data() + 4, &not_a_number, 4);
    auto floats = mixer.open_stream({SampleFormat::f32, 2, 44100}, nullptr, nullptr, error);
    CHECK(floats->put(nan_frame.data(), 8) && floats->resume());
    out = mix(mixer, 1);
    CHECK(out[0] == 0 && out[1] == 0);
    floats.reset();

    // Two streams add up, clamped.
    auto other = mixer.open_stream({SampleFormat::s16, 2, 44100}, nullptr, nullptr, error);
    CHECK(stereo->set_gain(1.0F) && stereo->put(pair.data(), 4));
    const auto loud = bytes_of(std::vector<int16_t>{24576, -8192});
    CHECK(other->put(loud.data(), 4) && other->resume());
    out = mix(mixer, 1);
    CHECK(out[0] == 32767 && out[1] == 8192);

    // Pause keeps the samples; clear drops them; destroying unregisters.
    CHECK(other->put(loud.data(), 4) && other->pause());
    out = mix(mixer, 1);
    CHECK(out[0] == 0 && out[1] == 0 && other->queued_bytes() == 4);
    other->clear();
    CHECK(other->resume() && other->queued_bytes() == 0);
    out = mix(mixer, 1);
    CHECK(out[0] == 0);
    const std::size_t open = mixer.stream_count();
    other.reset();
    CHECK(mixer.stream_count() == open - 1);
    CHECK(lock_depth == 0);
}

struct Counter {
    int16_t next{};
    int calls{};
    int32_t last_wanted{};
    bool locked_during_feed{};
};

void feed_counter(void* context, OutputStream& stream, int32_t wanted) {
    auto& counter = *static_cast<Counter*>(context);
    ++counter.calls;
    counter.last_wanted = wanted;
    counter.locked_during_feed = lock_depth > 0;
    std::vector<int16_t> samples(static_cast<std::size_t>(wanted) / 2);
    for (auto& sample : samples)
        sample = counter.next++;
    (void)stream.put(samples.data(), static_cast<int32_t>(samples.size() * 2));
}

void feeds() {
    // A mono stream at the output rate, fed as it plays: every frame once,
    // in order, across mixes.
    SoftwareMixer mixer(test_lock());
    Counter counter;
    std::string error;
    auto stream = mixer.open_stream({SampleFormat::s16, 1, 44100}, feed_counter, &counter, error);
    CHECK(stream->resume());
    int16_t expected = 0;
    for (int round = 0; round < 5; ++round) {
        const auto out = mix(mixer, 300);
        for (std::size_t frame = 0; frame < 300; ++frame) {
            CHECK(out[2 * frame] == expected && out[2 * frame + 1] == expected);
            ++expected;
        }
    }
    CHECK(counter.calls >= 1 && counter.last_wanted > 0 && counter.locked_during_feed);

    // A resampled stream: a flushed input of n frames at 22050 Hz plays as
    // 2n frames at 44100, then stops; it reports frames to play until the
    // last has played.
    auto slow = mixer.open_stream({SampleFormat::s16, 2, 22050}, nullptr, nullptr, error);
    stream.reset();
    std::vector<int16_t> constant(2 * 1000, 8192);
    const auto bytes = bytes_of(constant);
    CHECK(slow->put(bytes.data(), static_cast<int32_t>(bytes.size())) && slow->flush());
    CHECK(slow->resume());
    std::vector<int16_t> played;
    for (int round = 0; round < 4; ++round) {
        CHECK(slow->queued_bytes() > 0 || slow->available_bytes() > 0);
        const auto out = mix(mixer, 500);
        played.insert(played.end(), out.begin(), out.end());
    }
    CHECK(slow->queued_bytes() == 0 && slow->available_bytes() == 0);
    CHECK(played.size() == 2000 * 2);
    for (std::size_t sample = 2 * 64; sample + 2 * 64 < played.size(); ++sample)
        CHECK(played[sample] == 8192);
    for (const int16_t sample : mix(mixer, 512))
        CHECK(sample == 0);
}

void conversion_for_the_mixer() {
    // One channel stays one, at four times the frames from 11025 Hz and at
    // half its level.
    std::vector<int16_t> samples;
    const std::vector<uint8_t> eight(1000, 0xc0);
    CHECK(convert_for_mixer({SampleFormat::u8, 1, 11025}, eight, samples) == 1);
    CHECK(samples.size() == 4000);
    for (std::size_t frame = 64; frame + 64 < samples.size(); ++frame)
        CHECK(samples[frame] == 8192);
    // At the mixer's rate the samples are only halved, rounding halves
    // upward; more than two channels keep their first two.
    const auto wide = bytes_of(std::vector<int16_t>{2, 4, 6, 9, -3, 12});
    CHECK(convert_for_mixer({SampleFormat::s16, 3, 44100}, wide, samples) == 2);
    CHECK(samples == std::vector<int16_t>({1, 2, 5, -1}));
    // Played at converted_sample_gain, they come back at their own level.
    SoftwareMixer mixer(test_lock());
    std::string error;
    auto stream = mixer.open_stream({SampleFormat::s16, 1, 44100}, nullptr, nullptr, error);
    const auto half = bytes_of(std::vector<int16_t>{8192, -4096});
    CHECK(stream->set_gain(converted_sample_gain) && stream->put(half.data(), 4));
    CHECK(stream->resume());
    CHECK(mix(mixer, 2) == std::vector<int16_t>({16384, 16384, -8192, -8192}));
    stream.reset();
    // A format the mixer does not take converts nothing.
    CHECK(convert_for_mixer({SampleFormat::s16, 1, 10}, wide, samples) == 0 && samples.empty());
    CHECK(convert_for_mixer({SampleFormat::s16, 0, 44100}, wide, samples) == 0 && samples.empty());
    // The converted size is known before converting, and holds what
    // converting gives.
    CHECK(converted_bytes({SampleFormat::u8, 1, 11025}, eight.size()) == 4001 * 2);
    CHECK(convert_for_mixer({SampleFormat::u8, 1, 11025}, eight, samples) == 1);
    CHECK(samples.size() * 2 <= converted_bytes({SampleFormat::u8, 1, 11025}, eight.size()));
    CHECK(converted_bytes({SampleFormat::s16, 3, 44100}, wide.size()) == 3 * 2 * 2);
    CHECK(converted_bytes({SampleFormat::u8, 1, 1000}, 200000) == 8820001 * 2);
    CHECK(converted_bytes({SampleFormat::s16, 1, 10}, wide.size()) == 0);
}

// A device the test plays by hand: queued buffers stay busy until played.
struct FakeDevice {
    bool fail_open{};
    bool fail_queue{};
    bool open{};
    uint32_t rate{};
    uint32_t frames{};
    std::vector<bool> busy;
    std::vector<uint32_t> order;               // indexes in the order queued
    std::deque<uint32_t> playing;              // queued and not yet played, oldest first
    std::vector<std::vector<int16_t>> written; // samples, in the order queued
    void (*pump)(void*){};
    void* argument{};
    bool pumping{};
    int closes{};

    oa::platform::sound_device::Hooks hooks() {
        oa::platform::sound_device::Hooks device{};
        device.context = this;
        device.open =
            [](void* c, uint32_t rate, uint32_t frames, uint32_t count, std::string& error) {
                auto& self = *static_cast<FakeDevice*>(c);
                if (self.fail_open) {
                    error = "no device";
                    return false;
                }
                self.open = true;
                self.rate = rate;
                self.frames = frames;
                self.busy.assign(count, false);
                return true;
            };
        device.buffer_free = [](void* c, uint32_t index) {
            return !static_cast<FakeDevice*>(c)->busy[index];
        };
        device.queue_buffer = [](void* c, uint32_t index, const int16_t* samples, uint32_t frames) {
            auto& self = *static_cast<FakeDevice*>(c);
            if (self.fail_queue)
                return false;
            self.busy[index] = true;
            self.order.push_back(index);
            self.playing.push_back(index);
            self.written.emplace_back(samples, samples + frames * 2);
            return true;
        };
        device.start_pump = [](void* c, void (*pump)(void*), void* argument) {
            auto& self = *static_cast<FakeDevice*>(c);
            self.pump = pump;
            self.argument = argument;
            self.pumping = true;
            return true;
        };
        device.stop_pump = [](void* c) { static_cast<FakeDevice*>(c)->pumping = false; };
        device.close = [](void* c) {
            auto& self = *static_cast<FakeDevice*>(c);
            self.open = false;
            ++self.closes;
        };
        device.lock = [](void*) { mixer_mutex.lock(); };
        device.unlock = [](void*) { mixer_mutex.unlock(); };
        device.name = "fake";
        return device;
    }

    // Plays out the oldest busy buffer and calls the pump, as the device's thread would.
    void play_one() {
        if (!playing.empty()) {
            busy[playing.front()] = false;
            playing.pop_front();
        }
        pump(argument);
    }
};

void buffered_output() {
    FakeDevice device;
    std::string error;
    {
        device.fail_open = true;
        BufferedOutput output(device.hooks());
        CHECK(!output.start(error) && error == "no device" && !output.started());
        CHECK(output.last_error() == "no device");
        device.fail_open = false;
    }
    BufferedOutput output(device.hooks());
    CHECK(output.driver_name().empty());
    // Start fills and queues the whole ring, then starts the pump.
    CHECK(output.start(error) && output.started() && device.open && device.pumping);
    CHECK(device.rate == mixer_output_rate && device.frames == output_buffer_frames);
    CHECK(device.order == std::vector<uint32_t>({0, 1, 2, 3}) && output.driver_name() == "fake");
    for (const auto& buffer : device.written)
        for (const int16_t sample : buffer)
            CHECK(sample == 0);
    // Nothing is free: a pump queues nothing.
    CHECK(output.pump() == 0);

    // A stream's samples reach the next buffers to be refilled, in ring order.
    auto stream = output.open_stream({SampleFormat::s16, 2, 44100}, nullptr, nullptr, error);
    std::vector<int16_t> ramp(2 * output_buffer_frames * 2);
    for (std::size_t i = 0; i < ramp.size(); ++i)
        ramp[i] = static_cast<int16_t>(i);
    const auto bytes = bytes_of(ramp);
    CHECK(stream->put(bytes.data(), static_cast<int32_t>(bytes.size())) && stream->resume());
    device.play_one();
    device.play_one();
    CHECK(device.order.size() == 6 && device.order[4] == 0 && device.order[5] == 1);
    for (std::size_t i = 0; i < ramp.size(); ++i) {
        const auto& buffer = device.written[4 + i / (2 * output_buffer_frames)];
        CHECK(buffer[i % (2 * output_buffer_frames)] == ramp[i]);
    }
    device.play_one();
    CHECK(device.order.back() == 2);
    for (const int16_t sample : device.written.back())
        CHECK(sample == 0);
    CHECK(output.buffers_queued() == 7);

    // A refused buffer is reported; starts are counted.
    device.fail_queue = true;
    device.play_one();
    CHECK(!output.last_error().empty());
    device.fail_queue = false;
    CHECK(output.start(error));
    output.stop();
    CHECK(output.started() && device.open);
    stream.reset();
    output.stop();
    CHECK(!output.started() && !device.open && !device.pumping && device.closes == 1);
}

#ifdef _WIN32
// The wave-out mixer on the system's device: a tone plays out and the
// stream drains.
int wave_out() {
    auto output = wave_out_sound_output_create();
    CHECK(output != nullptr);
    std::string error;
    if (!output->start(error)) {
        std::printf("wave-out: skipped, no device (%s)\n", error.c_str());
        return skipped;
    }
    CHECK(output->driver_name() == "waveout");
    auto stream = output->open_stream({SampleFormat::s16, 1, 22050}, nullptr, nullptr, error);
    CHECK(stream != nullptr);
    std::vector<int16_t> tone(22050 / 2);
    for (std::size_t i = 0; i < tone.size(); ++i)
        tone[i] = static_cast<int16_t>(8000.0 * std::sin(0.1 * static_cast<double>(i)));
    CHECK(stream->put(tone.data(), static_cast<int32_t>(tone.size() * 2)) && stream->flush());
    CHECK(stream->resume());
    const auto begin = std::chrono::steady_clock::now();
    while (stream->queued_bytes() > 0 || stream->available_bytes() > 0) {
        CHECK(std::chrono::steady_clock::now() - begin < std::chrono::seconds(10));
        oa::base::threads::sleep_ms(10);
    }
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    // A device that keeps time takes about half a second; one that does
    // not (a silent stand-in) takes less.
    std::printf("wave-out: half a second of tone drained in %.2f s\n", seconds);
    stream.reset();
    output->stop();
    CHECK(!output->started());
    return 0;
}
#endif

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--wave-out") {
#ifdef _WIN32
        return wave_out();
#else
        std::puts("wave-out: skipped, not Windows");
        return skipped;
#endif
    }
    output_choice();
    formats();
    pcm_resampler_limits();
    pcm_resampler_lengths_and_chunks();
    pcm_resampler_quality();
    conversion_and_gain();
    feeds();
    conversion_for_the_mixer();
    buffered_output();
    std::puts("sound output: ok");
    return 0;
}
