// Windows port modifications by yaonikaixin999999, 2026-10-05.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_overlay.h"
#include "bbport_text_dialog.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>

#include <SDL3/SDL.h>
#include "bbport_settings.h"
#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

// DejaVu Sans (Cyrillic), embedded (third_party/fonts, Bitstream Vera license).
#ifdef _WIN32
asm(".section .rdata,\"dr\"\n"
    ".balign 16\n"
    ".global bb_font_ttf\n"
    "bb_font_ttf:\n"
    ".incbin \"" BB_FONT_PATH "\"\n"
    ".global bb_font_ttf_end\n"
    "bb_font_ttf_end:\n"
    ".text\n");
#else
asm(".section .rodata\n"
    ".balign 16\n"
    ".hidden bb_font_ttf\n"
    ".global bb_font_ttf\n"
    "bb_font_ttf:\n"
    ".incbin \"" BB_FONT_PATH "\"\n"
    ".hidden bb_font_ttf_end\n"
    ".global bb_font_ttf_end\n"
    "bb_font_ttf_end:\n"
    ".previous\n");
#endif
extern "C" const unsigned char bb_font_ttf[];
extern "C" const unsigned char bb_font_ttf_end[];

extern "C" void runtime_restart(void); // bb-probe (probe.c)

namespace BbOverlay {

namespace {

std::mutex imgui_mutex; // the ImGui context: window thread (input) and present thread
bool initialized = false;
std::atomic<bool> menu_open{false};
TextDialog text_dialog;
std::atomic<bool> text_captures{false};
unsigned text_key_guard = 0;
int text_stick_x = 0, text_stick_y = 0;
std::string text_composition;
bool l3_down = false, r3_down = false;
bool dirty = false; // settings changed while open: saved on close
float base_scale = 1.0f;

// Present rate for the FPS counter.
std::chrono::steady_clock::time_point last_present{};
float frame_ms_avg = 0.0f;

void UpdateTextCapture() {
    text_captures = text_dialog.CapturesInput() || text_key_guard;
}

void TextEntry() {
    const auto* viewport = ImGui::GetMainViewport();
    ImGui::GetBackgroundDrawList()->AddRectFilled(viewport->Pos,
        ImVec2(viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y),
        IM_COL32(0, 0, 0, 160));
    ImGui::SetNextWindowPos(ImVec2(viewport->Size.x * 0.5f, viewport->Size.y * 0.5f),
                           ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(700.0f * base_scale, 0));
    ImGui::Begin("角色名字 / Character name", nullptr,
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::TextUnformatted(text_dialog.prompt.c_str());
    ImGui::Separator();
    ImGui::TextWrapped("%s_", text_dialog.text.c_str());
    if (!text_composition.empty()) ImGui::TextDisabled("%s", text_composition.c_str());
    ImGui::TextDisabled("%u / %u", TextDialog::Units(text_dialog.text), text_dialog.max_length);
    ImGui::TextUnformatted("方向键 / 左摇杆移动   A 选择   B 取消   X 删除   Y / Menu 完成");
    ImGui::TextUnformatted("也可用键盘或鼠标；中文名字可用系统输入法。Enter 完成 / Esc 取消");
    ImGui::Separator();
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < TextDialog::Columns; ++col) {
            const int index = row * TextDialog::Columns + col;
            char label[24];
            char c = TextDialog::Keys[index];
            if (text_dialog.uppercase && c >= 'a' && c <= 'z') c -= 'a' - 'A';
            std::snprintf(label, sizeof(label), "%c##key%d", c, index);
            if (col) ImGui::SameLine();
            const bool selected = index == text_dialog.selected;
            if (selected) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.5f, 0.8f, 1));
            if (ImGui::Button(label, ImVec2(58.0f * base_scale, 38.0f * base_scale))) {
                text_dialog.Activate(index);
            }
            if (selected) ImGui::PopStyleColor();
        }
    }
    const char* actions[] = {"大小写", "删除", "空格", "完成", "取消"};
    for (int i = 0; i < 5; ++i) {
        if (i) ImGui::SameLine();
        const bool selected = text_dialog.selected == TextDialog::CharacterCount + i;
        if (selected) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.5f, 0.8f, 1));
        if (ImGui::Button(actions[i], ImVec2(122.0f * base_scale, 38.0f * base_scale))) {
            text_dialog.Activate(TextDialog::CharacterCount + i);
        }
        if (selected) ImGui::PopStyleColor();
    }
    ImGui::End();
    UpdateTextCapture();
}

void SetOpen(bool value) {
    if (menu_open.exchange(value) == value) {
        return;
    }
    ImGui::GetIO().MouseDrawCursor = value;
    if (!value && dirty) {
        dirty = false;
        BbSettings::Save();
    }
}

ImGuiKey KeyFromSdl(SDL_Keycode key) {
    switch (key) {
    case SDLK_TAB: return ImGuiKey_Tab;
    case SDLK_LEFT: return ImGuiKey_LeftArrow;
    case SDLK_RIGHT: return ImGuiKey_RightArrow;
    case SDLK_UP: return ImGuiKey_UpArrow;
    case SDLK_DOWN: return ImGuiKey_DownArrow;
    case SDLK_PAGEUP: return ImGuiKey_PageUp;
    case SDLK_PAGEDOWN: return ImGuiKey_PageDown;
    case SDLK_HOME: return ImGuiKey_Home;
    case SDLK_END: return ImGuiKey_End;
    case SDLK_DELETE: return ImGuiKey_Delete;
    case SDLK_BACKSPACE: return ImGuiKey_Backspace;
    case SDLK_SPACE: return ImGuiKey_Space;
    case SDLK_RETURN: return ImGuiKey_Enter;
    case SDLK_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDLK_ESCAPE: return ImGuiKey_Escape;
    case SDLK_LCTRL: return ImGuiKey_LeftCtrl;
    case SDLK_RCTRL: return ImGuiKey_RightCtrl;
    case SDLK_LSHIFT: return ImGuiKey_LeftShift;
    case SDLK_RSHIFT: return ImGuiKey_RightShift;
    case SDLK_LALT: return ImGuiKey_LeftAlt;
    case SDLK_RALT: return ImGuiKey_RightAlt;
    default: return ImGuiKey_None;
    }
}

ImGuiKey KeyFromGamepad(u8 button) {
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return ImGuiKey_GamepadFaceDown;
    case SDL_GAMEPAD_BUTTON_EAST: return ImGuiKey_GamepadFaceRight;
    case SDL_GAMEPAD_BUTTON_WEST: return ImGuiKey_GamepadFaceLeft;
    case SDL_GAMEPAD_BUTTON_NORTH: return ImGuiKey_GamepadFaceUp;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return ImGuiKey_GamepadDpadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return ImGuiKey_GamepadDpadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return ImGuiKey_GamepadDpadLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return ImGuiKey_GamepadDpadRight;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ImGuiKey_GamepadL1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ImGuiKey_GamepadR1;
    case SDL_GAMEPAD_BUTTON_START: return ImGuiKey_GamepadStart;
    case SDL_GAMEPAD_BUTTON_BACK: return ImGuiKey_GamepadBack;
    default: return ImGuiKey_None;
    }
}

float PixelDensity(SDL_WindowID id) {
    SDL_Window* window = SDL_GetWindowFromID(id);
    const float density = window ? SDL_GetWindowPixelDensity(window) : 1.0f;
    return density > 0.0f ? density : 1.0f;
}

// Marks the settings dirty when a widget changed them.
template <typename T>
void Store(std::atomic<T>& target, T value, bool changed) {
    if (changed) {
        target = value;
        dirty = true;
    }
}

void Checkbox(const char* label, std::atomic<bool>& value) {
    bool v = value;
    Store(value, v, ImGui::Checkbox(label, &v));
}

void Slider(const char* label, std::atomic<float>& value, float lo, float hi) {
    float v = value;
    Store(value, v, ImGui::SliderFloat(label, &v, lo, hi, "%.2f"));
}

void Hint(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void Menu() {
    auto& s = BbSettings::Get();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + 40.0f * base_scale,
                                   viewport->WorkPos.y + 40.0f * base_scale),
                            ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(ImVec2(620.0f * base_scale, 0.0f), ImGuiCond_Appearing);
    bool keep_open = true;
    if (!ImGui::Begin("Bloodborne — настройки  (Insert / L3+R3)", &keep_open,
                      ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    ImGui::Text("%.0f FPS  (%.1f мс)", frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f,
                frame_ms_avg);

    ImGui::SeparatorText("Временной апскейлер");
    static const char* upscalers[] = {"Выкл", "FSR 3.1", "FSR 4 (INT8)", "FSR 4.1.1 (INT8)",
                                     "TAA (нативное сглаживание)"};
    static const char* later[] = {"DLSS", "XeSS"};
    int upscaler = s.upscaler;
    if (ImGui::BeginCombo("Апскейлер", upscalers[upscaler])) {
        for (int i = 0; i < BbSettings::UpscalerCount; ++i) {
            const bool supported = i == BbSettings::UpscalerFsr4 ? s.fsr4_supported.load()
                : i == BbSettings::UpscalerFsr411 ? s.fsr411_supported.load() : true;
            ImGui::BeginDisabled(!supported);
            if (ImGui::Selectable(upscalers[i], i == upscaler)) {
                Store(s.upscaler, i, true);
            }
            ImGui::EndDisabled();
            if (!supported) {
                ImGui::SameLine();
                ImGui::TextDisabled("— не поддерживается видеокартой");
            }
        }
        for (const char* name : later) {
            ImGui::BeginDisabled();
            ImGui::Selectable(name, false);
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("— в работе");
        }
        ImGui::EndCombo();
    }
    if (const char* problem = s.fsr4_problem.load()) {
        ImGui::PushTextWrapPos();
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "FSR 4 недоступен: %s", problem);
        if (!BbSettings::IsFsr4(s.upscaler))
            ImGui::TextUnformatted("Активен режим, выбранный выше. FSR 4 можно выбрать снова.");
        ImGui::PopTextWrapPos();
    }
    if (BbSettings::IsFsr4(s.upscaler)) {
        if (s.upscaler == BbSettings::UpscalerFsr411) {
            Hint("FSR 4.1.1 в режиме INT8: модель из DLL AMD 4.1.1, воспроизведённая в Vulkan "
                 "(результат совпадает с DLL). Одна модель для Native..Performance и отдельная "
                 "для Ultra Performance. Ассеты: tools/fsr4cap/build_assets.sh (нужны DLL и Proton).");
        } else {
            Hint("FSR 4 в режиме INT8 (модель v07 из исходников AMD FidelityFX SDK). Качество выше, "
                 "чем у FSR 3.1, но проход тяжелее. Смена пресета пересобирает модель (короткая "
                 "пауза). Ассеты: tools/fetch_fsr4_assets.sh.");
        }
        Checkbox("FSR 4: авто-экспозиция", s.fsr4_auto_exposure);
        Checkbox("FSR 4: обратный знак jitter", s.fsr4_invert_jitter);
        Hint("Проверка при гостинге: сеть FSR 4 нормирует цвет по экспозиции и по ней решает, "
             "когда отбросить прошлые кадры. Меняются сразу, без перезапуска.");
    }
    const bool upscaler_on = s.upscaler != BbSettings::UpscalerOff;
    const bool taa = s.upscaler == BbSettings::UpscalerTaa;
    ImGui::BeginDisabled(!upscaler_on);
    ImGui::BeginDisabled(taa);
    int preset = taa ? BbSettings::NativeAA : s.preset.load();
    char preset_label[64];
    std::snprintf(preset_label, sizeof(preset_label), "%s (x%.1f)", BbSettings::PresetName(preset),
                  BbSettings::PresetScale(preset));
    if (ImGui::BeginCombo("Пресет", preset_label)) {
        for (int i = 0; i < BbSettings::PresetCount; ++i) {
            char label[64];
            const float scale = BbSettings::PresetScale(i);
            const int output = s.output_res;
            std::snprintf(label, sizeof(label), "%s (x%.1f, рендер %dx%d)",
                          BbSettings::PresetName(i), scale,
                          int(std::lround(BbSettings::OutputWidths[output] / scale / 2) * 2),
                          int(std::lround(BbSettings::OutputHeights[output] / scale / 2) * 2));
            if (ImGui::Selectable(label, i == preset)) {
                Store(s.preset, i, true);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    if (taa) {
        ImGui::TextWrapped("TAA сглаживает сцену в разрешении вывода, без модели FSR и апскейлинга. "
                           "Сохранённый пресет FSR восстановится при выборе FSR.");
    }
    ImGui::Text("Активный рендер сцены: %d x %d", s.active_render_width.load(),
                s.active_render_height.load());
    if (BbSettings::FixedRenderSession()) {
        ImGui::Text("Пресет при запуске: %s", BbSettings::PresetName(s.startup_preset));
        if (const char* automatic = std::getenv("BB_AUTO_RENDER_RES");
            automatic && automatic[0] == '1') {
            Hint("При выводе не 1080p вся игра рисуется в разрешении пресета (патч при запуске): "
                 "это быстрее всего на Steam Deck и слабых GPU. Смена пресета или разрешения "
                 "вывода — после перезапуска. Пункт «Смена разрешения на лету» ниже включает "
                 "смену без перезапуска (постобработка тогда остаётся в 1080p, медленнее).");
        } else {
            Hint("BB_RENDER_RES фиксирует размер сцены при запуске. Уберите эту явную переменную "
                 "для смены разрешения и пресетов без перезапуска игры.");
        }
    } else {
        Hint("Native AA: FSR работает как сглаживание. Остальные пресеты уменьшают разрешение "
             "отрисовки сцены относительно вывода. Интерфейс рисуется в разрешении вывода. "
             "Пресет применяется со следующего кадра без перезапуска игры.");
    }
    Checkbox("Резкость (RCAS)", s.sharpen);
    ImGui::BeginDisabled(!s.sharpen);
    Slider("Сила резкости", s.sharpness, 0.0f, 2.0f);
    Hint("До 1 — резкость самого апскейлера (RCAS). Выше 1 добавляется ещё один проход RCAS. "
         "Ctrl+клик по ползунку — ввести точное значение.");
    ImGui::EndDisabled();
    Checkbox("Субпиксельный сдвиг (jitter)", s.jitter);
    Hint("Каждый кадр сцена сдвигается на долю пикселя, и апскейлер собирает из нескольких "
         "кадров больше деталей. Без него получается только сглаживание по истории.");

    ImGui::SeparatorText("Маска реактивности");
    ImGui::BeginDisabled(taa);
    Checkbox("Включить маску", s.reactive);
    Hint("Помечает прозрачные эффекты (частицы, дымку), чтобы апскейлер меньше опирался на "
         "прошлые кадры. Меньше шлейфов за эффектами, но под ними возвращается дрожание.");
    ImGui::BeginDisabled(!s.reactive);
    Slider("Масштаб", s.reactive_scale, 0.0f, 4.0f);
    Slider("Порог", s.reactive_threshold, 0.0f, 1.0f);
    Slider("Максимум", s.reactive_max, 0.0f, 1.0f);
    bool show_mask = s.debug_view == BbSettings::DebugReactive;
    if (ImGui::Checkbox("Показать маску (отладка)", &show_mask)) {
        s.debug_view = show_mask ? BbSettings::DebugReactive : BbSettings::DebugNone;
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    Checkbox("Векторы движения персонажей", s.object_motion);
    Hint("Точные векторы для анимированных объектов: одежда и оружие меньше рассыпаются "
         "при движении. Статичная сцена не получает дополнительный проход. "
         "Изменение применяется после перезапуска игры.");
    bool show_motion = s.debug_view == BbSettings::DebugMotion;
    if (ImGui::Checkbox("Показать векторы движения (отладка)", &show_motion)) {
        s.debug_view = show_motion ? BbSettings::DebugMotion : BbSettings::DebugNone;
    }
    Hint("Красный/зелёный: движение по горизонтали/вертикали (8 пикселей = полная яркость). "
         "Синий: пиксель получил точный вектор объекта, а не только движение камеры. "
         "Движущийся предмет без синего и без красного/зелёного апскейлер считает "
         "неподвижным, отсюда шлейф.");
    ImGui::EndDisabled(); // upscaler off

    ImGui::SeparatorText("Разрешение вывода");
    static const char* outputs[] = {"1280 x 720", "1920 x 1080", "2560 x 1440", "3840 x 2160"};
    int output = s.output_res;
    if (ImGui::BeginCombo("Разрешение вывода", outputs[output])) {
        for (int i = 0; i < BbSettings::OutputCount; ++i) {
            if (ImGui::Selectable(outputs[i], i == output)) {
                Store(s.output_res, i, true);
            }
        }
        ImGui::EndCombo();
    }
    if (BbSettings::FixedRenderSession()) {
        Hint("Размер готового кадра и интерфейса. Пресет задаёт размер сцены относительно "
             "вывода: 4K Performance = 1920x1080. Применяется после перезапуска игры.");
    } else {
        Hint("Размер готового кадра и интерфейса меняется на границе следующего кадра. "
             "Пресет задаёт размер сцены относительно вывода: 4K Performance = 1920x1080. "
             "Смена размера сбрасывает историю FSR и может вызвать короткую паузу.");
    }
    static const char* live_modes[] = {"Авто (по видеокарте)", "Выключена (быстрее)", "Включена"};
    int live = s.live_resolution + 1;
    if (ImGui::BeginCombo("Смена разрешения на лету", live_modes[live])) {
        for (int i = 0; i < 3; ++i) {
            if (ImGui::Selectable(live_modes[i], i == live)) {
                Store(s.live_resolution, i - 1, true);
            }
        }
        ImGui::EndCombo();
    }
    Hint("Включена: разрешение вывода и пресет меняются без перезапуска, но постобработка игры "
         "остаётся в 1080p — на Steam Deck и старых видеокартах это заметно медленнее. "
         "Выключена: всё рисуется в разрешении пресета, смена — через перезапуск. Авто включает "
         "её на мощных дискретных видеокартах. Применяется после перезапуска игры.");
    ImGui::SeparatorText("Эффекты игры (после перезапуска)");
    static const char* lods[] = {"Максимальная (-2)", "Как в игре", "Ниже (1)", "Минимальная (2)"};
    static constexpr int lod_values[] = {-2, 0, 1, 2};
    int lod_index = 1;
    for (int i = 0; i < 4; ++i) {
        if (lod_values[i] == s.model_lod) lod_index = i;
    }
    if (ImGui::BeginCombo("Детализация моделей", lods[lod_index])) {
        for (int i = 0; i < 4; ++i) {
            if (ImGui::Selectable(lods[i], i == lod_index)) {
                Store(s.model_lod, lod_values[i], true);
            }
        }
        ImGui::EndCombo();
    }
    for (int e = 0; e < BbSettings::EffectCount; ++e) {
        Checkbox(BbSettings::Effects[e].label, s.effects[e]);
    }
    Hint("Эффекты включаются и выключаются патчами игры при запуске (patches/Bloodborne.xml). "
         "Размытие в движении и тени от динамических источников заметно нагружают GPU.");
    Hint("Свободная камера: удерживайте Cross и нажимайте L3 (клавиатура: Space + Z). "
         "Debug menu: левый touchpad / Tab. Нужны DbgFont14h.ccm и DbgFont14h.tpf "
         "в dvdroot_ps4/font из мода Nexus #253. Правый touchpad: Backspace.");

    bool restart = s.object_motion != s.startup_object_motion ||
                   s.model_lod != s.startup_model_lod ||
                   s.live_resolution != s.startup_live_resolution ||
                   BbSettings::ResolutionNeedsRestart();
    for (int e = 0; e < BbSettings::EffectCount; ++e) {
        restart |= s.effects[e] != s.startup_effects[e];
    }
    if (restart) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                           "Изменения применятся после перезапуска игры");
        if (ImGui::Button("Применить и перезапустить игру")) {
            BbSettings::Save();
            runtime_restart();
        }
    }

    ImGui::SeparatorText("Прочее");
    Checkbox("Счётчик FPS в углу", s.show_fps);

    ImGui::Spacing();
    if (ImGui::Button("Закрыть")) {
        keep_open = false;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Настройки сохраняются в bbport.ini");
    ImGui::End();
    if (!keep_open) {
        SetOpen(false);
    }
}

void FpsCounter() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - pad,
                                   viewport->WorkPos.y + pad),
                            ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.5f);
    ImGui::Begin("##fps", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    const auto& s = BbSettings::Get();
    ImGui::Text("%.0f FPS  %.1f мс  %s", frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f,
                frame_ms_avg,
                s.upscaler == BbSettings::UpscalerFsr3   ? "FSR 3.1"
                : s.upscaler == BbSettings::UpscalerFsr4 ? "FSR 4"
                : s.upscaler == BbSettings::UpscalerFsr411 ? "FSR 4.1.1"
                : s.upscaler == BbSettings::UpscalerTaa ? "TAA"
                                                         : "");
    ImGui::End();
}

} // namespace

void Init(const Vulkan::Instance& instance, vk::Format format, u32 image_count) {
    std::scoped_lock lock{imgui_mutex};
    if (initialized) {
        return;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // window positions are not kept
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.BackendPlatformName = "bbport";

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.Colors[ImGuiCol_WindowBg].w = 0.92f;

    ImFontConfig font_config;
    font_config.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(bb_font_ttf),
                                   int(bb_font_ttf_end - bb_font_ttf), 18.0f, &font_config);
#ifdef _WIN32
    // Use the installed Windows font for Chinese names and instructions.
    const char* windir = std::getenv("WINDIR");
    const std::string chinese_font = std::string(windir ? windir : "C:/Windows") + "/Fonts/msyh.ttc";
    if (FILE* font = std::fopen(chinese_font.c_str(), "rb")) {
        std::fclose(font);
        ImFontConfig merge;
        merge.MergeMode = true;
        io.Fonts->AddFontFromFileTTF(chinese_font.c_str(), 18.0f, &merge,
                                   io.Fonts->GetGlyphRangesChineseFull());
    }
#endif

    const vk::Instance vk_instance = instance.GetInstance();
    ImGui_ImplVulkan_LoadFunctions(
        instance.ApiVersion(),
        [](const char* name, void* user) {
            return VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr(
                *static_cast<const vk::Instance*>(user), name);
        },
        const_cast<vk::Instance*>(&vk_instance));

    const VkFormat color_format = static_cast<VkFormat>(format);
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = instance.ApiVersion();
    info.Instance = vk_instance;
    info.PhysicalDevice = instance.GetPhysicalDevice();
    info.Device = instance.GetDevice();
    info.QueueFamily = instance.GetGraphicsQueueFamilyIndex();
    info.Queue = instance.GetGraphicsQueue();
    info.DescriptorPoolSize = 16;
    info.MinImageCount = std::max(image_count, 2u);
    info.ImageCount = std::max(image_count, 2u);
    info.UseDynamicRendering = true;
    info.PipelineInfoMain.PipelineRenderingCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &color_format,
    };
    if (!ImGui_ImplVulkan_Init(&info)) {
        std::printf("Overlay: ImGui Vulkan backend init failed\n");
        ImGui::DestroyContext();
        return;
    }
    initialized = true;
    std::printf("Overlay: menu ready (Insert or L3+R3)\n");
}

void UpdateTextInput(SDL_Window* window) {
    bool want = false;
    {
        std::scoped_lock lock{imgui_mutex};
        want = initialized && (text_dialog.active || (menu_open && ImGui::GetIO().WantTextInput));
        if (text_dialog.active) {
            int w = 0, h = 0;
            SDL_GetWindowSize(window, &w, &h);
            SDL_Rect area{std::max(0, w / 2 - 300), std::max(0, h / 2 - 120), 600, 40};
            SDL_SetTextInputArea(window, &area, 0);
        }
    }
    if (want != SDL_TextInputActive(window)) {
        if (want) {
            SDL_StartTextInput(window);
        } else {
            SDL_StopTextInput(window);
        }
    }
}

bool HandleEvent(const SDL_Event& event) {
    std::scoped_lock lock{imgui_mutex};
    if (!initialized) {
        return false;
    }
    ImGuiIO& io = ImGui::GetIO();
    const bool was_text = text_captures;
    if (event.type == SDL_EVENT_GAMEPAD_REMOVED || event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        text_dialog.ReleaseButtons();
        text_key_guard = 0;
        text_composition.clear();
        UpdateTextCapture();
    }
    if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || event.type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
        using B = TextDialog::Button;
        B button = B::Count;
        switch (event.gbutton.button) {
            case SDL_GAMEPAD_BUTTON_SOUTH: button = B::Select; break;
            case SDL_GAMEPAD_BUTTON_EAST: button = B::Cancel; break;
            case SDL_GAMEPAD_BUTTON_WEST: button = B::Delete; break;
            case SDL_GAMEPAD_BUTTON_NORTH:
            case SDL_GAMEPAD_BUTTON_START: button = B::Confirm; break;
            case SDL_GAMEPAD_BUTTON_DPAD_UP: button = B::Up; break;
            case SDL_GAMEPAD_BUTTON_DPAD_DOWN: button = B::Down; break;
            case SDL_GAMEPAD_BUTTON_DPAD_LEFT: button = B::Left; break;
            case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: button = B::Right; break;
        }
        if (button != B::Count) text_dialog.Pad(button, event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
        UpdateTextCapture();
        if (was_text) return true;
    }
    if (event.type == SDL_EVENT_KEY_UP) {
        if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER) text_key_guard &= ~1u;
        if (event.key.key == SDLK_ESCAPE) text_key_guard &= ~2u;
        UpdateTextCapture();
        if (was_text) return true;
    }
    if (text_dialog.active) {
        if (event.type == SDL_EVENT_TEXT_INPUT) {
            text_dialog.Append(event.text.text); text_composition.clear(); return true;
        }
        if (event.type == SDL_EVENT_TEXT_EDITING) {
            text_composition = event.edit.text ? event.edit.text : ""; return true;
        }
        if (event.type == SDL_EVENT_KEY_DOWN) {
            if (!text_composition.empty()) return true; // Enter commits an IME candidate first
            switch (event.key.key) {
                case SDLK_BACKSPACE: text_dialog.Delete(); break;
                case SDLK_RETURN:
                case SDLK_KP_ENTER: text_key_guard |= 1; text_dialog.Finish(true); break;
                case SDLK_ESCAPE: text_key_guard |= 2; text_dialog.Finish(false); break;
                case SDLK_UP: text_dialog.Move(0, -1); break;
                case SDLK_DOWN: text_dialog.Move(0, 1); break;
                case SDLK_LEFT: text_dialog.Move(-1, 0); break;
                case SDLK_RIGHT: text_dialog.Move(1, 0); break;
            }
            UpdateTextCapture(); return true;
        }
        if (event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
            const int direction = event.gaxis.value < -16000 ? -1 : event.gaxis.value > 16000 ? 1 : 0;
            if (event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX) {
                if (direction && direction != text_stick_x) text_dialog.Move(direction, 0);
                text_stick_x = direction;
            } else if (event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY) {
                if (direction && direction != text_stick_y) text_dialog.Move(0, direction);
                text_stick_y = direction;
            }
            return true;
        }
    }
    const bool is_open = menu_open || text_dialog.active;
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const bool down = event.type == SDL_EVENT_KEY_DOWN;
        if (down && !event.key.repeat &&
            (event.key.key == SDLK_INSERT || (is_open && event.key.key == SDLK_ESCAPE))) {
            SetOpen(event.key.key == SDLK_INSERT ? !is_open : false);
            return true;
        }
        if (!is_open) {
            return false;
        }
        io.AddKeyEvent(ImGuiMod_Ctrl, (event.key.mod & SDL_KMOD_CTRL) != 0);
        io.AddKeyEvent(ImGuiMod_Shift, (event.key.mod & SDL_KMOD_SHIFT) != 0);
        io.AddKeyEvent(ImGuiMod_Alt, (event.key.mod & SDL_KMOD_ALT) != 0);
        if (const ImGuiKey key = KeyFromSdl(event.key.key); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        const bool down = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        const u8 button = event.gbutton.button;
        if (button == SDL_GAMEPAD_BUTTON_LEFT_STICK) {
            l3_down = down;
        } else if (button == SDL_GAMEPAD_BUTTON_RIGHT_STICK) {
            r3_down = down;
        }
        if (down && l3_down && r3_down) {
            SetOpen(!is_open);
            return true;
        }
        if (!is_open) {
            return false;
        }
        if (const ImGuiKey key = KeyFromGamepad(button); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_TEXT_INPUT: {
        // Typed characters (Ctrl+click on a slider, a text field): key events alone erase but
        // do not type. SDL sends them while text input is on (UpdateTextInput).
        if (!is_open) {
            return false;
        }
        io.AddInputCharactersUTF8(event.text.text);
        return true;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        if (!is_open) {
            return false;
        }
        const float density = PixelDensity(event.motion.windowID);
        io.AddMousePosEvent(event.motion.x * density, event.motion.y * density);
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (!is_open) {
            return false;
        }
        const int button = event.button.button == SDL_BUTTON_LEFT    ? 0
                           : event.button.button == SDL_BUTTON_RIGHT  ? 1
                           : event.button.button == SDL_BUTTON_MIDDLE ? 2
                                                                      : -1;
        if (button >= 0) {
            io.AddMouseButtonEvent(button, event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        }
        return true;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        if (!is_open) {
            return false;
        }
        io.AddMouseWheelEvent(event.wheel.x, event.wheel.y);
        return true;
    default:
        return false;
    }
}

bool Visible() {
    return initialized && (menu_open || text_captures || BbSettings::Get().show_fps);
}

bool CapturesInput() {
    return menu_open || text_captures;
}

bool BeginTextInput(const std::string& initial, const std::string& prompt, u32 max_length) {
    std::scoped_lock lock{imgui_mutex};
    if (!initialized) return false;
    text_dialog.Begin(initial, prompt, max_length);
    text_composition.clear();
    text_stick_x = text_stick_y = 0;
    UpdateTextCapture();
    return true;
}

int PollTextInput(std::string& text) {
    std::scoped_lock lock{imgui_mutex};
    text = text_dialog.text;
    return text_dialog.state;
}

void EndTextInput() {
    std::scoped_lock lock{imgui_mutex};
    text_dialog.End();
    text_composition.clear();
    UpdateTextCapture();
}

void Render(vk::CommandBuffer cmdbuf, vk::ImageView view, vk::Extent2D extent) {
    // Present interval for the FPS readout (measured also while nothing is drawn).
    const auto now = std::chrono::steady_clock::now();
    const float ms = std::chrono::duration<float, std::milli>(now - last_present).count();
    last_present = now;
    if (ms > 0.0f && ms < 1000.0f) {
        frame_ms_avg = frame_ms_avg == 0.0f ? ms : frame_ms_avg * 0.95f + ms * 0.05f;
    }
    if (!Visible()) {
        return;
    }
    std::scoped_lock lock{imgui_mutex};
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(extent.width), float(extent.height));
    io.DeltaTime = ms > 0.0f && ms < 1000.0f ? ms / 1000.0f : 1.0f / 60.0f;
    // UI scale follows the display height (1080p = 1).
    const float scale = std::max(float(extent.height) / 1080.0f, 0.75f);
    if (std::abs(scale - base_scale) > 0.01f) {
        ImGuiStyle& style = ImGui::GetStyle();
        style.ScaleAllSizes(scale / base_scale);
        style.FontScaleMain = scale;
        base_scale = scale;
    }

    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    io.MouseDrawCursor = menu_open || text_dialog.active;
    if (text_dialog.active) {
        TextEntry();
    } else if (menu_open) {
        Menu();
    }
    if (BbSettings::Get().show_fps && !menu_open) {
        FpsCounter();
    }
    ImGui::Render();

    const vk::RenderingAttachmentInfo attachment{
        .imageView = view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    cmdbuf.beginRendering(vk::RenderingInfo{
        .renderArea = {{0, 0}, extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
    });
    {
        // Font atlas uploads submit to the graphics queue themselves.
        std::scoped_lock submit_lock{Vulkan::Scheduler::submit_mutex};
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmdbuf);
    }
    cmdbuf.endRendering();
}

} // namespace BbOverlay
