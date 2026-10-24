-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

-- The ai.* rules Classic plays by. A hack that is off keeps 3.1c's value,
-- even when the table still carries a parameter. Parameters are read only
-- while the hack is on.

local rules = {}

local income_fallback = {0.5, 0.7, 1.0}
local name_fallback = {"easy", "medium", "hard"}

local function hack_row(game, id)
    local row = game:hack(id)
    if type(row) ~= "table" then
        return nil
    end
    return row
end

local function turned_on(row)
    return row ~= nil and row.on == true
end

local function number_on(row, name, fallback)
    if not turned_on(row) then
        return fallback
    end
    local value = row[name]
    if type(value) ~= "number" then
        return fallback
    end
    return value
end

local function text_on(row, name, fallback)
    if not turned_on(row) then
        return fallback
    end
    local value = row[name]
    if type(value) ~= "string" then
        return fallback
    end
    return value
end

local function triple_on(row, name)
    local out = {income_fallback[1], income_fallback[2], income_fallback[3]}
    if not turned_on(row) then
        return out
    end
    local list = row[name]
    if type(list) ~= "table" then
        return out
    end
    for index = 1, 3 do
        if type(list[index]) == "number" then
            out[index] = list[index]
        end
    end
    return out
end

local function names_on(row)
    local out = {name_fallback[1], name_fallback[2], name_fallback[3]}
    if not turned_on(row) then
        return out
    end
    local list = row.names
    if type(list) ~= "table" then
        return out
    end
    for index = 1, 3 do
        if type(list[index]) == "string" then
            out[index] = list[index]
        end
    end
    return out
end

function rules.load(game)
    local income = hack_row(game, "ai.income-multipliers")
    local names = hack_row(game, "ai.difficulty-names")
    local filter = hack_row(game, "ai.factory-tick-filter")
    local skip_busy = false
    local power_toggle = "makes-metal"
    if turned_on(filter) then
        skip_busy = filter["skip-busy-background-queue"] == true
        if type(filter["power-toggle"]) == "string" then
            power_toggle = filter["power-toggle"]
        end
    end
    return {
        income = {
            production = triple_on(income, "production"),
            reclaim = triple_on(income, "reclaim"),
        },
        difficulty_names = names_on(names),
        squad5_factory_tick = turned_on(hack_row(game, "ai.squad5-factory-tick")),
        factory_tick_filter = {
            skip_busy = skip_busy,
            power_toggle = power_toggle,
        },
        builder_withhold = number_on(hack_row(game, "ai.builder-withhold-threshold"), "builders", 5),
        commander_keeps_orders = turned_on(hack_row(game, "ai.commander-keeps-orders-when-damaged")),
        attack_wave = number_on(hack_row(game, "ai.attack-wave-size"), "units", 6),
        patrol_group = number_on(hack_row(game, "ai.patrol-group-size"), "units", 5),
        patrol_null_enemy_skip = turned_on(hack_row(game, "ai.patrol-null-enemy-skip")),
        squad_assignment = text_on(hack_row(game, "ai.squad-assignment"), "rules", "base"),
        nearest_enemy = text_on(hack_row(game, "ai.nearest-enemy-filter"), "rules", "base"),
    }
end

return rules
