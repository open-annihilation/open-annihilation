-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

local hooks = {}
local due = 0

function hooks.onTick(tick)
  if tick < due then
    return
  end
  brain.section("task9")
  due = tick + brain.shared_random(150) + 30
end

return hooks
