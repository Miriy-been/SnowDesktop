// audio_spectrum.cpp
// WASAPI loopback 回环捕获 + FFT 分频段能量计算。
#include "audio_spectrum.h"

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace snowdesktop::audio {

namespace {

using Microsoft::WRL::ComPtr;

constexpr UINT32 kFftSize = 2048;      ///< FFT 点数（2 的幂，参考 Rainmeter FFTSize）
constexpr UINT32 kHop = 1024;          ///< 每多少个新采样做一次 FFT（50% 重叠，参考 Rainmeter FFTOverlap）
constexpr float kPi = 3.14159265358979323846f;
// 频段功率 dB 运算的数值下限（避免 log10(0)），约 -70dB
constexpr float kPowerFloor = 1e-7f;
// dB 感知响度映射范围：-52dB（很轻）→ 0，-14dB（响亮）→ 1。
// 用对数压缩把真实音乐的大动态范围稳定映射到 [0,1]，避免线性映射
// 在安静时全 0、稍大声就饱和到 1 的失真。
constexpr float kDbMin = -52.0f;
constexpr float kDbMax = -14.0f;
// attack / release 平滑系数，实现"快升快落"：
// 上升略快于回落，柱子既跟手又有节奏感；回落不再拖沓。
// 43Hz 数据源（50% 重叠 FFT）+ 此平滑 = 连续丝滑不"跳格"。
// 默认值同时在 audio_spectrum.h 的成员初始值中维护，
// 运行时可通过 SetSmoothing / SetActiveThreshold 从 Lua 组件设置实时调整。

// 迭代 radix-2 FFT（原地）
void FftRadix2(std::vector<float>& re, std::vector<float>& im)
{
    const size_t n = re.size();
    for (size_t i = 1, j = 0; i < n; ++i)
    {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
        {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }
    for (size_t len = 2; len <= n; len <<= 1)
    {
        const float ang = -2.0f * kPi / static_cast<float>(len);
        const float wRe = std::cos(ang);
        const float wIm = std::sin(ang);
        for (size_t i = 0; i < n; i += len)
        {
            float curRe = 1.0f, curIm = 0.0f;
            for (size_t j = 0; j < len / 2; ++j)
            {
                const float uRe = re[i + j];
                const float uIm = im[i + j];
                const float vRe = re[i + j + len / 2] * curRe -
                    im[i + j + len / 2] * curIm;
                const float vIm = re[i + j + len / 2] * curIm +
                    im[i + j + len / 2] * curRe;
                re[i + j] = uRe + vRe;
                im[i + j] = uIm + vIm;
                re[i + j + len / 2] = uRe - vRe;
                im[i + j + len / 2] = uIm - vIm;
                const float nextRe = curRe * wRe - curIm * wIm;
                curIm = curRe * wIm + curIm * wRe;
                curRe = nextRe;
            }
        }
    }
}

// 从 WAVEFORMATEX 中读取一个样本值并归一化到 [-1,1]
float SampleToFloat(const BYTE* p, const WAVEFORMATEX& fmt)
{
    const WORD tag = fmt.wFormatTag;
    const WORD bits = fmt.wBitsPerSample;
    if (tag == WAVE_FORMAT_IEEE_FLOAT)
    {
        if (bits == 32)
        {
            float v = 0.0f;
            std::memcpy(&v, p, sizeof(v));
            return v;
        }
        if (bits == 64)
        {
            double v = 0.0;
            std::memcpy(&v, p, sizeof(v));
            return static_cast<float>(v);
        }
        return 0.0f;
    }
    if (tag == WAVE_FORMAT_PCM)
    {
        if (bits == 8)
        {
            return (static_cast<int>(p[0]) - 128) / 128.0f;
        }
        if (bits == 16)
        {
            int16_t v = 0;
            std::memcpy(&v, p, sizeof(v));
            return v / 32768.0f;
        }
        if (bits == 24)
        {
            int32_t v = p[0] | (p[1] << 8) | (p[2] << 16);
            if (v & 0x800000)
                v |= ~0xFFFFFF;
            return static_cast<float>(v) / 8388608.0f;
        }
        if (bits == 32)
        {
            int32_t v = 0;
            std::memcpy(&v, p, sizeof(v));
            return static_cast<float>(v) / 2147483648.0f;
        }
        return 0.0f;
    }
    if (tag == WAVE_FORMAT_EXTENSIBLE && fmt.cbSize >= 22)
    {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(&fmt);
        if (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)
        {
            WAVEFORMATEX f = fmt;
            f.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
            return SampleToFloat(p, f);
        }
        if (ext->SubFormat == KSDATAFORMAT_SUBTYPE_PCM)
        {
            // 构造一个 PCM 格式（仅修改标签）后复用上面逻辑
            WAVEFORMATEX pcm = fmt;
            pcm.wFormatTag = WAVE_FORMAT_PCM;
            return SampleToFloat(p, pcm);
        }
    }
    return 0.0f;
}

} // namespace

AudioSpectrumService::AudioSpectrumService()
{
    peaks_.assign(kBandCount, 0.0f);
}

AudioSpectrumService::~AudioSpectrumService()
{
    Stop();
}

void AudioSpectrumService::Start()
{
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true))
        return;
    thread_ = std::thread([this] {
        // 后台捕获线程必须绝不使宿主进程崩溃；任何异常都只让本线程退出。
        try
        {
            CaptureLoop();
        }
        catch (...)
        {
        }
        capturing_.store(false);
        ResetPeaks();
    });
}

void AudioSpectrumService::Stop()
{
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false))
        return;
    if (thread_.joinable())
        thread_.join();
    capturing_.store(false);
}

bool AudioSpectrumService::Capturing() const { return capturing_.load(); }
bool AudioSpectrumService::Active() const { return active_.load(); }

std::vector<float> AudioSpectrumService::Peaks(int bandCount) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (bandCount <= 0)
        bandCount = 1;
    std::vector<float> out(static_cast<size_t>(bandCount), 0.0f);
    if (bandCount == kBandCount)
        return peaks_;
    // 线性重采样到任意段数。当输出段数 > 内部段数时，用线性插值
    // 扩展（而非空范围补零），保证所有柱子都有值。
    for (int i = 0; i < bandCount; ++i)
    {
        // 输出带 i 的中心在内部带坐标轴上的位置
        const float center = (static_cast<float>(i) + 0.5f) /
            static_cast<float>(bandCount) * static_cast<float>(kBandCount);
        const int lo = std::max(0, static_cast<int>(std::floor(center)));
        const int hi = std::min(kBandCount - 1,
            static_cast<int>(std::ceil(center)));
        if (lo == hi)
        {
            out[static_cast<size_t>(i)] =
                peaks_[static_cast<size_t>(lo)];
        }
        else
        {
            const float t = center - static_cast<float>(lo);
            out[static_cast<size_t>(i)] =
                peaks_[static_cast<size_t>(lo)] * (1.0f - t) +
                peaks_[static_cast<size_t>(hi)] * t;
        }
    }
    return out;
}
void AudioSpectrumService::CaptureLoop()
{
    // 捕获线程内的平滑状态
    std::vector<float> smooth(kBandCount, 0.0f);
    // 频段滑动平均历史（参考 Rainmeter AverageSize）：保存最近若干帧的频段能量，
    // 发布时按当前窗口大小取平均，让柱子连续平滑移动而非"跳格"。
    std::vector<std::vector<float>> avgHistory(16,
        std::vector<float>(kBandCount, 0.0f));
    size_t avgWrite = 0;
    size_t avgFrames = 0;  // 已写入的有效帧数（避免冷启动时被 0 拉低）
    // FFT 输入：环形缓冲保存最近 kFftSize 个采样，每收集 kHop 个新采样
    // 计算一次 FFT（50% 重叠窗口）。重叠让相邻 FFT 结果高度相关，
    // 数据更新率翻倍（44.1kHz 下约 43 次/秒），柱子过渡连续不"跳格"。
    std::vector<float> ring(kFftSize, 0.0f);
    size_t ringPos = 0;          // 下一个写入位置
    size_t samplesSinceFft = 0;  // 自上次 FFT 以来收集的新采样数
    // Hann 窗
    std::vector<float> window(kFftSize);
    for (size_t i = 0; i < kFftSize; ++i)
        window[i] = 0.5f * (1.0f - std::cos(2.0f * kPi * static_cast<float>(i) /
                                             static_cast<float>(kFftSize - 1)));

    while (running_.load())
    {
        if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
        {
            ResetPeaks();
            Sleep(500);
            continue;
        }

        ComPtr<IMMDeviceEnumerator> enumerator;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
            CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
        if (FAILED(hr))
        {
            CoUninitialize();
            ResetPeaks();
            Sleep(500);
            continue;
        }

        ComPtr<IMMDevice> device;
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        if (FAILED(hr))
        {
            CoUninitialize();
            ResetPeaks();
            Sleep(500);
            continue;
        }

        ComPtr<IAudioClient> audioClient;
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
            reinterpret_cast<void**>(audioClient.GetAddressOf()));
        if (FAILED(hr))
        {
            CoUninitialize();
            ResetPeaks();
            Sleep(500);
            continue;
        }

        WAVEFORMATEX* mixFormat = nullptr;
        hr = audioClient->GetMixFormat(&mixFormat);
        if (FAILED(hr) || !mixFormat)
        {
            CoUninitialize();
            ResetPeaks();
            Sleep(500);
            continue;
        }
        const UINT32 channels = std::max<UINT32>(1, mixFormat->nChannels);
        const double sampleRate = std::max(1.0,
            static_cast<double>(mixFormat->nSamplesPerSec));
        const WORD bytesPerSample = mixFormat->wBitsPerSample / 8;

        hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK |
                AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
            0, 0, mixFormat, nullptr);
        if (FAILED(hr))
        {
            CoTaskMemFree(mixFormat);
            CoUninitialize();
            ResetPeaks();
            Sleep(500);
            continue;
        }

        ComPtr<IAudioCaptureClient> capture;
        hr = audioClient->GetService(IID_PPV_ARGS(&capture));
        if (FAILED(hr))
        {
            CoTaskMemFree(mixFormat);
            CoUninitialize();
            ResetPeaks();
            Sleep(500);
            continue;
        }

        hr = audioClient->Start();
        if (FAILED(hr))
        {
            CoTaskMemFree(mixFormat);
            CoUninitialize();
            ResetPeaks();
            Sleep(500);
            continue;
        }
        capturing_.store(true);

        UINT32 packetLength = 0;
        bool lastReadError = false;
        while (running_.load())
        {
            // 等待数据（约一个缓冲周期的一半）
            UINT32 bufferFrames = 0;
            if (FAILED(audioClient->GetBufferSize(&bufferFrames)))
            {
                lastReadError = true;
                break;
            }
            REFERENCE_TIME sleepMs = static_cast<REFERENCE_TIME>(
                (static_cast<double>(bufferFrames) / sampleRate) * 500.0);
            Sleep(static_cast<DWORD>(std::max<REFERENCE_TIME>(2, sleepMs)));

            hr = capture->GetNextPacketSize(&packetLength);
            if (FAILED(hr))
            {
                lastReadError = true;
                break;
            }
            while (packetLength != 0)
            {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                hr = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
                if (FAILED(hr) || !data || frames == 0)
                {
                    lastReadError = true;
                    break;
                }
                // 有静音/数据无效标记则跳过本包数据（用零填充）
                const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                const BYTE* p = data;
                for (UINT32 f = 0; f < frames; ++f)
                {
                    float mono = 0.0f;
                    if (silent)
                    {
                        mono = 0.0f;
                        p += bytesPerSample * channels;
                    }
                    else
                    {
                        const BYTE* s = p;
                        for (UINT32 c = 0; c < channels; ++c)
                        {
                            mono += SampleToFloat(s, *mixFormat);
                            s += bytesPerSample;
                        }
                        mono /= static_cast<float>(channels);
                        p = s;
                    }
                    ring[ringPos] = mono;
                    ringPos = (ringPos + 1) % kFftSize;
                    ++samplesSinceFft;
                    if (samplesSinceFft >= kHop)
                    {
                        samplesSinceFft = 0;
                        // FFT + 频段能量（按时间顺序重建窗口）
                        std::vector<float> fftRe(kFftSize);
                        std::vector<float> fftIm(kFftSize, 0.0f);
                        for (size_t i = 0; i < kFftSize; ++i)
                        {
                            const float s = ring[(ringPos + i) % kFftSize];
                            fftRe[i] = s * window[i];
                        }
                        FftRadix2(fftRe, fftIm);

                        const float binRes =
                            static_cast<float>(sampleRate) / kFftSize;
                        // 对数频段边界：100Hz ~ 16.5kHz（对齐 Rainmeter
                        // FreqMin=100 / FreqMax=16500）。去掉无能量的极低频，
                        // 高频上限覆盖镲片/齿音区间，右半柱子有真实能量。
                        const float fLow = 100.0f;
                        const float fHigh = std::min(16500.0f,
                            static_cast<float>(sampleRate) * 0.5f);
                        const float ratio = fHigh / fLow;
                        // 极轻微的视觉权重：中间 1.0、两端 0.75，仅做柔和过渡。
                        // 不强制拱形——"中间高两边矮"由音乐中频能量天然最高
                        // 自然涌现，人工强包络反而会破坏跳动的自然观感。
                        const float mid =
                            static_cast<float>(kBandCount - 1) * 0.5f;
                        float bandEnergy[kBandCount];
                        float maxRawBand = 0.0f;
                        for (int b = 0; b < kBandCount; ++b)
                        {
                            const float fLo =
                                fLow * std::pow(ratio,
                                    static_cast<float>(b) / kBandCount);
                            const float fHi =
                                fLow * std::pow(ratio,
                                    static_cast<float>(b + 1) / kBandCount);
                            const int binLo = std::max(1, static_cast<int>(
                                std::floor(fLo / binRes)));
                            const int binHi = std::min<int>(kFftSize / 2 - 1,
                                static_cast<int>(std::ceil(fHi / binRes)));
                            float sum = 0.0f;
                            for (int bin = binLo; bin <= binHi; ++bin)
                            {
                                const float m = std::sqrt(
                                    fftRe[bin] * fftRe[bin] +
                                    fftIm[bin] * fftIm[bin]) /
                                    (static_cast<float>(kFftSize) * 0.5f);
                                sum += m * m;
                            }
                            const float meanPower =
                                sum / std::max(1, binHi - binLo + 1);
                            const float db = 10.0f * std::log10(
                                std::max(meanPower, kPowerFloor));
                            // dB 感知响度映射到 [0,1]：对数压缩真实音乐的
                            // 大动态范围，让安静与响亮之间仍有平滑的中间档位。
                            const float e = std::clamp(
                                (db - kDbMin) / (kDbMax - kDbMin),
                                0.0f, 1.0f);
                            maxRawBand = std::max(maxRawBand, e);
                            // 柔和视觉权重（0.75 ~ 1.0）
                            const float env = 0.75f + 0.25f * std::cos(
                                kPi * (static_cast<float>(b) - mid) /
                                (kBandCount - 1));
                            // 频率平衡：低频天然能量高于高频，视觉上"左活跃右寂静"。
                            // 用 balance 在频段轴上施加对数倾斜——balance>0 提高频
                            // 压低频、balance<0 相反、0 为自然。t∈[0,1] 为频段位置。
                            const float t =
                                static_cast<float>(b) / (kBandCount - 1);
                            const float balFactor = std::pow(10.0f,
                                frequencyBalance_.load() * (t - 0.5f));
                            bandEnergy[b] = e * env * balFactor;
                        }
                        // 滑动平均（参考 Rainmeter AverageSize）：把当前帧写入历史，
                        // 对窗口内各帧求平均作为该柱能量，避免单帧跳变。
                        std::vector<float>& cur = avgHistory[avgWrite];
                        for (int b = 0; b < kBandCount; ++b)
                            cur[b] = bandEnergy[b];
                        avgWrite = (avgWrite + 1) % avgHistory.size();
                        if (avgFrames < avgHistory.size())
                            ++avgFrames;
                        const int win = std::clamp(
                            static_cast<int>(averageSize_.load()), 1,
                            static_cast<int>(avgHistory.size()));
                        const size_t count = std::min(avgFrames,
                            static_cast<size_t>(win));
                        std::vector<float> avg(kBandCount, 0.0f);
                        for (size_t fi = 0; fi < count; ++fi)
                        {
                            const std::vector<float>& h =
                                avgHistory[(avgWrite + avgHistory.size() -
                                    count + fi) % avgHistory.size()];
                            for (int b = 0; b < kBandCount; ++b)
                                avg[b] += h[b];
                        }
                        for (int b = 0; b < kBandCount; ++b)
                            avg[b] /= static_cast<float>(count);
                        // 平滑（attack 快、release 慢）并发布
                        const float attack = attack_.load();
                        const float release = release_.load();
                        for (int b = 0; b < kBandCount; ++b)
                        {
                            const float e = avg[b];
                            smooth[b] = e >= smooth[b]
                                ? smooth[b] + (e - smooth[b]) * attack
                                : smooth[b] + (e - smooth[b]) * release;
                        }
                        {
                            std::lock_guard<std::mutex> lock(mutex_);
                            peaks_ = smooth;
                        }
                        active_.store(maxRawBand >= activeThreshold_.load());
                    }
                }
                capture->ReleaseBuffer(frames);
                if (lastReadError)
                    break;
                hr = capture->GetNextPacketSize(&packetLength);
                if (FAILED(hr))
                {
                    lastReadError = true;
                    break;
                }
            }
            if (lastReadError)
                break;
        }

        audioClient->Stop();
        capturing_.store(false);
        // 若运行中但捕获出错，短暂等待后重连
        if (running_.load())
        {
            ResetPeaks();
            CoTaskMemFree(mixFormat);
            CoUninitialize();
            Sleep(300);
        }
        else
        {
            CoTaskMemFree(mixFormat);
            CoUninitialize();
        }
    }
    ResetPeaks();
}

void AudioSpectrumService::ResetPeaks()
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::fill(peaks_.begin(), peaks_.end(), 0.0f);
    active_.store(false);
}

void AudioSpectrumService::SetSmoothing(float attack, float release)
{
    attack_.store(std::clamp(attack, 0.001f, 0.999f));
    release_.store(std::clamp(release, 0.001f, 0.999f));
}

void AudioSpectrumService::SetActiveThreshold(float threshold)
{
    activeThreshold_.store(std::clamp(threshold, 0.0f, 0.5f));
}

void AudioSpectrumService::SetFrequencyBalance(float balance)
{
    frequencyBalance_.store(std::clamp(balance, -1.0f, 1.0f));
}

void AudioSpectrumService::SetAverageSize(int averageSize)
{
    averageSize_.store(std::clamp(averageSize, 1, 16));
}

} // namespace snowdesktop::audio
