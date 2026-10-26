// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/audio/software_mixer.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace oa::audio {
namespace {

/// The value an unsigned 8-bit sample holds at silence.
constexpr int32_t unsigned_8_zero = 0x80;
/// Bits an 8-bit sample is shifted up by to become 16-bit.
constexpr uint32_t widen_8_bits = 8;
/// Bits a 32-bit sample is shifted down by to become 16-bit.
constexpr uint32_t narrow_32_bits = 16;
/// A float sample of 1 as a 16-bit sample.
constexpr float float_full_scale = 32768.0F;
/// The lowest 16-bit sample.
constexpr int32_t sample_min = -32768;
/// The highest 16-bit sample.
constexpr int32_t sample_max = 32767;
/// A gain of one at mixer_gain_bits fraction bits.
constexpr float unity_gain = static_cast<float>(int32_t{1} << mixer_gain_bits);
/// Bits a sample times a gain is shifted down by to join the sum.
constexpr uint32_t gain_to_sum_bits = mixer_gain_bits - mixer_sum_bits;
/// Added to the sum before its fraction bits are dropped, so that it rounds to nearest.
constexpr int32_t sum_rounding = int32_t{1} << (mixer_sum_bits - 1);
/// Most channels a converted stream keeps: left and right.
constexpr uint32_t mixed_channels_max = 2;
/// Converted frames kept before the mixed ones are dropped from the front.
constexpr std::size_t compact_after_samples = 1 << 15;

static_assert(mixer_sum_bits >= 1 && mixer_sum_bits <= mixer_gain_bits);

// Holds the mixer lock for a scope.
class Hold {
  public:

    explicit Hold(OutputStream& stream) : stream_(stream) { stream_.lock(); }

    ~Hold() { stream_.unlock(); }

    Hold(const Hold&) = delete;
    Hold& operator=(const Hold&) = delete;

  private:

    OutputStream& stream_;
};

/// Reads one sample as a 16-bit one.
///
/// @tparam Sample the sample's format
/// @param bytes the sample, little-endian
/// @return the sample, -32768..32767
template <SampleFormat Sample>
int32_t read_sample(const uint8_t* bytes) noexcept {
    if constexpr (Sample == SampleFormat::u8) {
        return (static_cast<int32_t>(bytes[0]) - unsigned_8_zero) * (int32_t{1} << widen_8_bits);
    } else if constexpr (Sample == SampleFormat::s16) {
        return static_cast<int16_t>(static_cast<uint16_t>(bytes[0] | (bytes[1] << 8)));
    } else if constexpr (Sample == SampleFormat::s32) {
        const auto value = static_cast<int32_t>(
            static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
            (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24)
        );
        // An arithmetic shift rounds toward negative infinity.
        return value >> narrow_32_bits;
    } else {
        float value = 0.0F;
        std::memcpy(&value, bytes, sizeof(value));
        const float scaled = value * float_full_scale;
        if (!(scaled == scaled)) // not a number: silence
            return 0;
        if (scaled <= static_cast<float>(sample_min))
            return sample_min;
        if (scaled >= static_cast<float>(sample_max))
            return sample_max;
        // Truncation toward zero after half is added away from zero rounds to nearest.
        return static_cast<int32_t>(scaled < 0.0F ? scaled - 0.5F : scaled + 0.5F);
    }
}

/// Returns a gain at mixer_gain_bits fraction bits.
///
/// @param gain the factor; held to 0..mixer_max_gain, and 0 when not a number
/// @return the gain, rounded to nearest
int32_t fixed_gain(float gain) {
    if (!(gain > 0.0F))
        return 0;
    return static_cast<int32_t>(std::min(gain, mixer_max_gain) * unity_gain + 0.5F);
}

/// Converts whole frames of one sample format to 16-bit samples of at most two channels.
///
/// @tparam Sample the frames' sample format
/// @param bytes the frames
/// @param frames how many to convert
/// @param bytes_per_frame bytes of one frame
/// @param bytes_per_sample bytes of one sample
/// @param kept channels converted of each frame: its first one or two
/// @param[out] out receives frames * kept samples
template <SampleFormat Sample>
void decode_frames_of(
    const uint8_t* bytes,
    std::size_t frames,
    uint32_t bytes_per_frame,
    uint32_t bytes_per_sample,
    uint32_t kept,
    int16_t* out
) noexcept {
    for (std::size_t i = 0; i < frames; ++i, bytes += bytes_per_frame)
        for (uint32_t channel = 0; channel < kept; ++channel)
            *out++ = static_cast<int16_t>(read_sample<Sample>(bytes + channel * bytes_per_sample));
}

/// Converts whole frames to 16-bit samples of at most two channels.
///
/// @param format the frames' format
/// @param bytes the frames
/// @param frames how many to convert
/// @param[out] samples receives frames * min(channels, 2) samples
void decode_frames(
    const StreamFormat& format,
    const uint8_t* bytes,
    std::size_t frames,
    std::vector<int16_t>& samples
) {
    const uint32_t bytes_per_sample = sample_bytes(format.sample);
    const uint32_t bytes_per_frame = frame_bytes(format);
    const uint32_t kept = std::min<uint32_t>(format.channels, mixed_channels_max);
    samples.resize(frames * kept);
    int16_t* const out = samples.data();
    // The sample format is chosen once for all the frames.
    switch (format.sample) {
    case SampleFormat::u8:
        decode_frames_of<SampleFormat::u8>(
            bytes, frames, bytes_per_frame, bytes_per_sample, kept, out
        );
        return;
    case SampleFormat::s16:
        decode_frames_of<SampleFormat::s16>(
            bytes, frames, bytes_per_frame, bytes_per_sample, kept, out
        );
        return;
    case SampleFormat::s32:
        decode_frames_of<SampleFormat::s32>(
            bytes, frames, bytes_per_frame, bytes_per_sample, kept, out
        );
        return;
    case SampleFormat::f32:
        decode_frames_of<SampleFormat::f32>(
            bytes, frames, bytes_per_frame, bytes_per_sample, kept, out
        );
        return;
    }
    // A format the mixer does not know reads as silence.
    std::fill(out, out + frames * kept, int16_t{0});
}

/// Tells whether a stream format is one the mixer takes.
///
/// @param format the format
/// @return true for 1 to max_stream_channels channels at a rate the resampler converts
bool mixable(const StreamFormat& format) {
    return format.channels > 0 && format.channels <= max_stream_channels &&
           format.rate >= resampler_min_rate && format.rate <= resampler_max_rate;
}

} // namespace

/// A stream of a SoftwareMixer: its queued bytes, their conversion and its state.
class MixerStream final : public OutputStream {
  public:

    MixerStream(SoftwareMixer& mixer, const StreamFormat& format, StreamFeed feed, void* context)
        : mixer_(mixer), format_(format), feed_(feed), context_(context),
          frame_bytes_(frame_bytes(format)),
          channels_(std::min<uint32_t>(format.channels, mixed_channels_max)) {}

    /// Configures the rate conversion.
    ///
    /// @return false when the resampler cannot convert the stream's rate
    [[nodiscard]] bool configure() {
        return resampler_.configure(format_.rate, mixer_output_rate, channels_);
    }

    ~MixerStream() override {
        mixer_.lock();
        std::erase(mixer_.streams_, this);
        mixer_.unlock();
    }

    bool put(const void* bytes, int32_t count) override {
        if (count < 0)
            return false;
        Hold hold(*this);
        if (ended_) {
            resampler_.reset();
            ended_ = false;
        }
        flushed_ = false;
        const auto* first = static_cast<const uint8_t*>(bytes);
        input_.insert(input_.end(), first, first + count);
        return true;
    }

    bool flush() override {
        Hold hold(*this);
        flushed_ = true;
        return true;
    }

    int32_t queued_bytes() override {
        Hold hold(*this);
        return static_cast<int32_t>(input_.size() - input_read_);
    }

    int32_t available_bytes() override {
        Hold hold(*this);
        // The converted frames not yet mixed, and those the resampler still
        // owes the flushed input, counted in the stream's own frames, rounded up.
        const uint64_t owed = flushed_ && !ended_ ? resampler_.owed_frames() : 0;
        const uint64_t frames = converted_frames() + owed;
        return static_cast<int32_t>(
            (frames * format_.rate + mixer_output_rate - 1) / mixer_output_rate * frame_bytes_
        );
    }

    void clear() override {
        Hold hold(*this);
        input_.clear();
        input_read_ = 0;
        converted_.clear();
        converted_read_ = 0;
        resampler_.reset();
        flushed_ = false;
        ended_ = false;
    }

    bool set_gain(float gain) override {
        Hold hold(*this);
        gain_ = gain;
        update_gains();
        return true;
    }

    bool set_side_gains(float left, float right) override {
        Hold hold(*this);
        side_left_ = left;
        side_right_ = right;
        update_gains();
        return true;
    }

    bool pause() override {
        Hold hold(*this);
        paused_ = true;
        return true;
    }

    bool resume() override {
        Hold hold(*this);
        paused_ = false;
        return true;
    }

    void lock() override { mixer_.lock(); }

    void unlock() override { mixer_.unlock(); }

    /// Adds the stream's next frames to a mix; the caller holds the lock.
    ///
    /// @param[in,out] sum the mix, frames * mixer_output_channels samples
    ///        with mixer_sum_bits fraction bits
    /// @param frames frames wanted
    void add_to(int32_t* sum, uint32_t frames) {
        if (paused_)
            return;
        uint32_t feed_calls = 0;
        while (converted_frames() < frames) {
            const std::size_t whole = (input_.size() - input_read_) / frame_bytes_;
            if (whole > 0) {
                convert(static_cast<uint32_t>(std::min<std::size_t>(whole, mixer_convert_frames)));
                continue;
            }
            if (feed_ != nullptr && feed_calls < mixer_feed_calls) {
                ++feed_calls;
                // Enough input for the frames still missing, and for the
                // frames the rate conversion holds back.
                const uint64_t missing = frames - converted_frames();
                const uint64_t input_frames =
                    (missing * format_.rate + mixer_output_rate - 1) / mixer_output_rate +
                    (resampler_.passthrough() ? 0 : pcm_resampler_zero_crossings * 2);
                const std::size_t before = input_.size() - input_read_;
                feed_(context_, *this, static_cast<int32_t>(input_frames * frame_bytes_));
                if (input_.size() - input_read_ > before)
                    continue;
            }
            if (flushed_ && !ended_) {
                resampler_.finish(converted_);
                ended_ = true;
                continue;
            }
            break;
        }
        const std::size_t take = std::min<std::size_t>(converted_frames(), frames);
        if (take == 0)
            return;
        const int32_t* from = converted_.data() + converted_read_;
        const int32_t left = left_gain_;
        const int32_t right = right_gain_;
        // A sample times a gain fits 64 bits; an arithmetic shift rounds it
        // toward negative infinity.
        if (channels_ == 1) {
            for (std::size_t i = 0; i < take; ++i, sum += 2) {
                const int64_t sample = from[i];
                sum[0] += static_cast<int32_t>((sample * left) >> gain_to_sum_bits);
                sum[1] += static_cast<int32_t>((sample * right) >> gain_to_sum_bits);
            }
        } else {
            for (std::size_t i = 0; i < take; ++i, sum += 2) {
                sum[0] += static_cast<int32_t>((int64_t{from[2 * i]} * left) >> gain_to_sum_bits);
                sum[1] +=
                    static_cast<int32_t>((int64_t{from[2 * i + 1]} * right) >> gain_to_sum_bits);
            }
        }
        converted_read_ += take * channels_;
        if (converted_read_ == converted_.size()) {
            converted_.clear();
            converted_read_ = 0;
        } else if (converted_read_ > compact_after_samples) {
            converted_.erase(
                converted_.begin(),
                converted_.begin() + static_cast<std::ptrdiff_t>(converted_read_)
            );
            converted_read_ = 0;
        }
    }

  private:

    [[nodiscard]] std::size_t converted_frames() const {
        return (converted_.size() - converted_read_) / channels_;
    }

    // Recomputes the sides' gains at mixer_gain_bits fraction bits.
    void update_gains() {
        left_gain_ = fixed_gain(gain_ * side_left_);
        right_gain_ = fixed_gain(gain_ * side_right_);
    }

    // Converts queued input frames to 16-bit samples and on to the output rate.
    void convert(uint32_t frames) {
        decode_frames(format_, input_.data() + input_read_, frames, decoded_);
        input_read_ += static_cast<std::size_t>(frames) * frame_bytes_;
        if (input_read_ == input_.size()) {
            input_.clear();
            input_read_ = 0;
        }
        resampler_.process(decoded_, converted_);
    }

    SoftwareMixer& mixer_;
    StreamFormat format_{};
    StreamFeed feed_{};
    void* context_{};
    uint32_t frame_bytes_{};
    uint32_t channels_{}; ///< channels mixed: the stream's first one or two
    PcmResampler resampler_;
    std::vector<uint8_t> input_; ///< bytes put, from input_read_ on not yet converted
    std::size_t input_read_{};
    std::vector<int16_t> decoded_; ///< one step's frames before rate conversion
    std::vector<int32_t>
        converted_; ///< frames at the output rate, from converted_read_ on not yet mixed
    std::size_t converted_read_{};
    float gain_{1.0F};
    float side_left_{1.0F};
    float side_right_{1.0F};
    int32_t left_gain_{fixed_gain(1.0F)};  ///< gain_ * side_left_, at mixer_gain_bits
    int32_t right_gain_{fixed_gain(1.0F)}; ///< gain_ * side_right_, at mixer_gain_bits
    bool paused_{true};
    bool flushed_{}; ///< the queued input is complete
    bool ended_{};   ///< the resampler has given the frames after the flushed input
};

SoftwareMixer::SoftwareMixer(MixerLock lock) : lock_(lock) {
}

SoftwareMixer::~SoftwareMixer() = default;

std::unique_ptr<OutputStream> SoftwareMixer::open_stream(
    const StreamFormat& format, StreamFeed feed, void* context, std::string& error
) {
    if (format.channels == 0 || format.channels > max_stream_channels) {
        error = "unsupported channel count " + std::to_string(format.channels);
        return nullptr;
    }
    auto stream = std::make_unique<MixerStream>(*this, format, feed, context);
    if (!stream->configure()) {
        error = "unsupported sample rate " + std::to_string(format.rate);
        return nullptr;
    }
    lock();
    streams_.push_back(stream.get());
    mixing_.reserve(streams_.size());
    unlock();
    return stream;
}

void SoftwareMixer::mix(int16_t* out, uint32_t frames) {
    lock();
    sum_.assign(static_cast<std::size_t>(frames) * mixer_output_channels, 0);
    // A feed may open or close streams; mix the ones open when the call began.
    // A stream a feed opens may move the list's room, so it is read by index.
    mixing_.assign(streams_.begin(), streams_.end());
    const std::size_t count = mixing_.size();
    for (std::size_t index = 0; index < count; ++index) {
        MixerStream* stream = mixing_[index];
        if (std::find(streams_.begin(), streams_.end(), stream) != streams_.end())
            stream->add_to(sum_.data(), frames);
    }
    unlock();
    // An arithmetic shift rounds toward negative infinity; with the half
    // added first, the sum rounds to nearest.
    for (std::size_t i = 0; i < sum_.size(); ++i)
        out[i] = static_cast<int16_t>(
            std::clamp((sum_[i] + sum_rounding) >> mixer_sum_bits, sample_min, sample_max)
        );
}

std::size_t SoftwareMixer::stream_count() {
    lock();
    const std::size_t count = streams_.size();
    unlock();
    return count;
}

void SoftwareMixer::lock() const {
    if (lock_.lock != nullptr)
        lock_.lock(lock_.context);
}

void SoftwareMixer::unlock() const {
    if (lock_.unlock != nullptr)
        lock_.unlock(lock_.context);
}

uint64_t converted_bytes(const StreamFormat& format, std::size_t input_bytes) noexcept {
    if (!mixable(format))
        return 0;
    const uint32_t channels = std::min<uint32_t>(format.channels, mixed_channels_max);
    const uint64_t frames = input_bytes / frame_bytes(format);
    const uint64_t converted = (frames * mixer_output_rate + format.rate - 1) / format.rate + 1;
    return converted * channels * sizeof(int16_t);
}

uint32_t convert_for_mixer(
    const StreamFormat& format, std::span<const uint8_t> bytes, std::vector<int16_t>& samples
) {
    samples.clear();
    PcmResampler resampler;
    const uint32_t channels = std::min<uint32_t>(format.channels, mixed_channels_max);
    if (!mixable(format) || !resampler.configure(format.rate, mixer_output_rate, channels))
        return 0;
    const std::size_t frames = bytes.size() / frame_bytes(format);
    std::vector<int16_t> decoded;
    decode_frames(format, bytes.data(), frames, decoded);
    std::vector<int32_t> converted;
    // ceil(frames * output rate / input rate) frames, and a frame to spare.
    converted.reserve(
        static_cast<std::size_t>(
            (static_cast<uint64_t>(frames) * mixer_output_rate + format.rate - 1) / format.rate + 1
        ) *
        channels
    );
    resampler.process(decoded, converted);
    resampler.finish(converted);
    samples.resize(converted.size());
    // An arithmetic shift rounds toward negative infinity; with one added
    // first, the half rounds to nearest.
    for (std::size_t i = 0; i < converted.size(); ++i)
        samples[i] =
            static_cast<int16_t>(std::clamp((converted[i] + 1) >> 1, sample_min, sample_max));
    return channels;
}

} // namespace oa::audio
