# SPDX-License-Identifier: GPL-2.0-or-later
# Windows trainer validation by yaonikaixin999999, 2026-10-06.
"""Verify explicit activation and failed-restoration behavior without game access."""
from pathlib import Path
import sys
import tempfile
import tkinter as tk
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import bloodborne_trainer as ui


class Session:
    pid = 123

    def __init__(self):
        self.enabled = set()
        self.calls = []
        self.alive = True
        self.fail_close = False

    def apply(self, enabled, **kwargs):
        self.enabled = set(enabled)
        self.calls.append((set(enabled), kwargs))

    def is_alive(self):
        return self.alive

    def close(self):
        if self.fail_close:
            raise RuntimeError('restore rejected')
        self.enabled.clear()


class TrainerUITests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.preferences = Path(self.temp.name) / 'trainer.json'
        self.session = Session()
        backend = SimpleNamespace(
            discover_games=lambda: [SimpleNamespace(pid=123, path='bb-probe.exe')],
            TrainerSession=SimpleNamespace(connect=lambda pid: self.session))
        self.root = tk.Tk()
        self.root.withdraw()
        self.dialog = patch.object(ui.messagebox, 'showerror')
        self.dialog.start()
        self.backend = backend

    def tearDown(self):
        if self.root.winfo_exists():
            self.root.destroy()
        self.dialog.stop()
        self.temp.cleanup()

    def app(self):
        return ui.TrainerApp(self.root, backend=self.backend, preferences_path=self.preferences)

    def test_saved_enabled_features_are_dormant_until_apply(self):
        preferences = ui.DEFAULT_PREFERENCES | {'health': True, 'items': True, 'items_amount': 9}
        ui.save_preferences(preferences, self.preferences)
        app = self.app()
        app.connect()
        self.assertEqual(self.session.calls, [])
        self.assertEqual(self.session.enabled, set())
        app.apply_settings()
        self.assertEqual(self.session.enabled, {'health', 'items'})
        self.assertEqual(self.session.calls[-1][1]['items'], 9)
        app.restore_all()
        self.assertEqual(self.session.enabled, set())
        self.assertTrue(all(not value.get() for value in app.feature_vars.values()))
        app.disconnect()
        self.assertIsNone(app.session)

    def test_invalid_number_does_not_apply_or_save(self):
        app = self.app()
        app.connect()
        app.feature_vars['echoes'].set(True)
        for value in ('-1', '999999999', '1.5', 'nan'):
            app.echoes.set(value)
            app.apply_settings()
        self.assertEqual(self.session.calls, [])
        self.assertFalse(self.preferences.exists())

    def test_restore_failure_keeps_window_and_session_for_retry(self):
        app = self.app()
        app.connect()
        self.session.enabled = {'health'}
        self.session.fail_close = True
        app.close_window()
        self.assertTrue(self.root.winfo_exists())
        self.assertIs(app.session, self.session)
        self.assertEqual(self.session.enabled, {'health'})
        self.session.fail_close = False
        app.disconnect()
        self.assertIsNone(app.session)

    def test_corrupt_preferences_cannot_enable_invalid_values(self):
        self.preferences.write_text('{"health":"yes","echoes_amount":true,"items_amount":999}', encoding='utf-8')
        self.assertEqual(ui.read_preferences(self.preferences), ui.DEFAULT_PREFERENCES)
        for value in (True, 1.1, '1e6'):
            with self.assertRaises(ValueError):
                ui.validate_amounts(value, 20)


if __name__ == '__main__':
    unittest.main()
