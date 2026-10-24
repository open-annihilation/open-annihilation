-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

-- The map cut into 8 by 8 sectors, and the features a unit shows the network.
-- Rows run north to south and columns west to east. Sector index is
-- row * 8 + column, counting from 0. A sector's centre is the middle of the
-- world span the explored grid uses for that cell.

local sectors = {}

local SIZE = 8
local CELLS = SIZE * SIZE

local function cell_start(index, span)
    return (index * span) // SIZE
end

-- A centre the unit can stand on. The movement map's cell value is 0 when
-- the cell is blocked or off the map, and 1 or 3 when the class can enter it.
local function can_stand(unit, x, z)
    local definition = unit.type
    if definition == nil then
        return false
    end
    local klass = definition.movementclass
    if klass == nil or klass == "" then
        return false
    end
    local cell_x, cell_z = game.map:anchor(klass, {x = x, z = z})
    if cell_x == nil or cell_z == nil then
        return false
    end
    local value = game.map:passable(klass, cell_x, cell_z)
    return value ~= nil and value > 0
end

local function cell_middle(index, span)
    local start = cell_start(index, span)
    local next_edge = span
    if index + 1 < SIZE then
        next_edge = cell_start(index + 1, span)
    end
    return start + (next_edge - start) // 2
end

-- Centre of sector `index` in whole world units.
function sectors.centre(index, width, height)
    local row = index // SIZE
    local column = index % SIZE
    return cell_middle(column, width), cell_middle(row, height)
end

-- The feature tensor for one unit, plus the numbers the written rule reads.
-- Channel 1 is the explored grid. Channel 2 is each sector centre's distance
-- from the unit, divided by the map's diagonal, in fixed counts.
-- `explored_raw[i]` is that share as a count of 65536ths. `squared[i]` is the
-- squared distance of the centres in whole world units. `stand[i]` is true
-- when this unit's movement class can stand on the centre's cell.
function sectors.look(unit, explored, width, height)
    if unit.x == nil or unit.z == nil then
        return nil
    end
    local diagonal = brain.distance({x = 0, z = 0}, {x = width, z = height})
    if diagonal == nil or diagonal:raw() == 0 then
        return nil
    end
    local ux = unit.x:floor()
    local uz = unit.z:floor()
    local values = {}
    local explored_raw = {}
    local squared = {}
    local stand = {}
    for index = 0, CELLS - 1 do
        local cx, cz = sectors.centre(index, width, height)
        local distance = brain.distance(unit, {x = cx, z = cz})
        if distance == nil then
            return nil
        end
        local dx = cx - ux
        local dz = cz - uz
        local share = explored:get(index + 1)
        values[index + 1] = share
        values[CELLS + index + 1] = distance / diagonal
        explored_raw[index + 1] = share:raw()
        squared[index + 1] = dx * dx + dz * dz
        stand[index + 1] = can_stand(unit, cx, cz)
    end
    return {
        tensor = brain.tensor(values, {2, SIZE, SIZE}),
        explored_raw = explored_raw,
        squared = squared,
        stand = stand,
    }
end

-- Send the unit to the centre of a sector.
function sectors.send(unit, index, width, height)
    if index == nil or index < 0 or index >= CELLS then
        return false
    end
    local cx, cz = sectors.centre(index, width, height)
    return unit:move({x = cx, z = cz})
end

return sectors
