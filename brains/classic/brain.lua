-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

-- Classic's seat: the squad sort's countdown and one task per squad.
-- Each task's first act is to set the tick it runs on next. Structures,
-- construction and an armed factory's tick then choose builds and sites.
-- Strike groups recruit and attack, rallies follow them, the aircraft wing
-- scouts or raids, and the siege searches and attacks a point.

local air_raid = require("tasks.air_raid")
local construction = require("tasks.construction")
local knowledge = require("knowledge")
local profile = require("profile")
local rally = require("tasks.rally")
local report = require("report")
local siege = require("tasks.siege")
local squads = require("squads")
local strike = require("tasks.strike")
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
    if hooks.seat ~= nil then
        -- A second start reloads the profile. The grids, the tasks and the
        -- metal spots stay, and this start draws nothing.
        profile.reload(hooks.seat)
        return
    end
    if saved ~= nil then
        -- A saved seat is accepted. This timetable does not apply it.
        -- A loaded game starts in a new seat, so saved is nil and the
        -- timetable below is the same one a new game builds.
    end
    local loaded_rules = rules.load(game)
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
        air_raid.run(seat, seat.tasks[8])
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
    seat.tasks[2].run = function()
        strike.run(seat, seat.tasks[2])
    end
    seat.tasks[3].run = function()
        rally.run(seat, seat.tasks[3])
    end
    seat.tasks[4].run = function()
        construction.run(seat, seat.tasks[4].squad)
    end
    seat.tasks[5].run = function()
        structures.run(seat, seat.tasks[5].squad)
    end
    seat.tasks[6].run = function()
        strike.run(seat, seat.tasks[6])
    end
    seat.tasks[7].run = function()
        rally.run(seat, seat.tasks[7])
    end
    seat.tasks[9].run = function()
        siege.run(seat, seat.tasks[9])
    end
    knowledge.create(seat)
    if me.handed_over then
        profile.apply_handed(seat)
    else
        profile.apply_all(seat)
    end
    knowledge.scan_metal(seat)
    hooks.seat = seat
end

function hooks.report()
    return report.write(hooks.seat)
end

function hooks.onTick(tick)
    local seat = hooks.seat
    if seat.builds_ready ~= 1 then
        types.refresh_builds(seat.types, seat.game)
        seat.builds_ready = 1
    end
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
