# Windows port modifications by yaonikaixin999999, 2026-10-05.
# SPDX-License-Identifier: GPL-2.0-or-later
"""Chinese launcher settings backed by the renderer's existing bbport.ini keys."""
from pathlib import Path
import math
import os
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent / 'scripts'))
from patches import read_settings, scaled_sizes, PRESET_SCALES

UPSCALER_LABELS = {'FSR 3.1': 'fsr3', 'FSR 4（INT8）': 'fsr4',
                   'FSR 4.1.1（Windows 适配待完成）': 'fsr411',
                   'TAA · 原生抗锯齿': 'taa', '关闭超分辨率': 'off'}
PRESET_LABELS = {'原生画质 / Native AA': '0', '画质优先 / Quality': '1',
                 '均衡 / Balanced': '2', '性能优先 / Performance': '3',
                 '极致性能 / Ultra Performance': '4'}
LOD_LABELS = {'最高细节': '-2', '游戏原版': '0', '较低细节': '1', '最低细节': '2'}
BOOLEAN_LABELS = {
    'sharpen': '画面锐化', 'jitter': '时序采样', 'object_motion': '角色运动向量',
    'reactive': '透明物体响应遮罩', 'fsr4_auto_exposure': 'FSR 4 自动曝光',
    'effect_chromatic_aberration': '色差效果', 'effect_dof': '景深（DoF）',
    'effect_motion_blur': '运动模糊', 'effect_ssao': '环境遮蔽（SSAO）',
    'effect_game_aa': '游戏自带抗锯齿', 'effect_dynamic_shadows': '动态光源阴影',
    'effect_ssr': '屏幕空间反射（SSR）', 'show_fps': '显示帧率', 'skip_intro': '跳过开场动画',
}
DEFAULTS = {
    'upscaler': 'fsr3', 'preset': '0', 'output_res': '1920x1080',
    'sharpen': '1', 'sharpness': '0.3', 'jitter': '1', 'object_motion': '1',
    'reactive': '0', 'fsr4_auto_exposure': '1',
    'reactive_scale': '1.00', 'reactive_threshold': '0.20', 'reactive_max': '0.90',
    'effect_chromatic_aberration': '0', 'effect_dof': '1', 'effect_motion_blur': '0',
    'effect_ssao': '1', 'effect_game_aa': '1', 'effect_dynamic_shadows': '1',
    'effect_ssr': '0', 'show_fps': '1', 'skip_intro': '0', 'model_lod': '0',
    'live_resolution': '0',
}
ENUMS = {'upscaler': set(UPSCALER_LABELS.values()), 'preset': set(PRESET_LABELS.values()),
         'model_lod': set(LOD_LABELS.values()),
         'output_res': {'1280x720', '1920x1080', '2560x1440', '3840x2160'},
         'live_resolution': {'-1', 'auto', '0', '1'}}


def validate_settings(updates):
    values = {}
    for key, value in updates.items():
        text = str(int(value)) if isinstance(value, bool) else str(value)
        if '\n' in text or '\r' in text or '=' in str(key) or '\n' in str(key):
            raise ValueError('画面设置格式不正确。')
        if key in ENUMS and text not in ENUMS[key]:
            raise ValueError(f'不支持的画面设置：{key}={text}')
        if key in BOOLEAN_LABELS and text not in ('0', '1'):
            raise ValueError(f'选项必须为开或关：{BOOLEAN_LABELS[key]}')
        ranges = {'sharpness': (0, 2), 'reactive_scale': (0, 16),
                  'reactive_threshold': (0, 1), 'reactive_max': (0, 1)}
        if key in ranges:
            lo, hi = ranges[key]
            try: amount = float(text)
            except ValueError: raise ValueError(f'{key} 必须是 {lo}～{hi} 的数字。') from None
            if not math.isfinite(amount) or not lo <= amount <= hi:
                raise ValueError(f'{key} 必须在 {lo}～{hi} 之间。')
            text = f'{amount:.2f}'
        values[key] = text
    return values


def load_settings(path):
    values = DEFAULTS.copy()
    for key, value in read_settings(Path(path)).items():
        try: values.update(validate_settings({key: value}))
        except ValueError: pass
    return values


def save_settings(path, updates):
    # Read at save time: an in-game edit made after the launcher opened survives
    # unless the same option was explicitly changed in this panel.
    changes = validate_settings(updates)
    path = Path(path)
    values = DEFAULTS | read_settings(path) | changes
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + '.launcher.tmp')
    temporary.write_text('# bbport Windows settings\n' +
                         ''.join(f'{key}={value}\n' for key, value in values.items()), encoding='utf-8')
    os.replace(temporary, path)
    return values


def render_description(settings):
    values = DEFAULTS | settings
    width, height = (int(v) for v in values['output_res'].split('x'))
    method = values['upscaler']
    if method == 'taa':
        return f'输出 {width}×{height}；TAA 使用原生场景抗锯齿。'
    if method == 'off':
        return f'输出 {width}×{height}；场景以原生分辨率渲染。'
    sizes = scaled_sizes(values)
    if sizes:
        render, _ = sizes
    else:
        scale = PRESET_SCALES[int(values['preset'])]
        render = tuple(max(2, round(v / scale / 2) * 2) for v in (width, height))
    return f'输出 {width}×{height}；场景渲染 {render[0]}×{render[1]}。'
