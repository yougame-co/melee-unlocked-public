// Independent, lazy disc reader for boot assets. Never opens the host's active disc or its caches.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace host {
class DiscImageReader {
 public:
  explicit DiscImageReader(const std::string& path) : path_(path) {}
  ~DiscImageReader() { if (file_) std::fclose(file_); }
  DiscImageReader(const DiscImageReader&) = delete;
  DiscImageReader& operator=(const DiscImageReader&) = delete;

  bool find_file(const std::string& name, uint32_t* offset, uint32_t* size) {
    if (!open()) return false;
    if (fst_.empty()) {
      if (fst_size_ < 12 || !range(fst_offset_, fst_size_)) return false;
      fst_.resize(fst_size_);
      if (!read_at(fst_offset_, fst_.data(), fst_.size())) { fst_.clear(); return false; }
    }
    const uint32_t entries = be32(fst_.data() + 8);
    if (!entries || entries > fst_.size() / 12) return false;
    const size_t strings = size_t(entries) * 12;
    // Match the active disc reader: all directories, exact name before case-insensitive name.
    for (int pass = 0; pass < 2; ++pass) {
      for (uint32_t i = 1; i < entries; ++i) {
        const auto* entry = fst_.data() + size_t(i) * 12;
        const uint32_t a = be32(entry);
        if (a >> 24) continue;
        const size_t so = strings + (a & 0xFFFFFFu);
        if (so >= fst_.size()) continue;
        const char* n = reinterpret_cast<const char*>(fst_.data() + so);
        const char* end = static_cast<const char*>(std::memchr(n, 0, fst_.size() - so));
        if (!end || size_t(end - n) != name.size()) continue;
        bool match = true;
        for (size_t j = 0; j < name.size(); ++j) {
          const auto lhs = static_cast<unsigned char>(n[j]);
          const auto rhs = static_cast<unsigned char>(name[j]);
          if (pass == 0 ? lhs != rhs : std::tolower(lhs) != std::tolower(rhs)) { match = false; break; }
        }
        if (!match) continue;
        const uint32_t off = be32(entry + 4), bytes = be32(entry + 8);
        if (!range(off, bytes)) return false;
        if (offset) *offset = off;
        if (size) *size = bytes;
        return true;
      }
    }
    return false;
  }

  bool read_file(const std::string& name, size_t expected, std::vector<uint8_t>& out) {
    uint32_t offset = 0, size = 0;
    if (!find_file(name, &offset, &size) || size != expected) return false;
    return read_bytes(offset, size, out);
  }
  bool read_dol(size_t expected, std::vector<uint8_t>& out) {
    return open() && read_bytes(dol_offset_, expected, out);
  }

 private:
  static uint32_t be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
  }
  static bool seek(FILE* f, uint64_t offset, int origin) {
#ifdef _WIN32
    return _fseeki64(f, static_cast<long long>(offset), origin) == 0;
#else
    return fseeko(f, static_cast<off_t>(offset), origin) == 0;
#endif
  }
  bool range(uint64_t offset, size_t bytes) const {
    return offset <= length_ && bytes <= length_ - offset;
  }
  bool read_at(uint64_t offset, void* dst, size_t bytes) {
    return range(offset, bytes) && seek(file_, offset, SEEK_SET) && std::fread(dst, 1, bytes, file_) == bytes;
  }
  bool read_bytes(uint32_t offset, size_t bytes, std::vector<uint8_t>& out) {
    if (!range(offset, bytes)) return false;
    std::vector<uint8_t> data(bytes);
    if (!read_at(offset, data.data(), data.size())) return false;
    out = std::move(data);
    return true;
  }
  bool open() {
    if (attempted_) return ready_;
    attempted_ = true;
    file_ = std::fopen(path_.c_str(), "rb");
    if (!file_ || !seek(file_, 0, SEEK_END)) return false;
#ifdef _WIN32
    const auto length = _ftelli64(file_);
#else
    const auto length = ftello(file_);
#endif
    if (length < 0) return false;
    length_ = static_cast<uint64_t>(length);
    uint8_t header[0x440];
    if (!read_at(0, header, sizeof header)) return false;
    dol_offset_ = be32(header + 0x420);
    fst_offset_ = be32(header + 0x424);
    fst_size_ = be32(header + 0x428);
    ready_ = true;
    return true;
  }
  std::string path_;
  FILE* file_ = nullptr;
  bool attempted_ = false, ready_ = false;
  uint64_t length_ = 0;
  uint32_t dol_offset_ = 0, fst_offset_ = 0, fst_size_ = 0;
  std::vector<uint8_t> fst_;
};
}  // namespace host
