"""Exercise the real Windows trainer against a synthetic target, never the game.

Set BB_TRAINER_FIXTURE to the compiled trainer_fixture_windows.exe path.
"""
# SPDX-License-Identifier: GPL-2.0-or-later
# Windows trainer validation by yaonikaixin999999, 2026-10-06.
import os
from pathlib import Path
import subprocess
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))


@unittest.skipUnless(os.name == 'nt' and os.environ.get('BB_TRAINER_FIXTURE'),
                     'Requires an explicit synthetic Windows fixture')
class NativeTrainerIntegration(unittest.TestCase):
    def test_actual_remote_hooks_reconfigure_and_restore(self):
        from trainer_windows import TrainerSession, WindowsAPI
        fixture = Path(os.environ['BB_TRAINER_FIXTURE']).resolve()
        target = subprocess.Popen([str(fixture)], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                  text=True, bufsize=1)
        session = None
        try:
            self.assertEqual(target.stdout.readline().strip(), f'READY {target.pid}')

            def tick(command='tick'):
                target.stdin.write(command + '\n')
                target.stdin.flush()
                reply = target.stdout.readline().strip().split()
                self.assertEqual(reply[0], 'VALUES')
                return tuple(map(int, reply[1:]))

            self.assertEqual(tick(), (50, 10, 123, 3))
            self.assertEqual(tick('tick_skip'), (50, 10, 123, 3))
            target.stdin.write('spin\n')
            target.stdin.flush()
            self.assertEqual(target.stdout.readline().strip(), 'SPINNING')
            session = TrainerSession.connect(target.pid, _api=WindowsAPI(), _allowed_paths=[fixture])
            self.assertTrue(session.is_alive())
            self.assertEqual(tick(), (50, 10, 123, 3))
            session.apply({'health', 'stamina', 'echoes', 'items'}, echoes=1000000, items=20)
            self.assertEqual(tick(), (1000, 160, 1000000, 20))
            self.assertEqual(tick('tick_skip'), (1000, 160, 1000000, 20))
            session.apply({'health', 'stamina', 'echoes', 'items'}, echoes=8765432, items=9)
            self.assertEqual(tick(), (1000, 160, 8765432, 9))
            self.assertEqual(tick('tick_skip'), (1000, 160, 8765432, 9))
            for index in range(20):
                session.apply({'health', 'stamina', 'echoes', 'items'}, echoes=1000 + index, items=20)
                self.assertEqual(tick(), (1000, 160, 1000 + index, 20))
            session.apply({'health'})
            self.assertEqual(tick(), (1000, 10, 123, 3))
            session.apply(set())
            self.assertEqual(tick(), (50, 10, 123, 3))
            session.apply({'stamina', 'items'}, items=12)
            self.assertEqual(tick(), (50, 160, 123, 12))
            session.close()
            session = None
            self.assertEqual(tick(), (50, 10, 123, 3))
        finally:
            if session is not None:
                session.close()
            if target.poll() is None:
                target.stdin.write('quit\n')
                target.stdin.flush()
            try:
                target.wait(timeout=5)
            except subprocess.TimeoutExpired:
                target.terminate()
                target.wait(timeout=5)
            target.stdin.close()
            target.stdout.close()
            stderr = target.stderr.read()
            target.stderr.close()
            self.assertEqual(target.returncode, 0, stderr)


if __name__ == '__main__':
    unittest.main()
