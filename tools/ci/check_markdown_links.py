"""Check repository-local Markdown links without network access."""
from __future__ import annotations

from pathlib import Path
import re
import sys
from urllib.parse import unquote

ROOT = Path(__file__).resolve().parents[2]
LINK = re.compile(r"!?\[[^\]]*\]\(([^)]+)\)")
SKIP_PARTS = {".git", "build", "logs", "backups", ".venv", "venv", "__pycache__"}


def markdown_files():
    for path in ROOT.rglob("*.md"):
        if not SKIP_PARTS.intersection(path.relative_to(ROOT).parts):
            yield path


def main() -> int:
    failures: list[str] = []
    checked = 0
    for document in markdown_files():
        text = document.read_text(encoding="utf-8-sig")
        for match in LINK.finditer(text):
            raw = match.group(1).strip()
            target = raw.split(maxsplit=1)[0].strip("<>")
            if not target or target.startswith(("#", "http://", "https://", "mailto:")):
                continue
            target = unquote(target.split("#", 1)[0])
            if not target:
                continue
            checked += 1
            resolved = (document.parent / target).resolve()
            try:
                resolved.relative_to(ROOT.resolve())
            except ValueError:
                failures.append(f"{document.relative_to(ROOT)} -> outside repository: {raw}")
                continue
            if not resolved.exists():
                line = text.count("\n", 0, match.start()) + 1
                failures.append(f"{document.relative_to(ROOT)}:{line} -> missing: {raw}")
    if failures:
        print("Broken local Markdown links:", file=sys.stderr)
        print("\n".join(failures), file=sys.stderr)
        return 1
    print(f"PASS: {checked} local links across repository Markdown files")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
