/*
 * 86Box    Windows host floppy access.
 *
 * Read sectors from a removable 3.5-inch drive through the Windows storage
 * driver. This backend deliberately never opens a device for writing: the
 * guest always sees a write-protected disk, including Reference Diskettes.
 */
#include <windows.h>
#include <winioctl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#define HAVE_STDARG_H
#include <86box/86box.h>
#include <86box/timer.h>
#include <86box/fdd.h>
#include <86box/plat_floppy_ioctl.h>

#define HOST_SECTOR_SIZE 512
#define HOST_MAX_SECTORS 36

typedef struct host_floppy_t {
    HANDLE   handle;
    char     path[8];
    int      tracks;
    int      sides;
    int      sectors;
    int      cached_track;
    int      cached_side;
    uint8_t  data[HOST_MAX_SECTORS * HOST_SECTOR_SIZE];
    uint64_t bytes_read;
} host_floppy_t;

static host_floppy_t host_floppies[FDD_NUM];
static int          buffering_enabled = 1;

void
fdd_set_host_device(int drive, const char *path)
{
    const char *letter = path;

    if (drive < 0 || drive >= FDD_NUM)
        return;

    host_floppies[drive].path[0] = '\0';
    if (!path)
        return;
    if (!strncmp(path, "\\\\.\\", 4))
        letter += 4;
    if (strlen(letter) != 2 || !isalpha((unsigned char) letter[0]) || letter[1] != ':')
        return;

    snprintf(host_floppies[drive].path, sizeof(host_floppies[drive].path),
             "\\\\.\\%c:", toupper((unsigned char) letter[0]));
}

const char *
fdd_get_host_device(int drive)
{
    return (drive >= 0 && drive < FDD_NUM) ? host_floppies[drive].path : "";
}

void
floppy_ioctl_set_buffering(int enabled)
{
    buffering_enabled = !!enabled;
    for (int drive = 0; drive < FDD_NUM; drive++)
        host_floppies[drive].cached_track = -1;
}

int
floppy_ioctl_get_buffering(void)
{
    return buffering_enabled;
}

void
floppy_ioctl_close(int drive)
{
    host_floppy_t *state;

    if (drive < 0 || drive >= FDD_NUM)
        return;
    state = &host_floppies[drive];
    if (state->handle && state->handle != INVALID_HANDLE_VALUE) {
        pclog("WIN FDD: closed %s, physical bytes read=%llu, writes=0\n",
              state->path, (unsigned long long) state->bytes_read);
        CloseHandle(state->handle);
    }
    state->handle       = NULL;
    state->tracks       = state->sides = state->sectors = 0;
    state->cached_track = state->cached_side = -1;
    state->bytes_read   = 0;
}

int
floppy_ioctl_open(int drive, int *tracks, int *sides, int *sectors, int *rate)
{
    host_floppy_t *state;
    DISK_GEOMETRY geometry;
    DWORD         returned;
    char          root[] = "A:\\";
    int           disk_rate;

    if (drive < 0 || drive >= FDD_NUM)
        return 0;
    state = &host_floppies[drive];
    floppy_ioctl_close(drive);
    if (!state->path[0])
        return 0;

    root[0] = state->path[4];
    if (GetDriveTypeA(root) != DRIVE_REMOVABLE) {
        pclog("WIN FDD: refusing non-removable device %s\n", state->path);
        return 0;
    }

    state->handle = CreateFileA(state->path, GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (state->handle == INVALID_HANDLE_VALUE) {
        pclog("WIN FDD: cannot open %s read-only, Windows error=%lu\n",
              state->path, (unsigned long) GetLastError());
        state->handle = NULL;
        return 0;
    }

    if (!DeviceIoControl(state->handle, IOCTL_DISK_GET_DRIVE_GEOMETRY,
                         NULL, 0, &geometry, sizeof(geometry), &returned, NULL)) {
        pclog("WIN FDD: cannot query %s geometry, Windows error=%lu\n",
              state->path, (unsigned long) GetLastError());
        floppy_ioctl_close(drive);
        return 0;
    }

    switch (geometry.MediaType) {
        case F3_720_512:   disk_rate = 2; break;
        case F3_1Pt44_512: disk_rate = 0; break;
        case F3_2Pt88_512: disk_rate = 3; break;
        default:
            pclog("WIN FDD: unsupported floppy media type %u on %s\n",
                  (unsigned) geometry.MediaType, state->path);
            floppy_ioctl_close(drive);
            return 0;
    }
    if (geometry.Cylinders.QuadPart != 80 || geometry.TracksPerCylinder != 2 ||
        geometry.BytesPerSector != HOST_SECTOR_SIZE ||
        geometry.SectorsPerTrack != (disk_rate == 2 ? 9U : disk_rate == 0 ? 18U : 36U)) {
        pclog("WIN FDD: unsupported floppy geometry on %s\n", state->path);
        floppy_ioctl_close(drive);
        return 0;
    }

    state->tracks  = *tracks  = (int) geometry.Cylinders.QuadPart;
    state->sides   = *sides   = (int) geometry.TracksPerCylinder;
    state->sectors = *sectors = (int) geometry.SectorsPerTrack;
    *rate = disk_rate;
    writeprot[drive] = fwriteprot[drive] = ui_writeprot[drive] = 1;
    pclog("WIN FDD: opened %s read-only, CHS=%d/%d/%d, sector=512\n",
          state->path, state->tracks, state->sides, state->sectors);
    return 1;
}

int
floppy_ioctl_read_sector(int drive, int track, int side, int sector, uint8_t *buffer)
{
    host_floppy_t *state;
    LARGE_INTEGER offset;
    DWORD         requested;
    DWORD         actual;

    if (drive < 0 || drive >= FDD_NUM || !buffer)
        return 0;
    state = &host_floppies[drive];
    if (!state->handle || track < 0 || track >= state->tracks ||
        side < 0 || side >= state->sides || sector < 1 || sector > state->sectors)
        return 0;

    if (buffering_enabled && state->cached_track == track && state->cached_side == side) {
        memcpy(buffer, &state->data[(sector - 1) * HOST_SECTOR_SIZE], HOST_SECTOR_SIZE);
        return 1;
    }

    state->cached_track = -1;
    offset.QuadPart = ((int64_t) track * state->sides + side) * state->sectors;
    if (!buffering_enabled)
        offset.QuadPart += sector - 1;
    offset.QuadPart *= HOST_SECTOR_SIZE;
    requested = buffering_enabled ? state->sectors * HOST_SECTOR_SIZE : HOST_SECTOR_SIZE;
    if (!SetFilePointerEx(state->handle, offset, NULL, FILE_BEGIN) ||
        !ReadFile(state->handle, state->data, requested, &actual, NULL) || actual != requested) {
        pclog("WIN FDD: read failed on %s C=%d H=%d S=%d, Windows error=%lu\n",
              state->path, track, side, sector, (unsigned long) GetLastError());
        return 0;
    }
    state->bytes_read += actual;
    if (buffering_enabled) {
        state->cached_track = track;
        state->cached_side  = side;
    }
    memcpy(buffer, &state->data[buffering_enabled ? (sector - 1) * HOST_SECTOR_SIZE : 0], HOST_SECTOR_SIZE);
    pclog("WIN FDD: physical read %s C=%d H=%d S=%d bytes=%lu total=%llu\n",
          state->path, track, side, sector, (unsigned long) actual,
          (unsigned long long) state->bytes_read);
    return 1;
}

int
floppy_ioctl_write_sector(int drive, int track, int side, int sector, const uint8_t *buffer)
{
    /* No Windows write operation is implemented or permitted. */
    (void) drive;
    (void) track;
    (void) side;
    (void) sector;
    (void) buffer;
    return 0;
}
