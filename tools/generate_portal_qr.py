#!/usr/bin/env python3
"""Generate the SoftAP portal QR bitmap header for the ADD WIFI screen."""

from __future__ import annotations

import argparse
from pathlib import Path

import qrcode

DEFAULT_URL = "http://192.168.4.1"
DEFAULT_OUT = Path(__file__).resolve().parents[1] / (
    "components/zectrix_demo_ui/font/zectrix_portal_qr.h"
)


def generate(url: str, scale: int, border: int, out: Path) -> None:
    qr = qrcode.QRCode(
        version=None,
        error_correction=qrcode.constants.ERROR_CORRECT_M,
        box_size=1,
        border=border,
    )
    qr.add_data(url)
    qr.make(fit=True)
    matrix = qr.get_matrix()
    size = len(matrix) * scale
    stride = (size + 7) // 8
    data = bytearray(stride * size)
    for y in range(size):
        for x in range(size):
            black = matrix[y // scale][x // scale]
            if not black:
                data[y * stride + x // 8] |= 0x80 >> (x & 7)

    lines = [
        "#ifndef ZECTRIX_PORTAL_QR_H_",
        "#define ZECTRIX_PORTAL_QR_H_",
        "",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
        f"// Auto-generated QR for {url}",
        f"// {size}x{size}, 1bpp MSB first, 0=black 1=white",
        f"constexpr int kZectrixPortalQrWidth = {size};",
        f"constexpr int kZectrixPortalQrHeight = {size};",
        f"constexpr size_t kZectrixPortalQrStride = {stride};",
        f'constexpr const char kZectrixPortalQrUrl[] = "{url}";',
        "constexpr uint8_t kZectrixPortalQrBits[] = {",
    ]
    for i in range(0, len(data), 12):
        chunk = ", ".join(f"0x{b:02x}" for b in data[i : i + 12])
        lines.append(f"    {chunk},")
    lines += ["};", "", "#endif  // ZECTRIX_PORTAL_QR_H_", ""]
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n".join(lines))
    print(f"wrote {out} ({size}x{size}, {len(data)} bytes)")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", default=DEFAULT_URL)
    parser.add_argument("--scale", type=int, default=5)
    parser.add_argument("--border", type=int, default=2)
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    args = parser.parse_args()
    generate(args.url, args.scale, args.border, args.out)


if __name__ == "__main__":
    main()
