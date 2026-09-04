-- audio_viz.lua - 音乐频谱组件
-- 读取系统当前播放音频的频谱（audio.peaks），渲染一排随音量跳动的竖条；
-- 中间频段高、两端平滑衰减形成拱形轮廓；无音乐时保持低矮静止。
name = l10n.tr("lua_widget.audio_viz.name")
useCustomStyle = true

bg = 0x000000
border = 0x000000
alpha = 0.0
borderAlpha = 0.0
gradientEndA = 0.0
textColor = 0xFFFFFF

settings = {
    fields = {
        { key = "preset", label = l10n.tr("lua_widget.audio_viz.preset"), type = "select",
          default = l10n.tr("lua_widget.audio_viz.preset_standard"),
          options = {
              l10n.tr("lua_widget.audio_viz.preset_standard"),
              l10n.tr("lua_widget.audio_viz.preset_soft"),
              l10n.tr("lua_widget.audio_viz.preset_punchy"),
              l10n.tr("lua_widget.audio_viz.preset_custom")
          } },
        { key = "barColor", label = l10n.tr("lua_widget.audio_viz.bar_color"), type = "color", default = 0xFFFFFF },
        { key = "barWidth", label = l10n.tr("lua_widget.audio_viz.bar_width"), type = "float", default = 4.0, min = 2.0, max = 24.0 },
        { key = "barGap", label = l10n.tr("lua_widget.audio_viz.bar_gap"), type = "float", default = 7.5, min = 1.0, max = 16.0 },
        { key = "sensitivity", label = l10n.tr("lua_widget.audio_viz.sensitivity"), type = "float", default = 0.70, min = 0.3, max = 3.0 },
        { key = "balance", label = l10n.tr("lua_widget.audio_viz.balance"), type = "float", default = 0.15, min = -1.0, max = 1.0 },
        { key = "smooth", label = l10n.tr("lua_widget.audio_viz.smooth"), type = "int", default = 6, min = 1, max = 16 },
        { key = "bars", label = l10n.tr("lua_widget.audio_viz.bars_label"), type = "float", default = 48.0, min = 8.0, max = 96.0 },
        { key = "attack", label = l10n.tr("lua_widget.audio_viz.attack"), type = "float", default = 0.48, min = 0.01, max = 0.99,
          visibleKey = "preset", visibleValue = l10n.tr("lua_widget.audio_viz.preset_custom") },
        { key = "release", label = l10n.tr("lua_widget.audio_viz.release"), type = "float", default = 0.40, min = 0.01, max = 0.99,
          visibleKey = "preset", visibleValue = l10n.tr("lua_widget.audio_viz.preset_custom") },
        { key = "activeThreshold", label = l10n.tr("lua_widget.audio_viz.active_threshold"), type = "float", default = 0.05, min = 0.0, max = 0.5,
          visibleKey = "preset", visibleValue = l10n.tr("lua_widget.audio_viz.preset_custom") },
    }
}

local function clamp(v, lo, hi)
    return math.max(lo, math.min(hi, v))
end

-- 上次已请求宿主的列数；仅在计算值变化时重新请求，避免每帧触发。
-- 不持久化：用户改设置后应即时生效，不被旧值卡住。
local lastRequestedCols = 0

function onVisible()
    widget.cancelTimer("audio-viz-tick")
    widget.setTimer("audio-viz-tick", 24, true) -- ~42fps，匹配 43Hz 频谱数据源
end

function onHidden()
    widget.cancelTimer("audio-viz-tick")
end

function onTimer(name)
    if name == "audio-viz-tick" then widget.invalidate() end
end

function render()
    local w = layout.width()
    local h = layout.height()

    local barColor = tonumber(storage.get("barColor")) or 0xFFFFFF
    local barWidth = clamp(tonumber(storage.get("barWidth")) or 4.0, 1.0, 32.0)
    local barGap = clamp(tonumber(storage.get("barGap")) or 7.5, 0.0, 20.0)
    local sensitivity = clamp(tonumber(storage.get("sensitivity")) or 0.70, 0.1, 6.0)
    local barCount = clamp(math.floor(tonumber(storage.get("bars")) or 48.0), 1, 128)
    barCount = math.min(barCount, 96)

    -- 组件宽度 = 柱条数 ×（柱条粗细 + 柱条间距）：请求宿主按内容所需宽度缩放
    local pitch = barWidth + barGap
    local padX = math.max(2.0, barWidth * 0.5)
    local contentW = barCount * pitch + padX * 2.0
    local cellW = layout.cellWidth()
    if cellW and cellW > 0 then
        local wantCols = math.max(1, math.ceil(contentW / cellW))
        wantCols = math.min(48, wantCols)
        if wantCols ~= lastRequestedCols then
            lastRequestedCols = wantCols
            widget.requestResize(wantCols, layout.rows())
        end
    end

    local active = audio.active()

    -- 配置预设：标准 / 更柔 / 更炸 / 自定义。
    -- 预设直接给出 attack/release 建议值；选"自定义"才使用下方滑块手动微调。
    local presetStandard = l10n.tr("lua_widget.audio_viz.preset_standard")
    local presetSoft = l10n.tr("lua_widget.audio_viz.preset_soft")
    local presetPunchy = l10n.tr("lua_widget.audio_viz.preset_punchy")
    local presetCustom = l10n.tr("lua_widget.audio_viz.preset_custom")
    local preset = storage.get("preset") or presetStandard

    local attack, release
    if preset == presetSoft then
        attack, release = 0.25, 0.12       -- 更柔：回落拖尾顺滑、轻轻起伏
    elseif preset == presetPunchy then
        attack, release = 0.60, 0.55       -- 更炸：快速弹起、利落归位
    elseif preset == presetCustom then
        attack = clamp(tonumber(storage.get("attack")) or 0.48, 0.01, 0.99)
        release = clamp(tonumber(storage.get("release")) or 0.40, 0.01, 0.99)
    else
        attack, release = 0.48, 0.40       -- 标准（默认，偏节奏利落）
    end

    -- 把平滑系数等体验参数实时下发到宿主（仅改设置后才会变化，捕获线程实时生效）
    audio.setSmoothing(attack, release)
    audio.setActiveThreshold(clamp(tonumber(storage.get("activeThreshold")) or 0.05, 0.0, 0.5))
    audio.setBalance(clamp(tonumber(storage.get("balance")) or 0.15, -1.0, 1.0))
    audio.setAverageSize(math.floor(clamp(tonumber(storage.get("smooth")) or 6, 1, 16)))

    local peaks = audio.peaks(barCount)

    -- 自然间距布局：每根柱占（粗细+间距）。若组件宽度不足以容纳全部柱条
    -- （如网格宽度上限），压缩间距避免溢出，不挤压重叠。
    local usableW = math.max(1, w - padX * 2)
    if contentW > usableW then
        pitch = usableW / barCount
    end
    local totalW = math.min(contentW, usableW)
    local startX = (w - totalW) * 0.5

    -- 底部留少量余量，顶部到组件顶部
    local bottomY = h * 0.92
    local maxH = math.max(1, h * 0.88)
    -- 静音时保留微小基础高度，不直接归零
    local baseH = math.max(1.5, h * 0.02)
    local radius = math.max(0.5, math.min(barWidth, pitch) * 0.3)

    for i = 1, barCount do
        local v = peaks[i] or 0.0
        local value = v * sensitivity
        if not active then value = value * 0.15 end
        value = clamp(value, 0.0, 1.0)
        local barH = baseH + value * (maxH - baseH)
        local x = startX + (i - 1) * pitch
        draw.rect(x, bottomY - barH, math.min(barWidth, pitch), barH, barColor, radius, 1.0)
    end
end
