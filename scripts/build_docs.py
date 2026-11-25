#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[1]
out = root / "docs" / "_build"
out.mkdir(parents=True, exist_ok=True)
index = out / "index.html"
body = (root / "docs" / "index.md").read_text(encoding="utf-8")
index.write_text("<html><body><pre>" + body + "</pre></body></html>\n", encoding="utf-8")
print(index)
