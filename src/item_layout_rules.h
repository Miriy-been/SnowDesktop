#pragma once

#include <algorithm>
#include <cstddef>

namespace snowdesktop::item_layout_rules
{

/**
 * @brief 判断组件是否应参与桌面网格尺寸变化后的重新安置。
 *
 * 组合内组件由其宿主负责布局；Dock 专属组件使用 Dock 伪页面保存位置，
 * 两者都不属于桌面网格，不能被当作越界组件迁回桌面。
 */
constexpr bool ShouldRelayoutDesktopWidget(
    bool grouped, bool dockExclusive)
{
    return !grouped && !dockExclusive;
}

constexpr int RoundPositive(float value)
{
    return value <= 0.0f
        ? 0
        : static_cast<int>(value + 0.5f);
}

/**
 * @brief 图标与标题之间的缩放间距。
 *
 * 四个逻辑像素能让中文标题与图标保持清晰分隔，同时在缩小的弹窗
 * 单元格中仍至少保留一个设备像素。
 */
constexpr int TitleGap(float layoutScale)
{
    return std::max(
        1, RoundPositive(4.0f * layoutScale));
}

/**
 * @brief 为两行标题保留完整行高和一个像素的抗锯齿余量。
 *
 * 第三行由 VisibleTextLengthForLineLimit 在排版阶段移除，因此这里
 * 可以安全保留中文第二行底部所需的绘制余量。
 */
constexpr int CollapsedTextHeight(float lineHeight)
{
    if (lineHeight <= 0.0f) return 1;
    const float twoLines = lineHeight * 2.0f;
    const int truncated = static_cast<int>(twoLines);
    const int roundedUp = truncated +
        (twoLines > static_cast<float>(
            truncated) ? 1 : 0);
    return std::max(
        1, roundedUp + 1);
}

/**
 * @brief 根据排版后的行度量截取前 maxLines 条视觉行。
 *
 * 最后一条可见行末尾的显式换行符必须移除，否则 DirectWrite 会再
 * 创建一个空白行。LineMetric 需提供 length 与 newlineLength 字段。
 */
template <typename LineMetric>
constexpr std::size_t VisibleTextLengthForLineLimit(
    const LineMetric* lines,
    std::size_t lineCount,
    std::size_t maxLines,
    std::size_t textLength)
{
    if (!lines || maxLines == 0)
        return 0;
    if (lineCount <= maxLines)
        return textLength;

    const std::size_t visibleLines =
        std::min(lineCount, maxLines);
    std::size_t visibleLength = 0;
    for (std::size_t i = 0;
         i < visibleLines; ++i)
    {
        visibleLength +=
            static_cast<std::size_t>(
                lines[i].length);
    }

    const std::size_t trailingNewline =
        static_cast<std::size_t>(
            lines[visibleLines - 1].
                newlineLength);
    visibleLength -= std::min(
        visibleLength, trailingNewline);
    return std::min(
        visibleLength, textLength);
}

constexpr int AvailableIconHeight(
    int cellHeight,
    int topInset,
    int titleGap,
    int textHeight)
{
    return std::max(
        1,
        cellHeight -
            std::max(0, topInset) -
            std::max(0, titleGap) -
            std::max(0, textHeight));
}

inline bool ToUpperAsciiEqualsIgnoreCase(
    std::wstring_view l,
    std::wstring_view r)
{
    if (l.size() != r.size())
        return false;
    for (size_t i = 0; i < l.size(); ++i)
    {
        auto up = [](wchar_t c) -> wchar_t {
            return (c >= L'a' && c <= L'z')
                ? static_cast<wchar_t>(
                    c - (L'a' - L'A'))
                : c;
        };
        if (up(l[i]) != up(r[i]))
            return false;
    }
    return true;
}

inline bool AsciiContainsIgnoreCase(
    std::wstring_view text,
    std::wstring_view needle)
{
    if (needle.empty())
        return true;
    if (needle.size() > text.size())
        return false;
    for (size_t i = 0;
         i + needle.size() <= text.size(); ++i)
        if (ToUpperAsciiEqualsIgnoreCase(
                text.substr(i, needle.size()),
                needle))
            return true;
    return false;
}

/**
 * @brief 判断桌面项是否为 OneDrive-Persona 系统占位符。
 *
 * 该虚拟条目（显示名如 "OneDrive-Persona"）枚举时通常不带桌面图标 CLSID，
 * 会被当作普通未放置项自动填入网格并随布局持久化，从而“重命名任意图标后
 * 跳到桌面”且无法移动/删除。通过显示名（含 onedrive 与 persona）识别，
 * 或通过 OneDrive Personal 的已知 CLSID 识别。
 */
inline bool IsOneDrivePersonaItem(
    const std::wstring& name,
    const std::wstring& desktopIconClsid)
{
    if (AsciiContainsIgnoreCase(
            name, L"Persona") &&
        AsciiContainsIgnoreCase(
            name, L"OneDrive"))
        return true;
    // OneDrive - Personal 已知 CLSID（区分大小写，系统恒为大写）
    return ToUpperAsciiEqualsIgnoreCase(
        desktopIconClsid,
        L"{018D5C66-4533-4307-9B53-224DE2ED1FE6}");
}

} // namespace snowdesktop::item_layout_rules
