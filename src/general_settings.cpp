/**
 * @file general_settings.cpp
 * @brief 通用设置的实现
 * @details 提供通用设置 JSON 文件的路径获取、加载、保存。
 */

#include "general_settings.h"
#include "data_paths.h"

#include <shlwapi.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>

namespace
{
    bool ReadBoolField(const std::string& text, const char* field, bool& out)
    {
        std::string marker = "\"" + std::string(field) + "\"";
        size_t p = text.find(marker);
        if (p == std::string::npos) return false;
        p = text.find(':', p);
        if (p == std::string::npos) return false;
        p = text.find_first_not_of(" \t\r\n", p + 1);
        if (p == std::string::npos) return false;
        if (text.compare(p, 4, "true") == 0) { out = true; return true; }
        if (text.compare(p, 5, "false") == 0) { out = false; return true; }
        return false;
    }

    bool ReadIntField(const std::string& text, const char* field, int& out)
    {
        std::string marker = "\"" + std::string(field) + "\"";
        size_t p = text.find(marker);
        if (p == std::string::npos) return false;
        p = text.find(':', p);
        if (p == std::string::npos) return false;
        p = text.find_first_not_of(" \t\r\n", p + 1);
        if (p == std::string::npos) return false;
        try { out = std::stoi(text.substr(p)); return true; }
        catch (...) { return false; }
    }

    bool ReadFloatField(const std::string& text, const char* field, float& out)
    {
        std::string marker = "\"" + std::string(field) + "\"";
        size_t p = text.find(marker);
        if (p == std::string::npos) return false;
        p = text.find(':', p);
        if (p == std::string::npos) return false;
        p = text.find_first_not_of(" \t\r\n", p + 1);
        if (p == std::string::npos) return false;
        try { out = std::stof(text.substr(p)); return true; }
        catch (...) { return false; }
    }

    bool ReadStringField(const std::string& text, const char* field, char* out, size_t outSize)
    {
        std::string marker = "\"" + std::string(field) + "\"";
        size_t p = text.find(marker);
        if (p == std::string::npos) return false;
        p = text.find(':', p);
        if (p == std::string::npos) return false;
        p = text.find('"', p + 1);
        if (p == std::string::npos) return false;
        size_t end = text.find('"', p + 1);
        if (end == std::string::npos) return false;
        std::string value = text.substr(p + 1, end - p - 1);
        std::strncpy(out, value.c_str(), outSize - 1);
        out[outSize - 1] = '\0';
        return true;
    }

}

std::wstring GetGeneralSettingsPath()
{
    return GetDataFilePath(L"SnowDesktop.general.json");
}

bool LoadGeneralSettings(const wchar_t* path, GeneralSettings& settings)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    std::ostringstream ss;
    ss << file.rdbuf();
    std::string text = ss.str();
    if (text.empty()) return false;

    bool val = false;
    if (ReadBoolField(text, "softwareDesktopEnabled", val))
        settings.softwareDesktopEnabled = val;
    if (ReadBoolField(text, "doubleClickHideDesktop", val))
        settings.doubleClickHideDesktop = val;
    if (ReadBoolField(text, "desktopPassthroughHotkeyEnabled", val))
        settings.desktopPassthroughHotkeyEnabled = val;
    int hotkeyValue = 0;
    if (ReadIntField(text, "desktopPassthroughHotkeyModifiers",
        hotkeyValue))
    {
        settings.desktopPassthroughHotkeyModifiers =
            static_cast<UINT>(hotkeyValue) &
            (MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN);
    }
    if (ReadIntField(text, "desktopPassthroughHotkeyVirtualKey",
        hotkeyValue) &&
        hotkeyValue > 0 && hotkeyValue <= 0xFF)
    {
        settings.desktopPassthroughHotkeyVirtualKey =
            static_cast<UINT>(hotkeyValue);
    }
    int theme = 0;
    if (ReadIntField(text, "quickNavTheme", theme))
    {
        if (theme >= 4) theme -= 2;
        settings.quickNavTheme = std::clamp(theme, 0, 3);
    }
    float floatValue = 0.0f;
    int intValue = 0;
    if (ReadFloatField(text, "pageDotsGap", floatValue))
        settings.pageDotsGap = std::clamp(floatValue, 1.0f, 30.0f);
    if (ReadFloatField(text, "pageDotsActiveRadius", floatValue))
        settings.pageDotsActiveRadius = std::clamp(floatValue, 2.0f, 16.0f);
    if (ReadBoolField(text, "pageDotsShowName", val))
        settings.pageDotsShowName = val;
    if (ReadIntField(text, "pageDotsColorMode", intValue))
        settings.pageDotsColorMode = std::clamp(intValue, 0, 4);
    if (ReadIntField(text, "pageDotsColorR", intValue))
        settings.pageDotsColorR = std::clamp(intValue, 0, 255);
    if (ReadIntField(text, "pageDotsColorG", intValue))
        settings.pageDotsColorG = std::clamp(intValue, 0, 255);
    if (ReadIntField(text, "pageDotsColorB", intValue))
        settings.pageDotsColorB = std::clamp(intValue, 0, 255);
    if (ReadIntField(text, "pageDotsVertical", intValue))
        settings.pageDotsVertical = std::clamp(intValue, 0, 1);
    if (ReadIntField(text, "pageDotsHorizontal", intValue))
        settings.pageDotsHorizontal = std::clamp(intValue, 0, 2);
    if (ReadFloatField(text, "pageDotsMargin", floatValue))
        settings.pageDotsMargin = std::clamp(floatValue, 8.0f, 100.0f);
    ReadStringField(text, "language", settings.language, sizeof(settings.language));
    return true;
}

bool SaveGeneralSettings(const wchar_t* path, const GeneralSettings& settings)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file << "{\n";
    file << "  \"softwareDesktopEnabled\": "
         << (settings.softwareDesktopEnabled ? "true" : "false") << ",\n";
    file << "  \"doubleClickHideDesktop\": " << (settings.doubleClickHideDesktop ? "true" : "false") << ",\n";
    file << "  \"desktopPassthroughHotkeyEnabled\": "
         << (settings.desktopPassthroughHotkeyEnabled ? "true" : "false")
         << ",\n";
    file << "  \"desktopPassthroughHotkeyModifiers\": "
         << settings.desktopPassthroughHotkeyModifiers << ",\n";
    file << "  \"desktopPassthroughHotkeyVirtualKey\": "
         << settings.desktopPassthroughHotkeyVirtualKey << ",\n";
    file << "  \"quickNavTheme\": " << settings.quickNavTheme << ",\n";
    file << "  \"pageDotsGap\": " << settings.pageDotsGap << ",\n";
    file << "  \"pageDotsActiveRadius\": " << settings.pageDotsActiveRadius << ",\n";
    file << "  \"pageDotsShowName\": "
         << (settings.pageDotsShowName ? "true" : "false") << ",\n";
    file << "  \"pageDotsColorMode\": " << settings.pageDotsColorMode << ",\n";
    file << "  \"pageDotsColorR\": " << settings.pageDotsColorR << ",\n";
    file << "  \"pageDotsColorG\": " << settings.pageDotsColorG << ",\n";
    file << "  \"pageDotsColorB\": " << settings.pageDotsColorB << ",\n";
    file << "  \"pageDotsVertical\": " << settings.pageDotsVertical << ",\n";
    file << "  \"pageDotsHorizontal\": " << settings.pageDotsHorizontal << ",\n";
    file << "  \"pageDotsMargin\": " << settings.pageDotsMargin << ",\n";
    file << "  \"language\": \"" << settings.language << "\"\n";
    file << "}\n";
    return true;
}
