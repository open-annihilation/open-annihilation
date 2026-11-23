// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "demo_source.hpp"

#include "oa/formats/oademo/recording_reader.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <ios>
#include <limits>
#include <utility>

namespace oa::session::demo {

namespace {

namespace fs = std::filesystem;
namespace tad = formats::tad;
namespace oademo = formats::oademo;

/// The bytes a packet reader holds: room for every chunk, which a 16-bit word sizes.
constexpr std::size_t packet_buffer_bytes = std::size_t{1} << 20;

uint16_t load16(const uint8_t* at) noexcept {
    return static_cast<uint16_t>(at[0] | (at[1] << 8));
}

/// The text demo_load gives for a recording that does not parse.
std::string parse_failure(uint64_t offset, std::string_view message) {
    return "demo parse failed at offset " + std::to_string(offset) + ": " + std::string(message);
}

/// A recording held in memory.
class HeldBytes final : public RecordingBytes {
  public:

    explicit HeldBytes(std::span<const uint8_t> bytes) : bytes_(bytes) {}

    std::string read(std::span<uint8_t> out, std::size_t& got) override {
        got = static_cast<std::size_t>(std::min<uint64_t>(out.size(), bytes_.size() - at_));
        if (got != 0)
            std::memcpy(out.data(), bytes_.data() + at_, got);
        at_ += got;
        return {};
    }

    std::string seek(uint64_t position) override {
        if (position > bytes_.size())
            return "the recording ends before its packet";
        at_ = static_cast<std::size_t>(position);
        return {};
    }

    uint64_t position() const noexcept override { return at_; }

  private:

    std::span<const uint8_t> bytes_{};
    std::size_t at_{};
};

/// A recording read from its file.
class FileBytes final : public RecordingBytes {
  public:

    /// Opens the file.
    ///
    /// @param path the recording
    /// @return empty, or why it cannot be opened
    std::string open(const fs::path& path) {
        in_.open(path, std::ios::binary);
        if (!in_)
            return "the recording cannot be opened";
        return {};
    }

    std::string read(std::span<uint8_t> out, std::size_t& got) override {
        got = 0;
        if (out.empty() || in_.eof())
            return {};
        in_.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
        got = static_cast<std::size_t>(std::max<std::streamsize>(in_.gcount(), 0));
        at_ += got;
        if (in_.bad())
            return "the recording cannot be read";
        return {};
    }

    std::string seek(uint64_t position) override {
        if (position > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max()))
            return "the recording cannot be read";
        in_.clear();
        in_.seekg(static_cast<std::streamoff>(position));
        if (!in_)
            return "the recording cannot be read";
        at_ = position;
        return {};
    }

    uint64_t position() const noexcept override { return at_; }

  private:

    std::ifstream in_{};
    uint64_t at_{};
};

/// A replay's recording, read through its frame: from bytes held or from its file.
class ReplayBytes final : public RecordingBytes {
  public:

    /// Opens a replay held in memory.
    ///
    /// @param archive the `.oademo`'s bytes; they outlive the reader
    /// @return empty, or the sentence that says why not
    std::string open_held(std::span<const uint8_t> archive) {
        held_ = archive;
        return reader_.open(
            oademo::ArchiveSource{this, read_held, static_cast<uint64_t>(archive.size())}
        );
    }

    /// Opens a replay's file.
    ///
    /// @param path the `.oademo`
    /// @return empty, or the sentence that says why not
    std::string open_file(const fs::path& path) {
        file_.open(path, std::ios::binary);
        if (!file_)
            return "cannot open demo";
        file_.seekg(0, std::ios::end);
        const std::streamoff end = file_.tellg();
        if (!file_ || end < 0)
            return "cannot open demo";
        return reader_.open(oademo::ArchiveSource{this, read_file, static_cast<uint64_t>(end)});
    }

    std::string read(std::span<uint8_t> out, std::size_t& got) override {
        return reader_.read(out, got);
    }

    std::string seek(uint64_t position) override {
        if (position < reader_.position()) {
            if (std::string why_not = reader_.rewind(); !why_not.empty())
                return why_not;
        }
        // A frame is read from its start: the bytes before the place are read and dropped.
        std::vector<uint8_t> dropped(
            static_cast<std::size_t>(std::min<uint64_t>(packet_buffer_bytes, position))
        );
        while (reader_.position() < position) {
            const auto want = static_cast<std::size_t>(
                std::min<uint64_t>(dropped.size(), position - reader_.position())
            );
            std::size_t got = 0;
            if (std::string why_not = reader_.read(std::span(dropped).first(want), got);
                !why_not.empty())
                return why_not;
            if (got == 0)
                return "the recording ends before its packet";
        }
        return {};
    }

    uint64_t position() const noexcept override { return reader_.position(); }

  private:

    static bool read_held(void* context, uint64_t offset, std::span<uint8_t> into) {
        const auto& self = *static_cast<const ReplayBytes*>(context);
        if (offset > self.held_.size() || into.size() > self.held_.size() - offset)
            return false;
        std::memcpy(into.data(), self.held_.data() + offset, into.size());
        return true;
    }

    static bool read_file(void* context, uint64_t offset, std::span<uint8_t> into) {
        auto& self = *static_cast<ReplayBytes*>(context);
        if (offset > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max()))
            return false;
        self.file_.clear();
        self.file_.seekg(static_cast<std::streamoff>(offset));
        self.file_.read(
            reinterpret_cast<char*>(into.data()), static_cast<std::streamsize>(into.size())
        );
        return self.file_.gcount() == static_cast<std::streamsize>(into.size());
    }

    std::span<const uint8_t> held_{};
    std::ifstream file_{};
    oademo::RecordingReader reader_{};
};

} // namespace

PacketReader::PacketReader(std::unique_ptr<RecordingBytes> bytes) : bytes_(std::move(bytes)) {
    buffer_.resize(packet_buffer_bytes);
}

void PacketReader::start_with(uint64_t offset, uint64_t time_ms, std::span<const uint8_t> read) {
    // A long part before the packets can leave more read than the buffer holds: it grows to keep it.
    if (read.size() > buffer_.size())
        buffer_.resize(read.size());
    if (!read.empty())
        std::memcpy(buffer_.data(), read.data(), read.size());
    at_ = 0;
    end_ = read.size();
    buffer_offset_ = offset;
    time_ms_ = time_ms;
    bytes_ended_ = false;
}

std::string PacketReader::start_at(uint64_t offset, uint64_t time_ms) {
    at_ = 0;
    end_ = 0;
    buffer_offset_ = offset;
    time_ms_ = time_ms;
    bytes_ended_ = false;
    return bytes_->seek(offset);
}

std::string PacketReader::fill(std::size_t count) {
    if (end_ - at_ >= count || bytes_ended_)
        return {};
    if (at_ != 0) {
        std::memmove(buffer_.data(), buffer_.data() + at_, end_ - at_);
        buffer_offset_ += at_;
        end_ -= at_;
        at_ = 0;
    }
    while (end_ < count && !bytes_ended_) {
        std::size_t got = 0;
        if (std::string why_not = bytes_->read(std::span(buffer_).subspan(end_), got);
            !why_not.empty())
            return why_not;
        if (got == 0)
            bytes_ended_ = true;
        end_ += got;
    }
    return {};
}

std::string PacketReader::next(tad::Packet& packet, bool& ended) {
    ended = false;
    // The checks and the texts a whole parse makes of a packet's chunk.
    if (std::string why_not = fill(tad::layout::chunk_length_bytes); !why_not.empty())
        return why_not;
    const uint64_t at = offset();
    if (end_ == at_) {
        ended = true;
        return {};
    }
    if (end_ - at_ < tad::layout::chunk_length_bytes)
        return parse_failure(at, "packet: missing chunk length");
    const std::size_t length = load16(buffer_.data() + at_);
    if (length < tad::layout::chunk_length_bytes)
        return parse_failure(at, "packet: chunk length below 2");
    if (std::string why_not = fill(length); !why_not.empty())
        return why_not;
    if (end_ - at_ < length)
        return parse_failure(at, "packet: chunk runs past end of file");
    const std::span<const uint8_t> data(
        buffer_.data() + at_ + tad::layout::chunk_length_bytes,
        length - tad::layout::chunk_length_bytes
    );
    if (data.size() < tad::layout::packet_fixed_bytes)
        return parse_failure(at, "packet chunk shorter than 3 bytes");
    const uint16_t delay = load16(data.data());
    time_ms_ += delay;
    packet = tad::Packet{
        static_cast<std::size_t>(at),
        delay,
        time_ms_,
        data[2],
        data.subspan(tad::layout::packet_fixed_bytes)
    };
    at_ += length;
    return {};
}

std::string PacketReader::drain() {
    at_ = end_;
    while (!bytes_ended_) {
        buffer_offset_ += end_;
        at_ = 0;
        end_ = 0;
        std::size_t got = 0;
        if (std::string why_not = bytes_->read(buffer_, got); !why_not.empty())
            return why_not;
        if (got == 0)
            bytes_ended_ = true;
        end_ = got;
        at_ = got;
    }
    return {};
}

std::string PlaybackSource::open_bytes(std::unique_ptr<RecordingBytes>& bytes) const {
    switch (origin) {
    case Origin::held_tad:
        if (held == nullptr)
            return "the recording holds nothing";
        bytes = std::make_unique<HeldBytes>(*held);
        return {};
    case Origin::file_tad: {
        auto file = std::make_unique<FileBytes>();
        if (std::string why_not = file->open(path); !why_not.empty())
            return why_not;
        bytes = std::move(file);
        return {};
    }
    case Origin::held_replay: {
        if (held == nullptr)
            return "the recording holds nothing";
        auto replay = std::make_unique<ReplayBytes>();
        if (std::string why_not = replay->open_held(*held); !why_not.empty())
            return why_not;
        bytes = std::move(replay);
        return {};
    }
    case Origin::file_replay: {
        auto replay = std::make_unique<ReplayBytes>();
        if (std::string why_not = replay->open_file(path); !why_not.empty())
            return why_not;
        bytes = std::move(replay);
        return {};
    }
    }
    return "the recording has no origin";
}

} // namespace oa::session::demo
