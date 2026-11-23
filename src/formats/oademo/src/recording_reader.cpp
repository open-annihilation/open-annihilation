// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/formats/oademo/recording_reader.hpp"

#include "detail.hpp"

#include "oa/base/sha256.hpp"
#include "oa/base/zstd/zstd.hpp"
#include "oa/formats/oademo/interface_words.hpp"
#include "oa/formats/zip.hpp"
#include "oa/formats/zip/stream.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::formats::oademo {

namespace detail {

zip::StreamLimits archive_limits() noexcept {
    zip::StreamLimits limits;
    limits.max_archive_bytes = most_streamed_archive_bytes;
    limits.max_entry_bytes = most_streamed_archive_bytes;
    return limits;
}

std::string load_stream_entry(
    const zip::SourceHooks& source,
    uint64_t archive_bytes,
    const zip::StreamEntry& entry,
    uint32_t cap,
    std::string_view oversize,
    std::vector<uint8_t>& bytes
) {
    if (entry.bytes > cap)
        return std::string(oversize);
    zip::ZipError error;
    if (!zip::read_stream_entry(source, archive_bytes, entry, cap, bytes, error))
        return std::string(not_a_replay);
    return {};
}

} // namespace detail

namespace {

/// The archive bytes one step of the recording entry reads: it hands on at
/// most four times as many of the entry's data.
constexpr uint64_t entry_step_bytes = 64U * 1024U;

bool keep_entry_data(void* context, std::span<const uint8_t> bytes) {
    auto& pending = *static_cast<std::vector<uint8_t>*>(context);
    pending.insert(pending.end(), bytes.begin(), bytes.end());
    return true;
}

bool hex_equal(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        unsigned char a = static_cast<unsigned char>(left[index]);
        unsigned char b = static_cast<unsigned char>(right[index]);
        if (a >= 'A' && a <= 'Z')
            a = static_cast<unsigned char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z')
            b = static_cast<unsigned char>(b - 'A' + 'a');
        if (a != b)
            return false;
    }
    return true;
}

} // namespace

struct RecordingReader::State {
    State() = default;
    State(const State&) = delete;
    State& operator=(const State&) = delete;

    /// Frees the frame's window.
    ~State() { base::zstd::frame_reader_free(frame); }

    /// Where the replay's bytes come from.
    ArchiveSource source{};
    /// The same source, as the zip reader takes it.
    zip::SourceHooks hooks{};
    /// The replay's directory.
    zip::StreamDirectory directory{};
    /// Where its recording is.
    detail::Layout layout{};
    /// Its description.
    Meta meta{};
    /// Its manifest.
    Manifest manifest{};
    /// The recording entry, being read.
    zip::EntryStream entry{};
    /// True once the entry's last byte was read and its CRC-32 checked.
    bool entry_done{};
    /// The zstd frame's window, for a stored recording.
    base::zstd::FrameReader frame{};
    /// Entry data read and not yet decoded or handed out.
    std::vector<uint8_t> pending{};
    /// How much of `pending` has been used.
    std::size_t pending_at{};
    /// The digest of the recording read so far.
    base::sha256::Hasher hasher{};
    /// Recording bytes handed out so far.
    uint64_t position{};
    /// True once the recording's end was read and it matched the description.
    bool finished{};
    /// The refusal that stopped the reads, which every later read returns.
    std::string failed{};
    /// The recording entry read again as stored, for read_stored_frame.
    zip::EntryStream stored{};
    /// True once `stored` was opened.
    bool stored_open{};
    /// True once its last byte was read and its CRC-32 checked.
    bool stored_done{};
    /// Stored bytes read and not yet handed out.
    std::vector<uint8_t> stored_pending{};
    /// How much of `stored_pending` has been handed out.
    std::size_t stored_at{};

    /// Records a refusal, which every later read returns.
    ///
    /// @param text the sentence
    /// @return the sentence
    std::string fail(std::string_view text) {
        if (failed.empty())
            failed = std::string(text);
        return failed;
    }

    /// Opens the recording entry at its first byte.
    ///
    /// @return empty, or not_a_replay
    std::string start() {
        failed.clear();
        finished = false;
        position = 0;
        pending.clear();
        pending_at = 0;
        entry_done = false;
        hasher = base::sha256::Hasher{};
        base::zstd::frame_reader_free(frame);
        entry = zip::EntryStream();
        zip::ZipError error;
        if (!entry.open(hooks, source.bytes, directory.entries[layout.recording], error))
            return fail(not_a_replay);
        if (layout.zstd_entry && base::zstd::frame_reader_begin(frame) != base::zstd::Error::none)
            return fail(recording_unreadable);
        return {};
    }

    /// Reads the entry on until `pending` holds unused bytes or the entry ends.
    ///
    /// @return false when the entry does not read
    bool refill() {
        if (pending_at == pending.size()) {
            pending.clear();
            pending_at = 0;
        }
        const zip::SinkHooks sink{&pending, keep_entry_data};
        while (!entry_done && pending_at == pending.size()) {
            zip::ZipError error;
            const zip::StreamStep step = entry.step(sink, entry_step_bytes, error);
            if (step == zip::StreamStep::failed)
                return false;
            entry_done = step == zip::StreamStep::done;
        }
        return true;
    }

    /// Reads a stored recording's frame into `out`.
    ///
    /// @param out room for the bytes
    /// @param[out] got bytes written to `out`
    /// @return empty, or the refusal
    std::string read_frame(std::span<uint8_t> out, std::size_t& got) {
        got = 0;
        while (got < out.size() && !frame.ended) {
            if (pending_at == pending.size() && !entry_done && !refill())
                return fail(not_a_replay);
            std::span<const uint8_t> input(
                pending.data() + pending_at, pending.size() - pending_at
            );
            const std::size_t offered = input.size();
            std::size_t decoded = 0;
            if (base::zstd::frame_reader_read(frame, input, out.subspan(got), decoded) !=
                base::zstd::Error::none)
                return fail(recording_unreadable);
            pending_at += offered - input.size();
            got += decoded;
            if (frame.ended)
                break;
            // A frame cut short: the entry has ended and nothing more comes out.
            if (decoded == 0 && offered == input.size() && entry_done &&
                pending_at == pending.size())
                return fail(recording_unreadable);
        }
        if (!frame.ended)
            return {};
        // The frame is the whole entry: a byte after it is damage.
        if (pending_at != pending.size())
            return fail(recording_unreadable);
        while (!entry_done) {
            if (!refill())
                return fail(not_a_replay);
            if (pending_at != pending.size())
                return fail(recording_unreadable);
        }
        return {};
    }

    /// Reads a deflated recording into `out`.
    ///
    /// @param out room for the bytes
    /// @param[out] got bytes written to `out`
    /// @return empty, or the refusal
    std::string read_plain(std::span<uint8_t> out, std::size_t& got) {
        got = 0;
        while (got < out.size()) {
            if (pending_at == pending.size()) {
                if (entry_done)
                    break;
                if (!refill())
                    return fail(not_a_replay);
                continue;
            }
            const std::size_t take = std::min(out.size() - got, pending.size() - pending_at);
            std::copy_n(
                pending.begin() + static_cast<std::ptrdiff_t>(pending_at),
                take,
                out.begin() + static_cast<std::ptrdiff_t>(got)
            );
            pending_at += take;
            got += take;
        }
        return {};
    }

    /// Tells whether the recording has no byte left to hand out.
    ///
    /// @return true at the end of the frame, or of a deflated entry
    [[nodiscard]] bool at_end() const noexcept {
        if (layout.zstd_entry)
            return frame.ended;
        return entry_done && pending_at == pending.size();
    }
};

RecordingReader::RecordingReader() = default;

RecordingReader::RecordingReader(RecordingReader&& other) noexcept = default;

RecordingReader& RecordingReader::operator=(RecordingReader&& other) noexcept = default;

RecordingReader::~RecordingReader() = default;

std::string RecordingReader::open(const ArchiveSource& source) {
    state_ = std::make_unique<State>();
    State& state = *state_;
    state.source = source;
    state.hooks = zip::SourceHooks{source.context, source.read_at};
    zip::ZipError error;
    if (!zip::read_stream_directory(
            state.hooks, source.bytes, detail::archive_limits(), state.directory, error
        ))
        return state.fail(
            error.status == zip::ZipStatus::duplicate_name ? not_one_recording : not_a_replay
        );
    const auto load = [&state](
                          std::size_t index,
                          uint32_t cap,
                          std::string_view oversize,
                          std::vector<uint8_t>& loaded
                      ) {
        return detail::load_stream_entry(
            state.hooks, state.source.bytes, state.directory.entries[index], cap, oversize, loaded
        );
    };
    // A deflated recording is inflated through a fixed window here, so its size is no reason to refuse it.
    const std::string problem = detail::describe_entries(
        std::span<const zip::StreamEntry>(state.directory.entries),
        most_streamed_archive_bytes,
        load,
        state.layout,
        state.meta,
        state.manifest
    );
    if (!problem.empty())
        return state.fail(problem);
    return state.start();
}

const Meta& RecordingReader::meta() const noexcept {
    static const Meta none{};
    return state_ != nullptr ? state_->meta : none;
}

const Manifest& RecordingReader::manifest() const noexcept {
    static const Manifest none{};
    return state_ != nullptr ? state_->manifest : none;
}

std::string RecordingReader::read_timeline(std::optional<std::string>& timeline) {
    timeline.reset();
    if (state_ == nullptr || state_->directory.entries.empty())
        return std::string(not_a_replay);
    State& state = *state_;
    if (!state.layout.with_timeline)
        return {};
    // The timeline is the fourth entry when there is one (detail::describe_entries).
    constexpr std::size_t timeline_entry = 3;
    std::vector<uint8_t> bytes;
    if (std::string problem = detail::load_stream_entry(
            state.hooks,
            state.source.bytes,
            state.directory.entries[timeline_entry],
            detail::description_cap,
            details_unreadable,
            bytes
        );
        !problem.empty())
        return problem;
    std::optional<std::string> compact = detail::canonical_json(
        std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size())
    );
    if (!compact.has_value())
        return std::string(details_unreadable);
    timeline = std::move(compact);
    return {};
}

uint64_t RecordingReader::stored_frame_bytes() const noexcept {
    if (state_ == nullptr || state_->directory.entries.empty() || !state_->layout.zstd_entry)
        return 0;
    return state_->directory.entries[state_->layout.recording].bytes;
}

std::string RecordingReader::read_stored_frame(std::span<uint8_t> out, std::size_t& got) {
    got = 0;
    if (state_ == nullptr || state_->directory.entries.empty())
        return std::string(not_a_replay);
    State& state = *state_;
    if (!state.layout.zstd_entry)
        return std::string(recording_unreadable);
    if (!state.stored_open) {
        zip::ZipError error;
        if (!state.stored.open(
                state.hooks,
                state.source.bytes,
                state.directory.entries[state.layout.recording],
                error
            ))
            return std::string(not_a_replay);
        state.stored_open = true;
    }
    const zip::SinkHooks sink{&state.stored_pending, keep_entry_data};
    while (got < out.size()) {
        if (state.stored_at == state.stored_pending.size()) {
            state.stored_pending.clear();
            state.stored_at = 0;
            if (state.stored_done)
                break;
            zip::ZipError error;
            const zip::StreamStep step = state.stored.step(sink, entry_step_bytes, error);
            if (step == zip::StreamStep::failed)
                return std::string(not_a_replay);
            state.stored_done = step == zip::StreamStep::done;
            continue;
        }
        const std::size_t take =
            std::min(out.size() - got, state.stored_pending.size() - state.stored_at);
        std::copy_n(
            state.stored_pending.begin() + static_cast<std::ptrdiff_t>(state.stored_at),
            take,
            out.begin() + static_cast<std::ptrdiff_t>(got)
        );
        state.stored_at += take;
        got += take;
    }
    return {};
}

std::string RecordingReader::read(std::span<uint8_t> out, std::size_t& got) {
    got = 0;
    if (state_ == nullptr)
        return std::string(not_a_replay);
    State& state = *state_;
    if (!state.failed.empty())
        return state.failed;
    if (state.finished)
        return {};
    std::string problem =
        state.layout.zstd_entry ? state.read_frame(out, got) : state.read_plain(out, got);
    if (!problem.empty())
        return problem;
    // Never more than the description names: a frame that runs on is refused as it passes.
    if (got > state.meta.recording.bytes - std::min(state.position, state.meta.recording.bytes))
        return state.fail(recording_mismatch);
    base::sha256::update(state.hasher, out.first(got));
    state.position += got;
    if (!state.at_end())
        return {};
    const auto hex = base::sha256::to_hex(base::sha256::finish(state.hasher));
    if (state.position != state.meta.recording.bytes ||
        !hex_equal(state.meta.recording.sha256, std::string_view(hex.data(), hex.size())))
        return state.fail(recording_mismatch);
    state.finished = true;
    return {};
}

std::string RecordingReader::rewind() {
    if (state_ == nullptr)
        return std::string(not_a_replay);
    if (state_->directory.entries.empty())
        return state_->fail(not_a_replay);
    return state_->start();
}

uint64_t RecordingReader::position() const noexcept {
    return state_ != nullptr ? state_->position : 0;
}

bool RecordingReader::finished() const noexcept {
    return state_ != nullptr && state_->finished;
}

} // namespace oa::formats::oademo
