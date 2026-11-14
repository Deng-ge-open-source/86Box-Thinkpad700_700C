/* Windows-only, source-level regression with a simulated storage driver.
   No real device is opened and no firmware or disk image is required. */
#include <windows.h>
#include <winioctl.h>
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static DWORD generation = 1;
static ULONGLONG ticks = 100;
static int reads, media_present = 1;
static uint64_t position;

static UINT mock_drive_type(LPCSTR path) { return path[0] == 'A' ? DRIVE_REMOVABLE : DRIVE_FIXED; }
static HANDLE mock_open(LPCSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
                        DWORD creation, DWORD attributes, HANDLE template_file)
{
    assert(!strcmp(path, "\\\\.\\A:"));
    assert(access == GENERIC_READ && creation == OPEN_EXISTING);
    (void) share; (void) sa; (void) attributes; (void) template_file;
    return (HANDLE) (uintptr_t) 1;
}
static BOOL mock_close(HANDLE handle) { (void) handle; return TRUE; }
static ULONGLONG mock_ticks(void) { return ticks; }
static BOOL mock_ioctl(HANDLE handle, DWORD code, LPVOID in, DWORD in_size,
                       LPVOID out, DWORD size, LPDWORD returned, LPOVERLAPPED overlapped)
{
    (void) handle; (void) in; (void) in_size; (void) overlapped;
    if (code == IOCTL_DISK_GET_DRIVE_GEOMETRY) {
        DISK_GEOMETRY *geometry = out;
        assert(size >= sizeof(*geometry));
        memset(geometry, 0, sizeof(*geometry));
        geometry->Cylinders.QuadPart = 80;
        geometry->MediaType = F3_1Pt44_512;
        geometry->TracksPerCylinder = 2;
        geometry->SectorsPerTrack = 18;
        geometry->BytesPerSector = 512;
        *returned = sizeof(*geometry);
        return TRUE;
    }
    assert(code == IOCTL_STORAGE_CHECK_VERIFY2);
    if (!media_present) {
        SetLastError(ERROR_NOT_READY);
        return FALSE;
    }
    *(DWORD *) out = generation;
    *returned = sizeof(generation);
    return TRUE;
}
static BOOL mock_seek(HANDLE handle, LARGE_INTEGER offset, PLARGE_INTEGER actual, DWORD method)
{
    (void) handle; (void) actual;
    assert(method == FILE_BEGIN);
    position = offset.QuadPart;
    return TRUE;
}
static BOOL mock_read(HANDLE handle, LPVOID out, DWORD size, LPDWORD actual, LPOVERLAPPED overlapped)
{
    (void) handle; (void) overlapped;
    reads++;
    for (DWORD i = 0; i < size; i++)
        ((uint8_t *) out)[i] = (uint8_t) (generation + (position + i) / 512);
    *actual = size;
    return TRUE;
}

#define GetDriveTypeA mock_drive_type
#define CreateFileA mock_open
#define CloseHandle mock_close
#define GetTickCount64 mock_ticks
#define DeviceIoControl mock_ioctl
#define SetFilePointerEx mock_seek
#define ReadFile mock_read
#include "../../../src/qt/win_floppy_ioctl.c"

int ui_writeprot[FDD_NUM], writeprot[FDD_NUM], fwriteprot[FDD_NUM];
void pclog(const char *format, ...) { (void) format; }

int main(void)
{
    int tracks, sides, sectors, rate;
    uint8_t data[512];
    fdd_set_host_device(0, "C:");
    assert(!floppy_ioctl_open(0, &tracks, &sides, &sectors, &rate));
    fdd_set_host_device(0, "A:");
    assert(floppy_ioctl_open(0, &tracks, &sides, &sectors, &rate));
    assert(tracks == 80 && sides == 2 && sectors == 18 && rate == 0);
    assert(ui_writeprot[0] && writeprot[0] && fwriteprot[0]);
    for (int pass = 0; pass < 5; pass++)
        for (int side = 0; side < 2; side++)
            for (int sector = 1; sector <= 18; sector++) {
                assert(floppy_ioctl_read_sector(0, 0, side, sector, data));
                assert(data[0] == generation + side * 18 + sector - 1);
            }
    assert(reads == 2);
    assert(floppy_ioctl_read_sector(0, 1, 0, 1, data));
    assert(floppy_ioctl_read_sector(0, 0, 0, 1, data));
    assert(reads == 3); /* Returning to an earlier cylinder stays cached. */
    ticks += 1001;
    generation++;
    assert(floppy_ioctl_read_sector(0, 0, 0, 1, data) && data[0] == generation);
    assert(reads == 4);
    ticks += 1001;
    media_present = 0;
    assert(!floppy_ioctl_read_sector(0, 0, 0, 1, data));
    media_present = 1;
    assert(floppy_ioctl_read_sector(0, 0, 0, 1, data));
    assert(reads == 5);
    floppy_ioctl_set_buffering(0);
    assert(floppy_ioctl_read_sector(0, 0, 0, 2, data) && data[0] == generation + 1);
    assert(reads == 6);
    assert(!floppy_ioctl_read_sector(0, 80, 0, 1, data));
    assert(!floppy_ioctl_read_sector(0, 0, 2, 1, data));
    assert(!floppy_ioctl_read_sector(0, 0, 0, 19, data));
    assert(!floppy_ioctl_write_sector(0, 0, 0, 1, data));
    floppy_ioctl_close(0);
    puts("Windows host floppy regression passed: cache, media change/removal, bounds and read-only access.");
    return 0;
}
