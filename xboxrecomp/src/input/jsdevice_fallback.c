/*
 * jsdevice_fallback.c -- Linux joystick-API fallback for the SDL backend.
 *
 * Some distros ship SDL2 builds where joystick enumeration comes up empty
 * (no usable udev backend), while the kernel's joystick devices are right
 * there (/dev/input/jsN, evdev nodes tagged ID_INPUT_JOYSTICK). Desktop
 * frontends read those fine; SDL-based ports then see no pad at all.
 *
 * When SDL reports no controllers, xinput_device.c calls this backend: it
 * opens /dev/input/jsN directly (RECOMP_JS_DEVICE=/dev/input/js0 to force
 * a node) and fills XBOX_INPUT_STATE from the standard joy layout:
 * axes 0/1 left stick, 2/3 right stick, 4/5 triggers, 6/7 d-pad hat
 * (what drivers/xpad reports through the js API today).
 */
#include "xinput_xbox.h"

#if !defined(_WIN32) && !defined(__SWITCH__)

#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/ioctl.h>

#ifndef JSIOCGAXES
#define JSIOCGAXES      _IOR('j', 0x11, char)
#endif
#ifndef JSIOCGBUTTONS
#define JSIOCGBUTTONS   _IOR('j', 0x13, char)
#endif
#ifndef JSIOCGNAME
#define JSIOCGNAME(len) _IOC(_IOC_READ, 'j', 0x06, len)
#endif

struct js_event {
    unsigned int time;
    short value;
    unsigned char type, number;
};
#define JS_EVENT_BUTTON 0x01
#define JS_EVENT_AXIS   0x02

#define JS_MAX_AXES 16
#define JS_MAX_BUTTONS 32

static int  s_fd = -1;
static int  s_naxes, s_nbuttons;
static int  s_had_events;               /* events read in the most recent poll */
static short s_axis_val[JS_MAX_AXES];
static unsigned char s_btn_val[JS_MAX_BUTTONS];

/* js button numbers on drivers/xpad ( Xbox Wireless Controller ):
 *   0 A, 1 B, 2 X, 3 Y, 4 LB, 5 RB, 6 back, 7 start, 8 guide,
 *   9 LS, 10 RS */
enum {
    JB_A = 0, JB_B, JB_X, JB_Y, JB_LB, JB_RB, JB_BACK, JB_START,
    JB_GUIDE, JB_LS, JB_RS
};

static int js_open(void)
{
    char buf[32];
    const char *forced = getenv("RECOMP_JS_DEVICE");
    int fd = -1, axes = 8, buttons = 16;
    char name[128];

    if (s_fd >= 0)
        return 1;
    if (forced) {
        fd = open(forced, O_RDONLY | O_NONBLOCK);
    } else {
        int i;
        for (i = 0; i < 4; i++) {
            snprintf(buf, sizeof buf, "/dev/input/js%d", i);
            fd = open(buf, O_RDONLY | O_NONBLOCK);
            if (fd >= 0)
                break;
        }
        if (i >= 4)
            return 0;
    }
    if (fd < 0)
        return 0;
    s_fd = fd;
    if (ioctl(s_fd, JSIOCGAXES, &axes) < 0)
        axes = 8;
    if (ioctl(s_fd, JSIOCGBUTTONS, &buttons) < 0)
        buttons = 16;
    s_naxes    = (axes > JS_MAX_AXES) ? JS_MAX_AXES : axes;
    s_nbuttons = (buttons > JS_MAX_BUTTONS) ? JS_MAX_BUTTONS : buttons;
    memset(s_axis_val, 0, sizeof s_axis_val);
    memset(s_btn_val, 0, sizeof s_btn_val);
    name[0] = 0;
    if (ioctl(s_fd, JSIOCGNAME((int)sizeof name - 1), name) < 0)
        name[0] = 0;
    name[sizeof name - 1] = 0;
    fprintf(stderr, "  [JSFB] pad opened: %s (axes %d, buttons %d)\n",
            name[0] ? name : "(unnamed)", s_naxes, s_nbuttons);
    fflush(stderr);
    return 1;
}

static void js_poll(void)
{
    struct js_event ev;
    s_had_events = 0;
    if (s_fd < 0 && !js_open())
        return;
    for (;;) {
        ssize_t n = read(s_fd, &ev, sizeof ev);
        if (n != (ssize_t)sizeof ev) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                return;             /* drained */
            if (n <= 0) {           /* device vanished; keep last state */
                close(s_fd);
                s_fd = -1;
                return;
            }
        }
        s_had_events = 1;
        if (ev.type & JS_EVENT_BUTTON) {
            if (ev.number < JS_MAX_BUTTONS)
                s_btn_val[ev.number] = ev.value ? 1 : 0;
        } else if (ev.type & JS_EVENT_AXIS) {
            if (ev.number < JS_MAX_AXES)
                s_axis_val[ev.number] = ev.value;   /* -32767..32767 already */
        }
    }
}

int xbox_js_fallback_get_state(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    unsigned char *a;
    WORD btn = 0;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pState)
        return 0;
    if (dwPort != 0)                 /* fallback covers player 1 only */
        return 0;
    if (!js_open())
        return 0;
    js_poll();
    if (s_fd < 0 && !s_had_events)      /* nothing since open, device gone */
        return 0;
    a = pState->Gamepad.bAnalogButtons;
    memset(pState, 0, sizeof *pState);

    if (s_btn_val[JB_A])     a[0] = 255;
    if (s_btn_val[JB_B])     a[1] = 255;
    if (s_btn_val[JB_X])     a[2] = 255;
    if (s_btn_val[JB_Y])     a[3] = 255;
    if (s_btn_val[JB_LB])    a[4] = 255;   /* Black */
    if (s_btn_val[JB_RB])    a[5] = 255;   /* White */
    if (s_btn_val[JB_BACK])  btn |= XBOX_GAMEPAD_BACK;
    if (s_btn_val[JB_START]) btn |= XBOX_GAMEPAD_START;
    if (s_btn_val[JB_LS])    btn |= XBOX_GAMEPAD_LEFT_THUMB;
    if (s_btn_val[JB_RS])    btn |= XBOX_GAMEPAD_RIGHT_THUMB;

    /* In-game pad quit: hold Start+Back ~1 s. Batocera's own exit plumbing
     * (hotkey daemon keystrokes) never reaches a console title — the pads
     * emit BTN codes it doesn't map — so this lives where the pad state
     * already is. RECOMP_QUIT_COMBO=0 disables. */
    if (s_btn_val[JB_BACK] && s_btn_val[JB_START]) {
        static const char *en = (const char *)1;
        static unsigned long t0, checked;
        if (en == (const char *)1) {
            const char *e = getenv("RECOMP_QUIT_COMBO");
            en = (e && e[0] == '0') ? NULL : "";
        }
        {
            unsigned long now = (unsigned long)GetTickCount();
            if (en)
                t0 = (t0 && now - checked < 3000) ? t0 : now, checked = now;
            if (en && now - t0 >= 1000) {
                t0 = 0;
                fprintf(stderr, "[quit] Start+Back held -> exiting\n");
                fflush(stderr);
                exit(0);
            }
        }
    }

    if (s_naxes >= 6) {
        /* standard xpad js layout: 0 LX, 1 LY, 2 RX, 3 RY, 4 LT, 5 RT */
        pState->Gamepad.sThumbLX = s_axis_val[0];
        /* js Y points down; the Xbox Y axis points up */
        pState->Gamepad.sThumbLY = (s_axis_val[1] == -32767) ? 32767 :
                                   (short)-s_axis_val[1];
        pState->Gamepad.sThumbRX = s_axis_val[2];
        pState->Gamepad.sThumbRY = (s_axis_val[3] == -32767) ? 32767 :
                                   (short)-s_axis_val[3];
        a[6] = (unsigned char)((s_axis_val[4] + 32767) / 257);
        a[7] = (unsigned char)((s_axis_val[5] + 32767) / 257);
    } else {
        pState->Gamepad.sThumbLX = s_axis_val[0];
        pState->Gamepad.sThumbLY = (s_axis_val[1] == -32767) ? 32767 :
                                   (short)-s_axis_val[1];
    }

    /* d-pad hat on axes 6/7 (xpad reports it there, +-1) */
    if (s_naxes > 7 &&
        (s_axis_val[6] || s_axis_val[7])) {
        if (s_axis_val[6] == -1) btn |= XBOX_GAMEPAD_DPAD_LEFT;
        if (s_axis_val[6] ==  1) btn |= XBOX_GAMEPAD_DPAD_RIGHT;
        if (s_axis_val[7] == -1) btn |= XBOX_GAMEPAD_DPAD_UP;
        if (s_axis_val[7] ==  1) btn |= XBOX_GAMEPAD_DPAD_DOWN;
    }

    pState->Gamepad.wButtons = btn;
    pState->dwPacketNumber++;
    return 1;
}

void xbox_js_fallback_close(void)
{
    if (s_fd >= 0) {
        close(s_fd);
        s_fd = -1;
    }
}

int xbox_js_fallback_probe(void)
{
    return js_open();
}

#endif /* !_WIN32 && !__SWITCH__ */