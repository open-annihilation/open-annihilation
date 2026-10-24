-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

-- Classic's seat: the squad sort's countdown and one task per squad.
-- Each task's first act is to set the tick it runs on next. Structures,
-- construction and an armed factory's tick then choose builds and sites.
-- The aircraft wing scouts or raids. Strike, rally and siege keep the
-- timetable.

local air = require("tasks.air")
local construction = require("tasks.construction")
local knowledge = require("knowledge")
local profile = require("profile")
local squads = require("squads")
local structures = require("tasks.structures")
local types = require("types")
local rules = require("rules")

local hooks = {}

local function toward_zero_half(whole)
    if type(whole) ~= "number" then
        return 0
    end
    local value = math.tointeger(whole)
    if value == nil then
        value = math.modf(whole)
    end
    if value < 0 then
        return -((-value) // 2)
    end
    return value // 2
end

local function signed32(value)
    local bits = value & 0xFFFFFFFF
    if bits >= 0x80000000 then
        return bits - 0x100000000
    end
    return bits
end

-- Map centre in 16.16, from half the world width and height in whole units.
local function map_centre(game)
    local width = 0
    local height = 0
    local map = game.map
    if type(map) == "userdata" or type(map) == "table" then
        width = map.world_width or 0
        height = map.world_height or 0
    end
    return {
        x = signed32(toward_zero_half(width) * 65536),
        y = 0,
        z = signed32(toward_zero_half(height) * 65536),
    }
end

local function point_of(centre)
    return {x = centre.x, y = centre.y, z = centre.z}
end

local function blank(squad)
    return {
        kind = "none",
        squad = squad,
        next = 0,
        min_size = 0,
        launch = 0,
        merge_radius = 0,
        source = 0,
        attacking = 0,
        every = 0,
        run = function() end,
    }
end

function hooks.start(game, me, saved)
    if saved ~= nil then
        -- A saved seat is accepted. This timetable does not apply it.
    end
    local loaded_rules = rules.load(game)
    local previous = hooks.seat
    local seat = {
        player = me.player,
        me = me,
        game = game,
        sort_countdown = 30,
        rules = loaded_rules,
        types = types.load(game),
        tasks = {},
    }
    for index = 0, 9 do
        seat.tasks[index] = blank(index)
    end
    local wave = loaded_rules.attack_wave
    seat.tasks[1].kind = "structures"
    seat.tasks[1].every = 30
    seat.tasks[2].kind = "strike"
    seat.tasks[2].min_size = 3
    seat.tasks[2].launch = wave
    seat.tasks[2].merge_radius = 20000
    seat.tasks[2].source = 3
    seat.tasks[2].every = 300
    seat.tasks[3].kind = "rally"
    seat.tasks[3].source = 2
    seat.tasks[3].every = 150
    seat.tasks[4].kind = "construction"
    seat.tasks[4].every = 90
    if loaded_rules.squad5_factory_tick then
        seat.tasks[5].kind = "structures"
        seat.tasks[5].every = 30
    else
        seat.tasks[5].kind = "idle"
    end
    seat.tasks[6].kind = "strike"
    seat.tasks[6].min_size = 3
    seat.tasks[6].launch = wave
    seat.tasks[6].merge_radius = 50000
    seat.tasks[6].source = 7
    seat.tasks[6].every = 300
    seat.tasks[7].kind = "rally"
    seat.tasks[7].source = 6
    seat.tasks[7].every = 150
    seat.tasks[8].kind = "air_raid"
    seat.tasks[8].every = function()
        return 30 + brain.shared_random(900)
    end
    seat.tasks[8].run = function()
        air.run(seat, seat.tasks[8].squad)
    end
    -- @quirk The sort never fills squad 9, so the siege task only keeps its
    -- timetable unless a unit was already in that squad.
    local centre = map_centre(game)
    seat.tasks[9].kind = "siege"
    seat.tasks[9].every = function()
        return 30 + brain.shared_random(150)
    end
    seat.tasks[9].siege_target = point_of(centre)
    seat.tasks[9].siege_probe = point_of(centre)
    seat.tasks[9].siege_step = point_of(centre)
    seat.tasks[9].siege_weight = 0
    seat.tasks[1].run = function()
        structures.run(seat, seat.tasks[1].squad)
    end
    seat.tasks[4].run = function()
        construction.run(seat, seat.tasks[4].squad)
    end
    seat.tasks[5].run = function()
        structures.run(seat, seat.tasks[5].squad)
    end
    knowledge.create(seat)
    if previous ~= nil then
        seat.plan = previous.plan or 1
        seat.grids_ready = previous.grids_ready or 0
        seat.land_grid = previous.land_grid
        seat.water_grid = previous.water_grid
        seat.metal = previous.metal
        seat.placement_radius = previous.placement_radius or 0
        profile.reload(seat)
    else
        if me.handed_over then
            profile.apply_handed(seat)
        else
            profile.apply_all(seat)
        end
        knowledge.scan_metal(seat)
    end
    hooks.seat = seat
end

function hooks.onTick(tick)
    local seat = hooks.seat
    if seat.grids_ready ~= 1 then
        knowledge.seed_grids(seat)
    end
    seat.sort_countdown = seat.sort_countdown - 1
    if seat.sort_countdown < 1 then
        seat.sort_countdown = 30
        brain.section("sort")
        squads.sort(seat)
    end
    for index = 0, 9 do
        local task = seat.tasks[index]
        if task.kind ~= "none" and task.next <= tick then
            if task.kind ~= "idle" then
                brain.section("task" .. index)
                local interval = task.every
                if type(interval) == "function" then
                    interval = interval()
                end
                task.next = (tick + interval) & 0xFFFFFFFF
                task.run()
            end
        end
    end
end

return hooks
