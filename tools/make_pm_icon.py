"""Draws the icon of LS Project Manager (the board page, tools/dev.py serve): a dark rounded tile with three board columns of cards, the middle one lit.
Writes tools/ls-project-manager.ico (16..256, each size drawn on its own).

    python tools/make_pm_icon.py
"""
import os

from PIL import Image, ImageDraw

BG = (28, 31, 38)
COLS = [(96, 165, 250), (167, 139, 250), (74, 222, 128)]   # doing / next / done


def draw(n):
    s = 4                       # drawn at 4x and shrunk, for smooth edges
    N = n * s
    im = Image.new('RGBA', (N, N), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([0, 0, N - 1, N - 1], radius=N * 0.2, fill=BG)
    pad = N * (0.14 if n >= 32 else 0.10)
    gap = N * (0.07 if n >= 32 else 0.06)
    w = (N - 2 * pad - 2 * gap) / 3
    cards = [3, 2, 1] if n >= 32 else [2, 2, 1]
    for c in range(3):
        x0 = pad + c * (w + gap)
        d.rounded_rectangle([x0, pad, x0 + w, pad + N * 0.07], radius=N * 0.03, fill=COLS[c])   # column head
        ch = (N - 2 * pad - N * 0.07 - N * 0.05) / max(cards[c] + (1 if c == 0 else 0), 3) - gap * 0.6
        y = pad + N * 0.07 + N * 0.05
        for k in range(cards[c]):
            d.rounded_rectangle([x0, y, x0 + w, y + ch], radius=N * 0.03, fill=tuple(int(v * 0.45 + 255 * 0.08) for v in COLS[c]))
            y += ch + gap * 0.6
    return im.resize((n, n), Image.LANCZOS)


if __name__ == '__main__':
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'ls-project-manager.ico')
    sizes = [16, 24, 32, 48, 64, 128, 256]
    imgs = [draw(n) for n in sizes]
    imgs[-1].save(out, format='ICO', sizes=[(n, n) for n in sizes], append_images=imgs[:-1])
    imgs[-1].save(out.replace('.ico', '.png'))
    print(out)
