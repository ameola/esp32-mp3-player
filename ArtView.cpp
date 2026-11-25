#include "ArtView.h"

#include <esp32/rom/tjpgd.h>
#include <new>

#include "CardFile.h"
#include "Lcd.h"

namespace {

#define POOL_BYTES 3900  // TJpgDec work area (3100 bytes per its docs, with a margin)

struct Acc {
  uint16_t r, g, b, n;
};

struct Session {
  FIL *fil;
  Acc *acc;                    // thumbnail mode: sums per thumbnail pixel
  int16_t outW, outH;          // thumbnail mode: size of the decoded image
  int16_t x0, y0;              // screen position of the image's (0, 0) after scaling
  int16_t boxX, boxY, boxW, boxH;
  uint16_t block[16 * 16];     // one MCU, RGB565
};

UINT input(JDEC *jd, BYTE *buf, UINT n) {
  Session *s = static_cast<Session *>(jd->device);
  if (!buf) {  // skip
    return f_lseek(s->fil, f_tell(s->fil) + n) == FR_OK ? n : 0;
  }
  UINT got = 0;
  return f_read(s->fil, buf, n, &got) == FR_OK ? got : 0;
}

// One decoded block (RGB888) at rect: convert, clip to the box, send.
UINT output(JDEC *jd, void *bitmap, JRECT *rect) {
  Session *s = static_cast<Session *>(jd->device);
  const int w = rect->right - rect->left + 1, h = rect->bottom - rect->top + 1;
  const int x = s->x0 + rect->left, y = s->y0 + rect->top;
  const int left = max<int>(x, s->boxX), top = max<int>(y, s->boxY);
  const int right = min<int>(x + w, s->boxX + s->boxW), bottom = min<int>(y + h, s->boxY + s->boxH);
  if (left >= right || top >= bottom) return 1;  // outside the box: keep going
  const int cw = right - left, ch = bottom - top;
  const uint8_t *rgb = static_cast<const uint8_t *>(bitmap);
  for (int row = 0; row < ch; row++) {
    const uint8_t *p = rgb + ((top - y + row) * w + (left - x)) * 3;
    uint16_t *out = s->block + row * cw;
    for (int i = 0; i < cw; i++, p += 3) {
      out[i] = ((p[0] & 0xF8) << 8) | ((p[1] & 0xFC) << 3) | (p[2] >> 3);
    }
  }
  lcdPushPixels(left, top, cw, ch, s->block);
  return 1;
}

// Thumbnail mode: add each decoded pixel to its thumbnail cell.
UINT thumbOutput(JDEC *jd, void *bitmap, JRECT *rect) {
  Session *s = static_cast<Session *>(jd->device);
  const int w = rect->right - rect->left + 1, h = rect->bottom - rect->top + 1;
  const uint8_t *p = static_cast<const uint8_t *>(bitmap);
  for (int y = 0; y < h; y++) {
    const int ty = min<int>(ART_THUMB - 1, (rect->top + y) * ART_THUMB / s->outH);
    for (int x = 0; x < w; x++, p += 3) {
      const int tx = min<int>(ART_THUMB - 1, (rect->left + x) * ART_THUMB / s->outW);
      Acc &a = s->acc[ty * ART_THUMB + tx];
      a.r += p[0];
      a.g += p[1];
      a.b += p[2];
      a.n++;
    }
  }
  return 1;
}

const char *errorText(JRESULT r) {
  switch (r) {
    case JDR_INP: return "read error";
    case JDR_MEM1:
    case JDR_MEM2: return "image too complex";
    case JDR_FMT1: return "damaged file";
    case JDR_FMT2:
    case JDR_FMT3: return "not a baseline JPEG";
    default: return "decode error";
  }
}

// ---------------------------------------------------------------- DC-only decoder
// Progressive JPEGs (which TJpgDec can't read) start with scans of each
// 8x8 block's DC coefficient: its average color. That is the image at 1/8
// scale, ~31 px for a 250 px photo: enough for a thumbnail, and it needs
// only the DC values (2 bytes per block) instead of all coefficients
// (~190 KB for a 250 px image). Not for baseline files (TJpgDec's job):
// their scan mixes in AC data this doesn't decode.
struct DcHuff {
  bool present = false;
  uint8_t counts[17] = {};
  uint8_t symbols[16] = {};  // DC symbols are categories 0..15 (12 used)
  int32_t maxCode[18], valPtr[17], minCode[17];

  void build() {
    int32_t code = 0, k = 0;
    for (int l = 1; l <= 16; l++) {
      valPtr[l] = k;
      minCode[l] = code;
      code += counts[l];
      k += counts[l];
      maxCode[l] = counts[l] ? code - 1 : -1;
      code <<= 1;
    }
    present = true;
  }
};

struct DcComp {
  uint8_t id = 0, h = 1, v = 1, tq = 0, td = 0;
  int bw = 0, bh = 0;     // blocks across and down
  int16_t *dc = nullptr;  // bw x bh, after the point transform is undone
  bool done = false;
  int pred = 0;
};

class DcDecoder {
 public:
  const uint8_t *d;
  size_t n, pos = 0;
  uint16_t q[4] = {1, 1, 1, 1};  // DC quantizer of each table
  DcHuff huff[4];
  DcComp comp[3];
  int ncomp = 0, width = 0, height = 0, hmax = 1, vmax = 1, restart = 0;
  uint32_t bits = 0;
  int nbits = 0;
  bool hitMarker = false;

  DcDecoder(const uint8_t *data, size_t size) : d(data), n(size) {}
  ~DcDecoder() {
    for (DcComp &c : comp) free(c.dc);
  }

  int byteAt(size_t i) const { return i < n ? d[i] : -1; }
  int u16(size_t i) const { return i + 1 < n ? (d[i] << 8) | d[i + 1] : -1; }

  bool run() {
    if (u16(0) != 0xFFD8) return false;
    pos = 2;
    while (pos + 4 <= n) {
      if (d[pos] != 0xFF) return false;
      const int m = d[pos + 1];
      if (m == 0xFF) {  // fill byte
        pos++;
        continue;
      }
      if (m == 0xD9) break;
      const int len = u16(pos + 2);
      if (len < 2 || pos + 2 + len > n) return false;
      const uint8_t *seg = d + pos + 4;
      const int sl = len - 2;
      if (m == 0xDB && !dqt(seg, sl)) return false;
      if (m == 0xC4 && !dht(seg, sl)) return false;
      if (m == 0xDD && sl >= 2) restart = (seg[0] << 8) | seg[1];
      if ((m == 0xC0 || m == 0xC1 || m == 0xC2) && !sof(seg, sl)) return false;
      if (m == 0xC3 || (m >= 0xC5 && m <= 0xCF && m != 0xC8 && m != 0xCC)) return false;  // lossless etc.
      pos += 2 + len;
      if (m == 0xDA) {
        if (!scan(seg, sl)) return false;
        if (allDone()) return true;
        skipEntropy();
      }
    }
    return allDone();
  }

  bool allDone() const {
    if (!ncomp) return false;
    for (int i = 0; i < ncomp; i++) {
      if (!comp[i].done) return false;
    }
    return true;
  }

 private:
  bool dqt(const uint8_t *p, int len) {
    for (int i = 0; i < len;) {
      const int pq = p[i] >> 4, tq = p[i] & 3;
      if (i + 1 + (pq ? 128 : 64) > len) return false;
      q[tq] = pq ? (p[i + 1] << 8) | p[i + 2] : p[i + 1];  // the first value is DC's
      i += 1 + (pq ? 128 : 64);
    }
    return true;
  }

  bool dht(const uint8_t *p, int len) {
    for (int i = 0; i < len;) {
      if (i + 17 > len) return false;
      const int tc = p[i] >> 4, th = p[i] & 3;
      int total = 0;
      for (int l = 1; l <= 16; l++) total += p[i + l];
      if (i + 17 + total > len) return false;
      if (tc == 0) {  // DC table; AC tables aren't needed
        if (total > 16) return false;
        DcHuff &h = huff[th];
        for (int l = 1; l <= 16; l++) h.counts[l] = p[i + l];
        memcpy(h.symbols, p + i + 17, total);
        h.build();
      }
      i += 17 + total;
    }
    return true;
  }

  bool sof(const uint8_t *p, int len) {
    if (len < 6 || p[0] != 8) return false;  // 8-bit samples only
    height = (p[1] << 8) | p[2];
    width = (p[3] << 8) | p[4];
    ncomp = p[5];
    if ((ncomp != 1 && ncomp != 3) || len < 6 + 3 * ncomp || !width || !height) return false;
    for (int i = 0; i < ncomp; i++) {
      DcComp &c = comp[i];
      c.id = p[6 + 3 * i];
      c.h = p[7 + 3 * i] >> 4;
      c.v = p[7 + 3 * i] & 15;
      c.tq = p[8 + 3 * i] & 3;
      if (c.h < 1 || c.h > 2 || c.v < 1 || c.v > 2) return false;
      hmax = max<int>(hmax, c.h);
      vmax = max<int>(vmax, c.v);
    }
    const int mcuW = (width + 8 * hmax - 1) / (8 * hmax), mcuH = (height + 8 * vmax - 1) / (8 * vmax);
    for (int i = 0; i < ncomp; i++) {
      DcComp &c = comp[i];
      c.bw = mcuW * c.h;
      c.bh = mcuH * c.v;
      if (static_cast<uint32_t>(c.bw) * c.bh > 128 * 128) return false;  // > ~1000 px
      c.dc = static_cast<int16_t *>(calloc(c.bw * c.bh, sizeof(int16_t)));
      if (!c.dc) return false;
    }
    return true;
  }

  // Bits of entropy-coded data; stops at a marker (reads zeros after it).
  int nextBit() {
    if (!nbits) {
      int b = byteAt(pos);
      if (b < 0 || hitMarker) {
        b = 0;
      } else if (b == 0xFF) {
        const int next = byteAt(pos + 1);
        if (next == 0) {
          pos += 2;
        } else {
          hitMarker = true;
          b = 0;
        }
      } else {
        pos++;
      }
      bits = b;
      nbits = 8;
    }
    nbits--;
    return (bits >> nbits) & 1;
  }

  int receive(int s) {
    int v = 0;
    for (int i = 0; i < s; i++) v = (v << 1) | nextBit();
    if (s && v < (1 << (s - 1))) v -= (1 << s) - 1;  // EXTEND
    return v;
  }

  int decodeDc(const DcHuff &h) {
    int32_t code = 0;
    for (int l = 1; l <= 16; l++) {
      code = (code << 1) | nextBit();
      if (h.maxCode[l] >= 0 && code <= h.maxCode[l]) {
        const int k = h.valPtr[l] + code - h.minCode[l];
        return k < 16 ? h.symbols[k] : -1;
      }
    }
    return -1;
  }

  void restartMarker() {  // byte-align, skip RSTn, reset predictors
    nbits = 0;
    hitMarker = false;
    if (byteAt(pos) == 0xFF && byteAt(pos + 1) >= 0xD0 && byteAt(pos + 1) <= 0xD7) pos += 2;
  }

  bool scan(const uint8_t *p, int len) {
    if (len < 1) return false;
    const int ns = p[0];
    if (ns < 1 || ns > 3 || len < 1 + 2 * ns + 3) return false;
    DcComp *sc[3];
    for (int i = 0; i < ns; i++) {
      sc[i] = nullptr;
      for (int c = 0; c < ncomp; c++) {
        if (comp[c].id == p[1 + 2 * i]) sc[i] = &comp[c];
      }
      if (!sc[i]) return false;
      sc[i]->td = p[2 + 2 * i] >> 4;
    }
    const int ss = p[1 + 2 * ns], ah = p[3 + 2 * ns] >> 4, al = p[3 + 2 * ns] & 15;
    if (ss != 0 || ah != 0) return true;  // AC or refinement: not needed
    for (int i = 0; i < ns; i++) {
      if (!huff[sc[i]->td].present) return false;
      sc[i]->pred = 0;
    }
    bits = 0;
    nbits = 0;
    hitMarker = false;
    // One component alone: its blocks in raster order, only those inside
    // the image. Several: MCUs, each with h x v blocks of each.
    int unitsW, unitsH;
    if (ns == 1) {
      const DcComp &c = *sc[0];
      unitsW = (width * c.h / hmax + 7) / 8;
      unitsH = (height * c.v / vmax + 7) / 8;
    } else {
      unitsW = (width + 8 * hmax - 1) / (8 * hmax);
      unitsH = (height + 8 * vmax - 1) / (8 * vmax);
    }
    int count = 0;
    for (int uy = 0; uy < unitsH; uy++) {
      for (int ux = 0; ux < unitsW; ux++) {
        if (restart && count && count % restart == 0) {
          restartMarker();
          for (int i = 0; i < ns; i++) sc[i]->pred = 0;
        }
        count++;
        for (int i = 0; i < ns; i++) {
          DcComp &c = *sc[i];
          const int bh = ns == 1 ? 1 : c.v, bw = ns == 1 ? 1 : c.h;
          for (int by = 0; by < bh; by++) {
            for (int bx = 0; bx < bw; bx++) {
              const int s = decodeDc(huff[c.td]);
              if (s < 0 || s > 11) return false;
              c.pred += receive(s);
              const int x = ns == 1 ? ux : ux * c.h + bx, y = ns == 1 ? uy : uy * c.v + by;
              if (x < c.bw && y < c.bh) c.dc[y * c.bw + x] = c.pred * (1 << al);
            }
          }
        }
      }
    }
    for (int i = 0; i < ns; i++) sc[i]->done = true;
    return true;
  }

  void skipEntropy() {  // to the next marker that isn't RSTn
    while (pos + 1 < n) {
      if (d[pos] == 0xFF && d[pos + 1] != 0 && !(d[pos + 1] >= 0xD0 && d[pos + 1] <= 0xD7)) return;
      pos++;
    }
  }
};

// The thumbnail from the DC values: each thumbnail pixel averages the
// 1/8-scale pixels it covers (at least one).
bool dcThumb(const uint8_t *data, size_t size, uint16_t *out) {
  DcDecoder *dec = new (std::nothrow) DcDecoder(data, size);
  if (!dec) return false;
  const bool ok = dec->run();
  if (ok) {
    const int sw = (dec->width + 7) / 8, sh = (dec->height + 7) / 8;  // 1/8-scale size
    auto sample = [&](int ci, int x, int y) {  // a component at 1/8-scale pixel (x, y), 0-centered
      const DcComp &c = dec->comp[ci];
      const int bx = min(c.bw - 1, x * c.h / dec->hmax), by = min(c.bh - 1, y * c.v / dec->vmax);
      return c.dc[by * c.bw + bx] * static_cast<int>(dec->q[c.tq]) / 8;
    };
    for (int ty = 0; ty < ART_THUMB; ty++) {
      const int y0 = ty * sh / ART_THUMB, y1 = max(y0 + 1, (ty + 1) * sh / ART_THUMB);
      for (int tx = 0; tx < ART_THUMB; tx++) {
        const int x0 = tx * sw / ART_THUMB, x1 = max(x0 + 1, (tx + 1) * sw / ART_THUMB);
        int r = 0, g = 0, b = 0, k = 0;
        for (int y = y0; y < y1; y++) {
          for (int x = x0; x < x1; x++, k++) {
            const int Y = sample(0, x, y) + 128;
            const int cb = dec->ncomp == 3 ? sample(1, x, y) : 0, cr = dec->ncomp == 3 ? sample(2, x, y) : 0;
            r += constrain(Y + (91881 * cr >> 16), 0, 255);
            g += constrain(Y - ((22554 * cb + 46802 * cr) >> 16), 0, 255);
            b += constrain(Y + (116130 * cb >> 16), 0, 255);
          }
        }
        r /= k;
        g /= k;
        b /= k;
        out[ty * ART_THUMB + tx] = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
      }
    }
  }
  delete dec;
  return ok;
}

#define DC_MAX_FILE (96 * 1024)

}  // namespace

ArtDrawResult artDrawJpeg(const String &path, int16_t boxX, int16_t boxY, int16_t boxW, int16_t boxH) {
  ArtDrawResult res;
  const uint32_t t0 = millis();
  Session *s = static_cast<Session *>(malloc(sizeof(Session)));
  void *pool = malloc(POOL_BYTES);
  FIL *fil = cardNewFil();
  if (!s || !pool || !fil) {
    res.error = "out of memory";
  } else if (f_open(fil, path.c_str(), FA_READ) != FR_OK) {
    res.error = "can't open file";
    cardDeleteFil(fil);
    fil = nullptr;
  } else {
    res.bytes = f_size(fil);
    s->fil = fil;
    JDEC jd;
    JRESULT r = jd_prepare(&jd, input, pool, POOL_BYTES, s);
    if (r == JDR_OK) {
      res.width = jd.width;
      res.height = jd.height;
      // Scale down only images much bigger than the box; crop the rest.
      while (res.scale < 3 && (max<int>(jd.width, jd.height) >> res.scale) > max<int>(boxW, boxH) * 5 / 4) {
        res.scale++;
      }
      const int w = jd.width >> res.scale, h = jd.height >> res.scale;
      s->boxX = boxX;
      s->boxY = boxY;
      s->boxW = boxW;
      s->boxH = boxH;
      s->x0 = boxX + (boxW - w) / 2;
      s->y0 = boxY + (boxH - h) / 2;
      r = jd_decomp(&jd, output, res.scale);
    }
    res.ok = r == JDR_OK;
    if (!res.ok) res.error = errorText(r);
    f_close(fil);
  }
  cardDeleteFil(fil);
  free(pool);
  free(s);
  res.ms = millis() - t0;
  return res;
}

String artThumbPath(const String &coverPath) {
  String p = coverPath;
  if (p.endsWith(".jpg")) p = p.substring(0, p.length() - 4);
  return p + ".thm";
}

bool artMakeThumb(const String &jpgPath, uint16_t *out) {
  Session *s = static_cast<Session *>(malloc(sizeof(Session)));
  void *pool = malloc(POOL_BYTES);
  Acc *acc = static_cast<Acc *>(calloc(ART_THUMB * ART_THUMB, sizeof(Acc)));
  FIL *fil = cardNewFil();
  bool ok = false, dcOk = false;
  if (s && pool && acc && fil && f_open(fil, jpgPath.c_str(), FA_READ) == FR_OK) {
    s->fil = fil;
    s->acc = acc;
    JDEC jd;
    if (jd_prepare(&jd, input, pool, POOL_BYTES, s) == JDR_OK && jd.width >= ART_THUMB && jd.height >= ART_THUMB) {
      // The most the decoder can scale down while every cell still gets a
      // pixel; a few pixels per cell are then averaged (box filter).
      uint8_t scale = 0;
      while (scale < 3 && (min<int>(jd.width, jd.height) >> (scale + 1)) >= ART_THUMB) scale++;
      s->outW = jd.width >> scale;
      s->outH = jd.height >> scale;
      ok = jd_decomp(&jd, thumbOutput, scale) == JDR_OK;
    }
    if (!ok && f_size(fil) <= DC_MAX_FILE) {  // progressive (or odd): the DC-only decoder
      const UINT size = f_size(fil);
      uint8_t *data = static_cast<uint8_t *>(malloc(size));
      UINT got = 0;
      if (data && f_lseek(fil, 0) == FR_OK && f_read(fil, data, size, &got) == FR_OK && got == size) {
        dcOk = dcThumb(data, size, out);
      }
      free(data);
    }
    f_close(fil);
    for (int i = 0; ok && i < ART_THUMB * ART_THUMB; i++) {
      const Acc &a = acc[i];
      const int n = a.n ? a.n : 1;
      const int r = a.r / n, g = a.g / n, b = a.b / n;
      out[i] = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    }
    ok = ok || dcOk;
  }
  cardDeleteFil(fil);
  free(acc);
  free(pool);
  free(s);
  return ok;
}

bool artReadThumb(const String &thumbPath, uint16_t *pixels) {
  FIL *fil = cardNewFil();
  bool ok = false;
  if (fil && f_open(fil, thumbPath.c_str(), FA_READ) == FR_OK) {
    UINT got = 0;
    ok = f_size(fil) == ART_THUMB_BYTES && f_read(fil, pixels, ART_THUMB_BYTES, &got) == FR_OK &&
         got == ART_THUMB_BYTES;
    f_close(fil);
  }
  cardDeleteFil(fil);
  return ok;
}
