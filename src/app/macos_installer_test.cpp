// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A small macOS installer built in the test. The game files are copied in
// one pass, the payload is not extracted, and the check reports each file.

#include "oa/app/content_copy.hpp"
#include "oa/platform/files.hpp"
#include "oa/test/check.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <zlib.h>

namespace {

namespace fs = std::filesystem;

struct Scratch {
    fs::path root;

    Scratch() {
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        root = fs::temp_directory_path() / ("G19-macos-" + std::to_string(tick));
        fs::create_directories(root);
    }

    ~Scratch() {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
};

void put16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value));
}

void put32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value >> 24));
    out.push_back(static_cast<uint8_t>(value >> 16));
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value));
}

void put64(std::vector<uint8_t>& out, uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8)
        out.push_back(static_cast<uint8_t>(value >> shift));
}

void put_octal(std::vector<uint8_t>& out, uint64_t value, int width) {
    for (int shift = (width - 1) * 3; shift >= 0; shift -= 3)
        out.push_back(static_cast<uint8_t>('0' + ((value >> shift) & 7u)));
}

void put_text(std::vector<uint8_t>& out, std::string_view text) {
    out.insert(out.end(), text.begin(), text.end());
}

void odc_file(
    std::vector<uint8_t>& out,
    uint32_t mode,
    std::string_view name,
    std::string_view data,
    uint64_t device = 0,
    uint64_t inode = 0
) {
    put_text(out, "070707");
    put_octal(out, device, 6);
    put_octal(out, inode, 6);
    put_octal(out, mode, 6);
    put_octal(out, 0, 6);
    put_octal(out, 0, 6);
    put_octal(out, 1, 6);
    put_octal(out, 0, 6);
    put_octal(out, 1600000000, 11);
    put_octal(out, name.size() + 1, 6);
    put_octal(out, data.size(), 11);
    put_text(out, name);
    out.push_back(0);
    put_text(out, data);
}

[[nodiscard]] std::vector<uint8_t> gzip_of(const std::vector<uint8_t>& plain) {
    z_stream stream{};
    if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) !=
        Z_OK)
        return {};
    const uLong bound = deflateBound(&stream, static_cast<uLong>(plain.size())) + 64;
    std::vector<uint8_t> compressed(bound);
    stream.next_in = const_cast<Bytef*>(plain.data());
    stream.avail_in = static_cast<uInt>(plain.size());
    stream.next_out = compressed.data();
    stream.avail_out = static_cast<uInt>(compressed.size());
    const int result = deflate(&stream, Z_FINISH);
    const std::size_t produced = stream.total_out;
    deflateEnd(&stream);
    if (result != Z_STREAM_END)
        return {};
    compressed.resize(produced);
    return compressed;
}

[[nodiscard]] std::vector<uint8_t> zlib_of(std::string_view text) {
    uLongf bound = compressBound(static_cast<uLong>(text.size()));
    std::vector<uint8_t> compressed(bound);
    const int result = compress2(
        compressed.data(),
        &bound,
        reinterpret_cast<const Bytef*>(text.data()),
        static_cast<uLong>(text.size()),
        Z_DEFAULT_COMPRESSION
    );
    if (result != Z_OK)
        return {};
    compressed.resize(bound);
    return compressed;
}

[[nodiscard]] std::vector<uint8_t>
package_of(const std::vector<uint8_t>& payload, bool outer_gzip) {
    const std::string xml =
        "<xar><toc><file id=\"1\"><name>package.pkg</name><type>directory</type>"
        "<file id=\"2\"><name>Scripts</name><type>file</type><data><length>" +
        std::to_string(payload.size()) + "</length><offset>0</offset><size>" +
        std::to_string(payload.size()) + "</size><encoding style=\"" +
        (outer_gzip ? "application/x-gzip" : "application/octet-stream") +
        "\"/></data></file></file></toc></xar>";
    const std::vector<uint8_t> toc = zlib_of(xml);
    std::vector<uint8_t> file;
    put32(file, 0x78617221);
    put16(file, 28);
    put16(file, 1);
    put64(file, toc.size());
    put64(file, xml.size());
    put32(file, 0);
    file.insert(file.end(), toc.begin(), toc.end());
    file.insert(file.end(), payload.begin(), payload.end());
    return file;
}

[[nodiscard]] std::vector<uint8_t> fixture_cpio(std::string_view rev) {
    std::vector<uint8_t> archive;
    odc_file(archive, 0040755, ".", "");
    odc_file(archive, 0040755, "./payload", "");
    odc_file(archive, 0040755, "./payload/drive_c", "");
    odc_file(archive, 0040755, "./payload/drive_c/Total Annihilation", "");
    odc_file(archive, 0040755, "./payload/drive_c/Total Annihilation/Data", "");
    odc_file(archive, 0100644, "./payload/drive_c/Total Annihilation/rev31.gp3", rev);
    odc_file(archive, 0100644, "./payload/drive_c/Total Annihilation/Data/2.zrb", "zrb-fixture\n");
    odc_file(
        archive, 0100644, "./payload/drive_c/Total Annihilation/totala4.hpi", "campaign-fixture\n"
    );
    odc_file(archive, 0100644, "./payload/secret.txt", "do-not-copy\n");
    odc_file(archive, 0100644, "./payload/../escaped.txt", "escaped\n");
    odc_file(archive, 0100644, "TRAILER!!!", "");
    return archive;
}

[[nodiscard]] bool write_bytes(const fs::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(
        reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())
    );
    return static_cast<bool>(out);
}

[[nodiscard]] std::string read_bytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

[[nodiscard]] bool tree_has(const fs::path& root, const std::string& name) {
    std::error_code failure;
    if (!fs::exists(root, failure))
        return false;
    fs::recursive_directory_iterator cursor(
        root, fs::directory_options::skip_permission_denied, failure
    );
    const fs::recursive_directory_iterator end;
    while (!failure && cursor != end) {
        if (cursor->path().filename() == name)
            return true;
        cursor.increment(failure);
    }
    return false;
}

void copy_and_check() {
    const Scratch scratch;
    const std::vector<uint8_t> cpio = fixture_cpio("rev31-fixture\n");
    const std::vector<uint8_t> gzip = gzip_of(cpio);
    OA_CHECK(!gzip.empty());
    const std::vector<uint8_t> package = package_of(gzip, false);
    const fs::path pkg_dir = scratch.root / "pkg";
    const fs::path user = scratch.root / "user";
    fs::create_directories(pkg_dir);
    fs::create_directories(user);
    const fs::path pkg = pkg_dir / "installer.pkg";
    OA_CHECK(write_bytes(pkg, package));
    const std::string before = read_bytes(pkg);

    oa::app::MacosInstallerSource source(pkg, oa::app::copy_check_bundles(), false);
    oa::app::CopyRequest request;
    request.user_folder = user;
    request.bundles = oa::app::copy_check_bundles();
    const oa::app::CopyResult first = oa::app::copy_content(source, request);
    OA_CHECK(first.status == oa::app::CopyStatus::finished);
    OA_CHECK(first.files_copied == 3);
    OA_CHECK(read_bytes(pkg) == before);
    OA_CHECK(!tree_has(user, "payload"));
    OA_CHECK(!tree_has(user, "secret.txt"));
    OA_CHECK(!tree_has(user, "escaped.txt"));
    OA_CHECK(!tree_has(pkg_dir, "payload"));
    const oa::app::CopyResult again = oa::app::copy_content(source, request);
    OA_CHECK(again.status == oa::app::CopyStatus::finished);
    OA_CHECK(again.files_copied == 0);
    OA_CHECK(again.files_skipped == 3);
    OA_CHECK(read_bytes(pkg) == before);

    for (const oa::app::CopyCheckFile& file : oa::app::copy_check_files()) {
        const oa::data::content_bundles::Bundle* bundle = nullptr;
        for (const oa::data::content_bundles::Bundle& candidate : oa::app::copy_check_bundles()) {
            if (candidate.id == file.bundle_id)
                bundle = &candidate;
        }
        OA_CHECK(bundle != nullptr);
        if (bundle == nullptr)
            continue;
        const std::string stored = read_bytes([&] {
            fs::path path = oa::app::bundle_folder(user, *bundle);
            std::string part;
            for (char byte : file.copy_name) {
                if (byte == '/') {
                    path /= part;
                    part.clear();
                } else {
                    part.push_back(byte);
                }
            }
            if (!part.empty())
                path /= part;
            return path;
        }());
        OA_CHECK(stored == file.bytes);
    }

    std::ostringstream report;
    OA_CHECK(
        oa::app::report_macos_installer(pkg, oa::app::copy_check_bundles(), false, report) == 0
    );
    OA_CHECK(report.str().find("installer check: passed") != std::string::npos);
    OA_CHECK(report.str().find("macOS installer") != std::string::npos);

    const std::vector<uint8_t> changed =
        package_of(gzip_of(fixture_cpio("rev31-fixturX\n")), false);
    const fs::path other = pkg_dir / "changed.pkg";
    OA_CHECK(write_bytes(other, changed));
    std::ostringstream failed;
    OA_CHECK(
        oa::app::report_macos_installer(other, oa::app::copy_check_bundles(), false, failed) == 1
    );
    OA_CHECK(failed.str().find("installer check: FAILED") != std::string::npos);

    std::ostringstream refused;
    OA_CHECK(
        oa::app::report_macos_installer(pkg, oa::app::copy_check_bundles(), true, refused) == 1
    );
    OA_CHECK(refused.str().find("recognised") != std::string::npos);
    const fs::path nowhere = scratch.root / "nowhere";
    fs::create_directories(nowhere);
    oa::app::MacosInstallerSource refused_source(pkg, oa::app::copy_check_bundles(), true);
    request.user_folder = nowhere;
    const oa::app::CopyResult blocked = oa::app::copy_content(refused_source, request);
    OA_CHECK(blocked.status == oa::app::CopyStatus::failed);
    OA_CHECK(blocked.error.find("recognised") != std::string::npos);
    OA_CHECK(!tree_has(nowhere, "rev31.gp3"));
}

/// A partial file is resumed: kept bytes that differ are written again, and kept bytes that all
/// match, part of the file or the whole of it, are kept and the copy goes on after them. The
/// bytes copied show which way each went. A copy that read the kept bytes and then wrote with
/// no seek between failed here in the C runtime under Wine.
void resumes_a_partial_file() {
    const Scratch scratch;
    const std::vector<uint8_t> package =
        package_of(gzip_of(fixture_cpio("rev31-fixture\n")), false);
    const fs::path pkg = scratch.root / "installer.pkg";
    const fs::path user = scratch.root / "user";
    fs::create_directories(user);
    OA_CHECK(write_bytes(pkg, package));
    const oa::data::content_bundles::Bundle* totala = nullptr;
    for (const oa::data::content_bundles::Bundle& candidate : oa::app::copy_check_bundles()) {
        if (candidate.id == "totala")
            totala = &candidate;
    }
    OA_CHECK(totala != nullptr);
    if (totala == nullptr)
        return;
    const fs::path partial = oa::app::bundle_folder(user, *totala) / "rev31.gp3.part";
    fs::create_directories(partial.parent_path());
    {
        std::ofstream out(partial, std::ios::binary);
        out << "XXXX";
    }
    oa::app::MacosInstallerSource source(pkg, oa::app::copy_check_bundles(), false);
    oa::app::CopyRequest request;
    request.user_folder = user;
    request.bundles = oa::app::copy_check_bundles();
    const oa::app::CopyResult copied = oa::app::copy_content(source, request);
    OA_CHECK(copied.status == oa::app::CopyStatus::finished);
    OA_CHECK(read_bytes(oa::app::bundle_folder(user, *totala) / "rev31.gp3") == "rev31-fixture\n");

    const fs::path user_prefix = scratch.root / "prefix";
    fs::create_directories(user_prefix);
    const fs::path kept = oa::app::bundle_folder(user_prefix, *totala) / "rev31.gp3.part";
    fs::create_directories(kept.parent_path());
    {
        std::ofstream out(kept, std::ios::binary);
        out << "rev3";
    }
    oa::app::MacosInstallerSource continued(pkg, oa::app::copy_check_bundles(), false);
    request.user_folder = user_prefix;
    const oa::app::CopyResult rest = oa::app::copy_content(continued, request);
    OA_CHECK(rest.status == oa::app::CopyStatus::finished);
    OA_CHECK(
        read_bytes(oa::app::bundle_folder(user_prefix, *totala) / "rev31.gp3") == "rev31-fixture\n"
    );
    // The four kept bytes were kept, not written again: the first copy wrote its differing
    // bytes again, the whole file.
    OA_CHECK(rest.files_copied == copied.files_copied);
    OA_CHECK(rest.bytes_copied + 4 == copied.bytes_copied);
    OA_CHECK(!fs::exists(kept));

    // The whole file kept as the partial: nothing is written, and it is finished all the same.
    const fs::path user_whole = scratch.root / "whole";
    fs::create_directories(user_whole);
    const fs::path whole = oa::app::bundle_folder(user_whole, *totala) / "rev31.gp3.part";
    fs::create_directories(whole.parent_path());
    {
        std::ofstream out(whole, std::ios::binary);
        out << "rev31-fixture\n";
    }
    oa::app::MacosInstallerSource again(pkg, oa::app::copy_check_bundles(), false);
    request.user_folder = user_whole;
    const oa::app::CopyResult kept_whole = oa::app::copy_content(again, request);
    OA_CHECK(kept_whole.status == oa::app::CopyStatus::finished);
    OA_CHECK(
        read_bytes(oa::app::bundle_folder(user_whole, *totala) / "rev31.gp3") == "rev31-fixture\n"
    );
    OA_CHECK(kept_whole.files_copied == copied.files_copied);
    OA_CHECK(kept_whole.bytes_copied + 14 == copied.bytes_copied);
    OA_CHECK(!fs::exists(whole));
}

const oa::data::content_bundles::Bundle* totala_bundle() {
    for (const oa::data::content_bundles::Bundle& bundle : oa::app::copy_check_bundles()) {
        if (bundle.id == "totala")
            return &bundle;
    }
    return nullptr;
}

void hard_link_and_second_tree() {
    const Scratch scratch;
    std::vector<uint8_t> archive;
    odc_file(archive, 0040755, "./payload", "");
    odc_file(archive, 0040755, "./payload/drive_c/Total Annihilation", "");
    odc_file(archive, 0040755, "./payload/drive_c/Total Annihilation/Data", "");
    odc_file(archive, 0100644, "./payload/drive_c/Total Annihilation/rev31.gp3", "", 1, 9);
    odc_file(
        archive, 0100644, "./payload/drive_c/Total Annihilation/holder", "rev31-fixture\n", 1, 9
    );
    odc_file(archive, 0120000, "./payload/drive_c/Total Annihilation/rev31.gp3", "link");
    odc_file(archive, 0100644, "./payload/drive_c/Total Annihilation/rev31.gp3", "rev31-fixturX\n");
    odc_file(archive, 0100644, "./payload/drive_c/Total Annihilation/Data/2.zrb", "zrb-fixture\n");
    odc_file(
        archive, 0100644, "./payload/drive_c/Total Annihilation/totala4.hpi", "campaign-fixture\n"
    );
    odc_file(archive, 0100644, "TRAILER!!!", "");
    const fs::path pkg = scratch.root / "installer.pkg";
    const fs::path user = scratch.root / "user";
    fs::create_directories(user);
    OA_CHECK(write_bytes(pkg, package_of(gzip_of(archive), false)));
    oa::app::MacosInstallerSource source(pkg, oa::app::copy_check_bundles(), false);
    oa::app::CopyRequest request;
    request.user_folder = user;
    request.bundles = oa::app::copy_check_bundles();
    const oa::app::CopyResult copied = oa::app::copy_content(source, request);
    OA_CHECK(copied.status == oa::app::CopyStatus::finished);
    OA_CHECK(copied.files_copied == 3);
    const oa::data::content_bundles::Bundle* totala = totala_bundle();
    OA_CHECK(totala != nullptr);
    if (totala == nullptr)
        return;
    OA_CHECK(read_bytes(oa::app::bundle_folder(user, *totala) / "rev31.gp3") == "rev31-fixture\n");
}

void outer_gzip_member() {
    const Scratch scratch;
    std::vector<uint8_t> archive;
    odc_file(archive, 0100644, "./drive_c/rev31.gp3", "rev31-fixture\n");
    odc_file(archive, 0100644, "./drive_c/Data/2.zrb", "zrb-fixture\n");
    odc_file(archive, 0100644, "./drive_c/totala4.hpi", "campaign-fixture\n");
    odc_file(archive, 0100644, "TRAILER!!!", "");
    const std::vector<uint8_t> package = package_of(gzip_of(archive), true);
    const fs::path pkg = scratch.root / "installer.pkg";
    const fs::path user = scratch.root / "user";
    fs::create_directories(user);
    OA_CHECK(write_bytes(pkg, package));
    oa::app::MacosInstallerSource source(pkg, oa::app::copy_check_bundles(), false);
    oa::app::CopyRequest request;
    request.user_folder = user;
    request.bundles = oa::app::copy_check_bundles();
    const oa::app::CopyResult copied = oa::app::copy_content(source, request);
    OA_CHECK(copied.status == oa::app::CopyStatus::finished);
    OA_CHECK(copied.files_copied == 3);
}

} // namespace

int main() {
    copy_and_check();
    resumes_a_partial_file();
    hard_link_and_second_tree();
    outer_gzip_member();
    return oa::test::check_exit_status();
}
