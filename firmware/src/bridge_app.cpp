#include "bridge_app.h"
#include <stdio.h>
#include <string.h>

namespace flr {

BridgeApp::BridgeApp(Transport& t, void* sinkCtx, ByteSink sink)
    : t_(t), sinkCtx_(sinkCtx), sink_(sink) {}

void BridgeApp::begin(uint32_t nowMs) {
  nowMs_ = nowMs;
  arq_.begin(&t_, nowMs);
  arq_.onDelivered(&BridgeApp::onDelivered_, this);
  def_.begin(&BridgeApp::onSerialFrame_, this);
  t_.onFrame(&BridgeApp::onRadio_, this);
}

void BridgeApp::onSerialFrame_(void* ctx, uint8_t type, const uint8_t* p,
                               size_t n) {
  auto* self = static_cast<BridgeApp*>(ctx);
  if (type == 0x01) {
    self->arq_.tx(p, n, self->nowMs_);  // counters carry rejection paths
  } else if (type == 0x02) {
    self->handleCtrl_(p, n);
  }
}

void BridgeApp::onRadio_(void* ctx, const uint8_t* d, size_t n, int16_t) {
  auto* self = static_cast<BridgeApp*>(ctx);
  self->arq_.onPdu(d, n, self->nowMs_);
}

void BridgeApp::onDelivered_(void* ctx, const uint8_t* d, size_t n) {
  auto* self = static_cast<BridgeApp*>(ctx);
  self->emitFrame_(0x01, d, n);
}

void BridgeApp::handleCtrl_(const uint8_t* payload, size_t n) {
  char buf[64] = {0};
  const size_t c = n < sizeof(buf) - 1 ? n : sizeof(buf) - 1;
  memcpy(buf, payload, c);
  if (strstr(buf, "\"cmd\":\"stats\"") != nullptr) {
    const ArqStats& s = arq_.stats();
    char line[384];  // pinned 14-key stats schema; worst case < 384 B
    snprintf(line, sizeof(line),
        "{\"stats\":{\"tx_frames\":%lu,\"tx_frags\":%lu,\"tx_retries\":%lu,"
        "\"tx_dropped\":%lu,\"tx_too_big\":%lu,\"tx_window_overflow\":%lu,"
        "\"meta_conflict\":%lu,\"rx_frags\":%lu,\"rx_delivered\":%lu,"
        "\"rx_dups\":%lu,\"rx_stale\":%lu,\"rx_window_overflow\":%lu,"
        "\"nak_sent\":%lu,\"ack_sent\":%lu}}",
        (unsigned long)s.txFrames, (unsigned long)s.txFrags,
        (unsigned long)s.txRetries, (unsigned long)s.txDropped,
        (unsigned long)s.txTooBig, (unsigned long)s.txWindowOverflow,
        (unsigned long)s.metaConflict, (unsigned long)s.rxFrags,
        (unsigned long)s.rxDelivered, (unsigned long)s.rxDups,
        (unsigned long)s.rxStale, (unsigned long)s.rxWindowOverflow,
        (unsigned long)s.nakSent, (unsigned long)s.ackSent);
    emitFrame_(0x02, (const uint8_t*)line, strlen(line));
    return;
  }
  const char err[] = "{\"err\":\"unknown_cmd\"}";
  emitFrame_(0x02, (const uint8_t*)err, sizeof(err) - 1);
}

void BridgeApp::emitFrame_(uint8_t innerType, const uint8_t* payload,
                           size_t n) {
  uint8_t wire[1060];
  const size_t w = hdlcFrame(innerType, payload, n, wire, sizeof(wire));
  if (w == 0) {
    oDropped_ += n;  // oversize for the wire (cannot happen via deframer)
    return;
  }
  const size_t used = (oHead_ >= oTail_)
      ? (oHead_ - oTail_) : (sizeof(oring_) - oTail_ + oHead_);
  if (used + w > sizeof(oring_) - 1) {
    oDropped_ += w;  // host not draining; counted, never silent
    return;
  }
  for (size_t i = 0; i < w; ++i) {
    oring_[oHead_] = wire[i];
    oHead_ = (oHead_ + 1) % sizeof(oring_);
  }
}

void BridgeApp::onSerialByte(uint8_t b, uint32_t nowMs) {
  nowMs_ = nowMs;
  def_.feed(b);
}

void BridgeApp::tick(uint32_t nowMs) {
  nowMs_ = nowMs;
  if (t_.takeSendFailed()) arq_.onTransportSendFailed(nowMs);
  arq_.tick(nowMs);
  if (sink_ && oHead_ != oTail_) {
    uint8_t chunk[256];
    size_t n = 0;
    while (oHead_ != oTail_ && n < sizeof(chunk)) {
      chunk[n++] = oring_[oTail_];
      oTail_ = (oTail_ + 1) % sizeof(oring_);
    }
    sink_(sinkCtx_, chunk, n);
  }
}

}  // namespace flr
