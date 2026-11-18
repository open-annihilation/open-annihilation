// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/audio/sound_output.hpp"

#include "oa/audio/sound_output_backends.hpp"
#include "oa/platform/machine.hpp"
#include "oa/platform/system.hpp"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string_view>

namespace oa::audio {
namespace {

constexpr uint8_t unsigned_8_silence = 0x80;
// The values of sound_output_variable that choose an output.
constexpr std::string_view wave_out_choice = "waveout";
constexpr std::string_view sdl_choice = "sdl";

// An output for a build with no sound device: nothing starts.
class SilentOutput final : public SoundOutput {
  public:

    bool start(std::string& error) override {
        error = "this build has no sound output";
        return false;
    }

    void stop() override {}

    bool started() const override { return false; }

    std::unique_ptr<OutputStream>
    open_stream(const StreamFormat&, StreamFeed, void*, std::string& error) override {
        error = "this build has no sound output";
        return nullptr;
    }

    std::string driver_name() const override { return ""; }

    std::string last_error() const override { return "this build has no sound output"; }
};

SoundOutput* chosen_output = nullptr;
// The process's own output, made on first use. sound_output() is first
// called on the main thread, before any stream exists.
SoundOutput* own_output = nullptr;

std::unique_ptr<SoundOutput> make_own_output() {
    const auto variable = platform::environment_value(sound_output_variable);
    const SoundOutputKind first = choose_sound_output(
        variable ? std::optional<std::string_view>(*variable) : std::nullopt,
        platform::running_on_windows_before_vista()
    );
    std::unique_ptr<SoundOutput> output;
    if (first == SoundOutputKind::wave_out)
        output = wave_out_sound_output_create();
    if (output == nullptr)
        output = sdl_sound_output_create();
    if (output == nullptr)
        output = wave_out_sound_output_create();
    if (output == nullptr)
        output = std::make_unique<SilentOutput>();
    return output;
}

} // namespace

SoundOutputKind
choose_sound_output(std::optional<std::string_view> variable, bool windows_before_vista) noexcept {
    if (variable && *variable == wave_out_choice)
        return SoundOutputKind::wave_out;
    if (variable && *variable == sdl_choice)
        return SoundOutputKind::sdl;
    return windows_before_vista ? SoundOutputKind::wave_out : SoundOutputKind::sdl;
}

uint32_t sample_bytes(SampleFormat sample) noexcept {
    switch (sample) {
    case SampleFormat::u8:
        return 1;
    case SampleFormat::s16:
        return 2;
    case SampleFormat::s32:
    case SampleFormat::f32:
        return 4;
    }
    return 1;
}

uint32_t frame_bytes(const StreamFormat& format) noexcept {
    return sample_bytes(format.sample) * format.channels;
}

uint8_t silence_byte(SampleFormat sample) noexcept {
    return sample == SampleFormat::u8 ? unsigned_8_silence : 0;
}

namespace {

constexpr uint64_t nanoseconds_per_microsecond = 1000;
constexpr uint32_t microseconds_per_millisecond = 1000;
/// Microseconds in a hundredth of a millisecond, the last place a report gives.
constexpr uint32_t microseconds_per_hundredth = 10;
/// The least number of hundredths written with two digits of its own.
constexpr uint32_t two_digit_hundredths = 10;

/// Returns a span of nanoseconds in whole microseconds, held to 32 bits.
///
/// @param nanoseconds the span
/// @return the microseconds, rounded down
uint32_t microseconds_of(uint64_t nanoseconds) noexcept {
    const uint64_t microseconds = nanoseconds / nanoseconds_per_microsecond;
    constexpr uint64_t most = std::numeric_limits<uint32_t>::max();
    return static_cast<uint32_t>(std::min(microseconds, most));
}

/// Raises a value to at least another, whatever thread raises it too.
///
/// @param[in,out] value the value
/// @param candidate the value it is raised to when that is more
void raise_to(std::atomic<uint32_t>& value, uint32_t candidate) noexcept {
    uint32_t seen = value.load(std::memory_order_relaxed);
    while (seen < candidate &&
           !value.compare_exchange_weak(seen, candidate, std::memory_order_relaxed)) {
    }
}

/// Writes microseconds as milliseconds with two decimal places.
///
/// @param microseconds the span
/// @return the text, such as "25.03 ms"
std::string milliseconds_text(uint32_t microseconds) {
    const uint32_t hundredths =
        microseconds % microseconds_per_millisecond / microseconds_per_hundredth;
    return std::to_string(microseconds / microseconds_per_millisecond) + '.' +
           (hundredths < two_digit_hundredths ? "0" : "") + std::to_string(hundredths) + " ms";
}

} // namespace

void FeedTimer::restart() noexcept {
    fed_ = false;
}

void FeedTimer::note_feed(
    uint64_t asked_ns, uint64_t locked_ns, uint64_t done_ns, uint64_t lasts_ns, uint64_t period_ns
) noexcept {
    feeds_.fetch_add(1, std::memory_order_relaxed);
    // The device held at most the last request's sound and one period.
    const uint64_t held_ns = last_lasts_ns_ + period_ns;
    if (fed_ && asked_ns > last_asked_ns_ &&
        asked_ns - last_asked_ns_ > held_ns + late_feed_margin_ns) {
        late_feeds_.fetch_add(1, std::memory_order_relaxed);
        raise_to(longest_silence_us_, microseconds_of(asked_ns - last_asked_ns_ - held_ns));
    }
    raise_to(longest_mix_us_, microseconds_of(done_ns > asked_ns ? done_ns - asked_ns : 0));
    raise_to(longest_wait_us_, microseconds_of(locked_ns > asked_ns ? locked_ns - asked_ns : 0));
    last_asked_ns_ = asked_ns;
    last_lasts_ns_ = lasts_ns;
    fed_ = true;
}

FeedReport FeedTimer::take() noexcept {
    FeedReport report;
    report.feeds = feeds_.exchange(0, std::memory_order_relaxed);
    report.late_feeds = late_feeds_.exchange(0, std::memory_order_relaxed);
    report.longest_silence_us = longest_silence_us_.exchange(0, std::memory_order_relaxed);
    report.longest_mix_us = longest_mix_us_.exchange(0, std::memory_order_relaxed);
    report.longest_wait_us = longest_wait_us_.exchange(0, std::memory_order_relaxed);
    return report;
}

std::string describe_late_feeds(const FeedReport& report, uint32_t seconds) {
    if (report.late_feeds == 0)
        return {};
    return "sound output: late requests from the sound device in the last " +
           std::to_string(seconds) + " s: " + std::to_string(report.late_feeds) + " of " +
           std::to_string(report.feeds) + ", the worst after at least " +
           milliseconds_text(report.longest_silence_us) + " without sound; longest mix " +
           milliseconds_text(report.longest_mix_us) + ", " +
           milliseconds_text(report.longest_wait_us) + " of it waiting for the game's sounds\n";
}

SoundOutput& sound_output() {
    if (chosen_output != nullptr)
        return *chosen_output;
    if (own_output == nullptr)
        own_output = make_own_output().release(); // lives until the process ends
    return *own_output;
}

void set_sound_output(SoundOutput* output) noexcept {
    chosen_output = output;
}

} // namespace oa::audio
