local hooks = {}

function hooks.fits()
    if game.in_match then
        if game.map == nil then
            return "the map is missing in a match"
        end
        return true
    end
    if game.map ~= nil then
        return "the map is set outside a match"
    end
    return true
end

function hooks.start()
end

function hooks.onTick()
    game.map:reachable({x = 168, z = 168}, {x = 200, z = 168}, "kbot")
end

return hooks
