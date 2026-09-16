-- weather/tests/test_json.lua - JSON decoder behavior on QWeather payloads.
local json = module.require("modules/json.lua")

return {
    ["decodes a QWeather now payload"] = function()
        local value = json.decode(
            '{"code":"200","now":{"temp":"28","text":"多云","humidity":"62"}}') -- l10n-allow: 模拟 QWeather 返回的中文天气文案
        assert(type(value) == "table", "decode failed")
        assert(value.code == "200", "code mismatch")
        assert(value.now.temp == "28", "temp mismatch")
        assert(value.now.text == "多云", "text mismatch") -- l10n-allow: API 原样中文字段
        return true
    end,
    ["decodes arrays, numbers and booleans"] = function()
        local value = json.decode('{"hourly":[{"temp":1},{"temp":-2.5}],"ok":true,"none":null}')
        assert(#value.hourly == 2, "array length mismatch")
        assert(value.hourly[1].temp == 1, "number mismatch")
        assert(value.hourly[2].temp == -2.5, "negative number mismatch")
        assert(value.ok == true, "boolean mismatch")
        assert(value.none == nil, "null mismatch")
        return true
    end,
    ["decodes escapes and surrogate pairs"] = function()
        local value = json.decode('"A\\u00e9\\u4e2d\\ud83d\\ude00\\n"') -- l10n-allow: 转义序列测试数据
        assert(value == "Aé中😀\n", "escape mismatch") -- l10n-allow: 转义序列测试数据
        return true
    end,
    ["rejects malformed input"] = function()
        local value, position = json.decode('{"code":}')
        assert(value == nil and position ~= nil, "should fail")
        local truncated = json.decode('{"code":"200"')
        assert(truncated == nil, "truncated input should fail")
        local trailing = json.decode('{"code":"200"} extra')
        assert(trailing == nil, "trailing content should fail")
        return true
    end,
}
