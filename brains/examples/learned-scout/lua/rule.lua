-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

-- The rule the network is taught.
-- Channel 1 already writes a centre the unit cannot stand on as fully
-- explored. Among sectors whose explored share is below one half, pick the
-- sector whose centre is nearest the unit. Distance is the squared distance
-- in whole world units. The lowest index wins a tie. When every sector is
-- at least half explored, pick the farthest, and again the lowest index
-- on a tie.
-- The brain records this choice every 30 ticks. It sends the unit when the
-- head order is absent or its idle bit is set.

local rule = {}

-- One half, as a count of 65536ths. A share of exactly one half is not below it.
local HALF = 32768

function rule.choose(explored_raw, squared)
    local any_open = false
    for index = 1, #explored_raw do
        if explored_raw[index] < HALF then
            any_open = true
            break
        end
    end
    local best = nil
    local best_distance = nil
    for index = 1, #squared do
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
    return best
end

return rule
