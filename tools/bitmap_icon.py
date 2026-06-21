#!/usr/bin/env python3
"""Convert an image into target-size monochrome icon bitmap data."""

from __future__ import annotations

import argparse
import json
import math
import re
import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError as exc:  # pragma: no cover - depends on local environment
    raise SystemExit(
        "Pillow is required to read image files. Install it with:\n"
        "  python3 -m pip install Pillow"
    ) from exc


SIZE_RE = re.compile(r"^(\d+)[xX](\d+)$")
NAME_RE = re.compile(r"[^0-9A-Za-z_]")


def parse_size(value: str) -> tuple[int, int]:
    match = SIZE_RE.match(value.strip())
    if not match:
        raise argparse.ArgumentTypeError("size must look like WIDTHxHEIGHT, for example 32x32")

    width, height = int(match.group(1)), int(match.group(2))
    if width <= 0 or height <= 0:
        raise argparse.ArgumentTypeError("width and height must be positive")
    return width, height


def parse_color(value: str) -> tuple[int, int, int]:
    value = value.strip().lower()
    named = {
        "white": (255, 255, 255),
        "black": (0, 0, 0),
    }
    if value in named:
        return named[value]

    if value.startswith("#") and len(value) == 7:
        try:
            return tuple(int(value[i : i + 2], 16) for i in (1, 3, 5))  # type: ignore[return-value]
        except ValueError as exc:
            raise argparse.ArgumentTypeError("background hex color must look like #ffffff") from exc

    parts = value.split(",")
    if len(parts) == 3:
        try:
            rgb = tuple(int(part) for part in parts)
        except ValueError as exc:
            raise argparse.ArgumentTypeError("background RGB color must look like 255,255,255") from exc
        if all(0 <= part <= 255 for part in rgb):
            return rgb  # type: ignore[return-value]

    raise argparse.ArgumentTypeError("background must be white, black, #rrggbb, or r,g,b")


def c_identifier(path: Path) -> str:
    stem = NAME_RE.sub("_", path.stem)
    stem = stem.strip("_") or "icon"
    if stem[0].isdigit():
        stem = f"icon_{stem}"
    return stem


def resample_filter() -> int:
    return getattr(Image, "Resampling", Image).LANCZOS


def scale_image(image: Image.Image, target_size: tuple[int, int], mode: str, background: tuple[int, int, int]) -> Image.Image:
    width, height = target_size
    image = image.convert("RGBA")

    if mode == "stretch":
        resized = image.resize((width, height), resample_filter())
        canvas = Image.new("RGBA", (width, height), (*background, 255))
        canvas.alpha_composite(resized)
        return canvas

    src_w, src_h = image.size
    if src_w <= 0 or src_h <= 0:
        raise ValueError("input image has invalid dimensions")

    scale = min(width / src_w, height / src_h) if mode == "contain" else max(width / src_w, height / src_h)
    new_w = max(1, round(src_w * scale))
    new_h = max(1, round(src_h * scale))
    resized = image.resize((new_w, new_h), resample_filter())

    if mode == "cover":
        left = (new_w - width) // 2
        top = (new_h - height) // 2
        cropped = resized.crop((left, top, left + width, top + height))
        matte = Image.new("RGBA", (width, height), (*background, 255))
        matte.alpha_composite(cropped)
        return matte

    canvas = Image.new("RGBA", (width, height), (*background, 255))
    x = (width - new_w) // 2
    y = (height - new_h) // 2
    canvas.alpha_composite(resized, (x, y))
    return canvas


def to_black_pixels(image: Image.Image, threshold: int, invert: bool, dither: str) -> list[list[bool]]:
    gray = image.convert("L")

    if dither == "floyd":
        mono = gray.convert("1", dither=Image.Dither.FLOYDSTEINBERG)
        rows = [
            [mono.getpixel((x, y)) == 0 for x in range(mono.width)]
            for y in range(mono.height)
        ]
    else:
        rows = [
            [gray.getpixel((x, y)) < threshold for x in range(gray.width)]
            for y in range(gray.height)
        ]

    if invert:
        return [[not pixel for pixel in row] for row in rows]
    return rows


def pack_rows(rows: list[list[bool]], bit_order: str, black_is: int) -> list[int]:
    if not rows:
        return []

    width = len(rows[0])
    row_bytes = math.ceil(width / 8)
    packed: list[int] = []

    for row in rows:
        for byte_index in range(row_bytes):
            value = 0
            for bit_index in range(8):
                x = byte_index * 8 + bit_index
                is_black = x < width and row[x]
                stored_bit = is_black if black_is == 1 else not is_black
                if stored_bit:
                    shift = 7 - bit_index if bit_order == "msb" else bit_index
                    value |= 1 << shift
            packed.append(value)

    return packed


def format_c_array(name: str, width: int, height: int, packed: list[int], args: argparse.Namespace) -> str:
    row_bytes = math.ceil(width / 8)
    lines = [
        "/*",
        f" * Generated from: {args.input}",
        f" * Size: {width}x{height}, row bytes: {row_bytes}",
        f" * Bit order: {args.bit_order}, black pixel bit value: {args.black_is}",
        " */",
        "#include <stdint.h>",
        "",
        f"static const uint16_t {name}_width = {width};",
        f"static const uint16_t {name}_height = {height};",
        f"static const uint16_t {name}_row_bytes = {row_bytes};",
        f"static const uint8_t {name}_bitmap[] = {{",
    ]

    for row_start in range(0, len(packed), row_bytes):
        row = packed[row_start : row_start + row_bytes]
        values = ", ".join(f"0x{value:02X}" for value in row)
        lines.append(f"    {values},")

    lines.append("};")
    return "\n".join(lines) + "\n"


def format_hex(packed: list[int]) -> str:
    return " ".join(f"{value:02X}" for value in packed) + "\n"


def format_bits(rows: list[list[bool]]) -> str:
    return "\n".join("".join("1" if pixel else "0" for pixel in row) for row in rows) + "\n"


def format_ascii(rows: list[list[bool]]) -> str:
    return "\n".join("".join("#" if pixel else "." for pixel in row) for row in rows) + "\n"


def format_json(name: str, width: int, height: int, packed: list[int], rows: list[list[bool]], args: argparse.Namespace) -> str:
    payload = {
        "name": name,
        "source": str(args.input),
        "width": width,
        "height": height,
        "row_bytes": math.ceil(width / 8),
        "bit_order": args.bit_order,
        "black_is": args.black_is,
        "bytes": [f"0x{value:02X}" for value in packed],
        "pixels": [[1 if pixel else 0 for pixel in row] for row in rows],
    }
    return json.dumps(payload, indent=2) + "\n"


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Convert an image into target-size monochrome bitmap icon data.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("input", type=Path, help="source image path")
    parser.add_argument("-s", "--size", type=parse_size, required=True, help="target icon size, for example 32x32")
    parser.add_argument("-o", "--output", type=Path, help="write output to this file instead of stdout")
    parser.add_argument("-n", "--name", help="C/JSON icon name; defaults to the input filename stem")
    parser.add_argument(
        "-f",
        "--format",
        choices=("c", "hex", "bits", "ascii", "json"),
        default="c",
        help="output format",
    )
    parser.add_argument(
        "--fit",
        choices=("contain", "cover", "stretch"),
        default="contain",
        help="how to map the source image into the target size",
    )
    parser.add_argument(
        "-t",
        "--threshold",
        type=int,
        default=160,
        help="0..255 grayscale threshold; lower values become black when not dithering",
    )
    parser.add_argument("--invert", action="store_true", help="swap black and white pixels after conversion")
    parser.add_argument(
        "--dither",
        choices=("none", "floyd"),
        default="none",
        help="monochrome dithering; floyd uses Pillow's default 1-bit conversion threshold",
    )
    parser.add_argument(
        "--bit-order",
        choices=("msb", "lsb"),
        default="msb",
        help="bit packing order inside each byte",
    )
    parser.add_argument(
        "--black-is",
        type=int,
        choices=(0, 1),
        default=1,
        help="stored bit value for black pixels; use 0 for this project's raw EPD framebuffer convention",
    )
    parser.add_argument(
        "--background",
        type=parse_color,
        default=(255, 255, 255),
        help="matte color for transparent pixels and padding",
    )
    parser.add_argument("--preview", action="store_true", help="print an ASCII preview to stderr")
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)

    if not 0 <= args.threshold <= 255:
        parser.error("--threshold must be between 0 and 255")

    if not args.input.exists():
        parser.error(f"input image does not exist: {args.input}")

    name = args.name or c_identifier(args.input)
    if NAME_RE.search(name) or name[0].isdigit():
        parser.error("--name must be a valid C-style identifier")

    width, height = args.size
    image = Image.open(args.input)
    scaled = scale_image(image, (width, height), args.fit, args.background)
    rows = to_black_pixels(scaled, args.threshold, args.invert, args.dither)
    packed = pack_rows(rows, args.bit_order, args.black_is)

    if args.preview:
        print(format_ascii(rows), file=sys.stderr, end="")

    if args.format == "c":
        output = format_c_array(name, width, height, packed, args)
    elif args.format == "hex":
        output = format_hex(packed)
    elif args.format == "bits":
        output = format_bits(rows)
    elif args.format == "ascii":
        output = format_ascii(rows)
    else:
        output = format_json(name, width, height, packed, rows, args)

    if args.output:
        args.output.write_text(output, encoding="utf-8")
    else:
        print(output, end="")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
