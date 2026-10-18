"""Render the LiveArea images of the PS Vita package.

The Vita only accepts 8 bit palette PNG files for these, in fixed sizes:

    icon0.png                      128 x 128   bubble on the home screen
    livearea/contents/bg.png       840 x 500   LiveArea background
    livearea/contents/startup.png  280 x 158   gate image that starts the game

The artwork is drawn here from scratch (a star on a dark field and the title),
so the repository contains nothing taken from the game. Run this script again
after changing it; the generated files are checked in.

    python make_livearea.py [path to a bold TrueType font]
"""

import math
import os
import sys

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))

BACKGROUND_TOP = (24, 30, 22)
BACKGROUND_BOTTOM = (8, 10, 8)
GOLD = (214, 170, 62)
GOLD_DARK = (120, 88, 24)
TEXT = (232, 226, 205)

DEFAULT_FONTS = (
    r"C:\Windows\Fonts\arialbd.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
)


def find_font():
    if len(sys.argv) > 1:
        return sys.argv[1]
    for path in DEFAULT_FONTS:
        if os.path.exists(path):
            return path
    raise SystemExit("no bold TrueType font found; pass one as the first argument")


def gradient(width, height):
    image = Image.new("RGB", (width, height))
    draw = ImageDraw.Draw(image)
    for y in range(height):
        t = y / max(1, height - 1)
        color = tuple(int(a + (b - a) * t) for a, b in zip(BACKGROUND_TOP, BACKGROUND_BOTTOM))
        draw.line([(0, y), (width, y)], fill=color)
    return image


def star_points(cx, cy, outer, inner):
    points = []
    for i in range(10):
        radius = outer if i % 2 == 0 else inner
        angle = -math.pi / 2 + i * math.pi / 5
        points.append((cx + radius * math.cos(angle), cy + radius * math.sin(angle)))
    return points


def draw_star(draw, cx, cy, size):
    draw.polygon(star_points(cx, cy + size * 0.04, size, size * 0.40), fill=GOLD_DARK)
    draw.polygon(star_points(cx, cy, size * 0.94, size * 0.37), fill=GOLD)


def centered_text(draw, text, font, cx, y, fill):
    left, top, right, bottom = draw.textbbox((0, 0), text, font=font)
    draw.text((cx - (right - left) / 2 - left, y - top), text, font=font, fill=fill)
    return bottom - top


def save(image, relative_path):
    path = os.path.join(HERE, relative_path)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    # The Vita rejects true color images here.
    image.convert("P", palette=Image.ADAPTIVE, colors=255).save(path, optimize=True)
    print("wrote", relative_path)


def main():
    font_path = find_font()

    icon = gradient(128, 128)
    draw = ImageDraw.Draw(icon)
    draw_star(draw, 64, 50, 40)
    centered_text(draw, "GENERALS", ImageFont.truetype(font_path, 19), 64, 98, TEXT)
    save(icon, "icon0.png")

    background = gradient(840, 500)
    draw = ImageDraw.Draw(background)
    draw_star(draw, 420, 190, 130)
    height = centered_text(draw, "GENERALS", ImageFont.truetype(font_path, 72), 420, 340, TEXT)
    centered_text(draw, "PS Vita port of the open source game engine", ImageFont.truetype(font_path, 22), 420, 350 + height, GOLD)
    save(background, os.path.join("livearea", "contents", "bg.png"))

    startup = gradient(280, 158)
    draw = ImageDraw.Draw(startup)
    draw_star(draw, 140, 58, 44)
    centered_text(draw, "START", ImageFont.truetype(font_path, 26), 140, 114, TEXT)
    save(startup, os.path.join("livearea", "contents", "startup.png"))


if __name__ == "__main__":
    main()
