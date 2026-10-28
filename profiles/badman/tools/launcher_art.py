"""Prepare launcher background art from the player's own game, locally.

title: takes the game's own 480x272 title-screen capture (BADMAN_DUMP_FRAME BMP
or PNG), or a larger redraw of it, rebuilds the blue ray background under the
subtitle line and the in-game menu row, and writes a PNG at the input size. The
launcher draws its own subtitle and controls over it; native art scales with
pixel-exact sampling, larger art with smooth filtering.

screenshot: recovers the native 480x272 frame from an integer nearest-neighbour
window capture (for example a 5x capture inside letterbox bars).

Outputs stay in the player's local art folder; no game art is distributed.
Pure Python (no imaging library) so it runs in the launcher's bundled Python.
"""
import argparse
import struct
import sys
import zlib
from pathlib import Path

WIDTH, HEIGHT = 480, 272


class ArtError(Exception):
    pass


# ---------------------------------------------------------------- image I/O
def read_bmp(data):
    if data[:2] != b'BM':
        raise ArtError('Not a BMP image')
    offset = struct.unpack_from('<I', data, 10)[0]
    width, height = struct.unpack_from('<ii', data, 18)
    bits, compression = struct.unpack_from('<HI', data, 28)
    if bits not in (24, 32) or compression not in (0, 3) or width <= 0 or height == 0:
        raise ArtError('Unsupported BMP format')
    stride = (width * bits // 8 + 3) & ~3
    rows = []
    for row in range(abs(height)):
        source = row if height < 0 else abs(height) - 1 - row
        start = offset + source * stride
        line = data[start:start + width * bits // 8]
        step = bits // 8
        rows.append([(line[x * step + 2], line[x * step + 1], line[x * step]) for x in range(width)])
    return width, abs(height), rows


def read_png(data):
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ArtError('Not a PNG image')
    at, idat, header = 8, bytearray(), None
    while at < len(data):
        length, kind = struct.unpack_from('>I4s', data, at)
        body = data[at + 8:at + 8 + length]
        at += 12 + length
        if kind == b'IHDR':
            header = struct.unpack('>IIBBBBB', body)
        elif kind == b'IDAT':
            idat += body
        elif kind == b'IEND':
            break
    if not header:
        raise ArtError('PNG has no header')
    width, height, depth, color, _, _, interlace = header
    channels = {2: 3, 6: 4}.get(color)
    if depth != 8 or channels is None or interlace:
        raise ArtError('Only 8-bit RGB/RGBA non-interlaced PNG is supported')
    raw = zlib.decompress(bytes(idat))
    stride, rows, previous = width * channels, [], bytearray(width * channels)
    for row in range(height):
        start = row * (stride + 1)
        kind, line = raw[start], bytearray(raw[start + 1:start + 1 + stride])
        if kind == 1:
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 255
        elif kind == 2:
            for i in range(stride):
                line[i] = (line[i] + previous[i]) & 255
        elif kind == 3:
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + ((left + previous[i]) >> 1)) & 255
        elif kind == 4:
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                b = previous[i]
                c = previous[i - channels] if i >= channels else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        elif kind != 0:
            raise ArtError('Invalid PNG filter')
        rows.append([tuple(line[x * channels:x * channels + 3]) for x in range(width)])
        previous = line
    return width, height, rows


def read_image(path):
    data = Path(path).read_bytes()
    return read_png(data) if data[:4] == b'\x89PNG' else read_bmp(data)


def write_png(path, rows):
    height, width = len(rows), len(rows[0])
    raw = b''.join(b'\0' + bytes(value for pixel in row for value in pixel) for row in rows)
    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body) & 0xffffffff)
    png = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)) +
           chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))
    Path(path).write_bytes(png)


# ---------------------------------------------------------------- title art
def light(pixel):
    return pixel[2] > 150


def edges(row, left, right, min_run=1):
    """Light/dark boundaries; runs shorter than min_run (upscaling noise) are ignored."""
    found, current, x = [], light(row[left]), left
    while x < right - 1:
        if light(row[x + 1]) != current:
            run = 1
            while run < min_run and x + 1 + run < right and light(row[x + 1 + run]) != current:
                run += 1
            if run >= min_run or x + 1 + run >= right:
                found.append(x)
                current = not current
                x += run
                continue
        x += 1
    return found


def rebuild_band(rows, top, bottom, left, right, min_run=1, blend=False):
    """Interpolate straight two-tone ray edges between clean anchor rows.

    Each rebuilt pixel copies the colour found at the proportional position of
    the same edge segment on the nearer anchor row, so regions keep their own
    shades instead of one flat colour. With blend, each ray segment instead takes
    its median colour on both anchor rows, mixed by height, so the noise and soft
    edges of a high-resolution redraw are not stretched into streaks.
    """
    upper, lower = edges(rows[top], left, right, min_run), edges(rows[bottom], left, right, min_run)
    if len(upper) != len(lower) or light(rows[top][left]) != light(rows[bottom][left]):
        raise ArtError('Title background edges could not be matched; capture is not the expected title screen')
    top_bounds = [left] + [x + 1 for x in upper] + [right]
    bottom_bounds = [left] + [x + 1 for x in lower] + [right]
    source_top, source_bottom = [list(rows[top]), list(rows[bottom])]
    def medians(source, bounds):
        result = []
        for begin, finish in zip(bounds, bounds[1:]):
            inner = source[begin + min_run:finish - min_run] or source[begin:finish] or [source[min(begin, len(source) - 1)]]
            result.append(tuple(sorted(pixel[c] for pixel in inner)[len(inner) // 2] for c in range(3)))
        return result
    if blend:
        top_colours, bottom_colours = medians(source_top, top_bounds), medians(source_bottom, bottom_bounds)
    for y in range(top + 1, bottom):
        t = (y - top) / (bottom - top)
        bounds = [round(a + (b - a) * t) for a, b in zip(top_bounds, bottom_bounds)]
        anchor, anchor_bounds = (source_top, top_bounds) if t < 0.5 else (source_bottom, bottom_bounds)
        for segment in range(len(bounds) - 1):
            begin, finish = bounds[segment], bounds[segment + 1]
            a_begin, a_finish = anchor_bounds[segment], anchor_bounds[segment + 1]
            if blend:
                colour = tuple(round(u + (v - u) * t) for u, v in zip(top_colours[segment], bottom_colours[segment]))
                rows[y][begin:finish] = [colour] * (finish - begin)
                continue
            for x in range(begin, finish):
                f = (x - begin + 0.5) / max(1, finish - begin)
                ax = min(a_finish - 1, max(a_begin, int(a_begin + f * (a_finish - a_begin))))
                rows[y][x] = anchor[ax]


def white(pixel):
    return sum(pixel) > 700


def matched_anchors(rows, top, bottom, left, right, min_run, reach):
    """Nearest text-free rows around the white text whose ray edges correspond.

    A redraw is not an exact multiple of the native grid, so the fixed native
    anchor rows can land inside text or across a ray edge. Anchors stay two
    game pixels (2 * min_run) clear of the text, past its soft edge and shadow.
    """
    text = [y for y in range(top, bottom + 1) if any(white(rows[y][x]) for x in range(left, right))]
    if not text:
        raise ArtError('Title text row was not found')
    def clean(y):  # brighter than the light rays means text or its soft edge
        return 0 <= y < len(rows) and not any(sum(rows[y][x]) > 450 for x in range(left, right))
    def shape(y):
        return light(rows[y][left]), len(edges(rows[y], left, right, min_run))
    for distance in range(2 * reach):
        for up in range(distance + 1):
            above, below = text[0] - 2 * min_run - up, text[-1] + 2 * min_run + distance - up
            if clean(above) and clean(below) and shape(above) == shape(below):
                return above, below
    raise ArtError('Title background edges could not be matched; image is not the expected title screen')


def title_art(source, output):
    """Accept the native capture or a higher-resolution redraw of the same title.

    A redraw may omit the copyright bar (480x240 shape, e.g. 1441x720); the
    launcher fits either shape to the window by its own size.
    """
    width, height, rows = read_image(source)
    scale = width / WIDTH
    if scale < 1 or min(abs(height / scale - shape) for shape in (HEIGHT, 240)) > 2:
        raise ArtError(f'Expected the {WIDTH}x{HEIGHT} title capture or a larger redraw of it, got {width}x{height}')
    def at(value):
        return round(value * scale)
    count = sum(1 for y in range(at(157), at(169)) for x in range(at(155), at(447)) if white(rows[y][x]))
    if count < 800 * scale * scale:
        raise ArtError('This image does not show the title screen subtitle')
    noise = max(1, round(scale))  # ignore light/dark specks smaller than one game pixel
    # "What Did I Do to Deserve This?", then Start / Almanac / Sound / Data and the cursor.
    for top, bottom, left in ((151, 172, at(140)), (181, 199, 0)):
        if scale != 1:
            top, bottom = matched_anchors(rows, at(top), at(bottom), left, width, noise, 4 * noise)
        rebuild_band(rows, top, bottom, left, width, noise, blend=scale != 1)
    write_png(output, rows)


def screenshot_art(source, output):
    width, height, rows = read_image(source)
    scale = min(width // WIDTH, height // HEIGHT)
    if scale < 1:
        raise ArtError('Screenshot is smaller than the native frame')
    left, top = (width - WIDTH * scale) // 2, (height - HEIGHT * scale) // 2
    # Snap to the actual pixel-cell grid using colour changes along a row/column.
    def phase(values):
        counts = [0] * scale
        for i in range(1, len(values)):
            if values[i] != values[i - 1]:
                counts[i % scale] += 1
        return max(range(scale), key=counts.__getitem__)
    middle_row, middle_col = rows[height // 2], [rows[y][width // 2] for y in range(height)]
    left += (phase(middle_row) - left) % scale
    top += (phase(middle_col) - top) % scale
    if left + WIDTH * scale > width:
        left -= scale
    if top + HEIGHT * scale > height:
        top -= scale
    native = [[rows[top + y * scale + scale // 2][left + x * scale + scale // 2] for x in range(WIDTH)] for y in range(HEIGHT)]
    write_png(output, native)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=('title', 'screenshot'))
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    try:
        (title_art if args.mode == 'title' else screenshot_art)(args.source, args.output)
        print(f'Wrote {args.output}')
    except (ArtError, OSError, struct.error, zlib.error) as error:
        print(f'Launcher art failed: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
