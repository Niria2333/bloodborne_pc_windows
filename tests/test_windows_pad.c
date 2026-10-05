// Windows port modifications by yaonikaixin999999, 2026-10-05.
// SPDX-License-Identifier: GPL-2.0-or-later
/* Drive SDL's real virtual gamepad through the libScePad entry points. This
 * checks physical Xbox inputs, rather than the PS4-semantic replay facility. */
#include "../src/runtime_pad.c"
#include <assert.h>

static int captured;
int bbgpu_overlay_captures_input(void) { return captured; }
uintptr_t runtime_lookup(const RuntimeExport *table,size_t count,const char *name) {
    for (size_t i=0;i<count;++i)
        if (!strcmp(table[i].name,name)) return (uintptr_t)table[i].function;
    return 0;
}

static SDL_Joystick *virtual_pad;
static PadData read_state(void) {
    SDL_UpdateGamepads();
    PadData state;
    assert(pad_read_state(PAD_HANDLE,&state)==0);
    assert(state.connected && state.timestamp);
    return state;
}
static void set_button(SDL_GamepadButton button,int down) {
    assert(SDL_SetJoystickVirtualButton(virtual_pad,button,down));
}
static void set_axis(SDL_GamepadAxis axis_value,int16_t value) {
    assert(SDL_SetJoystickVirtualAxis(virtual_pad,axis_value,value));
}
static void assert_neutral(PadData state) {
    assert(state.buttons==0);
    assert(state.left_x==128 && state.left_y==128);
    assert(state.right_x==128 && state.right_y==128);
    assert(state.l2==0 && state.r2==0 && state.touch_count==0);
}
static void test_layout(const char *layout,const uint32_t expected[4]) {
    assert(_putenv_s("BB_PAD_LAYOUT",layout)==0);
    const SDL_GamepadButton buttons[]={SDL_GAMEPAD_BUTTON_SOUTH,
        SDL_GAMEPAD_BUTTON_EAST,SDL_GAMEPAD_BUTTON_WEST,SDL_GAMEPAD_BUTTON_NORTH};
    for (unsigned i=0;i<4;++i) {
        set_button(buttons[i],1);
        assert(read_state().buttons==expected[i]);
        set_button(buttons[i],0);
        assert_neutral(read_state());
    }
    printf("Pad layout %s: physical A/B/X/Y passed\n",*layout ? layout : "default");
}
static void test_other_inputs(const char *layout) {
    assert(_putenv_s("BB_PAD_LAYOUT",layout)==0);
    const struct { SDL_GamepadButton sdl; uint32_t ps; } buttons[]={
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,BTN_L1},
        {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,BTN_R1},
        {SDL_GAMEPAD_BUTTON_LEFT_STICK,BTN_L3},
        {SDL_GAMEPAD_BUTTON_RIGHT_STICK,BTN_R3},
        {SDL_GAMEPAD_BUTTON_START,BTN_OPTIONS},
        {SDL_GAMEPAD_BUTTON_BACK,BTN_TOUCHPAD},
        {SDL_GAMEPAD_BUTTON_TOUCHPAD,BTN_TOUCHPAD},
        {SDL_GAMEPAD_BUTTON_DPAD_UP,BTN_UP},
        {SDL_GAMEPAD_BUTTON_DPAD_DOWN,BTN_DOWN},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT,BTN_LEFT},
        {SDL_GAMEPAD_BUTTON_DPAD_RIGHT,BTN_RIGHT},
    };
    for (unsigned i=0;i<sizeof(buttons)/sizeof(*buttons);++i) {
        set_button(buttons[i].sdl,1);
        PadData state=read_state();
        assert(state.buttons==buttons[i].ps);
        if (buttons[i].ps==BTN_TOUCHPAD)
            assert(state.touch_count==1 && state.touches[0].x==480);
        set_button(buttons[i].sdl,0);
        assert_neutral(read_state());
    }
    set_axis(SDL_GAMEPAD_AXIS_LEFTX,-32768);
    set_axis(SDL_GAMEPAD_AXIS_LEFTY,32767);
    set_axis(SDL_GAMEPAD_AXIS_RIGHTX,32767);
    set_axis(SDL_GAMEPAD_AXIS_RIGHTY,-32768);
    set_axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER,32767);
    set_axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,0);
    PadData state=read_state();
    assert(state.left_x==0 && state.left_y==255);
    assert(state.right_x==255 && state.right_y==0);
    assert(state.l2==255 && state.r2>=127 && state.r2<=128);
    assert(state.buttons==(BTN_L2|BTN_R2));

    /* A modal dialog must neutralize even a held controller. The same held
     * inputs become available after the dialog releases its capture. */
    set_button(SDL_GAMEPAD_BUTTON_SOUTH,1);
    set_button(SDL_GAMEPAD_BUTTON_START,1);
    state=read_state();
    captured=1;
    assert_neutral(read_state());
    captured=0;
    PadData restored=read_state();
    assert(restored.buttons==state.buttons);
    assert(restored.left_x==state.left_x && restored.left_y==state.left_y);
    assert(restored.right_x==state.right_x && restored.right_y==state.right_y);
    assert(restored.l2==state.l2 && restored.r2==state.r2);
    set_button(SDL_GAMEPAD_BUTTON_SOUTH,0);
    set_button(SDL_GAMEPAD_BUTTON_START,0);
    set_axis(SDL_GAMEPAD_AXIS_LEFTX,0);
    set_axis(SDL_GAMEPAD_AXIS_LEFTY,0);
    set_axis(SDL_GAMEPAD_AXIS_RIGHTX,0);
    set_axis(SDL_GAMEPAD_AXIS_RIGHTY,0);
    set_axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER,-32768);
    set_axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,-32768);
    assert_neutral(read_state());
    printf("Pad layout %s: buttons, axes, capture and resume passed\n",layout);
}

int main(void) {
    assert(_putenv_s("BB_PAD_FILE","")==0);
    assert(_putenv_s("BB_PAD_RECORD","")==0);
    assert(_putenv_s("BB_PAD_REPLAY","")==0);
    assert(SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,"1"));
    assert(SDL_Init(SDL_INIT_GAMEPAD));
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type=SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.name="BB Xbox layout regression controller";
    desc.naxes=SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons=SDL_GAMEPAD_BUTTON_COUNT;
    desc.axis_mask=(1u<<SDL_GAMEPAD_AXIS_COUNT)-1;
    desc.button_mask=(1u<<SDL_GAMEPAD_BUTTON_COUNT)-1;
    SDL_JoystickID id=SDL_AttachVirtualJoystick(&desc);
    assert(id && SDL_IsGamepad(id));
    virtual_pad=SDL_OpenJoystick(id);
    assert(virtual_pad);
    /* Pin the runtime to this virtual device so a plugged-in physical Xbox
     * controller cannot change what the regression test samples. */
    gamepad=SDL_OpenGamepad(id);
    assert(gamepad);
    connected_count=1;
    set_axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER,-32768);
    set_axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,-32768);
    assert(pad_init()==0 && pad_open(1,0,0,NULL)==PAD_HANDLE);
    assert_neutral(read_state());
    const uint32_t ps4[]={BTN_CROSS,BTN_CIRCLE,BTN_SQUARE,BTN_TRIANGLE};
    const uint32_t xbox[]={BTN_CIRCLE,BTN_CROSS,BTN_TRIANGLE,BTN_SQUARE};
    test_layout("",ps4);
    test_layout("ps4",ps4);
    test_layout("xbox",xbox);
    test_layout("invalid",ps4);
    test_other_inputs("ps4");
    test_other_inputs("xbox");
    assert(pad_close(PAD_HANDLE)==0);
    SDL_CloseGamepad(gamepad); gamepad=NULL;
    SDL_CloseJoystick(virtual_pad);
    assert(SDL_DetachVirtualJoystick(id));
    SDL_Quit();
    puts("SDL virtual gamepad regression passed");
    return 0;
}
