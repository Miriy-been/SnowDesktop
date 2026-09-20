-- weather/main.lua - Open-Meteo current conditions, hourly trend and 7-day
-- forecast with adaptive multi-size layout and vector weather glyphs.
-- Zero configuration: IP-based location via ipwho.is, optional manual city.
local descriptor
local json = module.require("modules/json.lua")
local codes = module.require("modules/codes.lua")

local fluent = {
    refresh = utf8.char(0xF13D),
}

local LANG_MAP = {
    ["zh-CN"] = "zh", ["zh-TW"] = "zh", ["en-US"] = "en",
    ["ja-JP"] = "ja", ["ko-KR"] = "ko", ["de-DE"] = "de",
    ["fr-FR"] = "fr", ["es-ES"] = "es", ["es-419"] = "es",
    ["pt-BR"] = "pt",
}

-- Literal weekday keys (index = weekday + 1, 0 = Sunday) so the localization
-- contract sees every key as referenced.
local WEEKDAY_KEYS = {
    "lua_widget.weather.day_w0",
    "lua_widget.weather.day_w1",
    "lua_widget.weather.day_w2",
    "lua_widget.weather.day_w3",
    "lua_widget.weather.day_w4",
    "lua_widget.weather.day_w5",
    "lua_widget.weather.day_w6",
}

local settings = {
    fields = {
        { key = "location", label = l10n.tr("lua_widget.weather.settings_location"),
            type = "text", default = "",
            description = l10n.tr("lua_widget.weather.settings_location_desc") },
        { key = "interval", label = l10n.tr("lua_widget.weather.settings_interval"),
            type = "int", default = 1800, min = 600, max = 21600 },
    },
}

local function componentMetrics()
    local row = ui.metrics().layoutRowHeight
    local scale = row / 28
    return {
        row = row,
        scale = scale,
        xs = 4 * scale,
        sm = 8 * scale,
        md = 12 * scale,
        lg = 16 * scale,
        caption = 10 * scale,
        small = 11 * scale,
        body = 12 * scale,
        title = 14 * scale,
    }
end

local function config()
    local interval = tonumber(storage.get("interval")) or 1800
    return {
        location = tostring(storage.get("location") or ""),
        interval = math.max(600, math.min(21600, interval)),
    }
end

local function palette(context)
    if context.theme and context.theme.mode == "light" then
        return {
            primary = 0x1E293B, secondary = 0x475569, tertiary = 0x64748B,
            faint = 0x94A3B8, warm = 0xB45309, cool = 0x0369A1,
            track = 0x334155, error = 0xB91C1C,
        }
    end
    return {
        primary = 0xF8FAFC, secondary = 0xE2E8F0, tertiary = 0xB6C2D4,
        faint = 0x8A97A8, warm = 0xFBBF24, cool = 0x7DD3FC,
        track = 0xF8FAFC, error = 0xFF8B8B,
    }
end

local function tr(key, ...)
    return l10n.tr(key, ...)
end

local function formatTemp(value)
    local number = tonumber(value)
    if not number then return tostring(value or "") .. "°" end
    local rounded = number >= 0 and math.floor(number + 0.5)
        or math.ceil(number - 0.5)
    return string.format("%d°", rounded)
end

local function urlEncode(value)
    return (tostring(value):gsub("[^%w%-_%.~]", function(char)
        return string.format("%%%02X", char:byte())
    end))
end

local function isoToday()
    local parts = time.parts(time.now())
    return string.format("%04d-%02d-%02d", parts.year, parts.month, parts.day)
end

local function isoHourStamp()
    local parts = time.parts(time.now())
    return string.format("%04d-%02d-%02dT%02d:00",
        parts.year, parts.month, parts.day, parts.hour)
end

local function isoClock(value)
    if type(value) ~= "string" then return value end
    return value:match("T(%d%d:%d%d)$") or value
end

local function currentNight(model)
    -- Preview-only knob (--storage previewNight=1) to render night glyphs.
    if model.forceNight then return true end
    local parts = time.parts(time.now())
    local today = string.format("%04d-%02d-%02d",
        parts.year, parts.month, parts.day)
    local sunrise, sunset
    local first = model.daily and model.daily[1]
    if first and first.fxDate == today then
        sunrise = first.sunrise
        sunset = first.sunset
    end
    return codes.isNight(
        string.format("%02d:%02d", parts.hour, parts.min),
        sunrise or "06:00", sunset or "18:00")
end

local function hourIsNight(hour, model)
    local first = model.daily and model.daily[1]
    if first and first.sunrise and first.sunset then
        return codes.isNight(string.format("%02d:00", hour),
            first.sunrise, first.sunset)
    end
    return hour < 6 or hour >= 18
end

---------------------------------------------------------------------
-- Vector weather glyphs
---------------------------------------------------------------------

--- Cloud silhouette from primitives: three overlapping circles plus a
-- rounded base bar (same opaque color, seams invisible). Primitive calls
-- are quota-safe; a 30-command path per cloud exceeded the render quota
-- when drawing 15+ glyphs per frame.
local function drawCloud(cx, cy, s, color, alpha)
    local a = alpha or 1
    draw.circle(cx - 0.26 * s, cy + 0.06 * s, 0.185 * s, color, a)
    draw.circle(cx + 0.02 * s, cy - 0.06 * s, 0.27 * s, color, a)
    draw.circle(cx + 0.30 * s, cy + 0.05 * s, 0.20 * s, color, a)
    draw.rect(cx - 0.42 * s, cy + 0.06 * s, 0.92 * s, 0.22 * s, color,
        0.11 * s, a)
end

local function drawSun(cx, cy, s, color, alpha)
    draw.circle(cx, cy, 0.24 * s, color, alpha or 1)
    local thickness = math.max(1, 0.045 * s)
    for index = 0, 7 do
        local angle = math.rad(index * 45)
        draw.line(cx + math.cos(angle) * 0.35 * s,
            cy + math.sin(angle) * 0.35 * s,
            cx + math.cos(angle) * 0.47 * s,
            cy + math.sin(angle) * 0.47 * s,
            thickness, color, (alpha or 1) * 0.9)
    end
end

--- Crescent geometry, precomputed once at load in unit space (unit r,
-- rotated -30°, recentered in the glyph box). drawMoon then only scales
-- and translates with plain arithmetic — no per-frame closures, keeping
-- the render quota for frames with 15+ glyphs.
local MOON_COMMANDS = (function()
    local unit = {
        { "move", 0.438, -0.899 },
        { "cubic", 0.017, -1.104, -0.488, -0.990, -0.781, -0.625 },
        { "cubic", -1.074, -0.259, -1.074, 0.259, -0.781, 0.625 },
        { "cubic", -0.488, 0.990, 0.017, 1.104, 0.438, 0.899 },
        { "cubic", -0.059, 0.899, -0.461, 0.497, -0.461, 0 },
        { "cubic", -0.461, -0.497, -0.059, -0.899, 0.438, -0.899 },
    }
    local cosT, sinT = 0.866, -0.5
    local out = {}
    for index = 1, #unit do
        local src = unit[index]
        local dst = { src[1] }
        for field = 2, #src, 2 do
            dst[#dst + 1] = src[field] * cosT - src[field + 1] * sinT + 0.42
            dst[#dst + 1] = src[field] * sinT + src[field + 1] * cosT + 0.18
        end
        out[index] = dst
    end
    return out
end)()

local function drawMoon(cx, cy, s, color, alpha)
    local r = 0.40 * s
    local path = {}
    for index = 1, #MOON_COMMANDS do
        local src = MOON_COMMANDS[index]
        if src[1] == "move" then
            path[index] = { op = "move", x = cx + src[2] * r,
                y = cy + src[3] * r }
        else
            path[index] = { op = "cubic", x1 = cx + src[2] * r,
                y1 = cy + src[3] * r, x2 = cx + src[4] * r,
                y2 = cy + src[5] * r, x = cx + src[6] * r,
                y = cy + src[7] * r }
        end
    end
    draw.path(path, { fillColor = color, alpha = alpha or 1 })
end

--- Four-point sparkle star centered at (cx, cy) with arm length s.
local function drawSparkle(cx, cy, s, color, alpha)
    draw.path({
        { op = "move", x = cx, y = cy - s },
        { op = "quadratic", x1 = cx + 0.12 * s, y1 = cy - 0.12 * s,
            x = cx + s, y = cy },
        { op = "quadratic", x1 = cx + 0.12 * s, y1 = cy + 0.12 * s,
            x = cx, y = cy + s },
        { op = "quadratic", x1 = cx - 0.12 * s, y1 = cy + 0.12 * s,
            x = cx - s, y = cy },
        { op = "quadratic", x1 = cx - 0.12 * s, y1 = cy - 0.12 * s,
            x = cx, y = cy - s },
    }, { fillColor = color, alpha = alpha or 1 })
end

local function drawNightSky(cx, cy, s, colors, alpha)
    drawMoon(cx, cy, s, colors.primary, alpha)
    -- Sparkle pair in the crescent's opening, like the reference icon.
    drawSparkle(cx + 0.30 * s, cy - 0.28 * s, 0.08 * s, colors.primary,
        (alpha or 1) * 0.95)
    drawSparkle(cx + 0.34 * s, cy + 0.02 * s, 0.05 * s, colors.primary,
        (alpha or 1) * 0.7)
end

local function drawWeatherGlyph(kind, cx, cy, s, colors)
    if kind == "sun" then
        drawSun(cx, cy, s, colors.warm)
    elseif kind == "moon" then
        drawNightSky(cx, cy, s, colors, 1)
    elseif kind == "cloudSun" then
        drawSun(cx + 0.24 * s, cy - 0.26 * s, 0.60 * s, colors.warm)
        drawCloud(cx - 0.02 * s, cy + 0.10 * s, 0.9 * s, colors.primary)
    elseif kind == "cloudMoon" then
        -- Classic "moon beside cloud": whole crescent visible in the sky,
        -- bottom tip just meeting the cloud shoulder.
        drawMoon(cx + 0.32 * s, cy - 0.32 * s, 0.48 * s, colors.primary)
        drawCloud(cx - 0.02 * s, cy + 0.10 * s, 0.9 * s, colors.primary)
    elseif kind == "rain" or kind == "rainHeavy" then
        drawCloud(cx - 0.02 * s, cy - 0.16 * s, 0.9 * s, colors.primary)
        local heavy = kind == "rainHeavy"
        local drops = heavy and 4 or 3
        local step = heavy and 0.17 or 0.24
        for index = 0, drops - 1 do
            local x = cx + (-0.26 + index * step) * s
            draw.line(x, cy + 0.22 * s, x - 0.05 * s, cy + 0.38 * s,
                math.max(1, 0.05 * s), colors.cool, 1)
        end
    elseif kind == "thunder" then
        drawCloud(cx - 0.02 * s, cy - 0.16 * s, 0.9 * s, colors.primary)
        draw.path({
            { op = "move", x = cx + 0.07 * s, y = cy + 0.06 * s },
            { op = "line", x = cx - 0.09 * s, y = cy + 0.30 * s },
            { op = "line", x = cx + 0.01 * s, y = cy + 0.30 * s },
            { op = "line", x = cx - 0.11 * s, y = cy + 0.52 * s },
            { op = "line", x = cx + 0.11 * s, y = cy + 0.24 * s },
            { op = "line", x = cx - 0.00 * s, y = cy + 0.24 * s },
            { op = "line", x = cx + 0.15 * s, y = cy + 0.06 * s },
        }, { fillColor = colors.warm, alpha = 1 })
    elseif kind == "snow" then
        drawCloud(cx - 0.02 * s, cy - 0.16 * s, 0.9 * s, colors.primary)
        for index = 0, 2 do
            draw.circle(cx + (-0.20 + index * 0.20) * s, cy + 0.32 * s,
                0.045 * s, colors.cool, 1)
        end
    elseif kind == "fog" then
        drawCloud(cx - 0.02 * s, cy - 0.18 * s, 0.85 * s, colors.primary)
        draw.line(cx - 0.30 * s, cy + 0.24 * s, cx + 0.26 * s, cy + 0.24 * s,
            math.max(1, 0.05 * s), colors.secondary, 0.85)
        draw.line(cx - 0.20 * s, cy + 0.36 * s, cx + 0.36 * s, cy + 0.36 * s,
            math.max(1, 0.05 * s), colors.secondary, 0.6)
    else
        drawCloud(cx, cy, s, colors.primary)
    end
end

---------------------------------------------------------------------
-- Data layer (Open-Meteo, zero configuration)
---------------------------------------------------------------------

local model_lat = 0
local model_lon = 0

local FORECAST_PARAMS =
    "current=temperature_2m,apparent_temperature,relative_humidity_2m," ..
    "weather_code,wind_direction_10m,wind_speed_10m" ..
    "&hourly=temperature_2m,weather_code,precipitation_probability" ..
    "&daily=weather_code,temperature_2m_max,temperature_2m_min,sunrise,sunset" ..
    "&timezone=auto&forecast_days=7"

local function buildRequestUrl(cfg, kind)
    if kind == "ip" then
        return "https://ipwho.is/"
    elseif kind == "geocode" then
        return "https://geocoding-api.open-meteo.com/v1/search?name=" ..
            urlEncode(cfg.location) .. "&count=10&format=json&language=" ..
            (LANG_MAP[l10n.language()] or "en")
    end
    return "https://api.open-meteo.com/v1/forecast?latitude=" ..
        tostring(model_lat or 0) .. "&longitude=" .. tostring(model_lon or 0) ..
        "&" .. FORECAST_PARAMS
end

local function startNext(model)
    if model.requestId then return end
    local kind = table.remove(model.queue, 1)
    if not kind then
        model.loading = false
        return
    end
    local requestId, requestError = task.start("network.request", {
        url = buildRequestUrl(config(), kind),
        timeoutMs = 12000,
        cacheSeconds = kind == "forecast" and 600 or 3600,
        maxBytes = 768 * 1024,
    })
    if requestId then
        model.requestId = requestId
        model.requestKind = kind
        model.loading = true
        model.error = nil
    else
        model.loading = false
        model.error = true
        model.queue = {}
        widget.log("warn", "network.request rejected: " ..
            tostring(requestError))
    end
end

local function refreshAll(model)
    local cfg = config()
    model.error = nil
    model.queue = {}
    local lat, lon = codes.parseCoords(cfg.location)
    if lat then
        -- Coordinate mode: skip place lookup entirely.
        model_lat = lat
        model_lon = lon
        model.city = cfg.location
        model.queue[1] = "forecast"
    elseif cfg.location ~= "" then
        model.queue[1] = "geocode"
        model.queue[2] = "forecast"
    else
        model.queue[1] = "ip"
        model.queue[2] = "forecast"
    end
    model.chainEpoch = time.monotonic()
    startNext(model)
end

local function applyForecast(model, payload)
    local current = type(payload.current) == "table" and payload.current or nil
    local hourly = type(payload.hourly) == "table" and payload.hourly or nil
    local daily = type(payload.daily) == "table" and payload.daily or nil
    if not current or not hourly or not daily or
        type(hourly.time) ~= "table" or type(daily.time) ~= "table" then
        return false
    end
    model.now = {
        temp = current.temperature_2m,
        feelsLike = current.apparent_temperature,
        humidity = current.relative_humidity_2m,
        wmo = current.weather_code,
        windDeg = current.wind_direction_10m,
        windSpeed = current.wind_speed_10m,
    }
    local stamp = isoHourStamp()
    model.hourly = {}
    local count = 0
    for index = 1, #hourly.time do
        if tostring(hourly.time[index]) >= stamp and count < 8 then
            count = count + 1
            model.hourly[count] = {
                time = tostring(hourly.time[index]),
                temp = hourly.temperature_2m[index],
                wmo = hourly.weather_code[index],
                pop = hourly.precipitation_probability[index],
            }
        end
    end
    model.daily = {}
    for index = 1, math.min(#daily.time, 7) do
        model.daily[index] = {
            fxDate = tostring(daily.time[index]),
            tempMax = daily.temperature_2m_max[index],
            tempMin = daily.temperature_2m_min[index],
            wmo = daily.weather_code[index],
            sunrise = isoClock(tostring(daily.sunrise[index] or "")),
            sunset = isoClock(tostring(daily.sunset[index] or "")),
        }
    end
    return true
end

local function finishChain(model)
    model.loading = false
    model.error = nil
    model.updatedEpoch = time.now()
    storage.set("snapshot", {
        now = model.now,
        hourly = model.hourly,
        daily = model.daily,
        city = model.city,
        updatedEpoch = model.updatedEpoch,
    })
    widget.invalidate()
end

local function handleTaskComplete(model, value)
    if value.taskId ~= model.requestId then return end
    model.requestId = nil
    local kind = model.requestKind
    model.requestKind = nil
    if not value.ok or type(value.value) ~= "table" or
        type(value.value.body) ~= "string" or value.value.body == "" then
        model.loading = false
        model.error = true
        model.queue = {}
        widget.invalidate()
        return
    end
    local payload = json.decode(value.value.body)
    if kind == "ip" then
        if type(payload) ~= "table" or tonumber(payload.latitude) == nil then
            model.loading = false
            model.error = true
            model.queue = {}
            widget.invalidate()
            return
        end
        model_lat = tonumber(payload.latitude) or 0
        model_lon = tonumber(payload.longitude) or 0
        model.city = tostring(payload.city or "")
    elseif kind == "geocode" then
        local chosen = codes.pickLocation(payload.results, config().location)
        if not chosen or tonumber(chosen.latitude) == nil then
            model.loading = false
            model.error = true
            model.queue = {}
            widget.invalidate()
            return
        end
        model_lat = tonumber(chosen.latitude) or 0
        model_lon = tonumber(chosen.longitude) or 0
        model.city = tostring(chosen.name or "")
    elseif kind == "forecast" then
        if type(payload) ~= "table" or not applyForecast(model, payload) then
            model.loading = false
            model.error = true
            model.queue = {}
            widget.invalidate()
            return
        end
        finishChain(model)
        return
    end
    startNext(model)
end

local function applySnapshot(model, snapshot)
    if type(snapshot) ~= "table" then return end
    if type(snapshot.now) == "table" then model.now = snapshot.now end
    if type(snapshot.hourly) == "table" then model.hourly = snapshot.hourly end
    if type(snapshot.daily) == "table" then model.daily = snapshot.daily end
    model.city = tostring(snapshot.city or "")
    model.updatedEpoch = tonumber(snapshot.updatedEpoch) or nil
end

local function loadPreview(model)
    model.forceNight = storage.get("previewNight") == "1"
    model.city = tr("lua_widget.weather.preview_city")
    model.now = {
        temp = 28, feelsLike = 30, humidity = 62, wmo = 2,
        windDeg = 135, windSpeed = 12,
    }
    local temps = { 28, 28, 27, 26, 25, 24, 24, 23 }
    local wmos = { 2, 0, 1, 61, 63, 3, 1, 0 }
    local pops = { 0, 0, 10, 46, 72, 20, 0, 0 }
    model.hourly = {}
    for index = 1, 8 do
        local epoch = time.now() + (index - 1) * 3600000
        local hourParts = time.parts(epoch)
        model.hourly[index] = {
            time = string.format("%04d-%02d-%02dT%02d:00",
                hourParts.year, hourParts.month, hourParts.day,
                hourParts.hour),
            temp = temps[index], wmo = wmos[index], pop = pops[index],
        }
    end
    local maxTemps = { 28, 30, 26, 24, 27, 31, 33 }
    local minTemps = { 19, 21, 18, 16, 19, 22, 24 }
    local dayWmos = { 2, 0, 61, 65, 3, 0, 2 }
    model.daily = {}
    for index = 1, 7 do
        local epoch = time.now() + (index - 1) * 86400000
        local dayParts = time.parts(epoch)
        local date = string.format("%04d-%02d-%02d",
            dayParts.year, dayParts.month, dayParts.day)
        model.daily[index] = {
            fxDate = date,
            tempMax = maxTemps[index], tempMin = minTemps[index],
            wmo = dayWmos[index],
            -- Clock-only times, same shape as isoClock() in applyForecast.
            sunrise = "06:00", sunset = "18:00",
        }
    end
end

---------------------------------------------------------------------
-- Render helpers
---------------------------------------------------------------------

local function drawCenteredText(cx, y, text, font, color, maxWidth, bold,
    alpha)
    local measured = draw.measureText(text, font, 0, bold or false)
    local width = math.max(measured.width, 1)
    draw.text(cx - width / 2, y, text, font, color,
        math.max(maxWidth or width, width + 2), bold or false, true,
        nil, alpha)
    return measured.height
end

local function drawRightText(rightX, y, text, font, color, bold)
    local measured = draw.measureText(text, font, 0, bold or false)
    draw.text(rightX - measured.width, y, text, font, color,
        measured.width + 2, bold or false, true)
    return measured.height
end

local function detailParts(model)
    local now = model.now
    if not now then return {} end
    local parts = {}
    if now.feelsLike ~= nil and tostring(now.feelsLike) ~= "" then
        parts[#parts + 1] = tr("lua_widget.weather.feels_like_fmt",
            formatTemp(now.feelsLike))
    end
    if now.humidity ~= nil and tostring(now.humidity) ~= "" then
        parts[#parts + 1] = tr("lua_widget.weather.humidity_fmt",
            tostring(now.humidity))
    end
    if now.windSpeed ~= nil and tonumber(now.windSpeed) ~= nil then
        parts[#parts + 1] = tr("lua_widget.weather.wind_fmt",
            tr(codes.windKey(now.windDeg)), tostring(math.floor(
                tonumber(now.windSpeed) + 0.5)))
    end
    return parts
end

--- Join detail parts with " ·", dropping trailing parts until the text fits
-- maxWidth (never cutting mid-word). A small safety margin absorbs the
-- measure/render discrepancy so the host ellipsis never engages.
local function fitDetails(parts, font, maxWidth)
    local budget = math.max(10, maxWidth - 6)
    while #parts > 1 do
        local text = table.concat(parts, " · ")
        if draw.measureText(text, font, 0, false).width <= budget then
            return text
        end
        table.remove(parts)
    end
    return parts[1] or ""
end

local function drawHero(x, y, w, h, model, colors, m, mode)
    local now = model.now
    if not now then return end
    local kind = codes.wmoKind(now.wmo, currentNight(model))
    -- Weather condition and city share one line: "晴 · 杭州".
    local condition = tr(codes.wmoTextKey(now.wmo))
    if model.city ~= "" then
        condition = condition .. " · " .. model.city
    end
    if mode == "stacked" then
        local details = fitDetails(detailParts(model), m.caption,
            math.max(20, w - m.sm))
        local iconS = math.min(w * 0.50, h * 0.34)
        local tempFont = math.min(w * 0.26, h * 0.24)
        local tempText = formatTemp(now.temp)
        local tempM = draw.measureText(tempText, tempFont, 0, false)
        local condM = draw.measureText(condition, m.title, 0, false)
        local detailM = details ~= ""
            and draw.measureText(details, m.caption, 0, false) or nil
        local stackH = iconS + m.sm + tempM.height + m.xs + condM.height
        if detailM and stackH + m.xs + detailM.height <= h then
            stackH = stackH + m.xs + detailM.height
        else
            detailM = nil
        end
        local cursor = y + math.max(0, (h - stackH) / 2)
        drawWeatherGlyph(kind, x + w / 2, cursor + iconS / 2, iconS, colors)
        cursor = cursor + iconS + m.sm
        drawCenteredText(x + w / 2, cursor, tempText, tempFont,
            colors.primary, w, true)
        cursor = cursor + tempM.height + m.xs
        drawCenteredText(x + w / 2, cursor, condition, m.title,
            colors.secondary, w)
        cursor = cursor + condM.height + m.xs
        if detailM then
            drawCenteredText(x + w / 2, cursor, details, m.caption,
                colors.tertiary, w)
        end
    else
        local iconS = math.min(h * 0.84, w * 0.32)
        drawWeatherGlyph(kind, x + w - iconS / 2, y + h * 0.52, iconS, colors)
        local textW = w - iconS - m.sm * 2
        local details = fitDetails(detailParts(model), m.caption,
            math.max(20, textW))
        local tempFont = math.min(h * 0.54, textW * 0.34)
        local tempText = formatTemp(now.temp)
        local tempM = draw.measureText(tempText, tempFont, 0, false)
        local condM = draw.measureText(condition, m.title, 0, false)
        local detailM = details ~= ""
            and draw.measureText(details, m.caption, 0, false) or nil
        local stackH = tempM.height + m.xs + condM.height
        if detailM then stackH = stackH + m.xs + detailM.height end
        local cursor = y + math.max(0, (h - stackH) / 2)
        draw.text(x, cursor, tempText, tempFont, colors.primary, textW,
            false, true)
        cursor = cursor + tempM.height + m.xs
        draw.text(x, cursor, condition, m.title, colors.secondary, textW,
            false, true)
        cursor = cursor + condM.height + m.xs
        if detailM then
            draw.text(x, cursor, details, m.caption, colors.tertiary, textW,
                false, true)
        end
    end
end

local function drawHourlyStrip(x, y, w, h, model, colors, m, night)
    local count = math.min(#model.hourly, 8)
    if count <= 0 then return end
    count = math.max(1, math.min(count, math.floor(w / (44 * m.scale))))
    local cellW = w / count
    local labelFont = m.caption
    local tempFont = m.small
    local labelH = draw.measureText("00", labelFont, 0, false).height
    local tempH = draw.measureText("00°", tempFont, 0, false).height
    local iconS = math.min(cellW * 0.60, h - labelH - tempH - m.xs * 3)
    iconS = math.max(iconS, 8)
    for index = 1, count do
        local hour = model.hourly[index]
        local cx = x + (index - 0.5) * cellW
        local label
        if index == 1 then
            label = tr("lua_widget.weather.hour_now")
        else
            local hourNumber = codes.hourFromISO(hour.time)
            label = hourNumber
                and tr("lua_widget.weather.hour_fmt", tostring(hourNumber))
                or ""
        end
        drawCenteredText(cx, y, label, labelFont, colors.faint, cellW)
        local iconY = y + labelH + m.xs
        local kind = codes.wmoKind(hour.wmo,
            hourIsNight(codes.hourFromISO(hour.time) or 12, model))
        drawWeatherGlyph(kind, cx, iconY + iconS / 2, iconS, colors)
        local tempY = iconY + iconS + m.xs
        drawCenteredText(cx, tempY, formatTemp(hour.temp), tempFont,
            colors.secondary, cellW)
        local pop = tonumber(hour.pop)
        if pop and pop >= 30 then
            drawCenteredText(cx, tempY + tempH + 1, pop .. "%", m.caption,
                colors.cool, cellW)
        end
    end
end

local function drawDaily(x, y, w, h, model, colors, m, night, withBar)
    -- Minimum readable row height; never overlap rows when the surface is
    -- short — drop the trailing days instead.
    local minRowH = math.max(m.row * 0.82, 16 * m.scale)
    local maxRows = math.max(1, math.floor(h / minRowH))
    local rowCount = math.min(#model.daily, 7, maxRows)
    if rowCount <= 0 or h < minRowH * 0.6 then return end
    local rowH = math.min(m.row * 1.14, h / rowCount)
    local today = isoToday()
    local tomorrowEpoch = time.now() + 86400000
    local tomorrowParts = time.parts(tomorrowEpoch)
    local tomorrow = string.format("%04d-%02d-%02d",
        tomorrowParts.year, tomorrowParts.month, tomorrowParts.day)
    local weekMin, weekMax
    for index = 1, rowCount do
        local low = tonumber(model.daily[index].tempMin)
        local high = tonumber(model.daily[index].tempMax)
        if low and high then
            weekMin = weekMin and math.min(weekMin, low) or low
            weekMax = weekMax and math.max(weekMax, high) or high
        end
    end
    if weekMin == nil then weekMin, weekMax = 0, 1 end
    if weekMax <= weekMin then weekMax = weekMin + 1 end
    local iconS = math.min(rowH * 0.72, 22 * m.scale)
    local tempFont = m.small
    local labelW = 52 * m.scale
    local iconZone = iconS + m.sm
    local barW = math.min(w * 0.26, 84 * m.scale)
    for index = 1, rowCount do
        local day = model.daily[index]
        local rowY = y + (index - 1) * rowH
        local label
        if day.fxDate == today then
            label = tr("lua_widget.weather.day_today")
        elseif day.fxDate == tomorrow then
            label = tr("lua_widget.weather.day_tomorrow")
        else
            local weekday = codes.weekdayFromISO(day.fxDate) or 1
            label = tr(WEEKDAY_KEYS[weekday + 1])
        end
        draw.text(x, rowY + (rowH - draw.measureText(label, m.small, 0,
            false).height) / 2, label, m.small, colors.secondary,
            labelW, false, true)
        local kind = codes.wmoKind(day.wmo, night)
        drawWeatherGlyph(kind, x + labelW + iconZone / 2,
            rowY + rowH / 2, iconS, colors)
        local textCenterY = rowY + (rowH - draw.measureText("00°", tempFont,
            0, false).height) / 2
        if withBar and weekMax > weekMin then
            local maxText = formatTemp(day.tempMax)
            local maxM = draw.measureText(maxText, tempFont, 0, false)
            draw.text(x + w - maxM.width, textCenterY, maxText, tempFont,
                colors.primary, maxM.width + 2, false, true)
            local minText = formatTemp(day.tempMin)
            local minM = draw.measureText(minText, tempFont, 0, false)
            -- Range bar sits between the two temperatures, never under
            -- either text.
            local trackStart = x + w - maxM.width - m.sm - barW
            local minRight = trackStart - m.sm
            draw.text(minRight - minM.width, textCenterY, minText, tempFont,
                colors.tertiary, minM.width + 2, false, true)
            local low = tonumber(day.tempMin) or weekMin
            local high = tonumber(day.tempMax) or weekMax
            if trackStart > minRight + math.max(1, m.scale) then
                local midY = rowY + rowH / 2
                draw.line(trackStart, midY, trackStart + barW, midY,
                    math.max(1, 3 * m.scale), colors.track, 0.14)
                local barStart = trackStart + barW * math.max(0,
                    math.min(1, (low - weekMin) / (weekMax - weekMin)))
                local barEnd = trackStart + barW * math.max(0,
                    math.min(1, (high - weekMin) / (weekMax - weekMin)))
                if barEnd > barStart then
                    draw.gradientRect(barStart,
                        midY - 1.5 * m.scale, barEnd - barStart,
                        3 * m.scale, colors.cool, colors.warm,
                        "horizontal", 1.5 * m.scale, 0.85)
                end
            end
        else
            local rangeText = formatTemp(day.tempMin) .. " / " ..
                formatTemp(day.tempMax)
            drawRightText(x + w, textCenterY, rangeText, tempFont,
                colors.secondary, false)
        end
        if index < rowCount then
            draw.line(x, rowY + rowH - math.max(1, m.scale), x + w,
                rowY + rowH - math.max(1, m.scale),
                math.max(1, m.scale), colors.track, 0.07)
        end
    end
end

local function drawStateCard(model, width, height, colors, m, kind, text,
    textFont, textColor, hint)
    local iconS = math.min(width * 0.38, m.row * 2.8)
    drawWeatherGlyph(kind, width / 2, height * 0.26 + iconS / 2, iconS, colors)
    local cursor = height * 0.26 + iconS + m.md
    cursor = cursor + drawCenteredText(width / 2, cursor, text, textFont,
        textColor, width - m.md * 2) + m.xs
    if hint then
        drawCenteredText(width / 2, cursor, hint, m.caption, colors.faint,
            width - m.md * 2)
    end
end

local function render(context, model)
    local width = layout.contentWidth()
    local height = layout.contentHeight()
    if width <= 0 or height <= 0 then return end
    local colors = palette(context)
    local m = componentMetrics()
    local pad = m.md
    local unit = math.max(1, m.scale)
    interaction.region({
        key = "weather.surface",
        shape = { type = "rect", x = 0, y = 0, width = width, height = height },
        events = { contextMenu = { id = "weather.menu", scope = "component" } },
        accessibility = { role = "group", label = descriptor.name },
    })

    local title = model.city ~= "" and model.city
        or tr("lua_widget.weather.name")
    widget.setTitle(title)

    if not model.now and model.error then
        drawStateCard(model, width, height, colors, m, "rain",
            tr("lua_widget.weather.request_failed"), m.body, colors.error,
            tr("lua_widget.weather.settings_hint"))
        return
    end
    if not model.now then
        drawStateCard(model, width, height, colors, m, "cloud",
            tr("lua_widget.weather.loading"), m.body, colors.secondary, nil)
        return
    end

    local night = currentNight(model)
    local ratio = width / height
    local isWide = ratio >= 1.95
    local isTall = ratio < 0.95

    if isWide then
        local heroW = math.max(width * 0.30, m.row * 5.5)
        drawHero(pad, pad, heroW, height - pad * 2, model, colors, m,
            "stacked")
        local rightX = pad + heroW + m.lg
        local rightW = width - rightX - pad
        if rightW > 80 * m.scale then
            local stripH = m.row * 2.4
            local remaining = height - pad * 2 - stripH - m.md
            if remaining >= m.row * 0.8 then
                drawHourlyStrip(rightX, pad, rightW, stripH, model, colors,
                    m, night)
                local dividerY = pad + stripH + m.md / 2
                draw.line(rightX, dividerY, width - pad, dividerY, unit,
                    colors.track, 0.10)
                drawDaily(rightX, pad + stripH + m.md, rightW, remaining,
                    model, colors, m, night, true)
            else
                drawHourlyStrip(rightX, pad, rightW, height - pad * 2, model,
                    colors, m, night)
            end
        end
        return
    end

    -- Compact and standard: hero on top, secondary content below.
    local heroH = math.max(m.row * 2.3,
        math.min(height * 0.46, m.row * 4.4))
    if isTall then
        drawHero(pad, pad, width - pad * 2, heroH, model, colors, m,
            "stacked")
    else
        drawHero(pad, pad, width - pad * 2, heroH, model, colors, m, "row")
    end
    local cursorY = pad + heroH
    local availH = height - cursorY - pad

    if availH < m.row * 1.2 then return end
    -- Breathing gap between the hero text block and the hourly strip.
    local stripY = cursorY + m.sm
    local stripH = m.row * 2.3
    if #model.hourly > 0 and availH >= stripH + m.sm then
        drawHourlyStrip(pad, stripY, width - pad * 2, stripH, model, colors,
            m, night)
        cursorY = stripY + stripH
        availH = height - cursorY - pad
    end
    if #model.daily > 0 and availH >= m.row * 1.1 then
        drawDaily(pad, cursorY + m.xs, width - pad * 2,
            availH - m.xs, model, colors, m, night,
            not isTall and width >= 250 * m.scale)
    end
end

---------------------------------------------------------------------
-- Lifecycle
---------------------------------------------------------------------

local function setup(context)
    local model = {
        now = nil, hourly = {}, daily = {}, city = "",
        loading = false, error = nil,
        requestId = nil, requestKind = nil, queue = {}, chainEpoch = 0,
        updatedEpoch = nil,
    }
    widget.setTitle(tr("lua_widget.weather.name"))
    schedule.every("weather-tick", 60000, { whenHidden = "pause" })
    if context.preview then
        loadPreview(model)
        return model
    end
    applySnapshot(model, storage.get("snapshot"))
    refreshAll(model)
    return model
end

local function event(_context, model, value)
    if value.kind == "environment" then
        widget.setTitle(model.city ~= "" and model.city
            or tr("lua_widget.weather.name"))
        return
    elseif value.kind == "schedule" and value.id == "weather-tick" then
        local cfg = config()
        if not model.requestId and
            time.monotonic() - (model.chainEpoch or 0) >=
                cfg.interval * 1000 then
            refreshAll(model)
        end
        return
    elseif value.kind == "settings.changed" then
        if value.preview then return end
        local keys = value.keys or {}
        for _, key in ipairs(keys) do
            if key == "location" then
                refreshAll(model)
                break
            end
        end
        return
    elseif value.kind == "task.complete" then
        if value.taskId == model.requestId then
            handleTaskComplete(model, value)
        end
        return
    elseif value.kind ~= "action" then
        return
    end

    if value.id == "weather.refresh" then
        refreshAll(model)
    end
end

local function menu(_context, _model, request)
    if request.id ~= "weather.menu" then return nil end
    return ui.menu({
        { id = "weather.refresh",
            label = tr("lua_widget.weather.refresh_now"),
            icon = fluent.refresh, iconFont = "fluent" },
    })
end

local function dispose(_context, model)
    if model.requestId then task.cancel(model.requestId) end
    schedule.cancel("weather-tick")
end

descriptor = {
    name = tr("lua_widget.weather.name"),
    useCustomStyle = true,
    followPersonalizationDefault = true,
    bottomBarHover = false,
    bg = 0x14283E,
    border = 0xFFFFFF,
    alpha = 0.40,
    borderAlpha = 0.16,
    gradientEndA = 0.28,
    settings = settings,
    setup = setup,
    render = render,
    event = event,
    menu = menu,
    dispose = dispose,
}

return widget.define(descriptor)
