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
        self.assertIn("TranslationMigrationFailure(", migrate)

    def test_conflict_is_rejected_on_the_data_dir_page(self):
        self.assertIn("ConfigMigrationProblem(", function_body("NextButtonClick"))

    def test_cloud_choice_runs_after_required_post_install_steps(self):
        post_install = function_body("CurStepChanged", "procedure")
        for step in ("CreateWatchdogLogonTask;", "EnsureImeUserDataDir;", "'MetasequoiaImeWatchdog'"):
            self.assertLess(post_install.index(step), post_install.index("ApplyNetworkChoiceToUserConfig;"))

    def test_usage_reporting_decision_is_read_before_migration_and_written_last(self):
        prepare = function_body("PrepareToInstall")
        self.assertLess(prepare.index("RememberUsageReportingDecision;"), prepare.index("MigrateUserDataDir("))
        post_install = function_body("CurStepChanged", "procedure")
        for step in ("CreateWatchdogLogonTask;", "EnsureImeUserDataDir;", "ApplyNetworkChoiceToUserConfig;"):
            self.assertLess(post_install.index(step), post_install.index("ApplyUsageReportingChoice(not WizardSilent);"))

    def test_usage_reporting_page_is_not_limited_to_fresh_installs(self):
        # 升级用户占大多数：询问页只看有没有记下选择，不能像云候选页那样因为已有 config.toml 就跳过。
        skip = function_body("ShouldSkipPage")
        self.assertIn("ShouldSkipUsageReportingPage", skip)
        self.assertNotIn("UserConfigExistsBeforeInstall", skip[skip.index("UsageReportingPage"):])


if __name__ == "__main__":
    unittest.main()
