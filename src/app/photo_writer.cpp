// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The photo writer. Encoding and the file write run on this thread, never
// on the frame that queued the picture.
#include "photo_writer.hpp"

#include "oa/formats/png.hpp"

#include <atomic>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>

namespace oa::app {
namespace {

constexpr std::string_view kNotWritten = "the file could not be written";

} // namespace

PhotoWriter::PhotoWriter() = default;

PhotoWriter::~PhotoWriter() {
    finish();
}

bool PhotoWriter::enqueue(Picture picture) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (outstanding_ >= 4)
        return false;
    if (!thread_.joinable()) {
        stopping_ = false;
        thread_ = std::thread([this] { run(); });
    }
    reserved_.insert(picture.file_name);
    ++outstanding_;
    queue_.push_back(std::move(picture));
    work_.notify_one();
    return true;
}

bool PhotoWriter::holds(std::string_view file_name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return reserved_.find(std::string(file_name)) != reserved_.end() || writing_name_ == file_name;
}

int PhotoWriter::outstanding() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return outstanding_;
}

std::optional<PhotoWriter::Notice> PhotoWriter::poll() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (notices_.empty())
        return std::nullopt;
    Notice notice = std::move(notices_.front());
    notices_.pop_front();
    return notice;
}

void PhotoWriter::wait_until_idle() {
    std::unique_lock<std::mutex> lock(mutex_);
    idle_.wait(lock, [this] { return outstanding_ == 0; });
}

void PhotoWriter::finish() noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    work_.notify_all();
    if (thread_.joinable())
        thread_.join();
    stopping_ = false;
}

void PhotoWriter::run() {
    for (;;) {
        Picture picture;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            work_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (queue_.empty())
                return;
            picture = std::move(queue_.front());
            queue_.pop_front();
            writing_name_ = picture.file_name;
        }
        Notice notice;
        try {
            notice = save(picture);
        } catch (...) {
            notice.saved = false;
            notice.file_name = picture.file_name;
            notice.reason = std::string(kNotWritten);
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            reserved_.erase(picture.file_name);
            writing_name_.clear();
            notices_.push_back(std::move(notice));
            --outstanding_;
            if (outstanding_ == 0)
                idle_.notify_all();
        }
    }
}

PhotoWriter::Notice PhotoWriter::save(const Picture& picture) {
    Notice notice;
    notice.file_name = picture.file_name;
    notice.reason = std::string(kNotWritten);
    const std::size_t pixels = static_cast<std::size_t>(picture.width) * picture.height;
    if (picture.width == 0 || picture.height == 0 || picture.rgb.size() != pixels * 3U)
        return notice;

    oa::formats::png::Header header;
    header.width = picture.width;
    header.height = picture.height;
    header.bit_depth = 8;
    header.color_type = oa::formats::png::ColorType::rgb;
    header.interlace = oa::formats::png::Interlace::none;
    // The chunk views point at this job's strings, which live until write returns.
    std::vector<oa::formats::png::TextChunk> chunks;
    const std::size_t texts = picture.text_keywords.size() < picture.text_values.size()
                                  ? picture.text_keywords.size()
                                  : picture.text_values.size();
    chunks.reserve(texts);
    for (std::size_t index = 0; index < texts; ++index) {
        if (picture.text_keywords[index].empty())
            continue;
        oa::formats::png::TextChunk chunk;
        chunk.kind = oa::formats::png::TextKind::utf8;
        chunk.keyword = picture.text_keywords[index];
        chunk.text = picture.text_values[index];
        chunks.push_back(chunk);
    }
    oa::formats::png::Image image;
    image.header = header;
    image.rows = picture.rgb;
    image.texts = chunks;
    std::vector<uint8_t> file;
    if (!oa::formats::png::write(image, &file))
        return notice;

    static std::atomic<unsigned> next_id{1};
    const unsigned id = next_id.fetch_add(1U);
    const std::filesystem::path temporary =
        picture.path.parent_path() / (std::string(".photo-") + std::to_string(id) + ".partial");
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out)
            return notice;
        out.write(
            reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size())
        );
        out.close();
        if (!out) {
            std::error_code remove_error;
            std::filesystem::remove(temporary, remove_error);
            return notice;
        }
    }
    std::error_code rename_error;
    std::filesystem::rename(temporary, picture.path, rename_error);
    if (rename_error) {
        std::error_code remove_error;
        std::filesystem::remove(temporary, remove_error);
        return notice;
    }
    notice.saved = true;
    notice.reason.clear();
    return notice;
}

} // namespace oa::app
