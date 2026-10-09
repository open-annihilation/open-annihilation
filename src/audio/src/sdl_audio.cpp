// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/audio/sdl_audio.hpp"

#include "oa/audio/software_mixer.hpp"
#include "oa/audio/sound_output.hpp"
#include "oa/audio/spatial_gain.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::audio::game_audio {

struct LoopingTrack {
    std::vector<uint8_t> pcm;
    std::size_t offset = 0;
};

void refill_loop(void* userdata, OutputStream& stream, int32_t additional) {
    auto* loop = static_cast<LoopingTrack*>(userdata);
    if (loop == nullptr || loop->pcm.empty() || additional <= 0)
        return;
    while (additional > 0) {
        if (loop->offset >= loop->pcm.size())
            loop->offset = 0;
        const auto remain = loop->pcm.size() - loop->offset;
        const auto put = std::min(remain, static_cast<std::size_t>(additional));
        if (!stream.put(loop->pcm.data() + loop->offset, static_cast<int32_t>(put)))
            return;
        loop->offset += put;
        additional -= static_cast<int>(put);
    }
}

/// The most bytes of decoded sound files the player keeps, as 16-bit samples
/// at the mixer's rate: about a minute and a half of one channel. A sound
/// played or looped from the cache converts to no more than this; the
/// largest effect of the game and the mods it was measured against converts
/// to 6.3 MB.
constexpr std::size_t max_cached_sound_bytes = std::size_t{8} << 20;
/// The most bytes a streamed sound, such as a briefing's narration,
/// converts to: about three minutes of two channels. The longest narration
/// of the game converts to 9.0 MB.
constexpr std::size_t max_streamed_sound_bytes = std::size_t{32} << 20;
/// The most streams the player keeps open for its effects while none plays on them.
constexpr std::size_t max_idle_streams = 8;

/// Tells whether two stream formats are the same.
///
/// @param a a format
/// @param b a format
/// @return true when the sample format, channels and rate match
bool same_format(const StreamFormat& a, const StreamFormat& b) noexcept {
    return a.sample == b.sample && a.channels == b.channels && a.rate == b.rate;
}

/// One of a sound's buffers, as the game's mixer keeps up to sample_buffers
/// of them per sound: it plays one start at a time, on a stream it holds
/// while it plays.
struct SoundBuffer {
    bool present{}; ///< made: the first with the sound, the others as starts need them
    std::unique_ptr<OutputStream> stream;
    StreamFormat format{}; ///< the stream's format
    int32_t put_bytes{};   ///< bytes the start it plays put to the stream
    float level{1.0F};     ///< the voice volume relative to the near volume
};

/// A sound file as SDL decoded it, converted once to the mixer's rate
/// (convert_for_mixer, at half its level), and its buffers.
struct CachedSound {
    StreamFormat format{}; ///< the samples' format: 16-bit at mixer_output_rate
    std::vector<int16_t> samples;
    std::array<SoundBuffer, sample_buffers> buffers{};
    uint64_t last_started{}; ///< the count of starts when it last started
};

/// One of the default_voice_limit voices; a restarted buffer holds two.
struct SoundVoice {
    SoundBuffer* buffer{};
    int32_t serial{}; ///< start order, for the oldest-voice rule
};

/// Tells whether a buffer's stream has samples left to play.
///
/// @param buffer the buffer
/// @return true while it plays
bool buffer_playing(const SoundBuffer& buffer) noexcept {
    return buffer.stream != nullptr &&
           (buffer.stream->queued_bytes() > 0 || buffer.stream->available_bytes() > 0);
}

/// Returns how much of its start a buffer has played.
///
/// @param buffer the buffer
/// @return bytes in its stream's format
int32_t buffer_played(const SoundBuffer& buffer) noexcept {
    if (buffer.stream == nullptr)
        return 0;
    return buffer.put_bytes - buffer.stream->queued_bytes() - buffer.stream->available_bytes();
}

struct SdlWavPlayer::Impl {
    const oa::AssetStore& assets;
    // Each sound file played, decoded once, under its path with its letters
    // lowered; at most max_cached_sound_bytes of samples.
    std::map<std::string, CachedSound, std::less<>> sounds;
    std::size_t cached_bytes{};
    std::vector<SoundVoice> voices;
    int32_t serial{};
    uint64_t starts{};
    // Streams no buffer plays on, kept to play the next starts on.
    std::vector<std::pair<StreamFormat, std::unique_ptr<OutputStream>>> idle_streams;
    std::unique_ptr<OutputStream> looping;
    LoopingTrack loop_track;
    bool loop_held{};                     // hold_loop: the loop, and any loop started, stays paused
    std::unique_ptr<OutputStream> stream; // the one streamed sound, played once
    uint32_t wave_out_volume{full_wave_out_volume};
    uint32_t fx_volume{default_fx_volume};

    explicit Impl(const oa::AssetStore& value) : assets(value) {}

    ~Impl() {
        looping.reset();
        stream.reset();
        voices.clear();
        sounds.clear();
        idle_streams.clear();
    }

    /// Takes a buffer's stream back once it has stopped playing, keeping up
    /// to max_idle_streams of them for later starts.
    ///
    /// @param buffer a buffer that does not play
    void release_stream(SoundBuffer& buffer) {
        if (buffer.stream == nullptr)
            return;
        buffer.stream->clear();
        if (idle_streams.size() < max_idle_streams)
            idle_streams.emplace_back(buffer.format, std::move(buffer.stream));
        buffer.stream.reset();
    }

    /// Frees the voices whose buffer no longer plays, and the streams of
    /// buffers that have stopped.
    void collect_voices() {
        std::erase_if(voices, [](const SoundVoice& voice) {
            return !buffer_playing(*voice.buffer);
        });
        for (auto& [key, sound] : sounds)
            for (auto& buffer : sound.buffers)
                if (buffer.stream != nullptr && !buffer_playing(buffer))
                    release_stream(buffer);
    }

    /// Stops the buffer of the oldest voice and frees that voice.
    ///
    /// @return false when no voice plays
    bool evict_oldest_voice() {
        if (voices.empty())
            return false;
        const auto oldest = std::min_element(
            voices.begin(), voices.end(), [](const SoundVoice& a, const SoundVoice& b) {
                return a.serial < b.serial;
            }
        );
        release_stream(*oldest->buffer);
        voices.erase(oldest);
        return true;
    }
};

namespace {

constexpr uint64_t milliseconds_per_second = 1000;

// The stream format of samples SDL decoded from a WAV.
bool stream_format(const SDL_AudioSpec& spec, StreamFormat& format, std::string& error) {
    switch (spec.format) {
    case SDL_AUDIO_U8:
        format.sample = SampleFormat::u8;
        break;
    case SDL_AUDIO_S16LE:
        format.sample = SampleFormat::s16;
        break;
    case SDL_AUDIO_S32LE:
        format.sample = SampleFormat::s32;
        break;
    case SDL_AUDIO_F32LE:
        format.sample = SampleFormat::f32;
        break;
    default:
        error = "unsupported WAV sample format";
        return false;
    }
    if (spec.channels <= 0 || spec.channels > max_stream_channels || spec.freq <= 0) {
        error = "unsupported WAV channels or rate";
        return false;
    }
    format.channels = static_cast<uint8_t>(spec.channels);
    format.rate = static_cast<uint32_t>(spec.freq);
    return true;
}

// Decodes a WAV the asset store holds; the caller frees `wav` with SDL_free.
bool load_wav(
    const oa::AssetStore& assets,
    std::string_view resource,
    SDL_AudioSpec& spec,
    Uint8*& wav,
    Uint32& length,
    std::string& error
) {
    AssetData data;
    try {
        data = assets.read(resource);
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    }
    SDL_IOStream* io = SDL_IOFromConstMem(data.bytes.data(), data.bytes.size());
    if (!io) {
        error = SDL_GetError();
        return false;
    }
    if (!SDL_LoadWAV_IO(io, true, &spec, &wav, &length)) {
        error = SDL_GetError();
        return false;
    }
    return true;
}

/// Decodes a WAV the asset store holds and converts it to the mixer's rate,
/// at half its level (convert_for_mixer), so that it plays with no
/// conversion as it plays.
///
/// A WAV whose converted samples would pass `largest` bytes is refused
/// before it is converted.
///
/// @param assets the asset store
/// @param resource the WAV's archive path
/// @param largest the most bytes the converted samples may take
/// @param[out] format the converted samples' format
/// @param[out] samples the converted samples
/// @param[out] error why the WAV cannot be played; untouched on success
/// @return true when the WAV was decoded and converted
bool load_converted(
    const oa::AssetStore& assets,
    std::string_view resource,
    std::size_t largest,
    StreamFormat& format,
    std::vector<int16_t>& samples,
    std::string& error
) {
    SDL_AudioSpec spec{};
    Uint8* wav = nullptr;
    Uint32 length = 0;
    if (!load_wav(assets, resource, spec, wav, length, error))
        return false;
    StreamFormat decoded{};
    const bool known = stream_format(spec, decoded, error);
    if (known && converted_bytes(decoded, length) > largest) {
        SDL_free(wav);
        error = "the sound is too long to play: it converts to more than " +
                std::to_string(largest) + " bytes";
        return false;
    }
    const uint32_t channels =
        known ? convert_for_mixer(decoded, std::span<const uint8_t>(wav, length), samples) : 0;
    SDL_free(wav);
    if (!known)
        return false;
    if (channels == 0) {
        error = "unsupported WAV channels or rate";
        return false;
    }
    format = StreamFormat{SampleFormat::s16, static_cast<uint8_t>(channels), mixer_output_rate};
    return true;
}

/// Returns the gain a converted sound plays at: the effects' output gain,
/// raised for samples kept at half their level.
///
/// @param wave_out_volume the restored WaveOutVolume scalar, 0..0xffff
/// @param fx_volume the effects volume preference, 0..64
/// @return the stream gain
float converted_gain(uint32_t wave_out_volume, uint32_t fx_volume) noexcept {
    return output_gain(wave_out_volume, fx_volume) * converted_sample_gain;
}

/// Returns the bytes of a converted sound's samples.
///
/// @param samples the samples
/// @return their byte count
std::size_t sample_bytes_of(const std::vector<int16_t>& samples) noexcept {
    return samples.size() * sizeof(int16_t);
}

} // namespace

SdlWavPlayer::SdlWavPlayer(const oa::AssetStore& assets) : impl_(std::make_unique<Impl>(assets)) {
}

SdlWavPlayer::~SdlWavPlayer() = default;
SdlWavPlayer::SdlWavPlayer(SdlWavPlayer&&) noexcept = default;
SdlWavPlayer& SdlWavPlayer::operator=(SdlWavPlayer&&) noexcept = default;

bool SdlWavPlayer::play(const Selection& selection, std::string& error) {
    if (selection.status != SelectionStatus::selected || selection.sound == nullptr) {
        error = "sound selection is not playable";
        return false;
    }
    return play_resource(selection.sound->resource, error);
}

bool SdlWavPlayer::play_resource(std::string_view resource, std::string& error) {
    return play_placed(resource, oa::audio::volume_near, oa::audio::Spatial{}, error);
}

namespace {

/// Returns the key a sound file is cached under: its path with ASCII letters lowered.
///
/// @param resource the file's path in the archives
/// @return the key
std::string sound_key(std::string_view resource) {
    std::string key(resource);
    for (auto& character : key)
        if (character >= 'A' && character <= 'Z')
            character = static_cast<char>(character - 'A' + 'a');
    return key;
}

} // namespace

bool SdlWavPlayer::play_placed(
    std::string_view resource, int32_t volume, const oa::audio::Spatial& spatial, std::string& error
) {
    auto& impl = *impl_;
    collect_finished();
    // The sound, decoded on its first start and kept while the cache has room.
    const std::string key = sound_key(resource);
    auto found = impl.sounds.find(key);
    if (found == impl.sounds.end()) {
        StreamFormat format{};
        std::vector<int16_t> samples;
        if (!load_converted(impl.assets, resource, max_cached_sound_bytes, format, samples, error))
            return false;
        const std::size_t length = sample_bytes_of(samples);
        // The least recently started sounds that no buffer plays make room.
        while (impl.cached_bytes + length > max_cached_sound_bytes) {
            auto oldest = impl.sounds.end();
            for (auto it = impl.sounds.begin(); it != impl.sounds.end(); ++it) {
                const bool busy = std::any_of(
                    it->second.buffers.begin(), it->second.buffers.end(), buffer_playing
                );
                if (!busy && (oldest == impl.sounds.end() ||
                              it->second.last_started < oldest->second.last_started))
                    oldest = it;
            }
            if (oldest == impl.sounds.end())
                break;
            impl.cached_bytes -= sample_bytes_of(oldest->second.samples);
            impl.sounds.erase(oldest);
        }
        CachedSound& sound = impl.sounds[key];
        sound.format = format;
        sound.samples = std::move(samples);
        sound.buffers[0].present = true;
        impl.cached_bytes += length;
        found = impl.sounds.find(key);
    }
    CachedSound& sound = found->second;
    sound.last_started = ++impl.starts;

    // The game's voice policy: at most default_voice_limit voices, the
    // oldest stopping for a start that finds them all taken.
    while (default_voice_limit <= static_cast<int32_t>(impl.voices.size()))
        if (!impl.evict_oldest_voice())
            break;
    // An idle buffer of the sound, a new one while fewer than sample_buffers
    // exist (the last empty index first), or else the one that has played
    // furthest (the lowest index among equals).
    SoundBuffer* chosen = nullptr;
    int32_t furthest = 0;
    std::size_t furthest_index = 0;
    std::size_t empty_index = 0;
    for (std::size_t k = 0; k < sample_buffers; ++k) {
        SoundBuffer& buffer = sound.buffers[k];
        if (!buffer.present) {
            empty_index = k;
            continue;
        }
        if (!buffer_playing(buffer)) {
            chosen = &buffer;
            break;
        }
        if (furthest < buffer_played(buffer)) {
            furthest = buffer_played(buffer);
            furthest_index = k;
        }
    }
    if (chosen == nullptr) {
        if (empty_index < 1) {
            chosen = &sound.buffers[furthest_index];
        } else {
            chosen = &sound.buffers[empty_index];
            chosen->present = true;
        }
    }

    // The start plays the sound as decoded, its sides weighed for its placement.
    const StreamFormat format = sound.format;
    const oa::audio::StereoGain sides = spatial.mode == oa::audio::SpatialMode::normal
                                            ? oa::audio::spatial_stereo_gain(spatial)
                                            : oa::audio::StereoGain{};
    const float level =
        std::pow(10.0F, static_cast<float>(volume - oa::audio::volume_near) / 2000.0F);
    // A restarted buffer plays its new start on the stream it has; another
    // takes an idle stream of the format, or opens one.
    SoundOutput& output = sound_output();
    if (chosen->stream != nullptr && !same_format(chosen->format, format))
        chosen->stream.reset();
    if (chosen->stream != nullptr) {
        chosen->stream->clear();
    } else {
        const auto idle = std::find_if(
            impl.idle_streams.begin(), impl.idle_streams.end(), [&](const auto& entry) {
                return same_format(entry.first, format);
            }
        );
        if (idle != impl.idle_streams.end()) {
            chosen->stream = std::move(idle->second);
            impl.idle_streams.erase(idle);
        } else {
            chosen->stream = output.open_stream(format, nullptr, nullptr, error);
        }
        chosen->format = format;
    }
    if (chosen->stream == nullptr)
        return false;
    const auto length = static_cast<int32_t>(sample_bytes_of(sound.samples));
    chosen->put_bytes = length;
    chosen->level = level;
    // An output that cannot weigh the sides apart plays the start centred.
    static_cast<void>(chosen->stream->set_side_gains(sides.left, sides.right));
    const bool queued =
        chosen->stream->set_gain(converted_gain(impl.wave_out_volume, impl.fx_volume) * level) &&
        chosen->stream->put(sound.samples.data(), length) && chosen->stream->flush() &&
        chosen->stream->resume();
    if (!queued) {
        error = output.last_error();
        chosen->stream.reset();
        return false;
    }
    // A restarted buffer keeps the voice it had and gains another, as the
    // game's mixer counts it: it holds two of the voices until it stops.
    impl.voices.push_back({chosen, ++impl.serial});
    error.clear();
    return true;
}

bool SdlWavPlayer::start_loop_resource(std::string_view resource, std::string& error) {
    stop_loop();
    StreamFormat format{};
    std::vector<int16_t> samples;
    if (!load_converted(impl_->assets, resource, max_cached_sound_bytes, format, samples, error))
        return false;
    const auto* first = reinterpret_cast<const uint8_t*>(samples.data());
    impl_->loop_track.pcm.assign(first, first + sample_bytes_of(samples));
    impl_->loop_track.offset = 0;
    SoundOutput& output = sound_output();
    impl_->looping = output.open_stream(format, refill_loop, &impl_->loop_track, error);
    if (impl_->looping == nullptr) {
        impl_->loop_track.pcm.clear();
        return false;
    }
    // A held loop stays paused, as the stream opens, until the hold ends.
    if (!impl_->looping->set_gain(converted_gain(impl_->wave_out_volume, impl_->fx_volume)) ||
        (!impl_->loop_held && !impl_->looping->resume())) {
        error = output.last_error();
        stop_loop();
        return false;
    }
    error.clear();
    return true;
}

void SdlWavPlayer::stop_loop() noexcept {
    impl_->looping.reset();
    impl_->loop_track = {};
}

void SdlWavPlayer::hold_loop(bool held) noexcept {
    if (held == impl_->loop_held)
        return;
    impl_->loop_held = held;
    if (impl_->looping == nullptr)
        return;
    // A paused stream keeps the samples queued, so the loop plays on from
    // where it stopped.
    if (held)
        impl_->looping->pause();
    else
        impl_->looping->resume();
}

bool SdlWavPlayer::play_stream(std::string_view resource, uint32_t delay_ms, std::string& error) {
    stop_stream();
    StreamFormat format{};
    std::vector<int16_t> samples;
    if (!load_converted(impl_->assets, resource, max_streamed_sound_bytes, format, samples, error))
        return false;
    SoundOutput& output = sound_output();
    auto stream = output.open_stream(format, nullptr, nullptr, error);
    if (stream == nullptr)
        return false;
    // The delay plays as silence ahead of the sound.
    const auto delay_frames = static_cast<std::size_t>(
        static_cast<uint64_t>(format.rate) * delay_ms / milliseconds_per_second
    );
    const std::vector<uint8_t> silence(
        delay_frames * frame_bytes(format), silence_byte(format.sample)
    );
    const bool queued =
        stream->set_gain(converted_gain(impl_->wave_out_volume, impl_->fx_volume)) &&
        (silence.empty() || stream->put(silence.data(), static_cast<int32_t>(silence.size()))) &&
        stream->put(samples.data(), static_cast<int32_t>(sample_bytes_of(samples))) &&
        stream->flush() && stream->resume();
    if (!queued) {
        error = output.last_error();
        return false;
    }
    impl_->stream = std::move(stream);
    error.clear();
    return true;
}

void SdlWavPlayer::stop_stream() noexcept {
    impl_->stream.reset();
}

bool SdlWavPlayer::stream_busy() const noexcept {
    return impl_->stream != nullptr &&
           (impl_->stream->queued_bytes() > 0 || impl_->stream->available_bytes() > 0);
}

void SdlWavPlayer::stop_all() noexcept {
    impl_->voices.clear();
    for (auto& [key, sound] : impl_->sounds)
        for (auto& buffer : sound.buffers)
            buffer.stream.reset();
    impl_->idle_streams.clear();
    stop_loop();
    stop_stream();
}

bool SdlWavPlayer::playing() const noexcept {
    const bool effect =
        std::any_of(impl_->voices.begin(), impl_->voices.end(), [](const SoundVoice& voice) {
            return buffer_playing(*voice.buffer);
        });
    return effect || impl_->looping != nullptr || stream_busy();
}

void SdlWavPlayer::set_volume(uint32_t wave_out_volume, uint32_t fx_volume) noexcept {
    impl_->wave_out_volume = wave_out_volume;
    impl_->fx_volume = fx_volume;
    const float gain = converted_gain(wave_out_volume, fx_volume);
    for (auto& [key, sound] : impl_->sounds)
        for (auto& buffer : sound.buffers)
            if (buffer.stream != nullptr)
                buffer.stream->set_gain(gain * buffer.level);
    if (impl_->looping != nullptr)
        impl_->looping->set_gain(gain);
    if (impl_->stream != nullptr)
        impl_->stream->set_gain(gain);
}

int32_t SdlWavPlayer::effect_voices() const noexcept {
    return static_cast<int32_t>(impl_->voices.size());
}

void SdlWavPlayer::collect_finished() noexcept {
    try {
        impl_->collect_voices();
    } catch (const std::exception&) {
        // Keeping an idle stream can only fail for memory; it is dropped instead.
        impl_->idle_streams.clear();
    }
    if (impl_->stream != nullptr && !stream_busy())
        stop_stream();
}

} // namespace oa::audio::game_audio

namespace oa::audio {

struct SdlVoice {
    std::unique_ptr<OutputStream> stream;
    std::vector<uint8_t> pcm;
    PcmFormat format{};
    uint32_t read{};
    bool looping{};
    bool playing{};
    float gain{1.0F};
};

struct SdlAudioDevice {
    bool open{};
    std::map<BufferHandle, std::unique_ptr<SdlVoice>> voices;
    BufferHandle next{1};
    uint32_t wave_out_packed{0xffffffffU};

    SdlVoice* find(BufferHandle handle) {
        const auto found = voices.find(handle);
        return found == voices.end() ? nullptr : found->second.get();
    }

    float master_gain() const {
        return static_cast<float>(wave_out_packed & max_device_volume) /
               static_cast<float>(max_device_volume);
    }
};

namespace {

// Runs on the sound output's thread with the stream locked.
void feed_voice(void* user, OutputStream& stream, int32_t additional) {
    auto* voice = static_cast<SdlVoice*>(user);
    while (voice->playing && additional > 0 && !voice->pcm.empty()) {
        if (voice->read >= voice->pcm.size()) {
            if (!voice->looping) {
                voice->playing = stream.queued_bytes() > 0;
                return;
            }
            voice->read = 0;
        }
        const auto count = std::min<std::size_t>(
            voice->pcm.size() - voice->read, static_cast<std::size_t>(additional)
        );
        const auto* bytes = voice->pcm.data() + voice->read;
        if (!stream.put(bytes, static_cast<int32_t>(count)))
            return;
        voice->read += static_cast<uint32_t>(count);
        additional -= static_cast<int>(count);
    }
}

BufferHandle open_voice(SdlAudioDevice& device, std::unique_ptr<SdlVoice> voice) {
    if (!device.open || (voice->format.bits != 8 && voice->format.bits != 16) ||
        voice->format.channels == 0 || voice->format.channels > max_stream_channels ||
        voice->format.sample_rate == 0)
        return no_buffer;
    StreamFormat format{};
    format.sample = voice->format.bits == 8 ? SampleFormat::u8 : SampleFormat::s16;
    format.channels = static_cast<uint8_t>(voice->format.channels);
    format.rate = voice->format.sample_rate;
    std::string error;
    voice->stream = sound_output().open_stream(format, feed_voice, voice.get(), error);
    if (voice->stream == nullptr)
        return no_buffer;
    voice->stream->set_gain(device.master_gain());
    const BufferHandle handle = device.next++;
    device.voices.emplace(handle, std::move(voice));
    return handle;
}

template <typename Function>
bool with_voice(void* context, BufferHandle handle, Function&& function) {
    auto* voice = static_cast<SdlAudioDevice*>(context)->find(handle);
    if (voice == nullptr)
        return false;
    voice->stream->lock();
    const bool result = function(*voice);
    voice->stream->unlock();
    return result;
}

} // namespace

SdlAudioDevice* sdl_audio_device_create() {
    return new SdlAudioDevice{};
}

void sdl_audio_device_destroy(SdlAudioDevice* device) noexcept {
    if (device == nullptr)
        return;
    device->voices.clear();
    delete device;
}

AudioSink sdl_audio_sink(SdlAudioDevice* device) noexcept {
    AudioSink sink{};
    sink.context = device;
    sink.open_device = [](void* context) {
        auto& self = *static_cast<SdlAudioDevice*>(context);
        SoundOutput& output = sound_output();
        std::string error;
        if (!output.started() && !output.start(error))
            return DeviceResult::no_driver;
        self.open = true;
        return DeviceResult::ok;
    };
    sink.set_primary_format = [](void*, PcmFormat) { return DeviceResult::ok; };
    sink.close_device = [](void* context) {
        auto& self = *static_cast<SdlAudioDevice*>(context);
        self.voices.clear();
        self.open = false;
    };
    sink.create_buffer = [](void* context, BufferKind, PcmFormat format, uint32_t bytes) {
        auto voice = std::make_unique<SdlVoice>();
        voice->format = format;
        voice->pcm.assign(bytes, format.bits == 8 ? 0x80 : 0);
        return open_voice(*static_cast<SdlAudioDevice*>(context), std::move(voice));
    };
    sink.duplicate_buffer = [](void* context, BufferHandle source) {
        auto& self = *static_cast<SdlAudioDevice*>(context);
        const auto* original = self.find(source);
        if (original == nullptr)
            return no_buffer;
        auto voice = std::make_unique<SdlVoice>();
        voice->format = original->format;
        voice->pcm = original->pcm;
        return open_voice(self, std::move(voice));
    };
    sink.release_buffer = [](void* context, BufferHandle handle) {
        auto& self = *static_cast<SdlAudioDevice*>(context);
        const auto found = self.voices.find(handle);
        if (found == self.voices.end())
            return;
        self.voices.erase(found);
    };
    sink.write_buffer = [](void* context,
                           BufferHandle handle,
                           uint32_t offset,
                           const uint8_t* bytes,
                           uint32_t count) {
        return with_voice(context, handle, [&](SdlVoice& voice) {
            if (offset > voice.pcm.size() || count > voice.pcm.size() - offset)
                return false;
            std::memcpy(voice.pcm.data() + offset, bytes, count);
            return true;
        });
    };
    sink.query_playing = [](void* context, BufferHandle handle, bool* playing) {
        return with_voice(context, handle, [&](SdlVoice& voice) {
            *playing = voice.playing || voice.stream->queued_bytes() > 0;
            return true;
        });
    };
    sink.play_position = [](void* context, BufferHandle handle, uint32_t* position) {
        return with_voice(context, handle, [&](SdlVoice& voice) {
            *position = voice.read;
            return true;
        });
    };
    sink.set_play_position = [](void* context, BufferHandle handle, uint32_t position) {
        return with_voice(context, handle, [&](SdlVoice& voice) {
            const auto size = static_cast<uint32_t>(voice.pcm.size());
            voice.read = std::min(position, size);
            voice.stream->clear();
            return true;
        });
    };
    sink.set_volume = [](void* context, BufferHandle handle, int32_t centibels) {
        const float master = static_cast<SdlAudioDevice*>(context)->master_gain();
        return with_voice(context, handle, [&](SdlVoice& voice) {
            voice.gain = std::pow(10.0F, static_cast<float>(centibels) / 2000.0F);
            return voice.stream->set_gain(voice.gain * master);
        });
    };
    sink.set_spatial = [](void*, BufferHandle, const Spatial*) { return false; };
    sink.play = [](void* context, BufferHandle handle, bool looping) {
        const bool started = with_voice(context, handle, [&](SdlVoice& voice) {
            voice.looping = looping;
            voice.playing = true;
            return true;
        });
        auto* voice = static_cast<SdlAudioDevice*>(context)->find(handle);
        return started && voice->stream->resume();
    };
    sink.stop = [](void* context, BufferHandle handle) {
        with_voice(context, handle, [](SdlVoice& voice) {
            voice.playing = false;
            voice.stream->clear();
            return true;
        });
    };
    sink.wave_out_count = [](void*) { return 1; };
    sink.get_wave_out_volume = [](void* context, int32_t, uint32_t* packed) {
        *packed = static_cast<SdlAudioDevice*>(context)->wave_out_packed;
        return true;
    };
    sink.set_wave_out_volume = [](void* context, int32_t, uint32_t packed) {
        auto& self = *static_cast<SdlAudioDevice*>(context);
        self.wave_out_packed = packed;
        for (auto& [handle, voice] : self.voices)
            voice->stream->set_gain(voice->gain * self.master_gain());
        return true;
    };
    return sink;
}

} // namespace oa::audio
