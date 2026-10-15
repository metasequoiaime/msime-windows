"""Check the production install order that protects user configuration."""

from pathlib import Path
import unittest


SCRIPT = (Path(__file__).resolve().parents[1] / "msime_setup.iss").read_text(encoding="utf-8")


def function_body(name: str, kind: str = "function") -> str:
    start = SCRIPT.index(f"{kind} {name}(")
    return SCRIPT[start : SCRIPT.index("\nend;", start)]


class CloudChoiceTests(unittest.TestCase):
    def test_migration_conflict_is_checked_before_stop_and_migration(self):
        prepare = function_body("PrepareToInstall")
        migrate = function_body("MigrateUserDataDir")

        self.assertLess(prepare.index("ConfigMigrationProblem("), prepare.index("StopImeProcesses;"))
        self.assertLess(migrate.index("ConfigMigrationProblem("), migrate.index("Log('Migrating user data"))
        self.assertIn("ConfigMigrationFailure(", migrate)

    def test_conflict_is_rejected_on_the_data_dir_page(self):
        self.assertIn("ConfigMigrationProblem(", function_body("NextButtonClick"))

    def test_cloud_choice_runs_after_required_post_install_steps(self):
        post_install = function_body("CurStepChanged", "procedure")
        for step in ("CreateWatchdogLogonTask;", "EnsureImeUserDataDir;", "'MetasequoiaImeWatchdog'"):
            self.assertLess(post_install.index(step), post_install.index("ApplyNetworkChoiceToUserConfig;"))


if __name__ == "__main__":
    unittest.main()
