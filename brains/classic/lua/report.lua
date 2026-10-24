-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

-- The console's per-player weight report. The clock is 30 ticks a second.
-- A name stops at the first NUL and at its field width. An empty path is
-- the word (null). Each row is one type the seat and the world both have.

local report = {}

local ticks_per_second = 30
local ticks_per_minute = 60 * ticks_per_second
local ticks_per_hour = 60 * ticks_per_minute
local name_width = 30
local unit_name_width = 32

local function whole(value)
    if type(value) ~= "number" then
        return 0
    end
    local integer = math.tointeger(value)
    if integer ~= nil then
        return integer
    end
    local toward_zero = math.modf(value)
    return toward_zero
end

local function cut_field(value, width)
    if type(value) ~= "string" then
        return ""
    end
    local zero = string.find(value, "\0", 1, true)
    if zero ~= nil then
        value = string.sub(value, 1, zero - 1)
    end
    if #value > width then
        return string.sub(value, 1, width)
    end
    return value
end

local function printable(value)
    if type(value) ~= "string" then
        return "(null)"
    end
    return value
end

local function controller_of(me)
    if me == nil or type(me.players) ~= "function" then
        return "INVALID"
    end
    local players = me:players()
    if type(players) ~= "table" then
        return "INVALID"
    end
    local slot = whole(me.player)
    for index = 1, #players do
        local row = players[index]
        if type(row) == "table" and whole(row.player) == slot then
            if row.computer == true then
                return "AI"
            end
            return "HUMAN"
        end
    end
    return "INVALID"
end

local function challenge_of(seat)
    local game = seat.game
    local word = nil
    if game ~= nil then
        word = game.difficulty
    end
    local number = nil
    if word == "easy" then
        number = 0
    elseif word == "medium" then
        number = 1
    elseif word == "hard" then
        number = 2
    end
    if number == nil then
        return "(null)"
    end
    local names = nil
    if type(seat.rules) == "table" then
        names = seat.rules.difficulty_names
    end
    if type(names) ~= "table" or type(names[number + 1]) ~= "string" then
        return "(null)"
    end
    local upper = string.upper(names[number + 1])
    if upper == "EASY" or upper == "MEDIUM" or upper == "HARD" then
        return upper
    end
    return "(null)"
end

local function path_of(game, field)
    if game == nil then
        return nil
    end
    if field == "map" then
        local map = game.map
        if map == nil then
            return nil
        end
        return map.path
    end
    local profile = game.ai_profile
    if profile == nil then
        return nil
    end
    return profile.report_path
end

function report.write(seat)
    if type(seat) ~= "table" then
        return ""
    end
    local me = seat.me
    local tick = whole(me and me.tick)
    if tick < 0 then
        tick = 0
    end
    local within_hour = tick % ticks_per_hour
    local lines = {
        string.format(
            "Match clock: %02d:%02d:%02d\r\n",
            tick // ticks_per_hour,
            within_hour // ticks_per_minute,
            (within_hour % ticks_per_minute) // ticks_per_second
        ),
        string.format(
            "Name: '%s' in player slot %d\r\n",
            cut_field(me and me.name, name_width),
            whole(me and me.player)
        ),
        string.format("Played by: %s\r\n", controller_of(me)),
        string.format("Map file: '%s'\r\n", printable(path_of(seat.game, "map"))),
        string.format("AI settings file: '%s'\r\n", printable(path_of(seat.game, "profile"))),
        string.format("Challenge level: '%s'\r\n", challenge_of(seat)),
        "================================================\r\n",
        "Columns: build limit - priority : metal value : energy value = weight percent before "
            .. "economy adjustment - short name : full name\r\n",
    }
    if type(seat.weight_percent) ~= "table" or type(seat.limits) ~= "table" then
        return table.concat(lines)
    end
    local type_count = 1
    if type(seat.types) == "table" then
        type_count = #seat.types + 1
    end
    local catalog = units
    local world_count = 1
    if type(catalog) == "table" or type(catalog) == "userdata" then
        world_count = #catalog + 1
    end
    local last = type_count
    if world_count < last then
        last = world_count
    end
    for type_index = 1, last - 1 do
        local limit = whole(seat.limits[type_index])
        local limit_text = "n/a "
        if limit >= 0 then
            limit_text = string.format("%4d", limit)
        end
        local priority = 0
        local metal = 0
        local energy = 0
        if type(brain.strengths) == "function" then
            local first, second, third = brain.strengths(type_index)
            if first ~= nil then
                priority = whole(first)
                metal = whole(second)
                energy = whole(third)
            end
        end
        local short_name = ""
        local long_name = ""
        if catalog ~= nil then
            local unit = catalog[type_index]
            if unit ~= nil then
                short_name = cut_field(unit.unitname, unit_name_width)
                long_name = cut_field(unit.name, unit_name_width)
            end
        end
        lines[#lines + 1] = string.format(
            "%s - %3d : %3d : %3d = %3d - '%s\t\t:%s'\r\n",
            limit_text,
            priority,
            metal,
            energy,
            whole(seat.weight_percent[type_index]),
            short_name,
            long_name
        )
    end
    return table.concat(lines)
end

return report
