#include "doomgeneric.h"
#include "doomkeys.h"

#include <stdlib.h>
#include <stdint.h>

#include <fs/file.h>
#include <devices/keyboard.h>
#include <syscalls/open.h>
#include <syscalls/read.h>
#include <syscalls/ioctl.h>
#include <syscalls/mapfb.h>
#include <syscalls/femtoseconds.h>
#include <graphics/display.h>
#include <string.h>
#include <stdio.h>

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define TTY_ECHO         (1 << 1)
#define TTY_CANNONICAL   (1 << 2)
#define TTY_NONBLOCK     (1 << 4)

#define TTY_IOCTL_SET_FLAGS  0x5402
#define TTY_FLAGS_DOOM (TTY_NONBLOCK)

static uint32_t* fb0;
static uint32_t fb_pitch_pixels;
static struct fb_info fb_info_local;
static int tty_fd = -1;

static keyboard_event_t event;
static uint64_t start_fs = 0;

static const uint8_t font8x8[15][8] = {
    {0x3C, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x3C},
    {0x08, 0x18, 0x08, 0x08, 0x08, 0x08, 0x08, 0x1C},
    {0x3C, 0x42, 0x02, 0x02, 0x3C, 0x40, 0x40, 0x7E},
    {0x3C, 0x42, 0x02, 0x1C, 0x02, 0x02, 0x42, 0x3C},
    {0x04, 0x0C, 0x14, 0x24, 0x44, 0x7E, 0x04, 0x04},
    {0x7E, 0x40, 0x40, 0x7C, 0x02, 0x02, 0x42, 0x3C},
    {0x3C, 0x40, 0x40, 0x7C, 0x42, 0x42, 0x42, 0x3C},
    {0x7E, 0x02, 0x04, 0x08, 0x10, 0x20, 0x20, 0x20},
    {0x3C, 0x42, 0x42, 0x3C, 0x42, 0x42, 0x42, 0x3C},
    {0x3C, 0x42, 0x42, 0x3E, 0x02, 0x02, 0x42, 0x3C},
    {0x7E, 0x40, 0x40, 0x78, 0x40, 0x40, 0x40, 0x40},
    {0x7C, 0x42, 0x42, 0x7C, 0x40, 0x40, 0x40, 0x40},
    {0x3E, 0x40, 0x40, 0x3C, 0x02, 0x02, 0x02, 0x7C},
    {0x00, 0x18, 0x18, 0x00, 0x00, 0x18, 0x18, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00}
};

static void draw_fps_digit(int x, int y, int digit, uint32_t color) {
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 8; j++) {
            if (font8x8[digit][i] & (1 << (7 - j))) {
                int tx = x + j;
                int ty = y + i;
                if (tx < DOOMGENERIC_RESX && ty < DOOMGENERIC_RESY) {
                    DG_ScreenBuffer[ty * DOOMGENERIC_RESX + tx] = (pixel_t)color;
                }
            }
        }
    }
}

static unsigned char map_key(const keyboard_event_t* ev)
{
    switch (ev->keycode)
    {
        case Escape:         return KEY_ESCAPE;
        case CarriageReturn: return KEY_ENTER;
        case LineFeed:       return KEY_ENTER;
        case FileSeparator:  return KEY_ENTER;
        case GroupSeparator: return KEY_FIRE;
        case HorizontalTab:  return KEY_TAB;
        case Backspace:      return KEY_BACKSPACE;
        case ArrowLeft:      return KEY_LEFTARROW;
        case ArrowRight:     return KEY_RIGHTARROW;
        case ArrowUp:        return KEY_UPARROW;
        case ArrowDown:      return KEY_DOWNARROW;
        case Home:           return KEY_HOME;
        case End:            return KEY_END;
        case Insert:         return KEY_INS;
        case Delete:         return KEY_DEL;
        case PageUp:         return KEY_PGUP;
        case PageDown:       return KEY_PGDN;
        case F1:             return KEY_F1;
        case F2:             return KEY_F2;
        case F3:             return KEY_F3;
        case F4:             return KEY_F4;
        case F5:             return KEY_F5;
        case F6:             return KEY_F6;
        case F7:             return KEY_F7;
        case F8:             return KEY_F8;
        case F9:             return KEY_F9;
        case F10:            return KEY_F10;
        case F11:            return KEY_F11;
        case F12:            return KEY_F12;
        case 42:             return KEY_RSHIFT;
        case 54:             return KEY_RSHIFT;
        default:             break;
    }
    char ascii = tty_key_to_ascii(ev);
    if (ascii == '\n' || ascii == '\r') return KEY_ENTER;
    if (ascii >= 'A' && ascii <= 'Z') ascii = (char)(ascii + ('a' - 'A'));
    return (unsigned char)ascii;
}

void DG_Init(void)
{
    uint32_t flags = TTY_FLAGS_DOOM;
    tty_fd = _open("/dev/tty0", O_RDWR);
    if (tty_fd >= 0) _ioctl(tty_fd, TTY_IOCTL_SET_FLAGS, &flags);
    _ioctl(0, TTY_IOCTL_SET_FLAGS, &flags);

    int framebuffer = _open("/dev/fb0", O_RDWR);
    if (framebuffer < 0) { fb0 = NULL; return; }
    if (_ioctl(framebuffer, FB_IOC_GET_INFO, &fb_info_local) < 0) { fb0 = NULL; return; }

    size_t outpages;
    fb0 = (uint32_t*)mapfb(&outpages);
    fb_pitch_pixels = fb_info_local.pitch >> 2;
    start_fs = _femtoseconds();
    
    // Clear screen once to black
    memset(fb0, 0, fb_info_local.height * fb_info_local.pitch);
}

void DG_DrawFrame(void)
{
    if (!fb0 || !DG_ScreenBuffer) return;

    // --- Rolling Average FPS ---
    #define FPS_AVG_WINDOW 60
    static uint64_t delta_buffer[FPS_AVG_WINDOW] = {0};
    static int delta_idx = 0;
    static uint64_t rolling_sum = 0;
    static int sample_count = 0;
    static uint64_t last_frame_fs = 0;
    uint64_t current_fs_now = _femtoseconds();
    uint32_t fps_scaled = 0;

    if (last_frame_fs != 0) {
        uint64_t delta = current_fs_now - last_frame_fs;
        rolling_sum -= delta_buffer[delta_idx];
        delta_buffer[delta_idx] = delta;
        rolling_sum += delta;
        delta_idx = (delta_idx + 1) % FPS_AVG_WINDOW;
        if (sample_count < FPS_AVG_WINDOW) sample_count++;
        if (rolling_sum > 0) {
            uint64_t avg_delta = rolling_sum / sample_count;
            fps_scaled = (uint32_t)((femtosecondsPerSecond * 10) / avg_delta);
        }
    }
    last_frame_fs = current_fs_now;

    // UI Drawing
    uint32_t fps_int = fps_scaled / 10;
    uint32_t color = 0xFFFFFF;
    draw_fps_digit(5, 5, 10, color);
    draw_fps_digit(13, 5, 11, color);
    draw_fps_digit(21, 5, 12, color);
    color = (fps_int < 20) ? 0xCC0000 : (fps_int < 40 ? 0xCCCC00 : 0x00CC00);
    draw_fps_digit(45, 5, (fps_int / 10) % 10, color);
    draw_fps_digit(53, 5, fps_int % 10, color);

    uint32_t sw = fb_info_local.width;
    uint32_t sh = fb_info_local.height;
    uint32_t rw = (sw > 1280) ? 1280 : sw;
    uint32_t rh = (sh > 800) ? 800 : sh;
    uint32_t ox = (sw > rw) ? (sw - rw) / 2 : 0;
    uint32_t oy = (sh > rh) ? (sh - rh) / 2 : 0;

    static uint32_t* x_map = NULL;
    static uint32_t last_rw = 0;
    
    if (rw != last_rw) {
        if (x_map) free(x_map);
        x_map = (uint32_t*)malloc(rw * sizeof(uint32_t));
        uint32_t x_ratio = (DOOMGENERIC_RESX << 16) / rw;
        for (uint32_t x = 0; x < rw; x++) {
            x_map[x] = (x * x_ratio) >> 16;
        }
        last_rw = rw;
    }

    uint32_t y_ratio = (DOOMGENERIC_RESY << 16) / rh;
    uint32_t src_y_fp = 0;

    static uint32_t* line_buf = NULL;
    static uint32_t last_sw = 0;
    if (sw != last_sw) {
        if (line_buf) free(line_buf);
        line_buf = (uint32_t*)malloc(sw * sizeof(uint32_t));
        last_sw = sw;
    }

    for (uint32_t y = 0; y < rh; y++) {
        uint32_t* dst = &fb0[(y + oy) * fb_pitch_pixels];
        const pixel_t* src_row = &DG_ScreenBuffer[(src_y_fp >> 16) * DOOMGENERIC_RESX];
        src_y_fp += y_ratio;

        for (uint32_t x = 0; x < rw; x++) {
            line_buf[x + ox] = src_row[x_map[x]];
        }
        
        memcpy(dst + ox, line_buf + ox, rw * sizeof(uint32_t));
    }
}

void DG_SleepMs(uint32_t ms) {
    uint64_t target = _femtoseconds() + (uint64_t)ms * femtosecondsPerMillisecond;
    while (_femtoseconds() < target) { }
}

uint32_t DG_GetTicksMs(void) {
    return (uint32_t)((_femtoseconds() - start_fs) / femtosecondsPerMillisecond);
}

int DG_GetKey(int* pressed, unsigned char* doomKey) {
    if (_read(0, &event, sizeof(event)) != (int)sizeof(event)) return 0;
    unsigned char key = map_key(&event);
    if (key == 0) return 0;
    *pressed = (event.action != KEY_RELEASE);
    *doomKey = key;
    return 1;
}

void DG_SetWindowTitle(const char* title) { (void)title; }