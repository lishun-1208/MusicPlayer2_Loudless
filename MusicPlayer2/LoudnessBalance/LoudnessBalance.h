#pragma once
// =============================================================================
//  LoudnessBalance —— 实时响度平衡模块（核心实现，不依赖 MFC / BASS / 播放器）
//
//  设计要点
//  1) 只读源文件
//     响度由 ffmpeg 子进程用 ebur128 滤镜分析（EBU R128 / ITU-R BS.1770）。
//     ffmpeg 只读取音频数据，输出到 NUL，不会写入标签或音频数据，
//     源文件的任何字节都不会被修改。
//  2) 实时施加增益
//     分析结果被整理成一条"时间 -> 增益(dB)"曲线；播放时由 DSP 按当前播放
//     位置逐样本乘上线性增益，全程只在内存中进行。曲线按时间索引，因此
//     拖动进度条、AB 重复、变速播放都能立刻对齐到正确的位置。
//  3) 两种平衡方式
//     dynamic = false（默认）：整曲响度平衡。用综合响度 I 算出一个固定增益，
//                              同一首曲子内部动态完整保留，只平衡曲目间差异。
//     dynamic = true ：逐段响度平衡。用短时响度 S 生成随时间变化的增益曲线，
//                              同时压平曲目内部的段落差异（类似慢速压缩器）。
//  4) 峰值保护
//     增益受 true peak 约束，保证施加增益后的峰值不超过 ceiling_dbtp，
//     即模块不会引入削波。
//
//  该文件可以脱离播放器单独编译（见 test/loudness_test.cpp）。
// =============================================================================

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace loudness
{
    // 无法识别的响度值（静音 / 不足一帧）统一按此值处理
    constexpr float INVALID_LUFS = -70.0f;
    // ffmpeg ebur128 的逐帧步长（秒）
    constexpr double FRAME_STEP = 0.1;

    struct Settings
    {
        bool  enable = true;         // 模块总开关
        bool  dynamic = false;       // false: 整曲响度平衡；true: 逐段响度平衡
        float target_lufs = -18.0f;  // 目标响度（LUFS）
        float max_boost_db = 12.0f;  // 最大提升量（dB）
        float max_cut_db = 12.0f;    // 最大衰减量（dB）
        float ceiling_dbtp = -1.0f;  // 峰值上限（dBTP），增益不会让峰值超过它
        float attack_ms = 200.0f;    // 增益变大时的时间常数（逐段模式用）
        float release_ms = 800.0f;   // 增益变小时的时间常数（逐段模式用）
        float max_rate_db_per_sec = 24.0f;  // 增益变化速率上限，防止突变产生爆音
        bool  prefetch = true;       // 播放当前曲目时提前分析下一曲
        bool  pause_until_ready = true; // 曲线就绪前暂停播放，避免开头按原始音量播放
        std::wstring ffmpeg_path;    // ffmpeg.exe 路径，留空则自动查找
    };

    // 时间索引的增益曲线
    struct GainCurve
    {
        struct Point
        {
            double t;       // 时间（秒，相对音频文件开头）
            float gain_db;  // 增益（dB）
        };

        std::vector<Point> points;
        float integrated_lufs = 0.0f;   // 整曲综合响度 I
        float true_peak_dbtp = 0.0f;    // 整曲 true peak
        float min_gain_db = 0.0f;
        float max_gain_db = 0.0f;
        bool  dynamic = false;
        double duration = 0.0;          // 音频总时长（秒）

        bool Empty() const { return points.empty(); }
        // 取指定时间点的增益（线性插值，越界时取端点值）
        float GainAt(double seconds) const;
    };

    // 查找可用的 ffmpeg.exe。查找顺序：settings.ffmpeg_path -> 程序目录 ->
    // 程序目录\ffmpeg\bin -> PATH。找不到返回空字符串。
    std::wstring FindFfmpeg(const Settings& settings);

    // 分析一个音频文件的响度并生成增益曲线。
    // 该函数为阻塞调用（普通曲目耗时约 0.3~1.5 秒），不要在播放线程里直接调用，
    // 一般通过 Balancer 使用。失败时返回 false 并填写 error。
    bool AnalyzeFile(const std::wstring& file_path, const Settings& settings,
                     GainCurve& curve, std::wstring& error);

    // 实时增益施加器（DSP 侧）。由音频线程调用，内部有锁但临界区极短。
    class GainApplier
    {
    public:
        void SetSettings(const Settings& settings);
        void SetCurve(std::shared_ptr<const GainCurve> curve);
        void Reset();   // 换曲时调用，清掉曲线与增益状态

        // 原地处理一个交错排列的 PCM 缓冲。
        // frames: 采样帧数；channels: 声道数；freq: 采样率；
        // pos_sec: 本缓冲第一个采样对应的播放位置（秒）。
        void ProcessFloat(float* buffer, size_t frames, int channels, uint32_t freq, double pos_sec);
        void ProcessS16(int16_t* buffer, size_t frames, int channels, uint32_t freq, double pos_sec);

        float GetCurrentGainDb() const { return m_cur_db; }
        bool HasCurve() const;

    private:
        struct BlockGain
        {
            float lin_start = 1.0f;   // 本块第一个采样的线性增益
            float factor = 1.0f;      // 每个采样递增的乘数（保证块内增益线性变化）
        };
        BlockGain ComputeBlock(size_t frames, uint32_t freq, double pos_sec);

        mutable std::mutex m_mutex;
        std::shared_ptr<const GainCurve> m_curve;
        Settings m_settings;
        float m_cur_db = 0.0f;          // 当前已施加的增益（dB）
        double m_last_end_pos = -1.0;   // 上一块的结束位置，用于检测跳转
        uint32_t m_fast_frames = 0;     // 跳转后允许的快速过渡剩余采样数
    };

    // 全局协调器：负责后台分析线程、曲线缓存、播放状态通知。
    class Balancer
    {
    public:
        enum class State
        {
            Disabled,   // 模块被关闭
            Idle,       // 空闲
            Analyzing,  // 正在分析
            Ready,      // 分析完成，曲线可用
            Failed,     // 分析失败
            NoFfmpeg,   // 找不到 ffmpeg
        };

        static Balancer& Instance();

        // 设置参数。参数发生变化时会清空缓存（下次播放重新分析）。
        void Configure(const Settings& settings);
        Settings GetSettings() const;

        // 通知模块"开始播放某个文件"，立即返回，分析在后台线程进行。
        // 同一个文件在同一会话内只会分析一次（有内存缓存）。
        void OnTrackOpened(const std::wstring& file_path);
        // 通知模块"停止播放"，取消正在进行的分析。
        void OnTrackClosed();

        // 预取：提前分析即将播放的文件，结果只写入缓存，不改变当前播放状态。
        // 用于"播放当前曲目的同时处理下一曲"：切歌时缓存已命中，不会出现
        // 开头几百毫秒仍按原音量播放的情况。立即返回，分析在独立线程进行。
        // 同一时间只保留一个预取任务，新任务会覆盖尚未开始的旧任务。
        void Prefetch(const std::wstring& file_path);

        // 供音频线程取当前曲线（try_lock，绝不阻塞音频线程）
        std::shared_ptr<const GainCurve> TryGetCurve();

        State GetState() const { return m_state; }
        std::wstring GetLastError() const;   // 线程安全
        float GetIntegratedLufs() const { return m_lufs; }
        std::wstring GetCurrentFile() const; // 线程安全

        // 诊断日志回调（可为空）。会在后台线程中被调用。
        void SetLogger(std::function<void(const std::wstring&)> logger);

        // 状态变化回调（可为空）。会在后台线程中被调用，用于"分析完成后自动恢复播放"。
        void SetStateCallback(std::function<void(State)> cb);

        Balancer(const Balancer&) = delete;
        Balancer& operator=(const Balancer&) = delete;
        ~Balancer();

    private:
        Balancer() = default;

        void JoinWorker();
        void AnalyseWorker(std::wstring file_path, Settings settings, uint64_t generation);
        void StopPrefetch();
        void PrefetchWorker();

        mutable std::mutex m_curve_mutex;       // 保护 m_curve
        std::shared_ptr<const GainCurve> m_curve;

        mutable std::mutex m_settings_mutex;    // 保护 m_settings
        Settings m_settings;
        uint64_t m_config_gen = 0;              // 参数代数：变化时递增，用于丢弃过期的预取结果

        mutable std::mutex m_state_mutex;       // 保护 m_state / m_error / m_lufs / m_file
        State m_state = State::Idle;
        std::wstring m_error;
        float m_lufs = 0.0f;
        std::wstring m_file;

        std::mutex m_worker_mutex;              // 保护 m_worker / m_generation
        std::thread m_worker;
        uint64_t m_generation = 0;
        std::atomic<bool> m_cancel{ false };

        std::mutex m_log_mutex;
        std::function<void(const std::wstring&)> m_logger;

        std::mutex m_cb_mutex;
        std::function<void(State)> m_state_cb;
        void NotifyState(State state);      // 在锁外调用状态回调

        // 会话内缓存（按文件路径）
        std::mutex m_cache_mutex;
        std::vector<std::pair<std::wstring, std::shared_ptr<const GainCurve>>> m_cache;

        // 预取线程：播放当前曲目时，提前分析下一曲并写入缓存
        std::thread m_prefetch_thread;
        std::mutex m_pf_mutex;
        std::condition_variable m_pf_cv;
        std::wstring m_pf_path;             // 待预取的路径，空表示没有任务
        bool m_pf_exit = false;
        std::atomic<bool> m_pf_cancel{ false };

        void Log(const std::wstring& msg);
        std::shared_ptr<const GainCurve> FindCached(const std::wstring& path);
        void PutCache(const std::wstring& path, std::shared_ptr<const GainCurve> curve);
    };

} // namespace loudness
