# Windows port modifications by yaonikaixin999999, 2026-10-05.
# SPDX-License-Identifier: GPL-2.0-or-later
"""Validate game gating and the 60 FPS/4K preparation path without game data."""
from paths import ROOT
import importlib.util
import json
import os
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

spec=importlib.util.spec_from_file_location('windows_launcher',ROOT/'run_windows.py')
launcher=importlib.util.module_from_spec(spec)
spec.loader.exec_module(launcher)

def sfo_fixture(title='CUSA03173',version='01.09'):
    keys=bytearray(); values=bytearray(); entries=bytearray()
    for key,value in (('TITLE_ID',title),('APP_VER',version)):
        encoded=value.encode()+b'\0'
        entries+=struct.pack('<HHIII',len(keys),0x204,len(encoded),len(encoded),len(values))
        keys+=key.encode()+b'\0'; values+=encoded
    key_start=20+len(entries)
    return struct.pack('<4sIIII',b'\0PSF',0x101,key_start,key_start+len(keys),2)+entries+keys+values

def game_fixture(root,title='CUSA03173',version='01.09'):
    for name in ('eboot.bin','sce_module/libc.prx','sce_module/libSceFios2.prx'):
        file=root/name; file.parent.mkdir(parents=True,exist_ok=True); file.write_bytes(b'fixture')
    (root/'sce_sys').mkdir(exist_ok=True)
    (root/'sce_sys/param.sfo').write_bytes(sfo_fixture(title,version))
    (root/'dvdroot_ps4').mkdir(exist_ok=True)
    return root

def language_fixture(game,folder):
    directory=game/'dvdroot_ps4/msg'/folder
    directory.mkdir(parents=True,exist_ok=True)
    for name in ('menu.msgbnd.dcx','item.msgbnd.dcx'):
        (directory/name).write_bytes(b'fixture')

class WindowsLauncherTests(unittest.TestCase):
    def test_parent_resolution_overrides_do_not_desynchronize_patches_and_renderer(self):
        inherited={'BB_UPSCALER':'off','BB_UPSCALE_PRESET':'4','BB_RENDER_RES':'640x360',
                   'BB_OUTPUT_RES':'1280x720','BB_FPS':'90','BB_VBLANK_HZ':'90',
                   'BB_LANGUAGE':'1','BB_FULLSCREEN':'0','BB_PAD_LAYOUT':'xbox','BB_PRESENT_MODE':'Mailbox',
                   'BB_FPS_LIMIT':'25','BB_JITTER':'0','BB_REACTIVE':'1','BB_OBJECT_MOTION':'0',
                   'BB_FSR_SHARPNESS':'2','BB_CONFIG':'old-config.ini'}
        with patch.dict(os.environ,inherited):
            environment=launcher.runtime_environment()
        for key in inherited:
            self.assertNotIn(key,environment)

    def test_game_version_and_data_are_required_before_preparation(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            with self.assertRaisesRegex(ValueError,'不完整'): launcher.validate_game(root)
            game_fixture(root,title='CUSA00207')
            with self.assertRaisesRegex(ValueError,'CUSA03173'): launcher.validate_game(root)
            game_fixture(root,version='01.00')
            with self.assertRaisesRegex(ValueError,'1.09'): launcher.validate_game(root)
            game_fixture(root)
            self.assertEqual(launcher.validate_game(root),root.resolve())
            game_fixture(root,title='CUSA03023')
            self.assertEqual(launcher.validate_game(root),root.resolve())

    def test_upscaled_and_native_4k_are_distinct(self):
        with tempfile.TemporaryDirectory() as tmp:
            config=Path(tmp)/'bbport.ini'
            config.write_text('debug_camera=1\n',encoding='utf-8')
            for profile,preset in (('4k',2),('4k-quality',1),('4k-native',0)):
                settings=launcher.write_profile(config,profile)
                self.assertEqual(settings['output_res'],'3840x2160')
                self.assertEqual(settings['preset'],str(preset))
                self.assertEqual(settings['debug_camera'],'1')
                self.assertEqual(settings['live_resolution'],'0')
            self.assertEqual(launcher.scaled_sizes(settings),((3840,2160),(3840,2160)))

    def test_preparation_targets_windows_and_60_fps(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); game=game_fixture(root/'game')
            with patch.object(launcher,'ROOT',root),patch.object(launcher,'execute') as execute:
                launcher.launch(game,'4k',prepare_only=True)
                commands=[call.args[0] for call in execute.call_args_list]
            self.assertEqual(len(commands),5)
            for index in (1,2):
                self.assertEqual(commands[index][-2:],['--target','windows'])
            patch_command=commands[-1]
            self.assertEqual(patch_command[patch_command.index('--fps')+1],'60')
            self.assertEqual(patch_command[patch_command.index('--output-res')+1],'3840x2160')
            self.assertEqual(patch_command[patch_command.index('--render-res')+1],'2258x1270')
            self.assertEqual(execute.call_args.args[1]['BB_PRESENT_MODE'],'Immediate')
            self.assertFalse((root/'out/windows-data/launch.json').read_text().find('false')<0)

    def test_auto_language_prefers_available_chinese_and_can_be_overridden(self):
        with tempfile.TemporaryDirectory() as tmp:
            game=game_fixture(Path(tmp)/'game')
            self.assertEqual(launcher.resolve_language(game),('en',1))
            language_fixture(game,'zhotw')
            self.assertEqual(launcher.resolve_language(game),('zh-tw',10))
            language_fixture(game,'zhocn')
            self.assertEqual(launcher.resolve_language(game),('zh-cn',11))
            self.assertEqual(launcher.resolve_language(game,'zh-tw'),('zh-tw',10))
            self.assertEqual(launcher.resolve_language(game,'en'),('en',1))
            (game/'dvdroot_ps4/msg/zhocn/item.msgbnd.dcx').write_bytes(b'')
            self.assertEqual(launcher.resolve_language(game),('zh-tw',10))
            with self.assertRaisesRegex(ValueError,'zhocn'):
                launcher.resolve_language(game,'zh-cn')

    def test_preparation_persists_language_fullscreen_and_renderer_environment(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); game=game_fixture(root/'game')
            language_fixture(game,'zhocn')
            with patch.object(launcher,'ROOT',root),patch.object(launcher,'execute') as execute, \
                 patch.dict(os.environ,{'BB_LANGUAGE':'1','BB_FULLSCREEN':'1'}):
                launcher.launch(game,'4k',prepare_only=True,fullscreen=False,vsync=True)
                preferences=launcher.read_preferences()
                self.assertEqual(preferences['language'],'auto')
                self.assertEqual(preferences['resolution'],'4k')
                self.assertFalse(preferences['fullscreen'])
                self.assertTrue(preferences['vsync'])
                self.assertEqual(preferences['game_dir'],str(game.resolve()))
                for call in execute.call_args_list:
                    self.assertEqual(call.args[1]['BB_LANGUAGE'],'11')
                    self.assertEqual(call.args[1]['BB_FULLSCREEN'],'0')
                    self.assertEqual(call.args[1]['BB_PRESENT_MODE'],'Fifo')
                state=json.loads((root/'out/windows-data/launch.json').read_text(encoding='utf-8'))
                self.assertEqual(state['resolved_language'],'zh-cn')
                self.assertEqual(state['language_id'],11)
                self.assertFalse(state['fullscreen'])
                self.assertTrue(state['vsync'])
                launcher.launch(game,prepare_only=True)
                self.assertEqual(launcher.read_preferences(),preferences)
                self.assertEqual(execute.call_args_list[-1].args[1]['BB_FULLSCREEN'],'0')
                self.assertEqual(execute.call_args_list[-1].args[1]['BB_PRESENT_MODE'],'Fifo')
                launcher.launch(game,'1440p',prepare_only=True)
                self.assertEqual(launcher.read_preferences()['resolution'],'1440p')
                self.assertEqual(execute.call_args_list[-1].args[1]['BB_LANGUAGE'],'11')
                self.assertEqual(execute.call_args_list[-1].args[1]['BB_FULLSCREEN'],'0')
                launcher.launch(game,prepare_only=True,vsync=False)
                self.assertFalse(launcher.read_preferences()['vsync'])
                self.assertEqual(execute.call_args_list[-1].args[1]['BB_PRESENT_MODE'],'Immediate')
                self.assertFalse(json.loads((root/'out/windows-data/launch.json').read_text(encoding='utf-8'))['vsync'])

    def test_controller_layout_defaults_persists_and_overrides_inherited_environment(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); game=game_fixture(root/'game')
            with patch.object(launcher,'ROOT',root),patch.object(launcher,'execute') as execute, \
                 patch.dict(os.environ,{'BB_PAD_LAYOUT':'xbox'}):
                launcher.launch(game,'1080p',prepare_only=True)
                self.assertEqual(launcher.read_preferences()['controller_layout'],'ps4')
                for call in execute.call_args_list:
                    self.assertEqual(call.args[1]['BB_PAD_LAYOUT'],'ps4')
                execute.reset_mock()
                for layout in ('swap-ab','swap-xy','xbox'):
                    with self.subTest(layout=layout):
                        execute.reset_mock()
                        launcher.launch(game,prepare_only=True,controller_layout=layout)
                        self.assertEqual(launcher.read_preferences()['controller_layout'],layout)
                        state=json.loads((root/'out/windows-data/launch.json').read_text(encoding='utf-8'))
                        self.assertEqual(state['controller_layout'],layout)
                        launcher.launch(game,prepare_only=True)
                        for call in execute.call_args_list:
                            self.assertEqual(call.args[1]['BB_PAD_LAYOUT'],layout)
                execute.reset_mock()
                with self.assertRaisesRegex(ValueError,'手柄按键'):
                    launcher.launch(game,prepare_only=True,controller_layout='invalid')
                execute.assert_not_called()
                self.assertEqual(launcher.read_preferences()['controller_layout'],'xbox')

    def test_renderer_restart_preserves_explicit_language_and_fullscreen(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); game=game_fixture(root/'game')
            language_fixture(game,'zhocn'); language_fixture(game,'zhotw')
            executable=root/'dist/windows/bb-probe.exe'
            executable.parent.mkdir(parents=True); executable.write_bytes(b'fixture')
            with patch.object(launcher,'ROOT',root),patch.object(launcher,'execute'), \
                 patch.object(launcher.subprocess,'call',side_effect=[75,0]) as renderer:
                launcher.launch(game,'1080p',language='zh-tw',fullscreen=False,controller_layout='xbox',vsync=True)
                self.assertEqual(renderer.call_count,2)
                for call in renderer.call_args_list:
                    self.assertEqual(call.kwargs['env']['BB_LANGUAGE'],'10')
                    self.assertEqual(call.kwargs['env']['BB_FULLSCREEN'],'0')
                    self.assertEqual(call.kwargs['env']['BB_FPS'],'60')
                    self.assertEqual(call.kwargs['env']['BB_PAD_LAYOUT'],'xbox')
                    self.assertEqual(call.kwargs['env']['BB_PRESENT_MODE'],'Fifo')
                self.assertEqual(launcher.read_preferences()['language'],'zh-tw')
                self.assertFalse(launcher.read_preferences()['fullscreen'])

    def test_independent_swap_choices_reach_renderer_and_survive_restart(self):
        for layout in ('swap-ab','swap-xy'):
            with self.subTest(layout=layout),tempfile.TemporaryDirectory() as tmp:
                root=Path(tmp); game=game_fixture(root/'game')
                executable=root/'dist/windows/bb-probe.exe'
                executable.parent.mkdir(parents=True); executable.write_bytes(b'fixture')
                with patch.object(launcher,'ROOT',root),patch.object(launcher,'execute'), \
                     patch.object(launcher.subprocess,'call',side_effect=[75,0]) as renderer:
                    launcher.launch(game,'1080p',controller_layout=layout)
                    self.assertEqual(renderer.call_count,2)
                    for call in renderer.call_args_list:
                        self.assertEqual(call.kwargs['env']['BB_PAD_LAYOUT'],layout)

    def test_invalid_preferences_fall_back_and_cli_forwards_choices(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); (root/'user').mkdir()
            preferences=root/'user/launcher.json'
            preferences.write_text(json.dumps({'resolution':[],'language':{},'fullscreen':'false',
                                               'controller_layout':[],'vsync':'false'}))
            with patch.object(launcher,'ROOT',root):
                self.assertEqual(launcher.read_preferences()['language'],'auto')
                self.assertFalse(launcher.read_preferences()['fullscreen'])
                self.assertEqual(launcher.read_preferences()['controller_layout'],'ps4')
                self.assertFalse(launcher.read_preferences()['vsync'])
                preferences.write_text('{invalid')
                self.assertEqual(launcher.read_preferences()['resolution'],'1080p')
            for flag in ('--windowed','--no-fullscreen'):
                with self.subTest(flag=flag), \
                     patch.object(launcher.sys,'argv',['run_windows.py','--game',str(root),
                                  '--language','zh-cn','--controller-layout','xbox',flag]), \
                     patch.object(launcher,'launch') as launch:
                    self.assertEqual(launcher.main(),0)
                    launch.assert_called_once_with(root,None,False,language='zh-cn',fullscreen=False,
                                                   controller_layout='xbox',fps=None,vsync=None)
            with patch.object(launcher.sys,'argv',['run_windows.py','--gui','--language','en','--fullscreen',
                                                 '--controller-layout','xbox']), \
                 patch.object(launcher,'gui') as gui:
                self.assertEqual(launcher.main(),0)
                gui.assert_called_once_with(None,None,'en',True,'xbox',None,None)

    def test_legacy_launch_profile_is_restored_and_saved_choices_take_precedence(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            state=root/'out/windows-data/launch.json'
            state.parent.mkdir(parents=True)
            state.write_text(json.dumps({'game_dir':'previous game','resolution':'4k-native',
                                         'language':'zh-tw','fullscreen':True}))
            with patch.object(launcher,'ROOT',root):
                self.assertEqual(launcher.read_preferences(),{
                    'game_dir':str((root/'previous game').resolve()),'resolution':'4k-native',
                    'language':'zh-tw','fullscreen':True,'controller_layout':'ps4','fps':'60','vsync':False})
                launcher.save_preferences({'resolution':'1440p','language':'auto','fullscreen':False})
                self.assertEqual(launcher.read_preferences(),{
                    'game_dir':str((root/'previous game').resolve()),'resolution':'1440p',
                    'language':'auto','fullscreen':False,'controller_layout':'ps4','fps':'60','vsync':False})

    def test_cli_display_flags_are_optional_and_mutually_exclusive(self):
        with patch.object(launcher.sys,'argv',['run_windows.py','--game','unused']), \
             patch.object(launcher,'launch') as launch:
            self.assertEqual(launcher.main(),0)
            launch.assert_called_once_with(Path('unused'),None,False,language=None,fullscreen=None,
                                           controller_layout=None,fps=None,vsync=None)
        with patch.object(launcher.sys,'argv',['run_windows.py','--gui','--fullscreen','--windowed']), \
             patch.object(launcher.sys,'stderr'),self.assertRaises(SystemExit) as error:
            launcher.main()
        self.assertEqual(error.exception.code,2)

    def test_cli_vsync_flags_forward_and_are_mutually_exclusive(self):
        for flag,enabled in (('--vsync',True),('--no-vsync',False)):
            with self.subTest(flag=flag), \
                 patch.object(launcher.sys,'argv',['run_windows.py','--game','unused',flag]), \
                 patch.object(launcher,'launch') as launch:
                self.assertEqual(launcher.main(),0)
                self.assertIs(launch.call_args.kwargs['vsync'],enabled)
            with self.subTest(gui=flag), \
                 patch.object(launcher.sys,'argv',['run_windows.py','--gui',flag]), \
                 patch.object(launcher,'gui') as gui:
                self.assertEqual(launcher.main(),0)
                self.assertIs(gui.call_args.args[-1],enabled)
        with patch.object(launcher.sys,'argv',['run_windows.py','--gui','--vsync','--no-vsync']), \
             patch.object(launcher.sys,'stderr'),self.assertRaises(SystemExit) as error:
            launcher.main()
        self.assertEqual(error.exception.code,2)

    def test_bundled_game_preferences_survive_relocation(self):
        with tempfile.TemporaryDirectory() as tmp:
            base=Path(tmp); original=base/'original'; moved=base/'moved folder'
            game=game_fixture(original/'game/CUSA03023',title='CUSA03023')
            with patch.object(launcher,'ROOT',original):
                self.assertEqual(launcher.read_preferences()['game_dir'],str(game.resolve()))
                preferences={'game_dir':str(game.resolve()),'language':'zh-cn','fps':'30'}
                launcher.save_preferences(preferences)
                self.assertEqual(preferences['game_dir'],str(game.resolve()))
                saved=json.loads((original/'user/launcher.json').read_text(encoding='utf-8'))
                self.assertEqual(saved['game_dir'],'game/CUSA03023')
            original.rename(moved)
            with patch.object(launcher,'ROOT',moved), \
                 patch.object(launcher.sys,'argv',['run_windows.py','--prepare-only']), \
                 patch.object(launcher,'launch') as launch:
                self.assertEqual(launcher.read_preferences()['game_dir'],str((moved/'game/CUSA03023').resolve()))
                self.assertEqual(launcher.main(),0)
                self.assertEqual(launch.call_args.args[0],(moved/'game/CUSA03023').resolve())

    def test_bundled_game_fallback_keeps_valid_external_selection(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)/'package'; external=game_fixture(Path(tmp)/'external')
            bundled=game_fixture(root/'game/CUSA03023',title='CUSA03023')
            with patch.object(launcher,'ROOT',root):
                launcher.save_preferences({'game_dir':str(root/'missing')})
                self.assertEqual(launcher.read_preferences()['game_dir'],str(bundled.resolve()))
                launcher.save_preferences({'game_dir':str(external.resolve())})
                self.assertEqual(launcher.read_preferences()['game_dir'],str(external.resolve()))

    def test_cli_rejects_unknown_controller_layout(self):
        with patch.object(launcher.sys,'argv',['run_windows.py','--game','unused','--controller-layout','invalid']), \
             patch.object(launcher.sys,'stderr'),patch.object(launcher,'launch') as launch, \
             self.assertRaises(SystemExit) as error:
            launcher.main()
        self.assertEqual(error.exception.code,2)
        launch.assert_not_called()

    def test_cli_independent_swap_layouts_are_forwarded(self):
        for layout in ('swap-ab','swap-xy'):
            with self.subTest(layout=layout), \
                 patch.object(launcher.sys,'argv',['run_windows.py','--game','unused','--controller-layout',layout]), \
                 patch.object(launcher,'launch') as launch:
                self.assertEqual(launcher.main(),0)
                self.assertEqual(launch.call_args.kwargs['controller_layout'],layout)

    def test_fps_choice_matches_game_patch_and_vblank_and_survives_restart(self):
        for fps,hz in (('30','60'),('60','60'),('90','90'),('uncap','0')):
            with self.subTest(fps=fps),tempfile.TemporaryDirectory() as tmp:
                root=Path(tmp); game=game_fixture(root/'game')
                executable=root/'dist/windows/bb-probe.exe'
                executable.parent.mkdir(parents=True); executable.write_bytes(b'fixture')
                with patch.object(launcher,'ROOT',root),patch.object(launcher,'execute') as execute, \
                     patch.object(launcher.subprocess,'call',side_effect=[75,0]) as renderer:
                    launcher.launch(game,'4k',fps=fps)
                    self.assertEqual(launcher.read_preferences()['fps'],fps)
                    self.assertEqual(renderer.call_count,2)
                    for call in renderer.call_args_list:
                        self.assertEqual(call.kwargs['env']['BB_FPS'],fps)
                        self.assertEqual(call.kwargs['env']['BB_VBLANK_HZ'],hz)
                    patches=[call.args[0] for call in execute.call_args_list
                             if str(call.args[0][1]).endswith('patches.py')]
                    self.assertEqual(len(patches),2)
                    for command in patches:
                        self.assertEqual(command[command.index('--fps')+1],fps)
                    state=json.loads((root/'out/windows-data/launch.json').read_text(encoding='utf-8'))
                    self.assertEqual(state['fps'],fps)
                    self.assertEqual(state['target_fps'],int(fps) if fps!='uncap' else 'display')

    def test_bad_fps_does_not_write_preferences_or_prepare(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); game=game_fixture(root/'game')
            with patch.object(launcher,'ROOT',root),patch.object(launcher,'execute') as execute:
                with self.assertRaisesRegex(ValueError,'帧率'):
                    launcher.launch(game,'1080p',prepare_only=True,fps='45')
                self.assertFalse((root/'user/launcher.json').exists())
                execute.assert_not_called()

    def test_cli_fps_30_and_saved_default_are_forwarded(self):
        with patch.object(launcher.sys,'argv',['run_windows.py','--game','unused','--fps','30']), \
             patch.object(launcher,'launch') as launch:
            self.assertEqual(launcher.main(),0)
            self.assertEqual(launch.call_args.kwargs['fps'],'30')
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); game=game_fixture(root/'game')
            with patch.object(launcher,'ROOT',root),patch.object(launcher,'execute') as execute:
                launcher.save_preferences({'fps':'30'})
                launcher.launch(game,'1080p',prepare_only=True)
                command=execute.call_args.args[0]
                self.assertEqual(command[command.index('--fps')+1],'30')

if __name__=='__main__': unittest.main()
