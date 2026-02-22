#pragma once

#include <Arduino.h>
#include <LittleFS.h>
#include <stdint.h>

#ifndef DEBUG_PRINTS
#define DEBUG_PRINTS 0 // Supresses Debug Prints if not defined
#endif

class GBAMultiBoot {
 public:
  struct Pins {
    uint8_t sc;  // GBA SC (Serial Clock) -> MCU output
    uint8_t so;  // GBA SO (Serial Out)   -> MCU input
    uint8_t si;  // GBA SI (Serial In)    -> MCU output
    int8_t sd;   // GBA SD (Serial Data)  -> MCU input (for tri-stated handling); set to -1 if unused
  };

  struct Timing {
    uint32_t bit_delay_us = 1;              // BIT_DELAY_US
    uint32_t word_gap_us = 300;             // WORD_GAP_US

    uint16_t delay_16th_ms = 1000u / 16u;   // 1/16 second

    uint32_t init_timeout_ms = 8000u;       // overall init timeout

    uint16_t words_per_update = 16u;        // non-blocking chunk size
    uint16_t yield_every_bytes = 0x1000u;   // optional cooperative yield
    uint16_t yield_delay_ms = 1u;
  };

  struct Options {
    uint8_t slave_bit = 2;                  // SLAVE_BIT (2/4/8) or 0=auto-detect from 720x
    uint8_t palette_data = 0xD1;            // pp (aka palette_data)
    bool so_pullup = true;
    bool auto_mount_fs = true;
  };

  enum class Result : uint8_t {
    kOk = 0,
    kRunning,
    kFsBeginFailed,
    kFileOpenFailed,
    kFileSizeInvalid,
    kTimeout,
    kProtocolMismatch,
    kAborted
  };

  enum class State : uint8_t {
    kIdle = 0,

    kWaitEnterMode,   // ... | send 6200 | recieve FFFF then 0000 | slave entered multi/normal mode
    kWait720x,        // try 15 times | send 6200 | recieve 720x | if fail: delay 1/16s and restart
    kExchangeInfo1,   // send 610y | recieve 720x | recognition okay, exchange master/slave info
    kHeader,          // try 60h times | send xxxx | transfer C0h bytes header data in units of 16bits
    kHeaderDone,      // send 6200 | Transfer of header data completed
    kExchangeInfo2,   // send 620y | Exchange master/slave info again
    kPaletteWait,     // ... | send 63pp | recieve 720x | Wait until all slaves reply 73cc instead 720x
    kHandshake,       // send 64hh | Send handshake_data for final transfer completion
    kDelay16th,       // delay 1/16s
    kLenInfo,         // send llll | Send length information
    kPayload,         // send yyyy | Transfer main data block in units of 16 or 32 bits
    kChecksumFinalize,
    kCrcRequest,
    kCrcWait,
    kCrcSignal,
    kCrcExchange,

    kDone,
    kFailed
  };

  using ProgressCallback = void (*)(uint8_t percent, State state, void* user);
  using RunningCallback = void (*)(void* user);

  static constexpr uint32_t kHeaderBytes = 0xC0;

  GBAMultiBoot();

  void setPins(const Pins& pins);
  void setTiming(const Timing& timing);
  void setOptions(const Options& options);

  void setProgressCallback(ProgressCallback cb, void* user);
  void setRunningCallback(RunningCallback cb, void* user);

  void setDebugStream(Print* stream);

  bool begin();

  Result upload(const char* littlefs_path);

  Result start(const char* littlefs_path);
  Result update();
  void abort();

  bool isRunning() const;
  Result result() const;
  State state() const;

  uint8_t progressPercent() const;
  uint32_t bytesSent() const;
  uint32_t bytesTotal() const;

  uint32_t lastTx() const;
  uint32_t lastRx() const;

  static const char* resultName(Result r);
  static const char* stateName(State s);

 private:
  void configurePins_();
  bool ensureFsMounted_();
  bool openFile_(const char* path);

  uint32_t exchange32_(uint32_t output);
  uint32_t xfer32_(uint32_t out32);
  uint16_t xfer16_(uint16_t out16);

  void readFill_(uint8_t* buf, size_t n);
  void progressMaybe_();
  void fail_(Result r);

  void dbgState_(State s);
  void dbgFail_(Result r);
  void dbgWord_(const __FlashStringHelper* label, uint32_t v);

  Pins pins_{3, 4, 5, -1};
  Timing timing_{};
  Options options_{};

  ProgressCallback progress_cb_{nullptr};
  void* progress_user_{nullptr};
  RunningCallback running_cb_{nullptr};
  void* running_user_{nullptr};

  Print* debug_{nullptr};

  File file_;
  uint32_t raw_len_{0};
  uint32_t len_{0};
  uint32_t bytes_sent_{0};
  uint32_t bytes_total_{0};
  uint8_t last_percent_{255};

  State state_{State::kIdle};
  Result result_{Result::kOk};
  bool aborted_{false};
  bool fs_ready_{false};

  uint32_t start_ms_{0};
  uint32_t stage_start_ms_{0};
  uint32_t next_ms_{0};

  uint8_t init_tries_{0};

  uint8_t cc_{0};      // cc = random client_data[1..3] from slave 1-3, FFh if slave not exists
  uint8_t hh_{0};      // hh = handshake_data, 11h+client_data[1]+client_data[2]+client_data[3]
  uint8_t rr_{0};      // rr = random data from each slave for encryption, FFh if slave not exists
  uint16_t llll_{0};   // llll = download length/4-34h

  uint32_t c_{0};      // chksum
  uint32_t x_{0};      // chkxor
  uint32_t k_{0};      // keyxor
  uint32_t m_{0};      // keymul
  uint32_t fin_{0};    // chkfin

  uint32_t ptr_{0};    // ptr (00C0h..len)

  uint32_t last_tx_{0};
  uint32_t last_rx_{0};
};
