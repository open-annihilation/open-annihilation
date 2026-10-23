// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// oa-tool brain net, run in-process. The test writes every ONNX byte and
// every calibration line. A dense model is checked against hand-quantised
// weights, a grid and a Gather are read back, and a bad file is refused.

#include "command.hpp"
#include "onnx.hpp"

#include "oa/base/sha256.hpp"
#include "oa/formats/json.hpp"
#include "oa/formats/oann/oann.hpp"
#include "oa/test/check.hpp"
#include "oa/test/scratch_directory.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iterator>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace json = oa::formats::json;
namespace oann = oa::formats::oann;

/// One protocol-buffer message, built by the test.
struct Proto {
    std::vector<uint8_t> bytes;

    /// Appends an unsigned varint.
    ///
    /// @param value the bits
    void varint(uint64_t value) {
        while (value > 0x7f) {
            bytes.push_back(static_cast<uint8_t>((value & 0x7fU) | 0x80U));
            value >>= 7U;
        }
        bytes.push_back(static_cast<uint8_t>(value));
    }

    /// Appends a field key.
    ///
    /// @param field the field number
    /// @param wire the wire type
    void key(uint32_t field, uint32_t wire) { varint((uint64_t{field} << 3U) | wire); }

    /// Appends a varint field.
    ///
    /// @param field the field number
    /// @param value the bits
    void vint_field(uint32_t field, uint64_t value) {
        key(field, 0);
        varint(value);
    }

    /// Appends a length-delimited field.
    ///
    /// @param field the field number
    /// @param data the bytes
    void bytes_field(uint32_t field, const std::vector<uint8_t>& data) {
        key(field, 2);
        varint(data.size());
        bytes.insert(bytes.end(), data.begin(), data.end());
    }

    /// Appends a string field.
    ///
    /// @param field the field number
    /// @param text the text
    void str(uint32_t field, std::string_view text) {
        const auto* data = reinterpret_cast<const uint8_t*>(text.data());
        bytes_field(field, std::vector<uint8_t>(data, data + text.size()));
    }

    /// Appends a nested message.
    ///
    /// @param field the field number
    /// @param nested the message
    void msg(uint32_t field, const Proto& nested) { bytes_field(field, nested.bytes); }
};

/// Little-endian bytes of one float.
///
/// @param value the float
/// @return four bytes
std::vector<uint8_t> f32_bytes(float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return {
        static_cast<uint8_t>(bits),
        static_cast<uint8_t>(bits >> 8U),
        static_cast<uint8_t>(bits >> 16U),
        static_cast<uint8_t>(bits >> 24U)
    };
}

/// A float tensor whose dimensions are unpacked varints and whose elements are raw bytes.
///
/// @param name the tensor name
/// @param dims the shape
/// @param values the elements
/// @return the tensor message
Proto float_raw(
    std::string_view name, const std::vector<int64_t>& dims, const std::vector<float>& values
) {
    Proto tensor;
    for (const int64_t dim : dims)
        tensor.vint_field(1, static_cast<uint64_t>(dim));
    tensor.vint_field(2, 1);
    tensor.str(8, name);
    std::vector<uint8_t> raw;
    for (const float value : values) {
        const std::vector<uint8_t> piece = f32_bytes(value);
        raw.insert(raw.end(), piece.begin(), piece.end());
    }
    tensor.bytes_field(9, raw);
    return tensor;
}

/// A float tensor whose dimensions and elements are packed.
///
/// @param name the tensor name
/// @param dims the shape
/// @param values the elements
/// @return the tensor message
Proto float_packed(
    std::string_view name, const std::vector<int64_t>& dims, const std::vector<float>& values
) {
    Proto tensor;
    Proto packed_dims;
    for (const int64_t dim : dims)
        packed_dims.varint(static_cast<uint64_t>(dim));
    tensor.bytes_field(1, packed_dims.bytes);
    tensor.vint_field(2, 1);
    std::vector<uint8_t> packed;
    for (const float value : values) {
        const std::vector<uint8_t> piece = f32_bytes(value);
        packed.insert(packed.end(), piece.begin(), piece.end());
    }
    tensor.bytes_field(4, packed);
    tensor.str(8, name);
    return tensor;
}

/// An integer attribute.
///
/// @param name the attribute name
/// @param value the integer
/// @return the attribute message
Proto attr_int(std::string_view name, int64_t value) {
    Proto attribute;
    attribute.str(1, name);
    attribute.vint_field(3, static_cast<uint64_t>(value));
    return attribute;
}

/// A packed list of integers.
///
/// @param name the attribute name
/// @param values the integers
/// @return the attribute message
Proto attr_ints(std::string_view name, const std::vector<int64_t>& values) {
    Proto attribute;
    attribute.str(1, name);
    Proto packed;
    for (const int64_t value : values)
        packed.varint(static_cast<uint64_t>(value));
    attribute.bytes_field(8, packed.bytes);
    return attribute;
}

/// One node.
///
/// @param op the operator
/// @param inputs the input names, in order
/// @param output the output name
/// @param attributes the attributes
/// @return the node message
Proto node_of(
    std::string_view op,
    const std::vector<std::string>& inputs,
    std::string_view output,
    const std::vector<Proto>& attributes
) {
    Proto body;
    for (const std::string& input : inputs)
        body.str(1, input);
    body.str(2, output);
    body.str(4, op);
    for (const Proto& attribute : attributes)
        body.msg(5, attribute);
    return body;
}

/// A named dimension.
///
/// @param name the dimension's name
/// @return the dimension message
Proto dim_param(std::string_view name) {
    Proto dim;
    dim.str(2, name);
    return dim;
}

/// A numbered dimension.
///
/// @param value the length
/// @return the dimension message
Proto dim_value(int64_t value) {
    Proto dim;
    dim.vint_field(1, static_cast<uint64_t>(value));
    return dim;
}

/// A value-info for a tensor type.
///
/// @param name the tensor name
/// @param elem_type 1 float or 7 int64
/// @param dims the shape, batch included
/// @return the value-info message
Proto value_info(std::string_view name, int32_t elem_type, const std::vector<Proto>& dims) {
    Proto shape;
    for (const Proto& dim : dims)
        shape.msg(1, dim);
    Proto tensor_type;
    tensor_type.vint_field(1, static_cast<uint64_t>(elem_type));
    tensor_type.msg(2, shape);
    Proto type;
    type.msg(1, tensor_type);
    Proto value;
    value.str(1, name);
    value.msg(2, type);
    return value;
}

/// A model whose graph is one length-delimited field, plus an opset.
///
/// @param graph the graph message
/// @param version the default-domain opset
/// @return the model message
Proto model_of(const Proto& graph, int64_t version) {
    Proto opset;
    opset.vint_field(2, static_cast<uint64_t>(version));
    Proto model;
    model.msg(7, graph);
    model.msg(8, opset);
    return model;
}

/// The dense threat model: Gemm, Relu, Gemm, Softmax and ArgMax. Opset 13.
///
/// Weights are raw little-endian floats. The first matrix is the identity and
/// the second keeps six times the first feature. transB is 1, so each matrix
/// is already [outputs, inputs].
///
/// @return the model bytes
std::vector<uint8_t> threat_model() {
    Proto graph;
    graph.msg(11, value_info("features", 1, {dim_param("batch"), dim_value(2)}));
    graph.msg(5, float_raw("hidden_w", {2, 2}, {1.f, 0.f, 0.f, 1.f}));
    graph.msg(5, float_raw("hidden_b", {2}, {0.f, 0.f}));
    graph.msg(5, float_raw("score_w", {2, 2}, {6.f, 0.f, 0.f, 0.f}));
    graph.msg(5, float_raw("score_b", {2}, {0.f, 0.f}));
    graph.msg(
        1, node_of("Gemm", {"features", "hidden_w", "hidden_b"}, "hidden", {attr_int("transB", 1)})
    );
    graph.msg(1, node_of("Relu", {"hidden"}, "activated", {}));
    graph.msg(
        1, node_of("Gemm", {"activated", "score_w", "score_b"}, "scores", {attr_int("transB", 1)})
    );
    graph.msg(1, node_of("Softmax", {"scores"}, "chances", {attr_int("axis", 1)}));
    graph.msg(1, node_of("ArgMax", {"chances"}, "choice", {attr_int("axis", 1)}));
    graph.msg(12, value_info("chances", 1, {dim_param("batch"), dim_value(2)}));
    graph.msg(12, value_info("choice", 7, {dim_param("batch"), dim_value(1)}));
    return model_of(graph, 13).bytes;
}

/// A one-input Gemm whose weight is 0.1, so int8 and int16 land on different scales.
///
/// @return the model bytes
std::vector<uint8_t> level_model() {
    Proto graph;
    graph.msg(11, value_info("features", 1, {dim_param("batch"), dim_value(1)}));
    graph.msg(5, float_raw("scale_w", {1, 1}, {0.1f}));
    graph.msg(1, node_of("Gemm", {"features", "scale_w"}, "level", {attr_int("transB", 1)}));
    graph.msg(12, value_info("level", 1, {dim_param("batch"), dim_value(1)}));
    return model_of(graph, 13).bytes;
}

/// A grid: Conv, Relu, MaxPool, Conv, Tanh, Flatten, then MatMul fused with Add, then Sigmoid.
///
/// The weights use packed dimensions and packed floats.
///
/// @return the model bytes
std::vector<uint8_t> grid_model() {
    const std::vector<float> ones4(4, 1.f);
    const std::vector<float> ones8(8, 1.f);
    Proto graph;
    graph.msg(
        11, value_info("picture", 1, {dim_param("batch"), dim_value(1), dim_value(4), dim_value(4)})
    );
    graph.msg(5, float_packed("w1", {2, 1, 2, 2}, ones8));
    graph.msg(5, float_packed("w2", {1, 2, 1, 1}, {1.f, 1.f}));
    graph.msg(5, float_packed("w3", {1, 1}, {1.f}));
    graph.msg(5, float_packed("b3", {1}, {0.f}));
    graph.msg(
        1,
        node_of(
            "Conv",
            {"picture", "w1"},
            "c1",
            {attr_ints("kernel_shape", {2, 2}), attr_ints("strides", {1, 1})}
        )
    );
    graph.msg(1, node_of("Relu", {"c1"}, "r1", {}));
    graph.msg(
        1,
        node_of(
            "MaxPool",
            {"r1"},
            "p1",
            {attr_ints("kernel_shape", {2, 2}), attr_ints("strides", {2, 2})}
        )
    );
    graph.msg(1, node_of("Conv", {"p1", "w2"}, "c2", {attr_ints("kernel_shape", {1, 1})}));
    graph.msg(1, node_of("Tanh", {"c2"}, "t1", {}));
    graph.msg(1, node_of("Flatten", {"t1"}, "f1", {attr_int("axis", 1)}));
    graph.msg(1, node_of("MatMul", {"f1", "w3"}, "m1", {}));
    graph.msg(1, node_of("Add", {"m1", "b3"}, "a1", {}));
    graph.msg(1, node_of("Sigmoid", {"a1"}, "s1", {}));
    graph.msg(12, value_info("s1", 1, {dim_param("batch"), dim_value(1)}));
    return model_of(graph, 13).bytes;
}

/// Gather of a constant table, concatenated with a second input.
///
/// @return the model bytes
std::vector<uint8_t> gather_model() {
    Proto graph;
    graph.msg(11, value_info("kinds", 7, {dim_param("batch"), dim_value(1)}));
    graph.msg(11, value_info("extra", 1, {dim_param("batch"), dim_value(1), dim_value(2)}));
    graph.msg(5, float_packed("table", {4, 2}, {0.f, 0.25f, 0.5f, 0.75f, 1.f, 0.f, 0.f, 0.f}));
    graph.msg(1, node_of("Gather", {"table", "kinds"}, "rows", {attr_int("axis", 0)}));
    graph.msg(1, node_of("Concat", {"rows", "extra"}, "joined", {attr_int("axis", 2)}));
    graph.msg(12, value_info("joined", 1, {dim_param("batch"), dim_value(1), dim_value(4)}));
    return model_of(graph, 13).bytes;
}

/// 200 samples of features in [0.5, 1], each a whole number of 65536ths, with the second half the first.
///
/// @return the calibration file
std::string threat_calibration() {
    std::string text;
    for (int step = 0; step < 200; ++step) {
        const int64_t offset = (int64_t{4096} * step) / 199;
        const int64_t first = 32768 + offset * 8;
        const int64_t second = first / 8;
        text += "{\"kind\":\"sample\",\"features\":{\"features\":{\"shape\":[1,2],\"values\":[";
        text += std::to_string(first);
        text += ',';
        text += std::to_string(second);
        text += "]}}}\n";
    }
    return text;
}

/// One sample whose single feature is 1.
///
/// @return the calibration line
std::string level_calibration() {
    return "{\"kind\":\"sample\",\"features\":{\"features\":{\"shape\":[1,1],\"values\":[65536]}}}"
           "\n";
}

/// One picture of 16 values.
///
/// @return the calibration line
std::string grid_calibration() {
    std::string text =
        "{\"kind\":\"sample\",\"features\":{\"picture\":{\"shape\":[1,1,4,4],\"values\":[";
    for (int cell = 0; cell < 16; ++cell) {
        if (cell != 0)
            text += ',';
        text += std::to_string(int64_t{cell} * 4096);
    }
    text += "]}}}\n";
    return text;
}

/// An index of 2 and two extra features.
///
/// @return the calibration line
std::string gather_calibration() {
    return "{\"kind\":\"sample\",\"features\":{"
           "\"kinds\":{\"shape\":[1,1],\"values\":[131072]},"
           "\"extra\":{\"shape\":[1,1,2],\"values\":[65536,0]}"
           "}}\n";
}

struct Scratch {
    fs::path path;

    Scratch() : path(oa::test::make_scratch_directory("N04-brain-net")) {}

    ~Scratch() {
        std::error_code error;
        fs::remove_all(path, error);
    }

    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
};

struct Captured {
    int status = 0;
    std::string out;
    std::string err;
};

/// Runs oa-tool in-process.
///
/// @param arguments the command line, without the program name
/// @return the status and the two streams
Captured run(std::vector<std::string> arguments) {
    std::ostringstream out;
    std::ostringstream err;
    oa::tool::Output output{out, err};
    const int status = oa::tool::run_tool(std::move(arguments), output);
    return {status, out.str(), err.str()};
}

/// True when `text` contains `part`.
///
/// @param text the text
/// @param part the part
/// @return true when it does
bool contains(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

/// Writes text into the scratch directory.
///
/// @param path the file
/// @param text the bytes
void write_text(const fs::path& path, std::string_view text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    OA_CHECK(static_cast<bool>(out));
}

/// Writes bytes into the scratch directory.
///
/// @param path the file
/// @param bytes the bytes
void write_bytes(const fs::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(
        reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())
    );
    OA_CHECK(static_cast<bool>(out));
}

/// Reads a file the test wrote.
///
/// @param path the file
/// @return the bytes
std::vector<uint8_t> read_bytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>(
        std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()
    );
}

/// The SHA-256 of some bytes, as 64 hex digits.
///
/// @param bytes the bytes
/// @return the hex text
std::string digest_hex(const std::vector<uint8_t>& bytes) {
    const auto digest = oa::base::sha256::digest_of(bytes);
    const auto hex = oa::base::sha256::to_hex(digest);
    return std::string(hex.begin(), hex.end());
}

/// The tensor of a name, or null.
///
/// @param model the network
/// @param name the tensor name
/// @return the tensor, or null
const oann::Tensor* find_tensor(const oann::Model& model, std::string_view name) {
    for (const oann::Tensor& tensor : model.tensors) {
        if (tensor.name == name)
            return &tensor;
    }
    return nullptr;
}

/// Reads a .oann the command wrote.
///
/// @param path the file
/// @return the model, or nothing
std::optional<oann::Model> read_oann(const fs::path& path) {
    const std::vector<uint8_t> bytes = read_bytes(path);
    oann::Problem problem;
    return oann::read(bytes, problem);
}

/// The number after a label, such as `difference: chances `.
///
/// @param text the report
/// @param label the label, including the trailing space
/// @return the number, or nothing
std::optional<double> labeled_number(std::string_view text, std::string_view label) {
    const std::size_t at = text.find(label);
    if (at == std::string_view::npos)
        return std::nullopt;
    const char* first = text.data() + at + label.size();
    const char* last = text.data() + text.size();
    double value = 0.0;
    const std::from_chars_result converted = std::from_chars(first, last, value);
    if (converted.ec != std::errc{})
        return std::nullopt;
    return value;
}

/// Checks a status and, when it differs, prints both streams.
///
/// @param captured the run
/// @param status the expected status
/// @param name the case, for the print
void expect_status(const Captured& captured, int status, const char* name) {
    if (captured.status != status) {
        std::fprintf(
            stderr,
            "%s: status %d\nstdout:\n%s\nstderr:\n%s\n",
            name,
            captured.status,
            captured.out.c_str(),
            captured.err.c_str()
        );
    }
    OA_CHECK(captured.status == status);
}

/// Converts a model and returns the run.
///
/// @param scratch the directory
/// @param model the ONNX bytes
/// @param calibration the calibration text
/// @param out_name the .oann name
/// @param extra further arguments
/// @return the run
Captured convert(
    const Scratch& scratch,
    const std::vector<uint8_t>& model,
    std::string_view calibration,
    std::string_view out_name,
    const std::vector<std::string>& extra
) {
    const fs::path onnx = scratch.path / "model.onnx";
    const fs::path data = scratch.path / "samples.jsonl";
    const fs::path out = scratch.path / out_name;
    write_bytes(onnx, model);
    write_text(data, calibration);
    std::vector<std::string> arguments = {
        "brain", "net", onnx.string(), "--calibrate", data.string(), "--out", out.string()
    };
    arguments.insert(arguments.end(), extra.begin(), extra.end());
    return run(std::move(arguments));
}

void test_reader() {
    // The file is one length-delimited field, the graph, and that field's
    // length covers every byte after it. A proper prefix then either cuts the
    // key or the length, or leaves fewer bytes than the length names, so
    // every truncation is refused. An empty prefix is refused as well.
    Proto node;
    node.str(4, "Relu");
    Proto graph;
    graph.msg(1, node);
    Proto model;
    model.msg(7, graph);
    std::string problem;
    OA_CHECK(oa::tool::read_onnx(model.bytes, problem).has_value());
    OA_CHECK(problem.empty());
    for (std::size_t length = 0; length < model.bytes.size(); ++length) {
        problem.clear();
        const std::vector<uint8_t> prefix(
            model.bytes.begin(), model.bytes.begin() + static_cast<std::ptrdiff_t>(length)
        );
        OA_CHECK(!oa::tool::read_onnx(prefix, problem).has_value());
        OA_CHECK(!problem.empty());
    }

    std::vector<uint8_t> too_long{0x08};
    for (int piece = 0; piece < 10; ++piece)
        too_long.push_back(0x80);
    too_long.push_back(0x00);
    problem.clear();
    OA_CHECK(!oa::tool::read_onnx(too_long, problem).has_value());
    OA_CHECK(contains(problem, "a varint is longer than 10 bytes"));

    Proto negative;
    negative.vint_field(1, ~uint64_t{0});
    problem.clear();
    const std::optional<oa::tool::OnnxModel> read = oa::tool::read_onnx(negative.bytes, problem);
    OA_CHECK(read.has_value());
    if (read)
        OA_CHECK(read->ir_version == -1);

    problem.clear();
    const std::vector<uint8_t> group{0x0B};
    OA_CHECK(!oa::tool::read_onnx(group, problem).has_value());
    OA_CHECK(contains(problem, "a group (wire type 3)"));

    // Four wraps put the empty type at depth 16: model 1, then each wrap is a
    // graph, a node and an attribute (3), and the inner graph, its value and
    // its type follow. Five wraps put the inner graph at depth 17.
    Proto type;
    Proto value;
    value.str(1, "x");
    value.msg(2, type);
    Proto inner;
    inner.msg(11, value);
    auto wrapped = [&](int wraps) {
        Proto current = inner;
        for (int wrap = 0; wrap < wraps; ++wrap) {
            Proto attribute;
            attribute.msg(6, current);
            Proto nested_node;
            nested_node.str(4, "Identity");
            nested_node.msg(5, attribute);
            Proto nested_graph;
            nested_graph.msg(1, nested_node);
            current = std::move(nested_graph);
        }
        Proto wrapped_model;
        wrapped_model.msg(7, current);
        return wrapped_model;
    };
    problem.clear();
    OA_CHECK(oa::tool::read_onnx(wrapped(4).bytes, problem).has_value());
    OA_CHECK(problem.empty());
    problem.clear();
    OA_CHECK(!oa::tool::read_onnx(wrapped(5).bytes, problem).has_value());
    OA_CHECK(contains(problem, "a message nests 17 deep"));

    Proto external;
    external.str(8, "fc1.weight");
    external.vint_field(14, 1);
    external.bytes_field(9, f32_bytes(1.f));
    Proto external_graph;
    external_graph.msg(5, external);
    Proto external_model;
    external_model.msg(7, external_graph);
    problem.clear();
    OA_CHECK(!oa::tool::read_onnx(external_model.bytes, problem).has_value());
    OA_CHECK(contains(
        problem,
        "tensor fc1.weight keeps its data in another file; export with the data inside the model"
    ));
}

void test_dense(const Scratch& scratch) {
    const Captured first =
        convert(scratch, threat_model(), threat_calibration(), "threat.oann", {});
    expect_status(first, oa::tool::exit_done, "threat");
    const fs::path out = scratch.path / "threat.oann";
    const std::vector<uint8_t> bytes = read_bytes(out);
    const std::optional<oann::Model> model = read_oann(out);
    OA_CHECK(model.has_value());
    if (!model)
        return;
    OA_CHECK(model->layers.size() == 5);
    if (model->layers.size() == 5) {
        OA_CHECK(model->layers[0].kind == oann::LayerKind::dense);
        OA_CHECK(model->layers[1].kind == oann::LayerKind::relu);
        OA_CHECK(model->layers[2].kind == oann::LayerKind::dense);
        OA_CHECK(model->layers[3].kind == oann::LayerKind::softmax);
        OA_CHECK(model->layers[4].kind == oann::LayerKind::argmax);
    }
    const oann::Tensor* hidden_w = find_tensor(*model, "hidden_w");
    const oann::Tensor* hidden_b = find_tensor(*model, "hidden_b");
    const oann::Tensor* score_w = find_tensor(*model, "score_w");
    const oann::Tensor* score_b = find_tensor(*model, "score_b");
    const oann::Tensor* features = find_tensor(*model, "features");
    const oann::Tensor* chances = find_tensor(*model, "chances");
    const oann::Tensor* choice = find_tensor(*model, "choice");
    OA_CHECK(
        hidden_w != nullptr && hidden_b != nullptr && score_w != nullptr && score_b != nullptr
    );
    OA_CHECK(features != nullptr && chances != nullptr && choice != nullptr);
    if (hidden_w != nullptr) {
        OA_CHECK(hidden_w->type == oann::ElementType::int8);
        OA_CHECK(hidden_w->exponent == -6);
        OA_CHECK(hidden_w->data == std::vector<uint8_t>({64, 0, 0, 64}));
    }
    if (score_w != nullptr) {
        OA_CHECK(score_w->type == oann::ElementType::int8);
        OA_CHECK(score_w->exponent == -4);
        OA_CHECK(score_w->data == std::vector<uint8_t>({96, 0, 0, 0}));
    }
    if (hidden_b != nullptr) {
        OA_CHECK(hidden_b->type == oann::ElementType::int32);
        OA_CHECK(hidden_b->exponent == -20);
        OA_CHECK(hidden_b->data == std::vector<uint8_t>(8, 0));
    }
    if (score_b != nullptr) {
        OA_CHECK(score_b->type == oann::ElementType::int32);
        OA_CHECK(score_b->exponent == -18);
        OA_CHECK(score_b->data == std::vector<uint8_t>(8, 0));
    }
    if (features != nullptr)
        OA_CHECK(features->exponent == -14);
    if (chances != nullptr)
        OA_CHECK(chances->exponent == -15);
    if (choice != nullptr) {
        OA_CHECK(choice->type == oann::ElementType::int32);
        OA_CHECK(choice->exponent == 0);
        OA_CHECK(choice->rank == 1 && choice->dims[0] == 1);
    }
    OA_CHECK(contains(first.out, "samples: 200"));
    OA_CHECK(contains(first.out, "agreement: choice 100.0% (200 of 200)"));
    OA_CHECK(contains(first.out, "sha256: " + digest_hex(bytes)));
    OA_CHECK(contains(first.out, "inputs: features [2] exponent -14"));
    const std::optional<double> gap = labeled_number(first.out, "difference: chances ");
    OA_CHECK(gap.has_value());
    if (gap)
        OA_CHECK(*gap < 0.02);

    const Captured second =
        convert(scratch, threat_model(), threat_calibration(), "threat-again.oann", {"--json"});
    expect_status(second, oa::tool::exit_done, "threat json");
    OA_CHECK(read_bytes(scratch.path / "threat-again.oann") == bytes);
    json::JsonError error;
    const std::optional<json::Json> parsed = json::parse_json(second.out, error);
    OA_CHECK(parsed.has_value());
    if (parsed) {
        const json::Json* samples = parsed->find("samples");
        OA_CHECK(samples != nullptr && samples->integer() == 200);
        const json::Json* hash = parsed->find("sha256");
        OA_CHECK(
            hash != nullptr && hash->string() != nullptr && *hash->string() == digest_hex(bytes)
        );
    }
}

void test_weight_widths(const Scratch& scratch) {
    const std::vector<uint8_t> model = level_model();
    const std::string calibration = level_calibration();
    const Captured narrow =
        convert(scratch, model, calibration, "level-int8.oann", {"--weights", "int8"});
    const Captured wide =
        convert(scratch, model, calibration, "level-int16.oann", {"--weights", "int16"});
    expect_status(narrow, oa::tool::exit_done, "int8");
    expect_status(wide, oa::tool::exit_done, "int16");
    const std::optional<double> narrow_gap = labeled_number(narrow.out, "difference: level ");
    const std::optional<double> wide_gap = labeled_number(wide.out, "difference: level ");
    OA_CHECK(narrow_gap.has_value() && wide_gap.has_value());
    if (narrow_gap && wide_gap) {
        OA_CHECK(*narrow_gap > 0.0);
        OA_CHECK(*wide_gap < *narrow_gap);
    }
    const fs::path refused = scratch.path / "level-limited.oann";
    const Captured limited = convert(
        scratch, model, calibration, "level-limited.oann", {"--max-difference", "0.000001"}
    );
    expect_status(limited, oa::tool::exit_failed, "max-difference");
    OA_CHECK(contains(limited.err, "a difference passes --max-difference 0.000001"));
    OA_CHECK(contains(limited.out, "difference: level "));
    OA_CHECK(!fs::exists(refused));
}

void test_grid_and_gather(const Scratch& scratch) {
    const Captured grid = convert(scratch, grid_model(), grid_calibration(), "grid.oann", {});
    expect_status(grid, oa::tool::exit_done, "grid");
    const std::optional<oann::Model> grid_net = read_oann(scratch.path / "grid.oann");
    OA_CHECK(grid_net.has_value());
    if (grid_net && grid_net->layers.size() == 8) {
        OA_CHECK(grid_net->layers[0].kind == oann::LayerKind::conv2d);
        OA_CHECK(grid_net->layers[1].kind == oann::LayerKind::relu);
        OA_CHECK(grid_net->layers[2].kind == oann::LayerKind::maxpool);
        OA_CHECK(grid_net->layers[3].kind == oann::LayerKind::conv2d);
        OA_CHECK(grid_net->layers[4].kind == oann::LayerKind::tanh);
        OA_CHECK(grid_net->layers[5].kind == oann::LayerKind::flatten);
        OA_CHECK(grid_net->layers[6].kind == oann::LayerKind::dense);
        OA_CHECK(grid_net->layers[7].kind == oann::LayerKind::sigmoid);
    } else {
        OA_CHECK(grid_net && grid_net->layers.size() == 8);
    }

    const Captured gathered =
        convert(scratch, gather_model(), gather_calibration(), "joined.oann", {});
    expect_status(gathered, oa::tool::exit_done, "gather");
    const std::optional<oann::Model> joined = read_oann(scratch.path / "joined.oann");
    OA_CHECK(joined.has_value());
    if (joined && joined->layers.size() == 2) {
        OA_CHECK(joined->layers[0].kind == oann::LayerKind::embedding);
        OA_CHECK(joined->layers[1].kind == oann::LayerKind::concat);
        OA_CHECK(joined->layers[1].axis == 1);
        const oann::Tensor* kinds = find_tensor(*joined, "kinds");
        const oann::Tensor* result = find_tensor(*joined, "joined");
        OA_CHECK(kinds != nullptr && kinds->exponent == 0);
        OA_CHECK(
            result != nullptr && result->rank == 2 && result->dims[0] == 1 && result->dims[1] == 4
        );
    } else {
        OA_CHECK(joined && joined->layers.size() == 2);
    }
}

void test_refusals(const Scratch& scratch) {
    const fs::path onnx = scratch.path / "steady-commander-threat.onnx";
    const fs::path data = scratch.path / "samples.jsonl";
    const fs::path out = scratch.path / "refused.oann";
    write_bytes(onnx, level_model());
    write_text(data, level_calibration());

    Proto resize_graph;
    resize_graph.msg(11, value_info("features", 1, {dim_param("batch"), dim_value(1)}));
    resize_graph.msg(1, node_of("Resize", {"features"}, "y", {}));
    resize_graph.msg(12, value_info("y", 1, {dim_param("batch"), dim_value(1)}));
    write_bytes(scratch.path / "resize.onnx", model_of(resize_graph, 13).bytes);
    const Captured resize = run(
        {"brain",
         "net",
         (scratch.path / "resize.onnx").string(),
         "--calibrate",
         data.string(),
         "--out",
         out.string()}
    );
    expect_status(resize, oa::tool::exit_failed, "resize");
    OA_CHECK(
        contains(resize.err, "node 1 (Resize): Resize is not one of the layers a .oann holds")
    );
    OA_CHECK(!fs::exists(out));

    Proto external;
    external.str(8, "fc1.weight");
    external.vint_field(14, 1);
    external.bytes_field(9, f32_bytes(1.f));
    Proto external_graph;
    external_graph.msg(5, external);
    write_bytes(scratch.path / "external.onnx", model_of(external_graph, 13).bytes);
    const Captured external_run = run(
        {"brain",
         "net",
         (scratch.path / "external.onnx").string(),
         "--calibrate",
         data.string(),
         "--out",
         out.string()}
    );
    expect_status(external_run, oa::tool::exit_failed, "external");
    OA_CHECK(contains(
        external_run.err,
        "tensor fc1.weight keeps its data in another file; export with the data inside the model"
    ));

    write_bytes(scratch.path / "old.onnx", model_of(resize_graph, 10).bytes);
    const Captured old = run(
        {"brain",
         "net",
         (scratch.path / "old.onnx").string(),
         "--calibrate",
         data.string(),
         "--out",
         out.string()}
    );
    expect_status(old, oa::tool::exit_failed, "opset");
    OA_CHECK(contains(old.err, "the opset is 10, and a model is converted from opset 11 to 21"));

    write_text(data, "\n{\"kind\":\"note\"}\n\n{\"kind\":\"sample\",\"features\":{}}\n");
    const Captured missing =
        run({"brain", "net", onnx.string(), "--calibrate", data.string(), "--out", out.string()});
    expect_status(missing, oa::tool::exit_failed, "missing feature");
    OA_CHECK(contains(missing.err, "calibration line 4: feature features is missing"));

    write_text(data, "");
    const Captured empty =
        run({"brain", "net", onnx.string(), "--calibrate", data.string(), "--out", out.string()});
    expect_status(empty, oa::tool::exit_failed, "empty calibration");
    OA_CHECK(contains(empty.err, "the calibration file holds no sample"));

    write_text(
        data,
        "{\"kind\":\"sample\",\"features\":{\"features\":{\"shape\":[3],\"values\":[1,2,3]}}}\n"
    );
    const Captured counted =
        run({"brain", "net", onnx.string(), "--calibrate", data.string(), "--out", out.string()});
    expect_status(counted, oa::tool::exit_failed, "count");
    OA_CHECK(contains(
        counted.err, "calibration line 1: feature features has 3 values and the input has 1"
    ));

    write_text(data, level_calibration());
    write_bytes(out, {1, 2, 3, 4});
    const Captured blocked =
        run({"brain", "net", onnx.string(), "--calibrate", data.string(), "--out", out.string()});
    expect_status(blocked, oa::tool::exit_failed, "exists");
    OA_CHECK(contains(blocked.err, "pass --force to replace it"));
    OA_CHECK(blocked.out.empty());
    OA_CHECK(read_bytes(out) == std::vector<uint8_t>({1, 2, 3, 4}));
    const Captured forced = run(
        {"brain",
         "net",
         onnx.string(),
         "--calibrate",
         data.string(),
         "--out",
         out.string(),
         "--force"}
    );
    expect_status(forced, oa::tool::exit_done, "force");
    OA_CHECK(read_oann(out).has_value());

    const Captured unknown = run({"brain", "net", onnx.string(), "--nope"});
    expect_status(unknown, oa::tool::exit_usage, "unknown option");
    OA_CHECK(contains(unknown.err, "unknown option '--nope'"));

    const Captured bare = run({"brain", "net"});
    expect_status(bare, oa::tool::exit_usage, "bare");
}

} // namespace

int main() {
    const Scratch scratch;
    const auto run_case = [](const char* name, auto&& function) {
        try {
            function();
        } catch (const std::exception& error) {
            std::fprintf(stderr, "%s: %s\n", name, error.what());
            ++oa::test::failed_checks();
        }
    };
    run_case("test_reader", [] { test_reader(); });
    run_case("test_dense", [&] { test_dense(scratch); });
    run_case("test_weight_widths", [&] { test_weight_widths(scratch); });
    run_case("test_grid_and_gather", [&] { test_grid_and_gather(scratch); });
    run_case("test_refusals", [&] { test_refusals(scratch); });
    return oa::test::check_exit_status();
}
