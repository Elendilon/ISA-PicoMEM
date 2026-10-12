// os8088-pm114: WHAT CORE 0 WAS RUNNING when the speaker went silent. The
// I2S DMA interrupt that finds no mixed buffer runs on core 0 - on top of
// whatever stalled the mixer - and counts the silent buffer against
// pm_site (pm_audio.h). Each stretch of core 0 that can run long sets it
// on the way in and puts the old value back on the way out; pm_ahealth.h's
// AH_SITE rows carry the counts to the PC (tests/pmdma's A key).
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
extern volatile uint8_t pm_site;
#ifdef __cplusplus
}
#endif
enum {
  PMS_LOOP = 0,   // the main loop, between the stretches below
  PMS_USB,        // tuh_task: the USB host
  PMS_SBW,        // dev_sbdsp_update: the SB worker
  PMS_MIX,        // pm_audio_domix from the main loop
  PMS_TICK,       // the 100 ms / 1 s housekeeping
  PMS_CMD,        // PM_ExecCMD, any PC command but int 13h
  PMS_I13,        // INT13_DiskHandler, outside the stretches below
  PMS_FAT,        // FatFs and the SD transfers of a disk-image sector
  PMS_SDWAIT,     // (not set: the SD waits count as FAT - see AH04 notes)
  PMS_PCWAIT,     // waiting for the PC's sector copy, audio pumped
  PMS_PCCMD,      // (not set: a PC command end counts as the caller's)
  PMS_NET,        // ne2000_initiate_send
  PMS_N = 12
};
#define PM_SITE_IN(x)  uint8_t pm_site_sv = pm_site; pm_site = (x)
#define PM_SITE_OUT()  (pm_site = pm_site_sv)
