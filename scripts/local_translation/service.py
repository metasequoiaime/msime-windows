"""Loopback-only DeepLX-compatible sentence translation with a Windows CPU cap."""
from __future__ import annotations

import argparse
from collections import OrderedDict
import hmac
from http.server import BaseHTTPRequestHandler, HTTPServer
import json
import os
from pathlib import Path
from socketserver import ThreadingMixIn
import threading
import time

from cpu_limit import CpuLimit, rate_units
from models import MAX_CHARACTERS, Models, TranslationError

MAX_BODY_BYTES = 4096
REQUEST_SECONDS = 2.2  # The native client times out after 2.5 seconds.
CACHE_ENTRIES = 256
CACHE_SECONDS = 1800


def language(value: str, text: str = "") -> str:
    if not isinstance(value, str):
        raise TranslationError(400, "Language must be a string")
    value = value.lower().replace("_", "-")
    if value in ("zh", "zh-cn", "zh-hans", "zh-tw", "zh-hant"):
        return "zh"
    if value == "en":
        return "en"
    if value == "auto":
        return "zh" if any("\u3400" <= character <= "\u9fff" for character in text) else "en"
    raise TranslationError(400, "Only Chinese and English are supported")


def validate_request(body):
    if not isinstance(body, dict) or not isinstance(body.get("text"), str):
        raise TranslationError(400, "Expected a JSON object with a text string")
    text = body["text"].strip()
    if not text:
        raise TranslationError(400, "Text must not be empty")
    if len(text) > MAX_CHARACTERS:
        raise TranslationError(413, "Sentence exceeds the character limit")
    if any(ord(character) < 32 and character not in "\n\r\t" for character in text):
        raise TranslationError(400, "Text contains unsupported control characters")
    source = language(body.get("source_lang", "auto"), text)
    target = language(body.get("target_lang", "en"))
    return text, source, target


class Translations:
    def __init__(self, models, clock=time.monotonic):
        self.models = models
        self.clock = clock
        self.gate = threading.Lock()
        self.cache_lock = threading.Lock()
        self.cache = OrderedDict()

    def translate(self, body):
        text, source, target = validate_request(body)
        if source == target:
            return text, True
        key = (source, target, text)
        now = self.clock()
        with self.cache_lock:
            # Expire every stale entry, including text that is never requested again.
            for old_key, (created, _) in list(self.cache.items()):
                if now - created >= CACHE_SECONDS:
                    del self.cache[old_key]
            if key in self.cache:
                self.cache.move_to_end(key)
                return self.cache[key][1], True
        if not self.gate.acquire(blocking=False):
            raise TranslationError(429, "Translator is busy; retry later")
        try:
            deadline = self.clock() + REQUEST_SECONDS
            result = self.models.translate(text, source, target, deadline)
            if self.clock() >= deadline:
                raise TranslationError(504, "Translation deadline exceeded")
            with self.cache_lock:
                self.cache[key] = (self.clock(), result)
                self.cache.move_to_end(key)
                while len(self.cache) > CACHE_ENTRIES:
                    self.cache.popitem(last=False)
            return result, False
        finally:
            self.gate.release()


class Server(ThreadingMixIn, HTTPServer):
    daemon_threads = True
    block_on_close = False
    request_queue_size = 4

    def __init__(self, address, translations, token, cpu_percent):
        self.translations = translations
        self.token = token
        self.cpu_percent = cpu_percent
        self.handlers = threading.BoundedSemaphore(4)
        super().__init__(address, Handler)

    def process_request(self, request, client_address):
        if not self.handlers.acquire(blocking=False):
            self.shutdown_request(request)
            return
        try:
            super().process_request(request, client_address)
        except BaseException:
            self.handlers.release()
            raise

    def process_request_thread(self, request, client_address):
        try:
            super().process_request_thread(request, client_address)
        finally:
            self.handlers.release()

    def handle_error(self, request, client_address):
        pass  # Never write request bodies, credentials or user input to a log.


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "MSIME-Local-Translation"

    def setup(self):
        super().setup()
        self.connection.settimeout(3)

    def log_message(self, *args):
        pass

    def reply(self, code, **fields):
        payload = json.dumps({"code": code, **fields}, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(payload)

    def authorized(self):
        supplied = self.headers.get("Authorization", "")
        expected = "Bearer " + self.server.token
        if not hmac.compare_digest(supplied.encode("utf-8"), expected.encode("utf-8")):
            self.close_connection = True
            self.reply(401, message="Bearer token required")
            return False
        # A local web page cannot use this endpoint via CORS or a cross-site form.
        if self.headers.get("Origin"):
            self.close_connection = True
            self.reply(403, message="Browser cross-origin requests are not supported")
            return False
        return True

    def do_GET(self):
        if not self.authorized():
            return
        if self.path != "/health":
            self.reply(404, message="Unknown endpoint")
            return
        self.reply(200, status="ready", device="cpu", compute_type="int8", cpu_limit_percent=self.server.cpu_percent,
                   logical_processors=os.cpu_count(), pid=os.getpid(), max_characters=MAX_CHARACTERS)

    def do_POST(self):
        if not self.authorized():
            return
        if self.path not in ("/translate", "/shutdown"):
            self.close_connection = True
            self.reply(404, message="Unknown endpoint")
            return
        try:
            if self.headers.get("Transfer-Encoding"):
                raise TranslationError(400, "Chunked requests are not supported")
            raw_length = self.headers.get("Content-Length", "")
            if not raw_length.isdecimal():
                raise TranslationError(411, "Content-Length required")
            length = int(raw_length)
            if not 0 <= length <= MAX_BODY_BYTES:
                raise TranslationError(413, "Request body is too large")
            if self.headers.get_content_type() != "application/json":
                raise TranslationError(415, "Content-Type must be application/json")
            raw_body = self.rfile.read(length)
            if len(raw_body) != length:
                raise TranslationError(400, "Incomplete request body")
            body = json.loads(raw_body.decode("utf-8"))
            if self.path == "/shutdown":
                self.reply(200, status="stopping")
                threading.Thread(target=self.server.shutdown, daemon=True).start()
                return
            translated, cached = self.server.translations.translate(body)
            self.reply(200, data=translated, cached=cached)
        except TranslationError as error:
            self.close_connection = True
            self.reply(error.status, message=str(error))
        except (ValueError, UnicodeError, RecursionError):
            self.close_connection = True
            self.reply(400, message="Malformed JSON request")
        except (TimeoutError, ConnectionError, OSError):
            self.close_connection = True
        except Exception:
            self.close_connection = True
            self.reply(500, message="Translation failed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, required=True)
    args = parser.parse_args()
    config = json.loads(args.config.read_text(encoding="utf-8"))
    rate_units(config.get("cpu_percent", 1))
    token = config["token"]
    if not isinstance(token, str) or len(token) < 32 or not token.isascii():
        raise ValueError("A random ASCII Bearer token of at least 32 characters is required")
    port = config.get("port", 1188)
    if isinstance(port, bool) or not isinstance(port, int) or not 1024 <= port <= 65535:
        raise ValueError("Port must be between 1024 and 65535")
    # First bind the port, so a second instance exits without loading another model.
    server = Server(("127.0.0.1", port), None, token, config.get("cpu_percent", 1))
    try:
        cap = CpuLimit(config.get("cpu_percent", 1))
        models = Models(Path(config["models"]))
        server.translations = Translations(models)
        server.cpu_percent = cap.percent
        # Warm both directions under the same CPU cap before accepting requests.
        for text, source, target in (("你好", "zh", "en"), ("Hello", "en", "zh")):
            models.translate(text, source, target, time.monotonic() + 30)
        server.serve_forever(poll_interval=0.5)
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
