#pragma once

#include <stdlib.h>  // for "abs"

#include "../../../pm_debug.h"

#define PM_AUDIO_FREQUENCY 49716   // Adlib output frequency
//#define PM_AUDIO_FREQUENCY 44100
#define PM_SAMPLES_PER_BUFFER 64
// os8088-pm114: 48, was 5. The I2S DMA plays a mixed buffer per 64 samples
// (1.29 ms) and the mixer runs only in core 0's main loop, so the queue is
// all the sound in hand when that loop stalls - and stock it was 4 buffers,
// 5.1 ms, against SD reads of up to 42 ms: a video streamed from the card's
// disk dropped sound every time. 47 in flight is 60 ms. With the byte at
// AH_STOCK set (pm_ahealth.h) the mixer keeps the stock 4 in flight. The
// pool is a malloc beside the NE2000's 32KB, so it falls back to 24, 12 and
// the stock 5 buffers when the heap is short; pm_audio_nbuf is what it got.
#define PM_AUDIO_BUFFERS  48
#define PM_AUDIO_STOCK_BUFFERS 5
#define PM_AUDIO_STOCK_INFLIGHT 4
#define PM_SAMPLES_STRIDE 4        // (Nb of byte per sample 16Bit X 2)
// 8*64*4 buffer (16Bit Stereo) is 2KB

#define PM_AUDIO_BUFFERSIZE PM_SAMPLES_PER_BUFFER*PM_SAMPLES_STRIDE

/** \brief Audio format definition
 */
typedef struct audio_format {
    uint32_t sample_freq;      ///< Sample frequency in Hz
    uint16_t format;           ///< Audio format \ref audio_formats
    uint16_t channel_count;    ///< Number of channels  (Not used in this code, default is 2)
} audio_format_t;

typedef struct pm_audio
 {
    uint8_t *bufferpool;       // Pointer to the Buffer Pool (PM_AUDIO_BUFFERS * PM_SAMPLES_PER_BUFFER * PM_SAMPLES_STRIDE )
    uint8_t *poolend;
    uint8_t *playing_buffer;
    uint8_t *mixing_buffer;
    uint32_t mixed_buffers;     // Total Nb of mixed buffer
    uint32_t played_buffers;    // Total Nb buffer played
    bool enabled;
 } pm_audio_t;

extern volatile pm_audio_t pm_audio;
extern volatile uint32_t pm_audio_starved;   // os8088-pm114: silent buffers
extern volatile uint32_t pm_audio_starverun; //  played, and the longest run
extern volatile uint32_t pm_audio_starvemax; //  of them (pm_ahealth.h)
extern volatile uint32_t pm_audio_nbuf;      // the pool's buffers
extern volatile uint8_t PM_DP_RAM[];

extern bool pm_audio_init(uint32_t sample_freq);
extern bool pm_audio_stop();
//extern void pm_audio_update_frequency(uint32_t sample_freq);
//extern static void audio_i2s_update_frequency(uint32_t sample_freq);


// Take a mixed buffer to give to the DMA (Always the buffer after the last played one)
__force_inline uint8_t * take_mixed_buffer()
{
//if (pm_audio.playing_buffer==NULL) {PM_ERROR("Err: playing_buffer NULL");}
uint32_t pm_buffers_ready=pm_audio.mixed_buffers-pm_audio.played_buffers;
if ((pm_buffers_ready)!=0)
   {
    //printf("-%d",pm_audio.mixed_buffers);
    pm_audio.played_buffers++;
    pm_audio.playing_buffer=pm_audio.playing_buffer+PM_AUDIO_BUFFERSIZE;
    if (pm_audio.playing_buffer>=pm_audio.poolend) pm_audio.playing_buffer=pm_audio.bufferpool;
    pm_audio_starverun=0;
    return pm_audio.playing_buffer;
   }
//PM_INFO("!Z");
pm_audio_starved++;
if (++pm_audio_starverun>pm_audio_starvemax) pm_audio_starvemax=pm_audio_starverun;
return pm_audio.poolend;  // poolend contains an empty buffer
}

// Take an empty buffer to give to the DMA (Always the buffer after the last mixed one)
__force_inline uint8_t * take_empty_buffer()
{
//if (pm_audio.mixing_buffer==NULL) {PM_ERROR("Err: mixing_buffer NULL");}
// 2 counters are needed so that the DMA Interrupt don't cut the buffer increment instruction in two.
int32_t pm_buffers_ready=pm_audio.mixed_buffers-pm_audio.played_buffers;
int32_t pm_inflight=PM_DP_RAM[0x1F85] ? PM_AUDIO_STOCK_INFLIGHT : (int32_t) pm_audio_nbuf-1; // AH_STOCK
if (pm_buffers_ready<pm_inflight)  // Remove one to remove the playing buffer
   {
    uint8_t * res=pm_audio.mixing_buffer+PM_AUDIO_BUFFERSIZE;
    if (res>=pm_audio.poolend) res=pm_audio.bufferpool;
    pm_audio.mixing_buffer=res;
   // printf(" TB:%x %x",((uint32_t)res)&0x0FFF,pm_audio.mixed_buffers);
    return res;
   }
return NULL;
}

__force_inline void give_mixed_buffer()
{
//uint32_t volatile test=pm_audio.mixed_buffers;
pm_audio.mixed_buffers++;
//printf("+GB %d ",pm_audio.mixed_buffers);
}

#define AUDIO_BUFFER_FORMAT_PCM_S16 1          ///< signed 16bit PCM
#define AUDIO_BUFFER_FORMAT_PCM_S8 2           ///< signed 8bit PCM
#define AUDIO_BUFFER_FORMAT_PCM_U16 3          ///< unsigned 16bit PCM
#define AUDIO_BUFFER_FORMAT_PCM_U8 4           ///< unsigned 8bit PCM
