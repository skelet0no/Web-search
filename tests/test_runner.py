from unittest.mock import patch

from tests import __main__ as runner
from tests.support import UnitTestCase


class RunnerTests(UnitTestCase):
    def invoke(self, args=(), codes=(0, 0, 0, 0)):
        with patch.object(runner, "execute", side_effect=codes) as execute, patch.object(runner.shutil, "which", return_value="tool"):
            result = runner.main(list(args))
        return result, [call.args[0] for call in execute.call_args_list]

    def test_default_runs_both_languages_and_all_cpp_modules(self):
        result, commands = self.invoke()
        self.assertEqual(result, 0)
        self.assertIn("unittest", commands[0])
        self.assertIn("-DZIPF_BUILD=ON", commands[1])
        self.assertIn("-DBOOLEAN_BUILD=ON", commands[1])
        self.assertIn("--build", commands[2])
        self.assertEqual(commands[3][0], "ctest")
        self.assertIn("--no-tests=error", commands[3])

    def test_python_failure_is_preserved_after_successful_cpp_tests(self):
        result, commands = self.invoke(codes=(1, 0, 0, 0))
        self.assertEqual(result, 1)
        self.assertEqual(len(commands), 4)

    def test_configure_and_build_failure_prevent_ctest(self):
        for codes, length in (((0, 1), 2), ((0, 0, 1), 3)):
            result, commands = self.invoke(codes=codes)
            self.assertEqual(result, 1)
            self.assertEqual(len(commands), length)

    def test_python_section_needs_no_cmake(self):
        result, commands = self.invoke(("--section", "crawler"), codes=(0,))
        self.assertEqual(result, 0)
        self.assertEqual(len(commands), 1)
        self.assertIn("tests/crawler", commands[0])

    def test_cpp_section_and_unit_filter(self):
        result, commands = self.invoke(("--section", "boolean", "--unit-only"), codes=(0, 0, 0))
        self.assertEqual(result, 0)
        self.assertNotIn("unittest", commands[0])
        self.assertIn("^boolean$", commands[-1])
        self.assertIn("^(integration|dictionary|cli)$", commands[-1])

    def test_missing_tool_is_not_reported_as_success(self):
        with patch.object(runner, "execute", return_value=0), patch.object(runner.shutil, "which", return_value=None):
            self.assertEqual(runner.main([]), 2)

    def test_subprocess_execution_uses_project_root_and_propagates_status(self):
        with patch.object(runner.subprocess, "run") as run:
            run.return_value.returncode = 7
            self.assertEqual(runner.execute(["tool", 1]), 7)
        run.assert_called_once_with(["tool", "1"], cwd=runner.ROOT, check=False)
