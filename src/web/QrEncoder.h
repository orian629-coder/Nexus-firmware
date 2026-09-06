#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nexus::web {

// Minimal, self-contained QR Code encoder (byte mode, ECC level M, versions 1–10). Enough for the
// short strings the speaker needs to show — a WIFI: join payload or a setup URL — with no external
// dependency, so the offline dashboard can render a scannable code as an SVG.
//
// encode() returns the module matrix (true = dark) including the quiet zone is NOT added here;
// toSvg() adds a 4-module quiet zone as the spec requires for reliable scanning.
class QrEncoder {
 public:
  // Encode `text` (UTF-8, up to ~150 bytes) into a QR module matrix. Throws std::runtime_error if
  // the text is too long for version 10 at ECC-M.
  static std::vector<std::vector<bool>> encode(const std::string& text);

  // Render a module matrix to a self-contained SVG string (black modules on white, 4-module quiet
  // zone). `px` is the pixel size of one module.
  static std::string toSvg(const std::vector<std::vector<bool>>& matrix, int px = 4);

  // Convenience: encode + render in one call.
  static std::string toSvg(const std::string& text, int px = 4) {
    return toSvg(encode(text), px);
  }
};

}  // namespace nexus::web
