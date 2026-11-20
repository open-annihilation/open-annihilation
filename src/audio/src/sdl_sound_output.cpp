// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The sound output over SDL: the engine's software mixer mixes every stream
// into one SDL audio stream, which SDL plays on the default playback device.

#include "oa/audio/sound_output_backends.hpp"

#include "oa/audio/software_mixer.hpp"

#include <SDL3/SDL.h>

#include <atomic>
#include <string>
#include <vector>

namespace oa::audio {
namespace {

/// Bytes of one mixed frame: 16-bit left and right.
constexpr int mixed_frame_bytes = static_cast<int>(mixer_output_channels * sizeof(int16_t));
constexpr uint64_t nanoseconds_per_second = 1'000'000'000;
constexpr uint64_t milliseconds_per_second = 1000;

/// Returns how long some frames play.
///
/// @param frames the frames
/// @param rate their rate in hertz; 0 plays nothing
/// @return the time in nanoseconds
uint64_t frames_ns(uint64_t frames, uint64_t rate) noexcept {
    return rate == 0 ? 0 : frames * nanoseconds_per_second / rate;
}

class SdlOutput final : public SoundOutput {
  public:

    SdlOutput() : mutex_(SDL_CreateMutex()), mixer_(MixerLock{this, lock_thunk, unlock_thunk}) {}

    // Every stream of the mixer must have been destroyed first.
    ~SdlOutput() override {
        close_device();
        if (mutex_ != nullptr)
            SDL_DestroyMutex(mutex_);
    }

    SdlOutput(const SdlOutput&) = delete;
    SdlOutput& operator=(const SdlOutput&) = delete;

    bool start(std::string& error) override {
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            error = SDL_GetError();
            return false;
        }
        if (!open_device(error)) {
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
            return false;
        }
        ++start_count_;
        return true;
    }

    void stop() override {
        if (start_count_ > 0)
            --start_count_;
        // The device closes with the last start once no stream is left to play on it.
        if (start_count_ == 0 && mixer_.stream_count() == 0)
            close_device();
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }

    bool started() const override { return SDL_WasInit(SDL_INIT_AUDIO) != 0; }

    std::unique_ptr<OutputStream> open_stream(
        const StreamFormat& format, StreamFeed feed, void* context, std::string& error
    ) override {
        if (!open_device(error))
            return nullptr;
        return mixer_.open_stream(format, feed, context, error);
    }

    std::string driver_name() const override {
        const char* name = SDL_GetCurrentAudioDriver();
        return name != nullptr ? name : "";
    }

    std::string last_error() const override { return SDL_GetError(); }

    std::string take_log() override {
        std::string log;
        log.swap(opened_log_);
        if (device_ == nullptr)
            return log;
        const uint64_t now = SDL_GetTicks();
        if (now - last_report_ms_ < late_feed_report_ms)
            return log;
        log += describe_late_feeds(
            feeds_.take(), static_cast<uint32_t>((now - last_report_ms_) / milliseconds_per_second)
        );
        last_report_ms_ = now;
        // The default device may have moved to another since it opened.
        const std::string device = describe_device();
        if (device != device_log_) {
            device_log_ = device;
            log += device;
        }
        return log;
    }

  private:

    /// Takes the mixer's lock, which the thread holding it may take again.
    ///
    /// @param output the SdlOutput
    static void lock_thunk(void* output) { SDL_LockMutex(static_cast<SdlOutput*>(output)->mutex_); }

    /// Releases one hold of the mixer's lock.
    ///
    /// @param output the SdlOutput
    static void unlock_thunk(void* output) {
        SDL_UnlockMutex(static_cast<SdlOutput*>(output)->mutex_);
    }

    /// Mixes what SDL asks for; runs on SDL's audio thread.
    ///
    /// @param output the SdlOutput
    /// @param stream the stream SDL plays
    /// @param additional bytes SDL needs now
    static void SDLCALL feed_device(void* output, SDL_AudioStream* stream, int additional, int) {
        auto& self = *static_cast<SdlOutput*>(output);
        if (additional <= 0)
            return;
        const uint64_t asked = SDL_GetTicksNS();
        const int frames = (additional + mixed_frame_bytes - 1) / mixed_frame_bytes;
        self.mixed_.resize(static_cast<std::size_t>(frames) * mixer_output_channels);
        // The lock is taken here first, so that the wait for it is timed
        // apart; mix() takes it again.
        lock_thunk(&self);
        const uint64_t locked = SDL_GetTicksNS();
        self.mixer_.mix(self.mixed_.data(), static_cast<uint32_t>(frames));
        unlock_thunk(&self);
        SDL_PutAudioStreamData(stream, self.mixed_.data(), frames * mixed_frame_bytes);
        self.feeds_.note_feed(
            asked,
            locked,
            SDL_GetTicksNS(),
            frames_ns(static_cast<uint64_t>(frames), mixer_output_rate),
            self.period_ns_.load(std::memory_order_relaxed)
        );
    }

    /// Describes the device the stream plays on, for the log.
    ///
    /// Also notes how long the sound it takes at a time plays.
    ///
    /// @return a line ending in a line feed, starting "sound output: "
    std::string describe_device() {
        const SDL_AudioDeviceID device = SDL_GetAudioStreamDevice(device_);
        SDL_AudioSpec spec{};
        int frames = 0;
        if (device == 0 || !SDL_GetAudioDeviceFormat(device, &spec, &frames))
            return "sound output: the device's format is not known: " +
                   std::string(SDL_GetError()) + '\n';
        period_ns_.store(
            frames_ns(
                static_cast<uint64_t>(frames > 0 ? frames : 0),
                static_cast<uint64_t>(spec.freq > 0 ? spec.freq : 0)
            ),
            std::memory_order_relaxed
        );
        const char* name = SDL_GetAudioDeviceName(device);
        const char* format = SDL_GetAudioFormatName(spec.format);
        return "sound output: SDL's " + driver_name() + " driver plays \"" +
               (name != nullptr ? name : "") + "\" at " + std::to_string(spec.freq) + " Hz, " +
               std::to_string(spec.channels) + " channels of " + (format != nullptr ? format : "") +
               ", " + std::to_string(frames) + " frames at a time; the game mixes " +
               std::to_string(mixer_output_channels) + " channels of 16 bits at " +
               std::to_string(mixer_output_rate) + " Hz\n";
    }

    /// Opens the device stream that plays the mix, unless it is open.
    ///
    /// The stream holds a start of SDL's audio subsystem of its own, so it
    /// stays open while streams play on it. SDL_Quit ends it with the rest of
    /// SDL; it is opened again after the next start.
    ///
    /// @param[out] error why it cannot be opened; untouched on success
    /// @return true when the device stream is open
    bool open_device(std::string& error) {
        if (device_ != nullptr && SDL_WasInit(SDL_INIT_AUDIO) == 0)
            device_ = nullptr; // SDL has quit, and its streams with it
        if (device_ != nullptr)
            return true;
        if (mutex_ == nullptr) {
            error = "cannot make the sound output's lock";
            return false;
        }
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            error = SDL_GetError();
            return false;
        }
        const SDL_AudioSpec spec{
            SDL_AUDIO_S16LE,
            static_cast<int>(mixer_output_channels),
            static_cast<int>(mixer_output_rate)
        };
        // No device's thread asks for sound until the stream opens, and it
        // asks for none until the stream is resumed, by when the period is known.
        feeds_.restart();
        device_ =
            SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, feed_device, this);
        if (device_ != nullptr)
            device_log_ = describe_device();
        if (device_ == nullptr || !SDL_ResumeAudioStreamDevice(device_)) {
            error = SDL_GetError();
            if (device_ != nullptr)
                SDL_DestroyAudioStream(device_);
            device_ = nullptr;
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
            return false;
        }
        opened_log_ += device_log_;
        last_report_ms_ = SDL_GetTicks();
        return true;
    }

    /// Closes the device stream, if it is open, with its start of SDL's audio.
    void close_device() {
        if (device_ == nullptr)
            return;
        if (SDL_WasInit(SDL_INIT_AUDIO) != 0) {
            SDL_DestroyAudioStream(device_);
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
        }
        device_ = nullptr;
    }

    SDL_Mutex* mutex_{};         ///< the mixer's lock
    SoftwareMixer mixer_;        ///< mixes every stream
    SDL_AudioStream* device_{};  ///< the stream SDL plays the mix from
    std::vector<int16_t> mixed_; ///< one call's mix, for SDL's audio thread
    uint32_t start_count_{};     ///< start() calls not yet matched by stop()
    FeedTimer feeds_;            ///< times the device's requests for sound
    /// How long the sound the device takes at a time plays, in nanoseconds.
    std::atomic<uint64_t> period_ns_{};
    std::string opened_log_;    ///< lines about devices opened, not yet logged
    std::string device_log_;    ///< the line about the device last logged
    uint64_t last_report_ms_{}; ///< when late requests were last reported, on SDL's clock
};

} // namespace

std::unique_ptr<SoundOutput> sdl_sound_output_create() {
    return std::make_unique<SdlOutput>();
}

} // namespace oa::audio
