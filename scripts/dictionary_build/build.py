"""构建流程：按 Rust 构建器的阶段顺序生成四个产物，写清单与校验和，整套检查通过后再替换 MetasequoiaImeDict/out/。"""

from __future__ import annotations

import hashlib
import importlib.util
import json
import shutil
import sqlite3
import time
from contextlib import closing
from datetime import datetime, timezone
from pathlib import Path

from . import english_db, inputs, japanese_model, others_db, pinyin_db, symbols_db, textutil

ROOT = Path(__file__).resolve().parents[2]
DICT_ROOT = ROOT / "MetasequoiaImeDict"
OUT = DICT_ROOT / "out"
BACKUP = DICT_ROOT / "out.bak"
CACHE = DICT_ROOT / "cache" / "inputs"
NOTICE = DICT_ROOT / "source" / "mozc_dictionary_oss" / "README.txt"
MARKER = "local-dictionary.json"
REPOSITORY = "metasequoiaime/msime-dictionary"

# 随产物放在 out/ 里的声明：Mozc 要求分发模型时附上 README 与 LICENSE，SCOWL 要求每份拷贝附上它的版权声明。
NOTICE_FILES = ("mozc_dictionary_oss_README.txt", "mozc_LICENSE.txt", "scowl_Copyright.txt")

_spec = importlib.util.spec_from_file_location("dictionary_product", ROOT / "scripts" / "dictionary_product.py")
_product = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_product)


class Stages:
    """逐阶段打印耗时与结果，和 Rust 构建器的 [build]/[done] 输出对应。"""

    def __call__(self, name: str, action):
        print(f"[build] {name}", flush=True)
        started = time.perf_counter()
        summary = action()
        print(f"[done] {name} in {time.perf_counter() - started:.1f}s: {summary}", flush=True)
        return summary


def build_msime(source: inputs.Inputs, path: Path, stage: Stages) -> None:
    connection = sqlite3.connect(path)
    try:
        stage("quanpin", lambda: pinyin_db.build_quanpin(
            connection, pinyin_db.wanxiang_files(source.dictionary / pinyin_db.WANXIANG)))

        def words(relative: str):
            entries = pinyin_db.parse_word_list(textutil.read(source.source(relative)), relative)
            inserted, promoted, unchanged = pinyin_db.apply_words(connection, entries)
            return f"{len(entries)} entries: {inserted} inserted, {promoted} promoted, {unchanged} already at or above their weight"

        stage("custom-words", lambda: words("custom/words.txt"))

        def corrections():
            name = "pinyin-reading-corrections.txt"
            entries = pinyin_db.parse_reading_corrections(textutil.read(source.resource(name)), name)
            removed, stale, kept = pinyin_db.apply_reading_corrections(connection, entries)
            return (f"{len(entries)} entries: {removed} rows removed, {stale} with no wrong row, "
                    f"{kept} kept for lack of a row at the correct reading")

        stage("reading-corrections", corrections)

        def wubi():
            imported, skipped, outside = pinyin_db.build_wubi86(
                connection, [source.source("sources/wubi/wubi86-jidian.txt"), source.source("sources/wubi/wubi86-supplement.txt")])
            return f"{imported} rows imported, {skipped} skipped, {outside} outside the basic CJK set left out"

        stage("wubi", wubi)
        def quick_phrases():
            pinyin_db.build_quick_phrases(connection)
            return "empty table, no built-in phrases"

        stage("quick-phrases", quick_phrases)
        stage("japanese-lexicon", lambda: f"{pinyin_db.build_japanese_lexicon(connection, source.jp_sela)} entries")
        textutil.integrity_check(connection)
        with connection:
            textutil.analyze(connection, optimize=True)
    finally:
        connection.close()
    textutil.freeze(path)


def build_english(source: inputs.Inputs, path: Path, msime: Path, stage: Stages) -> bytes:
    """返回 SCOWL 的 Copyright 原文，它同时写进库里的 source_notices 和 out/ 下的 scowl_Copyright.txt。"""
    package = english_db.read_scowl_package(source.scowl)
    scowl_name = "sources/english/scowl-words.txt"
    scowl_words = english_db.parse_word_list(textutil.read(source.source(scowl_name)), scowl_name)
    connection = sqlite3.connect(path)
    try:
        def words():
            base = english_db.parse_base_dict_words(textutil.read(source.source("sources/english/rime-ice-en.txt")),
                                                    "rime-ice-en.txt")
            supplement = english_db.parse_base_dict_words(
                textutil.read(source.source("sources/english/rime-ice-en-supplement.txt")), "rime-ice-en-supplement.txt")
            supplement_added = english_db.merge_words(base, supplement)
            scowl_added = english_db.merge_words(base, {word: list(displays) for word, displays in scowl_words.items()})
            counts = english_db.parse_google_counts(textutil.read(source.source("sources/english/google-word-counts.txt")))
            custom = english_db.parse_custom_english(textutil.read(source.source("custom/english.txt")), "custom/english.txt")
            total, base_rows, added, replaced = english_db.build_english_words(
                connection, base, counts, english_db.letter_forms(package), custom)
            english_db.write_notices(connection, [(english_db.NOTICE_SOURCE, package.copyright.decode("utf-8"))])
            return (f"{total} words in {base_rows} rows ({supplement_added} from rime-ice supplement, {scowl_added} from SCOWL), "
                    f"{len(custom)} custom rows: {added} added, {replaced} replacing a base row")

        stage("english", words)

        def glosses():
            # 只由 SCOWL 带来的词不进中译英（scowl-words.txt 本来就没有 rime-ice 与自定义词）。
            with closing(sqlite3.connect(f"file:{msime}?mode=ro", uri=True)) as pinyin:
                en_zh, zh_en = english_db.derive_glosses(source.ecdict, connection, pinyin, set(scowl_words))
            english_db.write_glosses(connection, en_zh, zh_en)
            return f"{len(en_zh)} English-to-Chinese, {len(zh_en)} Chinese-to-English"

        stage("english-glosses", glosses)

        def cn_en():
            name = "sources/cn-en/machine-translations.txt"
            entries = english_db.parse_cn_en_translations(textutil.read(source.source(name)), name)
            replaced, added = english_db.apply_cn_en_translations(connection, entries)
            return f"{len(entries)} Chinese-to-English: {replaced} replacing an ECDICT gloss, {added} added"

        # 排在 ECDICT 释义之后、custom/translations.txt 之前：覆盖前者，再被后者覆盖。
        stage("cn-en-translations", cn_en)

        def translations():
            entries = english_db.parse_custom_translations(textutil.read(source.source("custom/translations.txt")),
                                                           "custom/translations.txt")
            english_db.apply_custom_translations(connection, entries)
            return f"{len(entries)} overrides"

        stage("custom-translations", translations)
    finally:
        connection.close()
    textutil.freeze(path)
    return package.copyright


def build_others(source: inputs.Inputs, path: Path, stage: Stages) -> None:
    pinyin = others_db.Pinyin(textutil.read(source.resource("pinyin-overrides.txt")), "pinyin-overrides.txt")
    connection = sqlite3.connect(path)
    try:
        def emoji():
            catalog = others_db.read_emoji_catalog(textutil.read(source.resource("emoji/emoji_catalog.txt")), "emoji_catalog.txt")
            zh = others_db.load_keyword_map(textutil.read(source.resource("emoji/emoji.txt")), "emoji.txt")
            en = others_db.load_keyword_map(textutil.read(source.resource("emoji/emoji_en.txt")), "emoji_en.txt")
            rows, keys = others_db.emoji_rows(pinyin, catalog, zh, en)
            return f"{len(rows)} emoji, {others_db.build_emoji(connection, rows, keys)} search keys"

        stage("emoji", emoji)

        def kaomoji():
            mapping = others_db.load_keyword_map(textutil.read(source.resource("kaomoji/kaomoji.txt")), "kaomoji.txt")
            rows, entries = others_db.build_kaomoji(connection, pinyin, mapping)
            return f"{entries} kaomoji, {rows} keyword rows"

        stage("kaomoji", kaomoji)

        def symbols():
            groups = symbols_db.parse_wanxiang(textutil.read(source.source(symbols_db.WANXIANG)))
            codex = symbols_db.parse_codex(textutil.read(source.source(symbols_db.CODEX)))
            excluded = symbols_db.parse_unrenderable(textutil.read(source.source(symbols_db.UNRENDERABLE)))
            readings = symbols_db.parse_readings(textutil.read(source.source(symbols_db.READINGS)))
            catalog = symbols_db.catalog_rows(pinyin, groups, codex, excluded)
            codes = symbols_db.code_rows(groups, readings, codex, excluded)
            symbols_db.build(connection, catalog, codes)
            return (f"{len(catalog)} panel symbols, {len(codes)} code rows from {len(groups)} groups, "
                    f"{len(readings)} pinyin readings and {len(codex)} codex names, "
                    f"{len(excluded)} unrenderable left out")

        stage("symbols", symbols)
    finally:
        connection.close()
    textutil.freeze(path)


def build_japanese(source: inputs.Inputs, path: Path, stage: Stages) -> None:
    def model():
        data, tokens, added, filtered = japanese_model.build(source.dictionary)
        path.write_bytes(data)
        return f"{tokens} tokens ({added} added from the aux dictionary and manual word lists, {filtered} base lines filtered)"

    stage("japanese-model", model)


def build_all(source: inputs.Inputs, staging: Path) -> None:
    stage = Stages()
    build_msime(source, staging / "msime.db", stage)
    copyright_text = build_english(source, staging / "english.db", staging / "msime.db", stage)
    build_others(source, staging / "others.db", stage)
    build_japanese(source, staging / "dict_japanese.dat", stage)
    shutil.copyfile(source.source(japanese_model.NOTICE), staging / "mozc_dictionary_oss_README.txt")
    shutil.copyfile(source.source(japanese_model.LICENSE), staging / "mozc_LICENSE.txt")
    (staging / "scowl_Copyright.txt").write_bytes(copyright_text)


def check_outputs(directory: Path) -> None:
    """与 Prepare-PackageFiles.ps1 和引擎读取方式对应的最低检查。"""
    # 连接要显式关掉，否则 Windows 上临时目录删不掉。
    with closing(sqlite3.connect(f"file:{directory / 'msime.db'}?mode=ro", uri=True)) as msime:
        tables = {row[0] for row in msime.execute("SELECT name FROM sqlite_master WHERE type='table'")}
        for table in ("wubi86", "japanese_lexicon", "tbl_1_a", "tbl_2_n", "tbl_others_s"):
            if table not in tables or not msime.execute(f'SELECT 1 FROM "{table}" LIMIT 1').fetchone():
                raise ValueError(f"msime.db 缺少表或表为空：{table}")
        # 快捷短语表必须在（用户快捷短语写在这里），但出厂必须是空的。
        if "quick_parases" not in tables:
            raise ValueError("msime.db 缺少表：quick_parases")
        if msime.execute('SELECT 1 FROM "quick_parases" LIMIT 1').fetchone():
            raise ValueError("msime.db 的 quick_parases 不应带内置快捷短语")
    with closing(sqlite3.connect(f"file:{directory / 'english.db'}?mode=ro", uri=True)) as english:
        columns = list(english.execute("PRAGMA table_info(english_words)"))
    if "weight" not in {c[1] for c in columns} or [c[1] for c in columns if c[5] > 0] != ["word", "display"]:
        raise ValueError("english.db 的 english_words schema 与 Windows 端不符")
    with (directory / "dict_japanese.dat").open("rb") as model:
        if model.read(8) != japanese_model.MAGIC:
            raise ValueError("dict_japanese.dat 头部不是 MSJPDT1")


def write_manifests(directory: Path, commit: str, dirty: bool, msime_commit: str) -> None:
    files = {name: {"sha256": inputs.sha256(directory / name), "size": (directory / name).stat().st_size}
             for name in sorted(_product.DESKTOP_FILES)}
    lock = json.loads(inputs.LOCK.read_text(encoding="utf-8"))
    # 与产品锁里旧词库的 dictionary-manifest.json 同一格式，dictionary_product.verify_product 能校验。
    manifest = {
        "manifest_version": 1,
        "profile": "desktop",
        "format_version": 1,
        "engine_compatibility": {"dictionary_format": 1, "japanese_model_magic": "MSJPDT1"},
        "source": {"repository": REPOSITORY, "path": ".", "commit": commit, "dirty": dirty},
        "sqlite_journal_mode": "delete",
        "format_contract_commit": msime_commit,
        "custom_dictionary_commit": commit,
        "custom_dictionary_repository": REPOSITORY,
        "custom_dictionary_path": "custom",
        "references": {
            "msime": {"repository": f"https://github.com/{lock['msime']['repository']}.git", "commit": msime_commit},
            **{name: {"url": entry["url"], "sha256": entry["sha256"]} for name, entry in lock["external"].items()},
        },
        "features": ["pinyin", "wubi", "quick_phrases", "english", "emoji", "kaomoji", "symbols", "japanese"],
        "files": files,
    }
    (directory / _product.MANIFEST_NAME).write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n",
                                                    encoding="utf-8")
    _product.verify_product(directory, "desktop")

    outputs = sorted(set(_product.DESKTOP_FILES) | set(NOTICE_FILES) | {_product.MANIFEST_NAME})
    (directory / "SHA256SUMS.txt").write_text(
        "".join(f"{inputs.sha256(directory / name)}  {name}\n" for name in outputs), encoding="utf-8")
    marker = {
        "built_from": "source",
        "repository": REPOSITORY,
        "custom_dictionary_commit": commit,
        "dirty": dirty,
        "msime_commit": msime_commit,
        "builder": builder_digest(),
        "built_at": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "files": {name: inputs.sha256(directory / name) for name in outputs + ["SHA256SUMS.txt"]},
    }
    (directory / MARKER).write_text(json.dumps(marker, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def builder_digest() -> str:
    """构建器自身（本目录的 .py 与 inputs.lock.json，加上入口脚本）的摘要：构建规则变了，同一份源数据也要重建。"""
    digest = hashlib.sha256()
    here = Path(__file__).resolve().parent
    for path in sorted([*here.glob("*.py"), here / "inputs.lock.json", ROOT / "scripts" / "build-dictionary.py"]):
        digest.update(path.name.encode("utf-8") + b"\0" + path.read_bytes().replace(b"\r\n", b"\n") + b"\0")
    return digest.hexdigest()


def stale_reason(commit: str, dirty: bool) -> str | None:
    """out/ 不是由这份 msime-dictionary 提交和当前构建器构建的就返回原因；工作区有未提交改动时总是重建。"""
    try:
        marker = verify_out()
    except ValueError as error:
        return str(error)
    if dirty or not commit:
        return "msime-dictionary 有未提交的改动"
    if marker.get("dirty"):
        return "上次构建时 msime-dictionary 有未提交的改动"
    if marker.get("custom_dictionary_commit") != commit:
        return f"msime-dictionary 已从 {marker.get('custom_dictionary_commit', '')[:12]} 变为 {commit[:12]}"
    if marker.get("builder") != builder_digest():
        return "构建脚本有改动"
    return None


def verify_out() -> dict:
    marker_path = OUT / MARKER
    if not marker_path.is_file():
        raise ValueError(f"{OUT} 不是 build-dictionary.py 构建的词库（没有 {MARKER}）")
    marker = json.loads(marker_path.read_text(encoding="utf-8"))
    for name, digest in marker["files"].items():
        path = OUT / name
        if not path.is_file() or inputs.sha256(path) != digest:
            raise ValueError(f"{name} 与 {MARKER} 记录的摘要不符，重新运行 build-dictionary.py")
    if not NOTICE.is_file():
        raise ValueError(f"缺少 {NOTICE}，重新运行 build-dictionary.py")
    return marker


def install(staging: Path) -> None:
    if OUT.exists():
        if not (OUT / MARKER).is_file() and not BACKUP.exists():
            # 产品锁的旧词库只备份一次，之后再构建不会把备份覆盖掉。
            print(f"备份旧词库到 {BACKUP}")
            shutil.copytree(OUT, BACKUP)
        shutil.rmtree(OUT)
    shutil.copytree(staging, OUT)
    NOTICE.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(OUT / "mozc_dictionary_oss_README.txt", NOTICE)
