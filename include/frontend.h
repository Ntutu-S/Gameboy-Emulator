#ifndef FRONTEND_H
#define FRONTEND_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <X11/extensions/XShm.h>
#include <alsa/asoundlib.h>

struct GBA;

#define WINDOW_SCALE 3
#define WINDOW_WIDTH  (240 * WINDOW_SCALE) // 720
#define WINDOW_HEIGHT (160 * WINDOW_SCALE) // 480

typedef struct Frontend {
    // X11 state
    Display *display;
    int screen;
    Window window;
    GC gc;
    Atom wm_delete_window;

    // XShm video
    bool use_shm;
    XShmSegmentInfo shm_info;
    XImage *ximage;
    uint32_t *scaled_pixels;

    // ALSA audio
    snd_pcm_t *pcm_handle;
    bool audio_enabled;

    // Window dimensions & Fullscreen
    int win_width;
    int win_height;
    bool is_fullscreen;
    int *x_map;
    int cached_render_w;

    // Fast forward & frame sync
    bool fast_forward;
    struct timespec last_frame_time;

    struct GBA *gba;
} Frontend;

bool frontend_init(Frontend *fe, struct GBA *gba, const char *game_title);
void frontend_cleanup(Frontend *fe);
bool frontend_poll_events(Frontend *fe);
void frontend_render_frame(Frontend *fe, const uint32_t *framebuffer);
void frontend_push_audio(Frontend *fe, const int16_t *samples, size_t count);
void frontend_sync(Frontend *fe);
void frontend_toggle_fullscreen(Frontend *fe);

#endif // FRONTEND_H
