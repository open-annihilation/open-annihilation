// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/platform/files.hpp"
#include "oa/ui/gui_layout.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: oa-gui-layout-inspect FILE.GUI...\n";
        return 2;
    }
    bool failed = false;
    for (int index = 1; index < argc; ++index) {
        const std::filesystem::path path(argv[index]);
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            std::cerr << oa::platform::path_to_utf8(path) << ": cannot open\n";
            failed = true;
            continue;
        }
        std::vector<uint8_t> bytes(
            (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>()
        );
        const auto result = oa::ui::gui_layout::parse(bytes);
        if (!result.ok()) {
            std::cerr << oa::platform::path_to_utf8(path) << ": " << result.error->message
                      << " (offset " << result.error->offset << ")\n";
            failed = true;
            continue;
        }
        const auto& root = result.layout->gadgets.front();
        std::cout << oa::platform::path_to_utf8(path) << '\t' << result.layout->gadgets.size()
                  << '\t' << root.common.name << '\n';
    }
    return failed ? 1 : 0;
}
