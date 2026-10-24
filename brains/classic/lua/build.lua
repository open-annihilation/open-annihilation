-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

-- How much the seat wants a type, and which type a builder keeps.
-- The score follows the stores, the income and the type's strength.
-- A pick walks that builder's list once and keeps the same side.

local knowledge = require("knowledge")

local build = {}

local minimum_energy = 50
local minimum_metal = 25
local energy_storage_cap = 1000
local metal_storage_cap = 500
local flag_downloadable = 0x20

local function div_tz(numer, denom)
    if denom == 0 or numer == 0 then
        return 0
    end
    local negative = (numer < 0) ~= (denom < 0)
    local left = numer < 0 and -numer or numer
    local right = denom < 0 and -denom or denom
    local quot = left // right
    if negative then
        return -quot
    end
    return quot
end

local function clamp_percent(value)
    if value < 1 then
        return 0
    end
    if value < 100 then
        return value
    end
    return 100
end

local function as_double(value)
    if type(value) == "userdata" or type(value) == "table" then
        local method = value.tonumber
        if type(method) == "function" then
            return method(value)
        end
    end
    if type(value) ~= "number" then
        return 0
    end
    return value
end

-- A store that is not a finite value inside the 32-bit range counts as the
-- lowest 32-bit value, which the cap then replaces.
local function truncate_to_int32(number)
    if number ~= number or number == math.huge or number == -math.huge then
        return -2147483648
    end
    if number >= 2147483648.0 or number < -2147483648.0 then
        return -2147483648
    end
    local whole = math.modf(number)
    return whole
end

local function downloads_blocked(game)
    if game == nil then
        return false
    end
    local allowed = game.downloadables_allowed
    if type(allowed) == "boolean" then
        return allowed == false
    end
    return game.mode == "campaign"
end

local function within_limit(type_index, type_count, limit, owned)
    if type_index == 0 or type_count <= type_index then
        return false
    end
    if limit == -1 then
        return true
    end
    return owned < limit
end

local function type_count(seat)
    local listed = seat.types
    if type(listed) ~= "table" then
        return 1
    end
    return #listed + 1
end

function build.score(seat, type_index)
    if type(seat) ~= "table" or type(type_index) ~= "number" then
        return 0
    end
    local row = seat.types[type_index]
    if type(row) ~= "table" then
        return 0
    end
    local me = seat.me
    if me == nil or me.energy == nil or me.metal == nil then
        return 0
    end
    if me.energy < minimum_energy or me.metal < minimum_metal then
        return 0
    end
    if downloads_blocked(seat.game) and (row.flags & flag_downloadable) ~= 0 then
        return 0
    end
    local energy_cap = truncate_to_int32(as_double(me.energy_storage))
    if energy_cap > energy_storage_cap then
        energy_cap = energy_storage_cap
    end
    local metal_cap = truncate_to_int32(as_double(me.metal_storage))
    if metal_cap > metal_storage_cap then
        metal_cap = metal_storage_cap
    end
    local energy_need = truncate_to_int32(math.max(0, (energy_cap - as_double(me.energy)) * 0.125))
    local metal_need = truncate_to_int32(math.max(0, (metal_cap - as_double(me.metal)) * 0.25))
    local energy_surplus = me.energy_surplus
    local metal_surplus = me.metal_surplus
    if type(energy_surplus) == "number" and energy_surplus < 1 then
        energy_need = energy_need + 20
    end
    if type(metal_surplus) == "number" and metal_surplus < 1 then
        metal_need = metal_need + 20
    end
    if me.energy_produced < 50 then
        energy_need = energy_need + 100
    elseif me.energy_produced < 200 then
        energy_need = energy_need + 10
    end
    if me.metal_produced < 3 then
        metal_need = metal_need + 100
    elseif me.metal_produced < 5 then
        metal_need = metal_need + 20
    end
    local metal_share = clamp_percent(metal_need)
    local energy_share = clamp_percent(energy_need - metal_share)
    local base_share = 100 - metal_share - energy_share
    if base_share < 1 then
        base_share = 0
    end
    local limits = seat.limits
    local limit = -1
    if type(limits) == "table" and limits[type_index] ~= nil then
        limit = limits[type_index]
    end
    if not within_limit(type_index, type_count(seat), limit, knowledge.owned(seat, type_index)) then
        return 0
    end
    if type(brain.strengths) ~= "function" then
        return 0
    end
    local priority, economy, attack = brain.strengths(type_index)
    if priority == nil then
        return 0
    end
    local weight = 0
    local weights = seat.weight_percent
    if type(weights) == "table" and weights[type_index] ~= nil then
        weight = weights[type_index]
    end
    local weighted = (economy * metal_share + attack * energy_share + priority * base_share) * weight
    return div_tz(weighted, 10000)
end

function build.pick(seat, builder)
    if type(seat) ~= "table" or builder == nil then
        return 0
    end
    local builder_type = builder.type_index
    if type(builder_type) ~= "number" then
        return 0
    end
    local row = seat.types[builder_type]
    if type(row) ~= "table" or (row.build_count or 0) == 0 then
        return 0
    end
    local pick = 0
    local total = 0
    for index = 1, row.build_count do
        local candidate = row.build_ids[index]
        local score = build.score(seat, candidate)
        if score > 0 then
            total = (total + score) & 0xFFFFFFFF
            if brain.shared_random(total) < score then
                pick = candidate
            end
        end
    end
    if pick == 0 then
        return 0
    end
    local chosen = seat.types[pick]
    if type(chosen) ~= "table" or chosen.side ~= row.side then
        return 0
    end
    return pick
end

return build
