"""Find and fix the typographic characters that read as machine-written.

Em-dashes are the giveaway, and here they are also a hazard: the user-region
marker ends up inside generated .c files, and DayZ's config parser is not
UTF-8 safe, so a stray dash reports an error on the wrong line.

This tool only performs swaps that cannot change meaning. Em-dashes need a
human decision (comma, colon, full stop or parentheses, never " - "), so they
are reported with their line and left alone.

    python tools/ascii_sweep.py           report only
    python tools/ascii_sweep.py --fix     apply the safe swaps
"""

import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SKIP_DIRS = {"build", ".git", "node_modules"}
PATTERNS = ("*.cpp", "*.h", "*.md", "*.py", "*.sdzn")

# Unambiguous: same meaning, ASCII spelling.
SAFE = {
    "–": "-",      # en-dash
    "−": "-",      # minus sign
    "“": '"', "”": '"',
    "‘": "'", "’": "'",
    "…": "...",    # fine in a menu label, still spelled ASCII
    "→": "->",
    "·": "-",      # middot used as a separator in node subtitles
}

# Needs judgement, reported only.
MANUAL = {"—": "em-dash"}

# Glyphs that carry meaning in the status bar rather than being punctuation.
ALLOWED = {"●", "▲"}


def files():
    for pattern in PATTERNS:
        for path in ROOT.rglob(pattern):
            if any(part in SKIP_DIRS for part in path.parts):
                continue
            if path.name == "ascii_sweep.py":
                continue
            yield path


def main():
    fix = "--fix" in sys.argv
    swapped = 0
    manual = []

    for path in files():
        text = path.read_text(encoding="utf-8")
        rel = path.relative_to(ROOT)

        for line_no, line in enumerate(text.split("\n"), 1):
            for ch, name in MANUAL.items():
                if ch in line:
                    manual.append((rel, line_no, line.strip()))

        new = text
        for bad, good in SAFE.items():
            new = new.replace(bad, good)
        if new != text:
            hits = sum(text.count(b) for b in SAFE)
            swapped += hits
            print(f"{'fixed' if fix else 'would fix'} {hits:4d}  {rel}")
            if fix:
                path.write_text(new, encoding="utf-8")

    print(f"\n{swapped} safe swaps")
    print(f"{len(manual)} em-dashes need re-punctuating by hand:")
    by_file = {}
    for rel, line_no, line in manual:
        by_file.setdefault(str(rel), []).append(line_no)
    for name, lines in sorted(by_file.items(), key=lambda kv: -len(kv[1])):
        print(f"  {len(lines):4d}  {name}  lines {lines[:8]}"
              f"{'...' if len(lines) > 8 else ''}")

    leftover = {}
    for path in files():
        for ch in path.read_text(encoding="utf-8"):
            if ord(ch) > 127 and ch not in ALLOWED and ch not in MANUAL:
                leftover.setdefault(f"U+{ord(ch):04X} {ch}", 0)
                leftover[f"U+{ord(ch):04X} {ch}"] += 1
    if leftover:
        print("other non-ascii still present:", leftover)


if __name__ == "__main__":
    main()
