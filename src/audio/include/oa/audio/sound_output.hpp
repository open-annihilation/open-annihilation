// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// The sound output every sound of the game plays through: effects, speech,
// music and the movies' sound. A stream takes samples in its own format and
// rate; the output converts and mixes the streams and plays them on the
// system's sound device. Two outputs exist: SDL's, wherever the game uses
// SDL, and a mixer of the engine's own over the Windows wave-out device,
// for systems SDL does not run on.

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace oa::audio {

/// How the samples of a stream are stored; every format is little-endian.
enum class SampleFormat : uint8_t {
    u8,  ///< 8-bit unsigned, silence at 128
    s16, ///< 16-bit signed
    s32, ///< 32-bit signed
    f32, ///< 32-bit float, -1..1
};

/// The samples a stream takes: their format, interleaved channels and rate.
struct StreamFormat {
    SampleFormat sample{SampleFormat::s16};
    uint8_t channels{};
    uint32_t rate{}; ///< frames per second
};

/// Most channels a stream may have.
inline constexpr uint8_t max_stream_channels = 8;

/// Returns the bytes of one sample of a format.
///
/// @param sample the sample format
/// @return 1, 2 or 4
[[nodiscard]] uint32_t sample_bytes(SampleFormat sample) noexcept;

/// Returns the bytes of one frame of a stream format: one sample per channel.
///
/// @param format the stream format
/// @return the frame size in bytes
[[nodiscard]] uint32_t frame_bytes(const StreamFormat& format) noexcept;

/// Returns the byte that fills a buffer of silence in a sample format.
///
/// @param sample the sample format
/// @return 0x80 for 8-bit unsigned samples, 0 otherwise
[[nodiscard]] uint8_t silence_byte(SampleFormat sample) noexcept;

class OutputStream;

/// Asks for more of a stream's samples. Runs on the output's own thread with
/// the stream locked, when the samples queued run short.
///
/// @param context the context the stream was opened with
/// @param stream the stream to put samples to
/// @param wanted_bytes about how many bytes in the stream's format would fill it
using StreamFeed = void (*)(void* context, OutputStream& stream, int32_t wanted_bytes);

/// One stream of samples, mixed with the output's other streams.
///
/// A stream opens paused. Its samples play in the order they are put; a
/// stream with a feed is asked for more as it plays. Destroying the stream
/// stops it at once.
class OutputStream {
  public:

    virtual ~OutputStream() = default;

    /// Queues samples to play after those already queued.
    ///
    /// @param bytes the samples, in the stream's format
    /// @param count bytes to queue; whole frames
    /// @return false when the samples cannot be queued
    [[nodiscard]] virtual bool put(const void* bytes, int32_t count) = 0;

    /// Marks the queued samples as complete, so the last of them play out
    /// without waiting for samples that follow.
    ///
    /// @return false when the stream cannot be flushed
    virtual bool flush() = 0;

    /// Returns the bytes put and not yet taken for playing.
    ///
    /// @return the byte count in the stream's format
    [[nodiscard]] virtual int32_t queued_bytes() = 0;

    /// Returns how much has been converted for playing and not yet played.
    ///
    /// @return the bytes of the stream's format those samples stand for, 0
    ///         when nothing waits to play
    [[nodiscard]] virtual int32_t available_bytes() = 0;

    /// Drops every sample queued or converted and not yet played.
    virtual void clear() = 0;

    /// Sets the factor every sample is multiplied by.
    ///
    /// @param gain the factor; 1 plays the samples as they are
    /// @return false when the gain cannot be set
    virtual bool set_gain(float gain) = 0;

    /// Sets the factors the left and right sides are multiplied by, on top
    /// of the gain: a stream of one channel plays it on both sides at
    /// these levels. A stream starts with both at 1.
    ///
    /// An output that cannot weigh the sides apart keeps playing both at the
    /// gain alone and returns false.
    ///
    /// @param left the left side's factor; 1 plays it as it is
    /// @param right the right side's factor; 1 plays it as it is
    /// @return false when the sides cannot be set
    virtual bool set_side_gains(float left, float right) {
        static_cast<void>(left);
        static_cast<void>(right);
        return false;
    }

    /// Stops playing, keeping the samples queued.
    ///
    /// @return false when the stream cannot be paused
    virtual bool pause() = 0;

    /// Starts or continues playing.
    ///
    /// @return false when the stream cannot play
    virtual bool resume() = 0;

    /// Holds off the output's thread, and so the stream's feed, until unlock().
    ///
    /// A thread that holds the lock may take it again.
    virtual void lock() = 0;

    /// Releases one lock().
    virtual void unlock() = 0;
};

/// The system's sound device, with the streams it mixes.
class SoundOutput {
  public:

    virtual ~SoundOutput() = default;

    /// Starts the sound device, or counts one more user when it has started.
    ///
    /// @param[out] error why the device cannot start; untouched on success
    /// @return true when streams can be opened
    [[nodiscard]] virtual bool start(std::string& error) = 0;

    /// Counts one user less, stopping the device after the last.
    virtual void stop() = 0;

    /// Tells whether the device has started.
    ///
    /// @return true between the first start() and the last stop()
    [[nodiscard]] virtual bool started() const = 0;

    /// Opens a paused stream.
    ///
    /// @param format the samples the stream takes
    /// @param feed asks for more samples as the stream plays; null for a
    ///        stream that plays what is put to it
    /// @param context passed back to the feed
    /// @param[out] error why the stream cannot be opened; untouched on success
    /// @return the stream, or null on failure
    [[nodiscard]] virtual std::unique_ptr<OutputStream>
    open_stream(const StreamFormat& format, StreamFeed feed, void* context, std::string& error) = 0;

    /// Names the system interface the device plays through, for diagnostics.
    ///
    /// @return the name, or "" when the device has not started
    [[nodiscard]] virtual std::string driver_name() const = 0;

    /// Returns the reason for the last call that failed, for diagnostics.
    ///
    /// @return the reason, "" when none is known
    [[nodiscard]] virtual std::string last_error() const = 0;

    /// Returns lines for the log about the device: which one the output
    /// opened and in what format, once each time it opens one, and, at most
    /// once every late_feed_report_ms, how often the device asked for sound
    /// late (describe_late_feeds). Called on the thread that opens streams.
    ///
    /// @return the lines, each ending in a line feed; "" when there is
    ///         nothing new to say
    [[nodiscard]] virtual std::string take_log() { return {}; }
};

/// How long the device may ask for sound after the sound it held ran out,
/// as far as the times of its requests tell, before the request counts as
/// late: allowance for the jitter of its thread, in nanoseconds.
inline constexpr uint64_t late_feed_margin_ns = 5'000'000;
/// Least time between two reports of late feeds in the log, in milliseconds.
inline constexpr uint32_t late_feed_report_ms = 60'000;

/// What the device's requests for sound showed since the last report.
struct FeedReport {
    uint32_t feeds{};      ///< requests the device made
    uint32_t late_feeds{}; ///< of those, requests made after its sound would have run out
    /// The longest the device went without sound before a late request, at
    /// least, in microseconds.
    uint32_t longest_silence_us{};
    uint32_t longest_mix_us{};  ///< the longest a request took to mix
    uint32_t longest_wait_us{}; ///< the longest a mix waited for the streams' lock
};

/// Times the device's requests for sound and the mixes that answer them.
///
/// The device asks for sound as it plays out what it holds: at most what the
/// last request gave it and one period of its own. A request made later
/// than that, by more than late_feed_margin_ns, finds the device has gone
/// without sound for at least the difference, heard as a click or a
/// crackle. note_feed runs on the device's thread and take on another.
class FeedTimer {
  public:

    /// Forgets the last request, as a device opens.
    void restart() noexcept;

    /// Notes one request and its mix.
    ///
    /// @param asked_ns when the device asked, on a steady clock in nanoseconds
    /// @param locked_ns when the mix had taken the streams' lock
    /// @param done_ns when the mix was handed to the device
    /// @param lasts_ns how long the sound handed to the device plays
    /// @param period_ns how long the sound the device takes at a time plays
    void note_feed(
        uint64_t asked_ns,
        uint64_t locked_ns,
        uint64_t done_ns,
        uint64_t lasts_ns,
        uint64_t period_ns
    ) noexcept;

    /// Returns what the requests showed since the last call, and starts afresh.
    ///
    /// @return the report
    [[nodiscard]] FeedReport take() noexcept;

  private:

    // Only the device's thread reads and writes these.
    uint64_t last_asked_ns_{}; ///< when the last request came
    uint64_t last_lasts_ns_{}; ///< how long the sound it was given plays
    bool fed_{};               ///< a request came since the device opened
    // Read and reset by take().
    std::atomic<uint32_t> feeds_{};
    std::atomic<uint32_t> late_feeds_{};
    std::atomic<uint32_t> longest_silence_us_{};
    std::atomic<uint32_t> longest_mix_us_{};
    std::atomic<uint32_t> longest_wait_us_{};
};

/// Words a report of late requests for the log.
///
/// @param report what the requests showed
/// @param seconds how long the report covers
/// @return one line ending in a line feed, starting "sound output: "; ""
///         when no request was late
[[nodiscard]] std::string describe_late_feeds(const FeedReport& report, uint32_t seconds);

/// Returns the sound output of the process.
///
/// Unless set_sound_output() chose another, it is the one
/// choose_sound_output() (oa/audio/sound_output_backends.hpp) picks: on
/// Windows before Vista the wave-out mixer, elsewhere SDL's, and the
/// environment variable OA_SOUND_OUTPUT set to "waveout" or "sdl" chooses
/// either. An output the build or the system lacks gives way to the other.
///
/// @return the output, which lives until the process ends
[[nodiscard]] SoundOutput& sound_output();

/// Makes every later sound_output() call return another output, as a test's.
///
/// @param output the output to use, which must outlive its use; null
///        returns to the process's own
void set_sound_output(SoundOutput* output) noexcept;

} // namespace oa::audio
