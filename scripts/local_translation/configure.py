"""Back up and patch the existing IME translation provider without reserializing TOML."""
from __future__ import annotations

import argparse
import datetime
import json
from pathlib import Path
import re
import tomllib

UPDATES = {
    "general": {"candidate_translations": True},
    "tencent_tmt": {"enabled": False, "target_language": "en"},
    "niutrans": {"enabled": False},
}


def patch_section(text: str, section: str, updates: dict) -> str:
    header = re.search(r"(?m)^\[" + re.escape(section) + r"\][ \t]*(?:#.*)?$", text)
    if not header:
        text = text.rstrip() + f"\n\n[{section}]\n"
        header = re.search(r"(?m)^\[" + re.escape(section) + r"\]$", text)
    following = re.search(r"(?m)^\[", text[header.end():])
    end = header.end() + following.start() if following else len(text)
    content = text[header.end():end]
    for key, value in updates.items():
        literal = json.dumps(value, ensure_ascii=False)
        match = re.search(r"(?m)^([ \t]*" + re.escape(key) + r"[ \t]*=[ \t]*).*$", content)
        line = f"{key} = {literal}"
        if match:
            content = content[:match.start()] + line + content[match.end():]
        else:
            content = content.rstrip() + "\n" + line + "\n\n"
    return text[:header.end()] + content + text[end:]


def configure(config_path: Path, service_config: dict, backup_directory: Path):
    original = config_path.read_bytes()
    text = original.decode("utf-8-sig").replace("\r\n", "\n")
    tomllib.loads(text)  # Refuse an invalid existing configuration.
    updates = {**UPDATES, "custom_translation": {"enabled": True,
               "endpoint": f"http://127.0.0.1:{service_config['port']}/translate", "api_key": service_config["token"]}}
    for section, values in updates.items():
        text = patch_section(text, section, values)
    parsed = tomllib.loads(text)
    for section, values in updates.items():
        for key, value in values.items():
            if parsed[section][key] != value:
                raise ValueError("Translation configuration did not round-trip")
    backup_directory.mkdir(parents=True, exist_ok=True)
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    backup = backup_directory / f"ime-config-before-{stamp}.toml"
    backup.write_bytes(original)
    incoming = config_path.with_name(config_path.name + ".local-translation.incoming")
    incoming.write_text(text, encoding="utf-8", newline="\n")
    incoming.replace(config_path)
    return backup


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ime-config", type=Path, required=True)
    parser.add_argument("--service-config", type=Path, required=True)
    parser.add_argument("--backup-directory", type=Path, required=True)
    args = parser.parse_args()
    config = json.loads(args.service_config.read_text(encoding="utf-8"))
    backup = configure(args.ime_config, config, args.backup_directory)
    print(f"Configured loopback translation; original configuration backed up to {backup}")


if __name__ == "__main__":
    main()
