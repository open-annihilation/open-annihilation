local hooks = {}

local function flag(value)
    if value == nil then
        return "nil"
    end
    if value then
        return "true"
    end
    return "false"
end

local function words_of(line)
    local parts = {}
    for index = 1, #line.words do
        parts[index] = line.words[index]
    end
    return table.concat(parts, "\t")
end

local function dump_line(line)
    local body = tostring(line.number) .. "|" .. words_of(line) .. "|" .. line.command
    if line.command == "plan" then
        body = body .. "|any=" .. flag(line.any) .. "|easy=" .. flag(line.easy)
            .. "|medium=" .. flag(line.medium) .. "|hard=" .. flag(line.hard)
    elseif line.command == "weight" then
        body = body .. "|name=" .. line.name .. "|factor=" .. tostring(line.factor:bits())
            .. "|unit=" .. flag(line.unit)
    elseif line.command == "limit" then
        body = body .. "|name=" .. line.name .. "|count=" .. tostring(line.count)
            .. "|unit=" .. flag(line.unit)
    end
    return body
end

local function dump_lines(lines)
    local parts = {}
    for index = 1, #lines do
        parts[index] = dump_line(lines[index])
    end
    return table.concat(parts, "\n")
end

local function numbers(rows)
    local parts = {}
    for index = 1, #rows do
        parts[index] = tostring(rows[index].number)
    end
    return table.concat(parts, ",")
end

local function must_contain(err, text)
    if string.find(tostring(err), text, 1, true) == nil then
        error("error was " .. tostring(err) .. " missing " .. text)
    end
end

local function outside()
    if game.build_list ~= nil or game.build_list_cap ~= nil or game.downloadables_allowed ~= nil then
        return "a match field is set outside a match"
    end
    if brain.strengths ~= nil then
        return "strengths is set outside a match"
    end
    if game.ai_profile == nil or game.ai_profile.path ~= "ai/Sample.txt" then
        return "ai_profile path"
    end
    if game.ai_profile.name ~= "Sample" or game.ai_profile.report_path ~= nil then
        return "ai_profile name or report"
    end
    if game.ai_profile.too_large ~= false or game.ai_profile.text == nil then
        return "ai_profile text"
    end
    if game.ai_profile.lines[1].unit ~= nil or game.ai_profile.lines[5].unit ~= nil then
        return "unit is set outside a match"
    end
    local medium = game.ai_profile:select("medium")
    if medium.weights[1] == nil or medium.weights[1].number ~= 1 or medium.weights[1].unit ~= nil then
        return "outside select"
    end
    if medium.limits[1] == nil or medium.limits[1].number ~= 5 or medium.limits[1].unit ~= nil then
        return "outside limit"
    end
    local parsed = game.parse_profile("weight core 0.5\n")
    if parsed.path ~= nil or parsed.name ~= nil or parsed.report_path ~= nil then
        return "parse_profile names a file"
    end
    if parsed.lines[1].unit ~= nil or parsed.lines[1].name ~= "core" then
        return "parse_profile line"
    end
    if parsed.lines[1].command ~= "weight" then
        return "parse_profile command"
    end
    local profiles = game.ai_profiles()
    if #profiles ~= 3 or profiles[1].name ~= "alpha" or profiles[3].name ~= "zeta" then
        return "ai_profiles order"
    end
    if profiles[2].name ~= "mid" or profiles[2].too_large ~= true or profiles[2].text ~= nil then
        return "oversized profile"
    end
    if profiles[2].lines[1].command ~= "weight" or profiles[2].lines[1].name ~= "core" then
        return "oversized lines"
    end
    local accepted, err = pcall(function()
        game.ai_profile:select("nope")
    end)
    if accepted then
        return "select accepted nope"
    end
    if string.find(tostring(err), "select expects easy, medium or hard", 1, true) == nil then
        return "select error was " .. tostring(err)
    end
    return true
end

function hooks.fits()
    if game.in_match then
        return true
    end
    return outside()
end

function hooks.start()
    if units[2].unitname ~= "ARMSOLAR" then
        error("collector is " .. tostring(units[2].unitname))
    end
    local profile = game.ai_profile
    seen_text = profile.text
    seen_name = profile.name
    seen_path = profile.path
    if profile.report_path == nil then
        seen_report = ""
    else
        seen_report = profile.report_path
    end
    seen_cap = game.build_list_cap
    seen_downloads = flag(game.downloadables_allowed)
    seen_lines = dump_lines(profile.lines)
    seen_medium_w = numbers(profile:select("medium").weights)
    seen_medium_l = numbers(profile:select("medium").limits)
    seen_easy_w = numbers(profile:select("easy").weights)
    seen_easy_l = numbers(profile:select("easy").limits)
    seen_hard_w = numbers(profile:select("hard").weights)
    seen_hard_l = numbers(profile:select("hard").limits)
    local parsed = game.parse_profile(units[2].ai_weight)
    seen_parsed_name = parsed.lines[1].name
    seen_parsed_bits = parsed.lines[1].factor:bits()
    seen_parsed_unit = flag(parsed.lines[1].unit)
    seen_parsed_path = parsed.path == nil and "nil" or parsed.path
    local before = brain.strengths(2)
    seen_before = before == nil and "nil" or "set"
    local profiles = game.ai_profiles()
    seen_profile_count = #profiles
    seen_p1 = profiles[1].path .. "|" .. profiles[1].name .. "|" .. profiles[1].lines[1].command
    seen_p2_large = flag(profiles[2].too_large)
    seen_p2_text = profiles[2].text == nil and "nil" or "text"
    seen_p2_name = profiles[2].lines[1].name
    seen_p3 = profiles[3].name
    local accepted, err = pcall(function()
        profile:select("Easy")
    end)
    if accepted then
        error("select accepted Easy")
    end
    must_contain(err, "select expects easy, medium or hard")
    local wrote, write_err = pcall(function()
        profile.lines[1].number = 9
    end)
    if wrote then
        error("a profile line was assigned")
    end
    must_contain(write_err, "a profile is read-only")
end

local function ids(value)
    local built = game.build_list(value)
    local parts = {}
    for index = 1, #built do
        parts[index] = tostring(built[index]:index())
    end
    return table.concat(parts, ",")
end

function hooks.after()
    local rows = {}
    for unit_type = 1, 5 do
        rows[unit_type] = tostring(unit_type) .. "=" .. ids(unit_type)
    end
    seen_lists = table.concat(rows, ";")
    if ids("ARMCV") ~= ids(1) or ids(units[1]) ~= ids(1) then
        error("vehicle build list")
    end
    if ids("ARMSOLAR") ~= ids(2) then
        error("collector build list")
    end
    local function strength_text(value)
        local priority, metal, energy = brain.strengths(value)
        if priority == nil then
            return "nil"
        end
        return tostring(priority) .. "," .. tostring(metal) .. "," .. tostring(energy)
    end
    local from_index = strength_text(2)
    if from_index ~= strength_text(units[2]) or from_index ~= strength_text("ARMSOLAR") then
        error("strength forms differ")
    end
    seen_strength = from_index
    local accepted, err = pcall(function()
        game.build_list(0)
    end)
    if accepted then
        error("build_list accepted 0")
    end
    must_contain(err, "unknown unit type")
end

return hooks
