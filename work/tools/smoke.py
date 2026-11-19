import os, sys, time, subprocess, ctypes

sys.path.insert(0, r"E:\loundless\tools")
from shot import find_main

DIST = r"E:\loundless\dist"
LOG = os.path.join(DIST, "error.log")
EXE = os.path.join(DIST, "MusicPlayer2.exe")

for i in range(3):
    if os.path.exists(LOG):
        os.remove(LOG)
    p = subprocess.Popen([EXE], cwd=DIST)
    time.sleep(4.5)
    alive = p.poll() is None
    log = open(LOG, "rb").read().decode("utf-8", "replace") if os.path.exists(LOG) else ""
    auto = "播放已暂停" in log or "分析完成，继续播放" in log
    ready = "已就绪" in log
    print(f"第 {i+1} 次：存活={alive}  自动播放痕迹={auto}  曲线已就绪={ready}")
    if alive:
        p.terminate()
        p.wait(timeout=10)
    time.sleep(1.0)
