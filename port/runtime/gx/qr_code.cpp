// SPDX-License-Identifier: GPL-2.0-or-later
// qrcodegen's one source file is built here, so the runtime source globs (port/CMakeLists.txt,
// port/linux/full/CMakeLists.txt, port/mac/build_full.py) carry it into every build.
#include "../../third_party/qrcodegen/qrcodegen.cpp"

#include "qr_code.h"

namespace qr_code {

Matrix encode(const std::string& text) {
  Matrix m;
  if (text.empty()) return m;
  try {
    const qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(text.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
    m.size = qr.getSize();
    m.dark.assign((size_t)m.size * m.size, 0);
    for (int y = 0; y < m.size; ++y)
      for (int x = 0; x < m.size; ++x) m.dark[(size_t)y * m.size + x] = qr.getModule(x, y) ? 1 : 0;
  } catch (...) {
    m = Matrix{};
  }
  return m;
}

}  // namespace qr_code
