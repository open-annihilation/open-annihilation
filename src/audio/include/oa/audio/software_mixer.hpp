// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "oa/audio/resampler.hpp"
#include "oa/audio/sound_output.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace oa::audio {

/// Sample rate of the mixed output, in hertz.
inline constexpr uint32_t mixer_output_rate = 44100;
/// Channels of the mixed output: left and right, interleaved.
inline constexpr uint32_t mixer_output_channels = 2;
/// Most input frames a stream converts in one step.
inline constexpr uint32_t mixer_convert_frames = 1024;
/// Most times one mix asks a stream's feed for samples.
inline constexpr uint32_t mixer_feed_calls = 4;
/// Fraction bits of the gains a stream's samples are multiplied by.
inline constexpr uint32_t mixer_gain_bits = 16;
/// Fraction bits the sum of the streams keeps below a 16-bit sample.
inline constexpr uint32_t mixer_sum_bits = 8;
/// The largest gain a side of a stream takes; larger ones are held to it.
inline constexpr float mixer_max_gain = 16.0F;
/// The gain that plays convert_for_mixer's samples at their own level: they
/// are kept at half of it.
inline constexpr float converted_sample_gain = 2.0F;

/// The lock that keeps the mixing thread and the streams' owners apart.
struct MixerLock {
    void* context{};
    /// Takes the lock; the thread holding it may take it again. Null takes no lock.
    void (*lock)(void* context){};
    /// Releases one hold of the lock. Null releases nothing.
    void (*unlock)(void* context){};
};

class MixerStream;

/// Mixes streams of any format into 16-bit stereo at mixer_output_rate, in
/// integer arithmetic.
///
/// Each stream's samples become 16-bit: 8-bit unsigned ones around 128
/// times 256, 32-bit ones without their low 16 bits (rounding toward
/// negative infinity), and float ones, -1..1, times 32768, rounded to
/// nearest and held to -32768..32767. One channel plays on both sides, and
/// more than two play their first two. A stream at another rate is
/// converted by PcmResampler, as its samples arrive, and keeps the
/// conversion's overshoot past full scale until the sum is held. Each side is
/// multiplied by the stream's gain times that side's gain, both rounded to
/// mixer_gain_bits fraction bits, and the streams are summed with
/// mixer_sum_bits fraction bits kept; the sum is rounded to nearest (halves
/// upward) and held to -32768..32767. A stream at the output rate and a
/// gain of one plays its 16-bit samples unchanged. A paused stream keeps
/// its samples and adds nothing, as does a stream with nothing left to
/// play.
class SoftwareMixer {
  public:

    /// Creates a mixer with no streams.
    ///
    /// @param lock the lock that mix() and every stream call take
    explicit SoftwareMixer(MixerLock lock);
    /// Destroys the mixer; every stream must have been destroyed first.
    ~SoftwareMixer();
    SoftwareMixer(const SoftwareMixer&) = delete;
    SoftwareMixer& operator=(const SoftwareMixer&) = delete;

    /// Opens a paused stream that the mixer mixes until it is destroyed.
    ///
    /// @param format the samples the stream takes; 1 to max_stream_channels
    ///        channels at a rate the resampler converts
    /// @param feed asks for more samples as the stream plays; null for none
    /// @param context passed back to the feed
    /// @param[out] error why the stream cannot be opened; untouched on success
    /// @return the stream, or null when the format is outside those limits
    [[nodiscard]] std::unique_ptr<OutputStream>
    open_stream(const StreamFormat& format, StreamFeed feed, void* context, std::string& error);

    /// Mixes the next frames of every playing stream, taking the lock.
    ///
    /// @param[out] out room for frames * mixer_output_channels samples
    /// @param frames frames to mix
    void mix(int16_t* out, uint32_t frames);

    /// Returns how many streams are open.
    ///
    /// @return the open stream count
    [[nodiscard]] std::size_t stream_count();

  private:

    friend class MixerStream;

    /// Takes the mixer's lock.
    void lock() const;

    /// Releases the mixer's lock.
    void unlock() const;

    MixerLock lock_;
    std::vector<MixerStream*> streams_;
    /// The streams open when the current mix() call began; room for every
    /// open stream is kept, so a mix does not allocate it.
    std::vector<MixerStream*> mixing_;
    std::vector<int32_t> sum_; ///< the mix of one call, with mixer_sum_bits fraction bits
};

/// Converts samples to mixer_output_rate, as SoftwareMixer converts a
/// stream's samples, all at once, into 16-bit samples at half their level.
///
/// Half the level leaves room for the conversion's overshoot past full
/// scale; a stream plays them at their own level at converted_sample_gain
/// times the gain it would give the samples as they were. Each converted
/// sample is rounded to nearest, halves upward. One channel stays one, and
/// more than two keep their first two. A sound converted once plays at the
/// mixer's rate with no conversion as it plays.
///
/// @param format the samples' format; 1 to max_stream_channels channels at a
///        rate the resampler converts
/// @param bytes the samples, whole frames; a trailing partial frame is ignored
/// @param[out] samples the converted interleaved samples, at half their level
/// @return the channels of the converted samples, 1 or 2; 0, with
///         `samples` empty, when the format is outside those limits
[[nodiscard]] uint32_t convert_for_mixer(
    const StreamFormat& format, std::span<const uint8_t> bytes, std::vector<int16_t>& samples
);

/// Returns the bytes convert_for_mixer's samples would take, without
/// converting anything: the converted frames, one more than the input's
/// frames take at mixer_output_rate rounded up, of 16-bit samples on its
/// one or two channels.
///
/// @param format the samples' format
/// @param input_bytes bytes of samples; a trailing partial frame is ignored
/// @return the converted bytes, counted at 64 bits; 0 for a format
///         convert_for_mixer refuses
[[nodiscard]] uint64_t
converted_bytes(const StreamFormat& format, std::size_t input_bytes) noexcept;

} // namespace oa::audio
