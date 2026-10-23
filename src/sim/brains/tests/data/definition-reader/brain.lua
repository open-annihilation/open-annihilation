local hooks = {}

local function expect(got, want, label)
    if got ~= want then
        error(label .. ": got " .. tostring(got) .. " want " .. tostring(want))
    end
end

local function must_fail(fn, text)
    local ok, err = pcall(fn)
    if ok then
        error("expected an error containing " .. text)
    end
    if string.find(tostring(err), text, 1, true) == nil then
        error("error was " .. tostring(err) .. " missing " .. text)
    end
end

local function names_of(collection)
    local names = {}
    for _, record in pairs(collection) do
        names[#names + 1] = record.unitname or record:index()
    end
    return table.concat(names, ",")
end

function hooks.fits()
    if game.in_match then
        return true
    end
    if #units ~= 0 or #weapons ~= 0 or #features ~= 0 then
        return "definitions are not empty outside a match"
    end
    if units[1] ~= nil or units.armcv ~= nil or weapons[0] ~= nil or weapons[1] ~= nil then
        return "a unit or weapon lookup is not nil outside a match"
    end
    if features[1] ~= nil or features.rock ~= nil then
        return "a feature lookup is not nil outside a match"
    end
    return true
end

function hooks.read_definitions()
    expect(#units, 5, "unit count")
    if units[0] ~= nil or units[6] ~= nil then
        error("unit index")
    end
    expect(names_of(units), "ARMCV,ARMSOLAR,ARMRELAY,ARMSILO,ARMMAKENUKE", "unit order")
    local walked = {}
    units:each(function(record)
        walked[#walked + 1] = record.unitname
    end)
    expect(table.concat(walked, ","), "ARMCV,ARMSOLAR,ARMRELAY,ARMSILO,ARMMAKENUKE", "unit each")

    local vehicle = units[1]
    expect(vehicle:index(), 1, "vehicle index")
    expect(units.armcv:index(), 1, "armcv")
    expect(units.ARMCV:index(), 1, "ARMCV")
    expect(vehicle.unitname, "ARMCV", "unitname")
    expect(vehicle:get("unitname"), "ARMCV", "get unitname")
    expect(vehicle.side, "ARM", "side")
    expect(vehicle.builder, true, "builder")
    expect(vehicle.bmcode, 1, "bmcode")
    expect(vehicle.footprintx, 2, "footprint")
    expect(vehicle.category[1], "ARM", "category arm")
    expect(vehicle.category[2], "LEVEL1", "category level")
    if vehicle.category[3] ~= nil then
        error("category tail")
    end
    expect(vehicle.maxvelocity:raw(), velocity_raw, "maxvelocity")
    expect(vehicle:fixed("maxvelocity"):raw(), velocity_raw, "fixed maxvelocity")
    expect(vehicle.energyuse:bits(), energy_bits, "energyuse")
    expect(vehicle.downloadable, true, "downloadable")
    expect(vehicle.ai_weight, "ridge-weight", "ai weight")
    if vehicle.copyright ~= nil or vehicle.yardmap ~= nil then
        error("omitted unit field")
    end
    local saw_name = false
    for _, key in ipairs(vehicle:keys()) do
        if key == "copyright" or key == "yardmap" then
            error("omitted key listed")
        end
        if key == "unitname" then
            saw_name = true
        end
    end
    if not saw_name then
        error("unitname missing from keys")
    end

    expect(#weapons, 1, "weapon count")
    if weapons[0] ~= nil or weapons[2] ~= nil then
        error("empty weapon slot")
    end
    local nuke = weapons.nuke
    expect(nuke:index(), 1, "nuke id")
    expect(weapons[1]:index(), 1, "weapon index")
    expect(nuke.reloadtime, nuke_reload, "reload")
    expect(nuke:fixed("reloadtime"):raw(), nuke_reload * 65536, "fixed reload")
    expect(nuke.energypershot:bits(), energy_shot_bits, "energy per shot")
    expect(nuke.stockpile, true, "stockpile")
    expect(nuke:damage(), damage_default, "default damage")
    expect(nuke:damage(nil), damage_default, "nil damage")
    expect(nuke:damage("ARMSOLAR"), damage_solar, "named damage")
    expect(nuke:damage(units[2]), damage_solar, "record damage")
    expect(nuke:damage(2), damage_solar, "index damage")
    expect(nuke:damage("armcv"), damage_default, "unlisted damage")
    local weapon_count = 0
    for key, record in pairs(weapons) do
        weapon_count = weapon_count + 1
        expect(key, 1, "weapon pair key")
        expect(record:index(), 1, "weapon pair index")
    end
    expect(weapon_count, 1, "weapon pairs")
    must_fail(function() nuke:damage(1.5) end, "damage expects a unit type or nothing")
    must_fail(function() nuke:damage(true) end, "damage expects a unit type or nothing")
    must_fail(function() nuke:damage("no-such-unit") end, "damage: unknown unit type no-such-unit")

    expect(#features, 3, "feature count")
    local rock = features.rock
    expect(rock:index(), 0, "first rock")
    expect(features[1].description, "a rock", "feature description")
    expect(rock.featuredead, "Tree", "feature dead")
    if rock.featureburnt ~= nil then
        error("feature burnt")
    end
    expect(rock.reclaimable, true, "reclaimable")
    expect(rock.metal:bits(), metal_bits, "feature metal")
    expect(features[2]:index(), 1, "tree index")
    expect(features[2].name or features[2]:index(), 1, "tree")
    local feature_count = 0
    features:each(function()
        feature_count = feature_count + 1
    end)
    expect(feature_count, 3, "feature each")

    expect(resolve_type(units[1]), 1, "resolve record")
    expect(resolve_type("armcv"), 1, "resolve name")
    expect(resolve_type(1), 1, "resolve index")
    must_fail(function() resolve_type("no-such-unit") end, "resolve: unknown unit type no-such-unit")
    must_fail(function() resolve_type(1.5) end,
        "resolve expects a unit type: a record, a name or an index")
    must_fail(function() resolve_type(true) end,
        "resolve expects a unit type: a record, a name or an index")

    expect(own_unit.type:index(), 1, "own type")
    expect(own_unit.type.unitname, "ARMCV", "own type name")
    if foe_unit.type ~= nil then
        error("hidden type")
    end

    must_fail(function() units.extra = 1 end, "a collection cannot be assigned")
    must_fail(function() vehicle.unitname = "NO" end, "a record cannot be assigned")
    must_fail(function() vehicle:get(1) end, "get expects a field name")
    must_fail(function() vehicle:fixed(1) end, "fixed expects a field name")
    units = {}
    if units.armcv ~= nil then
        error("global replacement")
    end
    return true
end

return hooks
