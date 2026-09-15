/**
 * @file general_settings.h
 * @brief 通用设置
 * @details 定义通用设置数据结构（双击空白处隐藏桌面等），
 *          以及设置文件的读写函数。
 */

#pragma once

#include <windows.h>

#include <string>
#include "animation_settings.h"
#include "surface_theme.h"

struct GeneralSettings
{
    // Runtime projection of SnowDesktop's unified per-user logon task. The
    // scheduled task remains the source of truth; this field is deliberately
    // not serialized into SnowDesktop.general.json.
    bool autoStartEnabled = false;
    bool softwareDesktopEnabled = true;
    bool demoModeEnabled = false;
    bool doubleClickHideDesktop = false;
    bool desktopPassthroughHotkeyEnabled = false;
    UINT desktopPassthroughHotkeyModifiers = MOD_CONTROL | MOD_ALT;
    UINT desktopPassthroughHotkeyVirtualKey = VK_OEM_3;
    bool pageNavigationKeyboardEnabled = true;
    UINT pageNavigationPreviousModifiers = 0;
    UINT pageNavigationPreviousVirtualKey = VK_PRIOR;
    UINT pageNavigationNextModifiers = 0;
    UINT pageNavigationNextVirtualKey = VK_NEXT;
    int quickNavTheme = 1;
    // 0=dark, 1=light, 2=dark acrylic, 3=light acrylic.
    // Dark preserves the legacy collection-popup appearance when absent.
    int collectionPopupTheme = 0;
    snowdesktop::SurfaceTheme quickNavigationAppearance;
    snowdesktop::SurfaceTheme collectionPopupAppearance;
    bool dockEnabled = false;
    bool widgetDeveloperToolsEnabled = false;
    int animationMode = 0;
    int popupAnimationEffect = 2;
    int animationSpeed = 1;
    int animationFrameLimit = 0;
    bool animationEnergySaver = true;
    bool animationOnBattery = false;
    // 常驻圆点分页指示器外观
    float pageDotsGap = 6.0f;          ///< 圆点中心间距 - 2*radius（像素）
    float pageDotsActiveRadius = 7.0f; ///< 当前页圆点半径（像素）
    bool pageDotsShowName = true;      ///< 悬停圆点时显示页面名
    int pageDotsColorMode = 0;         ///< 圆点颜色模式：0=跟随主题 1=白色 2=黑色 3=蓝色 4=自定义
    int pageDotsColorR = 255;          ///< 自定义颜色 R（0-255）
    int pageDotsColorG = 255;          ///< 自定义颜色 G（0-255）
    int pageDotsColorB = 255;          ///< 自定义颜色 B（0-255）
    int pageDotsVertical = 0;          ///< 垂直位置：0=底部 1=顶部
    int pageDotsHorizontal = 0;        ///< 水平位置：0=居中 1=左侧 2=右侧
    float pageDotsMargin = 32.0f;      ///< 距边距离（像素）
    bool altWheelTurnPage = true;      ///< Alt + 鼠标滚轮翻页
    bool widgetFreePosition = false;   ///< 组件全局自由摆放（不吸附网格）
    bool widgetHideBorder = false;     ///< 全局隐藏组件边框
    char language[85] = "system";
};

inline void NormalizeGeneralAnimationSettings(GeneralSettings& settings) noexcept
{
    using namespace snowdesktop::animation;
    settings.animationMode = NormalizeMode(settings.animationMode);
    settings.popupAnimationEffect = NormalizePopupEffect(settings.popupAnimationEffect);
    settings.animationSpeed = NormalizeSpeed(settings.animationSpeed);
    settings.animationFrameLimit = NormalizeFrameLimit(settings.animationFrameLimit);
}

std::wstring GetGeneralSettingsPath();
bool LoadGeneralSettings(const wchar_t* path, GeneralSettings& settings);
bool SaveGeneralSettings(const wchar_t* path, const GeneralSettings& settings);
