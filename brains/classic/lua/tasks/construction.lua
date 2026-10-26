-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

-- The builders' tick. Each builder places a weighted pick near the base,
-- and an idle one patrols home. A pick that is a round for the builder's
-- weapon is queued as a factory's pick is, and stockpiled. A capturing
-- builder builds only while the player is under the withhold count and the
-- hold tick has passed.
-- @quirk ai.builder-withhold-threshold moves only the build limit: an idle
-- capturing builder still circles the base from five build-capable units.

local build = require("build")
local knowledge = require("knowledge")
local place = require("place")

local construction = {}

local ability_capture = 0x1000
local commander_patrol_quota = 5
local builder_patrol_radius = 0x1400000
local builder_nudge_radius = 0x100000
local commander_patrol_radius = 0x2800000

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

local function point(x, y, z)
    return {x = fixed.raw(x), y = fixed.raw(y), z = fixed.raw(z)}
end

local function negated_scaled(angle, magnitude)
    local sine = i32(-geometry.sine_scaled(angle, fixed.raw(magnitude)):raw())
    local cosine = i32(-geometry.cosine_scaled(angle, fixed.raw(magnitude)):raw())
    return sine, cosine
end

local function captures(row)
    return ((row.abilities or 0) & ability_capture) ~= 0
end

-- A round for the builder's first weapon to stockpile: the type's unit name
-- holds MAKENUKE or MAKEANTI, as the build queue tells it.
local function stockpile_round(row)
    if type(row) ~= "table" or type(row.unit_name) ~= "string" then
        return false
    end
    return string.find(row.unit_name, "MAKENUKE", 1, true) ~= nil
        or string.find(row.unit_name, "MAKEANTI", 1, true) ~= nil
end

function construction.run(seat, squad)
    local me = seat.me
    if me == nil or type(me.squad_size) ~= "function" then
        return
    end
    local count = me:squad_size(squad)
    if type(count) ~= "number" then
        return
    end
    local withhold = 5
    if type(seat.rules) == "table" and type(seat.rules.builder_withhold) == "number" then
        withhold = seat.rules.builder_withhold
    end
    local base = knowledge.base(seat)
    local tick = me.tick or 0
    local held = me.builds_held_until
    local builders = knowledge.builders(seat)
    local map = seat.game ~= nil and seat.game.map or nil
    for index = 1, count do
        local unit = me:squad_member(squad, index)
        if unit ~= nil then
            local type_index = unit.type_index
            local row = type(type_index) == "number" and seat.types[type_index] or nil
            if type(row) == "table" and (row.build_count or 0) ~= 0 then
                local held_back = false
                if captures(row) then
                    if builders >= withhold then
                        held_back = true
                    elseif held ~= nil and tick < held then
                        held_back = true
                    end
                end
                local _, busy = unit:head_order()
                if not held_back and busy ~= true then
                    local pick = build.pick(seat, unit)
                    if pick ~= 0 and stockpile_round(seat.types[pick]) then
                        -- A round is no unit to place: the queue stockpiles it.
                        unit:queue(pick, 1)
                    elseif pick ~= 0 then
                        local position = unit.position
                        local builder_at = {
                            x = raw_of(position.x),
                            y = raw_of(position.y),
                            z = raw_of(position.z),
                        }
                        local site = place.build(seat, builder_at, pick)
                        local placed = site ~= nil
                        if placed and captures(row) and map ~= nil then
                            local cells_x = map.width or 0
                            local cells_z = 0
                            if type(map.height) == "function" then
                                cells_z = map:height() or 0
                            end
                            local reach = wrap_mul(
                                div_tz(cells_x * 16 - 32 + cells_z * 16 - 128, 3), 0x10000
                            )
                            local length = length_of(
                                wrap_sub(site.x, base.x), 0, wrap_sub(site.z, base.z)
                            )
                            if length > reach then
                                placed = false
                            end
                        end
                        if placed then
                            unit:build(pick, point(site.x, site.y, site.z))
                        end
                    end
                end
            end
        end
    end
    for index = 1, count do
        local unit = me:squad_member(squad, index)
        if unit ~= nil then
            local type_index = unit.type_index
            local row = type(type_index) == "number" and seat.types[type_index] or nil
            if type(row) == "table" then
                local present, _, idle = unit:head_order()
                if not (present ~= nil and idle ~= true) then
                    if not (captures(row) and builders < commander_patrol_quota) then
                        local position = unit.position
                        local ux = raw_of(position.x)
                        local uy = raw_of(position.y)
                        local uz = raw_of(position.z)
                        if captures(row) then
                            local offset_x = wrap_sub(base.x, ux)
                            local offset_z = wrap_sub(base.z, uz)
                            local length = length_of(offset_x, 0, offset_z)
                            if length > commander_patrol_radius then
                                local angle = brain.shared_random(0x10000)
                                offset_x, offset_z = negated_scaled(angle, commander_patrol_radius)
                            end
                            unit:move(
                                point(wrap_add(base.x, offset_x), uy, wrap_add(base.z, offset_z)),
                                false
                            )
                            unit:patrol(point(base.x, uy, base.z), true)
                        else
                            local offset_x = wrap_sub(base.x, ux)
                            local offset_y = wrap_sub(base.y, uy)
                            local offset_z = wrap_sub(base.z, uz)
                            local length = length_of(offset_x, offset_y, offset_z)
                            local destination_x = base.x
                            local destination_y = base.y
                            local destination_z = base.z
                            if length < builder_patrol_radius then
                                if length < builder_nudge_radius then
                                    local angle = brain.shared_random(0x10000)
                                    offset_x, offset_z = negated_scaled(angle, builder_patrol_radius)
                                    offset_y = 0
                                else
                                    local factor = i32(div_tz(0x140 << 32, length))
                                    local sx, sy, sz = geometry.scale(
                                        fixed.raw(offset_x),
                                        fixed.raw(offset_y),
                                        fixed.raw(offset_z),
                                        fixed.raw(factor)
                                    )
                                    offset_x = i32(sx:raw())
                                    offset_y = i32(sy:raw())
                                    offset_z = i32(sz:raw())
                                end
                                destination_x = wrap_add(ux, offset_x)
                                destination_y = wrap_add(uy, offset_y)
                                destination_z = wrap_add(uz, offset_z)
                            end
                            unit:patrol(point(destination_x, destination_y, destination_z), false)
                        end
                    end
                end
            end
        end
    end
end

return construction
