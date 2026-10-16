"""Validate the documented skin package contract and all shipped asset references."""
from pathlib import Path
import os
import re
import tomllib
import unittest

ROOT = Path(__file__).resolve().parents[1]
# MSIME_SKINS_DIR points the same checks at another skin collection, e.g. the repository's skins/.
SKINS_DIR = Path(os.environ.get("MSIME_SKINS_DIR", ROOT / "skins")).resolve()
BUILTIN_SKINS = ("fluent", "wechat", "graphite", "willow_green", "autumn_osmanthus", "microsoft")
# Mirrors the Server catalog: community bases shared with msime, adapted onto fluent at load time.
SHARED_BASES = ("system", "shuishan", "light", "paper", "night", "ink")


class ManifestTests(unittest.TestCase):
    def test_shipped_skins(self):
        manifests = list(SKINS_DIR.glob("*/skin.toml"))
        self.assertTrue(manifests, "No skin packages found")
        for path in manifests:
            with self.subTest(skin=path.parent.name):
                data = tomllib.loads(path.read_text(encoding="utf-8"))
                self.assertIn(data["schema_version"], (1, 2))
                self.assertEqual(data["id"], path.parent.name)
                self.assertRegex(data["id"], r"^[a-z0-9._-]{1,64}$")
                # Mirrors the Server catalog: name, version and base are required, author is not.
                for key in ("name", "version", "base"):
                    self.assertTrue(data[key].strip(), key)
                self.assertIn(data["base"], BUILTIN_SKINS + SHARED_BASES)
                window = data.get("candidate_window", {})
                assets = {key: data.get(key) for key in ("toolbar_stylesheet", "preview")}
                for table in ("decoration", "background"):
                    if table in window:
                        assets[f"candidate_window.{table}.image"] = window[table]["image"]
                if "border_image" in window:
                    assets["candidate_window.border_image.source"] = window["border_image"]["source"]
                for i, dec in enumerate(window.get("decorations", [])):
                    if "image" in dec:
                        assets[f"candidate_window.decorations[{i}].image"] = dec["image"]
                if "decoration" in window:
                    for key in ("top_inset_dip", "width_dip"):
                        self.assertGreater(window["decoration"][key], 0, key)
                for key, rel in assets.items():
                    if rel is None:
                        continue
                    asset = (path.parent / rel).resolve()
                    self.assertTrue(asset.is_relative_to(path.parent.resolve()), key)
                    self.assertTrue(asset.is_file(), f"Missing {key}: {asset}")
                    if asset.suffix.lower() == ".png":
                        self.assertLessEqual(asset.stat().st_size, 500 * 1024, key)
                for scope in ("candidate_window", "toolbar"):
                    if "corner_radius_dip" in data.get(scope, {}):
                        self.assertTrue(0 <= data[scope]["corner_radius_dip"] <= 32, scope)
                if "background" in window:
                    self.assertTrue(0 <= window["background"].get("opacity", 1) <= 1)
                for key, limit in (("border_width_dip", 4), ("item_corner_radius_dip", 16)):
                    if key in window:
                        self.assertTrue(0 <= window[key] <= limit, key)
                if "shadow" in window:
                    self.assertIn(window["shadow"], ("none", "soft", "strong"))
                if "page_arrows" in window:
                    self.assertIsInstance(window["page_arrows"], bool, "page_arrows")
                if "fit_content" in window:
                    self.assertIsInstance(window["fit_content"], bool, "fit_content")
                if "font_family" in window:
                    # Mirrors the Server catalog: the name is pasted into a quoted CSS font-family value.
                    self.assertRegex(window["font_family"], r"^[^\x00-\x1f\x7f\"'\\,;{}<>`]+$")
                    self.assertLessEqual(len(window["font_family"].encode("utf-8")), 64)
                for theme, colors in data.get("candidate", {}).items():
                    if "menu" in colors:
                        self.assertIsInstance(colors["menu"], dict, f"candidate.{theme}.menu")
                for theme in data["supports"]["themes"]:
                    self.assertIn(theme, data["candidate"])
                    if "toolbar" in data:
                        self.assertIn(theme, data["toolbar"])

    def test_default_skin_settings(self):
        # skins/default/<id>/skin.toml carries the built-in skins' switches; only collections that ship it are checked.
        default_dir = SKINS_DIR / "default"
        if not default_dir.is_dir():
            self.skipTest("No default skin settings in this collection")
        manifests = {path.parent.name: path for path in default_dir.glob("*/skin.toml")}
        self.assertEqual(sorted(manifests), sorted(BUILTIN_SKINS))
        for skin_id, path in manifests.items():
            with self.subTest(skin=skin_id):
                data = tomllib.loads(path.read_text(encoding="utf-8"))
                self.assertEqual(data["schema_version"], 1)
                self.assertEqual(data["id"], skin_id)
                self.assertIsInstance(data.get("candidate_window", {}).get("page_arrows", True), bool)
                self.assertIsInstance(data.get("candidate_window", {}).get("fit_content", True), bool)


if __name__ == "__main__":
    unittest.main()
