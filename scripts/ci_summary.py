#!/usr/bin/env python3
"""Краткий отчёт и хвосты логов для страницы Actions."""
import html
import json
import os
from pathlib import Path


def main():
    root = Path("twister-out")
    report = root / "twister.json"
    with Path(os.environ["GITHUB_STEP_SUMMARY"]).open("a") as summary:
        summary.write("## Тесты QEMU\n\n")
        if report.exists():
            data = json.loads(report.read_text())
            for suite in data.get("testsuites", []):
                summary.write(html.escape(
                    f"{suite.get('name')}: {suite.get('status')} — {suite.get('reason', '')}"
                ) + "\n\n")
        for path in sorted(root.rglob("*.log")):
            if path.name not in {"build.log", "handler.log", "handler_stderr.log"}:
                continue
            text = path.read_text(errors="replace")[-6000:]
            summary.write(f"<details><summary>{html.escape(str(path))}</summary>\n\n")
            summary.write(f"<pre>{html.escape(text)}</pre>\n\n</details>\n\n")


if __name__ == "__main__":
    main()
