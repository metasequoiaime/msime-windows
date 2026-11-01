"""Merge glosses.tsv into a copy of english.db as the zh_glosses table.

    zh_glosses(chinese TEXT, lang TEXT, gloss TEXT, PRIMARY KEY(chinese, lang))

Only headwords that already exist in zh_en_glosses are kept, so the local pack
can never answer for a word the Chinese dictionary cannot produce.

Usage:
    python merge_glosses.py --source english.db --tsv glosses.tsv --out english.local.db
"""

from __future__ import annotations

import argparse
import shutil
import sqlite3
from collections import Counter
from pathlib import Path

CREATE = (
    "CREATE TABLE zh_glosses ("
    "chinese TEXT COLLATE BINARY NOT NULL,"
    "lang TEXT COLLATE BINARY NOT NULL,"
    "gloss TEXT NOT NULL,"
    "PRIMARY KEY(chinese, lang)) WITHOUT ROWID"
)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True)
    parser.add_argument("--tsv", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--version", type=int, default=4)
    args = parser.parse_args()

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(args.source, out)

    connection = sqlite3.connect(out)
    known = {row[0] for row in connection.execute("SELECT chinese FROM zh_en_glosses")}

    rows: list[tuple[str, str, str]] = []
    skipped_unknown = 0
    seen: set[tuple[str, str]] = set()
    with open(args.tsv, encoding="utf-8") as handle:
        for line in handle:
            fields = line.rstrip("\n").split("\t")
            if len(fields) != 3:
                continue
            chinese, language, gloss = (field.strip() for field in fields)
            if not chinese or not language or not gloss:
                continue
            if chinese not in known:
                skipped_unknown += 1
                continue
            if (chinese, language) in seen:
                continue
            seen.add((chinese, language))
            rows.append((chinese, language, gloss))

    with connection:
        connection.execute("DROP TABLE IF EXISTS zh_glosses")
        connection.execute(CREATE)
        connection.executemany("INSERT INTO zh_glosses(chinese, lang, gloss) VALUES(?,?,?)", rows)
        connection.execute(f"PRAGMA user_version={args.version}")
    connection.execute("ANALYZE")
    connection.commit()

    counts = Counter(language for _, language, _ in rows)
    integrity = connection.execute("PRAGMA integrity_check").fetchone()[0]
    print(f"headwords in zh_en_glosses: {len(known)}")
    print(f"gloss rows written: {len(rows)}  (skipped {skipped_unknown} rows for unknown headwords)")
    for language, count in sorted(counts.items()):
        print(f"  {language}: {count}")
    print(f"integrity: {integrity}")
    print(f"size: {out.stat().st_size / 1048576:.1f} MB")
    print(f"user_version: {connection.execute('PRAGMA user_version').fetchone()[0]}")
    connection.close()


if __name__ == "__main__":
    main()
