import os, sys, time, subprocess, ctypes, ctypes.wintypes as wt

sys.path.insert(0, r"E:\loundless\tools")
from shot import find_main, grab

ctypes.windll.shcore.SetProcessDpiAwareness(2)
u = ctypes.windll.user32
DIST = r"E:\loundless\dist"
LOG = os.path.join(DIST, "error.log")

# 播放按钮在窗口内的相对坐标（由截图标定，窗口 1770x1214）
PLAY_BTN = (210, 1163)


def click(x, y):
    u.SetCursorPos(x, y)
    time.sleep(0.15)
    u.mouse_event(0x0002, 0, 0, 0, 0)   # LEFTDOWN
    time.sleep(0.08)
    u.mouse_event(0x0004, 0, 0, 0, 0)   # LEFTUP


def logtail(n=12):
    if not os.path.exists(LOG):
        return ""
    t = open(LOG, "rb").read().decode("utf-8", "replace")
    return "\n".join([l for l in t.splitlines() if "响度平衡" in l][-n:])


p = subprocess.Popen([os.path.join(DIST, "MusicPlayer2.exe")], cwd=DIST)
time.sleep(3.0)
h = find_main(p.pid)
u.SetForegroundWindow(h)
u.SetWindowPos(h, -1, 0, 0, 0, 0, 0x0001 | 0x0002)
time.sleep(0.5)

r = wt.RECT()
u.GetWindowRect(h, ctypes.byref(r))
bx, by = r.left + PLAY_BTN[0], r.top + PLAY_BTN[1]
print("窗口:", r.left, r.top, "点击播放按钮:", bx, by)

click(bx, by)
time.sleep(2.0)
grab(h, r"E:\loundless\shot_playing.bmp")
print("--- 点击播放后 ---")
print(logtail())

# 再点一次 = 暂停
click(bx, by)
time.sleep(1.5)
grab(h, r"E:\loundless\shot_paused.bmp")
print("--- 再点一次（暂停）---")
print(logtail())

u.SetWindowPos(h, -2, 0, 0, 0, 0, 0x0001 | 0x0002)
p.terminate()
