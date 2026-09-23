#include "ping_app.h"
#include <stdio.h>
#include "m1_frame.h"

#ifndef FLR_CHANNEL
#define FLR_CHANNEL 1
#endif
#ifndef FLR_DIST_M
#define FLR_DIST_M 0
#endif

namespace flr {

namespace {
constexpr uint32_t kIntervalMs = 100;
constexpr uint32_t kTotalFrames = 1000;
constexpr uint32_t kGraceMs = 5000;
}

PingApp::PingApp(Transport& t, void* sinkCtx, SinkFn sink)
    : t_(t), sinkCtx_(sinkCtx), sink_(sink) {
  for (uint32_t i = 0; i < kTotalFrames; ++i) rtt_[i] = 0;
  for (size_t i = 0; i < sizeof(seen_); ++i) seen_[i] = 0;
}

void PingApp::begin(uint32_t nowMs) {
  nextTxMs_ = nowMs;
  emit_("{\"t\":\"boot\",\"role\":\"ping\",\"fw\":\"m1\"}");
}

void PingApp::tick(uint32_t nowMs) {
  nowMs_ = nowMs;
  if (!done_ && tx_ < kTotalFrames && nowMs >= nextTxMs_) {
    uint8_t f[M1_FRAME_LEN];
    if (m1Encode(f, M1_TYPE_PING, tx_ + 1, nowMs) && t_.send(f, sizeof(f))) {
      ++tx_;
      lastTxMs_ = nowMs;
      if (tx_ % 100 == 0) {
        char line[224];  // worst case ~203 B with 20-digit %lu values
        snprintf(line, sizeof(line),
                 "{\"t\":\"stats\",\"seq\":%lu,\"tx\":%lu,\"rx\":%lu,"
                 "\"loss\":%lu,\"rtt_last\":%u,\"send_fail\":%lu,"
                 "\"ring_overflow\":%lu}",
                 (unsigned long)tx_, (unsigned long)tx_, (unsigned long)rx_,
                 (unsigned long)(tx_ - rx_), rttLast_,
                 (unsigned long)t_.stats().txFail,
                 (unsigned long)t_.stats().ringOverflow);
        emit_(line);
      }
    }
    nextTxMs_ += kIntervalMs;
  }
  if (!done_ && tx_ == kTotalFrames && nowMs - lastTxMs_ >= kGraceMs) {
    emitResult_();
    done_ = true;
  }
}

void PingApp::onFrame(const uint8_t* data, size_t len, int16_t /*rssi*/) {
  M1Decoded d;
  if (!m1Decode(data, len, &d) || d.type != M1_TYPE_PONG) return;
  if (d.seq == 0 || d.seq > kTotalFrames) return;
  const uint32_t bit = d.seq - 1;
  if (seen_[bit / 8] & (1u << (bit % 8))) return;  // late/duplicate
  seen_[bit / 8] |= (1u << (bit % 8));
  // Last tick as "now"; valid RTT < 65536 ms by construction (grace is 5 s).
  const uint16_t rtt = (uint16_t)(nowMs_ - d.ts);
  rttLast_ = rtt;
  insertRtt_(rtt);
  ++rx_;
}

void PingApp::insertRtt_(uint16_t ms) {
  // Insert ascending; array holds rx_ valid entries before this insert.
  // The clamp below is unreachable: bitmap dedup caps rx_ below
  // kTotalFrames.
  uint32_t i = (rx_ < kTotalFrames) ? rx_ : (kTotalFrames - 1);
  while (i > 0 && rtt_[i - 1] > ms) { rtt_[i] = rtt_[i - 1]; --i; }
  rtt_[i] = ms;
}

void PingApp::emitResult_() {
  const uint32_t loss = kTotalFrames - rx_;
  const size_t n = rx_ > 0 ? rx_ : 1;
  const uint16_t p50 = percentileOf(rtt_, n, 50);
  const uint16_t p95 = percentileOf(rtt_, n, 95);
  const uint16_t mx = rx_ > 0 ? rtt_[rx_ - 1] : 0;
  char line[256];  // worst case ~249 B with 20-digit %lu values
  snprintf(line, sizeof(line),
           "{\"t\":\"m1_result\",\"tx\":%lu,\"rx\":%lu,\"loss\":%lu,"
           "\"send_fail\":%lu,\"ring_overflow\":%lu,\"rtt_p50_ms\":%u,"
           "\"rtt_p95_ms\":%u,\"rtt_max_ms\":%u,\"channel\":%d,\"dist_m\":%d,"
           "\"verdict\":\"%s\"}",
           (unsigned long)kTotalFrames, (unsigned long)rx_,
           (unsigned long)loss, (unsigned long)t_.stats().txFail,
           (unsigned long)t_.stats().ringOverflow,
           p50, p95, mx, FLR_CHANNEL, FLR_DIST_M,
           loss == 0 ? "PASS" : "FAIL");
  emit_(line);
}

void PingApp::emit_(const char* line) { flr::emitLine(sinkCtx_, sink_, line); }

}  // namespace flr
