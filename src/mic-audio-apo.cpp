#define UNICODE
#define _UNICODE
#define NOMINMAX

#include <windows.h>
#include <initguid.h>

#include <audioenginebaseapo.h>
#include <baseaudioprocessingobject.h>
#include <ksmedia.h>
#include <strsafe.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <new>
#include <string>

#include "mic-audio-apo-shared.h"

DEFINE_GUID(IID_IAudioProcessingObject,
    0xfd7f2b29, 0x24d0, 0x4b5c, 0xb1, 0x77, 0x59, 0x2c, 0x39, 0xf9, 0xca, 0x10);
DEFINE_GUID(IID_IAudioProcessingObjectRT,
    0x9e1d6a6d, 0xddbc, 0x4e95, 0xa4, 0xc7, 0xad, 0x64, 0xba, 0x37, 0x84, 0x6c);
DEFINE_GUID(IID_IAudioProcessingObjectConfiguration,
    0x0e5ed805, 0xaba6, 0x49c3, 0x8f, 0x9a, 0x2b, 0x8c, 0x88, 0x9c, 0x4f, 0xa8);

#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "AudioBaseProcessingObjectV140.lib")
#pragma comment(lib, "AudioEng.lib")
#pragma comment(lib, "audiomediatypecrt.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "OleAut32.lib")
#pragma comment(lib, "User32.lib")
#pragma comment(lib, "Uuid.lib")
namespace {

constexpr UINT kMaxChannels = 8;
constexpr float kPi = 3.14159265358979323846f;
constexpr UINT kDefaultHighPassHz = 90;
constexpr UINT kDefaultGateThresholdCent = 250;
constexpr UINT kDefaultNoiseReductionPercent = 82;
constexpr UINT kFftSize = 256;
constexpr UINT kHopSize = kFftSize / 2;

struct Biquad {
    void Configure(float sampleRate, float frequency, float quality) noexcept {
        const float omega = 2.0f * kPi * frequency / sampleRate;
        const float alpha = std::sin(omega) / (2.0f * quality);
        const float cosine = std::cos(omega);
        const float a0 = 1.0f + alpha;
        b0 = 1.0f / a0;
        b1 = -2.0f * cosine / a0;
        b2 = b0;
        a1 = b1;
        a2 = (1.0f - alpha) / a0;
    }

    float Process(float input) noexcept {
        const float output = b0 * input + b1 * input1 + b2 * input2 - a1 * output1 - a2 * output2;
        input2 = input1;
        input1 = input;
        output2 = output1;
        output1 = output;
        return output;
    }

    float b0 = 1.0f;
    float b1 = 0.0f;
    float b2 = 0.0f;
    float a1 = 0.0f;
    float a2 = 0.0f;
    float input1 = 0.0f;
    float input2 = 0.0f;
    float output1 = 0.0f;
    float output2 = 0.0f;
};

void FourierTransform(std::array<std::complex<float>, kFftSize>& values, bool inverse) noexcept {
    for (UINT index = 1, bitReversed = 0; index < kFftSize; ++index) {
        UINT bit = kFftSize >> 1;
        for (; bitReversed & bit; bit >>= 1) bitReversed ^= bit;
        bitReversed ^= bit;
        if (index < bitReversed) std::swap(values[index], values[bitReversed]);
    }

    for (UINT length = 2; length <= kFftSize; length <<= 1) {
        const float angle = 2.0f * kPi / static_cast<float>(length) * (inverse ? 1.0f : -1.0f);
        const std::complex<float> step(std::cos(angle), std::sin(angle));
        for (UINT start = 0; start < kFftSize; start += length) {
            std::complex<float> current(1.0f, 0.0f);
            for (UINT offset = 0; offset < length / 2; ++offset) {
                const std::complex<float> even = values[start + offset];
                const std::complex<float> odd = current * values[start + offset + length / 2];
                values[start + offset] = even + odd;
                values[start + offset + length / 2] = even - odd;
                current *= step;
            }
        }
    }

    if (inverse) {
        for (auto& value : values) value /= static_cast<float>(kFftSize);
    }
}

class SpectralDenoiser {
public:
    void Configure(UINT sampleRate, float noiseReduction, float gateThreshold) noexcept {
        sampleRate_ = std::max<UINT>(1, sampleRate);
        suppressionStrength_ = std::clamp(noiseReduction / 100.0f, 0.0f, 1.0f);
        gateThreshold_ = std::clamp(gateThreshold, 1.2f, 6.0f);
        for (UINT index = 0; index < kFftSize; ++index) {
            window_[index] = 0.5f - 0.5f * std::cos(2.0f * kPi * static_cast<float>(index) /
                                                     static_cast<float>(kFftSize));
        }
        Reset();
    }

    void Reset() noexcept {
        input_.fill(0.0f);
        overlap_.fill(0.0f);
        output_.fill(0.0f);
        noiseMagnitudes_.fill(0.001f);
        inputCount_ = 0;
        outputRead_ = 0;
        outputWrite_ = 0;
        outputCount_ = 0;
        noiseFloor_ = 0.01f;
        gateGain_ = 0.04f;
        calibrationBlocks_ = std::max<UINT>(1, sampleRate_ / kHopSize);
    }

    float Process(float sample) noexcept {
        input_[inputCount_++] = sample;
        if (inputCount_ == kFftSize) ProcessFrame();
        if (outputCount_ == 0) return 0.0f;

        const float result = output_[outputRead_];
        outputRead_ = (outputRead_ + 1) % static_cast<UINT>(output_.size());
        --outputCount_;
        return result;
    }

private:
    void Enqueue(float value) noexcept {
        if (outputCount_ == output_.size()) {
            outputRead_ = (outputRead_ + 1) % static_cast<UINT>(output_.size());
            --outputCount_;
        }
        output_[outputWrite_] = value;
        outputWrite_ = (outputWrite_ + 1) % static_cast<UINT>(output_.size());
        ++outputCount_;
    }

    void ProcessFrame() noexcept {
        float blockPower = 0.0f;
        for (UINT index = 0; index < kFftSize; ++index) blockPower += input_[index] * input_[index];
        const float blockRms = std::sqrt(blockPower / static_cast<float>(kFftSize));
        const bool quiet = blockRms < noiseFloor_ * gateThreshold_;

        if (calibrationBlocks_ > 0) {
            noiseFloor_ = noiseFloor_ * 0.98f + blockRms * 0.02f;
            --calibrationBlocks_;
        } else if (quiet) {
            noiseFloor_ = noiseFloor_ * 0.995f + blockRms * 0.005f;
        }

        for (UINT index = 0; index < kFftSize; ++index) {
            spectrum_[index] = input_[index] * window_[index];
        }
        FourierTransform(spectrum_, false);

        for (UINT index = 0; index < kFftSize; ++index) {
            const float magnitude = std::abs(spectrum_[index]);
            if (calibrationBlocks_ > 0 || quiet) {
                noiseMagnitudes_[index] = noiseMagnitudes_[index] * 0.97f + magnitude * 0.03f;
            }
            const float power = magnitude * magnitude;
            const float noisePower = noiseMagnitudes_[index] * noiseMagnitudes_[index];
            const float ratio = noisePower / std::max(power, 0.0000001f);
            const float gain = std::clamp(1.0f - suppressionStrength_ * ratio, 0.12f, 1.0f);
            spectrum_[index] *= gain;
        }
        FourierTransform(spectrum_, true);

        const float targetGate = quiet ? 0.04f : 1.0f;
        for (UINT index = 0; index < kHopSize; ++index) {
            const float scale = 4.0f / 3.0f;
            const float firstHalf = spectrum_[index].real() * window_[index] * scale + overlap_[index];
            overlap_[index] = spectrum_[index + kHopSize].real() * window_[index + kHopSize] * scale;
            const float smoothing = targetGate > gateGain_ ? 0.12f : 0.025f;
            gateGain_ += (targetGate - gateGain_) * smoothing;
            Enqueue(firstHalf * gateGain_);
        }

        for (UINT index = 0; index < kHopSize; ++index) input_[index] = input_[index + kHopSize];
        inputCount_ = kHopSize;
    }

    UINT sampleRate_ = 48000;
    float suppressionStrength_ = static_cast<float>(kDefaultNoiseReductionPercent) / 100.0f;
    float gateThreshold_ = static_cast<float>(kDefaultGateThresholdCent) / 100.0f;
    float noiseFloor_ = 0.01f;
    float gateGain_ = 0.04f;
    UINT calibrationBlocks_ = 1;
    UINT inputCount_ = 0;
    UINT outputRead_ = 0;
    UINT outputWrite_ = 0;
    UINT outputCount_ = 0;
    std::array<float, kFftSize> input_{};
    std::array<float, kFftSize> window_{};
    std::array<float, kHopSize> overlap_{};
    std::array<float, kFftSize> noiseMagnitudes_{};
    std::array<float, kFftSize> output_{};
    std::array<std::complex<float>, kFftSize> spectrum_{};
};

struct ChannelState {
    Biquad notch50;
    Biquad notch100;
    SpectralDenoiser denoiser;
    float highPassState = 0.0f;
    float previousInput = 0.0f;
};

MIDL_INTERFACE("6C4F3C8A-7E21-4B2D-9F73-28916542C03E")
IMicAudioCleanerApo : IUnknown {
};

volatile LONG g_liveObjects = 0;
HMODULE g_moduleInstance = nullptr;

class __declspec(uuid("A1D5E6F4-4E58-4D67-A42A-5114A9B4E177")) CMicAudioCleanerApo final :
    public CBaseAudioProcessingObject,
    public IMicAudioCleanerApo {
public:
    CMicAudioCleanerApo()
        : CBaseAudioProcessingObject(sm_RegProperties), referenceCount_(1) {
        InterlockedIncrement(&g_liveObjects);
    }

    ~CMicAudioCleanerApo() override {
        InterlockedDecrement(&g_liveObjects);
    }

    STDMETHOD(QueryInterface)(REFIID iid, void** output) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;

    STDMETHOD_(void, APOProcess)(UINT32 inputCount,
        APO_CONNECTION_PROPERTY** inputs,
        UINT32 outputCount,
        APO_CONNECTION_PROPERTY** outputs) noexcept;
    STDMETHOD(GetLatency)(HNSTIME* time);
    STDMETHOD(LockForProcess)(UINT32 inputCount,
        APO_CONNECTION_DESCRIPTOR** inputDescriptors,
        UINT32 outputCount,
        APO_CONNECTION_DESCRIPTOR** outputDescriptors);
    STDMETHOD(Initialize)(UINT32 dataSize, BYTE* data);

    static const CRegAPOProperties<1> sm_RegProperties;

private:
    void LoadSettings() noexcept;
    void ConfigureState() noexcept;

    std::array<ChannelState, kMaxChannels> channels_{};
    UINT sampleRate_ = 48000;
    UINT channelCount_ = 1;
    float highPassHz_ = static_cast<float>(kDefaultHighPassHz);
    float gateThreshold_ = static_cast<float>(kDefaultGateThresholdCent) / 100.0f;
    float noiseReduction_ = static_cast<float>(kDefaultNoiseReductionPercent);
    float highPassAlpha_ = 0.0f;
    volatile LONG referenceCount_;
};

const AVRT_DATA CRegAPOProperties<1> CMicAudioCleanerApo::sm_RegProperties(
    CLSID_MicAudioCleanerApo,
    L"MicHotkeyRemapper direct microphone cleaner",
    L"Microck",
    1,
    0,
    IID_IAudioProcessingObject);

void CMicAudioCleanerApo::LoadSettings() noexcept {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kMicAudioApoRegistryPath, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return;
    }

    DWORD value = 0;
    DWORD size = sizeof(value);
    if (RegGetValueW(key, nullptr, kMicAudioApoHighPassHz, RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS) {
        highPassHz_ = static_cast<float>(std::clamp(value, static_cast<DWORD>(40), static_cast<DWORD>(300)));
    }
    value = kDefaultGateThresholdCent;
    size = sizeof(value);
    if (RegGetValueW(key, nullptr, kMicAudioApoGateThresholdCent, RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS) {
        gateThreshold_ = std::clamp(static_cast<float>(value) / 100.0f, 1.2f, 6.0f);
    }
    value = kDefaultNoiseReductionPercent;
    size = sizeof(value);
    if (RegGetValueW(key, nullptr, kMicAudioApoNoiseReductionPercent, RRF_RT_REG_DWORD,
                     nullptr, &value, &size) == ERROR_SUCCESS) {
        noiseReduction_ = static_cast<float>(std::clamp(value, static_cast<DWORD>(0), static_cast<DWORD>(100)));
    }
    RegCloseKey(key);
}

void CMicAudioCleanerApo::ConfigureState() noexcept {
    sampleRate_ = std::max<UINT>(1, static_cast<UINT>(GetFramesPerSecond()));
    channelCount_ = std::min<UINT>(GetSamplesPerFrame(), kMaxChannels);
    const float cutoff = std::clamp(highPassHz_, 40.0f, 300.0f);
    highPassAlpha_ = (2.0f * kPi * cutoff) / (2.0f * kPi * cutoff + static_cast<float>(sampleRate_));
    for (UINT channel = 0; channel < kMaxChannels; ++channel) {
        channels_[channel] = {};
        channels_[channel].notch50.Configure(static_cast<float>(sampleRate_), 50.0f, 10.0f);
        channels_[channel].notch100.Configure(static_cast<float>(sampleRate_), 100.0f, 10.0f);
        channels_[channel].denoiser.Configure(sampleRate_, noiseReduction_, gateThreshold_);
    }
}

STDMETHODIMP CMicAudioCleanerApo::Initialize(UINT32 dataSize, BYTE* data) {
    if (m_bIsInitialized) return APOERR_ALREADY_INITIALIZED;
    if (data == nullptr || dataSize < sizeof(APOInitBaseStruct)) return E_INVALIDARG;

    const auto* initialization = reinterpret_cast<const APOInitBaseStruct*>(data);
    if (!IsEqualGUID(initialization->clsid, CLSID_MicAudioCleanerApo)) return APOERR_INVALID_APO_CLSID;

    if (dataSize != sizeof(APOInitBaseStruct) &&
        dataSize != sizeof(APOInitSystemEffects) &&
        dataSize != sizeof(APOInitSystemEffects2)) {
        return E_INVALIDARG;
    }

    LoadSettings();
    m_bIsInitialized = true;
    return S_OK;
}

STDMETHODIMP CMicAudioCleanerApo::QueryInterface(REFIID iid, void** output) {
    if (output == nullptr) return E_POINTER;
    *output = nullptr;
    if (iid == IID_IUnknown || iid == IID_IMicAudioCleanerApo) {
        *output = static_cast<IMicAudioCleanerApo*>(this);
    } else if (iid == IID_IAudioProcessingObject) {
        *output = static_cast<IAudioProcessingObject*>(this);
    } else if (iid == IID_IAudioProcessingObjectRT) {
        *output = static_cast<IAudioProcessingObjectRT*>(this);
    } else if (iid == IID_IAudioProcessingObjectConfiguration) {
        *output = static_cast<IAudioProcessingObjectConfiguration*>(this);
    } else {
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) CMicAudioCleanerApo::AddRef() {
    return static_cast<ULONG>(InterlockedIncrement(&referenceCount_));
}

STDMETHODIMP_(ULONG) CMicAudioCleanerApo::Release() {
    const ULONG remaining = static_cast<ULONG>(InterlockedDecrement(&referenceCount_));
    if (remaining == 0) delete this;
    return remaining;
}

STDMETHODIMP CMicAudioCleanerApo::LockForProcess(
    UINT32 inputCount,
    APO_CONNECTION_DESCRIPTOR** inputDescriptors,
    UINT32 outputCount,
    APO_CONNECTION_DESCRIPTOR** outputDescriptors) {
    HRESULT result = CBaseAudioProcessingObject::LockForProcess(
        inputCount, inputDescriptors, outputCount, outputDescriptors);
    if (FAILED(result)) return result;
    ConfigureState();
    return S_OK;
}

STDMETHODIMP CMicAudioCleanerApo::GetLatency(HNSTIME* time) {
    if (time == nullptr) return E_POINTER;
    *time = 0;
    return S_OK;
}

#pragma AVRT_CODE_BEGIN
STDMETHODIMP_(void) CMicAudioCleanerApo::APOProcess(
    UINT32 inputCount,
    APO_CONNECTION_PROPERTY** inputs,
    UINT32 outputCount,
    APO_CONNECTION_PROPERTY** outputs) noexcept {
    if (inputCount == 0 || outputCount == 0 || inputs == nullptr || outputs == nullptr ||
        inputs[0] == nullptr || outputs[0] == nullptr) {
        return;
    }

    APO_CONNECTION_PROPERTY* input = inputs[0];
    APO_CONNECTION_PROPERTY* output = outputs[0];
    output->u32ValidFrameCount = input->u32ValidFrameCount;

    auto* destination = reinterpret_cast<float*>(output->pBuffer);
    const auto* source = reinterpret_cast<const float*>(input->pBuffer);
    if (destination == nullptr || source == nullptr) return;

    const UINT channels = std::max<UINT>(1, channelCount_);
    if (input->u32BufferFlags == BUFFER_SILENT) {
        ZeroMemory(destination, sizeof(float) * input->u32ValidFrameCount * channels);
        for (UINT channel = 0; channel < channels; ++channel) {
            channels_[std::min(channel, kMaxChannels - 1)].denoiser.Reset();
        }
        output->u32BufferFlags = BUFFER_SILENT;
        return;
    }
    if (input->u32BufferFlags != BUFFER_VALID) {
        output->u32BufferFlags = input->u32BufferFlags;
        return;
    }

    for (UINT32 frame = 0; frame < input->u32ValidFrameCount; ++frame) {
        for (UINT channel = 0; channel < channels; ++channel) {
            ChannelState& state = channels_[std::min(channel, kMaxChannels - 1)];
            const float sample = source[frame * channels + channel];
            const float highPassed = highPassAlpha_ * (state.highPassState + sample - state.previousInput);
            state.previousInput = sample;
            state.highPassState = highPassed;
            float cleaned = state.notch50.Process(highPassed);
            cleaned = state.notch100.Process(cleaned);
            destination[frame * channels + channel] = state.denoiser.Process(cleaned);
        }
    }
    output->u32BufferFlags = BUFFER_VALID;
}
#pragma AVRT_CODE_END

class MicAudioApoClassFactory final : public IClassFactory {
public:
    MicAudioApoClassFactory() : referenceCount_(1) {}

    STDMETHOD(QueryInterface)(REFIID iid, void** output) override {
        if (output == nullptr) return E_POINTER;
        *output = nullptr;
        if (iid == IID_IUnknown || iid == IID_IClassFactory) {
            *output = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    STDMETHOD_(ULONG, AddRef)() override {
        return static_cast<ULONG>(InterlockedIncrement(&referenceCount_));
    }

    STDMETHOD_(ULONG, Release)() override {
        const ULONG remaining = static_cast<ULONG>(InterlockedDecrement(&referenceCount_));
        if (remaining == 0) delete this;
        return remaining;
    }

    STDMETHOD(CreateInstance)(IUnknown* outer, REFIID iid, void** output) override {
        if (outer != nullptr) return CLASS_E_NOAGGREGATION;
        if (output == nullptr) return E_POINTER;
        *output = nullptr;
        auto* object = new (std::nothrow) CMicAudioCleanerApo();
        if (object == nullptr) return E_OUTOFMEMORY;
        const HRESULT result = object->QueryInterface(iid, output);
        object->Release();
        return result;
    }

    STDMETHOD(LockServer)(BOOL lock) override {
        if (lock) InterlockedIncrement(&g_liveObjects);
        else InterlockedDecrement(&g_liveObjects);
        return S_OK;
    }

private:
    volatile LONG referenceCount_;
};

std::wstring ClassIdPath() {
    wchar_t guid[64] = {};
    StringFromGUID2(CLSID_MicAudioCleanerApo, guid, ARRAYSIZE(guid));
    return L"SOFTWARE\\Classes\\CLSID\\" + std::wstring(guid);
}

} // namespace

extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    UNREFERENCED_PARAMETER(reserved);
    if (reason == DLL_PROCESS_ATTACH) {
        g_moduleInstance = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}

extern "C" HRESULT WINAPI DllCanUnloadNow() {
    return g_liveObjects == 0 ? S_OK : S_FALSE;
}

extern "C" HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID iid, LPVOID* output) {
    if (!IsEqualGUID(clsid, CLSID_MicAudioCleanerApo)) return CLASS_E_CLASSNOTAVAILABLE;
    auto* factory = new (std::nothrow) MicAudioApoClassFactory();
    if (factory == nullptr) return E_OUTOFMEMORY;
    const HRESULT result = factory->QueryInterface(iid, output);
    factory->Release();
    return result;
}

extern "C" HRESULT WINAPI DllRegisterServer() {
    wchar_t modulePath[MAX_PATH] = {};
    if (GetModuleFileNameW(g_moduleInstance, modulePath, ARRAYSIZE(modulePath)) == 0) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    HKEY key = nullptr;
    const std::wstring path = ClassIdPath() + L"\\InprocServer32";
    LONG result = RegCreateKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
                                  KEY_SET_VALUE, nullptr, &key, nullptr);
    if (result != ERROR_SUCCESS) return HRESULT_FROM_WIN32(result);
    result = RegSetValueExW(key, nullptr, 0, REG_SZ,
                            reinterpret_cast<const BYTE*>(modulePath),
                            static_cast<DWORD>((wcslen(modulePath) + 1) * sizeof(wchar_t)));
    if (result == ERROR_SUCCESS) {
        const wchar_t threadingModel[] = L"Both";
        result = RegSetValueExW(key, L"ThreadingModel", 0, REG_SZ,
                                reinterpret_cast<const BYTE*>(threadingModel), sizeof(threadingModel));
    }
    RegCloseKey(key);
    return result == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(result);
}

extern "C" HRESULT WINAPI DllUnregisterServer() {
    const std::wstring path = ClassIdPath();
    const LONG result = RegDeleteTreeW(HKEY_LOCAL_MACHINE, path.c_str());
    return result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND ? S_OK : HRESULT_FROM_WIN32(result);
}
