/**
 * @file general_settings.h
 * @brief 通用设置
 * @details 定义通用设置数据结构（双击空白处隐藏桌面等），
 *          以及设置文件的读写函数。
 */

#pragma once

#include <windows.h>

#include <string>

struct GeneralSettings
{
    bool softwareDesktopEnabled = true;
    bool doubleClickHideDesktop = false;
    bool desktopPassthroughHotkeyEnabled = false;
    UINT desktopPassthroughHotkeyModifiers = MOD_CONTROL | MOD_ALT;
    UINT desktopPassthroughHotkeyVirtualKey = VK_OEM_3;
    int quickNavTheme = 1;
    bool dockEnabled = false;
    char language[85] = "system";
    // 常驻圆点分页指示器外观
    float pageDotsGap = 6.0f;          ///< 圆点中心间距 - 2*radius（像素）
    float pageDotsActiveRadius = 5.0f; ///< 当前页圆点半径（像素）
    bool pageDotsShowName = true;      ///< 悬停圆点时显示页面名
    int pageDotsColorMode = 0;         ///< 圆点颜色模式：0=跟随主题 1=白色 2=黑色 3=蓝色 4=自定义
    int pageDotsColorR = 255;          ///< 自定义颜色 R（0-255）
    int pageDotsColorG = 255;          ///< 自定义颜色 G（0-255）
    int pageDotsColorB = 255;          ///< 自定义颜色 B（0-255）
    int pageDotsVertical = 0;          ///< 垂直位置：0=底部 1=顶部
    int pageDotsHorizontal = 0;        ///< 水平位置：0=居中 1=左侧 2=右侧
    float pageDotsMargin = 32.0f;      ///< 距边距离（像素）
    bool widgetFreePosition = false;   ///< 全局自由摆放（不吸附网格）
    bool widgetHideBorder = false;     ///< 全局隐藏组件边框
};

std::wstring GetGeneralSettingsPath();
bool LoadGeneralSettings(const wchar_t* path, GeneralSettings& settings);
bool SaveGeneralSettings(const wchar_t* path, const GeneralSettings& settings);
