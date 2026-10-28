#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
mkvideo.py  —  视频 -> 摆件能离线播放的 .v64 片段

为什么是 64x64 / 10fps:
  板载 flash 分给 LittleFS 最多 2MB。
    128x128 RGB565 一帧 32768B  ->  2MB 只有 61 帧 -> 4 秒。没法用。
    64x64  RGB565 一帧  8192B  ->  2MB 有 240 帧  -> 24 秒。可用。
  播放时固件把每个像素放大 2x2，铺满 128x128(马赛克风，但流畅)。
  要全分辨率长视频只有两条路: 外挂 SD 卡，或者用 stream_video.py 实时推流。

文件格式 (固件里的 VidHdr):
  offset 0  : "IV1\\0"
  offset 4  : uint16 w
  offset 6  : uint16 h
  offset 8  : uint16 fps
  offset 10 : uint16 frames
  offset 12 : 保留 4 字节
  之后就是连续的裸帧，每帧 w*h*2 字节，RGB565 little-endian

用法:
    python tools/mkvideo.py in.mp4                       # 64x64 10fps 上限 1.8MB
    python tools/mkvideo.py in.mp4 -o cat.v64 --fps 12
    python tools/mkvideo.py in.mp4 --ss 30 --size 64 --max-kb 900
    python tools/mkvideo.py in.gif --size 128 --fps 8    # 128x128 只够几秒，自己看着用

依赖: ffmpeg 在 PATH 里。
"""
import argparse, os, struct, subprocess, sys, shutil


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("-o", "--out", default=None)
    ap.add_argument("--size", type=int, default=64, help="正方形边长(64 推荐, 128 也支持)")
    ap.add_argument("--fps", type=int, default=10)
    ap.add_argument("--ss", default=None, help="起始时间，如 00:00:30")
    ap.add_argument("--t", default=None, help="截取时长秒数")
    ap.add_argument("--max-kb", type=int, default=1800, help="输出上限(留点空间给图片)")
    a = ap.parse_args()

    if not shutil.which("ffmpeg"):
        sys.exit("找不到 ffmpeg，先装上并加进 PATH")
    if a.size < 8 or a.size > 128:
        sys.exit("size 只能 8..128")

    out = a.out or (os.path.splitext(os.path.basename(a.src))[0][:12] + ".v64")
    fb = a.size * a.size * 2
    max_frames = min(65535, (a.max_kb * 1024 - 16) // fb)
    if max_frames < 1:
        sys.exit("max-kb 太小")

    cmd = ["ffmpeg", "-hide_banner", "-loglevel", "error"]
    if a.ss:
        cmd += ["-ss", a.ss]
    cmd += ["-i", a.src]
    if a.t:
        cmd += ["-t", str(a.t)]
    cmd += [
        "-vf", "fps=%d,scale=%d:%d:force_original_aspect_ratio=increase,crop=%d:%d"
               % (a.fps, a.size, a.size, a.size, a.size),
        "-pix_fmt", "rgb565le", "-f", "rawvideo", "-",
    ]

    print("ffmpeg 解码中...")
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE)

    frames = 0
    with open(out, "wb") as fh:
        fh.write(b"IV1\x00" + struct.pack("<HHHHI", a.size, a.size, a.fps, 0, 0))
        while frames < max_frames:
            buf = p.stdout.read(fb)
            if len(buf) < fb:
                break
            fh.write(buf)
            frames += 1
        # 回填真实帧数
        fh.seek(10)
        fh.write(struct.pack("<H", frames))
    p.stdout.close()
    p.wait()

    if frames == 0:
        os.remove(out)
        sys.exit("一帧都没解出来，检查输入文件")

    size = 16 + frames * fb
    print("%s : %dx%d %dfps %d 帧 = %.1f 秒, %.0f KB"
          % (out, a.size, a.size, a.fps, frames, frames / a.fps, size / 1024.0))
    if frames == max_frames:
        print("已经到 --max-kb 上限，视频被截短了。想更长就 --size 48 或者降 fps。")
    print("接下来: 手机/电脑打开摆件的网页 -> VIDEO 片段 -> 上传这个文件")


if __name__ == "__main__":
    main()
