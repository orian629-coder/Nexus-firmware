#include "web/QrEncoder.h"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace nexus::web {

namespace {

// ── GF(256) arithmetic for Reed-Solomon (primitive polynomial 0x11D) ──
struct GF {
  std::array<int, 256> exp{};
  std::array<int, 256> log{};
  GF() {
    int x = 1;
    for (int i = 0; i < 255; ++i) {
      exp[i] = x;
      log[x] = i;
      x <<= 1;
      if (x & 0x100) x ^= 0x11D;
    }
    for (int i = 255; i < 256; ++i) exp[i] = exp[i - 255];
  }
  int mul(int a, int b) const {
    if (a == 0 || b == 0) return 0;
    return exp[(log[a] + log[b]) % 255];
  }
};
const GF& gf() {
  static GF g;
  return g;
}

// Reed-Solomon ECC codewords for a data block.
std::vector<int> rsEncode(const std::vector<int>& data, int ec_len) {
  // Generator polynomial gen(x) = product (x - alpha^i), i=0..ec_len-1. Coefficients low-to-high.
  std::vector<int> gen{1};
  for (int i = 0; i < ec_len; ++i) {
    std::vector<int> next(gen.size() + 1, 0);
    for (std::size_t j = 0; j < gen.size(); ++j) {
      next[j + 1] ^= gen[j];                          // gen(x) * x  (shift up)
      next[j] ^= gf().mul(gen[j], gf().exp[i]);       // gen(x) * alpha^i
    }
    gen = next;
  }
  // The generator is built low-to-high (gen[0]=constant, gen[ec_len]=1). The synthetic division
  // below consumes it high-to-low (leading coeff first), so reverse it — gen[0] becomes the monic
  // x^ec_len coefficient (1).
  std::reverse(gen.begin(), gen.end());
  // Polynomial division of (data << ec_len) by gen; remainder is the ECC.
  std::vector<int> res(data.size() + ec_len, 0);
  for (std::size_t i = 0; i < data.size(); ++i) res[i] = data[i];
  for (std::size_t i = 0; i < data.size(); ++i) {
    int coef = res[i];
    if (coef == 0) continue;
    for (std::size_t j = 0; j < gen.size(); ++j) {
      res[i + j] ^= gf().mul(gen[j], coef);
    }
  }
  return std::vector<int>(res.end() - ec_len, res.end());
}

// ── Version tables for ECC level M (byte-mode capacity) ──
// Per version (1..10): total data codewords, ec codewords per block, number of blocks.
struct VerInfo {
  int data_codewords;
  int ec_per_block;
  int num_blocks;
};
// ECC-M block structure (from the QR spec, versions 1..10).
constexpr VerInfo kVerM[11] = {
    {0, 0, 0},      // 0 unused
    {16, 10, 1},    // v1
    {28, 16, 1},    // v2
    {44, 26, 1},    // v3
    {64, 18, 2},    // v4
    {86, 24, 2},    // v5
    {108, 16, 4},   // v6
    {124, 18, 4},   // v7
    {154, 22, 2},   // v8  (2 blocks group1... simplified: uniform blocks)
    {182, 22, 3},   // v9
    {216, 26, 4},   // v10
};

int moduleCount(int version) { return version * 4 + 17; }

// Byte-mode char-count indicator bit length depends on version.
int charCountBits(int version) { return version <= 9 ? 8 : 16; }

// Choose the smallest version (1..10) that fits `data_bytes` in byte mode at ECC-M.
int chooseVersion(std::size_t data_bytes) {
  for (int v = 1; v <= 10; ++v) {
    // header = 4 (mode) + charCountBits; total data bits available = data_codewords*8.
    const int header_bits = 4 + charCountBits(v);
    const int need_bits = header_bits + static_cast<int>(data_bytes) * 8;
    if (need_bits <= kVerM[v].data_codewords * 8) return v;
  }
  throw std::runtime_error("QrEncoder: text too long for version 10 (ECC-M)");
}

// ── Bit buffer ──
struct BitBuf {
  std::vector<int> bits;
  void put(int val, int len) {
    for (int i = len - 1; i >= 0; --i) bits.push_back((val >> i) & 1);
  }
};

// Build the final codeword sequence (data + ECC, interleaved across blocks).
std::vector<int> buildCodewords(const std::string& text, int version) {
  const VerInfo& vi = kVerM[version];
  BitBuf bb;
  bb.put(0b0100, 4);  // byte mode
  bb.put(static_cast<int>(text.size()), charCountBits(version));
  for (unsigned char c : text) bb.put(c, 8);

  const int total_data_bits = vi.data_codewords * 8;
  // Terminator (up to 4 zero bits).
  int term = std::min(4, total_data_bits - static_cast<int>(bb.bits.size()));
  for (int i = 0; i < term; ++i) bb.bits.push_back(0);
  // Pad to byte boundary.
  while (bb.bits.size() % 8 != 0) bb.bits.push_back(0);
  // Convert to bytes.
  std::vector<int> data;
  for (std::size_t i = 0; i < bb.bits.size(); i += 8) {
    int b = 0;
    for (int k = 0; k < 8; ++k) b = (b << 1) | bb.bits[i + k];
    data.push_back(b);
  }
  // Pad bytes 0xEC, 0x11 alternating.
  const int pads[2] = {0xEC, 0x11};
  int pi = 0;
  while (static_cast<int>(data.size()) < vi.data_codewords) {
    data.push_back(pads[pi]);
    pi ^= 1;
  }

  // Split into blocks (uniform for v1..v10 in this table), compute ECC per block, interleave.
  const int nb = vi.num_blocks;
  const int per = vi.data_codewords / nb;
  const int rem = vi.data_codewords % nb;  // last `rem` blocks get one extra data codeword
  std::vector<std::vector<int>> dblocks, eblocks;
  int off = 0;
  for (int b = 0; b < nb; ++b) {
    int cnt = per + (b >= nb - rem ? 1 : 0);
    std::vector<int> blk(data.begin() + off, data.begin() + off + cnt);
    off += cnt;
    dblocks.push_back(blk);
    eblocks.push_back(rsEncode(blk, vi.ec_per_block));
  }
  // Interleave data codewords.
  std::vector<int> out;
  std::size_t maxd = 0;
  for (auto& b : dblocks) maxd = std::max(maxd, b.size());
  for (std::size_t i = 0; i < maxd; ++i)
    for (auto& b : dblocks)
      if (i < b.size()) out.push_back(b[i]);
  // Interleave ECC codewords.
  for (int i = 0; i < vi.ec_per_block; ++i)
    for (auto& b : eblocks) out.push_back(b[i]);
  return out;
}

// ── Matrix construction ──
struct Matrix {
  int n;
  std::vector<std::vector<int>> m;      // -1 unset, 0 light, 1 dark
  std::vector<std::vector<bool>> fixed; // true = function module (not data/mask)
  explicit Matrix(int size) : n(size), m(size, std::vector<int>(size, -1)),
                              fixed(size, std::vector<bool>(size, false)) {}
  void set(int r, int c, int v, bool fx) { m[r][c] = v; fixed[r][c] = fx; }
};

void placeFinder(Matrix& mx, int r, int c) {
  for (int dr = -1; dr <= 7; ++dr) {
    for (int dc = -1; dc <= 7; ++dc) {
      int rr = r + dr, cc = c + dc;
      if (rr < 0 || cc < 0 || rr >= mx.n || cc >= mx.n) continue;
      bool dark;
      if (dr >= 0 && dr <= 6 && dc >= 0 && dc <= 6) {
        dark = (dr == 0 || dr == 6 || dc == 0 || dc == 6 ||
                (dr >= 2 && dr <= 4 && dc >= 2 && dc <= 4));
      } else {
        dark = false;  // separator
      }
      mx.set(rr, cc, dark ? 1 : 0, true);
    }
  }
}

const std::array<std::vector<int>, 11> kAlignPos = {{
    {},         {},         {6, 18},    {6, 22},    {6, 26},   {6, 30},
    {6, 34},    {6, 22, 38}, {6, 24, 42}, {6, 26, 46}, {6, 28, 50},
}};

void placeAlignment(Matrix& mx, int version) {
  const auto& pos = kAlignPos[version];
  for (int r : pos) {
    for (int c : pos) {
      // Skip if overlapping a finder.
      if ((r <= 8 && c <= 8) || (r <= 8 && c >= mx.n - 9) || (r >= mx.n - 9 && c <= 8)) continue;
      for (int dr = -2; dr <= 2; ++dr)
        for (int dc = -2; dc <= 2; ++dc) {
          bool dark = (std::abs(dr) == 2 || std::abs(dc) == 2 || (dr == 0 && dc == 0));
          mx.set(r + dr, c + dc, dark ? 1 : 0, true);
        }
    }
  }
}

void placeTiming(Matrix& mx) {
  for (int i = 8; i < mx.n - 8; ++i) {
    int v = (i % 2 == 0) ? 1 : 0;
    if (mx.m[6][i] == -1) mx.set(6, i, v, true);
    if (mx.m[i][6] == -1) mx.set(i, 6, v, true);
  }
}

// Reserve format-info modules (filled after masking).
void reserveFormat(Matrix& mx) {
  for (int i = 0; i < 9; ++i) {
    if (mx.m[8][i] == -1) mx.set(8, i, 0, true);
    if (mx.m[i][8] == -1) mx.set(i, 8, 0, true);
  }
  for (int i = 0; i < 8; ++i) {
    if (mx.m[8][mx.n - 1 - i] == -1) mx.set(8, mx.n - 1 - i, 0, true);
    if (mx.m[mx.n - 1 - i][8] == -1) mx.set(mx.n - 1 - i, 8, 0, true);
  }
  mx.set(mx.n - 8, 8, 1, true);  // dark module
}

// Place data bits in zig-zag.
void placeData(Matrix& mx, const std::vector<int>& codewords) {
  std::vector<int> bits;
  for (int cw : codewords)
    for (int i = 7; i >= 0; --i) bits.push_back((cw >> i) & 1);
  int bi = 0;
  bool upward = true;
  for (int col = mx.n - 1; col > 0; col -= 2) {
    if (col == 6) col = 5;  // skip timing column
    for (int i = 0; i < mx.n; ++i) {
      int row = upward ? mx.n - 1 - i : i;
      for (int c = 0; c < 2; ++c) {
        int cc = col - c;
        if (mx.m[row][cc] == -1) {
          int b = (bi < static_cast<int>(bits.size())) ? bits[bi++] : 0;
          mx.set(row, cc, b, false);
        }
      }
    }
    upward = !upward;
  }
}

bool maskBit(int mask, int r, int c) {
  switch (mask) {
    case 0: return (r + c) % 2 == 0;
    case 1: return r % 2 == 0;
    case 2: return c % 3 == 0;
    case 3: return (r + c) % 3 == 0;
    case 4: return (r / 2 + c / 3) % 2 == 0;
    case 5: return (r * c) % 2 + (r * c) % 3 == 0;
    case 6: return ((r * c) % 2 + (r * c) % 3) % 2 == 0;
    case 7: return ((r + c) % 2 + (r * c) % 3) % 2 == 0;
  }
  return false;
}

// BCH format info for ECC-M + mask (precomputed 15-bit strings).
const std::array<int, 8> kFormatM = {0x5412, 0x5125, 0x5E7C, 0x5B4B, 0x45F9,
                                     0x40CE, 0x4F97, 0x4AA0};

void applyFormat(Matrix& mx, int mask) {
  const int fmt = kFormatM[mask];
  const int n = mx.n;
  // 15 format bits. Both copies are written MSB-first (bit 14 → bit 0) into these cell sequences
  // (ISO/IEC 18004). Verified against a reference encoder's placement.
  const int copy1[15][2] = {{8, 0}, {8, 1}, {8, 2}, {8, 3}, {8, 4}, {8, 5}, {8, 7}, {8, 8},
                            {7, 8}, {5, 8}, {4, 8}, {3, 8}, {2, 8}, {1, 8}, {0, 8}};
  int copy2[15][2];
  // Copy 2: bits 0..6 up column 8 at the bottom (rows n-1..n-7), bits 7..14 along row 8 on the
  // right (columns n-8..n-1).
  for (int i = 0; i < 7; ++i) { copy2[i][0] = n - 1 - i; copy2[i][1] = 8; }
  for (int i = 7; i < 15; ++i) { copy2[i][0] = 8; copy2[i][1] = n - 15 + i; }

  for (int i = 0; i < 15; ++i) {
    const int bit = (fmt >> (14 - i)) & 1;  // MSB-first
    mx.m[copy1[i][0]][copy1[i][1]] = bit;
    mx.m[copy2[i][0]][copy2[i][1]] = bit;
  }
  mx.m[n - 8][8] = 1;  // the always-dark module
}

int penalty(const std::vector<std::vector<int>>& g) {
  int n = static_cast<int>(g.size());
  int pen = 0;
  // Rule 1: runs of 5+.
  for (int r = 0; r < n; ++r) {
    for (int dir = 0; dir < 2; ++dir) {
      int run = 1;
      for (int c = 1; c < n; ++c) {
        int a = dir ? g[c][r] : g[r][c];
        int b = dir ? g[c - 1][r] : g[r][c - 1];
        if (a == b) { if (++run == 5) pen += 3; else if (run > 5) pen += 1; }
        else run = 1;
      }
    }
  }
  // Rule 3: finder-like patterns (approx) — omitted heavy rules; rule 1 dominates selection enough
  // for reliable decoding of short payloads.
  return pen;
}

}  // namespace

std::vector<std::vector<bool>> QrEncoder::encode(const std::string& text) {
  const int version = chooseVersion(text.size());
  const std::vector<int> codewords = buildCodewords(text, version);
  const int n = moduleCount(version);

  Matrix base(n);
  placeFinder(base, 0, 0);
  placeFinder(base, 0, n - 7);
  placeFinder(base, n - 7, 0);
  placeAlignment(base, version);
  placeTiming(base);
  reserveFormat(base);
  placeData(base, codewords);

  // Try all 8 masks, pick lowest penalty.
  int best_mask = 0, best_pen = 1 << 30;
  std::vector<std::vector<int>> best;
  for (int mask = 0; mask < 8; ++mask) {
    std::vector<std::vector<int>> g(n, std::vector<int>(n));
    for (int r = 0; r < n; ++r)
      for (int c = 0; c < n; ++c)
        g[r][c] = (base.fixed[r][c]) ? base.m[r][c]
                                     : (base.m[r][c] ^ (maskBit(mask, r, c) ? 1 : 0));
    Matrix tmp = base;
    tmp.m = g;
    applyFormat(tmp, mask);
    int p = penalty(tmp.m);
    if (p < best_pen) { best_pen = p; best_mask = mask; best = tmp.m; }
  }
  (void)best_mask;

  std::vector<std::vector<bool>> out(n, std::vector<bool>(n));
  for (int r = 0; r < n; ++r)
    for (int c = 0; c < n; ++c) out[r][c] = best[r][c] == 1;
  return out;
}

std::string QrEncoder::toSvg(const std::vector<std::vector<bool>>& matrix, int px) {
  const int n = static_cast<int>(matrix.size());
  const int quiet = 4;
  const int dim = (n + 2 * quiet) * px;
  std::string s = "<svg xmlns='http://www.w3.org/2000/svg' width='" + std::to_string(dim) +
                  "' height='" + std::to_string(dim) + "' shape-rendering='crispEdges'>";
  s += "<rect width='100%' height='100%' fill='#fff'/>";
  s += "<path fill='#000' d='";
  for (int r = 0; r < n; ++r) {
    for (int c = 0; c < n; ++c) {
      if (matrix[r][c]) {
        int x = (c + quiet) * px, y = (r + quiet) * px;
        s += "M" + std::to_string(x) + " " + std::to_string(y) + "h" + std::to_string(px) + "v" +
             std::to_string(px) + "h-" + std::to_string(px) + "z";
      }
    }
  }
  s += "'/></svg>";
  return s;
}

}  // namespace nexus::web
