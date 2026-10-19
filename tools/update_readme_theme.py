"""Document the dark/light theme system in README.md.

Run:  python update_readme_theme.py
"""

import io
import os

HERE = os.path.dirname(os.path.abspath(__file__))
README = os.path.normpath(os.path.join(HERE, "..", "README.md"))

text = io.open(README, encoding="utf-8").read()

# --- replace the palette section with the theme system ---------------------
start = text.index("### Palette")
end = text.index("### Rebranding note")

new = """### Themes

Two neutrally-toned themes plus a "follow Windows" mode. The selector sits in the
ACTIVE SPEED LIMITER header, and the choice persists as `themeMode` in
`settings.json`.

| Mode | Behaviour |
| --- | --- |
| **Auto** (default) | Follows the Windows apps-light/dark setting, read from `AppsUseLightTheme`, and re-applies live while the app runs |
| **Light** | Forces the white theme |
| **Dark** | Forces the dark theme |

| Role | Dark | Light |
| --- | --- | --- |
| Window background | `#121417` | `#F1F3F5` |
| Card / panel | `#1A1D21` | `#FFFFFF` |
| Raised (inputs, rows) | `#24282D` | `#F7F8FA` |
| Sunken (wells, graph bed) | `#0E1013` | `#ECEEF1` |
| Border | `#2E3339` | `#D8DCE1` |
| Primary text | `#F3F5F7` | `#1B1F24` |
| Secondary text | `#A8B0B9` | `#555D67` |
| Download / incoming | `#22C7E0` | `#0E869B` |
| Upload / outgoing | `#FF952E` | `#C26600` |

Both themes are **neutral grey** - there is no blue cast in the chrome, so the
only saturated things on screen are the two directional colours.

The directional colours are **deliberately kept, not neutralised**: cyan means
incoming and orange means outgoing, and that mapping is load-bearing. The
accents are darkened in the light theme so they keep contrast on white (the
dark-theme values wash out badly there). Warning, error and healthy colours are
likewise never re-tinted - they carry meaning.

The palette lives in `theme.cpp` as two `Palette` structs behind a runtime
accessor, so `theme::kPanel()`-style calls resolve to whichever theme is active
and custom-painted widgets just repaint on a switch.

"""

text = text[:start] + new + text[end:]

# --- test counts and suite table -------------------------------------------
text = text.replace("tools\\run_tests.ps1     # 155 checks", "tools\\run_tests.ps1     # 155 checks")
text = text.replace("tools\\run_ui_test.ps1   # 62 checks", "tools\\run_ui_test.ps1   # 64 checks")
text = text.replace(
    "Seven test suites plus a headless UI test \u2014 **217 checks** \u2014 none of which need\nAdministrator rights or touch the network:",
    "Ten test suites \u2014 **298 checks** \u2014 none of which need Administrator rights or\ntouch the network:",
)

marker = "| `test_interfaces` (38) |"
extra = (
    "| `test_bugfixes` (48) | Regression guards for six reported UI bugs: arrow-glyph "
    "encoding, the toggle's ON/OFF caption, spin-box typing plus two-way slider sync and "
    "range expansion, graph bottom padding, the single-line rule row, and helper-text "
    "placement |\n"
    "| `test_theme` (31) | Renders widgets in both themes and compares pixels: the chrome is "
    "neutral (no colour cast), text inverts, directional accents keep contrast and stay "
    "recognisably cyan/orange, the selector persists the choice, and an older settings file "
    "defaults to Auto |\n"
)
if "| `test_bugfixes`" not in text:
    text = text.replace(marker, extra + marker)

# --- note the extra test scripts in the layout block ------------------------
text = text.replace(
    "  test_interfaces.cpp, test_speedmonitor.cpp, test_toggle.cpp",
    "  test_interfaces.cpp, test_speedmonitor.cpp, test_toggle.cpp,\n"
    "  test_bugfixes.cpp, test_theme.cpp, run_bugfix_tests.ps1, run_theme_tests.ps1",
)

io.open(README, "w", encoding="utf-8", newline="").write(text)
print("README theme section updated")
print("lines:", len(text.splitlines()))
