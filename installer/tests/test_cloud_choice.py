"""Check the production install order that protects user configuration."""

from pathlib import Path
import unittest


SCRIPT = (Path(__file__).resolve().parents[1] / "msime_setup.iss").read_text(encoding="utf-8")


def function_body(name: str) -> str:
    start = SCRIPT.index(f"function {name}(")
    return SCRIPT[start : SCRIPT.index("\nend;", start)]


class CloudChoiceTests(unittest.TestCase):
    def test_migration_conflict_is_checked_before_stop_and_migration(self):
        prepare = function_body("PrepareToInstall")
        migrate = function_body("MigrateUserDataDir")

        self.assertLess(prepare.index("ConfigMigrationProblem("), prepare.index("StopImeProcesses;"))
        self.assertLess(migrate.index("ConfigMigrationProblem("), migrate.index("Log('Migrating user data"))


if __name__ == "__main__":
    unittest.main()
