-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

-- Squad sort. Selectable units in the seat's range take standing orders,
-- and a unit still in squad 0 joins the squad its type sorts into.
-- A unit already in a squad, including squad -1, keeps that squad.

local types = require("types")

local squads = {}

local flag_builder = 0x00000040
local flag_can_fly = 0x00000800
local flag_amphibious = 0x00200000
local ability_capture = 0x00001000
local ability_commander = 0x00040000

local squad_none = 0
local squad_structures = 1
local squad_land_army = 3
local squad_builders = 4
local squad_armed_structures = 5
local squad_navy = 7
local squad_aircraft = 8

local role_energy_high_word = 0x4269
local role_navy_depth = 128

local function assignment_of(rules)
    if type(rules) == "string" then
        return rules
    end
    if type(rules) == "table" and type(rules.squad_assignment) == "string" then
        return rules.squad_assignment
    end
    return "base"
end

-- The float's high 16 bits, read as a signed number.
local function energy_high_word(energy)
    local bits = types.bits(energy)
    local word = (bits >> 16) & 0xFFFF
    if word >= 0x8000 then
        word = word - 0x10000
    end
    return word
end

local function has_bit(value, mask)
    if type(value) ~= "number" then
        return false
    end
    return (value & mask) ~= 0
end

function squads.sort_squad(unit, type_row, rules)
    local assignment = assignment_of(rules)
    local flags = 0
    local min_water = 0
    local max_water = 0
    if type(type_row) == "table" then
        flags = type_row.flags or 0
        min_water = type_row.min_water_depth or 0
        max_water = type_row.max_water_depth or 0
    end
    if assignment == "role-squads" then
        if unit.building then
            local spends = energy_high_word(type_row and type_row.energy_use or nil) > role_energy_high_word
            if has_bit(flags, flag_builder) or spends then
                return squad_structures
            end
            return squad_armed_structures
        end
        if has_bit(flags, flag_builder) then
            return squad_builders
        end
        if has_bit(flags, flag_can_fly) then
            return squad_aircraft
        end
        if min_water > 0 or max_water >= role_navy_depth or has_bit(flags, flag_amphibious) then
            return squad_navy
        end
        return squad_land_army
    end
    if unit.building then
        if unit.armed then
            return squad_armed_structures
        end
        return squad_structures
    end
    if has_bit(flags, flag_builder) then
        return squad_builders
    end
    if has_bit(flags, flag_can_fly) then
        return squad_aircraft
    end
    if min_water > 0 then
        return squad_navy
    end
    if unit.armed then
        return squad_land_army
    end
    return squad_none
end

function squads.sort(seat)
    local assignment = assignment_of(seat.rules)
    local manoeuvre_ability = ability_capture
    if assignment == "role-squads" then
        manoeuvre_ability = ability_commander
    end
    local list = seat.me:units()
    for index = 1, #list do
        local unit = list[index]
        if unit.selectable then
            local type_index = unit.type_index
            local type_row = nil
            if type(type_index) == "number" then
                type_row = seat.types[type_index]
            end
            if type_row ~= nil then
                local move = "roam"
                if has_bit(type_row.abilities, manoeuvre_ability) then
                    move = "manoeuvre"
                end
                unit:stance({move = move, fire = "at_will", direct = true})
                if unit.squad == 0 then
                    local chosen = squads.sort_squad(unit, type_row, assignment)
                    if chosen ~= squad_none then
                        seat.me:assign(unit, chosen)
                    end
                end
            end
        end
    end
end

-- Whole-unit words, distances and squad orders shared by the strike, rally,
-- aircraft and siege tasks. Sums and products stay in 32 bits.

local squad_navy = 7
local max_recruits = 5000

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

local function sign8(value)
    local bits = u32(value) & 0xFF
    if bits >= 0x80 then
        return bits - 0x100
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

local function raw_of(component)
    if type(component) == "userdata" or type(component) == "table" then
        local method = component.raw
        if type(method) == "function" then
            return i32(method(component))
        end
    end
    return i32(component or 0)
end

local function high_word(component)
    return sign16(u32(component) >> 16)
end

-- The high 32 bits of a squared 16.16 offset, as a signed number.
local function square_high(delta)
    local product = delta * delta
    return i32(product >> 32)
end

local function word_of(unit)
    if unit == nil or type(unit.position) ~= "table" then
        return 0, 0, 0
    end
    local position = unit.position
    return high_word(raw_of(position.x)), high_word(raw_of(position.y)), high_word(raw_of(position.z))
end

local function spread(dx, dz)
    return wrap_add(wrap_mul(dz, dz), wrap_mul(dx, dx))
end

function squads.count(me, squad)
    if me == nil or type(me.squad_size) ~= "function" then
        return 0
    end
    local count = me:squad_size(squad)
    if type(count) ~= "number" then
        return 0
    end
    return count
end

function squads.fixed_point(point)
    return {
        x = fixed.raw(point.x or 0),
        y = fixed.raw(point.y or 0),
        z = fixed.raw(point.z or 0),
    }
end

-- Average of the squad's positions from the signed high words, back in 16.16.
-- An empty squad has no centre.
function squads.centroid(me, squad)
    local count = squads.count(me, squad)
    if count < 1 or me == nil or type(me.squad_member) ~= "function" then
        return nil
    end
    local x = 0
    local y = 0
    local z = 0
    for index = 1, count do
        local wx, wy, wz = word_of(me:squad_member(squad, index))
        x = wrap_add(x, wx)
        y = wrap_add(y, wy)
        z = wrap_add(z, wz)
    end
    return {
        x = wrap_mul(div_tz(x, count), 0x10000),
        y = wrap_mul(div_tz(y, count), 0x10000),
        z = wrap_mul(div_tz(z, count), 0x10000),
    }
end

-- Armed structures, else structures, else builders.
function squads.home(me)
    local armed = squads.centroid(me, squad_armed_structures)
    if armed ~= nil then
        return armed
    end
    local structures = squads.centroid(me, squad_structures)
    if structures ~= nil then
        return structures
    end
    return squads.centroid(me, squad_builders)
end

-- The engine's nearest-enemy search. Under skip-submerged every task but the
-- naval strike passes over a fully submerged unit.
function squads.nearest_enemy(seat, task, at)
    local me = seat.me
    if me == nil or type(me.nearest_enemy) ~= "function" or type(at) ~= "table" then
        return nil
    end
    local filter = nil
    local rules = seat.rules
    if type(rules) == "table" and rules.nearest_enemy == "skip-submerged" then
        local source = 0
        if type(task) == "table" and type(task.source) == "number" then
            source = task.source
        end
        filter = {widened = true, skip_submerged = source ~= squad_navy}
    end
    return me:nearest_enemy(fixed.raw(at.x or 0), fixed.raw(at.z or 0), filter)
end

-- Every unit of this squad in the seat's range, in slot order.
function squads.order(me, squad, command, queued, target, at)
    if me == nil or type(me.units) ~= "function" then
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
            elseif command == "patrol" then
                unit:patrol(at, queued)
            elseif command == "attack" then
                unit:attack(target)
            end
        end
    end
end

-- Seed an empty squad, shed whoever stands farther than count times the
-- radius, then take source members inside that same distance.
function squads.recruit(me, task, source, radius)
    if me == nil or type(task) ~= "table" or source == task.squad then
        return
    end
    if type(me.squad_member) ~= "function" or type(me.assign) ~= "function" then
        return
    end
    if squads.count(me, task.squad) == 0 then
        if squads.count(me, source) == 0 then
            return
        end
        local seed = me:squad_member(source, 1)
        if seed == nil then
            return
        end
        me:assign(seed, task.squad)
    end
    local sum_x = 0
    local sum_z = 0
    local members = squads.count(me, task.squad)
    for index = 1, members do
        local wx, _, wz = word_of(me:squad_member(task.squad, index))
        sum_x = wrap_add(sum_x, wx)
        sum_z = wrap_add(sum_z, wz)
    end
    local center_x = 0
    local center_z = 0
    while true do
        members = squads.count(me, task.squad)
        if members < 1 then
            break
        end
        center_x = div_tz(sum_x, members)
        center_z = div_tz(sum_z, members)
        if members == 1 then
            break
        end
        local farthest = 0
        local straggler = nil
        local straggler_x = 0
        local straggler_z = 0
        for index = 1, members do
            local unit = me:squad_member(task.squad, index)
            local wx, _, wz = word_of(unit)
            local distance = spread(wrap_sub(wx, center_x), wrap_sub(wz, center_z))
            if farthest < distance then
                farthest = distance
                straggler = unit
                straggler_x = wx
                straggler_z = wz
            end
        end
        if farthest < wrap_mul(members, radius) then
            break
        end
        if straggler == nil then
            break
        end
        me:assign(straggler, source)
        sum_x = wrap_sub(sum_x, straggler_x)
        sum_z = wrap_sub(sum_z, straggler_z)
    end
    local own = squads.count(me, task.squad)
    local candidates = squads.count(me, source)
    local limit = wrap_mul(own, radius)
    local joining = {}
    for index = 1, candidates do
        if #joining >= max_recruits then
            break
        end
        local unit = me:squad_member(source, index)
        local wx, _, wz = word_of(unit)
        if unit ~= nil and spread(wrap_sub(wx, center_x), wrap_sub(wz, center_z)) < limit then
            joining[#joining + 1] = unit
        end
    end
    for index = 1, #joining do
        me:assign(joining[index], task.squad)
    end
end

-- Signed base weights of seen units, dead ones included, within the radius
-- on the ground. A type outside the table adds nothing. The sum wraps in 32 bits.
function squads.sighted_weight(seat, at, radius)
    local me = seat.me
    if me == nil or type(me.seen_raw) ~= "function" or type(at) ~= "table" then
        return 0
    end
    local seen = me:seen_raw()
    if type(seen) ~= "table" then
        return 0
    end
    local type_count = 1
    if type(seat.types) == "table" then
        type_count = #seat.types + 1
    end
    local weights = seat.base_weights
    local reach = wrap_mul(radius, radius)
    local ax = raw_of(at.x)
    local az = raw_of(at.z)
    local sum = 0
    for index = 1, #seen do
        local row = seen[index]
        if type(row) == "table" then
            local type_index = row.type_index or 0
            local distance = wrap_add(
                square_high(wrap_sub(az, raw_of(row.z))),
                square_high(wrap_sub(ax, raw_of(row.x)))
            )
            if not (distance > reach) and type_index < type_count and type(weights) == "table" then
                local weight = weights[type_index]
                if type(weight) == "number" then
                    sum = u32(sum + u32(sign8(weight)))
                end
            end
        end
    end
    return i32(sum)
end

squads.u32 = u32
squads.i32 = i32
squads.div_tz = div_tz
squads.wrap_add = wrap_add
squads.wrap_sub = wrap_sub
squads.wrap_mul = wrap_mul
squads.raw_of = raw_of

return squads
