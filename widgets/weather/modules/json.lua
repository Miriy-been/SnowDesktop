-- weather/modules/json.lua - minimal strict JSON decoder for QWeather payloads.
-- Pure Lua: only string/table/math/utf8, safe for the widget test sandbox.
-- All internal parsers return (ok, value, nextPos) to keep JSON `null`
-- unambiguous with parse failures.

local json = {}

local function skipWhitespace(text, pos)
    while pos <= #text do
        local c = text:sub(pos, pos)
        if c == " " or c == "\t" or c == "\n" or c == "\r" then
            pos = pos + 1
        else
            break
        end
    end
    return pos
end

local parseValue

local function parseString(text, pos)
    pos = skipWhitespace(text, pos)
    if text:sub(pos, pos) ~= '"' then return false, nil, pos end
    pos = pos + 1
    local parts = {}
    while pos <= #text do
        local c = text:sub(pos, pos)
        if c == '"' then
            return true, table.concat(parts), pos + 1
        elseif c == "\\" then
            local esc = text:sub(pos + 1, pos + 1)
            pos = pos + 2
            if esc == '"' then
                parts[#parts + 1] = "\""
            elseif esc == "\\" then
                parts[#parts + 1] = "\\"
            elseif esc == "/" then
                parts[#parts + 1] = "/"
            elseif esc == "b" then
                parts[#parts + 1] = "\b"
            elseif esc == "f" then
                parts[#parts + 1] = "\f"
            elseif esc == "n" then
                parts[#parts + 1] = "\n"
            elseif esc == "r" then
                parts[#parts + 1] = "\r"
            elseif esc == "t" then
                parts[#parts + 1] = "\t"
            elseif esc == "u" then
                local hex = tonumber(text:sub(pos, pos + 3), 16)
                if not hex then return false, nil, pos end
                pos = pos + 4
                if hex >= 0xD800 and hex <= 0xDBFF and
                    text:sub(pos, pos + 1) == "\\u" then
                    local low = tonumber(text:sub(pos + 2, pos + 5), 16)
                    if low and low >= 0xDC00 and low <= 0xDFFF then
                        hex = 0x10000 + (hex - 0xD800) * 0x400 + (low - 0xDC00)
                        pos = pos + 6
                    end
                end
                local ok, char = pcall(utf8.char, hex)
                if not ok then return false, nil, pos end
                parts[#parts + 1] = char
            else
                return false, nil, pos
            end
        else
            parts[#parts + 1] = c
            pos = pos + 1
        end
    end
    return false, nil, pos
end

local function parseNumber(text, pos)
    pos = skipWhitespace(text, pos)
    local start = pos
    if text:sub(pos, pos) == "-" then pos = pos + 1 end
    while true do
        local c = text:sub(pos, pos)
        if c:match("[%d%.eE%+%-]") then pos = pos + 1 else break end
    end
    local literal = text:sub(start, pos - 1)
    local value = literal ~= "" and literal ~= "-" and tonumber(literal) or nil
    if not value then return false, nil, start end
    return true, value, pos
end

local function parseLiteral(text, pos, literal, value)
    pos = skipWhitespace(text, pos)
    if text:sub(pos, pos + #literal - 1) == literal then
        return true, value, pos + #literal
    end
    return false, nil, pos
end

local function parseArray(text, pos)
    pos = skipWhitespace(text, pos + 1)
    local items = {}
    if text:sub(pos, pos) == "]" then return true, items, pos + 1 end
    while true do
        local ok, value
        ok, value, pos = parseValue(text, pos)
        if not ok then return false, nil, pos end
        items[#items + 1] = value
        pos = skipWhitespace(text, pos)
        local c = text:sub(pos, pos)
        if c == "," then
            pos = pos + 1
        elseif c == "]" then
            return true, items, pos + 1
        else
            return false, nil, pos
        end
    end
end

local function parseObject(text, pos)
    pos = skipWhitespace(text, pos + 1)
    local object = {}
    if text:sub(pos, pos) == "}" then return true, object, pos + 1 end
    while true do
        local ok, key
        ok, key, pos = parseString(text, pos)
        if not ok then return false, nil, pos end
        pos = skipWhitespace(text, pos)
        if text:sub(pos, pos) ~= ":" then return false, nil, pos end
        pos = pos + 1
        local value
        ok, value, pos = parseValue(text, pos)
        if not ok then return false, nil, pos end
        object[key] = value
        pos = skipWhitespace(text, pos)
        local c = text:sub(pos, pos)
        if c == "," then
            pos = pos + 1
        elseif c == "}" then
            return true, object, pos + 1
        else
            return false, nil, pos
        end
    end
end

parseValue = function(text, pos)
    pos = skipWhitespace(text, pos)
    local c = text:sub(pos, pos)
    if c == '"' then
        return parseString(text, pos)
    elseif c == "{" then
        return parseObject(text, pos)
    elseif c == "[" then
        return parseArray(text, pos)
    elseif c == "t" then
        return parseLiteral(text, pos, "true", true)
    elseif c == "f" then
        return parseLiteral(text, pos, "false", false)
    elseif c == "n" then
        return parseLiteral(text, pos, "null", nil)
    else
        return parseNumber(text, pos)
    end
end

--- Decode a JSON document. Returns value, errorPosition(nil on success).
function json.decode(text)
    if type(text) ~= "string" or text == "" then
        return nil, 1
    end
    local ok, value, pos = parseValue(text, 1)
    if not ok then
        return nil, pos
    end
    pos = skipWhitespace(text, pos)
    if pos <= #text then
        return nil, pos
    end
    return value, nil
end

return json
