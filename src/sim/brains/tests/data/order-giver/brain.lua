-- A quiet order giver. It moves one unit once, when a test asks, and otherwise
-- leaves the match alone.
local hooks = {}

function hooks.fits()
    return true
end

function hooks.start()
    return true
end

function hooks.onTick()
    if move_once and mover then
        move_result = mover:move({x = move_x, z = move_z})
        move_once = false
    end
end

return hooks
