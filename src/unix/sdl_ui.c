#include <SDL.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <86box/86box.h>
#include <86box/keyboard.h>
#include <86box/machine.h>
#include <86box/plat.h>
#include <86box/ui.h>
#include <86box/version.h>

#include "sdl_osd.h"

/*
 * The physical ThinkPad 700C-style indicator panel is driven by a separate
 * host service.  Export only the guest-owned state here; the service keeps
 * exclusive ownership of the serial controller and merges these signals with
 * its battery, volume and display-control inputs.
 */
static SDL_atomic_t tp700_hdd_hold_until;
static SDL_atomic_t tp700_fdd_hold_until;
static int          tp700_panel_export_mouse_state = -1;

static void
tp700_panel_note_activity(int tag, int active)
{
    SDL_atomic_t *hold_until = NULL;

    if ((tag & 0xf0) == SB_HDD)
        hold_until = &tp700_hdd_hold_until;
    else if ((tag & 0xf0) == SB_FLOPPY)
        hold_until = &tp700_fdd_hold_until;

    /* A short hold makes real panel lamps visible even for quick commands. */
    if (hold_until != NULL && active)
        SDL_AtomicSet(hold_until, (int) (SDL_GetTicks() + 140));
}

static int
tp700_panel_activity_active(SDL_atomic_t *hold_until, uint32_t now)
{
    return (int32_t) ((uint32_t) SDL_AtomicGet(hold_until) - now) > 0;
}

void
tp700_panel_export_tick(void)
{
    static uint32_t last_write;
    const char     *path;
    uint32_t        now;
    uint8_t         caps = 0;
    uint8_t         num = 0;
    uint8_t         scroll = 0;
    char            temporary[1024];
    struct timespec realtime;
    FILE           *stream;

    path = getenv("TP700_PANEL_STATUS");
    if (path == NULL || path[0] == '\0' ||
        !machine_is_thinkpad_700())
        return;

    /* The physical controls follow pointer ownership.  With no captured
       pointer, remove the guest-status lease so the existing panel service
       immediately resumes the host display/audio controls. */
    if (!mouse_capture) {
        if (tp700_panel_export_mouse_state != 0)
            unlink(path);
        tp700_panel_export_mouse_state = 0;
        return;
    }
    tp700_panel_export_mouse_state = 1;

    now = SDL_GetTicks();
    if (last_write != 0 && (uint32_t) (now - last_write) < 100)
        return;
    last_write = now;

    keyboard_get_states(&caps, &num, &scroll, NULL);
    if (clock_gettime(CLOCK_REALTIME, &realtime) != 0)
        return;

    snprintf(temporary, sizeof(temporary), "%s.tmp.%ld", path, (long) getpid());
    stream = fopen(temporary, "w");
    if (stream == NULL)
        return;

    fprintf(stream,
            "{\"timestamp_unix\":%.3f,\"pid\":%ld,\"machine\":\"%s\","
            "\"running\":true,\"num\":%u,\"caps\":%u,\"scroll\":%u,"
            "\"hdd\":%d,\"floppy\":%d,\"mouse_captured\":%s}\n",
            (double) realtime.tv_sec + (double) realtime.tv_nsec / 1000000000.0,
            (long) getpid(), machine_get_internal_name(), num, caps, scroll,
            tp700_panel_activity_active(&tp700_hdd_hold_until, now),
            tp700_panel_activity_active(&tp700_fdd_hold_until, now),
            mouse_capture ? "true" : "false");
    if (fclose(stream) != 0 || rename(temporary, path) != 0)
        unlink(temporary);
}

void
tp700_panel_export_close(void)
{
    const char *path = getenv("TP700_PANEL_STATUS");

    if (path != NULL && path[0] != '\0')
        unlink(path);
    tp700_panel_export_mouse_state = -1;
}

int
ui_msgbox(int flags, char *message)
{
    return ui_msgbox_header(flags, NULL, message);
}

int
ui_msgbox_header(int flags, char *header, char *message)
{
    SDL_MessageBoxData       msgdata;
    SDL_MessageBoxButtonData msgbtn;

    if (!header) {
        if (flags & MBX_FATAL)
            header = "Fatal error";
        else if (flags & MBX_ERROR)
            header = "Error";
        else
            header = EMU_NAME;
    }

    msgbtn.buttonid = 1;
    msgbtn.text     = "OK";
    msgbtn.flags    = 0;
    memset(&msgdata, 0, sizeof(SDL_MessageBoxData));
    msgdata.numbuttons = 1;
    msgdata.buttons    = &msgbtn;
    int msgflags       = 0;
    if ((flags & MBX_ERROR) || (flags & MBX_FATAL))
        msgflags |= SDL_MESSAGEBOX_ERROR;
    else if (flags & MBX_WARNING)
        msgflags |= SDL_MESSAGEBOX_WARNING;
    else
        msgflags |= SDL_MESSAGEBOX_INFORMATION;
    msgdata.flags   = msgflags;
    int button      = 0;
    msgdata.title   = header;
    msgdata.message = message;
    SDL_ShowMessageBox(&msgdata, &button);
    return button;
}

void
ui_sb_update_icon_state(int tag, int state)
{
    osd_ui_sb_update_icon_state(tag, state);
}

void
ui_sb_update_icon(int tag, int active)
{
    tp700_panel_note_activity(tag, active);
    osd_ui_sb_update_icon(tag, active);
}

void
ui_sb_update_icon_write(int tag, int active)
{
    tp700_panel_note_activity(tag, active);
    osd_ui_sb_update_icon_write(tag, active);
}

void
ui_sb_update_icon_wp(int tag, int state)
{
    osd_ui_sb_update_icon_wp(tag, state);
}

void
ui_sb_update_tip(UNUSED(int arg))
{
    /* No-op. */
}

void
ui_sb_update_panes(void)
{
    /* No-op. */
}

void
ui_sb_update_text(void)
{
    /* No-op. */
}

void
ui_sb_set_text(UNUSED(char *wstr))
{
    /* No-op. */
}

void
ui_sb_bugui(UNUSED(char *str))
{
    /* No-op. */
}

void
ui_sb_set_ready(UNUSED(int ready))
{
    /* No-op. */
}

void
ui_sb_mt32lcd(UNUSED(char *str))
{
    /* No-op. */
}

void
ui_hard_reset_completed(void)
{
    /* No-op. */
}

void
ui_update_force_interpreter(void)
{
    /* No-op. */
}
