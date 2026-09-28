#pragma once
#include <stddef.h>
#include <stdint.h>
#include "core/arq_session.h"
#include "core/config_store.h"
#include "core/hdlc.h"
#include "core/transport.h"

namespace flr {

using ByteSink = void (*)(void*, const uint8_t*, size_t);

// Serial HDLC <-> L2 <-> radio bridge (M2). Serial bytes in via
// onSerialByte; framed serial bytes out via the injected sink, at most
// 256 B per tick (remaining bytes park in a static output ring sized
// for one worst-case wire frame plus headroom — a full ring means the
// host stopped draining; dropped bytes are counted in
// droppedOutputBytes()).
// The RNS wire format has NO type byte: CTRL is detected by a content
// heuristic (payload starts with `{"` and is < 96 B); everything else
// is DATA for the ARQ layer. CTRL is request/reply only; the bridge
// never originates unsolicited wire frames (decision 1).
class BridgeApp {
 public:
  // Context: setup. Blocking: never.
  BridgeApp(Transport& t, void* sinkCtx, ByteSink sink);
  // Context: setup. Registers the radio callback, starts the session.
  void begin(uint32_t nowMs);
  // Context: main-loop serial poll. Blocking: never.
  void onSerialByte(uint8_t b, uint32_t nowMs);
  // Context: main loop. Polls takeSendFailed, drives session timers,
  // emits one <=256 B chunk from the output ring.
  void tick(uint32_t nowMs);
  uint32_t droppedOutputBytes() const { return oDropped_; }

  // Register the persist callback (main does the EEPROM write).
  // Context: setup. Blocking: never. Failure: none.
  void onConfigPersist(void (*cb)(const Config&, void*), void* ctx);
  // Seed the live config (main loads EEPROM once at boot).
  // Context: setup, before serving requests. Blocking: never.
  void setConfig(const Config& c) { cfg_ = c; }
  // The live config; get/set mutations land here (main wiring owns
  // persistence via onConfigPersist). Context: pump/loop. Blocking: O(1).
  const Config& config() const { return cfg_; }

 private:
  static void onRadio_(void* ctx, const uint8_t* d, size_t n, int16_t);
  static void onDelivered_(void* ctx, const uint8_t* d, size_t n);
  static void onSerialFrame_(void* ctx, const uint8_t* p, size_t n);
  void emitFrame_(const uint8_t* payload, size_t n);
  void handleCtrl_(const uint8_t* payload, size_t n);
  void handleGetSet_(const char* buf);
  void replyRaw_(const char* s);
  Transport& t_;
  void* sinkCtx_;
  ByteSink sink_;
  ArqSession arq_;
  HdlcDeframer def_;
  Config cfg_{};
  void (*persistCb_)(const Config&, void*) = nullptr;
  void* persistCtx_ = nullptr;
  uint32_t nowMs_ = 0;
  uint8_t oring_[1300];
  size_t oHead_ = 0, oTail_ = 0;
  uint32_t oDropped_ = 0;
};

}  // namespace flr
