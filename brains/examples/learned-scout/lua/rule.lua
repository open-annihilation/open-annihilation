-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

-- The rule the network is taught.
-- A sector counts only when this unit can stand on its centre. Among those
-- whose explored share is below one half, pick the sector whose centre is
-- nearest the unit. Distance is the squared distance in whole world units.
-- The lowest index wins a tie. When every standable sector is at least half
-- explored, pick the farthest, and again the lowest index on a tie.
-- The brain records this choice every 30 ticks. It sends the unit when the
-- unit has no order, and again once the unit is standing within its footprint
-- of the centre it was sent to, even when that move order is still reported.

local rule = {}

-- One half, as a count of 65536ths. A share of exactly one half is not below it.
local HALF = 32768

function rule.choose(explored_raw, squared, stand)
    local any_open = false
    for index = 1, #explored_raw do
        if stand[index] and explored_raw[index] < HALF then
            any_open = true
            break
        end
    end
    local best = nil
    local best_distance = nil
    for index = 1, #squared do
        if stand[index] then
            local take = false
            if any_open then
                if explored_raw[index] < HALF then
                    if best == nil or squared[index] < best_distance then
                        take = true
                    end
                end
            elseif best == nil or squared[index] > best_distance then
                take = true
            end
            if take then
                best = index - 1
                best_distance = squared[index]
            end
        end
    end
    return best
end

return rule
