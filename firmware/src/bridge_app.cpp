#include "bridge_app.h"
#include <stdio.h>
#include <stdlib.h>
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

void BridgeApp::onSerialFrame_(void* ctx, const uint8_t* p, size_t n) {
  auto* self = static_cast<BridgeApp*>(ctx);
  // CTRL detection heuristic (the RNS wire format has no type byte):
  // a JSON-looking frame (`{"` prefix) at control-message size whose
  // every byte is printable ASCII (0x20..0x7E) — binary payloads that
  // merely start with `{"` fall through to the DATA path. ONE trailing
  // '\n' is exempt from the scan (§8: requests are \n-terminated;
  // handleCtrl_ trims it).
  size_t plen = n;
  if (plen > 0 && p[plen - 1] == '\n') --plen;
  bool printable = true;
  for (size_t i = 0; i < plen; ++i) {
    if (p[i] < 0x20 || p[i] > 0x7E) { printable = false; break; }
  }
  if (n >= 2 && n < 96 && printable && p[0] == '{' && p[1] == '"') {
    self->handleCtrl_(p, n);
  } else {
    self->arq_.tx(p, n, self->nowMs_);  // counters carry rejection paths
  }
}

void BridgeApp::onRadio_(void* ctx, const uint8_t* d, size_t n, int16_t) {
  auto* self = static_cast<BridgeApp*>(ctx);
  self->arq_.onPdu(d, n, self->nowMs_);
}

void BridgeApp::onDelivered_(void* ctx, const uint8_t* d, size_t n) {
  auto* self = static_cast<BridgeApp*>(ctx);
  self->emitFrame_(d, n);
}

void BridgeApp::handleCtrl_(const uint8_t* payload, size_t n) {
  char buf[64] = {0};
  const size_t c = n < sizeof(buf) - 1 ? n : sizeof(buf) - 1;
  memcpy(buf, payload, c);
  // One trailing '\n' tolerated and trimmed (requests may arrive
  // \n-terminated, as in protocol.md's examples).
  size_t len = c;
  if (len > 0 && buf[len - 1] == '\n') buf[--len] = '\0';
  // strict section-8 example format, no whitespace tolerance
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
    emitFrame_((const uint8_t*)line, strlen(line));
    return;
  }
  if (strstr(buf, "\"cmd\":\"get\"") != nullptr ||
      strstr(buf, "\"cmd\":\"set\"") != nullptr) {
    handleGetSet_(buf);
    return;
  }
  const char err[] = "{\"err\":\"unknown_cmd\"}";
  emitFrame_((const uint8_t*)err, sizeof(err) - 1);
}

void BridgeApp::onConfigPersist(void (*cb)(const Config&, void*),
                                void* ctx) {
  persistCb_ = cb;
  persistCtx_ = ctx;
}

void BridgeApp::replyRaw_(const char* s) {
  emitFrame_((const uint8_t*)s, strlen(s));
}

void BridgeApp::handleGetSet_(const char* buf) {
  // Extract "key":"<name>" from the bounded, strict-format copy.
  const char* k = strstr(buf, "\"key\":\"");
  if (k == nullptr) { replyRaw_("{\"err\":\"unknown_key\"}"); return; }
  k += 7;
  char key[16] = {0};
  size_t ki = 0;
  while (k[ki] != '"' && k[ki] != '\0' && ki < sizeof(key) - 1) {
    key[ki] = k[ki];
    ++ki;
  }
  key[ki] = '\0';
  const bool isSet = strstr(buf, "\"cmd\":\"set\"") != nullptr;
  if (!isSet) {
    // GET: channel/tx_power/stats_period report their live value; pmk is
    // write-only; rate is transport-bound (unsupported on ESP-NOW).
    if (strcmp(key, "channel") == 0) {
      char r[24];
      snprintf(r, sizeof(r), "{\"channel\":%u}", (unsigned)cfg_.channel);
      replyRaw_(r);
    } else if (strcmp(key, "tx_power") == 0) {
      char r[28];
      snprintf(r, sizeof(r), "{\"tx_power\":%u}", (unsigned)cfg_.txPower);
      replyRaw_(r);
    } else if (strcmp(key, "stats_period") == 0) {
      char r[32];
      snprintf(r, sizeof(r), "{\"stats_period\":%u}", (unsigned)cfg_.statsPeriod);
      replyRaw_(r);
    } else if (strcmp(key, "pmk") == 0) {
      replyRaw_("{\"err\":\"write_only\"}");
    } else if (strcmp(key, "rate") == 0) {
      replyRaw_("{\"err\":\"unsupported_transport\"}");
    } else {
      replyRaw_("{\"err\":\"unknown_key\"}");
    }
    return;
  }
  // SET: "val":<num> or "val":"<32 hex chars>".
  const char* v = strstr(buf, "\"val\":");
  if (v == nullptr) { replyRaw_("{\"err\":\"bad_value\"}"); return; }
  v += 6;
  if (strcmp(key, "channel") == 0) {
    // Regulatory domain is a HARD boundary: reject, never wrap around.
    const long val = strtol(v, nullptr, 10);
    if (val < ConfigStore::kMinChannel || val > ConfigStore::kMaxChannel) {
      replyRaw_("{\"err\":\"bad_value\"}");
      return;
    }
    cfg_.channel = (uint8_t)val;
  } else if (strcmp(key, "tx_power") == 0) {
    // EIRP cap clamps DOWN, never rejects (compliance, never above 17).
    long val = strtol(v, nullptr, 10);
    if (val < 0) val = 0;
    if (val > ConfigStore::kMaxPower) val = ConfigStore::kMaxPower;
    cfg_.txPower = (uint8_t)val;
  } else if (strcmp(key, "stats_period") == 0) {
    long val = strtol(v, nullptr, 10);
    if (val <= 0 || val > 3600) val = 5;  // same default as load()
    cfg_.statsPeriod = (uint16_t)val;
  } else if (strcmp(key, "pmk") == 0) {
    if (v[0] != '"') { replyRaw_("{\"err\":\"bad_value\"}"); return; }
    ++v;
    uint8_t pmk[16];
    for (int i = 0; i < 16; ++i) {
      const char hex[3] = {v[2 * i], v[2 * i + 1], '\0'};
      if (hex[0] == '\0' || hex[1] == '\0') {
        replyRaw_("{\"err\":\"bad_value\"}");
        return;
      }
      const long byte = strtol(hex, nullptr, 16);
      pmk[i] = (uint8_t)byte;
    }
    memcpy(cfg_.pmk, pmk, 16);
  } else if (strcmp(key, "rate") == 0) {
    replyRaw_("{\"err\":\"unsupported_transport\"}");
    return;
  } else {
    replyRaw_("{\"err\":\"unknown_key\"}");
    return;
  }
  replyRaw_("{\"ok\":true}");
  if (persistCb_) persistCb_(cfg_, persistCtx_);
}

void BridgeApp::emitFrame_(const uint8_t* payload, size_t n) {
  uint8_t wire[1060];  // ~1 KB transient stack — fine in the 4 KB loop stack
  const size_t w = hdlcFrame(payload, n, wire, sizeof(wire));
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
