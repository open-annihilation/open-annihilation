-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

-- A waiting squad. When it and the strike group both have members, it moves
-- to the strike group's centre. The timetable is already set.

local squads = require("squads")

local rally = {}

function rally.run(seat, task)
    local me = seat.me
    if squads.count(me, task.squad) < 1 or squads.count(me, task.source) < 1 then
        return
    end
    local center = squads.centroid(me, task.source)
    if center == nil then
        return
    end
    squads.order(me, task.squad, "move", false, nil, squads.fixed_point(center))
end

return rally
