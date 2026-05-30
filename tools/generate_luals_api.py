#!/usr/bin/env python3
"""Generate protocols/protoscope_api.lua from its JSON manifest."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


def generate(manifest_path: Path) -> str:
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    lines: list[str] = []
    lines.extend(manifest.get("header", []))
    for section in manifest.get("sections", []):
        if lines and lines[-1] != "":
            lines.append("")
        lines.extend(section.get("lines", []))
    return "\n".join(lines).rstrip() + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", default="protocols/protoscope_api_manifest.json")
    parser.add_argument("--output", default="protocols/protoscope_api.lua")
    parser.add_argument("--check", action="store_true", help="fail if output is not up to date")
    args = parser.parse_args()

    manifest_path = Path(args.manifest)
    output_path = Path(args.output)
    text = generate(manifest_path)

    if args.check:
        current = output_path.read_text(encoding="utf-8") if output_path.exists() else ""
        if current != text:
            print(f"{output_path} is not generated from {manifest_path}")
            return 1
        return 0

    output_path.write_text(text, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
