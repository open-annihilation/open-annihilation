// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The typed decoder of a director script: walks the document tree key by
// key, fills the Script, and reports every error and warning with its key
// path and position.

#include "oa/formats/oascript.hpp"

#include <array>
#include <string>
#include <string_view>

namespace oa::formats::oascript {

namespace {

/// Zero, the lower bound of every range that must be above it.
constexpr Decimal zero_decimal{0, 0};
/// The powers of ten a whole-number check divides by, up to max_decimal_places.
constexpr std::array<int64_t, max_decimal_places + 1> powers_of_ten{
    1,
    10,
    100,
    1000,
    10000,
    100000,
    1000000,
    10000000,
    100000000,
    1000000000,
    10000000000,
    100000000000,
    1000000000000,
    10000000000000,
    100000000000000,
    1000000000000000,
    10000000000000000,
    100000000000000000,
    1000000000000000000,
};

/// Returns the whole number a decimal stands for.
///
/// @param value the decimal
/// @param[out] whole its value when it has no fractional part
/// @return true when `value` is a whole number
bool whole_value(Decimal value, int64_t& whole) noexcept {
    if (value.places == 0) {
        whole = value.mantissa;
        return true;
    }
    if (value.places > max_decimal_places) {
        whole = 0;
        return value.mantissa == 0;
    }
    const int64_t divisor{powers_of_ten[value.places]};
    if (value.mantissa % divisor != 0)
        return false;
    whole = value.mantissa / divisor;
    return true;
}

/// Tells whether a decimal lies above zero and at most a bound.
///
/// @param value the decimal
/// @param highest the bound
/// @return true for 0 < value <= highest
bool above_zero_at_most(Decimal value, Decimal highest) noexcept {
    return compare_decimals(value, zero_decimal) > 0 && compare_decimals(value, highest) <= 0;
}

/// Walks a document tree and fills a script.
class Decoder {
  public:

    /// Starts a decode that reports into a report.
    ///
    /// @param report the report errors and warnings go to
    explicit Decoder(DecodeReport& report) noexcept : report_{&report} {}

    /// Decodes the whole document.
    ///
    /// @param root the top-level mapping
    /// @param[out] script the script
    void decode(const Node& root, Script& script) {
        if (!expect(root, NodeKind::mapping, ""))
            return;
        bool has_version{};
        bool has_input{};
        bool has_director{};
        for (const Node& entry : root.children) {
            const std::string path{entry.key};
            if (entry.key == "oascript") {
                has_version = true;
                int64_t version{};
                if (read_whole(entry, path, version) && version != int64_t{script_version})
                    error(path, entry.position, "must be " + std::to_string(script_version));
            } else if (entry.key == "input") {
                has_input = true;
                decode_input(entry, path, script.input);
            } else if (entry.key == "output") {
                decode_output(entry, path, script.output);
            } else if (entry.key == "director") {
                has_director = true;
                decode_director(entry, path, script.director);
            } else {
                unknown(entry, path);
            }
        }
        if (!has_version)
            missing("oascript");
        if (!has_input)
            missing("input");
        if (!has_director)
            missing("director");
    }

  private:

    /// Records an error.
    ///
    /// @param path the key path
    /// @param position where its value starts
    /// @param message what is wrong
    void error(std::string path, TextPosition position, std::string message) {
        report_->errors.push_back(Diagnostic{std::move(path), position, std::move(message)});
    }

    /// Records a warning.
    ///
    /// @param path the key path
    /// @param position where its value starts
    /// @param message what is odd
    void warning(std::string path, TextPosition position, std::string message) {
        report_->warnings.push_back(Diagnostic{std::move(path), position, std::move(message)});
    }

    /// Records a required key that is missing.
    ///
    /// @param path the key's path
    void missing(std::string path) { error(std::move(path), TextPosition{}, "is required"); }

    /// Records a key the script format does not define.
    ///
    /// @param entry the entry
    /// @param path its path
    void unknown(const Node& entry, std::string path) {
        error(std::move(path), entry.key_position, "is not a known key");
    }

    /// Returns the path of a mapping entry.
    ///
    /// @param parent the mapping's path
    /// @param key the entry's key
    /// @return parent.key
    static std::string child_path(const std::string& parent, std::string_view key) {
        return parent + "." + std::string{key};
    }

    /// Checks a node's kind, reporting a mismatch.
    ///
    /// @param node the node
    /// @param kind the kind required
    /// @param path the node's path
    /// @return true when the kind matches
    bool expect(const Node& node, NodeKind kind, const std::string& path) {
        if (node.kind == kind)
            return true;
        const char* message{};
        switch (kind) {
        case NodeKind::mapping:
            message = "must be a mapping";
            break;
        case NodeKind::sequence:
            message = "must be a sequence";
            break;
        case NodeKind::string:
            message = "must be a string";
            break;
        case NodeKind::number:
            message = "must be a number";
            break;
        case NodeKind::boolean:
            message = "must be true or false";
            break;
        case NodeKind::null_value:
            message = "must be null";
            break;
        }
        error(path, node.position, message);
        return false;
    }

    /// Reads a number.
    ///
    /// @param node the node
    /// @param path its path
    /// @param[out] value the number
    /// @return true when the node is a number
    bool read_decimal(const Node& node, const std::string& path, Decimal& value) {
        if (!expect(node, NodeKind::number, path))
            return false;
        value = node.number;
        return true;
    }

    /// Reads a whole number.
    ///
    /// @param node the node
    /// @param path its path
    /// @param[out] value the number
    /// @return true when the node is a whole number
    bool read_whole(const Node& node, const std::string& path, int64_t& value) {
        Decimal number{};
        if (!read_decimal(node, path, number))
            return false;
        if (whole_value(number, value))
            return true;
        error(path, node.position, "must be a whole number");
        return false;
    }

    /// Reads a tick: a whole number from 0 to max_tick.
    ///
    /// @param node the node
    /// @param path its path
    /// @param[out] tick the tick
    /// @return true when valid
    bool read_tick(const Node& node, const std::string& path, uint32_t& tick) {
        if (!expect(node, NodeKind::number, path))
            return false;
        int64_t whole{};
        if (!whole_value(node.number, whole) || whole < 0 || whole > int64_t{max_tick}) {
            error(
                path, node.position, "must be a whole number from 0 to " + std::to_string(max_tick)
            );
            return false;
        }
        tick = static_cast<uint32_t>(whole);
        return true;
    }

    /// Reads a rate-like number: above zero, at most a bound, and with at
    /// most max_rate_places decimal places.
    ///
    /// @param node the node
    /// @param path its path
    /// @param highest the bound
    /// @param[out] value the number
    /// @return true when valid
    bool read_limited(const Node& node, const std::string& path, Decimal highest, Decimal& value) {
        Decimal number{};
        if (!read_decimal(node, path, number))
            return false;
        bool valid{true};
        if (!above_zero_at_most(number, highest)) {
            error(path, node.position, "must be above 0 and at most " + decimal_text(highest));
            valid = false;
        }
        if (number.places > max_rate_places) {
            error(
                path,
                node.position,
                "must have at most " + std::to_string(max_rate_places) + " decimal places"
            );
            valid = false;
        }
        if (valid)
            value = number;
        return valid;
    }

    /// Reads one side of the resolution: an even whole number from
    /// min_dimension to max_dimension.
    ///
    /// @param node the node
    /// @param path its path
    /// @param[out] side the side, in pixels
    void read_dimension(const Node& node, const std::string& path, uint32_t& side) {
        if (!expect(node, NodeKind::number, path))
            return;
        int64_t whole{};
        if (!whole_value(node.number, whole) || whole < int64_t{min_dimension} ||
            whole > int64_t{max_dimension}) {
            error(
                path,
                node.position,
                "must be a whole number from " + std::to_string(min_dimension) + " to " +
                    std::to_string(max_dimension)
            );
            return;
        }
        if (whole % 2 != 0) {
            error(path, node.position, "must be an even number");
            return;
        }
        side = static_cast<uint32_t>(whole);
    }

    /// Decodes the input key.
    ///
    /// @param node the value
    /// @param path its path
    /// @param[out] input the input
    void decode_input(const Node& node, const std::string& path, Input& input) {
        if (!expect(node, NodeKind::mapping, path))
            return;
        bool has_recording{};
        const Node* stage{};
        for (const Node& entry : node.children) {
            const std::string entry_path{child_path(path, entry.key)};
            if (entry.key == "demo" || entry.key == "stage") {
                if (entry.key == "demo")
                    has_recording = true;
                else
                    stage = &entry;
                if (!expect(entry, NodeKind::string, entry_path))
                    continue;
                if (entry.text.empty())
                    error(entry_path, entry.position, "must not be empty");
                else
                    (entry.key == "demo" ? input.recording : input.stage) = entry.text;
            } else if (entry.key == "tickrate") {
                (void)read_limited(entry, entry_path, max_tickrate, input.tickrate);
            } else {
                unknown(entry, entry_path);
            }
        }
        if (has_recording && stage != nullptr)
            error(
                child_path(path, "stage"),
                stage->position,
                "cannot be given with a demo: a script plays one or the other"
            );
        else if (!has_recording && stage == nullptr)
            missing(child_path(path, "demo"));
    }

    /// Decodes the output key.
    ///
    /// @param node the value
    /// @param path its path
    /// @param[out] output the output
    void decode_output(const Node& node, const std::string& path, Output& output) {
        if (!expect(node, NodeKind::mapping, path))
            return;
        for (const Node& entry : node.children) {
            const std::string entry_path{child_path(path, entry.key)};
            if (entry.key == "resolution") {
                decode_resolution(entry, entry_path, output);
            } else if (entry.key == "framerate") {
                (void)read_limited(entry, entry_path, max_framerate, output.framerate);
            } else if (entry.key == "chunking") {
                decode_chunking(entry, entry_path, output.chunking);
            } else if (entry.key == "showUx") {
                if (expect(entry, NodeKind::boolean, entry_path))
                    output.show_ux = entry.boolean;
            } else {
                unknown(entry, entry_path);
            }
        }
    }

    /// Decodes output.resolution.
    ///
    /// @param node the value
    /// @param path its path
    /// @param[out] output the output its width and height go to
    void decode_resolution(const Node& node, const std::string& path, Output& output) {
        if (!expect(node, NodeKind::mapping, path))
            return;
        for (const Node& entry : node.children) {
            const std::string entry_path{child_path(path, entry.key)};
            if (entry.key == "width")
                read_dimension(entry, entry_path, output.width);
            else if (entry.key == "height")
                read_dimension(entry, entry_path, output.height);
            else
                unknown(entry, entry_path);
        }
    }

    /// Decodes output.chunking.
    ///
    /// @param node the value
    /// @param path its path
    /// @param[out] chunking the chunking
    void decode_chunking(const Node& node, const std::string& path, Chunking& chunking) {
        if (!expect(node, NodeKind::mapping, path))
            return;
        const Node* length{};
        for (const Node& entry : node.children) {
            const std::string entry_path{child_path(path, entry.key)};
            if (entry.key == "mode") {
                if (!expect(entry, NodeKind::string, entry_path))
                    continue;
                if (entry.text == "seconds")
                    chunking.mode = ChunkMode::seconds;
                else if (entry.text == "ticks")
                    chunking.mode = ChunkMode::ticks;
                else
                    error(entry_path, entry.position, "must be seconds or ticks");
            } else if (entry.key == "length") {
                length = &entry;
            } else {
                unknown(entry, entry_path);
            }
        }
        const bool ticks{chunking.mode == ChunkMode::ticks};
        if (length == nullptr) {
            chunking.length = ticks ? default_chunk_ticks : default_chunk_seconds;
            return;
        }
        const std::string length_path{child_path(path, "length")};
        Decimal value{};
        if (!read_limited(*length, length_path, ticks ? max_chunk_ticks : max_chunk_seconds, value))
            return;
        int64_t whole{};
        if (ticks && !whole_value(value, whole)) {
            error(length_path, length->position, "must be a whole number of ticks");
            return;
        }
        chunking.length = value;
    }

    /// Decodes the director key.
    ///
    /// @param node the value
    /// @param path its path
    /// @param[out] director the director
    void decode_director(const Node& node, const std::string& path, Director& director) {
        if (!expect(node, NodeKind::mapping, path))
            return;
        bool has_shots{};
        const Node* end_tick{};
        std::string end_tick_path{};
        std::optional<uint32_t> last_tick{};
        for (const Node& entry : node.children) {
            const std::string entry_path{child_path(path, entry.key)};
            if (entry.key == "shots") {
                has_shots = true;
                last_tick = decode_shots(entry, entry_path, director.shots);
            } else if (entry.key == "endTick") {
                end_tick = &entry;
                end_tick_path = entry_path;
            } else {
                unknown(entry, entry_path);
            }
        }
        if (!has_shots)
            missing(child_path(path, "shots"));
        if (end_tick == nullptr)
            return;
        uint32_t tick{};
        if (!read_tick(*end_tick, end_tick_path, tick))
            return;
        director.end_tick = tick;
        if (last_tick.has_value() && tick <= *last_tick)
            error(end_tick_path, end_tick->position, "must be greater than the last shot's tick");
    }

    /// Decodes director.shots.
    ///
    /// @param node the value
    /// @param path its path
    /// @param[out] shots the shots
    /// @return the last shot's tick, when every shot's tick was valid
    std::optional<uint32_t>
    decode_shots(const Node& node, const std::string& path, std::vector<Shot>& shots) {
        if (!expect(node, NodeKind::sequence, path))
            return std::nullopt;
        if (node.children.empty()) {
            error(path, node.position, "must hold at least one shot");
            return std::nullopt;
        }
        if (node.children.size() > max_shot_count) {
            error(
                path,
                node.position,
                "must hold at most " + std::to_string(max_shot_count) + " shots"
            );
            return std::nullopt;
        }
        shots.clear();
        shots.reserve(node.children.size());
        // The tick of the shot before, when it had one.
        uint32_t previous_tick{};
        bool has_previous_tick{};
        bool ticks_valid{true};
        for (size_t index{}; index < node.children.size(); ++index) {
            const std::string shot_path{path + "[" + std::to_string(index) + "]"};
            Shot shot{};
            std::optional<uint32_t> tick{};
            decode_shot(node.children[index], shot_path, index == 0, shot, tick);
            if (!tick.has_value()) {
                ticks_valid = false;
                has_previous_tick = false;
            } else {
                if (has_previous_tick && *tick <= previous_tick) {
                    const Node* tick_node{find_entry(node.children[index], "tick")};
                    error(
                        shot_path + ".tick",
                        tick_node != nullptr ? tick_node->position : TextPosition{},
                        "must be greater than the previous shot's tick"
                    );
                }
                previous_tick = *tick;
                has_previous_tick = true;
            }
            shots.push_back(std::move(shot));
        }
        if (!ticks_valid)
            return std::nullopt;
        return shots.back().tick;
    }

    /// Decodes one shot.
    ///
    /// @param node the value
    /// @param path its path
    /// @param first whether it is the first shot
    /// @param[out] shot the shot
    /// @param[out] tick its tick, when valid
    void decode_shot(
        const Node& node,
        const std::string& path,
        bool first,
        Shot& shot,
        std::optional<uint32_t>& tick
    ) {
        shot.position = node.position;
        if (!expect(node, NodeKind::mapping, path))
            return;
        bool has_tick{};
        bool has_camera_end{};
        bool has_camera_start{};
        for (const Node& entry : node.children) {
            const std::string entry_path{child_path(path, entry.key)};
            if (entry.key == "tick") {
                has_tick = true;
                uint32_t value{};
                if (read_tick(entry, entry_path, value)) {
                    shot.tick = value;
                    tick = value;
                }
            } else if (entry.key == "cameraStart") {
                has_camera_start = true;
                CameraState camera{};
                decode_camera(entry, entry_path, camera);
                shot.camera_start = camera;
            } else if (entry.key == "cameraEnd") {
                has_camera_end = true;
                decode_camera(entry, entry_path, shot.camera_end);
            } else if (entry.key == "motion") {
                decode_motion(entry, entry_path, shot.motion);
            } else if (entry.key == "transition") {
                if (first) {
                    error(entry_path, entry.position, "is not allowed on the first shot");
                    continue;
                }
                Transition transition{};
                if (decode_transition(entry, entry_path, transition))
                    shot.transition = transition;
            } else {
                unknown(entry, entry_path);
            }
        }
        if (!has_tick)
            missing(child_path(path, "tick"));
        if (first && !has_camera_start)
            error(child_path(path, "cameraStart"), TextPosition{}, "is required on the first shot");
        if (!has_camera_end)
            missing(child_path(path, "cameraEnd"));
    }

    /// Decodes a camera: cameraStart or cameraEnd.
    ///
    /// @param node the value
    /// @param path its path
    /// @param[out] camera the camera
    void decode_camera(const Node& node, const std::string& path, CameraState& camera) {
        if (!expect(node, NodeKind::mapping, path))
            return;
        bool has_position{};
        for (const Node& entry : node.children) {
            const std::string entry_path{child_path(path, entry.key)};
            if (entry.key == "position") {
                has_position = true;
                decode_position(entry, entry_path, camera.position);
            } else if (entry.key == "orientation") {
                CameraOrientation orientation{};
                if (decode_orientation(entry, entry_path, orientation))
                    camera.orientation = orientation;
            } else {
                unknown(entry, entry_path);
            }
        }
        if (!has_position)
            missing(child_path(path, "position"));
    }

    /// Decodes a camera's position.
    ///
    /// @param node the value
    /// @param path its path
    /// @param[out] position the position
    void decode_position(const Node& node, const std::string& path, CameraPosition& position) {
        if (!expect(node, NodeKind::mapping, path))
            return;
        bool has_x{};
        bool has_y{};
        bool has_z{};
        for (const Node& entry : node.children) {
            const std::string entry_path{child_path(path, entry.key)};
            if (entry.key == "x") {
                has_x = true;
                (void)read_decimal(entry, entry_path, position.x);
            } else if (entry.key == "y") {
                has_y = true;
                Decimal height{};
                if (!read_decimal(entry, entry_path, height))
                    continue;
                if (compare_decimals(height, zero_decimal) <= 0)
                    error(entry_path, entry.position, "must be above 0");
                else
                    position.y = height;
            } else if (entry.key == "z") {
                has_z = true;
                (void)read_decimal(entry, entry_path, position.z);
            } else {
                unknown(entry, entry_path);
            }
        }
        if (!has_x)
            missing(child_path(path, "x"));
        if (!has_y)
            missing(child_path(path, "y"));
        if (!has_z)
            missing(child_path(path, "z"));
    }

    /// Decodes a camera's orientation, warning about a roll that is not
    /// zero.
    ///
    /// @param node the value
    /// @param path its path
    /// @param[out] orientation the angles
    /// @return true when the value is a mapping of numbers
    bool
    decode_orientation(const Node& node, const std::string& path, CameraOrientation& orientation) {
        if (!expect(node, NodeKind::mapping, path))
            return false;
        bool valid{true};
        for (const Node& entry : node.children) {
            const std::string entry_path{child_path(path, entry.key)};
            Decimal* angle{};
            if (entry.key == "pitch")
                angle = &orientation.pitch;
            else if (entry.key == "yaw")
                angle = &orientation.yaw;
            else if (entry.key == "roll")
                angle = &orientation.roll;
            if (angle == nullptr) {
                unknown(entry, entry_path);
                valid = false;
                continue;
            }
            if (!read_decimal(entry, entry_path, *angle)) {
                valid = false;
                continue;
            }
            if (angle == &orientation.roll && angle->mantissa != 0)
                warning(entry_path, entry.position, "is ignored: a camera does not roll");
        }
        return valid;
    }

    /// Decodes a shot's motion: exactly one of linear (an empty mapping) and spring.
    ///
    /// @param node the value
    /// @param path its path
    /// @param[out] motion the motion
    void decode_motion(const Node& node, const std::string& path, ShotMotion& motion) {
        if (!expect(node, NodeKind::mapping, path))
            return;
        size_t kinds{};
        for (const Node& entry : node.children) {
            const std::string entry_path{child_path(path, entry.key)};
            if (entry.key == "linear") {
                ++kinds;
                if (entry.kind != NodeKind::mapping || !entry.children.empty()) {
                    error(entry_path, entry.position, "must be an empty mapping");
                    continue;
                }
                motion = ShotMotion{};
            } else if (entry.key == "spring") {
                ++kinds;
                decode_spring(entry, entry_path, motion);
            } else {
                unknown(entry, entry_path);
            }
        }
        if (kinds != 1)
            error(path, node.position, "must hold exactly one of linear and spring");
    }

    /// Decodes motion.spring.
    ///
    /// @param node the value
    /// @param path its path
    /// @param[out] motion the motion
    void decode_spring(const Node& node, const std::string& path, ShotMotion& motion) {
        if (!expect(node, NodeKind::mapping, path))
            return;
        motion.kind = Motion::spring;
        bool has_frequency{};
        for (const Node& entry : node.children) {
            const std::string entry_path{child_path(path, entry.key)};
            if (entry.key == "frequency") {
                has_frequency = true;
                Decimal value{};
                if (!read_decimal(entry, entry_path, value))
                    continue;
                if (!above_zero_at_most(value, max_spring_frequency))
                    error(
                        entry_path,
                        entry.position,
                        "must be above 0 and at most " + decimal_text(max_spring_frequency)
                    );
                else
                    motion.frequency = value;
            } else if (entry.key == "dampingRatio") {
                Decimal value{};
                if (!read_decimal(entry, entry_path, value))
                    continue;
                if (!above_zero_at_most(value, max_damping_ratio))
                    error(
                        entry_path,
                        entry.position,
                        "must be above 0 and at most " + decimal_text(max_damping_ratio)
                    );
                else
                    motion.damping_ratio = value;
            } else {
                unknown(entry, entry_path);
            }
        }
        if (!has_frequency)
            missing(child_path(path, "frequency"));
    }

    /// Decodes a shot's transition.
    ///
    /// @param node the value
    /// @param path its path
    /// @param[out] transition the transition
    /// @return true when it is valid
    bool decode_transition(const Node& node, const std::string& path, Transition& transition) {
        if (!expect(node, NodeKind::mapping, path))
            return false;
        bool valid{true};
        bool has_duration{};
        for (const Node& entry : node.children) {
            const std::string entry_path{child_path(path, entry.key)};
            if (entry.key == "type") {
                if (!expect(entry, NodeKind::string, entry_path)) {
                    valid = false;
                } else if (entry.text == "fade") {
                    transition.kind = TransitionKind::fade;
                } else if (entry.text == "wipe") {
                    transition.kind = TransitionKind::wipe;
                } else if (entry.text == "dissolve") {
                    transition.kind = TransitionKind::dissolve;
                } else if (entry.text == "checkerboard") {
                    transition.kind = TransitionKind::checkerboard;
                } else {
                    error(
                        entry_path, entry.position, "must be fade, wipe, dissolve or checkerboard"
                    );
                    valid = false;
                }
            } else if (entry.key == "duration") {
                has_duration = true;
                if (!read_limited(entry, entry_path, max_transition_seconds, transition.duration))
                    valid = false;
            } else {
                unknown(entry, entry_path);
                valid = false;
            }
        }
        if (!has_duration) {
            missing(child_path(path, "duration"));
            valid = false;
        }
        return valid;
    }

    DecodeReport* report_{};
};

} // namespace

bool decode_script(const Node& root, Script& script, DecodeReport& report) {
    script = Script{};
    report = DecodeReport{};
    Decoder decoder{report};
    decoder.decode(root, script);
    return report.errors.empty();
}

bool read_script(std::span<const uint8_t> text, Script& script, DecodeReport& report) {
    Node root{};
    ReadError read_error{};
    if (!read_document(text, root, read_error)) {
        script = Script{};
        report = DecodeReport{};
        report.errors.push_back(
            Diagnostic{std::string{}, read_error.position, read_status_message(read_error.status)}
        );
        return false;
    }
    return decode_script(root, script, report);
}

} // namespace oa::formats::oascript
