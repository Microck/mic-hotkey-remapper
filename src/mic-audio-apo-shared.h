#pragma once

#include <guiddef.h>

// The CLSID is used both by COM and by the Windows audio endpoint FxProperties
// store. Keep it stable because an installed endpoint points at this identifier.
DEFINE_GUID(CLSID_MicAudioCleanerApo,
    0xa1d5e6f4, 0x4e58, 0x4d67, 0xa4, 0x2a, 0x51, 0x14, 0xa9, 0xb4, 0xe1, 0x77);

DEFINE_GUID(IID_IMicAudioCleanerApo,
    0x6c4f3c8a, 0x7e21, 0x4b2d, 0x9f, 0x73, 0x28, 0x91, 0x65, 0x42, 0xc0, 0x3e);

constexpr wchar_t kMicAudioApoRegistryPath[] =
    L"SOFTWARE\\MicHotkeyRemapper\\AudioApo";
constexpr wchar_t kMicAudioApoInstalledEndpoint[] = L"EndpointRegistryId";
constexpr wchar_t kMicAudioApoPreviousValue[] = L"PreviousFxValue";
constexpr wchar_t kMicAudioApoPreviousExists[] = L"PreviousFxValueExists";
constexpr wchar_t kMicAudioApoHighPassHz[] = L"HighPassHz";
constexpr wchar_t kMicAudioApoGateThresholdCent[] = L"GateThresholdCent";
