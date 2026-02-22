#define DEBUG_PRINTS 0
#include <GBAMultiBoot.h>

GBAMultiBoot mb;

void setup() {
  mb.setPins({3, 4, 5, -1});     // sc, so, si, sd

  GBAMultiBoot::Timing timing;
  timing.bit_delay_us = 1;
  timing.word_gap_us = 300;
  mb.setTiming(timing);

  GBAMultiBoot::Options opt;
  opt.slave_bit = 2;             // 2/4/8 or 0=auto
  opt.palette_data = 0xD1;
  opt.auto_mount_fs = true;
  mb.setOptions(opt);

  mb.begin();
  (void)mb.upload("/rom.mb");    // blocks until success/failure
}

void loop() {}
