#pragma once

#include <array>
#include <cstdint>

namespace hws::http {

// Character classes from RFC 9110 / RFC 9112, resolved with one table lookup per byte.
enum CharClass : uint8_t {
  kTchar = 1 << 0,       // token characters (method, header field-name)
  kFieldValue = 1 << 1,  // field-vchar / SP / HTAB / obs-text
  kTarget = 1 << 2,      // request-target bytes we accept (visible ASCII except '#')
  kHex = 1 << 3,
};

constexpr std::array<uint8_t, 256> make_char_table() {
  std::array<uint8_t, 256> t{};
  for (int c = 0; c < 256; ++c) {
    uint8_t v = 0;
    const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    const bool digit = c >= '0' && c <= '9';
    if (alpha || digit) v |= kTchar;
    switch (c) {
      case '!': case '#': case '$': case '%': case '&': case '\'': case '*': case '+':
      case '-': case '.': case '^': case '_': case '`': case '|': case '~':
        v |= kTchar;
        break;
      default:
        break;
    }
    if (c == '\t' || c == ' ' || (c >= 0x21 && c <= 0x7e) || c >= 0x80) v |= kFieldValue;
    if (c >= 0x21 && c <= 0x7e && c != '#') v |= kTarget;
    if (digit || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) v |= kHex;
    t[static_cast<size_t>(c)] = v;
  }
  return t;
}

inline constexpr std::array<uint8_t, 256> kCharTable = make_char_table();

inline bool is_tchar(char c) noexcept { return kCharTable[static_cast<unsigned char>(c)] & kTchar; }
inline bool is_field_value_char(char c) noexcept {
  return kCharTable[static_cast<unsigned char>(c)] & kFieldValue;
}
inline bool is_target_char(char c) noexcept { return kCharTable[static_cast<unsigned char>(c)] & kTarget; }
inline bool is_hex(char c) noexcept { return kCharTable[static_cast<unsigned char>(c)] & kHex; }
inline int hex_value(char c) noexcept {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace hws::http
