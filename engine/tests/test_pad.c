#define _GNU_SOURCE
#include <assert.h>
#include <unistd.h>
#include "../src/runtime_pad.c"

/* A failed check names its line on stdout (MinGW's assert reports elsewhere, if at all). */
#undef assert
#define assert(e) ((e) ? (void)0 : (printf("FAIL: test_pad.c:%d: %s\n", __LINE__, #e), abort()))

#ifdef _WIN32
/* MinGW has no setenv; getenv in runtime_pad.c reads what _putenv_s sets. */
static int setenv(const char *name, const char *value, int overwrite) {
    (void)overwrite;
    return _putenv_s(name, value);
}
#endif

/* A file of its own in the temp folder (TEMP on Windows, /tmp elsewhere), written whole. */
static void temp_file(char *out, size_t size, const char *name, const char *text) {
    const char *dir = getenv("TEMP");
    snprintf(out, size, "%s/bbport-%s-%d", dir && *dir ? dir : "/tmp", name, (int)getpid());
    FILE *f = fopen(out, "w");
    assert(f);
    fputs(text, f);
    fclose(f);
}

static int capture, menu;
// The runtime clock (src/runtime_host.c), not linked into this test.
uint64_t host_monotonic_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
int bbgpu_overlay_captures_input(void) { return capture; }
// The map (bbgpu): on only where the test says, counting the map button's toggles.
static int map_on, map_toggles;
int bbgpu_map_available(void) { return map_on; }
void bbgpu_map_toggle(void) { ++map_toggles; }
int bbgpu_pc_input(BbPcPad* p,const bool* keys,int n,int gamepad) {
    (void)p;(void)keys;(void)n;(void)gamepad;return 0;
}
int bbgpu_menu_buttons(uint32_t* confirm,uint32_t* back) { *confirm=BTN_CROSS; *back=BTN_CIRCLE; return menu; }
// Co-op ping / gather (the party's, bbgpu): no party in this test.
int bbgpu_ping_combo_enabled(void) { return 0; }
void bbgpu_ping_request(void) {}
int bbgpu_gather_combo_enabled(void) { return 0; }
void bbgpu_gather_request(const char *source) { (void)source; }
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    (void)table; (void)count; (void)name;
    return 0;
}

static void inject(const char *path, const char *tokens) {
    FILE *f=fopen(path,"w");
    assert(f);
    fputs(tokens,f);
    fclose(f);
    usleep(25000);
}

int main(void) {
    setvbuf(stdout,NULL,_IONBF,0);  /* the lines before a failed assert reach the log */
    char path[512];
    temp_file(path,sizeof path,"pad-test","");
    setenv("BB_PAD_FILE",path,1);
    /* Every gamepad button and input the launcher's controller page can write (Core/PadMap.cs:
     * Buttons, Defaults) is one this parser knows: a launcher binding never falls back silently. */
    static const char *const launcher_buttons[]={"a","b","x","y","leftshoulder","rightshoulder","lefttrigger",
        "righttrigger","leftstick","rightstick","dpup","dpdown","dpleft","dpright","start","back","touchpad",
        "misc1","paddle1","paddle2","paddle3","paddle4"};
    for (size_t i=0;i<sizeof launcher_buttons/sizeof *launcher_buttons;++i)
        assert(pad_button_from_name(launcher_buttons[i])>=0);
    static const char *const launcher_inputs[]={"cross","circle","square","triangle","l1","r1","l2","r2","l3","r3",
        "options","touchpad","touchpad_right","up","down","left","right","ping","gather","menu_confirm","menu_back","map"};
    for (size_t i=0;i<sizeof launcher_inputs/sizeof *launcher_inputs;++i) {
        int found=-1;
        for (int k=0;k<IN_COUNT;++k) if (!strcmp(launcher_inputs[i],input_names[k])) found=k;
        assert(found>=0 && !(found>=IN_MOVE_UP && found<IN_MENU_CONFIRM));  /* a pad input, not keyboard-only */
    }
    /* bbport.ini controls: buttons moved, a trigger as a button and a button as a trigger. */
    char config[512];
    temp_file(config,sizeof config,"pad-config",
              "upscaler=fsr3\npad.cross=b\npad.circle=a\npad.r2=rightshoulder\n"
              "pad.r1=righttrigger\nkey.cross=X, Space\npad.bogus=a\npad.menu_back=b\n");
    setenv("BB_CONFIG",config,1);
    setenv("SDL_VIDEODRIVER","dummy",1);
    /* Only the virtual test controller is a gamepad, whatever is plugged in. */
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT,"0x1d50/0x6189");
    assert(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMEPAD));
    assert(pad_init()==0 && pad_open(1,0,0,NULL)==1);
    PadData data;
    inject(path,"cross l3 touchpad_left");
    assert(pad_read_state(1,&data)==0);
    assert((data.buttons & (BTN_CROSS|BTN_L3|BTN_TOUCHPAD))==(BTN_CROSS|BTN_L3|BTN_TOUCHPAD));
    /* A new touch gets a new id (1..127), as from a DualShock 4: the game ignores id 0. */
    assert(data.touch_count==1 && data.touches[0].x==480 && data.touches[0].y==471 && data.touches[0].id==1);
    inject(path,"touchpad_right");
    assert(pad_read_state(1,&data)==0 && data.touch_count==1 && data.touches[0].x==1440);
    inject(path,"");
    assert(pad_read_state(1,&data)==0 && data.buttons==0 && data.touch_count==0);

    SDL_VirtualJoystickTouchpadDesc touch={.nfingers=2};
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type=SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes=SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons=SDL_GAMEPAD_BUTTON_COUNT;
    desc.button_mask=(1u<<SDL_GAMEPAD_BUTTON_COUNT)-1;
    desc.axis_mask=(1u<<SDL_GAMEPAD_AXIS_COUNT)-1;
    desc.name="bbport test controller";
    desc.vendor_id=0x1d50;
    desc.product_id=0x6189;
    desc.ntouchpads=1;
    desc.touchpads=&touch;
    SDL_JoystickID id=SDL_AttachVirtualJoystick(&desc);
    assert(id!=0);
    SDL_Joystick *joystick=SDL_OpenJoystick(id);
    assert(joystick);
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,0,true,0.75f,0.5f,1.0f));
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,1,true,0.25f,1.0f,1.0f));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_TOUCHPAD,true));
    SDL_UpdateJoysticks();
    SDL_UpdateGamepads();
    assert(pad_read_state(1,&data)==0);
    assert(gamepad && data.touch_count==2 && (data.buttons & BTN_TOUCHPAD));
    assert(data.touches[0].x==1439 && data.touches[0].y==471 && data.touches[0].id==2);
    assert(data.touches[1].x==480 && data.touches[1].y==942 && data.touches[1].id==3);
    capture=1;
    assert(pad_read_state(1,&data)==0 && data.touch_count==0 && data.buttons==0);
    capture=0;
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,0,false,0,0,0));
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,1,false,0,0,0));
    SDL_UpdateJoysticks();
    SDL_UpdateGamepads();
    assert(pad_read_state(1,&data)==0 && data.touch_count==1 && data.touches[0].x==480);
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_TOUCHPAD,false));
    /* Everything let go of once: buttons held through the overlay's capture stay hidden until
     * then (hold_after_capture). */
    SDL_UpdateJoysticks();
    SDL_UpdateGamepads();
    assert(pad_read_state(1,&data)==0 && data.buttons==0);
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_EAST,true));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,true));
    assert(SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,32767));
    SDL_UpdateJoysticks();
    SDL_UpdateGamepads();
    assert(pad_read_state(1,&data)==0);
    assert(data.buttons==(BTN_CROSS|BTN_R2|BTN_R1) && data.r2==255);
    /* pad.menu_back=b: in a menu, B goes back (Circle) and no longer gives its world input (Cross). */
    menu=1;
    assert(pad_read_state(1,&data)==0);
    assert((data.buttons & (BTN_CROSS|BTN_CIRCLE))==BTN_CIRCLE);
    menu=0;
    /* Still held as the menu closed: no Cross in the world until B is let go of. */
    assert(pad_read_state(1,&data)==0 && !(data.buttons & (BTN_CROSS|BTN_CIRCLE)));
    assert(bindings[IN_CROSS].key_count==2 && bindings[IN_CROSS].keys[0]==SDL_SCANCODE_X &&
           bindings[IN_CROSS].keys[1]==SDL_SCANCODE_SPACE);

    /* Every PS4 button remapped at once: each input moved to the next physical button (triggers
     * included), as the launcher's controller page writes them. Each physical button then gives
     * exactly its new input and nothing else. */
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_EAST,false));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,false));
    assert(SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,SDL_JOYSTICK_AXIS_MIN));
    SDL_UpdateJoysticks();
    SDL_UpdateGamepads();
    assert(pad_read_state(1,&data)==0 && data.buttons==0);

    /* The map button (pad.map, default start, shared with Options). Map off: Start is Options at once. */
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_START,true));
    SDL_UpdateJoysticks();
    SDL_UpdateGamepads();
    assert(pad_read_state(1,&data)==0 && data.buttons==BTN_OPTIONS);
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_START,false));
    SDL_UpdateJoysticks();
    SDL_UpdateGamepads();
    assert(pad_read_state(1,&data)==0 && data.buttons==0);
    /* Map on, a tap: held back while down, the game's Options when let go of (for a moment). */
    map_on=1;
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_START,true));
    SDL_UpdateJoysticks();
    SDL_UpdateGamepads();
    assert(pad_read_state(1,&data)==0 && data.buttons==0);
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_START,false));
    SDL_UpdateJoysticks();
    SDL_UpdateGamepads();
    assert(pad_read_state(1,&data)==0 && data.buttons==BTN_OPTIONS && map_toggles==0);
    usleep(PAD_HOLD_TAP_US+20000);
    assert(pad_read_state(1,&data)==0 && data.buttons==0);
    /* A hold: the map, once, and never Options, also when let go of. */
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_START,true));
    SDL_UpdateJoysticks();
    SDL_UpdateGamepads();
    assert(pad_read_state(1,&data)==0 && data.buttons==0);
    usleep(MAP_HOLD_US+20000);
    assert(pad_read_state(1,&data)==0 && data.buttons==0 && map_toggles==1);
    assert(pad_read_state(1,&data)==0 && data.buttons==0 && map_toggles==1);
    /* The map view holds the game's input; Start still down when it closes is no new press. */
    capture=1;
    assert(pad_read_state(1,&data)==0 && data.buttons==0);
    capture=0;
    usleep(MAP_HOLD_US+20000);
    assert(pad_read_state(1,&data)==0 && data.buttons==0 && map_toggles==1);
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_START,false));
    SDL_UpdateJoysticks();
    SDL_UpdateGamepads();
    assert(pad_read_state(1,&data)==0 && data.buttons==0);
    usleep(PAD_HOLD_TAP_US+20000);
    assert(pad_read_state(1,&data)==0 && data.buttons==0 && map_toggles==1);
    /* A button of its own (paddle1, nothing in the game): a press opens the map at once. */
    pad_hold_reset(&map_hold,0);
    bindings[IN_MAP].pad[0]=SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1;
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1,true));
    SDL_UpdateJoysticks();
    SDL_UpdateGamepads();
    assert(pad_read_state(1,&data)==0 && data.buttons==0 && map_toggles==2);
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1,false));
    SDL_UpdateJoysticks();
    SDL_UpdateGamepads();
    assert(pad_read_state(1,&data)==0 && data.buttons==0 && map_toggles==2);
    map_on=0;

    static const struct { const char *name; int button; } physical[]={
        {"a",SDL_GAMEPAD_BUTTON_SOUTH}, {"b",SDL_GAMEPAD_BUTTON_EAST}, {"x",SDL_GAMEPAD_BUTTON_WEST},
        {"y",SDL_GAMEPAD_BUTTON_NORTH}, {"leftshoulder",SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
        {"rightshoulder",SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER}, {"lefttrigger",PAD_LEFT_TRIGGER},
        {"righttrigger",PAD_RIGHT_TRIGGER}, {"leftstick",SDL_GAMEPAD_BUTTON_LEFT_STICK},
        {"rightstick",SDL_GAMEPAD_BUTTON_RIGHT_STICK}, {"start",SDL_GAMEPAD_BUTTON_START},
        {"back",SDL_GAMEPAD_BUTTON_BACK}, {"dpup",SDL_GAMEPAD_BUTTON_DPAD_UP},
        {"dpdown",SDL_GAMEPAD_BUTTON_DPAD_DOWN}, {"dpleft",SDL_GAMEPAD_BUTTON_DPAD_LEFT},
        {"dpright",SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
    };
    static const int remapped[]={IN_CROSS,IN_CIRCLE,IN_SQUARE,IN_TRIANGLE,IN_L1,IN_R1,IN_L2,IN_R2,IN_L3,IN_R3,
                                 IN_OPTIONS,IN_TOUCHPAD,IN_UP,IN_DOWN,IN_LEFT,IN_RIGHT};
    enum { N=sizeof remapped/sizeof *remapped };
    _Static_assert(sizeof physical/sizeof *physical==N, "one physical button per input");
    char rotated[1024]="";
    for (int i=0;i<N;++i) {
        char line[64];
        snprintf(line,sizeof line,"pad.%s=%s\n",input_names[remapped[i]],physical[(i+1)%N].name);
        strcat(rotated,line);
    }
    char rotated_path[512];
    temp_file(rotated_path,sizeof rotated_path,"pad-rotated",rotated);
    setenv("BB_CONFIG",rotated_path,1);
    load_bindings();
    for (int i=0;i<N;++i) {
        const int p=(i+1)%N, b=physical[p].button;
        const int axis=b==PAD_LEFT_TRIGGER ? SDL_GAMEPAD_AXIS_LEFT_TRIGGER : b==PAD_RIGHT_TRIGGER ? SDL_GAMEPAD_AXIS_RIGHT_TRIGGER : -1;
        if (axis>=0) assert(SDL_SetJoystickVirtualAxis(joystick,axis,32767));
        else assert(SDL_SetJoystickVirtualButton(joystick,b,true));
        SDL_UpdateJoysticks();
        SDL_UpdateGamepads();
        assert(pad_read_state(1,&data)==0);
        if (data.buttons!=input_buttons[remapped[i]])
            printf("pad remap: %s on %s gave buttons 0x%x, wanted 0x%x\n",input_names[remapped[i]],physical[p].name,
                   (unsigned)data.buttons,(unsigned)input_buttons[remapped[i]]);
        assert(data.buttons==input_buttons[remapped[i]]);
        if (axis>=0) assert(SDL_SetJoystickVirtualAxis(joystick,axis,SDL_JOYSTICK_AXIS_MIN));  /* released: the axis minimum */
        else assert(SDL_SetJoystickVirtualButton(joystick,b,false));
        SDL_UpdateJoysticks();
        SDL_UpdateGamepads();
        assert(pad_read_state(1,&data)==0 && data.buttons==0);
    }
    unlink(rotated_path);
    SDL_CloseJoystick(joystick);
    if (gamepad) SDL_CloseGamepad(gamepad);
    gamepad=NULL;
    assert(SDL_DetachVirtualJoystick(id));
    SDL_Quit();
    unlink(path);
    unlink(config);
    puts("PASS: pad ABI, debug camera chord, left/right clicks, SDL touch coordinates, overlay capture, controls, menu buttons, every button remapped at once");
}
