/* Windows scheduler regression: run the real HDD worker without any emulated
   sound-poll events, as happens during a blocking USB read. No audio device,
   firmware or disk is opened. Link with whole-program/section GC. */
#include <assert.h>
#include <stdio.h>
#include "../../../src/sound/sound.c"

int dopause;
static uint32_t test_clock;
static uint32_t last_delivery;
static int deliveries;
static int host_selected;
static int infinite_waits;

uint32_t plat_get_ticks(void) { return test_clock; }
int fdd_is_host(int drive) { return host_selected && drive == 0; }
void thread_set_event(event_t *event) { (void) event; }
void thread_reset_event(event_t *event) { (void) event; }

int thread_wait_event(event_t *event, int timeout)
{
    (void) event;
    if (timeout < 0) {
        infinite_waits++;
        hddaudioon = 0; /* Only shutdown wakes an image-backed idle worker. */
        return 0;
    }
    /* Early guest events must not speed up host-clock audio. After 50 ms,
       withhold all guest events to reproduce a synchronous host read. */
    if (test_clock < 50 && timeout > 1)
        test_clock++;
    else
        test_clock += timeout ? (uint32_t) timeout : 1;
    dopause = test_clock >= 500 && test_clock < 900;
    if (test_clock >= 2000)
        hddaudioon = 0;
    return 1;
}

void hdd_audio_callback(int16_t *buffer, int length)
{
    (void) buffer;
    assert(length == SOUNDBUFLEN * 2);
    assert(!dopause);
    if (last_delivery)
        assert(test_clock - last_delivery >= 20);
    last_delivery = test_clock;
    deliveries++;
}
void givealbuffer_hdd(const void *buffer, uint32_t size)
{
    (void) buffer;
    assert(size == SOUNDBUFLEN * 2);
}

int main(void)
{
    host_selected = 1;
    hddaudioon = 1;
    sound_hdd_thread(NULL);
    assert(!infinite_waits);
    assert(deliveries >= 75 && deliveries <= 80);
    assert(last_delivery >= 1960);
    printf("Host-wait HDD audio: %d blocks in 2 s, including a 400 ms pause; no guest events after 50 ms.\n", deliveries);
    host_selected = 0;
    hddaudioon = 1;
    deliveries = 0;
    sound_hdd_thread(NULL);
    assert(infinite_waits == 1 && deliveries == 0);
    puts("HDD audio scheduler regression passed: host wait, pacing, pause and legacy idle.");
    return 0;
}
