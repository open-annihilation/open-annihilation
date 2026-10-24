// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// brain.network, brain.tensor and the tensor methods. Nothing here reads the
// seat's match. A network handle points at a network the load already built.

#include "lua_networks.hpp"

#include "handles_internal.hpp"
#include "internal.hpp"

#include "oa/data/neural/arithmetic.hpp"
#include "oa/data/neural/network.hpp"
#include "oa/data/script_host/fixed.hpp"
#include "oa/data/script_host/script_host.hpp"
#include "oa/sim/brains/seat.hpp"

#include "lua.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>

namespace oa::sim::brains {
namespace {

constexpr const char* tensor_meta_name = "oa.brain.tensor";
constexpr const char* network_meta_name = "oa.brain.network";
constexpr uint32_t max_elements = 1u << 22;
constexpr int64_t count_min = int64_t{-2147483647} - 1;
constexpr int64_t count_max = 2147483646;
constexpr lua_Integer whole_min = -32768;
constexpr lua_Integer whole_max = 32767;
constexpr int max_ports = 16;

char network_cache_token;

struct TensorHead {
    uint32_t count{};
    uint8_t rank{};
    uint8_t pad[3]{};
    uint32_t dims[4]{};
};

static_assert(sizeof(TensorHead) == 24, "a tensor head is 24 bytes, then its elements");

struct NetworkBody {
    oa::data::neural::Network* network{};
    char id[id_bytes + 1]{};
};

struct PortCopy {
    const char* name{};
    std::size_t name_length{};
    int8_t exponent{};
    uint8_t rank{};
    uint32_t dims[4]{};
    uint32_t elements{};
    bool index{};
};

int32_t* tensor_elements(TensorHead* head) {
    return reinterpret_cast<int32_t*>(reinterpret_cast<unsigned char*>(head) + sizeof(TensorHead));
}

TensorHead* tensor_at(lua_State* state, int index) {
    if (lua_type(state, index) != LUA_TUSERDATA)
        return nullptr;
    if (lua_getmetatable(state, index) == 0)
        return nullptr;
    luaL_getmetatable(state, tensor_meta_name);
    const int same = lua_rawequal(state, -1, -2);
    lua_pop(state, 2);
    if (same == 0)
        return nullptr;
    return static_cast<TensorHead*>(lua_touserdata(state, index));
}

NetworkBody* network_at(lua_State* state, int index) {
    if (lua_type(state, index) != LUA_TUSERDATA)
        return nullptr;
    if (lua_getmetatable(state, index) == 0)
        return nullptr;
    luaL_getmetatable(state, network_meta_name);
    const int same = lua_rawequal(state, -1, -2);
    lua_pop(state, 2);
    if (same == 0)
        return nullptr;
    return static_cast<NetworkBody*>(lua_touserdata(state, index));
}

TensorHead* need_tensor(lua_State* state, const char* method) {
    TensorHead* head = tensor_at(state, 1);
    if (head == nullptr) {
        luaL_error(state, "brain.tensor: %s expects a tensor", method);
        return nullptr;
    }
    return head;
}

NetworkBody* need_network(lua_State* state) {
    NetworkBody* body = network_at(state, 1);
    if (body == nullptr || body->network == nullptr) {
        luaL_error(state, "run expects a network");
        return nullptr;
    }
    return body;
}

uint64_t groups_of(uint64_t count) {
    return (count + 63u) / 64u;
}

/// Lua's formatter accepts `%I` for a `lua_Integer`, and nothing wider.
lua_Integer lua_count(uint64_t value) {
    const auto limit = static_cast<uint64_t>(std::numeric_limits<lua_Integer>::max());
    if (value > limit)
        return std::numeric_limits<lua_Integer>::max();
    return static_cast<lua_Integer>(value);
}

/// Reads one value as a count of 65536ths. Raises on a bad value.
int32_t read_count(lua_State* state, int index, int which) {
    int64_t scaled = 0;
    if (script_host::to_fixed(state, index, &scaled)) {
        if (scaled < count_min || scaled > count_max) {
            luaL_error(state, "brain.tensor: value %d is outside -32768 to 32767", which);
            return 0;
        }
        return static_cast<int32_t>(scaled);
    }
    if (lua_isinteger(state, index) != 0) {
        const lua_Integer whole = lua_tointeger(state, index);
        if (whole < whole_min || whole > whole_max) {
            luaL_error(state, "brain.tensor: value %d is outside -32768 to 32767", which);
            return 0;
        }
        scaled = static_cast<int64_t>(whole) * int64_t{65536};
        return static_cast<int32_t>(scaled);
    }
    if (lua_type(state, index) != LUA_TNUMBER) {
        luaL_error(state, "brain.tensor: value %d is not a number", which);
        return 0;
    }
    const double number = lua_tonumber(state, index);
    // A non-finite value makes this difference a non-number as well.
    if (number != number || (number - number) != 0.0) {
        luaL_error(state, "brain.tensor: value %d is not a finite number", which);
        return 0;
    }
    if (number < -32768.0 || number > 32767.99998) {
        luaL_error(state, "brain.tensor: value %d is outside -32768 to 32767", which);
        return 0;
    }
    scaled = static_cast<int64_t>(number * 65536.0);
    if (scaled < count_min || scaled > count_max) {
        luaL_error(state, "brain.tensor: value %d is outside -32768 to 32767", which);
        return 0;
    }
    return static_cast<int32_t>(scaled);
}

void copy_ports(
    oa::data::neural::Network* network, bool outputs, PortCopy* dest, std::size_t* count
) {
    const oa::data::neural::Port* ports = nullptr;
    std::size_t found = 0;
    if (outputs) {
        const auto listed = network->outputs();
        ports = listed.data();
        found = listed.size();
    } else {
        const auto listed = network->inputs();
        ports = listed.data();
        found = listed.size();
    }
    if (found > static_cast<std::size_t>(max_ports))
        found = static_cast<std::size_t>(max_ports);
    for (std::size_t index = 0; index < found; ++index) {
        const oa::data::neural::Port& port = ports[index];
        dest[index].name = port.name.data();
        dest[index].name_length = port.name.size();
        dest[index].exponent = port.exponent;
        dest[index].rank = port.rank;
        dest[index].dims[0] = port.dims[0];
        dest[index].dims[1] = port.dims[1];
        dest[index].dims[2] = port.dims[2];
        dest[index].dims[3] = port.dims[3];
        dest[index].elements = port.elements;
        dest[index].index = port.index;
    }
    *count = found;
}

uint64_t element_total(const PortCopy* ports, std::size_t count) {
    uint64_t total = 0;
    for (std::size_t index = 0; index < count; ++index)
        total += ports[index].elements;
    return total;
}

bool port_named(const PortCopy* ports, std::size_t count, const char* text, std::size_t length) {
    for (std::size_t index = 0; index < count; ++index) {
        if (ports[index].name_length == length &&
            (length == 0 || std::memcmp(ports[index].name, text, length) == 0))
            return true;
    }
    return false;
}

extern "C" {

static int tensor_get(lua_State* state) {
    TensorHead* head = need_tensor(state, "get");
    if (head == nullptr)
        return 0;
    lua_Integer index = 0;
    if (lua_isinteger(state, 2) != 0)
        index = lua_tointeger(state, 2);
    if (index < 1 || static_cast<uint64_t>(index) > head->count) {
        luaL_error(
            state,
            "brain.tensor: index %I is outside 1 to %I",
            index,
            static_cast<lua_Integer>(head->count)
        );
        return 0;
    }
    script_host::charge_work(state, script_host::element_charge);
    const int32_t* elements = tensor_elements(head);
    script_host::push_fixed(state, elements[static_cast<uint32_t>(index) - 1u]);
    return 1;
}

static int tensor_argmax(lua_State* state) {
    TensorHead* head = need_tensor(state, "argmax");
    if (head == nullptr)
        return 0;
    script_host::charge_work(state, groups_of(head->count));
    const int32_t* elements = tensor_elements(head);
    int32_t best = elements[0];
    uint32_t at = 0;
    for (uint32_t index = 1; index < head->count; ++index) {
        if (elements[index] > best) {
            best = elements[index];
            at = index;
        }
    }
    lua_pushinteger(state, static_cast<lua_Integer>(at) + 1);
    return 1;
}

static int tensor_totable(lua_State* state) {
    TensorHead* head = need_tensor(state, "totable");
    if (head == nullptr)
        return 0;
    script_host::charge_work(
        state, script_host::element_charge * static_cast<uint64_t>(head->count)
    );
    const int32_t* elements = tensor_elements(head);
    lua_createtable(state, static_cast<int>(head->count), 0);
    for (uint32_t index = 0; index < head->count; ++index) {
        script_host::push_fixed(state, elements[index]);
        lua_rawseti(state, -2, static_cast<lua_Integer>(index) + 1);
    }
    return 1;
}

static int tensor_size(lua_State* state) {
    TensorHead* head = need_tensor(state, "size");
    if (head == nullptr)
        return 0;
    script_host::charge_work(state, script_host::element_charge);
    lua_pushinteger(state, static_cast<lua_Integer>(head->count));
    return 1;
}

static int tensor_shape(lua_State* state) {
    TensorHead* head = need_tensor(state, "shape");
    if (head == nullptr)
        return 0;
    script_host::charge_work(state, script_host::element_charge * head->rank);
    lua_createtable(state, head->rank, 0);
    for (uint8_t axis = 0; axis < head->rank; ++axis) {
        lua_pushinteger(state, static_cast<lua_Integer>(head->dims[axis]));
        lua_rawseti(state, -2, static_cast<lua_Integer>(axis) + 1);
    }
    return 1;
}

static int tensor_len(lua_State* state) {
    return tensor_size(state);
}

static int tensor_tostring(lua_State* state) {
    TensorHead* head = need_tensor(state, "tostring");
    if (head == nullptr)
        return 0;
    script_host::charge_work(state, script_host::element_charge);
    char text[80];
    int used = std::snprintf(text, sizeof text, "tensor: %u", head->dims[0]);
    for (uint8_t axis = 1; axis < head->rank && used > 0 && used < static_cast<int>(sizeof text);
         ++axis) {
        const int more = std::snprintf(
            text + used, sizeof text - static_cast<std::size_t>(used), "x%u", head->dims[axis]
        );
        if (more < 0)
            break;
        used += more;
    }
    lua_pushstring(state, text);
    return 1;
}

static int tensor_newindex(lua_State* state) {
    luaL_error(state, "a tensor cannot be changed");
    return 0;
}

static int network_newindex(lua_State* state) {
    luaL_error(state, "a network cannot be changed");
    return 0;
}

static int brain_tensor(lua_State* state) {
    if (lua_type(state, 1) != LUA_TTABLE) {
        luaL_error(state, "brain.tensor: expects 1 to 4194304 numbers");
        return 0;
    }
    const lua_Unsigned raw_count = lua_rawlen(state, 1);
    if (raw_count < 1 || raw_count > max_elements) {
        luaL_error(state, "brain.tensor: expects 1 to 4194304 numbers");
        return 0;
    }
    const auto count = static_cast<uint32_t>(raw_count);
    for (uint32_t index = 1; index <= count; ++index) {
        lua_rawgeti(state, 1, static_cast<lua_Integer>(index));
        (void)read_count(state, -1, static_cast<int>(index));
        lua_pop(state, 1);
    }

    uint32_t dims[4] = {count, 0, 0, 0};
    uint8_t rank = 1;
    if (lua_gettop(state) >= 2 && lua_isnil(state, 2) == 0) {
        if (lua_type(state, 2) != LUA_TTABLE) {
            luaL_error(state, "brain.tensor: the shape must be 1 to 4 positive integers");
            return 0;
        }
        const lua_Unsigned raw_rank = lua_rawlen(state, 2);
        if (raw_rank < 1 || raw_rank > 4) {
            luaL_error(state, "brain.tensor: the shape must be 1 to 4 positive integers");
            return 0;
        }
        rank = static_cast<uint8_t>(raw_rank);
        uint64_t product = 1;
        for (uint8_t axis = 0; axis < rank; ++axis) {
            lua_rawgeti(state, 2, static_cast<lua_Integer>(axis) + 1);
            if (lua_isinteger(state, -1) == 0) {
                luaL_error(state, "brain.tensor: the shape must be 1 to 4 positive integers");
                return 0;
            }
            const lua_Integer dim = lua_tointeger(state, -1);
            lua_pop(state, 1);
            if (dim < 1) {
                luaL_error(state, "brain.tensor: the shape must be 1 to 4 positive integers");
                return 0;
            }
            const auto wide = static_cast<uint64_t>(dim);
            if (wide > ~uint64_t{0} / product) {
                luaL_error(
                    state,
                    "brain.tensor: the shape covers %I values, not %I",
                    lua_count(~uint64_t{0}),
                    lua_count(count)
                );
                return 0;
            }
            product *= wide;
            dims[axis] = static_cast<uint32_t>(wide > 0xffffffffu ? 0xffffffffu : wide);
        }
        if (product != count) {
            luaL_error(
                state,
                "brain.tensor: the shape covers %I values, not %I",
                lua_count(product),
                lua_count(count)
            );
            return 0;
        }
    }

    const uint64_t units = script_host::element_charge * (static_cast<uint64_t>(count) + rank);
    script_host::charge_work(state, units);
    int32_t* elements = push_zero_tensor(state, rank, dims);
    if (elements == nullptr)
        return 0;
    for (uint32_t index = 1; index <= count; ++index) {
        lua_rawgeti(state, 1, static_cast<lua_Integer>(index));
        elements[index - 1u] = read_count(state, -1, static_cast<int>(index));
        lua_pop(state, 1);
    }
    return 1;
}

static int brain_network(lua_State* state) {
    const char* text = "";
    std::size_t length = 0;
    if (lua_type(state, 1) == LUA_TSTRING)
        text = lua_tolstring(state, 1, &length);
    Seat* seat = seat_of(state);
    oa::data::neural::Network* network = nullptr;
    if (seat != nullptr && seat->networks != nullptr && text != nullptr)
        network = seat->networks->find(std::string_view(text, length));
    if (network == nullptr) {
        luaL_error(
            state, "brain.network: this brain has no network '%s'", text != nullptr ? text : ""
        );
        return 0;
    }
    script_host::charge_work(state, script_host::element_charge);

    lua_rawgetp(state, LUA_REGISTRYINDEX, &network_cache_token);
    if (lua_type(state, -1) != LUA_TTABLE) {
        lua_pop(state, 1);
        lua_newtable(state);
        lua_pushvalue(state, -1);
        lua_rawsetp(state, LUA_REGISTRYINDEX, &network_cache_token);
    }
    const int cache = lua_gettop(state);
    lua_rawgetp(state, cache, network);
    if (lua_type(state, -1) == LUA_TUSERDATA) {
        lua_remove(state, cache);
        return 1;
    }
    lua_pop(state, 1);

    auto* body = static_cast<NetworkBody*>(lua_newuserdatauv(state, sizeof(NetworkBody), 0));
    body->network = network;
    const std::size_t copy_n = length > id_bytes ? id_bytes : length;
    if (copy_n > 0)
        std::memcpy(body->id, text, copy_n);
    body->id[copy_n] = '\0';
    luaL_getmetatable(state, network_meta_name);
    lua_setmetatable(state, -2);
    lua_pushvalue(state, -1);
    lua_rawsetp(state, cache, network);
    lua_remove(state, cache);
    return 1;
}

static int network_run(lua_State* state) {
    NetworkBody* body = need_network(state);
    if (body == nullptr)
        return 0;
    if (lua_type(state, 2) != LUA_TTABLE) {
        luaL_error(state, "network %s: run expects a table of inputs", body->id);
        return 0;
    }

    PortCopy inputs[max_ports];
    PortCopy outputs[max_ports];
    std::size_t input_count = 0;
    std::size_t output_count = 0;
    uint64_t units = 0;
    {
        copy_ports(body->network, false, inputs, &input_count);
        copy_ports(body->network, true, outputs, &output_count);
        units = body->network->charge_units() + groups_of(element_total(inputs, input_count)) +
                groups_of(element_total(outputs, output_count)) + script_host::scan_call_charge;
    }
    script_host::charge_work(state, units);

    lua_pushnil(state);
    const int champion = lua_gettop(state);
    bool have_unknown = false;
    bool non_string = false;
    lua_pushnil(state);
    while (lua_next(state, 2) != 0) {
        const int key = lua_gettop(state) - 1;
        if (lua_type(state, key) != LUA_TSTRING) {
            non_string = true;
        } else {
            std::size_t key_length = 0;
            const char* key_text = lua_tolstring(state, key, &key_length);
            const bool known = port_named(inputs, input_count, key_text, key_length);
            if (!known && key_text != nullptr) {
                bool replace = !have_unknown;
                if (!replace) {
                    std::size_t champ_length = 0;
                    const char* champ = lua_tolstring(state, champion, &champ_length);
                    const std::size_t compared =
                        key_length < champ_length ? key_length : champ_length;
                    int order = 0;
                    if (compared > 0 && champ != nullptr)
                        order = std::memcmp(key_text, champ, compared);
                    if (order == 0) {
                        if (key_length < champ_length)
                            order = -1;
                        else if (key_length > champ_length)
                            order = 1;
                    }
                    replace = order < 0;
                }
                if (replace) {
                    lua_pushlstring(state, key_text, key_length);
                    lua_replace(state, champion);
                    have_unknown = true;
                }
            }
        }
        lua_pop(state, 1);
    }

    if (have_unknown) {
        const char* name = lua_tostring(state, champion);
        luaL_error(state, "network %s: unknown input '%s'", body->id, name != nullptr ? name : "");
        return 0;
    }
    if (non_string) {
        luaL_error(state, "network %s: input names must be strings", body->id);
        return 0;
    }

    for (std::size_t index = 0; index < input_count; ++index) {
        lua_getfield(state, 2, inputs[index].name);
        if (lua_isnil(state, -1) != 0) {
            luaL_error(state, "network %s: missing input '%s'", body->id, inputs[index].name);
            return 0;
        }
        TensorHead* head = tensor_at(state, -1);
        if (head == nullptr) {
            luaL_error(
                state, "network %s: input '%s' must be a tensor", body->id, inputs[index].name
            );
            return 0;
        }
        if (head->count != inputs[index].elements) {
            luaL_error(
                state,
                "network %s: input '%s' has %I elements, not %I",
                body->id,
                inputs[index].name,
                static_cast<lua_Integer>(head->count),
                static_cast<lua_Integer>(inputs[index].elements)
            );
            return 0;
        }
        lua_pop(state, 1);
    }

    for (std::size_t index = 0; index < input_count; ++index) {
        lua_getfield(state, 2, inputs[index].name);
        TensorHead* head = tensor_at(state, -1);
        const int32_t* elements = head != nullptr ? tensor_elements(head) : nullptr;
        int16_t* dest = nullptr;
        std::size_t dest_count = 0;
        {
            const auto filled = body->network->input(index);
            dest = filled.data();
            dest_count = filled.size();
        }
        const std::size_t used =
            dest_count < inputs[index].elements ? dest_count : inputs[index].elements;
        for (std::size_t element = 0; element < used && elements != nullptr; ++element) {
            const int64_t sample = oa::data::neural::rescale(
                elements[element], -16, static_cast<int32_t>(inputs[index].exponent)
            );
            dest[element] = oa::data::neural::saturate_int16(sample);
        }
        lua_pop(state, 1);
    }

    const oa::data::neural::RunError stopped = body->network->run();
    if (stopped.failed) {
        luaL_error(
            state,
            "network %s: layer %I (embedding) has no row %I",
            body->id,
            static_cast<lua_Integer>(stopped.layer),
            static_cast<lua_Integer>(stopped.row)
        );
        return 0;
    }

    lua_settop(state, 2);
    lua_createtable(state, 0, static_cast<int>(output_count));
    const int results = lua_gettop(state);
    for (std::size_t index = 0; index < output_count; ++index) {
        lua_pushlstring(state, outputs[index].name, outputs[index].name_length);
        if (outputs[index].index) {
            const int64_t chosen =
                static_cast<int64_t>(body->network->output_index(index)) + int64_t{1};
            lua_pushinteger(state, static_cast<lua_Integer>(chosen));
        } else {
            int32_t* elements = push_zero_tensor(state, outputs[index].rank, outputs[index].dims);
            const int16_t* produced = nullptr;
            std::size_t produced_count = 0;
            {
                const auto listed = body->network->output(index);
                produced = listed.data();
                produced_count = listed.size();
            }
            const uint32_t used = produced_count < outputs[index].elements
                                      ? static_cast<uint32_t>(produced_count)
                                      : outputs[index].elements;
            for (uint32_t element = 0; element < used && elements != nullptr && produced != nullptr;
                 ++element) {
                const int64_t sample = oa::data::neural::rescale(
                    produced[element], static_cast<int32_t>(outputs[index].exponent), -16
                );
                elements[element] = oa::data::neural::saturate_int32(sample);
            }
        }
        lua_settable(state, results);
    }
    return 1;
}

static int network_names(lua_State* state, bool outputs) {
    NetworkBody* body = need_network(state);
    if (body == nullptr)
        return 0;
    const char* names[max_ports];
    std::size_t lengths[max_ports];
    std::size_t count = 0;
    {
        PortCopy ports[max_ports];
        copy_ports(body->network, outputs, ports, &count);
        for (std::size_t index = 0; index < count; ++index) {
            names[index] = ports[index].name;
            lengths[index] = ports[index].name_length;
        }
    }
    const uint64_t units = count == 0 ? script_host::element_charge : count;
    script_host::charge_work(state, units);
    lua_createtable(state, static_cast<int>(count), 0);
    for (std::size_t index = 0; index < count; ++index) {
        lua_pushlstring(state, names[index], lengths[index]);
        lua_rawseti(state, -2, static_cast<lua_Integer>(index) + 1);
    }
    return 1;
}

static int network_inputs(lua_State* state) {
    return network_names(state, false);
}

static int network_outputs(lua_State* state) {
    return network_names(state, true);
}

static int network_cost(lua_State* state) {
    NetworkBody* body = need_network(state);
    if (body == nullptr)
        return 0;
    const uint64_t units = body->network->charge_units();
    script_host::charge_work(state, script_host::element_charge);
    lua_pushinteger(state, static_cast<lua_Integer>(units));
    return 1;
}

} // extern "C"

void ensure_tensor_meta(lua_State* state) {
    if (luaL_newmetatable(state, tensor_meta_name) == 0) {
        lua_pop(state, 1);
        return;
    }
    lua_newtable(state);
    lua_pushcfunction(state, tensor_get);
    lua_setfield(state, -2, "get");
    lua_pushcfunction(state, tensor_argmax);
    lua_setfield(state, -2, "argmax");
    lua_pushcfunction(state, tensor_totable);
    lua_setfield(state, -2, "totable");
    lua_pushcfunction(state, tensor_size);
    lua_setfield(state, -2, "size");
    lua_pushcfunction(state, tensor_shape);
    lua_setfield(state, -2, "shape");
    lua_setfield(state, -2, "__index");
    lua_pushcfunction(state, tensor_newindex);
    lua_setfield(state, -2, "__newindex");
    lua_pushcfunction(state, tensor_len);
    lua_setfield(state, -2, "__len");
    lua_pushcfunction(state, tensor_tostring);
    lua_setfield(state, -2, "__tostring");
    lua_pushboolean(state, 0);
    lua_setfield(state, -2, "__metatable");
    lua_pop(state, 1);
}

void ensure_network_meta(lua_State* state) {
    if (luaL_newmetatable(state, network_meta_name) == 0) {
        lua_pop(state, 1);
        return;
    }
    lua_newtable(state);
    lua_pushcfunction(state, network_run);
    lua_setfield(state, -2, "run");
    lua_pushcfunction(state, network_inputs);
    lua_setfield(state, -2, "inputs");
    lua_pushcfunction(state, network_outputs);
    lua_setfield(state, -2, "outputs");
    lua_pushcfunction(state, network_cost);
    lua_setfield(state, -2, "cost");
    lua_setfield(state, -2, "__index");
    lua_pushcfunction(state, network_newindex);
    lua_setfield(state, -2, "__newindex");
    lua_pushboolean(state, 0);
    lua_setfield(state, -2, "__metatable");
    lua_pop(state, 1);
}

} // namespace

int32_t* push_zero_tensor(lua_State* state, uint8_t rank, const uint32_t dims[4]) {
    if (rank < 1 || rank > 4 || dims == nullptr) {
        luaL_error(state, "brain.tensor: expects 1 to 4194304 numbers");
        return nullptr;
    }
    uint64_t count = 1;
    for (uint8_t axis = 0; axis < rank; ++axis) {
        if (dims[axis] == 0 || count > static_cast<uint64_t>(max_elements) / dims[axis]) {
            luaL_error(state, "brain.tensor: expects 1 to 4194304 numbers");
            return nullptr;
        }
        count *= dims[axis];
    }
    const std::size_t bytes =
        sizeof(TensorHead) + static_cast<std::size_t>(count) * sizeof(int32_t);
    void* block = lua_newuserdatauv(state, bytes, 0);
    std::memset(block, 0, bytes);
    auto* head = static_cast<TensorHead*>(block);
    head->count = static_cast<uint32_t>(count);
    head->rank = rank;
    for (uint8_t axis = 0; axis < rank; ++axis)
        head->dims[axis] = dims[axis];
    ensure_tensor_meta(state);
    luaL_getmetatable(state, tensor_meta_name);
    lua_setmetatable(state, -2);
    return tensor_elements(head);
}

void install_networks(lua_State* state, Seat& seat) {
    (void)seat;
    ensure_tensor_meta(state);
    ensure_network_meta(state);
    lua_newtable(state);
    lua_rawsetp(state, LUA_REGISTRYINDEX, &network_cache_token);
    lua_getglobal(state, "brain");
    if (lua_type(state, -1) != LUA_TTABLE) {
        lua_pop(state, 1);
        lua_newtable(state);
    }
    lua_pushcfunction(state, brain_network);
    lua_setfield(state, -2, "network");
    lua_pushcfunction(state, brain_tensor);
    lua_setfield(state, -2, "tensor");
    lua_setglobal(state, "brain");
}

} // namespace oa::sim::brains
