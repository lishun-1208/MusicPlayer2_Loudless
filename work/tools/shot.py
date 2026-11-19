import os, sys, time, struct, subprocess, ctypes, ctypes.wintypes as wt

ctypes.windll.shcore.SetProcessDpiAwareness(2)
u = ctypes.windll.user32
g = ctypes.windll.gdi32
DIST = r"E:\loundless\dist"
OUT = r"E:\loundless\shot.bmp"


def find_main(pid):
    r = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(h, l):
        if u.IsWindowVisible(h):
            n = u.GetWindowTextLengthW(h)
            b = ctypes.create_unicode_buffer(n + 1)
            u.GetWindowTextW(h, b, n + 1)
            if b.value.endswith("MusicPlayer2"):
                q = wt.DWORD()
                u.GetWindowThreadProcessId(h, ctypes.byref(q))
                if q.value == pid:
                    r.append(h)
        return True

    u.EnumWindows(cb, 0)
    return r[0] if r else 0


def grab(hwnd, path):
    r = wt.RECT()
    u.GetWindowRect(hwnd, ctypes.byref(r))
    w, hgt = r.right - r.left, r.bottom - r.top
    hdc = u.GetWindowDC(hwnd)
    mdc = g.CreateCompatibleDC(hdc)
    bmp = g.CreateCompatibleBitmap(hdc, w, hgt)
    g.SelectObject(mdc, bmp)
    g.BitBlt(mdc, 0, 0, w, hgt, hdc, 0, 0, 0x00CC0020)

    class BIH(ctypes.Structure):
        _fields_ = [("biSize", wt.DWORD), ("biWidth", wt.LONG), ("biHeight", wt.LONG),
                    ("biPlanes", wt.WORD), ("biBitCount", wt.WORD), ("biCompression", wt.DWORD),
                    ("biSizeImage", wt.DWORD), ("biXPelsPerMeter", wt.LONG),
                    ("biYPelsPerMeter", wt.LONG), ("biClrUsed", wt.DWORD),
                    ("biClrImportant", wt.DWORD)]

    bi = BIH()
    bi.biSize = ctypes.sizeof(bi)
    bi.biWidth = w
    bi.biHeight = -hgt
    bi.biPlanes = 1
    bi.biBitCount = 32
    buf = ctypes.create_string_buffer(w * hgt * 4)
    g.GetDIBits(mdc, bmp, 0, hgt, buf, ctypes.byref(bi), 0)
    data = buf.raw
    hdr = struct.pack("<2sIHHI", b"BM", 54 + len(data), 0, 0, 54)
    inf = struct.pack("<IiiHHIIiiii", 40, w, -hgt, 1, 32, 0, len(data), 0, 0, 0, 0)
    open(path, "wb").write(hdr + inf + data)
    return w, hgt, r.left, r.top


if __name__ == "__main__":
    p = subprocess.Popen([os.path.join(DIST, "MusicPlayer2.exe")], cwd=DIST)
    time.sleep(3)
    h = find_main(p.pid)
    u.SetWindowPos(h, -1, 0, 0, 0, 0, 0x0001 | 0x0002)
    time.sleep(0.6)
    print(grab(h, OUT))
    u.SetWindowPos(h, -2, 0, 0, 0, 0, 0x0001 | 0x0002)
    p.terminate()
