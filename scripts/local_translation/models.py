"""Offline OPUS/Argos inference without Torch or sentence-segmentation models."""
from __future__ import annotations

import json
import os
from pathlib import Path
import time

MAX_CHARACTERS = 160
MAX_INPUT_TOKENS = 256
MAX_OUTPUT_TOKENS = 192


class TranslationError(Exception):
    def __init__(self, status: int, message: str):
        super().__init__(message)
        self.status = status


class Models:
    def __init__(self, root: Path):
        # These apply before importing either runtime. The job cap covers all threads.
        for name in ("OMP_NUM_THREADS", "MKL_NUM_THREADS", "OPENBLAS_NUM_THREADS"):
            os.environ[name] = "1"
        import ctranslate2
        import sentencepiece

        self.pairs = {}
        for source, target in (("zh", "en"), ("en", "zh")):
            directory = root / f"translate-{source}_{target}-1_9"
            metadata = json.loads((directory / "metadata.json").read_text(encoding="utf-8"))
            if (metadata["from_code"], metadata["to_code"]) != (source, target):
                raise ValueError("Unexpected translation model language pair")
            translator = ctranslate2.Translator(str(directory / "model"), device="cpu", compute_type="int8",
                                               inter_threads=1, intra_threads=1, max_queued_batches=1)
            tokenizer = sentencepiece.SentencePieceProcessor(model_file=str(directory / "sentencepiece.model"))
            self.pairs[(source, target)] = (translator, tokenizer, metadata.get("target_prefix", ""))

    def translate(self, text: str, source: str, target: str, deadline: float) -> str:
        translator, tokenizer, prefix = self.pairs[(source, target)]
        tokens = tokenizer.encode(text, out_type=str)
        if len(tokens) > MAX_INPUT_TOKENS:
            raise TranslationError(413, "Sentence exceeds the input token limit")
        expired = False

        def stop(step):
            nonlocal expired
            expired = time.monotonic() >= deadline
            return expired

        if time.monotonic() >= deadline:
            raise TranslationError(504, "Translation deadline exceeded")
        result = translator.translate_batch(
            [tokens], target_prefix=[[prefix]] if prefix else None,
            beam_size=1, num_hypotheses=1, replace_unknowns=True,
            max_input_length=MAX_INPUT_TOKENS, max_decoding_length=MAX_OUTPUT_TOKENS,
            return_end_token=True, callback=stop,
        )[0].hypotheses[0]
        if expired or time.monotonic() >= deadline:
            raise TranslationError(504, "Translation deadline exceeded")
        # Never present a length-limited or timed-out fragment as a complete sentence.
        if len(result) >= MAX_OUTPUT_TOKENS and result[-1] != "</s>":
            raise TranslationError(422, "Translation exceeds the output token limit")
        result = [token for token in result if token != "</s>"]
        translated = tokenizer.decode(result).replace("▁", " ")
        if prefix and translated.startswith(prefix):
            translated = translated[len(prefix):]
        translated = translated.strip()
        if not translated:
            raise TranslationError(422, "Model returned an empty translation")
        return translated
