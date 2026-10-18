"""Make the LiveArea images of the PS Vita package from the game's artwork.

Takes icon.jpg (the bubble), livearea/contents/generals_bg.png (the background)
and livearea/contents/generals_startup.jpg (the logo on white) from this folder
and writes icon0.png, livearea/contents/bg.png and livearea/contents/startup.png
as 8 bit palette PNG files in the sizes the Vita accepts. The white around the
logo is made transparent, so that the gate image shows the background through
it; white areas the letters enclose are taken out too, the light letters stay.

make_livearea.py draws neutral placeholder images instead, for a repository that
is to contain nothing taken from the game.

    python convert_artwork.py
"""

import os
from collections import deque

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))

WHITE = 245             # a pixel this light in every channel can belong to the white background
SMALL_HOLE = 20         # enclosed light areas smaller than this are highlights of the letters
OPAQUE_BELOW = 200      # edge pixels with a channel this dark stay opaque
EDGE = 2                # width of the band along the background in which edges fade out


def to_palette(image):
    """256 colors, with the transparency in the palette if the image has an alpha channel."""
    if image.mode == 'RGBA':
        return image.quantize(colors=256, method=Image.FASTOCTREE)
    palette = image.quantize(colors=256, method=Image.MEDIANCUT)
    return image.quantize(palette=palette, dither=Image.FLOYDSTEINBERG)


def background_mask(image):
    """True for the white around the logo and for white areas the letters enclose."""
    width, height = image.size
    pixels = image.load()
    mask = [[False] * width for _ in range(height)]
    seen = [[False] * width for _ in range(height)]
    for y in range(height):
        for x in range(width):
            if seen[y][x] or min(pixels[x, y]) < WHITE:
                continue
            queue = deque([(x, y)])
            seen[y][x] = True
            area = []
            touches_border = False
            while queue:
                cx, cy = queue.popleft()
                area.append((cx, cy))
                if cx in (0, width - 1) or cy in (0, height - 1):
                    touches_border = True
                for nx, ny in ((cx + 1, cy), (cx - 1, cy), (cx, cy + 1), (cx, cy - 1)):
                    if 0 <= nx < width and 0 <= ny < height and not seen[ny][nx] and min(pixels[nx, ny]) >= WHITE:
                        seen[ny][nx] = True
                        queue.append((nx, ny))
            if touches_border or len(area) >= SMALL_HOLE:
                for cx, cy in area:
                    mask[cy][cx] = True
    return mask


def cut_out(image):
    """The logo with its white background transparent and its edges faded into it."""
    width, height = image.size
    mask = background_mask(image)
    pixels = image.load()
    result = Image.new('RGBA', image.size)
    out = result.load()
    for y in range(height):
        for x in range(width):
            r, g, b = pixels[x, y]
            if mask[y][x]:
                out[x, y] = (0, 0, 0, 0)
                continue
            near_background = any(
                mask[ny][nx]
                for ny in range(max(0, y - EDGE), min(height, y + EDGE + 1))
                for nx in range(max(0, x - EDGE), min(width, x + EDGE + 1)))
            alpha = 1.0
            if near_background:
                alpha = min(1.0, max(0.0, (255 - min(r, g, b)) / (255.0 - OPAQUE_BELOW)))
            if alpha <= 0.0:
                out[x, y] = (0, 0, 0, 0)
                continue
            # The color the pixel had before it was blended over white.
            unblend = [min(255, max(0, round((c - (1.0 - alpha) * 255) / alpha))) for c in (r, g, b)]
            out[x, y] = (unblend[0], unblend[1], unblend[2], round(alpha * 255))
    return result


def fit(logo, width, height, margin):
    """The visible part of the logo, as large as fits with a margin, centered on transparency."""
    logo = logo.crop(logo.getbbox())
    scale = min((width - 2 * margin) / logo.width, (height - 2 * margin) / logo.height)
    size = (round(logo.width * scale), round(logo.height * scale))
    # Scaled with premultiplied alpha, so that transparent pixels do not darken the edges.
    scaled = logo.convert('RGBa').resize(size, Image.LANCZOS).convert('RGBA')
    canvas = Image.new('RGBA', (width, height), (0, 0, 0, 0))
    canvas.paste(scaled, ((width - size[0]) // 2, (height - size[1]) // 2))
    return canvas


def main():
    contents = os.path.join(HERE, 'livearea', 'contents')

    icon = Image.open(os.path.join(HERE, 'icon.jpg')).convert('RGB').resize((128, 128), Image.LANCZOS)
    to_palette(icon).save(os.path.join(HERE, 'icon0.png'), optimize=True)

    background = Image.open(os.path.join(contents, 'generals_bg.png')).convert('RGB')
    background = background.resize((840, 500), Image.LANCZOS)
    to_palette(background).save(os.path.join(contents, 'bg.png'), optimize=True)

    logo = cut_out(Image.open(os.path.join(contents, 'generals_startup.jpg')).convert('RGB'))
    to_palette(fit(logo, 280, 158, 6)).save(os.path.join(contents, 'startup.png'), optimize=True)
    print('wrote icon0.png, livearea/contents/bg.png and livearea/contents/startup.png')


if __name__ == '__main__':
    main()
