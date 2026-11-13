#include <SDL.h>
#include <SDL_messagebox.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <time.h>
#include <sys/param.h>
/* This #undef is needed because a SDL include header redefines HAVE_STDARG_H. */
#undef HAVE_STDARG_H
#define HAVE_STDARG_H
#include <86box/86box.h>
#include <86box/device.h>
#include <86box/io.h>
#include <86box/machine.h>
#include <86box/plat.h>
#include <86box/plat_dynld.h>
#include <86box/sound.h>
#include <86box/video.h>
#include <86box/vid_lcd.h>
#include <86box/ui.h>
#include <86box/version.h>
#include <86box/ini.h>
#include <86box/config.h>

#include "sdl_render.h"
#include "sdl_osd.h"
#include "sdl_shader.h"

#define RENDERER_FULL_SCREEN 1
#define RENDERER_HARDWARE    2
#define RENDERER_OPENGL      4

typedef struct sdl_blit_params {
    int x;
    int y;
    int w;
    int h;
} sdl_blit_params;
extern sdl_blit_params params;
extern int             blitreq;

SDL_Window         *sdl_win     = NULL;
SDL_Renderer       *sdl_render  = NULL;
#ifndef USE_SDL_SHADER_PIPELINE
static SDL_Texture *sdl_tex     = NULL;
static SDL_Texture *sdl_osd_tex = NULL;
#endif
int                 sdl_w       = SCREEN_RES_X;
int                 sdl_h       = SCREEN_RES_Y;
static int          sdl_fs;
static int          sdl_flags   = -1;
static int          cur_w;
static int          cur_h;
static int          cur_wx      = 0;
static int          cur_wy      = 0;
static int          cur_ww      = 0;
static int          cur_wh      = 0;
static volatile int sdl_enabled = 1;
SDL_mutex          *sdl_mutex   = NULL;
int                 mouse_capture;
int                 title_set         = 0;
int                 resize_pending    = 0;
int                 resize_w          = 0;
int                 resize_h          = 0;
static void        *pixeldata         = NULL;

typedef struct tp700_panel_controls {
    int      baseline_set;
    int      brightness;
    int      contrast;
    int      invert;
    int      volume_mode;
    int      baseline_brightness;
    int      baseline_contrast;
    int      baseline_volume_mode;
    int      original_sound_muted;
    int      original_sound_gain;
    int      sound_saved;
    uint8_t  lut[256];
    uint32_t last_poll;
    uint32_t *pixels;
    size_t   pixel_capacity;
} tp700_panel_controls;

static tp700_panel_controls tp700_controls;

enum {
    TP700_VOLUME_MUTE = 0,
    TP700_VOLUME_MIDDLE,
    TP700_VOLUME_MAXIMUM
};

static int
tp700_status_fresh(const char *path)
{
    struct stat status;
    time_t      now = time(NULL);

    return stat(path, &status) == 0 && now >= status.st_mtime &&
           now - status.st_mtime <= 2;
}

static int
tp700_read_status(const char *path, char *buffer, size_t size)
{
    FILE  *stream;
    size_t count;

    if (!tp700_status_fresh(path) || size < 2)
        return 0;
    stream = fopen(path, "r");
    if (stream == NULL)
        return 0;
    count = fread(buffer, 1, size - 1, stream);
    fclose(stream);
    buffer[count] = '\0';
    return count > 0;
}

static const char *
tp700_json_value(const char *json, const char *key)
{
    const char *value = strstr(json, key);

    if (value == NULL)
        return NULL;
    value = strchr(value + strlen(key), ':');
    if (value == NULL)
        return NULL;
    do {
        ++value;
    } while (*value != '\0' && isspace((unsigned char) *value));
    return value;
}

static int
tp700_json_int(const char *json, const char *key, int *result)
{
    const char *value = tp700_json_value(json, key);
    char       *end;
    long        parsed;

    if (value == NULL || !isdigit((unsigned char) *value))
        return 0;
    parsed = strtol(value, &end, 10);
    if (end == value)
        return 0;
    *result = (int) parsed;
    return 1;
}

static int
tp700_json_bool(const char *json, const char *key, int *result)
{
    const char *value = tp700_json_value(json, key);

    if (value == NULL)
        return 0;
    if (!strncmp(value, "true", 4)) {
        *result = 1;
        return 1;
    }
    if (!strncmp(value, "false", 5)) {
        *result = 0;
        return 1;
    }
    return 0;
}

static int
tp700_slider_value(const char *json, const char *section_name, int *result)
{
    const char *section = strstr(json, section_name);

    return section != NULL && tp700_json_int(section, "\"filtered_percent\"", result);
}

static int
tp700_volume_value(const char *json, int *result)
{
    const char *value = tp700_json_value(json, "\"volume\"");

    if (value == NULL)
        return 0;
    if (!strncmp(value, "\"mute\"", 6))
        *result = TP700_VOLUME_MUTE;
    else if (!strncmp(value, "\"middle\"", 8))
        *result = TP700_VOLUME_MIDDLE;
    else if (!strncmp(value, "\"maximum\"", 9))
        *result = TP700_VOLUME_MAXIMUM;
    else
        return 0;
    return 1;
}

static int
tp700_volume_db(int mode)
{
    return mode == TP700_VOLUME_MIDDLE ? -6 : 0;
}

static void
tp700_controls_build_lut(void)
{
    double brightness_gain;
    double contrast_factor;

    brightness_gain = (double) MAX(tp700_controls.brightness, 1) /
                      (double) MAX(tp700_controls.baseline_brightness, 1);
    contrast_factor = 1.0 +
                      (double) (tp700_controls.contrast -
                                tp700_controls.baseline_contrast) / 50.0;
    if (contrast_factor < 0.25)
        contrast_factor = 0.25;
    if (contrast_factor > 3.0)
        contrast_factor = 3.0;

    for (int index = 0; index < 256; ++index) {
        double value = (double) index / 255.0;
        int    mapped;

        if (tp700_controls.invert)
            value = 1.0 - value;
        value = (value - 0.5) * contrast_factor + 0.5;
        value *= brightness_gain;
        mapped = (int) (value * 255.0 + 0.5);
        tp700_controls.lut[index] = (uint8_t) MIN(255, MAX(0, mapped));
    }
}

static void
tp700_controls_poll(void)
{
    const char *slider_path;
    const char *switch_path;
    char        slider_json[4096];
    char        switch_json[2048];
    uint32_t    now = SDL_GetTicks();
    int         host_controls_enabled;
    int         brightness;
    int         contrast;
    int         invert;
    int         volume_mode;
    int         changed;
    int         volume_changed;

    if (!machine_is_thinkpad_700())
        return;
    if (tp700_controls.last_poll != 0 &&
        (uint32_t) (now - tp700_controls.last_poll) < 100)
        return;
    tp700_controls.last_poll = now;

    slider_path = getenv("TP700_SLIDER_STATUS");
    switch_path = getenv("TP700_SWITCH_STATUS");
    if (slider_path == NULL)
        slider_path = "/run/thinkpad700c-leds/slider-status.json";
    if (switch_path == NULL)
        switch_path = "/run/thinkpad700c-leds/switch-status.json";
    if (!tp700_read_status(slider_path, slider_json, sizeof(slider_json)) ||
        !tp700_read_status(switch_path, switch_json, sizeof(switch_json)) ||
        !tp700_json_bool(slider_json, "\"host_controls_enabled\"",
                         &host_controls_enabled) ||
        host_controls_enabled ||
        !tp700_json_bool(switch_json, "\"host_controls_enabled\"",
                         &host_controls_enabled) ||
        host_controls_enabled ||
        !tp700_slider_value(slider_json, "\"brightness\"", &brightness) ||
        !tp700_slider_value(slider_json, "\"contrast\"", &contrast) ||
        !tp700_json_int(switch_json, "\"invert\"", &invert) ||
        !tp700_volume_value(switch_json, &volume_mode))
        return;

    brightness = MIN(100, MAX(1, brightness));
    contrast   = MIN(100, MAX(1, contrast));
    invert     = !!invert;

    if (!tp700_controls.baseline_set) {
        tp700_controls.baseline_set        = 1;
        tp700_controls.baseline_brightness = brightness;
        tp700_controls.baseline_contrast   = contrast;
        tp700_controls.baseline_volume_mode = volume_mode;
        tp700_controls.original_sound_muted = sound_muted;
        tp700_controls.original_sound_gain  = sound_gain;
        tp700_controls.sound_saved          = 1;
    }

    changed = brightness != tp700_controls.brightness ||
              contrast != tp700_controls.contrast ||
              invert != tp700_controls.invert;
    volume_changed = volume_mode != tp700_controls.volume_mode;
    tp700_controls.brightness  = brightness;
    tp700_controls.contrast    = contrast;
    tp700_controls.invert      = invert;
    tp700_controls.volume_mode = volume_mode;
    if (changed)
        tp700_controls_build_lut();

    if (volume_mode == TP700_VOLUME_MUTE) {
        sound_muted = 1;
    } else {
        sound_muted = 0;
        sound_gain = tp700_controls.original_sound_gain +
                     tp700_volume_db(volume_mode) -
                     tp700_volume_db(tp700_controls.baseline_volume_mode);
    }
    if (changed || volume_changed)
        pclog("TP700 PANEL: brightness=%d base=%d contrast=%d base=%d "
              "invert=%d volume=%d muted=%d gain=%d (guest-only)\n",
              brightness, tp700_controls.baseline_brightness,
              contrast, tp700_controls.baseline_contrast,
              invert, volume_mode, sound_muted, sound_gain);
}

static const void *
tp700_controls_transform(const void *source, int width, int height)
{
    const uint32_t *input = (const uint32_t *) source;
    size_t          needed;
    uint8_t         lut[256];
    uint8_t         keyboard_lut[256];

    tp700_controls_poll();
    const int physical_active = tp700_controls.baseline_set &&
        (tp700_controls.invert ||
         tp700_controls.brightness != tp700_controls.baseline_brightness ||
         tp700_controls.contrast != tp700_controls.baseline_contrast);
    const int keyboard_active = machine_is_thinkpad_700() &&
        (tp700_lcd_brightness != 100 || tp700_lcd_contrast != 100);
    if (!physical_active && !keyboard_active)
        return source;

    tp700_lcd_build_lut(keyboard_lut, keyboard_active ? tp700_lcd_brightness : 100,
                       keyboard_active ? tp700_lcd_contrast : 100);
    for (int i = 0; i < 256; ++i)
        lut[i] = keyboard_lut[physical_active ? tp700_controls.lut[i] : i];

    needed = (size_t) 2048 * (size_t) height;
    if (needed > tp700_controls.pixel_capacity) {
        uint32_t *replacement = realloc(tp700_controls.pixels,
                                        needed * sizeof(uint32_t));
        if (replacement == NULL)
            return source;
        tp700_controls.pixels        = replacement;
        tp700_controls.pixel_capacity = needed;
    }

    for (int row = 0; row < height; ++row) {
        const uint32_t *src = input + row * 2048;
        uint32_t       *dst = tp700_controls.pixels + row * 2048;

        for (int column = 0; column < width; ++column) {
            dst[column] = tp700_lcd_pixel(src[column], lut);
        }
    }
    return tp700_controls.pixels;
}

void
tp700_panel_controls_close(void)
{
    if (tp700_controls.sound_saved) {
        sound_muted = tp700_controls.original_sound_muted;
        sound_gain  = tp700_controls.original_sound_gain;
    }
    free(tp700_controls.pixels);
    tp700_controls.pixels        = NULL;
    tp700_controls.pixel_capacity = 0;
}

void sdl_reinit_texture(void);

static void
sdl_integer_scale(double *d, double *g)
{
    double ratio;

    if (*d > *g) {
        ratio = floor(*d / *g);
        *d    = *g * ratio;
    } else {
        ratio = ceil(*d / *g);
        *d    = *g / ratio;
    }
}

static void
sdl_get_output_size(int *w, int *h)
{
#ifdef USE_SDL_SHADER_PIPELINE
    SDL_GL_GetDrawableSize(sdl_win, w, h);
#else
    if (sdl_render && SDL_GetRendererOutputSize(sdl_render, w, h) == 0)
        return;
    SDL_GetWindowSize(sdl_win, w, h);
#endif
}

static void
sdl_stretch(int *w, int *h, int *x, int *y)
{
    double hw;
    double gw;
    double hh;
    double gh;
    double dx;
    double dy;
    double dw;
    double dh;
    double gsr;
    double hsr;
    int    real_sdl_w;
    int    real_sdl_h;
    int    scale_mode;

    sdl_get_output_size(&real_sdl_w, &real_sdl_h);

    hw  = (double) real_sdl_w;
    hh  = (double) real_sdl_h;
    gw  = (double) *w;
    gh  = (double) *h;
    hsr = hw / hh;
    scale_mode = video_gl_input_scale_mode;

    switch (scale_mode) {
        case FULLSCR_SCALE_FULL:
        default:
            *w = real_sdl_w;
            *h = real_sdl_h;
            *x = 0;
            *y = 0;
            break;
        case FULLSCR_SCALE_43:
        case FULLSCR_SCALE_KEEPRATIO:
            if (scale_mode == FULLSCR_SCALE_43)
                gsr = 4.0 / 3.0;
            else
                gsr = gw / gh;
            if (gsr <= hsr) {
                dw = hh * gsr;
                dh = hh;
            } else {
                dw = hw;
                dh = hw / gsr;
            }
            dx = (hw - dw) / 2.0;
            dy = (hh - dh) / 2.0;
            *w = (int) dw;
            *h = (int) dh;
            *x = (int) dx;
            *y = (int) dy;
            break;
        case FULLSCR_SCALE_INT:
        case FULLSCR_SCALE_INT43:
            if (scale_mode == FULLSCR_SCALE_INT43)
                gsr = 4.0 / 3.0;
            else
                gsr = gw / gh;
            if (gsr <= hsr) {
                dw = hh * gsr;
                dh = hh;
            } else {
                dw = hw;
                dh = hw / gsr;
            }
            sdl_integer_scale(&dw, &gw);
            sdl_integer_scale(&dh, &gh);
            dx = (hw - dw) / 2.0;
            dy = (hh - dh) / 2.0;
            *w = (int) dw;
            *h = (int) dh;
            *x = (int) dx;
            *y = (int) dy;
            break;
    }
}

static int
sdl_blit_invalid(int x, int y, int w, int h)
{
    if (!sdl_enabled || (x < 0) || (y < 0) || (w <= 0) || (h <= 0) || (w > 2048) || (h > 2048) || (buffer32 == NULL))
        return 1;

#ifdef USE_SDL_SHADER_PIPELINE
    return !sdl_shader_active();
#else
    return (sdl_render == NULL) || (sdl_tex == NULL);
#endif
}

void
sdl_blit_shim(int x, int y, int w, int h, int monitor_index)
{
    params.x = x;
    params.y = y;
    params.w = w;
    params.h = h;

    if (!sdl_blit_invalid(x, y, w, h) || (monitor_index >= 1))
        for (int row = 0; row < h; ++row)
            video_copy(&(((uint8_t *) pixeldata)[row * 2048 * sizeof(uint32_t)]), &(buffer32->line[y + row][x]), w * sizeof(uint32_t));

    if (monitors[monitor_index].mon_screenshots_raw)
        video_screenshot((uint32_t *) pixeldata, 0, 0, 2048);
    blitreq = 1;

    video_blit_complete_monitor(monitor_index);
}

void ui_window_title_real(void);

void
sdl_real_blit(SDL_Rect *r_src)
{
    SDL_Rect r_dst;
    int      winx;
    int      winy;
    const int tp700 = machine_is_thinkpad_700();
    const int monochrome = !strcmp(machine_get_internal_name(), "ibmtp700");
    char expansion_option[] = "lcd_vertical_expansion";
    const int vertical_expansion = tp700 && !monochrome &&
                                   machine_get_config_int(expansion_option);

    /* VGA briefly reports the 80 text columns as an 80-pixel frame while a
       new text timing is being latched.  It is not a complete scanout; keep
       the last valid panel image instead of magnifying that one-frame mode
       transition across the LCD. */
    if (tp700 && (r_src->w < 320))
        return;

    sdl_get_output_size(&winx, &winy);

    r_dst   = *r_src;
    r_dst.x = r_dst.y = 0;

    if (tp700) {
        const int post_memory = tp700_post_memory_display_active();

        /*
         * The 9552 always has one 640x480 physical LCD canvas.  Its VGA
         * sources do not all have the same height: POST/DOS text is normally
         * delivered as 400 lines and is centred on that canvas, while the
         * Reference Diskette and Windows also use true 480-line frames.
         *
         * Treating every source as 400 lines made a 480-line menu shrink a
         * second time; resizing the host window from the current source then
         * made the apparent bezel move at each mode set.  Establish the panel
         * rectangle first, and only letterbox sources which really are below
         * 480 lines.  The panel mapping is independent of the POST overlay
         * state, so its left and right edges cannot jump between stages.
         */
        if (sdl_fs) {
            r_dst.w = winx;
            r_dst.h = (winx * 3) / 4;
            if (r_dst.h > winy) {
                r_dst.h = winy;
                r_dst.w = (winy * 4) / 3;
            }
            r_dst.x = (winx - r_dst.w) / 2;
            r_dst.y = (winy - r_dst.h) / 2;
        } else {
            r_dst.w = winx;
            r_dst.h = winy;
        }

        /* Without LCD vertical expansion, preserve the source row height
           within the fixed 480-line panel, including the 700C Reference
           Diskette's 400-line text mode. */
        if (!vertical_expansion && (r_src->h < 480)) {
            int active_height = (r_dst.h * r_src->h) / 480;

            if (active_height < 1)
                active_height = 1;
            r_dst.y += (r_dst.h - active_height) / 2;
            r_dst.h = active_height;
        }

        /*
         * Every 80-column screen begins at VGA column zero, while the real
         * LCD has roughly one and a half character cells hidden behind its
         * left safe edge.  Keep the same text safe area for the complete
         * lifetime of every 400-line mode.  In particular, do not rescale the
         * picture when the POST memory checkpoint state changes halfway
         * through the count.  True 640x480 Windows graphics remains
         * pixel-for-pixel.
         */
        if (monochrome && (r_src->h == 400) &&
            ((r_src->w == 640) || (r_src->w == 720))) {
            int text_margin = r_dst.w / 80;
            int left_margin;

            if (text_margin < 1)
                text_margin = 1;
            left_margin = text_margin + text_margin / 2;
            if (r_dst.w > left_margin + text_margin) {
                r_dst.x += left_margin;
                r_dst.w -= left_margin + text_margin;
            }
        }

        {
            static int last_src_w = -1;
            static int last_src_h = -1;
            static int last_out_w = -1;
            static int last_out_h = -1;
            static int last_dst_x = -1;
            static int last_dst_y = -1;
            static int last_dst_w = -1;
            static int last_dst_h = -1;
            static int last_fs    = -1;
            static int last_post  = -1;

            if ((last_src_w != r_src->w) || (last_src_h != r_src->h) ||
                (last_out_w != winx) || (last_out_h != winy) ||
                (last_dst_x != r_dst.x) || (last_dst_y != r_dst.y) ||
                (last_dst_w != r_dst.w) || (last_dst_h != r_dst.h) ||
                (last_fs != sdl_fs) || (last_post != post_memory)) {
                pclog("TP700 LCD MAP: source=%dx%d output=%dx%d "
                      "destination=%d,%d %dx%d fullscreen=%d post=%d\n",
                      r_src->w, r_src->h, winx, winy, r_dst.x, r_dst.y,
                      r_dst.w, r_dst.h, sdl_fs,
                      post_memory);
                last_src_w = r_src->w;
                last_src_h = r_src->h;
                last_out_w = winx;
                last_out_h = winy;
                last_dst_x = r_dst.x;
                last_dst_y = r_dst.y;
                last_dst_w = r_dst.w;
                last_dst_h = r_dst.h;
                last_fs    = sdl_fs;
                last_post  = post_memory;
            }
        }
    } else if (sdl_fs) {
        sdl_stretch(&r_dst.w, &r_dst.h, &r_dst.x, &r_dst.y);
    } else {
        r_dst.w *= ((float) winx / (float) r_dst.w);
        r_dst.h *= ((float) winy / (float) r_dst.h);
    }

#ifdef USE_SDL_SHADER_PIPELINE
    sdl_shader_blit(sdl_win, pixeldata, r_src->w, r_src->h,
                    r_dst.x, r_dst.y, r_dst.w, r_dst.h);
#else
    SDL_Rect src_rect = { 0, 0, r_src->w, r_src->h };
    const void *texture_pixels;

    if (sdl_render == NULL || sdl_tex == NULL || sdl_osd_tex == NULL)
        return;

    texture_pixels = tp700_controls_transform(pixeldata, r_src->w, r_src->h);
    if (SDL_UpdateTexture(sdl_tex, &src_rect, texture_pixels,
                          2048 * (int) sizeof(uint32_t)) < 0)
        return;

    SDL_SetRenderDrawColor(sdl_render, 0, 0, 0, 255);
    SDL_RenderClear(sdl_render);
    SDL_RenderCopy(sdl_render, sdl_tex, &src_rect, &r_dst);

    osd_present(r_src->w, r_src->h);

    if (osd_is_visible()) {
        SDL_Surface *osd_surface = osd_get_surface();

        if (osd_surface && osd_surface->w == r_src->w && osd_surface->h == r_src->h) {
            if (SDL_UpdateTexture(sdl_osd_tex, &src_rect, osd_surface->pixels, osd_surface->pitch) == 0)
                SDL_RenderCopy(sdl_render, sdl_osd_tex, &src_rect, &r_dst);
        }
    }

    SDL_RenderPresent(sdl_render);
#endif
}

void
sdl_blit(int x, int y, int w, int h)
{
    SDL_Rect r_src;

    if (sdl_blit_invalid(x, y, w, h)) {
        r_src.x = x;
        r_src.y = y;
        r_src.w = w;
        r_src.h = h;
        sdl_real_blit(&r_src);
        blitreq = 0;
        return;
    }

    SDL_LockMutex(sdl_mutex);

    if (resize_pending) {
        if (!video_fullscreen)
            sdl_resize(resize_w, resize_h);
        resize_pending = 0;
    }

    r_src.x = x;
    r_src.y = y;
    r_src.w = w;
    r_src.h = h;
    blitreq = 0;

    sdl_real_blit(&r_src);
    SDL_UnlockMutex(sdl_mutex);
}

static void
sdl_destroy_window(void)
{
    if (sdl_win != NULL) {
        if (window_remember) {
            SDL_GetWindowSize(sdl_win, &window_w, &window_h);
            if (strncasecmp(SDL_GetCurrentVideoDriver(), "wayland", 7) != 0) {
                SDL_GetWindowPosition(sdl_win, &window_x, &window_y);
            }
        }
        SDL_DestroyWindow(sdl_win);
        sdl_win = NULL;
    }
}

static void
sdl_destroy_texture(void)
{
#ifdef USE_SDL_SHADER_PIPELINE
    sdl_shader_close();
#else
    if (sdl_osd_tex != NULL) {
        SDL_DestroyTexture(sdl_osd_tex);
        sdl_osd_tex = NULL;
    }
    if (sdl_tex != NULL) {
        SDL_DestroyTexture(sdl_tex);
        sdl_tex = NULL;
    }
    if (sdl_render != NULL) {
        SDL_DestroyRenderer(sdl_render);
        sdl_render = NULL;
    }
#endif
}

void
sdl_close(void)
{
    if (sdl_mutex != NULL)
        SDL_LockMutex(sdl_mutex);

    /* Unregister our renderer! */
    video_setblit(NULL);

    osd_deinit();

    if (sdl_enabled)
        sdl_enabled = 0;

    if (sdl_mutex != NULL) {
        SDL_DestroyMutex(sdl_mutex);
        sdl_mutex = NULL;
    }

    sdl_destroy_texture();
    sdl_destroy_window();

    if (pixeldata != NULL) {
        free(pixeldata);
        pixeldata = NULL;
    }

    /* Quit. */
    SDL_Quit();
    sdl_flags = -1;
}

void
sdl_enable(int enable)
{
    if (sdl_flags == -1)
        return;

    SDL_LockMutex(sdl_mutex);
    sdl_enabled = !!enable;

    if (enable == 1) {
        SDL_SetWindowSize(sdl_win, cur_ww, cur_wh);
        sdl_reinit_texture();
    }

    SDL_UnlockMutex(sdl_mutex);
}

static void
sdl_select_best_hw_driver(void)
{
    SDL_RendererInfo renderInfo;

    for (int i = 0; i < SDL_GetNumRenderDrivers(); ++i) {
        SDL_GetRenderDriverInfo(i, &renderInfo);
        if (renderInfo.flags & SDL_RENDERER_ACCELERATED) {
            SDL_SetHint(SDL_HINT_RENDER_DRIVER, renderInfo.name);
            return;
        }
    }
}

void
sdl_reinit_texture(void)
{
#ifndef USE_SDL_SHADER_PIPELINE
    if (pixeldata == NULL)
        return;

#ifdef USE_IMGUI
    osd_deinit();
#endif

    sdl_destroy_texture();

    if (sdl_flags & RENDERER_HARDWARE)
        sdl_render = SDL_CreateRenderer(sdl_win, -1, SDL_RENDERER_ACCELERATED);
    else
        sdl_render = SDL_CreateRenderer(sdl_win, -1, SDL_RENDERER_SOFTWARE);

    if ((sdl_render == NULL) && (sdl_flags & RENDERER_HARDWARE)) {
        SDL_SetHint(SDL_HINT_RENDER_DRIVER, "");
        sdl_render = SDL_CreateRenderer(sdl_win, -1, SDL_RENDERER_SOFTWARE);
    }
    if (sdl_render == NULL)
        return;

    sdl_tex = SDL_CreateTexture(sdl_render, SDL_PIXELFORMAT_ARGB8888,
                                SDL_TEXTUREACCESS_STREAMING, 2048, 2048);
    sdl_osd_tex = SDL_CreateTexture(sdl_render, SDL_PIXELFORMAT_ABGR8888,
                                    SDL_TEXTUREACCESS_STREAMING, 2048, 2048);
    if (sdl_tex == NULL || sdl_osd_tex == NULL) {
        sdl_destroy_texture();
        return;
    }

    SDL_SetTextureBlendMode(sdl_osd_tex, SDL_BLENDMODE_BLEND);

#ifdef USE_IMGUI
    osd_init();
#endif
#endif
}

void
sdl_set_fs(int fs)
{
    SDL_LockMutex(sdl_mutex);
#ifdef USE_SDL_SHADER_PIPELINE
    SDL_SetWindowFullscreen(sdl_win, fs ? SDL_WINDOW_FULLSCREEN : 0);
#else
    SDL_SetWindowFullscreen(sdl_win, fs ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
#endif
    if (fs) {
        SDL_SetRelativeMouseMode(SDL_TRUE);
        SDL_ShowCursor(SDL_FALSE);
        mouse_capture = 1;
    } else {
        SDL_SetRelativeMouseMode(SDL_FALSE);
        SDL_ShowCursor(SDL_TRUE);
        mouse_capture = 0;
    }

    sdl_fs = fs;

    if (fs)
        sdl_flags |= RENDERER_FULL_SCREEN;
    else
        sdl_flags &= ~RENDERER_FULL_SCREEN;

    sdl_reinit_texture();
    SDL_UnlockMutex(sdl_mutex);
}

void
sdl_resize(int x, int y)
{
    int target_x = x;
    int target_y = y;
    int ww = 0;
    int wh = 0;
    int wx = 0;
    int wy = 0;

    if (video_fullscreen & 2)
        return;

    /* The 9552 has one fixed-size LCD; guest mode changes occur inside it. */
    if (machine_is_thinkpad_700()) {
        target_x = 640;
        target_y = 480;
    }

    if ((target_x == cur_w) && (target_y == cur_h))
        return;

    SDL_LockMutex(sdl_mutex);

    ww = target_x;
    wh = target_y;

    cur_w = target_x;
    cur_h = target_y;

    cur_wx = wx;
    cur_wy = wy;
    cur_ww = ww;
    cur_wh = wh;

    SDL_SetWindowSize(sdl_win, cur_ww, cur_wh);
    sdl_get_output_size(&sdl_w, &sdl_h);

    sdl_reinit_texture();

    SDL_UnlockMutex(sdl_mutex);
}
void
sdl_reload(void)
{
    if (sdl_flags & RENDERER_HARDWARE) {
        SDL_LockMutex(sdl_mutex);

        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, video_filter_method ? "1" : "0");
        sdl_reinit_texture();

        SDL_UnlockMutex(sdl_mutex);
    }
}

int
plat_vidapi(UNUSED(const char *api))
{
    return 0;
}

static int
sdl_init_common(int flags)
{
    SDL_version ver;
    Uint32      window_flags = (vid_resize & 1 ? SDL_WINDOW_RESIZABLE : 0);

#ifdef USE_SDL_SHADER_PIPELINE
    window_flags |= SDL_WINDOW_OPENGL;
#endif

    /* Get and log the version of the DLL we are using. */
    SDL_GetVersion(&ver);
    fprintf(stderr, "SDL: version %d.%d.%d\n", ver.major, ver.minor, ver.patch);

    /* Initialize the SDL system. */
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL: initialization failed (%s)\n", SDL_GetError());
        return (0);
    }

    /* sdl_reinit_texture() needs the selected backend before the initial
       fullscreen/window setup creates its first texture. */
    sdl_flags = flags;

    // Ensure mouse and touchpads behaves the same for us, dunno if these really do something
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "1");
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "1");

    if (flags & RENDERER_HARDWARE) {
#ifdef USE_SDL_SHADER_PIPELINE
        if (flags & RENDERER_OPENGL) {
            SDL_SetHint(SDL_HINT_RENDER_DRIVER, "OpenGL");
        } else
#endif
            sdl_select_best_hw_driver();
    }

    sdl_mutex = SDL_CreateMutex();
    sdl_win   = SDL_CreateWindow("86Box", strncasecmp(SDL_GetCurrentVideoDriver(), "wayland", 7) != 0 && window_remember ? window_x : SDL_WINDOWPOS_CENTERED, strncasecmp(SDL_GetCurrentVideoDriver(), "wayland", 7) != 0 && window_remember ? window_y : SDL_WINDOWPOS_CENTERED, scrnsz_x, scrnsz_y, window_flags);
    sdl_set_fs(video_fullscreen);
    if (!(video_fullscreen & 1)) {
        if (vid_resize & 2)
            plat_resize(fixed_size_x, fixed_size_y, 0);
        else
            plat_resize(scrnsz_x, scrnsz_y, 0);
    }
    if ((vid_resize < 2) && window_remember) {
        SDL_SetWindowSize(sdl_win, window_w, window_h);
    }

#ifdef USE_SDL_SHADER_PIPELINE
    {
        const char *sp = config_get_string("GL3 Shaders", "shader0", "");
        if (sp && sp[0])
            sdl_shader_init(sdl_win, sp);
        /* Always need a GL context. Passthrough if no shader loaded. */
        if (!sdl_shader_active())
            sdl_shader_init_passthrough(sdl_win);
    }
#endif

    osd_init();

    /* Make sure we get a clean exit. */
    atexit(sdl_close);

    pixeldata = calloc(1, 2048 * 2048 * 4);
    sdl_reinit_texture();

    /* Register our renderer! */
    video_setblit(sdl_blit_shim);

    sdl_enabled = 1;

    return (1);
}

int
sdl_inits(void)
{
    return sdl_init_common(0);
}

int
sdl_inith(void)
{
    return sdl_init_common(RENDERER_HARDWARE);
}

int
sdl_initho(void)
{
    return sdl_init_common(RENDERER_HARDWARE | RENDERER_OPENGL);
}

int
sdl_pause(void)
{
    return 0;
}

void
plat_mouse_capture(int on)
{
    SDL_LockMutex(sdl_mutex);
    SDL_SetRelativeMouseMode((SDL_bool) on);
    if (!on) {
        /* Relative mode normally releases the grab by itself, but X11 and
           some compositors can leave an explicit window/capture grab behind
           after a fullscreen transition. */
        SDL_CaptureMouse(SDL_FALSE);
        SDL_SetWindowGrab(sdl_win, SDL_FALSE);
    }
    SDL_ShowCursor((SDL_bool) !on);
    mouse_capture = on;
    SDL_UnlockMutex(sdl_mutex);
}

void
plat_resize(int w, int h, UNUSED(int monitor_index))
{
    SDL_LockMutex(sdl_mutex);
    resize_w       = w;
    resize_h       = h;
    resize_pending = 1;
    SDL_UnlockMutex(sdl_mutex);
}

char    sdl_win_title[512] = EMU_NAME;
SDL_mutex *titlemtx           = NULL;

void
ui_window_title_real(void)
{
    SDL_SetWindowTitle(sdl_win, sdl_win_title);
    title_set = 0;
}
extern SDL_threadID eventthread;

/* Only activate threading path on macOS, otherwise it will softlock Xorg.
   Wayland doesn't seem to have this issue. */
char *
ui_window_title(char *str)
{
    if (!str)
        return sdl_win_title;
#ifdef __APPLE__
    if (eventthread == SDL_ThreadID())
#endif
    {
        memset(sdl_win_title, 0, sizeof(sdl_win_title));
        strncpy(sdl_win_title, str, sizeof(sdl_win_title) - 1);
        ui_window_title_real();
        return str;
    }
#ifdef __APPLE__
    memset(sdl_win_title, 0, sizeof(sdl_win_title));
    strncpy(sdl_win_title, str, sizeof(sdl_win_title) - 1);
    title_set = 1;
#endif
    return str;
}

void
ui_init_monitor(UNUSED(int monitor_index))
{
    /* No-op. */
}

void
ui_deinit_monitor(UNUSED(int monitor_index))
{
    /* No-op. */
}

void
plat_resize_request(UNUSED(int w), UNUSED(int h), int monitor_index)
{
    atomic_store((&doresize_monitors[monitor_index]), 1);
}
