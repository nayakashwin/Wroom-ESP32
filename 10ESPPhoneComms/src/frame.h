// frame.h — end-to-end wire frames: A5 5A <type> <len16le> <payload>.
//
// One framing runs the whole path: Android app <-> ESP32 (SPP byte stream)
// <-> ESP32 <-> Android app. The ESP32s parse SPP bytes into frames to route
// them (reliable blob vs best-effort voice) and re-serialize toward the peer
// phone; they never inspect payload contents.
#ifndef FRAME_H
#define FRAME_H

#include <stddef.h>
#include <stdint.h>

namespace frame {

enum Type : uint8_t {
  T_TEXT      = 0x01,  // utf-8 text message          -> reliable ARQ blob
  T_VOICE     = 0x02,  // codec chunk (phones codec)  -> best-effort
  T_PTT_BEGIN = 0x03,  // talker starts               -> best-effort
  T_PTT_END   = 0x04,  // talker stops                -> best-effort
  T_PING      = 0x05,  // u32 timestamp               -> best-effort
  T_PONG      = 0x06,  // u32 timestamp echo          -> best-effort
  T_INFO      = 0x07,  // key=value status lines      -> reliable blob
};

static const uint8_t  M1          = 0xA5;
static const uint8_t  M2          = 0x5A;
static const size_t   HDR_SIZE    = 5;
static const uint16_t MAX_PAYLOAD = 1024;   // matches e2e::sendBlob ceiling

const char* typeName(uint8_t t);

// Serialize one frame into `out` (cap >= HDR_SIZE + len). Returns total bytes
// written, or 0 on bad input / no room.
size_t encode(uint8_t type, const uint8_t* payload, uint16_t len,
              uint8_t* out, size_t cap);

typedef void (*Handler)(void* ctx, uint8_t type,
                        const uint8_t* payload, uint16_t len);

// Byte-stream resynchronizing parser: feed it anything, complete frames come
// out via the handler (loop context, never ISR). Non-frame noise is skipped.
class Parser {
 public:
  Parser() {}
  void begin(Handler h, void* ctx);
  void feed(const uint8_t* data, size_t n);
  void feed(uint8_t b);
  uint32_t noise() const { return noise_; }   // bytes dropped while hunting
  uint32_t frames() const { return frames_; }

 private:
  void reset() { pos_ = 0; }
  Handler   h_   = nullptr;
  void*     ctx_ = nullptr;
  uint8_t   buf_[HDR_SIZE + MAX_PAYLOAD];
  uint16_t  pos_ = 0;
  uint32_t  noise_ = 0, frames_ = 0;
};

}  // namespace frame

#endif  // FRAME_H
