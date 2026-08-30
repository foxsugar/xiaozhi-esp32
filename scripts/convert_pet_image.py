#!/usr/bin/env python3
"""Convert PNG/JPG to RGB565 C array for ESP32-S3 pet display.

Usage (single):
    python scripts/convert_pet_image.py <input.png/jpg> <output.c> [--width 240 --height 320]

Usage (batch):
    python scripts/convert_pet_image.py --batch <images_dir> [--output-dir <c_files_dir>]

Batch mode converts every *.png / *.jpg / *.jpeg in <images_dir> to a matching
<name>.c in the output directory. The output name is derived from the input
filename (without extension). Images are cropped/scaled to exactly 240x320.
"""

import argparse
import os
from PIL import Image


def rgb888_to_rgb565(r: int, g: int, b: int) -> int:
    """Pack 8-bit RGB into RGB565 (native uint16, R in high bits).

    生成标准 RGB565：R 在高位字节。esp_lcd_panel_draw_bitmap 对 ST7789 约定
    数组即 RGB565（R 高字节），由面板 rgb_ele_order 决定最终 BGR/RGB 解释，
    这里不做任何字节交换，否则会与 rgb_ele_order 双重反转导致红蓝对调（偏蓝）。
    """
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b & 0xF8) >> 3)


def convert(input_path: str, output_path: str, width: int = 240, height: int = 320) -> None:
    img = Image.open(input_path)
    if img.mode != "RGB":
        img = img.convert("RGB")

    # Resize with aspect ratio crop to fill target exactly
    src_w, src_h = img.size
    src_ratio = src_w / src_h
    dst_ratio = width / height
    if src_ratio > dst_ratio:
        # source is wider: crop width to match height
        new_h = height
        new_w = int(height * src_ratio)
        img = img.resize((new_w, new_h), Image.LANCZOS)
        left = (new_w - width) // 2
        img = img.crop((left, 0, left + width, height))
    elif src_ratio < dst_ratio:
        # source is taller: crop height to match width
        new_w = width
        new_h = int(width / src_ratio)
        img = img.resize((new_w, new_h), Image.LANCZOS)
        top = (new_h - height) // 2
        img = img.crop((0, top, width, top + height))
    else:
        img = img.resize((width, height), Image.LANCZOS)

    data = list(img.get_flattened_data())
    array = [rgb888_to_rgb565(r, g, b) for (r, g, b) in data]

    name = os.path.splitext(os.path.basename(output_path))[0]
    guard = name.upper() + "_H"
    var_name = "g_" + name

    os.makedirs(os.path.dirname(output_path), exist_ok=True)
    with open(output_path, "w", encoding="utf-8") as f:
        f.write(f"// Auto-generated from {os.path.basename(input_path)}\n")
        f.write(f"// {width}x{height} RGB565, little-endian bytes as stored by esp_lcd.\n")
        f.write(f"#ifndef {guard}\n")
        f.write(f"#define {guard}\n\n")
        f.write(f"#include <stdint.h>\n\n")
        f.write(f"#define {name.upper()}_WIDTH  {width}\n")
        f.write(f"#define {name.upper()}_HEIGHT {height}\n")
        f.write(f"#define {name.upper()}_PIXELS ({width}*{height})\n\n")
        f.write(f"const uint16_t {var_name}[{name.upper()}_PIXELS] = {{\n")

        for i in range(0, len(array), 16):
            line = ", ".join(f"0x{v:04X}" for v in array[i:i + 16])
            f.write(f"    {line},\n")

        f.write(f"}};\n\n")
        f.write(f"#endif // {guard}\n")

    print(f"Generated {output_path}: {width}x{height} ({len(array)} pixels, {len(array)*2} bytes)")


def batch_convert(images_dir: str, output_dir: str, width: int = 240, height: int = 320) -> None:
    """Convert all images in images_dir to C arrays in output_dir."""
    os.makedirs(output_dir, exist_ok=True)
    count = 0
    for entry in sorted(os.listdir(images_dir)):
        low = entry.lower()
        if not (low.endswith(".png") or low.endswith(".jpg") or low.endswith(".jpeg")):
            continue
        input_path = os.path.join(images_dir, entry)
        name = os.path.splitext(entry)[0]
        output_path = os.path.join(output_dir, name + ".c")
        convert(input_path, output_path, width, height)
        count += 1
    if count == 0:
        print(f"No .png/.jpg images found in {images_dir}")
    else:
        print(f"Batch complete: {count} image(s) -> {output_dir}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Convert image(s) to RGB565 C array")
    parser.add_argument("input", nargs="?", help="input PNG/JPG file (single mode)")
    parser.add_argument("output", nargs="?", help="output .c file (single mode)")
    parser.add_argument("--batch", help="batch convert all images in this directory")
    parser.add_argument("--output-dir", default="main/pet/assets",
                        help="output directory for batch mode (default: main/pet/assets)")
    parser.add_argument("--width", type=int, default=240)
    parser.add_argument("--height", type=int, default=320)
    args = parser.parse_args()

    if args.batch:
        batch_convert(args.batch, args.output_dir, args.width, args.height)
    elif args.input and args.output:
        convert(args.input, args.output, args.width, args.height)
    else:
        parser.print_help()
        raise SystemExit(1)
