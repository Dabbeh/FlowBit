# Analyses a rendered window and reports per-region colours and contrast, so the
# light theme can be judged without viewing the image.
#
# Run: python tools/analyse_shot.py build/tests/shot-light.png
import sys
from collections import Counter

from PIL import Image


def lum(c):
    r, g, b = [v / 255 for v in c[:3]]
    f = lambda u: u / 12.92 if u <= 0.03928 else ((u + 0.055) / 1.055) ** 2.4
    return 0.2126 * f(r) + 0.7152 * f(g) + 0.0722 * f(b)


def ratio(a, b):
    la, lb = lum(a), lum(b)
    hi, lo = max(la, lb), min(la, lb)
    return (hi + 0.05) / (lo + 0.05)


def main(path):
    im = Image.open(path).convert("RGB")
    W, H = im.size
    print(f"image {W}x{H}\n")

    counts = Counter(im.getdata())
    print("=== 12 most common colours ===")
    for c, n in counts.most_common(12):
        pct = 100 * n / (W * H)
        print(f"  {c}  {pct:5.1f}%   luma {lum(c)*100:5.1f}")

    print("\n=== horizontal scan at y=H/4 (card edges as luma steps) ===")
    y = H // 4
    row = [im.getpixel((x, y)) for x in range(W)]
    prev = row[0]
    print(f"  x=0    {prev}")
    for x in range(1, W):
        c = row[x]
        if max(abs(c[i] - prev[i]) for i in range(3)) > 6:
            print(f"  x={x:<5} {c}   (step from {prev})")
        prev = c

    print("\n=== vertical scan at x=W/2 (card top/bottom edges) ===")
    x = W // 2
    col = [im.getpixel((x, y)) for y in range(H)]
    prev = col[0]
    for yy in range(1, H):
        c = col[yy]
        if max(abs(c[i] - prev[i]) for i in range(3)) > 6:
            print(f"  y={yy:<5} {c}   (step from {prev})")
        prev = c

    # Distinct colours: too few means flat, too many means noise.
    print(f"\n=== distinct colours: {len(counts)} ===")

    # How much of the image is pure white, and how much is background?
    bg = counts.most_common(1)[0][0]
    white = sum(n for c, n in counts.items() if min(c) > 250)
    print(f"  most common (page): {bg} at {100*white/(W*H):.0f}% pure white")
    print(f"  background vs white-card contrast: {ratio(bg, (255,255,255)):.2f}:1")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "shot-light.png")
