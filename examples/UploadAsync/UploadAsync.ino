#define DEBUG_PRINTS 0
#include <GBAMultiBoot.h>

GBAMultiBoot mb;

static void onProgress(uint8_t percent, GBAMultiBoot::State, void*) {
  // user code: update UI, LED, etc.
  (void)percent;
}

static void whileRunning(void*) {
  // user code: keep other systems alive
}

void setup() {
  mb.setPins({3, 4, 5, -1});
  mb.setProgressCallback(onProgress, nullptr);
  mb.setRunningCallback(whileRunning, nullptr);

  GBAMultiBoot::Timing t;
  t.words_per_update = 16;
  mb.setTiming(t);

  mb.begin();
  (void)mb.start("/rom.mb");
}

void loop() {
  if (mb.isRunning()) (void)mb.update();
}
