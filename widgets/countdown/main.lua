-- countdown.lua - 倒数日组件
-- 设置目标日期与事件标题，显示距离目标日期的剩余天数；
-- 支持目标日期已过去的正向计时（显示已过去多少天）。
name = l10n.tr("lua_widget.countdown.name")
useCustomStyle = true

bg = 0x0F172A
border = 0xFFFFFF
alpha = 0.38
borderAlpha = 0.16
gradientEndA = 0.28
textColor = 0xFFFFFF

settings = {
    fields = {
        { key = "name", label = l10n.tr("lua_widget.countdown.name_label"), type = "text", default = "" },
        { key = "date", label = l10n.tr("lua_widget.countdown.date_label"), type = "text", default = "" },
        { key = "textColor", label = l10n.tr("lua_widget.common.text_color"), type = "color", default = 0xFFFFFF },
    }
}

-- 解析 "YYYY-MM-DD"，返回 {y,m,d}；非法返回 nil
local function parseDate(s)
    if type(s) ~= "string" then return nil end
    local y, m, d = s:match("^(%d%d%d%d)%-(%d%d)%-(%d%d)$")
    if not y then return nil end
    y, m, d = tonumber(y), tonumber(m), tonumber(d)
    if y < 1 or y > 9999 or m < 1 or m > 12 or d < 1 or d > 31 then
        return nil
    end
    return { y = y, m = m, d = d }
end

-- 公历转"从纪元起的天数"（Howard Hinnant 算法），自 1970-01-01 偏移 719468
local function daysFromCivil(y, m, d)
    y = y - (m <= 2 and 1 or 0)
    local era = math.floor((y >= 0 and y or y - 399) / 400)
    local yoe = y - era * 400
    local doy = math.floor((153 * (m + (m > 2 and -3 or 9)) + 2) / 5) + d - 1
    local doe = yoe * 365 + math.floor(yoe / 4) - math.floor(yoe / 100) + doy
    return era * 146097 + doe - 719468
end

-- 今天的公历天数
local function todayDays()
    local t = sys.getTime()
    return daysFromCivil(t.year, t.month, t.day)
end

function onVisible()
    widget.cancelTimer("countdown-day")
    widget.setTimer("countdown-day", 3600000, true) -- 每小时检查一次翻日
end

function onHidden()
    widget.cancelTimer("countdown-day")
end

function onTimer(name)
    if name == "countdown-day" then widget.invalidate() end
end

function render()
    local w = layout.width()
    local h = layout.height()
    local eventName = storage.get("name") or ""
    local dateStr = storage.get("date") or ""
    local textCol = tonumber(storage.get("textColor")) or textColor

    local target = parseDate(dateStr)
    if not target then
        local fs = layout.fontCu(13)
        local msg = l10n.tr("lua_widget.countdown.set_date")
        local m = draw.measureText(msg, fs, 0, false)
        draw.text((w - m.width) * 0.5, (h - m.height) * 0.5, msg, fs, textCol)
        return
    end

    local diff = daysFromCivil(target.y, target.m, target.d) - todayDays()

    local label = (eventName ~= nil and eventName ~= "")
        and eventName or l10n.tr("lua_widget.countdown.default_name")
    local bigText, statusText
    if diff > 0 then
        bigText = tostring(diff)
        statusText = l10n.tr("lua_widget.countdown.days_left", label, tostring(diff))
    elseif diff < 0 then
        bigText = tostring(-diff)
        statusText = l10n.tr("lua_widget.countdown.days_passed", label, tostring(-diff))
    else
        bigText = "0"
        statusText = l10n.tr("lua_widget.countdown.today", label)
    end

    -- 三行布局：事件名(顶,小) / 大数字(中,主) / 状态(底,小)
    local nameFs = layout.fontCu(10)
    local statusFs = layout.fontCu(9)
    local pad = math.max(6, math.min(w, h) * 0.08)
    local innerW = math.max(1, w - pad * 2)
    local innerH = math.max(1, h - pad * 2)

    -- 事件名（若为空则用日期本身）
    local topText = label
    local topM = draw.measureText(topText, nameFs, 0, false)
    if topM.width > innerW then
        local s = innerW / math.max(1, topM.width)
        nameFs = math.max(layout.fontCu(6), nameFs * s)
        topM = draw.measureText(topText, nameFs, 0, false)
    end
    draw.text(pad + (innerW - topM.width) * 0.5, pad, topText, nameFs,
        textCol, 0, false, false, 0, 0.9)

    -- 大数字（主视觉），随组件尺寸缩放
    local numSize = math.max(layout.fontCu(14),
        math.min(innerH * 0.5, innerW * 0.36))
    local numM = draw.measureText(bigText, numSize, 0, true)
    if numM.width > innerW then
        local s = innerW / math.max(1, numM.width)
        numSize = numSize * s
        numM = draw.measureText(bigText, numSize, 0, true)
    end
    local numTop = pad + topM.height + innerH * 0.06
    draw.text(pad + (innerW - numM.width) * 0.5, numTop, bigText,
        numSize, textCol, 0, true)

    -- 状态文字（底部）
    local statusM = draw.measureText(statusText, statusFs, 0, false)
    local statusY = h - pad - statusFs
    if statusY < numTop + numM.height then
        statusY = numTop + numM.height + math.max(2, innerH * 0.03)
    end
    draw.text(pad + (innerW - statusM.width) * 0.5, statusY, statusText,
        statusFs, textCol, 0, false, false, 0, 0.75)
end
