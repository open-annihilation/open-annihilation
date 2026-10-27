-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

-- Reads the seat's economy every turn and, unless told not to, has its first
-- builder put up one energy building. It has full awareness, so its site
-- search does not wait on what the seat has seen. Each turn appends a row to `rows`,
-- the figures' bits, for the test to read back. A turn whose figures differ
-- from the turn before prints them, so a headless brain match leaves them in
-- the seat's log. The name is not `brain`: that would hide the global.
local hooks = {}
rows = {}
ordered = nil
local last = nil

local function real_of(record, key)
    local value = record[key]
    if type(value) == "userdata" and type(value.tonumber) == "function" then
        return value:tonumber()
    end
    return type(value) == "number" and value or 0
end

-- A building that makes more energy than it uses: EnergyMake above its
-- EnergyUse, or a negative EnergyUse as a solar collector has.
local function makes_energy(record)
    return record.bmcode == 0 and real_of(record, "energymake") - real_of(record, "energyuse") > 0
end

local function first_builder()
    local found = nil
    me:each_unit(function(unit)
        if found == nil and unit.finished == true and unit.type ~= nil and unit.type.builder == true
            and unit.type.bmcode == 1 then
            found = unit
        end
    end)
    return found
end

local function order_build()
    if ordered ~= nil or me.options.build == false then return end
    local builder = first_builder()
    if builder == nil or builder.x == nil then return end
    local list = game.build_list(builder.type)
    if type(list) ~= "table" then return end
    for index = 1, #list do
        local record = list[index]
        if makes_energy(record) then
            local _, _, x, z = brain.place(record, { x = builder.x, z = builder.z }, 256, 0)
            if x ~= nil and builder:build(record, { x = x, z = z }, 0) == true then
                ordered = record.unitname
                print("ordered " .. tostring(ordered))
                return
            end
        end
    end
end

local function figures()
    return {
        tick = me.tick,
        energy = me.energy:bits(),
        metal = me.metal:bits(),
        energy_produced = me.energy_produced:bits(),
        energy_requested = me.energy_requested:bits(),
        energy_spent = me.energy_spent:bits(),
        metal_produced = me.metal_produced:bits(),
        metal_requested = me.metal_requested:bits(),
        metal_spent = me.metal_spent:bits(),
    }
end

local function same(a, b)
    return b ~= nil and a.energy_produced == b.energy_produced
        and a.energy_requested == b.energy_requested and a.energy_spent == b.energy_spent
        and a.metal_produced == b.metal_produced and a.metal_requested == b.metal_requested
        and a.metal_spent == b.metal_spent
end

local function shown(value)
    return string.format("%.2f", value:tonumber())
end

local function report()
    print("energy produced " .. shown(me.energy_produced) .. " requested "
        .. shown(me.energy_requested) .. " spent " .. shown(me.energy_spent) .. " surplus "
        .. string.format("%.2f", me.energy_surplus) .. " store " .. shown(me.energy))
    print("metal produced " .. shown(me.metal_produced) .. " requested "
        .. shown(me.metal_requested) .. " spent " .. shown(me.metal_spent) .. " surplus "
        .. string.format("%.2f", me.metal_surplus) .. " store " .. shown(me.metal))
end

function hooks.fits()
    return true
end

function hooks.start()
    rows, ordered, last = {}, nil, nil
end

function hooks.onTick()
    order_build()
    local now = figures()
    rows[#rows + 1] = now
    if not same(now, last) then
        report()
    end
    last = now
end

return hooks
