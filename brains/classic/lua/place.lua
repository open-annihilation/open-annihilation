-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

-- Where a builder puts the type it picked. The search grows until it is as
-- wide as the map, and it starts again after a site is kept. An extractor
-- looks at metal deposits when the map's surface metal loses a draw;
-- anything else tries the build grid thirty times.

local knowledge = require("knowledge")

local place = {}

local radius_step = 0xa0
local attempts = 30
local mex_slack = 0xa0

local function u32(value)
    return value & 0xFFFFFFFF
end

local function i32(value)
    local bits = u32(value)
    if bits >= 0x80000000 then
        return bits - 0x100000000
    end
    return bits
end

local function sign16(value)
    local bits = u32(value) & 0xFFFF
    if bits >= 0x8000 then
        return bits - 0x10000
    end
    return bits
end

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

local function mul_u32(a, b)
    local left = u32(a)
    local right = u32(b)
    local a0 = left & 0xFFFF
    local a1 = (left >> 16) & 0xFFFF
    local b0 = right & 0xFFFF
    local b1 = (right >> 16) & 0xFFFF
    local low = a0 * b0
    local mid = a0 * b1 + a1 * b0
    return u32(low + ((mid & 0xFFFF) << 16))
end

local function wrap_add(a, b)
    return i32(u32(a) + u32(b))
end

local function wrap_sub(a, b)
    return i32(u32(a) - u32(b))
end

local function wrap_mul(a, b)
    return i32(mul_u32(a, b))
end

local function raw_of(component)
    if type(component) == "userdata" or type(component) == "table" then
        local method = component.raw
        if type(method) == "function" then
            return i32(method(component))
        end
    end
    return i32(component or 0)
end

local function length_of(x, y, z)
    return i32(geometry.length(fixed.raw(x), fixed.raw(y), fixed.raw(z)):raw())
end

local function scale_of(x, y, z, factor)
    local sx, sy, sz = geometry.scale(fixed.raw(x), fixed.raw(y), fixed.raw(z), fixed.raw(factor))
    return i32(sx:raw()), i32(sy:raw()), i32(sz:raw())
end

local function shifted_cell(bits)
    return sign16(i32(bits) // 1048576)
end

local function centre_cell(position, footprint)
    local sum = u32(u32(position) + mul_u32(footprint, 0xfff80000) + 0x80000)
    return shifted_cell(sum)
end

-- A point, or the point this far from the builder toward it.
local function clamp_toward(from, to, reach)
    local toward_x = wrap_sub(to.x, from.x)
    local toward_y = wrap_sub(to.y, from.y)
    local toward_z = wrap_sub(to.z, from.z)
    local length = length_of(toward_x, toward_y, toward_z)
    if not (reach < length) then
        return {x = to.x, y = to.y, z = to.z}
    end
    local factor = i32(div_tz(reach << 16, length))
    local scaled_x, scaled_y, scaled_z = scale_of(toward_x, toward_y, toward_z, factor)
    return {
        x = wrap_add(from.x, scaled_x),
        y = wrap_add(from.y, scaled_y),
        z = wrap_add(from.z, scaled_z),
    }
end

local function site_ok(type_id, cell_x, cell_z, yard)
    if type(brain.site) ~= "function" then
        return false
    end
    local clear
    if yard then
        clear = brain.site(type_id, cell_x, cell_z, 0, "yard")
    else
        clear = brain.site(type_id, cell_x, cell_z, 0, "site")
    end
    return clear == true
end

local function cell_metal(map, x, z)
    if map == nil or type(map.metal) ~= "function" then
        return 0
    end
    local metal = map:metal(x, z)
    if type(metal) ~= "number" then
        return 0
    end
    return metal
end

local function footprint_metal(map, cell_x, cell_z, footprint_x, footprint_z)
    local metal = 0
    for z = 0, footprint_z - 1 do
        for x = 0, footprint_x - 1 do
            metal = metal + cell_metal(map, cell_x + x, cell_z + z)
        end
    end
    return metal
end

function place.on_metal(seat, type_id, row, center, radius)
    local spots = seat.metal
    if type(spots) ~= "table" or #spots == 0 then
        return nil
    end
    local map = seat.game.map
    local center_x = centre_cell(center.x, row.footprint_x)
    local center_z = centre_cell(center.z, row.footprint_z)
    local reach = i32(mul_u32(radius, radius))
    local kept = {}
    local distances = {}
    for index = 1, #spots do
        local spot = spots[index]
        local dx = sign16(spot.x) - center_x
        local dz = sign16(spot.z) - center_z
        local distance = wrap_add(wrap_mul(dx, dx), wrap_mul(dz, dz))
        if distance <= reach then
            kept[#kept + 1] = spot
            distances[#distances + 1] = distance
        end
    end
    if #distances == 0 or type(brain.float_heap_order) ~= "function" then
        return nil
    end
    local order = brain.float_heap_order(distances)
    if type(order) ~= "table" then
        return nil
    end
    local best_metal = 0
    local first_distance = -1
    local best_x = 0
    local best_z = 0
    for index = 1, #order do
        local spot = kept[order[index]]
        if type(spot) == "table" then
            local cell_x = sign16(sign16(spot.x) + div_tz(row.footprint_x - 3, -2))
            local cell_z = sign16(sign16(spot.z) + div_tz(row.footprint_z - 3, -2))
            local dx = cell_x - center_x
            local dz = cell_z - center_z
            local distance = wrap_add(wrap_mul(dx, dx), wrap_mul(dz, dz))
            if first_distance >= 0 and first_distance + mex_slack < distance then
                break
            end
            if site_ok(type_id, cell_x, cell_z, true) then
                local metal = footprint_metal(map, cell_x, cell_z, row.footprint_x, row.footprint_z)
                if best_metal < metal then
                    best_x = cell_x
                    best_z = cell_z
                    best_metal = metal
                    if first_distance == -1 then
                        first_distance = distance
                    end
                end
            end
        end
    end
    if best_metal == 0 then
        return nil
    end
    return {x = best_x, z = best_z}
end

local function grid_cell(position, offset, footprint, step, phase, jitter)
    local sum = u32(u32(position) - u32(offset) + mul_u32(footprint, 0xfff80000) + 0x80000)
    local raw = shifted_cell(sum)
    local snapped = 0
    if step ~= 0 then
        snapped = div_tz(raw, step) * step
    end
    return sign16(snapped + phase + sign16(jitter))
end

function place.on_grid(seat, type_id, row, center, radius)
    local grid = seat.land_grid
    if (row.min_water_depth or 0) >= 0 then
        grid = seat.water_grid
    end
    if type(grid) ~= "table" then
        return nil
    end
    local map = seat.game.map
    local surface = 0
    if map ~= nil and type(map.surface_metal) == "number" then
        surface = map.surface_metal
    end
    local footprint_x = row.footprint_x or 0
    local footprint_z = row.footprint_z or 0
    local metal_ceiling = wrap_mul(wrap_mul(wrap_mul(surface, footprint_x), footprint_z), 2)
    for _ = 1, attempts do
        local distance = brain.shared_random(u32(radius))
        local angle = brain.shared_random(0x10000)
        local magnitude = i32(u32(distance) << 16)
        local offset_x = i32(geometry.sine_scaled(angle, fixed.raw(magnitude)):raw())
        local offset_z = i32(geometry.cosine_scaled(angle, fixed.raw(magnitude)):raw())
        local jitter_x = brain.shared_random(
            u32((grid.step_x or 0) - footprint_x - (grid.margin or 0))
        )
        local jitter_z = brain.shared_random(
            u32((grid.step_z or 0) - footprint_z - (grid.margin or 0))
        )
        local cell_x = grid_cell(
            center.x, offset_x, footprint_x, grid.step_x or 0, grid.phase_x or 0, jitter_x
        )
        local cell_z = grid_cell(
            center.z, offset_z, footprint_z, grid.step_z or 0, grid.phase_z or 0, jitter_z
        )
        if site_ok(type_id, cell_x, cell_z, false) then
            local metal = footprint_metal(map, cell_x, cell_z, footprint_x, footprint_z)
            if metal <= metal_ceiling then
                return {x = cell_x, z = cell_z}
            end
        end
    end
    return nil
end

local function extracts(row)
    local value = row.extracts_metal
    if value == nil then
        return false
    end
    return value ~= f32(0)
end

function place.build(seat, builder_position, type_id)
    if type(seat) ~= "table" or type(builder_position) ~= "table" then
        return nil
    end
    local row = seat.types[type_id]
    if type(row) ~= "table" then
        return nil
    end
    local game = seat.game
    local map = game ~= nil and game.map or nil
    if map == nil then
        return nil
    end
    local cells_x = map.width or 0
    local cells_z = 0
    if type(map.height) == "function" then
        cells_z = map:height() or 0
    end
    local world_x = i32(u32(cells_x) << 4)
    local world_z = i32(u32(cells_z) << 4)
    local widest = world_x > world_z and world_x or world_z
    local radius = seat.placement_radius or 0
    if radius < widest then
        radius = radius + radius_step
        seat.placement_radius = radius
    end
    local reach = i32(u32(radius) << 16)
    local center = clamp_toward(builder_position, knowledge.base(seat), reach)
    local cell
    local surface = map.surface_metal or 0
    if extracts(row) and surface < brain.shared_random(0xff) then
        cell = place.on_metal(seat, type_id, row, center, i32(u32(radius) << 2))
    else
        cell = place.on_grid(seat, type_id, row, center, radius)
    end
    if cell == nil then
        return nil
    end
    local site = {
        x = i32(u32(row.footprint_x + cell.x * 2) << 19),
        y = builder_position.y,
        z = i32(u32(row.footprint_z + cell.z * 2) << 19),
    }
    seat.placement_radius = 0
    return site
end

return place
