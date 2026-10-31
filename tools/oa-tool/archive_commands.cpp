// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "command.hpp"
#include "files.hpp"

#include "oa/formats/hpi.hpp"
#include "oa/platform/files.hpp"
#include "oa/ui/decoded.hpp"

#include <ostream>

namespace oa::tool {

int run_list(std::span<const std::string> arguments, Output& output) {
    const auto archive = oa::ui::decoded::require(oa::open_hpi_file(arguments[0]), arguments[0]);
    for (const auto& entry : archive.entries())
        output.out << entry.path << '\t' << entry.size << '\n';
    return exit_done;
}

int run_extract(std::span<const std::string> arguments, Output&) {
    const auto archive = oa::ui::decoded::require(oa::open_hpi_file(arguments[0]), arguments[0]);
    const auto data = oa::ui::decoded::require(archive.read(arguments[1]), arguments[1]);
    write_file(arguments[2], data);
    return exit_done;
}

int run_asset_extract(std::span<const std::string> arguments, Output& output) {
    oa::AssetStore store(arguments[0]);
    for (std::size_t index = 3; index < arguments.size(); ++index)
        store.mount(arguments[index]);
    const auto asset = store.read(arguments[1]);
    write_file(arguments[2], asset.bytes);
    output.out << (asset.archived ? "archive: " : "loose: ")
               << oa::platform::path_to_utf8(asset.source) << " (" << asset.bytes.size()
               << " bytes)\n";
    return exit_done;
}

int run_preview(std::span<const std::string> arguments, Output& output) {
    const auto archive = oa::ui::decoded::require(oa::open_hpi_file(arguments[0]), arguments[0]);
    const auto data = oa::ui::decoded::require(archive.read(arguments[1]), arguments[1]);
    write_image(arguments[2], oa::ui::decoded::require(oa::decode_pcx(data), arguments[1]), output);
    return exit_done;
}

int run_decode_pcx(std::span<const std::string> arguments, Output& output) {
    write_image(
        arguments[1],
        oa::ui::decoded::require(oa::decode_pcx(read_file(arguments[0])), arguments[0]),
        output
    );
    return exit_done;
}

} // namespace oa::tool
