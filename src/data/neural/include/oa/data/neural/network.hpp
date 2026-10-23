// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// A runnable format-1 network. `build` copies a checked model into integer
// buffers. `run` reads the inputs the caller filled and writes every
// activation and output. A run allocates nothing and never throws.

#include "oa/data/neural/arithmetic.hpp"
#include "oa/formats/oann/oann.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::data::neural {

/// One input or output of a built network, in tensor-table order.
struct Port {
    std::string name;               ///< the tensor's name
    int8_t exponent{};              ///< a stored q stands for q times 2^exponent
    uint8_t rank{};                 ///< 1 to 4
    std::array<uint32_t, 4> dims{}; ///< the first rank are the shape; the rest are 0
    uint32_t elements{};            ///< the product of the first rank dimensions
    bool index{};                   ///< true when this output is an argmax index
};

/// Why a run stopped, or a success.
///
/// `failed` is false on success, and then `layer` and `row` are 0.
struct RunError {
    bool failed{};    ///< true when an embedding row was out of range
    uint16_t layer{}; ///< the layer, in table order, starting at 0
    int32_t row{};    ///< the row that was out of range
};

/// One built network.
///
/// The buffers live in this object. One run at a time reads and writes them.
/// Moving the network moves the buffers; a span from `input` or `output` is
/// then no longer usable.
class Network {
  public:

    /// Builds a runnable network from a model.
    ///
    /// The model must pass `formats::oann::check`. A layer kind this build does
    /// not run is refused, and so is a network of more than
    /// `max_operations_per_run` operations. `problem` is set only when nothing
    /// is returned. The charge and the memory are fixed here, from the file.
    ///
    /// @param model the network
    /// @param[out] problem the broken rule, when the network is refused
    /// @return the runnable network, or nothing when a rule fails
    [[nodiscard]] static std::optional<Network>
    build(const formats::oann::Model& model, formats::oann::Problem& problem);

    /// Returns the bytes this network holds.
    ///
    /// Constants count at their own type's size, and every input, activation
    /// and output counts at its type's size. The count comes from the file,
    /// not from an allocator.
    ///
    /// @return the byte count
    [[nodiscard]] std::size_t memory_bytes() const noexcept { return memory_bytes_; }

    /// Returns the operations one run performs.
    ///
    /// The count is fixed by the shapes in the file.
    ///
    /// @return the operation count
    [[nodiscard]] uint64_t operations() const noexcept { return operations_; }

    /// Returns the charge of one run, in units of `operations_per_charge`.
    ///
    /// This is the operation count divided by 64, rounded up. It is fixed
    /// when the network is built.
    ///
    /// @return the charge units
    [[nodiscard]] uint64_t charge_units() const noexcept;

    /// Counts the bytes a checked model would hold, without building it.
    ///
    /// The count is the same one `memory_bytes` returns for a network built
    /// from that model. A caller can refuse a network over its cap before
    /// any buffer is allocated.
    ///
    /// @param model a checked network
    /// @return the byte count
    [[nodiscard]] static std::size_t memory_bytes_of(const formats::oann::Model& model) noexcept;

    /// Returns the input ports, in tensor-table order.
    ///
    /// @return the inputs
    [[nodiscard]] std::span<const Port> inputs() const noexcept;

    /// Returns the output ports, in tensor-table order.
    ///
    /// @return the outputs
    [[nodiscard]] std::span<const Port> outputs() const noexcept;

    /// Returns the input the caller fills before a run.
    ///
    /// The span is empty when `index` is past the inputs. It points into this
    /// network and is not usable after the network is moved.
    ///
    /// @param index the input, in the order `inputs` lists them
    /// @return the int16 elements
    [[nodiscard]] std::span<int16_t> input(std::size_t index) noexcept;

    /// Runs the network on the inputs the caller filled.
    ///
    /// Writes every activation and output. Allocates nothing and throws
    /// nothing. Stops at the first embedding row that is below 0 or not less
    /// than the table's rows, and leaves the later layers unwritten.
    ///
    /// @return success, or the layer and the row that stopped the run
    [[nodiscard]] RunError run() noexcept;

    /// Returns one int16 output.
    ///
    /// The span is empty when `index` is past the outputs or the output is an
    /// argmax index. It points into this network.
    ///
    /// @param index the output, in the order `outputs` lists them
    /// @return the int16 elements, or an empty span for an index output
    [[nodiscard]] std::span<const int16_t> output(std::size_t index) const noexcept;

    /// Returns one argmax output.
    ///
    /// The index is 0-based, the first of equal values. A port that is not an
    /// index output, and an index past the outputs, returns 0.
    ///
    /// @param index the output, in the order `outputs` lists them
    /// @return the chosen element, or 0 when this output is not an index
    [[nodiscard]] int32_t output_index(std::size_t index) const noexcept;

  private:

    /// Where one tensor's elements sit in this network's buffers.
    struct Slot {
        enum class Store : uint8_t { values, index, weights8, weights16, bias };
        Store store{};                  ///< which buffer holds the elements
        uint32_t offset{};              ///< the first element in that buffer
        uint32_t elements{};            ///< how many elements
        int8_t exponent{};              ///< the tensor's exponent
        uint8_t rank{};                 ///< 1 to 4
        std::array<uint32_t, 4> dims{}; ///< the shape
    };

    /// One layer, with the tensors it reads and writes.
    struct Step {
        formats::oann::LayerKind kind{};  ///< which kernel to run
        uint16_t layer{};                 ///< the layer's place in the table
        int16_t alpha{};                  ///< leaky_relu slope in 32768ths
        uint8_t axis{};                   ///< concat axis
        uint8_t input_count{};            ///< how many of `inputs` are used
        std::array<uint16_t, 4> inputs{}; ///< tensor indexes
        uint16_t output{};                ///< tensor this layer writes
        uint16_t weights{};               ///< a constant tensor, when `has_weights`
        uint16_t bias{};                  ///< a constant tensor, when `has_bias`
        bool has_weights{};               ///< the layer has a weight tensor
        bool has_bias{};                  ///< the layer has a bias tensor
    };

    /// Copies constants and reserves every input, activation and output.
    ///
    /// @param model a model that has passed every check this build makes
    void lay_out(const formats::oann::Model& model);

    std::vector<Port> input_ports_;
    std::vector<Port> output_ports_;
    std::vector<uint16_t> input_tensors_;
    std::vector<uint16_t> output_tensors_;
    std::vector<Slot> slots_;
    std::vector<Step> steps_;
    std::vector<int16_t> values_;
    std::vector<int32_t> indexes_;
    std::vector<int8_t> weights8_;
    std::vector<int16_t> weights16_;
    std::vector<int32_t> biases_;
    std::size_t memory_bytes_{};
    uint64_t operations_{};
};

/// The networks one brain holds, kept under a byte cap.
class NetworkSet {
  public:

    /// Starts an empty set.
    ///
    /// @param cap the most bytes the set may hold; the default is the per-brain cap
    explicit NetworkSet(std::size_t cap = max_brain_network_bytes) noexcept;

    /// Holds a network under an id.
    ///
    /// Refuses an id the set already holds, and a network that would take the
    /// set past its cap. The network is kept only when this returns true.
    /// `problem` is set only when it returns false.
    ///
    /// @param id the name the caller looks the network up by
    /// @param network the built network
    /// @param[out] problem the broken rule, when the network is refused
    /// @return true when the set now holds the network
    bool add(std::string_view id, Network network, formats::oann::Problem& problem);

    /// Finds a network by the id it was added under.
    ///
    /// The pointer stays valid until the set is destroyed. It is null when
    /// the id is not held.
    ///
    /// @param id the name passed to `add`
    /// @return the network, or null
    [[nodiscard]] Network* find(std::string_view id) noexcept;

    /// Returns the bytes of every network the set holds.
    ///
    /// @return the sum of each network's `memory_bytes`
    [[nodiscard]] std::size_t memory_bytes() const noexcept { return used_; }

  private:

    struct Held {
        std::string id;
        Network network;
    };

    std::deque<Held> held_;
    std::size_t cap_{};
    std::size_t used_{};
};

} // namespace oa::data::neural
