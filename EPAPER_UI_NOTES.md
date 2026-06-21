# 2.13" E-paper UI Notes

## Panel Geometry

- Controller/framebuffer landscape size: `250 x 122`.
- Current module's practical visible/safe area: about `212 x 104`.
- The firmware still uses `setRotation(1)` and the controller framebuffer, but disk UI drawing should stay inside the safe area.
- Current safe-area constants live in `src/display.cpp`:
  - `DISK_UI_W = 212`
  - `DISK_UI_H = 104`
  - `DISK_UI_X = 0`
  - `DISK_UI_Y = 9`

Do not position disk-page UI against `g.width()` / `g.height()` unless the element is intended to use the full controller framebuffer. For the visible disk UI, use the 212x104 safe area.

The visible glass appears vertically centered inside the `250 x 122` controller framebuffer. Draw the disk page at `y = 9`; drawing from `y = 0` clips the top of the UI and leaves blank space at the bottom.

## Text Metrics

The built-in font is `src/font5x7.h`.

- Text size 1: `6 x 8` pixels per character, including spacing column.
- Text size 2: `12 x 16` pixels per character.
- Text size 3: `18 x 24` pixels per character.

Leave several pixels between text and borders. The physical panel edges crop more aggressively than the controller framebuffer suggests.

## Current Disk Page Layout

The disk page is based on the left half of the original UI mockup, simplified for the smaller safe area:

- Outer clipped-corner frame inside `212 x 104`.
- Top-left simplified disk icon, switching between SATA/HDD and NVMe-style drawings.
- Top badges for device type and health.
- Large device name.
- Model label and truncated model text.
- Two metric boxes:
  - Capacity
  - Temperature
- Bottom power-on time strip.

The right-side temperature history chart was removed because it does not fit legibly on this panel.

## Drawing Guidelines

- Keep all primary content within `x <= 204` and `y <= 102`.
- Keep at least 4-8 px of inner padding around frame/card borders when possible.
- Avoid `size 3` text except for short device names such as `sda`; use `size 2` for longer names like `nvme2`.
- Truncate model text rather than letting it run into cards or the frame.
- Prefer simple line icons over detailed drawings; dense pixel art becomes muddy after refresh.
- For future charts, use a separate screen/page rather than mixing charts with the disk summary.
- If adding a new label, test with the longest expected text, not only `sda` / `4 TB`.

## Preview Image

`epaper-ui-preview.png` is generated from a PIL script using the same 5x7 font data and current UI coordinates. Use it for README visuals, but verify on the physical panel before treating spacing as final.
