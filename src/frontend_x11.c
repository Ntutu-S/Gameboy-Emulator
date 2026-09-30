#include "frontend.h"
#include "keypad.h"
#include "gba.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/keysym.h>

/**
 * Native Linux X11/XShm and ALSA Frontend
 * "Ntutu Advance System"
 */

static void map_key(Frontend *fe, KeySym sym, bool pressed) {
    GBA *gba = fe->gba;
    if (!gba) return;

    switch (sym) {
        case XK_z:
        case XK_x:
            keypad_set_key(&gba->keypad, KEY_A, pressed);
            break;
        case XK_a:
        case XK_s:
            keypad_set_key(&gba->keypad, KEY_B, pressed);
            break;
        case XK_q:
            keypad_set_key(&gba->keypad, KEY_L, pressed);
            break;
        case XK_w:
        case XK_e:
            keypad_set_key(&gba->keypad, KEY_R, pressed);
            break;
        case XK_Return:
            keypad_set_key(&gba->keypad, KEY_START, pressed);
            break;
        case XK_space:
        case XK_BackSpace:
        case XK_Shift_R:
            keypad_set_key(&gba->keypad, KEY_SELECT, pressed);
            break;
        case XK_Up:
            keypad_set_key(&gba->keypad, KEY_UP, pressed);
            break;
        case XK_Down:
            keypad_set_key(&gba->keypad, KEY_DOWN, pressed);
            break;
        case XK_Left:
            keypad_set_key(&gba->keypad, KEY_LEFT, pressed);
            break;
        case XK_Right:
            keypad_set_key(&gba->keypad, KEY_RIGHT, pressed);
            break;
        case XK_Tab:
            if (pressed) {
                fe->fast_forward = !fe->fast_forward; // Toggle fast forward
            }
            break;
        case XK_F11:
        case XK_f:
        case XK_F:
            if (pressed) {
                frontend_toggle_fullscreen(fe);
            }
            break;
    }
}

static bool init_alsa(Frontend *fe) {
    // Silence ALSA internal stderr diagnostic spam
    snd_lib_error_set_handler(NULL);

    int err = snd_pcm_open(&fe->pcm_handle, "default", SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        fprintf(stderr, "[ALSA] Warning: Cannot open audio device: %s\n", snd_strerror(err));
        fe->audio_enabled = false;
        return false;
    }

    snd_pcm_hw_params_t *params;
    snd_pcm_hw_params_alloca(&params);
    snd_pcm_hw_params_any(fe->pcm_handle, params);

    snd_pcm_hw_params_set_access(fe->pcm_handle, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(fe->pcm_handle, params, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(fe->pcm_handle, params, 2);

    unsigned int rate = 44100;
    snd_pcm_hw_params_set_rate_near(fe->pcm_handle, params, &rate, 0);

    unsigned int buffer_time = 100000; // 100ms buffer for smooth playback cushion
    snd_pcm_hw_params_set_buffer_time_near(fe->pcm_handle, params, &buffer_time, 0);

    unsigned int period_time = 20000; // 20ms period
    snd_pcm_hw_params_set_period_time_near(fe->pcm_handle, params, &period_time, 0);

    err = snd_pcm_hw_params(fe->pcm_handle, params);
    if (err < 0) {
        fprintf(stderr, "[ALSA] Cannot set parameters: %s\n", snd_strerror(err));
        snd_pcm_close(fe->pcm_handle);
        fe->audio_enabled = false;
        return false;
    }

    // Prime the buffer with initial silence headroom
    int16_t silence[2048 * 2] = {0};
    snd_pcm_writei(fe->pcm_handle, silence, 2048);

    fe->audio_enabled = true;
    return true;
}

static void frontend_recreate_buffer(Frontend *fe, int width, int height) {
    if (!fe || !fe->display || width <= 0 || height <= 0) return;
    if (fe->ximage && fe->win_width == width && fe->win_height == height) return;

    // Clean up existing image/buffer
    if (fe->use_shm && fe->ximage) {
        XShmDetach(fe->display, &fe->shm_info);
        fe->ximage->data = NULL;
        XDestroyImage(fe->ximage);
        shmdt(fe->shm_info.shmaddr);
        shmctl(fe->shm_info.shmid, IPC_RMID, 0);
        fe->ximage = NULL;
        fe->scaled_pixels = NULL;
        fe->use_shm = false;
    } else if (fe->ximage) {
        XDestroyImage(fe->ximage);
        fe->ximage = NULL;
        fe->scaled_pixels = NULL;
    }

    fe->win_width = width;
    fe->win_height = height;
    fe->cached_render_w = 0;

    // Attempt XShm allocation
    if (XShmQueryExtension(fe->display)) {
        fe->ximage = XShmCreateImage(
            fe->display,
            DefaultVisual(fe->display, fe->screen),
            DefaultDepth(fe->display, fe->screen),
            ZPixmap, NULL, &fe->shm_info,
            fe->win_width, fe->win_height
        );
        if (fe->ximage) {
            fe->shm_info.shmid = shmget(IPC_PRIVATE, fe->ximage->bytes_per_line * fe->ximage->height, IPC_CREAT | 0777);
            if (fe->shm_info.shmid >= 0) {
                fe->shm_info.shmaddr = (char *)shmat(fe->shm_info.shmid, 0, 0);
                fe->ximage->data = fe->shm_info.shmaddr;
                fe->shm_info.readOnly = False;

                if (XShmAttach(fe->display, &fe->shm_info)) {
                    fe->use_shm = true;
                    fe->scaled_pixels = (uint32_t *)fe->ximage->data;
                    memset(fe->scaled_pixels, 0, fe->ximage->bytes_per_line * fe->ximage->height);
                } else {
                    shmdt(fe->shm_info.shmaddr);
                    shmctl(fe->shm_info.shmid, IPC_RMID, 0);
                    fe->ximage->data = NULL;
                    XDestroyImage(fe->ximage);
                    fe->ximage = NULL;
                }
            }
        }
    }

    // Fallback: standard XImage
    if (!fe->use_shm) {
        fe->scaled_pixels = (uint32_t *)calloc(fe->win_width * fe->win_height, sizeof(uint32_t));
        fe->ximage = XCreateImage(
            fe->display,
            DefaultVisual(fe->display, fe->screen),
            DefaultDepth(fe->display, fe->screen),
            ZPixmap, 0, (char *)fe->scaled_pixels,
            fe->win_width, fe->win_height, 32, 0
        );
    }
}

void frontend_toggle_fullscreen(Frontend *fe) {
    if (!fe || !fe->display || !fe->window) return;

    Atom wm_state = XInternAtom(fe->display, "_NET_WM_STATE", False);
    Atom wm_fullscreen = XInternAtom(fe->display, "_NET_WM_STATE_FULLSCREEN", False);

    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = ClientMessage;
    ev.xclient.window = fe->window;
    ev.xclient.message_type = wm_state;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = 2; // _NET_WM_STATE_TOGGLE
    ev.xclient.data.l[1] = wm_fullscreen;
    ev.xclient.data.l[2] = 0;
    ev.xclient.data.l[3] = 1; // source: normal application

    XSendEvent(fe->display, RootWindow(fe->display, fe->screen), False,
               SubstructureNotifyMask | SubstructureRedirectMask, &ev);
    XFlush(fe->display);
    fe->is_fullscreen = !fe->is_fullscreen;
}

bool frontend_init(Frontend *fe, GBA *gba, const char *game_title) {
    if (!fe) return false;
    memset(fe, 0, sizeof(*fe));
    fe->gba = gba;
    clock_gettime(CLOCK_MONOTONIC, &fe->last_frame_time);

    // Initialize ALSA
    init_alsa(fe);

    // Initialize X11 Display
    fe->display = XOpenDisplay(NULL);
    if (!fe->display) {
        fprintf(stderr, "[X11] Warning: Unable to open X display (headless environment)\n");
        return false;
    }

    fe->screen = DefaultScreen(fe->display);
    Window root = RootWindow(fe->display, fe->screen);

    // Create 720x480 window (3x integer scale of 240x160)
    fe->window = XCreateSimpleWindow(
        fe->display, root,
        100, 100, WINDOW_WIDTH, WINDOW_HEIGHT,
        1,
        BlackPixel(fe->display, fe->screen),
        BlackPixel(fe->display, fe->screen)
    );

    // Window title: "Ntutu Advance - [Game Title]"
    char window_title[256];
    snprintf(window_title, sizeof(window_title), "Ntutu Advance - %s", game_title ? game_title : "Ntutu Station");
    XStoreName(fe->display, fe->window, window_title);

    // Window close message
    fe->wm_delete_window = XInternAtom(fe->display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(fe->display, fe->window, &fe->wm_delete_window, 1);

    // Event mask: key presses, releases, window exposure
    XSelectInput(fe->display, fe->window, KeyPressMask | KeyReleaseMask | ExposureMask | StructureNotifyMask);
    XMapWindow(fe->display, fe->window);

    fe->gc = XCreateGC(fe->display, fe->window, 0, NULL);

    // Setup Shared Memory (XShm) or Fallback
    fe->use_shm = false;
    fe->win_width = WINDOW_WIDTH;
    fe->win_height = WINDOW_HEIGHT;
    fe->is_fullscreen = false;
    fe->x_map = NULL;
    fe->cached_render_w = 0;

    frontend_recreate_buffer(fe, WINDOW_WIDTH, WINDOW_HEIGHT);

    printf("[Frontend] X11 initialized (%dx%d, %s)\n", WINDOW_WIDTH, WINDOW_HEIGHT, fe->use_shm ? "XShm zero-copy" : "XPutImage fallback");
    return true;
}

void frontend_cleanup(Frontend *fe) {
    if (!fe) return;
    if (fe->audio_enabled && fe->pcm_handle) {
        snd_pcm_drain(fe->pcm_handle);
        snd_pcm_close(fe->pcm_handle);
        fe->pcm_handle = NULL;
    }
    if (fe->x_map) {
        free(fe->x_map);
        fe->x_map = NULL;
    }
    if (fe->display) {
        if (fe->use_shm && fe->ximage) {
            XShmDetach(fe->display, &fe->shm_info);
            fe->ximage->data = NULL;
            XDestroyImage(fe->ximage);
            shmdt(fe->shm_info.shmaddr);
            shmctl(fe->shm_info.shmid, IPC_RMID, 0);
            fe->ximage = NULL;
            fe->scaled_pixels = NULL;
        } else if (fe->ximage) {
            XDestroyImage(fe->ximage);
            fe->ximage = NULL;
            fe->scaled_pixels = NULL;
        }
        XFreeGC(fe->display, fe->gc);
        XDestroyWindow(fe->display, fe->window);
        XCloseDisplay(fe->display);
        fe->display = NULL;
    }
}

bool frontend_poll_events(Frontend *fe) {
    if (!fe || !fe->display) return true;

    while (XPending(fe->display)) {
        XEvent ev;
        XNextEvent(fe->display, &ev);

        if (ev.type == ClientMessage) {
            if ((Atom)ev.xclient.data.l[0] == fe->wm_delete_window) {
                return false; // Exit request
            }
        } else if (ev.type == ConfigureNotify) {
            if (ev.xconfigure.width > 0 && ev.xconfigure.height > 0 &&
                (ev.xconfigure.width != fe->win_width || ev.xconfigure.height != fe->win_height)) {
                frontend_recreate_buffer(fe, ev.xconfigure.width, ev.xconfigure.height);
            }
        } else if (ev.type == KeyPress || ev.type == KeyRelease) {
            KeySym sym = XLookupKeysym(&ev.xkey, 0);
            map_key(fe, sym, (ev.type == KeyPress));
        }
    }
    return true;
}

void frontend_render_frame(Frontend *fe, const uint32_t *framebuffer) {
    if (!fe || !fe->display || !fe->ximage || !fe->scaled_pixels) return;

    int win_w = fe->win_width;
    int win_h = fe->win_height;
    if (win_w <= 0 || win_h <= 0) return;

    // Aspect-fit dimensions (GBA 3:2 ratio = 240:160)
    int render_w = win_w;
    int render_h = (win_w * 160) / 240;

    if (render_h > win_h) {
        render_h = win_h;
        render_w = (win_h * 240) / 160;
    }

    if (render_w <= 0 || render_h <= 0) return;

    int offset_x = (win_w - render_w) / 2;
    int offset_y = (win_h - render_h) / 2;

    // Cache coordinate lookup table
    if (fe->cached_render_w != render_w || !fe->x_map) {
        free(fe->x_map);
        fe->x_map = (int *)malloc(render_w * sizeof(int));
        if (!fe->x_map) return;
        for (int x = 0; x < render_w; x++) {
            fe->x_map[x] = (x * 240) / render_w;
        }
        fe->cached_render_w = render_w;
    }

    // Scale 240x160 native GBA framebuffer to (render_w x render_h) centered
    for (int y = 0; y < render_h; y++) {
        int src_y = (y * 160) / render_h;
        const uint32_t *src_row = &framebuffer[src_y * 240];
        uint32_t *dst_row = &fe->scaled_pixels[(offset_y + y) * win_w + offset_x];

        for (int x = 0; x < render_w; x++) {
            dst_row[x] = src_row[fe->x_map[x]];
        }
    }

    if (fe->use_shm) {
        XShmPutImage(fe->display, fe->window, fe->gc, fe->ximage, 0, 0, 0, 0, win_w, win_h, False);
    } else {
        XPutImage(fe->display, fe->window, fe->gc, fe->ximage, 0, 0, 0, 0, win_w, win_h);
    }
    XSync(fe->display, False);
}

void frontend_push_audio(Frontend *fe, const int16_t *samples, size_t count) {
    if (!fe || !fe->audio_enabled || !fe->pcm_handle || count == 0) return;

    snd_pcm_sframes_t written = snd_pcm_writei(fe->pcm_handle, samples, count);
    if (written < 0) {
        if (written == -EPIPE) {
            snd_pcm_prepare(fe->pcm_handle);
            snd_pcm_writei(fe->pcm_handle, samples, count);
        } else {
            snd_pcm_recover(fe->pcm_handle, (int)written, 1);
        }
    }
}

void frontend_sync(Frontend *fe) {
    if (!fe) return;

    struct timespec current;
    clock_gettime(CLOCK_MONOTONIC, &current);

    // Target frame time: 16,743,300 nanoseconds (~59.73 Hz)
    long target_ns = fe->fast_forward ? 4185825 : 16743300;

    long elapsed_ns = (current.tv_sec - fe->last_frame_time.tv_sec) * 1000000000L +
                      (current.tv_nsec - fe->last_frame_time.tv_nsec);

    if (elapsed_ns < target_ns) {
        long sleep_ns = target_ns - elapsed_ns;
        struct timespec sleep_req = { 0, sleep_ns };
        nanosleep(&sleep_req, NULL);
    }

    clock_gettime(CLOCK_MONOTONIC, &fe->last_frame_time);
}
