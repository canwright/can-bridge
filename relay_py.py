# -*- coding: utf-8 -*-
"""
relay_py.py —— 虚拟通道中继（Python 版）

  周立功盒子  <--32位-->  VxlHelper.exe  <--共享内存-->  本程序  <--python-can-->  Vector 虚拟通道

为什么这么绕：
  · 周立功只有 32 位 DLL，64 位 Python 加载不了 → 交给 32 位的 VxlHelper
  · Vector 侧用 python-can（它的收发是实测可用的，绕开我们自写 C 的 API 坑）
"""
import ctypes, mmap, os, struct, subprocess, sys, time
import can
from can.interfaces.vector import VectorBus

SHM_NAME = "Local\\VxlFacadeShm_v1"
RING = 4096
FRAME = 32
OFF_RX = 36                      # 9 个 long 之后
OFF_TX = OFF_RX + RING * FRAME   # 36 + 131072
SHM_SIZE = OFF_TX + RING * FRAME

# 打包成 exe 后 __file__ 指向临时解压目录，必须用 exe 自己的目录
if getattr(sys, "frozen", False):
    HERE = os.path.dirname(os.path.abspath(sys.executable))
else:
    HERE = os.path.dirname(os.path.abspath(__file__))

def start_helper(dev=4, ch=0, baud=500000):
    exe = os.path.join(HERE, "VxlHelper.exe")
    if not os.path.exists(exe):
        print(f"[x] 找不到 {exe}")
        return None
    print(f"[..] 辅助进程: {exe}")
    p = subprocess.Popen([exe, str(dev), str(ch), str(baud)], cwd=HERE,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    print(f"[ok] VxlHelper 已启动 pid={p.pid}")
    return p

def open_shm():
    for _ in range(100):
        try:
            return mmap.mmap(-1, SHM_SIZE, tagname=SHM_NAME)
        except OSError:
            time.sleep(0.05)
    return None

def rd(shm, off):
    return struct.unpack_from("<l", shm, off)[0]

def wr(shm, off, v):
    struct.pack_into("<l", shm, off, v)

def pack_frame(shm, base, idx, cid, dlc, flags, data):
    o = base + (idx % RING) * FRAME
    shm[o:o+4] = struct.pack("<I", cid & 0x1FFFFFFF)
    shm[o+4:o+8] = struct.pack("<I", 0)
    shm[o+8] = dlc & 0xFF
    shm[o+9] = flags & 0xFF
    d = bytes(data[:8]).ljust(8, b"\x00")
    shm[o+10:o+18] = d

def unpack_frame(shm, base, idx):
    o = base + (idx % RING) * FRAME
    cid = struct.unpack_from("<I", shm, o)[0]
    ts = struct.unpack_from("<I", shm, o+4)[0]
    dlc = shm[o+8]
    flags = shm[o+9]
    data = bytes(shm[o+10:o+18])
    return cid, dlc, flags, data, ts

def main():
    dev = int(sys.argv[1]) if len(sys.argv) > 1 else 4
    vch = int(sys.argv[2]) if len(sys.argv) > 2 else 6
    baud = int(sys.argv[3]) if len(sys.argv) > 3 else 500000

    print("=" * 56)
    print("  虚拟通道中继 (Python)")
    print("=" * 56)

    # ---- 1. 周立功侧 ----
    hp = start_helper(dev, 0, baud)
    shm = open_shm()
    if shm is None:
        print("[x] 共享内存打不开")
        return 1
    for _ in range(200):
        if rd(shm, 4) == 1:
            print("[ok] 周立功盒子就绪")
            break
        if rd(shm, 12) != 0:
            print(f"[x] 辅助进程报错 err={rd(shm,12)}")
            return 1
        time.sleep(0.05)
    else:
        print("[x] 等周立功超时")
        return 1

    # ---- 2. Vector 侧 ----
    try:
        bus = VectorBus(channel=vch, bitrate=baud, app_name=None)
        print(f"[ok] Vector 虚拟通道 idx={vch} perm=0x{bus.permission_mask:X}")
    except Exception as e:
        print(f"[x] Vector 通道打开失败: {e}")
        return 1

    print("\n中继已运行，Ctrl+C 结束\n")
    n_v2h = 0   # 虚拟 → 硬件
    n_h2v = 0   # 硬件 → 虚拟
    last = time.time()

    try:
        while True:
            # --- 虚拟通道 → 周立功 ---
            m = bus.recv(timeout=0.005)
            if m is not None:
                th = rd(shm, 24); tt = rd(shm, 28)
                flags = 0
                if m.is_extended_id: flags |= 2
                if m.is_remote_frame: flags |= 1
                pack_frame(shm, OFF_TX, th, m.arbitration_id, len(m.data), flags, m.data)
                wr(shm, 24, th + 1)
                n_v2h += 1

            # --- 周立功 → 虚拟通道 ---
            rh = rd(shm, 16); rt = rd(shm, 20)
            while rt != rh:
                cid, dlc, flags, data, _ts = unpack_frame(shm, OFF_RX, rt)
                rt += 1
                try:
                    bus.send(can.Message(
                        arbitration_id=cid, data=data[:dlc],
                        is_extended_id=bool(flags & 2),
                        is_remote_frame=bool(flags & 1),
                        channel=vch))
                    n_h2v += 1
                except Exception:
                    pass
            wr(shm, 20, rt)

            now = time.time()
            if now - last >= 1.0:
                last = now
                print(f"\r  虚拟->硬件 {n_v2h} 帧   硬件->虚拟 {n_h2v} 帧   ", end="", flush=True)

    except KeyboardInterrupt:
        print("\n\n[停] 用户中断")
    finally:
        wr(shm, 8, 1)          # quit
        try: bus.shutdown()
        except Exception: pass
        time.sleep(0.3)
        try: hp.terminate()
        except Exception: pass
    return 0

if __name__ == "__main__":
    sys.exit(main())
