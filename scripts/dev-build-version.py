#!/usr/bin/env python3
"""Derive the dev-build version from version.txt.

The release owns version.txt (release-please), and dev builds must never masquerade as one.
The dev number keeps the release base for human recognition and appends a date and the short
commit: 0.7.1-dev.20261019.7fa6fb1a.

The strings go to the installer name and the settings page. The TSF DLL's VERSIONINFO quad has
four 16-bit fields, so the date compresses to YYMMDD in the fourth slot: 0.7.1.261019. yy > 79
sorts above every year this product exists, and any real release leaves that slot at 0, so a dev
DLL always reads as newer than its own base and older than the next release. The settings page's
parseVersion strips the -dev suffix and compares 0.7.1, which is why the update check stays quiet.

Windows filenames and Inno Setup accept the dots in the short hash; a detached HEAD (workflow
dispatch on a raw SHA) falls back to the first twelve hex digits.

    python scripts/dev-build-version.py                  # all three outputs, one per line
    python scripts/dev-build-version.py --display        # 0.7.1-dev.20261019.7fa6fb1a
    python scripts/dev-build-version.py --base           # 0.7.1
    python scripts/dev-build-version.py --date           # 261019
"""

from __future__ import annotations

import argparse
import datetime
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
VERSION_FILE = REPO_ROOT / "version.txt"

SEMVER = re.compile(r"^(\d+)\.(\d+)\.(\d+)$")
SHORT_SHA = re.compile(r"^[0-9a-f]{7,40}$")


def read_base() -> tuple[str, str, str]:
    if not VERSION_FILE.is_file():
        raise SystemExit(f"error: {VERSION_FILE} is missing")
    raw = VERSION_FILE.read_text(encoding="utf-8").strip()
    match = SEMVER.match(raw)
    if not match:
        raise SystemExit(f"error: version.txt must hold MAJOR.MINOR.PATCH, got {raw!r}")
    return match.groups()


def read_short_sha(explicit: str | None) -> str:
    if explicit:
        sha = explicit.strip().lower()
        if not SHORT_SHA.match(sha):
            raise SystemExit(f"error: --commit must be hex digits, got {explicit!r}")
        return sha[:12]
    result = subprocess.run(
        ["git", "rev-parse", "--short=12", "HEAD"],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        raise SystemExit(f"error: cannot read the commit: {result.stderr.strip()}")
    return result.stdout.strip()


def build(base: tuple[str, str, str], today: datetime.date, sha: str, channel: str = "dev") -> tuple[str, str]:
    major, minor, patch = base
    display = f"{major}.{minor}.{patch}-{channel}.{today:%Y%m%d}.{sha}"
    # 四段 quad 的末位放 YYMMDD：YY > 79 恒大于本产品存在的一切年份，任何正式版该位是 0，
    # 所以 dev DLL 恒读作比基线新、比下一个发布旧。
    date = today.strftime("%y%m%d")
    quad = f"{major}.{minor}.{patch}.{date}"
    return display, date


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    output = parser.add_mutually_exclusive_group()
    output.add_argument("--display", action="store_true", help="print the display version only")
    output.add_argument("--base", action="store_true", help="print the base version only")
    output.add_argument("--date", action="store_true", help="print the YYMMDD stamp only")
    parser.add_argument("--commit", metavar="SHA", help="use this short commit instead of reading git")
    parser.add_argument(
        "--channel",
        default="dev",
        help="prerelease label in the display version: dev for feature builds, nightly for the scheduled one",
    )
    args = parser.parse_args()

    base = read_base()
    display, date = build(base, datetime.date.today(), read_short_sha(args.commit), args.channel)
    if args.display:
        print(display)
    elif args.base:
        print(".".join(base))
    elif args.date:
        print(date)
    else:
        print(display)
        print(date)
    return 0


if __name__ == "__main__":
    sys.exit(main())
