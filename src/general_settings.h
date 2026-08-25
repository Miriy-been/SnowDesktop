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
};

std::wstring GetGeneralSettingsPath();
bool LoadGeneralSettings(const wchar_t* path, GeneralSettings& settings);
bool SaveGeneralSettings(const wchar_t* path, const GeneralSettings& settings);
