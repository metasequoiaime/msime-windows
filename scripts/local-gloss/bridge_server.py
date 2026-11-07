"""DeepLX 兼容的本地候选词翻译服务（水杉输入法 0.10.x 无需重新编译即可用）。

对外只暴露一个 POST /translate，请求体是水杉的自定义翻译协议：

    {"text": "银牌", "source_lang": "ZH", "target_lang": "FR"}
    ->  {"code": 200, "data": "médaille d'argent"}

查询顺序：
  1. english.db 的 zh_glosses 表（本地词包，微秒级）
  2. 本地没有 -> 调 TokenHub 的翻译模型（默认 hy-mt2-pro），并把结果写回本地包
     （这一步可以用 --no-persist 关掉，用 --no-ai 变成纯本地）

用法：
    $env:OH_KEY = '<token>'
    python bridge_server.py --db "D:\\Program Files\\Metasequoia\\english.db" --port 1188

    然后输入法设置里：在线翻译服务 = 自定义 DeepLX 兼容，
    接口地址 = http://127.0.0.1:1188/translate
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sqlite3
import threading
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

LANG_NAMES = {
    "en": "English",
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
    "Translate the given text into {lang}. If it is an English dictionary gloss with several senses "
    "separated by ';', keep at most two senses joined with '; ' and drop part-of-speech labels. "
    "Answer with the translation only: no numbering, no quotes, no explanation. "
    "Keep it as short as a candidate-window hint."
)

MAX_GLOSS_CHARS = 40


class Store:
    def __init__(self, db_path: str, persist: bool) -> None:
        self.db_path = db_path
        self.persist = persist
        self.lock = threading.Lock()
        self.reader = sqlite3.connect(f"file:{db_path}?mode=ro", uri=True, check_same_thread=False)
        try:
            self.reader.execute("SELECT 1 FROM zh_glosses LIMIT 1")
            self.has_table = True
        except sqlite3.Error:
            self.has_table = False
            print("提示：这个 english.db 还没有 zh_glosses 表，只能靠 AI", flush=True)
        self.writer: sqlite3.Connection | None = None
        if persist:
            self.writer = sqlite3.connect(db_path, check_same_thread=False, timeout=1.0)
            self.writer.execute("PRAGMA busy_timeout=800")

    def lookup(self, text: str, language: str) -> str:
        if not self.has_table:
            return ""
        with self.lock:
            row = self.reader.execute(
                "SELECT gloss FROM zh_glosses WHERE chinese=?1 AND lang=?2", (text, language)
            ).fetchone()
        return row[0] if row else ""

    def save(self, text: str, language: str, gloss: str) -> None:
        if not self.writer or not gloss:
            return
        try:
            with self.lock, self.writer:
                self.writer.execute(
                    "INSERT OR REPLACE INTO zh_glosses(chinese, lang, gloss) VALUES(?,?,?)",
                    (text, language, gloss),
                )
        except sqlite3.Error:
            pass  # 写不进去就只留在内存缓存里，不影响这次回答


class Translator:
    def __init__(self, base: str, key: str, model: str, timeout: float) -> None:
        self.base = base.rstrip("/")
        self.key = key
        self.model = model
        self.timeout = timeout

    def translate(self, text: str, language: str) -> str:
        if not self.key:
            return ""
        payload = {
            "model": self.model,
            "messages": [
                {"role": "system", "content": SYSTEM.format(lang=LANG_NAMES.get(language, language))},
                {"role": "user", "content": text},
            ],
            "temperature": 0,
            "max_tokens": 256,
            "stream": False,
        }
        request = urllib.request.Request(
            self.base + "/chat/completions",
            data=json.dumps(payload).encode(),
            headers={"Authorization": "Bearer " + self.key, "Content-Type": "application/json"},
        )
        with urllib.request.urlopen(request, timeout=self.timeout) as response:
            body = json.loads(response.read().decode("utf-8", "replace"))
        value = (body["choices"][0]["message"]["content"] or "").strip()
        value = re.sub(r"[\t\r\n]+", " ", value).strip().strip('"').strip()
        if len(value) > MAX_GLOSS_CHARS:
            cut = value[:MAX_GLOSS_CHARS]
            space = cut.rfind(" ")
            value = (cut[:space] if space > 12 else cut).rstrip(" ;,.")
        return value


def make_handler(store: Store, translator: Translator, cache: dict, cache_lock: threading.Lock):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt: str, *args) -> None:  # noqa: A003  (quiet log)
            return

        def reply(self, status: int, payload: dict) -> None:
            body = json.dumps(payload, ensure_ascii=False).encode()
            self.send_response(status)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self) -> None:  # noqa: N802
            self.reply(200, {"code": 200, "status": "ok"})

        def do_POST(self) -> None:  # noqa: N802
            length = int(self.headers.get("Content-Length") or 0)
            raw = self.rfile.read(length) if length else b"{}"
            try:
                request = json.loads(raw.decode("utf-8", "replace"))
            except json.JSONDecodeError:
                self.reply(400, {"code": 400, "data": ""})
                return

            text = str(request.get("text") or "").strip()
            language = str(request.get("target_lang") or "").strip().lower()
            if not text or not language:
                self.reply(200, {"code": 200, "data": ""})
                return

            key = (text, language)
            with cache_lock:
                hit = cache.get(key)
            if hit:
                self.reply(200, {"code": 200, "data": hit})
                return

            gloss = store.lookup(text, language)
            source = "local"
            if not gloss:
                try:
                    gloss = translator.translate(text, language)
                    source = "ai"
                except (urllib.error.URLError, TimeoutError, KeyError, ValueError, OSError):
                    gloss = ""
            if gloss:
                with cache_lock:
                    cache[key] = gloss
                if source == "ai":
                    store.save(text, language, gloss)
            if os.environ.get("BRIDGE_VERBOSE"):
                print(f"{source:<5} {language} {text} -> {gloss}", flush=True)
            self.reply(200, {"code": 200, "data": gloss})

    return Handler


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--db", default=r"D:\Program Files\Metasequoia\english.db")
    parser.add_argument("--port", type=int, default=1188)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--model", default=os.environ.get("OH_MODEL", "hy-mt2-pro"))
    parser.add_argument("--base", default=os.environ.get("OH_BASE", "https://tokenhub.tencentmaas.com/v1"))
    parser.add_argument("--timeout", type=float, default=2.2)
    parser.add_argument("--no-ai", action="store_true", help="只查本地词包，miss 返回空")
    parser.add_argument("--no-persist", action="store_true", help="AI 结果不写回本地词包")
    args = parser.parse_args()

    if not Path(args.db).exists():
        raise SystemExit(f"database not found: {args.db}")
    key = "" if args.no_ai else os.environ.get("OH_KEY", "").strip()
    if not key and not args.no_ai:
        print("警告：没有 OH_KEY，只能查本地词包（相当于 --no-ai）", flush=True)

    store = Store(args.db, persist=not args.no_persist and bool(key))
    translator = Translator(args.base, key, args.model, args.timeout)
    cache: dict[tuple[str, str], str] = {}
    cache_lock = threading.Lock()

    server = ThreadingHTTPServer((args.host, args.port), make_handler(store, translator, cache, cache_lock))
    server.daemon_threads = True
    print(f"listening on http://{args.host}:{args.port}/translate  db={args.db}", flush=True)
    started = time.time()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
        print(f"stopped after {time.time() - started:.0f}s", flush=True)


if __name__ == "__main__":
    main()
