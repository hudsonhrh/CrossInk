"""Make a four-page EPUB optical crossover fixture. Requires Pillow."""
from io import BytesIO
from pathlib import Path
import argparse
import random
import zipfile

from PIL import Image, ImageDraw

COLS, ROWS = 8, 10


def tile_signs():
    # Equal black and white area, balanced in both directions within each 2x2.
    rng = random.Random(240917)
    return [rng.choice((-1, 1)) for _ in range(COLS * ROWS // 4)]


def pattern(polarity):
    cell = 30
    image = Image.new("L", (COLS * cell, ROWS * cell), 255)
    draw = ImageDraw.Draw(image)
    for i, sign in enumerate(tile_signs()):
        for dy in range(2):
            for dx in range(2):
                x = ((i % (COLS // 2)) * 2 + dx) * cell
                y = ((i // (COLS // 2)) * 2 + dy) * cell
                white = sign * polarity * (-1) ** (dx + dy) > 0
                draw.rectangle((x, y, x + cell - 1, y + cell - 1), fill=255 if white else 0)
    # A narrow fixed border makes the outer edges of the entire grid visible.
    frame = Image.new("L", (image.width + 4, image.height + 4), 0)
    frame.paste(image, (2, 2))
    out = BytesIO()
    frame.convert("1").save(out, "PNG")
    return out.getvalue()


def make(path):
    pages = ["Pattern A", "Reading page", "Pattern B", "Reading page"]
    manifest = []
    spine = []
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as z:
        z.writestr("mimetype", "application/epub+zip", compress_type=zipfile.ZIP_STORED)
        z.writestr("META-INF/container.xml", """<container xmlns="urn:oasis:names:tc:opendocument:xmlns:container" version="1.0"><rootfiles><rootfile full-path="book.opf" media-type="application/oebps-package+xml"/></rootfiles></container>""")
        nav = []
        for i, title in enumerate(pages):
            if i % 2 == 0:
                letter = "a" if i == 0 else "b"
                img = f"{letter}.png"
                z.writestr(img, pattern(1 if i == 0 else -1))
                manifest.append(f'<item id="img{i}" href="{img}" media-type="image/png"/>')
                content = f'<p>{title}</p><img src="{img}" alt="{title}"/>'
            else:
                content = """<p>Reading page</p><p>At the river bend the path turned east. Sunlight fell across the water. A heron folded its wings and settled among the reeds.</p>"""
            n = f"page{i+1}.xhtml"
            z.writestr(n, f'<html xmlns="http://www.w3.org/1999/xhtml"><head><title>{title}</title></head><body>{content}</body></html>')
            manifest.append(f'<item id="p{i}" href="{n}" media-type="application/xhtml+xml"/>')
            spine.append(f'<itemref idref="p{i}"/>')
            nav.append(f'<navPoint id="n{i}" playOrder="{i+1}"><navLabel><text>{title}</text></navLabel><content src="{n}"/></navPoint>')
        z.writestr("nav.ncx", '<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1"><head><meta name="dtb:uid" content="urn:crossink:optical-1"/></head><docTitle><text>Ghosting check</text></docTitle><navMap>' + ''.join(nav) + '</navMap></ncx>')
        z.writestr("book.opf", '<package xmlns="http://www.idpf.org/2007/opf" unique-identifier="id" version="2.0"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:title>Ghosting check</dc:title><dc:language>en</dc:language><dc:identifier id="id">urn:crossink:optical-1</dc:identifier></metadata><manifest><item id="nav" href="nav.ncx" media-type="application/x-dtbncx+xml"/>' + ''.join(manifest) + '</manifest><spine toc="nav">' + ''.join(spine) + '</spine></package>')


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    make(parser.parse_args().output)
