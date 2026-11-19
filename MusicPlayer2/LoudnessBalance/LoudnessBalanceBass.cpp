#include "stdafx.h"
#include "LoudnessBalanceBass.h"

#include <atomic>

#include "bass.h"
#include "Common.h"
#include "FilePathHelper.h"
#include "IniHelper.h"
#include "MusicPlayer2.h"
#include "Player.h"

#include "LoudnessBalance.h"

namespace LoudnessBalance
{
    namespace
    {
        // ---- 模块状态 -----------------------------------------------------
        bool g_loaded = false;
        loudness::Settings g_settings;
        loudness::GainApplier g_applier;

        DWORD g_channel = 0;        // 已挂载 DSP 的通道
        HDSP  g_dsp = 0;
        DWORD g_freq = 0;
        int   g_chans = 0;
        bool  g_is_float = true;
        bool  g_logged_no_ffmpeg = false;

        // 因等待响度分析而暂停的通道（0 表示没有在等待）。
        // 同时记录对应的文件路径：切歌后旧任务的回调不应该去恢复新曲的播放。
        std::mutex g_wait_mutex;
        DWORD g_waiting_channel = 0;
        std::wstring g_waiting_path;

        void SetWaiting(DWORD channel, const std::wstring& path)
        {
            std::lock_guard<std::mutex> lock(g_wait_mutex);
            g_waiting_channel = channel;
            g_waiting_path = path;
        }

        void ClearWaiting()
        {
            std::lock_guard<std::mutex> lock(g_wait_mutex);
            g_waiting_channel = 0;
            g_waiting_path.clear();
        }

        // 只有真正能从文件里读出音频的对象才做响度分析：
        // 网络流（分析会把流再下载一遍）和 MIDI（播放时由音色库渲染，与文件响度无关）都不处理。
        bool IsAnalysable(const std::wstring& file_path)
        {
            if (file_path.empty()) return false;
            if (CCommon::IsURL(file_path)) return false;
            CFilePathHelper helper(file_path);
            std::wstring ext = helper.GetFileExtension();
            if (ext == L"mid" || ext == L"midi" || ext == L"rmi") return false;
            return CCommon::FileExist(file_path);
        }

        // 分析状态变化（在后台线程调用）：曲线就绪或失败后，恢复因等待而暂停的通道
        void OnBalancerStateChanged(loudness::Balancer::State state)
        {
            if (state == loudness::Balancer::State::Analyzing) return;

            const std::wstring cur = loudness::Balancer::Instance().GetCurrentFile();
            DWORD ch = 0;
            {
                std::lock_guard<std::mutex> lock(g_wait_mutex);
                if (g_waiting_channel != 0 && g_waiting_path == cur)
                {
                    ch = g_waiting_channel;
                    g_waiting_channel = 0;
                    g_waiting_path.clear();
                }
            }
            if (ch == 0) return;

            // 恢复播放。暂停是模块自己做的；如果用户在此期间点了暂停/停止，
            // 那些命令会先清掉等待状态，这里就不会再恢复。
            BASS_ChannelPlay(ch, FALSE);    // 句柄已失效时 BASS 只返回错误，不会崩
            theApp.WriteLog(L"[响度平衡] 分析完成，继续播放");
        }

        void DetachDsp()
        {
            ClearWaiting();
            if (g_dsp != 0 && g_channel != 0)
                BASS_ChannelRemoveDSP(g_channel, g_dsp);
            g_dsp = 0;
            g_channel = 0;
            g_freq = 0;
            g_chans = 0;
        }

        // BASS DSP 回调（在 BASS 的混音线程中被调用，必须尽量短）
        void CALLBACK DspProc(HDSP, DWORD channel, void* buffer, DWORD length, void*)
        {
            if (buffer == nullptr || length == 0 || g_freq == 0 || g_chans <= 0)
                return;

            // 取当前曲目的增益曲线（try_lock，取不到就沿用上一次的曲线）
            auto curve = loudness::Balancer::Instance().TryGetCurve();
            if (curve)
                g_applier.SetCurve(std::move(curve));

            // 播放位置：BASS 返回的是"即将被渲染"的位置，与曲线按时间对齐
            QWORD pos_bytes = BASS_ChannelGetPosition(channel, BASS_POS_BYTE);
            if (pos_bytes == (QWORD)-1)
                return;
            const double pos_sec = BASS_ChannelBytes2Seconds(channel, pos_bytes);

            if (g_is_float)
            {
                const size_t frames = length / (sizeof(float) * static_cast<size_t>(g_chans));
                g_applier.ProcessFloat(static_cast<float*>(buffer), frames, g_chans, g_freq, pos_sec);
            }
            else
            {
                const size_t frames = length / (sizeof(int16_t) * static_cast<size_t>(g_chans));
                g_applier.ProcessS16(static_cast<int16_t*>(buffer), frames, g_chans, g_freq, pos_sec);
            }
        }

        void LogOnceNoFfmpeg()
        {
            if (g_logged_no_ffmpeg) return;
            g_logged_no_ffmpeg = true;
            theApp.WriteLog(L"[响度平衡] 未找到 ffmpeg.exe，响度平衡功能不可用。"
                L"请把 ffmpeg.exe 放到程序目录，或在 config.ini 的 [loudness] 段中设置 ffmpeg_path。");
        }

        // 提前分析播放列表里的下一曲：只写缓存，不影响当前播放。
        // 下一曲按当前播放模式计算（顺序/循环/无序遍历），随机模式或列表末尾
        // 拿不到确定的下一曲时就不预取。
        void PrefetchNextTrack(const std::wstring& current_path)
        {
            SongInfo next = CPlayer::GetInstance().GetNextTrack();
            if (next.file_path.empty()) return;
            if (next.file_path == current_path) return;     // 单曲循环等情况，无需预取
            if (!IsAnalysable(next.file_path)) return;
            loudness::Balancer::Instance().Prefetch(next.file_path);
        }
    } // namespace

    // -----------------------------------------------------------------------
    // 配置读写
    // -----------------------------------------------------------------------
    void LoadSettings()
    {
        if (g_loaded) return;
        g_loaded = true;

        CIniHelper ini(theApp.m_config_path);

        // 首次运行时把默认参数写进配置文件，方便用户发现和修改
        bool first_time = (ini.GetString(L"loudness", L"enable", L"__none__") == L"__none__");

        g_settings.enable = ini.GetBool(L"loudness", L"enable", true);
        g_settings.dynamic = ini.GetBool(L"loudness", L"dynamic", false);
        g_settings.target_lufs = static_cast<float>(ini.GetDouble(L"loudness", L"target_lufs", -18.0));
        g_settings.max_boost_db = static_cast<float>(ini.GetDouble(L"loudness", L"max_boost_db", 12.0));
        g_settings.max_cut_db = static_cast<float>(ini.GetDouble(L"loudness", L"max_cut_db", 12.0));
        g_settings.ceiling_dbtp = static_cast<float>(ini.GetDouble(L"loudness", L"ceiling_dbtp", -1.0));
        g_settings.attack_ms = static_cast<float>(ini.GetDouble(L"loudness", L"attack_ms", 200.0));
        g_settings.release_ms = static_cast<float>(ini.GetDouble(L"loudness", L"release_ms", 800.0));
        g_settings.prefetch = ini.GetBool(L"loudness", L"prefetch", true);
        g_settings.pause_until_ready = ini.GetBool(L"loudness", L"pause_until_ready", true);
        g_settings.ffmpeg_path = ini.GetString(L"loudness", L"ffmpeg_path", L"");

        // 参数合法性检查，防止配置文件被改坏后出现异常增益
        if (g_settings.target_lufs > -5.0f) g_settings.target_lufs = -5.0f;
        if (g_settings.target_lufs < -40.0f) g_settings.target_lufs = -40.0f;
        if (g_settings.max_boost_db < 0.0f) g_settings.max_boost_db = 0.0f;
        if (g_settings.max_boost_db > 30.0f) g_settings.max_boost_db = 30.0f;
        if (g_settings.max_cut_db < 0.0f) g_settings.max_cut_db = 0.0f;
        if (g_settings.max_cut_db > 30.0f) g_settings.max_cut_db = 30.0f;
        if (g_settings.ceiling_dbtp > 0.0f) g_settings.ceiling_dbtp = 0.0f;
        if (g_settings.attack_ms < 10.0f) g_settings.attack_ms = 10.0f;
        if (g_settings.release_ms < 10.0f) g_settings.release_ms = 10.0f;

        g_applier.SetSettings(g_settings);
        loudness::Balancer::Instance().Configure(g_settings);
        loudness::Balancer::Instance().SetLogger([](const std::wstring& msg) { theApp.WriteLog(msg); });
        loudness::Balancer::Instance().SetStateCallback(OnBalancerStateChanged);

        if (first_time)
        {
            ini.WriteBool(L"loudness", L"enable", g_settings.enable);
            ini.WriteBool(L"loudness", L"dynamic", g_settings.dynamic);
            ini.WriteDouble(L"loudness", L"target_lufs", g_settings.target_lufs);
            ini.WriteDouble(L"loudness", L"max_boost_db", g_settings.max_boost_db);
            ini.WriteDouble(L"loudness", L"max_cut_db", g_settings.max_cut_db);
            ini.WriteDouble(L"loudness", L"ceiling_dbtp", g_settings.ceiling_dbtp);
            ini.WriteDouble(L"loudness", L"attack_ms", g_settings.attack_ms);
            ini.WriteDouble(L"loudness", L"release_ms", g_settings.release_ms);
            ini.WriteBool(L"loudness", L"prefetch", g_settings.prefetch);
            ini.WriteBool(L"loudness", L"pause_until_ready", g_settings.pause_until_ready);
            ini.Save();
        }
    }

    void SaveSettings()
    {
        CIniHelper ini(theApp.m_config_path);
        ini.WriteBool(L"loudness", L"enable", g_settings.enable);
        ini.WriteBool(L"loudness", L"dynamic", g_settings.dynamic);
        ini.WriteDouble(L"loudness", L"target_lufs", g_settings.target_lufs);
        ini.WriteDouble(L"loudness", L"max_boost_db", g_settings.max_boost_db);
        ini.WriteDouble(L"loudness", L"max_cut_db", g_settings.max_cut_db);
        ini.WriteDouble(L"loudness", L"ceiling_dbtp", g_settings.ceiling_dbtp);
        ini.WriteDouble(L"loudness", L"attack_ms", g_settings.attack_ms);
        ini.WriteDouble(L"loudness", L"release_ms", g_settings.release_ms);
        ini.WriteBool(L"loudness", L"prefetch", g_settings.prefetch);
        ini.WriteBool(L"loudness", L"pause_until_ready", g_settings.pause_until_ready);
        ini.WriteString(L"loudness", L"ffmpeg_path", g_settings.ffmpeg_path);
        ini.Save();
    }

    // -----------------------------------------------------------------------
    // 播放器事件
    // -----------------------------------------------------------------------
    void OnTrackOpened(const std::wstring& file_path, unsigned int bass_handle)
    {
        LoadSettings();

        // 先摘掉上一个文件的 DSP，避免它继续用旧曲线处理新文件的头几个缓冲
        DetachDsp();
        g_applier.Reset();
        g_applier.SetSettings(g_settings);

        auto& balancer = loudness::Balancer::Instance();

        if (!g_settings.enable || !IsAnalysable(file_path))
        {
            balancer.OnTrackClosed();
            return;
        }

        if (loudness::FindFfmpeg(g_settings).empty())
        {
            LogOnceNoFfmpeg();
            balancer.OnTrackClosed();
            return;
        }

        // 通知核心模块开始分析（异步，立即返回）
        balancer.Configure(g_settings);
        balancer.OnTrackOpened(file_path);

        // 只有 BASS 内核才能挂 DSP，其它内核（MCI / FFmpeg）不做处理
        if (bass_handle == 0) return;

        BASS_CHANNELINFO info{};
        if (!BASS_ChannelGetInfo(bass_handle, &info)) return;
        if (info.chans == 0 || info.freq == 0) return;

        g_freq = info.freq;
        g_chans = static_cast<int>(info.chans);
        g_is_float = (info.flags & BASS_SAMPLE_FLOAT) != 0;
        g_channel = bass_handle;
        g_dsp = BASS_ChannelSetDSP(bass_handle, DspProc, nullptr, 0);

        // 注意：这里不暂停、也不登记“等待恢复”。
        // 打开文件 != 要求播放：程序启动时即使没开启自动播放，也会 OPEN 上次的曲目，
        // 若在这里登记等待，分析完成后就会把音频直接放出来，表现为
        // “启动自动播放、设置里关不掉、界面还停在暂停”。
        // 等待逻辑只在 Command::PLAY / PLAY_PAUSE 调用的 HoldPlaybackUntilReady 里建立。
        ClearWaiting();

        // 播放当前曲目的同时，把下一曲也分析好：切歌时缓存已命中，
        // 不会有开头几百毫秒仍按原始音量播放的窗口。
        PrefetchNextTrack(file_path);
    }

    void OnTrackClosed(unsigned int bass_handle)
    {
        DetachDsp();
        g_applier.Reset();
        loudness::Balancer::Instance().OnTrackClosed();
    }

    void CancelPendingResume()
    {
        // 用户主动暂停或停止播放：不要再自动恢复播放
        ClearWaiting();
    }

    bool HoldPlaybackUntilReady()
    {
        LoadSettings();
        if (!g_settings.enable || !g_settings.pause_until_ready) return false;

        auto& balancer = loudness::Balancer::Instance();
        // 分析已经结束（就绪 / 失败 / 未启用）：不拦，正常播放即可
        if (balancer.GetState() != loudness::Balancer::State::Analyzing)
        {
            ClearWaiting();
            return false;
        }

        const DWORD ch = g_channel;
        if (ch == 0) return false;      // 非 BASS 内核或无有效通道

        // 走到这里说明播放器明确要求播放（PLAY / PLAY_PAUSE），此时才登记等待。
        // 登记点放在这里而不是 OnTrackOpened，是为了区分“打开文件”和“要求播放”。
        bool was_waiting;
        {
            std::lock_guard<std::mutex> lock(g_wait_mutex);
            was_waiting = (g_waiting_channel == ch);
        }
        SetWaiting(ch, balancer.GetCurrentFile());

        // 通道此刻通常是 stopped（OPEN 后还没 Play），Pause 只是兜底：
        // 确保曲线就绪之前不会有任何原始音量漏出来。
        BASS_ChannelPause(ch);

        if (!was_waiting)
            theApp.WriteLog(L"[响度平衡] 正在分析响度，播放已暂停，就绪后自动开始");
        return true;
    }

    // -----------------------------------------------------------------------
    // 开关与状态
    // -----------------------------------------------------------------------
    void SetEnabled(bool enable)
    {
        LoadSettings();
        if (g_settings.enable == enable) return;
        g_settings.enable = enable;
        g_applier.SetSettings(g_settings);
        loudness::Balancer::Instance().Configure(g_settings);
        if (!enable)
        {
            g_applier.Reset();
            loudness::Balancer::Instance().OnTrackClosed();
        }
        SaveSettings();
    }

    bool IsEnabled()
    {
        LoadSettings();
        return g_settings.enable;
    }

    std::wstring GetStatusText()
    {
        LoadSettings();
        if (!g_settings.enable) return L"响度平衡：已关闭";

        auto& balancer = loudness::Balancer::Instance();
        wchar_t buf[128] = { 0 };
        switch (balancer.GetState())
        {
        case loudness::Balancer::State::Analyzing:
            return L"响度平衡：分析中…";
        case loudness::Balancer::State::Ready:
            swprintf_s(buf, L"响度平衡：%.1f LUFS → %.1f LUFS",
                balancer.GetIntegratedLufs(), g_settings.target_lufs);
            return buf;
        case loudness::Balancer::State::Failed:
            return L"响度平衡：分析失败（" + balancer.GetLastError() + L"）";
        case loudness::Balancer::State::NoFfmpeg:
            return L"响度平衡：未找到 ffmpeg.exe";
        case loudness::Balancer::State::Disabled:
            return L"响度平衡：已关闭";
        default:
            return L"响度平衡：就绪";
        }
    }
}
