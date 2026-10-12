/* os8088-pm114: a stand-in for the 4MB General MIDI soundfont, which is not in
   the published source. dev_mpu.cpp is compiled on every board but the MPU-401
   synth is USE_MPU=0 below PicoMEM 1.5, so nothing loads this; were it asked
   to, tsf_load_memory refuses a 4-byte "soundfont" and the install fails. */
const unsigned char _4MBGMGSMT_SF2[4] = {0, 0, 0, 0};
