// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The rules of format 1, in the order the module README lists them. The
// first one that fails is the one reported.

#include "oa/formats/oann/oann.hpp"

#include "coding.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::formats::oann {
namespace {

using coding::layer_field;
using coding::tensor_field;

/// Builds a refusal.
///
/// @param offset file offset of the field
/// @param message what is wrong
/// @return the problem
std::optional<Problem> reject(uint32_t offset, std::string message) {
    return Problem{std::move(message), offset};
}

/// Returns a tensor's place in a message.
///
/// @param model the network
/// @param index the tensor
/// @return `tensor I` or `tensor I (name)`
std::string tensor_label(const Model& model, std::size_t index) {
    std::string text = "tensor " + std::to_string(index);
    if (!model.tensors[index].name.empty()) {
        text += " (";
        text += model.tensors[index].name;
        text += ")";
    }
    return text;
}

/// Returns a layer's place in a message.
///
/// @param model the network
/// @param index the layer
/// @return `layer I` or `layer I (kind)`
std::string layer_label(const Model& model, std::size_t index) {
    std::string text = "layer " + std::to_string(index);
    const std::string_view name = kind_name(model.layers[index].kind);
    if (!name.empty()) {
        text += " (";
        text.append(name);
        text += ")";
    }
    return text;
}

/// Returns the name of a role.
///
/// @param role the role
/// @return a short name, or "none" when the role is not one of the four
const char* role_name(Role role) noexcept {
    switch (role) {
    case Role::input:
        return "input";
    case Role::output:
        return "output";
    case Role::activation:
        return "activation";
    case Role::constant:
        return "constant";
    }
    return "none";
}

/// Returns the name of an element type.
///
/// @param type the type
/// @return "int8", "int16", "int32", or "none"
const char* type_name(ElementType type) noexcept {
    switch (type) {
    case ElementType::int8:
        return "int8";
    case ElementType::int16:
        return "int16";
    case ElementType::int32:
        return "int32";
    }
    return "none";
}

/// Prints a tensor's dimensions in brackets.
///
/// @param tensor the tensor
/// @return `[d0, d1, ...]` for the tensor's rank
std::string shape_text(const Tensor& tensor) {
    std::string text = "[";
    const uint8_t rank = tensor.rank <= 4 ? tensor.rank : 0;
    for (uint8_t axis = 0; axis < rank; ++axis) {
        if (axis != 0)
            text += ", ";
        text += std::to_string(tensor.dims[axis]);
    }
    text += "]";
    return text;
}

/// Prints a list of dimensions in brackets.
///
/// @param dims the dimensions
/// @param rank how many to print
/// @return `[d0, d1, ...]`
std::string shape_list(const uint64_t* dims, uint8_t rank) {
    std::string text = "[";
    for (uint8_t axis = 0; axis < rank; ++axis) {
        if (axis != 0)
            text += ", ";
        text += std::to_string(dims[axis]);
    }
    text += "]";
    return text;
}

/// Returns whether a name byte is allowed.
///
/// @param byte the character
/// @return true for a-z, 0-9 and _
bool name_byte(char byte) noexcept {
    return (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') || byte == '_';
}

/// Returns whether two tensors have the same rank and dimensions.
///
/// @param left one tensor
/// @param right the other
/// @return true when the shapes are equal
bool same_shape(const Tensor& left, const Tensor& right) noexcept {
    if (left.rank != right.rank)
        return false;
    for (uint8_t axis = 0; axis < left.rank; ++axis) {
        if (left.dims[axis] != right.dims[axis])
            return false;
    }
    return true;
}

/// Checks one tensor's fields.
///
/// @param model the network
/// @param index the tensor
/// @return the broken rule, or nothing
std::optional<Problem> check_tensor(const Model& model, std::size_t index) {
    const Tensor& tensor = model.tensors[index];
    const std::string label = tensor_label(model, index);
    const std::string& name = tensor.name;
    if (name.empty())
        return reject(tensor_field(index, 0), label + ": the name is empty");
    if (name.size() > 31) {
        return reject(
            tensor_field(index, 0),
            label + ": the name is " + std::to_string(name.size()) + " bytes, and must be 1 to 31"
        );
    }
    for (char byte : name) {
        if (!name_byte(byte)) {
            return reject(
                tensor_field(index, 0), label + ": the name has a byte outside a-z, 0-9 and _"
            );
        }
    }
    if (name[0] < 'a' || name[0] > 'z') {
        return reject(
            tensor_field(index, 0),
            label + ": the name starts with '" + std::string(1, name[0]) +
                "', and must start with a letter"
        );
    }
    for (std::size_t earlier = 0; earlier < index; ++earlier) {
        if (model.tensors[earlier].name == name) {
            return reject(
                tensor_field(index, 0),
                label + ": the name is the same as " + tensor_label(model, earlier)
            );
        }
    }

    const auto role_byte = static_cast<uint8_t>(tensor.role);
    if (role_byte < static_cast<uint8_t>(Role::input) ||
        role_byte > static_cast<uint8_t>(Role::constant)) {
        return reject(
            tensor_field(index, 32),
            label + ": the role is " + std::to_string(role_byte) +
                ", and must be input, output, activation or constant"
        );
    }
    const auto type_byte = static_cast<uint8_t>(tensor.type);
    if (type_byte < static_cast<uint8_t>(ElementType::int8) ||
        type_byte > static_cast<uint8_t>(ElementType::int32)) {
        return reject(
            tensor_field(index, 33),
            label + ": the type is " + std::to_string(type_byte) +
                ", and must be int8, int16 or int32"
        );
    }
    if (tensor.exponent < min_exponent || tensor.exponent > max_exponent) {
        return reject(
            tensor_field(index, 34),
            label + ": the exponent is " + std::to_string(tensor.exponent) + ", and must be " +
                std::to_string(min_exponent) + " to " + std::to_string(max_exponent)
        );
    }
    if (tensor.rank < 1 || tensor.rank > 4) {
        return reject(
            tensor_field(index, 35),
            label + ": the rank is " + std::to_string(tensor.rank) + ", and must be 1 to 4"
        );
    }
    for (uint8_t axis = 0; axis < 4; ++axis) {
        const uint32_t dim = tensor.dims[axis];
        if (axis < tensor.rank) {
            if (dim < 1 || dim > max_dimension) {
                return reject(
                    tensor_field(index, 36 + axis * 4U),
                    label + ": dim " + std::to_string(axis) + " is " + std::to_string(dim) +
                        ", and must be 1 to " + std::to_string(max_dimension)
                );
            }
        } else if (dim != 0) {
            return reject(
                tensor_field(index, 36 + axis * 4U),
                label + ": dim " + std::to_string(axis) + " is " + std::to_string(dim) +
                    ", and must be 0"
            );
        }
    }
    const uint64_t count = element_count(tensor);
    if (count > max_tensor_elements) {
        return reject(
            tensor_field(index, 36),
            label + ": the tensor has " + std::to_string(count) + " elements, and the limit is " +
                std::to_string(max_tensor_elements)
        );
    }
    const uint64_t need = count * element_bytes(tensor.type);
    if (tensor.role == Role::constant) {
        if (tensor.data.size() != need) {
            return reject(
                tensor_field(index, 56),
                label + ": a constant needs int8, int16 or int32 data of " + std::to_string(need) +
                    " bytes, the entry says " + std::to_string(tensor.data.size())
            );
        }
    } else if (!tensor.data.empty()) {
        return reject(
            tensor_field(index, 56),
            label + ": an " + std::string(role_name(tensor.role)) + " carries " +
                std::to_string(tensor.data.size()) + " bytes of data, and only a constant does"
        );
    }
    if ((tensor.role == Role::input || tensor.role == Role::activation) &&
        tensor.type != ElementType::int16) {
        return reject(
            tensor_field(index, 33),
            label + ": an " + std::string(role_name(tensor.role)) +
                " needs int16, and the type is " + type_name(tensor.type)
        );
    }
    return std::nullopt;
}

/// How a kind uses its fields.
struct KindUse {
    uint8_t min_inputs{};
    uint8_t max_inputs{};
    bool weights{};
    bool bias{};
    uint8_t spatial{}; ///< 0 none, 1 conv2d, 2 maxpool
    bool axis{};
    bool slope{};
};

/// Returns the field rules of a kind.
///
/// @param kind a kind already known to be one of the thirteen
/// @return which fields the kind uses
KindUse kind_use(LayerKind kind) noexcept {
    KindUse use{};
    use.min_inputs = 1;
    use.max_inputs = 1;
    switch (kind) {
    case LayerKind::dense:
        use.weights = true;
        use.bias = true;
        break;
    case LayerKind::conv2d:
        use.weights = true;
        use.bias = true;
        use.spatial = 1;
        break;
    case LayerKind::maxpool:
        use.spatial = 2;
        break;
    case LayerKind::leaky_relu:
        use.slope = true;
        break;
    case LayerKind::add:
        use.min_inputs = 2;
        use.max_inputs = 2;
        break;
    case LayerKind::concat:
        use.min_inputs = 2;
        use.max_inputs = 4;
        use.axis = true;
        break;
    case LayerKind::embedding:
        use.weights = true;
        break;
    case LayerKind::relu:
    case LayerKind::tanh:
    case LayerKind::sigmoid:
    case LayerKind::flatten:
    case LayerKind::softmax:
    case LayerKind::argmax:
        break;
    }
    return use;
}

/// Checks the fields every kind shares: slots, kernel, axis and slope.
///
/// @param model the network
/// @param index the layer
/// @param use what the kind allows
/// @return the broken rule, or nothing
std::optional<Problem> check_fields(const Model& model, std::size_t index, KindUse use) {
    const Layer& layer = model.layers[index];
    const std::string label = layer_label(model, index);
    const std::size_t tensors = model.tensors.size();
    if (layer.input_count < use.min_inputs || layer.input_count > use.max_inputs) {
        std::string need;
        if (use.min_inputs == use.max_inputs) {
            need = std::to_string(use.min_inputs) + (use.min_inputs == 1 ? " input" : " inputs");
        } else {
            need = std::to_string(use.min_inputs) + " to " + std::to_string(use.max_inputs) +
                   " inputs";
        }
        return reject(
            layer_field(tensors, index, 1),
            label + ": needs " + need + ", the entry says " + std::to_string(layer.input_count)
        );
    }
    for (uint8_t slot = 0; slot < 4; ++slot) {
        const uint16_t input = layer.inputs[slot];
        if (slot < layer.input_count) {
            if (input == no_tensor) {
                return reject(
                    layer_field(tensors, index, 2 + slot * 2U),
                    label + ": input " + std::to_string(slot) + " is none, and must name a tensor"
                );
            }
            if (input >= tensors) {
                return reject(
                    layer_field(tensors, index, 2 + slot * 2U),
                    label + ": input " + std::to_string(slot) + " is " + std::to_string(input) +
                        ", and the model has " + std::to_string(tensors) + " tensors"
                );
            }
        } else if (input != no_tensor) {
            return reject(
                layer_field(tensors, index, 2 + slot * 2U),
                label + ": input slot " + std::to_string(slot) + " is " + std::to_string(input) +
                    ", and an unused slot is none"
            );
        }
    }
    if (layer.output == no_tensor) {
        return reject(
            layer_field(tensors, index, 10), label + ": the output is none, and must name a tensor"
        );
    }
    if (layer.output >= tensors) {
        return reject(
            layer_field(tensors, index, 10),
            label + ": the output is " + std::to_string(layer.output) + ", and the model has " +
                std::to_string(tensors) + " tensors"
        );
    }
    if (use.weights) {
        if (layer.weights == no_tensor) {
            return reject(layer_field(tensors, index, 12), label + ": needs weights");
        }
        if (layer.weights >= tensors) {
            return reject(
                layer_field(tensors, index, 12),
                label + ": the weights are " + std::to_string(layer.weights) +
                    ", and the model has " + std::to_string(tensors) + " tensors"
            );
        }
    } else if (layer.weights != no_tensor) {
        return reject(
            layer_field(tensors, index, 12), label + ": names weights, and this layer has none"
        );
    }
    if (use.bias) {
        if (layer.bias != no_tensor && layer.bias >= tensors) {
            return reject(
                layer_field(tensors, index, 14),
                label + ": the bias is " + std::to_string(layer.bias) + ", and the model has " +
                    std::to_string(tensors) + " tensors"
            );
        }
    } else if (layer.bias != no_tensor) {
        return reject(
            layer_field(tensors, index, 14), label + ": names a bias, and this layer has none"
        );
    }

    const uint8_t kernel_max = use.spatial == 0 ? 0 : 15;
    const uint8_t kernel_min = use.spatial == 0 ? 0 : 1;
    const char* spatial_names[] = {
        "kernel height", "kernel width", "stride height", "stride width"
    };
    const uint8_t spatial_values[] = {
        layer.kernel_h, layer.kernel_w, layer.stride_h, layer.stride_w
    };
    for (uint8_t field = 0; field < 4; ++field) {
        const uint8_t value = spatial_values[field];
        const uint8_t lo = kernel_min;
        const uint8_t hi = kernel_max;
        if (value < lo || value > hi) {
            std::string range =
                lo == hi ? std::to_string(lo) : std::to_string(lo) + " to " + std::to_string(hi);
            return reject(
                layer_field(tensors, index, 16 + field),
                label + ": the " + spatial_names[field] + " is " + std::to_string(value) +
                    ", and must be " + range
            );
        }
    }
    if (use.spatial == 1) {
        if (layer.pad_h > layer.kernel_h - 1) {
            return reject(
                layer_field(tensors, index, 20),
                label + ": the pad height is " + std::to_string(layer.pad_h) +
                    ", and must be 0 to " + std::to_string(layer.kernel_h - 1)
            );
        }
        if (layer.pad_w > layer.kernel_w - 1) {
            return reject(
                layer_field(tensors, index, 21),
                label + ": the pad width is " + std::to_string(layer.pad_w) +
                    ", and must be 0 to " + std::to_string(layer.kernel_w - 1)
            );
        }
    } else if (layer.pad_h != 0 || layer.pad_w != 0) {
        const bool height = layer.pad_h != 0;
        return reject(
            layer_field(tensors, index, height ? 20 : 21),
            label + ": the pad " + std::string(height ? "height" : "width") + " is " +
                std::to_string(height ? layer.pad_h : layer.pad_w) + ", and must be 0"
        );
    }
    if (!use.axis && layer.axis != 0) {
        return reject(
            layer_field(tensors, index, 22),
            label + ": the axis is " + std::to_string(layer.axis) + ", and must be 0"
        );
    }
    if (use.slope) {
        if (layer.alpha < 0) {
            return reject(
                layer_field(tensors, index, 24),
                label + ": the slope is " + std::to_string(layer.alpha) + ", and must be 0 to 32767"
            );
        }
    } else if (layer.alpha != 0) {
        return reject(
            layer_field(tensors, index, 24),
            label + ": the slope is " + std::to_string(layer.alpha) + ", and must be 0"
        );
    }
    return std::nullopt;
}

/// Checks that a tensor used as an input is int16.
///
/// @param model the network
/// @param index the layer
/// @param slot which input
/// @return the broken rule, or nothing
std::optional<Problem> input_is_int16(const Model& model, std::size_t index, uint8_t slot) {
    const Tensor& tensor = model.tensors[model.layers[index].inputs[slot]];
    if (tensor.type == ElementType::int16)
        return std::nullopt;
    return reject(
        tensor_field(model.layers[index].inputs[slot], 33),
        layer_label(model, index) + ": the input's type is " + type_name(tensor.type) +
            ", and must be int16"
    );
}

/// Checks a weights tensor's role and integer width.
///
/// @param model the network
/// @param index the layer
/// @return the broken rule, or nothing when the weights are an int8 or int16 constant
std::optional<Problem> weights_are_narrow(const Model& model, std::size_t index) {
    const uint16_t id = model.layers[index].weights;
    const Tensor& weights = model.tensors[id];
    const std::string label = layer_label(model, index);
    if (weights.role != Role::constant) {
        return reject(
            layer_field(model.tensors.size(), index, 12),
            label + ": the weights are " + tensor_label(model, id) + ", " +
                (weights.role == Role::activation || weights.role == Role::input ? "an " : "a ") +
                role_name(weights.role) + ", and weights are a constant"
        );
    }
    if (weights.type != ElementType::int8 && weights.type != ElementType::int16) {
        return reject(
            tensor_field(id, 33),
            label + ": the weights' type is " + type_name(weights.type) +
                ", and must be int8 or int16"
        );
    }
    return std::nullopt;
}

/// Checks the bias exponent and the output's scale shift.
///
/// @param model the network
/// @param index the layer
/// @param taken the input
/// @param weights the weights
/// @param result the output
/// @return the broken rule, or nothing
std::optional<Problem> check_scale(
    const Model& model,
    std::size_t index,
    const Tensor& taken,
    const Tensor& weights,
    const Tensor& result
) {
    const Layer& layer = model.layers[index];
    const std::string label = layer_label(model, index);
    const int made = int{taken.exponent} + int{weights.exponent};
    if (layer.bias != no_tensor) {
        const Tensor& bias = model.tensors[layer.bias];
        if (int{bias.exponent} != made) {
            return reject(
                tensor_field(layer.bias, 34),
                label + ": the bias's exponent is " + std::to_string(int{bias.exponent}) +
                    ", the input's and the weights' make " + std::to_string(made)
            );
        }
    }
    const int shift = int{result.exponent} - int{taken.exponent} - int{weights.exponent};
    if (shift < 0 || shift > 62) {
        return reject(
            tensor_field(layer.output, 34),
            label + ": the output scale shift is " + std::to_string(shift) + ", and must be 0 to 62"
        );
    }
    return std::nullopt;
}

/// Checks a dense layer's tensors.
///
/// @param model the network
/// @param index the layer
/// @return the broken rule, or nothing
std::optional<Problem> check_dense(const Model& model, std::size_t index) {
    if (const std::optional<Problem> found = input_is_int16(model, index, 0))
        return found;
    if (const std::optional<Problem> found = weights_are_narrow(model, index))
        return found;
    const Layer& layer = model.layers[index];
    const Tensor& taken = model.tensors[layer.inputs[0]];
    const Tensor& weights = model.tensors[layer.weights];
    const Tensor& result = model.tensors[layer.output];
    const std::string label = layer_label(model, index);
    const uint64_t wide = element_count(taken);
    if (weights.rank != 2 || weights.dims[1] != wide) {
        uint64_t expect[2] = {weights.rank == 2 ? weights.dims[0] : 0, wide};
        if (weights.rank != 2)
            expect[0] = 0;
        return reject(
            tensor_field(layer.weights, 35),
            label + ": the weights are " + shape_text(weights) + ", and must be " +
                shape_list(expect, 2)
        );
    }
    const uint32_t channels = weights.dims[0];
    if (layer.bias != no_tensor) {
        const Tensor& bias = model.tensors[layer.bias];
        if (bias.role != Role::constant) {
            return reject(
                layer_field(model.tensors.size(), index, 14),
                label + ": the bias is " + tensor_label(model, layer.bias) + ", " +
                    (bias.role == Role::activation || bias.role == Role::input ? "an " : "a ") +
                    role_name(bias.role) + ", and a bias is a constant"
            );
        }
        if (bias.type != ElementType::int32) {
            return reject(
                tensor_field(layer.bias, 33),
                label + ": the bias's type is " + type_name(bias.type) + ", and must be int32"
            );
        }
        if (bias.rank != 1 || bias.dims[0] != channels) {
            return reject(
                tensor_field(layer.bias, 35),
                label + ": the bias is " + shape_text(bias) + ", and must be [" +
                    std::to_string(channels) + "]"
            );
        }
    }
    if (result.type != ElementType::int16) {
        return reject(
            tensor_field(layer.output, 33),
            label + ": the output's type is " + type_name(result.type) + ", and must be int16"
        );
    }
    if (result.rank != 1 || result.dims[0] != channels) {
        return reject(
            tensor_field(layer.output, 35),
            label + ": the output is " + shape_text(result) + ", and must be [" +
                std::to_string(channels) + "]"
        );
    }
    return check_scale(model, index, taken, weights, result);
}

/// The spatial size one conv2d or maxpool axis produces.
///
/// @param input the input length
/// @param pad padding on that axis; 0 for maxpool
/// @param kernel the window
/// @param stride the step
/// @param[out] produced the output length when the window fits
/// @return false when the padded input is shorter than the window
bool spatial_length(
    uint32_t input, uint32_t pad, uint32_t kernel, uint32_t stride, uint64_t& produced
) noexcept {
    const uint64_t padded = uint64_t{input} + uint64_t{pad} + uint64_t{pad};
    if (padded < kernel)
        return false;
    produced = (padded - kernel) / stride + 1;
    return true;
}

/// Checks a conv2d layer's tensors.
///
/// @param model the network
/// @param index the layer
/// @return the broken rule, or nothing
std::optional<Problem> check_conv(const Model& model, std::size_t index) {
    if (const std::optional<Problem> found = input_is_int16(model, index, 0))
        return found;
    const Layer& layer = model.layers[index];
    const Tensor& taken = model.tensors[layer.inputs[0]];
    const std::string label = layer_label(model, index);
    if (taken.rank != 3) {
        return reject(
            tensor_field(layer.inputs[0], 35),
            label + ": the input's rank is " + std::to_string(taken.rank) + ", and must be 3"
        );
    }
    if (const std::optional<Problem> found = weights_are_narrow(model, index))
        return found;
    const Tensor& weights = model.tensors[layer.weights];
    // The leading dimension is the layer's own choice; match the other three.
    if (weights.rank != 4 || weights.dims[1] != taken.dims[0] ||
        weights.dims[2] != layer.kernel_h || weights.dims[3] != layer.kernel_w) {
        uint64_t shown[4] = {
            weights.rank == 4 ? weights.dims[0] : 1, taken.dims[0], layer.kernel_h, layer.kernel_w
        };
        return reject(
            tensor_field(layer.weights, 35),
            label + ": the weights are " + shape_text(weights) + ", and must be " +
                shape_list(shown, 4)
        );
    }
    const uint32_t channels = weights.dims[0];
    if (layer.bias != no_tensor) {
        const Tensor& bias = model.tensors[layer.bias];
        if (bias.role != Role::constant) {
            return reject(
                layer_field(model.tensors.size(), index, 14),
                label + ": the bias is " + tensor_label(model, layer.bias) +
                    ", and a bias is a constant"
            );
        }
        if (bias.type != ElementType::int32) {
            return reject(
                tensor_field(layer.bias, 33),
                label + ": the bias's type is " + type_name(bias.type) + ", and must be int32"
            );
        }
        if (bias.rank != 1 || bias.dims[0] != channels) {
            return reject(
                tensor_field(layer.bias, 35),
                label + ": the bias is " + shape_text(bias) + ", and must be [" +
                    std::to_string(channels) + "]"
            );
        }
    }
    uint64_t height = 0;
    uint64_t width = 0;
    if (!spatial_length(taken.dims[1], layer.pad_h, layer.kernel_h, layer.stride_h, height)) {
        return reject(
            layer_field(model.tensors.size(), index, 20),
            label + ": the input height is " + std::to_string(taken.dims[1]) +
                " and the padding is " + std::to_string(layer.pad_h) +
                ", which is shorter than the kernel height " + std::to_string(layer.kernel_h)
        );
    }
    if (!spatial_length(taken.dims[2], layer.pad_w, layer.kernel_w, layer.stride_w, width)) {
        return reject(
            layer_field(model.tensors.size(), index, 21),
            label + ": the input width is " + std::to_string(taken.dims[2]) +
                " and the padding is " + std::to_string(layer.pad_w) +
                ", which is shorter than the kernel width " + std::to_string(layer.kernel_w)
        );
    }
    const Tensor& result = model.tensors[layer.output];
    if (result.type != ElementType::int16) {
        return reject(
            tensor_field(layer.output, 33),
            label + ": the output's type is " + type_name(result.type) + ", and must be int16"
        );
    }
    const uint64_t expect_out[3] = {channels, height, width};
    if (result.rank != 3 || result.dims[0] != channels || result.dims[1] != height ||
        result.dims[2] != width) {
        return reject(
            tensor_field(layer.output, 35),
            label + ": the output is " + shape_text(result) + ", and must be " +
                shape_list(expect_out, 3)
        );
    }
    return check_scale(model, index, taken, weights, result);
}

/// Checks a maxpool layer's tensors.
///
/// @param model the network
/// @param index the layer
/// @return the broken rule, or nothing
std::optional<Problem> check_pool(const Model& model, std::size_t index) {
    if (const std::optional<Problem> found = input_is_int16(model, index, 0))
        return found;
    const Layer& layer = model.layers[index];
    const Tensor& taken = model.tensors[layer.inputs[0]];
    const Tensor& result = model.tensors[layer.output];
    const std::string label = layer_label(model, index);
    if (taken.rank != 3) {
        return reject(
            tensor_field(layer.inputs[0], 35),
            label + ": the input's rank is " + std::to_string(taken.rank) + ", and must be 3"
        );
    }
    uint64_t height = 0;
    uint64_t width = 0;
    if (taken.dims[1] < layer.kernel_h) {
        return reject(
            layer_field(model.tensors.size(), index, 16),
            label + ": the input height is " + std::to_string(taken.dims[1]) +
                ", and must be at least the kernel height " + std::to_string(layer.kernel_h)
        );
    }
    if (taken.dims[2] < layer.kernel_w) {
        return reject(
            layer_field(model.tensors.size(), index, 17),
            label + ": the input width is " + std::to_string(taken.dims[2]) +
                ", and must be at least the kernel width " + std::to_string(layer.kernel_w)
        );
    }
    height = (uint64_t{taken.dims[1]} - layer.kernel_h) / layer.stride_h + 1;
    width = (uint64_t{taken.dims[2]} - layer.kernel_w) / layer.stride_w + 1;
    if (result.type != ElementType::int16) {
        return reject(
            tensor_field(layer.output, 33),
            label + ": the output's type is " + type_name(result.type) + ", and must be int16"
        );
    }
    const uint64_t expect_out[3] = {taken.dims[0], height, width};
    if (result.rank != 3 || result.dims[0] != taken.dims[0] || result.dims[1] != height ||
        result.dims[2] != width) {
        return reject(
            tensor_field(layer.output, 35),
            label + ": the output is " + shape_text(result) + ", and must be " +
                shape_list(expect_out, 3)
        );
    }
    if (result.exponent != taken.exponent) {
        return reject(
            tensor_field(layer.output, 34),
            label + ": the output's exponent is " + std::to_string(result.exponent) +
                ", the input's is " + std::to_string(taken.exponent)
        );
    }
    return std::nullopt;
}

/// Checks a layer whose output keeps the input's shape.
///
/// @param model the network
/// @param index the layer
/// @param fixed_exponent the output exponent, or 99 when it must match the input
/// @return the broken rule, or nothing
std::optional<Problem> check_same(const Model& model, std::size_t index, int fixed_exponent) {
    if (const std::optional<Problem> found = input_is_int16(model, index, 0))
        return found;
    const Layer& layer = model.layers[index];
    const Tensor& taken = model.tensors[layer.inputs[0]];
    const Tensor& result = model.tensors[layer.output];
    const std::string label = layer_label(model, index);
    if (result.type != ElementType::int16) {
        return reject(
            tensor_field(layer.output, 33),
            label + ": the output's type is " + type_name(result.type) + ", and must be int16"
        );
    }
    if (!same_shape(taken, result)) {
        return reject(
            tensor_field(layer.output, 35),
            label + ": the output is " + shape_text(result) + ", and must match the input " +
                shape_text(taken)
        );
    }
    if (fixed_exponent == 99) {
        if (result.exponent != taken.exponent) {
            return reject(
                tensor_field(layer.output, 34),
                label + ": the output's exponent is " + std::to_string(result.exponent) +
                    ", the input's is " + std::to_string(taken.exponent)
            );
        }
    } else if (result.exponent != fixed_exponent) {
        return reject(
            tensor_field(layer.output, 34),
            label + ": the output's exponent is " + std::to_string(result.exponent) +
                ", and must be " + std::to_string(fixed_exponent)
        );
    }
    return std::nullopt;
}

/// Checks an add layer.
///
/// @param model the network
/// @param index the layer
/// @return the broken rule, or nothing
std::optional<Problem> check_add(const Model& model, std::size_t index) {
    if (const std::optional<Problem> found = input_is_int16(model, index, 0))
        return found;
    if (const std::optional<Problem> found = input_is_int16(model, index, 1))
        return found;
    const Layer& layer = model.layers[index];
    const Tensor& left = model.tensors[layer.inputs[0]];
    const Tensor& right = model.tensors[layer.inputs[1]];
    const Tensor& result = model.tensors[layer.output];
    const std::string label = layer_label(model, index);
    if (!same_shape(left, right)) {
        return reject(
            tensor_field(layer.inputs[1], 35),
            label + ": input 1 is " + shape_text(right) + ", and must match input 0 " +
                shape_text(left)
        );
    }
    if (result.type != ElementType::int16) {
        return reject(
            tensor_field(layer.output, 33),
            label + ": the output's type is " + type_name(result.type) + ", and must be int16"
        );
    }
    if (!same_shape(left, result)) {
        return reject(
            tensor_field(layer.output, 35),
            label + ": the output is " + shape_text(result) + ", and must match the inputs " +
                shape_text(left)
        );
    }
    return std::nullopt;
}

/// Checks a concat layer.
///
/// @param model the network
/// @param index the layer
/// @return the broken rule, or nothing
std::optional<Problem> check_concat(const Model& model, std::size_t index) {
    const Layer& layer = model.layers[index];
    const std::string label = layer_label(model, index);
    for (uint8_t slot = 0; slot < layer.input_count; ++slot) {
        if (const std::optional<Problem> found = input_is_int16(model, index, slot))
            return found;
    }
    const Tensor& first = model.tensors[layer.inputs[0]];
    if (layer.axis >= first.rank) {
        return reject(
            layer_field(model.tensors.size(), index, 22),
            label + ": the axis is " + std::to_string(layer.axis) + ", and must be 0 to " +
                std::to_string(first.rank - 1)
        );
    }
    for (uint8_t slot = 1; slot < layer.input_count; ++slot) {
        const Tensor& next = model.tensors[layer.inputs[slot]];
        if (next.rank != first.rank) {
            return reject(
                tensor_field(layer.inputs[slot], 35),
                label + ": input " + std::to_string(slot) + "'s rank is " +
                    std::to_string(next.rank) + ", and must match input 0's rank " +
                    std::to_string(first.rank)
            );
        }
        for (uint8_t axis = 0; axis < first.rank; ++axis) {
            if (axis == layer.axis)
                continue;
            if (next.dims[axis] != first.dims[axis]) {
                return reject(
                    tensor_field(layer.inputs[slot], 36 + axis * 4U),
                    label + ": input " + std::to_string(slot) + "'s dim " + std::to_string(axis) +
                        " is " + std::to_string(next.dims[axis]) +
                        ", and must match input 0's dim " + std::to_string(axis) + " of " +
                        std::to_string(first.dims[axis])
                );
            }
        }
    }
    uint64_t joined = 0;
    for (uint8_t slot = 0; slot < layer.input_count; ++slot)
        joined += model.tensors[layer.inputs[slot]].dims[layer.axis];
    const Tensor& result = model.tensors[layer.output];
    if (result.type != ElementType::int16) {
        return reject(
            tensor_field(layer.output, 33),
            label + ": the output's type is " + type_name(result.type) + ", and must be int16"
        );
    }
    if (result.rank != first.rank) {
        return reject(
            tensor_field(layer.output, 35),
            label + ": the output's rank is " + std::to_string(result.rank) + ", and must be " +
                std::to_string(first.rank)
        );
    }
    if (joined > max_dimension || result.dims[layer.axis] != joined) {
        return reject(
            tensor_field(layer.output, 36 + layer.axis * 4U),
            label + ": the output's dim " + std::to_string(layer.axis) + " is " +
                std::to_string(result.dims[layer.axis]) + ", and the inputs join to " +
                std::to_string(joined)
        );
    }
    for (uint8_t axis = 0; axis < first.rank; ++axis) {
        if (axis == layer.axis)
            continue;
        if (result.dims[axis] != first.dims[axis]) {
            return reject(
                tensor_field(layer.output, 36 + axis * 4U),
                label + ": the output's dim " + std::to_string(axis) + " is " +
                    std::to_string(result.dims[axis]) + ", and must be " +
                    std::to_string(first.dims[axis])
            );
        }
    }
    return std::nullopt;
}

/// Checks a flatten layer.
///
/// @param model the network
/// @param index the layer
/// @return the broken rule, or nothing
std::optional<Problem> check_flatten(const Model& model, std::size_t index) {
    if (const std::optional<Problem> found = input_is_int16(model, index, 0))
        return found;
    const Layer& layer = model.layers[index];
    const Tensor& taken = model.tensors[layer.inputs[0]];
    const Tensor& result = model.tensors[layer.output];
    const std::string label = layer_label(model, index);
    const uint64_t count = element_count(taken);
    if (result.type != ElementType::int16) {
        return reject(
            tensor_field(layer.output, 33),
            label + ": the output's type is " + type_name(result.type) + ", and must be int16"
        );
    }
    if (result.rank != 1 || result.dims[0] != count) {
        return reject(
            tensor_field(layer.output, 35),
            label + ": the output is " + shape_text(result) + ", and must be a vector of " +
                std::to_string(count)
        );
    }
    if (result.exponent != taken.exponent) {
        return reject(
            tensor_field(layer.output, 34),
            label + ": the output's exponent is " + std::to_string(result.exponent) +
                ", the input's is " + std::to_string(taken.exponent)
        );
    }
    return std::nullopt;
}

/// Checks an argmax layer, including that nothing reads its output.
///
/// @param model the network
/// @param index the layer
/// @return the broken rule, or nothing
std::optional<Problem> check_argmax(const Model& model, std::size_t index) {
    if (const std::optional<Problem> found = input_is_int16(model, index, 0))
        return found;
    const Layer& layer = model.layers[index];
    const Tensor& result = model.tensors[layer.output];
    const std::string label = layer_label(model, index);
    if (result.role != Role::output) {
        return reject(
            tensor_field(layer.output, 32),
            label + ": the output's role is " + role_name(result.role) + ", and must be output"
        );
    }
    if (result.type != ElementType::int32) {
        return reject(
            tensor_field(layer.output, 33),
            label + ": the output's type is " + type_name(result.type) + ", and must be int32"
        );
    }
    if (result.rank != 1 || result.dims[0] != 1) {
        return reject(
            tensor_field(layer.output, 35),
            label + ": the output is " + shape_text(result) + ", and must be [1]"
        );
    }
    if (result.exponent != 0) {
        return reject(
            tensor_field(layer.output, 34),
            label + ": the output's exponent is " + std::to_string(result.exponent) +
                ", and must be 0"
        );
    }
    for (std::size_t other = 0; other < model.layers.size(); ++other) {
        const Layer& reader = model.layers[other];
        for (uint8_t slot = 0; slot < reader.input_count && slot < 4; ++slot) {
            if (reader.inputs[slot] == layer.output) {
                return reject(
                    layer_field(model.tensors.size(), other, 2 + slot * 2U),
                    label + ": " + tensor_label(model, layer.output) + " is read by " +
                        layer_label(model, other) + ", and an argmax output is read by no layer"
                );
            }
        }
        if (reader.weights == layer.output || reader.bias == layer.output) {
            return reject(
                layer_field(model.tensors.size(), other, reader.weights == layer.output ? 12 : 14),
                label + ": " + tensor_label(model, layer.output) + " is read by " +
                    layer_label(model, other) + ", and an argmax output is read by no layer"
            );
        }
    }
    return std::nullopt;
}

/// Checks an embedding layer.
///
/// @param model the network
/// @param index the layer
/// @return the broken rule, or nothing
std::optional<Problem> check_embedding(const Model& model, std::size_t index) {
    if (const std::optional<Problem> found = input_is_int16(model, index, 0))
        return found;
    if (const std::optional<Problem> found = weights_are_narrow(model, index))
        return found;
    const Layer& layer = model.layers[index];
    const Tensor& taken = model.tensors[layer.inputs[0]];
    const Tensor& weights = model.tensors[layer.weights];
    const Tensor& result = model.tensors[layer.output];
    const std::string label = layer_label(model, index);
    if (taken.exponent != 0) {
        return reject(
            tensor_field(layer.inputs[0], 34),
            label + ": the input's exponent is " + std::to_string(taken.exponent) +
                ", and must be 0"
        );
    }
    const uint64_t rows = element_count(taken);
    if (weights.rank != 2) {
        return reject(
            tensor_field(layer.weights, 35),
            label + ": the weights' rank is " + std::to_string(weights.rank) + ", and must be 2"
        );
    }
    if (result.type != ElementType::int16) {
        return reject(
            tensor_field(layer.output, 33),
            label + ": the output's type is " + type_name(result.type) + ", and must be int16"
        );
    }
    const uint64_t expect_out[2] = {rows, weights.dims[1]};
    if (result.rank != 2 || result.dims[0] != rows || result.dims[1] != weights.dims[1]) {
        return reject(
            tensor_field(layer.output, 35),
            label + ": the output is " + shape_text(result) + ", and must be " +
                shape_list(expect_out, 2)
        );
    }
    if (result.exponent != weights.exponent) {
        return reject(
            tensor_field(layer.output, 34),
            label + ": the output's exponent is " + std::to_string(result.exponent) +
                ", the weights' is " + std::to_string(weights.exponent)
        );
    }
    return std::nullopt;
}

/// Checks one layer's fields and its row of the kind table.
///
/// @param model the network
/// @param index the layer
/// @return the broken rule, or nothing
std::optional<Problem> check_layer(const Model& model, std::size_t index) {
    const LayerKind kind = model.layers[index].kind;
    const auto raw = static_cast<uint8_t>(kind);
    if (raw < 1 || raw > static_cast<uint8_t>(LayerKind::embedding)) {
        return reject(
            layer_field(model.tensors.size(), index, 0),
            layer_label(model, index) + ": the kind is " + std::to_string(raw) +
                ", and must be one of the thirteen"
        );
    }
    const KindUse use = kind_use(kind);
    if (const std::optional<Problem> found = check_fields(model, index, use))
        return found;
    switch (kind) {
    case LayerKind::dense:
        return check_dense(model, index);
    case LayerKind::conv2d:
        return check_conv(model, index);
    case LayerKind::maxpool:
        return check_pool(model, index);
    case LayerKind::relu:
    case LayerKind::leaky_relu:
        return check_same(model, index, 99);
    case LayerKind::tanh:
    case LayerKind::sigmoid:
    case LayerKind::softmax:
        return check_same(model, index, -15);
    case LayerKind::add:
        return check_add(model, index);
    case LayerKind::concat:
        return check_concat(model, index);
    case LayerKind::flatten:
        return check_flatten(model, index);
    case LayerKind::argmax:
        return check_argmax(model, index);
    case LayerKind::embedding:
        return check_embedding(model, index);
    }
    return std::nullopt;
}

/// One tensor's readers and writer while the graph is checked.
struct Touch {
    bool wrote{};
    std::size_t writer{};
    bool saw{};
};

/// Checks the graph: order, single writers, readers and the port limits.
///
/// @param model the network
/// @return the broken rule, or nothing
std::optional<Problem> check_graph(const Model& model) {
    const std::size_t tensors = model.tensors.size();
    std::vector<Touch> touch(tensors);
    for (std::size_t index = 0; index < model.layers.size(); ++index) {
        const Layer& layer = model.layers[index];
        const std::string label = layer_label(model, index);
        for (uint8_t slot = 0; slot < layer.input_count; ++slot) {
            const uint16_t id = layer.inputs[slot];
            const Tensor& tensor = model.tensors[id];
            if (tensor.role == Role::constant)
                continue;
            if (tensor.role == Role::input) {
                touch[id].saw = true;
                continue;
            }
            if (!touch[id].wrote) {
                return reject(
                    layer_field(tensors, index, 2 + slot * 2U),
                    label + ": reads " + tensor_label(model, id) + ", which no earlier layer writes"
                );
            }
            touch[id].saw = true;
        }
        const uint16_t out = layer.output;
        if (touch[out].wrote) {
            return reject(
                layer_field(tensors, index, 10),
                label + ": writes " + tensor_label(model, out) + ", which " +
                    layer_label(model, touch[out].writer) + " already writes"
            );
        }
        touch[out].wrote = true;
        touch[out].writer = index;
    }

    for (std::size_t index = 0; index < tensors; ++index) {
        const Role role = model.tensors[index].role;
        if ((role == Role::activation || role == Role::output) && !touch[index].wrote) {
            return reject(
                tensor_field(index, 32),
                tensor_label(model, index) + ": an " + std::string(role_name(role)) +
                    " is written by no layer"
            );
        }
    }
    for (std::size_t index = 0; index < tensors; ++index) {
        const Role role = model.tensors[index].role;
        if (touch[index].wrote && (role == Role::input || role == Role::constant)) {
            const char* article = role == Role::input ? "an" : "a";
            return reject(
                layer_field(tensors, touch[index].writer, 10),
                layer_label(model, touch[index].writer) + ": writes " + tensor_label(model, index) +
                    ", which is " + article + " " + role_name(role)
            );
        }
    }
    for (std::size_t index = 0; index < tensors; ++index) {
        const Role role = model.tensors[index].role;
        if ((role == Role::input || role == Role::activation) && !touch[index].saw) {
            return reject(
                tensor_field(index, 32),
                tensor_label(model, index) + ": an " + std::string(role_name(role)) +
                    " is read by no layer"
            );
        }
    }
    for (std::size_t index = 0; index < model.layers.size(); ++index) {
        const Layer& layer = model.layers[index];
        for (uint8_t slot = 0; slot < layer.input_count; ++slot) {
            const uint16_t id = layer.inputs[slot];
            if (model.tensors[id].role == Role::constant) {
                return reject(
                    layer_field(tensors, index, 2 + slot * 2U),
                    layer_label(model, index) + ": reads " + tensor_label(model, id) +
                        " as an input, and a constant is only read as weights or a bias"
                );
            }
        }
    }
    for (std::size_t index = 0; index < tensors; ++index) {
        if (model.tensors[index].role != Role::constant)
            continue;
        bool used = false;
        for (const Layer& layer : model.layers) {
            if (layer.weights == index || layer.bias == index)
                used = true;
        }
        if (!used) {
            return reject(
                tensor_field(index, 32),
                tensor_label(model, index) + ": a constant is used by no layer"
            );
        }
    }

    std::size_t inputs = 0;
    std::size_t outputs = 0;
    for (std::size_t index = 0; index < tensors; ++index) {
        if (model.tensors[index].role == Role::input) {
            ++inputs;
            if (inputs > max_ports) {
                return reject(
                    tensor_field(index, 32),
                    "the model has " + std::to_string(inputs) + " inputs, and the limit is " +
                        std::to_string(max_ports)
                );
            }
        }
        if (model.tensors[index].role == Role::output) {
            ++outputs;
            if (outputs > max_ports) {
                return reject(
                    tensor_field(index, 32),
                    "the model has " + std::to_string(outputs) + " outputs, and the limit is " +
                        std::to_string(max_ports)
                );
            }
        }
    }
    if (inputs < 1) {
        return reject(8, "the model has 0 inputs, and needs 1 to " + std::to_string(max_ports));
    }
    if (outputs < 1) {
        return reject(8, "the model has 0 outputs, and needs 1 to " + std::to_string(max_ports));
    }

    std::vector<uint8_t> counted(tensors, 0);
    uint64_t total = 0;
    for (std::size_t index = 0; index < model.layers.size(); ++index) {
        const Layer& layer = model.layers[index];
        if (layer.kind != LayerKind::dense && layer.kind != LayerKind::conv2d &&
            layer.kind != LayerKind::embedding)
            continue;
        const uint16_t id = layer.weights;
        if (id >= tensors || counted[id] != 0)
            continue;
        counted[id] = 1;
        const uint64_t count = element_count(model.tensors[id]);
        if (count > max_weights || total > max_weights - count) {
            return reject(
                layer_field(tensors, index, 12),
                layer_label(model, index) + ": the weights hold " + std::to_string(total + count) +
                    " elements, and the limit is " + std::to_string(max_weights)
            );
        }
        total += count;
    }
    return std::nullopt;
}

} // namespace

std::optional<Problem> check(const Model& model) {
    if (model.tensors.size() < 2 || model.tensors.size() > max_tensors) {
        const char* noun = model.tensors.size() == 1 ? " tensor" : " tensors";
        return reject(
            8,
            "the model has " + std::to_string(model.tensors.size()) + noun + ", and needs 2 to " +
                std::to_string(max_tensors)
        );
    }
    if (model.layers.empty() || model.layers.size() > max_layers) {
        const char* noun = model.layers.size() == 1 ? " layer" : " layers";
        return reject(
            10,
            "the model has " + std::to_string(model.layers.size()) + noun + ", and needs 1 to " +
                std::to_string(max_layers)
        );
    }
    for (std::size_t index = 0; index < model.tensors.size(); ++index) {
        if (const std::optional<Problem> found = check_tensor(model, index))
            return found;
    }
    const uint64_t bytes = coding::encoded_bytes(model);
    if (bytes > max_file_bytes) {
        return reject(
            12,
            "the network is " + std::to_string(bytes) + " bytes, and the limit is " +
                std::to_string(max_file_bytes)
        );
    }
    for (std::size_t index = 0; index < model.layers.size(); ++index) {
        if (const std::optional<Problem> found = check_layer(model, index))
            return found;
    }
    return check_graph(model);
}

} // namespace oa::formats::oann
