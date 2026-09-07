/**
 * @file settings_window.cpp
 * @brief SnowDesktop 设置窗口实现
 *
 * 本文件实现了 SettingsWindow 类 —— 基于 ImGui 构建的设置界面。
 * 包含以下设置页面：
 *   - 通用（General）：开机自启、快捷导航快捷键配置
 *   - 个性化（Personalization）：组件颜色、透明度、渐变等外观定制
 *   - 布局备份（Backup）：布局文件的保存、恢复与删除
 *   - 调试（Debug）：组件错误日志、组件诊断与重新加载
 *   - 关于（About）：应用信息、作者链接与开发者模式解锁
 *
 * 此外还管理窗口的 DirectX 交换链、字体加载、DPI 感知和
 *  Windows 消息处理（WndProc）。
 */

#include "settings_window.h"
#include "widget_engine.h"
#include "l10n.h"
#include "resource.h"
#include "crashlog.h"
#include "data_paths.h"
#include "deployment_context.h"
#include "full_data_backup.h"
#include "http_runtime.h"
#include "portable_data_migration.h"

#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>

#include <shlwapi.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <wbemidl.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cwctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <initializer_list>
#include <optional>
#include <sstream>
#include <utility>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

SettingsWindow* g_settingsWindow = nullptr;

namespace {
// 现代简约浅色主题的设计色板（极简瑞士风格）。
const ImVec4 kColWindowBg      = ImVec4(0.957f, 0.965f, 0.976f, 1.00f); // #F5F7F9
const ImVec4 kColCardBg        = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);   // #FFFFFF
const ImVec4 kColAccent        = ImVec4(0.145f, 0.388f, 0.922f, 1.00f); // #2563EB
const ImVec4 kColAccentHover   = ImVec4(0.114f, 0.306f, 0.847f, 1.00f); // #1D4ED8
const ImVec4 kColAccentActive  = ImVec4(0.118f, 0.251f, 0.690f, 1.00f); // #1E40AF
const ImVec4 kColAccentSoft    = ImVec4(0.918f, 0.945f, 0.996f, 1.00f); // #EAEFFE
const ImVec4 kColTextFg        = ImVec4(0.059f, 0.090f, 0.165f, 1.00f); // #0F172A
const ImVec4 kColTextMuted     = ImVec4(0.278f, 0.333f, 0.412f, 1.00f); // #475569
const ImVec4 kColTextDisabled  = ImVec4(0.580f, 0.639f, 0.702f, 1.00f); // #94A3B8
const ImVec4 kColBorder        = ImVec4(0.898f, 0.914f, 0.933f, 1.00f); // #E5E9EF
const ImVec4 kColFrameBg       = ImVec4(0.945f, 0.953f, 0.961f, 1.00f); // #F1F3F5
const ImVec4 kColFrameHover    = ImVec4(0.914f, 0.925f, 0.941f, 1.00f); // #E9ECEF
const ImVec4 kColFrameActive   = ImVec4(0.886f, 0.902f, 0.925f, 1.00f); // #E2E6EC
constexpr UINT_PTR kSettingsRefreshTimerId = 1;
constexpr UINT kSettingsRefreshIntervalMs = 500;
constexpr UINT_PTR kSettingsHotkeyCaptureTimerId = 2;
constexpr UINT kSettingsHotkeyCaptureIntervalMs = 16;
constexpr float kSettingControlWidthDip = 300.0f;
constexpr wchar_t kAutoStartRunSubKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kAutoStartRunValue[] = L"SnowDesktop";
constexpr DWORD kPortableAutoStartQueryIntervalMs = 2000;

std::string CodepointToUtf8(unsigned int codepoint)
{
    std::string result;
    if (codepoint <= 0x7F)
    {
        result.push_back(static_cast<char>(codepoint));
    }
    else if (codepoint <= 0x7FF)
    {
        result.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
        result.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
    else if (codepoint <= 0xFFFF)
    {
        result.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
        result.push_back(static_cast<char>(
            0x80 | ((codepoint >> 6) & 0x3F)));
        result.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
    else if (codepoint <= 0x10FFFF)
    {
        result.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
        result.push_back(static_cast<char>(
            0x80 | ((codepoint >> 12) & 0x3F)));
        result.push_back(static_cast<char>(
            0x80 | ((codepoint >> 6) & 0x3F)));
        result.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
    return result;
}

std::optional<std::filesystem::path> PickSettingsFile(HWND owner,
    const wchar_t* title, const COMDLG_FILTERSPEC* filters,
    UINT filterCount)
{
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        return std::nullopt;
    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options)))
        dialog->SetOptions(options | FOS_FORCEFILESYSTEM |
            FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
    dialog->SetTitle(title);
    if (filters && filterCount > 0)
        dialog->SetFileTypes(filterCount, filters);
    const HRESULT shown = dialog->Show(owner);
    if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED) || FAILED(shown))
        return std::nullopt;
    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(&item)) || !item)
        return std::nullopt;
    PWSTR selected = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &selected)) ||
        !selected)
    {
        if (selected) CoTaskMemFree(selected);
        return std::nullopt;
    }
    std::filesystem::path result(selected);
    CoTaskMemFree(selected);
    return result;
}

bool LaunchSteamWorkshopPublisher(
    const std::filesystem::path& developmentRoot)
{
    if (!WidgetEngine::IsSteamWorkshopBridgeAvailable()) return false;
    const std::filesystem::path manager =
        std::filesystem::path(GetExecutableDirectoryPath()) /
        L"SnowDesktopWorkshopManager.exe";
    std::error_code filesystemError;
    if (!std::filesystem::is_regular_file(manager, filesystemError))
        return false;
    const DWORD attributes = GetFileAttributesW(manager.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        return false;
    const std::string effectiveLanguage =
        Locale::Instance().GetEffectiveLanguage();
    const std::wstring managerLanguage(effectiveLanguage.begin(),
        effectiveLanguage.end());
    const std::wstring settingsFile = GetGeneralSettingsPath();
    std::wstring commandLine = L"\"" + manager.wstring() +
        L"\" --development-root \"" + developmentRoot.wstring() +
        L"\" --language \"" + managerLanguage +
        L"\" --settings-file \"" + settingsFile + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const std::wstring workingDirectory = manager.parent_path().wstring();
    if (!CreateProcessW(manager.c_str(), commandLine.data(), nullptr, nullptr,
        FALSE, CREATE_UNICODE_ENVIRONMENT, nullptr,
        workingDirectory.c_str(), &startup, &process))
        return false;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

bool IsSteamWorkshopPublisherAvailable()
{
    if (!WidgetEngine::IsSteamWorkshopBridgeAvailable()) return false;
    const std::filesystem::path manager =
        std::filesystem::path(GetExecutableDirectoryPath()) /
        L"SnowDesktopWorkshopManager.exe";
    std::error_code error;
    if (!std::filesystem::is_regular_file(manager, error) || error)
        return false;
    const DWORD attributes = GetFileAttributesW(manager.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

std::optional<std::filesystem::path> SaveSettingsFile(HWND owner,
    const wchar_t* title, const wchar_t* defaultName,
    const wchar_t* defaultExtension,
    const COMDLG_FILTERSPEC* filters, UINT filterCount)
{
    ComPtr<IFileSaveDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
    {
        return std::nullopt;
    }
    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options)))
    {
        dialog->SetOptions(options | FOS_FORCEFILESYSTEM |
            FOS_PATHMUSTEXIST | FOS_OVERWRITEPROMPT);
    }
    dialog->SetTitle(title);
    if (defaultName && *defaultName)
        dialog->SetFileName(defaultName);
    if (defaultExtension && *defaultExtension)
        dialog->SetDefaultExtension(defaultExtension);
    if (filters && filterCount > 0)
        dialog->SetFileTypes(filterCount, filters);
    const HRESULT shown = dialog->Show(owner);
    if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED) || FAILED(shown))
        return std::nullopt;
    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(&item)) || !item)
        return std::nullopt;
    PWSTR selected = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &selected)) ||
        !selected)
    {
        if (selected)
            CoTaskMemFree(selected);
        return std::nullopt;
    }
    std::filesystem::path result(selected);
    CoTaskMemFree(selected);
    return result;
}

std::filesystem::path FullBackupStateRoot()
{
    std::filesystem::path stateRoot =
        snowdesktop::deployment::GetPackageLocalStatePath();
    if (stateRoot.empty())
        stateRoot = GetExecutableDirectoryPath();
    return stateRoot;
}

snowdesktop::backup::FullDataBackupManager MakeFullBackupManager()
{
    return snowdesktop::backup::FullDataBackupManager(
        FullBackupStateRoot(),
        GetDataDirectoryPath(),
        SNOWDESKTOP_VERSION,
        snowdesktop::deployment::IsPackaged()
            ? "installed" : "portable");
}

std::string FormatBackupSize(std::uint64_t bytes)
{
    static constexpr const char* units[] = {
        "B", "KiB", "MiB", "GiB"
    };
    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < std::size(units))
    {
        value /= 1024.0;
        ++unit;
    }
    char buffer[64]{};
    if (unit == 0)
        std::snprintf(buffer, sizeof(buffer), "%llu %s",
            static_cast<unsigned long long>(bytes), units[unit]);
    else
        std::snprintf(buffer, sizeof(buffer), "%.1f %s",
            value, units[unit]);
    return buffer;
}

std::wstring QueryPortableAutoStartCommandViaWmi()
{
    ComPtr<IWbemLocator> locator;
    if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&locator))))
        return {};

    BSTR nameSpace = SysAllocString(L"ROOT\\CIMV2");
    if (!nameSpace)
        return {};
    ComPtr<IWbemServices> services;
    const HRESULT connectResult = locator->ConnectServer(
        nameSpace, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &services);
    SysFreeString(nameSpace);
    if (FAILED(connectResult))
        return {};

    if (FAILED(CoSetProxyBlanket(services.Get(), RPC_C_AUTHN_WINNT,
            RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
            RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE)))
        return {};

    BSTR queryLanguage = SysAllocString(L"WQL");
    BSTR query = SysAllocString(
        L"SELECT Command FROM Win32_StartupCommand "
        L"WHERE Name='SnowDesktop'");
    if (!queryLanguage || !query)
    {
        if (queryLanguage)
            SysFreeString(queryLanguage);
        if (query)
            SysFreeString(query);
        return {};
    }

    ComPtr<IEnumWbemClassObject> results;
    const HRESULT queryResult = services->ExecQuery(
        queryLanguage, query,
        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
        nullptr, &results);
    SysFreeString(queryLanguage);
    SysFreeString(query);
    if (FAILED(queryResult))
        return {};

    ComPtr<IWbemClassObject> item;
    ULONG returned = 0;
    if (FAILED(results->Next(1000, 1, &item, &returned)) ||
        returned == 0)
    {
        return {};
    }

    VARIANT command{};
    VariantInit(&command);
    const HRESULT getResult = item->Get(
        L"Command", 0, &command, nullptr, nullptr);
    std::wstring value;
    if (SUCCEEDED(getResult) && command.vt == VT_BSTR &&
        command.bstrVal != nullptr)
    {
        value = command.bstrVal;
    }
    VariantClear(&command);
    return value;
}

std::wstring ReadPortableAutoStartCommand()
{
    if (snowdesktop::deployment::IsPackaged())
    {
        static DWORD lastQueryTick = 0;
        static std::wstring cachedCommand;
        const DWORD now = GetTickCount();
        if (lastQueryTick == 0 ||
            now - lastQueryTick >= kPortableAutoStartQueryIntervalMs)
        {
            cachedCommand = QueryPortableAutoStartCommandViaWmi();
            lastQueryTick = now;
        }
        return cachedCommand;
    }

    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kAutoStartRunSubKey,
            0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return {};

    DWORD type = 0;
    DWORD size = 0;
    LONG result = RegQueryValueExW(key, kAutoStartRunValue,
        nullptr, &type, nullptr, &size);
    if (result != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ) ||
        size < sizeof(wchar_t))
    {
        RegCloseKey(key);
        return {};
    }

    std::vector<wchar_t> buffer(
        static_cast<size_t>(size / sizeof(wchar_t)) + 1, L'\0');
    result = RegQueryValueExW(key, kAutoStartRunValue,
        nullptr, &type, reinterpret_cast<BYTE*>(buffer.data()), &size);
    RegCloseKey(key);
    if (result != ERROR_SUCCESS)
        return {};

    std::wstring command(buffer.data());
    if (type != REG_EXPAND_SZ)
        return command;

    const DWORD expandedLength =
        ExpandEnvironmentStringsW(command.c_str(), nullptr, 0);
    if (expandedLength <= 1)
        return command;
    std::wstring expanded(expandedLength, L'\0');
    if (ExpandEnvironmentStringsW(
            command.c_str(), expanded.data(), expandedLength) == 0)
    {
        return command;
    }
    expanded.resize(expandedLength - 1);
    return expanded;
}

bool LooksLikeSnowDesktopDataDirectory(const std::filesystem::path& path)
{
    std::error_code error;
    if (!std::filesystem::is_directory(path, error))
        return false;

    return std::filesystem::is_regular_file(
               path / L"SnowDesktop.layout.json", error) ||
        std::filesystem::is_regular_file(
            path / L"SnowDesktop.general.json", error) ||
        std::filesystem::is_directory(path / L"widgets", error) ||
        std::filesystem::is_directory(path / L"backups", error);
}

std::wstring NormalizePathForComparison(const std::filesystem::path& path)
{
    std::error_code error;
    std::filesystem::path normalized =
        std::filesystem::weakly_canonical(path, error);
    if (error)
        normalized = path.lexically_normal();

    std::wstring result = normalized.wstring();
    std::replace(result.begin(), result.end(), L'/', L'\\');
    while (result.size() > 3 && result.back() == L'\\')
        result.pop_back();
    std::transform(result.begin(), result.end(), result.begin(),
        [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return result;
}

bool PathsOverlap(const std::filesystem::path& first,
    const std::filesystem::path& second)
{
    const std::wstring left = NormalizePathForComparison(first);
    const std::wstring right = NormalizePathForComparison(second);
    if (left == right)
        return true;
    const std::wstring leftPrefix = left + L"\\";
    const std::wstring rightPrefix = right + L"\\";
    return left.starts_with(rightPrefix) || right.starts_with(leftPrefix);
}

bool CopyDirectoryContents(const std::filesystem::path& source,
    const std::filesystem::path& destination)
{
    return snowdesktop::migration::CopyDataTree(
        source, destination).ok;
}

void DrawHelpTooltip(const char* description)
{
    if (!description || !description[0] || !ImGui::IsItemHovered())
        return;

    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
    ImGui::TextUnformatted(description);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

void DrawHelpMarker(const char* description)
{
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::TextDisabled("?");
    DrawHelpTooltip(description);
}

void DrawSettingSection(const char* label, const char* description = nullptr)
{
    if (!description || !description[0])
    {
        ImGui::SeparatorText(label);
        return;
    }

    const std::string displayLabel = std::string(label) + "  ?";
    ImGui::SeparatorText(displayLabel.c_str());
    DrawHelpTooltip(description);
}

bool DrawCollapsingHeaderWithHelp(const char* label, const char* description)
{
    const std::string displayLabel = std::string(label) + "  ?";
    const bool open = ImGui::CollapsingHeader(displayLabel.c_str());
    DrawHelpTooltip(description);
    return open;
}

float BeginSettingRow(const char* label, float controlWidth,
    const char* description = nullptr)
{
    const float rowStart = ImGui::GetCursorPosX();
    const float rowRight = rowStart + ImGui::GetContentRegionAvail().x;
    const float controlX = std::max(rowStart,
        rowRight - std::max(1.0f, controlWidth));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (description && description[0])
        DrawHelpMarker(description);
    ImGui::SameLine(controlX);
    return controlX;
}

// 返回 Win11 风格开关的宽度（随字号/DPI 缩放）。
float p10WinSwitchWidth()
{
    return ImGui::GetFrameHeight() * 1.75f;
}

/**
 * @brief 绘制 Windows 11 风格的切换开关（圆角轨道 + 圆形滑块）。
 *
 * 点击时切反 *value，返回是否发生切换。布尔返回值与 ImGui::Checkbox
 * 一致，可放入 `if (DrawWinSwitch(...))` 使用。
 */
bool DrawWinSwitch(const char* id, bool* value)
{
    const float h = ImGui::GetFrameHeight();
    const float w = p10WinSwitchWidth();
    const ImVec2 pos = ImGui::GetCursorScreenPos();

    ImGui::InvisibleButton(id, ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    const bool clicked = ImGui::IsItemClicked();
    if (clicked)
        *value = !*value;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImColor onCol = *value
        ? (active ? ImColor(37, 99, 235, 200) : ImColor(37, 99, 235, 255))
        : (active ? ImColor(0, 0, 0, 32) : ImColor(0, 0, 0, 20));
    const float radius = h * 0.5f;
    dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), onCol, radius);

    // 滑块（白色圆钮 + 细描边）
    const float m = 2.0f;
    const float kd = h - m * 2.0f;
    const float kx = *value ? pos.x + w - kd - m : pos.x + m;
    const float ky = pos.y + m;
    dl->AddRectFilled(ImVec2(kx, ky), ImVec2(kx + kd, ky + kd),
        IM_COL32(255, 255, 255, 255), kd * 0.5f);
    dl->AddRect(ImVec2(kx, ky), ImVec2(kx + kd, ky + kd),
        IM_COL32(0, 0, 0, 48), kd * 0.5f, 0, 1.0f);

    if (hovered && !*value)
    {
        // 悬停时略微加深灰色轨道
        dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h),
            IM_COL32(0, 0, 0, 14), radius);
    }

    return clicked;
}

bool DrawSettingCheckbox(const char* label, const char* id, bool* value,
    const char* description = nullptr)
{
    BeginSettingRow(label, p10WinSwitchWidth(), description);
    return DrawWinSwitch(id, value);
}

void DrawSettingValue(const char* label, const char* value)
{
    const float valueWidth = ImGui::CalcTextSize(value).x;
    BeginSettingRow(label, valueWidth);
    ImGui::TextDisabled("%s", value);
}

float SettingButtonWidth(const char* label)
{
    return ImGui::CalcTextSize(label, nullptr, true).x +
        ImGui::GetStyle().FramePadding.x * 2.0f;
}

// 依当前界面缩放返回像素值（默认 16px 基准字体随 DPI 缩放）。
float ScaledDip(float value)
{
    return value * (ImGui::GetFontSize() / 16.0f);
}

/**
 * @brief 在一张白色圆角卡片内开始绘制一组设置控件。
 *
 * 卡片顶部绘制加粗的分区标题（可含说明气泡），下方以细分割线
 * 与内容区隔开。卡片之间自动留出空隙。
 *
 * @param title       分区标题（已本地化）
 * @param description 可选的说明文字，出现时会绘制一个帮助气泡
 * @return bool 返回 false 表示后续内容应保持盒状折叠（暂未使用）
 */
void BeginSettingsCard(const char* title, const char* description = nullptr)
{
    const float cardPad = ScaledDip(20.0f);
    ImGui::Dummy(ImVec2(0.0f, ScaledDip(10.0f)));
    ImGui::PushID(title);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(cardPad, cardPad));
    ImGui::BeginChild(ImGui::GetID(title),
        ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGui::PopStyleVar();

    if (title && title[0])
    {
        ImGui::PushStyleColor(ImGuiCol_Text, kColTextFg);
        ImGui::SetWindowFontScale(1.15f);
        ImGui::TextUnformatted(title);
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopStyleColor();
        if (description && description[0])
        {
            ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
            DrawHelpMarker(description);
        }
        ImGui::Dummy(ImVec2(0.0f, ScaledDip(12.0f)));
        ImGui::PushStyleColor(ImGuiCol_Separator, kColBorder);
        ImGui::Separator();
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0.0f, ScaledDip(6.0f)));
    }
}

void EndSettingsCard()
{
    ImGui::EndChild();
    ImGui::PopID();
}

// 页面滚动容器：透明背景，四周留出外边距以呈现悬浮卡片效果。
void BeginPage()
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
        ImVec2(ScaledDip(18.0f), ScaledDip(4.0f)));
    ImGui::BeginChild("##PageScroll", ImVec2(0, 0),
        ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar();
}

void EndPage()
{
    ImGui::EndChild();
}

void CopyWideToUtf8Buffer(
    const std::wstring& text, char* buffer, size_t bufferSize);
}

/**
 * @brief 配置 ImGui 的现代化浅色主题。
 *
 * 中性的浅灰背景、纯白圆角卡片、细边框与蓝色强调色，
 * 营造简约、清爽、留白的视觉效果。
 */
static void SetupLightTheme()
{
    ImGuiStyle& s = ImGui::GetStyle();
    s.Alpha = 1.0f;
    s.WindowRounding = 0.0f;
    s.WindowBorderSize = 0.0f;
    s.ChildRounding = 12.0f;
    s.ChildBorderSize = 1.0f;
    s.FrameRounding = 6.0f;
    s.FrameBorderSize = 1.0f;
    s.PopupRounding = 10.0f;
    s.PopupBorderSize = 1.0f;
    s.ScrollbarSize = 9.0f;
    s.ScrollbarRounding = 6.0f;
    s.GrabRounding = 6.0f;
    s.GrabMinSize = 10.0f;
    s.TabRounding = 8.0f;
    s.ItemSpacing = ImVec2(10.0f, 9.0f);
    s.ItemInnerSpacing = ImVec2(8.0f, 6.0f);
    s.FramePadding = ImVec2(10.0f, 6.0f);
    s.WindowPadding = ImVec2(14.0f, 12.0f);
    s.CellPadding = ImVec2(8.0f, 6.0f);

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]             = kColWindowBg;
    c[ImGuiCol_ChildBg]              = kColCardBg;
    c[ImGuiCol_PopupBg]              = kColCardBg;
    c[ImGuiCol_Border]               = kColBorder;
    c[ImGuiCol_BorderShadow]         = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    // Win11 输入控件：白色底 + 细边框（悬停加深、按下更深的描边）
    c[ImGuiCol_FrameBg]              = kColCardBg;
    c[ImGuiCol_FrameBgHovered]       = kColCardBg;
    c[ImGuiCol_FrameBgActive]        = kColCardBg;
    c[ImGuiCol_TitleBg]              = kColWindowBg;
    c[ImGuiCol_TitleBgActive]        = kColWindowBg;
    c[ImGuiCol_TitleBgCollapsed]     = kColWindowBg;
    c[ImGuiCol_Text]                 = kColTextFg;
    c[ImGuiCol_TextDisabled]         = kColTextDisabled;
    c[ImGuiCol_TextSelectedBg]       = ImVec4(0.145f, 0.388f, 0.922f, 0.22f);
    c[ImGuiCol_InputTextCursor]      = kColTextFg;
    c[ImGuiCol_Header]               = kColAccentSoft;
    c[ImGuiCol_HeaderHovered]        = ImVec4(0.879f, 0.922f, 0.996f, 1.00f);
    c[ImGuiCol_HeaderActive]         = ImVec4(0.835f, 0.894f, 0.996f, 1.00f);
    c[ImGuiCol_Button]               = kColAccent;
    c[ImGuiCol_ButtonHovered]        = kColAccentHover;
    c[ImGuiCol_ButtonActive]         = kColAccentActive;
    c[ImGuiCol_CheckMark]            = kColAccent;
    c[ImGuiCol_SliderGrab]           = kColAccent;
    c[ImGuiCol_SliderGrabActive]     = kColAccentHover;
    c[ImGuiCol_Tab]                  = ImVec4(0.957f, 0.965f, 0.976f, 1.00f);
    c[ImGuiCol_TabHovered]           = kColAccentSoft;
    c[ImGuiCol_TabActive]            = kColCardBg;
    c[ImGuiCol_TabUnfocused]         = kColWindowBg;
    c[ImGuiCol_TabUnfocusedActive]   = kColCardBg;
    c[ImGuiCol_TableHeaderBg]        = kColFrameBg;
    c[ImGuiCol_TableBorderStrong]    = kColBorder;
    c[ImGuiCol_TableBorderLight]     = ImVec4(0.929f, 0.941f, 0.957f, 1.00f);
    c[ImGuiCol_Separator]            = kColBorder;
    c[ImGuiCol_ScrollbarBg]          = ImVec4(1.00f, 1.00f, 1.00f, 0.00f);
    c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.796f, 0.827f, 0.871f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.643f, 0.686f, 0.749f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.486f, 0.545f, 0.628f, 1.00f);
    c[ImGuiCol_ResizeGrip]           = ImVec4(0.898f, 0.914f, 0.933f, 1.00f);
    c[ImGuiCol_ResizeGripHovered]    = kColAccentSoft;
    c[ImGuiCol_ResizeGripActive]     = kColAccent;
    c[ImGuiCol_NavHighlight]         = kColAccent;
    c[ImGuiCol_NavWindowingHighlight]= kColAccent;
    c[ImGuiCol_MenuBarBg]            = ImVec4(0.957f, 0.965f, 0.976f, 1.00f);
}

/**
 * @brief 析构函数，自动调用 Shutdown() 释放资源。
 */
SettingsWindow::~SettingsWindow()
{
    Shutdown();
}

/**
 * @brief 初始化设置窗口。
 *
 * 执行以下初始化序列：
 * 1. 注册窗口类并创建 Win32 窗口（DPI 感知初始尺寸）
 * 2. 创建 DirectX 交换链和渲染目标视图
 * 3. 初始化 ImGui 上下文（Win32 + DX11 后端）
 * 4. 应用浅色主题、加载字体
 * 5. 从磁盘读取个性化与导航设置
 * 6. 将窗口居中显示在屏幕上
 *
 * @param instance  应用程序实例句柄（HINSTANCE）
 * @param device    Direct3D 11 设备指针（ComPtr 的原始指针）
 * @return true  初始化成功
 * @return false 初始化失败（窗口创建或交换链创建失败）
 */
bool SettingsWindow::Init(HINSTANCE instance, ID3D11Device* device)
{
    instance_ = instance;
    device_ = device;
    device_->GetImmediateContext(&context_);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APPICON));
    wc.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APPICON_SMALL),
        IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"SnowDesktopSettingsWindow";
    RegisterClassExW(&wc);

    // Get DPI for initial sizing
    UINT dpi = GetDpiForSystem();
    {
        HDC screenDc = GetDC(nullptr);
        if (screenDc)
        {
            dpi = GetDeviceCaps(screenDc, LOGPIXELSX);
            ReleaseDC(nullptr, screenDc);
        }
    }
    dpiScale_ = static_cast<float>(dpi) / 96.0f;
    windowWidth_ = static_cast<int>(800.0f * dpiScale_);
    windowHeight_ = static_cast<int>(560.0f * dpiScale_);

    hwnd_ = CreateWindowExW(
        WS_EX_APPWINDOW,
        wc.lpszClassName,
        _LW("app.settings.title"),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        windowWidth_, windowHeight_,
        nullptr, nullptr, instance, this);

    if (hwnd_ == nullptr) return false;
    if (wc.hIcon)
        SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(wc.hIcon));
    if (wc.hIconSm)
        SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(wc.hIconSm));

    if (!CreateSwapChain()) return false;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    SetupLightTheme();

    ImGui_ImplWin32_Init(hwnd_);
    ImGui_ImplDX11_Init(device_.Get(), context_.Get());

    SetupFonts();

    bool categorizedTabFontSizeLoaded = false;
    LoadPersonalization(
        GetPersonalizationPath().c_str(),
        personalization_,
        &categorizedTabFontSizeLoaded);
    LoadDockSettings(GetDockSettingsPath().c_str(), dockSettings_);
    LoadNavigationSettings(GetNavigationSettingsPath().c_str(), navigationSettings_);
    LoadGeneralSettings(GetGeneralSettingsPath().c_str(), generalSettings_);
    if (std::strcmp(generalSettings_.language, "system") != 0 &&
        !Locale::Instance().HasLanguage(generalSettings_.language))
    {
        std::strncpy(generalSettings_.language, "system",
            sizeof(generalSettings_.language) - 1);
        generalSettings_.language[sizeof(generalSettings_.language) - 1] = '\0';
    }
    categorySettings_ = CategorySettings::Defaults();
    LoadCategorySettings(GetCategorySettingsPath().c_str(), categorySettings_);
    if (!categorizedTabFontSizeLoaded)
    {
        // 1.0.1.0 之前该值保存在分类设置中。迁移一次后由外观设置持有，
        // 避免重置分类规则时意外重置三类组件的共同外观。
        personalization_.categorizedTabFontSize =
            std::clamp(
                categorySettings_.tabFontSize,
                10.0f, 22.0f);
        SavePersonalization(
            GetPersonalizationPath().c_str(),
            personalization_);
    }
    SyncCategoryRuleBuffersFromSettings();

    g_settingsWindow = this;

    RECT rc;
    GetWindowRect(hwnd_, &rc);
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    SetWindowPos(hwnd_, nullptr,
        (screenW - (rc.right - rc.left)) / 2,
        (screenH - (rc.bottom - rc.top)) / 2,
        0, 0, SWP_NOSIZE | SWP_NOZORDER);

    return true;
}

/**
 * @brief 关闭设置窗口并释放所有 ImGui 与 DirectX 资源。
 *
 * 清理顺序：销毁全局指针、关闭 ImGui DX11/Win32 后端、
 * 销毁 ImGui 上下文、清理交换链、销毁窗口句柄。
 */
void SettingsWindow::Shutdown()
{
    if (hwnd_ != nullptr)
    {
        KillTimer(hwnd_, kSettingsRefreshTimerId);
        KillTimer(hwnd_, kSettingsHotkeyCaptureTimerId);
    }
    hotkeyCaptureTarget_ = HotkeySettingTarget::None;

    if (personalizationDirty_)
    {
        SavePersonalization(GetPersonalizationPath().c_str(), personalization_);
        personalizationDirty_ = false;
        personalizationSaveRequested_ = false;
    }
    if (dockSettingsDirty_)
    {
        SaveDockSettings(GetDockSettingsPath().c_str(), dockSettings_);
        dockSettingsDirty_ = false;
        dockSettingsPreviewDirty_ = false;
        dockSettingsSaveRequested_ = false;
        if (dockSettingsChangedCallback_)
            dockSettingsChangedCallback_();
    }
    if (categorySettingsDirty_)
    {
        NormalizeCategoryRuleBuffers();
        SaveCategorySettings(GetCategorySettingsPath().c_str(), categorySettings_);
        categorySettingsDirty_ = false;
        categorySettingsSaveRequested_ = false;
        categorySettingsSavedTick_ = GetTickCount();
        if (categorySettingsChangedCallback_)
            categorySettingsChangedCallback_();
    }
    g_settingsWindow = nullptr;
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupSwapChain();
    if (hwnd_ != nullptr) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
    renderRequested_ = false;
}

/**
 * @brief 显示设置窗口（若尚未初始化则先初始化再显示）。
 *
 * 将窗口置于前台并设置焦点。
 */
void SettingsWindow::Show()
{
    if (snowdesktop::deployment::IsPackaged())
        packagedAutoStartStateKnown_ = false;

    if (hwnd_ == nullptr)
    {
        if (!Init(instance_, device_.Get()))
            return;
    }
    dockSettings_.systemTaskbarAutoHide = IsSystemTaskbarAutoHideEnabled();
    dockSettings_.systemTaskbarAlignment = IsSystemTaskbarAlignmentCentered() ? 1 : 0;
    ShowWindow(hwnd_, IsIconic(hwnd_) ? SW_RESTORE : SW_SHOW);
    SetTimer(hwnd_, kSettingsRefreshTimerId, kSettingsRefreshIntervalMs, nullptr);
    renderRequested_ = true;
    BringWindowToTop(hwnd_);
    SetForegroundWindow(hwnd_);
    SetFocus(hwnd_);
}

void SettingsWindow::ApplyLanguageChange()
{
    if (hwnd_ && IsWindow(hwnd_))
        SetWindowTextW(hwnd_, _LW("app.settings.title"));
    if (!updateCheckStatusKey_.empty())
    {
        updateCheckStatus_ = updateCheckStatusArgument_.empty()
            ? Locale::Instance().Tr(updateCheckStatusKey_.c_str())
            : Locale::Instance().TrFormat(
                updateCheckStatusKey_.c_str(), { updateCheckStatusArgument_ });
    }
    if (!categorySettingsDirty_)
    {
        SyncCategoryRuleBuffersFromSettings();
    }
    else
    {
        for (CategoryRuleEditBuffer& buffer : categoryRuleBuffers_)
        {
            if (!buffer.usesDefaultLabel)
                continue;
            CopyWideToUtf8Buffer(
                GetCategoryLabel(categorySettings_, buffer.id),
                buffer.label, sizeof(buffer.label));
        }
    }
    renderRequested_ = true;
}

void SettingsWindow::ShowDockSettings()
{
    activePage_ = 5;
    Show();
}

void SettingsWindow::ShowAppearanceSettings()
{
    activePage_ = 1;
    Show();
}

void SettingsWindow::ShowPageDotsEditor()
{
    editingPageDots_ = true;
    Show();
}

void SettingsWindow::ShowWidgetMigration()
{
    activePage_ = 7;
    Show();
}

/**
 * @brief 显示退出确认对话框。
 *
 * 设置 showExitConfirm_ 标记后调用 Show()，
 * 在下一帧 Render() 中弹出模态确认框。
 */
void SettingsWindow::ShowExitConfirm()
{
    showExitConfirm_ = true;
    Show();
}

/**
 * @brief 请求关闭设置窗口。
 *
 * 重置组件编辑状态并设置 pendingClose_ 标记，
 * Render() 在下一帧末尾检测到该标记时执行 Shutdown()。
 */
void SettingsWindow::RequestClose()
{
    showExitConfirm_ = false;
    if (editingWidgetIndex_ != static_cast<size_t>(-1))
        editingWidgetIndex_ = static_cast<size_t>(-1);
    if (personalizationDirty_)
        personalizationSaveRequested_ = true;
    if (dockSettingsDirty_)
        dockSettingsSaveRequested_ = true;
    if (categorySettingsDirty_)
        categorySettingsSaveRequested_ = true;
    pendingClose_ = true;
    renderRequested_ = true;
}

/**
 * @brief 主渲染函数，每帧被调用以绘制设置窗口 UI。
 *
 * 执行流程：
 * 1. 检查窗口可见性，不可见或最小化时提前返回
 * 2. 启动 ImGui 帧（DX11 + Win32 后端）
 * 3. 手动修正鼠标坐标（确保首次点击有效）
 * 4. 创建全客户区主窗口，根据编辑状态选择：
 *    - 正在编辑组件时绘制组件编辑器页面
 *    - 否则绘制左侧边栏 + 右侧活动页面
 * 5. 持久化标记为脏的个人化/导航设置
 * 6. 调用失效回调以通知外部刷新
 * 7. 处理退出确认模态弹窗
 * 8. 执行 ImGui 渲染并 Present 到交换链
 * 9. 检测 pendingClose_ 标记并执行清理
 */
void SettingsWindow::Render()
{
    if (hwnd_ == nullptr || !IsWindowVisible(hwnd_) || IsIconic(hwnd_)) return;
    if (swapChain_ == nullptr) return;
    if (renderInProgress_)
    {
        renderRequested_ = true;
        return;
    }

    renderInProgress_ = true;
    struct RenderScope final
    {
        bool& inProgress;
        ~RenderScope() { inProgress = false; }
    } renderScope{ renderInProgress_ };

    renderRequested_ = false;
    PollUpdateCheck();

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();

    // Feed current mouse position so first click works without prior WM_MOUSEMOVE
    POINT mp;
    GetCursorPos(&mp);
    ScreenToClient(hwnd_, &mp);
    ImGui::GetIO().MousePos = ImVec2((float)mp.x, (float)mp.y);

    ImGui::NewFrame();

    // Fill entire client area
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##MainFrame", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);

    if (editingPageDots_)
    {
        DrawPageDotsEditorPage();
    }
    else if (editingWidgetIndex_ != static_cast<size_t>(-1))
    {
        DrawWidgetEditorPage();
    }
    else
    {
        // Sidebar + Content layout
        const float sidebarW = 212.0f * dpiScale_;
        const float sidebarPad = 14.0f * dpiScale_;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, kColWindowBg);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sidebarPad, sidebarPad));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
        ImGui::BeginChild("##Sidebar", ImVec2(sidebarW, 0),
            ImGuiChildFlags_None,
            ImGuiWindowFlags_AlwaysVerticalScrollbar);
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor();
        DrawSidebar();
        ImGui::EndChild();

        ImGui::SameLine();
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::BeginChild("##Content", ImVec2(0, 0), ImGuiChildFlags_None,
            ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar();
        switch (activePage_)
        {
        case 0:  DrawGeneralPage(); break;
        case 1:  DrawAppearanceThemePage(); break;
        case 2:  DrawAppearanceDesktopIconsPage(); break;
        case 3:  DrawWidgetsAppearancePage(); break;
        case 4:  DrawCategorySettingsPage(); break;
        case 5:  DrawDockSettingsPage(); break;
        case 6:  DrawTaskbarPage(); break;
        case 7:  DrawWidgetPackagesPage(); break;
        case 8:  DrawBackupPage(); break;
        case 9:  DrawAboutPage(); break;
        case 10:
            if (debugUnlocked_)
                DrawDebugPage();
            else
                DrawAboutPage();
            break;
        }
        ImGui::EndChild();
    }

    ImGui::End();

    if (widgetEditorBackPending_)
    {
        widgetEditorBackPending_ = false;
        editingWidgetIndex_ = static_cast<size_t>(-1);
    }

    if (personalizationPreviewDirty_)
    {
        personalizationPreviewDirty_ = false;
        if (invalidateCallback_)
            invalidateCallback_();
    }

    if (personalizationSaveRequested_ && personalizationDirty_)
    {
        SavePersonalization(GetPersonalizationPath().c_str(), personalization_);
        personalizationDirty_ = false;
        personalizationSaveRequested_ = false;
        if (personalizationChangedCallback_)
            personalizationChangedCallback_();
    }

    if (dockSettingsPreviewDirty_)
    {
        dockSettingsPreviewDirty_ = false;
        if (dockSettingsPreviewChangedCallback_)
            dockSettingsPreviewChangedCallback_(dockSettings_);
    }

    if (dockSettingsSaveRequested_ && dockSettingsDirty_)
    {
        SaveDockSettings(GetDockSettingsPath().c_str(), dockSettings_);
        dockSettingsDirty_ = false;
        dockSettingsSaveRequested_ = false;
        if (dockSettingsChangedCallback_)
            dockSettingsChangedCallback_();
    }

    if (navigationSettingsDirty_)
    {
        SaveNavigationSettings(GetNavigationSettingsPath().c_str(), navigationSettings_);
        navigationSettingsDirty_ = false;
        if (navigationSettingsChangedCallback_)
            navigationSettingsChangedCallback_();
    }

    if (generalSettingsDirty_)
    {
        SaveGeneralSettings(GetGeneralSettingsPath().c_str(), generalSettings_);
        generalSettingsDirty_ = false;
        if (generalSettingsChangedCallback_)
            generalSettingsChangedCallback_();
    }

    if (categorySettingsSaveRequested_ && categorySettingsDirty_)
    {
        NormalizeCategoryRuleBuffers();
        SaveCategorySettings(GetCategorySettingsPath().c_str(), categorySettings_);
        categorySettingsDirty_ = false;
        categorySettingsSaveRequested_ = false;
        categorySettingsSavedTick_ = GetTickCount();
        if (categorySettingsChangedCallback_)
            categorySettingsChangedCallback_();
    }

    // Exit confirmation modal
    if (showExitConfirm_)
    {
        ImGui::OpenPopup(_L("app.settings.exit_confirm"));
        if (ImGui::BeginPopupModal(_L("app.settings.exit_confirm"), nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Text("%s", _L("app.settings.exit_confirm_text"));
            ImGui::Text("%s", _L("app.settings.exit_restore_text"));
            ImGui::Spacing();

            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
            bool okClicked = ImGui::Button(_L("app.settings.exit_ok"), ImVec2(120, 0));
            ImGui::PopStyleColor();
            if (okClicked)
            {
                showExitConfirm_ = false;
                ImGui::CloseCurrentPopup();
                if (exitCallback_) exitCallback_();
            }
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.55f, 0.60f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
            bool cancelClicked = ImGui::Button(_L("app.settings.cancel"), ImVec2(80, 0));
            ImGui::PopStyleColor(2);
            if (cancelClicked)
            {
                showExitConfirm_ = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    ImGui::Render();

    if (pendingClose_)
    {
        pendingClose_ = false;
        Shutdown();
        return;
    }

    const float clearColor[4] = {
        kColWindowBg.x, kColWindowBg.y, kColWindowBg.z, kColWindowBg.w };
    context_->OMSetRenderTargets(1, rtv_.GetAddressOf(), nullptr);
    context_->ClearRenderTargetView(rtv_.Get(), clearColor);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    swapChain_->Present(0, 0);
}

/**
 * @brief 绘制左侧导航边栏。
 *
 * 使用透明背景按钮样式，高亮当前激活页面。
 * 提供"通用"、"个性化"、"布局备份"、"关于"等入口；
 * 当 debugUnlocked_ 为 true 时额外显示"调试"入口。
 */
void SettingsWindow::DrawSidebar()
{
    const float sc = ImGui::GetFontSize() / 16.0f;
    const float navH = 42.0f * sc;
    const float itemW = ImGui::GetContentRegionAvail().x;

    // ── 品牌标题 ─────────────────────────────────────────────
    ImGui::PushStyleColor(ImGuiCol_Text, kColTextFg);
    ImGui::SetWindowFontScale(1.18f);
    ImGui::TextWrapped("SnowDesktop");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(0.0f, 4.0f * sc));
    ImGui::PushStyleColor(ImGuiCol_Separator, kColBorder);
    ImGui::Separator();
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0.0f, 12.0f * sc));

    // ── 导航按钮（indent 为子项左侧缩进，单位 sc）────────────
    // icon 为由绑定 Segoe MDL2 字体支持的 BMP 码点。
    auto BmpUtf8 = [](wchar_t c) {
        std::string s;
        unsigned cp = static_cast<unsigned>(c);
        if (cp < 0x80) s += static_cast<char>(cp);
        else if (cp < 0x800)
        {
            s += static_cast<char>(0xC0 | (cp >> 6));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        }
        else
        {
            s += static_cast<char>(0xE0 | (cp >> 12));
            s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        }
        return s;
    };
    auto SideButton = [&](int idx, wchar_t icon, const char* label, float indent) {
        const bool active = (activePage_ == idx);
        std::string text = BmpUtf8(icon) + "  " + std::string(label);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 4.0f * sc + indent);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 9.0f * sc);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12.0f * sc, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
        if (active)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, kColAccentSoft);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kColAccentSoft);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, kColAccentSoft);
            ImGui::PushStyleColor(ImGuiCol_Text, kColAccent);
        }
        else
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                ImVec4(0.878f, 0.898f, 0.925f, 0.6f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                ImVec4(0.878f, 0.898f, 0.925f, 0.9f));
            ImGui::PushStyleColor(ImGuiCol_Text, kColTextMuted);
        }
        if (ImGui::Button(text.c_str(), ImVec2(itemW - 8.0f * sc - indent, navH)))
            activePage_ = idx;
        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar(3);
    };

    // 分组标题（非可点击的小字分隔标签）
    auto SectionHeader = [&](const char* label) {
        ImGui::Dummy(ImVec2(0.0f, 10.0f * sc));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 12.0f * sc);
        ImGui::SetWindowFontScale(0.85f);
        ImGui::PushStyleColor(ImGuiCol_Text, kColTextDisabled);
        ImGui::TextUnformatted(label);
        ImGui::PopStyleColor();
        ImGui::SetWindowFontScale(1.0f);
        ImGui::Dummy(ImVec2(0.0f, 4.0f * sc));
    };

    SideButton(0, 0xE713, _L("app.settings.general"), 0.0f);

    // ── 外观（可展开分组）────────────────────────────────────
    ImGui::Dummy(ImVec2(0.0f, 2.0f * sc));
    {
        const bool active = activePage_ >= 1 && activePage_ <= 3;
        std::string header = BmpUtf8(0xE771) + "  " +
            std::string(_L("app.settings.appearance"));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 4.0f * sc);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 9.0f * sc);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12.0f * sc, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
            ImVec4(0.878f, 0.898f, 0.925f, 0.6f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
            ImVec4(0.878f, 0.898f, 0.925f, 0.9f));
        ImGui::PushStyleColor(ImGuiCol_Text, active ? kColAccent : kColTextFg);
        if (ImGui::Button(header.c_str(),
                ImVec2(itemW - 8.0f * sc - 24.0f * sc, navH)))
            appearanceGroupOpen_ = !appearanceGroupOpen_;
        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar(3);
        // 行尾 chevron 指示展开方向
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() - 28.0f * sc);
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, kColTextDisabled);
        ImGui::TextUnformatted(BmpUtf8(
            appearanceGroupOpen_ ? 0xE70D : 0xE76C).c_str());
        ImGui::PopStyleColor();
    }
    ImGui::Dummy(ImVec2(0.0f, 2.0f * sc));

    if (appearanceGroupOpen_)
    {
        SideButton(1, 0xE790, _L("app.settings.global_theme"), 14.0f * sc);
        SideButton(2, 0xE7B8, _L("app.settings.desktop_icons"), 14.0f * sc);
        SideButton(3, 0xE80A, _L("app.settings.page_dots"), 14.0f * sc);
    }

    // ── 桌面分组 ─────────────────────────────────────────────
    SectionHeader(_L("app.settings.nav_group_desktop"));
    SideButton(4, 0xE774, _L("app.settings.category"), 0.0f);
    SideButton(5, 0xEC4E, _L("app.settings.dock_bar"), 0.0f);
    SideButton(6, 0xE7F4, _L("app.settings.system_appearance"), 0.0f);
    SideButton(7, 0xEA37, _L("app.settings.widgets"), 0.0f);

    // ── 数据分组 ─────────────────────────────────────────────
    SectionHeader(_L("app.settings.nav_group_data"));
    SideButton(8, 0xE777, _L("app.settings.backup"), 0.0f);

    // ── 底部 ─────────────────────────────────────────────────
    SectionHeader("");

    SideButton(9, 0xE946, _L("app.settings.about"), 0.0f);
    if (debugUnlocked_)
        SideButton(10, 0xE705, _L("app.settings.debug"), 0.0f);
}

// ── UTF helpers ──────────────────────────────────────────────────
namespace {
/**
 * @brief 将 std::wstring 转换为 UTF-8 编码的 std::string。
 * @param w 宽字符串输入
 * @return UTF-8 编码的窄字符串，输入为空时返回空串
 */
    std::string WideToUtf8(const std::wstring& w)
    {
        if (w.empty()) return {};
        int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        std::string r(n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), r.data(), n, nullptr, nullptr);
        return r;
    }

/**
 * @brief 将 UTF-8 编码的 std::string 转换为 std::wstring。
 * @param u UTF-8 编码的窄字符串输入
 * @return 宽字符串，输入为空时返回空串
 */
    std::wstring Utf8ToWide(const std::string& u)
    {
        if (u.empty()) return {};
        int n = MultiByteToWideChar(CP_UTF8, 0, u.c_str(), static_cast<int>(u.size()), nullptr, 0);
        std::wstring r(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, u.c_str(), static_cast<int>(u.size()), r.data(), n);
        return r;
    }

    void CopyWideToUtf8Buffer(const std::wstring& text, char* buffer, size_t bufferSize)
    {
        if (!buffer || bufferSize == 0) return;
        std::string utf8 = WideToUtf8(text);
        std::strncpy(buffer, utf8.c_str(), bufferSize - 1);
        buffer[bufferSize - 1] = '\0';
    }

    std::wstring TrimWide(std::wstring value)
    {
        auto isSpace = [](wchar_t ch) { return std::iswspace(ch) != 0; };
        value.erase(value.begin(), std::find_if(value.begin(), value.end(),
            [&](wchar_t ch) { return !isSpace(ch); }));
        value.erase(std::find_if(value.rbegin(), value.rend(),
            [&](wchar_t ch) { return !isSpace(ch); }).base(), value.end());
        return value;
    }
}

/**
 * @brief 蓝色文字按钮辅助函数。
 *
 * 自动设置白色文字颜色，点击后恢复原始颜色。
 * @param label 按钮标签文本
 * @param size  按钮尺寸（可缺省，默认自适应）
 * @return true 按钮被点击
 */
static bool BlueButton(const char* label, const ImVec2& size = ImVec2(0, 0))
{
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
    bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor();
    return clicked;
}

static bool SecondaryButton(const char* label,
    const ImVec2& size = ImVec2(0, 0))
{
    const ImGuiStyle& style = ImGui::GetStyle();
    ImGui::PushStyleColor(ImGuiCol_Button,
        style.Colors[ImGuiCol_FrameBg]);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
        style.Colors[ImGuiCol_FrameBgHovered]);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,
        style.Colors[ImGuiCol_FrameBgActive]);
    const bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(3);
    return clicked;
}

namespace
{
    bool IsHotkeyPhysicalKeyDown(UINT virtualKey)
    {
        return (GetAsyncKeyState(static_cast<int>(virtualKey)) &
            0x8000) != 0;
    }

    UINT ReadHotkeyModifierMask()
    {
        UINT modifiers = 0;
        if (IsHotkeyPhysicalKeyDown(VK_CONTROL))
            modifiers |= MOD_CONTROL;
        if (IsHotkeyPhysicalKeyDown(VK_MENU))
            modifiers |= MOD_ALT;
        if (IsHotkeyPhysicalKeyDown(VK_SHIFT))
            modifiers |= MOD_SHIFT;
        if (IsHotkeyPhysicalKeyDown(VK_LWIN) ||
            IsHotkeyPhysicalKeyDown(VK_RWIN))
            modifiers |= MOD_WIN;
        return modifiers;
    }

    bool IsHotkeyModifierVirtualKey(UINT virtualKey)
    {
        switch (virtualKey)
        {
        case VK_SHIFT:
        case VK_CONTROL:
        case VK_MENU:
        case VK_LSHIFT:
        case VK_RSHIFT:
        case VK_LCONTROL:
        case VK_RCONTROL:
        case VK_LMENU:
        case VK_RMENU:
        case VK_LWIN:
        case VK_RWIN:
            return true;
        default:
            return false;
        }
    }

    UINT HotkeyModifierForVirtualKey(UINT virtualKey)
    {
        switch (virtualKey)
        {
        case VK_CONTROL:
        case VK_LCONTROL:
        case VK_RCONTROL:
            return MOD_CONTROL;
        case VK_MENU:
        case VK_LMENU:
        case VK_RMENU:
            return MOD_ALT;
        case VK_SHIFT:
        case VK_LSHIFT:
        case VK_RSHIFT:
            return MOD_SHIFT;
        case VK_LWIN:
        case VK_RWIN:
            return MOD_WIN;
        default:
            return 0;
        }
    }

    bool IsCapturableHotkeyVirtualKey(UINT virtualKey)
    {
        if (virtualKey <= VK_XBUTTON2 ||
            virtualKey == VK_ESCAPE ||
            virtualKey == VK_BACK ||
            virtualKey == VK_DELETE ||
            virtualKey == VK_PACKET)
            return false;
        return !IsHotkeyModifierVirtualKey(virtualKey);
    }

    const char* HotkeyTargetLabelKey(HotkeySettingTarget target)
    {
        switch (target)
        {
        case HotkeySettingTarget::QuickNavigation:
            return "app.settings.quick_navigation";
        case HotkeySettingTarget::DesktopPassthrough:
            return "app.settings.desktop_passthrough_hotkey";
        case HotkeySettingTarget::FloatingDock:
            return "app.settings.dock_bar";
        case HotkeySettingTarget::None:
        default:
            return "";
        }
    }
}

void SettingsWindow::StartHotkeyCapture(
    HotkeySettingTarget target)
{
    CancelHotkeyCapture();
    hotkeyCaptureTarget_ = target;
    hotkeyCaptureModifiers_ = 0;
    hotkeyCapturePressedModifiers_ = 0;
    hotkeyCaptureVirtualKey_ = 0;
    hotkeyCapturePrimarySeen_ = false;
    hotkeyCapturePrimaryDown_ = false;
    hotkeyCaptureClearPending_ = false;
    if (hwnd_ && IsWindow(hwnd_))
    {
        SetTimer(hwnd_, kSettingsHotkeyCaptureTimerId,
            kSettingsHotkeyCaptureIntervalMs, nullptr);
        SetFocus(hwnd_);
    }
    renderRequested_ = true;
}

void SettingsWindow::CancelHotkeyCapture()
{
    if (hwnd_ && IsWindow(hwnd_))
        KillTimer(hwnd_, kSettingsHotkeyCaptureTimerId);
    hotkeyCaptureTarget_ = HotkeySettingTarget::None;
    hotkeyCaptureModifiers_ = 0;
    hotkeyCapturePressedModifiers_ = 0;
    hotkeyCaptureVirtualKey_ = 0;
    hotkeyCapturePrimarySeen_ = false;
    hotkeyCapturePrimaryDown_ = false;
    hotkeyCaptureClearPending_ = false;
    renderRequested_ = true;
}

void SettingsWindow::CaptureRegisteredHotkey(
    UINT modifiers, UINT virtualKey)
{
    if (!IsHotkeyCaptureActive() || virtualKey == 0)
        return;
    const HotkeySettingTarget target =
        hotkeyCaptureTarget_;
    CancelHotkeyCapture();
    CommitHotkeyCapture(
        target,
        modifiers &
            (MOD_CONTROL | MOD_ALT |
                MOD_SHIFT | MOD_WIN),
        virtualKey);
}

void SettingsWindow::CommitHotkeyCapture(
    HotkeySettingTarget target,
    UINT modifiers,
    UINT virtualKey)
{
    modifiers &=
        MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN;
    if (virtualKey == 0)
        modifiers = 0;

    switch (target)
    {
    case HotkeySettingTarget::QuickNavigation:
        navigationSettings_.modifiers = modifiers;
        navigationSettings_.virtualKey = virtualKey;
        navigationSettingsDirty_ = true;
        break;
    case HotkeySettingTarget::DesktopPassthrough:
        generalSettings_.desktopPassthroughHotkeyModifiers =
            modifiers;
        generalSettings_.desktopPassthroughHotkeyVirtualKey =
            virtualKey;
        generalSettingsDirty_ = true;
        break;
    case HotkeySettingTarget::FloatingDock:
        dockSettings_.floatingHotkeyModifiers = modifiers;
        dockSettings_.floatingHotkeyVirtualKey = virtualKey;
        dockSettingsDirty_ = true;
        dockSettingsSaveRequested_ = true;
        break;
    case HotkeySettingTarget::None:
        return;
    }
    renderRequested_ = true;
}

void SettingsWindow::UpdateHotkeyCapture()
{
    if (!IsHotkeyCaptureActive())
        return;

    if (IsHotkeyPhysicalKeyDown(VK_ESCAPE))
    {
        CancelHotkeyCapture();
        return;
    }

    const UINT currentModifiers = ReadHotkeyModifierMask();
    hotkeyCaptureModifiers_ |= currentModifiers;

    if (!hotkeyCapturePrimarySeen_ &&
        !hotkeyCaptureClearPending_)
    {
        if (IsHotkeyPhysicalKeyDown(VK_BACK) ||
            IsHotkeyPhysicalKeyDown(VK_DELETE))
        {
            hotkeyCaptureClearPending_ = true;
        }
        else
        {
            for (UINT virtualKey = 1;
                 virtualKey <= 0xFE;
                 ++virtualKey)
            {
                if (!IsCapturableHotkeyVirtualKey(virtualKey) ||
                    !IsHotkeyPhysicalKeyDown(virtualKey))
                    continue;
                hotkeyCaptureVirtualKey_ = virtualKey;
                hotkeyCapturePrimarySeen_ = true;
                hotkeyCapturePrimaryDown_ = true;
                break;
            }
        }
    }

    if (hotkeyCaptureClearPending_)
    {
        if (!IsHotkeyPhysicalKeyDown(VK_BACK) &&
            !IsHotkeyPhysicalKeyDown(VK_DELETE) &&
            currentModifiers == 0)
        {
            const HotkeySettingTarget target =
                hotkeyCaptureTarget_;
            CancelHotkeyCapture();
            CommitHotkeyCapture(target, 0, 0);
        }
        return;
    }

    if (!hotkeyCapturePrimarySeen_)
        return;

    if (!IsHotkeyPhysicalKeyDown(hotkeyCaptureVirtualKey_) &&
        currentModifiers == 0)
    {
        const HotkeySettingTarget target =
            hotkeyCaptureTarget_;
        const UINT modifiers = hotkeyCaptureModifiers_;
        const UINT virtualKey = hotkeyCaptureVirtualKey_;
        CancelHotkeyCapture();
        CommitHotkeyCapture(target, modifiers, virtualKey);
    }
}

void SettingsWindow::HandleHotkeyCaptureKeyMessage(
    UINT message, WPARAM virtualKeyValue)
{
    if (!IsHotkeyCaptureActive())
        return;

    const UINT virtualKey =
        static_cast<UINT>(virtualKeyValue);
    const bool keyDown =
        message == WM_KEYDOWN ||
        message == WM_SYSKEYDOWN;
    const UINT modifier =
        HotkeyModifierForVirtualKey(virtualKey);

    if (keyDown)
    {
        if (virtualKey == VK_ESCAPE)
        {
            CancelHotkeyCapture();
            return;
        }
        if (modifier != 0)
        {
            hotkeyCaptureModifiers_ |= modifier;
            hotkeyCapturePressedModifiers_ |= modifier;
            return;
        }
        if (virtualKey == VK_BACK ||
            virtualKey == VK_DELETE)
        {
            hotkeyCaptureClearPending_ = true;
            return;
        }
        if (!IsCapturableHotkeyVirtualKey(virtualKey))
            return;
        if (!hotkeyCapturePrimarySeen_)
        {
            hotkeyCaptureVirtualKey_ = virtualKey;
            hotkeyCapturePrimarySeen_ = true;
        }
        if (hotkeyCaptureVirtualKey_ == virtualKey)
            hotkeyCapturePrimaryDown_ = true;
        return;
    }

    if (modifier != 0)
        hotkeyCapturePressedModifiers_ &= ~modifier;
    if (hotkeyCaptureClearPending_ &&
        (virtualKey == VK_BACK ||
            virtualKey == VK_DELETE) &&
        hotkeyCapturePressedModifiers_ == 0 &&
        ReadHotkeyModifierMask() == 0)
    {
        const HotkeySettingTarget target =
            hotkeyCaptureTarget_;
        CancelHotkeyCapture();
        CommitHotkeyCapture(target, 0, 0);
        return;
    }
    if (hotkeyCapturePrimarySeen_ &&
        virtualKey == hotkeyCaptureVirtualKey_)
        hotkeyCapturePrimaryDown_ = false;

    if (hotkeyCapturePrimarySeen_ &&
        !hotkeyCapturePrimaryDown_ &&
        hotkeyCapturePressedModifiers_ == 0 &&
        ReadHotkeyModifierMask() == 0)
    {
        const HotkeySettingTarget target =
            hotkeyCaptureTarget_;
        const UINT modifiers = hotkeyCaptureModifiers_;
        const UINT capturedVirtualKey =
            hotkeyCaptureVirtualKey_;
        CancelHotkeyCapture();
        CommitHotkeyCapture(
            target, modifiers, capturedVirtualKey);
    }
}

HotkeySettingTarget SettingsWindow::FindInternalHotkeyConflict(
    HotkeySettingTarget target,
    UINT modifiers,
    UINT virtualKey) const
{
    if (virtualKey == 0)
        return HotkeySettingTarget::None;
    const UINT normalizedModifiers =
        modifiers &
        (MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN);
    const auto matches = [&](UINT otherModifiers,
        UINT otherVirtualKey) {
        return normalizedModifiers ==
                (otherModifiers &
                    (MOD_CONTROL | MOD_ALT |
                        MOD_SHIFT | MOD_WIN)) &&
            virtualKey == otherVirtualKey;
    };

    if (target != HotkeySettingTarget::QuickNavigation &&
        navigationSettings_.enabled &&
        matches(navigationSettings_.modifiers,
            navigationSettings_.virtualKey))
        return HotkeySettingTarget::QuickNavigation;
    if (target != HotkeySettingTarget::DesktopPassthrough &&
        generalSettings_.desktopPassthroughHotkeyEnabled &&
        matches(
            generalSettings_.desktopPassthroughHotkeyModifiers,
            generalSettings_.desktopPassthroughHotkeyVirtualKey))
        return HotkeySettingTarget::DesktopPassthrough;
    if (target != HotkeySettingTarget::FloatingDock &&
        dockEnabled_ &&
        dockSettings_.floatingShortcutMode &&
        matches(dockSettings_.floatingHotkeyModifiers,
            dockSettings_.floatingHotkeyVirtualKey))
        return HotkeySettingTarget::FloatingDock;
    return HotkeySettingTarget::None;
}

void SettingsWindow::DrawHotkeyRecorder(
    HotkeySettingTarget target,
    const char* label,
    const char* id,
    bool enabled,
    UINT modifiers,
    UINT virtualKey,
    UINT defaultModifiers,
    UINT defaultVirtualKey)
{
    if (!enabled && hotkeyCaptureTarget_ == target)
        CancelHotkeyCapture();

    const bool capturing = hotkeyCaptureTarget_ == target;
    const HotkeySettingTarget internalConflict =
        enabled && !capturing
            ? FindInternalHotkeyConflict(
                target, modifiers, virtualKey)
            : HotkeySettingTarget::None;
    const bool systemAvailable =
        !enabled || capturing || virtualKey == 0 ||
        internalConflict != HotkeySettingTarget::None ||
        !hotkeyAvailabilityCallback_
            ? true
            : hotkeyAvailabilityCallback_(
                target, modifiers, virtualKey);

    NavigationSettings displaySettings;
    displaySettings.modifiers = modifiers;
    displaySettings.virtualKey = virtualKey;
    std::string displayText;
    if (capturing)
        displayText = _L("app.settings.hotkey_press");
    else if (virtualKey == 0)
        displayText = _L("app.settings.hotkey_not_set");
    else
        displayText = WideToUtf8(
            FormatNavigationHotkey(displaySettings));
    const std::string buttonLabel =
        displayText + id;
    const std::string resetLabel =
        std::string(_L("app.settings.restore_default")) +
        id + "Reset";

    std::string statusText;
    std::string statusDetails;
    ImVec4 statusColor;
    if (!enabled)
    {
        statusText = _L("app.settings.hotkey_status_disabled");
        statusColor = ImGui::GetStyle().Colors[ImGuiCol_TextDisabled];
    }
    else if (capturing)
    {
        statusText = _L("app.settings.hotkey_status_recording");
        statusDetails = _L("app.settings.hotkey_capture_active");
        statusColor = ImVec4(0.18f, 0.45f, 0.82f, 1.0f);
    }
    else if (virtualKey == 0)
    {
        statusText = _L("app.settings.hotkey_not_set");
        statusDetails = _L("app.settings.hotkey_not_set_warning");
        statusColor = ImVec4(0.82f, 0.46f, 0.08f, 1.0f);
    }
    else if (internalConflict != HotkeySettingTarget::None)
    {
        statusText = _L("app.settings.hotkey_status_conflict");
        statusDetails = _LF(
            "app.settings.hotkey_conflict_with",
            _L(HotkeyTargetLabelKey(internalConflict)));
        statusColor = ImVec4(0.80f, 0.12f, 0.12f, 1.0f);
    }
    else if (!systemAvailable)
    {
        statusText = _L("app.settings.hotkey_status_in_use");
        statusDetails = _L("app.settings.hotkey_conflict_system");
        statusColor = ImVec4(0.80f, 0.12f, 0.12f, 1.0f);
    }
    else if (modifiers == 0)
    {
        statusText = _L("app.settings.hotkey_status_no_modifier");
        statusDetails = _L("app.settings.hotkey_no_modifier_warning");
        statusColor = ImVec4(0.82f, 0.46f, 0.08f, 1.0f);
    }
    else
    {
        statusText = _L("app.settings.hotkey_status_available");
        statusDetails = _L("app.settings.hotkey_available");
        statusColor = ImVec4(0.12f, 0.58f, 0.30f, 1.0f);
    }

    const float controlWidth =
        kSettingControlWidthDip * dpiScale_;
    const float resetWidth =
        SettingButtonWidth(resetLabel.c_str());
    const float statusWidth =
        ImGui::CalcTextSize(statusText.c_str()).x;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float fieldWidth = std::max(
        96.0f * dpiScale_,
        controlWidth - resetWidth - spacing);
    const float controlX = BeginSettingRow(
        label, controlWidth,
        _L("app.settings.hotkey_capture_help"));

    ImGui::SetCursorPosX(std::max(
        0.0f, controlX - statusWidth - spacing));
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(statusColor, "%s", statusText.c_str());
    DrawHelpTooltip(statusDetails.c_str());
    ImGui::SameLine(controlX);

    ImGui::BeginDisabled(!enabled);
    if (capturing)
    {
        ImGui::PushStyleColor(ImGuiCol_Button,
            ImVec4(0.18f, 0.45f, 0.82f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
            ImVec4(0.20f, 0.49f, 0.88f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
            ImVec4(0.16f, 0.40f, 0.75f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text,
            ImVec4(1, 1, 1, 1));
    }
    else if (enabled &&
        (internalConflict != HotkeySettingTarget::None ||
            !systemAvailable))
    {
        ImGui::PushStyleColor(ImGuiCol_Button,
            ImVec4(1.0f, 0.88f, 0.88f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
            ImVec4(1.0f, 0.82f, 0.82f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
            ImVec4(0.97f, 0.76f, 0.76f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text,
            ImVec4(0.54f, 0.08f, 0.08f, 1.0f));
    }
    else
    {
        const ImGuiStyle& style = ImGui::GetStyle();
        ImGui::PushStyleColor(ImGuiCol_Button,
            style.Colors[ImGuiCol_FrameBg]);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
            style.Colors[ImGuiCol_FrameBgHovered]);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
            style.Colors[ImGuiCol_FrameBgActive]);
        ImGui::PushStyleColor(ImGuiCol_Text,
            style.Colors[ImGuiCol_Text]);
    }
    if (ImGui::Button(buttonLabel.c_str(),
            ImVec2(fieldWidth, 0)))
    {
        StartHotkeyCapture(target);
    }
    ImGui::PopStyleColor(4);
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text,
        ImVec4(1, 1, 1, 1));
    if (ImGui::Button(resetLabel.c_str(),
            ImVec2(resetWidth, 0)))
    {
        CancelHotkeyCapture();
        CommitHotkeyCapture(target,
            defaultModifiers, defaultVirtualKey);
    }
    ImGui::PopStyleColor();
    ImGui::EndDisabled();
}

/**
 * @brief 绘制"布局备份"页面。
 *
 * 提供以下功能区域：
 * - 输入备份名称并保存当前布局
 * - 列出已有备份，每项提供"恢复"与"删除"按钮
 * - 恢复操作成功后触发 reloadCallback_ 通知外部重载
 */
void SettingsWindow::DrawBackupPage()
{
    BeginPage();
    BeginSettingsCard(_L("app.settings.layout_backups"));

    const char* saveBackupLabel = _L("app.settings.save_backup");
    const char* openDataFolderLabel = _L("app.settings.open_data_folder");
    const float saveButtonW = std::max(
        84.0f * dpiScale_, SettingButtonWidth(saveBackupLabel));
    const float openDataButtonW = std::max(
        112.0f * dpiScale_, SettingButtonWidth(openDataFolderLabel));
    const float inputW = 180.0f * dpiScale_;
    const float controlW = inputW + saveButtonW + openDataButtonW +
        ImGui::GetStyle().ItemSpacing.x * 2.0f;
    BeginSettingRow(_L("app.settings.save_current_layout"), controlW);
    ImGui::SetNextItemWidth(inputW);
    ImGui::InputTextWithHint("##BackupName", _L("app.settings.backup_name_hint"), backupNameBuf_, sizeof(backupNameBuf_));

    ImGui::SameLine();
    if (BlueButton(saveBackupLabel, ImVec2(saveButtonW, 0)))
    {
        std::wstring name = Utf8ToWide(backupNameBuf_);
        if (name.empty()) name = MakeBackupTimestampName();
        if (SaveBackup(name))
        {
            backupNameBuf_[0] = '\0';
        }
    }
    ImGui::SameLine();
    if (BlueButton(openDataFolderLabel, ImVec2(openDataButtonW, 0)))
    {
        const std::wstring dataDir = GetDataDirectoryPath();
        ShellExecuteW(nullptr, L"open", dataDir.c_str(),
            nullptr, nullptr, SW_SHOW);
    }

    std::vector<LayoutBackup> backups = ListBackups();
    ImGui::Spacing();
    const float layoutBackupListHeight = backups.empty()
        ? 48.0f * dpiScale_
        : (std::min)(132.0f * dpiScale_,
            18.0f * dpiScale_ +
                static_cast<float>(backups.size()) *
                    ImGui::GetFrameHeightWithSpacing());
    ImGui::BeginChild("##BackupList",
        ImVec2(0, layoutBackupListHeight), true);
    if (backups.empty())
    {
        ImGui::TextDisabled("%s", _L("app.settings.no_backups"));
    }
    else
    {
        for (size_t i = 0; i < backups.size(); ++i)
        {
            const auto& b = backups[i];
            ImGui::PushID(static_cast<int>(i));
            const std::string label = WideToUtf8(b.displayName);
            const float actionButtonW = 56.0f * dpiScale_;
            const float actionsW = actionButtonW * 2.0f + ImGui::GetStyle().ItemSpacing.x;
            BeginSettingRow(label.c_str(), actionsW);
            if (BlueButton(_L("app.settings.restore"), ImVec2(actionButtonW, 0)))
            {
                if (RestoreBackup(b.filename) && reloadCallback_)
                    reloadCallback_();
            }
            ImGui::SameLine();
            if (BlueButton(_L("app.settings.delete"), ImVec2(actionButtonW, 0)))
            {
                DeleteBackup(b.filename);
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    ImGui::Spacing();
    EndSettingsCard();
    BeginSettingsCard(_L("app.settings.full_data_backups"));
    ImGui::TextWrapped("%s",
        _L("app.settings.full_data_backup_description"));
    ImGui::Spacing();

    const char* createFullLabel =
        _L("app.settings.create_full_backup");
    const char* importFullLabel =
        _L("app.settings.restore_from_backup_file");
    const char* openFullLabel =
        _L("app.settings.open_full_backup_folder");
    if (BlueButton(createFullLabel,
            ImVec2(SettingButtonWidth(createFullLabel), 0)))
    {
        CreateFullDataBackup();
    }
    ImGui::SameLine();
    if (BlueButton(importFullLabel,
            ImVec2(SettingButtonWidth(importFullLabel), 0)))
    {
        ImportFullDataBackup();
    }
    ImGui::SameLine();
    if (BlueButton(openFullLabel,
            ImVec2(SettingButtonWidth(openFullLabel), 0)))
    {
        auto manager = MakeFullBackupManager();
        std::error_code error;
        std::filesystem::create_directories(
            manager.BackupRoot(), error);
        ShellExecuteW(nullptr, L"open",
            manager.BackupRoot().c_str(), nullptr, nullptr, SW_SHOW);
    }

    const auto fullBackups = MakeFullBackupManager().List();
    ImGui::Spacing();
    const float fullBackupListHeight = fullBackups.empty()
        ? 48.0f * dpiScale_
        : (std::min)(172.0f * dpiScale_,
            18.0f * dpiScale_ +
                static_cast<float>(fullBackups.size()) *
                    ImGui::GetFrameHeightWithSpacing());
    ImGui::BeginChild("##FullBackupList",
        ImVec2(0, fullBackupListHeight), true);
    if (fullBackups.empty())
    {
        ImGui::TextDisabled("%s",
            _L("app.settings.no_full_data_backups"));
    }
    else
    {
        for (std::size_t index = 0;
            index < fullBackups.size(); ++index)
        {
            const auto& backup = fullBackups[index];
            ImGui::PushID(
                static_cast<int>(index) + 10000);
            const std::string timestamp = backup.createdAt.empty()
                ? _L("app.settings.full_backup_unknown_time")
                : backup.createdAt;
            const std::string label = backup.migrationRollback
                ? _LF("app.settings.migration_backup_item", timestamp)
                : _LF("app.settings.full_backup_item",
                    timestamp,
                    std::to_string(backup.fileCount),
                    FormatBackupSize(backup.totalBytes));
            const char* restoreLabel =
                _L("app.settings.restore");
            const char* exportLabel =
                _L("app.settings.export_backup");
            const char* openLabel =
                _L("app.settings.open");
            const char* deleteLabel =
                _L("app.settings.delete");
            const float restoreW =
                SettingButtonWidth(restoreLabel);
            const float exportW =
                SettingButtonWidth(exportLabel);
            const float openW =
                SettingButtonWidth(openLabel);
            const float deleteW =
                SettingButtonWidth(deleteLabel);
            const float actionsW =
                restoreW + exportW + openW + deleteW +
                ImGui::GetStyle().ItemSpacing.x * 3.0f;
            BeginSettingRow(label.c_str(), actionsW);
            if (BlueButton(restoreLabel, ImVec2(restoreW, 0)))
                RestoreFullDataBackup(backup);
            ImGui::SameLine();
            if (BlueButton(exportLabel, ImVec2(exportW, 0)))
                ExportFullDataBackup(backup);
            ImGui::SameLine();
            if (BlueButton(openLabel, ImVec2(openW, 0)))
            {
                ShellExecuteW(nullptr, L"open",
                    backup.root.c_str(), nullptr, nullptr, SW_SHOW);
            }
            ImGui::SameLine();
            if (BlueButton(deleteLabel, ImVec2(deleteW, 0)))
                DeleteFullDataBackup(backup);
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    ImGui::Spacing();
    EndSettingsCard();
    BeginSettingsCard(_L("app.settings.data_migration"));
    ImGui::TextWrapped("%s",
        _L("app.settings.data_migration_description"));
    ImGui::Spacing();
    if (BlueButton(_L("app.settings.migrate_all_data")))
        MigrateAllData();

    EndSettingsCard();
    EndPage();
}

/**
 * @brief 绘制"通用设置"页面。
 *
 * 提供以下配置项：
 * - 开机自启开关（通过 Windows 注册表 Run 键实现）
 * - 全局快捷导航开关、修饰键（Ctrl/Alt/Shift/Win）和主键组合选择
 * - 修改后的导航设置自动持久化并触发回调
 */
void SettingsWindow::DrawGeneralPage()
{
    BeginPage();

    BeginSettingsCard(_L("app.settings.general_settings"));

    bool autoStart = IsAutoStartEnabled();
    if (DrawSettingCheckbox(_L("app.settings.auto_start"), "##AutoStart", &autoStart))
        SetAutoStart(autoStart);

    if (snowdesktop::deployment::IsPackaged())
    {
        const std::wstring otherAutoStart =
            ReadPortableAutoStartCommand();
        if (!otherAutoStart.empty())
        {
            ImGui::Spacing();
            const std::string warning = _LF(
                "app.settings.auto_start_other_version",
                WideToUtf8(otherAutoStart));
            ImGui::PushStyleColor(
                ImGuiCol_Text, ImVec4(0.95f, 0.58f, 0.16f, 1.0f));
            ImGui::TextWrapped("%s", warning.c_str());
            ImGui::PopStyleColor();
            if (BlueButton(
                    _L("app.settings.auto_start_open_windows_settings")))
            {
                ShellExecuteW(nullptr, L"open",
                    L"ms-settings:startupapps",
                    nullptr, nullptr, SW_SHOW);
            }
            ImGui::Spacing();
        }
    }
    else
    {
        const auto installedState =
            snowdesktop::deployment::GetInstalledPackagedAutoStartState();
        if (snowdesktop::deployment::IsPackagedAutoStartStateEnabled(
                installedState))
        {
            ImGui::Spacing();
            ImGui::PushStyleColor(
                ImGuiCol_Text, ImVec4(0.95f, 0.58f, 0.16f, 1.0f));
            ImGui::TextWrapped("%s",
                _L("app.settings.auto_start_installed_version_active"));
            ImGui::PopStyleColor();
            if (BlueButton(
                    _L("app.settings.auto_start_open_windows_settings")))
            {
                ShellExecuteW(nullptr, L"open",
                    L"ms-settings:startupapps",
                    nullptr, nullptr, SW_SHOW);
            }
            ImGui::Spacing();
        }
    }

    if (DrawSettingCheckbox(_L("app.settings.software_desktop"), "##SoftwareDesktopEnabled",
        &generalSettings_.softwareDesktopEnabled))
        generalSettingsDirty_ = true;

    ImGui::Spacing();

    {
        const float controlW = kSettingControlWidthDip * dpiScale_;
        BeginSettingRow(_L("app.settings.language"), controlW);
        std::vector<std::string> langNames{ "system" };
        std::vector<std::string> langLabels{ _L("app.settings.language_system") };
        for (const LanguageInfo& language :
            Locale::Instance().GetAvailableLanguages())
        {
            langNames.push_back(language.code);
            std::string label = language.displayName;
            const std::string localizedName =
                Locale::Instance().GetLocalizedLanguageName(language.code);
            if (!localizedName.empty() &&
                localizedName != language.displayName)
            {
                label += " (" + localizedName + ")";
            }
            langLabels.push_back(std::move(label));
        }
        std::vector<const char*> langLabelPointers;
        langLabelPointers.reserve(langLabels.size());
        for (const std::string& label : langLabels)
            langLabelPointers.push_back(label.c_str());
        int langIdx = 0;
        for (size_t index = 0; index < langNames.size(); ++index)
            if (langNames[index] == generalSettings_.language)
                langIdx = static_cast<int>(index);
        ImGui::SetNextItemWidth(controlW);
        if (ImGui::Combo("##Language", &langIdx, langLabelPointers.data(),
            static_cast<int>(langLabelPointers.size())))
        {
            std::strncpy(generalSettings_.language,
                langNames[static_cast<size_t>(langIdx)].c_str(),
                sizeof(generalSettings_.language) - 1);
            generalSettings_.language[sizeof(generalSettings_.language) - 1] = '\0';
            Locale::Instance().SetLanguage(generalSettings_.language);
            generalSettingsDirty_ = true;
            if (languageChangedCallback_)
                languageChangedCallback_();
            renderRequested_ = true;
        }
    }

    ImGui::Spacing();
    EndSettingsCard();
    BeginSettingsCard(_L("app.settings.quick_navigation"));

    if (DrawSettingCheckbox(_L("app.settings.enable_global_navigation"), "##NavigationEnabled",
        &navigationSettings_.enabled))
        navigationSettingsDirty_ = true;

    DrawHotkeyRecorder(
        HotkeySettingTarget::QuickNavigation,
        _L("app.settings.hotkey"),
        "##QuickNavigationHotkeyRecorder",
        navigationSettings_.enabled,
        navigationSettings_.modifiers,
        navigationSettings_.virtualKey,
        MOD_CONTROL | MOD_ALT,
        VK_SPACE);

    ImGui::Spacing();
    EndSettingsCard();
    BeginSettingsCard(_L("app.settings.desktop_interact"));

    if (DrawSettingCheckbox(
            _L("app.settings.desktop_passthrough_hotkey"),
            "##DesktopPassthroughHotkeyEnabled",
            &generalSettings_.desktopPassthroughHotkeyEnabled))
        generalSettingsDirty_ = true;
    if (generalSettings_.desktopPassthroughHotkeyEnabled)
    {
        ImGui::Indent();
        ImGui::TextDisabled(
            "%s",
            _L("app.settings.desktop_passthrough_hotkey_hint"));
        ImGui::Unindent();
    }

    ImGui::Indent();
    DrawHotkeyRecorder(
        HotkeySettingTarget::DesktopPassthrough,
        _L("app.settings.hotkey"),
        "##DesktopPassthroughHotkeyRecorder",
        generalSettings_.desktopPassthroughHotkeyEnabled,
        generalSettings_.desktopPassthroughHotkeyModifiers,
        generalSettings_.desktopPassthroughHotkeyVirtualKey,
        MOD_CONTROL | MOD_ALT,
        VK_OEM_3);
    ImGui::Unindent();
    ImGui::Spacing();

    if (DrawSettingCheckbox(_L("app.settings.double_click_hide"), "##DoubleClickHideDesktop",
        &generalSettings_.doubleClickHideDesktop))
        generalSettingsDirty_ = true;

    if (DrawSettingCheckbox(_L("app.settings.hide_onedrive_persona"), "##HideOneDrivePersona",
        &generalSettings_.hideOneDrivePersona))
    {
        generalSettingsDirty_ = true;
        // 过滤项变化：立即用新值更新应用侧并重新枚举桌面图标。
        if (iconVisibilityChangedCallback_)
            iconVisibilityChangedCallback_(
                generalSettings_.hideOneDrivePersona);
    }

    EndSettingsCard();
    EndPage();
}

/**
 * @brief 绘制分页指示器（圆点 + 胶囊）的全部属性控件。
 *        供通用页与「组件设置」编辑器页共用，保证单点维护。
 */
void SettingsWindow::DrawPageDotsControls()
{
    const float dotsControlW = kSettingControlWidthDip * dpiScale_;

    // ── 胶囊「外观」：跟随全局 / 全局主题 / 自定义（与天气小组件一致）──
    if (DrawSettingCheckbox(_L("engine.editor.follow_global"),
            "##PageDotsCapsuleFollowGlobal",
            &generalSettings_.pageDotsCapsuleFollowGlobal))
        generalSettingsDirty_ = true;

    ImGui::BeginDisabled(generalSettings_.pageDotsCapsuleFollowGlobal);
    {
        BeginSettingRow(_L("app.settings.theme"), dotsControlW);
        constexpr const char* kThemeNameKeys[] = {
            "app.settings.dark", "app.settings.light",
            "app.settings.dark_glass", "app.settings.light_glass",
            "app.settings.dark_acrylic", "app.settings.light_acrylic",
        };
        std::vector<std::string> themeLabels;
        themeLabels.reserve(IM_ARRAYSIZE(kThemeNameKeys) + 1);
        for (const char* key : kThemeNameKeys)
            themeLabels.push_back(
                _LF("engine.editor.global_theme_format", _L(key)));
        themeLabels.push_back(_L("app.settings.custom"));
        std::vector<const char*> themePtrs;
        for (const std::string& label : themeLabels)
            themePtrs.push_back(label.c_str());
        int theme = std::clamp(generalSettings_.pageDotsCapsuleTheme, 0, 6);
        ImGui::SetNextItemWidth(dotsControlW);
        if (ImGui::Combo("##PageDotsCapsuleTheme", &theme,
                themePtrs.data(), static_cast<int>(themePtrs.size())))
        {
            generalSettings_.pageDotsCapsuleTheme = theme;
            generalSettingsDirty_ = true;
        }
    }
    ImGui::EndDisabled();

    if (!generalSettings_.pageDotsCapsuleFollowGlobal &&
        generalSettings_.pageDotsCapsuleTheme == 6)
    {
        ImGui::Indent();
        float bgColor[3] = {
            generalSettings_.pageDotsCapsuleColorR / 255.0f,
            generalSettings_.pageDotsCapsuleColorG / 255.0f,
            generalSettings_.pageDotsCapsuleColorB / 255.0f };
        BeginSettingRow(_L("app.settings.bg_color"),
            ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x);
        if (ImGui::ColorEdit3("##PageDotsCapsuleBg", bgColor,
                ImGuiColorEditFlags_NoInputs))
        {
            generalSettings_.pageDotsCapsuleColorR =
                static_cast<int>(lroundf(bgColor[0] * 255.0f));
            generalSettings_.pageDotsCapsuleColorG =
                static_cast<int>(lroundf(bgColor[1] * 255.0f));
            generalSettings_.pageDotsCapsuleColorB =
                static_cast<int>(lroundf(bgColor[2] * 255.0f));
            generalSettingsDirty_ = true;
        }
        BeginSettingRow(_L("app.settings.bg_opacity"), dotsControlW);
        ImGui::SetNextItemWidth(dotsControlW);
        if (ImGui::SliderFloat("##PageDotsCapsuleAlpha",
                &generalSettings_.pageDotsCapsuleAlpha, 0.0f, 1.0f, "%.2f"))
            generalSettingsDirty_ = true;
        float borderColor[3] = {
            generalSettings_.pageDotsCapsuleBorderR / 255.0f,
            generalSettings_.pageDotsCapsuleBorderG / 255.0f,
            generalSettings_.pageDotsCapsuleBorderB / 255.0f };
        BeginSettingRow(_L("app.settings.border_color"),
            ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x);
        if (ImGui::ColorEdit3("##PageDotsCapsuleBorder", borderColor,
                ImGuiColorEditFlags_NoInputs))
        {
            generalSettings_.pageDotsCapsuleBorderR =
                static_cast<int>(lroundf(borderColor[0] * 255.0f));
            generalSettings_.pageDotsCapsuleBorderG =
                static_cast<int>(lroundf(borderColor[1] * 255.0f));
            generalSettings_.pageDotsCapsuleBorderB =
                static_cast<int>(lroundf(borderColor[2] * 255.0f));
            generalSettingsDirty_ = true;
        }
        BeginSettingRow(_L("app.settings.border_opacity"), dotsControlW);
        ImGui::SetNextItemWidth(dotsControlW);
        if (ImGui::SliderFloat("##PageDotsCapsuleBorderAlpha",
                &generalSettings_.pageDotsCapsuleBorderAlpha, 0.0f, 1.0f, "%.2f"))
            generalSettingsDirty_ = true;
        ImGui::Unindent();
        ImGui::Spacing();
    }

    {
        BeginSettingRow(_L("app.settings.page_dots_gap"), dotsControlW);
        ImGui::SetNextItemWidth(dotsControlW);
        if (ImGui::SliderFloat("##PageDotsGap", &generalSettings_.pageDotsGap,
                1.0f, 30.0f, "%.1f"))
            generalSettingsDirty_ = true;
    }
    {
        BeginSettingRow(_L("app.settings.page_dots_size"), dotsControlW);
        ImGui::SetNextItemWidth(dotsControlW);
        if (ImGui::SliderFloat("##PageDotsSize", &generalSettings_.pageDotsActiveRadius,
                2.0f, 16.0f, "%.1f"))
            generalSettingsDirty_ = true;
    }
    if (DrawSettingCheckbox(_L("app.settings.page_dots_show_name"), "##PageDotsShowName",
            &generalSettings_.pageDotsShowName))
        generalSettingsDirty_ = true;
    if (DrawSettingCheckbox(_L("app.settings.wheel_turn_page"), "##AltWheelTurnPage",
            &generalSettings_.altWheelTurnPage))
        generalSettingsDirty_ = true;
    if (generalSettings_.altWheelTurnPage)
    {
        ImGui::Indent();
        ImGui::TextDisabled("%s", _L("app.settings.wheel_turn_page_hint"));
        ImGui::Unindent();
    }
    {
        BeginSettingRow(_L("app.settings.page_dots_color"), dotsControlW);
        const char* colorNames[] = {
            _L("app.settings.page_dots_color_theme"),
            _L("app.settings.page_dots_color_white"),
            _L("app.settings.page_dots_color_black"),
            _L("app.settings.page_dots_color_blue"),
            _L("app.settings.page_dots_color_custom")
        };
        int colorMode = std::clamp(generalSettings_.pageDotsColorMode, 0, 4);
        ImGui::SetNextItemWidth(dotsControlW);
        if (ImGui::Combo("##PageDotsColorMode", &colorMode,
                colorNames, IM_ARRAYSIZE(colorNames)))
        {
            generalSettings_.pageDotsColorMode = colorMode;
            generalSettingsDirty_ = true;
        }
    }
    if (generalSettings_.pageDotsColorMode == 4)
    {
        ImGui::SetNextItemWidth(dotsControlW);
        if (ImGui::SliderInt("##PageDotsColorR",
                &generalSettings_.pageDotsColorR, 0, 255, "R %d"))
            generalSettingsDirty_ = true;
        ImGui::SetNextItemWidth(dotsControlW);
        if (ImGui::SliderInt("##PageDotsColorG",
                &generalSettings_.pageDotsColorG, 0, 255, "G %d"))
            generalSettingsDirty_ = true;
        ImGui::SetNextItemWidth(dotsControlW);
        if (ImGui::SliderInt("##PageDotsColorB",
                &generalSettings_.pageDotsColorB, 0, 255, "B %d"))
            generalSettingsDirty_ = true;
    }
    {
        BeginSettingRow(_L("app.settings.page_dots_vertical"), dotsControlW);
        const char* verticalNames[] = {
            _L("app.settings.page_dots_bottom"),
            _L("app.settings.page_dots_top")
        };
        int vertical = std::clamp(generalSettings_.pageDotsVertical, 0, 1);
        ImGui::SetNextItemWidth(dotsControlW);
        if (ImGui::Combo("##PageDotsVertical", &vertical,
                verticalNames, IM_ARRAYSIZE(verticalNames)))
        {
            generalSettings_.pageDotsVertical = vertical;
            generalSettingsDirty_ = true;
        }
    }
    {
        BeginSettingRow(_L("app.settings.page_dots_horizontal"), dotsControlW);
        const char* horizontalNames[] = {
            _L("app.settings.page_dots_center"),
            _L("app.settings.page_dots_left"),
            _L("app.settings.page_dots_right")
        };
        int horizontal = std::clamp(generalSettings_.pageDotsHorizontal, 0, 2);
        ImGui::SetNextItemWidth(dotsControlW);
        if (ImGui::Combo("##PageDotsHorizontal", &horizontal,
                horizontalNames, IM_ARRAYSIZE(horizontalNames)))
        {
            generalSettings_.pageDotsHorizontal = horizontal;
            generalSettingsDirty_ = true;
        }
    }
    {
        BeginSettingRow(_L("app.settings.page_dots_margin"), dotsControlW);
        ImGui::SetNextItemWidth(dotsControlW);
        if (ImGui::SliderFloat("##PageDotsMargin",
                &generalSettings_.pageDotsMargin, 8.0f, 100.0f, "%.0f px"))
            generalSettingsDirty_ = true;
    }
}

/**
 * @brief 绘制「小组件外观」页（分页指示器的外观与胶囊设置）。
 */
void SettingsWindow::DrawWidgetsAppearancePage()
{
    BeginPage();
    BeginSettingsCard(_L("app.settings.page_dots"));
    DrawPageDotsControls();
    EndSettingsCard();
    EndPage();
}

/**
 * @brief 绘制分页指示器「组件设置」编辑器页。
 *
 * 与小组件编辑器页一致：顶部工具栏含返回按钮与标题，下方为可滚动
 * 的分页指示器属性控件（圆点/胶囊颜色、尺寸、位置等）。
 */
void SettingsWindow::DrawPageDotsEditorPage()
{
    const float pad = 14.0f * dpiScale_;
    const float toolbarH = 48.0f * dpiScale_;
    const ImVec2 toolbarPos = ImGui::GetCursorScreenPos();
    const float toolbarW = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    drawList->AddRectFilled(toolbarPos,
        ImVec2(toolbarPos.x + toolbarW, toolbarPos.y + toolbarH),
        IM_COL32(248, 248, 250, 255), 8.0f * dpiScale_);
    drawList->AddLine(ImVec2(toolbarPos.x, toolbarPos.y + toolbarH),
        ImVec2(toolbarPos.x + toolbarW, toolbarPos.y + toolbarH),
        IM_COL32(210, 210, 218, 255), 1.0f);

    ImGui::SetCursorScreenPos(ImVec2(toolbarPos.x + pad, toolbarPos.y + 8.0f * dpiScale_));
    const ImVec2 backSize(116.0f * dpiScale_, 32.0f * dpiScale_);
    const ImVec2 backPos = ImGui::GetCursorScreenPos();
    bool backClicked = ImGui::InvisibleButton("##PageDotsEditorBack", backSize);
    bool backHovered = ImGui::IsItemHovered();
    drawList->AddRectFilled(backPos,
        ImVec2(backPos.x + backSize.x, backPos.y + backSize.y),
        backHovered ? IM_COL32(226, 234, 246, 255) : IM_COL32(238, 242, 248, 255),
        16.0f * dpiScale_);
    drawList->AddRect(backPos,
        ImVec2(backPos.x + backSize.x, backPos.y + backSize.y),
        backHovered ? IM_COL32(110, 145, 190, 255) : IM_COL32(198, 208, 222, 255),
        16.0f * dpiScale_, 0, 1.0f);
    drawList->AddText(ImVec2(backPos.x + 14.0f * dpiScale_, backPos.y + 7.0f * dpiScale_),
        IM_COL32(42, 52, 68, 255), "<");
    drawList->AddText(ImVec2(backPos.x + 34.0f * dpiScale_, backPos.y + 7.0f * dpiScale_),
        IM_COL32(42, 52, 68, 255), _L("app.settings.widget_editor_back"));

    const char* title = _L("app.settings.page_dots");
    drawList->AddText(ImVec2(backPos.x + backSize.x + 18.0f * dpiScale_,
            toolbarPos.y + 15.0f * dpiScale_),
        IM_COL32(36, 39, 46, 255), title);

    ImGui::SetCursorScreenPos(ImVec2(toolbarPos.x, toolbarPos.y + toolbarH + 10.0f * dpiScale_));

    if (backClicked)
        editingPageDots_ = false;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
    ImGui::BeginChild("##PageDotsEditorScroll", ImVec2(0, 0),
        ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
        ImGuiWindowFlags_AlwaysVerticalScrollbar);
    ImGui::PopStyleVar();
    DrawPageDotsControls();
    ImGui::EndChild();
}

/**
 * @brief 绘制通用页中的 Dock 设置区域。
 */
void SettingsWindow::DrawDockPage()
{
    const float controlW = kSettingControlWidthDip * dpiScale_;
    const std::string thicknessResetLabel =
        std::string(_L("app.settings.restore_default")) + "##DockThicknessDefault";
    const float resetW = SettingButtonWidth(thicknessResetLabel.c_str());
    const float sliderActionW = controlW;
    const float actionSliderW = std::max(1.0f,
        sliderActionW - ImGui::GetStyle().ItemSpacing.x - resetW);
    auto markChanged = [&](bool saveImmediately) {
        dockSettingsDirty_ = true;
        dockSettingsPreviewDirty_ = true;
        if (saveImmediately)
            dockSettingsSaveRequested_ = true;
    };

    if (DrawSettingCheckbox(_L("app.dock.enable"), "##DockEnabled", &dockEnabled_))
    {
        if (dockEnabledChangedCallback_)
            dockEnabledChangedCallback_(dockEnabled_);
    }

    ImGui::BeginDisabled(!dockEnabled_);
    ImGui::Spacing();
    if (DrawSettingCheckbox(_L("app.dock.floating_shortcut_mode"),
        "##DockFloatingShortcutMode",
        &dockSettings_.floatingShortcutMode))
        markChanged(true);
    if (dockSettings_.floatingShortcutMode)
    {
        ImGui::Indent();
        ImGui::TextDisabled("%s", _L("app.dock.floating_shortcut_hint"));
        ImGui::Unindent();
    }

    ImGui::Spacing();
    ImGui::Indent();
    DrawHotkeyRecorder(
        HotkeySettingTarget::FloatingDock,
        _L("app.settings.hotkey"),
        "##FloatingDockHotkeyRecorder",
        dockEnabled_ &&
            dockSettings_.floatingShortcutMode,
        dockSettings_.floatingHotkeyModifiers,
        dockSettings_.floatingHotkeyVirtualKey,
        MOD_CONTROL | MOD_ALT,
        'D');
    ImGui::Unindent();

    ImGui::Spacing();
    if (DrawSettingCheckbox(
            _L("app.dock.floating_edge_swipe"),
            "##DockFloatingEdgeSwipe",
            &dockSettings_.floatingEdgeSwipeEnabled))
        markChanged(true);
    if (dockSettings_.floatingEdgeSwipeEnabled)
    {
        ImGui::TextDisabled(
            "%s",
            _L("app.dock.floating_edge_swipe_hint"));
    }

    ImGui::Spacing();
    BeginSettingRow(_L("app.settings.dock_position"), controlW);
    const char* positionNames[] = { _L("app.dock.bottom"), _L("app.dock.top"), _L("app.dock.left"), _L("app.dock.right") };
    int position = std::clamp(static_cast<int>(dockSettings_.position), 0, 3);
    ImGui::SetNextItemWidth(controlW);
    if (ImGui::Combo("##DockPosition", &position, positionNames, IM_ARRAYSIZE(positionNames)))
    {
        dockSettings_.position = static_cast<DockPosition>(position);
        markChanged(true);
    }

    BeginSettingRow(_L("app.settings.display_scope"), controlW);
    const char* monitorScopeNames[] = { _L("app.dock.first_screen"), _L("app.dock.last_screen"), _L("app.dock.all_screens") };
    int monitorScope = std::clamp(
        static_cast<int>(dockSettings_.monitorScope), 0, 2);
    ImGui::SetNextItemWidth(controlW);
    if (ImGui::Combo("##DockMonitorScope", &monitorScope,
        monitorScopeNames, IM_ARRAYSIZE(monitorScopeNames)))
    {
        dockSettings_.monitorScope = static_cast<DockMonitorScope>(monitorScope);
        markChanged(true);
    }
    BeginSettingRow(_L("app.dock.layout"), controlW);
    const char* layoutNames[] = { _L("app.dock.island"), _L("app.dock.edge") };
    int layoutMode = dockSettings_.edgeAttached ? 1 : 0;
    ImGui::SetNextItemWidth(controlW);
    if (ImGui::Combo("##DockLayoutMode", &layoutMode, layoutNames, IM_ARRAYSIZE(layoutNames)))
    {
        dockSettings_.edgeAttached = layoutMode == 1;
        markChanged(true);
    }
    BeginSettingRow(_L("app.settings.dock_thickness"), sliderActionW,
        _L("app.settings.dock_thickness_hint"));
    ImGui::SetNextItemWidth(actionSliderW);
    int thicknessPercent = static_cast<int>(std::round(
        dockSettings_.thicknessScale * 100.0f));
    if (ImGui::SliderInt("##DockThickness", &thicknessPercent, 50, 100, "%d%%"))
    {
        dockSettings_.thicknessScale = thicknessPercent / 100.0f;
        markChanged(false);
    }
    if (ImGui::IsItemDeactivatedAfterEdit() &&
        dockSettingsDirty_)
        dockSettingsSaveRequested_ = true;
    ImGui::SameLine();
    if (BlueButton(thicknessResetLabel.c_str(), ImVec2(resetW, 0)))
    {
        dockSettings_.thicknessScale = 1.0f;
        markChanged(true);
    }

    if (DrawSettingCheckbox(_L("app.dock.show_windows_button"), "##DockShowWindowsButton",
        &dockSettings_.showWindowsButton))
        markChanged(true);

    if (DrawSettingCheckbox(_L("app.dock.show_frequent_items"), "##DockShowFrequentItems",
        &dockSettings_.showFrequentItems))
        markChanged(true);

    ImGui::BeginDisabled(!dockSettings_.showFrequentItems);
    BeginSettingRow(_L("app.settings.show_count"), controlW);
    ImGui::SetNextItemWidth(controlW);
    if (ImGui::SliderInt("##DockFrequentItemCount",
        &dockSettings_.frequentItemCount, 1, 8, _L("app.settings.items_unit")))
        markChanged(false);
    if (ImGui::IsItemDeactivatedAfterEdit() &&
        dockSettingsDirty_)
        dockSettingsSaveRequested_ = true;
    ImGui::EndDisabled();

    ImGui::EndDisabled();

}

/**
 * @brief 绘制外观页中的 Windows 系统任务栏设置区域。
 */
void SettingsWindow::DrawSystemTaskbarPage()
{
    const float controlW = kSettingControlWidthDip * dpiScale_;
    auto markChanged = [&]() {
        dockSettingsDirty_ = true;
        dockSettingsPreviewDirty_ = true;
        dockSettingsSaveRequested_ = true;
    };

    if (DrawSettingCheckbox(_L("app.settings.auto_hide_taskbar"), "##SystemTaskbarAutoHide",
        &dockSettings_.systemTaskbarAutoHide))
        markChanged();

    BeginSettingRow(_L("app.settings.taskbar_alignment"), controlW,
        _L("app.settings.taskbar_alignment_hint"));
    const char* alignmentNames[] = { _L("app.settings.taskbar_left"), _L("app.settings.taskbar_center") };
    int alignment = std::clamp(dockSettings_.systemTaskbarAlignment, 0, 1);
    ImGui::SetNextItemWidth(controlW);
    if (ImGui::Combo("##SystemTaskbarAlignment", &alignment,
        alignmentNames, IM_ARRAYSIZE(alignmentNames)))
    {
        dockSettings_.systemTaskbarAlignment = alignment;
        markChanged();
    }

    const std::string restartExplorerLabel =
        std::string(_L("app.settings.restart_explorer")) + "##WindowsTheme";
    const float restartExplorerButtonW =
        SettingButtonWidth(restartExplorerLabel.c_str());
    const float windowsThemeComboW = std::max(1.0f,
        controlW - ImGui::GetStyle().ItemSpacing.x - restartExplorerButtonW);
    BeginSettingRow(_L("app.settings.system_panel"), controlW,
        (std::string(_L("app.settings.system_panel_hint")) + " "
         + _L("app.settings.system_panel_hint2")).c_str());
    const char* windowsThemeNames[] = {
        _L("app.settings.light"), _L("app.settings.dark")
    };
    int windowsTheme = IsWindowsSystemLightThemeEnabled() ? 0 : 1;
    ImGui::SetNextItemWidth(windowsThemeComboW);
    if (ImGui::Combo("##WindowsSystemTheme", &windowsTheme,
        windowsThemeNames, IM_ARRAYSIZE(windowsThemeNames)))
    {
        SetWindowsSystemLightThemeEnabled(windowsTheme == 0);
        dockSettingsDirty_ = true;
        dockSettingsPreviewDirty_ = true;
        dockSettingsSaveRequested_ = true;
    }
    ImGui::SameLine();
    if (BlueButton(restartExplorerLabel.c_str()))
    {
        if (!RestartWindowsExplorer())
            MessageBoxW(hwnd_, _LW("app.interact.restart_explorer_fail"),
                L"SnowDesktop", MB_OK | MB_ICONWARNING);
    }
}

/**
 * @brief 绘制「悬浮拓展坞」页。
 */
void SettingsWindow::DrawDockSettingsPage()
{
    BeginPage();
    BeginSettingsCard(_L("app.settings.dock_bar"));
    DrawDockPage();
    EndSettingsCard();
    EndPage();
}

/**
 * @brief 绘制外观页中的图标显示设置区域。
 */
void SettingsWindow::DrawDisplayPage()
{
    const float controlW = kSettingControlWidthDip * dpiScale_;
    const float sliderW = controlW;
    const float resetW = 84.0f * dpiScale_;
    const float sliderActionW = controlW;
    const float actionSliderW = std::max(1.0f,
        controlW - ImGui::GetStyle().ItemSpacing.x - resetW);

    auto markChanged = [&]() {
        if (displaySettingsChangedCallback_)
            displaySettingsChangedCallback_();
    };

    auto applyIconBeautifyPreset = [&](int preset) {
        iconBeautifyBgPreset_ = preset;
        switch (preset)
        {
        case 2:
            iconBeautifyBgOpacity_ = 0.50f;
            iconBeautifyGradientEnabled_ = false;
            iconBeautifyGradientDirection_ = 0;
            iconBeautifyBgStartR_ = 255.0f / 255.0f;
            iconBeautifyBgStartG_ = 255.0f / 255.0f;
            iconBeautifyBgStartB_ = 255.0f / 255.0f;
            iconBeautifyBgEndR_ = iconBeautifyBgStartR_;
            iconBeautifyBgEndG_ = iconBeautifyBgStartG_;
            iconBeautifyBgEndB_ = iconBeautifyBgStartB_;
            break;
        case 3:
            iconBeautifyBgOpacity_ = 0.82f;
            iconBeautifyGradientEnabled_ = true;
            iconBeautifyGradientDirection_ = 2;
            iconBeautifyBgStartR_ = 156.0f / 255.0f;
            iconBeautifyBgStartG_ = 216.0f / 255.0f;
            iconBeautifyBgStartB_ = 255.0f / 255.0f;
            iconBeautifyBgEndR_ = 74.0f / 255.0f;
            iconBeautifyBgEndG_ = 128.0f / 255.0f;
            iconBeautifyBgEndB_ = 255.0f / 255.0f;
            break;
        case 4:
            iconBeautifyBgOpacity_ = 0.78f;
            iconBeautifyGradientEnabled_ = true;
            iconBeautifyGradientDirection_ = 3;
            iconBeautifyBgStartR_ = 255.0f / 255.0f;
            iconBeautifyBgStartG_ = 218.0f / 255.0f;
            iconBeautifyBgStartB_ = 138.0f / 255.0f;
            iconBeautifyBgEndR_ = 255.0f / 255.0f;
            iconBeautifyBgEndG_ = 122.0f / 255.0f;
            iconBeautifyBgEndB_ = 164.0f / 255.0f;
            break;
        case 5:
            iconBeautifyBgOpacity_ = 0.70f;
            iconBeautifyGradientEnabled_ = true;
            iconBeautifyGradientDirection_ = 1;
            iconBeautifyBgStartR_ = 24.0f / 255.0f;
            iconBeautifyBgStartG_ = 32.0f / 255.0f;
            iconBeautifyBgStartB_ = 48.0f / 255.0f;
            iconBeautifyBgEndR_ = 87.0f / 255.0f;
            iconBeautifyBgEndG_ = 105.0f / 255.0f;
            iconBeautifyBgEndB_ = 135.0f / 255.0f;
            break;
        default:
            iconBeautifyBgPreset_ = 1;
            iconBeautifyBgOpacity_ = 0.65f;
            iconBeautifyGradientEnabled_ = false;
            iconBeautifyGradientDirection_ = 0;
            iconBeautifyBgStartR_ = 232.0f / 255.0f;
            iconBeautifyBgStartG_ = 236.0f / 255.0f;
            iconBeautifyBgStartB_ = 244.0f / 255.0f;
            iconBeautifyBgEndR_ = 222.0f / 255.0f;
            iconBeautifyBgEndG_ = 228.0f / 255.0f;
            iconBeautifyBgEndB_ = 240.0f / 255.0f;
            break;
        }
    };

    BeginSettingRow(_L("app.settings.icon_spacing"), sliderActionW);
    ImGui::SetNextItemWidth(actionSliderW);
    if (ImGui::SliderInt("##IconSpacing", &displaySpacingPct_, 1, 200, "%d%%", ImGuiSliderFlags_None))
    {
        iconSpacingScale_ = displaySpacingPct_ / 100.0f;
        markChanged();
        const float componentSpacingMaximum =
            componentSpacingMaximumProvider_
                ? componentSpacingMaximumProvider_()
                : snowdesktop::widget_spacing_rules::kMaximumComponentScale;
        componentSpacingScale_ = snowdesktop::widget_spacing_rules::
            ClampComponentScale(componentSpacingScale_, componentSpacingMaximum);
        componentSpacingPct_ = static_cast<int>(std::round(
            componentSpacingScale_ * 100.0f));
    }
    ImGui::SameLine();
    if (BlueButton((std::string(_L("app.settings.restore_default")) +
        "##IconSpacingDefault").c_str(), ImVec2(resetW, 0)))
    {
        displaySpacingPct_ = 100;
        iconSpacingScale_ = 1.0f;
        markChanged();
        const float componentSpacingMaximum =
            componentSpacingMaximumProvider_
                ? componentSpacingMaximumProvider_()
                : snowdesktop::widget_spacing_rules::kMaximumComponentScale;
        componentSpacingScale_ = snowdesktop::widget_spacing_rules::
            ClampComponentScale(componentSpacingScale_, componentSpacingMaximum);
        componentSpacingPct_ = static_cast<int>(std::round(
            componentSpacingScale_ * 100.0f));
    }

    BeginSettingRow(_L("app.settings.title_font_size"), sliderActionW);
    ImGui::SetNextItemWidth(actionSliderW);
    if (ImGui::SliderFloat("##ItemFontSize", &itemFontSize_,
        10.0f, 24.0f, "%.1f pt"))
        markChanged();
    ImGui::SameLine();
    if (BlueButton((std::string(_L("app.settings.restore_default")) +
        "##ItemFontSizeDefault").c_str(), ImVec2(resetW, 0)))
    {
        itemFontSize_ = 12.0f;
        markChanged();
    }

    BeginSettingRow(_L("app.settings.title_font_weight"), sliderActionW);
    ImGui::SetNextItemWidth(actionSliderW);
    if (ImGui::SliderFloat("##ItemFontWeight", &itemFontWeight_,
        100.0f, 900.0f, "%.0f"))
        markChanged();
    ImGui::SameLine();
    if (BlueButton((std::string(_L("app.settings.restore_default")) +
        "##ItemFontWeightDefault").c_str(), ImVec2(resetW, 0)))
    {
        itemFontWeight_ = 400.0f;
        markChanged();
    }

    ImGui::Spacing();
    const char* shortcutArrowModeNames[] = {
        _L("app.settings.arrow_default"),
        _L("app.settings.arrow_hide_all"),
        _L("app.settings.arrow_show_all"),
    };
    BeginSettingRow(_L("app.settings.shortcut_arrow"), controlW);
    ImGui::SetNextItemWidth(controlW);
    if (ImGui::Combo("##ShortcutArrowMode", &shortcutArrowMode_,
        shortcutArrowModeNames, static_cast<int>(sizeof(shortcutArrowModeNames) / sizeof(shortcutArrowModeNames[0]))))
    {
        markChanged();
    }

    ImGui::Spacing();
    if (DrawSettingCheckbox(_L("app.settings.icon_beautify"), "##IconBeautifyEnabled",
        &iconBeautifyEnabled_))
        markChanged();

    ImGui::BeginDisabled(!iconBeautifyEnabled_);
    const char* beautifyModeNames[] = {
        _L("app.settings.beautify_smart"),
        _L("app.settings.beautify_shrink_bg"),
    };
    BeginSettingRow(_L("app.settings.beautify_mode"), controlW);
    ImGui::SetNextItemWidth(controlW);
    if (ImGui::Combo("##IconBeautifyMode", &iconBeautifyMode_,
        beautifyModeNames, static_cast<int>(sizeof(beautifyModeNames) / sizeof(beautifyModeNames[0]))))
    {
        markChanged();
    }

    const char* presetNames[] = {
        _L("app.settings.beautify_preset_default_gray"),
        _L("app.settings.beautify_preset_white_glow"),
        _L("app.settings.beautify_preset_blue_gradient"),
        _L("app.settings.beautify_preset_warm_gradient"),
        _L("app.settings.beautify_preset_dark_glass"),
        _L("app.settings.custom"),
    };
    constexpr int presetValues[] = { 1, 2, 3, 4, 5, 0 };
    int presetSelection = 0;
    for (int i = 0; i < IM_ARRAYSIZE(presetValues); ++i)
    {
        if (presetValues[i] == iconBeautifyBgPreset_)
        {
            presetSelection = i;
            break;
        }
    }
    BeginSettingRow(_L("app.settings.beautify_bg_preset"), controlW);
    ImGui::SetNextItemWidth(controlW);
    if (ImGui::Combo("##IconBeautifyBgPreset", &presetSelection,
        presetNames, static_cast<int>(sizeof(presetNames) / sizeof(presetNames[0]))))
    {
        iconBeautifyBgPreset_ = presetValues[presetSelection];
        if (iconBeautifyBgPreset_ > 0)
            applyIconBeautifyPreset(iconBeautifyBgPreset_);
        markChanged();
    }

    if (iconBeautifyBgPreset_ == 0)
    {
    BeginSettingRow(_L("app.settings.default_bg"), ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x);
    float bgStart[3] = { iconBeautifyBgStartR_, iconBeautifyBgStartG_, iconBeautifyBgStartB_ };
    if (ImGui::ColorEdit3("##IconBeautifyBgStart", bgStart, ImGuiColorEditFlags_NoInputs))
    {
        iconBeautifyBgPreset_ = 0;
        iconBeautifyBgStartR_ = bgStart[0];
        iconBeautifyBgStartG_ = bgStart[1];
        iconBeautifyBgStartB_ = bgStart[2];
        markChanged();
    }

    BeginSettingRow(_L("app.settings.bg_opacity_val"), sliderW);
    ImGui::SetNextItemWidth(sliderW);
    int bgOpacityPercent = static_cast<int>(std::round(iconBeautifyBgOpacity_ * 100.0f));
    if (ImGui::SliderInt("##IconBeautifyBgOpacity", &bgOpacityPercent, 0, 100, "%d%%"))
    {
        iconBeautifyBgOpacity_ = bgOpacityPercent / 100.0f;
        iconBeautifyBgPreset_ = 0;
        markChanged();
    }

    if (DrawSettingCheckbox(_L("app.settings.enable_gradient_bg"), "##IconBeautifyGradient",
        &iconBeautifyGradientEnabled_))
    {
        iconBeautifyBgPreset_ = 0;
        markChanged();
    }

    BeginSettingRow(_L("app.settings.gradient_end_color"), ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x);
    ImGui::BeginDisabled(!iconBeautifyGradientEnabled_);
    float bgEnd[3] = { iconBeautifyBgEndR_, iconBeautifyBgEndG_, iconBeautifyBgEndB_ };
    if (ImGui::ColorEdit3("##IconBeautifyBgEnd", bgEnd, ImGuiColorEditFlags_NoInputs))
    {
        iconBeautifyBgPreset_ = 0;
        iconBeautifyBgEndR_ = bgEnd[0];
        iconBeautifyBgEndG_ = bgEnd[1];
        iconBeautifyBgEndB_ = bgEnd[2];
        markChanged();
    }

    const char* directionNames[] = {
        _L("app.settings.beautify_gradient_updown"),
        _L("app.settings.beautify_gradient_leftright"),
        _L("app.settings.beautify_gradient_topleft_bottomright"),
        _L("app.settings.beautify_gradient_bottomleft_topright"),
    };
    BeginSettingRow(_L("app.settings.beautify_gradient_dir"), controlW);
    ImGui::SetNextItemWidth(controlW);
    if (ImGui::Combo("##IconBeautifyGradientDirection", &iconBeautifyGradientDirection_,
        directionNames, static_cast<int>(sizeof(directionNames) / sizeof(directionNames[0]))))
    {
        iconBeautifyBgPreset_ = 0;
        markChanged();
    }
    ImGui::EndDisabled();
    }
    ImGui::EndDisabled();
}

void SettingsWindow::SyncCategoryRuleBuffersFromSettings()
{
    categoryRuleBuffers_.clear();
    categoryRuleBuffers_.reserve(categorySettings_.rules.size());
    for (const CategoryRule& rule : categorySettings_.rules)
    {
        CategoryRuleEditBuffer buffer;
        buffer.id = rule.id;
        buffer.usesDefaultLabel =
            rule.customLabel.empty() && IsBuiltinCategoryRuleId(rule.id);
        CopyWideToUtf8Buffer(GetCategoryLabel(categorySettings_, rule.id),
            buffer.label, sizeof(buffer.label));
        CopyWideToUtf8Buffer(rule.extensions, buffer.extensions, sizeof(buffer.extensions));
        categoryRuleBuffers_.push_back(std::move(buffer));
    }
}

void SettingsWindow::NormalizeCategoryRuleBuffers()
{
    categorySettings_.rules.clear();
    categorySettings_.rules.reserve(categoryRuleBuffers_.size());
    for (const CategoryRuleEditBuffer& buffer : categoryRuleBuffers_)
    {
        CategoryRule rule;
        rule.id = buffer.id;
        const std::wstring editedLabel = TrimWide(Utf8ToWide(buffer.label));
        if (!buffer.usesDefaultLabel)
            rule.customLabel = editedLabel;
        if (rule.customLabel.empty() && !IsBuiltinCategoryRuleId(rule.id))
            rule.customLabel = _LW("widget.categories.unnamed");
        rule.extensions = NormalizeCategoryExtensionText(Utf8ToWide(buffer.extensions));
        categorySettings_.rules.push_back(std::move(rule));
    }
    SyncCategoryRuleBuffersFromSettings();
}

/**
 * @brief 绘制"分类设置"页面。
 */
void SettingsWindow::DrawCategorySettingsPage()
{
    BeginPage();

    const float inputW = kSettingControlWidthDip * dpiScale_;

    auto markChanged = [&]() {
        categorySettingsDirty_ = true;
        categorySettingsSavedTick_ = 0;
    };

    const float subsectionContentIndent = 16.0f * dpiScale_;
    auto drawSubsectionLabel = [](const char* label, const char* description) {
        ImGui::TextUnformatted(label);
        if (description && description[0])
            DrawHelpMarker(description);
    };

    BeginSettingsCard(_L("app.settings.category_settings"));

    drawSubsectionLabel(_L("app.settings.category_type"),
        _L("app.settings.category_hint"));
    ImGui::Indent(subsectionContentIndent);
    ImGui::Spacing();

    int deleteIndex = -1;
    for (size_t i = 0; i < categoryRuleBuffers_.size(); ++i)
    {
        CategoryRuleEditBuffer& buffer = categoryRuleBuffers_[i];
        ImGui::PushID(static_cast<int>(i));

        const float actionWidth = 56.0f * dpiScale_;
        const float nameInputW = std::max(1.0f,
            inputW - actionWidth - ImGui::GetStyle().ItemSpacing.x);
        BeginSettingRow(_L("app.settings.category_name"), inputW);
        ImGui::SetNextItemWidth(nameInputW);
        if (ImGui::InputText("##CategoryLabel", buffer.label, sizeof(buffer.label)))
        {
            buffer.usesDefaultLabel = false;
            markChanged();
        }

        ImGui::SameLine();
        if (BlueButton(_L("app.settings.delete"), ImVec2(56.0f * dpiScale_, 0)))
            deleteIndex = static_cast<int>(i);

        BeginSettingRow(_L("app.settings.category_extensions"), inputW);
        ImGui::SetNextItemWidth(inputW);
        if (ImGui::InputText("##CategoryExtensions", buffer.extensions, sizeof(buffer.extensions)))
            markChanged();

        ImGui::Spacing();
        ImGui::PopID();
    }

    if (deleteIndex >= 0 && static_cast<size_t>(deleteIndex) < categoryRuleBuffers_.size())
    {
        categoryRuleBuffers_.erase(categoryRuleBuffers_.begin() + deleteIndex);
        markChanged();
    }

    ImGui::Unindent(subsectionContentIndent);
    ImGui::Spacing();
    drawSubsectionLabel(_L("app.settings.add_category"), nullptr);
    ImGui::Indent(subsectionContentIndent);
    ImGui::Spacing();

    const float actionWidth = 56.0f * dpiScale_;
    const float nameInputW = std::max(1.0f,
        inputW - actionWidth - ImGui::GetStyle().ItemSpacing.x);
    BeginSettingRow(_L("app.settings.category_name"), inputW);
    ImGui::SetNextItemWidth(nameInputW);
    ImGui::InputText("##NewCategoryLabel", newCategoryLabelBuf_, sizeof(newCategoryLabelBuf_));

    ImGui::SameLine();
    if (BlueButton(_L("app.settings.add"), ImVec2(56.0f * dpiScale_, 0)))
    {
        CategoryRuleEditBuffer buffer;
        std::wstring id = L"custom-" + std::to_wstring(GetTickCount64());
        bool unique = false;
        int suffix = 2;
        while (!unique)
        {
            unique = true;
            for (const auto& existing : categoryRuleBuffers_)
            {
                if (existing.id == id)
                {
                    unique = false;
                    id = L"custom-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(suffix++);
                    break;
                }
            }
        }
        buffer.id = id;

        std::wstring label = TrimWide(Utf8ToWide(newCategoryLabelBuf_));
        if (label.empty())
            label = _LW("app.settings.new_category");
        std::wstring extensions = NormalizeCategoryExtensionText(Utf8ToWide(newCategoryExtensionsBuf_));
        CopyWideToUtf8Buffer(label, buffer.label, sizeof(buffer.label));
        CopyWideToUtf8Buffer(extensions, buffer.extensions, sizeof(buffer.extensions));
        categoryRuleBuffers_.push_back(std::move(buffer));
        newCategoryLabelBuf_[0] = '\0';
        newCategoryExtensionsBuf_[0] = '\0';
        markChanged();
    }

    BeginSettingRow(_L("app.settings.category_extensions"), inputW);
    ImGui::SetNextItemWidth(inputW);
    ImGui::InputText("##NewCategoryExtensions", newCategoryExtensionsBuf_, sizeof(newCategoryExtensionsBuf_));

    ImGui::Unindent(subsectionContentIndent);
    ImGui::Spacing();
    drawSubsectionLabel(_L("app.settings.save_settings"), nullptr);
    ImGui::Indent(subsectionContentIndent);
    ImGui::Spacing();

    const float applyButtonW = 80.0f * dpiScale_;
    const float restoreButtonW = 96.0f * dpiScale_;
    const float saveActionsW = applyButtonW + ImGui::GetStyle().ItemSpacing.x + restoreButtonW;
    BeginSettingRow(_L("app.settings.category_rules"), saveActionsW);
    if (BlueButton(_L("app.settings.apply"), ImVec2(applyButtonW, 0)))
    {
        categorySettingsDirty_ = true;
        categorySettingsSaveRequested_ = true;
    }
    ImGui::SameLine();
    if (BlueButton(_L("app.settings.restore_default"), ImVec2(restoreButtonW, 0)))
    {
        categorySettings_ = CategorySettings::Defaults();
        SyncCategoryRuleBuffersFromSettings();
        markChanged();
        categorySettingsSaveRequested_ = true;
    }

    if (categorySettingsDirty_)
    {
        DrawSettingValue(_L("app.settings.save_status"), _L("app.settings.save_unsaved"));
    }
    else if (categorySettingsSavedTick_ != 0 && GetTickCount() - categorySettingsSavedTick_ < 2500)
    {
        DrawSettingValue(_L("app.settings.save_status"), _L("app.settings.saved"));
    }

    ImGui::Unindent(subsectionContentIndent);
    EndSettingsCard();
    EndPage();
}

/**
 * @brief 绘制统一外观页面。
 *
 * 提供以下定制能力：
 * - 六种全局主题快速切换与自定义参数调整
 * - Dock 固定继承、快捷搜索自定义主题与系统任务栏覆盖
 * - 修改立即通知桌面预览；连续拖动结束后再持久化
 */
void SettingsWindow::DrawAppearanceThemePage()
{
    BeginPage();

    auto markChanged = [&](bool saveImmediately) {
        personalizationDirty_ = true;
        personalizationPreviewDirty_ = true;
        if (saveImmediately)
            personalizationSaveRequested_ = true;
    };

    const float controlW = kSettingControlWidthDip * dpiScale_;
    const float sliderW = controlW;
    const float resetW = 84.0f * dpiScale_;
    const float sliderActionW = controlW;
    const float actionSliderW = std::max(1.0f,
        controlW - ImGui::GetStyle().ItemSpacing.x - resetW);
    BeginSettingsCard(_L("app.settings.global_theme"));

    auto presetForId = [](int id) { return MakeAppearancePreset(id); };

    const char* presetNames[] = {
        _L("app.settings.dark"),
        _L("app.settings.light"),
        _L("app.settings.dark_glass"),
        _L("app.settings.light_glass"),
        _L("app.settings.dark_acrylic"),
        _L("app.settings.light_acrylic"),
        _L("app.settings.custom")
    };
    constexpr int presetIds[] = {
        kAppearancePresetDark, kAppearancePresetLight,
        kAppearancePresetGlassDark, kAppearancePresetGlassLight,
        kAppearancePresetAcrylicDark, kAppearancePresetAcrylicLight,
        kAppearancePresetCustom
    };
    int presetIndex = 0;
    for (int i = 0; i < static_cast<int>(sizeof(presetIds) / sizeof(presetIds[0])); ++i)
    {
        if (presetIds[i] == personalization_.backgroundPreset)
        {
            presetIndex = i;
            break;
        }
    }
    BeginSettingRow(_L("app.settings.theme"), controlW);
    ImGui::SetNextItemWidth(controlW);
    if (ImGui::Combo("##WidgetBackgroundPreset", &presetIndex,
        presetNames, static_cast<int>(sizeof(presetNames) / sizeof(presetNames[0]))))
    {
        const int previousPreset = personalization_.backgroundPreset;
        const float cornerRadius = personalization_.cornerRadius;
        const float barHeight = personalization_.barHeight;
        const float categorizedTabFontSize =
            personalization_.categorizedTabFontSize;
        const int contextMenuStyle =
            personalization_.contextMenuStyle;
        if (presetIds[presetIndex] == kAppearancePresetCustom)
        {
            switch (NormalizeAppearancePresetId(previousPreset))
            {
            case kAppearancePresetLight: generalSettings_.quickNavTheme = 1; break;
            case kAppearancePresetAcrylicDark: generalSettings_.quickNavTheme = 2; break;
            case kAppearancePresetAcrylicLight: generalSettings_.quickNavTheme = 3; break;
            default: generalSettings_.quickNavTheme = 0; break;
            }
            generalSettingsDirty_ = true;
            personalization_.backgroundPreset = kAppearancePresetCustom;
        }
        else
        {
            personalization_ = presetForId(presetIds[presetIndex]);
        }
        personalization_.cornerRadius = cornerRadius;
        personalization_.barHeight = barHeight;
        personalization_.categorizedTabFontSize =
            categorizedTabFontSize;
        personalization_.contextMenuStyle = contextMenuStyle;
        markChanged(true);
    }

    if (personalization_.backgroundPreset == kAppearancePresetCustom)
    {
    ImGui::Spacing();
    ImGui::Indent(8.0f * dpiScale_);

    const char* quickNavThemeNames[] = {
        _L("app.settings.dark"), _L("app.settings.light"), _L("app.settings.dark_acrylic"), _L("app.settings.light_acrylic")
    };
    BeginSettingRow(_L("app.settings.quick_nav_theme"), controlW);
    ImGui::SetNextItemWidth(controlW);
    if (ImGui::Combo("##QuickNavTheme", &generalSettings_.quickNavTheme,
        quickNavThemeNames, IM_ARRAYSIZE(quickNavThemeNames)))
        generalSettingsDirty_ = true;

    BeginSettingRow(_L("app.settings.component_bg"), ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x);
    float bgColor[3] = { personalization_.widgetBgR, personalization_.widgetBgG, personalization_.widgetBgB };
    if (ImGui::ColorEdit3("##WidgetBgColor", bgColor, ImGuiColorEditFlags_NoInputs))
    {
        personalization_.widgetBgR = bgColor[0]; personalization_.widgetBgG = bgColor[1];
        personalization_.widgetBgB = bgColor[2];
        markChanged(false);
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && personalizationDirty_)
        personalizationSaveRequested_ = true;

    BeginSettingRow(_L("app.settings.component_border"), ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x);
    float borderColor[3] = { personalization_.widgetBorderR, personalization_.widgetBorderG, personalization_.widgetBorderB };
    if (ImGui::ColorEdit3("##WidgetBorderColor", borderColor, ImGuiColorEditFlags_NoInputs))
    {
        personalization_.widgetBorderR = borderColor[0]; personalization_.widgetBorderG = borderColor[1];
        personalization_.widgetBorderB = borderColor[2];
        markChanged(false);
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && personalizationDirty_)
        personalizationSaveRequested_ = true;

    ImGui::Spacing();

    BeginSettingRow(_L("app.settings.bg_opacity"), sliderW);
    ImGui::SetNextItemWidth(sliderW);
    int widgetAlphaPercent = static_cast<int>(std::round(personalization_.widgetAlpha * 100.0f));
    if (ImGui::SliderInt("##WidgetAlpha", &widgetAlphaPercent, 0, 100, "%d%%"))
    {
        personalization_.widgetAlpha = widgetAlphaPercent / 100.0f;
        markChanged(false);
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && personalizationDirty_)
        personalizationSaveRequested_ = true;

    BeginSettingRow(_L("app.settings.border_opacity"), sliderW);
    ImGui::SetNextItemWidth(sliderW);
    int widgetBorderAlphaPercent = static_cast<int>(std::round(
        personalization_.widgetBorderAlpha * 100.0f));
    if (ImGui::SliderInt("##WidgetBorderAlpha", &widgetBorderAlphaPercent, 0, 100, "%d%%"))
    {
        personalization_.widgetBorderAlpha = widgetBorderAlphaPercent / 100.0f;
        markChanged(false);
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && personalizationDirty_)
        personalizationSaveRequested_ = true;

    bool gradientToggle = personalization_.gradientEndA > 0.001f;
    if (DrawSettingCheckbox(_L("app.settings.enable_gradient"), "##GradientToggle", &gradientToggle))
    {
        personalization_.gradientEndA = gradientToggle
            ? presetForId(personalization_.backgroundPreset).gradientEndA
            : 0.0f;
        markChanged(true);
    }

    ImGui::BeginDisabled(!gradientToggle);
    BeginSettingRow(_L("app.settings.gradient_end_alpha"), sliderW);
    ImGui::SetNextItemWidth(sliderW);
    int gradientEndAlphaPercent = static_cast<int>(std::round(
        personalization_.gradientEndA * 100.0f));
    if (ImGui::SliderInt("##GradientEndAlpha", &gradientEndAlphaPercent, 0, 100, "%d%%"))
    {
        personalization_.gradientEndA = gradientEndAlphaPercent / 100.0f;
        markChanged(false);
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && personalizationDirty_)
        personalizationSaveRequested_ = true;
    ImGui::EndDisabled();

    if (DrawSettingCheckbox(_L("app.settings.glass_enabled"), "##WidgetGlassEnabled",
        &personalization_.glassEnabled))
        markChanged(true);

    ImGui::BeginDisabled(!personalization_.glassEnabled);
    BeginSettingRow(_L("app.settings.blur_radius"), controlW);
    ImGui::SetNextItemWidth(controlW);
    if (ImGui::SliderFloat("##GlassBlurRadius", &personalization_.glassBlurRadius, 4.0f, 48.0f, "%.0f px"))
        markChanged(false);
    if (ImGui::IsItemDeactivatedAfterEdit() && personalizationDirty_)
        personalizationSaveRequested_ = true;

    if (DrawSettingCheckbox(_L("app.settings.acrylic_noise"), "##WidgetAcrylicEnabled",
        &personalization_.acrylicEnabled))
        markChanged(true);
    ImGui::EndDisabled();

    BeginSettingRow(_L("app.settings.text_color"), controlW);
    const char* contentThemeNames[] = { _L("app.settings.light"), _L("app.settings.dark") };
    int contentTheme = personalization_.contentTheme;
    ImGui::SetNextItemWidth(controlW);
    if (ImGui::Combo("##ContentTheme", &contentTheme,
        contentThemeNames, IM_ARRAYSIZE(contentThemeNames)))
    {
        personalization_.contentTheme = contentTheme;
        markChanged(true);
    }

    ImGui::Unindent(8.0f * dpiScale_);
    }

    ImGui::Spacing();
    EndSettingsCard();
    BeginSettingsCard(_L("app.settings.context_menu_appearance"));

    const char* contextMenuStyleNames[] = {
        _L("app.settings.context_menu_follow_system"),
        _L("app.settings.context_menu_system_light_blur"),
        _L("app.settings.context_menu_system_dark_blur"),
        _L("app.settings.context_menu_opaque_light"),
        _L("app.settings.context_menu_opaque_dark")
    };
    BeginSettingRow(_L("app.settings.context_menu_style"), controlW);
    ImGui::SetNextItemWidth(controlW);
    if (ImGui::Combo("##ContextMenuStyle",
        &personalization_.contextMenuStyle,
        contextMenuStyleNames, IM_ARRAYSIZE(contextMenuStyleNames)))
    {
        personalization_.contextMenuStyle = std::clamp(
            personalization_.contextMenuStyle, 0, 4);
        markChanged(true);
    }

    ImGui::Spacing();
    EndSettingsCard();
    BeginSettingsCard(_L("app.settings.widget_layout"));

    const float componentSpacingMaximum =
        componentSpacingMaximumProvider_
            ? componentSpacingMaximumProvider_()
            : snowdesktop::widget_spacing_rules::kMaximumComponentScale;
    const int componentSpacingMax = std::max(50, static_cast<int>(std::round(
        componentSpacingMaximum * 100.0f)));
    componentSpacingScale_ = snowdesktop::widget_spacing_rules::
        ClampComponentScale(componentSpacingScale_, componentSpacingMaximum);
    componentSpacingPct_ = std::clamp(
        static_cast<int>(std::round(componentSpacingScale_ * 100.0f)),
        1, componentSpacingMax);
    BeginSettingRow(_L("app.settings.component_spacing"), sliderActionW,
        _L("app.settings.component_spacing_hint"));
    ImGui::SetNextItemWidth(actionSliderW);
    if (ImGui::SliderInt("##ComponentSpacing", &componentSpacingPct_,
        1, componentSpacingMax, "%d%%", ImGuiSliderFlags_None))
    {
        componentSpacingScale_ = snowdesktop::widget_spacing_rules::
            ClampComponentScale(
                componentSpacingPct_ / 100.0f, componentSpacingMaximum);
        if (displaySettingsChangedCallback_)
            displaySettingsChangedCallback_();
    }
    ImGui::SameLine();
    if (BlueButton((std::string(_L("app.settings.restore_default")) +
        "##ComponentSpacingDefault").c_str(), ImVec2(resetW, 0)))
    {
        componentSpacingScale_ = snowdesktop::widget_spacing_rules::
            ClampComponentScale(1.0f, componentSpacingMaximum);
        componentSpacingPct_ = static_cast<int>(std::round(
            componentSpacingScale_ * 100.0f));
        if (displaySettingsChangedCallback_)
            displaySettingsChangedCallback_();
    }

    BeginSettingRow(_L("app.settings.corner_radius"), sliderActionW);
    ImGui::SetNextItemWidth(actionSliderW);
    if (ImGui::SliderFloat("##WidgetCornerRadius", &personalization_.cornerRadius,
        4.0f, 28.0f, "%.0f cu"))
        markChanged(false);
    if (ImGui::IsItemDeactivatedAfterEdit() && personalizationDirty_)
        personalizationSaveRequested_ = true;
    ImGui::SameLine();
    if (BlueButton((std::string(_L("app.settings.restore_default")) +
        "##WidgetCornerRadiusDefault").c_str(), ImVec2(resetW, 0)))
    {
        personalization_.cornerRadius = 12.0f;
        markChanged(true);
    }

    BeginSettingRow(_L("app.settings.bar_height"), sliderActionW);
    ImGui::SetNextItemWidth(actionSliderW);
    if (ImGui::SliderFloat("##BarHeight", &personalization_.barHeight,
        16.0f, 48.0f, "%.0f cu"))
        markChanged(false);
    if (ImGui::IsItemDeactivatedAfterEdit() && personalizationDirty_)
        personalizationSaveRequested_ = true;
    ImGui::SameLine();
    if (BlueButton((std::string(_L("app.settings.restore_default")) +
        "##BarHeightDefault").c_str(), ImVec2(resetW, 0)))
    {
        personalization_.barHeight = 24.0f;
        markChanged(true);
    }

    BeginSettingRow(
        _L("app.settings.tab_font_size"),
        sliderActionW);
    ImGui::SetNextItemWidth(actionSliderW);
    if (ImGui::SliderFloat(
            "##CategorizedTabFontSize",
            &personalization_.categorizedTabFontSize,
            10.0f, 22.0f, "%.0f cu"))
        markChanged(false);
    if (ImGui::IsItemDeactivatedAfterEdit() &&
        personalizationDirty_)
        personalizationSaveRequested_ = true;
    ImGui::SameLine();
    if (BlueButton(
            (std::string(
                _L("app.settings.restore_default")) +
                "##CategorizedTabFontSizeDefault").c_str(),
            ImVec2(resetW, 0)))
    {
        personalization_.categorizedTabFontSize = 15.0f;
        markChanged(true);
    }

    auto presetSelectionForId = [&](int presetId) {
        const int normalized = NormalizeAppearancePresetId(presetId);
        for (int i = 0; i < IM_ARRAYSIZE(presetIds); ++i)
            if (presetIds[i] == normalized) return i;
        return 0;
    };

    EndSettingsCard();
    EndPage();
}

/**
 * @brief 绘制「桌面图标」外观页（间距、字号与图标美化）。
 */
void SettingsWindow::DrawAppearanceDesktopIconsPage()
{
    BeginPage();
    BeginSettingsCard(_L("app.settings.desktop_icons"));
    DrawDisplayPage();
    EndSettingsCard();
    EndPage();
}

/**
 * @brief 绘制「系统任务栏」页（任务栏覆盖外观与动态显示规则）。
 */
void SettingsWindow::DrawTaskbarPage()
{
    BeginPage();
    BeginSettingsCard(_L("app.settings.system_appearance"));

    const float controlW = kSettingControlWidthDip * dpiScale_;
    const float sliderW = controlW;

    auto drawOverrideAdvanced = [&](PersonalizationSettings& style,
        const char* id) {
        bool changed = false;
        ImGui::PushID(id);
        ImGui::Spacing();
        ImGui::Indent(8.0f * dpiScale_);
        float background[3] = { style.widgetBgR, style.widgetBgG, style.widgetBgB };
        BeginSettingRow(_L("app.settings.bg_color"), ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x);
        if (ImGui::ColorEdit3("##BackgroundAdvanced", background, ImGuiColorEditFlags_NoInputs))
        {
            style.widgetBgR = background[0];
            style.widgetBgG = background[1];
            style.widgetBgB = background[2];
            changed = true;
        }

        float border[3] = { style.widgetBorderR, style.widgetBorderG, style.widgetBorderB };
        BeginSettingRow(_L("app.settings.border_color"), ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x);
        if (ImGui::ColorEdit3("##BorderAdvanced", border, ImGuiColorEditFlags_NoInputs))
        {
            style.widgetBorderR = border[0];
            style.widgetBorderG = border[1];
            style.widgetBorderB = border[2];
            changed = true;
        }

        BeginSettingRow(_L("app.settings.bg_opacity"), sliderW);
        ImGui::SetNextItemWidth(sliderW);
        int backgroundAlphaPercent = static_cast<int>(std::round(style.widgetAlpha * 100.0f));
        if (ImGui::SliderInt("##BackgroundAlphaAdvanced", &backgroundAlphaPercent, 0, 100, "%d%%"))
        {
            style.widgetAlpha = backgroundAlphaPercent / 100.0f;
            changed = true;
        }

        BeginSettingRow(_L("app.settings.border_opacity"), sliderW);
        ImGui::SetNextItemWidth(sliderW);
        int borderAlphaPercent = static_cast<int>(std::round(
            style.widgetBorderAlpha * 100.0f));
        if (ImGui::SliderInt("##BorderAlphaAdvanced", &borderAlphaPercent, 0, 100, "%d%%"))
        {
            style.widgetBorderAlpha = borderAlphaPercent / 100.0f;
            changed = true;
        }

        if (DrawSettingCheckbox(_L("app.settings.glass_enabled"), "##GlassEnabled",
            &style.glassEnabled))
            changed = true;

        ImGui::BeginDisabled(!style.glassEnabled);
        BeginSettingRow(_L("app.settings.blur_radius"), controlW);
        ImGui::SetNextItemWidth(controlW);
        if (ImGui::SliderFloat("##GlassBlurRadius", &style.glassBlurRadius,
            4.0f, 48.0f, "%.0f px"))
            changed = true;

        if (DrawSettingCheckbox(_L("app.settings.acrylic_noise"), "##AcrylicEnabled",
            &style.acrylicEnabled))
            changed = true;
        ImGui::EndDisabled();

        ImGui::Unindent(8.0f * dpiScale_);
        ImGui::PopID();
        return changed;
    };

    int taskbarThemeMode;
    if (!dockSettings_.systemTaskbarBackdropEnabled)
        taskbarThemeMode = 0;
    else if (dockSettings_.systemTaskbarFollowPersonalization)
        taskbarThemeMode = 1;
    else
    {
        const int preset = NormalizeAppearancePresetId(
            dockSettings_.systemTaskbarAppearance.backgroundPreset);
        if (preset == kAppearancePresetCustom)
            taskbarThemeMode = 8;
        else switch (preset)
        {
        case kAppearancePresetDark:        taskbarThemeMode = 2; break;
        case kAppearancePresetLight:       taskbarThemeMode = 3; break;
        case kAppearancePresetGlassDark:   taskbarThemeMode = 4; break;
        case kAppearancePresetGlassLight:  taskbarThemeMode = 5; break;
        case kAppearancePresetAcrylicDark: taskbarThemeMode = 6; break;
        case kAppearancePresetAcrylicLight: taskbarThemeMode = 7; break;
        case kAppearancePresetTaskbarTransparent: taskbarThemeMode = 9; break;
        default:                            taskbarThemeMode = 2; break;
        }
    }

    BeginSettingRow(_L("app.settings.taskbar_theme"), controlW,
        _L("app.settings.taskbar_theme_hint"));
    const char* taskbarThemeNames[] = {
        _L("app.settings.taskbar_windows_native"), _L("app.settings.taskbar_follow_global"),
        _L("app.settings.dark"), _L("app.settings.light"), _L("app.settings.dark_glass"), _L("app.settings.light_glass"),
        _L("app.settings.dark_acrylic"), _L("app.settings.light_acrylic"), _L("app.settings.custom"),
        _L("app.settings.taskbar_transparent")
    };
    ImGui::SetNextItemWidth(controlW);
    if (ImGui::Combo("##TaskbarThemeMode", &taskbarThemeMode,
        taskbarThemeNames, IM_ARRAYSIZE(taskbarThemeNames)))
    {
        switch (taskbarThemeMode)
        {
        case 0:
            dockSettings_.systemTaskbarBackdropEnabled = false;
            break;
        case 1:
            dockSettings_.systemTaskbarBackdropEnabled = true;
            dockSettings_.systemTaskbarFollowPersonalization = true;
            dockSettings_.systemTaskbarContentTheme = -1;
            break;
        case 2:
        case 3:
        case 4:
        case 5:
        case 6:
        case 7:
            dockSettings_.systemTaskbarBackdropEnabled = true;
            dockSettings_.systemTaskbarFollowPersonalization = false;
            dockSettings_.systemTaskbarContentTheme = -1;
            {
                constexpr int modeToPreset[] = {
                    -1, -1,
                    kAppearancePresetDark,
                    kAppearancePresetLight,
                    kAppearancePresetGlassDark,
                    kAppearancePresetGlassLight,
                    kAppearancePresetAcrylicDark,
                    kAppearancePresetAcrylicLight
                };
                dockSettings_.systemTaskbarAppearance =
                    MakeAppearancePreset(
                        modeToPreset[taskbarThemeMode]);
            }
            break;
        case 8:
            dockSettings_.systemTaskbarBackdropEnabled = true;
            dockSettings_.systemTaskbarFollowPersonalization = false;
            dockSettings_.systemTaskbarAppearance.backgroundPreset =
                kAppearancePresetCustom;
            if (dockSettings_.systemTaskbarContentTheme < 0)
                dockSettings_.systemTaskbarContentTheme =
                    dockSettings_.systemTaskbarAppearance.contentTheme;
            break;
        case 9:
            dockSettings_.systemTaskbarBackdropEnabled = true;
            dockSettings_.systemTaskbarFollowPersonalization = false;
            dockSettings_.systemTaskbarContentTheme = -1;
            dockSettings_.systemTaskbarAppearance =
                MakeTransparentTaskbarAppearance();
            break;
        }
        dockSettingsDirty_ = true;
        dockSettingsPreviewDirty_ = true;
        dockSettingsSaveRequested_ = true;
    }

    const auto dynamicRuleNeedsHook =
        [](const SystemTaskbarDynamicRule& rule) {
            return rule.enabled &&
                rule.themeMode != SystemTaskbarThemeMode::Native;
        };
    if (taskbarThemeMode != 0 ||
        dynamicRuleNeedsHook(dockSettings_.systemTaskbarVisibleWindow) ||
        dynamicRuleNeedsHook(dockSettings_.systemTaskbarMaximizedWindow) ||
        dynamicRuleNeedsHook(dockSettings_.systemTaskbarShellUi))
    {
        const char* taskbarRuntimeStatus = nullptr;
        switch (GetSystemTaskbarBackdropRuntimeState())
        {
        case SystemTaskbarBackdropRuntimeState::Loading:
            taskbarRuntimeStatus = _L("app.settings.taskbar_connecting");
            break;
        case SystemTaskbarBackdropRuntimeState::Unsupported:
            taskbarRuntimeStatus = _L("app.settings.taskbar_unsupported");
            break;
        case SystemTaskbarBackdropRuntimeState::Failed:
            taskbarRuntimeStatus = _L("app.settings.taskbar_connect_failed");
            break;
        case SystemTaskbarBackdropRuntimeState::Disabled:
        case SystemTaskbarBackdropRuntimeState::Active:
        default:
            break;
        }
        if (taskbarRuntimeStatus)
        {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f),
                "%s", taskbarRuntimeStatus);
        }

    BeginSettingRow(_L("app.settings.widget_content_theme"), controlW);
        if (taskbarThemeMode == 8)
        {
            const char* ctNames[] = { _L("app.settings.light"), _L("app.settings.dark") };
            int ct = dockSettings_.systemTaskbarContentTheme;
            if (ct < 0)
                ct = dockSettings_.systemTaskbarAppearance.contentTheme;
            ImGui::SetNextItemWidth(controlW);
            if (ImGui::Combo("##ContentThemeTaskbar", &ct,
                ctNames, IM_ARRAYSIZE(ctNames)))
            {
                dockSettings_.systemTaskbarContentTheme = ct;
                dockSettingsDirty_ = true;
                dockSettingsPreviewDirty_ = true;
                dockSettingsSaveRequested_ = true;
            }
        }
        else
        {
            const char* ctNames[] = { _L("app.settings.taskbar_follow_theme"), _L("app.settings.light"), _L("app.settings.dark") };
            int ct = dockSettings_.systemTaskbarContentTheme + 1;
            ImGui::SetNextItemWidth(controlW);
            if (ImGui::Combo("##ContentThemeTaskbar", &ct,
                ctNames, IM_ARRAYSIZE(ctNames)))
            {
                dockSettings_.systemTaskbarContentTheme = ct - 1;
                dockSettingsDirty_ = true;
                dockSettingsPreviewDirty_ = true;
                dockSettingsSaveRequested_ = true;
            }
        }

        if (taskbarThemeMode == 8)
        {
            ImGui::Indent(8.0f * dpiScale_);
            if (drawOverrideAdvanced(
                dockSettings_.systemTaskbarAppearance,
                "OverrideAdvanced"))
            {
                dockSettingsDirty_ = true;
                dockSettingsPreviewDirty_ = true;
                dockSettingsSaveRequested_ = true;
            }
            ImGui::Unindent(8.0f * dpiScale_);
        }
    }

    auto drawDynamicTaskbarRule = [&](const char* label, const char* id,
        SystemTaskbarDynamicRule& rule) {
        ImGui::PushID(id);
        ImGui::Spacing();
        if (DrawSettingCheckbox(label, "##Enabled", &rule.enabled))
        {
            dockSettingsDirty_ = true;
            dockSettingsPreviewDirty_ = true;
            dockSettingsSaveRequested_ = true;
        }

        if (!rule.enabled)
        {
            ImGui::PopID();
            return;
        }

        ImGui::Indent(8.0f * dpiScale_);
        int mode = std::clamp(static_cast<int>(rule.themeMode),
            static_cast<int>(SystemTaskbarThemeMode::Native),
            static_cast<int>(SystemTaskbarThemeMode::Transparent));
        BeginSettingRow(_L("app.settings.taskbar_dynamic_theme"), controlW);
        ImGui::SetNextItemWidth(controlW);
        if (ImGui::Combo("##Theme", &mode, taskbarThemeNames,
            IM_ARRAYSIZE(taskbarThemeNames)))
        {
            const SystemTaskbarThemeMode previousMode = rule.themeMode;
            rule.themeMode = static_cast<SystemTaskbarThemeMode>(mode);
            if (rule.themeMode == SystemTaskbarThemeMode::Custom)
            {
                if (previousMode >= SystemTaskbarThemeMode::Dark &&
                    previousMode <= SystemTaskbarThemeMode::AcrylicLight)
                {
                    constexpr int modeToPreset[] = {
                        -1, -1,
                        kAppearancePresetDark,
                        kAppearancePresetLight,
                        kAppearancePresetGlassDark,
                        kAppearancePresetGlassLight,
                        kAppearancePresetAcrylicDark,
                        kAppearancePresetAcrylicLight
                    };
                    rule.appearance = MakeAppearancePreset(
                        modeToPreset[static_cast<int>(previousMode)]);
                }
                rule.appearance.backgroundPreset = kAppearancePresetCustom;
            }
            dockSettingsDirty_ = true;
            dockSettingsPreviewDirty_ = true;
            dockSettingsSaveRequested_ = true;
        }

        const char* dynamicContentThemeNames[] = {
            _L("app.settings.taskbar_follow_theme"),
            _L("app.settings.light"),
            _L("app.settings.dark")
        };
        int contentTheme = std::clamp(rule.contentTheme, -1, 1) + 1;
        BeginSettingRow(_L("app.settings.widget_content_theme"), controlW);
        ImGui::SetNextItemWidth(controlW);
        if (ImGui::Combo("##ContentTheme", &contentTheme,
            dynamicContentThemeNames,
            IM_ARRAYSIZE(dynamicContentThemeNames)))
        {
            rule.contentTheme = contentTheme - 1;
            dockSettingsDirty_ = true;
            dockSettingsPreviewDirty_ = true;
            dockSettingsSaveRequested_ = true;
        }

        if (rule.themeMode == SystemTaskbarThemeMode::Custom &&
            drawOverrideAdvanced(rule.appearance, "CustomAppearance"))
        {
            rule.appearance.backgroundPreset = kAppearancePresetCustom;
            dockSettingsDirty_ = true;
            dockSettingsPreviewDirty_ = true;
            dockSettingsSaveRequested_ = true;
        }
        ImGui::Unindent(8.0f * dpiScale_);
        ImGui::PopID();
    };

    drawDynamicTaskbarRule(
        _L("app.settings.taskbar_dynamic_visible_window"),
        "VisibleWindow", dockSettings_.systemTaskbarVisibleWindow);
    drawDynamicTaskbarRule(
        _L("app.settings.taskbar_dynamic_maximized_window"),
        "MaximizedWindow", dockSettings_.systemTaskbarMaximizedWindow);
    drawDynamicTaskbarRule(
        _L("app.settings.taskbar_dynamic_shell_ui"),
        "ShellUi", dockSettings_.systemTaskbarShellUi);

    ImGui::Spacing();
    DrawSystemTaskbarPage();

    EndSettingsCard();
    EndPage();
}

/**
 * @brief 打开组件编辑器界面。
 *
 * 设置当前编辑的组件索引、ID、名称和脚本路径，
 * 然后显示设置窗口（切换到编辑器页面）。
 * @param widgetIndex 在组件列表中的索引
 * @param widgetId    组件的唯一标识符
 * @param widgetName  组件的显示名称
 * @param scriptPath  组件脚本文件路径
 */
void SettingsWindow::ShowWidgetEditor(size_t widgetIndex,
    const wchar_t* widgetId, const wchar_t* widgetName, const wchar_t* scriptPath)
{
    editingWidgetIndex_ = widgetIndex;
    widgetEditorBackPending_ = false;
    editingWidgetId_ = widgetId;
    editingWidgetName_ = widgetName;
    editingScriptPath_ = scriptPath;
    Show();
}

/**
 * @brief 绘制组件编辑器页面。
 *
 * 页面顶部提供"返回主界面"按钮，显示当前正在编辑的组件名称。
 * 委托 WidgetEngine 进行具体编辑界面的渲染（调用
 * EnsureWidgetLoaded 和 RenderWidgetEditor）。
 */
void SettingsWindow::DrawWidgetEditorPage()
{
    const float pad = 14.0f * dpiScale_;
    const float toolbarH = 48.0f * dpiScale_;
    const ImVec2 toolbarPos = ImGui::GetCursorScreenPos();
    const float toolbarW = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    drawList->AddRectFilled(toolbarPos,
        ImVec2(toolbarPos.x + toolbarW, toolbarPos.y + toolbarH),
        IM_COL32(248, 248, 250, 255), 8.0f * dpiScale_);
    drawList->AddLine(ImVec2(toolbarPos.x, toolbarPos.y + toolbarH),
        ImVec2(toolbarPos.x + toolbarW, toolbarPos.y + toolbarH),
        IM_COL32(210, 210, 218, 255), 1.0f);

    ImGui::SetCursorScreenPos(ImVec2(toolbarPos.x + pad, toolbarPos.y + 8.0f * dpiScale_));
    const ImVec2 backSize(116.0f * dpiScale_, 32.0f * dpiScale_);
    const ImVec2 backPos = ImGui::GetCursorScreenPos();
    bool backClicked = ImGui::InvisibleButton("##WidgetEditorBack", backSize);
    bool backHovered = ImGui::IsItemHovered();
    drawList->AddRectFilled(backPos,
        ImVec2(backPos.x + backSize.x, backPos.y + backSize.y),
        backHovered ? IM_COL32(226, 234, 246, 255) : IM_COL32(238, 242, 248, 255),
        16.0f * dpiScale_);
    drawList->AddRect(backPos,
        ImVec2(backPos.x + backSize.x, backPos.y + backSize.y),
        backHovered ? IM_COL32(110, 145, 190, 255) : IM_COL32(198, 208, 222, 255),
        16.0f * dpiScale_, 0, 1.0f);
    drawList->AddText(ImVec2(backPos.x + 14.0f * dpiScale_, backPos.y + 7.0f * dpiScale_),
        IM_COL32(42, 52, 68, 255), "<");
    drawList->AddText(ImVec2(backPos.x + 34.0f * dpiScale_, backPos.y + 7.0f * dpiScale_),
        IM_COL32(42, 52, 68, 255), _L("app.settings.widget_editor_back"));

    std::string title = _L("app.settings.widget_editor");
    std::string name = WideToUtf8(editingWidgetName_);
    if (!name.empty())
        title += " / " + name;
    drawList->AddText(ImVec2(backPos.x + backSize.x + 18.0f * dpiScale_,
            toolbarPos.y + 15.0f * dpiScale_),
        IM_COL32(36, 39, 46, 255), title.c_str());

    ImGui::SetCursorScreenPos(ImVec2(toolbarPos.x, toolbarPos.y + toolbarH + 10.0f * dpiScale_));

    if (backClicked)
        widgetEditorBackPending_ = true;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
    ImGui::BeginChild("##WidgetEditorScroll", ImVec2(0, 0),
        ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
        ImGuiWindowFlags_AlwaysVerticalScrollbar);
    ImGui::PopStyleVar();

    if (widgetEngine_ && !widgetEditorBackPending_)
    {
        // Make input cursor clearly black
        ImGui::PushStyleColor(ImGuiCol_InputTextCursor, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));

        widgetEngine_->EnsureWidgetLoaded(editingWidgetId_, editingScriptPath_);
        bool sharedGlassSettingsChanged = false;
        bool sharedGlassSettingsSaveRequested = false;
        widgetEngine_->RenderWidgetEditor(editingWidgetId_, editingWidgetName_,
            personalization_, sharedGlassSettingsChanged,
            sharedGlassSettingsSaveRequested);
        if (sharedGlassSettingsChanged)
        {
            personalizationDirty_ = true;
            personalizationPreviewDirty_ = true;
        }
        if (sharedGlassSettingsSaveRequested && personalizationDirty_)
            personalizationSaveRequested_ = true;

        ImGui::PopStyleColor(1);
    }

    ImGui::EndChild();
}

void SettingsWindow::DrawWidgetPackagesPage()
{
    const float pad = 18.0f * dpiScale_;
    ImVec2 pageSize = ImGui::GetContentRegionAvail();
    pageSize.x = std::max(1.0f, pageSize.x);
    pageSize.y = std::max(1.0f, pageSize.y);
    // 页面滚动容器：透明背景，四周留出外边距以呈现悬浮卡片效果。
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, 4.0f * dpiScale_));
    ImGui::BeginChild("##WidgetPackagesPage", pageSize, ImGuiChildFlags_None,
        ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_AlwaysVerticalScrollbar);
    ImGui::PopStyleVar();

    // ── 全局组件设置 ──
    BeginSettingsCard(_L("app.settings.widget_global_settings"));

    if (DrawSettingCheckbox(_L("app.settings.widget_free_position"),
        "##WidgetFreePosition", &generalSettings_.widgetFreePosition))
        generalSettingsDirty_ = true;
    if (DrawSettingCheckbox(_L("app.settings.widget_hide_border"),
        "##WidgetHideBorder", &generalSettings_.widgetHideBorder))
        generalSettingsDirty_ = true;

    EndSettingsCard();

    const auto packages = WidgetEngine::ListWidgetPackages();
    const bool steamBridgeAvailable =
        WidgetEngine::IsSteamWorkshopBridgeAvailable();
    const bool workshopPublisherAvailable =
        steamBridgeAvailable && IsSteamWorkshopPublisherAvailable();
    const std::string currentLocale =
        Locale::Instance().GetEffectiveLanguage();
    auto localizedManifest = [&](snowdesktop::widget::PackageManifest manifest)
    {
        return snowdesktop::widget::LocalizePackageManifest(
            std::move(manifest), currentLocale);
    };
    auto permissionLabel = [](const std::string& permission)
    {
        if (permission == "desktop.action")
            return _L("app.settings.widgets_permission_desktop_action");
        if (permission == "desktop.read")
            return _L("app.settings.widgets_permission_desktop_read");
        if (permission == "everything.search")
            return _L("app.settings.widgets_permission_everything_search");
        if (permission == "media.action")
            return _L("app.settings.widgets_permission_media_action");
        if (permission == "media.read")
            return _L("app.settings.widgets_permission_media_read");
        if (permission == "network.http")
            return _L("app.settings.widgets_permission_network_http");
        if (permission == "system.read")
            return _L("app.settings.widgets_permission_system_read");
        if (permission == "ui.contextMenu")
            return _L("app.settings.widgets_permission_ui_context_menu");
        if (permission == "ui.input")
            return _L("app.settings.widgets_permission_ui_input");
        if (permission == "ui.notify")
            return _L("app.settings.widgets_permission_ui_notify");
        return permission.c_str();
    };
    auto drawPermissions = [&](const std::vector<std::string>& permissions)
    {
        const bool desktopAccess =
            std::find(permissions.begin(), permissions.end(),
                "desktop.read") != permissions.end() ||
            std::find(permissions.begin(), permissions.end(),
                "desktop.action") != permissions.end();
        const bool mediaAccess =
            std::find(permissions.begin(), permissions.end(),
                "media.read") != permissions.end() ||
            std::find(permissions.begin(), permissions.end(),
                "media.action") != permissions.end();
        std::string summary;
        for (const auto& permission : permissions)
        {
            if (permission == "ui.input" ||
                permission == "ui.contextMenu" ||
                permission == "desktop.read" ||
                permission == "desktop.action" ||
                permission == "media.read" ||
                permission == "media.action")
                continue;
            if (!summary.empty()) summary += ", ";
            summary += permissionLabel(permission);
        }
        if (desktopAccess)
        {
            if (!summary.empty()) summary += ", ";
            summary += _L("app.settings.widgets_permission_desktop");
        }
        if (mediaAccess)
        {
            if (!summary.empty()) summary += ", ";
            summary += _L("app.settings.widgets_permission_media");
        }
        if (summary.empty()) return;
        ImGui::TextWrapped("%s: %s",
            _L("app.settings.widgets_permissions"), summary.c_str());
    };
    auto sourceLabel = [](const std::string& providerId)
    {
        if (providerId == "builtin")
            return std::string(
                _L("app.settings.widgets_source_builtin"));
        if (providerId == "local-directory" || providerId == "local")
            return std::string(
                _L("app.settings.widgets_source_local"));
        if (providerId == "static-catalog")
            return std::string(
                _L("app.settings.widgets_source_catalog"));
        if (providerId == "steam-workshop")
            return std::string(
                _L("app.settings.widgets_source_steam"));
        return providerId;
    };
    auto needsInstallConfirmation = [](const std::wstring& message)
    {
        return message.find(L"requires explicit confirmation") !=
                std::wstring::npos ||
            message.find(L"requests a new permission") !=
                std::wstring::npos ||
            message.find(L"expands network access") != std::wstring::npos;
    };
    auto finishInstallAttempt = [&](bool ok, const std::wstring& installError,
        PendingWidgetInstallKind kind, const std::filesystem::path& localPath,
        const std::string& providerId, const std::string& externalId,
        const std::string& version)
    {
        if (ok)
        {
            widgetPackageStatus_ = _L("app.settings.widgets_install_ok");
            pendingWidgetInstallKind_ = PendingWidgetInstallKind::None;
            pendingWidgetInstallPath_.clear();
            pendingWidgetInstallProviderId_.clear();
            pendingWidgetInstallExternalId_.clear();
            pendingWidgetInstallVersion_.clear();
            pendingWidgetInstallReason_.clear();
            if (reloadCallback_) reloadCallback_();
            return;
        }
        widgetPackageStatus_ = WideToUtf8(installError);
        if (!needsInstallConfirmation(installError)) return;
        pendingWidgetInstallKind_ = kind;
        pendingWidgetInstallPath_ = localPath.wstring();
        pendingWidgetInstallProviderId_ = providerId;
        pendingWidgetInstallExternalId_ = externalId;
        pendingWidgetInstallVersion_ = version;
        pendingWidgetInstallReason_ = installError;
    };
    auto installLocalPackage = [&](const std::filesystem::path& path,
        bool confirmed)
    {
        std::wstring installError;
        const bool ok = widgetEngine_ &&
            widgetEngine_->InstallAndVerifyWidgetPackage(path.wstring(),
                installError, confirmed, confirmed);
        finishInstallAttempt(ok, installError,
            PendingWidgetInstallKind::Local, path, {}, {}, {});
    };
    auto installSourcePackage =
        [&](const snowdesktop::widget::PackageDetails& details,
            const std::string& providerId, bool confirmed)
    {
        std::wstring installError;
        const bool ok = widgetEngine_ &&
            widgetEngine_->InstallAndVerifyWidgetPackageFromSource(
                providerId, details.source.externalItemId,
                details.manifest.version, installError,
                confirmed, confirmed);
        finishInstallAttempt(ok, installError,
            PendingWidgetInstallKind::StaticCatalog, widgetCatalogPath_,
            providerId, details.source.externalItemId,
            details.manifest.version);
    };
    auto queryCatalog = [&](bool applySafeUpdates, bool announce)
    {
        widgetCatalogEntries_.clear();
        if (widgetPackageSourceId_.empty()) return;
        snowdesktop::widget::PackageQuery query;
        query.text = widgetCatalogSearch_;
        query.locale = currentLocale;
        query.limit = 200;
        std::string catalogError;
        widgetCatalogEntries_ = WidgetEngine::QueryWidgetPackageSource(
            widgetPackageSourceId_, query, catalogError);
        if (!catalogError.empty())
            widgetPackageStatus_ = catalogError;
        else if (announce)
            widgetPackageStatus_ =
                _L("app.settings.widgets_catalog_loaded");
        if (catalogError.empty() && applySafeUpdates && widgetEngine_)
        {
            std::string updateReport;
            const int updated =
                widgetEngine_->ApplySafeWidgetPackageUpdates(
                    widgetPackageSourceId_, updateReport);
            if (updated > 0)
            {
                char message[256]{};
                std::snprintf(message, sizeof(message),
                    _L("app.settings.widgets_safe_updates_applied"),
                    updated);
                widgetPackageStatus_ = message;
                if (reloadCallback_) reloadCallback_();
            }
        }
    };
    auto refreshCatalog = [&]()
    {
        queryCatalog(true, true);
    };
    const auto allSources = WidgetEngine::ListWidgetPackageSources();
    std::vector<snowdesktop::widget::PackageSourceInfo> sources;
    for (const auto& source : allSources)
    {
        if (!source.capabilities.query || !source.status.available ||
            source.providerId == "builtin" ||
            source.providerId == "local-directory")
            continue;
        sources.push_back(source);
    }
    const bool selectedSourceAvailable = std::any_of(
        sources.begin(), sources.end(), [&](const auto& source)
        {
            return source.providerId == widgetPackageSourceId_;
        });
    bool sourceChanged = false;
    if (!selectedSourceAvailable)
    {
        widgetPackageSourceId_ =
            sources.empty() ? std::string{} : sources.front().providerId;
        widgetCatalogEntries_.clear();
        sourceChanged = !widgetPackageSourceId_.empty();
    }
    if (!widgetCatalogInitialized_ ||
        widgetCatalogLocale_ != currentLocale || sourceChanged)
    {
        widgetCatalogInitialized_ = true;
        widgetCatalogLocale_ = currentLocale;
        queryCatalog(false, false);
    }

    if (!sources.empty())
    {
        ImGui::SeparatorText(
            _L("app.settings.widgets_catalog_results"));
    std::string sourcePreview = sourceLabel(widgetPackageSourceId_);
    if (sources.size() > 1)
    {
        ImGui::TextUnformatted(_L("app.settings.widgets_source"));
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::BeginCombo("##WidgetPackageSource",
            sourcePreview.c_str()))
        {
            for (const auto& source : sources)
            {
                const bool selected =
                    source.providerId == widgetPackageSourceId_;
                std::string label = sourceLabel(source.providerId);
                if (!source.status.available)
                {
                    label += " (";
                    label += _L("app.settings.widgets_source_offline");
                    label += ")";
                }
                if (ImGui::Selectable(label.c_str(), selected))
                {
                    widgetPackageSourceId_ = source.providerId;
                    refreshCatalog();
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }
        ImGui::SetNextItemWidth(-90.0f * dpiScale_);
        ImGui::InputTextWithHint("##WidgetCatalogSearch",
            _L("app.settings.widgets_search_hint"), widgetCatalogSearch_,
            sizeof(widgetCatalogSearch_));
        ImGui::SameLine();
        if (ImGui::Button(_L("app.settings.widgets_search")))
            refreshCatalog();

        if (widgetCatalogEntries_.empty())
            ImGui::TextDisabled("%s",
                _L("app.settings.widgets_catalog_empty"));
        if (!widgetCatalogEntries_.empty())
        {
            const float availableWidth =
                ImGui::GetContentRegionAvail().x;
            const float cardGap = 10.0f * dpiScale_;
            const float minimumCardWidth = 250.0f * dpiScale_;
            const int catalogColumns = std::clamp(
                static_cast<int>((availableWidth + cardGap) /
                    (minimumCardWidth + cardGap)), 1, 3);
            const float cardWidth =
                (availableWidth -
                    cardGap * static_cast<float>(catalogColumns - 1)) /
                static_cast<float>(catalogColumns);
            const ImVec2 gridStart = ImGui::GetCursorPos();
            std::vector<float> columnHeights(
                static_cast<std::size_t>(catalogColumns), 0.0f);
            for (const auto& details : widgetCatalogEntries_)
            {
                const auto shortest = std::min_element(
                    columnHeights.begin(), columnHeights.end());
                const int column = static_cast<int>(
                    std::distance(columnHeights.begin(), shortest));
                ImGui::SetCursorPos(ImVec2(
                    gridStart.x +
                        static_cast<float>(column) *
                            (cardWidth + cardGap),
                    gridStart.y + *shortest));
                const auto manifest =
                    localizedManifest(details.manifest);
                ImGui::PushID((details.source.externalItemId +
                    details.manifest.version).c_str());
                ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding,
                    6.0f * dpiScale_);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                    ImVec2(10.0f * dpiScale_,
                        9.0f * dpiScale_));
                ImGui::BeginChild("##WidgetCatalogCard",
                    ImVec2(cardWidth, 0),
                    ImGuiChildFlags_Borders |
                        ImGuiChildFlags_AlwaysUseWindowPadding |
                        ImGuiChildFlags_AutoResizeY |
                        ImGuiChildFlags_AlwaysAutoResize);
                ImGui::PopStyleVar(2);
                ImGui::TextUnformatted(manifest.name.c_str());
                ImGui::SameLine();
                ImGui::TextDisabled(
                    _L("app.settings.widgets_version"),
                    manifest.version.c_str());
                if (!manifest.description.empty())
                    ImGui::TextWrapped("%s",
                        manifest.description.c_str());
                drawPermissions(manifest.permissions);
                bool exactInstalled = false;
                bool packageInstalled = false;
                for (const auto& package : packages)
                {
                    if (!package.active ||
                        package.manifest.id != details.manifest.id)
                        continue;
                    packageInstalled = true;
                    if (package.manifest.version ==
                        details.manifest.version)
                        exactInstalled = true;
                }
                ImGui::Spacing();
                if (exactInstalled)
                {
                    ImGui::TextDisabled("%s",
                        _L("app.settings.widgets_installed"));
                }
                else
                {
                    const char* installLabel = packageInstalled
                        ? _L("app.settings.widgets_update")
                        : _L("app.settings.widgets_install");
                    if (BlueButton(installLabel))
                    {
                        installSourcePackage(
                            details, widgetPackageSourceId_, false);
                    }
                }
                ImGui::EndChild();
                columnHeights[static_cast<std::size_t>(column)] +=
                    ImGui::GetItemRectSize().y + cardGap;
                ImGui::PopID();
            }
            const float gridHeight =
                *std::max_element(
                    columnHeights.begin(), columnHeights.end()) -
                cardGap;
            ImGui::SetCursorPos(ImVec2(
                gridStart.x, gridStart.y + std::max(0.0f, gridHeight)));
            ImGui::Dummy(ImVec2(0, 0));
        }
    }

    BeginSettingsCard(_L("app.settings.widgets_management"));
    static constexpr COMDLG_FILTERSPEC packageFilters[] = {
        { L"SnowDesktop Component", L"*.snowwidget;widget.json" },
        { L"All Files", L"*.*" },
    };
    if (BlueButton(_L("app.settings.widgets_install_package")))
    {
        if (const auto selected = PickSettingsFile(hwnd_,
            _LW("app.settings.widgets_install_package"),
            packageFilters,
            static_cast<UINT>(std::size(packageFilters))))
        {
            installLocalPackage(*selected, false);
        }
    }
    if (steamBridgeAvailable)
    {
        ImGui::SameLine();
        if (SecondaryButton(_L("app.settings.widgets_open_steam_workshop")))
        {
            ShellExecuteW(nullptr, L"open",
                L"https://steamcommunity.com/workshop/", nullptr, nullptr,
                SW_SHOWNORMAL);
        }
    }
    ImGui::Spacing();

    const auto legacy = WidgetEngine::ListLegacyWidgetPackages();

    EndSettingsCard();
    BeginSettingsCard(_L("app.settings.widgets_my_components"));
    const int activePackageCount = static_cast<int>(std::count_if(
        packages.begin(), packages.end(),
        [](const auto& package) { return package.active; }));
    const int builtinPackageCount = static_cast<int>(std::count_if(
        packages.begin(), packages.end(), [](const auto& package)
        {
            return package.active && package.builtin;
        }));
    const int installedPackageCount = static_cast<int>(std::count_if(
        packages.begin(), packages.end(), [](const auto& package)
        {
            return package.active && !package.builtin &&
                !package.development;
        }));
    const int developmentPackageCount = static_cast<int>(std::count_if(
        packages.begin(), packages.end(), [](const auto& package)
        {
            return package.active && package.development;
        }));
    if ((widgetPackageFilter_ == 1 && builtinPackageCount == 0) ||
        (widgetPackageFilter_ == 2 && installedPackageCount == 0) ||
        (widgetPackageFilter_ == 3 && developmentPackageCount == 0))
    {
        widgetPackageFilter_ = 0;
    }
    auto drawFilterTag = [&](int filter, const char* label, int count)
    {
        std::string text = label;
        text += " ";
        text += std::to_string(count);
        const bool selected = widgetPackageFilter_ == filter;
        if ((selected ? BlueButton(text.c_str())
                      : SecondaryButton(text.c_str())))
        {
            widgetPackageFilter_ = filter;
        }
    };
    drawFilterTag(0, _L("app.settings.widgets_filter_all"),
        activePackageCount);
    if (builtinPackageCount > 0)
    {
        ImGui::SameLine();
        drawFilterTag(1,
            _L("app.settings.widgets_filter_builtin"),
            builtinPackageCount);
    }
    if (installedPackageCount > 0)
    {
        ImGui::SameLine();
        drawFilterTag(2,
            _L("app.settings.widgets_filter_installed"),
            installedPackageCount);
    }
    if (developmentPackageCount > 0)
    {
        ImGui::SameLine();
        drawFilterTag(3,
            _L("app.settings.widgets_filter_development"),
            developmentPackageCount);
    }

    if (!widgetPackageStatus_.empty())
        ImGui::TextWrapped("%s", widgetPackageStatus_.c_str());

    const int visiblePackageCount = static_cast<int>(std::count_if(
        packages.begin(), packages.end(), [&](const auto& package)
        {
            if (!package.active) return false;
            if (widgetPackageFilter_ == 1) return package.builtin;
            if (widgetPackageFilter_ == 2)
                return !package.builtin && !package.development;
            if (widgetPackageFilter_ == 3) return package.development;
            return true;
        }));
    if (visiblePackageCount == 0)
        ImGui::TextDisabled("%s",
            _L("app.settings.widgets_filter_empty"));
    if (visiblePackageCount > 0)
    {
        const float availableWidth =
            ImGui::GetContentRegionAvail().x;
        const float cardGap = 10.0f * dpiScale_;
        const float minimumCardWidth = 250.0f * dpiScale_;
        const int installedColumns = std::clamp(
            static_cast<int>((availableWidth + cardGap) /
                (minimumCardWidth + cardGap)), 1, 3);
        const float cardWidth =
            (availableWidth -
                cardGap * static_cast<float>(installedColumns - 1)) /
            static_cast<float>(installedColumns);
        const ImVec2 gridStart = ImGui::GetCursorPos();
        std::vector<float> columnHeights(
            static_cast<std::size_t>(installedColumns), 0.0f);
        for (const auto& package : packages)
        {
            if (!package.active) continue;
            if (widgetPackageFilter_ == 1 && !package.builtin)
                continue;
            if (widgetPackageFilter_ == 2 &&
                (package.builtin || package.development))
                continue;
            if (widgetPackageFilter_ == 3 && !package.development)
                continue;
            const auto shortest = std::min_element(
                columnHeights.begin(), columnHeights.end());
            const int column = static_cast<int>(
                std::distance(columnHeights.begin(), shortest));
            ImGui::SetCursorPos(ImVec2(
                gridStart.x +
                    static_cast<float>(column) *
                        (cardWidth + cardGap),
                gridStart.y + *shortest));
            const auto manifest = localizedManifest(package.manifest);
            ImGui::PushID(
                (package.manifest.id + package.manifest.version).c_str());
            const bool hasActions =
                !package.builtin && !package.development;
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding,
                6.0f * dpiScale_);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                ImVec2(10.0f * dpiScale_, 9.0f * dpiScale_));
            ImGui::BeginChild("##InstalledWidgetCard",
                ImVec2(cardWidth, 0),
                ImGuiChildFlags_Borders |
                    ImGuiChildFlags_AlwaysUseWindowPadding |
                    ImGuiChildFlags_AutoResizeY |
                    ImGuiChildFlags_AlwaysAutoResize);
            ImGui::PopStyleVar(2);
            ImGui::TextUnformatted(manifest.name.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled(_L("app.settings.widgets_version"),
                manifest.version.c_str());
            if (!manifest.description.empty())
                ImGui::TextWrapped("%s", manifest.description.c_str());
            const char* kind = package.builtin
                ? _L("app.settings.widgets_builtin")
                : package.development
                ? _L("app.settings.widgets_development")
                : package.enabled
                ? _L("app.settings.widgets_active")
                : _L("app.settings.widgets_disabled");
            ImGui::TextDisabled("%s", kind);
            drawPermissions(package.grantedPermissions.empty()
                ? manifest.permissions : package.grantedPermissions);
            if (hasActions)
            {
                ImGui::Spacing();
                const char* toggleLabel = package.enabled
                    ? _L("app.settings.widgets_disable")
                    : _L("app.settings.widgets_enable");
                if (SecondaryButton(toggleLabel))
                {
                    std::string error;
                    const bool enabled = !package.enabled;
                    if (WidgetEngine::SetWidgetPackageEnabled(
                        package.manifest.id, enabled, error))
                    {
                        widgetPackageStatus_ = enabled
                            ? _L("app.settings.widgets_enabled_ok")
                            : _L("app.settings.widgets_disabled_ok");
                        if (!enabled && widgetEngine_)
                        {
                            std::vector<std::wstring> instances;
                            for (const auto& widget :
                                widgetEngine_->GetWidgets())
                            {
                                if (widget.packageId ==
                                    package.manifest.id)
                                {
                                    instances.push_back(
                                        widget.widgetId);
                                }
                            }
                            for (const auto& instance : instances)
                                widgetEngine_->UnloadWidget(instance);
                        }
                        if (reloadCallback_) reloadCallback_();
                    }
                    else widgetPackageStatus_ = error;
                }
                ImGui::SameLine();
                if (SecondaryButton(
                    _L("app.settings.widgets_uninstall")))
                {
                    pendingWidgetPackageUninstall_ =
                        package.manifest.id;
                }
            }
            ImGui::EndChild();
            columnHeights[static_cast<std::size_t>(column)] +=
                ImGui::GetItemRectSize().y + cardGap;
            ImGui::PopID();
        }
        const float gridHeight =
            *std::max_element(
                columnHeights.begin(), columnHeights.end()) -
            cardGap;
        ImGui::SetCursorPos(ImVec2(
            gridStart.x, gridStart.y + std::max(0.0f, gridHeight)));
        ImGui::Dummy(ImVec2(0, 0));
    }

    if (ImGui::CollapsingHeader(
        _L("app.settings.widgets_advanced")))
    {
        if (workshopPublisherAvailable)
        {
            if (SecondaryButton(
                _L("app.settings.widgets_publish_steam")))
            {
                if (LaunchSteamWorkshopPublisher(
                    WidgetEngine::GetWidgetPackagePaths().development))
                    widgetPackageStatus_.clear();
                else
                    widgetPackageStatus_ =
                        _L("app.settings.widgets_publisher_launch_failed");
            }
            ImGui::Spacing();
        }
        if (!legacy.empty())
        {
            ImGui::SeparatorText(
                _L("app.settings.widgets_legacy_components"));
            ImGui::TextWrapped("%s",
                _L("app.settings.widgets_migration_required"));
            ImGui::Text(_L("app.settings.widgets_legacy_count"),
                static_cast<int>(legacy.size()));
            if (BlueButton(
                _L("app.settings.widgets_migrate_all")))
            {
                int migrated = 0;
                std::ostringstream failures;
                for (const auto& candidate : legacy)
                {
                    const auto result =
                        WidgetEngine::MigrateLegacyWidgetPackage(
                            candidate);
                    if (result.ok)
                        ++migrated;
                    else
                    {
                        failures << WideToUtf8(candidate.legacyName)
                            << ": " << result.error << '\n';
                    }
                }
                char message[256]{};
                std::snprintf(message, sizeof(message),
                    _L("app.settings.widgets_migration_result"),
                    migrated, static_cast<int>(legacy.size()));
                widgetPackageStatus_ = message;
                if (!failures.str().empty())
                    widgetPackageStatus_ += "\n" + failures.str();
                if (migrated > 0 && reloadCallback_)
                    reloadCallback_();
            }
            ImGui::Spacing();
        }

        const auto packagePaths = WidgetEngine::GetWidgetPackagePaths();
        auto countDirectories = [](const std::filesystem::path& path)
        {
            int count = 0;
            std::error_code error;
            for (std::filesystem::directory_iterator it(path, error), end;
                !error && it != end; it.increment(error))
            {
                if (it->is_directory(error)) ++count;
            }
            return count;
        };
        ImGui::Text(_L("app.settings.widgets_storage_summary"),
            countDirectories(packagePaths.development),
            countDirectories(packagePaths.quarantine));

        bool hasOlderVersions = false;
        for (const auto& package : packages)
        {
            if (!package.active && !package.builtin &&
                !package.development)
            {
                hasOlderVersions = true;
                break;
            }
        }
        if (hasOlderVersions)
        {
            ImGui::SeparatorText(
                _L("app.settings.widgets_old_versions"));
            for (const auto& package : packages)
            {
                if (package.active || package.builtin ||
                    package.development)
                    continue;
                const auto manifest = localizedManifest(package.manifest);
                ImGui::PushID(("rollback-" + package.manifest.id +
                    package.manifest.version).c_str());
                ImGui::TextUnformatted(manifest.name.c_str());
                ImGui::SameLine();
                ImGui::TextDisabled(
                    _L("app.settings.widgets_version"),
                    manifest.version.c_str());
                if (ImGui::Button(
                    _L("app.settings.widgets_rollback_action")))
                {
                    std::string error;
                    if (WidgetEngine::RollbackWidgetPackage(
                        package.manifest.id, package.manifest.version,
                        error))
                    {
                        if (widgetEngine_)
                        {
                            std::vector<std::wstring> instances;
                            for (const auto& widget :
                                widgetEngine_->GetWidgets())
                            {
                                if (widget.packageId ==
                                    package.manifest.id)
                                {
                                    instances.push_back(widget.widgetId);
                                }
                            }
                            for (const auto& instance : instances)
                                widgetEngine_->ReloadWidget(instance);
                        }
                        widgetPackageStatus_ =
                            _L("app.settings.widgets_rollback_ok");
                        if (reloadCallback_) reloadCallback_();
                    }
                    else widgetPackageStatus_ = error;
                }
                ImGui::PopID();
            }
        }
        ImGui::Spacing();
        DrawWidgetDeveloperTools();
    }

    if (pendingWidgetInstallKind_ != PendingWidgetInstallKind::None)
        ImGui::OpenPopup("##ConfirmWidgetPackageInstall");
    if (ImGui::BeginPopupModal("##ConfirmWidgetPackageInstall", nullptr,
        ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextWrapped("%s",
            _L("app.settings.widgets_install_confirm"));
        if (!pendingWidgetInstallReason_.empty())
        {
            const std::string rawReason =
                WideToUtf8(pendingWidgetInstallReason_);
            const std::string permissionMarker =
                "requests a new permission: ";
            const std::string domainMarker =
                "expands network access to: ";
            if (const auto position = rawReason.find(permissionMarker);
                position != std::string::npos)
            {
                const std::string permission = rawReason.substr(
                    position + permissionMarker.size());
                ImGui::TextWrapped(
                    _L("app.settings.widgets_new_permission"),
                    permissionLabel(permission));
            }
            else if (const auto domainPosition = rawReason.find(domainMarker);
                domainPosition != std::string::npos)
            {
                ImGui::TextWrapped(
                    _L("app.settings.widgets_new_website"),
                    rawReason.substr(
                        domainPosition + domainMarker.size()).c_str());
            }
            else
            {
                ImGui::TextWrapped("%s",
                    _L("app.settings.widgets_source_change"));
            }
            if (ImGui::CollapsingHeader(
                _L("app.settings.widgets_technical_details")))
            {
                ImGui::TextWrapped("%s", rawReason.c_str());
            }
        }
        if (BlueButton(_L("app.settings.widgets_confirm_install")))
        {
            const auto kind = pendingWidgetInstallKind_;
            const std::filesystem::path path =
                pendingWidgetInstallPath_;
            const std::string externalId =
                pendingWidgetInstallExternalId_;
            const std::string version = pendingWidgetInstallVersion_;
            const std::string providerId =
                pendingWidgetInstallProviderId_;
            pendingWidgetInstallKind_ = PendingWidgetInstallKind::None;
            if (kind == PendingWidgetInstallKind::Local)
            {
                installLocalPackage(path, true);
            }
            else
            {
                snowdesktop::widget::PackageDetails details;
                details.source.externalItemId = externalId;
                details.manifest.version = version;
                installSourcePackage(details, providerId, true);
            }
            if (pendingWidgetInstallKind_ ==
                PendingWidgetInstallKind::None)
                ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(_L("app.settings.cancel")))
        {
            pendingWidgetInstallKind_ = PendingWidgetInstallKind::None;
            pendingWidgetInstallPath_.clear();
            pendingWidgetInstallProviderId_.clear();
            pendingWidgetInstallExternalId_.clear();
            pendingWidgetInstallVersion_.clear();
            pendingWidgetInstallReason_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (!pendingWidgetPackageUninstall_.empty())
        ImGui::OpenPopup("##ConfirmWidgetPackageUninstall");
    if (ImGui::BeginPopupModal("##ConfirmWidgetPackageUninstall", nullptr,
        ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextWrapped("%s",
            _L("app.settings.widgets_uninstall_confirm"));
        if (BlueButton(_L("app.settings.widgets_uninstall")))
        {
            std::string error;
            const std::string packageId = pendingWidgetPackageUninstall_;
            if (widgetEngine_)
            {
                std::vector<std::wstring> instances;
                for (const auto& widget : widgetEngine_->GetWidgets())
                    if (widget.packageId == packageId)
                        instances.push_back(widget.widgetId);
                for (const auto& instance : instances)
                    widgetEngine_->UnloadWidget(instance);
            }
            if (WidgetEngine::UninstallWidgetPackage(packageId, error))
            {
                widgetPackageStatus_ =
                    _L("app.settings.widgets_uninstall_ok");
                pendingWidgetPackageUninstall_.clear();
                ImGui::CloseCurrentPopup();
                if (reloadCallback_) reloadCallback_();
            }
            else widgetPackageStatus_ = error;
        }
        ImGui::SameLine();
        if (ImGui::Button(_L("app.settings.cancel")))
        {
            pendingWidgetPackageUninstall_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    EndSettingsCard();
    ImGui::EndChild();
}

void SettingsWindow::DrawWidgetDeveloperTools()
{
    if (!ImGui::CollapsingHeader(
        _L("app.settings.widgets_developer_tools")))
        return;

    const auto packagePaths = WidgetEngine::GetWidgetPackagePaths();
    std::error_code createError;
    std::filesystem::create_directories(
        packagePaths.development, createError);
    if (SecondaryButton(
        _L("app.settings.widgets_open_development_folder")))
    {
        ShellExecuteW(nullptr, L"open",
            packagePaths.development.c_str(), nullptr, nullptr, SW_SHOW);
    }
    ImGui::SameLine();
    if (SecondaryButton(
        _L("app.settings.widgets_open_build_skill")))
    {
        const std::filesystem::path source =
            packagePaths.builtin / L"snowdesktop-lua-widget";
        std::filesystem::path target = source;
        std::error_code skillError;
        if (!std::filesystem::is_directory(source, skillError))
        {
            widgetPackageStatus_ =
                _L("app.settings.widgets_build_skill_missing");
        }
        else
        {
            if (snowdesktop::deployment::IsPackaged())
            {
                target = packagePaths.registry.parent_path() /
                    L"authoring" / L"snowdesktop-lua-widget";
                if (!CopyDirectoryContents(source, target))
                {
                    widgetPackageStatus_ =
                        _L("app.settings.widgets_build_skill_export_failed");
                    target.clear();
                }
            }
            if (!target.empty())
            {
                ShellExecuteW(nullptr, L"open", target.c_str(),
                    nullptr, nullptr, SW_SHOW);
            }
        }
    }
    ImGui::Spacing();

    if (widgetEngine_)
    {
        const std::string snapshotError = widgetEngine_->GetSystemSnapshotError();
        if (ImGui::CollapsingHeader(_L("app.settings.audio_devices")))
        {
            if (snapshotError.empty())
                ImGui::TextDisabled("%s", _L("app.settings.snapshot_service_ok"));
            else
                ImGui::TextWrapped(_L("app.settings.snapshot_recent_error"),
                    snapshotError.c_str());
        }
        ImGui::Spacing();
    }

    const auto drawIconGrid = [&](const char* titleKey,
        const char* hintKey, const char* notFoundKey,
        const char* countKey, const char* tooltipKey,
        const char* childId, const char* itemIdPrefix,
        ImFont* font, std::vector<unsigned int>& codepoints,
        std::initializer_list<std::pair<unsigned int, unsigned int>> ranges) {
        if (!DrawCollapsingHeaderWithHelp(_L(titleKey), _L(hintKey)))
            return;
        if (font && codepoints.empty())
        {
            for (const auto& [first, last] : ranges)
            {
                for (unsigned int codepoint = first;
                    codepoint <= last; ++codepoint)
                {
                    if (font->IsGlyphInFont(
                            static_cast<ImWchar>(codepoint)))
                        codepoints.push_back(codepoint);
                }
            }
        }
        if (!font || codepoints.empty())
        {
            ImGui::TextDisabled("%s", _L(notFoundKey));
            ImGui::Spacing();
            return;
        }

        ImGui::Text(_L(countKey), static_cast<int>(codepoints.size()));
        const float buttonSize = 38.0f * dpiScale_;
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const int columns = std::max(1, static_cast<int>(
            ImGui::GetContentRegionAvail().x / (buttonSize + spacing)));
        ImGui::BeginChild(childId, ImVec2(0, 220.0f * dpiScale_), true);
        const int rowCount = static_cast<int>(
            (codepoints.size() + static_cast<size_t>(columns) - 1) /
            static_cast<size_t>(columns));
        ImGuiListClipper clipper;
        clipper.Begin(rowCount,
            buttonSize + ImGui::GetStyle().ItemSpacing.y);
        while (clipper.Step())
        {
            for (int row = clipper.DisplayStart;
                row < clipper.DisplayEnd; ++row)
            {
                for (int column = 0; column < columns; ++column)
                {
                    const size_t i = static_cast<size_t>(
                        row * columns + column);
                    if (i >= codepoints.size())
                        break;
                    const unsigned int codepoint = codepoints[i];
                    const std::string glyph = CodepointToUtf8(codepoint);
                    const std::string buttonLabel = glyph + itemIdPrefix +
                        std::to_string(codepoint);
                    ImGui::PushFont(font, 18.0f * dpiScale_);
                    const bool clicked = ImGui::Button(buttonLabel.c_str(),
                        ImVec2(buttonSize, buttonSize));
                    ImGui::PopFont();
                    if (clicked)
                        ImGui::SetClipboardText(glyph.c_str());
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip(_L(tooltipKey), codepoint);
                    if (column + 1 < columns && i + 1 < codepoints.size())
                        ImGui::SameLine();
                }
            }
        }
        ImGui::EndChild();
        ImGui::Spacing();
    };

    drawIconGrid("app.settings.fluent_icons",
        "app.settings.fluent_icon_hint",
        "app.settings.fluent_not_found",
        "app.settings.fluent_valid_chars",
        "app.settings.fluent_copy_tooltip",
        "##FluentRegularGlyphs", "##fluent",
        fluentDebugFont_, fluentDebugCodepoints_,
        { { 0xE000, 0xF8FF }, { 0xF0000, 0xF0CCE } });
    drawIconGrid("app.settings.fa_icons",
        "app.settings.fa_icon_hint", "app.settings.fa_not_found",
        "app.settings.fa_valid_chars",
        "app.settings.fa_copy_tooltip",
        "##FontAwesomeGlyphs", "##fa",
        faDebugFont_, faDebugCodepoints_, { { 0xE000, 0xF8FF } });

    ImGui::Separator();

    std::vector<WidgetErrorEntry> errors;
    if (widgetEngine_)
        errors = widgetEngine_->GetWidgetErrors();
    ImGui::Text(_L("app.settings.error_count"), static_cast<int>(errors.size()));
    ImGui::SameLine();
    if (SecondaryButton(_L("app.settings.copy_all")))
    {
        std::string copyText;
        for (const auto& e : errors)
        {
            copyText += "[" + e.key + "]\n";
            copyText += e.message;
            copyText += "\n\n";
        }
        ImGui::SetClipboardText(copyText.c_str());
    }
    ImGui::SameLine();
    if (SecondaryButton(_L("app.settings.clear_all")))
    {
        if (widgetEngine_)
            widgetEngine_->ClearWidgetErrors();
        errors.clear();
    }

    ImGui::Spacing();

    if (errors.empty())
    {
        ImGui::TextDisabled("%s", _L("app.settings.no_widget_errors"));
        ImGui::Spacing();
    }
    else
    {
        ImGui::BeginChild("##DebugScroll", ImVec2(0, 160.0f * dpiScale_), true, ImGuiWindowFlags_HorizontalScrollbar);
        for (const auto& e : errors)
        {
            std::string itemText = "[" + e.key + "]\n" + e.message;
            if (ImGui::Selectable(itemText.c_str(), false, ImGuiSelectableFlags_SpanAllColumns, ImVec2(0, 0)))
                ImGui::SetClipboardText(itemText.c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _L("app.settings.copy_error"));
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
        }
        ImGui::EndChild();
        ImGui::Spacing();
    }

    ImGui::Separator();
    ImGui::Text("%s", _L("app.settings.widget_diagnostic"));

    std::vector<WidgetDiagnosticEntry> diagnostics;
    if (widgetEngine_)
        diagnostics = widgetEngine_->GetWidgetDiagnostics();

    if (diagnostics.empty())
    {
        ImGui::TextDisabled("%s", _L("app.settings.no_widgets_loaded"));
    }
    else
    {
        if (SecondaryButton(_L("app.settings.copy_diag")))
        {
            std::string text;
            for (const auto& d : diagnostics)
            {
                text += "[" + WideToUtf8(d.widgetId) + "] " + d.name + "\n";
                text += std::string("valid=") + (d.valid ? "true" : "false") +
                    ", manifest=" + (d.hasManifest ? "true" : "false") + "\n";
                text += "permissions=";
                for (size_t i = 0; i < d.permissions.size(); ++i)
                {
                    if (i > 0) text += ",";
                    text += d.permissions[i];
                }
                text += "\n";
                if (!d.lastError.empty())
                    text += "lastError=" + d.lastError + "\n";
                for (const auto& log : d.logs)
                    text += log.level + ": " + log.message + "\n";
                text += "\n";
            }
            ImGui::SetClipboardText(text.c_str());
        }

        ImGui::BeginChild("##WidgetDiagnostics", ImVec2(0, 180.0f * dpiScale_), true, ImGuiWindowFlags_HorizontalScrollbar);
        for (auto& d : diagnostics)
        {
            std::string header = "[" + WideToUtf8(d.widgetId) + "] " + d.name;
            if (ImGui::CollapsingHeader(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGui::Text(_L("app.settings.debug_script"),
                    WideToUtf8(d.scriptPath).c_str());
                ImGui::Text(_L("app.settings.debug_status_manifest"),
                    d.valid ? _L("app.settings.valid") : _L("app.settings.invalid"),
                    d.hasManifest ? _L("app.settings.yes") : _L("app.settings.no"));
                std::string perms;
                for (size_t i = 0; i < d.permissions.size(); ++i)
                {
                    if (i > 0) perms += ", ";
                    perms += d.permissions[i];
                }
                ImGui::Text(_L("app.settings.debug_permissions"),
                    perms.empty() ? _L("app.settings.none") : perms.c_str());
                if (!d.lastError.empty())
                    ImGui::TextWrapped(_L("app.settings.debug_last_error"),
                        d.lastError.c_str());
                if (SecondaryButton((std::string(_L("app.settings.reload")) + "##" +
                    WideToUtf8(d.widgetId)).c_str(), ImVec2(96, 0)))
                {
                    if (widgetEngine_)
                        widgetEngine_->ReloadWidget(d.widgetId);
                    if (invalidateCallback_)
                        invalidateCallback_();
                }
                if (!d.logs.empty())
                {
                    ImGui::Text("%s", _L("app.settings.recent_logs"));
                    for (const auto& log : d.logs)
                        ImGui::TextWrapped("[%s] %s", log.level.c_str(), log.message.c_str());
                }
            }
            ImGui::Separator();
        }
        ImGui::EndChild();
    }
}

void SettingsWindow::DrawDebugPage()
{
    const float pad = 16.0f * dpiScale_;
    ImVec2 pageSize = ImGui::GetContentRegionAvail();
    pageSize.x = std::max(1.0f, pageSize.x);
    pageSize.y = std::max(1.0f, pageSize.y);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
    ImGui::BeginChild("##DebugPageInner", pageSize,
        ImGuiChildFlags_Borders |
            ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();

    ImGui::Text("%s", _L("app.settings.debug_page"));
    ImGui::Separator();
    ImGui::Spacing();
    if (DrawSettingCheckbox(
            _L("app.settings.animation_diagnostics"),
            "##AnimationDiagnostics",
            &animationDiagnosticsEnabled_))
    {
        if (animationDiagnosticsToggleCallback_)
        {
            animationDiagnosticsToggleCallback_(
                animationDiagnosticsEnabled_);
        }
    }
    ImGui::TextWrapped(
        "%s", _L("app.settings.animation_diagnostics_desc"));
    if (animationDiagnosticsEnabled_ &&
        animationDiagnosticsProvider_)
    {
        const std::wstring status =
            animationDiagnosticsProvider_();
        if (!status.empty())
        {
            const std::string utf8 = WideToUtf8(status);
            ImGui::TextWrapped("%s", utf8.c_str());
        }
    }
    ImGui::Spacing();
    if (DrawCollapsingHeaderWithHelp(
        _L("app.settings.crash_test"),
        _L("app.settings.crash_test_desc")))
    {
        ImGui::Spacing();
        if (BlueButton(_L("app.settings.trigger_crash")))
            TriggerCrashForTesting();
        ImGui::Spacing();
    }
    ImGui::EndChild();
}

/**
 * @brief 绘制"关于"页面。
 *
 * 显示应用简介、作者信息、社交主页链接（Bilibili / GitHub / 抖音 / 小红书）。
 * 版本号支持彩蛋点击 —— 连续点击 5 次可解锁调试页面（debugUnlocked_）。
 */
void SettingsWindow::PerformUpdateCheck()
{
    if (updateCheckRequestId_ != 0)
        return;

    updateCheckStatus_ = "checking";
    updateCheckStatusKey_.clear();
    updateCheckStatusArgument_.clear();
    updateAvailable_ = false;
    latestVersion_.clear();
    downloadUrl_.clear();

    if (snowdesktop::deployment::IsPackaged())
    {
        const std::wstring storeUri =
            snowdesktop::deployment::GetStoreProductPageUri();
        if (!storeUri.empty())
        {
            ShellExecuteW(nullptr, L"open", storeUri.c_str(),
                nullptr, nullptr, SW_SHOW);
        }
        updateCheckStatusKey_ =
            L10N_KEY("app.settings.store_managed_updates");
        updateCheckStatus_ =
            _L("app.settings.store_managed_updates");
        return;
    }

    if (!updateHttpService_)
        updateHttpService_ = std::make_unique<AsyncHttpService>();

    HttpRequestOptions options;
    options.widgetId = L"SnowDesktop.UpdateCheck";
    options.url =
        L"https://api.github.com/repos/FreeFallingSnow/"
        L"SnowDesktop_Release/releases/latest";
    options.headers =
        L"Accept: application/vnd.github+json\r\n"
        L"X-GitHub-Api-Version: 2022-11-28\r\n";
    options.timeoutMs = 8000;
    options.allowedDomains = { "api.github.com" };
    updateCheckRequestId_ = updateHttpService_->Submit(std::move(options));
    if (updateCheckRequestId_ == 0)
    {
        updateCheckStatusKey_ = L10N_KEY("app.settings.update_http_init_failed");
        updateCheckStatus_ = _L("app.settings.update_http_init_failed");
    }
}

void SettingsWindow::PollUpdateCheck()
{
    if (!updateHttpService_ || updateCheckRequestId_ == 0)
        return;

    for (HttpResponse& response : updateHttpService_->Drain())
    {
        if (response.id != updateCheckRequestId_)
            continue;

        updateCheckRequestId_ = 0;
        if (!response.error.empty() || response.status < 200 ||
            response.status >= 300)
        {
            if (response.error.find("WinHttpOpen") != std::string::npos)
                updateCheckStatusKey_ =
                    L10N_KEY("app.settings.update_http_init_failed");
            else if (response.error.find("WinHttpConnect") !=
                std::string::npos)
                updateCheckStatusKey_ =
                    L10N_KEY("app.settings.update_connect_failed");
            else if (response.error.find("Invalid URL") !=
                std::string::npos)
                updateCheckStatusKey_ =
                    L10N_KEY("app.settings.update_url_parse_failed");
            else
                updateCheckStatusKey_ =
                    L10N_KEY("app.settings.update_receive_failed");
            updateCheckStatus_ =
                Locale::Instance().Tr(updateCheckStatusKey_.c_str());
            return;
        }

        if (response.body.empty())
        {
            updateCheckStatusKey_ =
                L10N_KEY("app.settings.update_empty_response");
            updateCheckStatus_ =
                _L("app.settings.update_empty_response");
            return;
        }

        auto extractJsonString = [](const std::string& json,
                                     const char* field) -> std::string {
            const std::string key = "\"" + std::string(field) + "\"";
            size_t pos = json.find(key);
            if (pos == std::string::npos) return {};
            pos += key.size();
            pos = json.find(':', pos);
            if (pos == std::string::npos) return {};
            ++pos;
            while (pos < json.size() &&
                (json[pos] == ' ' || json[pos] == '\t' ||
                 json[pos] == '\r' || json[pos] == '\n'))
                ++pos;
            if (pos >= json.size() || json[pos] != '"') return {};
            ++pos;
            size_t end = json.find('"', pos);
            if (end == std::string::npos) return {};
            return json.substr(pos, end - pos);
        };

        std::string tag =
            extractJsonString(response.body, "tag_name");
        if (tag.empty())
        {
            updateCheckStatusKey_ =
                L10N_KEY("app.settings.update_parse_failed");
            updateCheckStatus_ =
                _L("app.settings.update_parse_failed");
            return;
        }

        if (tag[0] == 'v' || tag[0] == 'V')
            tag.erase(0, 1);

        const std::string htmlUrl =
            extractJsonString(response.body, "html_url");

        auto compareVersion = [](const std::string& a,
                                  const std::string& b) -> int {
            std::istringstream sa(a), sb(b);
            std::string pa, pb;
            for (int i = 0; i < 4; ++i)
            {
                int va = 0, vb = 0;
                if (std::getline(sa, pa, '.')) va = std::atoi(pa.c_str());
                if (std::getline(sb, pb, '.')) vb = std::atoi(pb.c_str());
                if (va != vb) return va < vb ? -1 : 1;
            }
            return 0;
        };

        latestVersion_ = tag;
        downloadUrl_ = htmlUrl;

        if (compareVersion(SNOWDESKTOP_VERSION, tag) >= 0)
        {
            updateAvailable_ = false;
            updateCheckStatusKey_ =
                L10N_KEY("app.settings.already_latest");
            updateCheckStatusArgument_.clear();
            updateCheckStatus_ =
                _L("app.settings.already_latest");
        }
        else
        {
            updateAvailable_ = true;
            updateCheckStatusKey_ =
                L10N_KEY("app.settings.new_version");
            updateCheckStatusArgument_ = tag;
            updateCheckStatus_ =
                _LF("app.settings.new_version", tag);
        }
        return;
    }
}

void SettingsWindow::DrawAboutPage()
{
    BeginPage();

    BeginSettingsCard(_L("app.settings.about_snowdesktop"));

    ImGui::TextWrapped("%s", _L("app.settings.about_description"));

    ImGui::Spacing();
    ImGui::SeparatorText(_L("app.settings.author"));
    ImGui::Spacing();
    ImGui::Text("    逍遥飘雪（郭云哲）"); // l10n-allow: author name is intentionally fixed
    ImGui::Spacing();

    ImGui::SeparatorText(_L("app.settings.copyright"));
    ImGui::Spacing();
    ImGui::TextWrapped("    %s", _L("app.settings.copyright_notice"));
    ImGui::TextWrapped("    %s", _L("app.settings.license_notice"));
    ImGui::Spacing();

    auto LinkButton = [](const char* label, const char* url) {
        ImGui::Text("    ");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.30f, 0.60f, 0.95f, 1.00f), label);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ImGui::SetTooltip("%s", url);
        }
        if (ImGui::IsItemClicked())
        {
            ShellExecuteW(nullptr, L"open", Utf8ToWide(url).c_str(), nullptr, nullptr, SW_SHOW);
        }
    };

    ImGui::SeparatorText(_L("app.settings.personal_homepages"));
    ImGui::Spacing();
    LinkButton("Bilibili", "https://space.bilibili.com/32837853");
    ImGui::Dummy(ImVec2(0, 2));
    LinkButton("GitHub", "https://github.com/FreeFallingSnow/");
    ImGui::Dummy(ImVec2(0, 2));
    LinkButton(_L("app.settings.douyin"), "https://www.douyin.com/user/MS4wLjABAAAA-O94bwF3BK2sj9JOwM2R2zRlTOiYf4BbaSyIF9DZPyM");
    ImGui::Dummy(ImVec2(0, 2));
    LinkButton(_L("app.settings.xiaohongshu"), "https://www.xiaohongshu.com/user/profile/6819eed7000000000403bf0e");

    ImGui::Spacing();
    ImGui::SeparatorText(_L("app.settings.project_url"));
    ImGui::Spacing();
    LinkButton("GitHub (Release)", "https://github.com/FreeFallingSnow/SnowDesktop_Release");
    ImGui::Dummy(ImVec2(0, 2));
    LinkButton("GitHub (Source)", "https://github.com/FreeFallingSnow/SnowDesktop");

    ImGui::Spacing();
    ImGui::SeparatorText(_L("app.settings.community"));
    ImGui::Spacing();
    LinkButton(_L("app.settings.join_qq"), "https://qm.qq.com/q/HyazkCIRig");

    ImGui::Spacing();
    ImGui::SeparatorText(_L("app.settings.version"));
    ImGui::TextDisabled("SnowDesktop v" SNOWDESKTOP_VERSION);
    if (ImGui::IsItemClicked())
    {
        if (!debugUnlocked_)
        {
            ++versionClickCount_;
            if (versionClickCount_ >= 5)
            {
                debugUnlocked_ = true;
                activePage_ = 10;
            }
        }
    }

    if (snowdesktop::deployment::IsPackaged())
    {
        ImGui::SameLine();
        if (!updateCheckStatus_.empty())
        {
            if (updateCheckStatus_ == "checking")
            {
                ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%s", _L("app.settings.checking"));
            }
            else if (updateAvailable_)
            {
                ImGui::TextColored(ImVec4(0.30f, 0.85f, 0.40f, 1.0f), "%s", updateCheckStatus_.c_str());
            }
            else
            {
                ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%s", updateCheckStatus_.c_str());
            }
        }

        float updateButtonW = SettingButtonWidth(_L("app.settings.check_update")) + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - updateButtonW);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.13f, 0.45f, 0.90f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.18f, 0.55f, 1.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.10f, 0.35f, 0.75f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
        ImGui::BeginDisabled(updateCheckRequestId_ != 0);
        if (ImGui::Button(_L("app.settings.check_update"), ImVec2(updateButtonW, 0)))
        {
            PerformUpdateCheck();
        }
        ImGui::EndDisabled();
        ImGui::PopStyleColor(4);
    }

    ImGui::Spacing();
    ImGui::SeparatorText(_L("app.settings.third_party_libs"));
    ImGui::Spacing();

    LinkButton("Everything SDK", "https://www.voidtools.com/support/everything/sdk/");
    ImGui::SameLine();
    ImGui::TextDisabled("(MIT)");
    ImGui::TextDisabled("        Copyright (C) 2016 David Carpenter");

    LinkButton("Dear ImGui", "https://github.com/ocornut/imgui");
    ImGui::SameLine();
    ImGui::TextDisabled("(MIT)");
    ImGui::TextDisabled("        Copyright (c) 2014-2025 Omar Cornut");

    LinkButton("Lua", "https://www.lua.org/");
    ImGui::SameLine();
    ImGui::TextDisabled("(MIT)");
    ImGui::TextDisabled("        Copyright (C) 1994-2024 Lua.org, PUC-Rio");

    LinkButton("spdlog", "https://github.com/gabime/spdlog");
    ImGui::SameLine();
    ImGui::TextDisabled("(MIT)");
    ImGui::TextDisabled("        Copyright (c) 2016-present, Gabi Melman");

    LinkButton("pinyin-data", "https://github.com/mozillazg/pinyin-data");
    ImGui::SameLine();
    ImGui::TextDisabled("(MIT)");
    ImGui::TextDisabled("        Copyright (c) 2016 mozillazg");

    ImGui::Spacing();
    ImGui::SeparatorText(_L("app.settings.reference_programs"));
    ImGui::Spacing();

    LinkButton("TranslucentTB (modified portions)",
        "https://github.com/TranslucentTB/TranslucentTB/tree/322e2b7395a51975150126276308b415970e080b");
    ImGui::SameLine();
    ImGui::TextDisabled("(GPL-3.0-only)");
    ImGui::TextDisabled("        Copyright (c) TranslucentTB contributors");
    ImGui::TextDisabled("        Modified for SnowDesktop from upstream commit 322e2b7");

    EndSettingsCard();
    EndPage();
}

// ════════════════════════════════════════════════════════════════
//  布局备份、完整数据备份与数据迁移
// ════════════════════════════════════════════════════════════════

/**
 * @brief 清除当前设置页的待写状态并重启，以应用已排队的数据替换。
 */
void SettingsWindow::RestartAfterDataReplacement(
    const char* successMessageKey)
{
    // Prevent the current settings frame from writing old values over the
    // replacement data while the application is restarting.
    personalizationDirty_ = false;
    personalizationPreviewDirty_ = false;
    personalizationSaveRequested_ = false;
    dockSettingsDirty_ = false;
    dockSettingsPreviewDirty_ = false;
    dockSettingsSaveRequested_ = false;
    navigationSettingsDirty_ = false;
    generalSettingsDirty_ = false;
    categorySettingsDirty_ = false;
    categorySettingsSaveRequested_ = false;

    MessageBoxW(hwnd_, _LW(successMessageKey),
        _LW("app.settings.full_data_backups"),
        MB_OK | MB_ICONINFORMATION);
    if (restartCallback_)
        restartCallback_();
}

void SettingsWindow::CreateFullDataBackup()
{
    auto manager = MakeFullBackupManager();
    const auto result = manager.Create();
    if (!result.ok)
    {
        std::wstring message =
            _LW("app.settings.create_full_backup_failed");
        if (!result.error.empty())
            message += L"\n\n" + Utf8ToWide(result.error);
        MessageBoxW(hwnd_, message.c_str(),
            _LW("app.settings.full_data_backups"),
            MB_OK | MB_ICONERROR);
        return;
    }
    MessageBoxW(hwnd_,
        _LW("app.settings.create_full_backup_success"),
        _LW("app.settings.full_data_backups"),
        MB_OK | MB_ICONINFORMATION);
}

void SettingsWindow::ExportFullDataBackup(
    const snowdesktop::backup::BackupInfo& backup)
{
    const COMDLG_FILTERSPEC filters[] = {
        {
            _LW("app.settings.snowbackup_file_type"),
            L"*.snowbackup",
        },
        {
            _LW("app.settings.zip_file_type"),
            L"*.zip",
        },
    };
    std::wstring defaultName = L"SnowDesktop-";
    defaultName += backup.id.empty() ? L"Backup" : backup.id;
    defaultName += L".snowbackup";
    const auto selected = SaveSettingsFile(hwnd_,
        _LW("app.settings.export_backup"),
        defaultName.c_str(), L"snowbackup",
        filters, static_cast<UINT>(std::size(filters)));
    if (!selected)
        return;

    const auto result =
        MakeFullBackupManager().Export(backup, *selected);
    if (!result.ok)
    {
        std::wstring message =
            _LW("app.settings.export_backup_failed");
        if (!result.error.empty())
            message += L"\n\n" + Utf8ToWide(result.error);
        MessageBoxW(hwnd_, message.c_str(),
            _LW("app.settings.full_data_backups"),
            MB_OK | MB_ICONERROR);
        return;
    }
    MessageBoxW(hwnd_,
        _LW("app.settings.export_backup_success"),
        _LW("app.settings.full_data_backups"),
        MB_OK | MB_ICONINFORMATION);
}

void SettingsWindow::ImportFullDataBackup()
{
    const COMDLG_FILTERSPEC filters[] = {
        {
            _LW("app.settings.backup_archive_file_type"),
            L"*.snowbackup;*.zip",
        },
    };
    const auto selected = PickSettingsFile(hwnd_,
        _LW("app.settings.restore_from_backup_file"),
        filters, static_cast<UINT>(std::size(filters)));
    if (!selected)
        return;
    if (MessageBoxW(hwnd_,
            _LW("app.settings.restore_backup_file_confirm"),
            _LW("app.settings.full_data_backups"),
            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
    {
        return;
    }

    const auto result =
        MakeFullBackupManager().ImportAndQueue(*selected);
    if (!result.ok)
    {
        std::wstring message =
            _LW("app.settings.restore_backup_file_failed");
        if (!result.error.empty())
            message += L"\n\n" + Utf8ToWide(result.error);
        MessageBoxW(hwnd_, message.c_str(),
            _LW("app.settings.full_data_backups"),
            MB_OK | MB_ICONERROR);
        return;
    }
    RestartAfterDataReplacement(
        L10N_KEY("app.settings.restore_backup_file_success"));
}

void SettingsWindow::RestoreFullDataBackup(
    const snowdesktop::backup::BackupInfo& backup)
{
    if (MessageBoxW(hwnd_,
            _LW("app.settings.restore_full_backup_confirm"),
            _LW("app.settings.full_data_backups"),
            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
    {
        return;
    }
    const auto result =
        MakeFullBackupManager().QueueRestore(backup);
    if (!result.ok)
    {
        std::wstring message =
            _LW("app.settings.restore_full_backup_failed");
        if (!result.error.empty())
            message += L"\n\n" + Utf8ToWide(result.error);
        MessageBoxW(hwnd_, message.c_str(),
            _LW("app.settings.full_data_backups"),
            MB_OK | MB_ICONERROR);
        return;
    }
    RestartAfterDataReplacement(
        L10N_KEY("app.settings.restore_full_backup_success"));
}

void SettingsWindow::DeleteFullDataBackup(
    const snowdesktop::backup::BackupInfo& backup)
{
    if (MessageBoxW(hwnd_,
            _LW("app.settings.delete_full_backup_confirm"),
            _LW("app.settings.full_data_backups"),
            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
    {
        return;
    }
    const auto result =
        MakeFullBackupManager().Delete(backup);
    if (result.ok)
        return;

    std::wstring message =
        _LW("app.settings.delete_full_backup_failed");
    if (!result.error.empty())
        message += L"\n\n" + Utf8ToWide(result.error);
    MessageBoxW(hwnd_, message.c_str(),
        _LW("app.settings.full_data_backups"),
        MB_OK | MB_ICONERROR);
}

/**
 * @brief 从用户选择的其他 SnowDesktop 目录迁入全部数据。
 *
 * 用户既可选择 SnowDesktop 程序目录，也可直接选择其中的 data 目录。迁移
 * 时先把来源完整复制到当前状态目录的暂存目录，发布待处理事务后重启。
 * 新进程会在打开任何数据文件前将当前 data 原子备份并换入暂存数据。
 */
void SettingsWindow::MigrateAllData()
{
    const wchar_t* title = _LW("app.settings.data_migration");

    ComPtr<IFileOpenDialog> dialog;
    HRESULT result = CoCreateInstance(CLSID_FileOpenDialog, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(result))
    {
        MessageBoxW(hwnd_, _LW("app.settings.migrate_data_failed"),
            title, MB_OK | MB_ICONERROR);
        return;
    }

    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options)))
    {
        dialog->SetOptions(options | FOS_PICKFOLDERS |
            FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    }
    dialog->SetTitle(_LW("app.settings.select_migration_data"));

    result = dialog->Show(hwnd_);
    if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED))
        return;
    if (FAILED(result))
    {
        MessageBoxW(hwnd_, _LW("app.settings.migrate_data_failed"),
            title, MB_OK | MB_ICONERROR);
        return;
    }

    ComPtr<IShellItem> selectedItem;
    if (FAILED(dialog->GetResult(&selectedItem)) || !selectedItem)
    {
        MessageBoxW(hwnd_, _LW("app.settings.migrate_data_failed"),
            title, MB_OK | MB_ICONERROR);
        return;
    }

    PWSTR selectedPath = nullptr;
    if (FAILED(selectedItem->GetDisplayName(
            SIGDN_FILESYSPATH, &selectedPath)) || !selectedPath)
    {
        if (selectedPath)
            CoTaskMemFree(selectedPath);
        MessageBoxW(hwnd_, _LW("app.settings.migrate_data_failed"),
            title, MB_OK | MB_ICONERROR);
        return;
    }

    std::filesystem::path selectedDirectory(selectedPath);
    CoTaskMemFree(selectedPath);

    std::filesystem::path sourceData = selectedDirectory;
    std::filesystem::path sourceLegacyWidgets;
    const std::filesystem::path nestedData =
        selectedDirectory / L"data";
    if (LooksLikeSnowDesktopDataDirectory(nestedData))
    {
        sourceData = nestedData;
        sourceLegacyWidgets = selectedDirectory / L"widgets";
    }
    else if (_wcsicmp(
                 selectedDirectory.filename().c_str(), L"data") == 0)
    {
        sourceLegacyWidgets =
            selectedDirectory.parent_path() / L"widgets";
    }

    std::error_code sourceWidgetsError;
    if (!std::filesystem::is_directory(
            sourceLegacyWidgets, sourceWidgetsError))
    {
        sourceLegacyWidgets.clear();
    }

    const std::filesystem::path targetData(GetDataDirectoryPath());
    if (!LooksLikeSnowDesktopDataDirectory(sourceData) ||
        PathsOverlap(sourceData, targetData) ||
        (!sourceLegacyWidgets.empty() &&
            PathsOverlap(sourceLegacyWidgets, targetData)))
    {
        MessageBoxW(hwnd_, _LW("app.settings.migrate_data_invalid"),
            title, MB_OK | MB_ICONWARNING);
        return;
    }

    if (MessageBoxW(hwnd_, _LW("app.settings.migrate_data_confirm"),
            title, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
    {
        return;
    }

    std::filesystem::path stateRoot =
        snowdesktop::deployment::GetPackageLocalStatePath();
    if (stateRoot.empty())
        stateRoot = targetData.parent_path();

    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t token[96]{};
    swprintf_s(token, L"%04u%02u%02u-%02u%02u%02u-%lu-%llu",
        now.wYear, now.wMonth, now.wDay,
        now.wHour, now.wMinute, now.wSecond,
        GetCurrentProcessId(),
        static_cast<unsigned long long>(GetTickCount64()));

    const std::filesystem::path migrationRoot =
        stateRoot / L"TempState" / L"PortableMigration";
    const std::filesystem::path stagingData =
        migrationRoot / (std::wstring(L"staging-") + token);

    const auto copyResult =
        snowdesktop::migration::CopyDataTree(sourceData, stagingData);
    bool staged = copyResult.ok;
    std::string stageError = copyResult.error;
    if (staged && !sourceLegacyWidgets.empty())
    {
        // Current folder packages and authoring tools beside the executable
        // are application files, not user data. Only old loose pairs need to
        // enter the writable migration root; built-ins are rebound to the
        // installed copy and user-authored pairs remain explicit migrations.
        const auto legacyImport =
            snowdesktop::widget::ImportLegacyLooseWidgetPairs(
                sourceLegacyWidgets, stagingData / L"widgets");
        staged = legacyImport.ok;
        if (!staged)
        {
            stageError = legacyImport.error.empty()
                ? "legacy component import failed"
                : legacyImport.error;
            OutputDebugStringA(("SnowDesktop: legacy component "
                "import failed: " + stageError + "\n").c_str());
        }
    }
    if (!staged ||
        !LooksLikeSnowDesktopDataDirectory(stagingData))
    {
        if (staged && stageError.empty())
            stageError = "staged data validation failed";
        OutputDebugStringA(("SnowDesktop: data migration staging failed: " +
            stageError + "\n").c_str());
        std::error_code cleanupError;
        std::filesystem::remove_all(stagingData, cleanupError);
        std::wstring message =
            _LW("app.settings.migrate_data_failed");
        if (!stageError.empty())
            message += L"\n\n" + Utf8ToWide(stageError);
        MessageBoxW(hwnd_, message.c_str(), title,
            MB_OK | MB_ICONERROR);
        return;
    }

    std::string queueError;
    if (!snowdesktop::migration::Queue(
            stateRoot, token, queueError))
    {
        OutputDebugStringA(("SnowDesktop: cannot queue data "
            "migration: " + queueError + "\n").c_str());
        std::error_code cleanupError;
        std::filesystem::remove_all(stagingData, cleanupError);
        std::wstring message =
            _LW("app.settings.migrate_data_failed");
        message += L"\n\n" + Utf8ToWide(queueError);
        MessageBoxW(hwnd_, message.c_str(), title,
            MB_OK | MB_ICONERROR);
        return;
    }

    RestartAfterDataReplacement(
        L10N_KEY("app.settings.migrate_data_success"));
}

/**
 * @brief 获取备份文件存储目录路径。
 *
 * 目录位于当前部署数据目录下的 backups 子文件夹。
 * @return 备份目录的完整宽字符串路径
 */
std::wstring SettingsWindow::GetBackupDir() const
{
    return GetDataSubdirectoryPath(L"backups");
}

/**
 * @brief 列举所有已有备份。
 *
 * 扫描备份目录下所有 *.json 文件，解析文件名和最后写入时间，
 * 组装为 LayoutBackup 条目并按照时间倒序（最新在前）排序。
 * @return 备份条目列表，可能为空
 */
std::vector<LayoutBackup> SettingsWindow::ListBackups() const
{
    std::vector<LayoutBackup> result;
    std::wstring dir = GetBackupDir();
    std::wstring search = dir + L"\\*.json";

    WIN32_FIND_DATAW fd{};
    HANDLE hFind = FindFirstFileW(search.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return result;

    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

        std::wstring filename = fd.cFileName;
        // Skip storage companion files
        if (filename.size() > 13 && filename.substr(filename.size() - 13) == L".storage.json")
            continue;

        LayoutBackup b;
        b.filename = filename;
        b.timestamp = fd.ftLastWriteTime;

        // Parse display name from filename: remove .json and format timestamp
        std::wstring name = filename;
        if (name.size() > 5 && name.substr(name.size() - 5) == L".json")
            name = name.substr(0, name.size() - 5);

        SYSTEMTIME st;
        FileTimeToSystemTime(&fd.ftLastWriteTime, &st);
        wchar_t timeStr[64]{};
        swprintf_s(timeStr, L"%04d-%02d-%02d %02d:%02d",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);

        b.displayName = name + L"  [" + timeStr + L"]";
        result.push_back(b);
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);

    // Sort by timestamp descending (newest first)
    std::sort(result.begin(), result.end(), [](const LayoutBackup& a, const LayoutBackup& b) {
        return CompareFileTime(&a.timestamp, &b.timestamp) > 0;
    });

    return result;
}

/**
 * @brief 保存当前布局文件到备份目录。
 *
 * 将 data\SnowDesktop.layout.json 复制到 data\backups\ 下，
 * 备份文件名中不允许出现 : / \\ 字符（替换为 _），
 * 同名文件存在时自动在末尾追加递增序号。
 * @param name 备份名称
 * @return true 复制成功
 */
bool SettingsWindow::SaveBackup(const std::wstring& name)
{
    std::wstring backupDir = GetBackupDir();
    CreateDirectoryW(backupDir.c_str(), nullptr);

    std::wstring layoutPath = GetDataFilePath(L"SnowDesktop.layout.json");
    std::wstring storagePath = GetDataFilePath(L"SnowDesktop.storage.json");

    // Sanitize: remove colons for filename safety
    std::wstring safeName = name;
    for (auto& c : safeName) { if (c == L':' || c == L'\\' || c == L'/') c = L'_'; }

    std::wstring backupLayout = backupDir + L"\\" + safeName + L".json";
    std::wstring backupStorage = backupDir + L"\\" + safeName + L".storage.json";

    // Find existing file with same name, increment count
    int counter = 1;
    while (GetFileAttributesW(backupLayout.c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        backupLayout = backupDir + L"\\" + safeName + L"(" + std::to_wstring(counter) + L").json";
        backupStorage = backupDir + L"\\" + safeName + L"(" + std::to_wstring(counter) + L").storage.json";
        ++counter;
    }

    bool ok = CopyFileW(layoutPath.c_str(), backupLayout.c_str(), FALSE) != FALSE;
    if (GetFileAttributesW(storagePath.c_str()) != INVALID_FILE_ATTRIBUTES)
        CopyFileW(storagePath.c_str(), backupStorage.c_str(), FALSE);
    return ok;
}

/**
 * @brief 基于当前系统时间生成备份文件名（含年月日时分秒）。
 * @return 格式为 "YYYY-MM-DD hh-mm-ss" 的宽字符串
 */
std::wstring SettingsWindow::MakeBackupTimestampName() const
{
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t name[64]{};
    swprintf_s(name, L"%04d-%02d-%02d %02d-%02d-%02d",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return name;
}

/**
 * @brief 从备份文件恢复布局。
 *
 * 恢复前自动调用 SaveBackup() 生成一份"恢复前备份"快照。
 * @param filename 备份文件名（仅文件名，不含路径）
 * @return true 复制成功
 */
bool SettingsWindow::RestoreBackup(const std::wstring& filename)
{
    std::wstring layoutPath = GetDataFilePath(L"SnowDesktop.layout.json");
    std::wstring storagePath = GetDataFilePath(L"SnowDesktop.storage.json");

    std::wstring backupPath = GetBackupDir() + L"\\" + filename;

    // Derive storage backup filename: replace .json with .storage.json
    std::wstring storageFilename = filename;
    if (storageFilename.size() > 5 && storageFilename.substr(storageFilename.size() - 5) == L".json")
        storageFilename = storageFilename.substr(0, storageFilename.size() - 5) + L".storage.json";
    std::wstring storageBackupPath = GetBackupDir() + L"\\" + storageFilename;

    // First save current layout before restoring.
    SaveBackup(MakeBackupTimestampName() +
        _LW("app.settings.backup_before_restore_suffix"));

    bool ok = CopyFileW(backupPath.c_str(), layoutPath.c_str(), FALSE) != FALSE;
    if (GetFileAttributesW(storageBackupPath.c_str()) != INVALID_FILE_ATTRIBUTES)
        CopyFileW(storageBackupPath.c_str(), storagePath.c_str(), FALSE);
    return ok;
}

/**
 * @brief 删除指定的备份文件。
 * @param filename 要删除的备份文件名
 * @return true 删除成功
 */
bool SettingsWindow::DeleteBackup(const std::wstring& filename)
{
    std::wstring backupPath = GetBackupDir() + L"\\" + filename;

    std::wstring storageFilename = filename;
    if (storageFilename.size() > 5 && storageFilename.substr(storageFilename.size() - 5) == L".json")
        storageFilename = storageFilename.substr(0, storageFilename.size() - 5) + L".storage.json";
    std::wstring storageBackupPath = GetBackupDir() + L"\\" + storageFilename;
    if (GetFileAttributesW(storageBackupPath.c_str()) != INVALID_FILE_ATTRIBUTES)
        DeleteFileW(storageBackupPath.c_str());

    return DeleteFileW(backupPath.c_str()) != FALSE;
}

// ════════════════════════════════════════════════════════════════
//  交换链：创建与清理
// ════════════════════════════════════════════════════════════════

/**
 * @brief 创建 DirectX 交换链及渲染目标视图。
 *
 * 根据窗口当前客户区尺寸调整已有交换链，
 * 仅在尚未创建或调整失败时重新创建，并同步 ImGui DisplaySize。
 * @return true 创建成功
 */
bool SettingsWindow::CreateSwapChain()
{
    RECT cr;
    GetClientRect(hwnd_, &cr);
    windowWidth_ = (cr.right - cr.left > 1) ? (cr.right - cr.left) : 1;
    windowHeight_ = (cr.bottom - cr.top > 1) ? (cr.bottom - cr.top) : 1;

    if (!context_)
        device_->GetImmediateContext(&context_);
    context_->OMSetRenderTargets(0, nullptr, nullptr);
    rtv_.Reset();

    if (swapChain_)
    {
        const HRESULT resizeResult = swapChain_->ResizeBuffers(0,
            static_cast<UINT>(windowWidth_), static_cast<UINT>(windowHeight_),
            DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(resizeResult))
            swapChain_.Reset();
    }

    if (!swapChain_)
    {
        ComPtr<IDXGIDevice> dxgiDevice;
        if (FAILED(device_.As(&dxgiDevice))) return false;
        ComPtr<IDXGIAdapter> adapter;
        if (FAILED(dxgiDevice->GetAdapter(&adapter))) return false;
        ComPtr<IDXGIFactory2> factory;
        if (FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) return false;

        DXGI_SWAP_CHAIN_DESC1 desc = {};
        desc.Width = static_cast<UINT>(windowWidth_);
        desc.Height = static_cast<UINT>(windowHeight_);
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

        if (FAILED(factory->CreateSwapChainForHwnd(device_.Get(), hwnd_,
                &desc, nullptr, nullptr, &swapChain_)))
            return false;
    }

    ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) return false;
    if (FAILED(device_->CreateRenderTargetView(backBuffer.Get(), nullptr, &rtv_))) return false;

    if (ImGui::GetCurrentContext() != nullptr)
    {
        auto& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(static_cast<float>(windowWidth_), static_cast<float>(windowHeight_));
    }
    return true;
}

/**
 * @brief 释放交换链和渲染目标视图的 COM 资源。
 */
void SettingsWindow::CleanupSwapChain()
{
    rtv_.Reset();
    swapChain_.Reset();
}

// ════════════════════════════════════════════════════════════════
//  字体：加载系统字体
// ════════════════════════════════════════════════════════════════

/**
 * @brief 加载系统字体用于 ImGui 渲染。
 *
 * 从 C:\\Windows\\Fonts\\msyh.ttc 加载微软雅黑字体，
 * 字体大小根据 DPI 缩放系数调整，
 * 并包含简体中文常用字形和韩文字形范围。
 */
void SettingsWindow::SetupFonts()
{
    ImGuiIO& io = ImGui::GetIO();
    const float fontPx = 16.0f * dpiScale_;

    // 西文基底：Segoe UI Variable（Windows 11 原生界面字体，更轻盈现代）
    bool loadedBase = false;
    const std::string segoeVarPath = "C:\\Windows\\Fonts\\SegUIVar.ttf";
    if (FILE* f = fopen(segoeVarPath.c_str(), "rb"))
    {
        fclose(f);
        io.Fonts->AddFontFromFileTTF(segoeVarPath.c_str(), fontPx, nullptr,
            io.Fonts->GetGlyphRangesDefault());
        loadedBase = true;
    }
    if (!loadedBase)
        io.Fonts->AddFontDefault();

    // 中文：合并微软雅黑，仅覆盖 CJK，使西文保留 Segoe 观感
    const std::string cjkFontPath = "C:\\Windows\\Fonts\\msyh.ttc";
    if (FILE* f = fopen(cjkFontPath.c_str(), "rb"))
    {
        fclose(f);
        static const ImWchar cjkRanges[] = {
            0x3000, 0x303F, // CJK 标点
            0x4E00, 0x9FFF, // CJK 统一表意文字
            0xFF00, 0xFFEF, // 全角形式
            0x2018, 0x201F, // 引号
            0,
        };
        ImFontConfig cjkConfig;
        cjkConfig.MergeMode = true;
        io.Fonts->AddFontFromFileTTF(cjkFontPath.c_str(), fontPx,
            &cjkConfig, cjkRanges);
    }

    const std::string koreanFontPath =
        "C:\\Windows\\Fonts\\malgun.ttf";
    if (FILE* f = fopen(koreanFontPath.c_str(), "rb"))
    {
        fclose(f);
        ImFontConfig koreanConfig;
        koreanConfig.MergeMode = true;
        io.Fonts->AddFontFromFileTTF(koreanFontPath.c_str(),
            fontPx, &koreanConfig, io.Fonts->GetGlyphRangesKorean());
    }

    // 图标：合并 Windows 自带 Segoe MDL2 图标字体，供导航/分区使用
    if (loadedBase)
    {
        const std::string iconFontPath = "C:\\Windows\\Fonts\\segmdl2.ttf";
        if (FILE* f = fopen(iconFontPath.c_str(), "rb"))
        {
            fclose(f);
            static const ImWchar iconRanges[] = { 0xE700, 0xF1FF, 0 };
            ImFontConfig iconConfig;
            iconConfig.MergeMode = true;
            io.Fonts->AddFontFromFileTTF(iconFontPath.c_str(), fontPx,
                &iconConfig, iconRanges);
        }
    }

    HRSRC resource = FindResourceW(instance_, MAKEINTRESOURCEW(IDR_FA_FONT), RT_RCDATA);
    HGLOBAL resourceHandle = resource ? LoadResource(instance_, resource) : nullptr;
    void* fontData = resourceHandle ? LockResource(resourceHandle) : nullptr;
    DWORD fontSize = resource ? SizeofResource(instance_, resource) : 0;
    if (fontData && fontSize > 0)
    {
        static const ImWchar iconRanges[] = { 0xE000, 0xF8FF, 0 };
        ImFontConfig config;
        config.FontDataOwnedByAtlas = false;
        strcpy_s(config.Name, "Font Awesome 6 Free Solid");
        faDebugFont_ = io.Fonts->AddFontFromMemoryTTF(fontData, static_cast<int>(fontSize),
            18.0f * dpiScale_, &config, iconRanges);
    }

    resource = FindResourceW(instance_,
        MAKEINTRESOURCEW(IDR_FLUENT_REGULAR_FONT), RT_RCDATA);
    resourceHandle = resource ? LoadResource(instance_, resource) : nullptr;
    fontData = resourceHandle ? LockResource(resourceHandle) : nullptr;
    fontSize = resource ? SizeofResource(instance_, resource) : 0;
    if (fontData && fontSize > 0)
    {
        static const ImWchar iconRanges[] = {
            0xE000, 0xF8FF, 0xF0000, 0xF0CCE, 0,
        };
        ImFontConfig config;
        config.FontDataOwnedByAtlas = false;
        strcpy_s(config.Name, "FluentSystemIcons-Regular");
        fluentDebugFont_ = io.Fonts->AddFontFromMemoryTTF(fontData,
            static_cast<int>(fontSize), 18.0f * dpiScale_,
            &config, iconRanges);
    }
}

// ════════════════════════════════════════════════════════════════
//  开机自启：查询与设置（通过 Windows 注册表 Run 键）
// ════════════════════════════════════════════════════════════════

/**
 * @brief 检查当前是否已启用开机自启。
 *
 * 读取 HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run
 * 下 "SnowDesktop" 条目是否存在。
 * @return true 已启用开机自启
 */
bool SettingsWindow::IsAutoStartEnabled() const
{
    if (snowdesktop::deployment::IsPackaged())
    {
        if (!packagedAutoStartStateKnown_)
        {
            const auto state =
                snowdesktop::deployment::GetPackagedAutoStartState();
            packagedAutoStartEnabled_ =
                snowdesktop::deployment::IsPackagedAutoStartStateEnabled(
                    state);
            packagedAutoStartStateKnown_ = true;
        }
        return packagedAutoStartEnabled_;
    }

    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        0, KEY_READ, &key) != ERROR_SUCCESS)
        return false;

    wchar_t value[256]{};
    DWORD size = sizeof(value);
    DWORD type = REG_SZ;
    LONG result = RegQueryValueExW(key, L"SnowDesktop", nullptr, &type,
        reinterpret_cast<BYTE*>(value), &size);
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

/**
 * @brief 设置或取消开机自启。
 *
 * 在 HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run
 * 下创建或删除 "SnowDesktop" 条目。
 * @param enable true 添加注册表项启用自启，false 删除
 */
void SettingsWindow::SetAutoStart(bool enable) const
{
    if (snowdesktop::deployment::IsPackaged())
    {
        using snowdesktop::deployment::PackagedAutoStartState;
        const std::wstring otherAutoStart =
            ReadPortableAutoStartCommand();
        if (enable && !otherAutoStart.empty())
        {
            const std::wstring confirmation = _LFW(
                "app.settings.auto_start_portable_conflict",
                otherAutoStart);
            if (MessageBoxW(hwnd_, confirmation.c_str(),
                    _LW("app.settings.auto_start"),
                    MB_YESNO | MB_ICONWARNING) == IDYES)
            {
                ShellExecuteW(nullptr, L"open",
                    L"ms-settings:startupapps",
                    nullptr, nullptr, SW_SHOW);
            }
            return;
        }

        const PackagedAutoStartState state =
            snowdesktop::deployment::SetPackagedAutoStartEnabled(enable);
        if (state != PackagedAutoStartState::Unavailable)
        {
            packagedAutoStartStateKnown_ = true;
            packagedAutoStartEnabled_ =
                snowdesktop::deployment::IsPackagedAutoStartStateEnabled(
                    state);
        }

        if (enable && !packagedAutoStartEnabled_)
        {
            if (state == PackagedAutoStartState::DisabledByUser)
            {
                if (MessageBoxW(hwnd_,
                        _LW("app.settings.auto_start_manual_required"),
                        _LW("app.settings.auto_start"),
                        MB_YESNO | MB_ICONINFORMATION) == IDYES)
                {
                    ShellExecuteW(nullptr, L"open",
                        L"ms-settings:startupapps",
                        nullptr, nullptr, SW_SHOW);
                }
            }
            else if (state == PackagedAutoStartState::DisabledByPolicy)
            {
                MessageBoxW(hwnd_,
                    _LW("app.settings.auto_start_policy_disabled"),
                    _LW("app.settings.auto_start"),
                    MB_OK | MB_ICONWARNING);
            }
            else
            {
                MessageBoxW(hwnd_,
                    _LW("app.settings.auto_start_enable_failed"),
                    _LW("app.settings.auto_start"),
                    MB_OK | MB_ICONERROR);
            }
        }
        else if (!enable && packagedAutoStartEnabled_)
        {
            if (state == PackagedAutoStartState::EnabledByPolicy)
            {
                MessageBoxW(hwnd_,
                    _LW("app.settings.auto_start_policy_enabled"),
                    _LW("app.settings.auto_start"),
                    MB_OK | MB_ICONWARNING);
            }
            else
            {
                MessageBoxW(hwnd_,
                    _LW("app.settings.auto_start_disable_failed"),
                    _LW("app.settings.auto_start"),
                    MB_OK | MB_ICONERROR);
            }
        }
        return;
    }

    if (enable)
    {
        const auto installedState =
            snowdesktop::deployment::GetInstalledPackagedAutoStartState();
        if (snowdesktop::deployment::IsPackagedAutoStartStateEnabled(
                installedState))
        {
            if (MessageBoxW(hwnd_,
                    _LW("app.settings.auto_start_installed_conflict"),
                    _LW("app.settings.auto_start"),
                    MB_YESNO | MB_ICONWARNING) == IDYES)
            {
                ShellExecuteW(nullptr, L"open",
                    L"ms-settings:startupapps",
                    nullptr, nullptr, SW_SHOW);
            }
            return;
        }
    }

    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
        kAutoStartRunSubKey,
        0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;

    if (enable)
    {
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
        RegSetValueExW(key, kAutoStartRunValue, 0, REG_SZ,
            reinterpret_cast<const BYTE*>(path),
            static_cast<DWORD>((wcslen(path) + 1) * sizeof(wchar_t)));
    }
    else
    {
        RegDeleteValueW(key, kAutoStartRunValue);
    }
    RegCloseKey(key);
}

// ════════════════════════════════════════════════════════════════
//  窗口过程：消息处理
// ════════════════════════════════════════════════════════════════

/**
 * @brief 静态窗口过程函数，处理设置窗口的 Windows 消息。
 *
 * 处理的消息包括：
 * - ESC 键按下时请求关闭窗口
 * - 将输入事件转发给 ImGui 的 Win32 处理器
 * - WM_MOUSEACTIVATE：确保鼠标激活
 * - WM_SIZE：窗口尺寸变化时重建交换链并重绘
 * - WM_DPICHANGED：DPI 变化时更新缩放系数和建议尺寸
 * - WM_GETMINMAXINFO：设置最小窗口尺寸（500x350）
 * - WM_CLOSE：请求关闭而非直接销毁
 * @param hwnd   窗口句柄
 * @param msg    消息 ID
 * @param wParam 消息参数 WPARAM
 * @param lParam 消息参数 LPARAM
 * @return 消息处理结果（0 表示已处理，否则返回 DefWindowProcW）
 */
LRESULT CALLBACK SettingsWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    // 只有设置窗口自身的消息才会请求新帧。桌面窗口的鼠标、拖拽与
    // 定时器消息不再连带触发 ImGui 重建和交换链 Present。
    if (g_settingsWindow != nullptr && msg != WM_TIMER)
        g_settingsWindow->renderRequested_ = true;

    if (g_settingsWindow != nullptr &&
        (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) &&
        wParam == VK_ESCAPE)
    {
        if (g_settingsWindow->IsHotkeyCaptureActive())
        {
            g_settingsWindow->CancelHotkeyCapture();
            return 0;
        }
        g_settingsWindow->RequestClose();
        return 0;
    }

    if (g_settingsWindow != nullptr &&
        g_settingsWindow->IsHotkeyCaptureActive() &&
        (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN ||
            msg == WM_KEYUP || msg == WM_SYSKEYUP))
    {
        g_settingsWindow->HandleHotkeyCaptureKeyMessage(
            msg, wParam);
    }

    if (g_settingsWindow != nullptr && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_TIMER:
        if (g_settingsWindow != nullptr &&
            wParam == kSettingsHotkeyCaptureTimerId)
        {
            g_settingsWindow->UpdateHotkeyCapture();
            g_settingsWindow->renderRequested_ = true;
            return 0;
        }
        if (g_settingsWindow != nullptr && wParam == kSettingsRefreshTimerId)
        {
            // 低频更新后端状态、调试采样和文本光标等动态内容。
            g_settingsWindow->renderRequested_ = true;
            return 0;
        }
        break;
    case WM_KILLFOCUS:
        if (g_settingsWindow != nullptr &&
            g_settingsWindow->IsHotkeyCaptureActive())
        {
            g_settingsWindow->UpdateHotkeyCapture();
            if (g_settingsWindow->IsHotkeyCaptureActive() &&
                !g_settingsWindow->hotkeyCapturePrimarySeen_ &&
                !g_settingsWindow->hotkeyCaptureClearPending_)
            {
                g_settingsWindow->CancelHotkeyCapture();
            }
        }
        break;
    case WM_MOUSEACTIVATE:
        return MA_ACTIVATE;
    case WM_SIZE:
        if (g_settingsWindow != nullptr && wParam != SIZE_MINIMIZED)
        {
            if (g_settingsWindow->CreateSwapChain())
            {
                g_settingsWindow->renderRequested_ = true;
                // 交互式拖动边框时，系统的尺寸调整循环会暂停外层
                // GetMessage 循环，因此需要在 WM_SIZE 内立即 Present。
                if (IsWindowVisible(hwnd) && !IsIconic(hwnd))
                    g_settingsWindow->Render();
            }
        }
        return 0;
    case WM_DPICHANGED:
    {
        if (g_settingsWindow != nullptr)
        {
            g_settingsWindow->dpiScale_ = static_cast<float>(LOWORD(wParam)) / 96.0f;
        }
        RECT* suggested = reinterpret_cast<RECT*>(lParam);
        SetWindowPos(hwnd, nullptr,
            suggested->left, suggested->top,
            suggested->right - suggested->left,
            suggested->bottom - suggested->top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_GETMINMAXINFO:
    {
        MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        mmi->ptMinTrackSize.x = 500;
        mmi->ptMinTrackSize.y = 350;
        return 0;
    }
    case WM_CLOSE:
        g_settingsWindow->RequestClose();
        return 0;
    case WM_DESTROY:
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
