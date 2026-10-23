// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// `oa-tool brain net`: an ONNX model becomes a .oann. Scales come from a
// calibration file, weights are quantised, and the difference is measured by
// running N02's and N06's integer network on the same samples.

#include "arguments.hpp"
#include "command.hpp"
#include "files.hpp"
#include "onnx.hpp"

#include "oa/base/sha256.hpp"
#include "oa/data/neural/arithmetic.hpp"
#include "oa/data/neural/network.hpp"
#include "oa/formats/json.hpp"
#include "oa/formats/oann/oann.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <ostream>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace oa::tool {
namespace {

namespace fs = std::filesystem;
namespace json = oa::formats::json;
namespace neural = oa::data::neural;
namespace oann = oa::formats::oann;

constexpr int64_t k_unit = 65536;
constexpr std::size_t k_max_samples = 100000;
constexpr int k_min_opset = 11;
constexpr int k_max_opset = 21;

/// A planned layer, in run order. Names are the ONNX names, resolved through
/// Identity and Dropout.
struct Plan {
    oann::LayerKind kind{};
    int number{};
    std::string op;
    std::string output;
    std::string public_name;
    std::vector<std::string> inputs;
    std::string weight;
    std::string bias;
    bool transpose_weight{};
    bool graph_output{};
    bool argmax{};
    std::vector<uint32_t> dims;
    uint8_t kernel_h{};
    uint8_t kernel_w{};
    uint8_t stride_h{};
    uint8_t stride_w{};
    uint8_t pad_h{};
    uint8_t pad_w{};
    uint8_t axis{};
    int16_t alpha{};
    double alpha_real{};
    int8_t exponent{};
    double magnitude{};               ///< largest absolute activation over the samples
    std::string oann_name;            ///< the tensor name written into the .oann
    std::vector<uint32_t> input_dims; ///< conv2d and maxpool input shape, batch removed
};

/// One model input after the batch dimension is removed.
struct InputRec {
    std::string name;
    std::vector<uint32_t> dims;
    bool batch_only{};
    bool index{};
    double magnitude{};
    int8_t exponent{};
};

/// One calibration sample: each model input as whole numbers in 65536ths.
struct Sample {
    std::vector<std::vector<int64_t>> columns;
};

/// Where a tensor stands while the graph is walked.
struct Source {
    enum class Kind : uint8_t { input, made, constant };
    Kind kind{};
    std::vector<uint32_t> dims;
    bool batch_only{};
    bool index{};
    std::size_t slot{};
};

/// Returns a path as UTF-8 with '/' between parts.
///
/// @param path the path
/// @return its UTF-8 spelling
std::string utf8_of(const fs::path& path) {
    const auto text = path.generic_u8string();
    return {text.begin(), text.end()};
}

/// Names a node in a refusal.
///
/// @param number the node's place in the file, starting at 1
/// @param op the operator
/// @return `node N (Op)`
std::string node_text(int number, std::string_view op) {
    return "node " + std::to_string(number) + " (" + std::string(op) + ")";
}

/// Refuses a node.
///
/// @param number the node's place, starting at 1
/// @param op the operator
/// @param text the reason
/// @throws Failure always
[[noreturn]] void refuse_node(int number, std::string_view op, std::string_view text) {
    throw Failure(node_text(number, op) + ": " + std::string(text));
}

/// The product of a shape, or a refusal when it overflows.
///
/// @param dims the shape
/// @param what names the tensor in a refusal
/// @return the element count
uint64_t element_product(const std::vector<uint32_t>& dims, std::string_view what) {
    uint64_t count = 1;
    for (const uint32_t dim : dims) {
        if (dim != 0 && count > ~uint64_t{0} / dim)
            throw Failure(std::string(what) + ": the shape is too large");
        count *= dim;
    }
    return count;
}

/// Holds a dimension to the range a .oann stores.
///
/// @param dim the dimension
/// @param what names the tensor in a refusal
/// @return the dimension
uint32_t held_dim(int64_t dim, std::string_view what) {
    if (dim <= 0 || dim > static_cast<int64_t>(oann::max_dimension)) {
        throw Failure(
            std::string(what) + ": a dimension is " + std::to_string(dim) + ", and must be 1 to " +
            std::to_string(oann::max_dimension)
        );
    }
    return static_cast<uint32_t>(dim);
}

/// Formats a shape as `[d, d]`.
///
/// @param dims the shape
/// @return the text
std::string shape_text(const std::vector<uint32_t>& dims) {
    std::string text = "[";
    for (std::size_t index = 0; index < dims.size(); ++index) {
        if (index != 0)
            text += ", ";
        text += std::to_string(dims[index]);
    }
    text += "]";
    return text;
}

/// Formats a real number the way the report prints one.
///
/// @param value a finite number
/// @return the shortest text `%.6g` produces
std::string format_real(double value) {
    char buffer[64];
    const int wrote = std::snprintf(buffer, sizeof(buffer), "%.6g", value);
    if (wrote <= 0)
        return "0";
    return std::string(buffer, static_cast<std::size_t>(wrote));
}

/// The smallest exponent at which `magnitude` fits in `mantissa` times 2^e.
///
/// A magnitude of 0 uses `zero_exponent` instead of the smallest stored one.
/// A magnitude that fits at no exponent in range uses 16, and quantising
/// saturates it.
///
/// @param magnitude the largest absolute value
/// @param mantissa 32767 for an activation, 127 or 32767 for a weight
/// @param zero_exponent the exponent used when the magnitude is 0
/// @return an exponent from -48 to 16
int8_t exponent_for(double magnitude, double mantissa, int zero_exponent) {
    if (!(magnitude > 0.0))
        return static_cast<int8_t>(zero_exponent);
    for (int exponent = oann::min_exponent; exponent <= oann::max_exponent; ++exponent) {
        if (magnitude <= std::ldexp(mantissa, exponent))
            return static_cast<int8_t>(exponent);
    }
    return oann::max_exponent;
}

/// Rounds a real onto a power-of-two scale, half away from zero.
///
/// @param value the real
/// @param exponent the scale, as a stored q times 2^exponent
/// @param low the lowest stored integer
/// @param high the highest stored integer
/// @return the stored integer, held to the range
int64_t quantise(double value, int exponent, int64_t low, int64_t high) {
    const double rounded = std::round(value / std::ldexp(1.0, exponent));
    if (rounded < static_cast<double>(low))
        return low;
    if (rounded > static_cast<double>(high))
        return high;
    return static_cast<int64_t>(rounded);
}

/// Appends a little-endian integer.
///
/// @param[out] bytes the buffer
/// @param value the integer
/// @param width 1, 2 or 4
void store_le(std::vector<uint8_t>& bytes, int64_t value, int width) {
    const auto bits = static_cast<uint32_t>(static_cast<int32_t>(value));
    for (int index = 0; index < width; ++index)
        bytes.push_back(static_cast<uint8_t>(bits >> static_cast<unsigned>(index * 8)));
}

/// Makes an ONNX name into a tensor name: lower case, `_` for every other
/// byte, a leading letter, at most 31 bytes, and different from `taken`.
///
/// @param raw the ONNX name
/// @param[in,out] taken names already used
/// @return the tensor name
std::string unique_name(std::string raw, std::set<std::string>& taken) {
    for (char& character : raw) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte >= 'A' && byte <= 'Z')
            character = static_cast<char>(byte - 'A' + 'a');
        else if (!((byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9')))
            character = '_';
    }
    if (raw.empty() || raw[0] < 'a' || raw[0] > 'z')
        raw.insert(raw.begin(), 't');
    if (raw.size() > 31)
        raw.resize(31);
    if (taken.insert(raw).second)
        return raw;
    for (int number = 2; number < 100000; ++number) {
        const std::string suffix = "_" + std::to_string(number);
        std::string stem = raw;
        if (stem.size() + suffix.size() > 31)
            stem.resize(31 - suffix.size());
        const std::string candidate = stem + suffix;
        if (taken.insert(candidate).second)
            return candidate;
    }
    throw Failure("a tensor name cannot be made unique");
}

/// Finds an attribute by name.
///
/// @param node the node
/// @param name the attribute
/// @return the attribute, or null
const OnnxAttribute* attribute(const OnnxNode& node, std::string_view name) {
    for (const OnnxAttribute& attr : node.attributes) {
        if (attr.name == name)
            return &attr;
    }
    return nullptr;
}

/// Reads an integer attribute.
///
/// @param node the node
/// @param name the attribute
/// @param fallback the value when the attribute is absent
/// @param number the node number, for a refusal
/// @return the integer
int64_t attr_int(const OnnxNode& node, std::string_view name, int64_t fallback, int number) {
    const OnnxAttribute* attr = attribute(node, name);
    if (attr == nullptr)
        return fallback;
    if (attr->i.has_value())
        return *attr->i;
    if (attr->ints.size() == 1)
        return attr->ints[0];
    refuse_node(number, node.op_type, std::string(name) + " is not an integer");
}

/// Reads a float attribute.
///
/// @param node the node
/// @param name the attribute
/// @param fallback the value when the attribute is absent
/// @param number the node number, for a refusal
/// @return the float, widened
double attr_float(const OnnxNode& node, std::string_view name, double fallback, int number) {
    const OnnxAttribute* attr = attribute(node, name);
    if (attr == nullptr)
        return fallback;
    if (attr->f.has_value())
        return static_cast<double>(*attr->f);
    if (attr->floats.size() == 1)
        return static_cast<double>(attr->floats[0]);
    refuse_node(number, node.op_type, std::string(name) + " is not a float");
}

/// Reads a string attribute.
///
/// @param node the node
/// @param name the attribute
/// @param fallback the value when the attribute is absent
/// @param number the node number, for a refusal
/// @return the text
std::string
attr_string(const OnnxNode& node, std::string_view name, std::string fallback, int number) {
    const OnnxAttribute* attr = attribute(node, name);
    if (attr == nullptr)
        return fallback;
    if (attr->s.has_value())
        return *attr->s;
    refuse_node(number, node.op_type, std::string(name) + " is not a string");
}

/// Reads a list of integers, empty when the attribute is absent.
///
/// @param node the node
/// @param name the attribute
/// @param number the node number, for a refusal
/// @return the integers
std::vector<int64_t> attr_ints(const OnnxNode& node, std::string_view name, int number) {
    const OnnxAttribute* attr = attribute(node, name);
    if (attr == nullptr)
        return {};
    if (!attr->ints.empty())
        return attr->ints;
    if (attr->i.has_value())
        return {*attr->i};
    refuse_node(number, node.op_type, std::string(name) + " is not a list of integers");
}

/// One input of a node, or empty when that input was omitted.
///
/// @param node the node
/// @param index the input's place
/// @return the name, or empty
std::string input_at(const OnnxNode& node, std::size_t index) {
    if (index >= node.inputs.size())
        return {};
    return node.inputs[index];
}

/// The node's first output.
///
/// @param node the node
/// @param number the node number, for a refusal
/// @return the output name
std::string one_output(const OnnxNode& node, int number) {
    if (node.outputs.empty() || node.outputs[0].empty())
        refuse_node(number, node.op_type, "an output has no name");
    return node.outputs[0];
}

/// Feature shape of a model input or output: the batch dimension is removed.
///
/// A tensor that is only the batch becomes `[1]`, and `batch_only` is set so
/// a Gather does not treat that as a length.
///
/// @param value the ONNX value
/// @param[out] batch_only true when the ONNX shape was only the batch
/// @return the feature shape
std::vector<uint32_t> feature_dims(const OnnxValue& value, bool& batch_only) {
    if (value.dims.empty())
        throw Failure("input " + value.name + ": the shape has no batch dimension");
    const OnnxDim& batch = value.dims[0];
    if (batch.value.has_value()) {
        if (*batch.value != 1) {
            throw Failure(
                "input " + value.name + ": the batch dimension is " + std::to_string(*batch.value) +
                ", and must be 1 or a name"
            );
        }
    } else if (batch.param.empty()) {
        throw Failure("input " + value.name + ": the batch dimension is missing");
    }
    std::vector<uint32_t> dims;
    for (std::size_t index = 1; index < value.dims.size(); ++index) {
        const OnnxDim& dim = value.dims[index];
        if (!dim.value.has_value())
            throw Failure("input " + value.name + ": a dimension after the batch is a name");
        dims.push_back(held_dim(*dim.value, value.name));
    }
    batch_only = dims.empty();
    if (dims.empty())
        dims.push_back(1);
    if (dims.size() > 4)
        throw Failure("input " + value.name + ": the rank is above 4");
    return dims;
}

/// Feature shape of a declared output. An argmax may be `[batch]` or `[batch, 1]`.
///
/// @param value the declared output
/// @param argmax true when the layer is an argmax
/// @param[out] ok false when the value carries no shape to compare
/// @return the feature shape
std::vector<uint32_t> declared_dims(const OnnxValue& value, bool argmax, bool& ok) {
    ok = !value.dims.empty();
    if (!ok)
        return {};
    bool batch_only = false;
    std::vector<uint32_t> dims = feature_dims(value, batch_only);
    if (argmax && batch_only)
        return {};
    return dims;
}

/// Follows Identity and Dropout aliases.
///
/// @param name a tensor name
/// @param alias output name to the tensor it copies
/// @return the tensor that holds the values
std::string resolve(std::string name, const std::map<std::string, std::string>& alias) {
    for (int hops = 0; hops < 1024; ++hops) {
        const auto found = alias.find(name);
        if (found == alias.end())
            return name;
        name = found->second;
    }
    throw Failure("an Identity cycle includes " + name);
}

/// Reads the calibration file into at most 100,000 samples.
///
/// @param path the file
/// @param inputs the model inputs, in order
/// @param[out] skipped sample lines past the cap
/// @param[in,out] output receives the cap warning
/// @return the samples
std::vector<Sample> read_calibration(
    const fs::path& path, const std::vector<InputRec>& inputs, std::size_t& skipped, Output& output
) {
    std::ifstream in(path);
    if (!in)
        throw Failure("cannot read input: " + utf8_of(path));
    std::vector<Sample> samples;
    skipped = 0;
    std::string line;
    bool first = true;
    int line_number = 0;
    while (std::getline(in, line)) {
        ++line_number;
        if (first) {
            first = false;
            if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
                static_cast<unsigned char>(line[1]) == 0xBB &&
                static_cast<unsigned char>(line[2]) == 0xBF) {
                line.erase(0, 3);
            }
        }
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;
        const std::string where = "calibration line " + std::to_string(line_number);
        json::JsonError error;
        const std::optional<json::Json> parsed = json::parse_json(line, error);
        if (!parsed || parsed->type() != json::JsonType::object)
            throw Failure(where + ": the line is not a JSON object");
        const json::Json* kind = parsed->find("kind");
        if (kind == nullptr)
            continue;
        const std::string* kind_text = kind->string();
        if (kind_text == nullptr)
            throw Failure(where + ": kind is not a string");
        if (*kind_text != "sample")
            continue;
        if (samples.size() >= k_max_samples) {
            ++skipped;
            continue;
        }
        const json::Json* features = parsed->find("features");
        if (features == nullptr || features->type() != json::JsonType::object)
            throw Failure(where + ": feature " + inputs[0].name + " is missing");
        Sample sample;
        sample.columns.resize(inputs.size());
        for (std::size_t index = 0; index < inputs.size(); ++index) {
            const InputRec& input = inputs[index];
            const json::Json* feature = features->find(input.name);
            if (feature == nullptr || feature->type() != json::JsonType::object)
                throw Failure(where + ": feature " + input.name + " is missing");
            const json::Json* shape = feature->find("shape");
            const json::Json* values = feature->find("values");
            if (shape == nullptr || values == nullptr || shape->type() != json::JsonType::array ||
                values->type() != json::JsonType::array) {
                throw Failure(where + ": feature " + input.name + " needs a shape and values");
            }
            int64_t product = 1;
            for (const json::Json& axis : shape->elements()) {
                const std::optional<int64_t> length = axis.integer();
                if (!length.has_value() || *length < 0) {
                    throw Failure(
                        where + ": feature " + input.name +
                        " holds a value that is not a whole number"
                    );
                }
                if (*length != 0 && product > (INT64_C(0x7fffffffffffffff) / *length))
                    throw Failure(where + ": feature " + input.name + ": the shape is too large");
                product *= *length;
            }
            std::vector<int64_t> column;
            column.reserve(values->elements().size());
            for (const json::Json& item : values->elements()) {
                const std::optional<int64_t> number = item.integer();
                if (!number.has_value()) {
                    throw Failure(
                        where + ": feature " + input.name +
                        " holds a value that is not a whole number"
                    );
                }
                column.push_back(*number);
            }
            if (product != static_cast<int64_t>(column.size())) {
                throw Failure(
                    where + ": feature " + input.name + " has a shape of " +
                    std::to_string(product) + " and " + std::to_string(column.size()) + " values"
                );
            }
            const uint64_t expect = element_product(input.dims, input.name);
            if (column.size() != expect) {
                throw Failure(
                    where + ": feature " + input.name + " has " + std::to_string(column.size()) +
                    " values and the input has " + std::to_string(expect)
                );
            }
            sample.columns[index] = std::move(column);
        }
        samples.push_back(std::move(sample));
    }
    if (in.bad())
        throw Failure("cannot read input: " + utf8_of(path));
    if (samples.empty())
        throw Failure("the calibration file holds no sample");
    if (skipped != 0) {
        output.err << "warning: " << k_max_samples << " calibration samples were read and "
                   << skipped << " more were skipped\n";
    }
    return samples;
}

/// A graph of layers, its inputs and the constants those layers read.
struct Graph {
    std::vector<InputRec> inputs;
    std::vector<Plan> layers;
    std::map<std::string, const OnnxTensor*> constants;
    int64_t opset = 0;
};

/// The default-domain opset, which must be 11 to 21.
///
/// @param model the ONNX model
/// @return the opset version
int64_t require_opset(const OnnxModel& model) {
    bool saw = false;
    int64_t version = 0;
    for (const OnnxOpset& opset : model.opsets) {
        if (!opset.domain.empty() && opset.domain != "ai.onnx")
            throw Failure("domain " + opset.domain + " is not the default domain");
        saw = true;
        version = opset.version;
    }
    if (!saw)
        throw Failure("the model names no opset");
    if (version < k_min_opset || version > k_max_opset) {
        throw Failure(
            "the opset is " + std::to_string(version) + ", and a model is converted from opset " +
            std::to_string(k_min_opset) + " to " + std::to_string(k_max_opset)
        );
    }
    return version;
}

/// Requires every listed integer to be 1.
///
/// @param values the integers, empty when the attribute was omitted
/// @param number the node number
/// @param op the operator
/// @param name the attribute, for a refusal
void require_ones(
    const std::vector<int64_t>& values, int number, std::string_view op, std::string_view name
) {
    for (const int64_t value : values) {
        if (value != 1)
            refuse_node(number, op, std::string(name) + " is not 1");
    }
}

/// The spatial length one conv2d or maxpool axis produces.
///
/// @param input the input length
/// @param pad padding on one side
/// @param kernel the window
/// @param stride the step
/// @param number the node number
/// @param op the operator
/// @return the output length
uint32_t spatial_out(
    uint32_t input, uint32_t pad, uint32_t kernel, uint32_t stride, int number, std::string_view op
) {
    const uint64_t padded = uint64_t{input} + pad + pad;
    if (kernel == 0 || stride == 0 || padded < kernel)
        refuse_node(number, op, "the window does not fit the input");
    const uint64_t produced = (padded - kernel) / stride + 1;
    if (produced > oann::max_dimension)
        refuse_node(number, op, "an output dimension is above 65535");
    return static_cast<uint32_t>(produced);
}

/// A symmetric pair of pads, or a refusal.
///
/// @param pads four pads, or empty for none
/// @param number the node number
/// @param op the operator
/// @param[out] pad_h padding above and below
/// @param[out] pad_w padding left and right
void symmetric_pads(
    const std::vector<int64_t>& pads,
    int number,
    std::string_view op,
    uint8_t& pad_h,
    uint8_t& pad_w
) {
    if (pads.empty()) {
        pad_h = 0;
        pad_w = 0;
        return;
    }
    if (pads.size() != 4)
        refuse_node(number, op, "pads are four values");
    if (pads[0] != pads[2] || pads[1] != pads[3] || pads[0] < 0 || pads[1] < 0)
        refuse_node(number, op, "pads are not symmetric");
    if (pads[0] > 14 || pads[1] > 14)
        refuse_node(number, op, "a pad is above 14");
    pad_h = static_cast<uint8_t>(pads[0]);
    pad_w = static_cast<uint8_t>(pads[1]);
}

/// One kernel or stride axis, 1 to 15.
///
/// @param value the attribute value
/// @param number the node number
/// @param op the operator
/// @param name the axis, for a refusal
/// @return the axis
uint8_t spatial_axis(int64_t value, int number, std::string_view op, std::string_view name) {
    if (value < 1 || value > 15)
        refuse_node(number, op, std::string(name) + " must be 1 to 15");
    return static_cast<uint8_t>(value);
}

/// Builds the layer list. Identity and Dropout copy a name. A MatMul that one
/// later Add consumes, adding a constant, becomes one dense layer.
///
/// @param model the ONNX model
/// @return the graph
Graph plan_graph(const OnnxModel& model) {
    Graph graph;
    graph.opset = require_opset(model);
    std::map<std::string, Source> sources;
    for (std::size_t index = 0; index < model.initializers.size(); ++index) {
        const OnnxTensor& tensor = model.initializers[index];
        if (!sources.emplace(tensor.name, Source{Source::Kind::constant, {}, false, false, index})
                 .second) {
            throw Failure("an initializer named " + tensor.name + " is listed twice");
        }
        graph.constants.emplace(tensor.name, &tensor);
    }
    for (const OnnxValue& value : model.inputs) {
        if (graph.constants.count(value.name) != 0)
            continue;
        if (value.elem_type != 1 && value.elem_type != 7) {
            throw Failure(
                "input " + value.name + ": the element type is " + std::to_string(value.elem_type) +
                ", and must be float or int64"
            );
        }
        InputRec input;
        input.name = value.name;
        input.dims = feature_dims(value, input.batch_only);
        input.index = value.elem_type == 7;
        Source source;
        source.kind = Source::Kind::input;
        source.dims = input.dims;
        source.batch_only = input.batch_only;
        source.index = input.index;
        source.slot = graph.inputs.size();
        if (!sources.emplace(value.name, source).second)
            throw Failure("input " + value.name + " is listed twice");
        graph.inputs.push_back(std::move(input));
    }
    if (graph.inputs.empty())
        throw Failure("the model names no input");
    if (model.outputs.empty())
        throw Failure("the model names no output");

    std::set<std::string> graph_outputs;
    for (const OnnxValue& value : model.outputs)
        graph_outputs.insert(value.name);
    std::map<std::string, std::vector<std::size_t>> consumers;
    for (std::size_t index = 0; index < model.nodes.size(); ++index) {
        for (const std::string& name : model.nodes[index].inputs) {
            if (!name.empty())
                consumers[name].push_back(index);
        }
    }

    std::map<std::string, std::string> alias;
    std::set<std::size_t> fused;
    auto lookup = [&](const std::string& name, int number, std::string_view op) -> const Source& {
        const std::string resolved = resolve(name, alias);
        const auto found = sources.find(resolved);
        if (found == sources.end())
            refuse_node(number, op, "reads " + name + ", which no earlier node writes");
        return found->second;
    };

    for (std::size_t index = 0; index < model.nodes.size(); ++index) {
        const OnnxNode& node = model.nodes[index];
        const int number = static_cast<int>(index + 1);
        const std::string& op = node.op_type;
        if (!node.domain.empty() && node.domain != "ai.onnx")
            refuse_node(number, op, "domain " + node.domain + " is not the default domain");
        if (fused.count(index) != 0)
            continue;
        if (op == "Identity" || op == "Dropout") {
            const std::string taken = input_at(node, 0);
            if (taken.empty())
                refuse_node(number, op, "reads nothing");
            lookup(taken, number, op);
            const std::string written = one_output(node, number);
            alias.emplace(written, resolve(taken, alias));
            continue;
        }

        Plan layer;
        layer.number = number;
        layer.op = op;
        layer.output = one_output(node, number);
        layer.public_name = layer.output;

        auto made = [&](const std::string& name) -> const Source& {
            const Source& source = lookup(name, number, op);
            if (source.kind == Source::Kind::constant)
                refuse_node(number, op, "reads " + name + ", and that tensor is a constant");
            return source;
        };
        auto constant = [&](const std::string& name) -> const OnnxTensor& {
            const Source& source = lookup(name, number, op);
            if (source.kind != Source::Kind::constant)
                refuse_node(number, op, "reads " + name + ", and a weight is a constant");
            return model.initializers[source.slot];
        };

        if (op == "Gemm" || op == "MatMul") {
            const int64_t trans_a = op == "Gemm" ? attr_int(node, "transA", 0, number) : 0;
            const int64_t trans_b = op == "Gemm" ? attr_int(node, "transB", 0, number) : 0;
            const double alpha = op == "Gemm" ? attr_float(node, "alpha", 1.0, number) : 1.0;
            const double beta = op == "Gemm" ? attr_float(node, "beta", 1.0, number) : 1.0;
            if (trans_a != 0)
                refuse_node(number, op, "transA is not 0");
            if (trans_b != 0 && trans_b != 1)
                refuse_node(number, op, "transB is not 0 or 1");
            if (alpha != 1.0)
                refuse_node(number, op, "alpha is not 1");
            if (beta != 1.0)
                refuse_node(number, op, "beta is not 1");
            const std::string taken_name = input_at(node, 0);
            const std::string weight_name = input_at(node, 1);
            if (taken_name.empty() || weight_name.empty())
                refuse_node(number, op, "needs an input and a weight");
            const Source& taken = made(taken_name);
            if (taken.index || taken.batch_only)
                refuse_node(number, op, "the input is not a float vector");
            if (taken.dims.size() != 1)
                refuse_node(number, op, "the input rank is not 1");
            const OnnxTensor& weight = constant(weight_name);
            if (weight.data_type != 1 || weight.dims.size() != 2)
                refuse_node(number, op, "the weight is not a float matrix");
            const uint32_t columns = taken.dims[0];
            const bool transposed = trans_b == 0;
            const int64_t rows_dim = transposed ? weight.dims[1] : weight.dims[0];
            const int64_t cols_dim = transposed ? weight.dims[0] : weight.dims[1];
            if (cols_dim != static_cast<int64_t>(columns))
                refuse_node(number, op, "the weight's columns do not match the input");
            const uint32_t rows = held_dim(rows_dim, layer.output);
            std::string bias_name = op == "Gemm" ? input_at(node, 2) : std::string{};
            if (op == "MatMul" && graph_outputs.count(layer.output) == 0) {
                const std::vector<std::size_t>& users = consumers[layer.output];
                if (users.size() == 1 && users[0] > index &&
                    model.nodes[users[0]].op_type == "Add") {
                    const OnnxNode& add = model.nodes[users[0]];
                    const std::string first = input_at(add, 0);
                    const std::string second = input_at(add, 1);
                    const std::string other = first == layer.output ? second : first;
                    const bool paired = first == layer.output || second == layer.output;
                    if (paired && !other.empty() &&
                        lookup(other, number, op).kind == Source::Kind::constant) {
                        bias_name = other;
                        fused.insert(users[0]);
                        layer.output = one_output(add, static_cast<int>(users[0] + 1));
                        layer.public_name = layer.output;
                    }
                }
            }
            layer.kind = oann::LayerKind::dense;
            layer.inputs = {resolve(taken_name, alias)};
            layer.weight = weight_name;
            layer.bias = bias_name;
            if (!layer.bias.empty()) {
                const Source& bias_source = lookup(layer.bias, number, op);
                if (bias_source.kind != Source::Kind::constant)
                    refuse_node(number, op, "the bias is not a constant");
            }
            layer.transpose_weight = transposed;
            layer.dims = {rows};
        } else if (op == "Conv") {
            const std::string taken_name = input_at(node, 0);
            const std::string weight_name = input_at(node, 1);
            if (taken_name.empty() || weight_name.empty())
                refuse_node(number, op, "needs an input and a weight");
            const Source& taken = made(taken_name);
            if (taken.dims.size() != 3)
                refuse_node(number, op, "the input rank is not 3");
            const OnnxTensor& weight = constant(weight_name);
            if (weight.data_type != 1 || weight.dims.size() != 4)
                refuse_node(number, op, "the weight is not a float tensor of rank 4");
            if (attr_int(node, "group", 1, number) != 1)
                refuse_node(number, op, "group is not 1");
            require_ones(attr_ints(node, "dilations", number), number, op, "dilations");
            const std::string auto_pad = attr_string(node, "auto_pad", "NOTSET", number);
            if (auto_pad != "NOTSET" && auto_pad != "VALID")
                refuse_node(number, op, "auto_pad is not NOTSET or VALID");
            uint8_t pad_h = 0;
            uint8_t pad_w = 0;
            symmetric_pads(attr_ints(node, "pads", number), number, op, pad_h, pad_w);
            if (auto_pad == "VALID" && (pad_h != 0 || pad_w != 0))
                refuse_node(number, op, "VALID padding is not zero");
            const std::vector<int64_t> kernels = attr_ints(node, "kernel_shape", number);
            int64_t kernel_h = weight.dims[2];
            int64_t kernel_w = weight.dims[3];
            if (!kernels.empty()) {
                if (kernels.size() != 2)
                    refuse_node(number, op, "the kernel is not 2 axes");
                kernel_h = kernels[0];
                kernel_w = kernels[1];
            }
            const std::vector<int64_t> strides = attr_ints(node, "strides", number);
            const int64_t stride_h = strides.empty() ? 1 : strides[0];
            const int64_t stride_w = strides.size() < 2 ? stride_h : strides[1];
            if (strides.size() > 2)
                refuse_node(number, op, "strides are not 2 axes");
            if (weight.dims[1] != static_cast<int64_t>(taken.dims[0]) ||
                weight.dims[2] != kernel_h || weight.dims[3] != kernel_w) {
                refuse_node(number, op, "the weight shape does not match the input");
            }
            layer.kind = oann::LayerKind::conv2d;
            layer.inputs = {resolve(taken_name, alias)};
            layer.input_dims = taken.dims;
            layer.weight = weight_name;
            layer.bias = input_at(node, 2);
            if (!layer.bias.empty()) {
                const Source& bias_source = lookup(layer.bias, number, op);
                if (bias_source.kind != Source::Kind::constant)
                    refuse_node(number, op, "the bias is not a constant");
            }
            layer.kernel_h = spatial_axis(kernel_h, number, op, "kernel height");
            layer.kernel_w = spatial_axis(kernel_w, number, op, "kernel width");
            layer.stride_h = spatial_axis(stride_h, number, op, "stride height");
            layer.stride_w = spatial_axis(stride_w, number, op, "stride width");
            if (pad_h > layer.kernel_h - 1 || pad_w > layer.kernel_w - 1)
                refuse_node(number, op, "a pad is not smaller than the kernel");
            layer.pad_h = pad_h;
            layer.pad_w = pad_w;
            const uint32_t height =
                spatial_out(taken.dims[1], pad_h, layer.kernel_h, layer.stride_h, number, op);
            const uint32_t width =
                spatial_out(taken.dims[2], pad_w, layer.kernel_w, layer.stride_w, number, op);
            layer.dims = {held_dim(weight.dims[0], layer.output), height, width};
        } else if (op == "MaxPool") {
            const std::string taken_name = input_at(node, 0);
            const Source& taken = made(taken_name);
            if (taken.dims.size() != 3)
                refuse_node(number, op, "the input rank is not 3");
            require_ones(attr_ints(node, "dilations", number), number, op, "dilations");
            const std::vector<int64_t> pads = attr_ints(node, "pads", number);
            for (const int64_t pad : pads) {
                if (pad != 0)
                    refuse_node(number, op, "pads are not 0");
            }
            if (attr_int(node, "ceil_mode", 0, number) != 0)
                refuse_node(number, op, "ceil_mode is not 0");
            if (attr_int(node, "storage_order", 0, number) != 0)
                refuse_node(number, op, "storage_order is not 0");
            const std::vector<int64_t> kernels = attr_ints(node, "kernel_shape", number);
            if (kernels.size() != 2)
                refuse_node(number, op, "the kernel is not 2 axes");
            const std::vector<int64_t> strides = attr_ints(node, "strides", number);
            const int64_t stride_h = strides.empty() ? 1 : strides[0];
            const int64_t stride_w = strides.size() < 2 ? stride_h : strides[1];
            layer.kind = oann::LayerKind::maxpool;
            layer.inputs = {resolve(taken_name, alias)};
            layer.input_dims = taken.dims;
            layer.kernel_h = spatial_axis(kernels[0], number, op, "kernel height");
            layer.kernel_w = spatial_axis(kernels[1], number, op, "kernel width");
            layer.stride_h = spatial_axis(stride_h, number, op, "stride height");
            layer.stride_w = spatial_axis(stride_w, number, op, "stride width");
            const uint32_t height =
                spatial_out(taken.dims[1], 0, layer.kernel_h, layer.stride_h, number, op);
            const uint32_t width =
                spatial_out(taken.dims[2], 0, layer.kernel_w, layer.stride_w, number, op);
            layer.dims = {taken.dims[0], height, width};
        } else if (op == "Relu" || op == "Tanh" || op == "Sigmoid") {
            const Source& taken = made(input_at(node, 0));
            layer.kind = op == "Relu"   ? oann::LayerKind::relu
                         : op == "Tanh" ? oann::LayerKind::tanh
                                        : oann::LayerKind::sigmoid;
            layer.inputs = {resolve(input_at(node, 0), alias)};
            layer.dims = taken.dims;
        } else if (op == "LeakyRelu") {
            const Source& taken = made(input_at(node, 0));
            const double alpha = attr_float(node, "alpha", 0.01, number);
            const double scaled = std::round(alpha * 32768.0);
            if (!(alpha >= 0.0) || scaled > 32767.0)
                refuse_node(number, op, "alpha is outside 0 to 32767/32768");
            layer.kind = oann::LayerKind::leaky_relu;
            layer.inputs = {resolve(input_at(node, 0), alias)};
            layer.dims = taken.dims;
            layer.alpha = static_cast<int16_t>(scaled);
            layer.alpha_real = alpha;
        } else if (op == "Softmax") {
            const Source& taken = made(input_at(node, 0));
            const int onnx_rank = static_cast<int>(taken.dims.size()) + 1;
            int64_t axis = attr_int(node, "axis", graph.opset < 13 ? 1 : -1, number);
            if (axis < 0)
                axis += onnx_rank;
            if (axis != onnx_rank - 1)
                refuse_node(number, op, "the axis is not the last axis");
            for (std::size_t dim = 0; dim + 1 < taken.dims.size(); ++dim) {
                if (taken.dims[dim] != 1)
                    refuse_node(number, op, "a dimension other than the last is not 1");
            }
            layer.kind = oann::LayerKind::softmax;
            layer.inputs = {resolve(input_at(node, 0), alias)};
            layer.dims = taken.dims;
        } else if (op == "Add") {
            const std::string first_name = input_at(node, 0);
            const std::string second_name = input_at(node, 1);
            if (first_name.empty() || second_name.empty())
                refuse_node(number, op, "needs two inputs");
            const Source& first = lookup(first_name, number, op);
            const Source& second = lookup(second_name, number, op);
            if (first.kind == Source::Kind::constant || second.kind == Source::Kind::constant) {
                refuse_node(number, op, "an Add of a constant is only the bias of a MatMul");
            }
            if (first.dims != second.dims)
                refuse_node(number, op, "the inputs have different shapes");
            layer.kind = oann::LayerKind::add;
            layer.inputs = {resolve(first_name, alias), resolve(second_name, alias)};
            layer.dims = first.dims;
        } else if (op == "Concat") {
            std::vector<std::string> names;
            for (const std::string& name : node.inputs) {
                if (!name.empty())
                    names.push_back(name);
            }
            if (names.size() < 2 || names.size() > 4)
                refuse_node(number, op, "needs 2 to 4 inputs");
            const Source& first = made(names[0]);
            int64_t axis = attr_int(node, "axis", 1, number);
            const int onnx_rank = static_cast<int>(first.dims.size()) + 1;
            if (axis < 0)
                axis += onnx_rank;
            if (axis < 1 || axis >= onnx_rank)
                refuse_node(number, op, "the axis is not after the batch");
            const auto feature_axis = static_cast<uint8_t>(axis - 1);
            uint64_t joined = 0;
            for (const std::string& name : names) {
                const Source& part = made(name);
                if (part.dims.size() != first.dims.size())
                    refuse_node(number, op, "the inputs have different ranks");
                for (std::size_t dim = 0; dim < part.dims.size(); ++dim) {
                    if (dim != feature_axis && part.dims[dim] != first.dims[dim])
                        refuse_node(number, op, "the inputs differ off the axis");
                }
                joined += part.dims[feature_axis];
            }
            if (joined > oann::max_dimension)
                refuse_node(number, op, "the joined axis is above 65535");
            layer.kind = oann::LayerKind::concat;
            for (const std::string& name : names)
                layer.inputs.push_back(resolve(name, alias));
            layer.dims = first.dims;
            layer.dims[feature_axis] = static_cast<uint32_t>(joined);
            layer.axis = feature_axis;
        } else if (op == "Flatten" || op == "Reshape") {
            const Source& taken = made(input_at(node, 0));
            const uint64_t count = element_product(taken.dims, layer.output);
            if (op == "Flatten") {
                const int onnx_rank = static_cast<int>(taken.dims.size()) + 1;
                int64_t axis = attr_int(node, "axis", 1, number);
                if (axis < 0)
                    axis += onnx_rank;
                if (axis < 0 || axis > onnx_rank)
                    refuse_node(number, op, "the axis is outside the rank");
                int64_t prefix = 1;
                for (int dim = 0; dim < axis; ++dim)
                    prefix *=
                        dim == 0
                            ? 1
                            : static_cast<int64_t>(taken.dims[static_cast<std::size_t>(dim - 1)]);
                if (prefix != 1)
                    refuse_node(number, op, "the flattened tensor is not one feature dimension");
            } else {
                const std::string shape_name = input_at(node, 1);
                if (shape_name.empty())
                    refuse_node(number, op, "needs a shape");
                const OnnxTensor& shape = constant(shape_name);
                if (shape.ints.empty())
                    refuse_node(number, op, "the shape is not an integer tensor");
                std::vector<int64_t> spec = shape.ints;
                const int64_t total = static_cast<int64_t>(count);
                int infer = -1;
                int64_t known = 1;
                std::vector<int64_t> onnx_dims;
                onnx_dims.push_back(1);
                onnx_dims.insert(onnx_dims.end(), taken.dims.begin(), taken.dims.end());
                for (std::size_t dim = 0; dim < spec.size(); ++dim) {
                    if (spec[dim] == 0) {
                        if (dim >= onnx_dims.size())
                            refuse_node(number, op, "a shape 0 has no dimension to copy");
                        spec[dim] = onnx_dims[dim];
                    }
                    if (spec[dim] == -1) {
                        if (infer >= 0)
                            refuse_node(number, op, "more than one dimension is inferred");
                        infer = static_cast<int>(dim);
                    } else if (spec[dim] <= 0) {
                        refuse_node(number, op, "a shape dimension is not positive");
                    } else {
                        known *= spec[dim];
                    }
                }
                if (infer >= 0) {
                    if (known == 0 || total % known != 0)
                        refuse_node(number, op, "the inferred dimension does not divide the input");
                    spec[static_cast<std::size_t>(infer)] = total / known;
                    known = total;
                }
                if (known != total)
                    refuse_node(number, op, "the shape has a different element count");
                const bool one_feature = (spec.size() == 1) || (spec.size() == 2 && spec[0] == 1);
                if (!one_feature)
                    refuse_node(number, op, "the reshaped tensor is not one feature dimension");
            }
            if (count > oann::max_dimension)
                refuse_node(number, op, "the flattened length is above 65535");
            layer.kind = oann::LayerKind::flatten;
            layer.inputs = {resolve(input_at(node, 0), alias)};
            layer.dims = {static_cast<uint32_t>(count)};
        } else if (op == "ArgMax") {
            const Source& taken = made(input_at(node, 0));
            if (attr_int(node, "select_last_index", 0, number) != 0)
                refuse_node(number, op, "select_last_index is not 0");
            const int64_t keep = attr_int(node, "keepdims", 1, number);
            if (keep != 0 && keep != 1)
                refuse_node(number, op, "keepdims is not 0 or 1");
            const int onnx_rank = static_cast<int>(taken.dims.size()) + 1;
            int64_t axis = attr_int(node, "axis", 0, number);
            if (axis < 0)
                axis += onnx_rank;
            int non_unit = -1;
            for (std::size_t dim = 0; dim < taken.dims.size(); ++dim) {
                if (taken.dims[dim] == 1)
                    continue;
                if (non_unit >= 0)
                    refuse_node(number, op, "more than one axis is longer than 1");
                non_unit = static_cast<int>(dim);
            }
            if (non_unit >= 0 && axis != non_unit + 1)
                refuse_node(number, op, "the axis is not the only axis longer than 1");
            if (non_unit < 0 && (axis < 0 || axis >= onnx_rank))
                refuse_node(number, op, "the axis is outside the rank");
            layer.kind = oann::LayerKind::argmax;
            layer.argmax = true;
            layer.inputs = {resolve(input_at(node, 0), alias)};
            layer.dims = {1};
        } else if (op == "Gather") {
            const std::string table_name = input_at(node, 0);
            const std::string index_name = input_at(node, 1);
            if (table_name.empty() || index_name.empty())
                refuse_node(number, op, "needs a table and indices");
            if (attr_int(node, "axis", 0, number) != 0)
                refuse_node(number, op, "the axis is not 0");
            const OnnxTensor& table = constant(table_name);
            if (table.data_type != 1 || table.dims.size() != 2)
                refuse_node(number, op, "the table is not a float matrix");
            const Source& indices = made(index_name);
            if (indices.batch_only || indices.dims.size() != 1)
                refuse_node(number, op, "the indices are not a vector");
            if (indices.kind == Source::Kind::input)
                graph.inputs[indices.slot].index = true;
            layer.kind = oann::LayerKind::embedding;
            layer.inputs = {resolve(index_name, alias)};
            layer.weight = table_name;
            layer.dims = {indices.dims[0], held_dim(table.dims[1], layer.output)};
        } else {
            refuse_node(number, op, op + " is not one of the layers a .oann holds");
        }

        if (sources.count(layer.output) != 0)
            refuse_node(number, op, layer.output + " is written twice");
        Source made_source;
        made_source.kind = Source::Kind::made;
        made_source.dims = layer.dims;
        made_source.slot = graph.layers.size();
        sources.emplace(layer.output, made_source);
        graph.layers.push_back(std::move(layer));
    }

    if (graph.layers.empty())
        throw Failure("the model has no layer");
    for (const OnnxValue& value : model.outputs) {
        const std::string resolved = resolve(value.name, alias);
        const auto found = sources.find(resolved);
        if (found == sources.end())
            throw Failure("the output " + value.name + " is not written");
        if (found->second.kind == Source::Kind::input) {
            throw Failure(
                "the output " + value.name + " is an input, and a .oann has no copy layer"
            );
        }
        if (found->second.kind != Source::Kind::made)
            throw Failure("the output " + value.name + " is a constant");
        Plan& layer = graph.layers[found->second.slot];
        if (layer.graph_output && layer.public_name != value.name) {
            throw Failure(
                "the output " + value.name + " is the same tensor as " + layer.public_name
            );
        }
        layer.graph_output = true;
        layer.public_name = value.name;
        bool shaped = false;
        const std::vector<uint32_t> declared = declared_dims(value, layer.argmax, shaped);
        if (shaped) {
            const bool argmax_ok = layer.argmax && (declared.empty() || declared == layer.dims);
            if (!argmax_ok && declared != layer.dims) {
                throw Failure(
                    "output " + value.name + " is " + shape_text(declared) +
                    ", and the layer writes " + shape_text(layer.dims)
                );
            }
        }
    }
    for (const Plan& layer : graph.layers) {
        if (layer.argmax && !layer.graph_output)
            refuse_node(layer.number, layer.op, "an argmax is a network output");
        if (!layer.graph_output) {
            bool read = false;
            for (const Plan& later : graph.layers) {
                for (const std::string& name : later.inputs) {
                    if (name == layer.output)
                        read = true;
                }
            }
            if (!read)
                refuse_node(layer.number, layer.op, layer.output + " is read by no layer");
        }
    }
    return graph;
}

/// A bias broadcast to one value per output channel.
///
/// @param tensor the constant
/// @param channels how many values the layer needs
/// @param number the node number
/// @param op the operator
/// @return the bias values
std::vector<double>
bias_values(const OnnxTensor& tensor, uint32_t channels, int number, std::string_view op) {
    if (tensor.data_type != 1)
        refuse_node(number, op, "the bias is not a float tensor");
    std::vector<double> values;
    values.reserve(tensor.data.size());
    for (const float value : tensor.data) {
        if (!std::isfinite(value))
            refuse_node(number, op, "a bias is not a finite number");
        values.push_back(static_cast<double>(value));
    }
    if (values.size() == channels)
        return values;
    if (values.size() == 1)
        return std::vector<double>(channels, values[0]);
    refuse_node(number, op, "the bias length does not match the output");
}

/// One weight, read in ONNX order.
///
/// @param tensor the constant
/// @param number the node number
/// @param op the operator
/// @return the weights, widened
std::vector<double> weight_values(const OnnxTensor& tensor, int number, std::string_view op) {
    if (tensor.data_type != 1)
        refuse_node(number, op, "a weight is not a float tensor");
    std::vector<double> values;
    values.reserve(tensor.data.size());
    for (const float value : tensor.data) {
        if (!std::isfinite(value))
            refuse_node(number, op, "a weight is not a finite number");
        values.push_back(static_cast<double>(value));
    }
    return values;
}

/// Notes the largest absolute value.
///
/// @param[in,out] magnitude the largest so far
/// @param value one element
void note_magnitude(double& magnitude, double value) {
    const double absolute = std::fabs(value);
    if (absolute > magnitude)
        magnitude = absolute;
}

/// Runs one layer in double. Geometry matches the integer kernels: a conv
/// reads 0 outside its window, maxpool keeps the largest sample, and softmax
/// covers every element because every other dimension is 1.
///
/// @param layer the layer
/// @param env tensors written so far, and the sample's inputs
/// @param graph the constants
/// @return the layer's output
std::vector<double> eval_layer(
    const Plan& layer, const std::map<std::string, std::vector<double>>& env, const Graph& graph
) {
    auto take = [&](const std::string& name) -> const std::vector<double>& { return env.at(name); };
    if (layer.kind == oann::LayerKind::dense) {
        const std::vector<double>& input = take(layer.inputs[0]);
        const std::vector<double> weight =
            weight_values(*graph.constants.at(layer.weight), layer.number, layer.op);
        const uint32_t rows = layer.dims[0];
        const uint32_t columns = static_cast<uint32_t>(input.size());
        std::vector<double> bias(rows, 0.0);
        if (!layer.bias.empty())
            bias = bias_values(*graph.constants.at(layer.bias), rows, layer.number, layer.op);
        std::vector<double> output(rows, 0.0);
        for (uint32_t row = 0; row < rows; ++row) {
            double sum = bias[row];
            for (uint32_t column = 0; column < columns; ++column) {
                const std::size_t at = layer.transpose_weight
                                           ? static_cast<std::size_t>(column) * rows + row
                                           : static_cast<std::size_t>(row) * columns + column;
                sum += input[column] * weight[at];
            }
            output[row] = sum;
        }
        return output;
    }
    if (layer.kind == oann::LayerKind::conv2d) {
        const std::vector<double>& input = take(layer.inputs[0]);
        const std::vector<double> weight =
            weight_values(*graph.constants.at(layer.weight), layer.number, layer.op);
        if (layer.input_dims.size() != 3)
            refuse_node(layer.number, layer.op, "the input rank is not 3");
        const uint32_t in_c = layer.input_dims[0];
        const uint32_t in_h = layer.input_dims[1];
        const uint32_t in_w = layer.input_dims[2];
        const uint32_t out_c = layer.dims[0];
        const uint32_t out_h = layer.dims[1];
        const uint32_t out_w = layer.dims[2];
        const uint32_t kernel_h = layer.kernel_h;
        const uint32_t kernel_w = layer.kernel_w;
        const uint64_t expect = uint64_t{in_c} * in_h * in_w;
        if (input.size() != expect)
            refuse_node(layer.number, layer.op, "the input length does not match its shape");
        std::vector<double> bias(out_c, 0.0);
        if (!layer.bias.empty())
            bias = bias_values(*graph.constants.at(layer.bias), out_c, layer.number, layer.op);
        std::vector<double> output(static_cast<std::size_t>(out_c) * out_h * out_w, 0.0);
        for (uint32_t kernel = 0; kernel < out_c; ++kernel) {
            for (uint32_t out_y = 0; out_y < out_h; ++out_y) {
                for (uint32_t out_x = 0; out_x < out_w; ++out_x) {
                    double sum = bias[kernel];
                    const int32_t origin_y =
                        static_cast<int32_t>(out_y) * layer.stride_h - layer.pad_h;
                    const int32_t origin_x =
                        static_cast<int32_t>(out_x) * layer.stride_w - layer.pad_w;
                    for (uint32_t channel = 0; channel < in_c; ++channel) {
                        for (uint32_t kernel_y = 0; kernel_y < kernel_h; ++kernel_y) {
                            const int32_t in_y = origin_y + static_cast<int32_t>(kernel_y);
                            for (uint32_t kernel_x = 0; kernel_x < kernel_w; ++kernel_x) {
                                const int32_t in_x = origin_x + static_cast<int32_t>(kernel_x);
                                if (in_y < 0 || in_x < 0 || static_cast<uint32_t>(in_y) >= in_h ||
                                    static_cast<uint32_t>(in_x) >= in_w)
                                    continue;
                                const std::size_t sample_at =
                                    (static_cast<std::size_t>(channel) * in_h +
                                     static_cast<std::size_t>(in_y)) *
                                        in_w +
                                    static_cast<std::size_t>(in_x);
                                const std::size_t weight_at =
                                    ((static_cast<std::size_t>(kernel) * in_c + channel) *
                                         kernel_h +
                                     kernel_y) *
                                        kernel_w +
                                    kernel_x;
                                sum += input[sample_at] * weight[weight_at];
                            }
                        }
                    }
                    output[(static_cast<std::size_t>(kernel) * out_h + out_y) * out_w + out_x] =
                        sum;
                }
            }
        }
        return output;
    }
    if (layer.kind == oann::LayerKind::maxpool) {
        const std::vector<double>& input = take(layer.inputs[0]);
        if (layer.input_dims.size() != 3)
            refuse_node(layer.number, layer.op, "the input rank is not 3");
        const uint32_t in_c = layer.input_dims[0];
        const uint32_t in_h = layer.input_dims[1];
        const uint32_t in_w = layer.input_dims[2];
        const uint32_t out_c = layer.dims[0];
        const uint32_t out_h = layer.dims[1];
        const uint32_t out_w = layer.dims[2];
        const uint64_t expect = uint64_t{in_c} * in_h * in_w;
        if (input.size() != expect)
            refuse_node(layer.number, layer.op, "the input length does not match its shape");
        std::vector<double> output(static_cast<std::size_t>(out_c) * out_h * out_w);
        for (uint32_t channel = 0; channel < out_c; ++channel) {
            for (uint32_t out_y = 0; out_y < out_h; ++out_y) {
                for (uint32_t out_x = 0; out_x < out_w; ++out_x) {
                    const uint32_t origin_y = out_y * layer.stride_h;
                    const uint32_t origin_x = out_x * layer.stride_w;
                    double best = input
                        [(static_cast<std::size_t>(channel) * in_h + origin_y) * in_w + origin_x];
                    for (uint32_t kernel_y = 0; kernel_y < layer.kernel_h; ++kernel_y) {
                        for (uint32_t kernel_x = 0; kernel_x < layer.kernel_w; ++kernel_x) {
                            const uint32_t in_y = origin_y + kernel_y;
                            const uint32_t in_x = origin_x + kernel_x;
                            const double sample = input
                                [(static_cast<std::size_t>(channel) * in_h + in_y) * in_w + in_x];
                            if (sample > best)
                                best = sample;
                        }
                    }
                    output[(static_cast<std::size_t>(channel) * out_h + out_y) * out_w + out_x] =
                        best;
                }
            }
        }
        return output;
    }
    if (layer.kind == oann::LayerKind::relu || layer.kind == oann::LayerKind::leaky_relu ||
        layer.kind == oann::LayerKind::tanh || layer.kind == oann::LayerKind::sigmoid ||
        layer.kind == oann::LayerKind::softmax || layer.kind == oann::LayerKind::flatten ||
        layer.kind == oann::LayerKind::argmax) {
        const std::vector<double>& input = take(layer.inputs[0]);
        if (layer.kind == oann::LayerKind::flatten)
            return input;
        if (layer.kind == oann::LayerKind::argmax) {
            std::size_t best = 0;
            for (std::size_t index = 1; index < input.size(); ++index) {
                if (input[index] > input[best])
                    best = index;
            }
            return {static_cast<double>(best)};
        }
        std::vector<double> output(input.size());
        if (layer.kind == oann::LayerKind::relu) {
            for (std::size_t index = 0; index < input.size(); ++index)
                output[index] = input[index] > 0.0 ? input[index] : 0.0;
        } else if (layer.kind == oann::LayerKind::leaky_relu) {
            for (std::size_t index = 0; index < input.size(); ++index) {
                output[index] =
                    input[index] >= 0.0 ? input[index] : input[index] * layer.alpha_real;
            }
        } else if (layer.kind == oann::LayerKind::tanh) {
            for (std::size_t index = 0; index < input.size(); ++index)
                output[index] = std::tanh(input[index]);
        } else if (layer.kind == oann::LayerKind::sigmoid) {
            for (std::size_t index = 0; index < input.size(); ++index) {
                const double value = input[index];
                if (value >= 0.0) {
                    const double exponent = std::exp(-value);
                    output[index] = 1.0 / (1.0 + exponent);
                } else {
                    const double exponent = std::exp(value);
                    output[index] = exponent / (1.0 + exponent);
                }
            }
        } else {
            double peak = input.empty() ? 0.0 : input[0];
            for (const double value : input)
                if (value > peak)
                    peak = value;
            double total = 0.0;
            for (std::size_t index = 0; index < input.size(); ++index) {
                output[index] = std::exp(input[index] - peak);
                total += output[index];
            }
            if (total == 0.0)
                refuse_node(layer.number, layer.op, "a result is not a finite number");
            for (double& value : output)
                value /= total;
        }
        return output;
    }
    if (layer.kind == oann::LayerKind::add) {
        const std::vector<double>& first = take(layer.inputs[0]);
        const std::vector<double>& second = take(layer.inputs[1]);
        std::vector<double> output(first.size());
        for (std::size_t index = 0; index < first.size(); ++index)
            output[index] = first[index] + second[index];
        return output;
    }
    if (layer.kind == oann::LayerKind::concat) {
        const uint64_t prefix_count = [&] {
            uint64_t prefix = 1;
            for (uint8_t dim = 0; dim < layer.axis; ++dim)
                prefix *= layer.dims[dim];
            return prefix;
        }();
        uint64_t suffix = 1;
        for (std::size_t dim = static_cast<std::size_t>(layer.axis) + 1; dim < layer.dims.size();
             ++dim)
            suffix *= layer.dims[dim];
        const uint64_t out_along = layer.dims[layer.axis];
        std::vector<double> output(element_product(layer.dims, layer.output));
        uint64_t axis_base = 0;
        for (const std::string& name : layer.inputs) {
            const std::vector<double>& part = take(name);
            const uint64_t along = suffix == 0 ? 0 : (part.size() / suffix) / prefix_count;
            std::size_t cursor = 0;
            for (uint64_t outer = 0; outer < prefix_count; ++outer) {
                for (uint64_t step = 0; step < along; ++step) {
                    const uint64_t at = (outer * out_along + axis_base + step) * suffix;
                    for (uint64_t inner = 0; inner < suffix; ++inner)
                        output[static_cast<std::size_t>(at + inner)] = part[cursor++];
                }
            }
            axis_base += along;
        }
        return output;
    }
    // Embedding.
    const std::vector<double>& indices = take(layer.inputs[0]);
    const OnnxTensor& table = *graph.constants.at(layer.weight);
    const auto rows = static_cast<uint32_t>(table.dims[0]);
    const auto width = static_cast<uint32_t>(table.dims[1]);
    std::vector<double> output(indices.size() * width);
    for (std::size_t index = 0; index < indices.size(); ++index) {
        const double real = indices[index];
        const double rounded = std::round(real);
        if (std::fabs(real - rounded) > 1e-6)
            refuse_node(layer.number, layer.op, "an index is not a whole number");
        const auto row = static_cast<int64_t>(std::llround(real));
        if (row < 0 || static_cast<uint64_t>(row) >= rows) {
            throw Failure(
                "layer " + layer.public_name + ": row " + std::to_string(row) +
                " is outside the table of " + std::to_string(rows)
            );
        }
        for (uint32_t column = 0; column < width; ++column) {
            output[index * width + column] =
                static_cast<double>(table.data[static_cast<std::size_t>(row) * width + column]);
        }
    }
    return output;
}

/// Refuses a non-finite activation.
///
/// @param values the layer output
/// @param layer the layer
void require_finite(const std::vector<double>& values, const Plan& layer) {
    for (const double value : values) {
        if (!std::isfinite(value))
            refuse_node(layer.number, layer.op, "a result is not a finite number");
    }
}

/// Runs every sample and records each activation's largest magnitude.
///
/// @param graph the layers
/// @param samples the calibration samples
void measure(Graph& graph, const std::vector<Sample>& samples) {
    for (const Sample& sample : samples) {
        std::map<std::string, std::vector<double>> env;
        for (std::size_t index = 0; index < graph.inputs.size(); ++index) {
            InputRec& input = graph.inputs[index];
            std::vector<double> values;
            values.reserve(sample.columns[index].size());
            for (const int64_t raw : sample.columns[index]) {
                const double real = static_cast<double>(raw) / static_cast<double>(k_unit);
                values.push_back(real);
                if (!input.index)
                    note_magnitude(input.magnitude, real);
            }
            env.emplace(input.name, std::move(values));
        }
        for (Plan& layer : graph.layers) {
            std::vector<double> produced = eval_layer(layer, env, graph);
            require_finite(produced, layer);
            for (const double value : produced)
                note_magnitude(layer.magnitude, value);
            env.emplace(layer.output, std::move(produced));
        }
    }
}

/// Chooses every exponent. Fixed kinds take N01's exponent. A dense or conv2d
/// output is raised so its scale shift is 0 to 62.
///
/// @param[in,out] graph the layers and inputs
/// @param wide true when weights are int16
void assign_exponents(Graph& graph, bool wide) {
    std::map<std::string, int8_t> exponents;
    for (InputRec& input : graph.inputs) {
        input.exponent = input.index ? int8_t{0} : exponent_for(input.magnitude, 32767.0, -15);
        exponents.emplace(input.name, input.exponent);
    }
    const double weight_mantissa = wide ? 32767.0 : 127.0;
    std::map<std::string, int8_t> weight_exponents;
    for (Plan& layer : graph.layers) {
        auto input_exponent = [&](const std::string& name) -> int {
            const auto found = exponents.find(name);
            if (found == exponents.end())
                refuse_node(layer.number, layer.op, "reads " + name + ", which has no scale");
            return found->second;
        };
        if (!layer.weight.empty() && weight_exponents.count(layer.weight) == 0) {
            const std::vector<double> values =
                weight_values(*graph.constants.at(layer.weight), layer.number, layer.op);
            double magnitude = 0.0;
            for (const double value : values)
                note_magnitude(magnitude, value);
            weight_exponents.emplace(
                layer.weight, exponent_for(magnitude, weight_mantissa, oann::min_exponent)
            );
        }
        const int8_t produced = [&] {
            if (layer.kind == oann::LayerKind::relu || layer.kind == oann::LayerKind::leaky_relu ||
                layer.kind == oann::LayerKind::maxpool || layer.kind == oann::LayerKind::flatten) {
                return static_cast<int8_t>(input_exponent(layer.inputs[0]));
            }
            if (layer.kind == oann::LayerKind::tanh || layer.kind == oann::LayerKind::sigmoid ||
                layer.kind == oann::LayerKind::softmax) {
                return int8_t{-15};
            }
            if (layer.kind == oann::LayerKind::argmax)
                return int8_t{0};
            if (layer.kind == oann::LayerKind::embedding)
                return weight_exponents.at(layer.weight);
            const double magnitude = layer.magnitude;
            int exponent = exponent_for(magnitude, 32767.0, -15);
            if (layer.kind == oann::LayerKind::dense || layer.kind == oann::LayerKind::conv2d) {
                const int made =
                    input_exponent(layer.inputs[0]) + weight_exponents.at(layer.weight);
                if (!layer.bias.empty() &&
                    (made < oann::min_exponent || made > oann::max_exponent)) {
                    throw Failure(
                        "layer " + layer.public_name + ": the bias exponent is " +
                        std::to_string(made) + ", and must be -48 to 16"
                    );
                }
                int lower = made;
                int upper = made + 62;
                if (lower < oann::min_exponent)
                    lower = oann::min_exponent;
                if (upper > oann::max_exponent)
                    upper = oann::max_exponent;
                if (lower > upper) {
                    throw Failure(
                        "layer " + layer.public_name + ": the output scale shift cannot be 0 to 62"
                    );
                }
                if (exponent < lower)
                    exponent = lower;
                if (exponent > upper)
                    exponent = upper;
            }
            return static_cast<int8_t>(exponent);
        }();
        layer.exponent = produced;
        exponents.emplace(layer.output, produced);
    }
}

/// One quantised constant, interned when the same initializer is used twice
/// at the same scale.
struct StoredConstant {
    std::string key;
    uint16_t index{};
};

/// Builds the .oann model.
///
/// @param graph the planned layers, with exponents set
/// @param wide true when weights are int16
/// @return the model
oann::Model build_model(Graph& graph, bool wide) {
    oann::Model model;
    std::set<std::string> names;
    std::map<std::string, uint16_t> tensors;
    auto add_activation = [&](const std::string& onnx_name,
                              const std::string& public_name,
                              oann::Role role,
                              oann::ElementType type,
                              int8_t exponent,
                              const std::vector<uint32_t>& dims) {
        oann::Tensor tensor;
        tensor.name = unique_name(public_name, names);
        tensor.role = role;
        tensor.type = type;
        tensor.exponent = exponent;
        tensor.rank = static_cast<uint8_t>(dims.size());
        for (std::size_t index = 0; index < dims.size(); ++index)
            tensor.dims[index] = dims[index];
        const auto id = static_cast<uint16_t>(model.tensors.size());
        model.tensors.push_back(std::move(tensor));
        tensors.emplace(onnx_name, id);
        return id;
    };
    for (const InputRec& input : graph.inputs) {
        add_activation(
            input.name,
            input.name,
            oann::Role::input,
            oann::ElementType::int16,
            input.exponent,
            input.dims
        );
    }
    std::map<std::string, StoredConstant> constants;
    const oann::ElementType weight_type = wide ? oann::ElementType::int16 : oann::ElementType::int8;
    const int weight_width = wide ? 2 : 1;
    const int64_t weight_low = wide ? -32768 : -128;
    const int64_t weight_high = wide ? 32767 : 127;
    const double weight_mantissa = wide ? 32767.0 : 127.0;
    auto weight_exponent_of = [&](const std::string& name, const Plan& layer) {
        const std::vector<double> values =
            weight_values(*graph.constants.at(name), layer.number, layer.op);
        double magnitude = 0.0;
        for (const double value : values)
            note_magnitude(magnitude, value);
        return exponent_for(magnitude, weight_mantissa, oann::min_exponent);
    };
    for (Plan& layer : graph.layers) {
        const oann::Role role = layer.graph_output ? oann::Role::output : oann::Role::activation;
        const oann::ElementType type =
            layer.argmax ? oann::ElementType::int32 : oann::ElementType::int16;
        const std::string& shown = layer.graph_output ? layer.public_name : layer.output;
        const uint16_t id =
            add_activation(layer.output, shown, role, type, layer.exponent, layer.dims);
        layer.oann_name = model.tensors[id].name;
    }
    auto add_weight = [&](const Plan& layer) -> uint16_t {
        const int8_t exponent = weight_exponent_of(layer.weight, layer);
        const std::string key = layer.weight + "#w#" + std::to_string(int{exponent});
        const auto found = constants.find(key);
        if (found != constants.end())
            return found->second.index;
        const OnnxTensor& source = *graph.constants.at(layer.weight);
        const std::vector<double> values = weight_values(source, layer.number, layer.op);
        oann::Tensor tensor;
        tensor.name = unique_name(layer.weight, names);
        tensor.role = oann::Role::constant;
        tensor.type = weight_type;
        tensor.exponent = exponent;
        if (layer.kind == oann::LayerKind::dense) {
            const uint32_t rows = layer.dims[0];
            const auto input_id = tensors.at(layer.inputs[0]);
            const uint32_t width =
                static_cast<uint32_t>(oann::element_count(model.tensors[input_id]));
            tensor.rank = 2;
            tensor.dims[0] = rows;
            tensor.dims[1] = width;
            tensor.data.reserve(static_cast<std::size_t>(rows) * width);
            for (uint32_t row = 0; row < rows; ++row) {
                for (uint32_t column = 0; column < width; ++column) {
                    const std::size_t at = layer.transpose_weight
                                               ? static_cast<std::size_t>(column) * rows + row
                                               : static_cast<std::size_t>(row) * width + column;
                    const int64_t stored = quantise(values[at], exponent, weight_low, weight_high);
                    store_le(tensor.data, stored, weight_width);
                }
            }
        } else if (layer.kind == oann::LayerKind::conv2d) {
            tensor.rank = 4;
            tensor.dims[0] = static_cast<uint32_t>(source.dims[0]);
            tensor.dims[1] = static_cast<uint32_t>(source.dims[1]);
            tensor.dims[2] = static_cast<uint32_t>(source.dims[2]);
            tensor.dims[3] = static_cast<uint32_t>(source.dims[3]);
            for (const double value : values) {
                store_le(
                    tensor.data, quantise(value, exponent, weight_low, weight_high), weight_width
                );
            }
        } else {
            tensor.rank = 2;
            tensor.dims[0] = static_cast<uint32_t>(source.dims[0]);
            tensor.dims[1] = static_cast<uint32_t>(source.dims[1]);
            for (const double value : values) {
                store_le(
                    tensor.data, quantise(value, exponent, weight_low, weight_high), weight_width
                );
            }
        }
        const auto id = static_cast<uint16_t>(model.tensors.size());
        model.tensors.push_back(std::move(tensor));
        constants.emplace(key, StoredConstant{key, id});
        return id;
    };
    auto add_bias = [&](const Plan& layer, int8_t exponent) -> uint16_t {
        const std::string key = layer.bias + "#b#" + std::to_string(int{exponent});
        const auto found = constants.find(key);
        if (found != constants.end())
            return found->second.index;
        const uint32_t channels = layer.dims[0];
        const std::vector<double> values =
            bias_values(*graph.constants.at(layer.bias), channels, layer.number, layer.op);
        for (const double value : values) {
            if (!std::isfinite(value))
                refuse_node(layer.number, layer.op, "a bias is not a finite number");
        }
        oann::Tensor tensor;
        tensor.name = unique_name(layer.bias, names);
        tensor.role = oann::Role::constant;
        tensor.type = oann::ElementType::int32;
        tensor.exponent = exponent;
        tensor.rank = 1;
        tensor.dims[0] = channels;
        for (const double value : values) {
            const double rounded = std::round(value / std::ldexp(1.0, exponent));
            if (rounded < -2147483648.0 || rounded > 2147483647.0) {
                throw Failure("layer " + layer.public_name + ": the bias does not fit int32");
            }
            store_le(tensor.data, static_cast<int64_t>(rounded), 4);
        }
        const auto id = static_cast<uint16_t>(model.tensors.size());
        model.tensors.push_back(std::move(tensor));
        constants.emplace(key, StoredConstant{key, id});
        return id;
    };

    for (const Plan& layer : graph.layers) {
        oann::Layer entry{};
        entry.kind = layer.kind;
        entry.inputs = {oann::no_tensor, oann::no_tensor, oann::no_tensor, oann::no_tensor};
        entry.weights = oann::no_tensor;
        entry.bias = oann::no_tensor;
        entry.input_count = static_cast<uint8_t>(layer.inputs.size());
        for (std::size_t slot = 0; slot < layer.inputs.size(); ++slot)
            entry.inputs[slot] = tensors.at(layer.inputs[slot]);
        entry.output = tensors.at(layer.output);
        if (!layer.weight.empty())
            entry.weights = add_weight(layer);
        if (!layer.bias.empty()) {
            const int made = int{model.tensors[entry.inputs[0]].exponent} +
                             int{model.tensors[entry.weights].exponent};
            entry.bias = add_bias(layer, static_cast<int8_t>(made));
        }
        entry.kernel_h = layer.kernel_h;
        entry.kernel_w = layer.kernel_w;
        entry.stride_h = layer.stride_h;
        entry.stride_w = layer.stride_w;
        entry.pad_h = layer.pad_h;
        entry.pad_w = layer.pad_w;
        entry.axis = layer.axis;
        entry.alpha = layer.alpha;
        model.layers.push_back(entry);
    }
    return model;
}

/// One output's difference or agreement.
struct Score {
    std::string name;
    bool agreement{};
    double difference{};
    double largest{};
    uint64_t matched{};
};

/// Compares the integer network with a fresh floating-point run.
///
/// The floating-point tensors stay under their ONNX names. Each network port
/// is matched to the layer whose .oann name is that port.
///
/// @param network the integer network
/// @param model the file's model, used to name an embedding failure
/// @param graph the layers, for the floating-point run
/// @param samples the calibration samples
/// @return one score per output, in port order
std::vector<Score> compare(
    neural::Network& network,
    const oann::Model& model,
    const Graph& graph,
    const std::vector<Sample>& samples
) {
    std::vector<Score> scores;
    for (const neural::Port& port : network.outputs()) {
        Score score;
        score.name = port.name;
        score.agreement = port.index;
        scores.push_back(std::move(score));
    }
    std::map<std::string, const Plan*> by_name;
    for (const Plan& layer : graph.layers) {
        if (layer.graph_output)
            by_name.emplace(layer.oann_name, &layer);
    }
    for (const Sample& sample : samples) {
        std::map<std::string, std::vector<double>> env;
        for (std::size_t input_index = 0; input_index < graph.inputs.size(); ++input_index) {
            const neural::Port& port = network.inputs()[input_index];
            const std::span<int16_t> buffer = network.input(input_index);
            const std::vector<int64_t>& column = sample.columns[input_index];
            if (buffer.size() != column.size()) {
                throw Failure(
                    "input " + graph.inputs[input_index].name + ": the sample length does not match"
                );
            }
            std::vector<double> values;
            values.reserve(column.size());
            for (std::size_t element = 0; element < column.size(); ++element) {
                const int64_t raw = column[element];
                buffer[element] = neural::saturate_int16(neural::rescale(raw, -16, port.exponent));
                values.push_back(static_cast<double>(raw) / static_cast<double>(k_unit));
            }
            env.emplace(graph.inputs[input_index].name, std::move(values));
        }
        for (const Plan& layer : graph.layers) {
            std::vector<double> produced = eval_layer(layer, env, graph);
            require_finite(produced, layer);
            env.emplace(layer.output, std::move(produced));
        }
        const neural::RunError ran = network.run();
        if (ran.failed) {
            const oann::Layer& step = model.layers[ran.layer];
            const std::string& name = model.tensors[step.output].name;
            const uint32_t rows = model.tensors[step.weights].dims[0];
            throw Failure(
                "layer " + name + ": row " + std::to_string(ran.row) + " is outside the table of " +
                std::to_string(rows)
            );
        }
        for (std::size_t port_index = 0; port_index < network.outputs().size(); ++port_index) {
            const neural::Port& port = network.outputs()[port_index];
            const auto found = by_name.find(port.name);
            if (found == by_name.end())
                throw Failure("output " + port.name + " has no layer");
            const std::vector<double>& real = env.at(found->second->output);
            Score& score = scores[port_index];
            if (port.index) {
                const double chosen = real.empty() ? 0.0 : real[0];
                if (network.output_index(port_index) == static_cast<int32_t>(std::llround(chosen)))
                    ++score.matched;
                note_magnitude(score.largest, chosen);
                continue;
            }
            const std::span<const int16_t> quant = network.output(port_index);
            if (quant.size() != real.size())
                throw Failure("output " + port.name + ": the lengths differ");
            const double scale = std::ldexp(1.0, port.exponent);
            for (std::size_t element = 0; element < real.size(); ++element) {
                const double gap =
                    std::fabs(real[element] - static_cast<double>(quant[element]) * scale);
                if (gap > score.difference)
                    score.difference = gap;
                note_magnitude(score.largest, real[element]);
            }
        }
    }
    return scores;
}

/// The SHA-256 of a .oann, as 64 lower-case hex digits.
///
/// @param bytes the file
/// @return the hex text
std::string sha256_hex(std::span<const uint8_t> bytes) {
    const oa::base::sha256::Digest digest = oa::base::sha256::digest_of(bytes);
    const std::array<char, oa::base::sha256::hex_size> hex = oa::base::sha256::to_hex(digest);
    return std::string(hex.begin(), hex.end());
}

/// How many weight elements the file holds, biases excluded.
///
/// A bias is the int32 constant. Shared weights are one tensor, counted once.
///
/// @param model the network
/// @return the element count
uint64_t weight_elements(const oann::Model& model) {
    uint64_t count = 0;
    for (const oann::Tensor& tensor : model.tensors) {
        if (tensor.role == oann::Role::constant && tensor.type != oann::ElementType::int32)
            count += oann::element_count(tensor);
    }
    return count;
}

/// Formats a percentage with one digit after the point.
///
/// @param value the percentage
/// @return the text
std::string format_percent(double value) {
    char buffer[64];
    const int wrote = std::snprintf(buffer, sizeof(buffer), "%.1f", value);
    if (wrote <= 0)
        return "0.0";
    return std::string(buffer, static_cast<std::size_t>(wrote));
}

/// The feature shape of one port, as `[d, d]`.
///
/// @param port the port
/// @return the text
std::string port_shape(const neural::Port& port) {
    std::vector<uint32_t> dims(port.dims.begin(), port.dims.begin() + port.rank);
    return shape_text(dims);
}

/// Prints the report as lines, or as one JSON object.
///
/// @param output where the report is printed
/// @param as_json true for one JSON object
/// @param model_path the ONNX file
/// @param written_path the .oann path, whether or not it was written
/// @param file the .oann bytes
/// @param network the integer network
/// @param built the model those bytes came from
/// @param scores one entry per output
/// @param samples how many calibration samples were run
void print_report(
    Output& output,
    bool as_json,
    const fs::path& model_path,
    const fs::path& written_path,
    std::span<const uint8_t> file,
    const neural::Network& network,
    const oann::Model& built,
    const std::vector<Score>& scores,
    std::size_t samples
) {
    const std::string model_text = utf8_of(model_path);
    const std::string written_text = utf8_of(written_path);
    const std::string hash = sha256_hex(file);
    const uint64_t weights = weight_elements(built);
    const uint64_t bytes = network.memory_bytes();
    const uint64_t operations = network.operations();
    const uint64_t charge = network.charge_units();
    if (!as_json) {
        output.out << "model: " << model_text << '\n';
        output.out << "written: " << written_text << '\n';
        output.out << "sha256: " << hash << '\n';
        for (const neural::Port& port : network.inputs()) {
            output.out << "inputs: " << port.name << ' ' << port_shape(port) << " exponent "
                       << int{port.exponent} << '\n';
        }
        for (const neural::Port& port : network.outputs()) {
            output.out << "outputs: " << port.name << ' ' << port_shape(port) << " exponent "
                       << int{port.exponent} << '\n';
        }
        output.out << "layers: " << built.layers.size() << '\n';
        output.out << "weights: " << weights << '\n';
        output.out << "bytes: " << bytes << '\n';
        output.out << "operations: " << operations << '\n';
        output.out << "charge: " << charge << '\n';
        output.out << "samples: " << samples << '\n';
        for (const Score& score : scores) {
            if (score.agreement) {
                const double percent = samples == 0 ? 0.0
                                                    : 100.0 * static_cast<double>(score.matched) /
                                                          static_cast<double>(samples);
                output.out << "agreement: " << score.name << ' ' << format_percent(percent) << "% ("
                           << score.matched << " of " << samples << ")\n";
            } else {
                output.out << "difference: " << score.name << ' ' << format_real(score.difference)
                           << " (largest " << format_real(score.largest) << ")\n";
            }
        }
        return;
    }
    json::JsonWriter writer;
    writer.begin_object();
    writer.key("model");
    writer.string(model_text);
    writer.key("written");
    writer.string(written_text);
    writer.key("sha256");
    writer.string(hash);
    auto write_ports = [&](std::string_view key, std::span<const neural::Port> ports) {
        writer.key(key);
        writer.begin_array();
        for (const neural::Port& port : ports) {
            writer.begin_object();
            writer.key("name");
            writer.string(port.name);
            writer.key("shape");
            writer.begin_array();
            for (uint8_t axis = 0; axis < port.rank; ++axis)
                writer.integer(port.dims[axis]);
            writer.end_array();
            writer.key("exponent");
            writer.integer(port.exponent);
            writer.end_object();
        }
        writer.end_array();
    };
    write_ports("inputs", network.inputs());
    write_ports("outputs", network.outputs());
    writer.key("layers");
    writer.integer(static_cast<int64_t>(built.layers.size()));
    writer.key("weights");
    writer.integer(static_cast<int64_t>(weights));
    writer.key("bytes");
    writer.integer(static_cast<int64_t>(bytes));
    writer.key("operations");
    writer.integer(static_cast<int64_t>(operations));
    writer.key("charge");
    writer.integer(static_cast<int64_t>(charge));
    writer.key("samples");
    writer.integer(static_cast<int64_t>(samples));
    writer.key("results");
    writer.begin_array();
    for (const Score& score : scores) {
        writer.begin_object();
        writer.key("name");
        writer.string(score.name);
        if (score.agreement) {
            const double percent = samples == 0 ? 0.0
                                                : 100.0 * static_cast<double>(score.matched) /
                                                      static_cast<double>(samples);
            writer.key("agreement");
            writer.raw(format_percent(percent));
            writer.key("matched");
            writer.integer(static_cast<int64_t>(score.matched));
            writer.key("of");
            writer.integer(static_cast<int64_t>(samples));
        } else {
            writer.key("difference");
            writer.raw(format_real(score.difference));
            writer.key("largest");
            writer.raw(format_real(score.largest));
        }
        writer.end_object();
    }
    writer.end_array();
    writer.end_object();
    output.out << writer.text() << '\n';
}

/// True when a difference, not an agreement, is past the limit.
///
/// @param scores the outputs
/// @param limited true when `--max-difference` was set
/// @param limit the limit
/// @return true when the file must not be written
bool past_limit(const std::vector<Score>& scores, bool limited, double limit) {
    if (!limited)
        return false;
    for (const Score& score : scores) {
        if (!score.agreement && score.difference > limit)
            return true;
    }
    return false;
}

} // namespace

int run_brain_net(std::span<const std::string> arguments, Output& output) {
    auto usage = [&](const std::string& problem) {
        output.err << "oa-tool brain net: " << problem << '\n';
        output.err << "run 'oa-tool help brain net'\n";
        return exit_usage;
    };
    static constexpr OptionSpec options[] = {
        {"calibrate", true, false},
        {"out", true, false},
        {"weights", true, false},
        {"max-difference", true, false},
        {"force", false, false},
        {"json", false, false},
    };
    std::string problem;
    const std::optional<Arguments> parsed = parse_arguments(arguments, options, problem);
    if (!parsed)
        return usage(problem);
    if (parsed->positional.empty())
        return usage("name the ONNX model");
    if (parsed->positional.size() != 1)
        return usage("unexpected argument");
    const std::string* calibrate = parsed->value("calibrate");
    if (calibrate == nullptr)
        return usage("--calibrate is required");
    bool wide = false;
    if (const std::string* weights = parsed->value("weights")) {
        if (*weights == "int16")
            wide = true;
        else if (*weights != "int8")
            return usage("--weights is int8 or int16");
    }
    bool limited = false;
    double limit = 0.0;
    std::string limit_text;
    if (const std::string* text = parsed->value("max-difference")) {
        limit_text = *text;
        const char* first = text->data();
        const char* last = first + text->size();
        double value = 0.0;
        const std::from_chars_result converted = std::from_chars(first, last, value);
        if (converted.ec != std::errc{} || converted.ptr != last || !std::isfinite(value) ||
            value < 0.0) {
            return usage("--max-difference is a number that is not negative");
        }
        limited = true;
        limit = value;
    }
    const bool force = parsed->flags.count("force") != 0;
    const bool as_json = parsed->flags.count("json") != 0;
    const fs::path model_path{parsed->positional[0]};
    fs::path destination = model_path;
    destination.replace_extension(".oann");
    if (const std::string* chosen = parsed->value("out"))
        destination = fs::path{*chosen};

    const std::vector<uint8_t> onnx_bytes = read_file(model_path);
    std::string onnx_problem;
    const std::optional<OnnxModel> onnx = read_onnx(onnx_bytes, onnx_problem);
    if (!onnx)
        throw Failure(onnx_problem);
    Graph graph = plan_graph(*onnx);
    std::size_t skipped = 0;
    const std::vector<Sample> samples =
        read_calibration(fs::path{*calibrate}, graph.inputs, skipped, output);
    measure(graph, samples);
    assign_exponents(graph, wide);
    const oann::Model built = build_model(graph, wide);
    oann::Problem write_problem;
    const std::optional<std::vector<uint8_t>> file = oann::write(built, write_problem);
    if (!file)
        throw Failure(write_problem.message);
    oann::Problem build_problem;
    std::optional<neural::Network> ready = neural::Network::build(built, build_problem);
    if (!ready)
        throw Failure(build_problem.message);
    const std::vector<Score> scores = compare(*ready, built, graph, samples);
    if (past_limit(scores, limited, limit)) {
        print_report(
            output, as_json, model_path, destination, *file, *ready, built, scores, samples.size()
        );
        throw Failure("a difference passes --max-difference " + limit_text);
    }
    std::error_code exists_error;
    const bool already = fs::exists(destination, exists_error);
    if (exists_error)
        throw Failure("cannot check the output: " + utf8_of(destination));
    if (already && !force) {
        throw Failure(
            "the output already exists: " + utf8_of(destination) + "; pass --force to replace it"
        );
    }
    write_file(destination, *file);
    print_report(
        output, as_json, model_path, destination, *file, *ready, built, scores, samples.size()
    );
    return exit_done;
}

} // namespace oa::tool
