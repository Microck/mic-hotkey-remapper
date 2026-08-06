#define UNICODE
#define _UNICODE

#include <windows.h>
#include <dbt.h>
#include <setupapi.h>
#include <initguid.h>
#include <usbiodef.h>

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Gdi32.lib")
#pragma comment(lib, "Setupapi.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "User32.lib")

namespace {

constexpr wchar_t kBackgroundClass[] = L"MicHotkeyRemapper.Background";
constexpr wchar_t kConfigClass[] = L"MicHotkeyRemapper.Config";
constexpr wchar_t kMutexName[] = L"Local\\MicHotkeyRemapper.08BB.2902";
constexpr wchar_t kTargetToken[] = L"vid_08bb&pid_2902";
constexpr UINT kReloadMessage = WM_APP + 1;
constexpr UINT kTrayMessage = WM_APP + 2;
constexpr UINT kTrayToggle = 2001;
constexpr UINT kTrayConfigure = 2002;
constexpr UINT kTrayExit = 2003;
constexpr UINT_PTR kEnableTimerId = 1;
constexpr UINT_PTR kTapReleaseTimerId = 2;
constexpr UINT kDefaultEnableDelayMs = 500;
constexpr UINT kMaxEnableDelayMs = 5000;
constexpr UINT kDefaultTapDurationMs = 75;
constexpr UINT kMinTapDurationMs = 10;
constexpr UINT kMaxTapDurationMs = 1000;

UINT g_taskbarCreated = 0;

enum class Mode { Hold, Tap };

struct KeyCombo {
    std::vector<WORD> modifiers;
    WORD key = VK_F13;
    std::wstring text = L"F13";
};

struct Config {
    Mode mode = Mode::Tap;
    KeyCombo combo;
    UINT enableDelayMs = kDefaultEnableDelayMs;
    UINT tapDurationMs = kDefaultTapDurationMs;
};

struct AppState {
    Config config;
    bool devicePresent = false;
    bool initialized = false;
    bool keyHeld = false;
    bool tapActive = false;
    bool enablePending = false;
    bool remappingEnabled = true;
    HWND window = nullptr;
    HICON enabledIcon = nullptr;
    HICON disabledIcon = nullptr;
};

struct ConfigUi {
    Config config;
    bool startWithWindows = true;
    HWND window = nullptr;
    HWND holdRadio = nullptr;
    HWND tapRadio = nullptr;
    HWND comboEdit = nullptr;
    HWND delayEdit = nullptr;
    HWND tapDurationEdit = nullptr;
    HWND startCheck = nullptr;
    WNDPROC originalEditProc = nullptr;
};

ConfigUi* g_configUi = nullptr;

std::wstring Lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), towlower);
    return value;
}

std::wstring Trim(const std::wstring& value) {
    size_t first = 0;
    while (first < value.size() && iswspace(value[first])) ++first;
    size_t last = value.size();
    while (last > first && iswspace(value[last - 1])) --last;
    return value.substr(first, last - first);
}

std::vector<std::wstring> Split(const std::wstring& value, wchar_t separator) {
    std::vector<std::wstring> parts;
    size_t start = 0;
    while (start <= value.size()) {
        size_t end = value.find(separator, start);
        parts.push_back(Trim(value.substr(start, end == std::wstring::npos ? end : end - start)));
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return parts;
}

bool IsModifierToken(const std::wstring& token, WORD* key) {
    std::wstring normalized = Lower(token);
    if (normalized == L"ctrl" || normalized == L"control" || normalized == L"lctrl") {
        *key = VK_LCONTROL;
        return true;
    }
    if (normalized == L"rctrl" || normalized == L"rcontrol") {
        *key = VK_RCONTROL;
        return true;
    }
    if (normalized == L"alt" || normalized == L"lalt") {
        *key = VK_LMENU;
        return true;
    }
    if (normalized == L"ralt") {
        *key = VK_RMENU;
        return true;
    }
    if (normalized == L"shift" || normalized == L"lshift") {
        *key = VK_LSHIFT;
        return true;
    }
    if (normalized == L"rshift") {
        *key = VK_RSHIFT;
        return true;
    }
    if (normalized == L"win" || normalized == L"lwin" || normalized == L"windows") {
        *key = VK_LWIN;
        return true;
    }
    if (normalized == L"rwin" || normalized == L"rwindows") {
        *key = VK_RWIN;
        return true;
    }
    return false;
}

bool ParseKeyToken(const std::wstring& token, WORD* key) {
    std::wstring normalized = Lower(token);
    if (normalized.size() == 1) {
        wchar_t c = normalized[0];
        if ((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9')) {
            *key = static_cast<WORD>(towupper(c));
            return true;
        }
    }

    if (normalized.size() >= 2 && normalized[0] == L'f') {
        int number = _wtoi(normalized.c_str() + 1);
        if (number >= 1 && number <= 24) {
            *key = static_cast<WORD>(VK_F1 + number - 1);
            return true;
        }
    }

    static const std::map<std::wstring, WORD> namedKeys = {
        {L"space", static_cast<WORD>(VK_SPACE)}, {L"enter", static_cast<WORD>(VK_RETURN)}, {L"return", static_cast<WORD>(VK_RETURN)},
        {L"tab", static_cast<WORD>(VK_TAB)}, {L"escape", static_cast<WORD>(VK_ESCAPE)}, {L"esc", static_cast<WORD>(VK_ESCAPE)},
        {L"backspace", static_cast<WORD>(VK_BACK)}, {L"back", static_cast<WORD>(VK_BACK)}, {L"insert", static_cast<WORD>(VK_INSERT)},
        {L"delete", static_cast<WORD>(VK_DELETE)}, {L"home", static_cast<WORD>(VK_HOME)}, {L"end", static_cast<WORD>(VK_END)},
        {L"pgup", static_cast<WORD>(VK_PRIOR)}, {L"pageup", static_cast<WORD>(VK_PRIOR)}, {L"pgdn", static_cast<WORD>(VK_NEXT)},
        {L"pagedown", static_cast<WORD>(VK_NEXT)}, {L"up", static_cast<WORD>(VK_UP)}, {L"down", static_cast<WORD>(VK_DOWN)},
        {L"left", static_cast<WORD>(VK_LEFT)}, {L"right", static_cast<WORD>(VK_RIGHT)}, {L"capslock", static_cast<WORD>(VK_CAPITAL)},
        {L"numlock", static_cast<WORD>(VK_NUMLOCK)}, {L"scrolllock", static_cast<WORD>(VK_SCROLL)}, {L"printscreen", static_cast<WORD>(VK_SNAPSHOT)},
        {L"pause", static_cast<WORD>(VK_PAUSE)}, {L"apps", static_cast<WORD>(VK_APPS)},
    };
    auto found = namedKeys.find(normalized);
    if (found != namedKeys.end()) {
        *key = found->second;
        return true;
    }
    return false;
}

std::wstring KeyTokenFromVk(WORD key) {
    if (key >= 'A' && key <= 'Z') return std::wstring(1, static_cast<wchar_t>(key));
    if (key >= '0' && key <= '9') return std::wstring(1, static_cast<wchar_t>(key));
    if (key >= VK_F1 && key <= VK_F24) return L"F" + std::to_wstring(key - VK_F1 + 1);

    static const std::map<WORD, std::wstring> names = {
        {static_cast<WORD>(VK_SPACE), L"SPACE"}, {static_cast<WORD>(VK_RETURN), L"ENTER"}, {static_cast<WORD>(VK_TAB), L"TAB"},
        {static_cast<WORD>(VK_ESCAPE), L"ESC"}, {static_cast<WORD>(VK_BACK), L"BACKSPACE"}, {static_cast<WORD>(VK_INSERT), L"INSERT"},
        {static_cast<WORD>(VK_DELETE), L"DELETE"}, {static_cast<WORD>(VK_HOME), L"HOME"}, {static_cast<WORD>(VK_END), L"END"},
        {static_cast<WORD>(VK_PRIOR), L"PGUP"}, {static_cast<WORD>(VK_NEXT), L"PGDN"}, {static_cast<WORD>(VK_UP), L"UP"},
        {static_cast<WORD>(VK_DOWN), L"DOWN"}, {static_cast<WORD>(VK_LEFT), L"LEFT"}, {static_cast<WORD>(VK_RIGHT), L"RIGHT"},
        {static_cast<WORD>(VK_CAPITAL), L"CAPSLOCK"}, {static_cast<WORD>(VK_NUMLOCK), L"NUMLOCK"}, {static_cast<WORD>(VK_SCROLL), L"SCROLLLOCK"},
        {static_cast<WORD>(VK_SNAPSHOT), L"PRINTSCREEN"}, {static_cast<WORD>(VK_PAUSE), L"PAUSE"}, {static_cast<WORD>(VK_APPS), L"APPS"},
    };
    auto found = names.find(key);
    return found == names.end() ? L"F13" : found->second;
}

bool ParseCombo(const std::wstring& text, KeyCombo* combo) {
    std::vector<std::wstring> parts = Split(text, L'+');
    if (parts.empty()) return false;

    KeyCombo parsed;
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
        WORD modifier = 0;
        if (!IsModifierToken(parts[i], &modifier)) return false;
        if (std::find(parsed.modifiers.begin(), parsed.modifiers.end(), modifier) != parsed.modifiers.end()) {
            return false;
        }
        parsed.modifiers.push_back(modifier);
    }

    if (!ParseKeyToken(parts.back(), &parsed.key)) return false;
    parsed.text.clear();
    for (WORD modifier : parsed.modifiers) {
        if (!parsed.text.empty()) parsed.text += L"+";
        if (modifier == VK_LCONTROL || modifier == VK_RCONTROL) parsed.text += L"CTRL";
        else if (modifier == VK_LMENU || modifier == VK_RMENU) parsed.text += L"ALT";
        else if (modifier == VK_LSHIFT || modifier == VK_RSHIFT) parsed.text += L"SHIFT";
        else parsed.text += L"WIN";
    }
    if (!parsed.text.empty()) parsed.text += L"+";
    parsed.text += KeyTokenFromVk(parsed.key);
    *combo = parsed;
    return true;
}

bool ParseMilliseconds(const std::wstring& text, UINT minimum, UINT maximum, UINT* milliseconds) {
    std::wstring value = Trim(text);
    if (value.empty()) return false;

    UINT parsed = 0;
    for (wchar_t character : value) {
        if (character < L'0' || character > L'9') return false;
        UINT digit = static_cast<UINT>(character - L'0');
        if (parsed > (maximum - digit) / 10) return false;
        parsed = parsed * 10 + digit;
    }

    if (parsed < minimum) return false;
    *milliseconds = parsed;
    return true;
}

std::wstring ConfigDirectory() {
    wchar_t appData[MAX_PATH] = {};
    DWORD length = GetEnvironmentVariableW(L"APPDATA", appData, ARRAYSIZE(appData));
    if (length == 0 || length >= ARRAYSIZE(appData)) return L".";
    return std::wstring(appData) + L"\\MicHotkeyRemapper";
}

std::wstring ConfigPath() {
    return ConfigDirectory() + L"\\config.ini";
}

std::wstring ExecutablePath() {
    wchar_t path[MAX_PATH] = {};
    DWORD length = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    return length == 0 ? L"" : std::wstring(path, length);
}

void EnsureConfigDirectory() {
    std::wstring directory = ConfigDirectory();
    CreateDirectoryW(directory.c_str(), nullptr);
}

bool LoadConfig(Config* config) {
    std::wifstream input(ConfigPath());
    if (!input) return false;

    std::wstring line;
    while (std::getline(input, line)) {
        size_t equals = line.find(L'=');
        if (equals == std::wstring::npos) continue;
        std::wstring name = Lower(Trim(line.substr(0, equals)));
        std::wstring value = Trim(line.substr(equals + 1));
        if (name == L"mode") {
            std::wstring mode = Lower(value);
            if (mode == L"hold") config->mode = Mode::Hold;
            if (mode == L"tap") config->mode = Mode::Tap;
        } else if (name == L"key") {
            KeyCombo parsed;
            if (ParseCombo(value, &parsed)) config->combo = parsed;
        } else if (name == L"enable_delay_ms") {
            UINT delayMs = 0;
            if (ParseMilliseconds(value, 0, kMaxEnableDelayMs, &delayMs)) config->enableDelayMs = delayMs;
        } else if (name == L"tap_duration_ms") {
            UINT durationMs = 0;
            if (ParseMilliseconds(value, kMinTapDurationMs, kMaxTapDurationMs, &durationMs)) {
                config->tapDurationMs = durationMs;
            }
        }
    }
    return true;
}

bool SaveConfig(const Config& config) {
    EnsureConfigDirectory();
    std::wofstream output(ConfigPath(), std::ios::trunc);
    if (!output) return false;
    output << L"mode=" << (config.mode == Mode::Hold ? L"hold" : L"tap") << L"\n";
    output << L"key=" << config.combo.text << L"\n";
    output << L"enable_delay_ms=" << config.enableDelayMs << L"\n";
    output << L"tap_duration_ms=" << config.tapDurationMs << L"\n";
    return true;
}

bool GetAutostart(bool* configured) {
    *configured = false;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }
    wchar_t value[4096] = {};
    DWORD bytes = sizeof(value);
    DWORD type = 0;
    LSTATUS status = RegQueryValueExW(key, L"MicHotkeyRemapper", nullptr, &type, reinterpret_cast<BYTE*>(value), &bytes);
    RegCloseKey(key);
    if (status == ERROR_SUCCESS) *configured = true;
    return status == ERROR_SUCCESS && type == REG_SZ;
}

bool SetAutostart(bool enabled) {
    HKEY key = nullptr;
    DWORD disposition = 0;
    LSTATUS status = RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, nullptr, 0, KEY_WRITE, nullptr, &key, &disposition);
    if (status != ERROR_SUCCESS) return false;

    std::wstring command = L"\"" + ExecutablePath() + L"\" --background";
    if (enabled) {
        status = RegSetValueExW(key, L"MicHotkeyRemapper", 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()), static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        status = RegDeleteValueW(key, L"MicHotkeyRemapper");
        if (status == ERROR_FILE_NOT_FOUND) status = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

bool IsTargetDevicePath(const wchar_t* path) {
    return path != nullptr && Lower(path).find(kTargetToken) != std::wstring::npos;
}

bool IsTargetDevicePresent() {
    HDEVINFO info = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_USB_DEVICE, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (info == INVALID_HANDLE_VALUE) return false;

    bool present = false;
    for (DWORD index = 0; ; ++index) {
        SP_DEVICE_INTERFACE_DATA interfaceData = {};
        interfaceData.cbSize = sizeof(interfaceData);
        if (!SetupDiEnumDeviceInterfaces(info, nullptr, &GUID_DEVINTERFACE_USB_DEVICE, index, &interfaceData)) break;

        DWORD required = 0;
        SetupDiGetDeviceInterfaceDetailW(info, &interfaceData, nullptr, 0, &required, nullptr);
        if (required == 0) continue;

        std::vector<BYTE> buffer(required);
        auto detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (SetupDiGetDeviceInterfaceDetailW(info, &interfaceData, detail, required, &required, nullptr) && IsTargetDevicePath(detail->DevicePath)) {
            present = true;
            break;
        }
    }
    SetupDiDestroyDeviceInfoList(info);
    return present;
}

bool IsExtendedKey(WORD key) {
    switch (key) {
    case VK_RCONTROL:
    case VK_RMENU:
    case VK_LWIN:
    case VK_RWIN:
    case VK_INSERT:
    case VK_DELETE:
    case VK_HOME:
    case VK_END:
    case VK_PRIOR:
    case VK_NEXT:
    case VK_UP:
    case VK_DOWN:
    case VK_LEFT:
    case VK_RIGHT:
    case VK_NUMLOCK:
    case VK_SNAPSHOT:
    case VK_APPS:
        return true;
    default:
        return false;
    }
}

INPUT MakeKeyInput(WORD key, bool keyUp) {
    INPUT input = {};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = key;
    input.ki.dwFlags = (keyUp ? KEYEVENTF_KEYUP : 0) | (IsExtendedKey(key) ? KEYEVENTF_EXTENDEDKEY : 0);
    return input;
}

bool SendKeySequence(std::vector<INPUT>* inputs) {
    if (inputs->empty()) return true;
    UINT expected = static_cast<UINT>(inputs->size());
    return SendInput(expected, inputs->data(), sizeof(INPUT)) == expected;
}

bool PressCombo(const KeyCombo& combo) {
    std::vector<INPUT> inputs;
    inputs.reserve(combo.modifiers.size() + 1);
    for (WORD modifier : combo.modifiers) inputs.push_back(MakeKeyInput(modifier, false));
    inputs.push_back(MakeKeyInput(combo.key, false));
    return SendKeySequence(&inputs);
}

bool ReleaseCombo(const KeyCombo& combo) {
    std::vector<INPUT> inputs;
    inputs.reserve(combo.modifiers.size() + 1);
    inputs.push_back(MakeKeyInput(combo.key, true));
    for (auto modifier = combo.modifiers.rbegin(); modifier != combo.modifiers.rend(); ++modifier) {
        inputs.push_back(MakeKeyInput(*modifier, true));
    }
    return SendKeySequence(&inputs);
}

void CancelPendingEnable(AppState* state) {
    if (state->window != nullptr) KillTimer(state->window, kEnableTimerId);
    state->enablePending = false;
}

void FinishActiveTap(AppState* state) {
    if (state->window != nullptr) KillTimer(state->window, kTapReleaseTimerId);
    if (!state->tapActive) return;
    ReleaseCombo(state->config.combo);
    state->tapActive = false;
}

void StartTap(AppState* state) {
    FinishActiveTap(state);
    if (!PressCombo(state->config.combo)) {
        // A partial SendInput call can leave some keys down, so always attempt a full release.
        ReleaseCombo(state->config.combo);
        return;
    }

    state->tapActive = true;
    if (SetTimer(state->window, kTapReleaseTimerId, state->config.tapDurationMs, nullptr) == 0) {
        FinishActiveTap(state);
    }
}

void FirePendingEnable(AppState* state) {
    if (!state->enablePending) return;
    state->enablePending = false;

    if (!state->remappingEnabled || !state->devicePresent) return;
    if (state->config.mode == Mode::Hold) {
        if (PressCombo(state->config.combo)) {
            state->keyHeld = true;
        } else {
            ReleaseCombo(state->config.combo);
        }
    } else {
        StartTap(state);
    }
}

void ScheduleEnable(AppState* state) {
    CancelPendingEnable(state);
    state->enablePending = true;
    if (state->config.enableDelayMs == 0 || SetTimer(state->window, kEnableTimerId, state->config.enableDelayMs, nullptr) == 0) {
        FirePendingEnable(state);
    }
}

void DrawMicrophoneShape(HDC dc, int size, COLORREF color) {
    int stroke = max(1, size / 7);
    HPEN pen = CreatePen(PS_SOLID, stroke, color);
    HBRUSH brush = CreateSolidBrush(color);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, brush);

    int left = size * 3 / 8;
    int right = size * 5 / 8;
    int top = size / 8;
    int bottom = size * 5 / 8;
    RoundRect(dc, left, top, right, bottom, size / 5, size / 5);

    SelectObject(dc, GetStockObject(NULL_BRUSH));
    MoveToEx(dc, size / 4, size * 7 / 16, nullptr);
    LineTo(dc, size / 4, size * 5 / 8);
    LineTo(dc, size * 3 / 8, size * 3 / 4);
    LineTo(dc, size * 5 / 8, size * 3 / 4);
    LineTo(dc, size * 3 / 4, size * 5 / 8);
    LineTo(dc, size * 3 / 4, size * 7 / 16);

    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
}

HICON CreateMicrophoneIcon(COLORREF color) {
    int size = max(16, GetSystemMetrics(SM_CXSMICON));
    HDC screen = GetDC(nullptr);
    HDC colorDc = CreateCompatibleDC(screen);
    HBITMAP colorBitmap = CreateCompatibleBitmap(screen, size, size);
    HBITMAP maskBitmap = CreateBitmap(size, size, 1, 1, nullptr);
    HGDIOBJ oldColorBitmap = SelectObject(colorDc, colorBitmap);
    HGDIOBJ oldMaskBitmap = SelectObject(colorDc, maskBitmap);

    PatBlt(colorDc, 0, 0, size, size, WHITENESS);
    SelectObject(colorDc, colorBitmap);
    PatBlt(colorDc, 0, 0, size, size, BLACKNESS);
    DrawMicrophoneShape(colorDc, size, color);

    SelectObject(colorDc, maskBitmap);
    PatBlt(colorDc, 0, 0, size, size, WHITENESS);
    DrawMicrophoneShape(colorDc, size, RGB(0, 0, 0));

    ICONINFO iconInfo = {};
    iconInfo.fIcon = TRUE;
    iconInfo.hbmColor = colorBitmap;
    iconInfo.hbmMask = maskBitmap;
    HICON icon = CreateIconIndirect(&iconInfo);

    SelectObject(colorDc, oldMaskBitmap);
    SelectObject(colorDc, oldColorBitmap);
    DeleteObject(maskBitmap);
    DeleteObject(colorBitmap);
    DeleteDC(colorDc);
    ReleaseDC(nullptr, screen);
    return icon;
}

void UpdateTrayIcon(AppState* state) {
    if (state->window == nullptr) return;
    NOTIFYICONDATAW notification = {};
    notification.cbSize = sizeof(notification);
    notification.hWnd = state->window;
    notification.uID = 1;
    notification.uFlags = NIF_ICON | NIF_TIP;
    notification.hIcon = state->remappingEnabled ? state->enabledIcon : state->disabledIcon;
    wcscpy_s(notification.szTip, state->remappingEnabled ? L"Mic remapper: enabled" : L"Mic remapper: disabled");
    if (!Shell_NotifyIconW(NIM_MODIFY, &notification)) Shell_NotifyIconW(NIM_ADD, &notification);
}

void AddTrayIcon(AppState* state) {
    if (state->enabledIcon == nullptr) state->enabledIcon = CreateMicrophoneIcon(RGB(40, 190, 80));
    if (state->disabledIcon == nullptr) state->disabledIcon = CreateMicrophoneIcon(RGB(210, 55, 55));

    NOTIFYICONDATAW notification = {};
    notification.cbSize = sizeof(notification);
    notification.hWnd = state->window;
    notification.uID = 1;
    notification.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    notification.uCallbackMessage = kTrayMessage;
    notification.hIcon = state->remappingEnabled ? state->enabledIcon : state->disabledIcon;
    wcscpy_s(notification.szTip, state->remappingEnabled ? L"Mic remapper: enabled" : L"Mic remapper: disabled");
    Shell_NotifyIconW(NIM_ADD, &notification);
}

void RemoveTrayIcon(AppState* state) {
    if (state->window != nullptr) {
        NOTIFYICONDATAW notification = {};
        notification.cbSize = sizeof(notification);
        notification.hWnd = state->window;
        notification.uID = 1;
        Shell_NotifyIconW(NIM_DELETE, &notification);
    }
    if (state->enabledIcon != nullptr) DestroyIcon(state->enabledIcon);
    if (state->disabledIcon != nullptr) DestroyIcon(state->disabledIcon);
    state->enabledIcon = nullptr;
    state->disabledIcon = nullptr;
}

void SetRemappingEnabled(AppState* state, bool enabled) {
    if (state->remappingEnabled == enabled) return;
    CancelPendingEnable(state);
    FinishActiveTap(state);
    if (!enabled && state->keyHeld) {
        ReleaseCombo(state->config.combo);
        state->keyHeld = false;
    }
    state->remappingEnabled = enabled;
    if (enabled && state->devicePresent && state->config.mode == Mode::Hold) {
        ScheduleEnable(state);
    }
    UpdateTrayIcon(state);
}

void ShowTrayMenu(AppState* state) {
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) return;
    AppendMenuW(menu, MF_STRING, kTrayToggle, state->remappingEnabled ? L"Disable remapping" : L"Enable remapping");
    AppendMenuW(menu, MF_STRING, kTrayConfigure, L"Configure...");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kTrayExit, L"Exit");

    POINT cursor = {};
    GetCursorPos(&cursor);
    SetForegroundWindow(state->window);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, state->window, nullptr);
    PostMessageW(state->window, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

void ApplyPresence(AppState* state, bool present, bool initial) {
    if (!initial && state->initialized && state->devicePresent == present) return;

    if (state->keyHeld) {
        ReleaseCombo(state->config.combo);
        state->keyHeld = false;
    }
    CancelPendingEnable(state);
    FinishActiveTap(state);

    state->devicePresent = present;
    state->initialized = true;

    if (!state->remappingEnabled) return;

    if (state->config.mode == Mode::Hold) {
        if (present) {
            ScheduleEnable(state);
        }
    } else if (!initial) {
        if (present) ScheduleEnable(state);
        else StartTap(state);
    }
}

void ReloadConfig(AppState* state) {
    Config updated = state->config;
    if (!LoadConfig(&updated)) return;
    CancelPendingEnable(state);
    FinishActiveTap(state);
    if (state->keyHeld) {
        ReleaseCombo(state->config.combo);
        state->keyHeld = false;
    }
    state->config = updated;
    if (state->remappingEnabled && state->devicePresent && state->config.mode == Mode::Hold) {
        ScheduleEnable(state);
    }
}

void ReleaseHeldKey(AppState* state) {
    CancelPendingEnable(state);
    FinishActiveTap(state);
    if (state->keyHeld) {
        ReleaseCombo(state->config.combo);
        state->keyHeld = false;
    }
}

void LaunchConfigurator();

LRESULT CALLBACK BackgroundWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto state = reinterpret_cast<AppState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = reinterpret_cast<AppState*>(create->lpCreateParams);
        state->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }

    if (state != nullptr) {
        if (g_taskbarCreated != 0 && message == g_taskbarCreated) {
            AddTrayIcon(state);
            return 0;
        }
        if (message == kReloadMessage) {
            ReloadConfig(state);
            return 0;
        }
        if (message == WM_TIMER) {
            if (wParam == kEnableTimerId) {
                KillTimer(window, kEnableTimerId);
                FirePendingEnable(state);
                return 0;
            }
            if (wParam == kTapReleaseTimerId) {
                FinishActiveTap(state);
                return 0;
            }
        }
        if (message == kTrayMessage) {
            if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
                ShowTrayMenu(state);
            } else if (lParam == WM_LBUTTONDBLCLK) {
                LaunchConfigurator();
            }
            return 0;
        }
        if (message == WM_COMMAND) {
            switch (LOWORD(wParam)) {
            case kTrayToggle:
                SetRemappingEnabled(state, !state->remappingEnabled);
                return 0;
            case kTrayConfigure:
                LaunchConfigurator();
                return 0;
            case kTrayExit:
                PostMessageW(window, WM_CLOSE, 0, 0);
                return 0;
            default:
                break;
            }
        }
        if (message == WM_DEVICECHANGE && (wParam == DBT_DEVICEARRIVAL || wParam == DBT_DEVICEREMOVECOMPLETE) && lParam != 0) {
            auto header = reinterpret_cast<PDEV_BROADCAST_HDR>(lParam);
            if (header->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE) {
                auto device = reinterpret_cast<PDEV_BROADCAST_DEVICEINTERFACE_W>(lParam);
                if (IsTargetDevicePath(device->dbcc_name)) {
                    // Re-enumerating avoids trusting duplicate or out-of-order device notifications.
                    ApplyPresence(state, IsTargetDevicePresent(), false);
                }
            }
            return 0;
        }
        if (message == WM_CLOSE) {
            ReleaseHeldKey(state);
            DestroyWindow(window);
            return 0;
        }
    }
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool RegisterBackgroundClass(HINSTANCE instance) {
    WNDCLASSW windowClass = {};
    windowClass.lpfnWndProc = BackgroundWindowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = kBackgroundClass;
    return RegisterClassW(&windowClass) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

int RunBackground(HINSTANCE instance) {
    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (mutex == nullptr || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (mutex != nullptr) CloseHandle(mutex);
        return 0;
    }

    AppState state;
    LoadConfig(&state.config);
    if (!RegisterBackgroundClass(instance)) {
        CloseHandle(mutex);
        return 1;
    }

    HWND window = CreateWindowExW(0, kBackgroundClass, L"MicHotkeyRemapper", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, &state);
    if (window == nullptr) {
        CloseHandle(mutex);
        return 1;
    }

    DEV_BROADCAST_DEVICEINTERFACE_W filter = {};
    filter.dbcc_size = sizeof(filter);
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    filter.dbcc_classguid = GUID_DEVINTERFACE_USB_DEVICE;
    HDEVNOTIFY notification = RegisterDeviceNotificationW(window, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    AddTrayIcon(&state);
    ApplyPresence(&state, IsTargetDevicePresent(), true);

    MSG message = {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    if (notification != nullptr) UnregisterDeviceNotification(notification);
    RemoveTrayIcon(&state);
    ReleaseHeldKey(&state);
    CloseHandle(mutex);
    return 0;
}

void SetControlFont(HWND control) {
    HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

LRESULT CALLBACK ComboEditProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto ui = reinterpret_cast<ConfigUi*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_GETDLGCODE) return DLGC_WANTALLKEYS;
    if (ui != nullptr && (message == WM_KEYDOWN || message == WM_SYSKEYDOWN)) {
        WORD key = static_cast<WORD>(wParam);
        if (key == VK_LCONTROL || key == VK_RCONTROL || key == VK_LMENU || key == VK_RMENU || key == VK_LSHIFT || key == VK_RSHIFT || key == VK_LWIN || key == VK_RWIN) return 0;

        KeyCombo captured;
        captured.key = key;
        if (GetKeyState(VK_CONTROL) & 0x8000) captured.modifiers.push_back(VK_LCONTROL);
        if (GetKeyState(VK_MENU) & 0x8000) captured.modifiers.push_back(VK_LMENU);
        if (GetKeyState(VK_SHIFT) & 0x8000) captured.modifiers.push_back(VK_LSHIFT);
        if (GetKeyState(VK_LWIN) & 0x8000 || GetKeyState(VK_RWIN) & 0x8000) captured.modifiers.push_back(VK_LWIN);
        captured.text.clear();
        for (WORD modifier : captured.modifiers) {
            if (!captured.text.empty()) captured.text += L"+";
            if (modifier == VK_LCONTROL) captured.text += L"CTRL";
            else if (modifier == VK_LMENU) captured.text += L"ALT";
            else if (modifier == VK_LSHIFT) captured.text += L"SHIFT";
            else captured.text += L"WIN";
        }
        if (!captured.text.empty()) captured.text += L"+";
        captured.text += KeyTokenFromVk(captured.key);
        ui->config.combo = captured;
        SetWindowTextW(window, captured.text.c_str());
        return 0;
    }
    if (ui != nullptr && message == WM_CHAR) return 0;
    return CallWindowProcW(ui != nullptr ? ui->originalEditProc : DefWindowProcW, window, message, wParam, lParam);
}

void LaunchBackground() {
    std::wstring command = L"\"" + ExecutablePath() + L"\" --background";
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup = {sizeof(startup)};
    PROCESS_INFORMATION process = {};
    if (CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
}

void LaunchConfigurator() {
    std::wstring command = L"\"" + ExecutablePath() + L"\"";
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup = {sizeof(startup)};
    PROCESS_INFORMATION process = {};
    if (CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
}

void ReloadBackground() {
    HWND background = FindWindowW(kBackgroundClass, nullptr);
    if (background != nullptr) PostMessageW(background, kReloadMessage, 0, 0);
}

void StopBackground() {
    HWND background = FindWindowW(kBackgroundClass, nullptr);
    if (background != nullptr) PostMessageW(background, WM_CLOSE, 0, 0);
}

enum ControlId {
    kHoldRadio = 1001,
    kTapRadio = 1002,
    kComboEdit = 1003,
    kStartCheck = 1004,
    kSaveButton = 1005,
    kCancelButton = 1006,
    kDelayEdit = 1007,
    kTapDurationEdit = 1008,
};

LRESULT CALLBACK ConfigWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto ui = reinterpret_cast<ConfigUi*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        ui = reinterpret_cast<ConfigUi*>(create->lpCreateParams);
        ui->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ui));
    }

    if (ui == nullptr) return DefWindowProcW(window, message, wParam, lParam);

    if (message == WM_CREATE) {
        HINSTANCE instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(window, GWLP_HINSTANCE));
        CreateWindowW(L"STATIC", L"Microphone state shortcut", WS_CHILD | WS_VISIBLE, 16, 14, 300, 24, window, nullptr, instance, nullptr);
        HWND modeLabel = CreateWindowW(L"STATIC", L"Mode:", WS_CHILD | WS_VISIBLE, 16, 52, 80, 22, window, nullptr, instance, nullptr);
        ui->holdRadio = CreateWindowW(L"BUTTON", L"Hold", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_TABSTOP, 88, 48, 90, 28, window, reinterpret_cast<HMENU>(kHoldRadio), instance, nullptr);
        ui->tapRadio = CreateWindowW(L"BUTTON", L"Tap", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON, 184, 48, 90, 28, window, reinterpret_cast<HMENU>(kTapRadio), instance, nullptr);
        HWND keyLabel = CreateWindowW(L"STATIC", L"Shortcut:", WS_CHILD | WS_VISIBLE, 16, 88, 80, 22, window, nullptr, instance, nullptr);
        ui->comboEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", ui->config.combo.text.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 88, 84, 250, 26, window, reinterpret_cast<HMENU>(kComboEdit), instance, nullptr);
        HWND help = CreateWindowW(L"STATIC", L"Click the box, then press the desired key combination.", WS_CHILD | WS_VISIBLE, 88, 115, 340, 22, window, nullptr, instance, nullptr);
        HWND delayLabel = CreateWindowW(L"STATIC", L"Enable delay (ms):", WS_CHILD | WS_VISIBLE, 16, 148, 130, 22, window, nullptr, instance, nullptr);
        std::wstring delayText = std::to_wstring(ui->config.enableDelayMs);
        ui->delayEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", delayText.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL, 150, 144, 100, 26, window, reinterpret_cast<HMENU>(kDelayEdit), instance, nullptr);
        HWND delayHelp = CreateWindowW(L"STATIC", L"Wait after enable before sending the shortcut. 0 = immediate.", WS_CHILD | WS_VISIBLE, 16, 176, 390, 22, window, nullptr, instance, nullptr);
        HWND tapDurationLabel = CreateWindowW(L"STATIC", L"Tap duration (ms):", WS_CHILD | WS_VISIBLE, 16, 208, 130, 22, window, nullptr, instance, nullptr);
        std::wstring tapDurationText = std::to_wstring(ui->config.tapDurationMs);
        ui->tapDurationEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", tapDurationText.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL, 150, 204, 100, 26, window, reinterpret_cast<HMENU>(kTapDurationEdit), instance, nullptr);
        HWND tapDurationHelp = CreateWindowW(L"STATIC", L"How long Tap holds the shortcut. Accepted range: 10-1000.", WS_CHILD | WS_VISIBLE, 16, 236, 400, 22, window, nullptr, instance, nullptr);
        ui->startCheck = CreateWindowW(L"BUTTON", L"Start with Windows", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP, 16, 264, 220, 28, window, reinterpret_cast<HMENU>(kStartCheck), instance, nullptr);
        HWND save = CreateWindowW(L"BUTTON", L"Save and apply", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP, 178, 304, 130, 30, window, reinterpret_cast<HMENU>(kSaveButton), instance, nullptr);
        HWND cancel = CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 318, 304, 90, 30, window, reinterpret_cast<HMENU>(kCancelButton), instance, nullptr);
        SetControlFont(modeLabel);
        SetControlFont(keyLabel);
        SetControlFont(help);
        SetControlFont(delayLabel);
        SetControlFont(delayHelp);
        SetControlFont(tapDurationLabel);
        SetControlFont(tapDurationHelp);
        SetControlFont(save);
        SetControlFont(cancel);
        SetControlFont(ui->holdRadio);
        SetControlFont(ui->tapRadio);
        SetControlFont(ui->comboEdit);
        SetControlFont(ui->delayEdit);
        SetControlFont(ui->tapDurationEdit);
        SetControlFont(ui->startCheck);
        SendMessageW(ui->holdRadio, BM_SETCHECK, ui->config.mode == Mode::Hold, 0);
        SendMessageW(ui->tapRadio, BM_SETCHECK, ui->config.mode == Mode::Tap, 0);
        SendMessageW(ui->startCheck, BM_SETCHECK, ui->startWithWindows, 0);
        ui->originalEditProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(ui->comboEdit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ComboEditProc)));
        SetWindowLongPtrW(ui->comboEdit, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ui));
        return 0;
    }

    if (message == WM_COMMAND && HIWORD(wParam) == BN_CLICKED) {
        int id = LOWORD(wParam);
        if (id == kSaveButton) {
            wchar_t text[256] = {};
            GetWindowTextW(ui->comboEdit, text, ARRAYSIZE(text));
            KeyCombo parsed;
            if (!ParseCombo(text, &parsed)) {
                MessageBoxW(window, L"Enter a shortcut such as CTRL+ALT+F13 or F13.", L"Invalid shortcut", MB_OK | MB_ICONERROR);
                return 0;
            }
            wchar_t delayText[32] = {};
            GetWindowTextW(ui->delayEdit, delayText, ARRAYSIZE(delayText));
            UINT delayMs = 0;
            if (!ParseMilliseconds(delayText, 0, kMaxEnableDelayMs, &delayMs)) {
                MessageBoxW(window, L"Enter an enable delay from 0 to 5000 milliseconds.", L"Invalid delay", MB_OK | MB_ICONERROR);
                return 0;
            }
            wchar_t tapDurationText[32] = {};
            GetWindowTextW(ui->tapDurationEdit, tapDurationText, ARRAYSIZE(tapDurationText));
            UINT tapDurationMs = 0;
            if (!ParseMilliseconds(tapDurationText, kMinTapDurationMs, kMaxTapDurationMs, &tapDurationMs)) {
                MessageBoxW(window, L"Enter a Tap duration from 10 to 1000 milliseconds.", L"Invalid Tap duration", MB_OK | MB_ICONERROR);
                return 0;
            }
            ui->config.combo = parsed;
            ui->config.enableDelayMs = delayMs;
            ui->config.tapDurationMs = tapDurationMs;
            ui->config.mode = SendMessageW(ui->holdRadio, BM_GETCHECK, 0, 0) == BST_CHECKED ? Mode::Hold : Mode::Tap;
            ui->startWithWindows = SendMessageW(ui->startCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
            if (!SaveConfig(ui->config) || !SetAutostart(ui->startWithWindows)) {
                MessageBoxW(window, L"Could not save the configuration.", L"Save failed", MB_OK | MB_ICONERROR);
                return 0;
            }
            if (ui->startWithWindows) LaunchBackground();
            else StopBackground();
            ReloadBackground();
            DestroyWindow(window);
            return 0;
        }
        if (id == kCancelButton) {
            DestroyWindow(window);
            return 0;
        }
    }
    if (message == WM_CLOSE) {
        DestroyWindow(window);
        return 0;
    }
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

int RunConfigurator(HINSTANCE instance) {
    WNDCLASSW windowClass = {};
    windowClass.lpfnWndProc = ConfigWindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.lpszClassName = kConfigClass;
    if (RegisterClassW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return 1;

    ConfigUi ui;
    LoadConfig(&ui.config);
    bool autostartConfigured = false;
    ui.startWithWindows = true;
    bool autostartEnabled = GetAutostart(&autostartConfigured);
    if (autostartConfigured) ui.startWithWindows = autostartEnabled;
    HWND window = CreateWindowExW(WS_EX_DLGMODALFRAME, kConfigClass, L"Microphone Shortcut", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT, 440, 380, nullptr, nullptr, instance, &ui);
    if (window == nullptr) return 1;
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);

    MSG message = {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int) {
    std::wstring command = commandLine == nullptr ? L"" : Lower(commandLine);
    if (command.find(L"--background") != std::wstring::npos) return RunBackground(instance);
    return RunConfigurator(instance);
}
