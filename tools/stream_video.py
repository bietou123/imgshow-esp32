#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
stream_video.py  —  电脑 -> 摆件 128x128 全分辨率实时推流 (裸 TCP 8888)

这是"真的能在这块板子上看视频"的唯一办法:
  * 一条长连接背靠背发裸帧，没有 HTTP 头、没有重连开销。
  * ESP32 TCP 收包实测 0.8~1.2MB/s -> 128x128 RGB565(32768B/帧) = 25~35fps。
  * 长度不限，因为不落 flash。缺点是电脑/手机必须一直开着。
  (对比: 每帧一个 HTTP POST 的老做法只有 ~5fps。)

用法:
    python tools/stream_video.py 192.168.1.50 movie.mp4
    python tools/stream_video.py 192.168.1.50 movie.mp4 --fps 25 --loop
    python tools/stream_video.py 192.168.1.50 --screen          # 投屏(桌面左上角)
    python tools/stream_video.py 192.168.1.50 --cam 0           # 摄像头
    python tools/stream_video.py 192.168.1.50 rtsp://...        # 网络流

摆件那边: 进 Video 页 -> 双击 OK 切到 LIVE 模式(有客户端连上会自动切)。

依赖: ffmpeg 在 PATH 里。
"""
import argparse, shutil, socket, subprocess, sys, time

W = H = 128
FRAME = W * H * 2


def build_cmd(a):
    vf = ("scale=%d:%d:force_original_aspect_ratio=increase,crop=%d:%d" % (W, H, W, H))
    cmd = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-re"]
    if a.screen:
        if sys.platform.startswith("win"):
            cmd = ["ffmpeg", "-hide_banner", "-loglevel", "error",
                   "-f", "gdigrab", "-framerate", str(a.fps), "-i", "desktop"]
        else:
            cmd = ["ffmpeg", "-hide_banner", "-loglevel", "error",
                   "-f", "x11grab", "-framerate", str(a.fps), "-i", ":0.0"]
    elif a.cam is not None:
        if sys.platform.startswith("win"):
            cmd = ["ffmpeg", "-hide_banner", "-loglevel", "error",
                   "-f", "dshow", "-i", "video=%s" % a.cam]
        else:
            cmd = ["ffmpeg", "-hide_banner", "-loglevel", "error",
                   "-f", "v4l2", "-i", "/dev/video%s" % a.cam]
    else:
        if a.loop:
            cmd += ["-stream_loop", "-1"]
        cmd += ["-i", a.src]
    cmd += ["-vf", "fps=%d,%s" % (a.fps, vf), "-pix_fmt", "rgb565le", "-f", "rawvideo", "-"]
    return cmd


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("src", nargs="?", default=None)
    ap.add_argument("--port", type=int, default=8888)
    ap.add_argument("--fps", type=int, default=24)
    ap.add_argument("--loop", action="store_true")
    ap.add_argument("--screen", action="store_true")
    ap.add_argument("--cam", default=None)
    a = ap.parse_args()

    if not a.src and not a.screen and a.cam is None:
        sys.exit("要么给个文件/URL，要么 --screen / --cam")
    if not shutil.which("ffmpeg"):
        sys.exit("找不到 ffmpeg")

    print("连接 %s:%d ..." % (a.host, a.port))
    s = socket.create_connection((a.host, a.port), timeout=8)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    # 发送缓冲别太大，否则本地缓冲一堆帧，画面比声音慢好几秒
    s.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 64 * 1024)
    print("已连接。Ctrl+C 停止。")

    p = subprocess.Popen(build_cmd(a), stdout=subprocess.PIPE)
    n, t0, last = 0, time.time(), time.time()
    try:
        while True:
            buf = p.stdout.read(FRAME)
            if len(buf) < FRAME:
                break
            s.sendall(buf)
            n += 1
            now = time.time()
            if now - last >= 1.0:
                print("\r%d 帧, %.1f fps, %.0f KB/s   " %
                      (n, n / (now - t0), n * FRAME / 1024.0 / (now - t0)), end="")
                last = now
    except KeyboardInterrupt:
        print("\n停止")
    except (BrokenPipeError, ConnectionResetError):
        print("\n连接被断开(摆件离开了 LIVE 页面?)")
    finally:
        try:
            p.kill()
        except Exception:
            pass
        s.close()
    print("\n共发送 %d 帧" % n)


if __name__ == "__main__":
    main()
