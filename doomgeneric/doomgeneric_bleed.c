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

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define TTY_ECHO        (1 << 1)
#define TTY_CANNONICAL  (1 << 2)
#define TTY_NONBLOCK    (1 << 4)

#define TTY_IOCTL_SET_FLAGS  0x5402
#define TTY_FLAGS_DOOM (TTY_NONBLOCK)

static uint32_t* fb0;
static uint32_t fb_pitch_pixels;
static struct fb_info fb_info_local;
static int tty_fd = -1;

static keyboard_event_t event;
static uint64_t start_fs = 0;

static uint32_t frame_count = 0;
static uint32_t last_fps_time = 0;
static uint32_t current_fps = 0;

static const uint8_t font8x8[14][8] = {
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
    {0x00, 0x18, 0x18, 0x00, 0x00, 0x18, 0x18, 0x00}  
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
    if (tty_fd >= 0) {
        _ioctl(tty_fd, TTY_IOCTL_SET_FLAGS, &flags);
    }

    _ioctl(0, TTY_IOCTL_SET_FLAGS, &flags);

    int framebuffer = _open("/dev/fb0", O_RDWR);
    if (framebuffer < 0) {
        fb0 = NULL;
        return;
    }

    if (_ioctl(framebuffer, FB_IOC_GET_INFO, &fb_info_local) < 0) {
        fb0 = NULL;
        return;
    }

    size_t outpages;
    fb0 = (uint32_t*)mapfb(&outpages);
    if (fb0 == NULL) return;

    fb_pitch_pixels = fb_info_local.pitch >> 2;
    start_fs = _femtoseconds();
}

void DG_DrawFrame(void)
{
    if (!fb0 || !DG_ScreenBuffer) return;

    frame_count++;
    uint32_t currentTime = DG_GetTicksMs();
    if (currentTime - last_fps_time >= 1000) {
        current_fps = frame_count;
        frame_count = 0;
        last_fps_time = currentTime;
    }

    uint32_t text_color = 0xFFFFFFFF;
    const int fps_y_pos = (fb_info_local.height / 2) - 35; // really arbitrary value for just above the player HUD
    const int fps_x_off = 0;
    draw_fps_digit(fps_x_off + 5, fps_y_pos, 10, text_color); 
    draw_fps_digit(fps_x_off + 13, fps_y_pos, 11, text_color);
    draw_fps_digit(fps_x_off + 21, fps_y_pos, 12, text_color);
    draw_fps_digit(fps_x_off + 27, fps_y_pos, 13, text_color);

    int fps = current_fps;
    if (current_fps < 20) text_color = 0xCC0000;
    else if (current_fps > 20 && current_fps < 40) text_color = 0xCCCC00;
    else text_color = 0x00CC00;
    if (fps > 99) draw_fps_digit(fps_x_off + 37, fps_y_pos, (fps / 100) % 10, text_color);
    draw_fps_digit(fps_x_off + 45, fps_y_pos, (fps / 10) % 10, text_color);
    draw_fps_digit(fps_x_off + 53, fps_y_pos, fps % 10, text_color);

    uint32_t dst_w = fb_info_local.width;
    uint32_t dst_h = fb_info_local.height;
    if (dst_w == 0 || dst_h == 0) return;

    for (uint32_t y = 0; y < dst_h; y++)
    {
        uint32_t src_y = (y * DOOMGENERIC_RESY) / dst_h;
        uint32_t* dst = &fb0[y * fb_pitch_pixels];
#ifdef CMAP256
        const uint8_t* src = &DG_ScreenBuffer[src_y * DOOMGENERIC_RESX];
        for (uint32_t x = 0; x < dst_w; x++)
        {
            uint32_t src_x = (x * DOOMGENERIC_RESX) / dst_w;
            uint32_t p = src[src_x];
            dst[x] = (p << 16) | (p << 8) | p; 
        }
#else
        const pixel_t* src = &DG_ScreenBuffer[src_y * DOOMGENERIC_RESX];
        for (uint32_t x = 0; x < dst_w; x++)
        {
            uint32_t src_x = (x * DOOMGENERIC_RESX) / dst_w;
            dst[x] = src[src_x];
        }
#endif
    }
}

void DG_SleepMs(uint32_t ms)
{
    uint32_t target = DG_GetTicksMs() + ms;
    while ((int32_t)(target - DG_GetTicksMs()) > 0) { }
}

uint32_t DG_GetTicksMs(void)
{
    return (uint32_t)((_femtoseconds() - start_fs) / femtosecondsPerMillisecond);
}

int DG_GetKey(int* pressed, unsigned char* doomKey)
{
    if (_read(0, &event, sizeof(event)) != (int)sizeof(event)) return 0;

    unsigned char key = map_key(&event);
    if (key == 0) return 0;

    *pressed = (event.action != KEY_RELEASE);
    *doomKey = key;

    return 1;
}

void DG_SetWindowTitle(const char* title)
{
    (void)title;
}