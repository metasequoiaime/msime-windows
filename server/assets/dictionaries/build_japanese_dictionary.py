"""下载开放词典，在本机构建日语候选释义词库。需要 Python 3.10+。"""

import argparse
from contextlib import closing
from datetime import date
import json
from pathlib import Path
import re
import shutil
import sqlite3
import sys
import tempfile
from urllib.parse import quote
from urllib.request import urlopen
import zipfile

PRIMARY_RELEASE = "https://github.com/metasequoiaime/chinese-ime-lm/releases/download/offline-glosses-2026.09.02/"
JMDICT_RELEASE = "https://api.github.com/repos/scriptin/jmdict-simplified/releases/latest"
WORDNET = "https://raw.githubusercontent.com/omwn/omw-data/406bf83b3c507a3d1f26e88252d5d66893fd36bf/wns/"
SOURCES = {
    "primary.db": PRIMARY_RELEASE + "zh-ja.db",
    "jawiktionary.jsonl": "https://kaikki.org/jawiktionary/" + quote("中国語") + "/kaikki.org-dictionary-" + quote("中国語") + ".jsonl",
    "zhwiktionary.jsonl": "https://kaikki.org/zhwiktionary/" + quote("日语") + "/kaikki.org-dictionary-" + quote("日语") + ".jsonl",
    "chinese.tab": WORDNET + "cow/wn-data-cmn.tab",
    "japanese.tab": WORDNET + "jpn/wn-data-jpn.tab",
}
LICENSES = {
    "offline-glosses-NOTICE.txt": PRIMARY_RELEASE + "offline-glosses-NOTICE.txt",
    "jmdict-LICENSE.html": "https://www.edrdg.org/edrdg/licence.html",
    "cow-LICENSE.txt": WORDNET + "cow/LICENSE",
    "jpn-LICENSE.txt": WORDNET + "jpn/LICENSE",
}
HAN_WORD = re.compile(r"[\u3400-\u9fff]{1,12}")


def download(url, destination):
    print("下载：" + destination.name, flush=True)
    with urlopen(url, timeout=60) as response, destination.open("wb") as output:
        shutil.copyfileobj(response, output)


def jmdict_url():
    with urlopen(JMDICT_RELEASE, timeout=60) as response:
        release = json.load(response)
    return next(asset["browser_download_url"] for asset in release["assets"]
                if re.fullmatch(r"jmdict-eng-\d.*\.json\.zip", asset["name"]))


def lexical_parts(text):
    text = re.sub(r"^(?:\s*[（(〈《【][^）)〉》】]*[）)〉》】])+\s*", "", text)
    return [part.strip() for part in re.split(r"[、,，;；。/：:]", text)]


def first_translation(text):
    return re.split(r"[;,，；、]", text, maxsplit=1)[0].strip()


def load_jmdict(path):
    with zipfile.ZipFile(path) as archive:
        name = next(name for name in archive.namelist() if name.endswith(".json"))
        return json.loads(archive.read(name))["words"]


def japanese_priority(word, common):
    return word not in common, len(word), word


def add_japanese_wiktionary(entries, path, known):
    with path.open(encoding="utf-8") as source:
        for line in source:
            entry = json.loads(line)
            if entry.get("lang_code") != "zh":
                continue
            parts = (part for sense in entry.get("senses", [])
                     for gloss in sense.get("glosses", []) for part in lexical_parts(gloss))
            translation = next((part for part in parts if part in known), None)
            if translation is None:
                continue
            forms = [entry["word"], *(form["form"] for form in entry.get("forms", []))]
            for form in forms:
                if HAN_WORD.fullmatch(form):
                    entries.setdefault(form, (translation, "https://ja.wiktionary.org/wiki/" + quote(entry["word"])))


def add_chinese_wiktionary(entries, path, known, common):
    with path.open(encoding="utf-8") as source:
        words = [json.loads(line) for line in source]
    words.sort(key=lambda entry: japanese_priority(entry["word"], common))
    for entry in words:
        word = entry["word"]
        if entry.get("lang_code") != "ja" or word not in known:
            continue
        parts = (part for sense in entry.get("senses", [])
                 for gloss in sense.get("glosses", []) for part in lexical_parts(gloss))
        for part in parts:
            if len(part) <= 8 and HAN_WORD.fullmatch(part):
                entries.setdefault(part, (word, "https://zh.wiktionary.org/wiki/" + quote(word)))


def wordnet_lemmas(path, language):
    result = {}
    with path.open(encoding="utf-8-sig") as source:
        for line in source:
            fields = line.rstrip("\r\n").split("\t")
            if len(fields) == 3 and fields[1] == language + ":lemma":
                result.setdefault(fields[0], []).append(fields[2])
    return result


def add_wordnet(entries, directory, known, common):
    chinese = wordnet_lemmas(directory / "chinese.tab", "cmn")
    japanese = wordnet_lemmas(directory / "japanese.tab", "jpn")
    for synset, words in chinese.items():
        candidates = {word.replace("+", "") for word in japanese.get(synset, [])} & known
        if not candidates:
            continue
        translation = min(candidates, key=lambda word: japanese_priority(word, common))
        for word in words:
            if HAN_WORD.fullmatch(word):
                entries.setdefault(word, (translation, "open-multilingual-wordnet:" + synset))


def select_readings(words, targets):
    selected = {}
    for word in words:
        for kanji in word["kanji"]:
            form = kanji["text"]
            if form not in targets:
                continue
            for kana in word["kana"]:
                applies = kana["appliesToKanji"]
                if "*" not in applies and form not in applies:
                    continue
                priority = not kana["common"], not kanji["common"]
                if form not in selected or priority < selected[form][0]:
                    selected[form] = priority, kana["text"]
    return {form: reading for form, (_, reading) in selected.items()}


def compact_entry(gloss, readings):
    word = first_translation(gloss)
    reading = ""
    if re.search(r"[\u3400-\u9fff々]", word):
        reading = readings.get(word, "")
        if not reading and word.endswith("の") and word[:-1] in readings:
            reading = readings[word[:-1]] + "の"
    return word, reading


def build_database(directory, output):
    print("合并释义和假名读音", flush=True)
    words = load_jmdict(directory / "jmdict.zip")
    forms = [form for word in words for form in word["kanji"] + word["kana"]]
    known = {form["text"] for form in forms}
    common = {form["text"] for form in forms if form["common"]}
    with closing(sqlite3.connect((directory / "primary.db").as_uri() + "?mode=ro", uri=True)) as db:
        entries = {chinese: (gloss, "https://en.wiktionary.org/wiki/" + quote(source))
                   for chinese, gloss, source in db.execute("SELECT chinese, gloss, source FROM zh_glosses")}
    add_japanese_wiktionary(entries, directory / "jawiktionary.jsonl", known)
    add_chinese_wiktionary(entries, directory / "zhwiktionary.jsonl", known, common)
    add_wordnet(entries, directory, known, common)
    targets = {first_translation(gloss) for gloss, _ in entries.values()}
    readings = select_readings(words, targets | {word.removesuffix("の") for word in targets})
    with closing(sqlite3.connect(directory / "japanese.db")) as db:
        db.execute("CREATE TABLE zh_glosses(chinese TEXT COLLATE BINARY PRIMARY KEY, gloss TEXT NOT NULL,"
                   "reading TEXT NOT NULL, source TEXT NOT NULL) WITHOUT ROWID")
        db.executemany("INSERT INTO zh_glosses VALUES (?, ?, ?, ?)",
                       ((chinese, *compact_entry(gloss, readings), source)
                        for chinese, (gloss, source) in sorted(entries.items())))
        db.commit()
    # 运行中的 Server 可能仍持有旧词库；重启后由查询线程把新文件放到正式路径。
    (directory / "japanese.db").replace(output / "japanese.db.pending")
    print(f"已构建 {len(entries)} 个词头，重启输入法后生效", flush=True)


def write_notices(directory, output):
    notices = directory / "japanese-licenses"
    notices.mkdir()
    for name, url in LICENSES.items():
        download(url, notices / name)
    sources = "\n".join(f"- {name}: {url}" for name, url in SOURCES.items())
    (notices / "NOTICE.md").write_text(
        "# 日语候选释义词库\n\n"
        "Wiktionary contributors 的释义由 Kaikki/Wiktextract 提取。Wiktionary 改编数据采用 CC BY-SA 4.0。\n"
        "Chinese Open Wordnet（Francis Bond、Shan Wang）与 Japanese Wordnet（NICT）保留各自授权声明。\n"
        "假名读音及日语词形来自 JMdict，版权归 EDRDG / Jim Breen，采用 CC BY-SA 4.0。\n"
        "JMdict 的使用遵循 EDRDG 授权：https://www.edrdg.org/edrdg/licence.html\n"
        "CC BY-SA 4.0：https://creativecommons.org/licenses/by-sa/4.0/\n\n"
        "改编：按义项配对中日词头，按来源顺序补缺，保留首个释义并添加常用假名读音。\n\n"
        f"下载日期：{date.today().isoformat()}\n\n{sources}\n", encoding="utf-8")
    shutil.copytree(notices, output / "japanese-licenses", dirs_exist_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-root", type=Path, required=True, help="输入法数据目录")
    args = parser.parse_args()
    output = args.output_root.resolve()
    output.mkdir(parents=True, exist_ok=True)
    SOURCES["jmdict.zip"] = jmdict_url()
    with tempfile.TemporaryDirectory(prefix="msime-japanese-", dir=output) as temporary:
        directory = Path(temporary)
        for name, url in SOURCES.items():
            download(url, directory / name)
        write_notices(directory, output)
        build_database(directory, output)


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
