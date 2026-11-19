# 响度平衡模块（LoudnessBalance）

给 MusicPlayer2 增加"响度平衡"能力的独立模块：把不同曲目、不同母带的音量拉齐，
播放时**实时**施加增益，**绝不修改源文件**。

- 响度测量：调用 ffmpeg 的 `ebur128` 滤镜（EBU R128 / ITU-R BS.1770）
- 增益施加：在 BASS 的 DSP 回调里按播放位置逐样本乘增益（纯内存操作）
- 对源码的侵入：`Player.cpp` 加 3 行；`MusicPlayer2.vcxproj` 登记 4 个文件

---

## 1. 为什么是"实时"而不是"改文件"

传统做法（ReplayGain 标签 / 重新编码）要么往文件里写标签，要么生成新文件。
本模块两者都不做：

| 关注点 | 本模块的做法 |
| --- | --- |
| 源文件 | 只读。ffmpeg 的输入是文件，输出丢弃到 `null`，不写任何字节 |
| 生效时机 | 播放过程中由 DSP 实时施加，改参数（目标响度/模式）下一首立即生效 |
| 只读文件 / 网络盘 | 正常工作（不提权、不写盘） |
| 换目标响度 | 无需重新处理文件，重算一条曲线即可 |

---

## 2. 工作流程

```
 打开曲目
    │
    ├─► Player.cpp: LoudnessBalance::OnTrackOpened(file, bass_handle)
    │        │
    │        ├─(后台线程)─────────────────────────────────────────────┐
    │        │   ffmpeg -i file -af ebur128=peak=true:metadata=1,     │
    │        │            ametadata=mode=print  -f null -             │
    │        │   → 每 100ms 一行：M / S / I (LUFS) + true_peak(线性)  │
    │        │   → 生成"时间 → 增益(dB)"曲线（峰值约束 + 平滑）      │
    │        └──── 曲线就绪（约 0.7 秒，见下）───────────────────────┘
    │                     │
    └─► BASS: BASS_ChannelSetDSP(handle, DspProc)
                         │
                         ▼
              DspProc：取曲线 → 按 BASS_ChannelGetPosition 得到播放位置
                        → GainApplier 逐样本乘线性增益（速率限制，防爆音）
```

曲线是**按时间索引**的，所以拖动进度条、AB 重复、单曲循环都不需要额外处理：
位置跳到哪，增益就跟到哪（跳转后 50ms 内平滑过渡）。

分析一本 4~5 分钟的 FLAC 约 **0.7 秒**（ffmpeg 解码 + 响度统计约 600 倍速于实时），
而且结果在本会话内缓存——同一首歌重复播放、来回切歌都不会再跑 ffmpeg。

### 预取：播当前曲的同时处理下一曲

如果等到切歌之后才分析，开头几百毫秒会仍按原音量播放。为此模块在开始播放一首
曲目时，会顺便把播放列表里的**下一曲**也分析好并放入缓存：

```
开始播放 A
  ├─ 分析 A（后台线程，约 0.7 秒）
  └─ 预取 B（另一个后台线程，与 A 并行；结果只写缓存，不碰当前播放状态）
切到 B  ──► 缓存命中，0 延迟，第一块缓冲就用上正确增益
```

- 下一曲按当前播放模式计算（顺序 / 列表循环 / 无序遍历）。随机播放、或已经是
  列表最后一首时拿不到确定的下一曲，就不预取——切过去现分析，功能不受影响。
- 预取线程只有一个，新任务覆盖尚未开始的旧任务；已在缓存中的文件不会重复分析。
- 只在当前曲目确实由 BASS 播放时预取（MCI / FFmpeg 内核本来就跳过该模块）。
- 日志里可以直接看到预取生效：

  ```
  [响度平衡] 已就绪：I = -16.6 LUFS, 增益 -1.4 ~ -1.4 dB, 目标 -18.0 LUFS
  [响度平衡] 已预取下一曲：Harvest.flac（切歌时直接生效）
  [响度平衡] 已就绪（命中缓存，无需分析）：I = -9.2 LUFS, 增益 -8.8 ~ -8.8 dB, 目标 -18.0 LUFS
  ```

---

## 3. 两种平衡模式

| | 整曲平衡（默认，`dynamic=0`） | 逐段平衡（`dynamic=1`） |
| --- | --- | --- |
| 依据 | 整曲综合响度 `I` | 每 100ms 的短时响度 `S` |
| 增益 | 全曲一个固定值 | 随时间变化（-12 ~ +12 dB 内） |
| 曲目内动态 | 完整保留 | 被压平（类似慢速压缩器） |
| 曲目间音量 | 拉齐 | 拉齐 |
| 适用 | 音乐播放（推荐） | 有声书、播客、现场录音等忽大忽小的素材 |

实测（`test/run_test.py`，目标 -18 LUFS）：

```
整曲平衡：
  文件                      原始 LUFS   平衡后   true peak
  Harvest.flac                -9.20     -18.00    -8.20 dBTP
  Happy Birthday to.. .flac  -16.60     -18.00    -7.80 dBTP
  曲目间响度极差：7.4 dB → 0.00 dB

逐段平衡：
  Harvest.flac                -9.20     -17.60    -1.50 dBTP
  Happy Birthday to.. .flac  -16.60     -17.80    -1.00 dBTP
  曲目间响度极差：7.4 dB → 0.20 dB
```

---

## 4. 峰值保护（不会引入削波）

增益受 true peak 约束：

```
allowed_gain = ceiling_dbtp - true_peak(该段)
final_gain   = min(target_lufs - loudness, max_boost, allowed_gain)
```

- 整曲模式用整曲最大 true peak 做约束 → 全曲恒定增益，峰值绝不越界；
- 逐段模式用每 100ms 的 true peak 做约束，并且在**平滑之后**再夹一次
  （否则平滑会把相邻帧的较大增益带过来，导致峰值超限）。

两个测试文件里 `Harvest.flac` 本身就有 +0.5 dBTP 的过冲，处理后输出为 -8.2 dBTP，
说明模块只做衰减时不会让峰值变差；需要提升的素材则会被峰值上限"顶住"，
宁可少提升也不削波（若日后需要在保持峰值的前提下多提升，可以再加一级
look-ahead 限幅器，见第 9 节）。

---

## 5. 文件清单

```
MusicPlayer2/LoudnessBalance/
├─ LoudnessBalance.h          核心接口（纯 C++，不依赖 MFC/BASS）
├─ LoudnessBalance.cpp        核心实现：ffmpeg 分析、曲线生成、增益施加
├─ LoudnessBalanceBass.h      与播放器的适配层接口
├─ LoudnessBalanceBass.cpp    配置读写 + BASS DSP 挂载
├─ README.md
└─ test/
   ├─ loudness_test.cpp       独立验证程序（控制台，不需要 MFC）
   ├─ build_test.bat          编译上面这个程序
   └─ run_test.py             端到端验证：测量 → 渲染 → 复测 → 校验源文件
```

`LoudnessBalance.cpp` 在工程里被设置为**不使用预编译头**，因此可以脱离 MFC 单独编译，
测试程序就是直接把它和 `loudness_test.cpp` 一起编译出来的。

---

## 6. 接入方式（对原代码的全部改动）

`Player.cpp`：打开/关闭曲目时各通知一次

```cpp
#include "LoudnessBalance\LoudnessBalanceBass.h"

// MusicControl(Command::OPEN) 中，m_pCore->Open() 之后：
LoudnessBalance::OnTrackOpened(cur_song.file_path, GetBassHandle());

// MusicControl(Command::CLOSE) 中，m_pCore->Close() 之前：
LoudnessBalance::OnTrackClosed(GetBassHandle());
```

`MusicPlayer2.vcxproj`：登记 2 个 .h + 2 个 .cpp（其中 `LoudnessBalance.cpp`
的 `PrecompiledHeader` 设为 `NotUsing`）。

除此之外没有改动任何原有文件——均衡器、混响、波形/频谱、淡入淡出都不受影响
（响度增益作用在独立的 DSP 上，与通道音量相乘，两者互不干扰）。

---

## 7. 配置（`config.ini` 的 `[loudness]` 段）

首次运行时模块会自动把默认值写入配置文件：

```ini
[loudness]
enable=1              ; 总开关
dynamic=0             ; 0=整曲平衡（默认）, 1=逐段平衡
target_lufs=-18       ; 目标响度：-18 适合本地播放；想更响可用 -16/-14
max_boost_db=12       ; 最多提升多少 dB
max_cut_db=12         ; 最多衰减多少 dB
ceiling_dbtp=-1       ; 峰值上限（dBTP）
attack_ms=200         ; 增益变大时的时间常数（逐段模式）
release_ms=800        ; 增益变小时的时间常数（逐段模式）
prefetch=1            ; 播放当前曲时提前分析下一曲（默认开）
pause_until_ready=1   ; 分析没完成时暂停播放，就绪后自动开始（默认开）
ffmpeg_path=          ; 留空则自动查找
```

### 两个体验相关的开关

| 参数 | 默认 | 说明 |
| --- | --- | --- |
| `prefetch` | `1` | 播放当前曲目的同时，后台把下一曲也分析好。关掉后切歌时仍要现分析。每首曲子多一个 ffmpeg 进程（约 0.7 秒），机器很慢时可以关。 |
| `pause_until_ready` | `1` | 曲线还没算出来时先把播放暂停，算完自动继续。关掉则立即出声，但在分析完成前（约 0.5 秒）是原始音量。 |

`pause_until_ready` 的行为细节：

- 只在**该曲目没有缓存**时才可能暂停；命中预取缓存时直接播放，不会有停顿。
- 用户在等待期间点**暂停**或**停止**，模块不会在分析完成后强行播放。
- 分析失败时也会恢复播放（按原始音量），不会一直停着。

参数越界会被自动纠正（比如 `target_lufs` 被限制在 -40 ~ -5），
所以手改配置不会把增益搞成怪物值。

### 改完必须重启程序

配置在进程内**只加载一次**（`LoadSettings` 有 `g_loaded` 保护），
所以改完 `config.ini` 后要**完全退出程序再启动**，不是最小化、也不是切歌。
建议顺序：先关闭程序 → 改 ini → 再启动。

### 目标响度怎么选

| `target_lufs` | 适合场景 | 说明 |
|---|---|---|
| `-14` | 想听响、手机外放 | 接近 Spotify / YouTube 的响度水平，动态被压得较多 |
| `-16` | 流行 / 摇滚 | Apple Music、播客常用值，比默认响一档 |
| `-18` | 默认，本地曲库 | 舒适、耐听，和原文件差距大的歌会被明显拉平 |
| `-20` ~ `-23` | 古典、爵士、深夜听 | -23 是 EBU R128 广播标准，动态保留最多 |

同一批歌只要目标值一致，曲目之间就会一样响；目标值本身只决定"整体多响"。

**ffmpeg 查找顺序**：`ffmpeg_path` → 程序目录 `ffmpeg.exe` →
程序目录 `ffmpeg\bin\ffmpeg.exe` / `bin\ffmpeg.exe` → `PATH`。
找不到时功能自动停用，只写一条日志，不影响播放。

---

## 8. 测试与验证

```bat
cd MusicPlayer2\LoudnessBalance\test
build_test.bat                 :: 需要 MSVC（不需要 MFC）
python run_test.py             :: 整曲平衡
python run_test.py --dynamic 1 :: 逐段平衡
python run_test.py --target -16
```

`run_test.py` 对 `test/` 目录里的每个音频文件依次做：

1. 记录源文件 SHA-256；
2. ffmpeg 测量"原始"响度与 true peak；
3. `loudness_test analyze` —— 用真实模块（Balancer）输出解析结果；
4. `loudness_test render` —— 走与 DSP 完全相同的代码路径（`TryGetCurve` +
   `GainApplier` 逐块处理）渲染出 wav；
5. 再测渲染结果的响度与 true peak；
6. 重新计算源文件 SHA-256，确认**没有被修改**。

也可以单独用 `loudness_test` 看细节：

```bat
bin\loudness_test.exe analyze "D:\music\a.flac"
bin\loudness_test.exe render  "D:\music\a.flac" out.wav -16 1
```

---

## 9. 已知限制 / 可以继续做

- **只对 BASS 播放内核生效**（默认内核）。MCI / FFmpeg 内核拿不到可挂 DSP 的通道，
  调用时传入 0 句柄会自动跳过。
- **网络流和 MIDI 不做分析**：前者会让 ffmpeg 把流再下载一遍，后者播放时由音色库
  渲染，与文件本身的响度无关。
- **开头约 0.7 秒按 0 dB 播放**：分析还没完成。实际播放时这段正好在淡入过程中，
  听不出来；如果在意，可以在 `OnTrackOpened` 里先等分析完成再开始播放（代价是切歌卡顿）。
- **变速播放**：曲线按时间索引，位置取的是 BASS 通道位置，速度不为 1 时会有
  极小的对齐偏差（不影响听感）。
- 目前没有做 UI 开关（加复选框需要改 `.rc` 对话框模板）。`LoudnessBalanceBass.h`
  已经导出了 `SetEnabled()` / `IsEnabled()` / `GetStatusText()`，接一个菜单项或
  设置项只需要几行代码。
- 想进一步提升安静素材：在 `GainApplier` 后面加一级 look-ahead 峰值限幅器，
  就可以在保住 ceiling 的前提下突破"峰值顶住提升量"的限制。
- 想做跨会话缓存：把 `GainCurve` 按 `路径 + 大小 + 修改时间` 序列化到磁盘即可
  （当前是会话内内存缓存）。

---

## 10. 运行与部署

`E:\loundless\dist\` 是可直接运行的目录（便携模式，配置写在自己目录下的 `config.ini`）：

```
dist\
├─ MusicPlayer2.exe      ← 本次构建（含响度平衡）
├─ config.ini            ← [loudness] 段已启用，ffmpeg_path 已指向本机 ffmpeg
├─ global_cfg.ini        ← portable_mode=1
├─ bass.dll  bass_fx.dll  tag.dll
└─ Plugins\bassflac.dll  ← 必须是 x64 版，否则 FLAC 无法解码（表现为能载入但不能播放）
```

运行方式：直接双击 `dist\MusicPlayer2.exe`。播放任意曲目后，`dist\error.log` 里会出现

```
[响度平衡] 已就绪：I = -9.2 LUFS, 增益 -8.8 ~ -8.8 dB, 目标 -18.0 LUFS
```

说明分析与增益已生效。改 `config.ini` 里的 `target_lufs` / `dynamic` 后重开程序即可切换风格。

**关于偶发崩溃**：如果在某个终端/沙箱环境下启动，进程可能被注入第方 DLL
（例如 `tsbx.dll`），此时会出现与本模块无关的偶发 `0xC0000374`（堆损坏）/
`0xC0000005`。用同一套测试对比"不含本模块的构建"和"含本模块的构建"，
崩溃率基本相同（14/15 对 12/15），所以这类崩溃来自环境注入而非响度代码；
直接双击运行（不经过任何 hook 环境）即可避开。
