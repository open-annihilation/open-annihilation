-- Reads the game facts. In a match, fits keeps the table and start records
-- it. Outside a match, fits itself reports a rich game that does not match.

local seen_game
local seen_expansions
local seen_mod

local function join_list(list)
    local parts = {}
    for slot = 1, #list do
        parts[slot] = tostring(list[slot])
    end
    return table.concat(parts, "|")
end

local function join_field(list, field)
    local parts = {}
    for slot = 1, #list do
        parts[slot] = tostring(list[slot][field])
    end
    return table.concat(parts, "|")
end

local function write_error(body)
    local ok, err = pcall(body)
    if ok then
        return "wrote"
    end
    return tostring(err)
end

local function problem(game)
    if game.in_match then
        return "in match"
    end
    if game.base ~= "totala-demo" then
        return "base"
    end
    if join_list(game.expansions) ~= "campaign|core-contingency" then
        return "expansions"
    end
    if game.engine ~= "0.8.6" then
        return "engine"
    end
    if game.mode ~= "network" then
        return "mode"
    end
    if game.difficulty ~= "hard" then
        return "difficulty"
    end
    if game.mod.id ~= "ridge" or game.mod.name ~= "Ridge" or game.mod.version ~= "28" then
        return "mod"
    end
    if join_field(game.mod.sides, "name") ~= "North|Vale" then
        return "sides"
    end
    if join_field(game.mod.sides, "prefix") ~= "N|V" then
        return "prefixes"
    end
    if join_field(game.mod.sides, "commander") ~= "NCOM|VCOM" then
        return "commanders"
    end
    local wave = game.mod.hacks["ai.attack-wave-size"]
    if wave.on ~= true or wave.units ~= 10 then
        return "wave"
    end
    local squad = game.mod.hacks["ai.squad-assignment"]
    if squad.on ~= true or squad.rules ~= "role-squads" then
        return "squad"
    end
    local income = game.mod.hacks["ai.income-multipliers"]
    if income.on ~= true or income.production[1] ~= 1.25 or income.production[2] ~= 0.5 or
        income.production[3] ~= 2.5 then
        return "production"
    end
    if income.reclaim[1] ~= 0.5 or income.reclaim[2] ~= 0.7 or income.reclaim[3] ~= 1.0 then
        return "reclaim"
    end
    local names = game.mod.hacks["ai.difficulty-names"]
    if names.on ~= true or join_list(names.names) ~= "hard|medium|easy" then
        return "names"
    end
    if #game.mutators ~= 2 then
        return "mutator count"
    end
    local wider = game.mutators[1]
    if wider.id ~= "wider-waves" or wider.version ~= "1.2" then
        return "wider"
    end
    if type(wider.options.surge) ~= "boolean" or wider.options.surge ~= true then
        return "surge"
    end
    if type(wider.options.scale) ~= "number" or wider.options.scale ~= 1.5 then
        return "scale"
    end
    local pick = game.mutators[2]
    if pick.id ~= "side-pick" or pick.version ~= "3" then
        return "side pick"
    end
    if type(pick.options.side) ~= "string" or pick.options.side ~= "north" then
        return "side option"
    end
    if game:has_expansion("campaign") ~= true or game:has_expansion("core-contingency") ~= true then
        return "has expansion"
    end
    if game:has_expansion("music") ~= false or game:has_expansion("totala") ~= false or
        game:has_expansion("") ~= false then
        return "lacks expansion"
    end
    if not rawequal(game.mod, game.mod) then
        return "mod identity"
    end
    if not rawequal(game:hack("ai.attack-wave-size"), wave) then
        return "hack identity"
    end
    if game:hack("no.such.hack") ~= nil then
        return "missing hack"
    end
    if write_error(function()
        game.base = "x"
    end) ~= "game is read-only" then
        return "write base"
    end
    if write_error(function()
        game.mod.hacks["ai.attack-wave-size"].units = 1
    end) ~= "game is read-only" then
        return "write units"
    end
    if write_error(function()
        game.expansions[1] = "y"
    end) ~= "game is read-only" then
        return "write list"
    end
    return nil
end

local function record(game)
    same_game = rawequal(game, seen_game)
    expansions_same = rawequal(game.expansions, seen_expansions)
    mod_same = rawequal(game.mod, seen_mod)
    mod_self = rawequal(game.mod, game.mod)
    base = game.base
    engine = game.engine
    mode = game.mode
    difficulty = game.difficulty
    in_match = game.in_match
    expansion_text = join_list(game.expansions)
    expansion_count = #game.expansions
    side_names = join_field(game.mod.sides, "name")
    side_prefixes = join_field(game.mod.sides, "prefix")
    side_commanders = join_field(game.mod.sides, "commander")
    mod_id = game.mod.id
    mod_name = game.mod.name
    mod_version = game.mod.version
    local keys = {}
    local count = 0
    local ons = 0
    local off = true
    for key, hack in pairs(game.mod.hacks) do
        count = count + 1
        keys[count] = key
        if hack.on then
            ons = ons + 1
            off = false
        end
    end
    hack_keys = table.concat(keys, "|")
    hack_count = count
    on_count = ons
    all_off = off
    local limits = 0
    for _ in pairs(game.mod.limits) do
        limits = limits + 1
    end
    limit_count = limits
    local wave = game.mod.hacks["ai.attack-wave-size"]
    wave_on = wave.on
    wave_units = wave.units
    local squad = game.mod.hacks["ai.squad-assignment"]
    squad_rules = squad.rules
    local income = game.mod.hacks["ai.income-multipliers"]
    production_1 = income.production[1]
    production_2 = income.production[2]
    production_3 = income.production[3]
    reclaim_1 = income.reclaim[1]
    reclaim_2 = income.reclaim[2]
    reclaim_3 = income.reclaim[3]
    difficulty_names = join_list(game.mod.hacks["ai.difficulty-names"].names)
    limit_default = game.mod.limits["limits.units-per-player"].default
    thresholds = join_list(game.mod.hacks["veterancy.model"]["default-thresholds"])
    damage_cap = game.mod.hacks["veterancy.model"]["damage-dealt-cap"]
    state_count = #game.mod.hacks["orders.weapons-free-while-busy"].states
    name_format = game.mod.hacks["setup.ai-player-name-format"].format
    mutator_count = #game.mutators
    mutator_ids = join_field(game.mutators, "id")
    mutator_versions = join_field(game.mutators, "version")
    surge_type = ""
    surge_value = false
    scale_type = ""
    scale_value = 0
    side_option_type = ""
    side_option_value = ""
    if mutator_count >= 1 then
        surge_type = type(game.mutators[1].options.surge)
        surge_value = game.mutators[1].options.surge
        scale_type = type(game.mutators[1].options.scale)
        scale_value = game.mutators[1].options.scale
    end
    if mutator_count >= 2 then
        side_option_type = type(game.mutators[2].options.side)
        side_option_value = game.mutators[2].options.side
    end
    has_campaign = game:has_expansion("campaign")
    has_core = game:has_expansion("core-contingency")
    has_music = game:has_expansion("music")
    has_totala = game:has_expansion("totala")
    has_empty = game:has_expansion("")
    hack_same = rawequal(game:hack("ai.attack-wave-size"), wave)
    hack_missing = game:hack("no.such.hack") == nil
    write_base = write_error(function()
        game.base = "x"
    end)
    write_units = write_error(function()
        game.mod.hacks["ai.attack-wave-size"].units = 1
    end)
    write_list = write_error(function()
        game.expansions[1] = "y"
    end)
end

local hooks = {}

function hooks.fits(game)
    seen_game = game
    seen_expansions = game.expansions
    seen_mod = game.mod
    if game.in_match then
        return true
    end
    return problem(game) or true
end

function hooks.start(game)
    record(game)
end

return hooks
