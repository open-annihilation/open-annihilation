-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

-- The aircraft tick. A small wing scouts around the base, or queues a
-- patrol to the nearest enemy before a base exists. A wing at the
-- patrol-group size patrols one random map edge. Under
-- ai.patrol-null-enemy-skip a small wing with no base does that too.
-- The timetable draw is already taken; these draws are the orders.

local knowledge = require("knowledge")

local air = {}

local squad_navy = 7

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

local function wrap_mul(a, b)
    return i32(mul_u32(a, b))
end

local function sign16(value)
    local bits = u32(value) & 0xFFFF
    if bits >= 0x8000 then
        return bits - 0x10000
    end
    return bits
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

local function point(x, y, z)
    return {x = fixed.raw(x), y = fixed.raw(y), z = fixed.raw(z)}
end

local function world_span(game)
    local cells_x = 0
    local cells_z = 0
    local map = nil
    if type(game) == "table" or type(game) == "userdata" then
        map = game.map
    end
    if type(map) == "table" or type(map) == "userdata" then
        cells_x = map.width or 0
        if type(map.height) == "function" then
            cells_z = map:height() or 0
        end
    end
    return i32(u32(cells_x) << 4), i32(u32(cells_z) << 4)
end

local function high_word(component)
    return sign16(u32(component) >> 16)
end

local function no_base(base)
    return (u32(base.x) >> 16 | u32(base.z) >> 16) == 0
end

local function squad_count(me, squad)
    if me == nil or type(me.squad_size) ~= "function" then
        return 0
    end
    local count = me:squad_size(squad)
    if type(count) ~= "number" then
        return 0
    end
    return count
end

-- Average of the squad's positions from the signed high words, back in 16.16.
-- An empty squad stays at the origin.
local function centroid(me, squad, count)
    if count < 1 or me == nil or type(me.squad_member) ~= "function" then
        return {x = 0, y = 0, z = 0}
    end
    local x = 0
    local y = 0
    local z = 0
    for index = 1, count do
        local unit = me:squad_member(squad, index)
        if unit ~= nil and type(unit.position) == "table" then
            local position = unit.position
            x = x + high_word(raw_of(position.x))
            y = y + high_word(raw_of(position.y))
            z = z + high_word(raw_of(position.z))
        end
    end
    return {
        x = wrap_mul(div_tz(x, count), 0x10000),
        y = wrap_mul(div_tz(y, count), 0x10000),
        z = wrap_mul(div_tz(z, count), 0x10000),
    }
end

local function order_wing(me, squad, count, command, queued, at)
    if count < 1 or me == nil or type(me.units) ~= "function" then
        return
    end
    local list = me:units()
    if type(list) ~= "table" then
        return
    end
    for index = 1, #list do
        local unit = list[index]
        if unit ~= nil and unit.squad == squad and (unit.type_index or 0) ~= 0 then
            if command == "move" then
                unit:move(at, queued)
            else
                unit:patrol(at, queued)
            end
        end
    end
end

local function enemy_filter(seat, squad)
    local rules = seat.rules
    if type(rules) ~= "table" or rules.nearest_enemy ~= "skip-submerged" then
        return nil
    end
    local source = 0
    local task = seat.tasks[squad]
    if type(task) == "table" and type(task.source) == "number" then
        source = task.source
    end
    return {widened = true, skip_submerged = source ~= squad_navy}
end

local function draw_below(bound)
    return i32(brain.shared_random(u32(bound)))
end

local function around(draw, half, base)
    local shifted = wrap_add(draw, wrap_mul(half, 0xffff))
    return wrap_add(wrap_mul(shifted, 0x10000), base)
end

local function scout(seat, me, squad, count, base)
    local world_x, world_z = world_span(seat.game)
    local span_x = div_tz(world_x, 8)
    local span_z = div_tz(world_z, 8)
    local half_x = div_tz(span_x, 2)
    local half_z = div_tz(span_z, 2)
    local waypoints = draw_below(2) + 2
    for index = 1, waypoints do
        local x = around(draw_below(span_x), half_x, base.x)
        local z = around(draw_below(span_z), half_z, base.z)
        local command = "patrol"
        local queued = true
        if index == 1 then
            command = "move"
            queued = false
        end
        order_wing(me, squad, count, command, queued, point(x, base.y, z))
    end
end

local function edge_of(world_x, world_z)
    local x = 0
    local z = 0
    if brain.shared_random(2) ~= 0 then
        x = wrap_mul(draw_below(world_x), 0x10000)
        if brain.shared_random(2) == 0 then
            z = wrap_mul(wrap_add(world_z, 0xffff), 0x10000)
        end
    else
        if brain.shared_random(2) == 0 then
            x = wrap_mul(wrap_add(world_x, 0xffff), 0x10000)
        end
        z = wrap_mul(draw_below(world_z), 0x10000)
    end
    return x, z
end

function air.run(seat, squad)
    local me = seat.me
    local rules = seat.rules
    local limit = 5
    local skip_null = false
    if type(rules) == "table" then
        if type(rules.patrol_group) == "number" then
            limit = rules.patrol_group
        end
        skip_null = rules.patrol_null_enemy_skip == true
    end
    local count = squad_count(me, squad)
    local base = knowledge.base(seat)
    local missing = no_base(base)
    local small = count < limit
    if small and not (missing and skip_null) then
        if missing then
            local center = centroid(me, squad, count)
            if me ~= nil and type(me.nearest_enemy) == "function" then
                local filter = enemy_filter(seat, squad)
                local target = me:nearest_enemy(fixed.raw(center.x), fixed.raw(center.z), filter)
                if target ~= nil and type(target.position) == "table" then
                    local position = target.position
                    order_wing(
                        me,
                        squad,
                        count,
                        "patrol",
                        true,
                        point(raw_of(position.x), raw_of(position.y), raw_of(position.z))
                    )
                end
            end
            return
        end
        scout(seat, me, squad, count, base)
        return
    end
    local world_x, world_z = world_span(seat.game)
    local x, z = edge_of(world_x, world_z)
    order_wing(me, squad, count, "patrol", false, point(x, 0, z))
end

return air
