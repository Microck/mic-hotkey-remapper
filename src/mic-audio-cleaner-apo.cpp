#define UNICODE
#define _UNICODE
#define NOMINMAX

#include <windows.h>
#include <initguid.h>
#include <audioclient.h>
#include <audioenginebaseapo.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <propkey.h>
#include <propvarutil.h>
#include <shellapi.h>
#include <shlobj.h>
#include <strsafe.h>

#include <algorithm>
#include <string>
#include <vector>

#include "mic-audio-apo-shared.h"

#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "AudioBaseProcessingObjectV140.lib")
#pragma comment(lib, "Gdi32.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "OleAut32.lib")
#pragma comment(lib, "Propsys.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "User32.lib")

namespace {

constexpr wchar_t kWindowClass[] = L"MicAudioCleanerApo.Window";
constexpr wchar_t kTargetName[] = L"USB PnP Sound Device";
constexpr wchar_t kCurrentApoClsid[] = L"{A1D5E6F4-4E58-4D67-A42A-5114A9B4E177}";
constexpr UINT kDefaultHighPassHz = 90;
constexpr UINT kDefaultGateThresholdCent = 250;
constexpr int kInstallButton = 1001;
constexpr int kUninstallButton = 1002;
constexpr int kHighPassEdit = 1003;
constexpr int kGateEdit = 1004;
constexpr int kStatusText = 1005;

std::wstring g_embeddedApoDllPath;

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    T** put() {
        reset();
        return &value_;
    }
    T* get() const { return value_; }
    T* operator->() const { return value_; }
    void reset() {
        if (value_ != nullptr) value_->Release();
        value_ = nullptr;
    }

private:
    T* value_ = nullptr;
};

struct TargetEndpoint {
    std::wstring id;
    std::wstring registryId;
    std::wstring name;
};

std::wstring ErrorText(HRESULT result) {
    wchar_t* buffer = nullptr;
    DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                      FORMAT_MESSAGE_IGNORE_INSERTS,
                                  nullptr, static_cast<DWORD>(result), 0,
                                  reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring message = length == 0 ? L"Unknown Windows error" : std::wstring(buffer, length);
    if (buffer != nullptr) LocalFree(buffer);
    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' ')) {
        message.pop_back();
    }
    return message;
}

std::wstring ModulePath() {
    wchar_t buffer[MAX_PATH] = {};
    DWORD length = GetModuleFileNameW(nullptr, buffer, ARRAYSIZE(buffer));
    return length == 0 ? L"" : std::wstring(buffer, length);
}

std::wstring SiblingPath(const wchar_t* name) {
    std::wstring module = ModulePath();
    const size_t separator = module.find_last_of(L"\\/");
    return separator == std::wstring::npos ? std::wstring(name) : module.substr(0, separator + 1) + name;
}

std::wstring ApoDllPath() {
    return g_embeddedApoDllPath.empty() ? SiblingPath(L"mic-audio-apo.dll") : g_embeddedApoDllPath;
}

std::wstring FriendlyName(IMMDevice* device) {
    ComPtr<IPropertyStore> properties;
    if (FAILED(device->OpenPropertyStore(STGM_READ, properties.put()))) return L"";
    PROPVARIANT value;
    PropVariantInit(&value);
    std::wstring name;
    if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &value)) &&
        value.vt == VT_LPWSTR && value.pwszVal != nullptr) {
        name = value.pwszVal;
    }
    PropVariantClear(&value);
    return name;
}

bool FindTargetEndpoint(TargetEndpoint* target) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(enumerator.put())))) {
        return false;
    }

    ComPtr<IMMDeviceCollection> devices;
    if (FAILED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, devices.put()))) return false;

    UINT count = 0;
    if (FAILED(devices->GetCount(&count))) return false;
    for (UINT index = 0; index < count; ++index) {
        ComPtr<IMMDevice> device;
        if (FAILED(devices->Item(index, device.put()))) continue;
        const std::wstring name = FriendlyName(device.get());
        if (name.find(kTargetName) == std::wstring::npos) continue;

        LPWSTR rawId = nullptr;
        if (FAILED(device->GetId(&rawId)) || rawId == nullptr) continue;
        target->id = rawId;
        CoTaskMemFree(rawId);
        target->name = name;

        const size_t brace = target->id.find_last_of(L'{');
        if (brace == std::wstring::npos) return false;
        target->registryId = target->id.substr(brace);
        return true;
    }
    return false;
}

HRESULT OpenEndpointPropertyStore(const std::wstring& endpointId, DWORD mode,
                                   ComPtr<IPropertyStore>* store) {
    if (store == nullptr) return E_POINTER;
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      IID_PPV_ARGS(enumerator.put()));
    if (FAILED(result)) return result;

    ComPtr<IMMDevice> device;
    result = enumerator->GetDevice(endpointId.c_str(), device.put());
    if (FAILED(result)) return result;
    return device->OpenPropertyStore(mode, store->put());
}

HRESULT ReadEndpointFxValue(const std::wstring& endpointId, std::wstring* value) {
    if (value == nullptr) return E_POINTER;
    ComPtr<IPropertyStore> store;
    HRESULT result = OpenEndpointPropertyStore(endpointId, STGM_READ, &store);
    if (FAILED(result)) return result;

    PROPVARIANT property;
    PropVariantInit(&property);
    result = store->GetValue(PKEY_FX_EndpointEffectClsid, &property);
    if (SUCCEEDED(result)) {
        if (property.vt == VT_LPWSTR && property.pwszVal != nullptr) {
            *value = property.pwszVal;
        } else if (property.vt == VT_BSTR && property.bstrVal != nullptr) {
            *value = property.bstrVal;
        } else {
            result = S_FALSE;
        }
    }
    PropVariantClear(&property);
    return result;
}

HRESULT WriteEndpointFxValue(const std::wstring& endpointId, const std::wstring& value) {
    ComPtr<IPropertyStore> store;
    HRESULT result = OpenEndpointPropertyStore(endpointId, STGM_READWRITE, &store);
    if (FAILED(result)) return result;

    PROPVARIANT property;
    PropVariantInit(&property);
    result = InitPropVariantFromString(value.c_str(), &property);
    if (SUCCEEDED(result)) result = store->SetValue(PKEY_FX_EndpointEffectClsid, property);
    if (SUCCEEDED(result)) result = store->Commit();
    PropVariantClear(&property);
    return result;
}

HRESULT ClearEndpointFxValue(const std::wstring& endpointId) {
    ComPtr<IPropertyStore> store;
    HRESULT result = OpenEndpointPropertyStore(endpointId, STGM_READWRITE, &store);
    if (FAILED(result)) return result;

    PROPVARIANT empty;
    PropVariantInit(&empty);
    result = store->SetValue(PKEY_FX_EndpointEffectClsid, empty);
    if (SUCCEEDED(result)) result = store->Commit();
    PropVariantClear(&empty);
    return result;
}

std::wstring RegistryIdFromEndpointId(const std::wstring& endpointId) {
    const size_t brace = endpointId.find_last_of(L'{');
    return brace == std::wstring::npos ? L"" : endpointId.substr(brace);
}

bool ReadStringValue(HKEY root, const std::wstring& path, const wchar_t* valueName, std::wstring* value) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, path.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) return false;

    DWORD type = 0;
    DWORD bytes = 0;
    LONG result = RegGetValueW(key, nullptr, valueName, RRF_RT_REG_SZ, &type, nullptr, &bytes);
    if (result != ERROR_SUCCESS || bytes < sizeof(wchar_t)) {
        RegCloseKey(key);
        return false;
    }

    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, L'\0');
    result = RegGetValueW(key, nullptr, valueName, RRF_RT_REG_SZ, &type, buffer.data(), &bytes);
    RegCloseKey(key);
    if (result != ERROR_SUCCESS) return false;
    *value = buffer.data();
    return true;
}

bool WriteStringValue(HKEY root, const std::wstring& path, const wchar_t* valueName, const std::wstring& value) {
    HKEY key = nullptr;
    DWORD disposition = 0;
    LONG result = RegCreateKeyExW(root, path.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
                                  KEY_READ | KEY_WRITE, nullptr, &key, &disposition);
    if (result != ERROR_SUCCESS) return false;
    result = RegSetValueExW(key, valueName, 0, REG_SZ,
                            reinterpret_cast<const BYTE*>(value.c_str()),
                            static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

bool DeleteValue(HKEY root, const std::wstring& path, const wchar_t* valueName) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, path.c_str(), 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return true;
    const LONG result = RegDeleteValueW(key, valueName);
    RegCloseKey(key);
    return result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND;
}

bool ReadDwordValue(HKEY root, const std::wstring& path, const wchar_t* valueName, DWORD* value) {
    DWORD bytes = sizeof(*value);
    return RegGetValueW(root, path.c_str(), valueName, RRF_RT_REG_DWORD, nullptr, value, &bytes) == ERROR_SUCCESS;
}

LSTATUS WriteDwordValue(HKEY root, const std::wstring& path, const wchar_t* valueName, DWORD value) {
    return RegSetKeyValueW(root, path.c_str(), valueName, REG_DWORD, &value, sizeof(value));
}

bool DeleteApoSettingsKey() {
    const LONG result = RegDeleteKeyW(HKEY_LOCAL_MACHINE, kMicAudioApoRegistryPath);
    return result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND;
}

bool SaveBackup(const TargetEndpoint& target, const std::wstring& previousValue, bool previousExists) {
    if (!WriteStringValue(HKEY_LOCAL_MACHINE, kMicAudioApoRegistryPath,
                          kMicAudioApoInstalledEndpoint, target.id)) {
        return false;
    }
    if (previousExists) {
        if (!WriteStringValue(HKEY_LOCAL_MACHINE, kMicAudioApoRegistryPath,
                              kMicAudioApoPreviousValue, previousValue)) {
            return false;
        }
    } else {
        DeleteValue(HKEY_LOCAL_MACHINE, kMicAudioApoRegistryPath, kMicAudioApoPreviousValue);
    }
    return WriteDwordValue(HKEY_LOCAL_MACHINE, kMicAudioApoRegistryPath,
                           kMicAudioApoPreviousExists, previousExists ? 1u : 0u) == ERROR_SUCCESS;
}

bool ReadBackup(TargetEndpoint* target, std::wstring* previousValue, bool* previousExists) {
    if (!ReadStringValue(HKEY_LOCAL_MACHINE, kMicAudioApoRegistryPath,
                         kMicAudioApoInstalledEndpoint, &target->id)) {
        return false;
    }
    target->registryId = RegistryIdFromEndpointId(target->id);
    if (target->registryId.empty()) return false;
    DWORD exists = 0;
    ReadDwordValue(HKEY_LOCAL_MACHINE, kMicAudioApoRegistryPath,
                   kMicAudioApoPreviousExists, &exists);
    *previousExists = exists != 0;
    if (*previousExists && !ReadStringValue(HKEY_LOCAL_MACHINE, kMicAudioApoRegistryPath,
                                             kMicAudioApoPreviousValue, previousValue)) {
        return false;
    }
    return true;
}

HRESULT CallDllRegistration(const char* exportName) {
    const std::wstring dllPath = ApoDllPath();
    HMODULE module = LoadLibraryW(dllPath.c_str());
    if (module == nullptr) return HRESULT_FROM_WIN32(GetLastError());
    using RegistrationFunction = HRESULT(WINAPI*)();
    auto function = reinterpret_cast<RegistrationFunction>(GetProcAddress(module, exportName));
    const HRESULT result = function == nullptr ? HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND) : function();
    FreeLibrary(module);
    return result;
}

HRESULT RegisterApoWithAudioEngine() {
    APO_REG_PROPERTIES properties = {};
    properties.clsid = CLSID_MicAudioCleanerApo;
    properties.Flags = APO_FLAG_DEFAULT;
    StringCchCopyW(properties.szFriendlyName, ARRAYSIZE(properties.szFriendlyName),
                   L"MicHotkeyRemapper direct microphone cleaner");
    StringCchCopyW(properties.szCopyrightInfo, ARRAYSIZE(properties.szCopyrightInfo), L"Microck");
    properties.u32MajorVersion = 1;
    properties.u32MinorVersion = 0;
    properties.u32MinInputConnections = 1;
    properties.u32MaxInputConnections = 1;
    properties.u32MinOutputConnections = 1;
    properties.u32MaxOutputConnections = 1;
    properties.u32MaxInstances = ULONG_MAX;
    properties.u32NumAPOInterfaces = 1;
    properties.iidAPOInterfaceList[0] = IID_IMicAudioCleanerApo;
    return RegisterAPO(&properties);
}

HRESULT UnregisterApoFromAudioEngine() {
    return UnregisterAPO(CLSID_MicAudioCleanerApo);
}

void RestoreEndpointProperty() {
    TargetEndpoint backup;
    std::wstring previousValue;
    bool previousExists = false;
    if (!ReadBackup(&backup, &previousValue, &previousExists)) return;

    if (previousExists) {
        WriteEndpointFxValue(backup.id, previousValue);
    } else {
        ClearEndpointFxValue(backup.id);
    }
}

bool InstallApo(UINT highPassHz, UINT gateThresholdCent, std::wstring* error) {
    TargetEndpoint target;
    if (!FindTargetEndpoint(&target)) {
        *error = L"The active Microphone (USB PnP Sound Device) endpoint was not found. Toggle the mic on and retry.";
        return false;
    }
    if (GetFileAttributesW(ApoDllPath().c_str()) == INVALID_FILE_ATTRIBUTES) {
        *error = L"The embedded audio cleaner payload could not be prepared.";
        return false;
    }

    std::wstring installedEndpointId;
    const bool installedOnTarget = ReadStringValue(HKEY_LOCAL_MACHINE, kMicAudioApoRegistryPath,
                                                   kMicAudioApoInstalledEndpoint, &installedEndpointId) &&
                                   installedEndpointId == target.id;
    if (!installedOnTarget) {
        std::wstring currentValue;
        const HRESULT readResult = ReadEndpointFxValue(target.id, &currentValue);
        if (FAILED(readResult)) {
            *error = L"Could not read the USB microphone audio-effect property: " + ErrorText(readResult);
            return false;
        }
        const bool hasCurrentValue = readResult == S_OK && currentValue != kCurrentApoClsid;
        if (!SaveBackup(target, hasCurrentValue && currentValue != kCurrentApoClsid ? currentValue : L"",
                        hasCurrentValue && currentValue != kCurrentApoClsid)) {
            *error = L"Could not save the endpoint's existing audio effect setting.";
            return false;
        }
    }

    HRESULT result = CallDllRegistration("DllRegisterServer");
    if (FAILED(result)) {
        *error = L"COM registration failed: " + ErrorText(result);
        return false;
    }
    result = RegisterApoWithAudioEngine();
    if (FAILED(result)) {
        CallDllRegistration("DllUnregisterServer");
        *error = L"Windows APO registration failed: " + ErrorText(result);
        return false;
    }

    const HRESULT endpointResult = WriteEndpointFxValue(target.id, kCurrentApoClsid);
    const LSTATUS highPassResult = WriteDwordValue(HKEY_LOCAL_MACHINE, kMicAudioApoRegistryPath,
                                                   kMicAudioApoHighPassHz,
                                                   std::clamp(highPassHz, 40u, 300u));
    const LSTATUS gateResult = WriteDwordValue(HKEY_LOCAL_MACHINE, kMicAudioApoRegistryPath,
                                               kMicAudioApoGateThresholdCent,
                                               std::clamp(gateThresholdCent, 120u, 600u));
    if (FAILED(endpointResult) || highPassResult != ERROR_SUCCESS || gateResult != ERROR_SUCCESS) {
        RestoreEndpointProperty();
        UnregisterApoFromAudioEngine();
        CallDllRegistration("DllUnregisterServer");
        if (FAILED(endpointResult)) {
            *error = L"Could not write the USB microphone audio-effect property: " + ErrorText(endpointResult);
        } else if (highPassResult != ERROR_SUCCESS) {
            *error = L"Could not save the high-pass setting: " + ErrorText(HRESULT_FROM_WIN32(highPassResult));
        } else {
            *error = L"Could not save the gate setting: " + ErrorText(HRESULT_FROM_WIN32(gateResult));
        }
        return false;
    }

    return true;
}

bool UninstallApo(std::wstring* error) {
    RestoreEndpointProperty();
    HRESULT result = UnregisterApoFromAudioEngine();
    if (FAILED(result) && result != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) {
        *error = L"Windows APO removal failed: " + ErrorText(result);
        return false;
    }
    result = CallDllRegistration("DllUnregisterServer");
    if (FAILED(result) && result != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) {
        *error = L"COM removal failed: " + ErrorText(result);
        return false;
    }
    DeleteApoSettingsKey();
    return true;
}

bool IsInstalled() {
    TargetEndpoint target;
    std::wstring installedEndpointId;
    if (ReadStringValue(HKEY_LOCAL_MACHINE, kMicAudioApoRegistryPath,
                        kMicAudioApoInstalledEndpoint, &installedEndpointId)) {
        std::wstring current;
        return ReadEndpointFxValue(installedEndpointId, &current) == S_OK && current == kCurrentApoClsid;
    }
    return false;
}

bool LaunchElevated(const std::wstring& parameters) {
    SHELLEXECUTEINFOW execute = {sizeof(execute)};
    execute.fMask = SEE_MASK_NOCLOSEPROCESS;
    execute.lpVerb = L"runas";
    const std::wstring module = ModulePath();
    const std::wstring elevatedParameters = L"--audio-direct " + parameters;
    execute.lpFile = module.c_str();
    execute.lpParameters = elevatedParameters.c_str();
    execute.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&execute) != FALSE;
}

HWND g_window = nullptr;
HWND g_status = nullptr;
HWND g_highPass = nullptr;
HWND g_gate = nullptr;

void SetStatus(const std::wstring& text) {
    if (g_status != nullptr) SetWindowTextW(g_status, text.c_str());
}

bool ReadEditNumber(HWND edit, float minimum, float maximum, float* value) {
    wchar_t buffer[64] = {};
    GetWindowTextW(edit, buffer, ARRAYSIZE(buffer));
    wchar_t* end = nullptr;
    const float parsed = wcstof(buffer, &end);
    if (end == buffer || parsed < minimum || parsed > maximum) return false;
    *value = parsed;
    return true;
}

void RefreshStatus() {
    TargetEndpoint target;
    if (!FindTargetEndpoint(&target)) {
        SetStatus(L"Target mic is currently disconnected. Toggle its button on first.");
        return;
    }
    SetStatus(IsInstalled() ? L"Installed on the original USB PnP microphone endpoint."
                            : L"Target mic detected. Direct cleaner is not installed.");
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_CREATE) {
        HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        HWND title = CreateWindowW(L"STATIC",
            L"Direct-device audio cleaner",
            WS_CHILD | WS_VISIBLE,
            16, 14, 460, 24, window, nullptr, nullptr, nullptr);
        HWND explanation = CreateWindowW(L"STATIC",
            L"Processes the original USB microphone endpoint. Administrator permission is required.",
            WS_CHILD | WS_VISIBLE,
            16, 42, 460, 40, window, nullptr, nullptr, nullptr);
        HWND highPassLabel = CreateWindowW(L"STATIC", L"High-pass (Hz):", WS_CHILD | WS_VISIBLE,
                                           16, 92, 130, 24, window, nullptr, nullptr, nullptr);
        g_highPass = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"90",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER,
                                     150, 88, 80, 26, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kHighPassEdit)), nullptr, nullptr);
        HWND gateLabel = CreateWindowW(L"STATIC", L"Gate threshold (x noise):", WS_CHILD | WS_VISIBLE,
                                       250, 92, 160, 24, window, nullptr, nullptr, nullptr);
        g_gate = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"2.50",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                 415, 88, 65, 26, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kGateEdit)), nullptr, nullptr);
        HWND install = CreateWindowW(L"BUTTON", L"Install direct cleaner",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                     16, 132, 180, 30, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kInstallButton)), nullptr, nullptr);
        HWND uninstall = CreateWindowW(L"BUTTON", L"Uninstall",
                                       WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                       206, 132, 100, 30, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kUninstallButton)), nullptr, nullptr);
        g_status = CreateWindowW(L"STATIC", L"Checking target mic...", WS_CHILD | WS_VISIBLE,
                                 16, 178, 460, 42, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStatusText)), nullptr, nullptr);
        for (HWND control : {title, explanation, highPassLabel, g_highPass, gateLabel, g_gate, install, uninstall, g_status}) {
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        }
        DWORD savedHighPass = 0;
        if (ReadDwordValue(HKEY_LOCAL_MACHINE, kMicAudioApoRegistryPath,
                           kMicAudioApoHighPassHz, &savedHighPass)) {
            wchar_t highPassText[16] = {};
            swprintf_s(highPassText, L"%u",
                       std::clamp<DWORD>(savedHighPass, 40, 300));
            SetWindowTextW(g_highPass, highPassText);
        }
        DWORD savedGateCent = 0;
        if (ReadDwordValue(HKEY_LOCAL_MACHINE, kMicAudioApoRegistryPath,
                           kMicAudioApoGateThresholdCent, &savedGateCent)) {
            wchar_t gateText[16] = {};
            swprintf_s(gateText, L"%.2f",
                       static_cast<float>(std::clamp<DWORD>(savedGateCent, 120, 600)) / 100.0f);
            SetWindowTextW(g_gate, gateText);
        }
        RefreshStatus();
        return 0;
    }

    if (message == WM_COMMAND && HIWORD(wParam) == BN_CLICKED) {
        if (LOWORD(wParam) == kInstallButton) {
            float highPass = 0.0f;
            float gate = 0.0f;
            if (!ReadEditNumber(g_highPass, 40.0f, 300.0f, &highPass) ||
                !ReadEditNumber(g_gate, 1.2f, 6.0f, &gate)) {
                MessageBoxW(window, L"Use high-pass 40-300 and gate threshold 1.2-6.0.",
                            L"Invalid settings", MB_OK | MB_ICONWARNING);
                return 0;
            }
            wchar_t parameters[128] = {};
            swprintf_s(parameters, L"/install /highpass %u /gate %u",
                       static_cast<UINT>(highPass), static_cast<UINT>(gate * 100.0f + 0.5f));
            if (LaunchElevated(parameters)) SetStatus(L"Administrator installer started. Close and reopen audio apps afterward.");
            else SetStatus(L"Installation was canceled or could not be elevated.");
            return 0;
        }
        if (LOWORD(wParam) == kUninstallButton) {
            if (LaunchElevated(L"/uninstall")) SetStatus(L"Administrator uninstaller started.");
            else SetStatus(L"Uninstallation was canceled or could not be elevated.");
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

int RunGui(HINSTANCE instance) {
    WNDCLASSW windowClass = {};
    windowClass.hInstance = instance;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.lpszClassName = kWindowClass;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    if (!RegisterClassW(&windowClass)) return 1;

    g_window = CreateWindowExW(0, kWindowClass, L"Mic Audio Cleaner",
                               WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                               CW_USEDEFAULT, CW_USEDEFAULT, 520, 270,
                               nullptr, nullptr, instance, nullptr);
    if (g_window == nullptr) return 1;
    ShowWindow(g_window, SW_SHOW);
    UpdateWindow(g_window);

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

bool GetArgumentValue(int argc, wchar_t** argv, const wchar_t* name, UINT* value) {
    for (int index = 1; index + 1 < argc; ++index) {
        if (_wcsicmp(argv[index], name) != 0) continue;
        wchar_t* end = nullptr;
        unsigned long parsed = wcstoul(argv[index + 1], &end, 10);
        if (end == argv[index + 1] || parsed > 100000) return false;
        *value = static_cast<UINT>(parsed);
        return true;
    }
    return false;
}

} // namespace

int RunAudioApoCommandLine(HINSTANCE instance) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int exitCode = 0;
    bool install = false;
    bool uninstall = false;
    for (int index = 1; index < argc; ++index) {
        install = install || _wcsicmp(argv[index], L"/install") == 0;
        uninstall = uninstall || _wcsicmp(argv[index], L"/uninstall") == 0;
    }
    if (install) {
        UINT highPass = kDefaultHighPassHz;
        UINT gate = kDefaultGateThresholdCent;
        GetArgumentValue(argc, argv, L"/highpass", &highPass);
        GetArgumentValue(argc, argv, L"/gate", &gate);
        std::wstring error;
        if (InstallApo(highPass, gate, &error)) {
            MessageBoxW(nullptr,
                        L"The direct cleaner was installed. Close and reopen the STT application so it opens a new audio graph.",
                        L"Mic Audio Cleaner", MB_OK | MB_ICONINFORMATION);
        } else {
            MessageBoxW(nullptr, error.c_str(), L"Mic Audio Cleaner", MB_OK | MB_ICONERROR);
            exitCode = 1;
        }
    } else if (uninstall) {
        std::wstring error;
        if (UninstallApo(&error)) {
            MessageBoxW(nullptr, L"The direct cleaner was uninstalled. Close and reopen audio applications.",
                        L"Mic Audio Cleaner", MB_OK | MB_ICONINFORMATION);
        } else {
            MessageBoxW(nullptr, error.c_str(), L"Mic Audio Cleaner", MB_OK | MB_ICONERROR);
            exitCode = 1;
        }
    } else {
        exitCode = RunGui(instance);
    }

    if (argv != nullptr) LocalFree(argv);
    return exitCode;
}

int RunAudioApoGui(HINSTANCE instance) {
    return RunGui(instance);
}

void SetEmbeddedApoDllPath(const wchar_t* path) {
    g_embeddedApoDllPath = path == nullptr ? L"" : path;
}

#ifndef MIC_HOTKEY_REMAPPER_EMBEDDED
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(result)) return 1;
    const int exitCode = RunAudioApoCommandLine(instance);
    CoUninitialize();
    return exitCode;
}
#endif
