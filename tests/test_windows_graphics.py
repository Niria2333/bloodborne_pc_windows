# Windows port modifications by yaonikaixin999999, 2026-10-05.
# SPDX-License-Identifier: GPL-2.0-or-later
"""Verify launcher graphics edits against persisted settings and patch preparation."""
from paths import ROOT
import importlib.util
import json
import math
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))
import windows_graphics as graphics
from patches import effect_patches, read_settings, scaled_sizes

spec = importlib.util.spec_from_file_location('graphics_launcher', ROOT / 'run_windows.py')
launcher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(launcher)


def game_fixture(root):
    for name in ('eboot.bin', 'sce_module/libc.prx', 'sce_module/libSceFios2.prx'):
        file = root / name
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_bytes(b'fixture')
    keys, values, entries = bytearray(), bytearray(), bytearray()
    for key, value in (('TITLE_ID', 'CUSA03023'), ('APP_VER', '01.09')):
        encoded = value.encode() + b'\0'
        entries += struct.pack('<HHIII', len(keys), 0x204, len(encoded), len(encoded), len(values))
        keys += key.encode() + b'\0'
        values += encoded
    key_start = 20 + len(entries)
    (root / 'sce_sys').mkdir()
    (root / 'sce_sys/param.sfo').write_bytes(
        struct.pack('<4sIIII', b'\0PSF', 0x101, key_start, key_start + len(keys), 2)
        + entries + keys + values)
    (root / 'dvdroot_ps4').mkdir()
    return root


CUSTOM_SETTINGS = {
    'upscaler': 'fsr4', 'preset': '2', 'output_res': '3840x2160',
    'sharpness': '0.74', 'sharpen': '0', 'effect_chromatic_aberration': '1',
    'effect_motion_blur': '1', 'effect_dof': '0', 'effect_ssao': '0',
    'effect_ssr': '1', 'model_lod': '-2', 'show_fps': '0',
    'debug_camera': '1', 'future_renderer_option': 'keep-this',
}


class GraphicsSettingsTests(unittest.TestCase):
    def test_missing_config_returns_independent_defaults(self):
        with tempfile.TemporaryDirectory() as tmp:
            config = Path(tmp) / 'bbport.ini'
            first = graphics.load_settings(config)
            self.assertEqual(first, graphics.DEFAULTS)
            self.assertFalse(config.exists())
            first['upscaler'] = 'off'
            self.assertEqual(graphics.load_settings(config)['upscaler'], 'fsr3')

    def test_existing_config_loads_values_and_normalizes_corrupt_known_options(self):
        with tempfile.TemporaryDirectory() as tmp:
            config = Path(tmp) / 'bbport.ini'
            config.write_text(
                '# Existing in-game settings\nupscaler=fsr4\npreset=2\nsharpness=0.7\n'
                'effect_dof=0\nreactive_threshold=nan\neffect_ssao=invalid\n'
                'model_lod=100\noutput_res=bogus\nfuture_renderer_option=keep-this\n'
                'malformed line\n', encoding='utf-8')
            settings = graphics.load_settings(config)
            self.assertEqual(settings['upscaler'], 'fsr4')
            self.assertEqual(settings['preset'], '2')
            self.assertEqual(settings['sharpness'], '0.70')
            self.assertEqual(settings['effect_dof'], '0')
            self.assertEqual(settings['future_renderer_option'], 'keep-this')
            for key in ('reactive_threshold', 'effect_ssao', 'model_lod', 'output_res'):
                self.assertEqual(settings[key], graphics.DEFAULTS[key], key)

    def test_validation_only_returns_the_changed_values(self):
        self.assertEqual(graphics.validate_settings({
            'sharpen': False, 'effect_ssr': True, 'sharpness': 0.75,
            'reactive_scale': 2, 'future_renderer_option': 'custom'}), {
            'sharpen': '0', 'effect_ssr': '1', 'sharpness': '0.75',
            'reactive_scale': '2.00', 'future_renderer_option': 'custom'})
        self.assertEqual(graphics.validate_settings({}), {})

    def test_supported_enum_values_validate_and_unknown_values_fail(self):
        for key, choices in (
                ('upscaler', graphics.UPSCALER_LABELS.values()),
                ('preset', graphics.PRESET_LABELS.values()),
                ('model_lod', graphics.LOD_LABELS.values()),
                ('output_res', ('1280x720', '1920x1080', '2560x1440', '3840x2160')),
                ('live_resolution', ('-1', 'auto', '0', '1'))):
            for value in choices:
                with self.subTest(key=key, value=value):
                    self.assertEqual(graphics.validate_settings({key: value}), {key: value})
            with self.subTest(key=key, invalid=True), self.assertRaises(ValueError):
                graphics.validate_settings({key: 'invalid'})
        for key in graphics.BOOLEAN_LABELS:
            with self.subTest(key=key), self.assertRaises(ValueError):
                graphics.validate_settings({key: '2'})

    def test_numeric_boundaries_and_nonfinite_values(self):
        for key, low, high in (('sharpness', 0, 2), ('reactive_scale', 0, 16),
                               ('reactive_threshold', 0, 1), ('reactive_max', 0, 1)):
            for value in (low, high):
                with self.subTest(key=key, value=value):
                    self.assertEqual(graphics.validate_settings({key: value})[key], f'{value:.2f}')
            for value in (low - .01, high + .01, 'not-a-number', math.nan, math.inf, -math.inf):
                with self.subTest(key=key, invalid=value), self.assertRaises(ValueError):
                    graphics.validate_settings({key: value})

    def test_invalid_save_keeps_original_file_and_creates_no_temporary_output(self):
        with tempfile.TemporaryDirectory() as tmp:
            config = Path(tmp) / 'bbport.ini'
            config.write_text('upscaler=fsr4\nfuture_renderer_option=keep-this\n', encoding='utf-8')
            original = config.read_bytes()
            for invalid in ({'sharpness': -0.1}, {'preset': '5'}, {'upscaler': 'dlss'},
                            {'effect_ssr': 'yes'}, {'future_renderer_option': 'value\nupscaler=off'},
                            {'bad=key': 'value'}, {'bad\nkey': 'value'}):
                with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                    graphics.save_settings(config, {'show_fps': '1'} | invalid)
                self.assertEqual(config.read_bytes(), original)
                self.assertEqual(list(Path(tmp).iterdir()), [config])

    def test_save_preserves_unedited_custom_settings_and_new_in_game_edits(self):
        with tempfile.TemporaryDirectory() as tmp:
            config = Path(tmp) / 'bbport.ini'
            graphics.save_settings(config, CUSTOM_SETTINGS)
            # Simulate a renderer edit after the launcher's panel loaded.
            graphics.load_settings(config)
            config.write_text(config.read_text(encoding='utf-8')
                              + 'jitter=0\nfuture_option=changed-in-game\n', encoding='utf-8')
            graphics.save_settings(config, {'sharpness': 1.2, 'effect_ssao': True})
            saved = read_settings(config)
            expected = CUSTOM_SETTINGS | {'sharpness': '1.20', 'effect_ssao': '1',
                                          'jitter': '0', 'future_option': 'changed-in-game'}
            for key, value in expected.items():
                self.assertEqual(saved[key], value, key)
            self.assertFalse(config.with_name(config.name + '.launcher.tmp').exists())

    def test_settings_round_trip_utf8_values_in_a_unicode_directory(self):
        with tempfile.TemporaryDirectory() as tmp:
            config = Path(tmp) / '画面设置' / 'bbport.ini'
            graphics.save_settings(config, {'future_renderer_option': '自定义画质'})
            self.assertIn('future_renderer_option=自定义画质',
                          config.read_bytes().decode('utf-8').splitlines())
            self.assertEqual(graphics.load_settings(config)['future_renderer_option'], '自定义画质')

    def test_fsr_methods_use_requested_output_and_preset_sizes(self):
        presets = {'0': (3840, 2160), '1': (2560, 1440), '2': (2258, 1270),
                   '3': (1916, 1078), '4': (1280, 720)}
        for method in ('fsr3', 'fsr4'):
            for preset, render in presets.items():
                with self.subTest(method=method, preset=preset):
                    settings = {'upscaler': method, 'preset': preset, 'output_res': '3840x2160'}
                    self.assertEqual(scaled_sizes(settings), (render, (3840, 2160)))
                    description = graphics.render_description(settings)
                    self.assertIn('3840×2160', description)
                    self.assertIn(f'{render[0]}×{render[1]}', description)

    def test_off_and_taa_explain_native_rendering_regardless_of_quality_preset(self):
        for method in ('off', 'taa'):
            with self.subTest(method=method):
                settings = {'upscaler': method, 'preset': '4', 'output_res': '3840x2160'}
                description = graphics.render_description(settings)
                self.assertIn('3840×2160', description)
                self.assertIn('原生', description)
                self.assertNotIn('1280×720', description)
        self.assertEqual(scaled_sizes({'upscaler': 'off', 'preset': '4', 'output_res': '3840x2160'}),
                         ((3840, 2160), (3840, 2160)))


class GraphicsLauncherIntegrationTests(unittest.TestCase):
    def test_resolution_profile_preserves_chosen_upscaler_and_effects(self):
        with tempfile.TemporaryDirectory() as tmp:
            config = Path(tmp) / 'bbport.ini'
            graphics.save_settings(config, CUSTOM_SETTINGS)
            settings = launcher.write_profile(config, '1440p')
            self.assertEqual(settings['output_res'], '2560x1440')
            self.assertEqual(settings['preset'], '1')
            for key, value in CUSTOM_SETTINGS.items():
                if key not in ('output_res', 'preset'):
                    self.assertEqual(settings[key], value, key)

    def test_preparation_uses_the_saved_custom_effects_and_fsr4_resolution(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            game = game_fixture(root / 'game')
            config = root / 'bbport.ini'
            graphics.save_settings(config, CUSTOM_SETTINGS)
            captured = []

            def execute(command, environment):
                if str(command[1]).endswith('patches.py'):
                    path = Path(command[command.index('--settings') + 1])
                    captured.append((command, read_settings(path), environment.copy()))

            with patch.object(launcher, 'ROOT', root), patch.object(launcher, 'execute', side_effect=execute):
                launcher.launch(game, prepare_only=True)
            self.assertEqual(len(captured), 1)
            command, settings, environment = captured[0]
            self.assertEqual(settings['upscaler'], 'fsr4')
            self.assertEqual(command[command.index('--render-res') + 1], '2258x1270')
            self.assertEqual(command[command.index('--output-res') + 1], '3840x2160')
            self.assertEqual(environment['BB_RENDER_RES'], '2258x1270')
            self.assertEqual(environment['BB_OUTPUT_RES'], '3840x2160')
            names = effect_patches(settings)
            for name in ('Disable DoF', 'Disable SSAO',
                         'Enable Screen Space Reflections (READ NOTE)', 'Model LOD -2 (Highest)'):
                self.assertIn(name, names)
            for name in ('Disable Chromatic Aberration', 'Disable Motion Blur (perf increase)'):
                self.assertNotIn(name, names)
            state = json.loads((root / 'out/windows-data/launch.json').read_text(encoding='utf-8'))
            self.assertEqual(state['upscaler'], 'fsr4')
            self.assertEqual(state['output_res'], '3840x2160')

    def test_renderer_restart_keeps_in_game_graphics_changes(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            game = game_fixture(root / 'game')
            config = root / 'bbport.ini'
            graphics.save_settings(config, CUSTOM_SETTINGS)
            executable = root / 'dist/windows/bb-probe.exe'
            executable.parent.mkdir(parents=True)
            executable.write_bytes(b'fixture')
            starts = []

            def renderer(*args, **kwargs):
                starts.append(read_settings(config))
                if len(starts) == 1:
                    graphics.save_settings(config, {'upscaler': 'off', 'effect_dof': True,
                                                    'sharpness': 1.5})
                    return 75
                return 0

            with patch.object(launcher, 'ROOT', root), patch.object(launcher, 'execute'), \
                    patch.object(launcher.subprocess, 'call', side_effect=renderer):
                launcher.launch(game, '4k')
            self.assertEqual(len(starts), 2)
            self.assertEqual(starts[0]['upscaler'], 'fsr4')
            self.assertEqual(starts[1]['upscaler'], 'off')
            self.assertEqual(starts[1]['effect_dof'], '1')
            self.assertEqual(starts[1]['sharpness'], '1.50')
            self.assertEqual(starts[1]['effect_ssr'], '1')
            self.assertEqual(starts[1]['future_renderer_option'], 'keep-this')


if __name__ == '__main__':
    unittest.main()
