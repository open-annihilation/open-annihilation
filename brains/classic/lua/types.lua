-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

-- Type fields the computer player reads, one row per type index from 1.
-- Names are cut to the bytes the player keeps. EnergyUse and ExtractsMetal
-- stay float values so their bits can be read unchanged.

local types = {}

local flag_downloadable = 0x00000020
local flag_builder = 0x00000040
local flag_can_fly = 0x00000800
local flag_amphibious = 0x00200000
local ability_capture = 0x00001000
local ability_commander = 0x00040000

function types.bits(value)
    if value == nil then
        return 0
    end
    local method = value.bits
    if type(method) ~= "function" then
        return 0
    end
    return method(value)
end

local function text_of(value)
    if type(value) ~= "string" then
        return ""
    end
    local zero = string.find(value, "\0", 1, true)
    if zero == nil then
        return value
    end
    return string.sub(value, 1, zero - 1)
end

local function cut(value, limit)
    local text = text_of(value)
    if #text <= limit then
        return text
    end
    return string.sub(text, 1, limit)
end

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

local function flag_bit(value, mask)
    if value then
        return mask
    end
    return 0
end

-- Categories are joined by single spaces. A category that would take the
-- text past 255 bytes, counting the space before it and the terminating
-- NUL, is left off, and so is every category after it.
local function categories_of(list)
    if type(list) ~= "table" then
        return ""
    end
    local used = 0
    local parts = {}
    for index = 1, #list do
        local item = list[index]
        if type(item) ~= "string" then
            item = ""
        end
        local size = #item
        if used + size + 2 > 256 then
            break
        end
        if used ~= 0 then
            used = used + 1
        end
        used = used + size
        parts[#parts + 1] = item
    end
    return table.concat(parts, " ")
end

local function build_ids_of(game, index, builder)
    if builder ~= 1 or game == nil or type(game.build_list) ~= "function" then
        return {}
    end
    local list = game.build_list(index)
    if type(list) ~= "table" then
        return {}
    end
    local ids = {}
    for slot = 1, #list do
        local entry = list[slot]
        local id = 0
        if (type(entry) == "userdata" or type(entry) == "table") and type(entry.index) == "function" then
            id = whole(entry:index())
        end
        ids[#ids + 1] = id
    end
    return ids
end

local function row_of(record, game, index)
    local row = {
        unit_name = "",
        side = "",
        categories = "",
        ai_weight = "",
        flags = 0,
        abilities = 0,
        bm_code = 0,
        makes_metal = 0,
        min_water_depth = 0,
        max_water_depth = 0,
        energy_use = nil,
        footprint_x = 0,
        footprint_z = 0,
        extracts_metal = nil,
        downloadable = 0,
        has_build_list = 0,
        build_ids = {},
        build_count = 0,
    }
    if type(record) ~= "userdata" and type(record) ~= "table" then
        return row
    end
    row.unit_name = cut(record.unitname, 31)
    row.side = cut(record.side, 31)
    row.categories = categories_of(record.category)
    row.ai_weight = cut(record.ai_weight, 63)
    row.downloadable = flag_bit(record.downloadable, 1)
    row.has_build_list = flag_bit(record.builder, 1)
    row.flags = flag_bit(record.builder, flag_builder)
        | flag_bit(record.canfly, flag_can_fly)
        | flag_bit(record.amphibious, flag_amphibious)
        | flag_bit(record.downloadable, flag_downloadable)
    row.abilities = flag_bit(record.cancapture, ability_capture)
        | flag_bit(record.commander, ability_commander)
    row.build_ids = build_ids_of(game, index, row.has_build_list)
    row.build_count = #row.build_ids
    row.bm_code = whole(record.bmcode)
    row.makes_metal = whole(record.makesmetal)
    row.min_water_depth = whole(record.minwaterdepth)
    row.max_water_depth = whole(record.maxwaterdepth)
    row.energy_use = record.energyuse
    row.footprint_x = whole(record.footprintx)
    row.footprint_z = whole(record.footprintz)
    row.extracts_metal = record.extractsmetal
    return row
end

function types.load(game)
    local loaded = {}
    if game == nil then
        return loaded
    end
    -- `units` is the type catalog. `game.units` is the live unit list and
    -- does not answer a length.
    local catalog = units
    if catalog == nil then
        return loaded
    end
    local count = #catalog
    for index = 1, count do
        loaded[index] = row_of(catalog[index], game, index)
    end
    return loaded
end

-- Builder lists are published when the match sets its computer players up,
-- which is after start and before the first tick. A start that runs earlier
-- keeps empty lists, so the first tick reads them again.
function types.refresh_builds(loaded, game)
    if type(loaded) ~= "table" then
        return
    end
    for index = 1, #loaded do
        local row = loaded[index]
        if type(row) == "table" then
            row.build_ids = build_ids_of(game, index, row.has_build_list)
            row.build_count = #row.build_ids
        end
    end
end

return types
