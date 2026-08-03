#define UNICODE
#define _UNICODE
#define NOMINMAX

#include <windows.h>
#include <audioclient.h>
#include <avrt.h>
#include <propkey.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <propvarutil.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cwctype>
#include <cstring>
#include <cwchar>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "Avrt.lib")
#pragma comment(lib, "Gdi32.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Propsys.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "User32.lib")

namespace {

constexpr wchar_t kWindowClass[] = L"MicAudioCleaner.Window";
constexpr wchar_t kConfigSection[] = L"audio";
constexpr int kFftSize = 256;
constexpr int kHopSize = kFftSize / 2;
constexpr float kPi = 3.14159265358979323846f;

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
    explicit operator bool() const { return value_ != nullptr; }

    void reset() {
        if (value_ != nullptr) value_->Release();
        value_ = nullptr;
    }

private:
    T* value_ = nullptr;
};

template <typename T>
struct CoTaskDeleter {
    void operator()(T* value) const {
        if (value != nullptr) CoTaskMemFree(value);
    }
};

template <typename T>
using CoTaskPtr = std::unique_ptr<T, CoTaskDeleter<T>>;

class UniqueHandle {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE value) : value_(value) {}
    ~UniqueHandle() { reset(); }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    HANDLE* put() {
        reset();
        return &value_;
    }

    HANDLE get() const { return value_; }
    bool valid() const { return value_ != nullptr && value_ != INVALID_HANDLE_VALUE; }
    void reset(HANDLE value = nullptr) {
        if (valid()) CloseHandle(value_);
        value_ = value;
    }

private:
    HANDLE value_ = nullptr;
};

struct Endpoint {
    std::wstring id;
    std::wstring name;
};

struct AudioFormat {
    UINT sampleRate = 48000;
    UINT channels = 1;
    UINT blockAlign = 2;
    UINT bytesPerSample = 2;
    bool isFloat = false;
};

struct CleanerSettings {
    std::wstring inputId;
    std::wstring outputId;
    float highPassHz = 90.0f;
    float noiseReduction = 82.0f;
    float gateThreshold = 2.5f;
};

std::wstring HResultText(HRESULT result) {
    wchar_t* buffer = nullptr;
    DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                  nullptr, static_cast<DWORD>(result), 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring message = length == 0 ? L"Unknown Windows error" : std::wstring(buffer, length);
    if (buffer != nullptr) LocalFree(buffer);
    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' ')) message.pop_back();
    return message;
}

std::wstring AppDataPath() {
    wchar_t buffer[MAX_PATH] = {};
    DWORD length = GetEnvironmentVariableW(L"APPDATA", buffer, ARRAYSIZE(buffer));
    if (length == 0 || length >= ARRAYSIZE(buffer)) return L".";
    return std::wstring(buffer, length) + L"\\MicHotkeyRemapper";
}

std::wstring ConfigPath() {
    return AppDataPath() + L"\\audio-cleaner.ini";
}

void EnsureConfigDirectory() {
    std::wstring path = AppDataPath();
    CreateDirectoryW(path.c_str(), nullptr);
}

CleanerSettings LoadSettings() {
    CleanerSettings settings;
    wchar_t input[8192] = {};
    wchar_t output[8192] = {};
    wchar_t highPass[64] = {};
    wchar_t noiseReduction[64] = {};
    wchar_t gateThreshold[64] = {};
    GetPrivateProfileStringW(kConfigSection, L"input", L"", input, ARRAYSIZE(input), ConfigPath().c_str());
    GetPrivateProfileStringW(kConfigSection, L"output", L"", output, ARRAYSIZE(output), ConfigPath().c_str());
    GetPrivateProfileStringW(kConfigSection, L"highPassHz", L"90", highPass, ARRAYSIZE(highPass), ConfigPath().c_str());
    GetPrivateProfileStringW(kConfigSection, L"noiseReduction", L"82", noiseReduction, ARRAYSIZE(noiseReduction), ConfigPath().c_str());
    GetPrivateProfileStringW(kConfigSection, L"gateThreshold", L"2.5", gateThreshold, ARRAYSIZE(gateThreshold), ConfigPath().c_str());
    settings.inputId = input;
    settings.outputId = output;
    settings.highPassHz = std::clamp(static_cast<float>(wcstof(highPass, nullptr)), 40.0f, 300.0f);
    settings.noiseReduction = std::clamp(static_cast<float>(wcstof(noiseReduction, nullptr)), 0.0f, 100.0f);
    settings.gateThreshold = std::clamp(static_cast<float>(wcstof(gateThreshold, nullptr)), 1.2f, 6.0f);
    return settings;
}

void WriteFloatSetting(const wchar_t* key, float value) {
    wchar_t text[32] = {};
    swprintf_s(text, L"%.2f", value);
    WritePrivateProfileStringW(kConfigSection, key, text, ConfigPath().c_str());
}

void SaveSettings(const CleanerSettings& settings) {
    EnsureConfigDirectory();
    WritePrivateProfileStringW(kConfigSection, L"input", settings.inputId.c_str(), ConfigPath().c_str());
    WritePrivateProfileStringW(kConfigSection, L"output", settings.outputId.c_str(), ConfigPath().c_str());
    WriteFloatSetting(L"highPassHz", settings.highPassHz);
    WriteFloatSetting(L"noiseReduction", settings.noiseReduction);
    WriteFloatSetting(L"gateThreshold", settings.gateThreshold);
}

std::wstring FriendlyName(IMMDevice* device) {
    ComPtr<IPropertyStore> properties;
    if (FAILED(device->OpenPropertyStore(STGM_READ, properties.put()))) return L"Unknown audio device";
    PROPVARIANT value;
    PropVariantInit(&value);
    std::wstring name = L"Unknown audio device";
    if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR && value.pwszVal != nullptr) {
        name = value.pwszVal;
    }
    PropVariantClear(&value);
    return name;
}

std::vector<Endpoint> EnumerateEndpoints(EDataFlow flow) {
    std::vector<Endpoint> endpoints;
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(enumerator.put())))) return endpoints;

    ComPtr<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, collection.put()))) return endpoints;

    UINT count = 0;
    if (FAILED(collection->GetCount(&count))) return endpoints;
    for (UINT index = 0; index < count; ++index) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(index, device.put()))) continue;
        LPWSTR rawId = nullptr;
        if (FAILED(device->GetId(&rawId))) continue;
        Endpoint endpoint;
        endpoint.id = rawId;
        endpoint.name = FriendlyName(device.get());
        CoTaskMemFree(rawId);
        endpoints.push_back(std::move(endpoint));
    }
    return endpoints;
}

bool IsFloatFormat(const WAVEFORMATEX* format) {
    if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        return IsEqualGUID(extensible->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != FALSE;
    }
    return false;
}

AudioFormat DescribeFormat(const WAVEFORMATEX* format) {
    AudioFormat description;
    description.sampleRate = format->nSamplesPerSec;
    description.channels = format->nChannels;
    description.blockAlign = format->nBlockAlign;
    description.bytesPerSample = format->nBlockAlign / std::max<UINT>(1, format->nChannels);
    description.isFloat = IsFloatFormat(format);
    return description;
}

float ReadSample(const BYTE* data, const AudioFormat& format, UINT frame, UINT channel) {
    const BYTE* sample = data + frame * format.blockAlign + channel * format.bytesPerSample;
    if (format.isFloat && format.bytesPerSample >= sizeof(float)) return *reinterpret_cast<const float*>(sample);
    if (format.bytesPerSample == 2) return static_cast<float>(*reinterpret_cast<const int16_t*>(sample)) / 32768.0f;
    if (format.bytesPerSample >= 4) return static_cast<float>(*reinterpret_cast<const int32_t*>(sample)) / 2147483648.0f;
    return 0.0f;
}

void WriteSample(BYTE* data, const AudioFormat& format, UINT frame, UINT channel, float value) {
    value = std::clamp(value, -1.0f, 1.0f);
    BYTE* sample = data + frame * format.blockAlign + channel * format.bytesPerSample;
    if (format.isFloat && format.bytesPerSample >= sizeof(float)) {
        *reinterpret_cast<float*>(sample) = value;
    } else if (format.bytesPerSample == 2) {
        *reinterpret_cast<int16_t*>(sample) = static_cast<int16_t>(value * 32767.0f);
    } else if (format.bytesPerSample >= 4) {
        *reinterpret_cast<int32_t*>(sample) = static_cast<int32_t>(value * 2147483647.0f);
    }
}

void FourierTransform(std::vector<std::complex<float>>& values, bool inverse) {
    const size_t count = values.size();
    for (size_t i = 1, j = 0; i < count; ++i) {
        size_t bit = count >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(values[i], values[j]);
    }
    for (size_t length = 2; length <= count; length <<= 1) {
        float angle = 2.0f * kPi / static_cast<float>(length) * (inverse ? 1.0f : -1.0f);
        std::complex<float> step(std::cos(angle), std::sin(angle));
        for (size_t start = 0; start < count; start += length) {
            std::complex<float> current(1.0f, 0.0f);
            for (size_t offset = 0; offset < length / 2; ++offset) {
                std::complex<float> even = values[start + offset];
                std::complex<float> odd = current * values[start + offset + length / 2];
                values[start + offset] = even + odd;
                values[start + offset + length / 2] = even - odd;
                current *= step;
            }
        }
    }
    if (inverse) {
        for (auto& value : values) value /= static_cast<float>(count);
    }
}

class NotchFilter {
public:
    void Configure(float sampleRate, float frequency, float quality) {
        float omega = 2.0f * kPi * frequency / sampleRate;
        float alpha = std::sin(omega) / (2.0f * quality);
        float a0 = 1.0f + alpha;
        b0_ = 1.0f / a0;
        b1_ = -2.0f * std::cos(omega) / a0;
        b2_ = b0_;
        a1_ = -2.0f * std::cos(omega) / a0;
        a2_ = (1.0f - alpha) / a0;
    }

    float Process(float input) {
        float output = b0_ * input + b1_ * input1_ + b2_ * input2_ - a1_ * output1_ - a2_ * output2_;
        input2_ = input1_;
        input1_ = input;
        output2_ = output1_;
        output1_ = output;
        return output;
    }

private:
    float b0_ = 1.0f;
    float b1_ = 0.0f;
    float b2_ = 0.0f;
    float a1_ = 0.0f;
    float a2_ = 0.0f;
    float input1_ = 0.0f;
    float input2_ = 0.0f;
    float output1_ = 0.0f;
    float output2_ = 0.0f;
};

class DspProcessor {
public:
    DspProcessor(UINT sampleRate, float highPassHz, float noiseReduction, float gateThreshold)
        : sampleRate_(sampleRate), window_(kFftSize), noiseMagnitudes_(kFftSize, 0.001f), overlap_(kHopSize, 0.0f),
          suppressionStrength_(std::clamp(noiseReduction / 100.0f, 0.0f, 1.0f)),
          gateThreshold_(std::clamp(gateThreshold, 1.2f, 6.0f)), calibrationBlocks_(std::max(1, static_cast<int>(sampleRate / kHopSize))) {
        const float highPassCutoff = std::clamp(highPassHz, 40.0f, 300.0f);
        highPassAlpha_ = (2.0f * kPi * highPassCutoff) / (2.0f * kPi * highPassCutoff + static_cast<float>(sampleRate_));
        if (sampleRate_ > 300) {
            notch50_.Configure(static_cast<float>(sampleRate_), 50.0f, 10.0f);
            notch100_.Configure(static_cast<float>(sampleRate_), 100.0f, 10.0f);
        }
        for (int index = 0; index < kFftSize; ++index) {
            window_[index] = 0.5f - 0.5f * std::cos(2.0f * kPi * static_cast<float>(index) / static_cast<float>(kFftSize));
        }
    }

    void Process(const std::vector<float>& input, std::deque<float>& output) {
        for (float sample : input) {
            float highPassed = highPassAlpha_ * (highPassState_ + sample - previousInput_);
            previousInput_ = sample;
            highPassState_ = highPassed;
            highPassed = notch50_.Process(highPassed);
            highPassed = notch100_.Process(highPassed);
            pending_.push_back(highPassed);
        }

        while (pending_.size() >= kFftSize) ProcessFrame(output);
    }

private:
    void ProcessFrame(std::deque<float>& output) {
        float blockPower = 0.0f;
        for (int index = 0; index < kFftSize; ++index) blockPower += pending_[index] * pending_[index];
        float blockRms = std::sqrt(blockPower / static_cast<float>(kFftSize));

        if (calibrationBlocks_ > 0) {
            noiseFloor_ = noiseFloor_ * 0.98f + blockRms * 0.02f;
            --calibrationBlocks_;
        } else if (blockRms < noiseFloor_ * gateThreshold_) {
            noiseFloor_ = noiseFloor_ * 0.995f + blockRms * 0.005f;
        }

        std::vector<std::complex<float>> spectrum(kFftSize);
        for (int index = 0; index < kFftSize; ++index) spectrum[index] = pending_[index] * window_[index];
        FourierTransform(spectrum, false);

        for (int index = 0; index < kFftSize; ++index) {
            float magnitude = std::abs(spectrum[index]);
            if (calibrationBlocks_ > 0 || blockRms < noiseFloor_ * gateThreshold_) {
                noiseMagnitudes_[index] = noiseMagnitudes_[index] * 0.97f + magnitude * 0.03f;
            }
            float power = magnitude * magnitude;
            float noisePower = noiseMagnitudes_[index] * noiseMagnitudes_[index];
            float ratio = noisePower / std::max(power, 0.0000001f);
            float gain = std::clamp(1.0f - suppressionStrength_ * ratio, 0.12f, 1.0f);
            spectrum[index] *= gain;
        }

        FourierTransform(spectrum, true);
        float targetGate = blockRms < noiseFloor_ * gateThreshold_ ? 0.04f : 1.0f;
        for (int index = 0; index < kFftSize; ++index) {
            float value = spectrum[index].real() * window_[index] * (4.0f / 3.0f);
            if (index < kHopSize) {
                value += overlap_[index];
                gateGain_ += (targetGate - gateGain_) * (targetGate > gateGain_ ? 0.12f : 0.025f);
                output.push_back(value * gateGain_);
            } else {
                overlap_[index - kHopSize] = value;
            }
        }

        pending_.erase(pending_.begin(), pending_.begin() + kHopSize);
    }

    UINT sampleRate_;
    float highPassAlpha_ = 0.0f;
    float highPassState_ = 0.0f;
    float previousInput_ = 0.0f;
    float noiseFloor_ = 0.01f;
    float gateGain_ = 0.04f;
    float suppressionStrength_ = 0.82f;
    float gateThreshold_ = 2.5f;
    int calibrationBlocks_;
    NotchFilter notch50_;
    NotchFilter notch100_;
    std::vector<float> window_;
    std::vector<float> noiseMagnitudes_;
    std::vector<float> overlap_;
    std::deque<float> pending_;
};

HRESULT ConfigureClient(IMMDevice* device, bool capture, ComPtr<IAudioClient>& client, CoTaskPtr<WAVEFORMATEX>& format,
                        UniqueHandle& event, UINT32& bufferFrames, ComPtr<IAudioCaptureClient>& captureClient,
                        ComPtr<IAudioRenderClient>& renderClient) {
    HRESULT result = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.put()));
    if (FAILED(result)) return result;
    WAVEFORMATEX* rawFormat = nullptr;
    result = client->GetMixFormat(&rawFormat);
    if (FAILED(result)) return result;
    format.reset(rawFormat);
    event.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!event.valid()) return HRESULT_FROM_WIN32(GetLastError());

    result = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 100000, 0, format.get(), nullptr);
    if (FAILED(result)) return result;
    result = client->SetEventHandle(event.get());
    if (FAILED(result)) return result;
    result = client->GetBufferSize(&bufferFrames);
    if (FAILED(result)) return result;
    if (capture) {
        return client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(captureClient.put()));
    }
    return client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(renderClient.put()));
}

class AudioPipeline {
public:
    explicit AudioPipeline(const CleanerSettings& settings)
        : inputId_(settings.inputId), outputId_(settings.outputId), highPassHz_(settings.highPassHz),
          noiseReduction_(settings.noiseReduction), gateThreshold_(settings.gateThreshold) {}

    ~AudioPipeline() { Stop(); }

    bool Start() {
        if (running_.exchange(true)) return false;
        stopEvent_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!stopEvent_.valid()) {
            running_ = false;
            return false;
        }
        thread_ = std::thread(&AudioPipeline::ThreadMain, this);
        return true;
    }

    void Stop() {
        if (!running_.load() && !thread_.joinable()) return;
        if (stopEvent_.valid()) SetEvent(stopEvent_.get());
        if (thread_.joinable()) thread_.join();
        stopEvent_.reset();
        running_ = false;
    }

    bool IsRunning() const { return running_.load(); }

    std::wstring LastError() const {
        std::lock_guard<std::mutex> lock(errorMutex_);
        return lastError_;
    }

private:
    void ThreadMain() {
        HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(result)) {
            SetError(L"COM initialization failed: " + HResultText(result));
            running_ = false;
            return;
        }

        while (WaitForSingleObject(stopEvent_.get(), 0) != WAIT_OBJECT_0) {
            result = RunSession();
            if (WaitForSingleObject(stopEvent_.get(), 0) == WAIT_OBJECT_0) break;
            if (FAILED(result)) SetError(L"Waiting for audio device: " + HResultText(result));
            WaitForSingleObject(stopEvent_.get(), 750);
        }

        CoUninitialize();
        running_ = false;
    }

    HRESULT RunSession() {
        ComPtr<IMMDeviceEnumerator> enumerator;
        HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(enumerator.put()));
        if (FAILED(result)) return result;

        ComPtr<IMMDevice> inputDevice;
        ComPtr<IMMDevice> outputDevice;
        result = enumerator->GetDevice(inputId_.c_str(), inputDevice.put());
        if (FAILED(result)) return result;
        result = enumerator->GetDevice(outputId_.c_str(), outputDevice.put());
        if (FAILED(result)) return result;

        ComPtr<IAudioClient> inputClient;
        ComPtr<IAudioClient> outputClient;
        ComPtr<IAudioCaptureClient> captureClient;
        ComPtr<IAudioRenderClient> renderClient;
        CoTaskPtr<WAVEFORMATEX> inputFormat;
        CoTaskPtr<WAVEFORMATEX> outputFormat;
        UniqueHandle inputEvent;
        UniqueHandle outputEvent;
        UINT32 inputBufferFrames = 0;
        UINT32 outputBufferFrames = 0;

        result = ConfigureClient(inputDevice.get(), true, inputClient, inputFormat, inputEvent, inputBufferFrames, captureClient, renderClient);
        if (FAILED(result)) return result;
        result = ConfigureClient(outputDevice.get(), false, outputClient, outputFormat, outputEvent, outputBufferFrames, captureClient, renderClient);
        if (FAILED(result)) return result;

        AudioFormat inputDescription = DescribeFormat(inputFormat.get());
        AudioFormat outputDescription = DescribeFormat(outputFormat.get());
        DspProcessor processor(inputDescription.sampleRate, highPassHz_, noiseReduction_, gateThreshold_);
        std::deque<float> audioQueue;
        std::vector<float> mono;
        std::deque<float> processed;

        result = outputClient->Start();
        if (FAILED(result)) return result;
        result = inputClient->Start();
        if (FAILED(result)) {
            outputClient->Stop();
            return result;
        }

        HANDLE waitHandles[] = {stopEvent_.get(), inputEvent.get(), outputEvent.get()};
        bool stopped = false;
        while (!stopped) {
            DWORD waitResult = WaitForMultipleObjects(ARRAYSIZE(waitHandles), waitHandles, FALSE, 500);
            if (waitResult == WAIT_OBJECT_0) {
                stopped = true;
                break;
            }
            if (waitResult == WAIT_FAILED) {
                result = HRESULT_FROM_WIN32(GetLastError());
                break;
            }

            if (waitResult == WAIT_OBJECT_0 + 1 || waitResult == WAIT_TIMEOUT) {
                UINT32 packetFrames = 0;
                for (;;) {
                    result = captureClient->GetNextPacketSize(&packetFrames);
                    if (FAILED(result) || packetFrames == 0) break;
                    BYTE* data = nullptr;
                    UINT32 frames = 0;
                    DWORD flags = 0;
                    result = captureClient->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
                    if (FAILED(result)) break;
                    mono.assign(frames, 0.0f);
                    if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) == 0 && data != nullptr) {
                        for (UINT frame = 0; frame < frames; ++frame) {
                            float sum = 0.0f;
                            for (UINT channel = 0; channel < inputDescription.channels; ++channel) {
                                sum += ReadSample(data, inputDescription, frame, channel);
                            }
                            mono[frame] = sum / static_cast<float>(std::max<UINT>(1, inputDescription.channels));
                        }
                    }
                    captureClient->ReleaseBuffer(frames);
                    processor.Process(mono, processed);
                    while (!processed.empty()) {
                        audioQueue.push_back(processed.front());
                        processed.pop_front();
                    }
                    while (audioQueue.size() > inputDescription.sampleRate * 2) audioQueue.pop_front();
                    if (FAILED(result)) break;
                }
                if (FAILED(result)) break;
            }

            if (waitResult == WAIT_OBJECT_0 + 2 || waitResult == WAIT_TIMEOUT) {
                UINT32 padding = 0;
                result = outputClient->GetCurrentPadding(&padding);
                if (FAILED(result)) break;
                UINT32 available = outputBufferFrames > padding ? outputBufferFrames - padding : 0;
                if (available > 0) {
                    BYTE* data = nullptr;
                    result = renderClient->GetBuffer(available, &data);
                    if (FAILED(result)) break;
                    for (UINT frame = 0; frame < available; ++frame) {
                        float sample = audioQueue.empty() ? 0.0f : audioQueue.front();
                        if (!audioQueue.empty()) audioQueue.pop_front();
                        for (UINT channel = 0; channel < outputDescription.channels; ++channel) {
                            WriteSample(data, outputDescription, frame, channel, sample);
                        }
                    }
                    result = renderClient->ReleaseBuffer(available, 0);
                    if (FAILED(result)) break;
                }
            }
        }

        inputClient->Stop();
        outputClient->Stop();
        return stopped ? S_OK : result;
    }

    void SetError(std::wstring message) {
        std::lock_guard<std::mutex> lock(errorMutex_);
        lastError_ = std::move(message);
    }

    std::wstring inputId_;
    std::wstring outputId_;
    float highPassHz_ = 90.0f;
    float noiseReduction_ = 82.0f;
    float gateThreshold_ = 2.2f;
    UniqueHandle stopEvent_;
    std::thread thread_;
    std::atomic<bool> running_ = false;
    mutable std::mutex errorMutex_;
    std::wstring lastError_;
};

enum ControlId {
    kInputCombo = 1001,
    kOutputCombo = 1002,
    kRefreshButton = 1003,
    kStartButton = 1004,
    kStatusLabel = 1005,
    kHighPassEdit = 1006,
    kNoiseReductionEdit = 1007,
    kGateThresholdEdit = 1008,
};

struct ConfigUi {
    HWND window = nullptr;
    HWND inputCombo = nullptr;
    HWND outputCombo = nullptr;
    HWND highPassEdit = nullptr;
    HWND noiseReductionEdit = nullptr;
    HWND gateThresholdEdit = nullptr;
    HWND statusLabel = nullptr;
    HWND startButton = nullptr;
    std::vector<Endpoint> inputs;
    std::vector<Endpoint> outputs;
    CleanerSettings settings;
    std::unique_ptr<AudioPipeline> pipeline;
};

void SetControlFont(HWND control) {
    HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

void SetFloatText(HWND edit, float value) {
    wchar_t text[32] = {};
    swprintf_s(text, L"%.2f", value);
    SetWindowTextW(edit, text);
}

bool ReadFloatEdit(HWND edit, float minimum, float maximum, float* value) {
    wchar_t text[64] = {};
    GetWindowTextW(edit, text, ARRAYSIZE(text));
    wchar_t* end = nullptr;
    float parsed = wcstof(text, &end);
    if (end == text || *end != L'\0' || parsed < minimum || parsed > maximum) return false;
    *value = parsed;
    return true;
}

void SetStatus(ConfigUi* ui, const std::wstring& text) {
    SetWindowTextW(ui->statusLabel, text.c_str());
}

int FindEndpointIndex(const std::vector<Endpoint>& endpoints, const std::wstring& id, const std::wstring& preferredText) {
    for (size_t index = 0; index < endpoints.size(); ++index) {
        if (!id.empty() && endpoints[index].id == id) return static_cast<int>(index);
    }
    std::wstring loweredPreferred = preferredText;
    std::transform(loweredPreferred.begin(), loweredPreferred.end(), loweredPreferred.begin(), towlower);
    for (size_t index = 0; index < endpoints.size(); ++index) {
        std::wstring loweredName = endpoints[index].name;
        std::transform(loweredName.begin(), loweredName.end(), loweredName.begin(), towlower);
        if (!loweredPreferred.empty() && loweredName.find(loweredPreferred) != std::wstring::npos) return static_cast<int>(index);
    }
    return endpoints.empty() ? -1 : 0;
}

void PopulateCombo(HWND combo, const std::vector<Endpoint>& endpoints, int selectedIndex) {
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    for (size_t index = 0; index < endpoints.size(); ++index) {
        LRESULT item = SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(endpoints[index].name.c_str()));
        SendMessageW(combo, CB_SETITEMDATA, item, static_cast<LPARAM>(index));
    }
    if (selectedIndex >= 0) SendMessageW(combo, CB_SETCURSEL, selectedIndex, 0);
}

void RefreshDevices(ConfigUi* ui) {
    ui->inputs = EnumerateEndpoints(eCapture);
    ui->outputs = EnumerateEndpoints(eRender);
    int inputIndex = FindEndpointIndex(ui->inputs, ui->settings.inputId, L"usb pnp sound device");
    int outputIndex = FindEndpointIndex(ui->outputs, ui->settings.outputId, L"cable input");
    PopulateCombo(ui->inputCombo, ui->inputs, inputIndex);
    PopulateCombo(ui->outputCombo, ui->outputs, outputIndex);
    if (ui->inputs.empty()) SetStatus(ui, L"No active microphone found. Turn the mic on, then refresh.");
    else if (ui->outputs.empty()) SetStatus(ui, L"No active output endpoint found.");
    else SetStatus(ui, L"Select the cleaned output in your STT app.");
}

bool ReadSelectedEndpoint(HWND combo, const std::vector<Endpoint>& endpoints, std::wstring* id) {
    LRESULT selected = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (selected == CB_ERR || selected < 0 || selected >= static_cast<LRESULT>(endpoints.size())) return false;
    id->assign(endpoints[static_cast<size_t>(selected)].id);
    return true;
}

void TogglePipeline(ConfigUi* ui) {
    if (ui->pipeline != nullptr) {
        ui->pipeline->Stop();
        ui->pipeline.reset();
        SetWindowTextW(ui->startButton, L"Start cleaner");
        SetStatus(ui, L"Stopped.");
        return;
    }

    CleanerSettings settings;
    if (!ReadSelectedEndpoint(ui->inputCombo, ui->inputs, &settings.inputId) ||
        !ReadSelectedEndpoint(ui->outputCombo, ui->outputs, &settings.outputId)) {
        SetStatus(ui, L"Choose both an input and an output device first.");
        return;
    }
    if (!ReadFloatEdit(ui->highPassEdit, 40.0f, 300.0f, &settings.highPassHz) ||
        !ReadFloatEdit(ui->noiseReductionEdit, 0.0f, 100.0f, &settings.noiseReduction) ||
        !ReadFloatEdit(ui->gateThresholdEdit, 1.2f, 6.0f, &settings.gateThreshold)) {
        SetStatus(ui, L"Use valid settings: high-pass 40-300 Hz, reduction 0-100%, gate 1.2-6.0.");
        return;
    }
    ui->settings = settings;
    SaveSettings(settings);
    ui->pipeline = std::make_unique<AudioPipeline>(settings);
    if (!ui->pipeline->Start()) {
        ui->pipeline.reset();
        SetStatus(ui, L"Could not start the cleaner.");
        return;
    }
    SetWindowTextW(ui->startButton, L"Stop cleaner");
    SetStatus(ui, L"Running. If the mic is toggled off, it will reconnect automatically.");
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
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
        HWND inputLabel = CreateWindowW(L"STATIC", L"Microphone input:", WS_CHILD | WS_VISIBLE, 16, 18, 150, 24, window, nullptr, instance, nullptr);
        ui->inputCombo = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST, 170, 14, 390, 250, window, reinterpret_cast<HMENU>(kInputCombo), instance, nullptr);
        HWND outputLabel = CreateWindowW(L"STATIC", L"Cleaned output:", WS_CHILD | WS_VISIBLE, 16, 62, 150, 24, window, nullptr, instance, nullptr);
        ui->outputCombo = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST, 170, 58, 390, 250, window, reinterpret_cast<HMENU>(kOutputCombo), instance, nullptr);
        HWND highPassLabel = CreateWindowW(L"STATIC", L"High-pass (Hz):", WS_CHILD | WS_VISIBLE, 16, 106, 150, 24, window, nullptr, instance, nullptr);
        ui->highPassEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"90", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER, 170, 102, 90, 26, window, reinterpret_cast<HMENU>(kHighPassEdit), instance, nullptr);
        HWND reductionLabel = CreateWindowW(L"STATIC", L"Noise reduction (%):", WS_CHILD | WS_VISIBLE, 280, 106, 160, 24, window, nullptr, instance, nullptr);
        ui->noiseReductionEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"82", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER, 445, 102, 90, 26, window, reinterpret_cast<HMENU>(kNoiseReductionEdit), instance, nullptr);
        HWND gateLabel = CreateWindowW(L"STATIC", L"Gate threshold (x noise):", WS_CHILD | WS_VISIBLE, 16, 144, 190, 24, window, nullptr, instance, nullptr);
        ui->gateThresholdEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"2.50", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 210, 140, 90, 26, window, reinterpret_cast<HMENU>(kGateThresholdEdit), instance, nullptr);
        HWND hint = CreateWindowW(L"STATIC", L"Start with the mic silent for about one second so the cleaner can learn its noise floor.", WS_CHILD | WS_VISIBLE, 16, 184, 560, 24, window, nullptr, instance, nullptr);
        HWND refresh = CreateWindowW(L"BUTTON", L"Refresh devices", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 16, 226, 130, 30, window, reinterpret_cast<HMENU>(kRefreshButton), instance, nullptr);
        ui->startButton = CreateWindowW(L"BUTTON", L"Start cleaner", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 170, 226, 140, 30, window, reinterpret_cast<HMENU>(kStartButton), instance, nullptr);
        ui->statusLabel = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 16, 274, 560, 42, window, reinterpret_cast<HMENU>(kStatusLabel), instance, nullptr);
        SetControlFont(inputLabel);
        SetControlFont(outputLabel);
        SetControlFont(highPassLabel);
        SetControlFont(reductionLabel);
        SetControlFont(gateLabel);
        SetControlFont(hint);
        SetControlFont(refresh);
        SetControlFont(ui->startButton);
        SetControlFont(ui->highPassEdit);
        SetControlFont(ui->noiseReductionEdit);
        SetControlFont(ui->gateThresholdEdit);
        SetControlFont(ui->statusLabel);
        ui->settings = LoadSettings();
        SetFloatText(ui->highPassEdit, ui->settings.highPassHz);
        SetFloatText(ui->noiseReductionEdit, ui->settings.noiseReduction);
        SetFloatText(ui->gateThresholdEdit, ui->settings.gateThreshold);
        RefreshDevices(ui);
        SetTimer(window, 1, 500, nullptr);
        return 0;
    }

    if (message == WM_TIMER && ui->pipeline != nullptr && !ui->pipeline->IsRunning()) {
        std::wstring error = ui->pipeline->LastError();
        ui->pipeline.reset();
        SetWindowTextW(ui->startButton, L"Start cleaner");
        SetStatus(ui, error.empty() ? L"Cleaner stopped." : error);
        return 0;
    }

    if (message == WM_COMMAND && HIWORD(wParam) == BN_CLICKED) {
        int control = LOWORD(wParam);
        if (control == kRefreshButton) RefreshDevices(ui);
        if (control == kStartButton) TogglePipeline(ui);
        return 0;
    }

    if (message == WM_CLOSE) {
        if (ui->pipeline != nullptr) {
            ui->pipeline->Stop();
            ui->pipeline.reset();
        }
        DestroyWindow(window);
        return 0;
    }
    if (message == WM_DESTROY) {
        KillTimer(window, 1);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

int RunGui(HINSTANCE instance) {
    WNDCLASSW windowClass = {};
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.lpszClassName = kWindowClass;
    if (RegisterClassW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return 1;

    ConfigUi ui;
    HWND window = CreateWindowExW(WS_EX_DLGMODALFRAME, kWindowClass, L"Microphone Audio Cleaner",
                                 WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                 CW_USEDEFAULT, CW_USEDEFAULT, 620, 370, nullptr, nullptr, instance, &ui);
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

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(result)) return 1;
    int exitCode = RunGui(instance);
    CoUninitialize();
    return exitCode;
}
