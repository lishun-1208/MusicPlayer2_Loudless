#pragma once
// =============================================================================
//  LoudnessBalanceBass —— 实时响度平衡与播放器的适配层
//
//  这一层只做三件事：
//   1) 从配置文件读写响度平衡参数；
//   2) 在播放器打开/关闭音频时通知核心模块；
//   3) 把 BASS 的 DSP 回调转给核心模块（GainApplier）。
//  真正的响度分析与增益计算都在 LoudnessBalance.h/.cpp 中，与播放器无关。
// =============================================================================

#include <string>

namespace LoudnessBalance
{
    // 从配置文件的 [loudness] 段读取参数（首次调用时自动加载，之后直接返回缓存）
    void LoadSettings();
    // 把当前参数写回配置文件
    void SaveSettings();

    // 播放器打开了一个音频文件后调用。bass_handle 为 BASS 通道句柄（非 BASS 内核传 0）
    void OnTrackOpened(const std::wstring& file_path, unsigned int bass_handle);
    // 播放器关闭音频文件前调用
    void OnTrackClosed(unsigned int bass_handle);

    // 响度曲线还在分析、并且开了 pause_until_ready 时返回 true。
    // 播放器在真正开始播放前调用它：返回 true 就先不播，等分析完成后由模块自动开始。
    // 返回 false 时按正常流程播放。
    bool HoldPlaybackUntilReady();

    // 用户主动暂停/停止播放时调用：取消"分析完自动开始播放"的等待。
    void CancelPendingResume();

    // 开关（会立即生效并写入配置文件）
    void SetEnabled(bool enable);
    bool IsEnabled();

    // 当前状态的简短描述，可用于状态栏/提示
    std::wstring GetStatusText();
}
