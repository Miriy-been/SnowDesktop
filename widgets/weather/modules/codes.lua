-- weather/modules/codes.lua - pure mapping helpers for the weather widget.
-- WMO weather-code classification (Open-Meteo), weekday math and day/night
-- resolution. Only string/table/math are used, so the module runs in the
-- widget test sandbox.

local codes = {}

local KIND_KEYS = {
    "lua_widget.weather.wmo_0",
    "lua_widget.weather.wmo_1",
    "lua_widget.weather.wmo_2",
    "lua_widget.weather.wmo_3",
    "lua_widget.weather.wmo_45",
    "lua_widget.weather.wmo_51",
    "lua_widget.weather.wmo_61",
    "lua_widget.weather.wmo_63",
    "lua_widget.weather.wmo_65",
    "lua_widget.weather.wmo_71",
    "lua_widget.weather.wmo_75",
    "lua_widget.weather.wmo_95",
}

--- Map a WMO weather code to a localization key for the condition text.
function codes.wmoTextKey(code)
    local numeric = tonumber(code) or 3
    if numeric == 0 then return KIND_KEYS[1]
    elseif numeric == 1 then return KIND_KEYS[2]
    elseif numeric == 2 then return KIND_KEYS[3]
    elseif numeric == 3 then return KIND_KEYS[4]
    elseif numeric == 45 or numeric == 48 then return KIND_KEYS[5]
    elseif numeric >= 51 and numeric <= 57 then return KIND_KEYS[6]
    elseif numeric == 61 or numeric == 66 or numeric == 80 then
        return KIND_KEYS[7]
    elseif numeric == 63 or numeric == 81 then return KIND_KEYS[8]
    elseif numeric == 65 or numeric == 67 or numeric == 82 then
        return KIND_KEYS[9]
    elseif numeric == 71 or numeric == 73 or numeric == 77 or
        numeric == 85 then
        return KIND_KEYS[10]
    elseif numeric == 75 or numeric == 86 then return KIND_KEYS[11]
    elseif numeric >= 95 then return KIND_KEYS[12]
    end
    return KIND_KEYS[4]
end

--- Map a WMO weather code to a drawable glyph kind: sun, moon, cloudSun,
-- cloudMoon, cloud, rain, rainHeavy, thunder, snow, fog.
function codes.wmoKind(code, isNight)
    local numeric = tonumber(code) or 3
    if numeric == 0 then
        return isNight and "moon" or "sun"
    elseif numeric == 1 or numeric == 2 then
        return isNight and "cloudMoon" or "cloudSun"
    elseif numeric == 45 or numeric == 48 then
        return "fog"
    elseif numeric >= 51 and numeric <= 67 then
        return "rain"
    elseif numeric == 80 or numeric == 81 then
        return "rain"
    elseif numeric == 82 then
        return "rainHeavy"
    elseif numeric >= 71 and numeric <= 77 then
        return "snow"
    elseif numeric == 85 or numeric == 86 then
        return "snow"
    elseif numeric >= 95 then
        return "thunder"
    end
    return "cloud"
end

--- Map a wind bearing in degrees to one of eight localization keys.
function codes.windKey(degrees)
    local numeric = tonumber(degrees) or 0
    if numeric < 0 or numeric >= 360 then numeric = 0 end
    local bearing = math.floor((numeric + 22.5) / 45) % 8
    local keys = {
        "lua_widget.weather.wind_n",
        "lua_widget.weather.wind_ne",
        "lua_widget.weather.wind_e",
        "lua_widget.weather.wind_se",
        "lua_widget.weather.wind_s",
        "lua_widget.weather.wind_sw",
        "lua_widget.weather.wind_w",
        "lua_widget.weather.wind_nw",
    }
    return keys[bearing + 1]
end

--- Pick the best geocoding result for a query. Exact name matches win,
-- then prefix matches, then the first candidate. Supports district-level
-- queries such as "滨江区". Returns nil without usable candidates.
function codes.pickLocation(results, query)
    if type(results) ~= "table" or #results == 0 then return nil end
    local normalized = string.lower(tostring(query or ""))
    local prefix = nil
    for index = 1, #results do
        local candidate = results[index]
        local name = string.lower(tostring(candidate and candidate.name or ""))
        if name ~= "" then
            if name == normalized then return candidate end
            if not prefix and normalized ~= "" and
                name:sub(1, #normalized) == normalized then
                prefix = candidate
            end
        end
    end
    return prefix or results[1]
end

--- Parse a "latitude,longitude" coordinate pair such as "26.43, 106.67".
-- Returns two numbers when the value is a valid coordinate pair, otherwise
-- nil (letting the caller treat the value as a place-name query).
function codes.parseCoords(value)
    if type(value) ~= "string" then return nil end
    local latText, lonText = value:match(
        "^%s*(-?%d+%.?%d*)%s*,%s*(-?%d+%.?%d*)%s*$")
    local lat = tonumber(latText)
    local lon = tonumber(lonText)
    if not lat or not lon then return nil end
    if lat < -90 or lat > 90 or lon < -180 or lon > 180 then
        return nil
    end
    return lat, lon
end

--- Weekday index from an ISO date string "YYYY-MM-DD". 0 = Sunday .. 6 =
-- Saturday, via Zeller's congruence. Returns nil for malformed input.
function codes.weekdayFromISO(date)
    if type(date) ~= "string" then return nil end
    local year, month, day = date:match("^(%d%d%d%d)-(%d%d)-(%d%d)$")
    year = tonumber(year)
    month = tonumber(month)
    day = tonumber(day)
    if not year or not month or not day then return nil end
    if month <= 2 then
        year = year - 1
        month = month + 12
    end
    local century = math.floor(year / 100)
    local yearOfCentury = year % 100
    local h = (day + math.floor((month + 1) * 26 / 10) + yearOfCentury +
        math.floor(yearOfCentury / 4) + math.floor(century / 4) +
        5 * century) % 7
    return (h + 6) % 7
end

--- Whether the moment "HH:MM" falls outside [sunrise, sunset] (inclusive).
-- All arguments are "HH:MM" strings; missing values resolve to daytime.
function codes.isNight(time, sunrise, sunset)
    if type(time) ~= "string" or type(sunrise) ~= "string" or
        type(sunset) ~= "string" then
        return false
    end
    local hour, minute = time:match("^(%d%d?):(%d%d)$")
    local riseHour, riseMinute = sunrise:match("^(%d%d?):(%d%d)$")
    local setHour, setMinute = sunset:match("^(%d%d?):(%d%d)$")
    if not hour or not riseHour or not setHour then return false end
    local minutes = tonumber(hour) * 60 + tonumber(minute or 0)
    local rise = tonumber(riseHour) * 60 + tonumber(riseMinute or 0)
    local set = tonumber(setHour) * 60 + tonumber(setMinute or 0)
    return minutes < rise or minutes >= set
end

--- Numeric hour from an ISO datetime value like "2026-09-15T21:00".
function codes.hourFromISO(value)
    if type(value) ~= "string" then return nil end
    return tonumber(value:match("T(%d%d?):"))
end

return codes
