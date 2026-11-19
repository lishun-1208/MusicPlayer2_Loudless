import os, time, subprocess, ctypes, ctypes.wintypes as wt

DIST = r"E:\loundless\dist"
EXE = os.path.join(DIST, "MusicPlayer2.exe")
LOG = os.path.join(DIST, "error.log")
u = ctypes.windll.user32


def find_main(pid=None):
    found = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(h, l):
        if u.IsWindowVisible(h):
            n = u.GetWindowTextLengthW(h)
            b = ctypes.create_unicode_buffer(n + 1)
            u.GetWindowTextW(h, b, n + 1)
            if b.value.endswith("MusicPlayer2"):
                if pid is None:
                    found.append(h)
                else:
                    q = wt.DWORD()
                    u.GetWindowThreadProcessId(h, ctypes.byref(q))
                    if q.value == pid:
                        found.append(h)
        return True

    u.EnumWindows(cb, 0)
    return found[0] if found else 0


def log_text():
    if not os.path.exists(LOG):
        return ""
    return open(LOG, "rb").read().decode("utf-8", "replace")


def play_pause_hotkey(h):
    u.SetForegroundWindow(h)
    time.sleep(0.4)
    u.keybd_event(0x11, 0, 0, 0)
    u.keybd_event(0x10, 0, 0, 0)
    u.keybd_event(0x74, 0, 0, 0)
    time.sleep(0.06)
    u.keybd_event(0x74, 0, 2, 0)
    u.keybd_event(0x10, 0, 2, 0)
    u.keybd_event(0x11, 0, 2, 0)


print("=== 启动，1.2 秒后立即按播放（此时分析未完成）===")
p = subprocess.Popen([EXE], cwd=DIST)
time.sleep(1.2)
h = find_main(p.pid)
print("主窗口：", hex(h) if h else "未找到")
if h:
    play_pause_hotkey(h)
time.sleep(6)

txt = log_text()
print("---- 日志 ----")
for line in txt.splitlines():
    if "响度平衡" in line:
        print("   ", line)

print("\n[判定] 命中拦截并自动续播：", "播放已暂停" in txt, "/", "分析完成，继续播放" in txt)

print("\n=== 正常关闭，检查播放状态是否被记录为“播放中” ===")
h = find_main(p.pid)
if h:
    u.PostMessageW(h, 0x0010, 0, 0)
for _ in range(20):
    if p.poll() is not None:
        break
    time.sleep(0.3)
print("已退出：", p.poll() is not None)
if p.poll() is None:
    p.terminate()

d = open(os.path.join(DIST, "recent_list.dat"), "rb").read(8)
print("recent_list.dat 前 8 字节：", d.hex(), "-> playing =", int.from_bytes(d[:4], "little"))
