// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Builds a runnable network from a checked model and runs it. Constants are
// converted from the file's little-endian bytes once, here. A run only reads
// and writes the buffers build made.

#include "oa/data/neural/network.hpp"

#include "kernels.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace oa::data::neural {

using formats::oann::ElementType;
using formats::oann::Layer;
using formats::oann::LayerKind;
using formats::oann::Model;
using formats::oann::Problem;
using formats::oann::Role;
using formats::oann::Tensor;
using formats::oann::check;
using formats::oann::element_bytes;
using formats::oann::element_count;
using formats::oann::no_tensor;

namespace {

/// Multiplies, or returns the largest uint64 when the product does not fit.
///
/// @param left the first factor
/// @param right the second factor
/// @return the product, or the largest uint64 on overflow
uint64_t saturating_mul(uint64_t left, uint64_t right) noexcept {
    if (left != 0 && right > ~uint64_t{0} / left)
        return ~uint64_t{0};
    return left * right;
}

/// Adds, or returns the largest uint64 when the sum does not fit.
///
/// @param left the first term
/// @param right the second term
/// @return the sum, or the largest uint64 on overflow
uint64_t saturating_add(uint64_t left, uint64_t right) noexcept {
    if (left > ~uint64_t{0} - right)
        return ~uint64_t{0};
    return left + right;
}

/// Counts the operations one layer performs.
///
/// The thirteen kinds are all counted here, and each one has a kernel.
///
/// @param model the network
/// @param layer the layer
/// @return the operation count, or the largest uint64 when it does not fit
uint64_t operations_for(const Model& model, const Layer& layer) noexcept {
    switch (layer.kind) {
    case LayerKind::dense: {
        const Tensor& weights = model.tensors[layer.weights];
        return saturating_mul(weights.dims[0], weights.dims[1]);
    }
    case LayerKind::conv2d: {
        const Tensor& taken = model.tensors[layer.inputs[0]];
        const Tensor& result = model.tensors[layer.output];
        uint64_t count = result.dims[0];
        count = saturating_mul(count, result.dims[1]);
        count = saturating_mul(count, result.dims[2]);
        count = saturating_mul(count, taken.dims[0]);
        count = saturating_mul(count, layer.kernel_h);
        return saturating_mul(count, layer.kernel_w);
    }
    case LayerKind::maxpool: {
        uint64_t count = element_count(model.tensors[layer.output]);
        count = saturating_mul(count, layer.kernel_h);
        return saturating_mul(count, layer.kernel_w);
    }
    case LayerKind::softmax:
        return saturating_mul(element_count(model.tensors[layer.output]), 4);
    case LayerKind::argmax:
        return element_count(model.tensors[layer.inputs[0]]);
    case LayerKind::relu:
    case LayerKind::leaky_relu:
    case LayerKind::tanh:
    case LayerKind::sigmoid:
    case LayerKind::add:
    case LayerKind::concat:
    case LayerKind::flatten:
    case LayerKind::embedding:
        return element_count(model.tensors[layer.output]);
    }
    return 0;
}

/// Copies a tensor's description into a port.
///
/// @param tensor the tensor
/// @param index true when the port is an argmax output
/// @return the port
Port port_from(const Tensor& tensor, bool index) {
    Port port;
    port.name = tensor.name;
    port.exponent = tensor.exponent;
    port.rank = tensor.rank;
    port.dims = tensor.dims;
    port.elements = static_cast<uint32_t>(element_count(tensor));
    port.index = index;
    return port;
}

/// Appends little-endian int8 elements.
///
/// @param out the native array
/// @param data the file bytes
/// @param count how many elements
void append_i8(std::vector<int8_t>& out, const std::vector<uint8_t>& data, uint32_t count) {
    for (uint32_t element = 0; element < count; ++element) {
        const uint8_t byte = data[element];
        if (byte < 128)
            out.push_back(static_cast<int8_t>(byte));
        else
            out.push_back(static_cast<int8_t>(static_cast<int16_t>(byte) - 256));
    }
}

/// Appends little-endian int16 elements.
///
/// @param out the native array
/// @param data the file bytes
/// @param count how many elements
void append_i16(std::vector<int16_t>& out, const std::vector<uint8_t>& data, uint32_t count) {
    for (uint32_t element = 0; element < count; ++element) {
        const std::size_t at = static_cast<std::size_t>(element) * 2U;
        const uint32_t word = uint32_t{data[at]} | (uint32_t{data[at + 1]} << 8U);
        if (word < 32768U)
            out.push_back(static_cast<int16_t>(word));
        else
            out.push_back(static_cast<int16_t>(static_cast<int32_t>(word) - 65536));
    }
}

/// Appends little-endian int32 elements.
///
/// @param out the native array
/// @param data the file bytes
/// @param count how many elements
void append_i32(std::vector<int32_t>& out, const std::vector<uint8_t>& data, uint32_t count) {
    for (uint32_t element = 0; element < count; ++element) {
        const std::size_t at = static_cast<std::size_t>(element) * 4U;
        const uint32_t bits = uint32_t{data[at]} | (uint32_t{data[at + 1]} << 8U) |
                              (uint32_t{data[at + 2]} << 16U) | (uint32_t{data[at + 3]} << 24U);
        if (bits < 0x80000000U)
            out.push_back(static_cast<int32_t>(bits));
        else
            out.push_back(static_cast<int32_t>(static_cast<int64_t>(bits) - 4294967296LL));
    }
}

} // namespace

std::optional<Network> Network::build(const Model& model, Problem& problem) {
    if (const std::optional<Problem> found = check(model)) {
        problem = *found;
        return std::nullopt;
    }

    uint64_t operations = 0;
    for (std::size_t layer_index = 0; layer_index < model.layers.size(); ++layer_index) {
        const Layer& layer = model.layers[layer_index];
        operations = saturating_add(operations, operations_for(model, layer));
    }
    if (operations > max_operations_per_run) {
        problem.message = "a run would take " + std::to_string(operations) +
                          " operations; a run may take " + std::to_string(max_operations_per_run);
        problem.offset = 0;
        return std::nullopt;
    }

    Network network;
    network.operations_ = operations;
    network.memory_bytes_ = memory_bytes_of(model);
    network.lay_out(model);
    return network;
}

std::size_t Network::memory_bytes_of(const Model& model) noexcept {
    uint64_t total = 0;
    for (const Tensor& tensor : model.tensors) {
        const uint64_t count = element_count(tensor);
        const uint64_t width = element_bytes(tensor.type);
        if (width == 0 || (count != 0 && width > ~uint64_t{0} / count))
            return std::numeric_limits<std::size_t>::max();
        const uint64_t bytes = count * width;
        if (total > ~uint64_t{0} - bytes)
            return std::numeric_limits<std::size_t>::max();
        total += bytes;
    }
    if (total > std::numeric_limits<std::size_t>::max())
        return std::numeric_limits<std::size_t>::max();
    return static_cast<std::size_t>(total);
}

uint64_t Network::charge_units() const noexcept {
    const uint64_t unit = operations_per_charge;
    return (operations_ + (unit - 1)) / unit;
}

std::span<const Port> Network::inputs() const noexcept {
    return std::span<const Port>(input_ports_.data(), input_ports_.size());
}

std::span<const Port> Network::outputs() const noexcept {
    return std::span<const Port>(output_ports_.data(), output_ports_.size());
}

std::span<int16_t> Network::input(std::size_t index) noexcept {
    if (index >= input_tensors_.size())
        return {};
    const Slot& slot = slots_[input_tensors_[index]];
    return std::span<int16_t>(values_.data() + slot.offset, slot.elements);
}

std::span<const int16_t> Network::output(std::size_t index) const noexcept {
    if (index >= output_tensors_.size())
        return {};
    const Slot& slot = slots_[output_tensors_[index]];
    if (slot.store != Slot::Store::values)
        return {};
    return std::span<const int16_t>(values_.data() + slot.offset, slot.elements);
}

int32_t Network::output_index(std::size_t index) const noexcept {
    if (index >= output_tensors_.size())
        return 0;
    const Slot& slot = slots_[output_tensors_[index]];
    if (slot.store != Slot::Store::index)
        return 0;
    return indexes_[slot.offset];
}

void Network::lay_out(const Model& model) {
    slots_.resize(model.tensors.size());
    for (std::size_t tensor_index = 0; tensor_index < model.tensors.size(); ++tensor_index) {
        const Tensor& tensor = model.tensors[tensor_index];
        Slot& slot = slots_[tensor_index];
        slot.exponent = tensor.exponent;
        slot.rank = tensor.rank;
        slot.dims = tensor.dims;
        slot.elements = static_cast<uint32_t>(element_count(tensor));
        const bool index_output = tensor.role == Role::output && tensor.type == ElementType::int32;
        if (tensor.role == Role::input) {
            input_ports_.push_back(port_from(tensor, false));
            input_tensors_.push_back(static_cast<uint16_t>(tensor_index));
        } else if (tensor.role == Role::output) {
            output_ports_.push_back(port_from(tensor, index_output));
            output_tensors_.push_back(static_cast<uint16_t>(tensor_index));
        }

        if (tensor.role == Role::constant) {
            if (tensor.type == ElementType::int8) {
                slot.store = Slot::Store::weights8;
                slot.offset = static_cast<uint32_t>(weights8_.size());
                append_i8(weights8_, tensor.data, slot.elements);
            } else if (tensor.type == ElementType::int16) {
                slot.store = Slot::Store::weights16;
                slot.offset = static_cast<uint32_t>(weights16_.size());
                append_i16(weights16_, tensor.data, slot.elements);
            } else {
                slot.store = Slot::Store::bias;
                slot.offset = static_cast<uint32_t>(biases_.size());
                append_i32(biases_, tensor.data, slot.elements);
            }
            continue;
        }
        if (index_output) {
            slot.store = Slot::Store::index;
            slot.offset = static_cast<uint32_t>(indexes_.size());
            indexes_.push_back(0);
            continue;
        }
        slot.store = Slot::Store::values;
        slot.offset = static_cast<uint32_t>(values_.size());
        values_.insert(values_.end(), slot.elements, int16_t{0});
    }

    steps_.reserve(model.layers.size());
    for (std::size_t layer_index = 0; layer_index < model.layers.size(); ++layer_index) {
        const Layer& layer = model.layers[layer_index];
        Step step;
        step.kind = layer.kind;
        step.layer = static_cast<uint16_t>(layer_index);
        step.alpha = layer.alpha;
        step.axis = layer.axis;
        step.kernel_h = layer.kernel_h;
        step.kernel_w = layer.kernel_w;
        step.stride_h = layer.stride_h;
        step.stride_w = layer.stride_w;
        step.pad_h = layer.pad_h;
        step.pad_w = layer.pad_w;
        step.input_count = layer.input_count;
        step.inputs = layer.inputs;
        step.output = layer.output;
        step.weights = layer.weights;
        step.bias = layer.bias;
        step.has_weights = layer.weights != no_tensor;
        step.has_bias = layer.bias != no_tensor;
        steps_.push_back(step);
    }
}

RunError Network::run() noexcept {
    for (const Step& step : steps_) {
        switch (step.kind) {
        case LayerKind::dense: {
            const Slot& taken = slots_[step.inputs[0]];
            const Slot& made = slots_[step.output];
            const Slot& weight_slot = slots_[step.weights];
            std::span<const int32_t> bias;
            if (step.has_bias) {
                const Slot& bias_slot = slots_[step.bias];
                bias =
                    std::span<const int32_t>(biases_.data() + bias_slot.offset, bias_slot.elements);
            }
            const std::span<const int16_t> samples(values_.data() + taken.offset, taken.elements);
            const std::span<int16_t> written(values_.data() + made.offset, made.elements);
            if (weight_slot.store == Slot::Store::weights8) {
                dense<int8_t>(
                    samples,
                    std::span<const int8_t>(
                        weights8_.data() + weight_slot.offset, weight_slot.elements
                    ),
                    bias,
                    written,
                    taken.exponent,
                    weight_slot.exponent,
                    made.exponent
                );
            } else {
                dense<int16_t>(
                    samples,
                    std::span<const int16_t>(
                        weights16_.data() + weight_slot.offset, weight_slot.elements
                    ),
                    bias,
                    written,
                    taken.exponent,
                    weight_slot.exponent,
                    made.exponent
                );
            }
            break;
        }
        case LayerKind::relu: {
            const Slot& taken = slots_[step.inputs[0]];
            const Slot& made = slots_[step.output];
            relu(
                std::span<const int16_t>(values_.data() + taken.offset, taken.elements),
                std::span<int16_t>(values_.data() + made.offset, made.elements)
            );
            break;
        }
        case LayerKind::leaky_relu: {
            const Slot& taken = slots_[step.inputs[0]];
            const Slot& made = slots_[step.output];
            leaky_relu(
                std::span<const int16_t>(values_.data() + taken.offset, taken.elements),
                std::span<int16_t>(values_.data() + made.offset, made.elements),
                static_cast<int32_t>(step.alpha)
            );
            break;
        }
        case LayerKind::add: {
            const Slot& first = slots_[step.inputs[0]];
            const Slot& second = slots_[step.inputs[1]];
            const Slot& made = slots_[step.output];
            add(std::span<const int16_t>(values_.data() + first.offset, first.elements),
                first.exponent,
                std::span<const int16_t>(values_.data() + second.offset, second.elements),
                second.exponent,
                std::span<int16_t>(values_.data() + made.offset, made.elements),
                made.exponent);
            break;
        }
        case LayerKind::concat: {
            std::array<ConcatInput, 4> parts{};
            for (uint8_t slot = 0; slot < step.input_count; ++slot) {
                const Slot& taken = slots_[step.inputs[slot]];
                parts[slot].values = values_.data() + taken.offset;
                parts[slot].exponent = taken.exponent;
                parts[slot].along = taken.dims[step.axis];
            }
            const Slot& made = slots_[step.output];
            concat(
                std::span<const ConcatInput>(parts.data(), step.input_count),
                made.rank,
                step.axis,
                made.dims,
                made.exponent,
                std::span<int16_t>(values_.data() + made.offset, made.elements)
            );
            break;
        }
        case LayerKind::flatten: {
            const Slot& taken = slots_[step.inputs[0]];
            const Slot& made = slots_[step.output];
            flatten(
                std::span<const int16_t>(values_.data() + taken.offset, taken.elements),
                std::span<int16_t>(values_.data() + made.offset, made.elements)
            );
            break;
        }
        case LayerKind::argmax: {
            const Slot& taken = slots_[step.inputs[0]];
            const Slot& made = slots_[step.output];
            argmax(
                std::span<const int16_t>(values_.data() + taken.offset, taken.elements),
                indexes_[made.offset]
            );
            break;
        }
        case LayerKind::embedding: {
            const Slot& taken = slots_[step.inputs[0]];
            const Slot& weight_slot = slots_[step.weights];
            const Slot& made = slots_[step.output];
            int32_t bad_row = 0;
            const std::span<const int16_t> samples(values_.data() + taken.offset, taken.elements);
            const std::span<int16_t> written(values_.data() + made.offset, made.elements);
            bool accepted = false;
            if (weight_slot.store == Slot::Store::weights8) {
                accepted = embedding<int8_t>(
                    samples,
                    std::span<const int8_t>(
                        weights8_.data() + weight_slot.offset, weight_slot.elements
                    ),
                    weight_slot.dims[0],
                    weight_slot.dims[1],
                    written,
                    bad_row
                );
            } else {
                accepted = embedding<int16_t>(
                    samples,
                    std::span<const int16_t>(
                        weights16_.data() + weight_slot.offset, weight_slot.elements
                    ),
                    weight_slot.dims[0],
                    weight_slot.dims[1],
                    written,
                    bad_row
                );
            }
            if (!accepted)
                return RunError{true, step.layer, bad_row};
            break;
        }
        case LayerKind::conv2d: {
            const Slot& taken = slots_[step.inputs[0]];
            const Slot& made = slots_[step.output];
            const Slot& weight_slot = slots_[step.weights];
            std::span<const int32_t> bias;
            if (step.has_bias) {
                const Slot& bias_slot = slots_[step.bias];
                bias =
                    std::span<const int32_t>(biases_.data() + bias_slot.offset, bias_slot.elements);
            }
            const std::span<const int16_t> samples(values_.data() + taken.offset, taken.elements);
            const std::span<int16_t> written(values_.data() + made.offset, made.elements);
            if (weight_slot.store == Slot::Store::weights8) {
                conv2d<int8_t>(
                    samples,
                    taken.dims,
                    std::span<const int8_t>(
                        weights8_.data() + weight_slot.offset, weight_slot.elements
                    ),
                    bias,
                    written,
                    made.dims,
                    step.kernel_h,
                    step.kernel_w,
                    step.stride_h,
                    step.stride_w,
                    step.pad_h,
                    step.pad_w,
                    taken.exponent,
                    weight_slot.exponent,
                    made.exponent
                );
            } else {
                conv2d<int16_t>(
                    samples,
                    taken.dims,
                    std::span<const int16_t>(
                        weights16_.data() + weight_slot.offset, weight_slot.elements
                    ),
                    bias,
                    written,
                    made.dims,
                    step.kernel_h,
                    step.kernel_w,
                    step.stride_h,
                    step.stride_w,
                    step.pad_h,
                    step.pad_w,
                    taken.exponent,
                    weight_slot.exponent,
                    made.exponent
                );
            }
            break;
        }
        case LayerKind::maxpool: {
            const Slot& taken = slots_[step.inputs[0]];
            const Slot& made = slots_[step.output];
            maxpool(
                std::span<const int16_t>(values_.data() + taken.offset, taken.elements),
                taken.dims,
                std::span<int16_t>(values_.data() + made.offset, made.elements),
                made.dims,
                step.kernel_h,
                step.kernel_w,
                step.stride_h,
                step.stride_w
            );
            break;
        }
        case LayerKind::tanh: {
            const Slot& taken = slots_[step.inputs[0]];
            const Slot& made = slots_[step.output];
            tanh(
                std::span<const int16_t>(values_.data() + taken.offset, taken.elements),
                std::span<int16_t>(values_.data() + made.offset, made.elements),
                taken.exponent
            );
            break;
        }
        case LayerKind::sigmoid: {
            const Slot& taken = slots_[step.inputs[0]];
            const Slot& made = slots_[step.output];
            sigmoid(
                std::span<const int16_t>(values_.data() + taken.offset, taken.elements),
                std::span<int16_t>(values_.data() + made.offset, made.elements),
                taken.exponent
            );
            break;
        }
        case LayerKind::softmax: {
            const Slot& taken = slots_[step.inputs[0]];
            const Slot& made = slots_[step.output];
            softmax(
                std::span<const int16_t>(values_.data() + taken.offset, taken.elements),
                std::span<int16_t>(values_.data() + made.offset, made.elements),
                taken.exponent
            );
            break;
        }
        }
    }
    return {};
}

NetworkSet::NetworkSet(std::size_t cap) noexcept : cap_{cap} {
}

bool NetworkSet::add(std::string_view id, Network network, Problem& problem) {
    for (const Held& held : held_) {
        if (held.id == id) {
            problem.message = "network " + std::string(id) + " is already held";
            problem.offset = 0;
            return false;
        }
    }
    const std::size_t bytes = network.memory_bytes();
    if (used_ > cap_ || bytes > cap_ - used_) {
        const uint64_t would = uint64_t{used_} + uint64_t{bytes};
        problem.message = "networks would take " + std::to_string(would) +
                          " bytes; a brain may hold " + std::to_string(cap_);
        problem.offset = 0;
        return false;
    }
    used_ += bytes;
    held_.push_back(Held{std::string(id), std::move(network)});
    return true;
}

Network* NetworkSet::find(std::string_view id) noexcept {
    for (Held& held : held_) {
        if (held.id == id)
            return &held.network;
    }
    return nullptr;
}

} // namespace oa::data::neural
