// os8088-pm114: the Sound Blaster's AUDIO HEALTH record, for a field run with
// no debugger (os8088 docs/FIELD-NOTES.md 66; tests/pmdma's A key reads it at
// the BIOS segment : 4000h + AH_OFFS). A video streamed from the card's own
// disk plays with 2-6 ms dropouts while the disk is busy and none from RAM;
// these say which of the card's own stages runs dry. Byte stores only:
// PM_DP_RAM has no alignment promise. Every writer is on core 0.
#pragma once
#ifndef OS88_DIAG
#define OS88_DIAG 0
#endif
#include <stdint.h>
#include "pico/time.h"
extern volatile uint8_t PM_DP_RAM[];   // unsized: fits picomem.cpp and pm_gvars.h

#define AH_OFFS     0x1F80          // past the DMA write log (ends 1F80h)
#define AH_SIG      (AH_OFFS+0)     // 'AH08' (01: no AH_STOCK, 02: no AH_SILENT,
                                    // 03: no AH_SITE, 04: no AH_SLAST and
                                    // AH_UNDER counted every empty pass)
#define AH_RESET    (AH_OFFS+4)     // byte: the PC writes 1, core 0 clears all
#define AH_STOCK    (AH_OFFS+5)     // byte: nonzero = the card's STOCK sound
                                    // path (128-byte copies, a 512-sample
                                    // FIFO start). 0, the default, is
                                    // os8088's: 512-byte copies and a 1,024
                                    // start. The reset leaves it alone
#define AH_UNDER    (AH_OFFS+8)     // the SB's FIFO ran dry while its DMA
                                    // ran: once a dry spell (AH05; before
                                    // it, every mixer pass that found it
                                    // empty, idle or not)
#define AH_DSPRATE  (AH_OFFS+12)    // AH07: the sample rate the DSP took at
                                    // its last DMA start, Hz (was the FIFO's
                                    // lowest level)
#define AH_MAXBACK  (AH_OFFS+16)    // most samples the timer asked for that
                                    // were not yet in the FIFO
#define AH_MAXGAP   (AH_OFFS+20)    // longest gap between two SB worker
                                    // passes with a stream running, us
#define AH_MAXCOPY  (AH_OFFS+24)    // longest copy request -> PC's ack, us
#define AH_COPIES   (AH_OFFS+28)    // copy requests made
#define AH_DSPBLK   (AH_OFFS+32)    // AH07: its block, bytes, | autoinit << 16
                                    // (was int 13h calls served)
#define AH_DSPSTART (AH_OFFS+36)    // AH07: DMA starts (was the longest int 13h)
#define AH_WORKN    (AH_OFFS+40)    // SB worker passes with a stream running
#define AH_FMAX     (AH_OFFS+44)    // the FIFO's highest level, samples (AH06;
                                    // before, its level when last mixed)
#define AH_IRQS     (AH_OFFS+48)    // AH07: block IRQs sent (was int 13h time)
#define AH_SILENT   (AH_OFFS+52)    // output buffers (64 samples, 1.29 ms)
                                    // the speaker got SILENCE for: the
                                    // mixer had none ready
#define AH_SILMAX   (AH_OFFS+56)    // the longest run of them
#define AH_SDMIX    (AH_OFFS+60)    // mixes run inside the SD driver's waits
#define AH_DROP     (AH_OFFS+64)    // samples DROPPED: copied from the PC but
                                    // the FIFO had no room (AH06; before, the
                                    // heap never yet handed out, bytes, at
                                    // the last reset (holes not counted)
#define AH_SITE     (AH_OFFS+68)    // AH_NSITE x 32-bit: the silent buffers
                                    // by what core 0 ran (pm_site.h)
#define AH_NSITE    12
#define AH_SLAST    (AH_OFFS+116)   // the last silent buffer: site << 8 |
                                    // the PM command (PM_Command) running
#define AH_SMAXAT   (AH_OFFS+120)   // ...and the longest run's first
#define AH_STALE    (AH_OFFS+124)   // AH08: bytes a copy brought from the PC
                                    // that a new DMA start then made stale
                                    // (the 8237 reprogrammed): dropped
#define AH_END      (AH_OFFS+128)
#if AH_END > 0x2000
#error "the audio health record runs off the shared RAM"
#endif
#if AH_STOCK != 0x1F85
#error "pm_audio.h reads AH_STOCK as 0x1F85"
#endif

#if OS88_DIAG
#define OS88_STOCK() (PM_DP_RAM[AH_STOCK] != 0)   // the A/B: stock sound path
static inline uint32_t ah_get(uint32_t o)
{
  return PM_DP_RAM[o] | ((uint32_t) PM_DP_RAM[o+1] << 8) |
         ((uint32_t) PM_DP_RAM[o+2] << 16) | ((uint32_t) PM_DP_RAM[o+3] << 24);
}
static inline void ah_put(uint32_t o, uint32_t v)
{
  PM_DP_RAM[o] = (uint8_t) v;           PM_DP_RAM[o+1] = (uint8_t) (v >> 8);
  PM_DP_RAM[o+2] = (uint8_t) (v >> 16); PM_DP_RAM[o+3] = (uint8_t) (v >> 24);
}
static inline void ah_inc(uint32_t o) { ah_put(o, ah_get(o) + 1); }
static inline void ah_add(uint32_t o, uint32_t v) { ah_put(o, ah_get(o) + v); }
static inline void ah_max(uint32_t o, uint32_t v) { if (v > ah_get(o)) ah_put(o, v); }
static inline void ah_min(uint32_t o, uint32_t v) { if (v < ah_get(o)) ah_put(o, v); }
static inline void ah_clear(void)
{
  PM_DP_RAM[AH_RESET] = 0;
  for (uint32_t o = AH_UNDER; o < AH_END; o++) PM_DP_RAM[o] = 0;
}
static inline void ah_init(void)
{
  PM_DP_RAM[AH_SIG] = 'A'; PM_DP_RAM[AH_SIG+1] = 'H';
  PM_DP_RAM[AH_SIG+2] = '0'; PM_DP_RAM[AH_SIG+3] = '8';
  for (uint32_t o = AH_STOCK; o < AH_UNDER; o++) PM_DP_RAM[o] = 0;
  ah_clear();
}
#else  // the record and the switch are field diagnostics (CMakeLists.txt)
#define OS88_STOCK() 0
static inline uint32_t ah_get(uint32_t o) { (void) o; return 0; }
static inline void ah_put(uint32_t o, uint32_t v) { (void) o; (void) v; }
static inline void ah_inc(uint32_t o) { (void) o; }
static inline void ah_add(uint32_t o, uint32_t v) { (void) o; (void) v; }
static inline void ah_max(uint32_t o, uint32_t v) { (void) o; (void) v; }
static inline void ah_min(uint32_t o, uint32_t v) { (void) o; (void) v; }
static inline void ah_clear(void) {}
static inline void ah_init(void) {}
#endif
