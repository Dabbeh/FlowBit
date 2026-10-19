"""Repair the four mojibake-damaged literals in src/mainwindow.cpp.

An earlier bulk rebrand read the UTF-8 source as cp1252 and wrote it back, which
double-encoded the non-ASCII characters in these lines. Rather than try to
reverse the encoding, this rewrites each line from a stable ASCII anchor using
explicit \\u escapes, which are unambiguous in C++ source.
"""

import io
import os

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.normpath(os.path.join(HERE, "..", "src", "mainwindow.cpp"))

lines = io.open(SRC, encoding="utf-8").read().splitlines(keepends=True)

patched = 0
for i, line in enumerate(lines):
    indent = line[: len(line) - len(line.lstrip())]

    if "Shaping system-wide traffic with WinDivert" in line:
        lines[i] = (
            indent
            + 'statusBar()->showMessage(\n'
            + indent
            + '    QStringLiteral("Shaping system-wide traffic with WinDivert  '
            '\\u2022  runs elevated"));\n'
        )
        patched += 1

    elif "makeCounter" in line and "DOWNLOAD" in line:
        lines[i] = (
            indent
            + 'makeCounter(QStringLiteral("\\u2193"), QStringLiteral("DOWNLOAD"), '
            'theme::kCyan, &m_downCounter);\n'
        )
        patched += 1

    elif "makeCounter" in line and "UPLOAD" in line:
        lines[i] = (
            indent
            + 'makeCounter(QStringLiteral("\\u2191"), QStringLiteral("UPLOAD"), '
            'theme::kOrange, &m_upCounter);\n'
        )
        patched += 1

    elif "Status: Active (" in line and "applied" in line:
        lines[i] = (
            indent
            + 'm_footerStatus->setText(\n'
            + indent
            + '    QStringLiteral("Status: Active (%1 applied)'
            '  \\u2022  \\u2193 %2  \\u2191 %3")\n'
            + indent
            + '        .arg(limits.reason, formatRate(limits.down), '
            'formatRate(limits.up)));\n'
        )
        patched += 1

io.open(SRC, "w", encoding="utf-8", newline="").write("".join(lines))
print(f"patched {patched} line(s)")

# Report the result by codepoint so it is unambiguous.
text = io.open(SRC, encoding="utf-8").read()
for n, line in enumerate(text.splitlines(), 1):
    if any(ord(c) > 0x7F for c in line):
        cps = " ".join(f"U+{ord(c):04X}" for c in line if ord(c) > 0x7F)
        print(f"{n:4}: {cps}")
        print(f"      {line.strip()[:88]}")
