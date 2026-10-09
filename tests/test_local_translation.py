"""Exercise the real HTTP contract without downloading or importing an ML runtime."""
import http.client
import json
import os
from pathlib import Path
import sys
import threading
import tempfile
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts/local_translation"))
from cpu_limit import CpuLimit, rate_units
from configure import configure, patch_section
from models import TranslationError
from service import CACHE_ENTRIES, REQUEST_SECONDS, Server, Translations, validate_request
import tomllib


class FakeModels:
    def __init__(self):
        self.calls = 0
        self.entered = threading.Event()
        self.release = None

    def translate(self, text, source, target, deadline):
        self.calls += 1
        self.entered.set()
        if self.release:
            self.release.wait(3)
        return "整句译文" if target == "zh" else "A complete translated sentence."


class RequestTests(unittest.TestCase):
    def test_whole_sentence_and_language_aliases(self):
        self.assertEqual(validate_request({"text": "请明天再来。", "source_lang": "ZH", "target_lang": "EN"}),
                         ("请明天再来。", "zh", "en"))
        self.assertEqual(validate_request({"text": "Good morning.", "target_lang": "zh-CN"}),
                         ("Good morning.", "en", "zh"))

    def test_invalid_and_oversized_inputs(self):
        for body in ([], {"text": 1}, {"text": " "}, {"text": "a" * 161},
                     {"text": "hello", "target_lang": "fr"}, {"text": "a\x00b"}):
            with self.subTest(body=body), self.assertRaises(TranslationError):
                validate_request(body)

    def test_cap_rejects_unbounded_nan_and_excessive_values(self):
        self.assertEqual(rate_units(1), 100)
        self.assertEqual(rate_units(5), 500)
        for value in (0, -1, 5.01, float("nan"), float("inf")):
            with self.subTest(value=value), self.assertRaises(ValueError):
                rate_units(value)

    @unittest.skipIf(os.name == "nt", "Unsupported-platform check")
    def test_unsupported_platform_fails_closed(self):
        with self.assertRaises(OSError):
            CpuLimit(1)


class ConfigureTests(unittest.TestCase):
    def test_keeps_other_settings_and_backs_up_original_bytes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            config = root / "config.toml"
            original = ('# comment\r\n[general]\r\ncloud_candidates = false\r\n'
                        '[tencent_tmt]\r\nsecret_id = "unchanged-id"\r\nsecret_key = "unchanged-key"\r\n'
                        '[voice_input]\r\nenabled = true\r\n').encode()
            config.write_bytes(original)
            backup = configure(config, {"port": 1188, "token": "test-token"}, root / "backups")
            self.assertEqual(backup.read_bytes(), original)
            parsed = tomllib.loads(config.read_text(encoding="utf-8"))
            self.assertFalse(parsed["general"]["cloud_candidates"])
            self.assertTrue(parsed["voice_input"]["enabled"])
            self.assertEqual(parsed["tencent_tmt"]["secret_key"], "unchanged-key")
            self.assertFalse(parsed["tencent_tmt"]["enabled"])
            self.assertEqual(parsed["custom_translation"]["endpoint"], "http://127.0.0.1:1188/translate")

    def test_patch_handles_comments_bracketed_values_and_unicode_token(self):
        text = '[custom_translation] # provider\nendpoint = "https://example.com/[old]" # old\n\n[other]\nvalue = [1,2]\n'
        parsed = tomllib.loads(patch_section(text, "custom_translation", {"endpoint": "http://127.0.0.1/translate"}))
        self.assertEqual(parsed["other"]["value"], [1, 2])
        self.assertEqual(parsed["custom_translation"]["endpoint"], "http://127.0.0.1/translate")


class CacheTests(unittest.TestCase):
    def test_cache_is_bounded_and_expires(self):
        now = [0]
        model = FakeModels()
        translations = Translations(model, clock=lambda: now[0])
        request = {"text": "hello", "target_lang": "zh"}
        self.assertFalse(translations.translate(request)[1])
        self.assertTrue(translations.translate(request)[1])
        self.assertEqual(model.calls, 1)
        now[0] = 1801
        self.assertFalse(translations.translate(request)[1])
        for number in range(CACHE_ENTRIES + 10):
            translations.translate({"text": f"sentence {number}", "target_lang": "zh"})
        self.assertEqual(len(translations.cache), CACHE_ENTRIES)

    def test_only_one_model_request_runs_and_busy_does_not_queue(self):
        model = FakeModels()
        model.release = threading.Event()
        translations = Translations(model)
        thread = threading.Thread(target=translations.translate, args=({"text": "first", "target_lang": "zh"},))
        thread.start()
        self.assertTrue(model.entered.wait(2))
        try:
            with self.assertRaises(TranslationError) as caught:
                translations.translate({"text": "second", "target_lang": "zh"})
            self.assertEqual(caught.exception.status, 429)
            self.assertEqual(model.calls, 1)
        finally:
            model.release.set()
            thread.join(3)

    def test_expired_generation_is_never_cached_or_returned(self):
        now = [0]

        class SlowModels:
            def translate(self, *args):
                now[0] += REQUEST_SECONDS + 1
                return "partial"

        translations = Translations(SlowModels(), clock=lambda: now[0])
        with self.assertRaises(TranslationError) as caught:
            translations.translate({"text": "hello", "target_lang": "zh"})
        self.assertEqual(caught.exception.status, 504)
        self.assertFalse(translations.cache)
        self.assertFalse(translations.gate.locked())


class HttpTests(unittest.TestCase):
    def setUp(self):
        self.models = FakeModels()
        self.server = Server(("127.0.0.1", 0), Translations(self.models), "t" * 32, 1)
        self.thread = threading.Thread(target=self.server.serve_forever, kwargs={"poll_interval": 0.01})
        self.thread.start()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(3)

    def connection(self):
        return http.client.HTTPConnection("127.0.0.1", self.server.server_port, timeout=2)

    def request(self, body, **extra):
        connection = self.connection()
        headers = {"Authorization": "Bearer " + "t" * 32, "Content-Type": "application/json", **extra}
        connection.request("POST", "/translate", json.dumps(body, ensure_ascii=False).encode(), headers)
        response = connection.getresponse()
        status, body = response.status, json.loads(response.read())
        connection.close()
        return status, body

    def test_deeplx_utf8_response_and_keepalive(self):
        connection = self.connection()
        try:
            for _ in range(2):
                connection.request("POST", "/translate", b'{"text":"Good morning.","source_lang":"EN","target_lang":"ZH"}',
                                   {"Authorization": "Bearer " + "t" * 32, "Content-Type": "application/json; charset=utf-8"})
                response = connection.getresponse()
                self.assertEqual(response.status, 200)
                self.assertEqual(json.loads(response.read())["data"], "整句译文")
            self.assertEqual(self.models.calls, 1)
        finally:
            connection.close()

    def test_auth_origin_and_input_rejections_do_not_run_model(self):
        self.assertEqual(self.request({"text": "hello"}, Authorization="Bearer wrong")[0], 401)
        self.assertEqual(self.request({"text": "hello"}, Origin="https://example.com")[0], 403)
        self.assertEqual(self.request({"text": "x" * 161})[0], 413)
        self.assertEqual(self.request([])[0], 400)
        self.assertEqual(self.models.calls, 0)

    def test_health_reports_effective_resource_limit(self):
        connection = self.connection()
        connection.request("GET", "/health", headers={"Authorization": "Bearer " + "t" * 32})
        response = connection.getresponse()
        data = json.loads(response.read())
        connection.close()
        self.assertEqual(data["device"], "cpu")
        self.assertEqual(data["cpu_limit_percent"], 1)


if __name__ == "__main__":
    unittest.main()
