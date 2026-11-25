#include "AudioPlayer.h"

#include <AudioFileSource.h>
#include <SD.h>

#include "CardFile.h"
#include <atomic>
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
static bool IRAM_ATTR onSendQueueOverflow(i2s_chan_handle_t, i2s_event_data_t *, void *) {
  if (state == PLAYING && trackSamples > ringSamples) {
    underruns = underruns + 1;
  }
  return false;
}

class ContinuousI2S : public AudioOutputI2S {
public:
  bool begin() override {
    if (!started) {
      started = AudioOutputI2S::begin();
      if (started) {  // callbacks can only be registered on a disabled channel
        ringSamples = _buffers * _bufferWords;
        i2s_channel_disable(_tx_handle);
        i2s_event_callbacks_t callbacks = {};
        callbacks.on_send_q_ovf = onSendQueueOverflow;
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
      const int32_t sum = ms[LEFTCHANNEL] + ms[RIGHTCHANNEL];
      ms[LEFTCHANNEL] = ms[RIGHTCHANNEL] = (sum >> 1) & 0xffff;
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

  // Moves as many staged samples into the DMA ring as fit, without waiting.
  void drain() {
    if (!pending) {
      return;
    }
    size_t written = 0;
    i2s_channel_write(_tx_handle, stage, pending * sizeof(uint32_t), &written, 0);
    const size_t n = written / sizeof(uint32_t);
    trackSamples = trackSamples + n;
    if (n) {
      memmove(stage, stage + n, (pending - n) * sizeof(uint32_t));
      pending -= n;
    }
  }

  bool started = false;
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

// Song file read ahead into RAM by its own task. While the library indexer
// writes, the card can stall a read for 150 ms or more; the ring covers
// 0.3 s (320 kbps) to 0.75 s (128 kbps), so the decoder never waits on it.
// One producer (reader task) and one consumer (decoder, under `lock`);
// open/seek/close hold both `lock` and fileLock, so neither side is mid-way.
class PrefetchSource : public AudioFileSource {
public:
  static constexpr uint32_t RING = 12288;
  static constexpr uint32_t CHUNK = 2048;

  PrefetchSource() {
    fileLock = xSemaphoreCreateMutex();
  }

  ~PrefetchSource() override {
    closeFile();
    cardDeleteFil(fil);
    vSemaphoreDelete(fileLock);
  }

  bool ok() const { return fileLock != nullptr; }

  bool open(const char *path) override {
    FileLock held(fileLock);
    closeFile();
    if (!fil) fil = cardNewFil();
    isOpen_ = fil && f_open(fil, cardPath(path).c_str(), FA_READ) == FR_OK;
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
    const uint32_t at = rd.load() % RING;
    const uint32_t first = min(n, RING - at);
    memcpy(data, ring + at, first);
    memcpy(static_cast<uint8_t *>(data) + first, ring, n - first);
    rd.store(rd.load() + n, std::memory_order_release);
    const uint32_t left = avail - n;
    if (left < lowWater) lowWater = left;
    return n;
  }

  // Reader task: tops the ring up. Returns false when there was nothing to do.
  bool fill() {
    FileLock held(fileLock);
    if (!isOpen_ || atEof.load()) return false;
    const uint32_t w = wr.load();
    const uint32_t space = RING - (w - rd.load(std::memory_order_acquire));
    if (space < CHUNK) return false;
    const uint32_t at = w % RING;
    const uint32_t want = min(CHUNK, RING - at);
    UINT n = 0;
    if (f_read(fil, ring + at, want, &n) != FR_OK || n == 0) {
      atEof.store(true, std::memory_order_release);
    } else {
      wr.store(w + n, std::memory_order_release);
    }
    return true;
  }

  uint32_t lowWater = RING;     // least buffered after a read (Serial "audiocpu")
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
  uint8_t ring[RING];
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
        if (MP3Decode(hMP3Decoder, &in, &bytesLeft, outSample, 0) == 0) {
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
  audioFile->lowWater = PrefetchSource::RING;
  audioFile->starved = 0;
}
static Mp3Decoder *mp3 = nullptr;  // Helix decoder: about a third of libmad's CPU
static ContinuousI2S *audioOut = nullptr;
static volatile bool trackEnded = false;
static int volume = DEFAULT_VOLUME;

uint32_t audioDecodeUs = 0;  // time spent decoding (Serial "audiocpu")

// Decodes ahead until the output is full, then sleeps while it plays out.
static void audioTask(void *) {
  for (;;) {
    bool idle = true;
    {
      AudioLock held;
      if (state == PLAYING && mp3 && mp3->isRunning()) {
        const uint32_t t0 = micros();
        if (!mp3->loop()) {
          state = STOPPED;  // the generator stopped itself at end of file
          trackEnded = true;
        }
        audioDecodeUs += micros() - t0;
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
    xSemaphoreGive(readerLock);
    if (!worked) {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }
}

// Engine release (album art downloads).
static bool engineUp = false;
static String currentPath;            // to reopen a paused song
static uint32_t resumePos = 0, resumeSize = 0;
static void (*beforeRestore)() = nullptr;

static bool createEngine() {  // caller holds `lock`
  audioFile = new (std::nothrow) PrefetchSource();
  mp3 = audioFile && audioFile->ok() ? new (std::nothrow) Mp3Decoder() : nullptr;
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

bool audioPlay(const char *path, uint32_t startOffset) {
  audioStop();
  if (!ensureEngine()) return false;
  AudioLock held;
  trackEnded = false;
  trackSamples = 0;
  currentPath = path;
  if (audioFile->open(path) &&
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
    audioPlay(path.c_str(), pos);
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
  audioOut->SetGain(volume * GAIN_PER_STEP);
}

int audioVolume() {
  return volume;
}

uint32_t audioUnderruns() {
  return underruns;
}
