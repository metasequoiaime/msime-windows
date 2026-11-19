"""english.db：英文前缀候选表、由 ECDICT 推出的中英双向释义、ECDICT 的短语表、hallelujahIM 的音标、
sources/cn-en/ 的中译英释义、custom/translations.txt 的人工覆盖。

对应 Rust 构建器 english.rs / english_supplement.rs 的 english / english-glosses / custom-translations 阶段；
cn-en-translations 是 Rust 构建器还没有的阶段，排在 english-glosses 与 custom-translations 之间。
"""

from __future__ import annotations

import bz2
import csv
import io
import re
import sqlite3
import tarfile
from dataclasses import dataclass
from pathlib import Path

from . import textutil

CREATE_ENGLISH_WORDS = (
    "\n            CREATE TABLE english_words (\n                word TEXT COLLATE BINARY NOT NULL,\n"
    "                display TEXT NOT NULL,\n                weight INTEGER NOT NULL DEFAULT 0,\n"
    "                PRIMARY KEY (word, display)\n            ) WITHOUT ROWID\n            "
)

# SCOWL Aspell 包里决定首选大小写的三张词表。
SCOWL_LISTS = ("en-common.cwl", "en_US-wo_accents-only.cwl", "en_GB-ise-wo_accents-only.cwl")
NOTICE_SOURCE = "SCOWL"

# 一个词的每种大小写（按源文件里首次出现的顺序），键是小写形式。
EnglishWords = dict[str, list[str]]


def _is_ascii_word(value: str) -> bool:
    return textutil.is_ascii_alpha(value)


def _add_display(words: EnglishWords, display: str) -> None:
    displays = words.setdefault(display.lower(), [])
    if display not in displays:
        displays.append(display)


def parse_base_dict_words(text: str, name: str) -> EnglishWords:
    """rime-ice 英文词表的 `显示词 编码 [权重]` 行。编码不用：前缀查询按显示词本身。"""
    words: EnglishWords = {}
    for number, line in enumerate(textutil.universal_lines(text), start=1):
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        fields = stripped.split()
        if len(fields) < 2:
            raise ValueError(f"{name}:{number}: expected display input-code [weight]")
        if len(fields) >= 3 and fields[-1].isdecimal():
            fields.pop()
        fields.pop()
        display = " ".join(fields)
        if _is_ascii_word(display):
            _add_display(words, display)
    if not words:
        raise ValueError(f"{name}: no pure English words found")
    return words


def parse_word_list(text: str, name: str) -> EnglishWords:
    """sources/english/scowl-words.txt：# 表头之后每行一个 ASCII 单词，不重复。"""
    words: EnglishWords = {}
    seen = set()
    for number, line in enumerate(textutil.universal_lines(text), start=1):
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        if not _is_ascii_word(stripped):
            raise ValueError(f"{name}:{number}: expected one ASCII word, got {stripped!r}")
        if stripped in seen:
            raise ValueError(f"{name}:{number}: duplicate word {stripped!r}")
        seen.add(stripped)
        _add_display(words, stripped)
    if not words:
        raise ValueError(f"{name}: no words found")
    return words


def merge_words(words: EnglishWords, other: EnglishWords) -> int:
    added = 0
    for word in sorted(other):
        if word not in words:
            added += 1
        for display in other[word]:
            _add_display(words, display)
    return added


# ---- SCOWL Aspell 包 ----

def _prezip_words(data: bytes) -> list[bytes]:
    """Aspell 的 prezip 词表（aspell prog/prezip.c）：0x02 开头，每个词是共享前缀长度加剩余部分，0x1F 0xFF 结束。"""
    if data[:1] != b"\x02":
        raise ValueError("not a prezip word list")
    words: list[bytes] = []
    stored = bytearray()
    at = 1
    while True:
        if at >= len(data):
            raise ValueError("prezip word list ends without its terminator")
        first = data[at]
        at += 1
        prefix = first
        if first == 30:
            while True:
                if at >= len(data):
                    raise ValueError("prezip word list ends inside a prefix length")
                byte = data[at]
                at += 1
                prefix += byte
                if byte != 255:
                    break
        if prefix > len(stored):
            raise ValueError(f"prezip prefix length {prefix} exceeds the previous word")
        del stored[prefix:]
        while at < len(data) and data[at] > 30:
            stored.append(data[at])
            at += 1
        word = bytearray()
        index = 0
        finished = False
        while index < len(stored):
            if stored[index] != 31:
                word.append(stored[index])
                index += 1
                continue
            following = stored[index + 1] if index + 1 < len(stored) else None
            if following is not None and 32 <= following <= 63:
                word.append(following - 32)
            elif following == 255 and index + 2 == len(stored):
                finished = True
                break
            else:
                raise ValueError("corrupt prezip escape")
            index += 2
        if finished:
            if word:
                words.append(bytes(word))
            if at != len(data):
                raise ValueError("prezip word list has data after its terminator")
            return words
        words.append(bytes(word))


@dataclass
class ScowlPackage:
    lists: list[tuple[str, list[bytes]]]
    copyright: bytes


def read_scowl_package(path: Path) -> ScowlPackage:
    members: dict[str, list[bytes]] = {}
    with tarfile.open(fileobj=io.BytesIO(bz2.decompress(path.read_bytes())), mode="r:") as archive:
        for info in archive.getmembers():
            if info.isfile():
                members.setdefault(info.name.rsplit("/", 1)[-1], []).append(archive.extractfile(info).read())

    def member(name: str) -> bytes:
        found = members.get(name, [])
        if len(found) != 1:
            raise ValueError(f"the Aspell package has {len(found) or 'no'} {name}")
        return found[0]

    return ScowlPackage([(name, _prezip_words(member(name))) for name in SCOWL_LISTS], member("Copyright"))


def letter_forms(package: ScowlPackage) -> set[str]:
    """SCOWL 收录的纯 ASCII 字母写法，用来决定一个词哪种大小写打头。"""
    return {word.decode("ascii") for _, words in package.lists for word in words
            if word and all(65 <= b <= 90 or 97 <= b <= 122 for b in word)}


def _leading_display(word: str, displays: list[str], attested: set[str]) -> str | None:
    """SCOWL 只收了大写形式（Wikipedia、Ukraine）时取它收录的第一种写法；否则有全小写就用小写，没有就用源文件里的第一种。"""
    lowercase = next((d for d in displays if d == word), None)
    attested_first = next((d for d in displays if d in attested), None)
    if attested_first is not None and word not in attested:
        return attested_first
    return lowercase if lowercase is not None else (displays[0] if displays else None)


def parse_google_counts(text: str) -> dict[str, int]:
    counts = {}
    for line in textutil.universal_lines(text):
        stripped = line.strip()
        word, _, count = stripped.partition("\t")
        if not word or not count.isdecimal():
            continue
        parsed = textutil.parse_int(count)
        if parsed is not None and parsed < 1 << 63:
            counts[word.lower()] = parsed
    return counts


@dataclass
class CustomEnglishWord:
    word: str
    display: str
    weight: int


def parse_custom_english(text: str, name: str) -> list[CustomEnglishWord]:
    """custom/english.txt：`编码<TAB>显示词<TAB>权重`，同一 (编码, 显示词) 留最后一行。"""
    entries: dict[tuple[str, str], CustomEnglishWord] = {}
    for number, line in enumerate(textutil.without_bom(text).splitlines(), start=1):
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        fields = stripped.split("\t")
        if len(fields) != 3:
            raise ValueError(f"{name}:{number}: expected word, display and weight: {line!r}")
        word, display, weight_text = (field.strip() for field in fields)
        if not textutil.is_ascii_lower_word(word):
            raise ValueError(f"{name}:{number}: {word!r} is not a lowercase ASCII word")
        if not display:
            raise ValueError(f"{name}:{number}: the display is empty")
        weight = textutil.parse_int(weight_text)
        if weight is None:
            raise ValueError(f"{name}:{number}: weight {weight_text!r} is not an integer")
        if weight < 1:
            raise ValueError(f"{name}:{number}: weight {weight} is below 1")
        entries[(word, display)] = CustomEnglishWord(word, display, weight)
    return list(entries.values())


def build_english_words(connection: sqlite3.Connection, base: EnglishWords, counts: dict[str, int],
                        attested: set[str], custom: list[CustomEnglishWord]) -> tuple[int, int, int, int]:
    """基础词按 Google 词频加权（没有词频为 0）；一个词的首选写法拿词频、其余写法低 1。
    自定义词有词频就用词频，没有就用文件里的权重、但封顶在最小词频减 1。返回 (词数, 基础行, 新增自定义, 替换的自定义)。"""
    base_rows = custom_added = custom_replaced = 0
    with connection:
        connection.execute("DROP TABLE IF EXISTS english_words")
        connection.execute(CREATE_ENGLISH_WORDS)
        rows = []
        for word in sorted(base):
            count = counts.get(word, 0)
            displays = base[word]
            leading = _leading_display(word, displays, attested)
            # 没有词频的多写法词，首选写法给 1，免得都是 0 时退回按字节序排（大写全在前）。
            leading_weight = max(count, 1) if len(displays) > 1 else count
            for display in displays:
                rows.append((word, display, leading_weight if display == leading else leading_weight - 1))
        connection.executemany("INSERT INTO english_words(word, display, weight) VALUES (?, ?, ?)", rows)
        base_rows = len(rows)
        total, distinct = connection.execute("SELECT COUNT(*), COUNT(DISTINCT word) FROM english_words").fetchone()
        if total == 0 or total != base_rows or distinct != len(base):
            raise ValueError(f"english_words: unexpected row counts: rows={total}, distinct_words={distinct}")
        positive = [count for count in counts.values() if count > 0]
        ceiling = min(positive) - 1 if positive else (1 << 63) - 1
        for entry in custom:
            replaced = connection.execute("SELECT EXISTS(SELECT 1 FROM english_words WHERE word = ? AND display = ?)",
                                          (entry.word, entry.display)).fetchone()[0]
            count = counts.get(entry.word, 0)
            connection.execute("INSERT OR REPLACE INTO english_words(word, display, weight) VALUES (?, ?, ?)",
                               (entry.word, entry.display, max(count, min(entry.weight, ceiling))))
            if replaced:
                custom_replaced += 1
            else:
                custom_added += 1
    textutil.integrity_check(connection)
    primary_key = [row[0] for row in connection.execute(
        "SELECT name FROM pragma_table_info('english_words') WHERE pk > 0 ORDER BY pk")]
    if primary_key != ["word", "display"]:
        raise ValueError(f"english_words must use PRIMARY KEY(word, display); got {primary_key}")
    rows_now = connection.execute("SELECT COUNT(*) FROM english_words").fetchone()[0]
    if rows_now != base_rows + custom_added:
        raise ValueError(f"english_words: {rows_now} rows, expected {base_rows} base rows plus {custom_added} custom ones")
    # 统计信息只覆盖 english_words：释义表此时还没建，与发布的库一致。
    with connection:
        textutil.analyze(connection, optimize=True)
    return len(base), base_rows, custom_added, custom_replaced


def write_notices(connection: sqlite3.Connection, notices: list[tuple[str, str]]) -> None:
    """要求随每份拷贝附上声明的词表（SCOWL）的许可声明。引擎不读这张表，它只随库分发。"""
    with connection:
        connection.execute("DROP TABLE IF EXISTS source_notices")
        connection.execute("CREATE TABLE source_notices (source TEXT NOT NULL PRIMARY KEY, notice TEXT NOT NULL) WITHOUT ROWID")
        connection.executemany("INSERT INTO source_notices(source, notice) VALUES (?, ?)", notices)


# ---- ECDICT 释义 ----

_LEADING_DOMAIN = re.compile(r"^\s*(?:\[[^\]]+\]|【[^】]+】)\s*")
_LEADING_POS = re.compile(
    r"^\s*(?:(?:interj|abbr|modal|aux|adj|adv|prep|pron|conj|num|art|sing|pref|suff|vt|vi|ad|pl|int|n|v|a)\.?\s*)+",
    re.IGNORECASE)
_LEADING_PAREN = re.compile(r"^\s*[（(][^）)]*[）)]\s*")
_TRAILING_PAREN = re.compile(r"\s*[（(][^）)]*[）)]\s*\Z")
_INTERNAL_PAREN = re.compile(r"[（(].*[）)]")
_SPLIT = re.compile(r"[,，;；、]+")
_TRIM_PUNCTUATION = " \t\r\n.:：!?！？'\"“”‘’·•-—–_/\\"
# 以这些开头的反查项通常是解释而不是中文词头。
_REVERSE_EXPLANATION_PREFIXES = ("表示", "用于", "用来", "用作", "指代", "即为", "一种", "一个", "某种", "某个")
_TAG_BONUS = {"zk": 80_000, "gk": 75_000, "cet4": 70_000, "cet6": 60_000, "ky": 55_000, "ielts": 50_000,
              "toefl": 45_000, "gre": 30_000}
_TERM_TABLE = re.compile(r"\Atbl_(?:[1-7]|others)_[a-z]\Z")


def _is_cjk_term(text: str) -> bool:
    return bool(text) and all(0x3400 <= ord(c) <= 0x4DBF or 0x4E00 <= ord(c) <= 0x9FFF or 0xF900 <= ord(c) <= 0xFAFF
                              for c in text)


@dataclass
class GlossTerm:
    text: str
    line_index: int
    item_index: int
    domain_specific: bool
    reverse_domain_allowed: bool

    def reverse_eligible(self) -> bool:
        if (self.domain_specific and not self.reverse_domain_allowed) or not 2 <= len(self.text) <= 6:
            return False
        return not self.text.startswith(_REVERSE_EXPLANATION_PREFIXES)


def _positive_int(value: str) -> int:
    parsed = textutil.parse_int(value.strip())
    return max(parsed, 0) if parsed is not None else 0


def vocabulary_quality(collins: str, oxford: str, tag: str, bnc: str, frq: str) -> int:
    tag_bonus = max((_TAG_BONUS.get(name, 0) for name in tag.lower().split()), default=0)
    frq_value, bnc_value = _positive_int(frq), _positive_int(bnc)
    frq_bonus = max(50_000 - min(frq_value, 50_000), 0) if frq_value else 0
    bnc_bonus = max(25_000 - min(bnc_value, 50_000) // 2, 0) if bnc_value else 0
    return (min(_positive_int(collins), 5) * 100_000 + (80_000 if _positive_int(oxford) else 0)
            + tag_bonus + frq_bonus + bnc_bonus)


def _strip_parenthetical_qualifiers(value: str) -> str | None:
    """去掉首尾括号里的限定语；括号夹在中间说明是解释，整项不要。"""
    while True:
        following = _TRAILING_PAREN.sub("", _LEADING_PAREN.sub("", value, count=1), count=1)
        if following == value:
            break
        value = following
    return None if _INTERNAL_PAREN.search(value) else value


def extract_gloss_terms(translation: str) -> list[GlossTerm]:
    terms = []
    seen = set()
    # ECDICT 里很多换行存成了两个字符的 \n。
    for line_index, raw_line in enumerate(translation.replace("\\n", "\n").splitlines()):
        line = raw_line.strip()
        if not line:
            continue
        domain_specific = reverse_domain_allowed = False
        while (found := _LEADING_DOMAIN.match(line)) is not None:
            domain_specific = True
            label = found.group()
            reverse_domain_allowed = reverse_domain_allowed or "计" in label or "网络" in label
            line = line[found.end():]
        found = _LEADING_POS.match(line)
        line = (line[found.end():] if found else line).strip()
        for item_index, raw_item in enumerate(_SPLIT.split(line)):
            item = _strip_parenthetical_qualifiers(raw_item.strip(_TRIM_PUNCTUATION))
            if item is None:
                continue
            item = item.strip(_TRIM_PUNCTUATION)
            if len(item) > 8 or not _is_cjk_term(item) or item in seen:
                continue
            seen.add(item)
            terms.append(GlossTerm(item, line_index, item_index, domain_specific, reverse_domain_allowed))
    return terms


def choose_chinese_gloss(terms: list[GlossTerm], weights: dict[str, int]) -> str:
    """最多两个词用 ；连接：通用义在专业义之前，再按作为中文候选的常用程度、位置排。"""
    texts = {term.text for term in terms}
    ranked = [term for term in terms if not (term.text.endswith("的") and term.text[:-1] in texts)]
    ranked.sort(key=lambda t: (t.domain_specific, -weights.get(t.text, 0), t.line_index, t.item_index, len(t.text), t.text))
    return "；".join(term.text for term in ranked[:2])


def _chinese_term_weights(msime: sqlite3.Connection, terms: set[str]) -> dict[str, int]:
    tables = [row[0] for row in msime.execute(
        "SELECT name FROM sqlite_master WHERE type='table' AND name GLOB 'tbl_*_[a-z]' ORDER BY name")
        if _TERM_TABLE.match(row[0])]
    weights: dict[str, int] = {}
    for table in tables:
        for value, weight in msime.execute(f'SELECT value,weight FROM "{table}"'):
            if value is not None and value in terms:
                weights[value] = max(weights.get(value, 0), weight or 0)
    return weights


def _finalize_reverse_gloss(candidates: dict[str, int]) -> str | None:
    """一个中文词最好的英文：至多两个，各自不低于最高分的 45%。"""
    ranked = sorted(candidates.items(), key=lambda item: (-item[1], len(item[0]), item[0]))
    if not ranked:
        return None
    top = ranked[0][1]
    return "; ".join([english for english, score in ranked if score * 100 >= top * 45][:2])


def derive_glosses(ecdict: Path, english: sqlite3.Connection, msime: sqlite3.Connection,
                   reverse_excluded: set[str]) -> tuple[dict[str, str], dict[str, str]]:
    """ECDICT 与英文候选取交集，推出英译中和中译英。只有带语料或核心词表信号的通用义进入中译英，且中文必须能由全拼表打出。
    reverse_excluded（只由 SCOWL 带来的词）有英译中，但不进中译英，也不替它的屈折形式进中译英。"""
    candidates = set()
    for (word,) in english.execute("SELECT DISTINCT word FROM english_words"):
        if word is not None and textutil.is_ascii_lower_word(word.lower()):
            candidates.add(word.lower())

    text = textutil.without_bom(ecdict.read_bytes().decode("utf-8"))
    csv.field_size_limit(1 << 30)
    reader = csv.reader(io.StringIO(text, newline=""))
    headers = next(reader)
    required = ["word", "translation", "collins", "oxford", "tag", "bnc", "frq"]
    missing = [name for name in required if name not in headers]
    if missing:
        raise ValueError(f"ECDICT CSV is missing columns: {missing}")
    column = {name: headers.index(name) for name in required}
    exchange_column = headers.index("exchange") if "exchange" in headers else None

    entries: dict[str, tuple[str, int, list[GlossTerm]]] = {}
    for record in reader:
        def field(index: int | None) -> str:
            return record[index] if index is not None and index < len(record) else ""

        word = field(column["word"]).strip().lower()
        if word not in candidates:
            continue
        translation = field(column["translation"]).strip()
        if not translation:
            continue
        terms = extract_gloss_terms(translation)
        if not terms:
            continue
        quality = vocabulary_quality(field(column["collins"]), field(column["oxford"]), field(column["tag"]),
                                     field(column["bnc"]), field(column["frq"]))
        # exchange 里以 0:<原形> 给出的原形，在中译英里代替它的屈折形式。
        reverse_english = next(
            (lemma for lemma in (item[2:].strip().lower() for item in field(exchange_column).split("/") if item.startswith("0:"))
             if lemma in candidates and lemma not in reverse_excluded and textutil.is_ascii_lower_word(lemma)),
            word)
        previous = entries.get(word)
        if previous is None or (quality, len(terms)) > (previous[1], len(previous[2])):
            entries[word] = (reverse_english, quality, terms)

    reverse: dict[str, dict[str, int]] = {}
    for word in sorted(entries):
        reverse_english, quality, terms = entries[word]
        if quality <= 0 or word in reverse_excluded:
            continue
        for term in terms:
            if not term.reverse_eligible():
                continue
            position_bonus = max(30_000 - term.line_index * 2_000 - term.item_index * 500, 0)
            score = quality + position_bonus - len(reverse_english) * 10
            best = reverse.setdefault(term.text, {})
            best[reverse_english] = max(best.get(reverse_english, score), score)

    weights = _chinese_term_weights(msime, {term.text for _, _, terms in entries.values() for term in terms})
    en_zh = {word: choose_chinese_gloss(entries[word][2], weights) for word in entries}
    zh_en = {}
    for chinese, scored in reverse.items():
        if chinese in weights and (gloss := _finalize_reverse_gloss(scored)) is not None:
            zh_en[chinese] = gloss
    return en_zh, zh_en


_CREATE_GLOSS_TABLES = """
            DROP TABLE IF EXISTS en_zh_glosses_new;
            DROP TABLE IF EXISTS zh_en_glosses_new;
            CREATE TABLE en_zh_glosses_new (
                english TEXT COLLATE BINARY PRIMARY KEY,
                chinese_gloss TEXT NOT NULL
            ) WITHOUT ROWID;
            CREATE TABLE zh_en_glosses_new (
                chinese TEXT COLLATE BINARY PRIMARY KEY,
                english_gloss TEXT NOT NULL
            ) WITHOUT ROWID;
            """
_SWAP_GLOSS_TABLES = """
            DROP TABLE IF EXISTS en_zh_glosses;
            ALTER TABLE en_zh_glosses_new RENAME TO en_zh_glosses;
            DROP TABLE IF EXISTS zh_en_glosses;
            ALTER TABLE zh_en_glosses_new RENAME TO zh_en_glosses;
            PRAGMA user_version=3;
            """


def write_glosses(connection: sqlite3.Connection, en_zh: dict[str, str], zh_en: dict[str, str]) -> None:
    """两张释义表先建成 _new 再改名，与发布的库同一份表结构文本。"""
    connection.executescript(_CREATE_GLOSS_TABLES)
    with connection:
        connection.executemany("INSERT INTO en_zh_glosses_new(english,chinese_gloss) VALUES(?1,?2)", sorted(en_zh.items()))
        connection.executemany("INSERT INTO zh_en_glosses_new(chinese,english_gloss) VALUES(?1,?2)", sorted(zh_en.items()))
    connection.executescript(_SWAP_GLOSS_TABLES)
    textutil.integrity_check(connection)


def parse_cn_en_translations(text: str, name: str) -> dict[str, str]:
    """sources/cn-en/machine-translations.txt：`源词<TAB>译文`，每个源词一行，源词不能是纯 ASCII。
    译文原样保留：词性前缀（n.、proper n. 等）和用 `; ` 分隔的全部义项都进库，不像 ECDICT 推出的那样截到两个。"""
    entries: dict[str, str] = {}
    for number, line in enumerate(textutil.without_bom(text).splitlines(), start=1):
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        fields = stripped.split("\t")
        if len(fields) != 2:
            raise ValueError(f"{name}:{number}: expected source<TAB>gloss, got {line!r}")
        source, gloss = fields[0].strip(), fields[1].strip()
        if not source or not gloss:
            raise ValueError(f"{name}:{number}: empty source or gloss")
        # 〇（U+3007）这类源词不在 U+3400 以上，只拦纯 ASCII 的英文源词。
        if source.isascii():
            raise ValueError(f"{name}:{number}: {source!r} is not a Chinese source word")
        if source in entries:
            raise ValueError(f"{name}:{number}: duplicate source word {source!r}")
        entries[source] = gloss
    if not entries:
        raise ValueError(f"{name}: no translations found")
    return entries


def apply_cn_en_translations(connection: sqlite3.Connection, entries: dict[str, str]) -> tuple[int, int]:
    """覆盖或补充中译英释义。返回 (替换 ECDICT 释义的条数, 新增的条数)。"""
    with connection:
        existing = {row[0] for row in connection.execute("SELECT chinese FROM zh_en_glosses")}
        connection.executemany("INSERT OR REPLACE INTO zh_en_glosses(chinese,english_gloss) VALUES(?1,?2)",
                               sorted(entries.items()))
    replaced = sum(1 for source in entries if source in existing)
    return replaced, len(entries) - replaced


@dataclass
class CustomTranslation:
    chinese_to_english: bool
    source: str
    gloss: str


def parse_custom_translations(text: str, name: str) -> list[CustomTranslation]:
    """`源词<TAB>译文`：源词含 U+3400 及以上的字符是中译英，否则英译中；同一源词后面的行覆盖前面的。"""
    entries = []
    for number, line in enumerate(textutil.without_bom(text).splitlines(), start=1):
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        fields = stripped.split("\t")
        if len(fields) < 2:
            raise ValueError(f"{name}:{number}: expected source<TAB>gloss, got {line!r}")
        source, gloss = fields[0].strip(), fields[1].strip()
        if not source or not gloss:
            raise ValueError(f"{name}:{number}: empty source or gloss")
        entries.append(CustomTranslation(any(ord(c) >= 0x3400 for c in source), source, gloss))
    return entries


def apply_custom_translations(connection: sqlite3.Connection, entries: list[CustomTranslation]) -> None:
    with connection:
        for entry in entries:
            if entry.chinese_to_english:
                connection.execute("INSERT OR REPLACE INTO zh_en_glosses(chinese,english_gloss) VALUES(?1,?2)",
                                   (entry.source, entry.gloss))
            else:
                connection.execute("INSERT OR REPLACE INTO en_zh_glosses(english,chinese_gloss) VALUES(?1,?2)",
                                   (entry.source, entry.gloss))


# ---- 短语 ----

CREATE_ENGLISH_PHRASES = (
    "CREATE TABLE english_phrases (word TEXT COLLATE BINARY NOT NULL, display TEXT NOT NULL, "
    "weight INTEGER NOT NULL DEFAULT 0, PRIMARY KEY (word, display)) WITHOUT ROWID"
)
_PHRASE_SEPARATOR = re.compile(r"[ \-]")
_NON_LETTER = re.compile(r"[^a-z]")
_WHITESPACE = re.compile(r"\s+")
# 释义里挑不出中文词头（全是解释性的长句）时，退回用第一行，截到这么多字。
_PHRASE_GLOSS_FALLBACK_CHARS = 24


@dataclass
class EnglishPhrase:
    word: str     # 查询键：小写字母，去掉空格、连字符、撇号等，英文模式里连着敲字母就能查到
    display: str  # ECDICT 的写法
    gloss: str


def phrase_key(display: str) -> str:
    return _NON_LETTER.sub("", display.lower())


def _fallback_phrase_gloss(translation: str) -> str:
    for raw_line in translation.replace("\\n", "\n").splitlines():
        line = raw_line.strip()
        while (found := _LEADING_DOMAIN.match(line)) is not None:
            line = line[found.end():]
        found = _LEADING_POS.match(line)
        line = (line[found.end():] if found else line).strip()
        if line:
            return line if len(line) <= _PHRASE_GLOSS_FALLBACK_CHARS else line[:_PHRASE_GLOSS_FALLBACK_CHARS] + "…"
    return ""


def parse_ecdict_phrases(ecdict: Path) -> list[EnglishPhrase]:
    """ECDICT 里所有纯 ASCII、带空格或连字符的条目（according to、well-known），不管有没有词频信号。
    键少于两个字母的不要。释义按单词释义的同一套规则挑词头，挑不出来时用第一行。"""
    text = textutil.without_bom(ecdict.read_bytes().decode("utf-8"))
    csv.field_size_limit(1 << 30)
    reader = csv.reader(io.StringIO(text, newline=""))
    headers = next(reader)
    if "word" not in headers or "translation" not in headers:
        raise ValueError("ECDICT CSV is missing the word or translation column")
    word_column, translation_column = headers.index("word"), headers.index("translation")
    phrases: dict[str, EnglishPhrase] = {}
    for record in reader:
        if word_column >= len(record):
            continue
        display = _WHITESPACE.sub(" ", record[word_column]).strip()
        # 以 # 开头的会被当成文件里的注释行，不收。
        if not display.isascii() or not _PHRASE_SEPARATOR.search(display) or display.startswith("#"):
            continue
        key = phrase_key(display)
        if len(key) < 2 or display in phrases:
            continue
        translation = record[translation_column].strip() if translation_column < len(record) else ""
        gloss = choose_chinese_gloss(extract_gloss_terms(translation), {}) if translation else ""
        if not gloss and translation:
            gloss = _fallback_phrase_gloss(translation)
        gloss = _WHITESPACE.sub(" ", gloss).strip()
        phrases[display] = EnglishPhrase(key, display, gloss)
    if not phrases:
        raise ValueError("ECDICT has no phrases")
    return list(phrases.values())


PHRASES_FILE = "sources/english/ecdict-phrases.txt"


def format_phrases(phrases: list[EnglishPhrase]) -> list[str]:
    """ecdict-phrases.txt 的数据行：`写法<TAB>释义`（没有释义时只有写法），按字典序。"""
    ordered = sorted(phrases, key=lambda phrase: (phrase.display.lower(), phrase.display))
    return [f"{phrase.display}\t{phrase.gloss}" if phrase.gloss else phrase.display for phrase in ordered]


def parse_phrases(text: str, name: str) -> list[EnglishPhrase]:
    """msime-dictionary 的 sources/english/ecdict-phrases.txt（由 scripts/generate-english-extras.py 生成）。"""
    phrases: list[EnglishPhrase] = []
    seen = set()
    for number, line in enumerate(textutil.without_bom(text).splitlines(), start=1):
        if not line.strip() or line.startswith("#"):
            continue
        display, _, gloss = line.partition("\t")
        key = phrase_key(display)
        if (display != display.strip() or not display.isascii() or not _PHRASE_SEPARATOR.search(display)
                or len(key) < 2):
            raise ValueError(f"{name}:{number}: {display!r} is not an ASCII phrase with a space or a hyphen")
        if display in seen:
            raise ValueError(f"{name}:{number}: duplicate phrase {display!r}")
        seen.add(display)
        phrases.append(EnglishPhrase(key, display, gloss.strip()))
    if not phrases:
        raise ValueError(f"{name}: no phrases found")
    return phrases


def write_phrases(connection: sqlite3.Connection, phrases: list[EnglishPhrase]) -> tuple[int, int]:
    """短语单独一张表，与 english_words 同样的列。还没有词频，初始权重按字典序给：越靠前越高，
    前缀查询按权重降序出来就是字典序。释义写进 en_zh_glosses，键是小写写法（同一键的第一个非空释义），
    不覆盖已有的单词释义。返回 (短语数, 新写入的释义数)。"""
    ordered = sorted(phrases, key=lambda phrase: (phrase.display.lower(), phrase.display))
    total = len(ordered)
    glosses: dict[str, str] = {}
    for phrase in ordered:
        if phrase.gloss:
            glosses.setdefault(phrase.display.lower(), phrase.gloss)
    with connection:
        connection.execute("DROP TABLE IF EXISTS english_phrases")
        connection.execute(CREATE_ENGLISH_PHRASES)
        connection.executemany("INSERT INTO english_phrases(word, display, weight) VALUES (?, ?, ?)",
                               ((phrase.word, phrase.display, total - index) for index, phrase in enumerate(ordered)))
        before = connection.execute("SELECT COUNT(*) FROM en_zh_glosses").fetchone()[0]
        connection.executemany("INSERT OR IGNORE INTO en_zh_glosses(english, chinese_gloss) VALUES (?, ?)",
                               sorted(glosses.items()))
        added = connection.execute("SELECT COUNT(*) FROM en_zh_glosses").fetchone()[0] - before
    textutil.integrity_check(connection)
    return total, added


# ---- 音标 ----

IPA_FILE = "sources/english/hallelujahim-ipa.txt"
IPA_NOTICE_SOURCE = "hallelujahIM IPA"
IPA_NOTICE = (
    "The en_ipa table is read from msime-dictionary's sources/english/hallelujahim-ipa.txt, which is the ipa field "
    "of hallelujahIM's dictionary/words_with_frequency_and_translation_and_ipa.json "
    "(https://github.com/dongyuwei/hallelujahIM, GPL-3.0, by Dong Yuwei). hallelujahIM generated it with "
    "English-to-IPA (eng_to_ipa, https://github.com/mphilli/English-to-IPA, MIT), which transcribes the "
    "Carnegie Mellon Pronouncing Dictionary (CMUdict, BSD-2-Clause). Only the ipa field is used; the word "
    "frequencies and translations in the same file are not.")

CREATE_EN_IPA = "CREATE TABLE en_ipa (english TEXT COLLATE BINARY NOT NULL PRIMARY KEY, ipa TEXT NOT NULL) WITHOUT ROWID"


def parse_hallelujah_ipa(data: bytes) -> dict[str, str]:
    """hallelujahIM 的 `{词: {"frequency", "translation", "ipa"}}`，只取 ipa。

    eng_to_ipa 查不到 CMUdict 的词会原样返回并在末尾加 `*`，这类不是音标，丢掉。
    """
    import json

    entries = json.loads(textutil.without_bom(data.decode("utf-8")))
    result: dict[str, str] = {}
    for word, value in entries.items():
        ipa = value.get("ipa") if isinstance(value, dict) else None
        if not isinstance(ipa, str):
            continue
        ipa = ipa.strip()
        key = word.strip().lower()
        if not ipa or "*" in ipa or " " in ipa or not _is_ascii_word(key):
            continue
        result[key] = ipa
    return result


def format_ipa(ipa: dict[str, str]) -> list[str]:
    """hallelujahim-ipa.txt 的数据行：`小写单词<TAB>音标`，按单词排序。"""
    return [f"{word}\t{ipa[word]}" for word in sorted(ipa)]


def parse_ipa(text: str, name: str) -> dict[str, str]:
    """msime-dictionary 的 sources/english/hallelujahim-ipa.txt（由 scripts/generate-english-extras.py 生成）。"""
    result: dict[str, str] = {}
    for number, line in enumerate(textutil.without_bom(text).splitlines(), start=1):
        if not line.strip() or line.startswith("#"):
            continue
        word, separator, ipa = line.partition("\t")
        if not separator or not textutil.is_ascii_lower_word(word) or not ipa.strip() or " " in ipa:
            raise ValueError(f"{name}:{number}: expected a lowercase word<TAB>IPA, got {line!r}")
        if word in result:
            raise ValueError(f"{name}:{number}: duplicate word {word!r}")
        result[word] = ipa.strip()
    if not result:
        raise ValueError(f"{name}: no transcriptions found")
    return result


def write_ipa(connection: sqlite3.Connection, ipa: dict[str, str]) -> int:
    """只给 english_words 里有的词写音标。返回写入的条数。"""
    words = {row[0] for row in connection.execute("SELECT DISTINCT word FROM english_words")}
    rows = sorted((word, value) for word, value in ipa.items() if word in words)
    with connection:
        connection.execute("DROP TABLE IF EXISTS en_ipa")
        connection.execute(CREATE_EN_IPA)
        connection.executemany("INSERT INTO en_ipa(english, ipa) VALUES (?, ?)", rows)
        connection.execute("INSERT OR REPLACE INTO source_notices(source, notice) VALUES (?, ?)",
                           (IPA_NOTICE_SOURCE, IPA_NOTICE))
    return len(rows)
