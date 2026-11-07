"""Build the multi-language local candidate-gloss pack for msime.

Reads every Chinese headword from english.db's zh_en_glosses, translates the
existing English gloss into the eight other target languages with a Tencent
TokenHub chat model, and merges the result into a copy of english.db as

    zh_glosses(chinese TEXT, lang TEXT, gloss TEXT, PRIMARY KEY(chinese, lang))

so the IME can answer a candidate gloss locally for every target language and
only fall back to the online provider for words that are genuinely missing.

Usage:
    python build_local_glosses.py --source english.db --out english.local.db \
        --langs fr,ja,es,ru,de,ko,th,vi,it --workers 8 [--limit N] [--resume]
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sqlite3
import sys
import threading
import time
import urllib.error
import urllib.request
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

LANG_NAMES = {
    "fr": "French",
    "ja": "Japanese",
    "es": "Spanish",
    "ru": "Russian",
    "de": "German",
    "ko": "Korean",
    "th": "Thai",
    "vi": "Vietnamese",
    "it": "Italian",
}

SYSTEM = (
    "You are a dictionary glossing engine for a Chinese input method. "
    "Each numbered line contains one English dictionary gloss for a Chinese word; a gloss may hold "
    "several senses separated by ';'. Translate it into {lang}. Rules: keep at most two senses and "
    "join them with '; '; drop part-of-speech labels; never add explanations, quotes or numbering; "
    "stay as short as a candidate-window hint. "
    "Answer with one entry per input line in the form <number>|<translation> and nothing else."
)

MAX_GLOSS_CHARS = 40


def parse_numbered(text: str, expected: int) -> dict[int, str]:
    """The model often joins every entry on one line with '|', so slice by the
    positions of the '<n>|' markers rather than by lines."""
    marks = [(m.start(), m.end(), int(m.group(1))) for m in re.finditer(r"(\d{1,4})\s*[|>｜]", text)]
    starts: dict[int, int] = {}
    for index, (_, end, number) in enumerate(marks):
        if 1 <= number <= expected and number not in starts:
            starts[number] = index
    ordered = sorted(starts.items(), key=lambda item: item[1])
    out: dict[int, str] = {}
    for position, (number, mark_index) in enumerate(ordered):
        begin = marks[mark_index][1]
        end = marks[ordered[position + 1][1]][0] if position + 1 < len(ordered) else len(text)
        value = text[begin:end].strip().strip("|;>").strip()
        value = re.sub(r"[\t\r\n]+", " ", value)
        if not value:
            continue
        if len(value) > MAX_GLOSS_CHARS:
            cut = value[:MAX_GLOSS_CHARS]
            space = cut.rfind(" ")
            value = (cut[:space] if space > 12 else cut).rstrip(" ;,.")
        out[number] = value
    return out


class Client:
    def __init__(self, base: str, key: str, model: str) -> None:
        self.base = base.rstrip("/")
        self.key = key
        self.model = model
        self.lock = threading.Lock()
        self.requests = 0
        self.tokens = 0

    def chat(self, system: str, user: str, timeout: float = 180.0) -> str:
        payload = {
            "model": self.model,
            "messages": [{"role": "system", "content": system}, {"role": "user", "content": user}],
            "temperature": 0,
            "max_tokens": 8192,
            "stream": False,
        }
        data = json.dumps(payload).encode()
        last: Exception | None = None
        for attempt in range(4):
            request = urllib.request.Request(
                self.base + "/chat/completions",
                data=data,
                headers={"Authorization": "Bearer " + self.key, "Content-Type": "application/json"},
            )
            try:
                with urllib.request.urlopen(request, timeout=timeout) as response:
                    body = json.loads(response.read().decode("utf-8", "replace"))
                with self.lock:
                    self.requests += 1
                    self.tokens += int(body.get("usage", {}).get("total_tokens", 0) or 0)
                return body["choices"][0]["message"]["content"]
            except urllib.error.HTTPError as error:
                last = error
                if error.code in (429, 500, 502, 503, 504):
                    time.sleep(2 + attempt * 3)
                    continue
                raise
            except Exception as error:  # noqa: BLE001  (timeouts, resets)
                last = error
                time.sleep(2 + attempt * 3)
        raise last if last else RuntimeError("chat failed")


def translate_batch(client: Client, language: str, items: list[tuple[str, str]]) -> dict[str, str]:
    system = SYSTEM.format(lang=LANG_NAMES[language])
    user = "\n".join(f"{index}|{gloss}" for index, (_, gloss) in enumerate(items, start=1))
    text = client.chat(system, user)
    parsed = parse_numbered(text, len(items))
    done = {items[index - 1][0]: value for index, value in parsed.items()}
    missing = [entry for entry in items if entry[0] not in done]
    if missing and len(missing) < len(items):
        # Second pass for the gaps, as a smaller batch.
        user = "\n".join(f"{index}|{gloss}" for index, (_, gloss) in enumerate(missing, start=1))
        try:
            parsed = parse_numbered(client.chat(system, user), len(missing))
        except Exception:  # noqa: BLE001
            parsed = {}
        for index, value in parsed.items():
            done[missing[index - 1][0]] = value
    return done


def load_done(path: Path) -> dict[str, set[str]]:
    done: dict[str, set[str]] = {}
    if not path.exists():
        return done
    for line in path.read_text(encoding="utf-8").splitlines():
        fields = line.split("\t")
        if len(fields) != 3:
            continue
        done.setdefault(fields[1], set()).add(fields[0])
    return done


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True)
    parser.add_argument("--out", default="", help="仅作记录；真正写库的是 merge_glosses.py")
    parser.add_argument("--tsv", default="glosses.tsv")
    parser.add_argument("--langs", default="fr,ja,es,ru,de,ko,th,vi,it")
    parser.add_argument("--workers", type=int, default=8)
    parser.add_argument("--batch", type=int, default=120)
    parser.add_argument("--limit", type=int, default=0)
    parser.add_argument("--order", choices=("alpha", "weight"), default="alpha",
                        help="alpha = 按词头字典序；weight = 按拼音词典词频权重降序（高频优先）")
    parser.add_argument("--msime", default="", help="--order weight 时要读的 msime.db")
    parser.add_argument("--model", default=os.environ.get("OH_MODEL", "hy-mt2-pro"))
    parser.add_argument("--base", default=os.environ.get("OH_BASE", "https://tokenhub.tencentmaas.com/v1"))
    args = parser.parse_args()

    key = os.environ.get("OH_KEY", "").strip()
    if not key:
        sys.exit("OH_KEY is not set")

    source = sqlite3.connect(f"file:{args.source}?mode=ro", uri=True)
    rows = source.execute("SELECT chinese, english_gloss FROM zh_en_glosses").fetchall()
    if args.order == "weight":
        if not args.msime:
            sys.exit("--order weight needs --msime")
        msime = sqlite3.connect(f"file:{args.msime}?mode=ro", uri=True)
        wanted = {row[0] for row in rows}
        weights: dict[str, int] = {}
        tables = [r[0] for r in msime.execute(
            "SELECT name FROM sqlite_master WHERE type='table' AND name GLOB 'tbl_*_[a-z]'")]
        for table in tables:
            for value, weight in msime.execute(f'SELECT value,weight FROM "{table}"'):
                if value in wanted and weight and weight > weights.get(value, 0):
                    weights[value] = weight
        rows.sort(key=lambda row: (-weights.get(row[0], 0), row[0]))
        covered = sum(1 for row in rows if row[0] in weights)
        print(f"ranked by pinyin weight: {covered} of {len(rows)} headwords carry one", flush=True)
    else:
        rows.sort()
    if args.limit:
        rows = rows[: args.limit]
    print(f"headwords: {len(rows)}", flush=True)

    tsv = Path(args.tsv)
    done = load_done(tsv)
    languages = [code.strip() for code in args.langs.split(",") if code.strip()]

    tasks: list[tuple[str, list[tuple[str, str]]]] = []
    for language in languages:
        have = done.get(language, set())
        pending = [row for row in rows if row[0] not in have]
        for start in range(0, len(pending), args.batch):
            tasks.append((language, pending[start : start + args.batch]))
        print(f"{language}: {len(have)} cached, {len(pending)} pending, "
              f"{(len(pending) + args.batch - 1) // args.batch} requests", flush=True)

    client = Client(args.base, key, args.model)
    started = time.time()
    finished = 0
    failures = 0
    lock = threading.Lock()
    out = tsv.open("a", encoding="utf-8", buffering=1)

    def run(task: tuple[str, list[tuple[str, str]]]) -> tuple[str, dict[str, str]]:
        language, items = task
        return language, translate_batch(client, language, items)

    try:
        with ThreadPoolExecutor(max_workers=args.workers) as pool:
            futures = {pool.submit(run, task): task for task in tasks}
            for future in as_completed(futures):
                language, items = futures[future]
                try:
                    _, translated = future.result()
                except Exception as error:  # noqa: BLE001
                    failures += 1
                    print(f"! {language} batch of {len(items)} failed: {type(error).__name__} {error}", flush=True)
                    continue
                with lock:
                    for chinese, _ in items:
                        gloss = translated.get(chinese)
                        if gloss:
                            out.write(f"{chinese}\t{language}\t{gloss}\n")
                    finished += 1
                    if finished % 20 == 0:
                        rate = finished * args.batch / max(time.time() - started, 1)
                        print(f"  {finished}/{len(tasks)} batches  {rate:.0f} entries/s  "
                              f"tokens {client.tokens}", flush=True)
    finally:
        out.close()

    covered = sum(len(load_done(tsv).get(code, set())) for code in languages)
    elapsed = time.time() - started
    print(f"\ndone in {elapsed / 60:.1f} min  batches {finished}/{len(tasks)}  failures {failures}  "
          f"requests {client.requests}  tokens {client.tokens}", flush=True)
    print(f"gloss rows now: {covered} (target {len(rows) * len(languages)})", flush=True)


if __name__ == "__main__":
    main()
