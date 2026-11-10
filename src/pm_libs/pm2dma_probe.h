#pragma once
// pm2dma_probe.h : the PicoMEM 2's DMA-to-PC probe (os8088 PICOMEM2-DMA-DISK-PLAN, wave 0)
//
// A disk READ by DMA is the 8237's WRITE transfer: it raises AEN, asserts IOR
// for the device holding DACK and MEMW for the memory, at the address it
// counts. The 2.0 has no DACK-and-IOR input (PIN_DMAW is DACK and IOW, the
// Sound Blaster's direction), and this firmware never reads AEN, so whether
// the card can even SEE such a cycle's IOR is a question about the board,
// not the code. This probe answers it on the iron.
//
// The PC programs its 8237 channel (the jumper's: 1 or 3) for a single-mode
// write transfer of N bytes into a buffer, then arms the card here. The card
// raises DRQ and, while armed, counts every bus cycle by its control nibble
// (bit 0 MEMR, 1 MEMW, 2 IOR, 3 IOW) and every cycle whose 20-bit address
// falls in the buffer. In MODE 1 it also drives its pattern byte on the
// buffer's next expected MEMW (the data bus held until MEMW ends); in MODE 2
// it drives on a MEMW+IOR cycle (nibble 6) at that address, as an I/O read
// would. MODE 0 drives nothing: the counts alone say which strobes reach the
// card. The PC then reads the buffer and the counters (CMD_P2D_STAT).
//
// Off unless built with -DPM2DMA_PROBE=1, and 2.0 only (the 1.x has no DRQ).
// Nothing here is meant to ship: the SB must be idle while the probe runs,
// because it takes the same DRQ line.

#if PM2DMA_PROBE && BOARD_PM20

#include "hardware/pio.h"
#include "isa_dma.h"

#define P2D_NSTAT 40

struct p2d_t {
  volatile uint32_t armed;        // core 1 tests this on every cycle
  volatile uint32_t mode;         // 0 observe, 1 drive on MEMW, 2 drive on MEMW+IOR
  volatile uint32_t start, end;   // the buffer's 20-bit linear [start, end)
  volatile uint32_t next;         // the address the 8237 should use next
  volatile uint16_t stat[P2D_NSTAT];
};
extern p2d_t p2d;

// stat[] indices (CMD_P2D_STAT reads them by number)
#define P2D_SERVED    0   // bytes driven at the expected address
#define P2D_MW_IN     1   // MEMW-only cycles (nibble 2) inside the buffer
#define P2D_MW_ODD    2   //   ...of those, not at the expected address
#define P2D_N6_IN     3   // MEMW+IOR cycles (nibble 6) inside the buffer
#define P2D_IOR_IN    4   // IOR-only cycles (nibble 4) whose address is inside the buffer
#define P2D_FIRSTLO   5   // the first in-buffer address seen, low word
#define P2D_FIRSTHI   6   //   ...its bits 16-19, and its nibble in bits 8-11
#define P2D_LASTLO    7   // the last in-buffer address seen
#define P2D_LASTHI    8
#define P2D_NIB       16  // 16..31: every cycle while armed, by nibble 0..15
#define P2D_DONE      32  // 1 when the count ran out (DRQ dropped by core 1)
#define P2D_SNOOPA    33  // the card's snoop of the channel: base address
#define P2D_SNOOPP    34  //   page
#define P2D_SNOOPC    35  //   base count
#define P2D_SNOOPM    36  //   bit 0 auto-init, bit 1 masked, as the card saw them
#define P2D_VERSION   39  // 0x0001: this layout

__force_inline void p2d_drq(bool on)
{
  // PIN_DRQ belongs to the DMA state machine (isa_dma_w_tc's SET pin), and
  // the board inverts it: SET 0 raises DRQ, SET 1 drops it, as that program
  // does. Executed into the SM, so the pin never changes owner.
  pio_sm_exec(DMA_PIO, DMA_SM, pio_encode_set(pio_pins, on ? 0 : 1));
}

__force_inline void p2d_note(uint32_t a, uint32_t nib)
{
  if (!p2d.stat[P2D_FIRSTLO] && !p2d.stat[P2D_FIRSTHI]) {
    p2d.stat[P2D_FIRSTLO] = (uint16_t) a;
    p2d.stat[P2D_FIRSTHI] = (uint16_t) (((a >> 16) & 0xF) | (nib << 8));
  }
  p2d.stat[P2D_LASTLO] = (uint16_t) a;
  p2d.stat[P2D_LASTHI] = (uint16_t) (((a >> 16) & 0xF) | (nib << 8));
}

__force_inline uint8_t p2d_byte(uint32_t i)
{
  // The pattern, which the PC computes the same way: no two neighbours equal,
  // and a 256-byte period broken by the high byte so a slip by 256 shows.
  return (uint8_t) ((i * 7 + 0x13) ^ (i >> 8));
}

__force_inline void p2d_advance()
{
  p2d.stat[P2D_SERVED]++;
  if (++p2d.next >= p2d.end) {
    p2d_drq(false);
    p2d.armed = 0;
    p2d.stat[P2D_DONE] = 1;
  }
}

#endif
