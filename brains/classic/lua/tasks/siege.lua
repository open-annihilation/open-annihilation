-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

-- The siege. With members in its squad it turns one run in ten, steps 320
-- world units, and keeps a seen point whose draw beats the target's. Members
-- that can attack are ordered onto that point; one that cannot move only
-- when its first weapon reaches. The timetable draw is already taken.

local squads = require("squads")

local siege = {}

local search_radius = 160
local step_whole = 320
local turn_odds = 10

local function copy_point(point)
    return {x = point.x, y = point.y, z = point.z}
end

local function can_attack(type_index)
    if type(type_index) ~= "number" or type_index < 1 then
        return false
    end
    local catalog = units
    if catalog == nil then
        return false
    end
    local row = catalog[type_index]
    if row == nil then
        return false
    end
    return row.canattack == true
end

function siege.run(seat, task)
    local me = seat.me
    local count = squads.count(me, task.squad)
    if count < 1 or me == nil or type(me.squad_member) ~= "function" then
        return
    end
    if brain.shared_random(turn_odds) == 0 then
        task.siege_probe = copy_point(task.siege_target)
        local angle = squads.u32(brain.shared_random(0x10000)) & 0xFFFF
        local sine = geometry.sine_scaled(angle, step_whole)
        local cosine = geometry.cosine_scaled(angle, step_whole)
        task.siege_step = {
            x = squads.wrap_sub(0, squads.raw_of(sine)),
            y = 0,
            z = squads.wrap_sub(0, squads.raw_of(cosine)),
        }
    end
    local probe = task.siege_probe
    local step = task.siege_step
    task.siege_probe = {
        x = squads.wrap_add(probe.x, step.x),
        y = squads.wrap_add(probe.y, step.y),
        z = squads.wrap_add(probe.z, step.z),
    }
    probe = task.siege_probe
    if me:sees(fixed.raw(probe.x), fixed.raw(probe.z), fixed.raw(probe.y)) then
        local weight = squads.sighted_weight(seat, probe, search_radius)
        local draw = brain.shared_random(squads.u32(weight))
        local held = brain.shared_random(squads.u32(task.siege_weight))
        if squads.i32(draw) > squads.i32(held) then
            task.siege_target = copy_point(probe)
            task.siege_weight = weight
        end
    end
    local target = squads.fixed_point(task.siege_target)
    for index = 1, count do
        local unit = me:squad_member(task.squad, index)
        if unit ~= nil and can_attack(unit.type_index) then
            local reaches = true
            if not unit.mobile then
                reaches = unit:reaches(target) == true
            end
            if reaches then
                unit:attack_point(target)
            end
        end
    end
end

return siege
