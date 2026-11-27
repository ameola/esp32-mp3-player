#include "AudioPlayer.h"

#include <AudioFileSource.h>
#include <SD.h>

#include "CardFile.h"
#include <atomic>
#include <driver/gpio.h>
#include <esp_heap_caps.h>
#include <new>
#include <AudioGeneratorMP3a.h>
#include <AudioOutputI2S.h>

#include "Config.h"

// I2S output that starts once and never stops. ESP8266Audio normally tears
// down I2S at the end of every song, which leaves the MAX98357A's BCLK/LRC/DIN
// pins floating; floating inputs pick up SPI crosstalk during screen redraws
// and the amp plays it as noise. Keeping the clocks running with silent data
// holds those lines driven at all times (and avoids start/stop pops).
static volatile PlayerState state = STOPPED;
static volatile uint32_t underruns = 0;
// Samples handed to the DMA ring since the song started. Until the first
// ring's worth is in, a dry ring only means the song hasn't begun (the ring
// holds silence): not an audible gap, so not counted.
static volatile uint32_t trackSamples = 0;
static uint32_t ringSamples = 0;

// The DMA ring played out completely before new samples arrived (an audible
// gap). Silence while stopped or paused does the same, so only count playback.
// Health (Serial "audiostat"): frames written to the DMA ring vs. sent by the
// hardware; their difference is how much is queued. Its low point over a
// window shows how close playback came to running dry.
static volatile uint32_t framesWritten = 0, framesSent = 0, dmaFrames = 0;
static bool IRAM_ATTR onSent(i2s_chan_handle_t, i2s_event_data_t *, void *) {
  // Sending more than was written: the ring replayed (paused, stopped, or ran dry).
  const uint32_t sent = framesSent + dmaFrames;
  framesSent = static_cast<int32_t>(sent - framesWritten) > 0 ? framesWritten : sent;
  return false;
}
uint32_t statQueuedLow = UINT32_MAX;   // fewest frames queued after a write while playing
uint32_t statLoopMaxUs = 0;            // longest single decoder step
uint32_t statLockWaitMaxUs = 0;        // longest wait of the decoder task for `lock`
uint32_t statGapMaxUs = 0;             // longest time between two writes while playing
uint32_t statDecodeErrors = 0, statFrames = 0;  // frames Helix refused / decoded

static bool IRAM_ATTR onSendQueueOverflow(i2s_chan_handle_t, i2s_event_data_t *, void *) {
  if (state == PLAYING && trackSamples > ringSamples) {
    underruns = underruns + 1;
  }
  return false;
}

// Tone control: a low shelf (bass) and a high shelf (treble), RBJ cookbook
// biquads with slope 1, run on the mono mix (one speaker). With a boost the
// input is first turned down by half of it, so a boosted loud song clips
// less; the rest is clamped.
#define BASS_HZ 180.0f
#define TREBLE_HZ 4000.0f

struct Biquad {
  float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
  float x1 = 0, x2 = 0, y1 = 0, y2 = 0;

  void shelf(bool high, float hz, float rate, float db) {
    const float A = powf(10.0f, db / 40.0f), w = 2.0f * PI * hz / rate;
    const float c = cosf(w), alpha = sinf(w) / 2.0f * sqrtf(2.0f), k = 2.0f * sqrtf(A) * alpha;
    float n0, n1, n2, d0, d1, d2;
    if (!high) {
      n0 = A * ((A + 1) - (A - 1) * c + k);
      n1 = 2 * A * ((A - 1) - (A + 1) * c);
      n2 = A * ((A + 1) - (A - 1) * c - k);
      d0 = (A + 1) + (A - 1) * c + k;
      d1 = -2 * ((A - 1) + (A + 1) * c);
      d2 = (A + 1) + (A - 1) * c - k;
    } else {
      n0 = A * ((A + 1) + (A - 1) * c + k);
      n1 = -2 * A * ((A - 1) + (A + 1) * c);
      n2 = A * ((A + 1) + (A - 1) * c - k);
      d0 = (A + 1) - (A - 1) * c + k;
      d1 = 2 * ((A - 1) - (A + 1) * c);
      d2 = (A + 1) - (A - 1) * c - k;
    }
    b0 = n0 / d0, b1 = n1 / d0, b2 = n2 / d0, a1 = d1 / d0, a2 = d2 / d0;
  }
  float run(float x) {
    const float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
    x2 = x1, x1 = x, y2 = y1, y1 = y;
    return y;
  }
  void clear() { x1 = x2 = y1 = y2 = 0; }
};

class ContinuousI2S : public AudioOutputI2S {
public:
  // Bass and treble in dB (0, 0: off).
  void setTone(int8_t bass, int8_t treble) {
    bassDb = bass;
    trebleDb = treble;
    retune();
  }
  bool SetRate(int hz) override {
    const bool ok = AudioOutputI2S::SetRate(hz);
    retune();
    return ok;
  }

  bool begin() override {
    if (!started) {
      started = AudioOutputI2S::begin();
      if (started) {  // callbacks can only be registered on a disabled channel
        ringSamples = _buffers * _bufferWords;
        i2s_channel_disable(_tx_handle);
        i2s_event_callbacks_t callbacks = {};
        callbacks.on_send_q_ovf = onSendQueueOverflow;
        callbacks.on_sent = onSent;
        dmaFrames = _bufferWords;
        i2s_channel_register_event_callback(_tx_handle, &callbacks, nullptr);
        i2s_channel_enable(_tx_handle);
      }
    }
    return started;
  }
  bool stop() override {
    silence();  // fill the DMA ring with silence instead of shutting down
    return true;
  }

  // Leaves the DMA ring holding nothing but zeros. When nothing new is
  // written the I2S hardware keeps cycling through the ring, replaying what
  // it holds: the library's flush() pushed its zeros through the staging
  // buffer, so up to STAGE_SAMPLES of the song could stay in the ring and
  // repeat as a buzz while paused. Here the staged samples go out first,
  // then a whole ring of zeros (plus margin), waiting for room.
  void silence() {
    if (!i2sOn) return;
    for (int tries = 0; pending && tries < 50; tries++) {
      drain();
      if (pending) delay(2);
    }
    pending = 0;
    static const uint32_t zeros[64] = {};
    size_t frames = _buffers * _bufferWords + STAGE_SAMPLES;
    while (frames) {
      size_t written = 0;
      const size_t n = frames < 64 ? frames : 64;
      if (i2s_channel_write(_tx_handle, zeros, n * sizeof(uint32_t), &written, pdMS_TO_TICKS(200)) != ESP_OK ||
          !written) {
        break;
      }
      frames -= written / sizeof(uint32_t);
      framesWritten = framesWritten + written / sizeof(uint32_t);
    }
  }

  // The library hands each sample to the I2S driver with its own
  // i2s_channel_write() call, which cost ~40% of a core at 44.1 kHz. Samples
  // are staged here and written in blocks instead.
  bool ConsumeSample(int16_t sample[2]) override {
    if (!i2sOn) {
      return false;
    }
    if (pending == STAGE_SAMPLES) {
      drain();
      if (pending == STAGE_SAMPLES) {
        return false;  // DMA ring full: try again on the next loop
      }
    }
    int16_t ms[2] = {sample[0], sample[1]};
    MakeSampleStereo16(ms);
    if (mono) {
      int32_t v = (ms[LEFTCHANNEL] + ms[RIGHTCHANNEL]) >> 1;
      if (toneOn) {
        const float y = treble.run(bass.run(v * preamp));
        v = y > 32767.0f ? 32767 : y < -32768.0f ? -32768 : static_cast<int32_t>(y);
      }
      ms[LEFTCHANNEL] = ms[RIGHTCHANNEL] = v & 0xffff;
    }
    stage[pending++] = (static_cast<uint32_t>(Amplify(ms[RIGHTCHANNEL])) << 16) |
                       (Amplify(ms[LEFTCHANNEL]) & 0xffff);
    return true;
  }

  bool loop() override {
    drain();
    return true;
  }

  // True when no more samples fit until the DMA ring plays some out.
  bool full() const {
    return pending == STAGE_SAMPLES;
  }

private:
  static constexpr size_t STAGE_SAMPLES = 128;  // 2.9 ms at 44.1 kHz
  int8_t bassDb = 0, trebleDb = 0;
  bool toneOn = false;
  float preamp = 1.0f;
  Biquad bass, treble;

  void retune() {
    toneOn = bassDb || trebleDb;
    const float rate = hertz > 0 ? hertz : 44100;
    bass.shelf(false, BASS_HZ, rate, bassDb);
    treble.shelf(true, min(TREBLE_HZ, rate * 0.45f), rate, trebleDb);
    bass.clear();
    treble.clear();
    preamp = powf(10.0f, -max<int>(0, max(bassDb, trebleDb)) / 40.0f);
  }

  // Moves as many staged samples into the DMA ring as fit, without waiting.
  void drain() {
    if (!pending) {
      return;
    }
    size_t written = 0;
    i2s_channel_write(_tx_handle, stage, pending * sizeof(uint32_t), &written, 0);
    const size_t n = written / sizeof(uint32_t);
    trackSamples = trackSamples + n;
    framesWritten = framesWritten + n;
    if (n && state == PLAYING) {
      const uint32_t now = micros();
      if (lastWriteUs && now - lastWriteUs > statGapMaxUs) statGapMaxUs = now - lastWriteUs;
      lastWriteUs = now;
      const uint32_t queued = framesWritten - framesSent;
      if (queued < statQueuedLow) statQueuedLow = queued;
    } else if (state != PLAYING) {
      lastWriteUs = 0;
    }
    if (n) {
      memmove(stage, stage + n, (pending - n) * sizeof(uint32_t));
      pending -= n;
    }
  }

  bool started = false;
  uint32_t lastWriteUs = 0;
  uint32_t stage[STAGE_SAMPLES];
  size_t pending = 0;
};

// Decoding runs in its own task on core 0 (above the library indexer's
// priority), so drawing on core 1 can never starve it. Everything that touches
// the decoder, file or output takes `lock`.
#define AUDIO_TASK_PRIORITY 3  // indexer is 1, Arduino loop (core 1) is 1
#define AUDIO_TASK_CORE 0
#define AUDIO_IDLE_MS 4        // sleep when the output is full (~78 ms buffered)
#define READER_TASK_PRIORITY 2 // above the indexer, so it gets the card next

static SemaphoreHandle_t lock = nullptr;

struct AudioLock {
  AudioLock() { xSemaphoreTake(lock, portMAX_DELAY); }
  ~AudioLock() { xSemaphoreGive(lock); }
};

// Song file read ahead into RAM by its own task. The card can be held for
// seconds by others: the library indexer's writes, or a name lookup in a
// folder of thousands of files (FatFs keeps the volume locked while it scans
// the whole folder). With PSRAM the ring holds 256 KB (6.5 s at 320 kbps,
// 16 s at 128 kbps); without it 12 KB (0.3 to 0.75 s).
// One producer (reader task) and one consumer (decoder, under `lock`);
// open/seek/close hold both `lock` and fileLock, so neither side is mid-way.
class PrefetchSource : public AudioFileSource {
public:
  static constexpr uint32_t RING_PSRAM = 256 * 1024, CHUNK_PSRAM = 8192;
  static constexpr uint32_t RING_SMALL = 12288, CHUNK_SMALL = 2048;

  PrefetchSource() {
    fileLock = xSemaphoreCreateMutex();
    ring = static_cast<uint8_t *>(heap_caps_malloc(RING_PSRAM, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (ring) {
      cap = RING_PSRAM;
      chunk = CHUNK_PSRAM;
    } else {
      ring = static_cast<uint8_t *>(heap_caps_malloc(RING_SMALL, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
      cap = RING_SMALL;
      chunk = CHUNK_SMALL;
    }
    lowWater = cap;
  }

  ~PrefetchSource() override {
    closeFile();
    cardDeleteFil(fil);
    if (fileLock) vSemaphoreDelete(fileLock);
    free(ring);
  }

  bool ok() const { return fileLock != nullptr && ring != nullptr; }
  uint32_t capacity() const { return cap; }

  // The ring is full, or holds the rest of the song (Serial and the UI:
  // card work that can hold the card for long waits for this).
  // Over 0.8 s of audio (at 320 kbps) buffered: the reader can let others
  // (the screen, on the same SPI bus) have the card between its reads.
  bool comfortable() {
    return wr.load(std::memory_order_acquire) - rd.load() >= 32 * 1024 || atEof.load(std::memory_order_acquire);
  }

  bool settled() {
    const uint32_t buffered = wr.load(std::memory_order_acquire) - rd.load();
    return !isOpen_ || atEof.load(std::memory_order_acquire) || cap - buffered < 2 * chunk;
  }

  bool open(const char *path) override {
    return open(path, 0, 0);
  }

  // By start cluster when known (no name lookup: see cardOpenCluster), else by path.
  bool open(const char *path, uint32_t cluster, uint32_t fileSize) {
    FileLock held(fileLock);
    closeFile();
    if (!fil) fil = cardNewFil();
    isOpen_ = fil && ((cluster && cardOpenCluster(fil, cluster, fileSize)) ||
                      f_open(fil, cardPath(path).c_str(), FA_READ) == FR_OK);
    size = isOpen_ ? f_size(fil) : 0;
    reset(0);
    return isOpen_;
  }

  bool close() override {
    FileLock held(fileLock);
    closeFile();
    reset(0);
    atEof = true;
    return true;
  }

  bool isOpen() override {
    return isOpen_;
  }

  bool seek(int32_t pos, int dir) override {
    if (dir != SEEK_SET || pos < 0) return false;
    FileLock held(fileLock);
    if (!isOpen_ || f_lseek(fil, pos) != FR_OK) return false;
    reset(pos);
    return true;
  }

  uint32_t getSize() override {
    return size;
  }

  uint32_t getPos() override {
    return base + rd.load();
  }

  // Waits for data rather than returning 0 early: the decoder takes 0 as the
  // end of the song.
  uint32_t read(void *data, uint32_t len) override {
    if (len == 0) return 0;
    uint32_t avail;
    while ((avail = wr.load(std::memory_order_acquire) - rd.load()) == 0) {
      if (atEof.load(std::memory_order_acquire) && wr.load(std::memory_order_acquire) == rd.load()) {
        return 0;
      }
      starved = starved + 1;
      vTaskDelay(1);
    }
    const uint32_t n = min(len, avail);
    const uint32_t at = rd.load() % cap;
    const uint32_t first = min(n, cap - at);
    memcpy(data, ring + at, first);
    memcpy(static_cast<uint8_t *>(data) + first, ring, n - first);
    rd.store(rd.load() + n, std::memory_order_release);
    const uint32_t left = avail - n;
    if (left < lowWater) lowWater = left;
    return n;
  }

  // Reader task: tops the ring up. Returns false when there was nothing to do.
  // Once comfortable it reads 2 KB at a time: each read holds the SPI bus the
  // screen shares, and the screen waits for it between the lines it sends.
  bool fill() {
    FileLock held(fileLock);
    if (!isOpen_ || atEof.load()) return false;
    const uint32_t w = wr.load();
    const uint32_t space = cap - (w - rd.load(std::memory_order_acquire));
    if (space < chunk) return false;
    const uint32_t at = w % cap;
    const uint32_t step = comfortable() ? CHUNK_SMALL : chunk;
    const uint32_t want = min(step, cap - at);
    UINT n = 0;
    if (f_read(fil, ring + at, want, &n) != FR_OK || n == 0) {
      atEof.store(true, std::memory_order_release);
    } else {
      wr.store(w + n, std::memory_order_release);
    }
    return true;
  }

  uint32_t lowWater = 0;        // least buffered after a read (Serial "audiocpu")
  volatile uint32_t starved = 0;  // 1 ms waits for the card

private:
  struct FileLock {
    SemaphoreHandle_t m;
    explicit FileLock(SemaphoreHandle_t m) : m(m) { xSemaphoreTake(m, portMAX_DELAY); }
    ~FileLock() { xSemaphoreGive(m); }
  };

  void closeFile() {
    if (isOpen_) f_close(fil);
    isOpen_ = false;
  }

  void reset(uint32_t pos) {
    base = pos;
    rd.store(0);
    wr.store(0);
    atEof.store(false);
  }

  SemaphoreHandle_t fileLock;
  FIL *fil = nullptr;  // FatFs directly: no VFS slot or 4 KB stdio buffer
  bool isOpen_ = false;
  uint32_t size = 0;
  uint32_t base = 0;  // file offset of ring position 0
  std::atomic<uint32_t> rd{0}, wr{0};
  std::atomic<bool> atEof{true};
  uint8_t *ring = nullptr;
  uint32_t cap = 0, chunk = 0;
};

static PrefetchSource *audioFile = nullptr;

// Helix allocates its state in the constructor; ok() tells whether it got it.
// loop() is the library's, with one guard: after a seek into the middle of a
// file (resuming a paused song), Helix can accept a false frame sync and
// report 0 channels, and the library then divides by zero
// (validSamples = outputSamps / lastChannels). Such a frame is skipped.
class Mp3Decoder : public AudioGeneratorMP3a {
public:
  bool ok() const { return hMP3Decoder != nullptr; }

  bool loop() override {
    if (running) {
      while (validSamples) {
        lastSample[0] = outSample[curSample * 2];
        lastSample[1] = outSample[curSample * 2 + 1];
        if (!output->ConsumeSample(lastSample)) {
          goto done;  // output full
        }
        validSamples--;
        curSample++;
      }
      if (FillBufferWithValidFrame()) {
        unsigned char *in = reinterpret_cast<unsigned char *>(buff);
        int bytesLeft = buffValid;
        const int err = MP3Decode(hMP3Decoder, &in, &bytesLeft, outSample, 0);
        if (err) {
          statDecodeErrors++;
        } else {
          statFrames++;
        }
        if (err == 0) {
          lastFrameEnd = buffValid - bytesLeft;
          MP3FrameInfo fi;
          MP3GetLastFrameInfo(hMP3Decoder, &fi);
          if (fi.nChans >= 1 && fi.nChans <= 2 && fi.samprate > 0) {
            if (fi.samprate != lastRate) {
              output->SetRate(fi.samprate);
              lastRate = fi.samprate;
            }
            if (fi.nChans != lastChannels) {
              output->SetChannels(fi.nChans);
              lastChannels = fi.nChans;
            }
            curSample = 0;
            validSamples = fi.outputSamps / lastChannels;
          }
        }  // a decode error skips the frame, as in the library
      } else {
        running = false;  // end of file
      }
    }
  done:
    file->loop();
    output->loop();
    return running;
  }
};

void audioReadStats(uint32_t &lowWater, uint32_t &starved) {
  if (!audioFile) {
    lowWater = starved = 0;
    return;
  }
  lowWater = audioFile->lowWater;
  starved = audioFile->starved;
  audioFile->lowWater = audioFile->capacity();
  audioFile->starved = 0;
}
static Mp3Decoder *mp3 = nullptr;  // Helix decoder: about a third of libmad's CPU
static ContinuousI2S *audioOut = nullptr;
static volatile bool trackEnded = false;
static int volume = DEFAULT_VOLUME;
static uint8_t volumeLimit = 100;  // percent of full gain at the top step

uint32_t audioDecodeUs = 0;  // time spent decoding (Serial "audiocpu")

// Decodes ahead until the output is full, then sleeps while it plays out.
static void audioTask(void *) {
  for (;;) {
    bool idle = true;
    const uint32_t tw = micros();
    {
      AudioLock held;
      if (micros() - tw > statLockWaitMaxUs) statLockWaitMaxUs = micros() - tw;
      if (state == PLAYING && mp3 && mp3->isRunning()) {
        const uint32_t t0 = micros();
        if (!mp3->loop()) {
          state = STOPPED;  // the generator stopped itself at end of file
          trackEnded = true;
        }
        const uint32_t took = micros() - t0;
        audioDecodeUs += took;
        if (took > statLoopMaxUs) statLoopMaxUs = took;
        idle = audioOut->full();
      }
    }
    if (idle) {
      vTaskDelay(pdMS_TO_TICKS(AUDIO_IDLE_MS));
    }
  }
}

static TaskHandle_t audioTaskHandle = nullptr, readerTaskHandle = nullptr;

// Held by the reader task around each fill(), and while the ring is freed.
static SemaphoreHandle_t readerLock = nullptr;

static void readerTask(void *) {
  for (;;) {
    xSemaphoreTake(readerLock, portMAX_DELAY);
    const bool worked = audioFile && audioFile->fill();
    const bool relaxed = worked && audioFile->comfortable();
    xSemaphoreGive(readerLock);
    if (!worked) {
      vTaskDelay(pdMS_TO_TICKS(10));
    } else if (relaxed) {
      vTaskDelay(pdMS_TO_TICKS(6));  // ~300 KB/s still; the screen gets the bus in between
    }
  }
}

// Engine release (album art downloads).
static bool engineUp = false;
static String currentPath;            // to reopen a paused song
static uint32_t currentCluster = 0, currentSize = 0;
static uint32_t resumePos = 0, resumeSize = 0;
static void (*beforeRestore)() = nullptr;

static bool createEngine() {  // caller holds `lock`
  audioFile = new (std::nothrow) PrefetchSource();
  // Helix's state and tables (several blocks of 4-10 KB) in internal RAM: in
  // PSRAM every frame's work goes through the cache it shares with flash and
  // the read-ahead ring, which costs time. Blocks over 4 KB normally go to
  // PSRAM; this raises that limit while the decoder allocates.
  heap_caps_malloc_extmem_enable(32 * 1024);
  mp3 = audioFile && audioFile->ok() ? new (std::nothrow) Mp3Decoder() : nullptr;
  heap_caps_malloc_extmem_enable(4096);  // CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL
  if (!mp3 || !mp3->ok()) {
    xSemaphoreTake(readerLock, portMAX_DELAY);
    delete audioFile;
    audioFile = nullptr;
    xSemaphoreGive(readerLock);
    delete mp3;
    mp3 = nullptr;
    Serial.printf("[audio] no memory for the decoder (free %u)\n", heap_caps_get_free_size(MALLOC_CAP_8BIT));
    return false;
  }
  engineUp = true;
  return true;
}

// Rebuilds the engine if it was released. Takes `lock` itself.
static bool ensureEngine() {
  if (engineUp) return true;
  if (beforeRestore) beforeRestore();  // e.g. stop an art download, free WiFi
  AudioLock held;
  return engineUp || createEngine();
}

bool audioReleaseEngine() {
  AudioLock held;
  if (!engineUp) return true;
  if (state == PLAYING) return false;
  if (state == PAUSED) {
    resumePos = audioFile->getPos();
    resumeSize = audioFile->getSize();
  }
  if (mp3->isRunning()) mp3->stop();  // closes the file; the output plays silence
  xSemaphoreTake(readerLock, portMAX_DELAY);
  delete audioFile;
  audioFile = nullptr;
  xSemaphoreGive(readerLock);
  delete mp3;
  mp3 = nullptr;
  engineUp = false;
  return true;
}

bool audioEngineReleased() {
  return !engineUp;
}

void audioSetBeforeRestore(void (*fn)()) {
  beforeRestore = fn;
}

void audioInit() {
  lock = xSemaphoreCreateMutex();
  // Hold the amp's inputs low until I2S takes them over, so they never float.
  pinMode(I2S_BCLK, OUTPUT);
  digitalWrite(I2S_BCLK, LOW);
  pinMode(I2S_LRCLK, OUTPUT);
  digitalWrite(I2S_LRCLK, LOW);
  pinMode(I2S_DOUT, OUTPUT);
  digitalWrite(I2S_DOUT, LOW);

  audioOut = new ContinuousI2S();
  audioOut->SetPinout(I2S_BCLK, I2S_LRCLK, I2S_DOUT);
  audioOut->SetBuffers(6);  // 78 ms; the read-ahead ring covers card stalls
  audioOut->SetOutputModeMono(true);  // one speaker
  audioOut->SetRate(44100);
  audioOut->begin();
  // Strong drive: noise coupled from the screen's wires moves these less (Config.h).
  for (int pin : {I2S_BCLK, I2S_LRCLK, I2S_DOUT}) {
    gpio_set_drive_capability(static_cast<gpio_num_t>(pin), static_cast<gpio_drive_cap_t>(I2S_PIN_DRIVE));
  }
  audioSetVolume(DEFAULT_VOLUME);
  readerLock = xSemaphoreCreateMutex();
  {
    AudioLock held;
    createEngine();
  }
  // Stacks: measured peaks ~1.1 KB (decoder) and ~1.3 KB (reader).
  xTaskCreatePinnedToCore(audioTask, "audio", 3072, nullptr, AUDIO_TASK_PRIORITY, &audioTaskHandle,
                          AUDIO_TASK_CORE);
  xTaskCreatePinnedToCore(readerTask, "audioread", 2560, nullptr, READER_TASK_PRIORITY,
                          &readerTaskHandle, AUDIO_TASK_CORE);
}

void audioStop() {
  AudioLock held;
  if (mp3 && mp3->isRunning()) {
    mp3->stop();  // closes the file; output just flushes to silence
  }
  state = STOPPED;
}

static const char *playError = "";

// Another format named .mp3 (seen: a WMA file): the decoder finds false
// frame syncs all through it, plays noise and stutters, using half a core.
// Its first bytes tell; nullptr if they look like an MP3 (or ID3 tag).
static const char *otherFormat(const char *path, uint32_t cluster, uint32_t fileSize) {
  FIL *f = cardNewFil();
  uint8_t b[12] = {};
  UINT got = 0;
  if (f && ((cluster && cardOpenCluster(f, cluster, fileSize)) || f_open(f, cardPath(path).c_str(), FA_READ) == FR_OK)) {
    f_read(f, b, sizeof(b), &got);
    f_close(f);
  }
  cardDeleteFil(f);
  if (got < sizeof(b)) return nullptr;  // let the decoder decide
  static const uint8_t asf[] = {0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11};
  if (!memcmp(b, asf, sizeof(asf))) return "WMA";
  if (!memcmp(b, "fLaC", 4)) return "FLAC";
  if (!memcmp(b, "OggS", 4)) return "Ogg";
  if (!memcmp(b + 4, "ftyp", 4)) return "AAC";
  if (!memcmp(b, "RIFF", 4) && !memcmp(b + 8, "WAVE", 4)) return "WAV";
  return nullptr;
}

const char *audioPlayError() {
  return playError;
}

// A stored cluster is right if the song's first MPEG frame header is where
// the library says (an ID3 tag at the start also passes). A file deleted and
// copied back with the same name, size and date leaves the library looking
// current but the cluster stale; then it's opened by path.
static bool clusterLooksRight(uint32_t cluster, uint32_t fileSize, uint32_t audioStart) {
  FIL *f = cardNewFil();
  uint8_t b[3] = {};
  UINT got = 0;
  const bool ok = f && cardOpenCluster(f, cluster, fileSize) && f_lseek(f, audioStart) == FR_OK &&
                  f_read(f, b, sizeof(b), &got) == FR_OK && got == sizeof(b);
  if (f && ok) f_close(f);
  cardDeleteFil(f);
  return ok && ((b[0] == 0xFF && (b[1] & 0xE0) == 0xE0) || (audioStart == 0 && !memcmp(b, "ID3", 3)));
}

bool audioPlay(const char *path, uint32_t startOffset, uint32_t startCluster, uint32_t fileSize) {
  audioStop();
  playError = "";
  // Checked once per song; seeks and resumes reuse it (their offset is mid-song).
  static uint32_t checkedCluster = 0;
  if (startCluster && startCluster != checkedCluster) {
    if (clusterLooksRight(startCluster, fileSize, startOffset)) {
      checkedCluster = startCluster;
    } else {
      Serial.printf("[audio] stored cluster doesn't match the file: opening %s by path\n", path);
      startCluster = 0;
    }
  }
  if (const char *format = otherFormat(path, startCluster, fileSize)) {
    static char why[40];
    snprintf(why, sizeof(why), "a %s file, not an MP3", format);
    playError = why;
    return false;
  }
  if (!ensureEngine()) return false;
  AudioLock held;
  trackEnded = false;
  trackSamples = 0;
  currentPath = path;
  currentCluster = startCluster;
  currentSize = fileSize;
  if (audioFile->open(path, startCluster, fileSize) &&
      (startOffset == 0 || startOffset >= audioFile->getSize() ||
       audioFile->seek(startOffset, SEEK_SET)) &&
      mp3->begin(audioFile, audioOut)) {
    state = PLAYING;
    return true;
  }
  audioFile->close();
  return false;
}

void audioPause() {
  AudioLock held;
  if (state == PLAYING) {
    audioOut->silence();  // only zeros in the DMA ring, so the pause doesn't buzz
    state = PAUSED;
  }
}

void audioResume() {
  if (state == PAUSED && !engineUp) {  // paused, then released for an art download
    const String path = currentPath;
    const uint32_t pos = resumePos;
    state = STOPPED;
    audioPlay(path.c_str(), pos, currentCluster, currentSize);
    return;
  }
  AudioLock held;
  if (state == PAUSED) {
    state = PLAYING;
  }
}

PlayerState audioState() {
  return state;
}

bool audioTakeTrackEnded() {
  const bool ended = trackEnded;
  trackEnded = false;
  return ended;
}

uint32_t audioFilePos() {
  AudioLock held;
  if (state == STOPPED) return 0;
  return engineUp ? audioFile->getPos() : resumePos;
}

uint32_t audioFileSize() {
  AudioLock held;
  if (state == STOPPED) return 0;
  return engineUp ? audioFile->getSize() : resumeSize;
}

void audioSetVolume(int level) {
  AudioLock held;
  volume = constrain(level, 0, VOLUME_STEPS);
  audioOut->SetGain(volume * GAIN_PER_STEP * volumeLimit / 100.0f);
}

void audioSetTone(int8_t bassDb, int8_t trebleDb) {
  AudioLock held;
  audioOut->setTone(bassDb, trebleDb);
}

void audioSetVolumeLimit(uint8_t percent) {
  volumeLimit = constrain(percent, 10, 100);
  audioSetVolume(volume);
}

int audioVolume() {
  return volume;
}

uint32_t audioRingFrames() {
  return ringSamples;
}

uint32_t audioUnderruns() {
  return underruns;
}

bool audioReadAheadSettled() {
  AudioLock held;
  return !audioFile || state != PLAYING || audioFile->settled();
}
