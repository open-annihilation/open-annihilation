-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

-- Learned Scout walks the map. Every 30 ticks, each of this seat's units
-- that can move and has no order is sent to the centre of one sector.
-- The learned level asks nets/scout.oann which sector. The rule level
-- follows the written rule and records the features and the choice.

local sectors = require("sectors")
local rule = require("rule")

local hooks = {}
local scout = nil
local teaching = false

function hooks.fits()
    return true
end

function hooks.start()
    teaching = me.level == "rule"
    if not teaching then
        scout = brain.network("scout")
    end
    return true
end

local function consider(unit, explored, width, height)
    local definition = unit.type
    if definition == nil or definition.canmove ~= true then
        return
    end
    if unit:head_order() ~= nil then
        return
    end
    local look = sectors.look(unit, explored, width, height)
    if look == nil then
        return
    end
    local choice = nil
    if teaching then
        choice = rule.choose(look.explored_raw, look.squared)
        brain.record({
            features = {sectors = look.tensor},
            decision = choice,
        })
    else
        local out = scout:run({sectors = look.tensor})
        choice = out.choice - 1
    end
    sectors.send(unit, choice, width, height)
end

local function survey()
    local width = game.map.world_width
    local height = game.map.world_height
    if width == nil or height == nil or width <= 0 or height <= 0 then
        return
    end
    local explored = brain.grid({"explored"}, 8)
    if explored == nil then
        return
    end
    me:each_unit(function(unit)
        consider(unit, explored, width, height)
    end)
end

function hooks.onTick()
    if me.tick % 30 ~= 0 then
        return
    end
    survey()
end

return hooks
