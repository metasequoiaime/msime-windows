#!/usr/bin/env python3
"""生成 msime-dictionary 里只给本仓本地构建器读的两份英文数据，写到 checkout 的 sources/english/：

    hallelujahim-ipa.txt  英文候选的音标：hallelujahIM 词表里的 ipa 字段（CMUdict 经 eng_to_ipa 转写），
                          每行 `小写单词<TAB>音标`；eng_to_ipa 转不出来（带 *）和含空格的不收
    ecdict-phrases.txt    英文候选的短语：ECDICT 里全部纯 ASCII、带空格或连字符的条目，每行 `写法<TAB>释义`，
                          按字典序；释义按单词释义的规则挑两个中文词头，挑不出来时用第一行，没有释义只有写法

两份上游文件都按 URL、大小和 SHA-256 固定（ECDICT 用 dictionary_build/inputs.lock.json 里那一份），首次运行时下载到
MetasequoiaImeDict/cache/inputs/。文件头记下本仓提交，工作区 scripts/ 有未提交改动时带 -dirty，这样的文件不要提交。
msime 的 Rust 构建器不读这两份文件。

用法：
    python scripts/generate-english-extras.py                              # 默认写到与本仓同级的 msime-dictionary
    python scripts/generate-english-extras.py --dictionary D:/msime-dictionary
    python scripts/generate-english-extras.py --check                      # 只检查数据行是否与重新生成的一致
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from dictionary_build import build, english_db, inputs  # noqa: E402

HALLELUJAH = {
    "repository": "https://github.com/dongyuwei/hallelujahIM",
    "commit": "bce5580fecbdf88a80ce20f8393ceb346d251139",
    "path": "dictionary/words_with_frequency_and_translation_and_ipa.json",
    "size": 14582125,
    "sha256": "2342cca8ed2bf56ea4c82c157098d19719a5a8e643f7bac0392de288ba348830",
}
ECDICT_REPOSITORY = "https://github.com/skywind3000/ECDICT"


def generator_commit() -> str:
    root = build.ROOT
    commit = subprocess.run(["git", "-C", str(root), "rev-parse", "HEAD"], capture_output=True, text=True,
                            check=True).stdout.strip()
    dirty = subprocess.run(["git", "-C", str(root), "status", "--porcelain", "--", "scripts"], capture_output=True,
                           text=True, check=True).stdout.strip()
    return commit + ("-dirty" if dirty else "")


def ecdict_input() -> tuple[Path, str]:
    entry = json.loads(inputs.LOCK.read_text(encoding="utf-8"))["external"]["ecdict.csv"]
    path = inputs._ensure(build.CACHE / "ecdict.csv", entry["url"], entry["size"], entry["sha256"], "ecdict.csv")
    # https://raw.githubusercontent.com/skywind3000/ECDICT/<commit>/ecdict.csv
    return path, entry["url"].split("/")[-2]


def hallelujah_input() -> Path:
    url = f"https://raw.githubusercontent.com/dongyuwei/hallelujahIM/{HALLELUJAH['commit']}/{HALLELUJAH['path']}"
    return inputs._ensure(build.CACHE / "hallelujah-words-ipa.json", url, HALLELUJAH["size"], HALLELUJAH["sha256"],
                          "hallelujahIM words_with_frequency_and_translation_and_ipa.json")


def ipa_file(commit: str) -> tuple[list[str], list[str]]:
    entries = english_db.parse_hallelujah_ipa(hallelujah_input().read_bytes())
    header = [
        f"# 由 MSIME-Windows 仓库提交 {commit} 的 scripts/generate-english-extras.py 生成，不要手工编辑。",
        f"# 上游：{HALLELUJAH['repository']} 提交 {HALLELUJAH['commit']} 的 {HALLELUJAH['path']}"
        f"（SHA-256 {HALLELUJAH['sha256']}），GPL-3.0，作者 Dong Yuwei。",
        "# 只取 ipa 字段：hallelujahIM 用 English-to-IPA（MIT）转写 CMUdict（BSD-2-Clause）生成。词频与释义字段不用。",
        "# 规则：键转小写，只收纯 ASCII 字母的词；音标为空、带 *（eng_to_ipa 查不到的词）或含空格的不收。",
        "# 只由 MSIME-Windows 的本地构建器读（scripts/dictionary_build，english.db 的 en_ipa 表），msime 构建器不读。",
        f"# 格式：小写单词<TAB>音标，按单词排序。共 {len(entries)} 行。",
    ]
    return header, english_db.format_ipa(entries)


def phrases_file(commit: str) -> tuple[list[str], list[str]]:
    ecdict, ecdict_commit = ecdict_input()
    phrases = english_db.parse_ecdict_phrases(ecdict)
    with_gloss = sum(1 for phrase in phrases if phrase.gloss)
    header = [
        f"# 由 MSIME-Windows 仓库提交 {commit} 的 scripts/generate-english-extras.py 生成，不要手工编辑。",
        f"# 上游：{ECDICT_REPOSITORY} 提交 {ecdict_commit} 的 ecdict.csv，MIT。",
        "# 规则：收全部纯 ASCII、带空格或连字符的条目（不看词频），空白压成一个空格；去掉非字母后不足两个字母的、"
        "以 # 开头的不收。",
        "# 释义：按 english.db 单词释义的规则从 translation 挑至多两个中文词头，挑不出来时用第一行去掉领域和词性标记、"
        "截到 24 字。",
        "# 只由 MSIME-Windows 的本地构建器读（scripts/dictionary_build，english.db 的 english_phrases 表），msime 构建器不读。",
        f"# 格式：写法<TAB>释义（没有释义时只有写法），按字典序（不分大小写）。共 {len(phrases)} 行，{with_gloss} 行有释义。",
    ]
    return header, english_db.format_phrases(phrases)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dictionary", type=Path, default=None,
                        help="msime-dictionary checkout，默认取环境变量 MSIME_DICTIONARY，否则是本仓同级的 msime-dictionary")
    parser.add_argument("--check", action="store_true", help="只检查两份文件的数据行是否与重新生成的一致，不写文件")
    args = parser.parse_args()
    dictionary = (args.dictionary or Path(os.environ.get("MSIME_DICTIONARY") or build.ROOT.parent / "msime-dictionary"))
    dictionary = dictionary.resolve()
    if not (dictionary / "sources" / "english").is_dir():
        raise SystemExit(f"{dictionary} 不是 msime-dictionary checkout（没有 sources/english/），用 --dictionary 指定")

    commit = generator_commit()
    stale = []
    for relative, generate in ((english_db.IPA_FILE, ipa_file), (english_db.PHRASES_FILE, phrases_file)):
        header, rows = generate(commit)
        target = dictionary / relative
        if args.check:
            existing = target.read_text(encoding="utf-8").splitlines() if target.is_file() else []
            if [line for line in existing if line and not line.startswith("#")] != rows:
                stale.append(relative)
            continue
        target.write_bytes(("\n".join(header + rows) + "\n").encode("utf-8"))
        print(f"写入 {target}（{len(rows)} 行）")
    if stale:
        raise SystemExit(f"与重新生成的不一致：{', '.join(stale)}")
    if args.check:
        print("两份文件都是最新的")


if __name__ == "__main__":
    main()
