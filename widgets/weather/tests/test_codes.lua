-- weather/tests/test_codes.lua - WMO mapping, weekday and night helpers.
local codes = module.require("modules/codes.lua")

return {
    ["maps WMO codes to day and night glyphs"] = function()
        assert(codes.wmoKind(0, false) == "sun", "0 day")
        assert(codes.wmoKind(0, true) == "moon", "0 night")
        assert(codes.wmoKind(2, false) == "cloudSun", "2 day")
        assert(codes.wmoKind(2, true) == "cloudMoon", "2 night")
        assert(codes.wmoKind(3, false) == "cloud", "overcast")
        assert(codes.wmoKind(nil, false) == "cloud", "nil code")
        return true
    end,
    ["maps WMO codes to precipitation and sky glyphs"] = function()
        assert(codes.wmoKind(45, false) == "fog", "fog")
        assert(codes.wmoKind(51, false) == "rain", "drizzle")
        assert(codes.wmoKind(61, false) == "rain", "rain")
        assert(codes.wmoKind(65, false) == "rain", "heavy rain falls back")
        assert(codes.wmoKind(82, false) == "rainHeavy", "violent showers")
        assert(codes.wmoKind(71, false) == "snow", "snow")
        assert(codes.wmoKind(86, false) == "snow", "snow showers")
        assert(codes.wmoKind(95, false) == "thunder", "thunderstorm")
        return true
    end,
    ["maps WMO codes to localization keys"] = function()
        assert(codes.wmoTextKey(0) == "lua_widget.weather.wmo_0", "clear")
        assert(codes.wmoTextKey(2) == "lua_widget.weather.wmo_2", "partly")
        assert(codes.wmoTextKey(48) == "lua_widget.weather.wmo_45", "rime fog")
        assert(codes.wmoTextKey(57) == "lua_widget.weather.wmo_51", "drizzle")
        assert(codes.wmoTextKey(65) == "lua_widget.weather.wmo_65", "heavy")
        assert(codes.wmoTextKey(99) == "lua_widget.weather.wmo_95", "storm")
        return true
    end,
    ["maps wind bearings to eight sectors"] = function()
        assert(codes.windKey(0) == "lua_widget.weather.wind_n", "north")
        assert(codes.windKey(135) == "lua_widget.weather.wind_se", "southeast")
        assert(codes.windKey(270) == "lua_widget.weather.wind_w", "west")
        assert(codes.windKey(359) == "lua_widget.weather.wind_n", "wrap")
        assert(codes.windKey(-90) == "lua_widget.weather.wind_n", "negative")
        return true
    end,
    ["picks geocoding candidates by match quality"] = function()
        local hangzhou = { name = "Hangzhou", latitude = 30.29 }
        local binjiang = { name = "Binjiang", latitude = 30.21 }
        local changhe = { name = "Changhe", latitude = 30.20 }
        -- Exact name match wins regardless of order.
        local exact = codes.pickLocation(
            { hangzhou, binjiang, changhe }, "binjiang")
        assert(exact == binjiang, "exact match")
        -- Prefix match beats the first candidate.
        local prefix = codes.pickLocation(
            { hangzhou, binjiang, changhe }, "bin")
        assert(prefix == binjiang, "prefix match")
        -- Otherwise the first candidate is kept.
        local fallback = codes.pickLocation(
            { hangzhou, binjiang }, "xxx")
        assert(fallback == hangzhou, "fallback")
        assert(codes.pickLocation(nil, "x") == nil, "no results")
        return true
    end,
    ["parses manual coordinate pairs"] = function()
        local lat, lon = codes.parseCoords("26.43,106.67")
        assert(lat == 26.43 and lon == 106.67, "plain pair")
        lat, lon = codes.parseCoords(" 26.43 , 106.67 ")
        assert(lat == 26.43 and lon == 106.67, "spaced pair")
        lat, lon = codes.parseCoords("-34.6,-58.4")
        assert(lat == -34.6 and lon == -58.4, "negative pair")
        assert(codes.parseCoords("杭州") == nil, "place name") -- l10n-allow: 地名应被拒绝解析为坐标
        assert(codes.parseCoords("91,10") == nil, "latitude range")
        assert(codes.parseCoords("10,200") == nil, "longitude range")
        return true
    end,
    ["computes weekdays from ISO dates"] = function()
        -- 2026-09-13 is a Sunday, 2026-09-19 a Saturday.
        assert(codes.weekdayFromISO("2026-09-13") == 0, "Sunday")
        assert(codes.weekdayFromISO("2026-09-14") == 1, "Monday")
        assert(codes.weekdayFromISO("2026-09-19") == 6, "Saturday")
        -- 2000-01-01 was a Saturday.
        assert(codes.weekdayFromISO("2000-01-01") == 6, "millennium")
        assert(codes.weekdayFromISO("bad") == nil, "malformed")
        return true
    end,
    ["resolves night time against sunrise and sunset"] = function()
        assert(codes.isNight("23:00", "06:00", "18:00") == true, "late")
        assert(codes.isNight("03:00", "06:00", "18:00") == true, "early")
        assert(codes.isNight("12:00", "06:00", "18:00") == false, "noon")
        assert(codes.isNight("18:00", "06:00", "18:00") == true, "sunset")
        assert(codes.isNight("12:00", nil, "18:00") == false, "missing rise")
        return true
    end,
    ["extracts hours from ISO datetimes"] = function()
        assert(codes.hourFromISO("2026-09-15T21:00") == 21, "21h")
        assert(codes.hourFromISO("bad") == nil, "malformed")
        return true
    end,
}
