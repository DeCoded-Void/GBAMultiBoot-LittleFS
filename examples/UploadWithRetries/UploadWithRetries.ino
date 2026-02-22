#define DEBUG_PRINTS 1
#include <GBAMultiBoot.h>

GBAMultiBoot mb;

void setup() {
#if DEBUG_PRINTS
  Serial.begin(115200);
  while (!Serial) {}
  mb.setDebugStream(&Serial);
#endif

  mb.setPins({3, 4, 5, -1});

  GBAMultiBoot::Timing t;
  t.bit_delay_us = 1;
  t.word_gap_us = 300;
  mb.setTiming(t);

  mb.begin();

  for (int attempt = 1; attempt <= 10; ++attempt) {
#if DEBUG_PRINTS
    Serial.print("[mb] attempt "); Serial.println(attempt);
#endif

    const GBAMultiBoot::Result r = mb.upload("/rom.mb");
    if (r == GBAMultiBoot::Result::kOk) {
#if DEBUG_PRINTS
      Serial.println("[mb] success");
#endif
      break;
    }

#if DEBUG_PRINTS
    Serial.print("[mb] fail: ");
    Serial.print(GBAMultiBoot::resultName(r));
    Serial.print(" (state ");
    Serial.print(GBAMultiBoot::stateName(mb.state()));
    Serial.println(")");

    Serial.print("[mb] last_tx=0x"); Serial.println(mb.lastTx(), HEX);
    Serial.print("[mb] last_rx=0x"); Serial.println(mb.lastRx(), HEX);

    delay(1000 / 16);
#else
    delay(1000 / 16);
#endif
  }
}

void loop() {}
