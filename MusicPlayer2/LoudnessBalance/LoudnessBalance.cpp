// LoudnessBalance.cpp —— 实时响度平衡模块核心实现
// 依赖：Win32（CreateProcess 调用 ffmpeg）、标准库。不依赖 MFC / BASS。

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "LoudnessBalance.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace loudness
{
    namespace
    {
        // ---------------------------------------------------------------
        // 小工具
        // ---------------------------------------------------------------
        inline float ClampF(float v, float lo, float hi)
        {
            return v < lo ? lo : (v > hi ? hi : v);
        }

        inline float DbToLin(double db)
        {
            return static_cast<float>(std::pow(10.0, db / 20.0));
        }

        bool FileExistsW(const std::wstring& path)
        {
            if (path.empty()) return false;
            DWORD attr = GetFileAttributesW(path.c_str());
            return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
        }

        std::wstring GetModuleDir()
        {
            wchar_t buf[MAX_PATH * 2] = { 0 };
            DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
            std::wstring path(buf, len);
            size_t pos = path.find_last_of(L"\\/");
            if (pos == std::wstring::npos) return std::wstring();
            return path.substr(0, pos + 1);
        }

        // 按 Windows 命令行规则给参数加引号
        std::wstring QuoteArg(const std::wstring& arg)
        {
            if (arg.empty()) return L"\"\"";
            if (arg.find_first_of(L" \t\n\v\"") == std::wstring::npos)
                return arg;     // 无需引号

            std::wstring out;
            out.push_back(L'"');
            for (auto it = arg.begin();; ++it)
            {
                size_t backslashes = 0;
                while (it != arg.end() && *it == L'\\')
                {
                    ++it;
                    ++backslashes;
                }
                if (it == arg.end())
                {
                    out.append(backslashes * 2, L'\\');
                    break;
                }
                if (*it == L'"')
                {
                    out.append(backslashes * 2 + 1, L'\\');
                    out.push_back(L'"');
                }
                else
                {
                    out.append(backslashes, L'\\');
                    out.push_back(*it);
                }
            }
            out.push_back(L'"');
            return out;
        }

        // 手工解析十进制浮点数：避免 MFC 修改 C 运行库 locale 后 strtof 解析失败
        bool ParseFloatValue(const char* p, float& out)
        {
            if (p == nullptr) return false;
            while (*p == ' ' || *p == '\t') ++p;
            bool neg = false;
            if (*p == '-') { neg = true; ++p; }
            else if (*p == '+') { ++p; }

            double v = 0.0;
            int digits = 0;
            while (*p >= '0' && *p <= '9')
            {
                v = v * 10.0 + (*p - '0');
                ++p;
                ++digits;
            }
            if (*p == '.')
            {
                ++p;
                double scale = 0.1;
                while (*p >= '0' && *p <= '9')
                {
                    v += (*p - '0') * scale;
                    scale *= 0.1;
                    ++p;
                    ++digits;
                }
            }
            if (digits == 0) return false;
            if (*p == 'e' || *p == 'E')
            {
                ++p;
                bool eneg = false;
                if (*p == '-') { eneg = true; ++p; }
                else if (*p == '+') { ++p; }
                int e = 0;
                while (*p >= '0' && *p <= '9') { e = e * 10 + (*p - '0'); ++p; }
                v *= std::pow(10.0, eneg ? -e : e);
            }
            out = static_cast<float>(neg ? -v : v);
            return true;
        }

        // ---------------------------------------------------------------
        // 一次性子进程（读取 stdout 管道，stderr 落到临时文件）
        // ---------------------------------------------------------------
        class ChildProcess
        {
        public:
            ~ChildProcess() { Close(); }

            bool Start(const std::wstring& cmd_line, uint32_t timeout_ms, std::wstring& error)
            {
                error.clear();
                SECURITY_ATTRIBUTES sa{};
                sa.nLength = sizeof(sa);
                sa.bInheritHandle = TRUE;

                HANDLE read_pipe = nullptr;
                HANDLE write_pipe = nullptr;
                if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0))
                {
                    error = L"CreatePipe 失败";
                    return false;
                }
                SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

                // stderr -> 临时文件（ffmpeg 的错误信息可用于诊断）
                wchar_t temp_dir[MAX_PATH] = { 0 };
                GetTempPathW(MAX_PATH, temp_dir);
                wchar_t temp_file[MAX_PATH] = { 0 };
                if (GetTempFileNameW(temp_dir, L"lbm", 0, temp_file) != 0)
                    m_err_file = temp_file;
                HANDLE err_handle = INVALID_HANDLE_VALUE;
                if (!m_err_file.empty())
                {
                    err_handle = CreateFileW(m_err_file.c_str(), GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_TEMPORARY, nullptr);
                }
                if (err_handle == INVALID_HANDLE_VALUE)
                {
                    if (!m_err_file.empty()) DeleteFileW(m_err_file.c_str());
                    m_err_file.clear();
                    err_handle = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ,
                        &sa, OPEN_EXISTING, 0, nullptr);
                }

                // stdin -> NUL（配合 -nostdin，避免子进程等待输入）
                HANDLE nul_in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ,
                    &sa, OPEN_EXISTING, 0, nullptr);

                STARTUPINFOW si{};
                si.cb = sizeof(si);
                si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
                si.wShowWindow = SW_HIDE;
                si.hStdOutput = write_pipe;
                si.hStdError = err_handle;
                si.hStdInput = nul_in;

                std::wstring cmd = cmd_line;    // CreateProcess 需要可写缓冲区
                std::vector<wchar_t> cmd_buf(cmd.begin(), cmd.end());
                cmd_buf.push_back(L'\0');

                PROCESS_INFORMATION pi{};
                BOOL ok = CreateProcessW(nullptr, cmd_buf.data(), nullptr, nullptr, TRUE,
                    CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);

                CloseHandle(write_pipe);
                if (nul_in != INVALID_HANDLE_VALUE) CloseHandle(nul_in);
                if (err_handle != INVALID_HANDLE_VALUE && err_handle != write_pipe) CloseHandle(err_handle);

                if (!ok)
                {
                    CloseHandle(read_pipe);
                    error = L"无法启动 ffmpeg 进程（错误码 " + std::to_wstring(GetLastError()) + L"）";
                    return false;
                }

                CloseHandle(pi.hThread);
                pi.hThread = nullptr;       // 已关闭，置空避免 Close() 二次关闭同一句柄
                m_pi = pi;
                m_read = read_pipe;
                m_deadline = GetTickCount64() + timeout_ms;
                return true;
            }

            // 读取当前可用的数据追加到 buffer。返回 false 表示结束（原因见 reason）。
            enum class PumpResult { Data, Eof, Timeout, Cancelled };

            PumpResult Pump(std::string& buffer, const std::atomic<bool>* cancel)
            {
                if (m_read == nullptr) return PumpResult::Eof;
                for (;;)
                {
                    if (cancel != nullptr && cancel->load()) return PumpResult::Cancelled;
                    if (GetTickCount64() > m_deadline) return PumpResult::Timeout;

                    DWORD avail = 0;
                    if (!PeekNamedPipe(m_read, nullptr, 0, nullptr, &avail, nullptr))
                        return PumpResult::Eof;

                    if (avail == 0)
                    {
                        // 进程已退出且管道中无数据 => 结束
                        DWORD code = 0;
                        if (GetExitCodeProcess(m_pi.hProcess, &code) && code != STILL_ACTIVE)
                        {
                            if (!PeekNamedPipe(m_read, nullptr, 0, nullptr, &avail, nullptr) || avail == 0)
                                return PumpResult::Eof;
                        }
                        else
                        {
                            ::Sleep(1);
                            continue;
                        }
                    }

                    char tmp[8192];
                    DWORD want = avail > sizeof(tmp) ? static_cast<DWORD>(sizeof(tmp)) : avail;
                    DWORD got = 0;
                    if (!ReadFile(m_read, tmp, want, &got, nullptr) || got == 0)
                        return PumpResult::Eof;
                    buffer.append(tmp, got);
                    return PumpResult::Data;
                }
            }

            // 等待进程退出（毫秒），失败时强制结束
            void Finish(uint32_t wait_ms)
            {
                if (m_pi.hProcess == nullptr) return;
                if (WaitForSingleObject(m_pi.hProcess, wait_ms) == WAIT_TIMEOUT)
                {
                    TerminateProcess(m_pi.hProcess, 1);
                    WaitForSingleObject(m_pi.hProcess, 1000);
                }
                DWORD code = 0;
                if (GetExitCodeProcess(m_pi.hProcess, &code))
                    m_exit_code = static_cast<int>(code);
            }

            void Terminate()
            {
                if (m_pi.hProcess != nullptr)
                {
                    TerminateProcess(m_pi.hProcess, 1);
                    WaitForSingleObject(m_pi.hProcess, 1000);
                }
            }

            void Close()
            {
                if (m_read != nullptr) { CloseHandle(m_read); m_read = nullptr; }
                if (m_pi.hProcess != nullptr) { CloseHandle(m_pi.hProcess); m_pi.hProcess = nullptr; }
                if (m_pi.hThread != nullptr) { CloseHandle(m_pi.hThread); m_pi.hThread = nullptr; }
                if (!m_err_file.empty()) { DeleteFileW(m_err_file.c_str()); m_err_file.clear(); }
            }

            int GetExitCode() const { return m_exit_code; }

            // 读取子进程 stderr 文本（关闭前调用）
            std::string ReadErrorText() const
            {
                if (m_err_file.empty()) return std::string();
                HANDLE h = CreateFileW(m_err_file.c_str(), GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
                if (h == INVALID_HANDLE_VALUE) return std::string();
                std::string text;
                char buf[1024];
                DWORD got = 0;
                while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got > 0)
                    text.append(buf, got);
                CloseHandle(h);
                return text;
            }

        private:
            PROCESS_INFORMATION m_pi{};
            HANDLE m_read = nullptr;
            int m_exit_code = 0;
            ULONGLONG m_deadline = 0;
            std::wstring m_err_file;
        };

        std::string Utf16ToUtf8(const std::wstring& w)
        {
            if (w.empty()) return std::string();
            int size = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                nullptr, 0, nullptr, nullptr);
            std::string out(size, '\0');
            if (size > 0)
                WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], size, nullptr, nullptr);
            return out;
        }

        // ---------------------------------------------------------------
        // 逐帧响度数据
        // ---------------------------------------------------------------
        struct RawFrame
        {
            double t = 0.0;
            float s = INVALID_LUFS;
            float tp = INVALID_LUFS;
        };

        bool ParseMetadata(const std::string& text, std::vector<RawFrame>& frames, float& integrated)
        {
            frames.clear();
            integrated = INVALID_LUFS;

            RawFrame cur;
            bool opened = false;
            bool has_i = false;

            size_t pos = 0;
            while (pos < text.size())
            {
                size_t end = text.find('\n', pos);
                if (end == std::string::npos) break;    // 只处理完整行
                std::string line = text.substr(pos, end - pos);
                pos = end + 1;
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty()) continue;

                if (line.compare(0, 6, "frame:") == 0)
                {
                    if (opened) frames.push_back(cur);
                    cur = RawFrame{};
                    opened = true;
                    size_t tp_pos = line.find("pts_time:");
                    if (tp_pos != std::string::npos)
                    {
                        float t = 0.0f;
                        if (ParseFloatValue(line.c_str() + tp_pos + 9, t))
                            cur.t = t;
                    }
                    continue;
                }

                const std::string prefix = "lavfi.r128.";
                if (line.compare(0, prefix.size(), prefix) != 0) continue;
                size_t eq = line.find('=');
                if (eq == std::string::npos) continue;
                std::string key = line.substr(prefix.size(), eq - prefix.size());
                const char* value = line.c_str() + eq + 1;
                float v = 0.0f;
                if (!ParseFloatValue(value, v)) continue;

                if (key == "S") cur.s = v;
                else if (key == "M") { /* 保留，暂未使用 */ }
                else if (key == "I") { integrated = v; has_i = true; }
                else if (key == "true_peak")
                {
                    // 注意：ffmpeg 的 lavfi.r128.true_peak 是线性幅度（1.0 = 满刻度），
                    // 而 M/S/I 是 dB(LUFS)，这里统一换算成 dBTP
                    if (v <= 0.0f)
                    {
                        cur.tp = INVALID_LUFS;
                    }
                    else
                    {
                        float db = 20.0f * static_cast<float>(std::log10(v));
                        cur.tp = (db < -120.0f) ? -120.0f : db;
                    }
                }
            }
            if (opened) frames.push_back(cur);

            // 丢掉解析异常（时间戳全为 0）的尾部帧
            while (!frames.empty() && frames.back().t <= 0.0 && frames.size() > 1)
                frames.pop_back();

            return !frames.empty() && has_i;
        }

        // 用最近的有效值填补无效（静音/不足一帧）的采样
        void FillInvalid(std::vector<float>& values)
        {
            int last_valid = -1;
            for (size_t i = 0; i < values.size(); ++i)
            {
                if (values[i] > INVALID_LUFS + 0.01f)
                    last_valid = static_cast<int>(i);
                else if (last_valid >= 0)
                    values[i] = values[last_valid];
            }
            // 开头仍然无效的，用后面第一个有效值
            float first_valid = 0.0f;
            bool found = false;
            for (float v : values)
            {
                if (v > INVALID_LUFS + 0.01f) { first_valid = v; found = true; break; }
            }
            if (found)
            {
                for (float& v : values)
                {
                    if (v <= INVALID_LUFS + 0.01f) v = first_valid;
                    else break;
                }
            }
        }

        // 由逐帧数据生成增益曲线
        void BuildCurve(const std::vector<RawFrame>& frames, float integrated,
                        const Settings& s, GainCurve& out)
        {
            out = GainCurve();
            out.integrated_lufs = integrated;
            out.dynamic = s.dynamic;

            const size_t n = frames.size();
            if (n == 0) return;
            std::vector<float> s_vals(n);
            std::vector<float> tp_vals(n);
            out.true_peak_dbtp = INVALID_LUFS;
            for (size_t i = 0; i < n; ++i)
            {
                s_vals[i] = frames[i].s;
                tp_vals[i] = frames[i].tp;
                if (frames[i].tp > out.true_peak_dbtp) out.true_peak_dbtp = frames[i].tp;
            }
            FillInvalid(s_vals);
            out.duration = frames.empty() ? 0.0 : frames.back().t + FRAME_STEP;

            // 1) 计算每帧的目标增益
            //    allowed 是"该帧峰值不超标所允许的最大增益"，平滑之后还要再约束一次，
            //    否则平滑把相邻帧的较大增益带过来，仍可能让峰值越过上限。
            const float kNoLimit = 96.0f;
            std::vector<float> gain(n);
            std::vector<float> allowed(n, kNoLimit);
            if (s.dynamic)
            {
                for (size_t i = 0; i < n; ++i)
                {
                    gain[i] = ClampF(s.target_lufs - s_vals[i], -s.max_cut_db, s.max_boost_db);
                    if (tp_vals[i] > INVALID_LUFS + 0.01f)
                        allowed[i] = s.ceiling_dbtp - tp_vals[i];
                    if (gain[i] > allowed[i]) gain[i] = allowed[i];
                }
            }
            else
            {
                // 整曲平衡：一个固定增益，峰值约束取整曲最大 true peak
                float g = ClampF(s.target_lufs - integrated, -s.max_cut_db, s.max_boost_db);
                float limit = kNoLimit;
                if (out.true_peak_dbtp > INVALID_LUFS + 0.01f)
                    limit = s.ceiling_dbtp - out.true_peak_dbtp;
                if (g > limit) g = limit;
                std::fill(gain.begin(), gain.end(), g);
                std::fill(allowed.begin(), allowed.end(), limit);
            }

            // 2) 平滑：增益变大(attack)/变小(release)使用不同的时间常数，
            //    避免短时响度抖动导致增益抖动
            if (n > 1)
            {
                double a_att = 1.0 - std::exp(-FRAME_STEP * 1000.0 / std::max(1.0f, s.attack_ms));
                double a_rel = 1.0 - std::exp(-FRAME_STEP * 1000.0 / std::max(1.0f, s.release_ms));
                double cur = gain[0];
                for (size_t i = 1; i < n; ++i)
                {
                    double a = (gain[i] > cur) ? a_att : a_rel;
                    cur += (gain[i] - cur) * a;
                    gain[i] = static_cast<float>(cur);
                }
            }

            // 3) 平滑后再做一次峰值约束，保证不会因为平滑而让峰值超限
            for (size_t i = 0; i < n; ++i)
            {
                if (gain[i] > allowed[i]) gain[i] = allowed[i];
            }

            // 4) 输出
            out.points.reserve(n);
            float gmin = gain[0], gmax = gain[0];
            for (size_t i = 0; i < n; ++i)
            {
                out.points.push_back(GainCurve::Point{ frames[i].t, gain[i] });
                gmin = std::min(gmin, gain[i]);
                gmax = std::max(gmax, gain[i]);
            }
            out.min_gain_db = gmin;
            out.max_gain_db = gmax;
        }

        bool AnalyzeFileInternal(const std::wstring& file_path, const Settings& settings,
                                 GainCurve& curve, std::wstring& error,
                                 const std::atomic<bool>* cancel)
        {
            error.clear();
            curve = GainCurve();

            if (file_path.empty())
            {
                error = L"文件路径为空";
                return false;
            }
            std::wstring ffmpeg = settings.ffmpeg_path;
            if (ffmpeg.empty() || !FileExistsW(ffmpeg))
                ffmpeg = FindFfmpeg(settings);
            if (ffmpeg.empty())
            {
                error = L"找不到 ffmpeg.exe（可把它放到程序目录，或在配置中指定 ffmpeg_path）";
                return false;
            }

            // 说明：-af 只有一个参数且不含空格，不需要额外加引号；
            //       ebur128 逐帧输出通过 ametadata 打印到 stdout，便于边分析边解析。
            std::wstring cmd;
            cmd.reserve(512);
            cmd += QuoteArg(ffmpeg);
            cmd += L" -hide_banner -nostdin -nostats -loglevel error -i ";
            cmd += QuoteArg(file_path);
            cmd += L" -map 0:a:0 -vn -sn -dn -af ebur128=peak=true:metadata=1,ametadata=mode=print:file=-";
            cmd += L" -f null -";

            ChildProcess proc;
            if (!proc.Start(cmd, 120000, error))
                return false;

            std::string buffer;
            buffer.reserve(1 << 20);
            std::wstring fatal_error;
            const size_t kMaxBuffer = 64u << 20;    // 约 30 万帧（8 小时），超出部分丢弃
            for (;;)
            {
                auto r = proc.Pump(buffer, cancel);
                if (r == ChildProcess::PumpResult::Cancelled)
                {
                    proc.Terminate();
                    error = L"已取消";
                    return false;
                }
                if (r == ChildProcess::PumpResult::Timeout)
                {
                    fatal_error = L"ffmpeg 分析超时";
                    break;
                }
                if (r == ChildProcess::PumpResult::Eof) break;
                if (buffer.size() > kMaxBuffer) break;
            }

            std::string stderr_text = proc.ReadErrorText();
            proc.Finish(2000);
            proc.Close();

            if (!fatal_error.empty())
            {
                error = fatal_error;
                return false;
            }

            std::vector<RawFrame> frames;
            float integrated = INVALID_LUFS;
            if (!ParseMetadata(buffer, frames, integrated))
            {
                if (proc.GetExitCode() != 0 && !stderr_text.empty())
                    error = L"ffmpeg 分析失败：" + std::wstring(stderr_text.begin(), stderr_text.end());
                else
                    error = L"未能从 ffmpeg 输出中解析到响度数据";
                return false;
            }

            BuildCurve(frames, integrated, settings, curve);
            if (curve.Empty())
            {
                error = L"响度曲线为空";
                return false;
            }
            return true;
        }
    } // namespace

    // -------------------------------------------------------------------
    // GainCurve
    // -------------------------------------------------------------------
    float GainCurve::GainAt(double seconds) const
    {
        if (points.empty()) return 0.0f;
        if (seconds <= points.front().t) return points.front().gain_db;
        if (seconds >= points.back().t) return points.back().gain_db;

        size_t lo = 0;
        size_t hi = points.size() - 1;
        while (hi - lo > 1)
        {
            size_t mid = (lo + hi) / 2;
            if (points[mid].t <= seconds) lo = mid;
            else hi = mid;
        }
        double t0 = points[lo].t;
        double t1 = points[hi].t;
        float g0 = points[lo].gain_db;
        float g1 = points[hi].gain_db;
        if (t1 <= t0) return g1;
        double k = (seconds - t0) / (t1 - t0);
        return static_cast<float>(g0 + (g1 - g0) * k);
    }

    // -------------------------------------------------------------------
    // ffmpeg 查找
    // -------------------------------------------------------------------
    std::wstring FindFfmpeg(const Settings& settings)
    {
        if (!settings.ffmpeg_path.empty() && FileExistsW(settings.ffmpeg_path))
            return settings.ffmpeg_path;

        const std::wstring dir = GetModuleDir();
        if (!dir.empty())
        {
            const std::wstring candidates[] = {
                dir + L"ffmpeg.exe",
                dir + L"ffmpeg\\bin\\ffmpeg.exe",
                dir + L"bin\\ffmpeg.exe",
            };
            for (const auto& c : candidates)
            {
                if (FileExistsW(c)) return c;
            }
        }

        wchar_t found[MAX_PATH * 2] = { 0 };
        if (SearchPathW(nullptr, L"ffmpeg.exe", nullptr, MAX_PATH * 2, found, nullptr) > 0)
            return std::wstring(found);

        return std::wstring();
    }

    bool AnalyzeFile(const std::wstring& file_path, const Settings& settings,
                     GainCurve& curve, std::wstring& error)
    {
        return AnalyzeFileInternal(file_path, settings, curve, error, nullptr);
    }

    // -------------------------------------------------------------------
    // GainApplier
    // -------------------------------------------------------------------
    void GainApplier::SetSettings(const Settings& settings)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_settings = settings;
    }

    void GainApplier::SetCurve(std::shared_ptr<const GainCurve> curve)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_curve = std::move(curve);
    }

    void GainApplier::Reset()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_curve.reset();
        m_cur_db = 0.0f;
        m_last_end_pos = -1.0;
        m_fast_frames = 0;
    }

    bool GainApplier::HasCurve() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_curve && !m_curve->Empty();
    }

    GainApplier::BlockGain GainApplier::ComputeBlock(size_t frames, uint32_t freq, double pos_sec)
    {
        BlockGain bg;
        std::lock_guard<std::mutex> lock(m_mutex);

        if (!m_settings.enable || frames == 0 || freq == 0 || !m_curve || m_curve->Empty())
        {
            m_cur_db = 0.0f;
            m_last_end_pos = pos_sec;
            return bg;
        }

        const double dur = static_cast<double>(frames) / static_cast<double>(freq);

        // 曲目第一块：直接对齐到曲线（此时还没有声音，不存在爆音问题）
        if (m_last_end_pos < 0.0)
        {
            m_cur_db = m_curve->GainAt(pos_sec);
            m_last_end_pos = pos_sec;
        }

        // 检测进度跳转（拖动进度条、AB 重复、循环播放），跳转后允许快速过渡
        if (std::fabs(pos_sec - m_last_end_pos) > 0.25)
            m_fast_frames = freq / 20;      // 50ms 内过渡完成

        const double target = m_curve->GainAt(pos_sec + dur);
        const double rate = (m_fast_frames > 0) ? 800.0
                                                : std::max(1.0f, m_settings.max_rate_db_per_sec);
        const double max_delta = rate * dur;

        double g_end = target;
        if (g_end > m_cur_db + max_delta) g_end = m_cur_db + max_delta;
        else if (g_end < m_cur_db - max_delta) g_end = m_cur_db - max_delta;

        bg.lin_start = DbToLin(m_cur_db);
        const float lin_end = DbToLin(g_end);
        bg.factor = (frames > 1 && bg.lin_start > 0.0f)
            ? static_cast<float>(std::pow(static_cast<double>(lin_end) / bg.lin_start,
                                          1.0 / static_cast<double>(frames)))
            : 1.0f;

        m_cur_db = static_cast<float>(g_end);
        m_last_end_pos = pos_sec + dur;
        if (static_cast<size_t>(m_fast_frames) > frames) m_fast_frames -= static_cast<uint32_t>(frames);
        else m_fast_frames = 0;

        return bg;
    }

    void GainApplier::ProcessFloat(float* buffer, size_t frames, int channels, uint32_t freq, double pos_sec)
    {
        if (buffer == nullptr || frames == 0 || channels <= 0) return;
        BlockGain bg = ComputeBlock(frames, freq, pos_sec);

        const size_t total = frames * static_cast<size_t>(channels);
        if (bg.factor == 1.0f)
        {
            if (bg.lin_start == 1.0f) return;
            const float g = bg.lin_start;
            for (size_t i = 0; i < total; ++i) buffer[i] *= g;
            return;
        }

        float lin = bg.lin_start;
        float* p = buffer;
        for (size_t i = 0; i < frames; ++i)
        {
            for (int c = 0; c < channels; ++c) *p++ *= lin;
            lin *= bg.factor;
        }
    }

    void GainApplier::ProcessS16(int16_t* buffer, size_t frames, int channels, uint32_t freq, double pos_sec)
    {
        if (buffer == nullptr || frames == 0 || channels <= 0) return;
        BlockGain bg = ComputeBlock(frames, freq, pos_sec);
        if (bg.factor == 1.0f && bg.lin_start == 1.0f) return;

        float lin = bg.lin_start;
        for (size_t i = 0; i < frames; ++i)
        {
            for (int c = 0; c < channels; ++c)
            {
                float v = buffer[i * channels + c] * lin;
                if (v > 32767.0f) v = 32767.0f;
                else if (v < -32768.0f) v = -32768.0f;
                buffer[i * channels + c] = static_cast<int16_t>(v < 0 ? v - 0.5f : v + 0.5f);
            }
            lin *= bg.factor;
        }
    }

    // -------------------------------------------------------------------
    // Balancer
    // -------------------------------------------------------------------
    Balancer& Balancer::Instance()
    {
        static Balancer instance;
        return instance;
    }

    Balancer::~Balancer()
    {
        StopPrefetch();
        JoinWorker();
    }

    void Balancer::Log(const std::wstring& msg)
    {
        std::function<void(const std::wstring&)> logger;
        {
            std::lock_guard<std::mutex> lock(m_log_mutex);
            logger = m_logger;
        }
        if (logger) logger(msg);
    }

    void Balancer::SetLogger(std::function<void(const std::wstring&)> logger)
    {
        std::lock_guard<std::mutex> lock(m_log_mutex);
        m_logger = std::move(logger);
    }

    void Balancer::SetStateCallback(std::function<void(State)> cb)
    {
        std::lock_guard<std::mutex> lock(m_cb_mutex);
        m_state_cb = std::move(cb);
    }

    void Balancer::NotifyState(State state)
    {
        std::function<void(State)> cb;
        {
            std::lock_guard<std::mutex> lock(m_cb_mutex);
            cb = m_state_cb;
        }
        if (cb) cb(state);      // 在锁外调用，避免回调里查询状态造成死锁
    }

    void Balancer::Configure(const Settings& settings)
    {
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(m_settings_mutex);
            if (m_settings.target_lufs != settings.target_lufs ||
                m_settings.dynamic != settings.dynamic ||
                m_settings.max_boost_db != settings.max_boost_db ||
                m_settings.max_cut_db != settings.max_cut_db ||
                m_settings.ceiling_dbtp != settings.ceiling_dbtp ||
                m_settings.ffmpeg_path != settings.ffmpeg_path)
            {
                changed = true;
            }
            if (changed)
            {
                ++m_config_gen;     // 让正在进行的预取结果失效
            }
            m_settings = settings;
        }
        if (changed)
        {
            std::lock_guard<std::mutex> lock(m_cache_mutex);
            m_cache.clear();
        }
    }

    Settings Balancer::GetSettings() const
    {
        std::lock_guard<std::mutex> lock(m_settings_mutex);
        return m_settings;
    }

    std::wstring Balancer::GetLastError() const
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        return m_error;
    }

    std::wstring Balancer::GetCurrentFile() const
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        return m_file;
    }

    std::shared_ptr<const GainCurve> Balancer::FindCached(const std::wstring& path)
    {
        std::lock_guard<std::mutex> lock(m_cache_mutex);
        for (auto& item : m_cache)
        {
            if (item.first == path) return item.second;
        }
        return nullptr;
    }

    void Balancer::PutCache(const std::wstring& path, std::shared_ptr<const GainCurve> curve)
    {
        std::lock_guard<std::mutex> lock(m_cache_mutex);
        for (auto& item : m_cache)
        {
            if (item.first == path) { item.second = std::move(curve); return; }
        }
        if (m_cache.size() >= 64) m_cache.erase(m_cache.begin());
        m_cache.emplace_back(path, std::move(curve));
    }

    void Balancer::JoinWorker()
    {
        m_cancel.store(true);
        std::thread worker;
        {
            std::lock_guard<std::mutex> lock(m_worker_mutex);
            ++m_generation;                 // 使旧线程的结果失效
            worker = std::move(m_worker);
        }
        if (worker.joinable()) worker.join();
        m_cancel.store(false);
    }

    void Balancer::OnTrackOpened(const std::wstring& file_path)
    {
        Settings settings = GetSettings();
        if (!settings.enable)
        {
            JoinWorker();
            {
                std::lock_guard<std::mutex> lock(m_curve_mutex);
                m_curve.reset();
            }
            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                m_state = State::Disabled;
                m_file = file_path;
                m_lufs = 0.0f;
                m_error.clear();
            }
            NotifyState(State::Disabled);
            return;
        }

        // 命中缓存：直接可用，无需再跑 ffmpeg
        auto cached = FindCached(file_path);
        if (cached)
        {
            JoinWorker();
            {
                std::lock_guard<std::mutex> lock(m_curve_mutex);
                m_curve = cached;
            }
            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                m_state = State::Ready;
                m_file = file_path;
                m_lufs = cached->integrated_lufs;
                m_error.clear();
            }

            wchar_t buf[256] = { 0 };
            swprintf_s(buf, L"[响度平衡] 已就绪（命中缓存，无需分析）：I = %.1f LUFS, 增益 %.1f ~ %.1f dB, 目标 %.1f LUFS",
                cached->integrated_lufs, cached->min_gain_db, cached->max_gain_db, settings.target_lufs);
            Log(buf);
            NotifyState(State::Ready);
            return;
        }

        if (FindFfmpeg(settings).empty())
        {
            JoinWorker();
            {
                std::lock_guard<std::mutex> lock(m_curve_mutex);
                m_curve.reset();
            }
            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                m_state = State::NoFfmpeg;
                m_file = file_path;
                m_lufs = 0.0f;
                m_error = L"找不到 ffmpeg.exe，响度平衡已停用";
            }
            Log(L"[响度平衡] 找不到 ffmpeg.exe，响度平衡已停用");
            NotifyState(State::NoFfmpeg);
            return;
        }

        JoinWorker();
        {
            std::lock_guard<std::mutex> lock(m_curve_mutex);
            m_curve.reset();
        }

        uint64_t generation = 0;
        {
            std::lock_guard<std::mutex> lock(m_worker_mutex);
            generation = m_generation;
        }
        {
            std::lock_guard<std::mutex> lock(m_state_mutex);
            m_state = State::Analyzing;
            m_file = file_path;
            m_lufs = 0.0f;
            m_error.clear();
        }
        m_cancel.store(false);

        std::thread worker(&Balancer::AnalyseWorker, this, file_path, settings, generation);
        {
            std::lock_guard<std::mutex> lock(m_worker_mutex);
            m_worker = std::move(worker);
        }

        NotifyState(State::Analyzing);
    }

    void Balancer::OnTrackClosed()
    {
        JoinWorker();
        {
            std::lock_guard<std::mutex> lock(m_curve_mutex);
            m_curve.reset();
        }
        {
            std::lock_guard<std::mutex> state_lock(m_state_mutex);
            m_state = State::Idle;
        }
        NotifyState(State::Idle);
    }

    void Balancer::AnalyseWorker(std::wstring file_path, Settings settings, uint64_t generation)
    {
        GainCurve curve;
        std::wstring error;
        bool ok = AnalyzeFileInternal(file_path, settings, curve, error, &m_cancel);

        if (m_cancel.load())
            return;     // 已被取消（换曲/停止），不发布结果

        {
            std::lock_guard<std::mutex> lock(m_worker_mutex);
            if (generation != m_generation) return;     // 已被新的曲目取代
        }

        if (ok)
        {
            auto shared = std::make_shared<const GainCurve>(std::move(curve));
            PutCache(file_path, shared);
            {
                std::lock_guard<std::mutex> lock(m_curve_mutex);
                m_curve = shared;
            }
            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                m_state = State::Ready;
                m_file = file_path;
                m_lufs = shared->integrated_lufs;
            }

            wchar_t buf[256] = { 0 };
            swprintf_s(buf, L"[响度平衡] 已就绪：I = %.1f LUFS, 增益 %.1f ~ %.1f dB, 目标 %.1f LUFS",
                shared->integrated_lufs, shared->min_gain_db, shared->max_gain_db, settings.target_lufs);
            Log(buf);
            NotifyState(State::Ready);      // 必须在锁外调用：回调会查询状态/文件
        }
        else
        {
            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                m_state = State::Failed;
                m_file = file_path;
                m_error = error;
            }
            Log(L"[响度平衡] 分析失败：" + error);
            NotifyState(State::Failed);
        }
    }

    std::shared_ptr<const GainCurve> Balancer::TryGetCurve()
    {
        std::unique_lock<std::mutex> lock(m_curve_mutex, std::try_to_lock);
        if (!lock.owns_lock()) return nullptr;
        return m_curve;
    }

    // -----------------------------------------------------------------------
    // 预取：播放当前曲目时，提前分析下一曲
    // -----------------------------------------------------------------------
    void Balancer::Prefetch(const std::wstring& file_path)
    {
        if (file_path.empty()) return;

        Settings settings = GetSettings();
        if (!settings.enable || !settings.prefetch) return;
        if (FindCached(file_path)) return;      // 已经算过，不必重复

        // 正在播放的文件由主分析线程负责，这里不用管
        {
            std::lock_guard<std::mutex> lock(m_state_mutex);
            if (!m_file.empty() && _wcsicmp(m_file.c_str(), file_path.c_str()) == 0)
                return;
        }

        std::lock_guard<std::mutex> lock(m_pf_mutex);
        if (m_pf_path == file_path) return;     // 已在队列中等候
        m_pf_path = file_path;
        if (!m_prefetch_thread.joinable())
            m_prefetch_thread = std::thread(&Balancer::PrefetchWorker, this);
        m_pf_cv.notify_one();
    }

    void Balancer::PrefetchWorker()
    {
        for (;;)
        {
            std::wstring path;
            {
                std::unique_lock<std::mutex> lock(m_pf_mutex);
                m_pf_cv.wait(lock, [this] { return m_pf_exit || !m_pf_path.empty(); });
                if (m_pf_exit) return;
                path = m_pf_path;
                m_pf_path.clear();
            }

            m_pf_cancel.store(false);

            uint64_t gen = 0;
            Settings settings;
            {
                std::lock_guard<std::mutex> lock(m_settings_mutex);
                settings = m_settings;
                gen = m_config_gen;
            }

            GainCurve curve;
            std::wstring error;
            bool ok = AnalyzeFileInternal(path, settings, curve, error, &m_pf_cancel);

            if (m_pf_exit || !ok) continue;

            // 分析期间参数被改动过，这条曲线已经不符合当前设置
            {
                std::lock_guard<std::mutex> lock(m_settings_mutex);
                if (gen != m_config_gen) continue;
            }

            PutCache(path, std::make_shared<const GainCurve>(std::move(curve)));

            std::wstring name = path;
            size_t pos = name.find_last_of(L"\\/");
            if (pos != std::wstring::npos) name = name.substr(pos + 1);
            Log(L"[响度平衡] 已预取下一曲：" + name + L"（切歌时直接生效）");
        }
    }

    void Balancer::StopPrefetch()
    {
        m_pf_cancel.store(true);
        std::thread th;
        {
            std::lock_guard<std::mutex> lock(m_pf_mutex);
            m_pf_exit = true;
            m_pf_path.clear();
            m_pf_cv.notify_all();
            th = std::move(m_prefetch_thread);
        }
        if (th.joinable()) th.join();
    }

} // namespace loudness
