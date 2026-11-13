import argparse
from unittest.mock import Mock, patch

from corpus.__main__ import main, positive
from tests.support import UnitTestCase, config


class CliTests(UnitTestCase):
    def test_positive_argument(self):
        self.assertEqual(positive("3"), 3)
        with self.assertRaises(argparse.ArgumentTypeError):
            positive("0")

    def test_success_passes_selection_and_closes_repository(self):
        with patch("corpus.__main__.load_config", return_value=config()), patch("corpus.__main__.Repository") as repository, patch("corpus.__main__.run_analysis") as run:
            self.assertEqual(main(["inspect", "unit.yaml", "--run-id", "unit", "--source", "example", "--limit", "2"]), 0)
        self.assertEqual(run.call_args.kwargs["limit"], 2)
        self.assertEqual(run.call_args.kwargs["mode"], "inspect")
        repository.return_value.close.assert_called_once()

    def test_failures_and_interrupts_close_repository_and_return_exit_codes(self):
        for error, code in ((RuntimeError("bad"), 2), (KeyboardInterrupt(), 130)):
            with self.subTest(error=error):
                with patch("corpus.__main__.load_config", return_value=config()), patch("corpus.__main__.Repository") as repository, patch("corpus.__main__.run_analysis", side_effect=error):
                    self.assertEqual(main(["extract", "unit.yaml"]), code)
                repository.return_value.close.assert_called_once()

    def test_invalid_cli_does_not_open_repository(self):
        with patch("corpus.__main__.Repository") as repository:
            with self.assertRaises(SystemExit) as error:
                main(["inspect", "unit.yaml", "--limit", "0"])
        self.assertEqual(error.exception.code, 2)
        repository.assert_not_called()
