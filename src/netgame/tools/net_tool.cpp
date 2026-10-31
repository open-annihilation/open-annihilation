// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The network frame workbench: condenser frames decoded, re-encoded, stored
// and encoded.
#include "oa/netgame/network.hpp"
#include "oa/platform/files.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
std::vector<uint8_t> read_file(const std::filesystem::path& path) {
    const auto size = std::filesystem::file_size(path);
    if (size > 256 * 1024 * 1024)
        throw std::runtime_error("input exceeds 256 MiB limit");
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> bytes(static_cast<std::size_t>(size));
    if (!in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size)))
        throw std::runtime_error("cannot read input: " + oa::platform::path_to_utf8(path));
    return bytes;
}

void write_file(const std::filesystem::path& path, std::span<const uint8_t> bytes) {
    std::ofstream out(path, std::ios::binary);
    if (!out.write(
            reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())
        ))
        throw std::runtime_error("cannot write output: " + oa::platform::path_to_utf8(path));
    out.close();
    if (!out)
        throw std::runtime_error("cannot finish output: " + oa::platform::path_to_utf8(path));
}

void usage() {
    std::cerr << "open-annihilation network frame workbench\n"
                 "  oa-netgame-tool frame-decode INPUT.bin OUTPUT.bin\n"
                 "  oa-netgame-tool frame-reencode INPUT.bin OUTPUT.bin\n"
                 "  oa-netgame-tool frame-store PAYLOAD.bin OUTPUT.bin\n"
                 "  oa-netgame-tool frame-encode PAYLOAD.bin OUTPUT.bin\n";
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--help") {
            usage();
            return 0;
        }
        if (argc < 3) {
            usage();
            return 2;
        }
        const std::string command = argv[1];
        if (argc == 4 && (command == "frame-decode" || command == "frame-reencode" ||
                          command == "frame-store" || command == "frame-encode")) {
            const auto bytes = read_file(argv[2]);
            auto out = command == "frame-decode"     ? oa::netgame::network::decode_frame(bytes)
                       : command == "frame-reencode" ? oa::netgame::network::reencode_frame(bytes)
                       : command == "frame-encode"
                           ? oa::netgame::network::encode_frame(bytes)
                           : oa::netgame::network::encode_stored_frame(bytes);
            write_file(argv[3], out);
        } else {
            usage();
            return 2;
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "oa-netgame-tool: " << e.what() << '\n';
        return 1;
    }
}
