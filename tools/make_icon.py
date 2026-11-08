"""Generates res/app.ico: amber disc with three dark equalizer bars. Needs only the standard library."""
import math
import struct
import sys

AMBER = (245, 166, 35)
DARK = (20, 21, 24)


def pixel(x, y, size):
    # 4x supersampling for smooth edges.
    r = g = b = a = 0.0
    for sx in range(4):
        for sy in range(4):
            u = (x + (sx + 0.5) / 4) / size * 2 - 1
            v = (y + (sy + 0.5) / 4) / size * 2 - 1
            if math.hypot(u, v) > 0.97:
                continue
            col = AMBER
            for cx, h in ((-0.42, 0.28), (0.0, 0.58), (0.42, 0.40)):
                if abs(u - cx) < 0.13 and abs(v) < h:
                    col = DARK
            r += col[0]; g += col[1]; b += col[2]; a += 1
    if a == 0:
        return (0, 0, 0, 0)
    return (int(b / a), int(g / a), int(r / a), int(a / 16 * 255))


def image(size):
    rows = []
    for y in range(size - 1, -1, -1):  # DIB rows run bottom-up
        rows.append(b"".join(struct.pack("4B", *pixel(x, y, size)) for x in range(size)))
    mask = b"\0" * (((size + 31) // 32) * 4 * size)
    header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    return header + b"".join(rows) + mask


def main(path):
    sizes = [16, 20, 24, 32, 48, 256]
    images = [image(s) for s in sizes]
    out = struct.pack("<HHH", 0, 1, len(sizes))
    offset = 6 + 16 * len(sizes)
    for s, img in zip(sizes, images):
        out += struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32, len(img), offset)
        offset += len(img)
    with open(path, "wb") as fh:
        fh.write(out + b"".join(images))


if __name__ == "__main__":
    main(sys.argv[1])
