"""Check the install phase that decides whether to keep a user config."""

from pathlib import Path
import re
import unittest


INSTALLER = Path(__file__).resolve().parents[1]
SCRIPT = (INSTALLER / "msime_setup.iss").read_text(encoding="utf-8")
CHOICE = (INSTALLER / "cloud_choice.iss").read_text(encoding="utf-8")


def body(signature: str, source: str = SCRIPT) -> str:
    match = re.search(rf"(?m)^{re.escape(signature)}\b.*?^end;", source, re.DOTALL)
    if match is None:
        raise AssertionError(f"Missing installer function: {signature}")
    return match.group()


class CloudChoiceTests(unittest.TestCase):
    def test_final_directory_controls_config_preservation(self):
        wizard = body("procedure InitializeWizard")
        skip = body("function ShouldSkipPage")
        prepare = body("function PrepareToInstall")
        existing = body("function UserConfigExistsBeforeInstall", CHOICE)

        self.assertNotIn("UserConfigExistedBeforeInstall :=", wizard)
        self.assertIn('#include "cloud_choice.iss"', SCRIPT)
        self.assertIn("UserConfigExistedBeforeInstall := UserConfigExistsBeforeInstall", skip)
        self.assertLess(
            prepare.index("DataDirValue := AdoptableDataDir"),
            prepare.index("UserConfigExistedBeforeInstall := UserConfigExistsBeforeInstall"),
        )
        self.assertLess(
            prepare.index("UserConfigExistedBeforeInstall := UserConfigExistsBeforeInstall"),
            prepare.index("MigrateUserDataDir"),
        )
        self.assertIn("FileExists(UserConfigPath)", existing)
        self.assertIn("FileExists(AddBackslash(ResolvePreviousDataDir) + 'config.toml')", existing)

    def test_new_config_uses_utf8_and_existing_config_is_not_replaced(self):
        apply_choice = body("procedure ApplyNetworkChoiceToUserConfig", CHOICE)
        migrate = body("function MigrateUserDataDir")
        self.assertIn("if UserConfigExistedBeforeInstall", apply_choice)
        self.assertIn("SaveStringsToUTF8FileWithoutBOM(UserConfigPath", apply_choice)
        self.assertNotIn("SaveStringsToFile(UserConfigPath", apply_choice)
        self.assertIn("RaiseException('无法读取新安装的用户配置", apply_choice)
        self.assertIn("RaiseException('新安装的用户配置缺少 cloud_candidates", apply_choice)
        self.assertRegex(
            SCRIPT,
            r'DestName: "config\.toml";\s*\\\s*Flags: onlyifdoesntexist',
        )
        self.assertLess(migrate.index("目标数据目录已有 config.toml"), migrate.index("Log('Migrating user data"))


if __name__ == "__main__":
    unittest.main()
