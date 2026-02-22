#include "GBAMultiBoot.h"

GBAMultiBoot::GBAMultiBoot() = default;

void GBAMultiBoot::setPins(const Pins& pins) { pins_ = pins; }
void GBAMultiBoot::setTiming(const Timing& timing) { timing_ = timing; }
void GBAMultiBoot::setOptions(const Options& options) { options_ = options; }

void GBAMultiBoot::setProgressCallback(ProgressCallback cb, void* user) {
  progress_cb_ = cb;
  progress_user_ = user;
}

void GBAMultiBoot::setRunningCallback(RunningCallback cb, void* user) {
  running_cb_ = cb;
  running_user_ = user;
}

void GBAMultiBoot::setDebugStream(Print* stream) { debug_ = stream; }

void GBAMultiBoot::dbgWord_(const __FlashStringHelper* label, uint32_t v) {
  if (!debug_) return;
  debug_->print(label);
  debug_->print(F(": 0x"));
  debug_->println(v, HEX);
}

void GBAMultiBoot::dbgState_(State s) {
  if (!debug_) return;
  debug_->print(F("[mb] state: "));
  debug_->println(stateName(s));
}

void GBAMultiBoot::dbgFail_(Result r) {
  if (!debug_) return;
  debug_->print(F("[mb] fail: "));
  debug_->print(resultName(r));
  debug_->print(F(" (state "));
  debug_->print(stateName(state_));
  debug_->println(F(")"));
  dbgWord_(F("[mb] last_tx"), last_tx_);
  dbgWord_(F("[mb] last_rx"), last_rx_);
}

bool GBAMultiBoot::ensureFsMounted_() {
  if (!options_.auto_mount_fs) return true;
  if (fs_ready_) return true;
  fs_ready_ = LittleFS.begin();
  return fs_ready_;
}

bool GBAMultiBoot::begin() {
  configurePins_();
  if (!ensureFsMounted_()) {
    result_ = Result::kFsBeginFailed;
    state_ = State::kFailed;
  dbgFail_(result_);
    return false;
  }
  return true;
}

GBAMultiBoot::Result GBAMultiBoot::upload(const char* littlefs_path) {
  Result r = start(littlefs_path);
  while (r == Result::kRunning) r = update();
  return r;
}

GBAMultiBoot::Result GBAMultiBoot::start(const char* littlefs_path) {
  aborted_ = false;
  result_ = Result::kRunning;
  state_ = State::kIdle;

  bytes_sent_ = 0;
  bytes_total_ = 0;
  last_percent_ = 255;

  start_ms_ = millis();
  stage_start_ms_ = start_ms_;
  next_ms_ = 0;
  init_tries_ = 0;

  last_tx_ = 0;
  last_rx_ = 0;

  if (!begin()) return result_;

  if (!openFile_(littlefs_path)) {
    fail_(Result::kFileOpenFailed);
    return result_;
  }

  raw_len_ = static_cast<uint32_t>(file_.size());
  if (raw_len_ < kHeaderBytes || raw_len_ > 0x40000u) {
    fail_(Result::kFileSizeInvalid);
    return result_;
  }

  len_ = (raw_len_ + 15u) & ~15u;

  const uint32_t payload_len = len_ - kHeaderBytes;
  if (payload_len < 0x100u || payload_len > 0x3FF40u || (payload_len & 0xFu) != 0) {
    fail_(Result::kFileSizeInvalid);
    return result_;
  }

  bytes_total_ = len_;
  ptr_ = 0;
  file_.seek(0);

  state_ = State::kWaitEnterMode;
  dbgState_(state_);
  return result_;
}

GBAMultiBoot::Result GBAMultiBoot::update() {
  if (result_ != Result::kRunning) return result_;

  if (aborted_) {
    fail_(Result::kAborted);
    return result_;
  }

  if (running_cb_) running_cb_(running_user_);

  const uint32_t now = millis();
  if (now < next_ms_) return result_;

  if (now - start_ms_ > timing_.init_timeout_ms && state_ <= State::kLenInfo) {
    fail_(Result::kTimeout);
    return result_;
  }

  // Times  Send   Receive   Expl.
  // ...    6200   FFFF      Slave not in multiplay/normal mode yet
  // 1      6200   0000      Slave entered correct mode now
  if (state_ == State::kWaitEnterMode) {
    const uint16_t r = xfer16_(0x6200);
    if (r == 0x0000) {
      state_ = State::kWait720x;
      stage_start_ms_ = now;
      init_tries_ = 0;
      dbgState_(state_);
      return result_;
    }
    next_ms_ = now + 1;
    return result_;
  }

  // Times  Send   Receive   Expl.
  // 15     6200   720x      Repeat 15 times, if failed: delay 1/16s and restart
  if (state_ == State::kWait720x) {
    const uint16_t r = xfer16_(0x6200);

    if ((r & 0xFFF0u) == 0x7200u) {
      const uint8_t x = static_cast<uint8_t>(r & 0x000Fu);
      if (options_.slave_bit == 0) options_.slave_bit = x;
      if (x != options_.slave_bit) {
        fail_(Result::kProtocolMismatch);
        return result_;
      }

      state_ = State::kExchangeInfo1;
      stage_start_ms_ = now;
      dbgState_(state_);
      return result_;
    }

    if (++init_tries_ >= 15u) {
      state_ = State::kWaitEnterMode;
      stage_start_ms_ = now;
      init_tries_ = 0;
      next_ms_ = now + timing_.delay_16th_ms;
      dbgState_(state_);
      return result_;
    }

    next_ms_ = now + timing_.delay_16th_ms;
    return result_;
  }

  // Times  Send   Receive   Expl.
  // 1      610y   720x      Recognition okay, exchange master/slave info
  if (state_ == State::kExchangeInfo1) {
    const uint16_t send = static_cast<uint16_t>(0x6100u | options_.slave_bit);
    const uint16_t r = xfer16_(send);
    if (r != static_cast<uint16_t>(0x7200u | options_.slave_bit)) {
      dbgWord_(F("expected"), static_cast<uint16_t>(0x7200u | options_.slave_bit));
      dbgWord_(F("got"), r);
      fail_(Result::kProtocolMismatch);
      return result_;
    }

    state_ = State::kHeader;
    stage_start_ms_ = now;
    ptr_ = 0;
    file_.seek(0);
    dbgState_(state_);
    return result_;
  }

  // Times  Send   Receive   Expl.
  // 60h    xxxx   NN0x      Transfer C0h bytes header data in units of 16bits
  if (state_ == State::kHeader) {
    uint16_t words = timing_.words_per_update;
    if (words == 0) words = 1;

    while (words--) {
      uint8_t b[2];
      readFill_(b, 2);
      const uint16_t xxxx = static_cast<uint16_t>(b[0]) | (static_cast<uint16_t>(b[1]) << 8);
      const uint16_t r = xfer16_(xxxx);

      const uint8_t nn = static_cast<uint8_t>((r >> 8) & 0xFFu);
      const uint8_t x = static_cast<uint8_t>(r & 0xFFu);
      const uint8_t expectedNN = static_cast<uint8_t>((kHeaderBytes - ptr_) / 2u);

      if (nn != expectedNN || x != options_.slave_bit) {
        dbgWord_(F("[mb] expected_nn"), expectedNN);
        dbgWord_(F("[mb] got_nn"), nn);
        dbgWord_(F("[mb] expected_x"), options_.slave_bit);
        dbgWord_(F("[mb] got_x"), x);
        fail_(Result::kProtocolMismatch);
        return result_;
      }

      ptr_ += 2;
      bytes_sent_ = ptr_;
      progressMaybe_();

      if (ptr_ >= kHeaderBytes) {
        state_ = State::kHeaderDone;
        stage_start_ms_ = now;
        dbgState_(state_);
        break;
      }
    }

    return result_;
  }

  // Times  Send   Receive   Expl.
  // 1      6200   000x      Transfer of header data completed
  if (state_ == State::kHeaderDone) {
    const uint16_t r = xfer16_(0x6200);
    if (r != options_.slave_bit) {
      dbgWord_(F("[mb] expected_x"), options_.slave_bit);
      dbgWord_(F("[mb] got"), r);
      fail_(Result::kProtocolMismatch);
      return result_;
    }

    state_ = State::kExchangeInfo2;
    stage_start_ms_ = now;
    dbgState_(state_);
    return result_;
  }

  // Times  Send   Receive   Expl.
  // 1      620y   720x      Exchange master/slave info again
  if (state_ == State::kExchangeInfo2) {
    const uint16_t send = static_cast<uint16_t>(0x6200u | options_.slave_bit);
    const uint16_t r = xfer16_(send);
    if (r != static_cast<uint16_t>(0x7200u | options_.slave_bit)) {
      dbgWord_(F("[mb] expected"), static_cast<uint16_t>(0x7200u | options_.slave_bit));
      dbgWord_(F("[mb] got"), r);
      fail_(Result::kProtocolMismatch);
      return result_;
    }

    state_ = State::kPaletteWait;
    stage_start_ms_ = now;
    dbgState_(state_);
    return result_;
  }

  // Times  Send   Receive   Expl.
  // ...    63pp   720x      Wait until all slaves reply 73cc instead 720x
  // 1      63pp   73cc      Send palette_data and receive client_data[1-3]
  if (state_ == State::kPaletteWait) {
    if (now - stage_start_ms_ > timing_.init_timeout_ms) {
      fail_(Result::kTimeout);
      return result_;
    }

    const uint16_t send = static_cast<uint16_t>(0x6300u | options_.palette_data);
    const uint16_t r = xfer16_(send);
    if ((r & 0xFF00u) != 0x7300u) return result_;

    cc_ = static_cast<uint8_t>(r & 0xFFu);
    hh_ = static_cast<uint8_t>((0x11u + cc_ + 0xFFu + 0xFFu) & 0xFFu);

    state_ = State::kHandshake;
    stage_start_ms_ = now;
    dbgState_(state_);
    dbgWord_(F("cc"), cc_);
    dbgWord_(F("hh"), hh_);
    return result_;
  }

  // Times  Send   Receive   Expl.
  // 1      64hh   73uu      Send handshake_data for final transfer completion
  if (state_ == State::kHandshake) {
    const uint16_t send = static_cast<uint16_t>(0x6400u | hh_);
    const uint16_t r = xfer16_(send);
    if ((r & 0xFF00u) != 0x7300u) {
      dbgWord_(F("[mb] expected_prefix"), 0x7300u);
      dbgWord_(F("[mb] got"), r);

      return result_;
    }

    state_ = State::kDelay16th;
    stage_start_ms_ = now;
    next_ms_ = now + timing_.delay_16th_ms;
    dbgState_(state_);
    return result_;
  }

  // Times  Send   Receive   Expl.
  // DELAY  -      -         Wait 1/16 seconds at master side
  if (state_ == State::kDelay16th) {
    state_ = State::kLenInfo;
    stage_start_ms_ = now;
    dbgState_(state_);
    return result_;
  }

  // Times  Send   Receive   Expl.
  // 1      llll   73rr      Send length information and receive random data[1-3]
  if (state_ == State::kLenInfo) {
    llll_ = static_cast<uint16_t>((len_ - kHeaderBytes) / 4u - 0x34u);
    const uint16_t r = xfer16_(llll_);
    if ((r & 0xFF00u) != 0x7300u) {
      dbgWord_(F("[mb] expected_prefix"), 0x7300u);
      dbgWord_(F("[mb] got"), r);
      fail_(Result::kProtocolMismatch);
      return result_;
    }

    rr_ = static_cast<uint8_t>(r & 0xFFu);

    c_ = 0xC387u;
    x_ = 0xC37Bu;
    k_ = 0x43202F2Fu;

    m_ = 0xFFFF0000u | (static_cast<uint32_t>(cc_) << 8) | static_cast<uint32_t>(options_.palette_data);
    fin_ = 0xFFFF0000u | (static_cast<uint32_t>(rr_) << 8) | static_cast<uint32_t>(hh_);

    ptr_ = kHeaderBytes;
    file_.seek(kHeaderBytes);

    state_ = State::kPayload;
    stage_start_ms_ = now;
    dbgState_(state_);
    dbgWord_(F("[mb] llll"), llll_);
    dbgWord_(F("[mb] rr"), rr_);
    return result_;
  }

  // Times  Send   Receive   Expl.
  // LEN    yyyy   nnnn      Transfer main data block in units of 16 or 32 bits
  if (state_ == State::kPayload) {
    uint16_t words = timing_.words_per_update;
    if (words == 0) words = 1;

    while (words--) {
      uint8_t b[4];
      readFill_(b, 4);
      const uint32_t data = static_cast<uint32_t>(b[0]) |
                            (static_cast<uint32_t>(b[1]) << 8) |
                            (static_cast<uint32_t>(b[2]) << 16) |
                            (static_cast<uint32_t>(b[3]) << 24);

      // c=c xor data[ptr]:for i=1 to 32:c=c shr 1:if carry then c=c xor x:next
      c_ ^= data;
      for (int bit = 0; bit < 32; ++bit) {
        const uint32_t carry = c_ & 1u;
        c_ >>= 1;
        if (carry) c_ ^= x_;
      }

      // m=(6F646573h*m)+1
      m_ = (0x6F646573u * m_ + 1u) & 0xFFFFFFFFu;

      // send_32_or_2x16 (data[ptr] xor (-2000000h-ptr) xor m xor k)
      const uint32_t complement = static_cast<uint32_t>(-0x02000000) - ptr_;
      const uint32_t yyyy = data ^ complement ^ m_ ^ k_;

      const uint32_t resp = xfer32_(yyyy);
      if (static_cast<uint16_t>(resp >> 16) != static_cast<uint16_t>(ptr_ & 0xFFFFu)) {
        dbgWord_(F("[mb] expected_ptr"), static_cast<uint16_t>(ptr_ & 0xFFFFu));
        dbgWord_(F("[mb] got"), static_cast<uint16_t>(resp >> 16));
        fail_(Result::kProtocolMismatch);
        return result_;
      }

      ptr_ += 4;
      bytes_sent_ = ptr_;
      progressMaybe_();

      if (timing_.yield_every_bytes && (ptr_ % timing_.yield_every_bytes) == 0) {
        delay(timing_.yield_delay_ms);
        yield();
      }

      if (ptr_ >= len_) {
        file_.close();
        state_ = State::kChecksumFinalize;
        stage_start_ms_ = now;
        dbgState_(state_);
        break;
      }
    }

    return result_;
  }

  // c=c xor f:for i=1 to 32:c=c shr 1:if carry then c=c xor x:next
  if (state_ == State::kChecksumFinalize) {
    c_ ^= fin_;
    for (int bit = 0; bit < 32; ++bit) {
      const uint32_t carry = c_ & 1u;
      c_ >>= 1;
      if (carry) c_ ^= x_;
    }
    state_ = State::kCrcRequest;
    stage_start_ms_ = now;
    dbgState_(state_);
    return result_;
  }

  // Times  Send   Receive   Expl.
  // 1      0065   nnnn      Transfer of main data block completed, request CRC
  if (state_ == State::kCrcRequest) {
    const uint16_t r = xfer16_(0x0065);
    if (r != static_cast<uint16_t>(len_ & 0xFFFFu)) {
      dbgWord_(F("[mb] expected"), static_cast<uint16_t>(len_ & 0xFFFFu));
      dbgWord_(F("[mb] got"), r);
      fail_(Result::kProtocolMismatch);
      return result_;
    }
    state_ = State::kCrcWait;
    stage_start_ms_ = now;
    dbgState_(state_);
    return result_;
  }

  // Times  Send   Receive   Expl.
  // ...    0065   0074      Wait until all slaves reply 0075 instead 0074
  // 1      0065   0075      All slaves ready for CRC transfer
  if (state_ == State::kCrcWait) {
    if (now - stage_start_ms_ > timing_.init_timeout_ms) {
      fail_(Result::kTimeout);
      return result_;
    }

    const uint16_t r = xfer16_(0x0065);
    if (r == 0x0074) return result_;
    if (r != 0x0075) {
      fail_(Result::kProtocolMismatch);
      return result_;
    }

    state_ = State::kCrcSignal;
    stage_start_ms_ = now;
    dbgState_(state_);
    return result_;
  }

  // Times  Send   Receive   Expl.
  // 1      0066   0075      Signalize that transfer of CRC follows
  if (state_ == State::kCrcSignal) {
    const uint16_t r = xfer16_(0x0066);
    if (r != 0x0075) {
      dbgWord_(F("[mb] expected"), 0x0075);
      dbgWord_(F("[mb] got"), r);
      fail_(Result::kProtocolMismatch);
      return result_;
    }
    state_ = State::kCrcExchange;
    stage_start_ms_ = now;
    dbgState_(state_);
    return result_;
  }

  // Times  Send   Receive   Expl.
  // 1      zzzz   zzzz      Exchange CRC must be same for master and slaves
  if (state_ == State::kCrcExchange) {
    const uint16_t zzzz = static_cast<uint16_t>(c_ & 0xFFFFu);
    const uint16_t r = xfer16_(zzzz);
    if (r != zzzz) {
      dbgWord_(F("[mb] expected"), zzzz);
      dbgWord_(F("[mb] got"), r);
      fail_(Result::kProtocolMismatch);
      return result_;
    }
    result_ = Result::kOk;
    state_ = State::kDone;
    progressMaybe_();
    dbgState_(state_);
    return result_;
  }

  fail_(Result::kProtocolMismatch);
  return result_;
}

void GBAMultiBoot::abort() { aborted_ = true; }

bool GBAMultiBoot::isRunning() const { return result_ == Result::kRunning; }
GBAMultiBoot::Result GBAMultiBoot::result() const { return result_; }
GBAMultiBoot::State GBAMultiBoot::state() const { return state_; }

uint8_t GBAMultiBoot::progressPercent() const { return last_percent_ == 255 ? 0 : last_percent_; }
uint32_t GBAMultiBoot::bytesSent() const { return bytes_sent_; }
uint32_t GBAMultiBoot::bytesTotal() const { return bytes_total_; }

uint32_t GBAMultiBoot::lastTx() const { return last_tx_; }
uint32_t GBAMultiBoot::lastRx() const { return last_rx_; }

void GBAMultiBoot::configurePins_() {
  pinMode(pins_.sc, OUTPUT);
  pinMode(pins_.si, OUTPUT);

  if (options_.so_pullup) {
    pinMode(pins_.so, INPUT_PULLUP);
  } else {
    pinMode(pins_.so, INPUT);
  }

  digitalWrite(pins_.si, HIGH);
  digitalWrite(pins_.sc, HIGH);

  if (pins_.sd >= 0) {
    pinMode(static_cast<uint8_t>(pins_.sd), INPUT);
  }
}

bool GBAMultiBoot::openFile_(const char* path) {
  file_.close();
  file_ = LittleFS.open(path, "r");
  return static_cast<bool>(file_);
}

uint32_t GBAMultiBoot::exchange32_(uint32_t output) {
  uint32_t input = 0;
  for (int i = 31; i >= 0; --i) {
    digitalWrite(pins_.sc, LOW);
    digitalWrite(pins_.si, (output & (1u << i)) ? HIGH : LOW);
    delayMicroseconds(timing_.bit_delay_us);
    digitalWrite(pins_.sc, HIGH);
    input |= (digitalRead(pins_.so) == HIGH ? 1u : 0u) << i;
    delayMicroseconds(timing_.bit_delay_us);
  }
  digitalWrite(pins_.si, HIGH);
  return input;
}

uint32_t GBAMultiBoot::xfer32_(uint32_t out32) {
  last_tx_ = out32;
  last_rx_ = exchange32_(out32);
  if (timing_.word_gap_us) delayMicroseconds(timing_.word_gap_us);
  return last_rx_;
}

uint16_t GBAMultiBoot::xfer16_(uint16_t out16) {
  last_tx_ = static_cast<uint32_t>(out16);
  last_rx_ = exchange32_(static_cast<uint32_t>(out16));
  const uint16_t r = static_cast<uint16_t>(last_rx_ >> 16);
  if (timing_.word_gap_us) delayMicroseconds(timing_.word_gap_us);
  return r;
}

void GBAMultiBoot::readFill_(uint8_t* buf, size_t n) {
  const size_t got = file_.read(buf, n);
  for (size_t i = got; i < n; ++i) buf[i] = 0;
}

void GBAMultiBoot::progressMaybe_() {
  if (!progress_cb_ || bytes_total_ == 0) return;
  const uint8_t p = static_cast<uint8_t>((static_cast<uint64_t>(bytes_sent_) * 100u) / bytes_total_);
  if (p != last_percent_) {
    last_percent_ = p;
    progress_cb_(p, state_, progress_user_);
  }
}

void GBAMultiBoot::fail_(Result r) {
  result_ = r;
  state_ = (r == Result::kOk) ? State::kDone : State::kFailed;
  file_.close();
  dbgFail_(r);
}

const char* GBAMultiBoot::resultName(Result r) {
  switch (r) {
    case Result::kOk: return "ok";
    case Result::kRunning: return "running";
    case Result::kFsBeginFailed: return "fs_begin_failed";
    case Result::kFileOpenFailed: return "file_open_failed";
    case Result::kFileSizeInvalid: return "file_size_invalid";
    case Result::kTimeout: return "timeout";
    case Result::kProtocolMismatch: return "protocol_mismatch";
    case Result::kAborted: return "aborted";
    default: return "unknown";
  }
}

const char* GBAMultiBoot::stateName(State s) {
  switch (s) {
    case State::kIdle: return "idle";
    case State::kWaitEnterMode: return "wait_enter_mode";
    case State::kWait720x: return "wait_720x";
    case State::kExchangeInfo1: return "exchange_info_1";
    case State::kHeader: return "header";
    case State::kHeaderDone: return "header_done";
    case State::kExchangeInfo2: return "exchange_info_2";
    case State::kPaletteWait: return "palette_wait";
    case State::kHandshake: return "handshake";
    case State::kDelay16th: return "delay_16th";
    case State::kLenInfo: return "len_info";
    case State::kPayload: return "payload";
    case State::kChecksumFinalize: return "checksum_finalize";
    case State::kCrcRequest: return "crc_request";
    case State::kCrcWait: return "crc_wait";
    case State::kCrcSignal: return "crc_signal";
    case State::kCrcExchange: return "crc_exchange";
    case State::kDone: return "done";
    case State::kFailed: return "failed";
    default: return "unknown";
  }
}
