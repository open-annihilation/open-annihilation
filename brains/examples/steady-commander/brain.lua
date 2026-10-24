-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

-- Economy, then one land factory. Combat units gather until the attack
-- option's count is met, then go to the nearest enemy in sight.
-- save() keeps how many squads have been sent.

local hooks = {}
local sent, armed = 0, false
local commander, metal_type, energy_type, factory_type, combat_type
local metal_done, energy_done, factory_ordered = 0, 0, false
local factories, squad, sent_ids = {}, {}, {}
local ready = false

local function real_of(record, key)
    local value = record[key]
    if type(value) == "userdata" and type(value.tonumber) == "function" then
        return value:tonumber()
    end
    return type(value) == "number" and value or 0
end

local function positive(record, key)
    return real_of(record, key) > 0
end

local function words(record)
    local found, list = {}, record.category
    if type(list) ~= "table" then return found end
    for index = 1, #list do found[list[index]] = true end
    return found
end

-- A category the commander lacks, or no categories, so a copy is skipped.
local function labelled(record, commander_words)
    local any, fresh = false, false
    for word in pairs(words(record)) do
        any, fresh = true, fresh or commander_words[word] ~= true
    end
    return fresh or not any
end

local function land_combat(record)
    return record.canmove == true and record.canattack == true and record.builder ~= true
        and record.canfly ~= true and real_of(record, "minwaterdepth") <= 0
end

local function energy_record(record)
    return positive(record, "energymake") or positive(record, "makesmetal")
        or positive(record, "windgenerator") or positive(record, "tidalgenerator")
end

local function pick(list)
    if #list == 0 then return nil end
    return list[brain.random(#list)]
end

local function classify(list, commander_words)
    local metals, energies, plants = {}, {}, {}
    for index = 1, #list do
        local record = list[index]
        if record.commander ~= true and labelled(record, commander_words) then
            if positive(record, "extractsmetal") then
                metals[#metals + 1] = record
            elseif energy_record(record) then
                energies[#energies + 1] = record
            elseif record.builder == true and record.bmcode == 0
                and real_of(record, "minwaterdepth") <= 0 then
                local built, fights = game.build_list(record), {}
                if type(built) == "table" then
                    for slot = 1, #built do
                        if land_combat(built[slot]) then fights[#fights + 1] = built[slot] end
                    end
                end
                local fight = pick(fights)
                if fight ~= nil then plants[#plants + 1] = { plant = record, unit = fight } end
            end
        end
    end
    return metals, energies, plants
end

local function squad_size()
    if me.options.attack == "soon" then return 4 end
    if me.options.attack == "late" then return 12 end
    return 8
end

local function plan_counts()
    if me.difficulty == "easy" then return 1, 1 end
    if me.difficulty == "hard" then return 2, 2 end
    return 2, 1
end

local function find_commander()
    local found = nil
    me:each_unit(function(unit)
        if found == nil and unit.type ~= nil and unit.type.commander == true then found = unit end
    end)
    return found
end

local function free(unit)
    local kind, _, idle = unit:head_order()
    return kind == nil or idle == true
end

local function place_build(builder, record)
    if builder.position == nil then return false end
    local facing = brain.random(4) - 1
    local _, _, x, z = brain.place(record, builder.position, 2048, facing)
    if x == nil then return false end
    return builder:build(record, { x = x, z = z }, facing) == true
end

local function note_finished(unit)
    local record = unit.type
    if record == nil or record.commander == true or unit.alive ~= true then return end
    if positive(record, "extractsmetal") then metal_done = metal_done + 1
    elseif energy_record(record) then energy_done = energy_done + 1
    elseif record.builder == true and record.bmcode == 0 then
        factories[unit.id], factory_ordered = unit, true
    elseif land_combat(record) and sent_ids[unit.id] ~= true then squad[unit.id] = unit
    end
end

local function nearest(origin)
    local best, best_distance
    for index = 1, #me.seen do
        local unit = me.seen[index]
        if unit.alive == true and unit.owner ~= me.player and me:allied(unit.owner) ~= true then
            local distance = brain.distance(origin, unit)
            if distance ~= nil and (best_distance == nil or distance < best_distance) then
                best, best_distance = unit, distance
            end
        end
    end
    return best
end

local function count_squad()
    local count = 0
    for _ in pairs(squad) do count = count + 1 end
    return count
end

local function try_send()
    if count_squad() < squad_size() then return end
    local origin = (commander ~= nil and commander.alive == true) and commander or nil
    if origin == nil then
        for _, unit in pairs(squad) do origin = unit break end
    end
    local target = origin ~= nil and nearest(origin) or nil
    if target == nil then return end
    for id, unit in pairs(squad) do
        if unit.alive == true then unit:attack(target) end
        sent_ids[id] = true
    end
    squad, sent = {}, sent + 1
end

local function prepare()
    if ready then return end
    if commander == nil or commander.alive ~= true then commander = find_commander() end
    if commander == nil or commander.type == nil then return end
    local list = game.build_list(commander.type)
    if type(list) ~= "table" or #list == 0 then return end
    local metals, energies, plants = classify(list, words(commander.type))
    metal_type, energy_type = pick(metals), pick(energies)
    local plant = pick(plants)
    if plant ~= nil then factory_type, combat_type = plant.plant, plant.unit end
    ready = metal_type ~= nil or energy_type ~= nil or factory_type ~= nil
end

local function build_next()
    prepare()
    if commander == nil or commander.alive ~= true then commander = find_commander() end
    if commander == nil or not free(commander) then return end
    local metal_need, energy_need = plan_counts()
    local record, factory = nil, false
    if metal_type ~= nil and metal_done < metal_need then record = metal_type
    elseif energy_type ~= nil and energy_done < energy_need then record = energy_type
    elseif factory_type ~= nil and not factory_ordered then record, factory = factory_type, true
    end
    if record ~= nil and place_build(commander, record) and factory then factory_ordered = true end
end

local function queue_plants()
    if combat_type == nil then return end
    for _, plant in pairs(factories) do
        if plant.alive == true and free(plant) then plant:queue(combat_type) end
    end
end

local function step()
    build_next()
    queue_plants()
    try_send()
end

function hooks.fits() return true end

function hooks.start(_, _, saved)
    sent = type(saved) == "table" and type(saved.sent) == "number" and saved.sent or 0
    metal_done, energy_done, factory_ordered, factories, squad = 0, 0, false, {}, {}
    metal_type, energy_type, factory_type, combat_type = nil, nil, nil, nil
    ready, commander = false, find_commander()
    prepare()
    me:each_unit(function(unit)
        if unit.finished == true then note_finished(unit) end
    end)
    if not armed then
        brain.every(30, step)
        armed = true
    end
end

function hooks.onUnitFinished(unit)
    note_finished(unit)
    try_send()
end

function hooks.onUnitIdle(unit)
    local record = unit.type
    if record == nil then return end
    if record.commander == true then
        commander = unit
        build_next()
    elseif record.builder == true and record.bmcode == 0 then
        factories[unit.id] = unit
        queue_plants()
    end
end

function hooks.save() return { sent = sent } end

return hooks
