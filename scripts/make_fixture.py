#!/usr/bin/env python3
"""Generate a deterministic OCR test video.

Renders 20 seconds at 30fps of black 640x480, with:
  - "alpha bravo"   visible from t=2.0s to t=8.0s
  - "charlie delta" visible from t=10.0s to t=16.0s
Frames are written as PNGs then muxed by ffmpeg (no drawtext required).
"""
import subprocess
import sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

OUT_DIR = Path(__file__).resolve().parent.parent / "test_assets"
FRAMES_DIR = OUT_DIR / "_frames"
OUT_VIDEO = OUT_DIR / "fixture_search.mp4"

W, H, FPS, DUR = 640, 480, 30, 20
FONT_CANDIDATES = [
    "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
    "/System/Library/Fonts/Supplemental/Arial.ttf",
    "/Library/Fonts/Arial.ttf",
]


def pick_font(size: int):
    for p in FONT_CANDIDATES:
        if Path(p).exists():
            return ImageFont.truetype(p, size)
    return ImageFont.load_default()


def text_at(t: float):
    if 2.0 <= t < 8.0:
        return "alpha bravo"
    if 10.0 <= t < 16.0:
        return "charlie delta"
    return ""


def main():
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    FRAMES_DIR.mkdir(parents=True, exist_ok=True)
    for p in FRAMES_DIR.glob("*.png"):
        p.unlink()

    font = pick_font(64)
    nframes = FPS * DUR
    for i in range(nframes):
        t = i / FPS
        img = Image.new("RGB", (W, H), (0, 0, 0))
        draw = ImageDraw.Draw(img)
        msg = text_at(t)
        if msg:
            bbox = draw.textbbox((0, 0), msg, font=font)
            tw = bbox[2] - bbox[0]
            th = bbox[3] - bbox[1]
            x = (W - tw) // 2 - bbox[0]
            y = (H - th) // 2 - bbox[1]
            draw.text((x, y), msg, font=font, fill=(255, 255, 255))
        img.save(FRAMES_DIR / f"f{i:05d}.png")

    cmd = [
        "ffmpeg", "-y", "-loglevel", "error",
        "-framerate", str(FPS),
        "-i", str(FRAMES_DIR / "f%05d.png"),
        "-c:v", "libx264", "-pix_fmt", "yuv420p",
        "-preset", "ultrafast",
        str(OUT_VIDEO),
    ]
    subprocess.run(cmd, check=True)

    for p in FRAMES_DIR.glob("*.png"):
        p.unlink()
    FRAMES_DIR.rmdir()

    print(f"Wrote {OUT_VIDEO}")


if __name__ == "__main__":
    sys.exit(main())
