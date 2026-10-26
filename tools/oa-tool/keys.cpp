// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// keys pack, keys check, keys picture and keys show. Pack and check refuse a
// folder or a file of another kind, then run the generic command. The picture
// is K30's keyboard on the smooth canvas. Show resolves the base and the
// layout, with no mod and no own changes.

#include "command.hpp"

#include "arguments.hpp"
#include "check_keys.hpp"

#include "oa/app/package_install.hpp"
#include "oa/data/input_actions/actions.hpp"
#include "oa/formats/oakeys/manifest.hpp"
#include "oa/formats/png.hpp"
#include "oa/platform/text_font.hpp"
#include "oa/ui/key_bindings/chord.hpp"
#include "oa/ui/key_bindings/layout.hpp"
#include "oa/ui/key_bindings/layout_file.hpp"
#include "oa/ui/kit/canvas.hpp"
#include "oa/ui/kit/keys.hpp"
#include "oa/ui/kit/looks.hpp"
#include "oa/ui/paint/painter.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace oa::tool {
namespace {

namespace fs = std::filesystem;
namespace actions = oa::data::input_actions;
namespace bindings = oa::ui::key_bindings;
namespace install = oa::app::package_install;
namespace json = oa::formats::json;
namespace kit = oa::ui::kit;
namespace layouts = oa::formats::oakeys;
namespace paint = oa::ui::paint;
namespace png = oa::formats::png;
namespace text_font = oa::platform::text_font;

/// The widest picture, in pixels. Taller than this is a usage error.
constexpr int32_t most_picture_width = 8192;

/// Tells whether two texts are equal with ASCII letters folded.
///
/// @param left one text
/// @param right the other
/// @return true when they are equal
bool same_ascii(std::string_view left, std::string_view right) {
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto fold = [](unsigned char byte) {
            return byte >= 'A' && byte <= 'Z' ? static_cast<unsigned char>(byte - 'A' + 'a') : byte;
        };
        if (fold(static_cast<unsigned char>(left[index])) !=
            fold(static_cast<unsigned char>(right[index])))
            return false;
    }
    return true;
}

/// Returns a path made from UTF-8 text.
///
/// @param text the text
/// @return the path
fs::path path_of(std::string_view text) {
    return fs::path(std::u8string(text.begin(), text.end()));
}

/// Tells whether a name ends in an extension, compared without case.
///
/// @param name the file name
/// @param extension the extension, including its dot, already folded
/// @return true when the name ends in it and is longer
bool ends_with_extension(std::string_view name, std::string_view extension) {
    if (name.size() <= extension.size())
        return false;
    for (std::size_t index = 0; index < extension.size(); ++index) {
        const auto fold = [](unsigned char byte) {
            return byte >= 'A' && byte <= 'Z' ? static_cast<unsigned char>(byte - 'A' + 'a') : byte;
        };
        const unsigned char got =
            fold(static_cast<unsigned char>(name[name.size() - extension.size() + index]));
        if (got != static_cast<unsigned char>(extension[index]))
            return false;
    }
    return true;
}

/// The last component of a path, as UTF-8.
///
/// @param path the path
/// @return its filename
std::string filename_text(const fs::path& path) {
    const auto name = path.filename().generic_u8string();
    return std::string(name.begin(), name.end());
}

/// Tells whether a folder's top holds oakeys.yaml.
///
/// @param folder the folder
/// @return true when a file there is named oakeys.yaml, without regard to case
bool top_holds_oakeys(const fs::path& folder) {
    std::error_code listed;
    if (!fs::is_directory(fs::symlink_status(folder, listed)))
        return false;
    try {
        const fs::directory_iterator end;
        for (fs::directory_iterator cursor(folder); cursor != end; ++cursor) {
            std::error_code status_error;
            if (fs::is_directory(cursor->symlink_status(status_error)))
                continue;
            if (same_ascii(filename_text(cursor->path()), layouts::manifest_file))
                return true;
        }
    } catch (const std::exception&) {
        return false;
    }
    return false;
}

/// Tells whether a path is a .oakeys file, by its name.
///
/// @param path the path
/// @return true when the file name ends in .oakeys
bool oakeys_file_name(const fs::path& path) {
    return ends_with_extension(filename_text(path), ".oakeys");
}

/// Prints a usage failure for a keys command.
///
/// @param output where it is printed
/// @param command `keys pack`
/// @param problem what is wrong
void report_keys_usage(Output& output, std::string_view command, std::string_view problem) {
    output.err << "oa-tool " << command << ": " << problem << '\n';
    output.err << "run 'oa-tool help " << command << "'\n";
}

/// Prints problem lines and nothing else.
///
/// @param output where they are printed
/// @param problems the lines
void print_problems(Output& output, const std::vector<std::string>& problems) {
    for (const std::string& line : problems)
        output.err << line << '\n';
}

/// Which keyboard a picture draws. `any` draws ANSI.
///
/// @param keyboard the manifest's keyboard
/// @return the shape
kit::KeyboardShape shape_of(layouts::Keyboard keyboard) noexcept {
    switch (keyboard) {
    case layouts::Keyboard::iso:
        return kit::KeyboardShape::iso;
    case layouts::Keyboard::jis:
        return kit::KeyboardShape::jis;
    case layouts::Keyboard::ansi:
    case layouts::Keyboard::any:
        return kit::KeyboardShape::ansi;
    }
    return kit::KeyboardShape::ansi;
}

/// A layout loaded for a picture or a show, or the problems that stopped it.
struct LoadedLayout {
    layouts::Manifest manifest{};        ///< the manifest, when `ok`
    bool ok{false};                      ///< the manifest was read and every chord parsed
    std::vector<std::string> problems{}; ///< why not
};

/// Loads a .oakeys file or a folder whose top holds oakeys.yaml.
///
/// A chord that does not parse is a problem. A warning is not.
///
/// @param path the file or the folder
/// @return the layout, or the problems
LoadedLayout load_layout(const fs::path& path) {
    LoadedLayout loaded;
    std::error_code listed;
    if (fs::is_directory(fs::symlink_status(path, listed))) {
        const KeyFolder folder = inspect_key_folder(path);
        loaded.problems = folder.problems;
        if (!folder.have_files || !folder.problems.empty()) {
            if (loaded.problems.empty())
                loaded.problems.push_back(
                    std::string(layouts::manifest_file) + ": it cannot be read"
                );
            return loaded;
        }
        loaded.manifest = folder.manifest;
    } else {
        const install::PackageResult opened = install::open_package(path);
        if (!opened.package) {
            if (!opened.problem.lines.empty())
                loaded.problems = opened.problem.lines;
            else if (!opened.problem.detail.empty())
                loaded.problems.push_back(opened.problem.detail);
            else
                loaded.problems.push_back("it is not a key layout");
            return loaded;
        }
        if (opened.package->kind == nullptr || opened.package->kind->name != "oakeys") {
            loaded.problems.push_back("it is not a key layout");
            return loaded;
        }
        const PackageEntries entries = entries_of(*opened.package);
        if (!read_oakeys_manifest(entries, loaded.manifest, loaded.problems))
            return loaded;
    }

    bindings::Layout layout;
    std::vector<bindings::LayoutProblem> found;
    const bool parsed = bindings::layout_from_manifest(loaded.manifest, nullptr, layout, found);
    for (const bindings::LayoutProblem& one : found) {
        if (one.warning)
            continue;
        loaded.problems.push_back(
            std::string(layouts::manifest_file) + ":" + std::to_string(one.line) + ":" +
            std::to_string(one.column) + ": " + one.key + ": " + one.message
        );
    }
    if (!parsed || !loaded.problems.empty())
        return loaded;
    loaded.ok = true;
    return loaded;
}

/// Reads `--width` as a whole number from 1 to most_picture_width.
///
/// @param text the option's value
/// @param[out] width the number
/// @param[out] usage the reason, when the text is not such a number
/// @return true when `width` was written
bool parse_width(std::string_view text, int32_t& width, std::string& usage) {
    usage = "option '--width' needs a number from 1 to 8192";
    // Four digits cover 1 to 8192. A longer run would wrap a 32-bit total.
    if (text.empty() || text.size() > 4)
        return false;
    uint32_t value = 0;
    for (const char byte : text) {
        if (byte < '0' || byte > '9')
            return false;
        value = value * 10u + static_cast<unsigned>(byte - '0');
    }
    if (value < 1 || value > static_cast<uint32_t>(most_picture_width))
        return false;
    width = static_cast<int32_t>(value);
    return true;
}

/// Writes a PNG, replacing a file of that name.
///
/// @param path the file
/// @param bytes the PNG
/// @param[out] problem why the write failed; unchanged on success
/// @return true when the file holds the bytes
bool write_png(const fs::path& path, std::span<const uint8_t> bytes, std::string& problem) {
    std::error_code listed;
    if (!path.parent_path().empty() &&
        !fs::is_directory(fs::symlink_status(path.parent_path(), listed))) {
        problem = "cannot write " + filename_text(path);
        return false;
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        problem = "cannot write " + filename_text(path);
        return false;
    }
    if (!bytes.empty())
        output.write(
            reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())
        );
    output.close();
    if (!output) {
        problem = "cannot write " + filename_text(path);
        return false;
    }
    return true;
}

/// The context names a mask holds, joined with a comma and a space.
///
/// @param bits the context bits
/// @return the names, or empty when none are set
std::string context_text(uint32_t bits) {
    std::string text;
    const std::span<const actions::Context> rows = actions::contexts();
    for (std::size_t index = 0; index < rows.size(); ++index) {
        if ((bits & (uint32_t{1} << index)) == 0)
            continue;
        if (!text.empty())
            text += ", ";
        text.append(rows[index].name);
    }
    return text;
}

/// Writes one show row's chords into an array that is already open.
///
/// @param[in,out] writer the array
/// @param chords the chords
void write_chords(json::JsonWriter& writer, const std::vector<bindings::Chord>& chords) {
    writer.begin_array();
    for (const bindings::Chord& chord : chords)
        writer.string(bindings::chord_text(chord));
    writer.end_array();
}

/// Prints the resolved bindings.
///
/// @param resolved the bindings
/// @param as_json one JSON array
/// @param output where the text is printed
void print_resolved(const bindings::Resolved& resolved, bool as_json, Output& output) {
    if (as_json) {
        json::JsonWriter writer;
        writer.begin_array();
        for (const bindings::Bound& action : resolved.actions) {
            writer.begin_object();
            writer.key("action");
            writer.string(action.id);
            writer.key("context");
            writer.string(context_text(action.contexts));
            writer.key("chords");
            std::vector<bindings::Chord> chords;
            chords.reserve(action.chords.size());
            for (const bindings::BoundChord& bound : action.chords)
                chords.push_back(bound.chord);
            write_chords(writer, chords);
            writer.end_object();
        }
        for (const bindings::LayoutBinding& unknown : resolved.unknown) {
            writer.begin_object();
            writer.key("action");
            writer.string(unknown.action);
            writer.key("context");
            writer.string("");
            writer.key("chords");
            write_chords(writer, unknown.chords);
            writer.end_object();
        }
        writer.end_array();
        output.out << writer.text() << '\n';
        return;
    }
    const auto line = [&](std::string_view action,
                          std::string_view context,
                          const std::vector<bindings::Chord>& chords) {
        output.out << action;
        if (!context.empty())
            output.out << ' ' << context;
        for (const bindings::Chord& chord : chords)
            output.out << ' ' << bindings::chord_text(chord);
        output.out << '\n';
    };
    for (const bindings::Bound& action : resolved.actions) {
        std::vector<bindings::Chord> chords;
        chords.reserve(action.chords.size());
        for (const bindings::BoundChord& bound : action.chords)
            chords.push_back(bound.chord);
        line(action.id, context_text(action.contexts), chords);
    }
    for (const bindings::LayoutBinding& unknown : resolved.unknown)
        line(unknown.action, {}, unknown.chords);
}

} // namespace

int run_keys_pack(std::span<const std::string> arguments, Output& output) {
    static constexpr OptionSpec options[] = {
        {"out", true, false},
        {"force", false, false},
        {"game-dir", true, false},
    };
    std::string problem;
    const std::optional<Arguments> parsed = parse_arguments(arguments, options, problem);
    if (!parsed || parsed->positional.size() != 1)
        return run_pack(arguments, output);
    if (!top_holds_oakeys(path_of(parsed->positional[0]))) {
        report_keys_usage(output, "keys pack", "the folder holds no oakeys.yaml");
        return exit_usage;
    }
    return run_pack(arguments, output);
}

int run_keys_check(std::span<const std::string> arguments, Output& output) {
    static constexpr OptionSpec options[] = {
        {"game-dir", true, false},
        {"mod", true, true},
        {"accept-unimplemented-hacks", false, false},
        {"json", false, false},
        {"strict", false, false},
    };
    std::string problem;
    const std::optional<Arguments> parsed = parse_arguments(arguments, options, problem);
    if (!parsed || parsed->positional.size() != 1)
        return run_check(arguments, output);
    const fs::path target = path_of(parsed->positional[0]);
    if (!oakeys_file_name(target) && !top_holds_oakeys(target)) {
        report_keys_usage(
            output, "keys check", "the file is not a .oakeys and the folder holds no oakeys.yaml"
        );
        return exit_usage;
    }
    return run_check(arguments, output);
}

int run_keys_picture(std::span<const std::string> arguments, Output& output) {
    static constexpr OptionSpec options[] = {
        {"out", true, false},
        {"width", true, false},
        {"fonts", true, false},
    };
    std::string problem;
    const std::optional<Arguments> parsed = parse_arguments(arguments, options, problem);
    if (!parsed) {
        report_keys_usage(output, "keys picture", problem);
        return exit_usage;
    }
    if (parsed->positional.size() != 1) {
        report_keys_usage(
            output,
            "keys picture",
            "expected one file or folder, got " + std::to_string(parsed->positional.size())
        );
        return exit_usage;
    }
    const std::string* const out = parsed->value("out");
    if (out == nullptr || out->empty()) {
        report_keys_usage(output, "keys picture", "option '--out' needs a value");
        return exit_usage;
    }
    if (!ends_with_extension(*out, ".png")) {
        report_keys_usage(output, "keys picture", "the picture is a .png file");
        return exit_usage;
    }
    int32_t width = 1200;
    if (const std::string* const width_text = parsed->value("width")) {
        std::string usage;
        if (!parse_width(*width_text, width, usage)) {
            report_keys_usage(output, "keys picture", usage);
            return exit_usage;
        }
    }
    fs::path font_dir = text_font::bundled_font_directory();
    if (const std::string* const fonts = parsed->value("fonts")) {
        if (fonts->empty()) {
            report_keys_usage(output, "keys picture", "option '--fonts' needs a value");
            return exit_usage;
        }
        font_dir = path_of(*fonts);
    }
    if (font_dir.empty()) {
        report_keys_usage(output, "keys picture", "the fonts folder cannot be found");
        return exit_usage;
    }

    const LoadedLayout loaded = load_layout(path_of(parsed->positional[0]));
    if (!loaded.ok) {
        print_problems(output, loaded.problems);
        if (loaded.problems.empty())
            output.err << "it is not a key layout\n";
        return exit_failed;
    }

    const int32_t quarters = std::max(width / 91, int32_t{1});
    const int32_t height = 25 * quarters;
    paint::Canvas pixels = paint::make_canvas(static_cast<int>(width), static_cast<int>(height));
    paint::Painter painter(pixels);
    const std::unique_ptr<text_font::FontStack> fonts = text_font::FontStack::open(font_dir);
    if (fonts == nullptr) {
        output.err << "the fonts cannot be opened\n";
        return exit_failed;
    }
    kit::Canvas canvas;
    canvas.smooth.painter = &painter;
    canvas.smooth.fonts = fonts.get();
    canvas.smooth.scale_percent = 100;

    kit::KeyboardLook look;
    look.shape = shape_of(loaded.manifest.keyboard);
    for (const std::string& code : changed_key_codes(loaded.manifest)) {
        kit::KeyboardKey key;
        key.code = code;
        key.cap.mark = kit::KeyMark::changed;
        look.keys.push_back(std::move(key));
    }
    const kit::Rect rect{0, 0, width, height};
    kit::draw_keyboard(canvas, rect, look);

    png::Image image;
    image.header.width = static_cast<uint32_t>(width);
    image.header.height = static_cast<uint32_t>(height);
    image.header.bit_depth = 8;
    image.header.color_type = png::ColorType::rgb_alpha;
    image.header.interlace = png::Interlace::none;
    image.rows = pixels.rgba;
    std::vector<uint8_t> encoded;
    if (!png::write(image, &encoded)) {
        output.err << "the picture cannot be encoded\n";
        return exit_failed;
    }
    std::string write_problem;
    if (!write_png(path_of(*out), encoded, write_problem)) {
        output.err << write_problem << '\n';
        return exit_failed;
    }
    output.out << "wrote " << *out << ": " << width << "x" << height << ", " << encoded.size()
               << " bytes\n";
    return exit_done;
}

int run_keys_show(std::span<const std::string> arguments, Output& output) {
    static constexpr OptionSpec options[] = {
        {"json", false, false},
    };
    std::string problem;
    const std::optional<Arguments> parsed = parse_arguments(arguments, options, problem);
    if (!parsed) {
        report_keys_usage(output, "keys show", problem);
        return exit_usage;
    }
    if (parsed->positional.size() != 1) {
        report_keys_usage(
            output,
            "keys show",
            "expected one file, folder or oa-default, got " +
                std::to_string(parsed->positional.size())
        );
        return exit_usage;
    }

    bindings::ResolveInput input;
    input.base = bindings::Base::oa_default;
    input.keyboard = nullptr;
    bindings::Layout layout;
    if (parsed->positional[0] != "oa-default") {
        const LoadedLayout loaded = load_layout(path_of(parsed->positional[0]));
        if (!loaded.ok) {
            print_problems(output, loaded.problems);
            if (loaded.problems.empty())
                output.err << "it is not a key layout\n";
            return exit_failed;
        }
        std::vector<bindings::LayoutProblem> found;
        if (!bindings::layout_from_manifest(loaded.manifest, nullptr, layout, found)) {
            for (const bindings::LayoutProblem& one : found) {
                if (!one.warning)
                    output.err << layouts::manifest_file << ':' << one.line << ':' << one.column
                               << ": " << one.key << ": " << one.message << '\n';
            }
            return exit_failed;
        }
        input.base = loaded.manifest.base == layouts::Base::none ? bindings::Base::none
                                                                 : bindings::Base::oa_default;
        input.chosen = &layout;
    }

    const bindings::Resolved resolved = bindings::resolve(input);
    print_resolved(resolved, parsed->flags.contains("json"), output);
    return exit_done;
}

} // namespace oa::tool
