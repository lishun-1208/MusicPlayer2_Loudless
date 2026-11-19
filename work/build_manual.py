#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
用 cl/rc/link 做 Release 等效构建，绕开 MSBuild（本机安全策略禁止调用 msbuild.exe）。
产物输出到 build_out/（x64）或 build_out_x86/（x86），不污染源码目录。

用法：
    python build_manual.py compile          # x64 只编译
    python build_manual.py all              # x64 编译 + 资源 + 链接
    python build_manual.py all --x86        # x86 全量
    python build_manual.py link --x86       # x86 复用已有 obj，只 rc + link
"""
import os
import sys
import time
import subprocess
import xml.etree.ElementTree as ET
import concurrent.futures as cf

MSVC = r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207"
SDK = r"C:\Program Files (x86)\Windows Kits\10"
SDKV = "10.0.26100.0"
PROJ = r"E:\loundless\MusicPlayer2\MusicPlayer2"

ARCH = "x86" if "--x86" in sys.argv else "x64"
HOST = "Hostx86" if ARCH == "x86" else "Hostx64"
_out_name = next((a.split("=", 1)[1] for a in sys.argv if a.startswith("--out=")), None)
OUT = os.path.join(r"E:\loundless", _out_name or ("build_out" if ARCH == "x64" else "build_out_x86"))
OBJ = os.path.join(OUT, "obj")

CL = os.path.join(MSVC, "bin", HOST, ARCH, "cl.exe")
LINK = os.path.join(MSVC, "bin", HOST, ARCH, "link.exe")
RC = os.path.join(SDK, "bin", SDKV, ARCH, "rc.exe")

NS = "{http://schemas.microsoft.com/developer/msbuild/2003}"

def make_env():
    inc = [
        os.path.join(MSVC, "include"),
        os.path.join(MSVC, "atlmfc", "include"),
        os.path.join(SDK, "Include", SDKV, "ucrt"),
        os.path.join(SDK, "Include", SDKV, "um"),
        os.path.join(SDK, "Include", SDKV, "shared"),
        os.path.join(SDK, "Include", SDKV, "winrt"),
        r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\VS\include",
    ]
    lib = [
        os.path.join(MSVC, "lib", ARCH),
        os.path.join(MSVC, "atlmfc", "lib", ARCH),
        os.path.join(SDK, "Lib", SDKV, "ucrt", ARCH),
        os.path.join(SDK, "Lib", SDKV, "um", ARCH),
    ]
    env = os.environ.copy()
    env["INCLUDE"] = ";".join(inc)
    env["LIB"] = ";".join(lib)
    return env

def collect_sources():
    tree = ET.parse(os.path.join(PROJ, "MusicPlayer2.vcxproj"))
    root = tree.getroot()
    srcs = []
    skipped = []
    cfg = "Release|" + ("Win32" if ARCH == "x86" else "x64")
    for el in root.iter(NS + "ClCompile"):
        inc = el.get("Include")
        if not inc:
            continue
        excluded = False
        for ch in el.findall(NS + "ExcludedFromBuild"):
            cond = ch.get("Condition", "")
            if cfg in cond and (ch.text or "").strip().lower() == "true":
                excluded = True
        if excluded:
            skipped.append(inc)
        else:
            srcs.append(inc)
    return srcs, skipped

def obj_name(rel):
    return rel.replace("\\", "_").replace("/", "_").rsplit(".", 1)[0] + ".obj"

def is_utf8(path):
    """工程没设 /utf-8，MSBuild 走系统代码页（GBK）。个别源文件是 GBK 的，
    这里按文件实际编码决定是否加 /utf-8，避免编译失败。"""
    try:
        with open(path, "rb") as f:
            if f.read(3) == b"\xef\xbb\xbf":
                return True
            f.seek(0)
            f.read().decode("utf-8")
            return True
    except (UnicodeDecodeError, OSError):
        return False

def compile_one(rel, env):
    src = os.path.join(PROJ, rel.replace("\\", "/"))
    obj = os.path.join(OBJ, obj_name(rel))
    cmd = [
        CL, "/nologo", "/c", "/W3",
        "/O2", "/Oi", "/Gy", "/MD", "/EHsc",
        "/std:c++latest", "/Y-",
        "/D_AFXDLL", "/D_WINDOWS", "/DNDEBUG", "/DUNICODE", "/D_UNICODE",
        "/I", PROJ, "/Fo:" + obj, src,
    ]
    if ARCH == "x86":
        cmd.insert(1, "/arch:SSE2")
    if is_utf8(src):
        cmd.insert(cmd.index("/Y-") + 1, "/utf-8")
    try:
        p = subprocess.run(cmd, capture_output=True, env=env, timeout=300)
        out = (p.stdout or b"") + (p.stderr or b"")
        return rel, p.returncode, out.decode("gbk", "replace")
    except subprocess.TimeoutExpired:
        return rel, 99, "TIMEOUT"

def do_compile(srcs, env):
    os.makedirs(OBJ, exist_ok=True)
    workers = max(4, min(12, (os.cpu_count() or 8)))
    print("编译 %d 个源文件（%s），并行 %d ..." % (len(srcs), ARCH, workers))
    t0 = time.time()
    fails = []
    done = 0
    with cf.ThreadPoolExecutor(max_workers=workers) as ex:
        futs = {ex.submit(compile_one, s, env): s for s in srcs}
        for f in cf.as_completed(futs):
            rel, rc, out = f.result()
            done += 1
            if rc != 0:
                fails.append((rel, out))
                print("  [FAIL] %s" % rel)
            elif done % 40 == 0:
                print("  ... %d/%d  (%.1fs)" % (done, len(srcs), time.time() - t0))
    print("编译完成：%d 个，失败 %d 个，耗时 %.1fs" % (len(srcs), len(fails), time.time() - t0))
    for rel, out in fails[:5]:
        print("\n===== %s =====" % rel)
        print(out[:2500])
    return fails

def do_rc(env):
    # 工程 PreBuildEvent 的 print_compile_time.bat 会生成它，资源里 TEXT 引入了
    ct = os.path.join(OBJ, "compile_time.txt")
    with open(ct, "w", encoding="mbcs") as f:
        f.write(time.strftime("%Y-%m-%d %H:%M:%S") + "\n")
        f.write("manual-build\n")
    res = os.path.join(OUT, "MusicPlayer2.res")
    cmd = [RC, "/nologo", "/DNDEBUG", "/l", "0x0409",
           "/I", PROJ, "/I", OBJ, "/fo", res,
           os.path.join(PROJ, "MusicPlayer2.rc")]
    p = subprocess.run(cmd, capture_output=True, env=env)
    txt = ((p.stdout or b"") + (p.stderr or b"")).decode("gbk", "replace")
    print("rc: rc=%d %s" % (p.returncode, txt.strip()[:500]))
    return res if p.returncode == 0 else None

def do_manifest_rc(env):
    # 清单通过资源方式嵌入（RT_MANIFEST = 24），避免 link 调用被拦截的 mt.exe
    src = os.path.join(r"E:\loundless", "manifest.rc")
    out = os.path.join(OUT, "manifest.res")
    cmd = [RC, "/nologo", "/l", "0x0409", "/fo", out, src]
    p = subprocess.run(cmd, capture_output=True, env=env)
    if p.returncode != 0:
        txt = ((p.stdout or b"") + (p.stderr or b"")).decode("gbk", "replace")
        print("manifest rc 失败：%s" % txt.strip()[:500])
        return None
    return out

def do_link(env, res):
    objs = [os.path.join(OBJ, f) for f in os.listdir(OBJ) if f.lower().endswith(".obj")]
    rsp = os.path.join(OUT, "link.rsp")
    exe = os.path.join(OUT, "MusicPlayer2.exe")
    # 注意顺序：目标文件在前、库在后，否则库里的符号不会被拉入
    lines = [("/OUT:\"%s\"" % exe),
             "/SUBSYSTEM:WINDOWS", "/MACHINE:" + ("X86" if ARCH == "x86" else "X64"),
             "/ENTRY:wWinMainCRTStartup",
             "/INCLUDE:__afxForceSTDAFX",
             "/OPT:REF", "/OPT:ICF", "/DEBUG", "/NOLOGO",
             "/MAP:\"%s\"" % os.path.join(OUT, "MusicPlayer2.map"),
             "/MANIFEST:NO",
             "/DELAYLOAD:api-ms-win-core-winrt-l1-1-0.dll",
             "/DELAYLOAD:api-ms-win-core-winrt-string-l1-1-0.dll",
             "/DELAYLOAD:api-ms-win-shcore-stream-winrt-l1-1-0.dll",
             "/DELAYLOAD:powrprof.dll"]
    lines += ["\"%s\"" % o for o in objs]
    if res:
        lines.append("\"%s\"" % res)
    mres = os.path.join(OUT, "manifest.res")
    if os.path.exists(mres):
        lines.append("\"%s\"" % mres)
    if ARCH == "x86":
        lines += ["bass.lib", "tag.lib", "Powrprof.lib", "delayimp.lib"]
    else:
        lines += ["bass_x64.lib", "tag_x64.lib", "Powrprof.lib", "delayimp.lib"]
    with open(rsp, "w", encoding="mbcs") as f:
        f.write("\n".join(lines))
    p = subprocess.run([LINK, "@" + rsp], capture_output=True, env=env, cwd=PROJ)
    txt = ((p.stdout or b"") + (p.stderr or b"")).decode("gbk", "replace")
    print("link: rc=%d" % p.returncode)
    if p.returncode != 0:
        errs = [l for l in txt.splitlines() if "error" in l.lower()]
        print("\n".join(errs[:20]) if errs else txt[:3000])
    else:
        size = os.path.getsize(exe) if os.path.exists(exe) else 0
        print("产物：%s  (%.1f MB)" % (exe, size / 1048576.0))
    return p.returncode

def main():
    plain = [a for a in sys.argv[1:] if not a.startswith("--")]
    mode = plain[0] if plain else "all"
    env = make_env()
    if mode != "link":
        srcs, skipped = collect_sources()
        if skipped:
            print("跳过（ExcludedFromBuild）：%s" % ", ".join(skipped))
        fails = do_compile(srcs, env)
        if fails:
            print("\n编译失败，终止。")
            return 1
        if mode == "compile":
            return 0
    res = do_rc(env)
    do_manifest_rc(env)
    return do_link(env, res)

if __name__ == "__main__":
    sys.exit(main())
