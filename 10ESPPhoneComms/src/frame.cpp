// frame.cpp — see frame.h.
#include "frame.h"

#include <string.h>

namespace frame {

const char* typeName(uint8_t t) {
  switch (t) {
    case T_TEXT:      return "TEXT";
    case T_VOICE:     return "VOICE";
    case T_PTT_BEGIN: return "PTT_BEGIN";
    case T_PTT_END:   return "PTT_END";
    case T_PING:      return "PING";
    case T_PONG:      return "PONG";
    case T_INFO:      return "INFO";
    default:          return "?";
  }
}

size_t encode(uint8_t type, const uint8_t* payload, uint16_t len,
              uint8_t* out, size_t cap) {
  if (!out || cap < HDR_SIZE + (size_t)len) return 0;
  if (len && !payload) return 0;
  out[0] = M1;
  out[1] = M2;
  out[2] = type;
  out[3] = (uint8_t)(len & 0xFF);
  out[4] = (uint8_t)(len >> 8);
  if (len) memcpy(out + HDR_SIZE, payload, len);
  return HDR_SIZE + len;
}

void Parser::begin(Handler h, void* ctx) { h_ = h; ctx_ = ctx; }

void Parser::feed(const uint8_t* data, size_t n) {
  for (size_t i = 0; i < n; i++) feed(data[i]);
}

void Parser::feed(uint8_t b) {
  if (pos_ == 0) {                       // hunting for first magic byte
    if (b == M1) buf_[pos_++] = b;
    else noise_++;
    return;
  }
  if (pos_ == 1) {                       // second magic byte
    if (b == M2) { buf_[pos_++] = b; return; }
    if (b == M1) { return; }             // still a candidate start
    reset(); noise_++;
    return;
  }
  buf_[pos_++] = b;                      // type / len lo / len hi
  if (pos_ < HDR_SIZE) return;

  uint16_t len = (uint16_t)(buf_[3] | (buf_[4] << 8));
  if (len > MAX_PAYLOAD) { reset(); noise_ += HDR_SIZE; return; }
  if (len == 0 || pos_ == HDR_SIZE + len) {   // zero-length or full payload
    if (h_) h_(ctx_, buf_[2], buf_ + HDR_SIZE, len);
    frames_++;
    reset();
  }
}

}  // namespace frame
