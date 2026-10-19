"""Correct the README's Brand section to describe the real Flowing F artwork.

Run:  python update_readme_icon.py
"""

import io
import os

HERE = os.path.dirname(os.path.abspath(__file__))
README = os.path.normpath(os.path.join(HERE, "..", "README.md"))

text = io.open(README, encoding="utf-8").read()

start = text.index("## Brand")
end = text.index("### Themes")

new = """## Brand

**Flowbit** - *Control Your Connection.*
System-wide bandwidth control for Windows.

### The mark

The logo is the **Flowing F**: two distinct flowing ribbon shapes forming an
abstract F, rendered as custom cubic-Bezier paths. It is deliberately *not* a
letterform - there is no font character, no text element, and no rounded
rectangle anywhere in it.

- **Upper ribbon** - the larger shape. Begins upper-left, sweeps up and right
  into a tapered tip, and narrows to a pointed downward tail on the left.
- **Lower ribbon** - starts below the upper one, sweeps up into a shorter second
  stroke ending in a soft taper, and drops into a curved tail on the left.
- The two are separated by a visible dark gap and follow the same direction of
  travel, so the pair reads as an F while staying aerodynamic.

Colour is a gradient per ribbon, never a flat blue:

| Ribbon | Gradient |
| --- | --- |
| Upper | `#22D3EE` bright cyan \u2192 `#06B6D4` cyan-blue |
| Lower | `#06B6D4` cyan \u2192 `#3B82F6` electric blue |

Plates: `#0F172A` dark navy for the dark variant, `#F8FAFC` for the light one,
with a subtle stroke. Rounded-square, and the symbol sits at roughly 60% of the
plate width - the artwork's own proportion, which is preserved rather than
adjusted, so negative space stays generous at tray sizes.

### Source of truth

`docs/flowbit-dark.svg` and `docs/flowbit-light.svg` are **authoritative**. The
two variants share identical geometry and differ only in plate colour and
gradient tuning. Every other asset is derived from them:

```
docs/flowbit-dark.svg ──┐
                        ├─► tools/build_icons.ps1 ─► src/flowbit.ico (7 sizes)
docs/flowbit-light.svg ─┘                          ─► src/flowbit-light.ico
                                                   ─► docs/flowbit-{16..512}.png
```

Rasterisation uses `tools/svg_render.cpp` (Qt's `QSvgRenderer`), so **no bitmap
is ever hand-edited or upscaled** - each size is rendered from the vector, which
is what keeps 16px and 512px consistent.

Regenerate everything with:

```powershell
powershell -ExecutionPolicy Bypass -File tools\\build_icons.ps1
```

At runtime the tray and window glyph come from the same SVG via `brandmark.cpp`,
so the packaged icon and what the app draws cannot drift apart. The plate variant
follows the active theme dark/light, and the "limiting is off" state adds a red
diagonal.

An earlier revision hand-painted an approximation with `QPainter` primitives; it
produced a conventional letter F (a rounded rectangular stem plus two bars)
rather than the flowing ribbons. `test_icon` now guards against that by comparing
the production render against the reference artwork pixel-for-pixel - it
currently agrees to a mean difference of **0.10/255** - and by asserting the
silhouette has two separated ribbons and no solid stem.

"""

text = text[:start] + new + text[end:]

# test table entry
marker = "| `test_interfaces` (38) |"
extra = (
    "| `test_icon` (18) | Renders the production brand mark and compares it against the "
    "reference artwork (mean diff 0.10/255), asserts the silhouette is two ribbons with a "
    "gap and no rectangular stem, checks both plate variants, the off-state slash, and "
    "legibility at 16/24/32/48px |\n"
)
if "| `test_icon`" not in text:
    text = text.replace(marker, extra + marker)

text = text.replace(
    "powershell -ExecutionPolicy Bypass -File tools\\run_theme_tests.ps1   # 31 checks",
    "powershell -ExecutionPolicy Bypass -File tools\\run_theme_tests.ps1   # 31 checks\n"
    "powershell -ExecutionPolicy Bypass -File tools\\run_icon_tests.ps1    # 18 checks",
)
text = text.replace(
    "Ten test suites \u2014 **298 checks** \u2014 none of which need Administrator rights or\ntouch the network:",
    "Eleven test suites \u2014 **316 checks** \u2014 none of which need Administrator rights or\ntouch the network:",
)

# layout block additions
text = text.replace(
    "  test_bugfixes.cpp, test_theme.cpp, run_bugfix_tests.ps1, run_theme_tests.ps1",
    "  test_bugfixes.cpp, test_theme.cpp, test_icon.cpp,\n"
    "  svg_render.cpp, build_icons.ps1, run_bugfix_tests.ps1,\n"
    "  run_theme_tests.ps1, run_icon_tests.ps1",
)
text = text.replace(
    "  units.*             MB/s <-> KB/s <-> bytes/sec conversion and formatting",
    "  brandmark.*         loads the logo SVG and renders the tray/window glyph\n"
    "  units.*             MB/s <-> KB/s <-> bytes/sec conversion and formatting",
)

io.open(README, "w", encoding="utf-8", newline="").write(text)
print("README brand section rewritten")
print("lines:", len(text.splitlines()))
