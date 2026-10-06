# Windows port modifications by yaonikaixin999999, 2026-10-06.
# SPDX-License-Identifier: GPL-2.0-or-later
"""Verify trainer gating, reversible transactions, and thread safety without game data."""
from paths import ROOT
from pathlib import Path
import struct
import sys
import unittest
from unittest.mock import patch

if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))
import trainer_windows as trainer


class FakeAPI:
    """Sparse process memory with writes forbidden while threads are running."""

    def __init__(self, base=0x0000012300000000):
        self.base = base
        self.pid = 27183
        self.path = str(trainer.SUPPORTED_PATHS[0])
        self.alive = True
        self.memory = {}
        self.allocations = {base: 0x3000000}
        self.protections = {}
        self.owned = False
        self.paused = False
        self.closed = self.freed = self.suspends = self.resumes = self.writes = 0
        self.fail_write = None
        self.partial_failure = False
        self.rips = []
        self.other_regions = []
        for hook in trainer.HOOKS.values():
            self.seed(base + hook.offset, hook.original)

    def seed(self, address, data):
        self.memory.update((address + index, byte) for index, byte in enumerate(data))

    def open_process(self, pid):
        if pid != self.pid:
            raise trainer.TrainerError("unknown process")
        return pid

    def close_handle(self, _):
        self.closed += 1

    def process_path(self, _):
        return self.path

    def processes(self):
        return [trainer.ProcessInfo(self.pid, self.path), trainer.ProcessInfo(99, "C:/other.exe")]

    def is_alive(self, _):
        return self.alive

    def acquire_owner(self, _):
        if self.owned:
            raise trainer.TrainerError("already owned")
        self.owned = True
        return 123

    def release_owner(self, _):
        self.owned = False

    def regions(self, _):
        return [trainer.MemoryRegion(self.base, self.base, self.allocations[self.base],
                                     trainer.MEM_COMMIT, trainer.PAGE_EXECUTE_READ, trainer.MEM_PRIVATE),
                *self.other_regions]

    def _valid(self, address, size):
        if not any(base <= address and address + size <= base + length
                   for base, length in self.allocations.items()):
            raise trainer.TrainerError("unmapped read/write")

    def read(self, _, address, size):
        self._valid(address, size)
        return bytes(self.memory.get(address + index, 0) for index in range(size))

    def write(self, _, address, data):
        if not self.paused:
            raise AssertionError("write while game threads are running")
        self._valid(address, len(data))
        page = address & ~4095
        if self.protections.get(page, trainer.PAGE_EXECUTE_READ) != trainer.PAGE_EXECUTE_READWRITE:
            raise AssertionError("write to protected page")
        self.writes += 1
        if self.fail_write == self.writes:
            if self.partial_failure:
                self.seed(address, data[:max(1, len(data) // 2)])
            raise trainer.TrainerError("injected write failure")
        self.seed(address, data)

    def allocate_near(self, _, lowest, highest):
        base = self.base + 0x8000000
        if not lowest <= base <= highest:
            raise trainer.TrainerError("not in rel32 range")
        self.allocations[base] = trainer.CAVE_SIZE
        self.protections[base] = trainer.PAGE_READWRITE
        return base

    def free(self, _, address):
        self.freed += 1
        del self.allocations[address]
        self.protections.pop(address, None)
        for byte in range(address, address + trainer.CAVE_SIZE):
            self.memory.pop(byte, None)

    def protect(self, _, address, size, protection):
        self._valid(address, size)
        page = address & ~4095
        old = self.protections.get(page, trainer.PAGE_EXECUTE_READ)
        self.protections[page] = protection
        return old

    def flush(self, *_):
        pass

    def suspend_threads(self, _):
        self.suspends += 1
        if self.paused:
            raise AssertionError("nested suspension")
        self.paused = True
        rip = self.rips.pop(0) if self.rips else self.base + 0x100
        return [(11, rip), (12, self.base + 0x200)]

    def resume_threads(self, _):
        if not self.paused:
            raise AssertionError("resume without suspension")
        self.resumes += 1
        self.paused = False


class TrainerTests(unittest.TestCase):
    def setUp(self):
        self.api = FakeAPI()

    def connect(self):
        return trainer.TrainerSession.connect(self.api.pid, _api=self.api)

    def hook_state(self):
        return {key: self.api.read(self.api.pid, self.api.base + hook.offset, len(hook.original))
                for key, hook in trainer.HOOKS.items()}

    def assert_all_original(self):
        self.assertEqual(self.hook_state(), {key: hook.original for key, hook in trainer.HOOKS.items()})

    def test_discovery_accepts_only_exact_project_executables(self):
        found = trainer.discover_games(_api=self.api)
        self.assertEqual(found, [trainer.ProcessInfo(self.api.pid, self.api.path)])
        self.api.path = "C:/malicious/bb-probe.exe"
        self.assertEqual(trainer.discover_games(_api=self.api), [])

    def test_custom_paths_require_injected_api(self):
        with self.assertRaises(trainer.TrainerError):
            trainer.TrainerSession.connect(self.api.pid, _allowed_paths=["C:/fake.exe"])
        with self.assertRaises(trainer.TrainerError):
            trainer.discover_games(_allowed_paths=["C:/fake.exe"])
        self.api.path = "C:/fixture.exe"
        session = trainer.TrainerSession.connect(self.api.pid, _api=self.api,
                                                _allowed_paths=[self.api.path])
        session.close()

    def test_foreign_process_rejected_before_allocation_or_writes(self):
        self.api.path = "C:/other/bb-probe.exe"
        with self.assertRaises(trainer.TrainerError):
            self.connect()
        self.assertEqual(self.api.writes, 0)
        self.assertEqual(len(self.api.allocations), 1)
        self.assertFalse(self.api.owned)
        self.assertEqual(self.api.closed, 1)

    def test_every_fingerprint_is_required(self):
        for feature, hook in trainer.HOOKS.items():
            with self.subTest(feature=feature):
                self.setUp()
                self.api.seed(self.api.base + hook.offset, b"\x90" * len(hook.original))
                with self.assertRaises(trainer.TrainerError):
                    self.connect()
                self.assertEqual(self.api.writes, 0)
                self.assertFalse(self.api.owned)

    def test_multiple_matching_images_are_rejected(self):
        other = self.api.base + 0x300000000
        self.api.allocations[other] = 0x3000000
        self.api.other_regions = [trainer.MemoryRegion(other, other, 0x3000000,
                                                      trainer.MEM_COMMIT, trainer.PAGE_EXECUTE_READ,
                                                      trainer.MEM_PRIVATE)]
        for hook in trainer.HOOKS.values():
            self.api.seed(other + hook.offset, hook.original)
        with self.assertRaises(trainer.TrainerError):
            self.connect()
        self.assertFalse(self.api.owned)

    def test_connect_uses_aslr_base_and_does_not_modify_memory(self):
        session = self.connect()
        self.assertEqual(session.image_base, self.api.base)
        self.assertEqual(session.pid, self.api.pid)
        self.assertTrue(session.is_alive())
        self.assertEqual(session.enabled, set())
        self.assertEqual(self.api.writes, 0)
        self.assert_all_original()
        session.close()

    def test_one_owner_per_process(self):
        session = self.connect()
        with self.assertRaises(trainer.TrainerError):
            self.connect()
        session.close()
        self.assertFalse(self.api.owned)
        self.connect().close()

    def test_all_features_patch_restore_and_free(self):
        session = self.connect()
        session.apply(trainer.FEATURES, echoes=1234567, items=17)
        self.assertEqual(session.enabled, trainer.FEATURES)
        cave = session._cave
        self.assertEqual(self.api.protections[cave], trainer.PAGE_EXECUTE_READ)
        for feature, data in self.hook_state().items():
            address = self.api.base + trainer.HOOKS[feature].offset
            displacement = struct.unpack_from("<i", data, 1)[0]
            slot = list(trainer.HOOKS).index(feature) * trainer.SLOT_SIZE
            self.assertEqual(data[0], 0xE9)
            self.assertEqual(address + 5 + displacement, cave + slot)
        session.close()
        self.assert_all_original()
        self.assertEqual(self.api.freed, 1)
        self.assertFalse(self.api.owned)
        self.assertFalse(self.api.paused)
        self.assertEqual(self.api.suspends, self.api.resumes)
        self.assertFalse(session.is_alive())

    def test_switching_features_restores_disabled_sites(self):
        session = self.connect()
        session.apply({"health", "stamina"})
        session.apply({"items"}, items=99)
        for feature, data in self.hook_state().items():
            if feature != "items":
                self.assertEqual(data, trainer.HOOKS[feature].original)
        self.assertEqual(session.enabled, {"items"})
        session.close()

    def test_value_changes_update_same_cave_and_restore_protections(self):
        session = self.connect()
        session.apply({"echoes", "items"}, echoes=1, items=2)
        cave = session._cave
        hook_state = self.hook_state()
        session.apply({"echoes", "items"}, echoes=99999999, items=99)
        self.assertEqual(session._cave, cave)
        self.assertEqual(self.hook_state(), hook_state)
        echoes_code = self.api.read(self.api.pid, cave + 2 * trainer.SLOT_SIZE, 5)
        items_code = self.api.read(self.api.pid, cave + 3 * trainer.SLOT_SIZE + 2, 4)
        self.assertEqual(struct.unpack_from("<I", echoes_code, 1)[0], 99999999)
        self.assertEqual(struct.unpack("<I", items_code)[0], 99)
        self.assertTrue(all(protection == trainer.PAGE_EXECUTE_READ
                            for protection in self.api.protections.values()))
        session.close()

    def test_invalid_feature_and_values_do_not_touch_process(self):
        session = self.connect()
        for selected, echoes, items in (({"unknown"}, 1, 1), ({"health"}, -1, 1),
                                         ({"echoes"}, 100000000, 1), ({"items"}, 1, 0),
                                         ({"items"}, 1, 100), ({"health"}, True, 1),
                                         ({"items"}, 1, 1.2), ({"items"}, 1, True),
                                         (None, 1, 1)):
            with self.subTest(selected=selected, echoes=echoes, items=items):
                with self.assertRaises(trainer.TrainerError):
                    session.apply(selected, echoes=echoes, items=items)
        self.assertEqual(self.api.writes, 0)
        self.assertEqual(len(self.api.allocations), 1)
        session.close()

    def test_partial_first_application_rolls_back_every_hook(self):
        session = self.connect()
        self.api.fail_write, self.api.partial_failure = 3, True
        with self.assertRaises(trainer.TrainerError):
            session.apply(trainer.FEATURES)
        self.assert_all_original()
        self.assertEqual(session.enabled, set())
        self.assertEqual(self.api.freed, 1)
        self.assertFalse(self.api.paused)
        self.assertEqual(self.api.suspends, self.api.resumes)
        self.api.fail_write = None
        session.apply({"health"})
        session.close()

    def test_failed_value_change_preserves_previous_effects_and_cave(self):
        session = self.connect()
        session.apply(trainer.FEATURES, echoes=7, items=8)
        old_hooks, cave, code = self.hook_state(), session._cave, session._cave_code
        self.api.fail_write = self.api.writes + 1
        self.api.partial_failure = True
        with self.assertRaises(trainer.TrainerError):
            session.apply({"health", "items"}, echoes=9, items=10)
        self.assertEqual(self.hook_state(), old_hooks)
        self.assertEqual(self.api.read(self.api.pid, cave, len(code)), code)
        self.assertEqual(session.enabled, trainer.FEATURES)
        self.assertFalse(self.api.paused)
        self.api.fail_write = None
        session.close()

    def test_failed_disable_preserves_enabled_state(self):
        session = self.connect()
        session.apply({"health", "stamina"})
        old_hooks = self.hook_state()
        self.api.fail_write = self.api.writes + 2
        self.api.partial_failure = True
        with self.assertRaises(trainer.TrainerError):
            session.apply(set())
        self.assertEqual(self.hook_state(), old_hooks)
        self.assertEqual(session.enabled, {"health", "stamina"})
        self.assertFalse(self.api.paused)
        self.api.fail_write = None
        session.close()

    def test_third_party_hook_is_never_overwritten(self):
        session = self.connect()
        session.apply({"health"})
        address = self.api.base + trainer.HOOKS["health"].offset
        self.api.seed(address, b"\x90" * 6)
        writes = self.api.writes
        with self.assertRaises(trainer.TrainerError):
            session.close()
        self.assertEqual(self.api.writes, writes)
        self.assertEqual(self.api.read(self.api.pid, address, 6), b"\x90" * 6)
        self.assertEqual(self.api.freed, 0)
        self.assertFalse(self.api.paused)
        self.api.alive = False
        session.close()

    def test_third_party_cave_is_never_overwritten(self):
        session = self.connect()
        session.apply({"health"})
        self.api.seed(session._cave, b"\x90")
        writes = self.api.writes
        with self.assertRaises(trainer.TrainerError):
            session.apply({"items"})
        self.assertEqual(self.api.writes, writes)
        self.assertFalse(self.api.paused)
        self.api.alive = False
        session.close()

    def test_threads_in_hook_retry_before_mutation(self):
        session = self.connect()
        self.api.rips = [self.api.base + trainer.HOOKS["health"].offset]
        with patch.object(trainer.time, "sleep"):
            session.apply({"health"})
        self.assertEqual(self.api.suspends, 2)
        self.assertEqual(self.api.resumes, 2)
        self.assertFalse(self.api.paused)
        session.close()

    def test_threads_in_cave_block_removal_and_memory_free(self):
        session = self.connect()
        session.apply({"health"})
        self.api.rips = [session._cave + 1] * 12
        writes = self.api.writes
        with patch.object(trainer.time, "sleep"):
            with self.assertRaises(trainer.TrainerError):
                session.close()
        self.assertEqual(self.api.writes, writes)
        self.assertEqual(self.api.freed, 0)
        self.assertEqual(session.enabled, {"health"})
        self.assertFalse(self.api.paused)
        self.assertEqual(self.api.suspends, self.api.resumes)
        session.close()

    def test_exited_process_closes_without_memory_operations(self):
        session = self.connect()
        session.apply({"health"})
        writes = self.api.writes
        self.api.alive = False
        self.assertFalse(session.is_alive())
        with self.assertRaises(trainer.TrainerError):
            session.apply({"items"})
        session.close()
        session.close()
        self.assertEqual(self.api.writes, writes)
        self.assertEqual(self.api.freed, 0)
        self.assertFalse(self.api.owned)
        self.assertEqual(self.api.closed, 1)

    def test_noop_detects_ownership_loss(self):
        session = self.connect()
        session.apply({"health"})
        address = self.api.base + trainer.HOOKS["items"].offset
        self.api.seed(address, b"\x90" * 7)
        with self.assertRaises(trainer.TrainerError):
            session.apply({"health"})
        self.api.alive = False
        session.close()

    def test_rel32_bounds_are_checked_in_both_directions(self):
        source = 0x123400000000
        for distance in (-(1 << 31), (1 << 31) - 1, -1, 0, 123):
            data = trainer._rel32_jump(source, source + 5 + distance, 6)
            self.assertEqual(struct.unpack_from("<i", data, 1)[0], distance)
            self.assertEqual(data[-1], 0x90)
        for distance in (-(1 << 31) - 1, 1 << 31):
            with self.assertRaises(trainer.TrainerError):
                trainer._rel32_jump(source, source + 5 + distance)

    def test_stamina_preserves_rcx_and_pointer_until_final_original_load(self):
        code = trainer._trampoline("stamina", 0x1000, 0x2000, 0, 20)
        # Push RCX; load ECX from max; store to current; pop RCX; reproduce
        # original EAX load only after the last RAX-relative access.
        self.assertEqual(code[:-5], bytes.fromhex("518B8838010000898834010000598B8034010000"))
        self.assertEqual(code[-5], 0xE9)

    def test_failed_rollback_keeps_cave_and_rejects_further_writes(self):
        session = self.connect()
        original_write = self.api.write
        def failed_write(handle, address, data):
            if self.api.writes >= 2:
                raise trainer.TrainerError("persistent write failure")
            original_write(handle, address, data)
        self.api.write = failed_write
        with self.assertRaises(trainer.TrainerError):
            session.apply(trainer.FEATURES)
        self.assertTrue(session._faulted)
        self.assertEqual(self.api.freed, 0)
        self.assertFalse(self.api.paused)
        with self.assertRaises(trainer.TrainerError):
            session.apply({"health"})
        with self.assertRaises(trainer.TrainerError):
            session.close()
        self.api.alive = False
        session.close()


if __name__ == "__main__":
    unittest.main()
