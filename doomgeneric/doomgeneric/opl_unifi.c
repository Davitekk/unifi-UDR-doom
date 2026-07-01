// Non-SDL OPL driver for doomgeneric on UniFi UDR.
//
// Software Nuked OPL3 emulation. Instead of an SDL audio callback, our own
// mixer thread pulls audio via OPL_Unifi_Render(), which advances virtual
// time and fires the scheduled music-event callbacks. Closely modeled on
// Chocolate Doom's opl_sdl.c (pthreads instead of SDL primitives).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>

#include "opl3.h"
#include "opl.h"
#include "opl_internal.h"
#include "opl_queue.h"

typedef struct {
    unsigned int rate;
    unsigned int enabled;
    unsigned int value;
    uint64_t     expire_time;
} opl_timer_t;

// When callback_mutex is held (OPL_Lock), music-event callbacks are not invoked.
static pthread_mutex_t callback_mutex       = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t callback_queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static opl_callback_queue_t *callback_queue;

static uint64_t current_time;       // us since startup
static int      opl_unifi_paused;
static uint64_t pause_offset;

static opl3_chip opl_chip;
static int       opl_opl3mode;
static int       register_num = 0;

static opl_timer_t timer1 = { 12500, 0, 0, 0 };
static opl_timer_t timer2 = { 3125, 0, 0, 0 };

static int mixing_freq = 44100;

int opl_unifi_active = 0;           // set once the chip is ready to be pumped

// Advance virtual time by nsamples, invoking any due callbacks.
static void AdvanceTime(unsigned int nsamples)
{
    opl_callback_t callback;
    void *callback_data;
    uint64_t us;

    pthread_mutex_lock(&callback_queue_mutex);

    us = ((uint64_t)nsamples * OPL_SECOND) / mixing_freq;
    current_time += us;
    if (opl_unifi_paused) pause_offset += us;

    while (!OPL_Queue_IsEmpty(callback_queue)
        && current_time >= OPL_Queue_Peek(callback_queue) + pause_offset)
    {
        if (!OPL_Queue_Pop(callback_queue, &callback, &callback_data))
            break;

        // Hold callback_mutex (not the queue mutex) while invoking, so the
        // control thread's OPL_Lock blocks callbacks, and the callback can
        // schedule new callbacks via OPL_SetCallback.
        pthread_mutex_unlock(&callback_queue_mutex);
        pthread_mutex_lock(&callback_mutex);
        callback(callback_data);
        pthread_mutex_unlock(&callback_mutex);
        pthread_mutex_lock(&callback_queue_mutex);
    }

    pthread_mutex_unlock(&callback_queue_mutex);
}

// Pull-render: fill `out` (stereo int16, nframes frames) with OPL output and
// advance time/callbacks. Safe to call before music is initialized (silence).
void OPL_Unifi_Render(int16_t *out, unsigned int nframes)
{
    unsigned int filled = 0;

    if (!opl_unifi_active) {
        memset(out, 0, (size_t)nframes * 2 * sizeof(int16_t));
        return;
    }

    while (filled < nframes)
    {
        uint64_t nsamples;

        pthread_mutex_lock(&callback_queue_mutex);
        if (opl_unifi_paused || OPL_Queue_IsEmpty(callback_queue))
        {
            nsamples = nframes - filled;
        }
        else
        {
            uint64_t next = OPL_Queue_Peek(callback_queue) + pause_offset;
            if (next > current_time)
            {
                nsamples = (next - current_time) * mixing_freq;
                nsamples = (nsamples + OPL_SECOND - 1) / OPL_SECOND;
            }
            else
            {
                nsamples = 0;
            }
            if (nsamples > nframes - filled) nsamples = nframes - filled;
        }
        pthread_mutex_unlock(&callback_queue_mutex);

        if (nsamples > 0)
        {
            OPL3_GenerateStream(&opl_chip, out + filled * 2, (uint32_t)nsamples);
            filled += nsamples;
        }
        AdvanceTime((unsigned int)nsamples);
    }
}

// ---- driver interface -----------------------------------------------------
static int OPL_Unifi_Init(unsigned int port_base)
{
    (void)port_base;
    opl_unifi_paused = 0;
    pause_offset = 0;
    callback_queue = OPL_Queue_Create();
    current_time = 0;
    mixing_freq = opl_sample_rate;
    OPL3_Reset(&opl_chip, mixing_freq);
    opl_opl3mode = 0;
    opl_unifi_active = 1;            // mixer may begin pumping now
    return 1;
}

static void OPL_Unifi_Shutdown(void)
{
    opl_unifi_active = 0;
    if (callback_queue) { OPL_Queue_Destroy(callback_queue); callback_queue = NULL; }
}

static unsigned int OPL_Unifi_PortRead(opl_port_t port)
{
    unsigned int result = 0;
    if (port == OPL_REGISTER_PORT_OPL3) return 0xff;
    if (timer1.enabled && current_time > timer1.expire_time) { result |= 0x80; result |= 0x40; }
    if (timer2.enabled && current_time > timer2.expire_time) { result |= 0x80; result |= 0x20; }
    return result;
}

static void OPLTimer_CalculateEndTime(opl_timer_t *timer)
{
    if (timer->enabled)
    {
        int tics = 0x100 - timer->value;
        timer->expire_time = current_time + ((uint64_t)tics * OPL_SECOND) / timer->rate;
    }
}

static void WriteRegister(unsigned int reg_num, unsigned int value)
{
    switch (reg_num)
    {
        case OPL_REG_TIMER1:
            timer1.value = value; OPLTimer_CalculateEndTime(&timer1); break;
        case OPL_REG_TIMER2:
            timer2.value = value; OPLTimer_CalculateEndTime(&timer2); break;
        case OPL_REG_TIMER_CTRL:
            if (value & 0x80) { timer1.enabled = 0; timer2.enabled = 0; }
            else {
                if ((value & 0x40) == 0) { timer1.enabled = (value & 0x01) != 0; OPLTimer_CalculateEndTime(&timer1); }
                if ((value & 0x20) == 0) { timer2.enabled = (value & 0x02) != 0; OPLTimer_CalculateEndTime(&timer2); }
            }
            break;
        case OPL_REG_NEW:
            opl_opl3mode = value & 0x01;
            /* fall through to also write the register */
        default:
            OPL3_WriteRegBuffered(&opl_chip, reg_num, value);
            break;
    }
}

static void OPL_Unifi_PortWrite(opl_port_t port, unsigned int value)
{
    if (port == OPL_REGISTER_PORT)           register_num = value;
    else if (port == OPL_REGISTER_PORT_OPL3) register_num = value | 0x100;
    else if (port == OPL_DATA_PORT)          WriteRegister(register_num, value);
}

static void OPL_Unifi_SetCallback(uint64_t us, opl_callback_t callback, void *data)
{
    pthread_mutex_lock(&callback_queue_mutex);
    OPL_Queue_Push(callback_queue, callback, data, current_time - pause_offset + us);
    pthread_mutex_unlock(&callback_queue_mutex);
}

static void OPL_Unifi_ClearCallbacks(void)
{
    pthread_mutex_lock(&callback_queue_mutex);
    OPL_Queue_Clear(callback_queue);
    pthread_mutex_unlock(&callback_queue_mutex);
}

static void OPL_Unifi_Lock(void)            { pthread_mutex_lock(&callback_mutex); }
static void OPL_Unifi_Unlock(void)          { pthread_mutex_unlock(&callback_mutex); }
static void OPL_Unifi_SetPaused(int paused) { opl_unifi_paused = paused; }

static void OPL_Unifi_AdjustCallbacks(float factor)
{
    pthread_mutex_lock(&callback_queue_mutex);
    OPL_Queue_AdjustCallbacks(callback_queue, current_time, factor);
    pthread_mutex_unlock(&callback_queue_mutex);
}

opl_driver_t opl_unifi_driver =
{
    "Unifi",
    OPL_Unifi_Init,
    OPL_Unifi_Shutdown,
    OPL_Unifi_PortRead,
    OPL_Unifi_PortWrite,
    OPL_Unifi_SetCallback,
    OPL_Unifi_ClearCallbacks,
    OPL_Unifi_Lock,
    OPL_Unifi_Unlock,
    OPL_Unifi_SetPaused,
    OPL_Unifi_AdjustCallbacks,
};
