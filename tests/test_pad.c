#define _GNU_SOURCE
#include <assert.h>
#include <unistd.h>
#include "../src/runtime_pad.c"

static int capture;
static BbMouseState host_mouse={.focused=1};
int bbgpu_overlay_captures_input(void) { return capture; }
void bbgpu_mouse_state(BbMouseState *state) { *state=host_mouse; }
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
    char path[]="/tmp/bbport-pad-test-XXXXXX";
    int fd=mkstemp(path);
    assert(fd>=0);
    close(fd);
    setenv("BB_PAD_FILE",path,1);
    /* bbport.ini controls: buttons moved, a trigger as a button and a button as a trigger. */
    char config[]="/tmp/bbport-pad-config-XXXXXX";
    int config_fd=mkstemp(config);
    assert(config_fd>=0);
    const char controls[]="upscaler=fsr3\npad.cross=b\npad.circle=a\npad.r2=rightshoulder\n"
                          "pad.r1=righttrigger\nkey.cross=X, Space\npad.bogus=a\n"
                          "key.r1=Mouse Left, 3\nkey.r2=Mouse Right\nkey.r3=Mouse Middle\n"
                          "key.l2=Mouse X1\nkey.circle=Mouse X2, Left Shift\nmouse_sensitivity=0.10\n";
    assert(write(config_fd,controls,sizeof(controls)-1)==(ssize_t)(sizeof(controls)-1));
    close(config_fd);
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

    /* Android bridge: partial analog triggers, clamping, digital buttons, then release. */
    inject(path,"lx=0 ry=255 l2=127 r2=20");
    assert(pad_read_state(1,&data)==0 && data.left_x==0 && data.right_y==255);
    assert(data.l2==127 && data.r2==20 && data.buttons==BTN_L2);
    inject(path,"l2=-10 r2=999");
    assert(pad_read_state(1,&data)==0 && data.l2==0 && data.r2==255 && data.buttons==BTN_R2);
    inject(path,"l2 l2=0");
    assert(pad_read_state(1,&data)==0 && data.l2==255 && data.buttons==BTN_L2);
    inject(path,"");
    assert(pad_read_state(1,&data)==0 && data.buttons==0 && data.l2==0 && data.r2==0);

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
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_EAST,true));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,true));
    assert(SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,32767));
    SDL_UpdateJoysticks();
    SDL_UpdateGamepads();
    assert(pad_read_state(1,&data)==0);
    assert(data.buttons==(BTN_CROSS|BTN_R2|BTN_R1) && data.r2==255);
    assert(bindings[IN_CROSS].key_count==2 && bindings[IN_CROSS].keys[0]==SDL_SCANCODE_X &&
           bindings[IN_CROSS].keys[1]==SDL_SCANCODE_SPACE);
    /* Release the controller; exercise mouse buttons, camera, repeated reads and focus. */
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_EAST,false));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,false));
    assert(SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,-32768));
    SDL_UpdateJoysticks();
    SDL_UpdateGamepads();
    host_mouse=(BbMouseState){.buttons=SDL_BUTTON_LMASK|SDL_BUTTON_RMASK|SDL_BUTTON_MMASK|
                                        SDL_BUTTON_X1MASK|SDL_BUTTON_X2MASK,
                             .x=500,.y=-500,.focused=1};
    assert(pad_read_state(1,&data)==0);
    assert(data.buttons==(BTN_R1|BTN_R2|BTN_R3|BTN_L2|BTN_CIRCLE));
    assert(data.l2==255 && data.r2==255 && data.right_x>128 && data.right_y<128);
    PadData again;
    assert(pad_read_state(1,&again)==0 && again.right_x==data.right_x && again.right_y==data.right_y);
    mouse_invert_y=1;
    assert(pad_read_state(1,&data)==0 && data.right_y>128);
    mouse_invert_y=0;
    host_mouse.x=1e9f; host_mouse.y=-1e9f;
    assert(pad_read_state(1,&data)==0 && data.right_x==255 && data.right_y==1);
    capture=1;
    assert(pad_read_state(1,&data)==0 && data.buttons==0 && data.right_x==128 && data.right_y==128);
    capture=0;
    host_mouse.focused=0;
    assert(pad_read_state(1,&data)==0 && data.buttons==0 && data.right_x==128 && data.right_y==128);
    host_mouse=(BbMouseState){.focused=1};
    assert(pad_read_state(1,&data)==0 && data.buttons==0 && data.right_x==128 && data.right_y==128);
    bool keys[SDL_SCANCODE_COUNT]={0};
    keys[SDL_SCANCODE_W]=keys[SDL_SCANCODE_D]=keys[SDL_SCANCODE_SPACE]=true;
    apply_keyboard(&data,keys,0);
    assert(data.left_y==0 && data.left_x==255 && (data.buttons & BTN_CROSS));
    SDL_CloseJoystick(joystick);
    if (gamepad) SDL_CloseGamepad(gamepad);
    gamepad=NULL;
    assert(SDL_DetachVirtualJoystick(id));
    SDL_Quit();
    unlink(path);
    unlink(config);
    puts("PASS: pad ABI, touchpad, controller, keyboard/mouse bindings, camera, overlay and focus suppression");
}
