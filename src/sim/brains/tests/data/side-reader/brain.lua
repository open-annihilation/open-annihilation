-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

-- Reads the seat's side. The name is not `brain`: that would hide the global.
local hooks = {}

function hooks.start()
end

function read_all()
    local _player = me.player
    local _level = me.level
    local _difficulty = me.difficulty
    local _options = me.options
    local _name = me.name
    local _side = me.side
    local _side_name = me.side_name
    local _team = me.team
    local _color = me.color
    local _energy = me.energy
    local _metal = me.metal
    local _energy_storage = me.energy_storage
    local _metal_storage = me.metal_storage
    local _energy_produced = me.energy_produced
    local _metal_produced = me.metal_produced
    local _energy_requested = me.energy_requested
    local _metal_requested = me.metal_requested
    local _energy_surplus = me.energy_surplus
    local _metal_surplus = me.metal_surplus
    local _tick = me.tick
    local _handed = me.handed_over
    local _refused = me.refused_assignments
    local knowledge = me.knowledge
    if knowledge ~= nil then
        local _owned = knowledge:owned(1)
        local _builders = knowledge.builders
        local _base = knowledge.base
        local _refreshed = knowledge.tick
    end
    for other = 0, 9 do
        local _allied = me:allied(other)
    end
    local _allies = me:allies()
    local _players = me:players()
    local units = me:units()
    me:each_unit(function(unit)
        local _id = unit.id
        local _slot = unit.slot
        local _alive = unit.alive
        local _owner = unit.owner
        local _type_index = unit.type_index
        local _type_name = unit.type_name
        local _x = unit.x
        local _y = unit.y
        local _z = unit.z
        local _position = unit.position
        local _heading = unit.heading
        local _health = unit.health
        local _health_percent = unit.health_percent
        local _max_health = unit.max_health
        local _build_remaining = unit.build_remaining
        local _finished = unit.finished
        local _building = unit.building
        local _armed = unit.armed
        local _selectable = unit.selectable
        local _live = unit.live
        local _dying = unit.dying
        local _active = unit.active
        local _cloaked = unit.cloaked
        local _mobile = unit.mobile
        local _squad = unit.squad
        local _move_order = unit.move_order
        local _fire_order = unit.fire_order
        local _kills = unit.kills
        local _kind, _busy, _idle = unit:head_order()
        local _background = unit:has_background_order()
        local _orders = unit:orders()
        local _secondary = unit:orders(true)
        local _reaches = unit:reaches({x = 0, z = 0})
    end)
    for squad = 0, 9 do
        local _size = me:squad_size(squad)
        local _member = me:squad_member(squad, 1)
        local _missing = me:squad_member(squad, "no")
        local _list = me:squad(squad)
    end
    local _count = me:unit_count()
    local _apart = brain.distance({x = 0, z = 0}, {x = 1, z = 0})
    local _heading_to = brain.direction({x = 0, z = 0}, {x = 1, z = 0})
    if units[1] ~= nil then
        local _from_unit = brain.distance(units[1], {x = 0, z = 0})
        local _face = brain.direction(units[1], {x = 0, z = 0})
    end
end

return hooks
