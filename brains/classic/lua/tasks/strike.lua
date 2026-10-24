-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

-- A strike group. It recruits from its source squad, holds at home until it
-- is large enough to launch, and then attacks the nearest enemy. The
-- timetable is already set.

local squads = require("squads")

local strike = {}

function strike.run(seat, task)
    local me = seat.me
    squads.recruit(me, task, task.source, task.merge_radius)
    local count = squads.count(me, task.squad)
    if count < 1 then
        return
    end
    local attack = false
    if count > task.min_size then
        attack = task.attacking ~= 0 or count >= task.launch
    end
    if not attack then
        local home = squads.home(me)
        if home ~= nil then
            task.attacking = 0
            squads.order(me, task.squad, "move", false, nil, squads.fixed_point(home))
            return
        end
    end
    task.attacking = 1
    local center = squads.centroid(me, task.squad) or {x = 0, y = 0, z = 0}
    local target = squads.nearest_enemy(seat, task, center)
    if target == nil then
        return
    end
    squads.order(me, task.squad, "attack", false, target, nil)
end

return strike
