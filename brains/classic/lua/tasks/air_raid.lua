-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

-- The aircraft tick. A small wing scouts around the base, or queues a
-- patrol to the nearest enemy before a base exists. A wing at the
-- patrol-group size patrols one random map edge. The timetable draw is
-- already taken; these draws are the orders.

local knowledge = require("knowledge")
local squads = require("squads")

local air = {}

local function no_base(base)
    return (squads.u32(base.x) >> 16 | squads.u32(base.z) >> 16) == 0
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
    return squads.i32(squads.u32(cells_x) << 4), squads.i32(squads.u32(cells_z) << 4)
end

local function draw_below(bound)
    return squads.i32(brain.shared_random(squads.u32(bound)))
end

local function around(draw, half, base)
    local shifted = squads.wrap_add(draw, squads.wrap_mul(half, 0xffff))
    return squads.wrap_add(squads.wrap_mul(shifted, 0x10000), base)
end

local function scout(seat, me, squad, base)
    local world_x, world_z = world_span(seat.game)
    local span_x = squads.div_tz(world_x, 8)
    local span_z = squads.div_tz(world_z, 8)
    local half_x = squads.div_tz(span_x, 2)
    local half_z = squads.div_tz(span_z, 2)
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
        squads.order(me, squad, command, queued, nil, squads.fixed_point({x = x, y = base.y, z = z}))
    end
end

local function edge_of(world_x, world_z)
    local x = 0
    local z = 0
    if brain.shared_random(2) ~= 0 then
        x = squads.wrap_mul(draw_below(world_x), 0x10000)
        if brain.shared_random(2) == 0 then
            z = squads.wrap_mul(squads.wrap_add(world_z, 0xffff), 0x10000)
        end
    else
        if brain.shared_random(2) == 0 then
            x = squads.wrap_mul(squads.wrap_add(world_x, 0xffff), 0x10000)
        end
        z = squads.wrap_mul(draw_below(world_z), 0x10000)
    end
    return x, z
end

function air.run(seat, task)
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
    local squad = task.squad
    local count = squads.count(me, squad)
    local base = knowledge.base(seat)
    local missing = no_base(base)
    local small = count < limit
    if small and not (missing and skip_null) then
        if missing then
            local center = squads.centroid(me, squad) or {x = 0, y = 0, z = 0}
            local target = squads.nearest_enemy(seat, task, center)
            if target ~= nil and type(target.position) == "table" then
                local position = target.position
                squads.order(
                    me,
                    squad,
                    "patrol",
                    true,
                    nil,
                    squads.fixed_point({
                        x = squads.raw_of(position.x),
                        y = squads.raw_of(position.y),
                        z = squads.raw_of(position.z),
                    })
                )
            end
            return
        end
        scout(seat, me, squad, base)
        return
    end
    -- @quirk Under ai.patrol-null-enemy-skip a small wing with no base draws
    -- a map-edge patrol, the same three draws a large wing takes, and patrols
    -- there instead of queueing a patrol on the nearest enemy.
    local world_x, world_z = world_span(seat.game)
    local x, z = edge_of(world_x, world_z)
    squads.order(me, squad, "patrol", false, nil, squads.fixed_point({x = x, y = 0, z = z}))
end

return air
