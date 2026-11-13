import asyncio
from unittest.mock import AsyncMock, MagicMock, Mock, patch

from crawler.__main__ import main, run
from tests.support import AsyncUnitTestCase, UnitTestCase, config


class RunTests(AsyncUnitTestCase):
    async def test_exit_status_signal_handlers_and_resource_cleanup(self):
        for state, code in (("stopped", 0), ("shortfall", 2), ("failed", 2)):
            with self.subTest(state=state):
                client, store = Mock(), Mock()
                http = MagicMock()
                engine = Mock()
                engine.run = AsyncMock(return_value=state)
                loop = asyncio.get_running_loop()
                with patch("crawler.__main__.connect", return_value=(client, store)), patch("crawler.__main__.make_client", return_value=http), patch("crawler.__main__.Engine", return_value=engine), patch.object(loop, "add_signal_handler") as handler:
                    self.assertEqual(await run(config()), code)
                self.assertEqual(handler.call_count, 2)
                client.close.assert_called_once()
                http.__aexit__.assert_awaited_once()

    async def test_engine_error_still_closes_clients(self):
        client = Mock()
        http = MagicMock()
        engine = Mock(run=AsyncMock(side_effect=RuntimeError("failure")))
        with patch("crawler.__main__.connect", return_value=(client, Mock())), patch("crawler.__main__.make_client", return_value=http), patch("crawler.__main__.Engine", return_value=engine), patch.object(asyncio.get_running_loop(), "add_signal_handler"):
            with self.assertRaises(RuntimeError):
                await run(config())
        client.close.assert_called_once()
        http.__aexit__.assert_awaited_once()


class MainTests(UnitTestCase):
    def test_invalid_config_returns_argument_error(self):
        with patch("sys.argv", ["crawler", "bad.yaml"]), patch("crawler.__main__.load_config", side_effect=ValueError("bad")):
            with self.assertRaises(SystemExit) as error:
                main()
        self.assertEqual(error.exception.code, 2)

    def test_valid_config_is_passed_to_async_entrypoint(self):
        data = config()
        with patch("sys.argv", ["crawler", "unit.yaml"]), patch("crawler.__main__.load_config", return_value=data), patch("crawler.__main__.run", new=Mock(return_value="awaitable")) as entry, patch("crawler.__main__.asyncio.run", return_value=0):
            self.assertEqual(main(), 0)
        entry.assert_called_once_with(data)
