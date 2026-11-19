# MusicPlayer2 构建与 UI 配置工作流

> 工作根目录：`E:\loundless`
> 适用版本：GaussBlur 崩溃修复 + 实时响度平衡 + 预取 + 就绪前暂停 + DPI / 主题 UI 对齐
> 目标：在一台没有 MSBuild 权限的机器上，从源码稳定产出可运行的 x64 程序，并把界面调到与官方预编译版一致。

---

## 0. 全流程一览

| 步骤 | 操作 | 产物 | 校验点 |
|---|---|---|---|
| 1 环境 | 确认 MSVC / SDK / Python / ffmpeg 就位 | — | 四个路径都存在 |
| 2 编译 | `python build_manual.py all --out=build_ui` | `build_ui\obj\*.obj` | 失败数 = 0 |
| 3 资源 | 脚本内自动执行 rc（主资源 + manifest） | `.res` 两个 | rc = 0 |
| 4 链接 | 脚本内自动执行 link | `MusicPlayer2.exe`（约 9.7 MB） | 退出码 = 0 |
| 5 部署 | 拷贝 exe 与运行库到 `dist\` | 可运行目录 | 双击能起、DPI 正常 |
| 6 UI 配置 | 改 `dist\config.ini` + 资源目录 | 界面外观 | 尺寸 1770×1214、主题 #86BAF9 |
| 7 响度配置 | 改 `dist\config.ini` 的 `[loudness]` | 响度行为 | 日志出现 `[响度平衡]` 且 LUFS 收敛 |
| 8 验收 | 按第 8 节清单逐项过 | — | 全部通过 |

一句话版：

```bash
cd /e/loundless
python build_manual.py all --out=build_ui   # 编译 + 资源 + 链接
cp build_ui/MusicPlayer2.exe dist/          # 替换可执行文件
# 然后改 dist/config.ini 完成 UI 与响度配置
```

---

## 1. 环境前提

| 组件 | 路径 | 说明 |
|---|---|---|
| MSVC 编译器 | `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207` | 必须含 atlmfc（MFC） |
| Windows SDK | `C:\Program Files (x86)\Windows Kits\10`，版本 `10.0.26100.0` | 提供 rc.exe 与 um/ucrt 库 |
| Python | 3.x，`python` 可直接调用 | 只跑构建脚本，无需第三方包 |
| ffmpeg | `D:\App\ffmpeg-2026-02-04-git-627da1111c-full_build\bin\ffmpeg.exe` | 响度分析用，需支持 `ebur128` 滤镜 |
| 源码工程 | `E:\loundless\MusicPlayer2\MusicPlayer2\MusicPlayer2.vcxproj` | 脚本靠解析它拿源文件清单 |
| 预编译库 | 工程目录下 `bass_x64.lib` `tag_x64.lib`（x64）/ `bass.lib` `tag.lib`（x86） | 已随源码就位 |

上表中的 MSVC / SDK 版本号在 `build_manual.py` 顶部是硬编码常量，换机器要同步改：

```python
MSVC = r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207"
SDK  = r"C:\Program Files (x86)\Windows Kits\10"
SDKV = "10.0.26100.0"
PROJ = r"E:\loundless\MusicPlayer2\MusicPlayer2"
```

---

## 2. 为什么不用 MSBuild

`msbuild.exe` 在当前机器被安全策略硬拦截（调用即失败），所以不能用 `msbuild MusicPlayer2.sln /p:Configuration=Release`。

`build_manual.py` 的做法是：绕过 MSBuild，直接按 `Release|x64` 的实际参数调用 `cl.exe / rc.exe / link.exe`。好处是：

- 每个源文件一条独立命令，可以 4~12 线程并行；
- 参数显式可读，出问题能单文件复现；
- 支持只重编、或复用 obj 只重链，改一行代码不用等全量。

---

## 3. 构建

### 3.1 命令

```bash
cd /e/loundless

python build_manual.py compile                  # 只编译 x64，输出 build_out\obj
python build_manual.py all                      # x64 全流程（默认 build_out）
python build_manual.py all --out=build_ui       # 指定输出目录，避免覆盖旧产物
python build_manual.py all --x86                # x86 全流程，输出 build_out_x86
python build_manual.py link --x86               # 复用已有 obj，只做 rc + link
```

`--out=名字` 会在 `E:\loundless\` 下建同名目录。建议每次大改都换名字（`build_p5`、`build_ui`），旧产物留着对比。

### 3.2 脚本内部的四段

**① 枚举源文件**
解析 `MusicPlayer2.vcxproj` 里所有 `<ClCompile Include="...">`，读取子节点 `<ExcludedFromBuild Condition="...Release|x64...">true</ExcludedFromBuild>` 并跳过。这样自动与工程保持同步，不会漏文件。当前规模：带响度模块 241 个文件，原始工程 239 个。

**② 编译（每个文件一条 cl 命令）**

```
cl /nologo /c /W3 /O2 /Oi /Gy /MD /EHsc /std:c++latest /Y-
   /D_AFXDLL /D_WINDOWS /DNDEBUG /DUNICODE /D_UNICODE
   /I <PROJ> /Fo:<OBJ>\xxx.obj <源文件>
```

- `/Y-` 关掉预编译头，避免依赖 `pch`；
- `/MD` + `/D_AFXDLL` 对应 Release 的 MFC 动态链接；
- x86 额外加 `/arch:SSE2`；
- **编码**：工程没设 `/utf-8`，MSBuild 走系统代码页（GBK）。脚本用 `is_utf8()` 探测每个源文件，是 UTF-8（含 BOM）才追加 `/utf-8`，否则按 GBK 编译。这一条是编译能否全过的关键。

**③ 资源（rc 两次）**

- 主资源：`rc MusicPlayer2.rc`。工程原本由 PreBuildEvent 的 `print_compile_time.bat` 生成 `compile_time.txt`，脚本自己往 obj 目录写一份代替它，否则 rc 会报找不到文件。
- 清单资源：`rc manifest.rc`，把 `app.manifest` 以 `RT_MANIFEST(24)` 嵌进 exe，见第 5 节。

**④ 链接**

生成 `link.rsp` 再 `link @link.rsp`，关键点：

```
/SUBSYSTEM:WINDOWS /MACHINE:X64
/ENTRY:wWinMainCRTStartup      ← MFC 程序入口，缺了会报 unresolved
/INCLUDE:__afxForceSTDAFX      ← 强制拉入 MFC 的 stdafx 目标
/OPT:REF /OPT:ICF /DEBUG /MAP  ← 保留 MAP，崩溃定位要用
/MANIFEST:NO                   ← 关掉自动清单，改用 manifest.res
/DELAYLOAD:api-ms-win-core-winrt-*.dll /DELAYLOAD:powrprof.dll
```

对象文件必须**排在库前面**，否则库里的符号不会被拉入。库表：x64 是 `bass_x64.lib tag_x64.lib Powrprof.lib delayimp.lib`，x86 是 `bass.lib tag.lib Powrprof.lib delayimp.lib`。

### 3.3 产物清单

```
build_ui\
  MusicPlayer2.exe      ← 交付物，约 9.7 MB
  MusicPlayer2.pdb      ← 调试符号
  MusicPlayer2.map      ← 地址表，用于把崩溃地址换算成函数名
  MusicPlayer2.res      ← 主资源
  manifest.res          ← DPI / 控件清单
  link.rsp              ← 可复现的链接参数
  obj\*.obj             ← 241 个目标文件
```

看到 `编译完成：241 个，失败 0 个` 和 `产物：... (9.7 MB)` 才算成功；有 FAIL 时脚本会打印前 5 条错误并直接终止，不会产出半成品 exe。

---

## 4. 部署

### 4.1 全新部署（从零搭一个可运行目录）

```bash
mkdir dist
cp build_ui/MusicPlayer2.exe dist/
cp MusicPlayer2/MusicPlayer2/*.dll dist/        # bass.dll / bass_fx.dll / tag.dll（x64 版！）
```

再补齐这些**非 exe 资源**，否则界面缺字、缺皮肤、后台图缺失：

| 目录 / 文件 | 来源 | 缺失后果 |
|---|---|---|
| `Plugins\` | 官方版 `E:\CDs\MusicPlayer2` | 部分解码器不可用 |
| `language\*.ini` | 官方版（3 个：简体 / 繁体 / 英文） | 菜单空白或英文 |
| `skins\*.xml` + `miniMode\` + `panels\` | 官方版（10 套皮肤 + `skin.xsd`） | 皮肤列表为空，界面退回默认 |
| `default_background.jpg` | 官方版 | `enable_background` 打开时背景全黑 |

**位数必须一致**：x64 exe 配 x64 的 `bass.dll` / `tag.dll`；x86 exe 配 x86 版。混用会直接 0xc000007b 启动失败。

### 4.2 增量更新（日常最常用）

只替换 exe 即可，配置、缓存、歌单都不动：

```bash
cp build_ui/MusicPlayer2.exe dist/MusicPlayer2.exe
```

### 4.3 便携模式

`dist\global_cfg.ini`：

```ini
[config]
portable_mode= true
```

便携模式下所有数据写在程序目录（`config.ini`、`playlist\`、`song_data.dat`、`recent_list.dat`），不写注册表和 `%APPDATA%`。换机器整个目录拷走即可。调试和回滚都靠这个。

---

## 5. Manifest 与 DPI

### 5.1 为什么需要

原始工程产出的 exe 没有清单。在 150% 缩放的显示器上，进程不感知 DPI，窗口被 DWM 整体放大 → 界面发虚、控件边缘模糊、字体糊成一团。加清单后进程按物理像素绘制，画面锐利。

同时清单里声明了 Common-Controls v6，否则部分控件是 Win95 风格外观。

### 5.2 文件

`E:\loundless\app.manifest`：

```xml
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <dependency>
    <dependentAssembly>
      <assemblyIdentity type="win32" name="Microsoft.Windows.Common-Controls"
        version="6.0.0.0" processorArchitecture="amd64"
        publicKeyToken="6595b64144ccf1df" language="*"/>
    </dependentAssembly>
  </dependency>
  <trustInfo xmlns="urn:schemas-microsoft-com:asm.v3">
    <security><requestedPrivileges>
      <requestedExecutionLevel level="asInvoker" uiAccess="false"/>
    </requestedPrivileges></security>
  </trustInfo>
  <application xmlns="urn:schemas-microsoft-com:asm.v3">
    <windowsSettings>
      <dpiAware xmlns="http://schemas.microsoft.com/SMI/2005/WindowsSettings">true</dpiAware>
    </windowsSettings>
  </application>
</assembly>
```

`E:\loundless\manifest.rc`：

```
1 24 "E:\\loundless\\app.manifest"
```

- `1` = 资源 ID，`24` = `RT_MANIFEST`。
- **路径是绝对路径且双反斜杠**：移动工程目录时必须同步改这一行，否则 rc 报错。
- x86 构建要把 `processorArchitecture` 从 `amd64` 改成 `x86`，否则清单与二进制不匹配、系统忽略它。

### 5.3 为什么不用 mt.exe

常规做法是 link 之后 `mt.exe -manifest app.manifest -outputresource:xxx.exe;1`，但本机 `mt.exe` 调用被拦截（报 LNK1158 / 无法启动）。所以改成**资源方式**：rc 编译出 `manifest.res`，链接时作为普通 .res 追加，并加 `/MANIFEST:NO` 阻止链接器自己再生成一份（两份清单会导致 exe 无法启动）。

对应脚本函数：`do_manifest_rc()` + `do_link()` 里的 `manifest.res` 追加逻辑。

### 5.4 验证

先让程序跑起来，再用下面这段脚本量窗口物理尺寸（脚本自己先设为 DPI 感知，否则量到的是被缩放后的逻辑值）：

```bash
python -c "
import ctypes, ctypes.wintypes as w
ctypes.windll.shcore.SetProcessDpiAwareness(2)
u = ctypes.windll.user32
h = u.FindWindowW(None, 'MusicPlayer2')
r = w.RECT(); u.GetWindowRect(h, ctypes.byref(r))
print('窗口物理尺寸:', r.right - r.left, 'x', r.bottom - r.top)
"
```

实测：窗口物理尺寸从 1320×964 变为 **1770×1214**，文字锐利即为生效。若还是 1320 宽，说明 manifest 没嵌进去，检查 rc 是否成功、`/MANIFEST:NO` 是否加上。

---

## 6. UI 配置

### 6.1 配置文件在哪

| 文件 | 作用 |
|---|---|
| `dist\config.ini` | 主配置，含 `[config]` 界面段与 `[loudness]` 响度段 |
| `dist\global_cfg.ini` | 只有 `portable_mode` 一项 |
| `dist\user_ui.dat` | 皮肤 / 布局选择记忆（二进制，别手改） |
| `dist\recent_list.dat` | 上次播放列表与位置 |
| `dist\song_data.dat` | 曲目元数据缓存 |
| `dist\playlist\` | 用户歌单 |

`config.ini` 是 **UTF-8 with BOM**。BOM 不影响程序读取（`GetPrivateProfileString` 正常），但用脚本 grep `^\[loudness\]` 会匹配不到，别误判成"配置没写进去"。

### 6.2 界面关键项

对齐官方预编译版时，这几项起决定作用：

```ini
[config]
window_width = 1770          ; 物理像素。官方版在 150% 缩放下就是 1770，缩小一圈通常是 manifest 没生效
window_hight = 1214          ; 注意拼写是 hight，不是 height
theme_color = 16366214       ; COLORREF 0x00BBGGRR → #86BAF9 浅蓝
theme_color_follow_system = false
dark_mode = true             ; 深色主题
background_transparency = 40 ; 背景透明度，深色模式默认 40
transparency = 100           ; 窗口整体不透明度
font = Microsoft YaHei UI
font_size = 11
show_playlist = true
playlist_width_percent = 50
show_menu_bar = false        ; 隐藏菜单栏，界面更像官方预编译版
show_window_frame = true
button_round_corners = true
enable_background = true
album_cover_as_background = true
background_gauss_blur = true
gauss_blur_radius = 150
lyric_align = 2
show_spectrum = true
spectrum_height = 80
show_fps = true
UI_selected = 1              ; 当前皮肤索引，建议在程序内切换而不是手改
```

主题色换算（十进制 ↔ 十六进制）：

```bash
python -c "c=16366214; print('#%02X%02X%02X' % (c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF))"
# 输出 #86BAF9
```

反过来自定义颜色时：`r | (g<<8) | (b<<16)` 得到十进制写回 `theme_color`。

### 6.3 皮肤与语言

- 皮肤：`dist\skins\*.xml`，当前 10 套（`00_uiTest` / `01_simple` / `02_grooveMusic` / `03_grooveMusicWin11` / `test` / `ui3`~`ui5` / `ui11` / `ui12` 等），另有 `miniMode\` 迷你模式与 `panels\` 面板布局。**在程序内切换**，选择会写进 `user_ui.dat` 与 `UI_selected`。
- 语言：`dist\language\`（`Simplified_Chinese.ini` / `Traditional_Chinese.ini` / `English.ini`）。`[general] language_ =` 留空表示跟随系统。

### 6.4 配置生效时机

**改 `config.ini` 必须重启程序**。程序启动时读取并缓存，运行中不会重新加载 `[loudness]`（有 `g_loaded` 标志保证只读一次）。界面项退出时会被程序回写覆盖，所以：

> 改界面配置前先**完全退出**程序，改完再启动；运行中手改的值退出时会被内存里的旧值覆盖。

### 6.5 已知差异（官方版 vs 本项目）

- 官方版右侧默认停在"文件夹"标签页；本项目 `playlist_mode` 在源码里被注释停用，改由 `recent_list.dat` 恢复上次状态，**无法用配置强制**。属于行为差异，不是配置缺失。
- 官方版是 x86 构建，本项目默认 x64。视觉一致，性能与内存表现略有差别。

---

## 7. 响度平衡配置

### 7.1 参数表（`[loudness]` 段）

| 参数 | 默认 | 含义 |
|---|---|---|
| `enable` | true | 模块总开关。false 时完全不介入播放 |
| `dynamic` | false | false = 整曲平衡（固定增益，保留曲内动态）；true = 逐段平衡（增益随时间变化） |
| `target_lufs` | -18.0 | 目标响度（LUFS），所有曲目向它收敛。有效范围 -40 ~ -5 |
| `max_boost_db` | 12.0 | 最大提升量，0~30。防止把极安静的曲子推爆 |
| `max_cut_db` | 12.0 | 最大衰减量，0~30 |
| `ceiling_dbtp` | -1.0 | 峰值上限（dBTP）。增益后 true peak 不得超过它，≤ 0 |
| `attack_ms` | 200.0 | 增益**变大**时的时间常数（仅 dynamic=true 生效），最小 10 |
| `release_ms` | 800.0 | 增益**变小**时的时间常数（仅 dynamic=true 生效），最小 10 |
| `prefetch` | true | 播放当前曲目时提前分析下一曲，切歌零等待 |
| `pause_until_ready` | true | 点了播放但曲线还没算好时先不播，就绪后自动继续。**只对“要求播放”生效**：开机只打开文件、不点播放时不会被它拉起 |
| `ffmpeg_path` | 绝对路径 | ffmpeg.exe 位置；留空则自动查找 PATH |

超范围的值会被钳到合法区间，不会崩溃，但行为会和预期不符。首次运行若 `[loudness]` 段不存在，程序会把默认值写进去，方便发现。

### 7.2 两种模式怎么选

| | `dynamic = false`（默认，推荐） | `dynamic = true` |
|---|---|---|
| 增益 | 整曲一个固定值 = `target_lufs − I` | 每 100 ms 一个值，随短时响度 S 变化 |
| 效果 | 曲目之间响度一致，曲内起伏完整保留 | 曲内段落差异也被压平，像慢速压缩器 |
| 代价 | 无 | 密集段落被拉低，古典 / 动态大的录音会"平" |
| 适用 | 日常听歌、专辑连播 | 播客、有声书、背景音乐、素材响度参差 |

固定增益还会被 `ceiling_dbtp − true_peak` 二次约束，所以高峰值曲目不会被推到削波。

### 7.3 时间常数是什么

把增益变化当成 RC 低通滤波，每帧 100 ms 更新一次：

```
a = 1 - exp(-0.1 * 1000 / 时间常数_ms)
新增益 = 旧增益 + a * (目标增益 - 旧增益)
```

`attack_ms = 200` → a ≈ 0.393，63% 的差距在 0.2 s 内走完；`release_ms = 800` → a ≈ 0.118，63% 在 0.8 s 内走完。

- attack 小 → 遇到突然变响的段落压得快，但可能"抽气"；
- release 大 → 变安静后恢复慢，听感更稳，但长时间偏小；
- 另外有全局速率上限 `max_rate_db_per_sec = 24`（源码常量，不在 ini 里），拖动进度条时临时放宽到 800 dB/s，避免跳变爆音。

### 7.4 三套推荐配置

```ini
; A. 日常听歌：曲间平衡，保留动态，切歌不等待
[loudness]
enable = true
dynamic = false
target_lufs = -16.0
prefetch = true
pause_until_ready = true
```

```ini
; B. 播客 / 有声书：压平段落差异
[loudness]
enable = true
dynamic = true
target_lufs = -16.0
attack_ms = 100
release_ms = 1500
max_boost_db = 15
```

```ini
; C. 临时关闭：保留原声，只留程序其他功能
[loudness]
enable = false
```

### 7.5 日志确认

日志写在 `dist\error.log`（`[other] debug_log = 2` 时开启）。点播放且曲线未就绪时的完整序列：

```
[响度平衡] 正在分析响度，播放已暂停，就绪后自动开始
[响度平衡] 已就绪：I = -9.2 LUFS, 增益 -8.8 ~ -8.8 dB, 目标 -18.0 LUFS
[响度平衡] 分析完成，继续播放
[响度平衡] 已预取下一曲：xxx.flac（切歌时直接生效）
[响度平衡] 已就绪（命中缓存，无需分析）：I = ...
```

看到 `未找到 ffmpeg.exe，响度平衡功能不可用` 就是 `ffmpeg_path` 不对。

### 7.6 开机只打开文件不播放（重要语义）

`pause_until_ready` 曾经在“打开文件”时就登记“等待恢复”，于是程序启动 → OPEN 上次的曲目 → 分析完成 → 模块自己把音频放出来。表现出来就是：

- 启动就出声，`auto_play_when_start = false` 也关不掉；
- 声音在放，界面左上角却写着“已停止 / 暂停”，因为播放器状态机没收到 PLAY。

现在“等待”只在 `Command::PLAY` / `PLAY_PAUSE` 调用的 `HoldPlaybackUntilReady()` 里登记，OPEN 只挂 DSP、启动后台分析。判断口诀：**打开 ≠ 播放**。

因此正确的日志预期是：

| 场景 | 日志 |
|---|---|
| 启动程序，不动它 | 只有 `已就绪：I = ...`，没有“播放已暂停” |
| 启动后点播放（曲线未好） | `正在分析响度，播放已暂停` → `已就绪` → `分析完成，继续播放` |
| 启动后点播放（曲线已好） | 无响度日志，直接播 |
| 播完切下一曲 | `已就绪（命中缓存，无需分析）`（预取生效时） |

---

## 8. 验收清单

| 项 | 方法 | 通过标准 |
|---|---|---|
| 编译 | 看脚本输出 | 241 个文件，失败 0 个 |
| 链接 | 看 exe 大小 | 约 9.7 MB，退出码 0 |
| 启动 | 连续启停 6 次 | 每次都能起来，无 0xC0000374 / 0xC0000409 |
| 带歌单启动 | `playlist\` 有内容时启动 | 正常恢复，不崩 |
| DPI | 看窗口宽度 | 1770 物理像素，文字锐利 |
| 主题 | 采样进度条 / 强调色 | `#B0D0F8` 附近，与官方版一致 |
| 中文 | 打开菜单 | 全中文，无问号方块 |
| 皮肤 | 皮肤列表 | 10 套可选，切换即时生效 |
| 响度收敛 | 播 4 首不同响度的曲子，看日志 I 值 | 全部落到目标 ±0.5 LUFS，极差 ≈ 0.00 dB |
| 源文件 | 播前后比对 SHA-256 | 完全一致（模块只读音频） |
| 预取 | 连续切 5 次 | 日志出现"已预取下一曲"+"命中缓存" |
| 启动不自动播 | `auto_play_when_start=false`，上次非播放中退出，启动后不动 | 左上角“已停止”，无声音，日志无“播放已暂停” |
| 就绪前暂停 | 启动后立即点播放 | 先等 0.3~0.6 s 再出声，日志三行齐全，界面显示“正在播放” |
| 状态一致 | 点播放 / 点暂停 | 界面状态、按钮图标与实际声音三者一致 |
| 退出 | 关闭主窗口 | 1~2 s 内退出，无残留进程 |

自动化测试时记得把 `[other] show_close_main_window_inquery = false`，否则每次关窗弹确认框，脚本会卡住。

---

## 9. 故障排查

| 现象 | 原因 | 处理 |
|---|---|---|
| 第二次启动必崩，删 `playlist\` 就好 | `CGaussBlur` 构造函数未初始化 `m_pTempl`，取到栈上垃圾指针后 `realloc` → 堆损坏 0xC0000374 | 已在 `GaussBlur.cpp` 构造函数里补 `m_r=-1; m_sigma=-1.0; m_pTempl=NULL;`。删 playlist 只是绕过触发路径，不是修复 |
| 崩溃码 0xC0000409，`Rcx=7` | 同线程递归加 `std::mutex` → `std::system_error` → `terminate` | 检查回调是否在持锁时触发，把锁收窄到块作用域 |
| 启动 0xc000007b | 32/64 位动态库混用 | x64 exe 必须配 x64 的 bass.dll / tag.dll |
| link 报 LNK1158 / 无法运行 mt.exe | 本机拦截 mt.exe | 用 `manifest.rc` + `/MANIFEST:NO` 走资源嵌入 |
| rc 报找不到 compile_time.txt | 工程靠 PreBuildEvent 生成 | `do_rc()` 里已自己写一份到 obj 目录 |
| 个别源文件编译报"常量中有换行符" | 文件是 GBK，却按 UTF-8 编 | 依赖 `is_utf8()` 探测，不要给所有文件强加 `/utf-8` |
| link 报 unresolved external symbol | 库排在 obj 前面，或漏 `__afxForceSTDAFX` | 调顺序；保留 `/INCLUDE:__afxForceSTDAFX` |
| 界面小一圈、发虚 | manifest 没生效 | 确认 `manifest.res` 参与了链接且加了 `/MANIFEST:NO` |
| 改了 config.ini 没反应 | 程序启动才读，且退出时会回写 | 完全退出 → 改 → 再启动 |
| 预取不触发 | `repeat_mode` 为顺序模式且正播最后一首，没有"下一曲" | 换列表或开循环，属正常行为 |
| 响度完全不动作 | `enable=false` / ffmpeg 路径错 / 该格式不可分析 | 看日志三行判断 |
| 启动就自动播放，`auto_play_when_start` 关不掉，界面还显示“暂停” | 曾在 `OnTrackOpened` 里登记“等待恢复”，把“打开文件”当成“要求播放”，分析完成后模块自己 `BASS_ChannelPlay` | 等待只在 `HoldPlaybackUntilReady()`（PLAY / PLAY_PAUSE）里登记，OPEN 只挂 DSP。见 7.6 |
| 点播放没声音，但界面显示“正在播放” | 曲线就绪前被拦截，等待回调恢复；若日志一直没有“分析完成，继续播放”，说明等待路径不匹配（切歌后旧回调） | 看日志确认 `已就绪` 是否紧跟 `分析完成`；`CancelPendingResume` 会清等待 |
| DPI 异常想回退 | — | 临时删掉 `dist\MusicPlayer2.exe` 的 manifest 资源需重编，建议直接换回旧 exe |

---

## 10. 版本与回滚

当前保留两个可运行目录：

| 目录 | 内容 | 用途 |
|---|---|---|
| `dist\` | 崩溃修复 + 响度平衡 + 预取 + 就绪前暂停 + UI 对齐 | 主用版本 |
| `dist_clean\` | **只有** GaussBlur 崩溃修复，无响度模块，无 manifest | 对照 / 排障基线 |

`dist_clean` 是旧 exe（无 manifest，界面比 `dist` 小一圈）。需要一份带 manifest 的纯净版时：

```bash
# 临时把 LoudnessBalance 相关源文件与调用点排除，再全量构建
python build_manual.py all --out=build_clean
cp build_clean/MusicPlayer2.exe dist_clean/
```

`dist\MusicPlayer2.exe.bak_pre_fix` 是修“启动自动播放”之前的上一版，出问题可直接改回来：

```bash
cp dist/MusicPlayer2.exe.bak_pre_fix dist/MusicPlayer2.exe
```

回归对比的意义：`dist_clean` 6/6 次启动存活，证明崩溃修复独立于响度模块；若 `dist` 出问题而 `dist_clean` 正常，问题一定在响度 / 预取 / 暂停这条链上。

---

## 附：常用路径速查

```
源码            E:\loundless\MusicPlayer2\MusicPlayer2\
响度模块        ...\MusicPlayer2\LoudnessBalance\   (LoudnessBalance.h/.cpp 核心, LoudnessBalanceBass.h/.cpp 适配层)
崩溃修复点      ...\MusicPlayer2\GaussBlur.cpp       构造函数
播放拦截点      ...\MusicPlayer2\Player.cpp          Command::PLAY / PAUSE / STOP
构建脚本        E:\loundless\build_manual.py
清单            E:\loundless\app.manifest + manifest.rc
可运行目录      E:\loundless\dist\
配置            E:\loundless\dist\config.ini
日志            E:\loundless\dist\error.log
官方参考版      E:\CDs\MusicPlayer2
测试音频        E:\loundless\test\
```
