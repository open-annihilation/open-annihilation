-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

-- Per-type weights, limits and base weights, the two build grids, and the
-- metal spots. Owned counts, the builder count and the base position are the
-- engine's record: this module only reads them from me.knowledge.

local knowledge = {}

local function type_count(seat)
    local listed = seat.types
    if type(listed) ~= "table" then
        return 1
    end
    return #listed + 1
end

local function blank_grid()
    return {step_x = 0, step_z = 0, phase_x = 0, phase_z = 0, margin = 0}
end

function knowledge.create(seat)
    seat.plan = 1
    seat.grids_ready = 0
    seat.land_grid = blank_grid()
    seat.water_grid = blank_grid()
    seat.metal = {}
    knowledge.reset(seat)
    seat.placement_radius = 0
end

function knowledge.reset(seat)
    local count = type_count(seat)
    seat.weight_percent = {}
    seat.limits = {}
    seat.weight_locked = {}
    seat.limit_locked = {}
    seat.base_weights = {}
    for index = 0, count - 1 do
        local row = seat.types[index]
        local bm_code = 0
        local has_list = 0
        if type(row) == "table" then
            bm_code = row.bm_code or 0
            has_list = row.has_build_list or 0
        end
        local weight = 0
        if bm_code == 0 then
            weight = weight + 40
        end
        if has_list ~= 0 then
            weight = weight + 20
        end
        seat.base_weights[index] = weight
        seat.weight_percent[index] = 100
        seat.weight_locked[index] = 0
        seat.limits[index] = -1
        seat.limit_locked[index] = 0
    end
end

local function sign16(value)
    local bits = value & 0xFFFF
    if bits >= 0x8000 then
        return bits - 0x10000
    end
    return bits
end

local function half_span(span)
    if span < 0 then
        return -((-span) // 2)
    end
    return span // 2
end

local function step_bound(step)
    if step < 0 then
        return step & 0xFFFFFFFF
    end
    return step
end

local function draw_step(limit, margin)
    local roll = brain.setup_random(limit)
    return sign16(roll + margin + 8)
end

local function draw_phase(step)
    local roll = brain.setup_random(step_bound(step))
    local half = half_span(step) & 0xFFFFFFFF
    return sign16(roll - half)
end

local function draw_grid(margin, x_limit)
    local grid = {margin = margin}
    grid.step_x = draw_step(x_limit, margin)
    grid.step_z = draw_step(3, margin)
    grid.phase_x = draw_phase(grid.step_x)
    grid.phase_z = draw_phase(grid.step_z)
    return grid
end

function knowledge.seed_grids(seat)
    seat.land_grid = draw_grid(3, 10)
    seat.water_grid = draw_grid(6, 20)
    seat.grids_ready = 1
end

function knowledge.scan_metal(seat)
    seat.metal = {}
    local game = seat.game
    if game == nil then
        return
    end
    local map = game.map
    if (type(map) ~= "userdata" and type(map) ~= "table") or type(map.metal_spots) ~= "function" then
        return
    end
    local spots = map:metal_spots()
    if type(spots) ~= "table" then
        return
    end
    for index = 1, #spots do
        local spot = spots[index]
        local x = 0
        local z = 0
        if type(spot) == "table" then
            x = spot.cx or 0
            z = spot.cz or 0
        end
        seat.metal[index] = {x = x, z = z}
    end
end

local function snapshot(seat)
    local me = seat.me
    if me == nil then
        return nil
    end
    return me.knowledge
end

local function whole(value)
    if type(value) ~= "number" then
        return 0
    end
    local integer = math.tointeger(value)
    if integer ~= nil then
        return integer
    end
    local toward_zero = math.modf(value)
    return toward_zero
end

function knowledge.owned(seat, type_index)
    local record = snapshot(seat)
    if record == nil or type(record.owned) ~= "function" then
        return 0
    end
    return whole(record:owned(type_index))
end

function knowledge.builders(seat)
    local record = snapshot(seat)
    if record == nil then
        return 0
    end
    return whole(record.builders)
end

local function raw_component(component)
    if type(component) == "userdata" or type(component) == "table" then
        local method = component.raw
        if type(method) == "function" then
            return whole(method(component))
        end
    end
    return whole(component)
end

function knowledge.base(seat)
    local record = snapshot(seat)
    if record == nil or type(record.base) ~= "table" then
        return {x = 0, y = 0, z = 0}
    end
    local point = record.base
    return {
        x = raw_component(point.x),
        y = raw_component(point.y),
        z = raw_component(point.z),
    }
end

return knowledge
