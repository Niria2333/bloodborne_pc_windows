# Windows port modifications by yaonikaixin999999, 2026-10-05.
# SPDX-License-Identifier: GPL-2.0-or-later
"""Chinese graphics controls shared by the Windows launcher's settings page."""

import math
import tkinter as tk
from tkinter import ttk

from windows_graphics import (
    BOOLEAN_LABELS,
    DEFAULTS,
    LOD_LABELS,
    PRESET_LABELS,
    UPSCALER_LABELS,
    render_description,
    validate_settings,
)


class GraphicsPanel(ttk.Frame):
    """Edit a settings snapshot and report only the user's changed fields."""

    def __init__(self, parent, settings):
        super().__init__(parent, padding=(12, 10))
        self._initial = validate_settings({**DEFAULTS, **settings})
        self._vars = {}
        self._labels = {}
        self._scales = {}
        self._syncing_scale = False
        self._forced_dirty = set()
        self.dirty_keys = set()
        self._output_res = self._initial['output_res']
        self._ready = False

        notebook = ttk.Notebook(self)
        notebook.pack(fill='both', expand=True)
        quality = ttk.Frame(notebook, padding=12)
        effects = ttk.Frame(notebook, padding=12)
        advanced = ttk.Frame(notebook, padding=12)
        notebook.add(quality, text='图像质量')
        notebook.add(effects, text='游戏特效')
        notebook.add(advanced, text='细节调整')
        for tab in (quality, effects, advanced):
            tab.columnconfigure(1, weight=1)

        available = {label: value for label, value in UPSCALER_LABELS.items()
                     if value != 'fsr411'}
        self.upscaler_control = self._choice(
            quality, 0, '超分辨率 / 抗锯齿', 'upscaler', UPSCALER_LABELS,
            selectable=available,
        )
        self.preset_control = self._choice(
            quality, 1, '画质档位', 'preset', PRESET_LABELS,
        )
        self.summary = tk.StringVar()
        ttk.Label(quality, textvariable=self.summary, wraplength=590,
                  foreground='#444444').grid(
            row=2, column=0, columnspan=2, sticky='w', pady=(4, 8),
        )
        self.algorithm_hint = tk.StringVar()
        ttk.Label(quality, textvariable=self.algorithm_hint, wraplength=590,
                  foreground='#555555').grid(
            row=3, column=0, columnspan=2, sticky='w', pady=(0, 10),
        )
        self._check(quality, 4, 'sharpen')
        self._number(quality, 5, '锐度（0～2）', 'sharpness', 0.0, 2.0)
        self._check(quality, 6, 'jitter')
        self._check(quality, 7, 'object_motion')
        self._check(quality, 8, 'fsr4_auto_exposure')
        ttk.Label(quality, text='分辨率在「启动」页选择；修改后于下次启动应用。',
                  wraplength=590, foreground='#555555').grid(
            row=9, column=0, columnspan=2, sticky='w', pady=(12, 0),
        )

        self._choice(effects, 0, '模型细节', 'model_lod', LOD_LABELS)
        effect_keys = (
            'effect_chromatic_aberration', 'effect_dof', 'effect_motion_blur',
            'effect_ssao', 'effect_game_aa', 'effect_dynamic_shadows', 'effect_ssr',
        )
        for row, key in enumerate(effect_keys, 1):
            self._check(effects, row, key)
        ttk.Separator(effects).grid(row=8, column=0, columnspan=2,
                                    sticky='ew', pady=8)
        self._check(effects, 9, 'show_fps')
        self._check(effects, 10, 'skip_intro')
        ttk.Label(effects, text='特效越多，显卡负担越大；这些设置于下次启动应用。',
                  wraplength=590, foreground='#555555').grid(
            row=11, column=0, columnspan=2, sticky='w', pady=(10, 0),
        )

        self._check(advanced, 0, 'reactive')
        ttk.Label(advanced, text='反应遮罩帮助超分处理透明物体与粒子；遇到拖影时可尝试开启。',
                  wraplength=590, foreground='#555555').grid(
            row=1, column=0, columnspan=2, sticky='w', pady=(2, 12),
        )
        self._number(advanced, 2, '遮罩强度（0～16）', 'reactive_scale', 0.0, 16.0)
        self._number(advanced, 3, '遮罩阈值（0～1）', 'reactive_threshold', 0.0, 1.0)
        self._number(advanced, 4, '遮罩上限（0～1）', 'reactive_max', 0.0, 1.0)
        ttk.Label(advanced, text='通常保留现有值即可。关闭反应遮罩时，以上数值会保留。',
                  wraplength=590, foreground='#555555').grid(
            row=5, column=0, columnspan=2, sticky='w', pady=(12, 0),
        )

        self._ready = True
        self.refresh_summary()

    def _changed(self, key):
        if not self._ready:
            return
        raw = self._raw_values()
        if raw.get(key) != self._initial.get(key) or key in self._forced_dirty:
            self.dirty_keys.add(key)
        else:
            self.dirty_keys.discard(key)
        self.refresh_summary()

    def _choice(self, parent, row, title, key, labels, *, selectable=None):
        self._labels[key] = labels
        initial = self._initial.get(key, DEFAULTS[key])
        label = next((label for label, value in labels.items()
                      if str(value) == str(initial)), next(iter(labels)))
        var = tk.StringVar(value=label)
        self._vars[key] = var
        ttk.Label(parent, text=title).grid(row=row, column=0, sticky='w',
                                          padx=(0, 16), pady=4)
        control = ttk.Combobox(parent, textvariable=var, state='readonly',
                               values=list(selectable if selectable is not None else labels))
        control.grid(row=row, column=1, sticky='ew', pady=4)
        var.trace_add('write', lambda *_: self._changed(key))
        return control

    def _check(self, parent, row, key):
        var = tk.BooleanVar(value=self._initial.get(key, DEFAULTS[key]) == '1')
        self._vars[key] = var
        ttk.Checkbutton(parent, text=BOOLEAN_LABELS[key], variable=var).grid(
            row=row, column=0, columnspan=2, sticky='w', pady=3,
        )
        var.trace_add('write', lambda *_: self._changed(key))

    def _number(self, parent, row, title, key, minimum, maximum):
        var = tk.StringVar(value=self._initial.get(key, DEFAULTS[key]))
        self._vars[key] = var
        scale_var = tk.DoubleVar(value=float(var.get()))
        self._scales[key] = scale_var
        ttk.Label(parent, text=title).grid(row=row, column=0, sticky='w',
                                          padx=(0, 16), pady=5)
        controls = ttk.Frame(parent)
        controls.grid(row=row, column=1, sticky='ew', pady=5)
        controls.columnconfigure(0, weight=1)
        scale = ttk.Scale(controls, from_=minimum, to=maximum, variable=scale_var)
        scale.grid(row=0, column=0, sticky='ew', padx=(0, 12))
        ttk.Spinbox(controls, from_=minimum, to=maximum, increment=0.05,
                    textvariable=var, width=7, format='%.2f').grid(row=0, column=1)

        def from_scale(*_):
            if self._syncing_scale:
                return
            self._syncing_scale = True
            try:
                var.set(f'{scale_var.get():.2f}')
            finally:
                self._syncing_scale = False

        def from_entry(*_):
            if not self._syncing_scale:
                try:
                    number = float(var.get())
                    if math.isfinite(number) and minimum <= number <= maximum:
                        self._syncing_scale = True
                        scale_var.set(number)
                except (ValueError, tk.TclError):
                    pass
                finally:
                    self._syncing_scale = False
            self._changed(key)

        scale_var.trace_add('write', from_scale)
        var.trace_add('write', from_entry)

    def _raw_values(self):
        values = {'output_res': self._output_res}
        for key, variable in self._vars.items():
            value = variable.get()
            if key in self._labels:
                values[key] = str(self._labels[key][value])
            elif isinstance(variable, tk.BooleanVar):
                values[key] = '1' if value else '0'
            else:
                values[key] = str(value)
        return values

    def values(self):
        """Return validated control values without unrelated ini fields."""
        raw = self._raw_values()
        validated = validate_settings({**self._initial, **raw})
        return {key: validated[key] for key in raw}

    def changes(self):
        """Return edits only, so a stale window preserves in-game changes."""
        values = self.values()
        return {key: values[key] for key in self.dirty_keys}

    def mark_saved(self):
        self._initial.update(self.values())
        self._forced_dirty.clear()
        self.dirty_keys.clear()

    def set_profile(self, output_res, preset):
        """Apply an explicit resolution profile selected by the launcher."""
        preset = str(preset)
        candidate = validate_settings({**self._initial, 'output_res': output_res,
                                       'preset': preset})
        self._output_res = candidate['output_res']
        self._forced_dirty.update(('output_res', 'preset'))
        self.dirty_keys.update(('output_res', 'preset'))
        self._vars['preset'].set(next(label for label, value in PRESET_LABELS.items()
                                     if str(value) == candidate['preset']))
        self.refresh_summary()

    def refresh_summary(self):
        raw = self._raw_values()
        method = raw['upscaler']
        self.preset_control.configure(state='disabled' if method in ('off', 'taa')
                                      else 'readonly')
        if method == 'fsr4':
            self.algorithm_hint.set('FSR 4（INT8）：显卡负担较高，可降低画质档位来提高帧率。')
        elif method == 'fsr411':
            self.algorithm_hint.set('此 Windows 启动器暂不提供 FSR 4.1.1；不支持的配置会回退到 FSR 3.1。')
        elif method == 'taa':
            self.algorithm_hint.set('TAA：按输出分辨率进行原生渲染，并使用时域抗锯齿。')
        elif method == 'off':
            self.algorithm_hint.set('关闭超分：按输出分辨率进行原生渲染。')
        else:
            self.algorithm_hint.set('FSR 3.1：降低内部渲染分辨率，再重建到输出分辨率。')
        try:
            self.summary.set(render_description(validate_settings({**self._initial, **raw})))
        except ValueError as error:
            self.summary.set(str(error))
