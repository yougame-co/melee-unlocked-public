// A QR code's modules for a short text (the sign-in panel's link), from Nayuki's qrcodegen
// (port/third_party/qrcodegen, MIT), which qr_code.cpp builds into every runtime.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace qr_code {

struct Matrix {
  int size = 0;                 // modules per side; 0 when the text could not be encoded
  std::vector<uint8_t> dark;    // size * size, row by row
  bool at(int x, int y) const { return x >= 0 && y >= 0 && x < size && y < size && dark[(size_t)y * size + x] != 0; }
};
// The text exactly as given, byte mode as needed, error correction medium or better (qrcodegen boosts
// it when the same version has room). Empty text, or text too long for a QR code: size 0.
Matrix encode(const std::string& text);

}  // namespace qr_code
