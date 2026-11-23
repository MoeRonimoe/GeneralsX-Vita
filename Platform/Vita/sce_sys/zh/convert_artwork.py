"""Make the LiveArea images of the PS Vita package of Zero Hour from the game's artwork.

Takes the installed game's GeneralsZH.ico (the bubble), Install_Final.bmp (the
background) and launcher.bmp (the logo of the start gate) and writes icon0.png,
livearea/contents/bg.png and livearea/contents/startup.png next to this script, as
8 bit palette PNG files in the sizes the Vita accepts (see ../make_livearea.py).

    python convert_artwork.py <installed Zero Hour directory>

The artwork belongs to Electronic Arts.
"""

import os
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))

ICON_BACKGROUND = (12, 20, 48)		# the dark blue of the box art, behind the emblem


def to_palette(image):
    """256 colors, with the transparency in the palette if the image has an alpha channel."""
    if image.mode == 'RGBA':
        return image.quantize(colors=256, method=Image.FASTOCTREE)
    palette = image.quantize(colors=256, method=Image.MEDIANCUT)
    return image.quantize(palette=palette, dither=Image.FLOYDSTEINBERG)


def cover(image, width, height):
    """The image scaled to fill width x height, the overhanging part cut off in the middle."""
    scale = max(width / image.width, height / image.height)
    scaled = image.resize((round(image.width * scale), round(image.height * scale)), Image.LANCZOS)
    left = (scaled.width - width) // 2
    top = (scaled.height - height) // 2
    return scaled.crop((left, top, left + width, top + height))


def contain(image, width, height, margin, background):
    """The image as large as fits with a margin, centered on the background color."""
    scale = min((width - 2 * margin) / image.width, (height - 2 * margin) / image.height)
    size = (round(image.width * scale), round(image.height * scale))
    canvas = Image.new('RGB', (width, height), background)
    scaled = image.convert('RGBA').resize(size, Image.LANCZOS)
    canvas.paste(scaled, ((width - size[0]) // 2, (height - size[1]) // 2), scaled)
    return canvas


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    game = sys.argv[1]
    contents = os.path.join(HERE, 'livearea', 'contents')
    os.makedirs(contents, exist_ok=True)

    emblem = Image.open(os.path.join(game, 'GeneralsZH.ico')).convert('RGBA')
    icon = contain(emblem, 128, 128, 6, ICON_BACKGROUND)
    to_palette(icon).save(os.path.join(HERE, 'icon0.png'), optimize=True)

    background = cover(Image.open(os.path.join(game, 'Install_Final.bmp')).convert('RGB'), 840, 500)
    to_palette(background).save(os.path.join(contents, 'bg.png'), optimize=True)

    logo = Image.open(os.path.join(game, 'launcher.bmp')).convert('RGB')
    startup = contain(logo, 280, 158, 0, ICON_BACKGROUND)
    to_palette(startup).save(os.path.join(contents, 'startup.png'), optimize=True)
    print('wrote icon0.png, livearea/contents/bg.png and livearea/contents/startup.png')


if __name__ == '__main__':
    main()
