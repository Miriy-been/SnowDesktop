-- weather.lua - 天气小组件
-- 通过 IP 定位（ipwho.is）获取经纬度，再查询 Open-Meteo 当前天气。
name = l10n.tr("lua_widget.weather.name")
useCustomStyle = true

bg = 0x0F172A
border = 0xFFFFFF
alpha = 0.38
borderAlpha = 0.16
gradientEndA = 0.28
textColor = 0xFFFFFF

local state = "loading"      -- loading / ok / error
local loading = false
local locationRequestInFlight = false
local lat = nil
local lon = nil

-- Font Awesome 6 Free Solid 天气图标码位
local icons = {
    sun              = utf8.char(0xF185), -- fa-sun
    cloudSun         = utf8.char(0xF6C4), -- fa-cloud-sun
    cloud            = utf8.char(0xF0C2), -- fa-cloud
    cloudRain        = utf8.char(0xF73D), -- fa-cloud-rain
    cloudShowers     = utf8.char(0xF740), -- fa-cloud-showers-heavy
    cloudBolt        = utf8.char(0xF76C), -- fa-cloud-bolt
    snowflake        = utf8.char(0xF2DC), -- fa-snowflake
    smog             = utf8.char(0xF75F), -- fa-smog
}

-- WMO 天气码 → { 图标, 描述 key }
local function weatherInfo(code)
    if code == 0 then
        return { glyph = icons.sun, key = "sunny" }
    elseif code == 1 or code == 2 then
        return { glyph = icons.cloudSun, key = "partly_cloudy" }
    elseif code == 3 then
        return { glyph = icons.cloud, key = "cloudy" }
    elseif code == 45 or code == 48 then
        return { glyph = icons.smog, key = "fog" }
    elseif code == 51 or code == 53 or code == 55 or code == 56 or code == 57 then
        return { glyph = icons.cloudRain, key = "drizzle" }
    elseif code == 61 or code == 63 or code == 65 or code == 80 or code == 81 or code == 82 then
        if code >= 80 then
            return { glyph = icons.cloudShowers, key = "showers" }
        end
        return { glyph = icons.cloudShowers, key = "rain" }
    elseif code == 66 or code == 67 then
        return { glyph = icons.cloudRain, key = "freezing_rain" }
    elseif code == 71 or code == 73 or code == 75 or code == 77 or code == 85 or code == 86 then
        if code >= 85 then
            return { glyph = icons.snowflake, key = "snow_showers" }
        end
        return { glyph = icons.snowflake, key = "snow" }
    elseif code >= 95 then
        return { glyph = icons.cloudBolt, key = "thunderstorm" }
    end
    return { glyph = icons.cloud, key = "cloudy" }
end

-- 最小 JSON 解析器（对象/数组/字符串/数字/bool/null）
local json = {}
function json.parse(text)
    if type(text) ~= "string" or text == "" then return nil end
    local pos = 1
    local len = #text

    local function skipWs()
        while pos <= len do
            local c = text:sub(pos, pos)
            if c == " " or c == "\t" or c == "\n" or c == "\r" then
                pos = pos + 1
            else
                break
            end
        end
    end

    local function parseString()
        -- text[pos] 为 '"'
        pos = pos + 1
        local out = {}
        while pos <= len do
            local c = text:sub(pos, pos)
            if c == '"' then
                pos = pos + 1
                return table.concat(out)
            elseif c == "\\" then
                pos = pos + 1
                local e = text:sub(pos, pos)
                if e == "n" then out[#out + 1] = "\n"
                elseif e == "t" then out[#out + 1] = "\t"
                elseif e == "r" then out[#out + 1] = "\r"
                elseif e == "\\" then out[#out + 1] = "\\"
                elseif e == '"' then out[#out + 1] = '"'
                elseif e == "/" then out[#out + 1] = "/"
                elseif e == "u" then
                    local hex = text:sub(pos + 1, pos + 4)
                    out[#out + 1] = utf8.char(tonumber(hex, 16) or 63)
                    pos = pos + 4
                else
                    out[#out + 1] = e
                end
                pos = pos + 1
            else
                out[#out + 1] = c
                pos = pos + 1
            end
        end
        return nil
    end

    local function parseNumber()
        local start = pos
        while pos <= len do
            local c = text:sub(pos, pos)
            if c:match("[%d%.eE%+%-]") then
                pos = pos + 1
            else
                break
            end
        end
        return tonumber(text:sub(start, pos - 1))
    end

    local parseValue
    parseValue = function()
        skipWs()
        if pos > len then return nil end
        local c = text:sub(pos, pos)
        if c == "{" then
            pos = pos + 1
            local obj = {}
            skipWs()
            if text:sub(pos, pos) == "}" then pos = pos + 1 return obj end
            while true do
                skipWs()
                local key = parseString()
                if not key then return nil end
                skipWs()
                if text:sub(pos, pos) ~= ":" then return nil end
                pos = pos + 1
                local v = parseValue()
                if v == nil then return nil end
                obj[key] = v
                skipWs()
                local d = text:sub(pos, pos)
                if d == "," then pos = pos + 1
                elseif d == "}" then pos = pos + 1 return obj
                else return nil end
            end
        elseif c == "[" then
            pos = pos + 1
            local arr = {}
            skipWs()
            if text:sub(pos, pos) == "]" then pos = pos + 1 return arr end
            while true do
                local v = parseValue()
                if v == nil then return nil end
                arr[#arr + 1] = v
                skipWs()
                local d = text:sub(pos, pos)
                if d == "," then pos = pos + 1
                elseif d == "]" then pos = pos + 1 return arr
                else return nil end
            end
        elseif c == '"' then
            return parseString()
        elseif c == "t" then pos = pos + 4 return true
        elseif c == "f" then pos = pos + 5 return false
        elseif c == "n" then pos = pos + 4 return nil
        else
            return parseNumber()
        end
    end

    local value = parseValue()
    return value
end

-- 读取配置
local function readConfig()
    local la = tonumber(storage.get("latitude"))
    local lo = tonumber(storage.get("longitude"))
    lat = (la ~= nil and la ~= 0) and la or nil
    lon = (lo ~= nil and lo ~= 0) and lo or nil
end

local function displayTemp(c)
    if c == nil then return nil end
    local useF = storage.get("unit") == "f"
    local v = useF and (c * 9 / 5 + 32) or c
    return math.floor(v + 0.5)
end

local function requestWeather()
    if not lat or not lon then return end
    local url = "https://api.open-meteo.com/v1/forecast?latitude="
        .. tostring(lat) .. "&longitude=" .. tostring(lon)
        .. "&current=temperature_2m,weather_code,relative_humidity_2m,wind_speed_10m&timezone=auto"
    local id = http.request({
        url = url,
        method = "GET",
        timeoutMs = 10000,
        cacheSeconds = 600,
        headers = { ["Accept"] = "application/json" }
    })
    if not id then
        loading = false
        state = "error"
        widget.invalidate()
    end
end

local function fetch()
    if loading then return end
    loading = true
    readConfig()
    if lat and lon then
        requestWeather()
    else
        locationRequestInFlight = true
        local id = http.request({
            url = "https://ipwho.is/",
            method = "GET",
            timeoutMs = 10000,
            cacheSeconds = 3600,
            headers = { ["Accept"] = "application/json" }
        })
        if not id then
            loading = false
            state = "error"
            widget.invalidate()
        end
    end
end

function onVisible()
    readConfig()
    widget.cancelTimer("weather-refresh")
    widget.setTimer("weather-refresh", 600000, true)
    if storage.get("temperature") == nil then
        fetch()
    else
        state = "ok"
    end
end

function onHidden()
    widget.cancelTimer("weather-refresh")
end

function onTimer(name)
    if name == "weather-refresh" then fetch() end
end

function onHttpResponse(id, response)
    loading = false
    if not response.ok then
        state = "error"
        widget.invalidate()
        return
    end
    local body = response.body
    if body == "" then
        state = "error"
        widget.invalidate()
        return
    end
    local data = json.parse(body)
    if not data then
        state = "error"
        widget.invalidate()
        return
    end
    if locationRequestInFlight then
        locationRequestInFlight = false
        local la = tonumber(data.latitude)
        local lo = tonumber(data.longitude)
        if la and lo then
            lat = la
            lon = lo
            if type(data.city) == "string" and data.city ~= "" then
                storage.set("city", data.city)
            end
            requestWeather()
        else
            state = "error"
            widget.invalidate()
        end
    else
        local cur = data.current
        if type(cur) == "table" then
            storage.set("temperature", tostring(cur.temperature_2m or 0))
            storage.set("weatherCode", tostring(cur.weather_code or 0))
            if cur.relative_humidity_2m ~= nil then
                storage.set("humidity", tostring(cur.relative_humidity_2m))
            end
            if cur.wind_speed_10m ~= nil then
                storage.set("windSpeed", tostring(cur.wind_speed_10m))
            end
            state = "ok"
            widget.invalidate()
        else
            state = "error"
            widget.invalidate()
        end
    end
end

function onUiAction(id, value)
    if id == "weather-retry" then
        state = "loading"
        widget.invalidate()
        fetch()
    end
end

function render()
    readConfig()
    local w = layout.width()
    local h = layout.height()
    local hasData = storage.get("temperature") ~= nil

    if state == "error" and not hasData then
        local msg = l10n.tr("lua_widget.weather.error")
        local fs = layout.fontCu(14)
        local m = draw.measureText(msg, fs, 0, false)
        draw.text((w - m.width) * 0.5, h * 0.25, msg, fs, 0xE06060)
        local bw = layout.cu(70)
        local bh = layout.fontCu(22)
        ui.button("weather-retry", l10n.tr("lua_widget.weather.retry"),
            (w - bw) * 0.5, h * 0.45, bw, bh)
        return
    end

    if not hasData then
        local msg = l10n.tr("lua_widget.weather.loading")
        local fs = layout.fontCu(14)
        local m = draw.measureText(msg, fs, 0, false)
        draw.text((w - m.width) * 0.5, (h - m.height) * 0.5, msg, fs, textColor)
        return
    end

    local temp = tonumber(storage.get("temperature")) or 0
    local code = tonumber(storage.get("weatherCode")) or 0
    local cityName = storage.get("city") or ""
    local hum = tonumber(storage.get("humidity"))
    local wind = tonumber(storage.get("windSpeed"))
    local info = weatherInfo(code)
    local cond = l10n.tr("lua_widget.weather." .. info.key)

    -- 内边距与可用区域（防重叠的流式布局基础）
    local pad = math.max(6, math.min(w, h) * 0.08)
    local innerW = math.max(1, w - pad * 2)
    local innerH = math.max(1, h - pad * 2)

    -- 视觉层级：温度(主) > 图标 > 描述 > 城市 > 湿度/风
    -- 温度字号随组件尺寸缩放，并有下限避免小尺寸下过小
    local tempText = tostring(displayTemp(temp) or 0) .. "°"
    local tempSize = math.max(
        layout.fontCu(12),
        math.min(innerH * 0.46, innerW * 0.34))
    local tempM = draw.measureText(tempText, tempSize, 0, true)
    local iconSize = math.max(layout.fontCu(8), tempSize * 1.45)
    local gapIcon = math.max(4, tempSize * 0.28)

    -- 图标 + 温度 横向块，超出时整体缩放
    local blockW = iconSize + gapIcon + tempM.width
    if blockW > innerW then
        local scale = innerW / blockW
        iconSize = iconSize * scale
        tempSize = tempSize * scale
        tempM = draw.measureText(tempText, tempSize, 0, true)
        gapIcon = math.max(4, tempSize * 0.28)
        blockW = iconSize + gapIcon + tempM.width
    end

    -- 主区（图标 + 温度）位于上部，垂直居中
    local mainBlockH = math.max(iconSize, tempM.height)
    local mainTop = pad + innerH * 0.10
    local mainCenterY = mainTop + mainBlockH * 0.5
    local mainLeft = pad + (innerW - blockW) * 0.5
    draw.fa(info.glyph, mainLeft, mainCenterY - iconSize * 0.5, iconSize, textColor)
    draw.text(mainLeft + iconSize + gapIcon, mainCenterY - tempM.height * 0.5,
        tempText, tempSize, textColor, tempM.width + 2, true)

    -- 天气描述：紧贴主区下方，中等字号、略淡
    local condFs = math.max(layout.fontCu(8), tempSize * 0.42)
    local condM = draw.measureText(cond, condFs, 0, false)
    local condY = mainTop + mainBlockH + innerH * 0.05
    if condY + condM.height > h - pad - innerH * 0.16 then
        condY = h - pad - innerH * 0.16 - condM.height
    end
    draw.text(pad + (innerW - condM.width) * 0.5, condY, cond,
        condFs, textColor, 0, false, false, 0, 0.82)

    -- 辅助区（底部）：湿度/风 最底，城市在其上（小字号、低层级）
    local cityText = (cityName ~= nil and cityName ~= "")
        and cityName or l10n.tr("lua_widget.weather.unknown_city")
    local details = {}
    if hum ~= nil then
        details[#details + 1] = l10n.tr("lua_widget.weather.humidity", tostring(hum))
    end
    if wind ~= nil then
        details[#details + 1] = l10n.tr("lua_widget.weather.wind", tostring(wind))
    end
    local detailText = table.concat(details, "  ")
    local bottomFs = math.max(layout.fontCu(7), tempSize * 0.32)

    local bottomBaseY = h - pad - bottomFs
    if detailText ~= "" then
        local dM = draw.measureText(detailText, bottomFs, 0, false)
        draw.text(pad + (innerW - dM.width) * 0.5, bottomBaseY,
            detailText, bottomFs, textColor, 0, false, false, 0, 0.55)
        local cityY = bottomBaseY - bottomFs - math.max(2, innerH * 0.02)
        local cityM = draw.measureText(cityText, bottomFs, 0, false)
        draw.text(pad + (innerW - cityM.width) * 0.5, cityY,
            cityText, bottomFs, 0xCBD5E1, 0, false, false, 0, 0.9)
    else
        local cityM = draw.measureText(cityText, bottomFs, 0, false)
        draw.text(pad + (innerW - cityM.width) * 0.5, bottomBaseY,
            cityText, bottomFs, 0xCBD5E1, 0, false, false, 0, 0.9)
    end
end
