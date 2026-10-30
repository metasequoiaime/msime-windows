#!/usr/bin/env python3
"""从 msime-dictionary 的文本源数据本地构建 Windows 端词库，写到 MetasequoiaImeDict/out/。

读取 msime-dictionary checkout 的 sources/ 与 custom/，加上 scripts/dictionary_build/inputs.lock.json 固定的
仓外文件（msime 的读音纠错、emoji/颜文字表，ECDICT，SCOWL，rime-jp_sela），产出：

    msime.db           全拼表（sources/pinyin/wanxiang/ 的万象词库去调后导入，含自定义词条、读音纠错）、86 五笔、空的快捷短语表（出厂不带内置短语）、japanese_lexicon
    english.db         英文候选、中英双向释义（ECDICT）、sources/cn-en/ 的中译英释义（全部义项）、custom/translations.txt 覆盖
    others.db          emoji、颜文字、符号（sources/symbols/ 的万象符号表，剔除 wanxiang-unrenderable.txt 列出的）
    dict_japanese.dat  日文整句模型（sources/japanese/ 的 Mozc 词库）
    以及 Mozc 与 SCOWL 的声明、dictionary-manifest.json、SHA256SUMS.txt、local-dictionary.json

构建规则移植自 msime 的 Rust 构建器（crates/dict-builder）。--compare-release 把结果与官方 Release 逐表对比。
替换 out/ 前，如果里面是产品锁钉住的旧词库，先整份备份到 MetasequoiaImeDict/out.bak/。

用法：
    python scripts/build-dictionary.py                                  # 默认读与本仓同级的 msime-dictionary
    python scripts/build-dictionary.py --dictionary D:/msime-dictionary
    python scripts/build-dictionary.py --compare-release dict-v2.0.13   # 构建后与官方 Release 逐表对比
    python scripts/build-dictionary.py --verify                         # 只校验 out/ 是否仍是本脚本构建的结果
    python scripts/build-dictionary.py --if-stale                       # 源数据或构建脚本没变就跳过（installer\test.ps1 用）

读万象词库与构建 others.db 需要 pypinyin（python -m pip install pypinyin）。
"""

from __future__ import annotations

import argparse
import os
import sqlite3
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from dictionary_build import build, compare, inputs  # noqa: E402


def default_dictionary() -> Path:
    if os.environ.get("MSIME_DICTIONARY"):
        return Path(os.environ["MSIME_DICTIONARY"])
    return build.ROOT.parent / "msime-dictionary"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dictionary", type=Path, default=None,
                        help="msime-dictionary checkout，默认取环境变量 MSIME_DICTIONARY，否则是本仓同级的 msime-dictionary")
    parser.add_argument("--msime-resources", type=Path, default=None,
                        help="改用这个目录下的 msime resources/dictionary-sources 文件（本地修改用），不按锁校验")
    parser.add_argument("--compare-release", metavar="TAG", help="构建后与这个 dict-v* Release 逐表对比，不一致时失败")
    parser.add_argument("--verify", action="store_true", help="只校验 out/ 是否仍是本脚本构建的结果")
    parser.add_argument("--if-stale", action="store_true",
                        help="out/ 已由当前 msime-dictionary 提交和当前构建脚本构建、且工作区干净时跳过，否则重新构建")
    args = parser.parse_args()

    if args.verify:
        marker = build.verify_out()
        state = "，有未提交改动" if marker.get("dirty") else ""
        commit = marker["custom_dictionary_commit"][:12] or "不是 Git 工作区"
        print(f"词库已就绪：{build.OUT}（msime-dictionary {commit}{state}）")
        return

    dictionary = (args.dictionary or default_dictionary()).resolve()
    if not (dictionary / "sources").is_dir() or not (dictionary / "custom").is_dir():
        raise SystemExit(f"{dictionary} 不是 msime-dictionary checkout（没有 sources/ 和 custom/），用 --dictionary 指定")
    commit, dirty = inputs.git_state(dictionary)
    print(f"msime-dictionary：{dictionary}（{commit[:12] or '不是 Git 工作区'}{'，有未提交改动' if dirty else ''}）")
    if args.if_stale and not args.compare_release:
        reason = build.stale_reason(commit, dirty or args.msime_resources is not None)
        if reason is None:
            print(f"词库已是最新：{build.OUT}")
            return
        print(f"重新构建词库：{reason}")
    mismatched = inputs.check_upstream(dictionary)
    if mismatched:
        print(f"提示：{len(mismatched)} 个文件与 upstream.lock.json 记录的不同（本地改过的上游文件或生成表）："
              f"{', '.join(mismatched[:5])}{' ...' if len(mismatched) > 5 else ''}")

    source = inputs.fetch(dictionary, build.CACHE, args.msime_resources)
    started = time.perf_counter()
    build.DICT_ROOT.mkdir(parents=True, exist_ok=True)
    # 整套构建并检查通过之前不动已有的 out/。
    with tempfile.TemporaryDirectory(dir=build.DICT_ROOT, prefix="incoming-") as temporary:
        staging = Path(temporary)
        build.build_all(source, staging)
        build.check_outputs(staging)
        build.write_manifests(staging, commit, dirty or args.msime_resources is not None, source.msime_commit)
        if args.compare_release:
            problems = compare.compare(staging, args.compare_release, build.DICT_ROOT / "cache", commit,
                                       source.msime_commit)
            for problem in problems:
                print(f"  不一致：{problem}")
            if problems:
                raise SystemExit(f"与 {args.compare_release} 不一致，out/ 未替换")
        build.install(staging)
    build.verify_out()
    print(f"词库构建完毕（{time.perf_counter() - started:.0f}s）：{build.OUT}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, sqlite3.Error, subprocess.CalledProcessError) as error:
        raise SystemExit(f"失败：{error}") from error
