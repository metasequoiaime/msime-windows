#!/usr/bin/env python3
"""Maintain and consume the reviewed Windows product dependency lock.

Only `refresh` resolves upstream refs. All build commands are offline except fetching
the exact dictionary assets, whose bytes must match the committed SHA256 values.
"""

from __future__ import annotations

import argparse
import importlib.util
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import product_lock_shared as shared

ROOT = Path(__file__).resolve().parents[1]
_product_spec = importlib.util.spec_from_file_location("dictionary_product", Path(__file__).with_name("dictionary_product.py"))
_product = importlib.util.module_from_spec(_product_spec)
_product_spec.loader.exec_module(_product)
PRODUCT_MANIFEST = _product.MANIFEST_NAME
LEGACY_DICTIONARY_TAG = "dict-2026.09.05"
DICTIONARY_REPOSITORY = "metasequoiaime/MSIME-Engine"

# The tip, the server, the GUI framework, the pages, the installer and now the engine are all
# components of this repository, so a commit of this repository already pins them and there is
# nothing left to lock. The helpcodes live in the engine, so they are pinned the same way. What
# survives is the dictionary release alone, which is fetched at build time rather than built here.
#
# The engine's upstream provenance is recorded in engine/UPSTREAM.md, not here. A lock entry would
# claim the tree still matches an upstream commit, and a Windows-specific fix to engine/ makes that
# claim false the moment it lands -- with nothing in CI able to notice, because there is no longer a
# gitlink to compare against.
REPOSITORIES: dict[str, str] = {}
ROOT_COMPONENTS = ()
ASSETS = {
    "msime.db", "english.db", "others.db", "dict_japanese.dat",
    "mozc_dictionary_oss_README.txt", "SHA256SUMS.txt",
}
SHA = re.compile(r"[0-9a-f]{40}\Z")
DIGEST = re.compile(r"[0-9a-f]{64}\Z")
TAG = re.compile(r"dict-[A-Za-z0-9._-]+\Z")


def validate(data: dict) -> dict:
    if data.get("schema_version") != 1:
        raise ValueError("Unsupported product lock schema_version")
    repositories = data.get("repositories", {})
    if set(repositories) != set(REPOSITORIES):
        raise ValueError("Product lock must contain every expected repository")
    for name, repository in REPOSITORIES.items():
        entry = repositories[name]
        if entry.get("repository") != repository or not SHA.fullmatch(entry.get("commit", "")):
            raise ValueError(f"{name}: expected {repository} and a full immutable commit SHA")
    dictionary = data.get("dictionary", {})
    # The dictionary source and build entry point moved into the engine. The published MSIME-Dict
    # releases stay valid as immutable historical artefacts, so the old repository is still accepted
    # for the tag that shipped from it, and for nothing else: moving the publishing source is a
    # reviewed change to this file, not something a tag rename can do quietly.
    if dictionary.get("repository") != DICTIONARY_REPOSITORY and not (
        dictionary.get("repository") == "metasequoiaime/MSIME-Dict" and dictionary.get("tag") == LEGACY_DICTIONARY_TAG
    ):
        raise ValueError("Unexpected dictionary repository")
    if not TAG.fullmatch(dictionary.get("tag", "")):
        raise ValueError("Dictionary tag must be an explicit dict-* release, never latest")
    if not SHA.fullmatch(dictionary.get("source_commit", "")):
        raise ValueError("Dictionary source_commit must be a full immutable commit SHA")
    assets = dictionary.get("assets", {})
    expected_assets = ASSETS if dictionary['tag'] == LEGACY_DICTIONARY_TAG else ASSETS | {PRODUCT_MANIFEST}
    if set(assets) != expected_assets:
        raise ValueError("Dictionary lock must include all databases, model, notice and checksums")
    for name, digest in assets.items():
        if not DIGEST.fullmatch(digest):
            raise ValueError(f"Invalid SHA256 for {name}")
    return data


def load(path: Path) -> dict:
    return validate(json.loads(path.read_text(encoding="utf-8")))


def write_json(path: Path, data: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def sha256(path: Path) -> str:
    return shared.sha256(path)


def verify_assets(directory: Path, data: dict) -> None:
    shared.verify_digests(directory, data["dictionary"]["assets"])

    if PRODUCT_MANIFEST in data["dictionary"]["assets"]:
        required_files = set(data["dictionary"]["assets"]) - {"SHA256SUMS.txt", PRODUCT_MANIFEST}
        shared.verify_manifest_provenance(directory, PRODUCT_MANIFEST, _product.verify_product, required_files,
                                          data["dictionary"]["repository"], data["dictionary"]["source_commit"])


def verify_published(data: dict) -> None:
    """Every locked commit has to be reachable from its own repository's default branch, the dictionary source commit included.

    This is a release gate rather than a validate rule, deliberately. A pull request legitimately locks branch commits while one change lands across several repositories at once, and enforcing this in pull request CI would deadlock the very landing it exists to protect. At release time the situation is the opposite: an input that never reached its default branch is an input nobody merged, and shipping it makes the lock attest to a review that did not happen.
    """
    # The dictionary sits outside repositories but pins a commit exactly like one would, and the
    # manifest check only proves the release agrees with the lock about it, which a branch commit
    # does just as happily. So it goes through the same gate under its own name -- and since the
    # engine was vendored in-tree it is the only entry left, which is why the loop still exists
    # rather than being folded into a single call.
    dictionary = data["dictionary"]
    entries = {**data["repositories"],
               "dictionary": {"repository": dictionary["repository"], "commit": dictionary["source_commit"]}}
    for name, entry in entries.items():
        repository, commit = entry["repository"], entry["commit"]
        try:
            default = api(f"repos/{repository}")["default_branch"]
            status = api(f"repos/{repository}/compare/{default}...{commit}")["status"]
        except subprocess.CalledProcessError as error:
            # A release that shipped from the retired dictionary repository is an immutable historical
            # artefact that validate still accepts, and an archived or deleted repository answers
            # nothing about its default branch. Being unable to re-check that history is not the same
            # as finding it unmerged, so it is reported rather than turned into a release failure.
            if name == "dictionary" and repository != DICTIONARY_REPOSITORY:
                print(f"{name}: {commit[:12]} predates {DICTIONARY_REPOSITORY} and {repository} no longer answers")
                continue
            raise ValueError(f"{name}: cannot resolve {commit} in {repository}") from error
        # behind and identical both mean the locked commit is an ancestor of the default branch.
        # ahead and diverged mean it sits on something that was never merged into it.
        if status not in ("behind", "identical"):
            raise ValueError(f"{name}: {commit} is not on {repository}'s {default} ({status})")
        print(f"{name}: {commit[:12]} is on {repository}'s {default}")


def github_outputs(data: dict) -> str:
    return "".join(f"{name}_sha={entry['commit']}\n" for name, entry in data["repositories"].items()) + \
        f"dictionary_tag={data['dictionary']['tag']}\n"


def fetch_dictionaries(staging: Path, data: dict) -> None:
    target = staging / "MetasequoiaImeDict" / "out"
    target.parent.mkdir(parents=True, exist_ok=True)
    # Verify the complete set before replacing any usable files in the staging tree.
    with tempfile.TemporaryDirectory(dir=target.parent) as temporary:
        incoming = Path(temporary)
        command = ["gh", "release", "download", data["dictionary"]["tag"],
                   "--repo", data["dictionary"]["repository"], "--dir", str(incoming)]
        for name in sorted(data["dictionary"]["assets"]):
            command.extend(["--pattern", name])
        # gh hits api.github.com before the CDN; the nightly pipeline has seen
        # connection drops mid-week, so give the download one retry before the
        # digest verification below would fail the run anyway.
        try:
            subprocess.run(command, check=True)
        except subprocess.CalledProcessError as error:
            print(f"gh release download failed ({error.returncode}); retrying once", file=sys.stderr)
            subprocess.run(command, check=True)
        verify_assets(incoming, data)
        target.mkdir(exist_ok=True)
        for name in sorted(data["dictionary"]["assets"]):
            shutil.copyfile(incoming / name, target / name)
    notice = staging / "MetasequoiaImeDict" / "source" / "mozc_dictionary_oss" / "README.txt"
    notice.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(target / "mozc_dictionary_oss_README.txt", notice)


def api(endpoint: str) -> dict:
    return json.loads(subprocess.check_output(["gh", "api", endpoint], text=True))


def refresh(tag: str, refs: list[str]) -> dict:
    if not TAG.fullmatch(tag):
        raise ValueError("refresh requires an explicit dict-* release tag")
    # Nothing is resolved from a floating ref any more. Every first-party source, the engine
    # included, is a directory of this repository, so --ref has nothing left to override.
    if refs:
        raise ValueError("--ref has no effect: the dictionary release is the only locked source")
    release = api(f"repos/{DICTIONARY_REPOSITORY}/releases/tags/{tag}")
    if release["draft"]:
        raise ValueError("Cannot lock an unpublished dictionary release")
    assets = {}
    for asset in release["assets"]:
        if asset["name"] in ASSETS | {PRODUCT_MANIFEST}:
            digest = asset.get("digest") or ""
            if not digest.startswith("sha256:"):
                raise ValueError(f"Release asset has no SHA256 digest: {asset['name']}")
            assets[asset["name"]] = digest.removeprefix("sha256:")
    source_commit = api(f"repos/{DICTIONARY_REPOSITORY}/commits/{tag}")["sha"]
    return validate({"schema_version": 1, "repositories": {},
                     "dictionary": {"repository": DICTIONARY_REPOSITORY, "tag": tag,
                                    "source_commit": source_commit, "assets": assets}})


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lock", type=Path, default=ROOT / "product-lock.json")
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("validate")
    output = commands.add_parser("outputs")
    output.add_argument("--github-output", type=Path)
    commands.add_parser("verify-published")
    fetch = commands.add_parser("fetch-dictionaries")
    fetch.add_argument("--staging-root", type=Path, required=True)
    verify = commands.add_parser("verify-dictionaries")
    verify.add_argument("directory", type=Path)
    manifest = commands.add_parser("manifest")
    manifest.add_argument("--windows-commit", required=True)
    manifest.add_argument("--output", type=Path, required=True)
    update = commands.add_parser("refresh")
    update.add_argument("--dictionary-tag", required=True)
    update.add_argument("--ref", action="append", default=[])
    args = parser.parse_args()
    if args.command == "refresh":
        write_json(args.lock, refresh(args.dictionary_tag, args.ref))
        return
    data = load(args.lock)
    if args.command == "outputs":
        if args.github_output:
            with args.github_output.open("a", encoding="utf-8") as stream:
                stream.write(github_outputs(data))
        else:
            print(github_outputs(data), end="")
    elif args.command == "verify-published":
        verify_published(data)
    elif args.command == "fetch-dictionaries":
        fetch_dictionaries(args.staging_root, data)
    elif args.command == "verify-dictionaries":
        verify_assets(args.directory, data)
    elif args.command == "manifest":
        if not SHA.fullmatch(args.windows_commit):
            raise ValueError("Windows commit must be a full SHA")
        data["repositories"]["windows"] = {"repository": "metasequoiaime/MSIME-Windows",
                                           "commit": args.windows_commit}
        data["lock_sha256"] = sha256(args.lock)
        write_json(args.output, data)


if __name__ == "__main__":
    try:
        main()
    except (ValueError, KeyError, OSError, subprocess.CalledProcessError) as error:
        raise SystemExit(str(error)) from error
