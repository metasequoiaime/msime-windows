"""Download hash-pinned model packages and extract only the inference files."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import shutil
import tempfile
import urllib.request
import zipfile


def verify(path: Path, entry: dict):
    if path.stat().st_size != entry["bytes"]:
        raise ValueError("Model package size does not match the lock")
    with path.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    if digest != entry["sha256"]:
        raise ValueError("Model package SHA256 does not match the lock")


def extract(package: Path, root: Path, entry: dict):
    verify(package, entry)
    directory = entry["directory"]
    allowed = {"metadata.json", "README.md", "sentencepiece.model", "model/config.json",
               "model/model.bin", "model/shared_vocabulary.json"}
    with zipfile.ZipFile(package) as archive:
        selected = {}
        for member in archive.infolist():
            path = PurePosixPath(member.filename)
            if path.is_absolute() or ".." in path.parts or "\\" in member.filename:
                raise ValueError("Unsafe model package path")
            relative = path.relative_to(directory).as_posix()
            if relative in allowed:
                if relative in selected or member.file_size > 200_000_000:
                    raise ValueError("Unexpected model package member")
                selected[relative] = member
        if set(selected) != allowed:
            raise ValueError("Model package is missing inference or license files")
        destination = root / directory
        if destination.exists():
            # Never overwrite an existing model generation or partially merge packages.
            raise FileExistsError(destination)
        with tempfile.TemporaryDirectory(prefix="model-", dir=root) as staging:
            stage = Path(staging) / directory
            for relative, member in selected.items():
                output = stage / relative
                output.parent.mkdir(parents=True, exist_ok=True)
                with archive.open(member) as source, output.open("wb") as target:
                    shutil.copyfileobj(source, target)
            (stage / "PACKAGE_SHA256").write_text(entry["sha256"] + "\n", encoding="ascii")
            stage.rename(destination)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    args = parser.parse_args()
    args.root.mkdir(parents=True, exist_ok=True)
    downloads = args.root.parent / "downloads"
    downloads.mkdir(exist_ok=True)
    lock = json.loads(Path(__file__).with_name("model-lock.json").read_text(encoding="utf-8"))
    for entry in lock["models"]:
        destination = args.root / entry["directory"]
        if destination.exists():
            marker = destination / "PACKAGE_SHA256"
            if not marker.exists() or marker.read_text().strip() != entry["sha256"]:
                raise ValueError("Existing model does not match the lock")
            continue
        package = downloads / (entry["directory"] + ".argosmodel")
        if not package.exists():
            incoming = package.with_suffix(".incoming")
            with urllib.request.urlopen(entry["url"], timeout=60) as source, incoming.open("wb") as target:
                received = 0
                while block := source.read(1024 * 1024):
                    received += len(block)
                    if received > entry["bytes"]:
                        raise ValueError("Download exceeds the locked package size")
                    target.write(block)
            verify(incoming, entry)
            incoming.replace(package)
        extract(package, args.root, entry)
        print(f"Verified and installed {entry['directory']}")


if __name__ == "__main__":
    main()
