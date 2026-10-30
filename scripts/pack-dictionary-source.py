#!/usr/bin/env python3
"""把构建好的词库（MetasequoiaImeDict/out/ 下的 msime.db、english.db、others.db、dict_japanese.dat）导出成
安装时由 MetasequoiaImeDictionaryBuild.exe 还原的文本源。

SQLite 的页结构和索引压缩率很差，同样的内容按行写成文本，安装包里能小一大截。这里只做机械导出，
不重新解释数据：拼音去调、权重默认值、纠错这些规则仍然只在 build-dictionary.py 里。

数据库导出格式（UTF-8，只用 \\n 换行），每库一个 <库名>.dump：

    MSIME-DB-DUMP 1
    P<TAB>PRAGMA ...                     建表前执行（user_version 等）
    S<TAB>CREATE TABLE ...               执行一条 SQL（转义见下）
    T<TAB>表名<TAB>列编码<TAB>行数         其后紧跟「行数」行数据，字段以 TAB 分隔
    S<TAB>CREATE INDEX ...               数据之后再建索引、触发器、视图，最后原样写回 sqlite_stat1

列编码每列一个字母：i 整数、t 文本、x 逐值带类型（首字母 n 空、i 整数、r 浮点、t 文本、b 十六进制 BLOB）。
文本把 \\、TAB、\\n、\\r、NUL 转义成 \\\\、\\t、\\n、\\r、\\0。行按 rowid 顺序写出，还原后同权重的先后不变。

日文模型（MSJPDT1）拆成 dict_japanese.tokens（文本：头一行「MSIME-JP-DUMP 1」，第二行「矩阵边长 词条数 原文件 SHA-256」，
其后每行「读音<TAB>表层<TAB>左 id<TAB>右 id<TAB>代价」）和 dict_japanese.connection（原样的 i16 小端连接矩阵）。
还原程序按同样的规则重新打包，并核对 SHA-256 与原文件一致。

用法：
    python scripts/pack-dictionary-source.py --out MetasequoiaImeDict/out --target installer/dictionary_source
"""

from __future__ import annotations

import argparse
import hashlib
import shutil
import sqlite3
import struct
import sys
from contextlib import closing
from pathlib import Path

DATABASES = ("msime.db", "english.db", "others.db")
JAPANESE = "dict_japanese.dat"
JAPANESE_MAGIC = b"MSJPDT1\0"
# 建表前要恢复的库级设置；page_size 与发布的库一致，user_version 是 english.db 的 schema 版本号。
PRAGMAS = ("page_size", "user_version", "application_id")

_ESCAPES = str.maketrans({"\\": "\\\\", "\t": "\\t", "\n": "\\n", "\r": "\\r", "\0": "\\0"})


def escape(text: str) -> str:
    return text.translate(_ESCAPES)


def _tagged(value) -> str:
    if value is None:
        return "n"
    if isinstance(value, int):
        return f"i{value}"
    if isinstance(value, float):
        return f"r{value!r}"
    if isinstance(value, bytes):
        return f"b{value.hex()}"
    return f"t{escape(value)}"


def _codecs(connection: sqlite3.Connection, table: str, columns: int) -> str:
    """每列的值都是整数就用 i，都是文本就用 t，否则逐值带类型。"""
    codecs = []
    for index in range(columns):
        kinds = {row[0] for row in connection.execute(f'SELECT DISTINCT typeof("{_column(connection, table, index)}") FROM "{table}"')}
        codecs.append("i" if kinds <= {"integer"} else "t" if kinds <= {"text"} else "x")
    return "".join(codecs)


def _column(connection: sqlite3.Connection, table: str, index: int) -> str:
    return [row[1] for row in connection.execute(f'PRAGMA table_xinfo("{table}")') if row[6] == 0][index]


def _has_rowid(connection: sqlite3.Connection, table: str) -> bool:
    try:
        connection.execute(f'SELECT rowid FROM "{table}" LIMIT 0')
        return True
    except sqlite3.OperationalError:
        return False


def dump_database(path: Path, target: Path) -> int:
    rows_written = 0
    with closing(sqlite3.connect(f"file:{path}?mode=ro", uri=True)) as connection, \
            target.open("w", encoding="utf-8", newline="\n") as out:
        out.write("MSIME-DB-DUMP 1\n")
        for pragma in PRAGMAS:
            out.write(f"P\tPRAGMA {pragma}={connection.execute(f'PRAGMA {pragma}').fetchone()[0]}\n")
        schema = connection.execute(
            "SELECT type, name, sql FROM sqlite_master WHERE sql IS NOT NULL AND name NOT LIKE 'sqlite\\_%' ESCAPE '\\' "
            "ORDER BY rowid").fetchall()
        tables = [(name, sql) for kind, name, sql in schema if kind == "table"]
        for name, sql in tables:
            out.write(f"S\t{escape(sql)}\n")
        for name, _ in tables:
            columns = len([row for row in connection.execute(f'PRAGMA table_xinfo("{name}")') if row[6] == 0])
            count = connection.execute(f'SELECT count(*) FROM "{name}"').fetchone()[0]
            codecs = _codecs(connection, name, columns)
            out.write(f"T\t{escape(name)}\t{codecs}\t{count}\n")
            order = " ORDER BY rowid" if _has_rowid(connection, name) else ""
            for row in connection.execute(f'SELECT * FROM "{name}"{order}'):
                fields = []
                for codec, value in zip(codecs, row):
                    fields.append(str(value) if codec == "i" else escape(value) if codec == "t" else _tagged(value))
                out.write("\t".join(fields))
                out.write("\n")
            rows_written += count
        for kind, name, sql in schema:
            if kind != "table":
                out.write(f"S\t{escape(sql)}\n")
        # 统计信息原样搬过去，不在安装时重跑 ANALYZE：省时间，查询计划也与构建出的库一致。
        if connection.execute("SELECT 1 FROM sqlite_master WHERE name = 'sqlite_stat1'").fetchone():
            # sqlite_stat1 不许直接 CREATE；ANALYZE sqlite_schema 只建出这张表，不扫用户表。
            out.write("S\tANALYZE sqlite_schema\nS\tDELETE FROM sqlite_stat1\n")
            for literals in connection.execute("SELECT quote(tbl), quote(idx), quote(stat) FROM sqlite_stat1 ORDER BY rowid"):
                out.write(f"S\t{escape(f'INSERT INTO sqlite_stat1 VALUES ({literals[0]}, {literals[1]}, {literals[2]})')}\n")
    return rows_written


def dump_japanese(path: Path, tokens_target: Path, connection_target: Path) -> int:
    """按 japanese_model.pack 的布局读回词条和连接矩阵。"""
    data = path.read_bytes()
    if data[:8] != JAPANESE_MAGIC:
        raise ValueError(f"{path} 头部不是 MSJPDT1")
    version, count, size, _ = struct.unpack_from("<IIII", data, 8)
    token_offset, connection_offset, string_offset, string_size = struct.unpack_from("<QQQQ", data, 24)
    if version != 1 or token_offset != 56 or connection_offset != token_offset + count * 20 \
            or string_offset != connection_offset + size * size * 2 or string_offset + string_size != len(data):
        raise ValueError(f"{path} 的布局与 MSJPDT1 版本 1 不符")
    strings = data[string_offset:]
    record = struct.Struct("<IHIHHHi")
    with tokens_target.open("w", encoding="utf-8", newline="\n") as out:
        out.write("MSIME-JP-DUMP 1\n")
        out.write(f"{size} {count} {hashlib.sha256(data).hexdigest()}\n")
        for index in range(count):
            reading_at, reading_length, surface_at, surface_length, left, right, cost = record.unpack_from(
                data, token_offset + index * 20)
            reading = strings[reading_at:reading_at + reading_length].decode("utf-8")
            surface = strings[surface_at:surface_at + surface_length].decode("utf-8")
            out.write(f"{escape(reading)}\t{escape(surface)}\t{left}\t{right}\t{cost}\n")
    connection_target.write_bytes(data[connection_offset:string_offset])
    return count


def main() -> None:
    # CI 的 Windows runner 上 stdout 是 cp1252 管道，打印中文会抛 UnicodeEncodeError，让整个打包失败。
    # 进度提示只是日志，编码不了的字符写成 \uXXXX 即可。
    for stream in (sys.stdout, sys.stderr):
        stream.reconfigure(errors="backslashreplace")
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", type=Path, required=True, help="构建好的词库目录（MetasequoiaImeDict/out）")
    parser.add_argument("--target", type=Path, required=True, help="导出目录，已有内容会被清空")
    args = parser.parse_args()

    if args.target.exists():
        shutil.rmtree(args.target)
    args.target.mkdir(parents=True)
    for name in DATABASES:
        rows = dump_database(args.out / name, args.target / f"{Path(name).stem}.dump")
        print(f"{name}: {rows} 行")
    tokens = dump_japanese(args.out / JAPANESE, args.target / "dict_japanese.tokens",
                           args.target / "dict_japanese.connection")
    print(f"{JAPANESE}: {tokens} 个词条")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, sqlite3.Error) as error:
        raise SystemExit(f"导出词库源失败：{error}") from error
