#include "ThumbStore.h"

#include <esp_rom_crc.h>

#include "LibFormat.h"

#define STORE_PATH LIB_DIR "/art/thumbs.dat"
#define STORE_TMP LIB_DIR "/art/thumbs.tmp"
#define SLOT_BYTES 2048  // 4 sectors
#define STORE_SLOTS 8192
#define PROBES 16
#define SLOT_MAGIC 0x31424D54u  // "TMB1"

namespace {

struct __attribute__((packed)) SlotHead {
  uint32_t magic;
  uint32_t key;
  uint32_t crc;  // of the pixels
  uint8_t flags;
  uint8_t reserved[3];
};
static_assert(sizeof(SlotHead) + ART_THUMB_BYTES <= SLOT_BYTES, "slot size");

uint32_t firstSlot(uint32_t key) {
  static_assert(STORE_SLOTS == 1 << 13, "slot count: 2^13");
  return (key * 2654435761u) >> (32 - 13);  // Fibonacci hashing: the product's top bits
}

bool valid(const uint8_t *buf, const SlotHead &h) {
  return h.magic == SLOT_MAGIC &&
         h.crc == esp_rom_crc32_le(0, buf + sizeof(SlotHead), ART_THUMB_BYTES);
}

}  // namespace

bool ThumbStore::exists() {
  FILINFO info;
  return f_stat(cardPath(STORE_PATH).c_str(), &info) == FR_OK &&
         info.fsize == static_cast<FSIZE_t>(SLOT_BYTES) * STORE_SLOTS;
}

bool ThumbStore::create(const std::atomic<bool> &stop) {
  const uint32_t t0 = millis();
  f_unlink(cardPath(STORE_TMP).c_str());
  FIL *f = cardNewFil();
  uint8_t *zeros = static_cast<uint8_t *>(calloc(1, SLOT_BYTES));
  bool ok = f && zeros && f_open(f, cardPath(STORE_TMP).c_str(), FA_WRITE | FA_CREATE_ALWAYS) == FR_OK;
  if (ok) {
    for (uint32_t i = 0; ok && i < STORE_SLOTS; i++) {
      UINT w = 0;
      ok = !stop && cardWrite(f, zeros, SLOT_BYTES, &w) == FR_OK && w == SLOT_BYTES;
      if (i % 8 == 7) vTaskDelay(1);  // the card and core 0 for others now and then
    }
    ok = f_close(f) == FR_OK && ok;
    if (ok) {
      f_unlink(cardPath(STORE_PATH).c_str());
      ok = f_rename(cardPath(STORE_TMP).c_str(), cardPath(STORE_PATH).c_str()) == FR_OK;
    }
    if (!ok) f_unlink(cardPath(STORE_TMP).c_str());
  }
  free(zeros);
  cardDeleteFil(f);
  Serial.printf("[thumbs] store %s in %lu ms\n", ok ? "created" : "not created", millis() - t0);
  return ok;
}

bool ThumbStore::open(bool writable) {
  if (fil_ && (writable_ || !writable)) return true;
  close();
  if (openFailed_ && !writable) return false;
  fil_ = cardNewFil();
  buf_ = static_cast<uint8_t *>(malloc(SLOT_BYTES));
  const BYTE mode = writable ? FA_READ | FA_WRITE : FA_READ;
  if (!fil_ || !buf_ || f_open(fil_, cardPath(STORE_PATH).c_str(), mode) != FR_OK) {
    close();
    openFailed_ = true;
    return false;
  }
  if (f_size(fil_) != static_cast<FSIZE_t>(SLOT_BYTES) * STORE_SLOTS) {
    f_close(fil_);
    close();
    openFailed_ = true;
    return false;
  }
  writable_ = writable;
  seek_.build(fil_);
  return true;
}

void ThumbStore::close() {
  if (fil_ && fil_->obj.fs) f_close(fil_);
  cardDeleteFil(fil_);
  fil_ = nullptr;
  free(buf_);
  buf_ = nullptr;
  seek_.clear();
}

bool ThumbStore::readSlot(uint32_t slot) {
  UINT got = 0;
  return seek_.seek(fil_, static_cast<FSIZE_t>(slot) * SLOT_BYTES) == FR_OK &&
         f_read(fil_, buf_, SLOT_BYTES, &got) == FR_OK && got == SLOT_BYTES;
}

bool ThumbStore::read(uint32_t key, uint16_t *pixels, uint8_t *flags) {
  if (!key || !open(false)) return false;
  const uint32_t first = firstSlot(key);
  for (uint32_t i = 0; i < PROBES; i++) {
    if (!readSlot((first + i) % STORE_SLOTS)) return false;
    SlotHead h;
    memcpy(&h, buf_, sizeof(h));
    if (!valid(buf_, h)) return false;  // an empty slot ends the probe
    if (h.key == key) {
      memcpy(pixels, buf_ + sizeof(SlotHead), ART_THUMB_BYTES);
      if (flags) *flags = h.flags;
      return true;
    }
  }
  return false;
}

bool ThumbStore::write(uint32_t key, const uint16_t *pixels, uint8_t flags) {
  if (!key || !open(true)) return false;
  const uint32_t first = firstSlot(key);
  uint32_t slot = first;  // all PROBES slots taken by others: replace the first
  for (uint32_t i = 0; i < PROBES; i++) {
    const uint32_t s = (first + i) % STORE_SLOTS;
    if (!readSlot(s)) return false;
    SlotHead h;
    memcpy(&h, buf_, sizeof(h));
    if (!valid(buf_, h) || h.key == key) {
      slot = s;
      break;
    }
  }
  SlotHead h = {SLOT_MAGIC, key, esp_rom_crc32_le(0, reinterpret_cast<const uint8_t *>(pixels), ART_THUMB_BYTES),
                flags, {0, 0, 0}};
  memset(buf_, 0, SLOT_BYTES);
  memcpy(buf_, &h, sizeof(h));
  memcpy(buf_ + sizeof(SlotHead), pixels, ART_THUMB_BYTES);
  UINT w = 0;
  return seek_.seek(fil_, static_cast<FSIZE_t>(slot) * SLOT_BYTES) == FR_OK &&
         cardWrite(fil_, buf_, SLOT_BYTES, &w) == FR_OK && w == SLOT_BYTES && f_sync(fil_) == FR_OK;
}
