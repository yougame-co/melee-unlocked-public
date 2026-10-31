// Boot-only choice of disc callbacks, independent of the host's global disc and window.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "disc_image_reader.h"
#include "sysgen.h"

namespace host::sysgen {
inline Disc boot_disc(const std::string& mod_base_iso, DiscImageReader& base, const Disc& current,
                      size_t dol_size, bool (*verify_image)(const std::vector<uint8_t>&)) {
  if (mod_base_iso.empty()) return current;
  Disc disc;
  disc.verify_retail = [&base, dol_size, verify_image] {
    std::vector<uint8_t> image;
    return base.read_dol(dol_size, image) && verify_image(image);
  };
  disc.read = [&base](const std::string& name, size_t expected, std::vector<uint8_t>& out) {
    return base.read_file(name, expected, out);
  };
  return disc;
}
}  // namespace host::sysgen
