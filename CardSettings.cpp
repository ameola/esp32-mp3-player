#include "CardSettings.h"

#include "CardFile.h"
#include "LibFormat.h"

#define SETTINGS_PATH LIB_DIR "/settings.txt"

bool cardSettingsLoad(CardSettings &out) {
  FIL fil;
  if (f_open(&fil, cardPath(SETTINGS_PATH).c_str(), FA_READ) != FR_OK) {
    return false;
  }
  char buf[256];
  UINT n = 0;
  f_read(&fil, buf, sizeof(buf) - 1, &n);
  f_close(&fil);
  buf[n] = 0;
  out = CardSettings();
  for (char *line = strtok(buf, "\r\n"); line; line = strtok(nullptr, "\r\n")) {
    if (strcmp(line, "art=on") == 0) out.artDownload = true;
  }
  return true;
}

bool cardSettingsSave(const CardSettings &s) {
  const FRESULT md = f_mkdir(cardPath(LIB_DIR).c_str());
  if (md != FR_OK && md != FR_EXIST) {
    Serial.printf("settings: mkdir failed (FatFs error %d)\n", md);
    return false;
  }
  // Written to a temporary name, then renamed over the old file, so a
  // power cut never leaves a half-written file (which would read as "art off").
  const String tmp = cardPath(SETTINGS_PATH ".tmp");
  FIL fil;
  FRESULT fr = f_open(&fil, tmp.c_str(), FA_WRITE | FA_CREATE_ALWAYS);
  if (fr == FR_OK) {
    const String text = String("# MP3 Player card settings\nart=") + (s.artDownload ? "on" : "off") + "\n";
    UINT written = 0;
    fr = cardWrite(&fil, text.c_str(), text.length(), &written);
    const FRESULT cl = f_close(&fil);
    if (fr == FR_OK) fr = cl;
  }
  if (fr == FR_OK) {
    f_unlink(cardPath(SETTINGS_PATH).c_str());
    fr = f_rename(tmp.c_str(), cardPath(SETTINGS_PATH).c_str());
  }
  if (fr != FR_OK) {
    Serial.printf("settings: write failed (FatFs error %d)\n", fr);
  }
  return fr == FR_OK;
}
