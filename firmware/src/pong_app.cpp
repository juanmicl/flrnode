#include "pong_app.h"
#include <stdio.h>
#include "m1_frame.h"

namespace flr {

PongApp::PongApp(Transport& t, void* sinkCtx, SinkFn sink)
    : t_(t), sinkCtx_(sinkCtx), sink_(sink) {}

void PongApp::begin(uint32_t nowMs) {
  startMs_ = nowMs;
  emit_("{\"t\":\"boot\",\"role\":\"pong\",\"fw\":\"m1\"}");
}

void PongApp::onFrame(const uint8_t* data, size_t len, int16_t /*rssi*/) {
  M1Decoded d;
  if (!m1Decode(data, len, &d) || d.type != M1_TYPE_PING) return;
  uint8_t out[M1_FRAME_LEN];
  if (!m1Encode(out, M1_TYPE_PONG, d.seq, d.ts)) return;
  if (t_.send(out, sizeof(out))) ++tx_;
  ++rx_;
  if (d.seq > highestSeq_) highestSeq_ = d.seq;
  lastRxMs_ = tickNowMs_;  // onFrame has no clock param; use last tick
}

void PongApp::tick(uint32_t nowMs) {
  tickNowMs_ = nowMs;
  if (rx_ > 0 && rx_ % 100 == 0 && rx_ != lastHeartbeatAt_) {
    lastHeartbeatAt_ = rx_;
    char line[96];
    snprintf(line, sizeof(line),
             "{\"t\":\"stats\",\"role\":\"pong\",\"rx\":%lu,\"tx\":%lu}",
             (unsigned long)rx_, (unsigned long)tx_);
    emit_(line);
  }
  const bool idleTimeout = rx_ > 0 && nowMs - lastRxMs_ >= 5000;
  const bool hardTimeout = nowMs - startMs_ >= 120000;
  if (!finished_ && rx_ > 0 && (idleTimeout || hardTimeout)) {
    finished_ = true;
    char line[112];
    snprintf(line, sizeof(line),
             "{\"t\":\"pong_final\",\"rx\":%lu,\"tx\":%lu,\"highest_seq\":%lu}",
             (unsigned long)rx_, (unsigned long)tx_,
             (unsigned long)highestSeq_);
    emit_(line);
  }
}

void PongApp::emit_(const char* line) { flr::emitLine(sinkCtx_, sink_, line); }

}  // namespace flr
