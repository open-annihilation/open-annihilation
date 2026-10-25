// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The files a director script's render writes (docs/director.md), and the
// bundle (.oamovie) that carries a script with its recording.
//
// A render of a script named NAME.oascript writes, in its output directory:
//
//   NAME-000.mp4 ...    each chunk's video with its sound, when encoding is on
//   NAME-000.wav ...    each chunk's sound: 16-bit stereo PCM at 48 kHz
//   NAME-000.frames ... each chunk's frames, one "<frame> <tick> <sha256>" line
//                       a frame, the frame's RGB bytes hashed
//   NAME.mp4            the chunks joined, when every chunk was rendered and
//                       encoding is on
//   NAME.manifest       what was rendered and the hashes of every chunk's
//                       frames and sound, when every chunk was rendered
//
// and, for each frame --stills names, NAME-still-000000.png ...: the frame as
// drawn, an 8-bit RGB PNG, its number with at least still_number_digits
// digits.
//
// While they are needed, each chunk's encoded frames (NAME-000.video.mp4),
// the chunks' sound joined (NAME.wav) and the list of chunks to join
// (NAME.concat.txt) are kept beside them too; they are removed once the
// render is done, and a render that stops on an error leaves them.
//
// Chunk numbers have at least chunk_number_digits digits. The encoder is the
// ffmpeg program: each chunk's frames go as raw RGB through a pipe to it,
// which makes H.264 (libx264, High profile, 4:2:0, BT.709, constant quality
// 18, closed groups of half a second of frames); the chunk's sound is then
// joined to them as AAC at 48 kHz and 384 kbit/s. The joined video copies the
// chunks' frames and encodes the joined sound once. Setting the environment
// variable director_encoder_variable to director_encoder_off renders and
// hashes every frame and writes every file but the videos, running no
// encoder.
#pragma once

#include "oa/base/sha256.hpp"
#include "oa/formats/oascript.hpp"
#include "oa/formats/zip.hpp"

#include <SDL3/SDL.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app {

/// The program that encodes a director render, looked up on PATH.
inline constexpr const char* director_encoder = "ffmpeg";
/// The environment variable that names libx264's preset, such as veryfast.
inline constexpr const char* director_preset_variable = "OA_DIRECTOR_PRESET";
/// The preset when that variable is unset or empty.
inline constexpr const char* director_default_preset = "medium";
/// The environment variable that turns encoding off when it holds
/// director_encoder_off.
inline constexpr const char* director_encoder_variable = "OA_DIRECTOR_ENCODER";
/// The value of director_encoder_variable that turns encoding off.
inline constexpr const char* director_encoder_off = "none";
/// The fewest digits of a chunk's number in its files' names.
inline constexpr size_t chunk_number_digits = 3;
/// The fewest digits of a frame's number in a still's name.
inline constexpr size_t still_number_digits = 6;
/// Sample frames a second of a render's sound.
inline constexpr uint32_t director_sample_rate = 48000;
/// Channels of a render's sound: left and right.
inline constexpr uint32_t director_channels = 2;

/// Where a render's files go: a directory and the stem of their names.
struct DirectorPaths {
    std::filesystem::path directory{};
    std::string stem{}; ///< UTF-8
};

/// Returns a chunk's name: the stem, a hyphen and the chunk's number with at
/// least chunk_number_digits digits.
///
/// @param paths the render's paths
/// @param chunk the chunk, counted from 0
/// @return the name, such as game-007
[[nodiscard]] std::string chunk_stem(const DirectorPaths& paths, uint32_t chunk);

/// Returns the file a chunk's frames are encoded into before its sound joins
/// them: <chunk stem>.video.mp4, removed once the render is done.
///
/// @param paths the render's paths
/// @param chunk the chunk, counted from 0
/// @return the path
[[nodiscard]] std::filesystem::path
chunk_video_only_path(const DirectorPaths& paths, uint32_t chunk);

/// Returns a chunk's video with its sound: <chunk stem>.mp4.
///
/// @param paths the render's paths
/// @param chunk the chunk, counted from 0
/// @return the path
[[nodiscard]] std::filesystem::path chunk_video_path(const DirectorPaths& paths, uint32_t chunk);

/// Returns a chunk's sound: <chunk stem>.wav.
///
/// @param paths the render's paths
/// @param chunk the chunk, counted from 0
/// @return the path
[[nodiscard]] std::filesystem::path chunk_audio_path(const DirectorPaths& paths, uint32_t chunk);

/// Returns a chunk's frame manifest: <chunk stem>.frames.
///
/// @param paths the render's paths
/// @param chunk the chunk, counted from 0
/// @return the path
[[nodiscard]] std::filesystem::path chunk_manifest_path(const DirectorPaths& paths, uint32_t chunk);

/// Returns the joined sound: <stem>.wav.
///
/// @param paths the render's paths
/// @return the path
[[nodiscard]] std::filesystem::path joined_audio_path(const DirectorPaths& paths);

/// Returns the joined video: <stem>.mp4.
///
/// @param paths the render's paths
/// @return the path
[[nodiscard]] std::filesystem::path joined_video_path(const DirectorPaths& paths);

/// Returns the run manifest: <stem>.manifest.
///
/// @param paths the render's paths
/// @return the path
[[nodiscard]] std::filesystem::path run_manifest_path(const DirectorPaths& paths);

/// Returns the list of chunk videos the joined video is made from:
/// <stem>.concat.txt, removed once the render is done.
///
/// @param paths the render's paths
/// @return the path
[[nodiscard]] std::filesystem::path concat_list_path(const DirectorPaths& paths);

/// Returns a still of a frame: <stem>-still-<frame>.png, the frame's number
/// with at least still_number_digits digits.
///
/// @param paths the render's paths
/// @param frame the frame, counted from 0
/// @return the path, such as out/game-still-000120.png
[[nodiscard]] std::filesystem::path still_path(const DirectorPaths& paths, uint64_t frame);

/// Writes a frame as a still: an 8-bit RGB PNG of it as drawn, without
/// loss, at still_path; the directory is made when it is missing.
///
/// Throws std::logic_error when the frame is not width * height * 3 bytes,
/// and std::runtime_error when the file cannot be written.
///
/// @param paths the render's paths
/// @param frame the frame, counted from 0
/// @param rgb the frame's bytes, row by row from the top
/// @param width frame width in pixels
/// @param height frame height in pixels
/// @return the file written
std::filesystem::path write_still(
    const DirectorPaths& paths,
    uint64_t frame,
    std::span<const uint8_t> rgb,
    uint32_t width,
    uint32_t height
);

/// Returns how many frames make half a second, the encoder's group length.
///
/// @param framerate frames a second, above zero
/// @return floor(framerate / 2), at least 1
[[nodiscard]] uint64_t half_second_frames(oa::formats::oascript::Decimal framerate) noexcept;

/// Returns the encoder's arguments that encode one chunk's frames, raw RGB
/// on its standard input, into `video_only`.
///
/// @param video_only the file to make
/// @param width frame width in pixels, even
/// @param height frame height in pixels, even
/// @param framerate frames a second
/// @param preset libx264's preset
/// @return the arguments, the program's name first
[[nodiscard]] std::vector<std::string> chunk_encoder_arguments(
    const std::filesystem::path& video_only,
    uint32_t width,
    uint32_t height,
    oa::formats::oascript::Decimal framerate,
    std::string_view preset
);

/// Returns the encoder's arguments that join a chunk's encoded frames and
/// its sound into `out`: the frames as they are, the sound as AAC. The sound
/// holds exactly the chunk's samples, so nothing is cut to the shorter one.
///
/// @param video_only the chunk's encoded frames
/// @param wav the chunk's sound
/// @param out the file to make
/// @return the arguments, the program's name first
[[nodiscard]] std::vector<std::string> chunk_mux_arguments(
    const std::filesystem::path& video_only,
    const std::filesystem::path& wav,
    const std::filesystem::path& out
);

/// Returns the encoder's arguments that join every chunk's encoded frames,
/// as they are, and the joined sound, encoded once as AAC, into `out`.
///
/// @param concat_list the list concat_list_text wrote
/// @param joined_wav the joined sound
/// @param out the file to make
/// @return the arguments, the program's name first
[[nodiscard]] std::vector<std::string> stitch_arguments(
    const std::filesystem::path& concat_list,
    const std::filesystem::path& joined_wav,
    const std::filesystem::path& out
);

/// Returns the list the encoder's concat demuxer reads: a "file '<path>'"
/// line for each file, a single quote in a path written as '\''.
///
/// @param files the files, in order
/// @return the list's text
[[nodiscard]] std::string concat_list_text(std::span<const std::filesystem::path> files);

/// Returns a frame manifest's line.
///
/// @param frame the frame, counted from 0
/// @param tick the tick it shows
/// @param digest the SHA-256 of its RGB bytes
/// @return "<frame> <tick> <sha256 in lower-case hex>\n"
[[nodiscard]] std::string
manifest_line(uint64_t frame, uint32_t tick, const oa::base::sha256::Digest& digest);

/// Returns a digest's lower-case hex spelling.
///
/// @param digest the digest
/// @return 64 hex digits
[[nodiscard]] std::string digest_text(const oa::base::sha256::Digest& digest);

/// How a render encodes its chunks.
struct EncoderSettings {
    bool enabled{true}; ///< false: no encoder runs and no video is made
    std::string preset{director_default_preset};
};

/// Reads the encoder settings from the environment: director_encoder_variable
/// and director_preset_variable.
///
/// Throws std::runtime_error for a preset that is not one word of lower-case
/// letters.
///
/// @return the settings
[[nodiscard]] EncoderSettings encoder_settings_from_environment();

/// Runs the encoder with some arguments and waits for it.
///
/// Throws std::runtime_error when it cannot be started or ends with a
/// non-zero status.
///
/// @param arguments the arguments, the program's name first
void run_encoder(const std::vector<std::string>& arguments);

/// The encoder running on one chunk's frames, which it reads from a pipe.
class EncoderPipe {
  public:

    /// Starts the encoder.
    ///
    /// Throws std::runtime_error when it cannot be started.
    ///
    /// @param arguments the arguments, the program's name first
    explicit EncoderPipe(const std::vector<std::string>& arguments);

    /// Ends an encoder that was not finished, and waits for it.
    ~EncoderPipe();

    EncoderPipe(const EncoderPipe&) = delete;
    EncoderPipe& operator=(const EncoderPipe&) = delete;

    /// Writes one frame, whole.
    ///
    /// Throws std::runtime_error when the encoder stops taking frames.
    ///
    /// @param bytes the frame
    void write(std::span<const uint8_t> bytes);

    /// Ends the frames and waits for the encoder.
    ///
    /// Throws std::runtime_error when it ends with a non-zero status.
    void finish();

  private:

    /// Closes the pipe and waits for the encoder, whatever its status.
    ///
    /// @return the encoder's exit status; -1 when it did not exit
    int release() noexcept;

    SDL_Process* process_{};
    SDL_IOStream* input_{};
};

/// A chunk that was rendered: its frames, its sound and their hashes.
struct ChunkSummary {
    uint32_t chunk{};
    uint64_t first_frame{};
    uint64_t frame_count{};
    uint32_t first_tick{};                      ///< the tick its first frame shows
    uint32_t last_tick{};                       ///< the tick its last frame shows
    uint64_t sample_frames{};                   ///< stereo sample frames of its sound
    oa::base::sha256::Digest manifest_digest{}; ///< of its frame manifest's bytes
    oa::base::sha256::Digest pcm_digest{};      ///< of its sound's PCM bytes
};

/// What a render's manifest says besides its chunks.
struct RunDescription {
    std::string engine_version{};
    std::string script_name{};    ///< UTF-8
    std::string recording_name{}; ///< UTF-8
    oa::base::sha256::Digest script_digest{};
    oa::base::sha256::Digest recording_digest{};
    uint32_t width{};
    uint32_t height{};
    oa::formats::oascript::Decimal framerate{};
    oa::formats::oascript::Decimal tickrate{};
    uint32_t first_tick{};
    uint32_t end_tick{};
    uint64_t frame_count{};
    uint32_t chunk_count{};
    /// The driver of the graphics card whose Full tier drew the frames, as
    /// SDL names it; empty when the processor drew them.
    std::string graphics_card{};
};

/// Returns the run manifest's text: one line for each fact, one for each
/// chunk and one for the whole sound's hash. A render the graphics card drew
/// has a line saying so after the output's, whose frames are not pinned; a
/// render the processor drew has none.
///
/// @param run what was rendered
/// @param chunks every chunk, in order
/// @param pcm_digest the SHA-256 of every chunk's PCM bytes, in order
/// @return the text, '\n' line ends
[[nodiscard]] std::string run_manifest_text(
    const RunDescription& run,
    std::span<const ChunkSummary> chunks,
    const oa::base::sha256::Digest& pcm_digest
);

/// Where a render writes and how it encodes.
struct DirectorOutputSettings {
    DirectorPaths paths{};
    uint32_t width{};  ///< even
    uint32_t height{}; ///< even
    oa::formats::oascript::Decimal framerate{};
    EncoderSettings encoder{};
    /// Every chunk is rendered: the sound is also joined, and the joined
    /// video is made when encoding is on.
    bool joined{};
};

/// Writes a render's files, chunk by chunk, in order.
///
/// Frames and sound are handed to the open chunk; the chunk's files are
/// finished when it ends. Frames are hashed on worker threads, several at
/// once, and their manifest lines written in frame order. A chunk that was
/// begun and not ended when the output goes, as when the render stops on an
/// error, keeps what was written of it.
class DirectorOutput {
  public:

    /// Makes the output directory.
    ///
    /// Throws std::runtime_error when it cannot be made, or for an odd size.
    ///
    /// @param settings where and how the render writes
    explicit DirectorOutput(DirectorOutputSettings settings);

    /// Ends an open chunk's encoder.
    ~DirectorOutput();

    DirectorOutput(const DirectorOutput&) = delete;
    DirectorOutput& operator=(const DirectorOutput&) = delete;

    /// Opens a chunk's files and, when encoding is on, its encoder.
    ///
    /// Throws std::logic_error while a chunk is open, and std::runtime_error
    /// when a file cannot be written or the encoder not started.
    ///
    /// @param chunk the chunk, counted from 0; above the chunks begun before
    /// @param first_frame its first frame
    void begin_chunk(uint32_t chunk, uint64_t first_frame);

    /// Adds the open chunk's next frame: its manifest line and, when
    /// encoding is on, the frame itself to the encoder.
    ///
    /// Throws std::logic_error when no chunk is open or the frame is not the
    /// next one or not width * height * 3 bytes, and std::runtime_error when
    /// a file or the encoder cannot take it.
    ///
    /// @param frame the frame, counted from 0
    /// @param tick the tick it shows
    /// @param rgb its bytes, row by row
    void add_frame(uint64_t frame, uint32_t tick, std::span<const uint8_t> rgb);

    /// Adds the open chunk's next sound.
    ///
    /// Throws std::logic_error when no chunk is open or the samples are not
    /// whole sample frames, and std::runtime_error when a file cannot take
    /// them.
    ///
    /// @param samples interleaved left and right samples
    void add_samples(std::span<const int16_t> samples);

    /// Ends the open chunk: its manifest and sound are closed and, when
    /// encoding is on, the encoder finished and the sound joined to the
    /// frames.
    ///
    /// Throws std::logic_error when no chunk is open and
    /// std::runtime_error when a file or the encoder fails.
    ///
    /// @return the chunk's summary
    const ChunkSummary& end_chunk();

    /// Ends the render: when every chunk was rendered, closes the joined
    /// sound, joins the chunk videos with it when encoding is on and writes
    /// the run manifest; then removes the files kept only for joining.
    ///
    /// Throws std::logic_error while a chunk is open and std::runtime_error
    /// when a file or the encoder fails.
    ///
    /// @param run what was rendered
    void finish(const RunDescription& run);

    /// Returns the chunks ended so far, in order.
    ///
    /// @return the summaries
    [[nodiscard]] const std::vector<ChunkSummary>& chunks() const noexcept;

    /// Returns the SHA-256 of every ended chunk's PCM bytes, in order.
    ///
    /// @return the digest
    [[nodiscard]] oa::base::sha256::Digest pcm_digest() const noexcept;

  private:

    /// The frames being hashed, in frame order, and the threads that hash
    /// them (director_output.cpp).
    struct FrameHashing;

    /// Writes the manifest lines of the frames whose hashes are done, oldest
    /// first, waiting for the oldest while more than `in_flight` remain.
    ///
    /// Throws std::runtime_error when the manifest cannot be written.
    ///
    /// @param in_flight the most frames left being hashed
    void retire_frames(size_t in_flight);

    DirectorOutputSettings settings_{};
    std::unique_ptr<FrameHashing> hashing_{};
    bool open_{};
    bool finished_{};
    ChunkSummary current_{};
    uint64_t next_frame_{};
    std::ofstream manifest_{};
    std::ofstream wave_{};
    oa::base::sha256::Hasher manifest_hash_{};
    oa::base::sha256::Hasher pcm_hash_{};
    std::unique_ptr<EncoderPipe> encoder_{};
    std::ofstream joined_wave_{};
    uint64_t joined_sample_frames_{};
    oa::base::sha256::Hasher joined_pcm_hash_{};
    std::vector<ChunkSummary> chunks_{};
    std::vector<uint8_t> little_endian_{}; ///< samples as little-endian bytes
};

/// Returns how a script names its recording (input.demo): the recording's
/// path from the script's folder when the recording lies in that folder or
/// below it, else its absolute path, '/' separating directories in both.
///
/// Both paths are made absolute and have their links resolved as far as
/// they exist before they are compared.
///
/// @param recording the recording's path
/// @param script the script's path; the script need not exist yet
/// @return the name, UTF-8
[[nodiscard]] std::string
script_recording_key(const std::filesystem::path& recording, const std::filesystem::path& script);

/// A director bundle's script, and the archive's entries.
struct Bundle {
    std::string script_name{};           ///< the script's entry name
    std::vector<uint8_t> script_bytes{}; ///< the script's text
    oa::formats::zip::CentralDirectory directory{};
};

/// Reads a bundle's script: the archive must hold exactly one entry at its
/// root whose name ends in .oascript.
///
/// Throws std::runtime_error saying what is wrong with the archive.
///
/// @param archive the bundle's bytes
/// @return the script and the archive's entries
[[nodiscard]] Bundle read_bundle(std::span<const uint8_t> archive);

/// Reads one entry of a bundle: the recording its script names.
///
/// Throws std::runtime_error when the name is not a safe entry name, the
/// archive has no such entry, or the entry cannot be read.
///
/// @param archive the bundle's bytes
/// @param bundle what read_bundle read from them
/// @param name the entry's name, '/' separating directories
/// @return the entry's bytes
[[nodiscard]] std::vector<uint8_t>
read_bundle_entry(std::span<const uint8_t> archive, const Bundle& bundle, std::string_view name);

/// Writes a bundle: the script, then the recording, both stored, so the same
/// script and recording always give the same bytes.
///
/// Throws std::runtime_error when a name is not safe, the two names are the
/// same, or an entry is too large.
///
/// @param script_name the script's entry name, ending in .oascript
/// @param script_text the script's text
/// @param recording_name the recording's entry name, as the script's demo key gives it
/// @param recording_bytes the recording
/// @return the bundle's bytes
[[nodiscard]] std::vector<uint8_t> write_bundle(
    std::string_view script_name,
    std::string_view script_text,
    std::string_view recording_name,
    std::span<const uint8_t> recording_bytes
);

} // namespace oa::app
