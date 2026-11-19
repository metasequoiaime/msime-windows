"""构建输入：msime-dictionary checkout，以及 inputs.lock.json 按大小和 SHA-256 固定、首次使用时下载并缓存的仓外文件。

仓外文件有三类：msime 仓库 resources/dictionary-sources/ 下人工维护的读音纠错、emoji 和颜文字
（固定在 Release 所用的 msime 提交；符号表改在 msime-dictionary 的 sources/symbols/）；ECDICT 与 SCOWL Aspell 包（Rust 构建器的锁文件同样固定它们）；
rime-jp_sela 的 jp_sela.dict.yaml（旧 msime.db 的 japanese_lexicon 来源）。
英文的音标和短语不在这里下载：它们由 scripts/generate-english-extras.py 生成进 msime-dictionary 的
sources/english/，构建时和其他源数据一样从 checkout 读。
"""

from __future__ import annotations

import hashlib
import json
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

LOCK = Path(__file__).with_name("inputs.lock.json")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def download(url: str, target: Path) -> None:
    """curl 断点续传加重试；raw.githubusercontent.com 偶尔握手失败，多试几轮。"""
    target.parent.mkdir(parents=True, exist_ok=True)
    partial = target.with_name(target.name + ".part")
    curl = ["curl.exe" if sys.platform == "win32" else "curl", "-L", "--fail", "--progress-bar",
            "--retry", "5", "--retry-delay", "3", "--retry-all-errors"]
    for _ in range(3):
        if subprocess.run(curl + ["-C", "-", "-o", str(partial), url]).returncode == 0:
            partial.replace(target)
            return
    # 续传本身失败（比如 .part 已经完整时服务器回 416），丢掉 .part 从头再下一次。
    partial.unlink(missing_ok=True)
    subprocess.run(curl + ["-o", str(partial), url], check=True)
    partial.replace(target)


def _ensure(path: Path, url: str, size: int, digest: str, label: str) -> Path:
    if path.is_file() and path.stat().st_size == size and sha256(path) == digest:
        return path
    print(f"下载 {label} ...")
    download(url, path)
    if path.stat().st_size != size or sha256(path) != digest:
        raise ValueError(f"{label} 的大小或 SHA-256 与 {LOCK.name} 不符")
    return path


@dataclass
class Inputs:
    """所有输入文件的本地路径。"""
    dictionary: Path
    msime_commit: str
    msime_files: dict[str, Path]
    ecdict: Path
    scowl: Path
    jp_sela: Path

    def source(self, relative: str) -> Path:
        path = self.dictionary / relative
        if not path.is_file():
            raise FileNotFoundError(f"msime-dictionary checkout 缺少 {relative}")
        return path

    def resource(self, relative: str) -> Path:
        return self.msime_files[relative]


def fetch(dictionary: Path, cache: Path, resources: Path | None) -> Inputs:
    """resources 给出时直接读这个目录下的 msime resources/dictionary-sources 文件（本地修改用），不按锁校验。"""
    lock = json.loads(LOCK.read_text(encoding="utf-8"))
    msime = lock["msime"]
    files = {}
    for relative, entry in msime["files"].items():
        if resources is not None:
            path = resources / relative
            if not path.is_file():
                raise FileNotFoundError(f"--msime-resources 目录缺少 {relative}")
            files[relative] = path
            continue
        url = f"https://raw.githubusercontent.com/{msime['repository']}/{msime['commit']}/{msime['path']}/{relative}"
        files[relative] = _ensure(cache / "msime" / msime["commit"][:12] / relative, url, entry["size"],
                                  entry["sha256"], f"msime/{relative}")
    external = {name: _ensure(cache / name, entry["url"], entry["size"], entry["sha256"], name)
                for name, entry in lock["external"].items()}
    return Inputs(dictionary, msime["commit"], files, external["ecdict.csv"],
                  external["aspell6-en-2026.02.25-0.tar.bz2"], external["jp_sela.dict.yaml"])


def git_state(checkout: Path) -> tuple[str, bool]:
    """checkout 的 HEAD 提交，以及 sources/、custom/、upstream.lock.json 是否有未提交的改动。"""
    try:
        commit = subprocess.run(["git", "-C", str(checkout), "rev-parse", "HEAD"], capture_output=True, text=True,
                                check=True).stdout.strip()
        status = subprocess.run(["git", "-C", str(checkout), "status", "--porcelain", "--", "sources", "custom",
                                 "upstream.lock.json"], capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return "", True
    return commit, bool(status.strip())


def check_upstream(checkout: Path) -> list[str]:
    """upstream.lock.json 按大小和 SHA-256 记录上游原样文件与生成的补充表。返回对不上的路径（只提示，不拦：本地版允许改）。"""
    lock_path = checkout / "upstream.lock.json"
    if not lock_path.is_file():
        return ["upstream.lock.json 不存在"]
    lock = json.loads(lock_path.read_text(encoding="utf-8"))
    entries = lock.get("files", lock)
    problems = []
    for entry in entries if isinstance(entries, list) else []:
        path = checkout / entry["path"]
        if not path.is_file():
            problems.append(f"{entry['path']}（缺失）")
        elif path.stat().st_size != entry.get("size") or sha256(path) != entry.get("sha256"):
            problems.append(entry["path"])
    return problems
