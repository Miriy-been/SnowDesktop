#pragma once
// audio_spectrum.h
// 音乐音频频谱服务：通过 WASAPI 回环捕获系统正在播放的音频，
// 在后台线程计算分频段能量，供 Lua 组件实时读取绘制频谱条。

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace snowdesktop::audio {

/**
 * @class AudioSpectrumService
 * @brief 捕获系统回环音频并输出平滑后的分频段能量。
 *
 * - 后台线程使用 WASAPI loopback 捕获默认渲染设备当前播放的全部音频；
 * - 对捕获的 PCM 做 FFT 分频段（固定 kBandCount=64 段，对数分布）能量；
 * - 能量经过 attack/release 平滑后缓存在内部，供任意线程无阻塞读取；
 * - Peaks(bandCount) 按需把内部 64 段重采样为任意段数并返回 [0,1]。
 */
class AudioSpectrumService {
public:
    static constexpr int kBandCount = 64;

    AudioSpectrumService();
    ~AudioSpectrumService();
    AudioSpectrumService(const AudioSpectrumService&) = delete;
    AudioSpectrumService& operator=(const AudioSpectrumService&) = delete;

    /// 启动后台捕获线程（幂等，可重复调用）。
    void Start();
    /// 停止后台捕获线程（幂等）。
    void Stop();
    /// 是否正在捕获（成功进入捕获循环）。
    bool Capturing() const;
    /// 最近是否有非静音的有效音频。
    bool Active() const;
    /// 返回 bandCount 个平滑后的频段能量，范围 [0,1]。
    /// 内部固定 32 段，按需线性重采样到 bandCount（1..64）。
    std::vector<float> Peaks(int bandCount) const;

    /// 设置平滑系数（捕获线程实时读取，无需重启）。
    /// @param attack  上升系数（越大上升越快），范围 (0,1)
    /// @param release 回落系数（越大回落越快），范围 (0,1)
    void SetSmoothing(float attack, float release);
    /// 设置"有声音"判定阈值（映射后最大频段能量）。
    void SetActiveThreshold(float threshold);
    /// 设置频率平衡（-1 = 突出低频、削弱高频；+1 = 突出高频、削弱低频；0 = 自然）。
    /// 用于平衡"低频太活跃、高频太寂静"的视觉分布。
    void SetFrequencyBalance(float balance);
    /// 设置频段滑动平均窗口大小（参考 Rainmeter AverageSize）。窗口越大柱子越平滑、
    /// 移动越连续；1 = 关闭平均。范围 [1, 16]。
    void SetAverageSize(int averageSize);

private:
    void CaptureLoop();
    void ResetPeaks();

    std::thread thread_;
    std::atomic<bool> running_{ false };
    std::atomic<bool> capturing_{ false };
    std::atomic<bool> active_{ false };
    mutable std::mutex mutex_;
    std::vector<float> peaks_;   ///< 平滑后的频段能量（kBandCount 段，[0,1]）

    // 运行时可调的平滑与判定参数（捕获线程实时读取，跨线程需 atomic）
    std::atomic<float> attack_{ 0.48f };        ///< 上升系数（默认见 audio_spectrum.cpp 说明）
    std::atomic<float> release_{ 0.40f };       ///< 回落系数
    std::atomic<float> activeThreshold_{ 0.05f }; ///< 有声音判定阈值
    std::atomic<float> frequencyBalance_{ 0.15f }; ///< 频率平衡（默认轻微提高频，见 audio_spectrum.cpp 说明）
    std::atomic<int> averageSize_{ 6 };          ///< 频段滑动平均窗口（参考 Rainmeter AverageSize=6）
};

} // namespace snowdesktop::audio
