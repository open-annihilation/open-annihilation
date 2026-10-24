-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

-- Profile passes. A factor is the longest numeric prefix of the word, rounded
-- once to single precision. A limit is a base-10 integer clamped to 32 bits.
-- The plan flag is the seat's, shared by the passes and kept across a reload.

local knowledge = require("knowledge")

local profile = {}

local token_limit = 20
local token_bytes = 0x7e
local storage_bytes = token_bytes + token_limit
local int32_min = -2147483648
local int32_limit = 2147483648

local function lower_byte(text, index)
    local byte = string.byte(text, index)
    if byte == nil then
        return 0
    end
    if byte >= 65 and byte <= 90 then
        return byte + 32
    end
    return byte
end

local function is_space_byte(byte)
    return byte == 32 or byte == 9 or byte == 13 or byte == 10 or byte == 11 or byte == 12
end

local function equal_nocase(left, right)
    if type(left) ~= "string" then
        left = ""
    end
    if type(right) ~= "string" then
        right = ""
    end
    if #left ~= #right then
        return false
    end
    for index = 1, #left do
        if lower_byte(left, index) ~= lower_byte(right, index) then
            return false
        end
    end
    return true
end

local function tokenize(line)
    local tokens = {}
    local count = 0
    local used = 0
    local index = 1
    local length = #line
    while index <= length do
        while index <= length and is_space_byte(string.byte(line, index)) do
            index = index + 1
        end
        if index > length or string.byte(line, index) == 35 then
            return tokens
        end
        local parts = {}
        local part_count = 0
        while index <= length do
            local byte = string.byte(line, index)
            if is_space_byte(byte) or byte == 35 then
                break
            end
            if used < token_bytes then
                part_count = part_count + 1
                parts[part_count] = string.char(byte)
                used = used + 1
            end
            index = index + 1
        end
        while index <= length do
            local byte = string.byte(line, index)
            if is_space_byte(byte) or byte == 35 then
                break
            end
            index = index + 1
        end
        used = used + 1
        if count < token_limit then
            count = count + 1
            tokens[count] = table.concat(parts)
        end
        if used >= storage_bytes then
            return tokens
        end
    end
    return tokens
end

local function starts_with(text, index, word)
    for step = 1, #word do
        if lower_byte(text, index + step - 1) ~= string.byte(word, step) then
            return false
        end
    end
    return true
end

local function is_digit(byte)
    return byte ~= nil and byte >= 48 and byte <= 57
end

local function is_hex(byte)
    return is_digit(byte) or (byte ~= nil and byte >= 65 and byte <= 70) or
        (byte ~= nil and byte >= 97 and byte <= 102)
end

local function hex_prefix(text, index)
    if string.byte(text, index) ~= 48 then
        return nil
    end
    local mark = string.byte(text, index + 1)
    if mark ~= 120 and mark ~= 88 then
        return nil
    end
    local cursor = index + 2
    local digits = 0
    while is_hex(string.byte(text, cursor)) do
        digits = digits + 1
        cursor = cursor + 1
    end
    if string.byte(text, cursor) == 46 then
        cursor = cursor + 1
        while is_hex(string.byte(text, cursor)) do
            digits = digits + 1
            cursor = cursor + 1
        end
    end
    if digits == 0 then
        return nil
    end
    local exponent = string.byte(text, cursor)
    if exponent == 112 or exponent == 80 then
        local after = cursor + 1
        local sign = string.byte(text, after)
        if sign == 43 or sign == 45 then
            after = after + 1
        end
        local exp_digits = 0
        while is_digit(string.byte(text, after)) do
            exp_digits = exp_digits + 1
            after = after + 1
        end
        if exp_digits > 0 then
            cursor = after
        end
    end
    return tonumber(string.sub(text, index, cursor - 1))
end

local function decimal_prefix(text, index)
    local start = index
    local digits = 0
    while is_digit(string.byte(text, index)) do
        digits = digits + 1
        index = index + 1
    end
    if string.byte(text, index) == 46 then
        index = index + 1
        while is_digit(string.byte(text, index)) do
            digits = digits + 1
            index = index + 1
        end
    end
    if digits == 0 then
        return 0
    end
    local exponent = string.byte(text, index)
    if exponent == 101 or exponent == 69 then
        local after = index + 1
        local sign = string.byte(text, after)
        if sign == 43 or sign == 45 then
            after = after + 1
        end
        local exp_digits = 0
        while is_digit(string.byte(text, after)) do
            exp_digits = exp_digits + 1
            after = after + 1
        end
        if exp_digits > 0 then
            index = after
        end
    end
    local value = tonumber(string.sub(text, start, index - 1))
    if value == nil then
        return 0
    end
    return value
end

-- Longest numeric prefix, then one rounding to single precision.
local function read_factor(word)
    if type(word) ~= "string" then
        word = ""
    end
    local index = 1
    local length = #word
    while index <= length and is_space_byte(string.byte(word, index)) do
        index = index + 1
    end
    local sign = 1
    local mark = string.byte(word, index)
    if mark == 43 or mark == 45 then
        if mark == 45 then
            sign = -1
        end
        index = index + 1
    end
    local parsed = 0
    if starts_with(word, index, "infinity") or starts_with(word, index, "inf") then
        parsed = math.huge
    elseif starts_with(word, index, "nan") then
        parsed = 0 / 0
    else
        local hex = hex_prefix(word, index)
        if hex ~= nil then
            parsed = hex
        else
            parsed = decimal_prefix(word, index)
        end
    end
    if sign < 0 and parsed == parsed then
        parsed = -parsed
    end
    if parsed ~= parsed then
        return f32(0 / 0):tonumber()
    end
    return f32(parsed):tonumber()
end

-- Base-10 integer, clamped to the 32-bit range.
local function read_limit(word)
    if type(word) ~= "string" then
        return 0
    end
    local index = 1
    local length = #word
    while index <= length and is_space_byte(string.byte(word, index)) do
        index = index + 1
    end
    local negative = false
    local mark = string.byte(word, index)
    if mark == 43 or mark == 45 then
        negative = mark == 45
        index = index + 1
    end
    if not is_digit(string.byte(word, index)) then
        return 0
    end
    local value = 0
    local overflow = false
    while is_digit(string.byte(word, index)) do
        local digit = string.byte(word, index) - 48
        if not overflow then
            if not negative and (value > 214748364 or (value == 214748364 and digit > 7)) then
                overflow = true
            elseif negative and (value > 214748364 or (value == 214748364 and digit > 8)) then
                overflow = true
            end
        end
        if not overflow then
            value = value * 10 + digit
        end
        index = index + 1
    end
    if overflow then
        if negative then
            return int32_min
        end
        return 2147483647
    end
    if negative then
        return -value
    end
    return value
end

local function truncate_to_int32(value)
    if type(value) ~= "number" or value ~= value or value == math.huge or value == -math.huge then
        return int32_min
    end
    if value >= int32_limit or value < int32_min then
        return int32_min
    end
    if value >= 0 then
        return math.floor(value)
    end
    return math.ceil(value)
end

local function clamp_percent(value)
    if value < 1 then
        return 0
    end
    if value > 100 then
        return 100
    end
    return value
end

local function type_count(seat)
    if type(seat.types) ~= "table" then
        return 1
    end
    return #seat.types + 1
end

local function name_is_unit(seat, name)
    local listed = seat.types
    if type(listed) ~= "table" then
        return false
    end
    for index = 1, #listed do
        local row = listed[index]
        if type(row) == "table" and equal_nocase(row.unit_name, name) then
            return true
        end
    end
    return false
end

local function type_matches(row, name, exact)
    if type(row) ~= "table" then
        return false
    end
    if exact then
        return equal_nocase(row.unit_name, name)
    end
    if equal_nocase(name, "all") then
        return true
    end
    local categories = row.categories or ""
    local index = 1
    while index <= #categories do
        while index <= #categories and string.byte(categories, index) == 32 do
            index = index + 1
        end
        local start = index
        while index <= #categories and string.byte(categories, index) ~= 32 do
            index = index + 1
        end
        if start == index then
            break
        end
        if equal_nocase(string.sub(categories, start, index - 1), name) then
            return true
        end
    end
    return false
end

local function apply_weight(seat, name, factor)
    local exact = name_is_unit(seat, name)
    local count = type_count(seat)
    for index = 1, count - 1 do
        if type_matches(seat.types[index], name, exact) and seat.weight_locked[index] == 0 then
            local scaled = truncate_to_int32(seat.weight_percent[index] * factor)
            seat.weight_percent[index] = clamp_percent(scaled)
            if exact then
                seat.weight_locked[index] = 1
            end
        end
    end
end

local function apply_limit(seat, name, limit)
    local exact = name_is_unit(seat, name)
    local count = type_count(seat)
    for index = 1, count - 1 do
        if type_matches(seat.types[index], name, exact) and seat.limit_locked[index] == 0 then
            seat.limits[index] = limit
            if exact then
                seat.limit_locked[index] = 1
            end
        end
    end
end

local function difficulty_number(word)
    if equal_nocase(word, "easy") then
        return 0
    end
    if equal_nocase(word, "medium") then
        return 1
    end
    if equal_nocase(word, "hard") then
        return 2
    end
    return nil
end

local function plan_keyword(seat, difficulty)
    if type(difficulty) ~= "number" or difficulty < 0 or difficulty > 2 then
        return nil
    end
    local names = seat.rules and seat.rules.difficulty_names
    if type(names) ~= "table" then
        return nil
    end
    local word = names[difficulty + 1]
    if type(word) ~= "string" then
        return nil
    end
    return word
end

local function game_difficulty(seat)
    local game = seat.game
    local word = nil
    if game ~= nil then
        word = game.difficulty
    end
    return difficulty_number(word) or 1
end

local function profile_text(seat)
    local game = seat.game
    if game == nil then
        return ""
    end
    local script = game.ai_profile
    if type(script) ~= "table" and type(script) ~= "userdata" then
        return ""
    end
    local text = script.text
    if type(text) ~= "string" then
        return ""
    end
    return text
end

local function seat_is_computer(seat)
    local me = seat.me
    if me == nil or type(me.players) ~= "function" then
        return false
    end
    local player = me.player
    local listed = me:players()
    if type(listed) ~= "table" then
        return false
    end
    for index = 1, #listed do
        local row = listed[index]
        if type(row) == "table" and row.player == player then
            return row.computer == true
        end
    end
    return false
end

local function computer_count(seat)
    local me = seat.me
    if me == nil or type(me.players) ~= "function" then
        return 0
    end
    local listed = me:players()
    if type(listed) ~= "table" then
        return 0
    end
    local count = 0
    for index = 1, #listed do
        local row = listed[index]
        if type(row) == "table" and row.computer == true then
            count = count + 1
        end
    end
    return count
end

-- @quirk "any" counts only as the first word after "plan", and that same
-- word is tested again on every later word of the line.
local function plan_line(tokens, keyword)
    local matches = false
    for index = 2, #tokens do
        if equal_nocase(tokens[2], "any") then
            matches = true
        end
        if keyword ~= nil and equal_nocase(tokens[index], keyword) then
            matches = true
        end
    end
    return matches
end

local function each_line(text, visit)
    if type(text) ~= "string" or text == "" then
        return
    end
    local start = 1
    while start <= #text do
        local newline = string.find(text, "\n", start, true)
        local line
        if newline == nil then
            line = string.sub(text, start)
            start = #text + 1
        else
            line = string.sub(text, start, newline - 1)
            start = newline + 1
        end
        local tokens = tokenize(line)
        if #tokens > 0 then
            visit(tokens)
        end
    end
end

function profile.apply(seat, text, plan, difficulty)
    local keyword = plan_keyword(seat, difficulty)
    local computer = seat_is_computer(seat)
    local matched = plan
    if matched ~= 0 and matched ~= 1 then
        matched = 1
    end
    each_line(text, function(tokens)
        local command = tokens[1]
        if equal_nocase(command, "plan") then
            if plan_line(tokens, keyword) then
                matched = 1
            else
                matched = 0
            end
        elseif matched == 1 and equal_nocase(command, "weight") then
            apply_weight(seat, tokens[2] or "", read_factor(tokens[3] or ""))
        elseif matched == 1 and equal_nocase(command, "limit") and computer then
            apply_limit(seat, tokens[2] or "", read_limit(tokens[3] or ""))
        end
    end)
    return matched
end

function profile.downloadable(seat, locks, plan, difficulty)
    plan = 1
    local count = type_count(seat)
    for index = 1, count - 1 do
        local row = seat.types[index]
        local locked = 0
        if type(locks) == "table" then
            locked = locks[index] or 0
        end
        if type(row) == "table" and row.downloadable == 1 and locked ~= 1 then
            local text = row.ai_weight or ""
            if text ~= "" then
                plan = profile.apply(seat, text, plan, difficulty)
            end
        end
    end
    return plan
end

local function noted_difficulty(seat)
    local me = seat.me
    if me == nil then
        return nil
    end
    return difficulty_number(me.difficulty)
end

local function reapply(seat, starting)
    local saved = seat.plan or 1
    local noted = noted_difficulty(seat)
    local game = game_difficulty(seat)
    if noted == nil or noted == game then
        seat.plan = saved
        return
    end
    knowledge.reset(seat)
    local plan = profile.apply(seat, profile_text(seat), starting, noted)
    local computers = computer_count(seat)
    for _ = 1, computers do
        local weight_plan = 0
        weight_plan = profile.downloadable(seat, seat.weight_locked, weight_plan, noted)
        local limit_plan = 0
        profile.downloadable(seat, seat.limit_locked, limit_plan, noted)
    end
    if plan ~= 0 and plan ~= 1 then
        plan = saved
    end
    seat.plan = saved
end

function profile.apply_all(seat)
    local starting = seat.plan or 1
    local game = game_difficulty(seat)
    seat.plan = profile.apply(seat, profile_text(seat), starting, game)
    local computers = computer_count(seat)
    -- @quirk A downloadable type's ai_weight is applied once per computer
    -- player for each lock table, so a weight compounds by that count.
    for _ = 1, computers do
        seat.plan = profile.downloadable(seat, seat.weight_locked, seat.plan, game)
        seat.plan = profile.downloadable(seat, seat.limit_locked, seat.plan, game)
    end
    reapply(seat, starting)
end

function profile.apply_handed(seat)
    local saved = seat.plan or 1
    local game = game_difficulty(seat)
    local plan = profile.apply(seat, profile_text(seat), saved, game)
    plan = profile.downloadable(seat, seat.weight_locked, plan, game)
    profile.downloadable(seat, seat.limit_locked, plan, game)
    seat.plan = saved
end

function profile.reload(seat)
    knowledge.reset(seat)
    profile.apply_all(seat)
end

function profile.weight(seat, name, percent)
    local factor = percent
    if type(percent) ~= "number" then
        factor = 0
    end
    apply_weight(seat, name or "", f32(factor):tonumber())
end

function profile.limit(seat, name, count)
    local limit = count
    if type(count) ~= "number" then
        limit = 0
    else
        limit = truncate_to_int32(count)
    end
    apply_limit(seat, name or "", limit)
end

return profile
