"""Create paired, synthetic X4 Pro books and pixel references. Requires Pillow on the build host."""
import argparse
import io
import json
from pathlib import Path
from shutil import copytree
import struct
import zipfile
import zlib

from PIL import Image, ImageDraw

CONTAINER = ('<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">'
             '<rootfiles><rootfile full-path="book.opf" media-type="application/oebps-package+xml"/>'
             '</rootfiles></container>')
SENTENCE = 'The small red fox crossed the old stone bridge before sunrise.'
NAMESPACE = 'http://www.w3.org/1999/xhtml'


def document(title, body):
    return f'<html xmlns="{NAMESPACE}"><head><title>{title}</title></head><body>{body}</body></html>'


def epub(path, chapters, order, toc, extra=None):
    # Same ZIP member bytes and DOS timestamps in every trial, independent of build date.
    items = {name: (body, 'application/xhtml+xml') for name, body in chapters.items()}
    assets = {k: v for k, v in (extra or {}).items() if k.endswith('.png')}
    items.update({k: (v, 'image/png') for k, v in assets.items()})
    manifest = ''.join(f'<item id="i{n}" href="{k}" media-type="{v[1]}"/>' for n, (k, v) in enumerate(items.items()))
    ids = {name: i for i, name in enumerate(items)}
    spine = ''.join(f'<itemref idref="i{ids[name]}"/>' for name in order)
    title = path.stem
    opf = (f'<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="id">'
           f'<metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:title>{title}</dc:title>'
           f'<dc:identifier id="id">urn:crossink:test:{title}</dc:identifier><dc:language>en</dc:language>'
           '<meta property="dcterms:modified">2026-01-01T00:00:00Z</meta></metadata>'
           f'<manifest><item id="nav" properties="nav" href="nav.xhtml" media-type="application/xhtml+xml"/>'
           f'{manifest}</manifest><spine>{spine}</spine></package>')
    links = ''.join(f'<li><a href="{href}">{label}</a></li>' for label, href in toc)
    nav = document('Contents', f'<nav xmlns:epub="http://www.idpf.org/2007/ops" epub:type="toc"><ol>{links}</ol></nav>')
    entries = {'mimetype': 'application/epub+zip', 'META-INF/container.xml': CONTAINER, 'book.opf': opf,
               'nav.xhtml': nav, **{k: v[0] for k, v in items.items()}, **(extra or {})}
    with zipfile.ZipFile(path, 'w') as out:
        for name, data in entries.items():
            entry = zipfile.ZipInfo(name)
            entry.compress_type = zipfile.ZIP_STORED if name == 'mimetype' or name.startswith('META-INF/crossink/') else zipfile.ZIP_DEFLATED
            out.writestr(entry, data)


def dense(path, count):
    # Last spine position deliberately repeats chapter 002. The nav visits chapters in reverse.
    chapters, toc = {}, []
    for i in range(1, count):
        title = f'Chapter {i:03d}'
        body = f'<h1 id="top">{title}</h1><p>CrossInk chapter marker {i:03d}.</p>'
        if i in (1, 2):
            body += f'<p>{SENTENCE}</p>' + ''.join(f'<p>Passage {n:03d}. {SENTENCE} {SENTENCE}</p>' for n in range(1, 65))
            body += '<h2 id="end">Copper lantern</h2><p>The copper lantern marks the far anchor.</p>'
        name = f'c{i:03d}.xhtml'
        chapters[name] = document(title, body)
        toc.append((title, name + '#top'))
    nav = [('Far anchor', 'c002.xhtml#end'), ('Near anchor', 'c002.xhtml#top'), *reversed(toc)]
    epub(path, chapters, [*chapters, 'c002.xhtml'], nav)


def chart(size, kind=0, label=''):
    w, h = size
    im = Image.new('L', size, 255)
    d = ImageDraw.Draw(im)
    # Permanent calibration areas at x=.1 and x=.9, y=.12. Sample central
    # left and right at (.25,.52) and (.75,.52) on the ghosting pair.
    d.rectangle((w//18, h//12, w//6, h//6), fill=0)
    d.rectangle((5*w//6, h//12, 17*w//18, h//6), outline=0)
    d.text((w//4, h//12), label, fill=0, font_size=18)
    if kind == 0:
        for i, tone in enumerate([0, 85, 170, 255]):
            d.rectangle((i*w//4, h//3, (i+1)*w//4-1, 2*h//3), fill=tone)
        for i in range(1, 8):
            d.line((w//10, 7*h//10 + i*3, 9*w//10, 7*h//10 + i*3), fill=0, width=i%3+1)
    if kind == 1:
        d.rectangle((w//10, h//3, 2*w//5, 2*h//3), fill=0)
    d.rectangle((0, 0, w-1, h-1), outline=0, width=2)
    for x,y in ((3,3), (w-14,h-14)):
        d.rectangle((x,y,x+9,y+9), fill=0)
    return im


def xtc(path, pages, gray):
    page_data = []
    for im in pages:
        w,h = im.size
        if gray:
            # XTH source 0=white, 1=dark gray, 2=light gray, 3=black.
            code = {255:0, 170:2, 85:1, 0:3}
            pix = im.point(lambda v: min(255, round(v/85)*85)).load()
            planes = [bytearray(), bytearray()]
            for x in reversed(range(w)):
                for y in range(0,h,8):
                    val = [0,0]
                    for b in range(min(8,h-y)):
                        shade = code[pix[x,y+b]]
                        val[0] |= (shade >> 1) << (7-b)
                        val[1] |= (shade & 1) << (7-b)
                    for p in range(2):
                        planes[p].append(val[p])
            payload = planes[0] + planes[1]
        else:
            payload = im.convert('1', dither=Image.Dither.NONE).tobytes()
        header = struct.pack('<4sHHBBIQ', b'XTH\0' if gray else b'XTG\0', w,h,0,0,len(payload),0)
        page_data.append((w,h,header+payload))
    offset = 56+16*len(pages)
    header = struct.pack('<4sBBHBBBBIQQQQII', b'XTCH' if gray else b'XTC\0',1,0,len(pages),0,0,0,0,0,0,56,offset,0,0,0)
    table = bytearray()
    for w,h,page in page_data:
        table += struct.pack('<QIHH',offset,len(page),w,h)
        offset += len(page)
    path.write_bytes(header+table+b''.join(p for _,_,p in page_data))


def optimized(path, im, valid):
    w,h=im.size
    pixels=im.point(lambda v: min(3, round(v/85))).load()
    raw=bytearray()
    for y in range(h):
        for x in range(0,w,4):
            raw.append(sum(pixels[x+i,y]<<(6-2*i) for i in range(min(4,w-x))))
    blocks=bytearray()
    for i,start in enumerate(range(0,len(raw),2048)):
        part=raw[start:start+2048]
        encoder=zlib.compressobj(8,wbits=-15)
        packed=encoder.compress(part)+encoder.flush()
        codec=1 if len(packed)<len(part) else 0
        if not codec:
            packed=part
        blocks+=struct.pack('<BBHHHI',codec,0,len(part),len(packed),i,zlib.crc32(part))+packed
    crc=zlib.crc32(raw)
    data=struct.pack('<4sBB7H3I',b'PXC2',2,1,32,w,h,(w+3)//4,2048,(len(raw)+2047)//2048,0,len(raw),crc,32+len(blocks))+blocks
    if not valid:
        data=data[:-1]+bytes([data[-1]^255])
    name='META-INF/crossink/pxc/chart.pxc2'
    record=dict(href='chart.png',pxc=name,width=w,height=h,pxcFormat='pxc2',pxcBytes=len(data),pixelCrc32=crc)
    buf=io.BytesIO()
    im.save(buf,'PNG')
    extra={'chart.png':buf.getvalue(), name:data, 'META-INF/crossink/optimizer-v1.json':json.dumps(dict(format='crossink-optimizer',version=1,images=[record]))}
    name='image.xhtml'
    epub(path,{name:document('Chart','<p>Chart of four tones</p><img src="chart.png" alt="Four tone chart"/>')},[name],[('Chart',name)],extra)


def generate(root):
    books=root/'SD card'/'CrossInk-Test'/'Books'
    refs=root/'Reference images'
    refs.mkdir(parents=True,exist_ok=True)
    trial=books/'A'/'Trial-1'
    trial.mkdir(parents=True)
    lines=[f'Line {i:04d}. {SENTENCE} Word spacing stays visible between neighboring words.' for i in range(1,1001)]
    (trial/'Numbered.txt').write_bytes(('\r\n'.join(lines)).encode())
    for n in [299,300]:
        dense(trial/f'Spine-{n}.epub',n)
    for i in (2,3):
        copytree(trial,trial.parent/f'Trial-{i}')
    visual=books/'A'/'Visual'
    visual.mkdir()
    (visual/'Boundaries.txt').write_bytes(('Start fi fl ffi AV Wa.\r\n\r\nCafé cafe\u0301 — 東京 — مرحبا 123 — שָׁלוֹם.\r\n'
       +(' ' * 16)+'Indented line.\r\n'+('Longword' * 1250)+'\r\nAFTER THE LONG WORD\r\n'+
       'Last anchored paragraph. No terminal newline.').encode())
    (visual/'Empty.txt').write_bytes(b'')
    # Three-byte zero-width selectors cross the 8 KiB rolling buffer boundary.
    (visual/'Utf8-window.txt').write_bytes(('\ufe0f' * 3000 + '\r\nWINDOW END / COMPLETE.').encode())
    styled = (
        '<h1>Type and rules</h1><p>The small <em>red</em> fox crossed the <strong>old</strong> stone '
        'bridge before sunrise.</p><hr/><p><u>Underline</u> and an em dash — then office, affinity, '
        'efficient, AV and Wa.</p><p>soft&#173;hyphen café cafe&#769; and non&#160;breaking space.</p>'
        '<p>Japanese ruby: <ruby>海<rt>うみ</rt></ruby>. Hebrew: שלום 123. Arabic: مرحبا 123.</p>'
        '<ol><li>First item ends here.</li><li>Second item starts here.</li></ol>'
        '<blockquote>A left border or indentation can mark this passage.</blockquote>'
    )
    styled += ''.join(f'<p>Passage {i:03d}. {SENTENCE} {SENTENCE}</p>' for i in range(1,45))
    epub(visual/'Typography.epub', {'type.xhtml': document('Type and rules', styled)}, ['type.xhtml'],
         [('Type and rules', 'type.xhtml')])

    chart_full=chart((480,800),label='PAGE 1 / FOUR TONES')
    fill=chart((480,800),1,'PAGE 2 / INK')
    clear=chart((480,800),2,'PAGE 3 / CLEAR')
    odd=chart((319,417),label='PAGE 4 / SMALL')
    end=chart((480,800),label='PAGE 5 / FULL AGAIN')
    # Quantize text antialiasing too so that the ink and clear pages are genuinely binary.
    fill=fill.point(lambda p: 255 if p>=128 else 0)
    clear=clear.point(lambda p: 255 if p>=128 else 0)
    pages=[chart_full,fill,clear,odd,end]
    for i,im in enumerate(pages,1):
        im.point(lambda v: round(v/85)*85).save(refs/f'Panel-page-{i}.png')
    xtc(visual/'Panel.xtch',pages,True)
    xtc(visual/'Mixed-mono.xtc',[chart((480,800),2,'PAGE 1 / FULL'),chart((319,417),1,'PAGE 2 / SMALL'),chart((480,800),label='PAGE 3 / FULL AGAIN')],False)
    damaged=bytearray((visual/'Mixed-mono.xtc').read_bytes())
    pos,=struct.unpack_from('<Q',damaged,56+16)
    struct.pack_into('<H',damaged,pos+4,311)  # Interior header disagrees with table, file itself is intact.
    (visual/'Damaged-page-2.xtc').write_bytes(damaged)
    pix=chart((319,257),label='ODD WIDTH')
    pix=pix.point(lambda v: round(v/85)*85)
    pix.save(refs/'Optimized-chart.png')
    for name,ok in (('Optimized',True),('Damaged-sidecar',False)):
        optimized(visual/f'{name}.epub',pix,ok)
    # Firmware-native 2-bit BMP, including an incomplete last byte and BMP row padding.
    w,h=pix.size
    stride=((w+3)//4+3)&~3
    raster=bytearray()
    for y in reversed(range(h)):
        row=bytearray(stride)
        for x in range(w):
            row[x//4] |= (pix.getpixel((x,y))//85)<<(6-2*(x%4))
        raster+=row
    palette=b''.join(bytes([n,n,n,0]) for n in [0,85,170,255])
    ofs=54+len(palette)
    (visual/'Palette.bmp').write_bytes(struct.pack('<2sIHHI',b'BM',ofs+len(raster),0,0,ofs)+
        struct.pack('<IiiHHIIiiII',40,w,h,1,2,0,len(raster),0,0,4,0)+palette+raster)
    true=chart((1103,859),label='TRUECOLOR / SCALED').convert('RGB')
    gradient=Image.linear_gradient('L').resize((300,160)).rotate(90,expand=True)
    true.paste(gradient,(290,130))
    true.save(visual/'Truecolor.bmp')
    true.thumbnail((480,800))
    true.save(refs/'Truecolor.png')
    copytree(books/'A',books/'B')
    (books.parent/'X4PRO-INDEX-RESULTS.txt').write_text('build,operation,path,position_zero_based,elapsed_us\n')


if __name__ == '__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('destination',type=Path)
    generate(p.parse_args().destination)
