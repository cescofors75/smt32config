/*
 * AudioEngine.cpp
 * Implementació del motor d'àudio
 * Master effects inspired by torvalds/AudioNoise (GPL-2.0)
 * - Soft clipping: limit_value pattern from util.h
 * - Delay/Echo: circular buffer pattern from echo.h
 * - Phaser: cascaded allpass from phaser.h
 * - Flanger: modulated delay from flanger.h
 * - LFO: phase accumulator + sine LUT from lfo.h
 */

#include "AudioEngine.h"

// Static member initialization
float AudioEngine::lfoSineTable[LFO_TABLE_SIZE] = {0};
bool AudioEngine::lfoTableInitialized = false;

AudioEngine::AudioEngine() : i2sPort(I2S_NUM_0),
                             processCount(0), lastCpuCheck(0), cpuLoad(0.0f),
                             voiceAge(0), delayBuffer(nullptr) {
  // Initialize LFO sine lookup table (once, shared across instances)
  initLFOTable();
  
  // Initialize voices
  for (int i = 0; i < MAX_VOICES; i++) {
    resetVoice(i);
  }
  
  // Initialize sample buffers (16 sequencer + 8 XTRA pads)
  for (int i = 0; i < MAX_PADS; i++) {
    sampleBuffers[i] = nullptr;
    sampleLengths[i] = 0;
  }
  
  // Initialize FX (existing global filter/lofi chain)
  fx.filterType = FILTER_NONE;
  fx.cutoff = 8000.0f;
  fx.resonance = 1.0f;
  fx.gain = 0.0f;
  fx.bitDepth = 16;
  fx.distortion = 0.0f;
  fx.sampleRate = SAMPLE_RATE;
  fx.state.x1 = fx.state.x2 = 0.0f;
  fx.state.y1 = fx.state.y2 = 0.0f;
  fx.srHold = 0;
  fx.srCounter = 0;
  distortionMode = DIST_SOFT;
  calculateBiquadCoeffs();
  
  // Initialize per-track and per-pad filters
  for (int i = 0; i < MAX_AUDIO_TRACKS; i++) {
    trackFilters[i].filterType = FILTER_NONE;
    trackFilters[i].cutoff = 1000.0f;
    trackFilters[i].resonance = 1.0f;
    trackFilters[i].gain = 0.0f;
    trackFilters[i].bitDepth = 16;
    trackFilters[i].distortion = 0.0f;
    trackFilters[i].state.x1 = trackFilters[i].state.x2 = 0.0f;
    trackFilters[i].state.y1 = trackFilters[i].state.y2 = 0.0f;
    trackFilterActive[i] = false;
    trackDistortionMode[i] = DIST_SOFT;
  }
  
  for (int i = 0; i < MAX_PADS; i++) {
    padFilters[i].filterType = FILTER_NONE;
    padFilters[i].cutoff = 1000.0f;
    padFilters[i].resonance = 1.0f;
    padFilters[i].gain = 0.0f;
    padFilters[i].bitDepth = 16;
    padFilters[i].distortion = 0.0f;
    padFilters[i].state.x1 = padFilters[i].state.x2 = 0.0f;
    padFilters[i].state.y1 = padFilters[i].state.y2 = 0.0f;
    padFilterActive[i] = false;
    padDistortionMode[i] = DIST_SOFT;
    
    // Scratch state
    scratchState[i].lfoPhase = 0.0f;
    scratchState[i].lfoRate = 5.0f;
    scratchState[i].depth = 0.85f;
    scratchState[i].lpState1 = 0.0f;
    scratchState[i].lpState2 = 0.0f;
    scratchState[i].noiseState = 12345 + i * 7919;
    scratchState[i].filterCutoff = 4000.0f;
    scratchState[i].crackleAmount = 0.25f;
    
    // Turntablism state
    turntablismState[i].mode = 0;
    turntablismState[i].modeTimer = 35280;
    turntablismState[i].gatePhase = 0.0f;
    turntablismState[i].lpState1 = 0.0f;
    turntablismState[i].lpState2 = 0.0f;
    turntablismState[i].noiseState = 67890 + i * 6271;
    turntablismState[i].autoMode = true;
    turntablismState[i].brakeLen = 15435;
    turntablismState[i].backspinLen = 19845;
    turntablismState[i].transformRate = 11.0f;
    turntablismState[i].vinylNoise = 0.35f;
    
    // Reverse / Pitch / Stutter state
    sampleReversed[i] = false;
    trackPitchShift[i] = 1.0f;
    stutterActive[i] = false;
    stutterInterval[i] = 100;
    padLoopEnabled[i] = false;
  }
  
  // Initialize volume
  masterVolume = 100;
  sequencerVolume = 10;
  liveVolume = 80;
  livePitchShift = 1.0f;
  
  // ============= Initialize NEW Master Effects =============
  
  // Delay/Echo - allocate buffer in PSRAM
  delayBuffer = (float*)ps_malloc(DELAY_BUFFER_SIZE * sizeof(float));
  if (delayBuffer) {
    memset(delayBuffer, 0, DELAY_BUFFER_SIZE * sizeof(float));
    Serial.printf("[AudioEngine] Delay buffer allocated: %d bytes in PSRAM\n", 
                  DELAY_BUFFER_SIZE * (int)sizeof(float));
  } else {
    Serial.println("[AudioEngine] WARNING: Failed to allocate delay buffer in PSRAM!");
  }
  delayParams.active = false;
  delayParams.time = 250.0f;
  delayParams.feedback = 0.3f;
  delayParams.mix = 0.3f;
  delayParams.delaySamples = (uint32_t)(250.0f * SAMPLE_RATE / 1000.0f);
  delayParams.writePos = 0;
  
  // Phaser
  phaserParams.active = false;
  phaserParams.rate = 0.5f;
  phaserParams.depth = 0.7f;
  phaserParams.feedback = 0.3f;
  phaserParams.lastOutput = 0.0f;
  for (int i = 0; i < PHASER_STAGES; i++) {
    phaserParams.stages[i] = {0, 0, 0, 0};
  }
  phaserParams.lfo.phase = 0;
  phaserParams.lfo.depth = 1.0f;  // LFO siempre [-1,+1]
  phaserParams.lfo.waveform = LFO_SINE;
  updateLFOPhaseInc(phaserParams.lfo, 0.5f);
  
  // Flanger
  memset(flangerBuffer, 0, sizeof(flangerBuffer));
  flangerParams.active = false;
  flangerParams.rate = 0.3f;
  flangerParams.depth = 0.5f;
  flangerParams.feedback = 0.4f;
  flangerParams.mix = 0.5f;
  flangerParams.writePos = 0;
  flangerParams.lfo.phase = 0;
  flangerParams.lfo.depth = 1.0f;  // LFO siempre [-1,+1]
  flangerParams.lfo.waveform = LFO_SINE;
  updateLFOPhaseInc(flangerParams.lfo, 0.3f);
  
  // Compressor
  compressorParams.active = false;
  compressorParams.threshold = 0.5f;
  compressorParams.ratio = 4.0f;
  compressorParams.attackCoeff = expf(-1.0f / (SAMPLE_RATE * 0.010f));   // 10ms
  compressorParams.releaseCoeff = expf(-1.0f / (SAMPLE_RATE * 0.100f));  // 100ms
  compressorParams.makeupGain = 1.0f;
  compressorParams.envelope = 0.0f;
  
  // ============= Initialize Per-track Live FX (SLAVE controller) =============
  trackFlangerBuffers = (float*)ps_calloc(MAX_AUDIO_TRACKS * TRACK_FLANGER_BUF, sizeof(float));
  trackFxInputBuf = (float*)ps_calloc(MAX_AUDIO_TRACKS * DMA_BUF_LEN, sizeof(float));
  for (int i = 0; i < MAX_AUDIO_TRACKS; i++) {
    trackEchoBuffer[i] = nullptr;
    trackEcho[i].active = false;
    trackEcho[i].time = 100.0f;
    trackEcho[i].feedback = 0.4f;
    trackEcho[i].mix = 0.5f;
    trackEcho[i].delaySamples = 4410;
    trackEcho[i].writePos = 0;
    trackFlanger[i].active = false;
    trackFlanger[i].rate = 0.5f;
    trackFlanger[i].depth = 0.5f;
    trackFlanger[i].feedback = 0.3f;
    trackFlanger[i].writePos = 0;
    trackFlanger[i].lfo.phase = 0;
    trackFlanger[i].lfo.depth = 1.0f;  // LFO siempre [-1,+1]
    trackFlanger[i].lfo.waveform = LFO_SINE;
    updateLFOPhaseInc(trackFlanger[i].lfo, 0.5f);
    trackComp[i].active = false;
    trackComp[i].threshold = 0.5f;
    trackComp[i].ratio = 4.0f;
    trackComp[i].attackCoeff = expf(-1.0f / (SAMPLE_RATE * 0.002f));   // 2ms attack (fast for drums)
    trackComp[i].releaseCoeff = expf(-1.0f / (SAMPLE_RATE * 0.060f));  // 60ms release (punchy)
    trackComp[i].envelope = 0.0f;
    sidechain.envelope[i] = 0.0f;
    sidechain.holdSamples[i] = 0;
  }
  sidechain.active = false;
  sidechain.sourceTrack = 0;
  sidechain.destinationMask = 0;
  sidechain.amount = 0.0f;
  sidechain.knee = 0.4f;
  sidechain.attackCoeff = expf(-1.0f / (SAMPLE_RATE * 0.006f));   // 6ms
  sidechain.releaseCoeff = expf(-1.0f / (SAMPLE_RATE * 0.160f));  // 160ms
  Serial.printf("[AudioEngine] Per-track live FX buffers: flanger=%d bytes, input=%d bytes PSRAM\n",
                MAX_AUDIO_TRACKS * TRACK_FLANGER_BUF * (int)sizeof(float),
                MAX_AUDIO_TRACKS * DMA_BUF_LEN * (int)sizeof(float));
  
  // Clear mix accumulator
  memset(mixAcc, 0, sizeof(mixAcc));
  
  // Initialize peak tracking
  memset((void*)trackPeaks, 0, sizeof(trackPeaks));
  memset(trackPeakDecay, 0, sizeof(trackPeakDecay));
  masterPeak = 0.0f;
  masterPeakDecay = 0.0f;
}

AudioEngine::~AudioEngine() {
  i2s_driver_uninstall(i2sPort);
  if (delayBuffer) {
    free(delayBuffer);
    delayBuffer = nullptr;
  }
  // Free per-track live FX buffers
  for (int i = 0; i < MAX_AUDIO_TRACKS; i++) {
    if (trackEchoBuffer[i]) { free(trackEchoBuffer[i]); trackEchoBuffer[i] = nullptr; }
  }
  if (trackFlangerBuffers) { free(trackFlangerBuffers); trackFlangerBuffers = nullptr; }
  if (trackFxInputBuf) { free(trackFxInputBuf); trackFxInputBuf = nullptr; }
}

// ============= LFO Initialization =============

void AudioEngine::initLFOTable() {
  if (lfoTableInitialized) return;
  // Full sine table (256 entries) - inspired by torvalds/AudioNoise lfo.h quarter-sine approach
  for (int i = 0; i < LFO_TABLE_SIZE; i++) {
    lfoSineTable[i] = sinf(2.0f * PI * (float)i / (float)LFO_TABLE_SIZE);
  }
  lfoTableInitialized = true;
  Serial.println("[AudioEngine] LFO sine table initialized");
}

void AudioEngine::updateLFOPhaseInc(LFOState& lfo, float rateHz) {
  // Phase increment per sample: rate * 2^32 / SAMPLE_RATE
  // Using 64-bit intermediate to avoid overflow
  lfo.phaseInc = (uint32_t)((double)rateHz * 4294967296.0 / (double)SAMPLE_RATE);
}

// LFO tick - returns value in range [-depth, +depth]
// Inspired by torvalds/AudioNoise lfo.h phase accumulator pattern
float AudioEngine::lfoTick(LFOState& lfo) {
  lfo.phase += lfo.phaseInc;
  
  // Extract table index from top 8 bits of 32-bit phase
  uint8_t idx = lfo.phase >> 24;
  
  switch (lfo.waveform) {
    case LFO_SINE:
      return lfoSineTable[idx] * lfo.depth;
      
    case LFO_TRIANGLE: {
      float t = (float)(lfo.phase >> 16) / 65536.0f;  // 0.0 - 1.0
      float tri = (t < 0.5f) ? (4.0f * t - 1.0f) : (3.0f - 4.0f * t);
      return tri * lfo.depth;
    }
    
    case LFO_SAWTOOTH: {
      float saw = 2.0f * (float)(lfo.phase >> 16) / 65536.0f - 1.0f;
      return saw * lfo.depth;
    }
    
    default:
      return 0.0f;
  }
}

bool AudioEngine::begin(int bckPin, int wsPin, int dataPin) {
  // I2S configuration para DAC externo
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = DMA_BUF_COUNT,
    .dma_buf_len = DMA_BUF_LEN,
    .use_apll = true,           // APLL = better audio clock accuracy
    .tx_desc_auto_clear = true,
    .fixed_mclk = 0
  };
  
  // I2S pin configuration
  i2s_pin_config_t pin_config = {
    .bck_io_num = bckPin,
    .ws_io_num = wsPin,
    .data_out_num = dataPin,
    .data_in_num = I2S_PIN_NO_CHANGE
  };
  
  // Install and start I2S driver
  esp_err_t err = i2s_driver_install(i2sPort, &i2s_config, 0, NULL);
  if (err != ESP_OK) {
    Serial.printf("I2S driver install failed: %d\n", err);
    return false;
  }
  
  err = i2s_set_pin(i2sPort, &pin_config);
  if (err != ESP_OK) {
    Serial.printf("I2S set pin failed: %d\n", err);
    return false;
  }
  
  // Set I2S clock
  i2s_set_clk(i2sPort, SAMPLE_RATE, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);
  
  Serial.println("I2S External DAC initialized successfully");
  return true;
}

bool AudioEngine::setSampleBuffer(int padIndex, int16_t* buffer, uint32_t length) {
  if (padIndex < 0 || padIndex >= MAX_PADS) return false;
  
  sampleBuffers[padIndex] = buffer;
  sampleLengths[padIndex] = length;
  
  Serial.printf("[AudioEngine] Sample buffer set: Pad %d, Buffer: %p, Length: %d samples\n", 
                padIndex, buffer, length);
  
  return true;
}

void AudioEngine::triggerSample(int padIndex, uint8_t velocity) {
  triggerSampleLive(padIndex, velocity);
}

void AudioEngine::triggerSampleSequencer(int padIndex, uint8_t velocity, uint8_t trackVolume, uint32_t maxSamples) {
  if (padIndex < 0 || padIndex >= MAX_PADS || sampleBuffers[padIndex] == nullptr) return;

  if (padIndex < MAX_AUDIO_TRACKS) {
    triggerSidechain(padIndex, velocity);
  }
  
  int voiceIndex = findFreeVoice();
  if (voiceIndex < 0) return; // Voice stealing handled inside findFreeVoice
  
  voices[voiceIndex].buffer = sampleBuffers[padIndex];
  voices[voiceIndex].position = 0;
  voices[voiceIndex].length = sampleLengths[padIndex];
  voices[voiceIndex].maxLength = maxSamples;  // 0 = full sample
  voices[voiceIndex].active = true;
  voices[voiceIndex].velocity = velocity;
  voices[voiceIndex].volume = constrain((sequencerVolume * trackVolume) / 100, 0, 150);
  voices[voiceIndex].pitchShift = trackPitchShift[padIndex];
  voices[voiceIndex].loop = false;
  voices[voiceIndex].padIndex = padIndex;
  voices[voiceIndex].isLivePad = false;
  voices[voiceIndex].startAge = ++voiceAge;
  voices[voiceIndex].filterState = {0.0f, 0.0f, 0.0f, 0.0f}; // Clear biquad state to avoid transient artifacts
  voices[voiceIndex].scratchPos = 0.0f;
}

void AudioEngine::triggerSampleLive(int padIndex, uint8_t velocity) {
  if (padIndex < 0 || padIndex >= MAX_PADS || sampleBuffers[padIndex] == nullptr) return;
  
  int voiceIndex = findFreeVoice();
  if (voiceIndex < 0) return;
  
  voices[voiceIndex].buffer = sampleBuffers[padIndex];
  voices[voiceIndex].position = 0;
  voices[voiceIndex].length = sampleLengths[padIndex];
  voices[voiceIndex].maxLength = 0;  // Live pads always play full sample
  voices[voiceIndex].active = true;
  voices[voiceIndex].velocity = velocity;
  voices[voiceIndex].volume = constrain((liveVolume * 120) / 100, 0, 180);
  voices[voiceIndex].pitchShift = (trackPitchShift[padIndex] != 1.0f) ? trackPitchShift[padIndex] : livePitchShift;
  voices[voiceIndex].loop = padLoopEnabled[padIndex];
  voices[voiceIndex].loopStart = 0;
  voices[voiceIndex].loopEnd = sampleLengths[padIndex];
  voices[voiceIndex].padIndex = padIndex;
  voices[voiceIndex].isLivePad = true;
  voices[voiceIndex].startAge = ++voiceAge;
  voices[voiceIndex].filterState = {0.0f, 0.0f, 0.0f, 0.0f}; // Clear biquad state
  voices[voiceIndex].scratchPos = 0.0f;
}

void AudioEngine::setPadLoop(int padIndex, bool enabled) {
  if (padIndex < 0 || padIndex >= MAX_PADS) return;
  padLoopEnabled[padIndex] = enabled;
  // Update any currently active voices for this pad
  for (int i = 0; i < MAX_VOICES; i++) {
    if (voices[i].active && voices[i].padIndex == padIndex) {
      voices[i].loop = enabled;
      if (enabled) {
        voices[i].loopStart = 0;
        voices[i].loopEnd = sampleLengths[padIndex];
      }
    }
  }
  Serial.printf("[Audio] Pad %d loop: %s\n", padIndex, enabled ? "ON" : "OFF");
}

bool AudioEngine::isPadLooping(int padIndex) {
  if (padIndex < 0 || padIndex >= MAX_PADS) return false;
  return padLoopEnabled[padIndex];
}

void AudioEngine::stopSample(int padIndex) {
  // Stop all voices playing this sample
  for (int i = 0; i < MAX_VOICES; i++) {
    if (voices[i].active && voices[i].buffer == sampleBuffers[padIndex]) {
      voices[i].active = false;
    }
  }
}

void AudioEngine::stopAll() {
  for (int i = 0; i < MAX_VOICES; i++) {
    voices[i].active = false;
  }
}

void AudioEngine::setLivePitchShift(float pitch) {
  livePitchShift = constrain(pitch, 0.25f, 3.0f);
  // Apply to all currently active live pad voices
  for (int i = 0; i < MAX_VOICES; i++) {
    if (voices[i].active && voices[i].isLivePad) {
      voices[i].pitchShift = livePitchShift;
    }
  }
}

float AudioEngine::getLivePitchShift() {
  return livePitchShift;
}

void AudioEngine::setPitch(int voiceIndex, float pitch) {
  if (voiceIndex < 0 || voiceIndex >= MAX_VOICES) return;
  voices[voiceIndex].pitchShift = pitch;
}

void AudioEngine::setLoop(int voiceIndex, bool loop, uint32_t start, uint32_t end) {
  if (voiceIndex < 0 || voiceIndex >= MAX_VOICES) return;
  
  voices[voiceIndex].loop = loop;
  voices[voiceIndex].loopStart = start;
  voices[voiceIndex].loopEnd = end > 0 ? end : voices[voiceIndex].length;
}

void IRAM_ATTR AudioEngine::process() {
  // Fill mix buffer
  fillBuffer(mixBuffer, DMA_BUF_LEN);
  
  // Write to I2S External DAC
  size_t bytes_written;
  i2s_write(i2sPort, mixBuffer, DMA_BUF_LEN * 4, &bytes_written, portMAX_DELAY);
  
  // Update CPU load calculation (lightweight, no serial)
  processCount++;
  uint32_t now = millis();
  if (now - lastCpuCheck > 1000) {
    cpuLoad = (processCount * DMA_BUF_LEN * 1000.0f) / (SAMPLE_RATE * (now - lastCpuCheck));
    processCount = 0;
    lastCpuCheck = now;
  }
}

void IRAM_ATTR AudioEngine::fillBuffer(int16_t* buffer, size_t samples) {
  // Clear output buffer and accumulator
  memset(buffer, 0, samples * sizeof(int16_t) * 2);
  memset(mixAcc, 0, samples * sizeof(int32_t) * 2);
  
  // Determine which tracks have per-track live FX and clear their input buffers
  bool trackHasLiveFx[MAX_AUDIO_TRACKS] = {};
  if (trackFxInputBuf) {
    for (int t = 0; t < MAX_AUDIO_TRACKS; t++) {
      trackHasLiveFx[t] = (trackEcho[t].active && trackEchoBuffer[t]) ||
                           trackFlanger[t].active || trackComp[t].active;
      if (trackHasLiveFx[t]) {
        memset(&trackFxInputBuf[t * DMA_BUF_LEN], 0, DMA_BUF_LEN * sizeof(float));
      }
    }
  }

  static float sidechainGain[MAX_AUDIO_TRACKS][DMA_BUF_LEN];
  for (int t = 0; t < MAX_AUDIO_TRACKS; t++) {
    for (size_t i = 0; i < samples; i++) {
      if (!sidechain.active) {
        sidechainGain[t][i] = 1.0f;
        sidechain.envelope[t] = 0.0f;
        sidechain.holdSamples[t] = 0;
        continue;
      }

      bool targeted = (sidechain.destinationMask & (1U << t)) != 0;
      if (!targeted || t == sidechain.sourceTrack) {
        sidechainGain[t][i] = 1.0f;
        sidechain.envelope[t] = 0.0f;
        sidechain.holdSamples[t] = 0;
        continue;
      }

      float target = (sidechain.holdSamples[t] > 0) ? 1.0f : 0.0f;
      float env = sidechain.envelope[t];
      float coeff = (target > env) ? sidechain.attackCoeff : sidechain.releaseCoeff;
      env = coeff * env + (1.0f - coeff) * target;
      sidechain.envelope[t] = env;
      if (sidechain.holdSamples[t] > 0) sidechain.holdSamples[t]--;

      float shaped = powf(constrain(env, 0.0f, 1.0f), 1.0f + sidechain.knee * 3.0f);
      float gain = 1.0f - sidechain.amount * shaped;
      if (gain < 0.08f) gain = 0.08f;
      sidechainGain[t][i] = gain;
    }
  }
  
  // Mix all active voices
  for (int v = 0; v < MAX_VOICES; v++) {
    if (!voices[v].active) continue;
    
    Voice& voice = voices[v];
    
    // Detect scratch/turntablism special processing for live pad voices
    FilterType specialFxType = FILTER_NONE;
    if (voice.isLivePad && voice.padIndex >= 0 && voice.padIndex < MAX_PADS && padFilterActive[voice.padIndex]) {
      FilterType ft = padFilters[voice.padIndex].filterType;
      if (ft == FILTER_SCRATCH || ft == FILTER_TURNTABLISM) specialFxType = ft;
    }
    
    if (specialFxType != FILTER_NONE) {
      // ====== SCRATCH / TURNTABLISM - Special vinyl DSP ======
      int pi = voice.padIndex;
      float fLen = (float)voice.length;
      
      for (size_t i = 0; i < samples; i++) {
        float posAdvance = 1.0f;
        float vinylFilterCutoff = 4000.0f;
        bool addCrackle = false;
        bool gateOff = false;
        
        if (specialFxType == FILTER_SCRATCH) {
          // Scratch LFO: triangle wave for natural vinyl scratch feel
          ScratchState& ss = scratchState[pi];
          ss.lfoPhase += ss.lfoRate / (float)SAMPLE_RATE;
          if (ss.lfoPhase >= 1.0f) ss.lfoPhase -= 1.0f;
          // Triangle wave: -1 to +1  
          float tri = (ss.lfoPhase < 0.5f) ? (ss.lfoPhase * 4.0f - 1.0f) : (3.0f - ss.lfoPhase * 4.0f);
          // Scratch speed: oscillates forward and backward
          posAdvance = tri * ss.depth * 3.0f;
          // Vinyl filter: configurable cutoff, brighter when moving fast
          vinylFilterCutoff = (ss.filterCutoff * 0.075f) + fabsf(posAdvance) * ss.filterCutoff * 0.875f;
          addCrackle = true;
          
        } else { // FILTER_TURNTABLISM
          TurntablismState& ts = turntablismState[pi];
          if (ts.modeTimer == 0) {
            if (ts.autoMode) {
              ts.mode = (ts.mode + 1) % 4;
            }
            // else in manual mode, stay on current mode and restart timer
            switch (ts.mode) {
              case 0: ts.modeTimer = 33075; break;                 // Normal ~750ms
              case 1: ts.modeTimer = ts.brakeLen; break;           // Brake (configurable)
              case 2: ts.modeTimer = ts.backspinLen; break;        // Backspin (configurable)
              case 3: ts.modeTimer = 24255; ts.gatePhase = 0; break; // Transform ~550ms
            }
          }
          ts.modeTimer--;
          
          switch (ts.mode) {
            case 0: // Normal playback
              posAdvance = 1.0f;
              vinylFilterCutoff = 12000.0f;
              break;
            case 1: { // Vinyl brake - pitch ramps down  
              float progress = 1.0f - (float)ts.modeTimer / (float)ts.brakeLen;
              posAdvance = 1.0f - progress * 0.97f; // Down to 0.03
              vinylFilterCutoff = 10000.0f * (1.0f - progress * 0.92f) + 150.0f;
              addCrackle = (progress > 0.7f);
              break;
            }
            case 2: { // Backspin - reverse playback
              float progress = (float)ts.modeTimer / (float)ts.backspinLen;
              posAdvance = -1.8f * progress * progress; // Quadratic deceleration
              vinylFilterCutoff = 1500.0f + progress * 2500.0f;
              addCrackle = true;
              break;
            }
            case 3: { // Stutter / transform scratch
              ts.gatePhase += ts.transformRate * 6.28318f / (float)SAMPLE_RATE;
              if (ts.gatePhase > 6.28318f) ts.gatePhase -= 6.28318f;
              float gate = (ts.gatePhase < 3.14159f) ? 1.0f : 0.0f;
              posAdvance = gate;
              gateOff = (gate == 0.0f);
              vinylFilterCutoff = 5000.0f;
              break;
            }
          }
        }
        
        // Advance float position with wrapping (sample loops in scratch mode)
        voice.scratchPos += posAdvance;
        while (voice.scratchPos >= fLen) voice.scratchPos -= fLen;
        while (voice.scratchPos < 0.0f) voice.scratchPos += fLen;
        
        int readPos = (int)voice.scratchPos;
        if (readPos < 0) readPos = 0;
        if (readPos >= (int)voice.length) readPos = (int)voice.length - 1;
        
        // Read and scale sample
        int32_t scaled = ((int32_t)voice.buffer[readPos] * voice.velocity * voice.volume) / 12700;
        float fSample = (float)constrain(scaled, -32768, 32767) / 32768.0f;
        
        // Hard gate for stutter OFF segments
        if (gateOff) {
          fSample = 0.0f;
        } else {
          // Vinyl character: 2-pole one-pole LP (warm analog tone)
          float alpha = vinylFilterCutoff / (vinylFilterCutoff + (float)SAMPLE_RATE * 0.159155f);
          
          if (specialFxType == FILTER_SCRATCH) {
            ScratchState& ss = scratchState[pi];
            ss.lpState1 += alpha * (fSample - ss.lpState1);
            ss.lpState2 += alpha * (ss.lpState1 - ss.lpState2);
            fSample = ss.lpState2;
            
            // Vinyl crackle: sparse random pops (configurable intensity)
            ss.noiseState = ss.noiseState * 1103515245u + 12345u;
            uint8_t crackleThreshold = (uint8_t)(ss.crackleAmount * 28.0f); // 0-28 range
            if ((ss.noiseState >> 24) < crackleThreshold) {
              float crackle = (float)((int32_t)(ss.noiseState >> 16) - 32768) / 32768.0f;
              fSample += crackle * (0.015f + ss.crackleAmount * 0.035f);
            }
          } else {
            TurntablismState& ts = turntablismState[pi];
            ts.lpState1 += alpha * (fSample - ts.lpState1);
            ts.lpState2 += alpha * (ts.lpState1 - ts.lpState2);
            fSample = ts.lpState2;
            
            // Backspin and brake vinyl noise (configurable)
            if (addCrackle) {
              ts.noiseState = ts.noiseState * 1103515245u + 12345u;
              uint8_t noiseThreshold = (uint8_t)(ts.vinylNoise * 28.0f); // 0-28 range
              if ((ts.noiseState >> 24) < noiseThreshold) {
                float crackle = (float)((int32_t)(ts.noiseState >> 16) - 32768) / 32768.0f;
                fSample += crackle * (0.02f + ts.vinylNoise * 0.04f);
              }
            }
          }
        }
        
        // Clamp and mix
        int16_t out = (int16_t)(fSample * 32768.0f);
        if (out > 32767) out = 32767;
        if (out < -32768) out = -32768;
        
        mixAcc[i * 2] += out;
        mixAcc[i * 2 + 1] += out;
      }

      // Keep voice alive (scratch loops indefinitely)
      voice.position = (uint32_t)voice.scratchPos;
      continue;
    }
    
    // ====== NORMAL VOICE PROCESSING ======
    const bool hasPitchShift = (voice.pitchShift < 0.99f || voice.pitchShift > 1.01f);
    if (hasPitchShift) voice.scratchPos = (float)voice.position;
    
    // Effective length: limited by note length if set (maxLength > 0)
    const uint32_t effectiveLength = (voice.maxLength > 0 && voice.maxLength < voice.length)
                                      ? voice.maxLength : voice.length;
    
    for (size_t i = 0; i < samples; i++) {
      if (voice.position >= effectiveLength) {
        if (voice.loop && voice.loopEnd > voice.loopStart && voice.maxLength == 0) {
          voice.position = voice.loopStart;
          if (hasPitchShift) voice.scratchPos = (float)voice.loopStart;
        } else {
          voice.active = false;
          break;
        }
      }
      
      // Get sample and apply velocity + volume in one step
      int32_t scaled = ((int32_t)voice.buffer[voice.position] * voice.velocity * voice.volume) / 12700;
      
      // Apply per-pad or per-track filter if active (using per-voice state)
      int16_t filtered = (int16_t)constrain(scaled, -32768, 32767);
      if (voice.padIndex >= 0 && voice.padIndex < MAX_PADS) {
        FXParams* chFx = nullptr;
        DistortionMode chDistMode = DIST_SOFT;
        
        if (voice.isLivePad && padFilterActive[voice.padIndex]) {
          chFx = &padFilters[voice.padIndex];
          chDistMode = padDistortionMode[voice.padIndex];
        } else if (!voice.isLivePad && voice.padIndex < MAX_AUDIO_TRACKS && trackFilterActive[voice.padIndex]) {
          chFx = &trackFilters[voice.padIndex];
          chDistMode = trackDistortionMode[voice.padIndex];
        }
        
        if (chFx) {
          // 1. Per-channel distortion
          if (chFx->distortion > 0.1f) {
            float x = (float)filtered / 32768.0f;
            float amt = chFx->distortion / 100.0f;
            x *= (1.0f + amt * 3.0f);
            switch (chDistMode) {
              case DIST_HARD:
                if (x > 1.0f) x = 1.0f; else if (x < -1.0f) x = -1.0f;
                break;
              case DIST_TUBE:
                x = (x >= 0.0f) ? (1.0f - expf(-x)) : -(1.0f - expf(x * 1.2f));
                break;
              case DIST_FUZZ:
                x = x / (1.0f + fabsf(x)); x *= 2.0f; x = x / (1.0f + fabsf(x));
                break;
              default: // DIST_SOFT
                x = x / (1.0f + fabsf(x));
                break;
            }
            filtered = (int16_t)(x * 32768.0f);
          }
          
          // 2. Per-channel biquad filter
          if (chFx->filterType != FILTER_NONE) {
            float x = (float)filtered;
            float y = chFx->coeffs.b0 * x + voice.filterState.x1;
            voice.filterState.x1 = chFx->coeffs.b1 * x - chFx->coeffs.a1 * y + voice.filterState.x2;
            voice.filterState.x2 = chFx->coeffs.b2 * x - chFx->coeffs.a2 * y;
            if (y > 32767.0f) y = 32767.0f;
            else if (y < -32768.0f) y = -32768.0f;
            filtered = (int16_t)y;
          }
          
          // 3. Per-channel bitcrush
          if (chFx->bitDepth < 16) {
            int shift = 16 - chFx->bitDepth;
            filtered = (filtered >> shift) << shift;
          }
        }
      }

      if (!voice.isLivePad && voice.padIndex >= 0 && voice.padIndex < MAX_AUDIO_TRACKS) {
        filtered = (int16_t)(filtered * sidechainGain[voice.padIndex][i]);
      }
      
      // Mix to accumulator or per-track live FX input
      if (!voice.isLivePad && voice.padIndex >= 0 && voice.padIndex < MAX_AUDIO_TRACKS && trackHasLiveFx[voice.padIndex]) {
        trackFxInputBuf[voice.padIndex * DMA_BUF_LEN + i] += (float)filtered / 32768.0f;
      } else {
        mixAcc[i * 2] += filtered;
        mixAcc[i * 2 + 1] += filtered;
      }
      
      // Track per-track peak levels for VU meters
      if (voice.padIndex >= 0 && voice.padIndex < MAX_AUDIO_TRACKS) {
        float absF = fabsf((float)filtered / 32768.0f);
        if (absF > trackPeakDecay[voice.padIndex]) trackPeakDecay[voice.padIndex] = absF;
      }
      
      // Advance position (with pitch shift support)
      if (hasPitchShift) {
        voice.scratchPos += voice.pitchShift;
        voice.position = (uint32_t)voice.scratchPos;
      } else {
        voice.position++;
      }
    }
  }
  
  // Process per-track live FX (echo, flanger, compressor) - SLAVE controller
  // Update per-track peaks and apply decay
  for (int t = 0; t < MAX_AUDIO_TRACKS; t++) {
    trackPeaks[t] = trackPeakDecay[t];
    trackPeakDecay[t] *= 0.92f; // Fast decay ~60ms at 128-sample blocks
    
    bool hasEcho = trackEcho[t].active && trackEchoBuffer[t];
    bool hasFlanger = trackFlanger[t].active && trackFlangerBuffers;
    bool hasComp = trackComp[t].active;
    if (!hasEcho && !hasFlanger && !hasComp) continue;
    
    for (size_t i = 0; i < samples; i++) {
      float s = trackFxInputBuf ? trackFxInputBuf[t * DMA_BUF_LEN + i] : 0.0f;
      if (hasEcho) s = processTrackEcho(t, s);
      if (hasFlanger) s = processTrackFlanger(t, s);
      if (hasComp) s = processTrackCompressor(t, s);
      int32_t out = (int32_t)(s * 32768.0f);
      mixAcc[i * 2] += out;
      mixAcc[i * 2 + 1] += out;
    }
  }
  
  // Check which FX chains are active
  const bool hasOldFX = (fx.distortion > 0.1f) || (fx.filterType != FILTER_NONE) || 
                        (fx.sampleRate < SAMPLE_RATE) || (fx.bitDepth < 16);
  const bool hasNewFX = delayParams.active || phaserParams.active || 
                        flangerParams.active || compressorParams.active;
  
  // Process mono signal with master volume, soft clipping, and FX chains
  for (size_t i = 0; i < samples; i++) {
    // Mono from accumulator (L=R for drum samples)
    int32_t val = (mixAcc[i * 2] * masterVolume) / 100;
    
    // Normalize to float [-1.0, 1.0] range for processing
    float fval = (float)val / 32768.0f;
    
    // Soft clipping with knee (replaces old hard clamp)
    // Inspired by torvalds/AudioNoise limit_value: x / (1 + |x|)
    fval = softClipKnee(fval);
    
    // Convert back to int16 for legacy FX chain
    int16_t sample = (int16_t)(fval * 32767.0f);
    
    // Apply legacy FX chain (distortion, filter, SR reduction, bitcrush)
    if (hasOldFX) {
      sample = processFX(sample);
    }
    
    // Apply NEW master effects chain (in float domain for precision)
    if (hasNewFX) {
      float fs = (float)sample / 32768.0f;
      
      // Phaser (4-stage cascaded allpass with LFO)
      if (phaserParams.active) fs = processPhaser(fs);
      
      // Flanger (short modulated delay)
      if (flangerParams.active) fs = processFlanger(fs);
      
      // Delay/Echo (longer delay with feedback)
      if (delayParams.active) fs = processDelay(fs);
      
      // Compressor/Limiter (dynamics control - last in chain)
      if (compressorParams.active) fs = processCompressor(fs);
      
      // Final safety limiter (Torvalds limit_value)
      fs = fs / (1.0f + fabsf(fs));
      fs *= 2.0f;  // Compensate limit_value gain loss
      
      sample = (int16_t)constrain((int32_t)(fs * 32767.0f), -32768, 32767);
    }
    
    // Write stereo output (mono source)
    buffer[i * 2] = sample;
    buffer[i * 2 + 1] = sample;
    
    // Track master peak level for VU meter
    float absVal = fabsf(fval);
    if (absVal > masterPeakDecay) masterPeakDecay = absVal;
  }
  
  // Update master peak (atomic-safe for single writer)
  masterPeak = masterPeakDecay;
  // Decay master peak
  masterPeakDecay *= 0.95f;
}

void AudioEngine::setSidechain(bool active, int sourceTrack, uint16_t destinationMask,
                               float amount, float attackMs, float releaseMs, float knee) {
  sidechain.active = active;
  sidechain.sourceTrack = constrain(sourceTrack, 0, MAX_AUDIO_TRACKS - 1);
  sidechain.destinationMask = destinationMask;
  sidechain.amount = constrain(amount, 0.0f, 1.0f);
  sidechain.knee = constrain(knee, 0.0f, 1.0f);

  float aMs = constrain(attackMs, 0.1f, 80.0f);
  float rMs = constrain(releaseMs, 10.0f, 1200.0f);
  sidechain.attackCoeff = expf(-1.0f / (SAMPLE_RATE * aMs / 1000.0f));
  sidechain.releaseCoeff = expf(-1.0f / (SAMPLE_RATE * rMs / 1000.0f));

  if (!active) {
    for (int i = 0; i < MAX_AUDIO_TRACKS; i++) {
      sidechain.envelope[i] = 0.0f;
      sidechain.holdSamples[i] = 0;
    }
  }

  Serial.printf("[AudioEngine] Sidechain %s src=%d mask=0x%04X amt=%.2f atk=%.1fms rel=%.1fms knee=%.2f\n",
                active ? "ON" : "OFF", sidechain.sourceTrack, sidechain.destinationMask,
                sidechain.amount, aMs, rMs, sidechain.knee);
}

void AudioEngine::triggerSidechain(int sourceTrack, uint8_t velocity) {
  if (!sidechain.active) return;
  if (sourceTrack != sidechain.sourceTrack) return;

  float velNorm = constrain((float)velocity / 127.0f, 0.25f, 1.0f);
  uint16_t hold = (uint16_t)(SAMPLE_RATE * (0.008f + 0.016f * velNorm)); // 8-24ms
  for (int t = 0; t < MAX_AUDIO_TRACKS; t++) {
    if (t == sidechain.sourceTrack) continue;
    if (sidechain.destinationMask & (1U << t)) {
      sidechain.holdSamples[t] = hold;
    }
  }
}

void AudioEngine::clearSidechain() {
  setSidechain(false, 0, 0, 0.0f, 6.0f, 160.0f, 0.4f);
}

int AudioEngine::findFreeVoice() {
  // 1. Look for inactive voice
  for (int i = 0; i < MAX_VOICES; i++) {
    if (!voices[i].active) return i;
  }
  
  // 2. Steal oldest voice (lowest startAge)
  int oldest = 0;
  uint32_t oldestAge = voices[0].startAge;
  for (int i = 1; i < MAX_VOICES; i++) {
    if (voices[i].startAge < oldestAge) {
      oldestAge = voices[i].startAge;
      oldest = i;
    }
  }
  return oldest;
}

void AudioEngine::resetVoice(int voiceIndex) {
  voices[voiceIndex].buffer = nullptr;
  voices[voiceIndex].position = 0;
  voices[voiceIndex].length = 0;
  voices[voiceIndex].maxLength = 0;
  voices[voiceIndex].active = false;
  voices[voiceIndex].velocity = 127;
  voices[voiceIndex].volume = 100;
  voices[voiceIndex].pitchShift = 1.0f;
  voices[voiceIndex].loop = false;
  voices[voiceIndex].loopStart = 0;
  voices[voiceIndex].loopEnd = 0;
  voices[voiceIndex].padIndex = -1;
  voices[voiceIndex].isLivePad = false;
  voices[voiceIndex].startAge = 0;
  voices[voiceIndex].filterState = {0.0f, 0.0f, 0.0f, 0.0f};
  voices[voiceIndex].scratchPos = 0.0f;
}

// ============= FX IMPLEMENTATION =============

void AudioEngine::setFilterType(FilterType type) {
  fx.filterType = type;
  calculateBiquadCoeffs();
}

void AudioEngine::setFilterCutoff(float cutoff) {
  fx.cutoff = constrain(cutoff, 100.0f, 16000.0f);
  calculateBiquadCoeffs();
}

void AudioEngine::setFilterResonance(float resonance) {
  fx.resonance = constrain(resonance, 0.5f, 20.0f);
  calculateBiquadCoeffs();
}

void AudioEngine::setBitDepth(uint8_t bits) {
  fx.bitDepth = constrain(bits, 4, 16);
}

void AudioEngine::setDistortion(float amount) {
  fx.distortion = constrain(amount, 0.0f, 100.0f);
}

void AudioEngine::setDistortionMode(DistortionMode mode) {
  distortionMode = mode;
  Serial.printf("[AudioEngine] Distortion mode: %d\n", mode);
}

void AudioEngine::setSampleRateReduction(uint32_t rate) {
  fx.sampleRate = constrain(rate, 8000, SAMPLE_RATE);
  fx.srCounter = 0;
}

// Volume Control
void AudioEngine::setMasterVolume(uint8_t volume) {
  masterVolume = constrain(volume, 0, 150);
}

uint8_t AudioEngine::getMasterVolume() {
  return masterVolume;
}

void AudioEngine::setSequencerVolume(uint8_t volume) {
  sequencerVolume = constrain(volume, 0, 150);
}

uint8_t AudioEngine::getSequencerVolume() {
  return sequencerVolume;
}

void AudioEngine::setLiveVolume(uint8_t volume) {
  liveVolume = constrain(volume, 0, 150);
}

uint8_t AudioEngine::getLiveVolume() {
  return liveVolume;
}

// Biquad filter coefficient calculation (optimized)
void AudioEngine::calculateBiquadCoeffs() {
  if (fx.filterType == FILTER_NONE) return;
  
  float omega = 2.0f * PI * fx.cutoff / SAMPLE_RATE;
  float sn = sinf(omega);
  float cs = cosf(omega);
  float alpha = sn / (2.0f * fx.resonance);
  
  switch (fx.filterType) {
    case FILTER_LOWPASS:
      fx.coeffs.b0 = (1.0f - cs) / 2.0f;
      fx.coeffs.b1 = 1.0f - cs;
      fx.coeffs.b2 = (1.0f - cs) / 2.0f;
      fx.coeffs.a1 = -2.0f * cs;
      fx.coeffs.a2 = 1.0f - alpha;
      break;
      
    case FILTER_HIGHPASS:
      fx.coeffs.b0 = (1.0f + cs) / 2.0f;
      fx.coeffs.b1 = -(1.0f + cs);
      fx.coeffs.b2 = (1.0f + cs) / 2.0f;
      fx.coeffs.a1 = -2.0f * cs;
      fx.coeffs.a2 = 1.0f - alpha;
      break;
      
    case FILTER_BANDPASS:
      fx.coeffs.b0 = alpha;
      fx.coeffs.b1 = 0.0f;
      fx.coeffs.b2 = -alpha;
      fx.coeffs.a1 = -2.0f * cs;
      fx.coeffs.a2 = 1.0f - alpha;
      break;
      
    case FILTER_NOTCH:
      fx.coeffs.b0 = 1.0f;
      fx.coeffs.b1 = -2.0f * cs;
      fx.coeffs.b2 = 1.0f;
      fx.coeffs.a1 = -2.0f * cs;
      fx.coeffs.a2 = 1.0f - alpha;
      break;
      
    default:
      break;
  }
  
  // Normalize by a0
  float a0 = 1.0f + alpha;
  fx.coeffs.b0 /= a0;
  fx.coeffs.b1 /= a0;
  fx.coeffs.b2 /= a0;
  fx.coeffs.a1 /= a0;
  fx.coeffs.a2 /= a0;
}

// Biquad filter processing (Direct Form II Transposed - optimized)
inline int16_t AudioEngine::applyFilter(int16_t input) {
  if (fx.filterType == FILTER_NONE) return input;
  
  float x = (float)input;
  float y = fx.coeffs.b0 * x + fx.state.x1;
  
  fx.state.x1 = fx.coeffs.b1 * x - fx.coeffs.a1 * y + fx.state.x2;
  fx.state.x2 = fx.coeffs.b2 * x - fx.coeffs.a2 * y;
  
  // Clamp to prevent overflow
  if (y > 32767.0f) y = 32767.0f;
  else if (y < -32768.0f) y = -32768.0f;
  
  return (int16_t)y;
}

// Bit crusher (super fast)
inline int16_t AudioEngine::applyBitCrush(int16_t input) {
  if (fx.bitDepth >= 16) return input;
  
  int shift = 16 - fx.bitDepth;
  return (input >> shift) << shift;
}

// Distortion with multiple modes (inspired by torvalds/AudioNoise distortion.h)
inline int16_t AudioEngine::applyDistortion(int16_t input) {
  if (fx.distortion < 0.1f) return input;
  
  float x = (float)input / 32768.0f;
  float amount = fx.distortion / 100.0f;
  
  // Drive boost
  x *= (1.0f + amount * 3.0f);
  
  switch (distortionMode) {
    case DIST_SOFT:
      // Torvalds limit_value: x / (1 + |x|) - smooth analog saturation
      x = x / (1.0f + fabsf(x));
      break;
      
    case DIST_HARD:
      // Hard clip at threshold
      if (x > 1.0f) x = 1.0f;
      else if (x < -1.0f) x = -1.0f;
      break;
      
    case DIST_TUBE: {
      // Asymmetric exponential saturation (torvalds/AudioNoise tube.h inspired)
      // Positive half: soft compression, negative half: harder clip
      if (x >= 0.0f) {
        x = 1.0f - expf(-x);           // Exponential saturation
      } else {
        x = -(1.0f - expf(x * 1.2f));  // Slightly harder on negative half
      }
      break;
    }
    
    case DIST_FUZZ:
      // Extreme: double soft clip for heavy saturation
      x = x / (1.0f + fabsf(x));
      x *= 2.0f;
      x = x / (1.0f + fabsf(x));
      break;
      
    default:
      x = x / (1.0f + fabsf(x));
      break;
  }
  
  return (int16_t)(x * 32768.0f);
}

// Complete FX chain (optimized order)
inline int16_t AudioEngine::processFX(int16_t input) {
  int16_t output = input;
  
  // 1. Distortion (before filtering for analog character)
  if (fx.distortion > 0.1f) {
    output = applyDistortion(output);
  }
  
  // 2. Filter
  if (fx.filterType != FILTER_NONE) {
    output = applyFilter(output);
  }
  
  // 3. Sample rate reduction (decimation)
  if (fx.sampleRate < SAMPLE_RATE) {
    uint32_t decimation = SAMPLE_RATE / fx.sampleRate;
    if (fx.srCounter++ >= decimation) {
      fx.srHold = output;
      fx.srCounter = 0;
    }
    output = fx.srHold;
  }
  
  // 4. Bit crush (last for lo-fi effect)
  if (fx.bitDepth < 16) {
    output = applyBitCrush(output);
  }
  
  return output;
}

int AudioEngine::getActiveVoices() {
  int count = 0;
  for (int i = 0; i < MAX_VOICES; i++) {
    if (voices[i].active) count++;
  }
  return count;
}

float AudioEngine::getCpuLoad() {
  return cpuLoad * 100.0f;
}

// ============= Peak Level Tracking =============

float AudioEngine::getTrackPeak(int track) {
  if (track < 0 || track >= MAX_AUDIO_TRACKS) return 0.0f;
  return trackPeaks[track];
}

float AudioEngine::getMasterPeak() {
  return masterPeak;
}

void AudioEngine::getTrackPeaks(float* outPeaks, int count) {
  int n = (count < MAX_AUDIO_TRACKS) ? count : MAX_AUDIO_TRACKS;
  for (int i = 0; i < n; i++) {
    outPeaks[i] = trackPeaks[i];
  }
}

// ============= NEW: Soft Clip with Knee =============
// Linear below ±0.9, smooth Torvalds-style limiting above
// Preserves dynamics for normal signals, only clips peaks
inline float AudioEngine::softClipKnee(float x) {
  const float knee = 0.9f;
  if (x > knee) {
    float excess = x - knee;
    return knee + (1.0f - knee) * excess / (1.0f + excess * 10.0f);
  } else if (x < -knee) {
    float excess = -x - knee;
    return -(knee + (1.0f - knee) * excess / (1.0f + excess * 10.0f));
  }
  return x;
}

// ============= NEW: Delay/Echo Processing =============
// Inspired by torvalds/AudioNoise echo.h circular buffer pattern

inline float AudioEngine::processDelay(float input) {
  if (!delayBuffer) return input;
  
  // Read from delay buffer (circular)
  uint32_t readPos = (delayParams.writePos + DELAY_BUFFER_SIZE - delayParams.delaySamples) 
                     % DELAY_BUFFER_SIZE;
  float delayed = delayBuffer[readPos];
  
  // Write input + feedback to delay buffer
  float writeVal = input + delayed * delayParams.feedback;
  // Prevent feedback runaway with Torvalds limit_value
  writeVal = writeVal / (1.0f + fabsf(writeVal));
  delayBuffer[delayParams.writePos] = writeVal;
  delayParams.writePos = (delayParams.writePos + 1) % DELAY_BUFFER_SIZE;
  
  // Mix dry and wet
  return input * (1.0f - delayParams.mix) + delayed * delayParams.mix;
}

// ============= NEW: Phaser Processing =============
// 4-stage cascaded allpass, LFO-modulated
// Inspired by torvalds/AudioNoise phaser.h

inline float AudioEngine::processPhaser(float input) {
  // Get LFO value (0.0 to 1.0 range for frequency sweep)
  float lfoVal = (lfoTick(phaserParams.lfo) + 1.0f) * 0.5f;
  
  // Map LFO to allpass coefficient
  // Sweep center frequency from ~200Hz to ~4000Hz
  float minFreq = 200.0f;
  float maxFreq = 4000.0f;
  float freq = minFreq + (maxFreq - minFreq) * lfoVal * phaserParams.depth;
  
  // 1st-order allpass coefficient from frequency
  // coeff = (1 - tan(pi*f/sr)) / (1 + tan(pi*f/sr))
  float omega = PI * freq / SAMPLE_RATE;
  // Fast tan approximation for small angles: tan(x) ≈ x + x³/3
  float tn = omega + (omega * omega * omega) * 0.333333f;
  float coeff = (1.0f - tn) / (1.0f + tn);
  
  // Mix feedback into input
  float x = input + phaserParams.lastOutput * phaserParams.feedback;
  
  // Cascade through 4 allpass stages
  // Each stage: y = coeff * x + x1 - coeff * y1 (1st-order allpass)
  for (int s = 0; s < PHASER_STAGES; s++) {
    float y = coeff * x + phaserParams.stages[s].x1 - coeff * phaserParams.stages[s].y1;
    phaserParams.stages[s].x1 = x;
    phaserParams.stages[s].y1 = y;
    x = y;
  }
  
  phaserParams.lastOutput = x;
  
  // Mix original and phased signal (50/50 for classic phaser)
  return (input + x) * 0.5f;
}

// ============= NEW: Flanger Processing =============
// Short LFO-modulated delay with feedback
// Inspired by torvalds/AudioNoise flanger.h

inline float AudioEngine::processFlanger(float input) {
  // Write to flanger buffer
  flangerBuffer[flangerParams.writePos] = input;
  
  // Get LFO-modulated delay in samples (0 to ~4ms = 0 to ~176 samples)
  float lfoVal = (lfoTick(flangerParams.lfo) + 1.0f) * 0.5f;  // 0.0 - 1.0
  float delaySamplesF = lfoVal * flangerParams.depth * 176.0f + 1.0f;  // 1 to 177
  
  // Interpolated read from flanger buffer (linear interpolation)
  uint32_t delayInt = (uint32_t)delaySamplesF;
  float frac = delaySamplesF - (float)delayInt;
  
  uint32_t readPos1 = (flangerParams.writePos + FLANGER_BUFFER_SIZE - delayInt) 
                      % FLANGER_BUFFER_SIZE;
  uint32_t readPos2 = (readPos1 + FLANGER_BUFFER_SIZE - 1) % FLANGER_BUFFER_SIZE;
  
  float delayed = flangerBuffer[readPos1] * (1.0f - frac) + flangerBuffer[readPos2] * frac;
  
  // Add feedback to the written sample
  flangerBuffer[flangerParams.writePos] += delayed * flangerParams.feedback;
  
  // Advance write position
  flangerParams.writePos = (flangerParams.writePos + 1) % FLANGER_BUFFER_SIZE;
  
  // Mix dry and wet
  return input * (1.0f - flangerParams.mix) + delayed * flangerParams.mix;
}

// ============= NEW: Compressor/Limiter Processing =============

inline float AudioEngine::processCompressor(float input) {
  // Envelope follower (peak detection)
  float absInput = fabsf(input);
  if (absInput > compressorParams.envelope) {
    compressorParams.envelope = compressorParams.attackCoeff * compressorParams.envelope 
                               + (1.0f - compressorParams.attackCoeff) * absInput;
  } else {
    compressorParams.envelope = compressorParams.releaseCoeff * compressorParams.envelope 
                               + (1.0f - compressorParams.releaseCoeff) * absInput;
  }
  
  // Calculate gain reduction
  float gain = 1.0f;
  if (compressorParams.envelope > compressorParams.threshold) {
    // Ratio compression
    float excess = compressorParams.envelope / compressorParams.threshold;
    float targetGain = compressorParams.threshold * powf(excess, 1.0f / compressorParams.ratio - 1.0f);
    gain = targetGain;
  }
  
  return input * gain * compressorParams.makeupGain;
}

// ============= NEW: Master Effects Setters =============

// --- Delay/Echo ---
void AudioEngine::setDelayActive(bool active) {
  delayParams.active = active;
  if (active && delayBuffer) {
    memset(delayBuffer, 0, DELAY_BUFFER_SIZE * sizeof(float));
    delayParams.writePos = 0;
  }
  Serial.printf("[AudioEngine] Delay: %s\n", active ? "ON" : "OFF");
}

void AudioEngine::setDelayTime(float ms) {
  delayParams.time = constrain(ms, 10.0f, 750.0f);
  delayParams.delaySamples = (uint32_t)(delayParams.time * SAMPLE_RATE / 1000.0f);
  if (delayParams.delaySamples >= DELAY_BUFFER_SIZE) {
    delayParams.delaySamples = DELAY_BUFFER_SIZE - 1;
  }
  Serial.printf("[AudioEngine] Delay time: %.0f ms (%d samples)\n", delayParams.time, delayParams.delaySamples);
}

void AudioEngine::setDelayFeedback(float feedback) {
  delayParams.feedback = constrain(feedback, 0.0f, 0.95f);
}

void AudioEngine::setDelayMix(float mix) {
  delayParams.mix = constrain(mix, 0.0f, 1.0f);
}

// --- Phaser ---
void AudioEngine::setPhaserActive(bool active) {
  phaserParams.active = active;
  if (active) {
    phaserParams.lastOutput = 0.0f;
    for (int i = 0; i < PHASER_STAGES; i++) {
      phaserParams.stages[i] = {0, 0, 0, 0};
    }
  }
  Serial.printf("[AudioEngine] Phaser: %s\n", active ? "ON" : "OFF");
}

void AudioEngine::setPhaserRate(float hz) {
  phaserParams.rate = constrain(hz, 0.05f, 5.0f);
  phaserParams.lfo.depth = 1.0f;  // LFO siempre [-1,+1]
  updateLFOPhaseInc(phaserParams.lfo, phaserParams.rate);
}

void AudioEngine::setPhaserDepth(float depth) {
  phaserParams.depth = constrain(depth, 0.0f, 1.0f);
  phaserParams.lfo.depth = 1.0f;  // LFO siempre [-1,+1], depth solo en freq calc
}

void AudioEngine::setPhaserFeedback(float feedback) {
  phaserParams.feedback = constrain(feedback, -0.9f, 0.9f);
}

// --- Flanger ---
void AudioEngine::setFlangerActive(bool active) {
  flangerParams.active = active;
  if (active) {
    memset(flangerBuffer, 0, sizeof(flangerBuffer));
    flangerParams.writePos = 0;
  }
  Serial.printf("[AudioEngine] Flanger: %s\n", active ? "ON" : "OFF");
}

void AudioEngine::setFlangerRate(float hz) {
  flangerParams.rate = constrain(hz, 0.05f, 5.0f);
  updateLFOPhaseInc(flangerParams.lfo, flangerParams.rate);
}

void AudioEngine::setFlangerDepth(float depth) {
  flangerParams.depth = constrain(depth, 0.0f, 1.0f);
  flangerParams.lfo.depth = 1.0f;  // LFO siempre [-1,+1], depth solo en delay calc
}

void AudioEngine::setFlangerFeedback(float feedback) {
  flangerParams.feedback = constrain(feedback, -0.9f, 0.9f);
}

void AudioEngine::setFlangerMix(float mix) {
  flangerParams.mix = constrain(mix, 0.0f, 1.0f);
}

// --- Compressor ---
void AudioEngine::setCompressorActive(bool active) {
  compressorParams.active = active;
  if (active) {
    compressorParams.envelope = 0.0f;
  }
  Serial.printf("[AudioEngine] Compressor: %s\n", active ? "ON" : "OFF");
}

void AudioEngine::setCompressorThreshold(float threshold) {
  // Input in dB (-60 to 0), convert to linear
  float db = constrain(threshold, -60.0f, 0.0f);
  compressorParams.threshold = powf(10.0f, db / 20.0f);
}

void AudioEngine::setCompressorRatio(float ratio) {
  compressorParams.ratio = constrain(ratio, 1.0f, 20.0f);
}

void AudioEngine::setCompressorAttack(float ms) {
  float t = constrain(ms, 0.1f, 100.0f);
  compressorParams.attackCoeff = expf(-1.0f / (SAMPLE_RATE * t / 1000.0f));
}

void AudioEngine::setCompressorRelease(float ms) {
  float t = constrain(ms, 10.0f, 1000.0f);
  compressorParams.releaseCoeff = expf(-1.0f / (SAMPLE_RATE * t / 1000.0f));
}

void AudioEngine::setCompressorMakeupGain(float db) {
  float d = constrain(db, 0.0f, 24.0f);
  compressorParams.makeupGain = powf(10.0f, d / 20.0f);
}

// ============= PER-TRACK LIVE FX (SLAVE Controller) =============

void AudioEngine::setTrackEcho(int track, bool active, float time, float feedback, float mix) {
  if (track < 0 || track >= MAX_AUDIO_TRACKS) return;
  
  trackEcho[track].active = active;
  if (active) {
    // Allocate echo buffer in PSRAM if needed
    if (!trackEchoBuffer[track]) {
      trackEchoBuffer[track] = (float*)ps_calloc(TRACK_ECHO_SIZE, sizeof(float));
      if (!trackEchoBuffer[track]) {
        Serial.printf("[AudioEngine] ERROR: Failed to alloc echo buffer for track %d\n", track);
        trackEcho[track].active = false;
        return;
      }
      Serial.printf("[AudioEngine] Echo buffer allocated track %d (%d bytes PSRAM)\n",
                    track, TRACK_ECHO_SIZE * (int)sizeof(float));
    }
    trackEcho[track].time = constrain(time, 10.0f, 200.0f);
    trackEcho[track].feedback = constrain(feedback / 100.0f, 0.0f, 0.9f);
    trackEcho[track].mix = constrain(mix / 100.0f, 0.0f, 1.0f);
    trackEcho[track].delaySamples = (uint32_t)(trackEcho[track].time * SAMPLE_RATE / 1000.0f);
    if (trackEcho[track].delaySamples >= TRACK_ECHO_SIZE) {
      trackEcho[track].delaySamples = TRACK_ECHO_SIZE - 1;
    }
  } else {
    // Free echo buffer when deactivated
    if (trackEchoBuffer[track]) {
      free(trackEchoBuffer[track]);
      trackEchoBuffer[track] = nullptr;
    }
    trackEcho[track].writePos = 0;
  }
  Serial.printf("[AudioEngine] Track %d echo: %s (time:%.0fms fb:%.0f%% mix:%.0f%%)\n",
                track, active ? "ON" : "OFF", time, feedback, mix);
}

void AudioEngine::setTrackFlanger(int track, bool active, float rate, float depth, float feedback) {
  if (track < 0 || track >= MAX_AUDIO_TRACKS) return;
  
  trackFlanger[track].active = active;
  if (active) {
    trackFlanger[track].rate = constrain(rate / 100.0f, 0.05f, 5.0f);
    trackFlanger[track].depth = constrain(depth / 100.0f, 0.0f, 1.0f);
    trackFlanger[track].feedback = constrain(feedback / 100.0f, -0.9f, 0.9f);
    trackFlanger[track].lfo.depth = 1.0f;  // LFO siempre [-1,+1], depth se aplica solo en delay calc
    updateLFOPhaseInc(trackFlanger[track].lfo, trackFlanger[track].rate);
    // Clear flanger buffer for this track
    if (trackFlangerBuffers) {
      memset(&trackFlangerBuffers[track * TRACK_FLANGER_BUF], 0, TRACK_FLANGER_BUF * sizeof(float));
    }
    trackFlanger[track].writePos = 0;
  }
  Serial.printf("[AudioEngine] Track %d flanger: %s (rate:%.2fHz depth:%.0f%% fb:%.0f%%)\n",
                track, active ? "ON" : "OFF", rate / 100.0f, depth, feedback);
}

void AudioEngine::setTrackCompressor(int track, bool active, float threshold, float ratio) {
  if (track < 0 || track >= MAX_AUDIO_TRACKS) return;
  
  trackComp[track].active = active;
  if (active) {
    float db = constrain(threshold, -60.0f, 0.0f);
    trackComp[track].threshold = powf(10.0f, db / 20.0f);
    trackComp[track].ratio = constrain(ratio, 1.0f, 20.0f);
    trackComp[track].attackCoeff = expf(-1.0f / (SAMPLE_RATE * 0.002f));   // 2ms fast attack for drums
    trackComp[track].releaseCoeff = expf(-1.0f / (SAMPLE_RATE * 0.060f));  // 60ms punchy release
    trackComp[track].envelope = 0.0f;
  }
  Serial.printf("[AudioEngine] Track %d compressor: %s (thresh:%.1fdB ratio:%.1f)\n",
                track, active ? "ON" : "OFF", threshold, ratio);
}

void AudioEngine::clearTrackLiveFX(int track) {
  if (track < 0 || track >= MAX_AUDIO_TRACKS) return;
  trackEcho[track].active = false;
  if (trackEchoBuffer[track]) { free(trackEchoBuffer[track]); trackEchoBuffer[track] = nullptr; }
  trackEcho[track].writePos = 0;
  trackFlanger[track].active = false;
  trackComp[track].active = false;
  trackComp[track].envelope = 0.0f;
  Serial.printf("[AudioEngine] Track %d live FX cleared\n", track);
}

bool AudioEngine::getTrackEchoActive(int track) const {
  if (track < 0 || track >= MAX_AUDIO_TRACKS) return false;
  return trackEcho[track].active;
}

bool AudioEngine::getTrackFlangerActive(int track) const {
  if (track < 0 || track >= MAX_AUDIO_TRACKS) return false;
  return trackFlanger[track].active;
}

bool AudioEngine::getTrackCompressorActive(int track) const {
  if (track < 0 || track >= MAX_AUDIO_TRACKS) return false;
  return trackComp[track].active;
}

// --- Per-track Echo Processing ---
inline float AudioEngine::processTrackEcho(int track, float input) {
  TrackEchoState& e = trackEcho[track];
  float* buf = trackEchoBuffer[track];
  if (!buf) return input;
  
  uint32_t readPos = (e.writePos + TRACK_ECHO_SIZE - e.delaySamples) % TRACK_ECHO_SIZE;
  float delayed = buf[readPos];
  
  float writeVal = input + delayed * e.feedback;
  writeVal = writeVal / (1.0f + fabsf(writeVal)); // Prevent feedback runaway
  buf[e.writePos] = writeVal;
  e.writePos = (e.writePos + 1) % TRACK_ECHO_SIZE;
  
  return input * (1.0f - e.mix) + delayed * e.mix;
}

// --- Per-track Flanger Processing ---
inline float AudioEngine::processTrackFlanger(int track, float input) {
  TrackFlangerState& f = trackFlanger[track];
  float* buf = &trackFlangerBuffers[track * TRACK_FLANGER_BUF];
  
  buf[f.writePos] = input;
  
  // LFO devuelve [-1,+1] ya que lfo.depth=1.0; depth controla solo rango de delay
  float lfoVal = (lfoTick(f.lfo) + 1.0f) * 0.5f;  // [0, 1]
  float delaySamplesF = lfoVal * f.depth * 400.0f + 1.0f;  // hasta ~9ms max (más audible)
  if (delaySamplesF >= TRACK_FLANGER_BUF - 1) delaySamplesF = TRACK_FLANGER_BUF - 2;
  
  uint32_t delayInt = (uint32_t)delaySamplesF;
  float frac = delaySamplesF - (float)delayInt;
  
  uint32_t readPos1 = (f.writePos + TRACK_FLANGER_BUF - delayInt) % TRACK_FLANGER_BUF;
  uint32_t readPos2 = (readPos1 + TRACK_FLANGER_BUF - 1) % TRACK_FLANGER_BUF;
  
  float delayed = buf[readPos1] * (1.0f - frac) + buf[readPos2] * frac;
  buf[f.writePos] += delayed * f.feedback;
  
  f.writePos = (f.writePos + 1) % TRACK_FLANGER_BUF;
  
  // Wet/dry mix: depth controls mix intensity (more depth = more wet)
  float wetMix = 0.5f + f.depth * 0.4f;  // 0.5 to 0.9
  return input * (1.0f - wetMix) + (input + delayed) * wetMix;
}

// --- Per-track Compressor Processing ---
inline float AudioEngine::processTrackCompressor(int track, float input) {
  TrackCompressorState& c = trackComp[track];
  
  float absInput = fabsf(input);
  if (absInput > c.envelope) {
    c.envelope = c.attackCoeff * c.envelope + (1.0f - c.attackCoeff) * absInput;
  } else {
    c.envelope = c.releaseCoeff * c.envelope + (1.0f - c.releaseCoeff) * absInput;
  }
  
  float gain = 1.0f;
  if (c.envelope > c.threshold) {
    float excess = c.envelope / c.threshold;
    float compGain = powf(excess, 1.0f / c.ratio - 1.0f);
    gain = compGain;
    // Makeup gain: compensate for compression to maintain perceived volume
    // Higher ratio = more makeup needed
    float makeup = 1.0f + (c.ratio - 1.0f) * 0.15f;
    gain *= makeup;
  }
  
  return input * gain;
}

// ============= PER-TRACK FILTER MANAGEMENT =============

bool AudioEngine::setTrackFilter(int track, FilterType type, float cutoff, float resonance, float gain) {
  if (track < 0 || track >= MAX_AUDIO_TRACKS) return false;
  
  // Check if enabling a new filter would exceed the limit of 8
  if (type != FILTER_NONE && !trackFilterActive[track]) {
    if (getActiveTrackFiltersCount() >= 8) {
      Serial.println("[AudioEngine] ERROR: Max 8 track filters active");
      return false;
    }
  }
  
  trackFilters[track].filterType = type;
  trackFilters[track].cutoff = constrain(cutoff, 100.0f, 16000.0f);
  trackFilters[track].resonance = constrain(resonance, 0.5f, 20.0f);
  trackFilters[track].gain = constrain(gain, -12.0f, 12.0f);
  trackFilterActive[track] = (type != FILTER_NONE);
  
  // Calculate coefficients for this filter
  if (type != FILTER_NONE) {
    calculateBiquadCoeffs(trackFilters[track]);
    Serial.printf("[AudioEngine] Track %d filter ACTIVE: %s (cutoff: %.1f Hz, Q: %.2f, gain: %.1f dB)\n",
                  track, getFilterName(type), cutoff, resonance, gain);
  } else {
    Serial.printf("[AudioEngine] Track %d filter CLEARED\n", track);
  }
  return true;
}

void AudioEngine::clearTrackFilter(int track) {
  if (track < 0 || track >= MAX_AUDIO_TRACKS) return;
  trackFilters[track].filterType = FILTER_NONE;
  trackFilterActive[track] = false;
  // Reset biquad state + coefficients to prevent residual artifacts
  trackFilters[track].state.x1 = trackFilters[track].state.x2 = 0.0f;
  trackFilters[track].state.y1 = trackFilters[track].state.y2 = 0.0f;
  trackFilters[track].coeffs = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  Serial.printf("[AudioEngine] Track %d filter cleared\n", track);
}

FilterType AudioEngine::getTrackFilter(int track) {
  if (track < 0 || track >= MAX_AUDIO_TRACKS) return FILTER_NONE;
  return trackFilters[track].filterType;
}

int AudioEngine::getActiveTrackFiltersCount() {
  int count = 0;
  for (int i = 0; i < MAX_AUDIO_TRACKS; i++) {
    if (trackFilterActive[i]) count++;
  }
  return count;
}

// ============= PER-PAD FILTER MANAGEMENT =============

bool AudioEngine::setPadFilter(int pad, FilterType type, float cutoff, float resonance, float gain) {
  if (pad < 0 || pad >= MAX_PADS) return false;
  
  // Check if enabling a new filter would exceed the limit of 8
  if (type != FILTER_NONE && !padFilterActive[pad]) {
    if (getActivePadFiltersCount() >= 8) {
      Serial.println("[AudioEngine] ERROR: Max 8 pad filters active");
      return false;
    }
  }
  
  padFilters[pad].filterType = type;
  padFilters[pad].cutoff = constrain(cutoff, 100.0f, 16000.0f);
  padFilters[pad].resonance = constrain(resonance, 0.5f, 20.0f);
  padFilters[pad].gain = constrain(gain, -12.0f, 12.0f);
  padFilterActive[pad] = (type != FILTER_NONE);
  
  // Special effects: initialize scratch/turntablism state
  if (type == FILTER_SCRATCH) {
    scratchState[pad].lfoPhase = 0.0f;
    scratchState[pad].lfoRate = 5.0f;
    scratchState[pad].depth = 0.85f;
    scratchState[pad].lpState1 = 0.0f;
    scratchState[pad].lpState2 = 0.0f;
    Serial.printf("[AudioEngine] Pad %d: SCRATCH effect initialized (rate: %.1f Hz, depth: %.2f)\n", pad, 5.0f, 0.85f);
  } else if (type == FILTER_TURNTABLISM) {
    turntablismState[pad].mode = 0;
    turntablismState[pad].modeTimer = 33075;
    turntablismState[pad].gatePhase = 0.0f;
    turntablismState[pad].lpState1 = 0.0f;
    turntablismState[pad].lpState2 = 0.0f;
    Serial.printf("[AudioEngine] Pad %d: TURNTABLISM effect initialized\n", pad);
  } else if (type != FILTER_NONE) {
    // Calculate biquad coefficients for standard filters only
    calculateBiquadCoeffs(padFilters[pad]);
  }
  
  if (type != FILTER_SCRATCH && type != FILTER_TURNTABLISM) {
    Serial.printf("[AudioEngine] Pad %d filter: %s (cutoff: %.1f Hz, Q: %.2f, gain: %.1f dB)\n",
                  pad, getFilterName(type), cutoff, resonance, gain);
  }
  return true;
}

void AudioEngine::clearPadFilter(int pad) {
  if (pad < 0 || pad >= MAX_PADS) return;
  padFilters[pad].filterType = FILTER_NONE;
  padFilterActive[pad] = false;
  // Reset biquad state + coefficients to prevent residual artifacts
  padFilters[pad].state.x1 = padFilters[pad].state.x2 = 0.0f;
  padFilters[pad].state.y1 = padFilters[pad].state.y2 = 0.0f;
  padFilters[pad].coeffs = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  Serial.printf("[AudioEngine] Pad %d filter cleared\n", pad);
}

FilterType AudioEngine::getPadFilter(int pad) {
  if (pad < 0 || pad >= MAX_PADS) return FILTER_NONE;
  return padFilters[pad].filterType;
}

int AudioEngine::getActivePadFiltersCount() {
  int count = 0;
  for (int i = 0; i < MAX_PADS; i++) {
    if (padFilterActive[i]) count++;
  }
  return count;
}

// ============= PER-PAD/TRACK FX (Distortion + BitCrush) =============

void AudioEngine::setPadDistortion(int pad, float amount, DistortionMode mode) {
  if (pad < 0 || pad >= MAX_PADS) return;
  padFilters[pad].distortion = constrain(amount, 0.0f, 100.0f);
  padDistortionMode[pad] = mode;
  // Activate pad filter channel if any FX is set
  if (amount > 0.1f || padFilters[pad].filterType != FILTER_NONE || padFilters[pad].bitDepth < 16) {
    padFilterActive[pad] = true;
  }
  Serial.printf("[AudioEngine] Pad %d distortion: %.1f%% mode=%d\n", pad, amount, mode);
}

void AudioEngine::setPadBitCrush(int pad, uint8_t bits) {
  if (pad < 0 || pad >= MAX_PADS) return;
  padFilters[pad].bitDepth = constrain(bits, 4, 16);
  if (bits < 16 || padFilters[pad].filterType != FILTER_NONE || padFilters[pad].distortion > 0.1f) {
    padFilterActive[pad] = true;
  }
  Serial.printf("[AudioEngine] Pad %d bitcrush: %d bits\n", pad, bits);
}

void AudioEngine::clearPadFX(int pad) {
  if (pad < 0 || pad >= MAX_PADS) return;
  padFilters[pad].distortion = 0.0f;
  padFilters[pad].bitDepth = 16;
  padDistortionMode[pad] = DIST_SOFT;
  // Only deactivate if filter is also off
  if (padFilters[pad].filterType == FILTER_NONE) {
    padFilterActive[pad] = false;
  }
  Serial.printf("[AudioEngine] Pad %d FX cleared\n", pad);
}

void AudioEngine::setTrackDistortion(int track, float amount, DistortionMode mode) {
  if (track < 0 || track >= MAX_AUDIO_TRACKS) return;
  trackFilters[track].distortion = constrain(amount, 0.0f, 100.0f);
  trackDistortionMode[track] = mode;
  if (amount > 0.1f || trackFilters[track].filterType != FILTER_NONE || trackFilters[track].bitDepth < 16) {
    trackFilterActive[track] = true;
  }
  Serial.printf("[AudioEngine] Track %d distortion: %.1f%% mode=%d\n", track, amount, mode);
}

void AudioEngine::setTrackBitCrush(int track, uint8_t bits) {
  if (track < 0 || track >= MAX_AUDIO_TRACKS) return;
  trackFilters[track].bitDepth = constrain(bits, 4, 16);
  if (bits < 16 || trackFilters[track].filterType != FILTER_NONE || trackFilters[track].distortion > 0.1f) {
    trackFilterActive[track] = true;
  }
  Serial.printf("[AudioEngine] Track %d bitcrush: %d bits\n", track, bits);
}

void AudioEngine::clearTrackFX(int track) {
  if (track < 0 || track >= MAX_AUDIO_TRACKS) return;
  trackFilters[track].distortion = 0.0f;
  trackFilters[track].bitDepth = 16;
  trackDistortionMode[track] = DIST_SOFT;
  if (trackFilters[track].filterType == FILTER_NONE) {
    trackFilterActive[track] = false;
  }
  Serial.printf("[AudioEngine] Track %d FX cleared\n", track);
}

// ============= FILTER PRESETS =============

const FilterPreset* AudioEngine::getFilterPreset(FilterType type) {
  static const FilterPreset presets[] = {
    {FILTER_NONE, 0.0f, 1.0f, 0.0f, "None"},
    {FILTER_LOWPASS, 800.0f, 3.0f, 0.0f, "Low Pass"},
    {FILTER_HIGHPASS, 800.0f, 3.0f, 0.0f, "High Pass"},
    {FILTER_BANDPASS, 1200.0f, 4.0f, 0.0f, "Band Pass"},
    {FILTER_NOTCH, 1000.0f, 5.0f, 0.0f, "Notch"},
    {FILTER_ALLPASS, 1000.0f, 3.0f, 0.0f, "All Pass"},
    {FILTER_PEAKING, 1000.0f, 3.0f, 9.0f, "Peaking EQ"},
    {FILTER_LOWSHELF, 200.0f, 1.0f, 9.0f, "Low Shelf"},
    {FILTER_HIGHSHELF, 5000.0f, 1.0f, 8.0f, "High Shelf"},
    {FILTER_RESONANT, 800.0f, 12.0f, 0.0f, "Resonant"},
    {FILTER_SCRATCH, 0.0f, 0.0f, 0.0f, "Scratch"},
    {FILTER_TURNTABLISM, 0.0f, 0.0f, 0.0f, "Turntablism"}
  };
  
  if (type >= FILTER_NONE && type <= FILTER_TURNTABLISM) {
    return &presets[type];
  }
  return &presets[0];
}

const char* AudioEngine::getFilterName(FilterType type) {
  const FilterPreset* preset = getFilterPreset(type);
  return preset ? preset->name : "Unknown";
}

// ============= REVERSE / PITCH SHIFT / STUTTER =============

void AudioEngine::setReverseSample(int padIndex, bool reverse) {
  if (padIndex < 0 || padIndex >= MAX_PADS) return;
  if (sampleBuffers[padIndex] == nullptr || sampleLengths[padIndex] == 0) return;
  
  // Only reverse if state actually changes
  if (sampleReversed[padIndex] == reverse) return;
  sampleReversed[padIndex] = reverse;
  
  // Reverse the sample buffer in-place
  int16_t* buf = sampleBuffers[padIndex];
  uint32_t len = sampleLengths[padIndex];
  for (uint32_t i = 0; i < len / 2; i++) {
    int16_t temp = buf[i];
    buf[i] = buf[len - 1 - i];
    buf[len - 1 - i] = temp;
  }
  
  Serial.printf("[AudioEngine] Sample %d %s\n", padIndex, reverse ? "REVERSED" : "NORMAL");
}

void AudioEngine::setTrackPitchShift(int padIndex, float pitch) {
  if (padIndex < 0 || padIndex >= MAX_PADS) return;
  trackPitchShift[padIndex] = constrain(pitch, 0.25f, 4.0f);
  Serial.printf("[AudioEngine] Track %d pitch: %.2f\n", padIndex, trackPitchShift[padIndex]);
}

void AudioEngine::setStutter(int padIndex, bool active, int intervalMs) {
  if (padIndex < 0 || padIndex >= MAX_PADS) return;
  stutterActive[padIndex] = active;
  stutterInterval[padIndex] = constrain(intervalMs, 10, 500);
  
  if (active) {
    // Set up loop on the beginning of the sample
    uint32_t stutterSamples = (SAMPLE_RATE * intervalMs) / 1000;
    if (sampleLengths[padIndex] > 0 && stutterSamples < sampleLengths[padIndex]) {
      // Find active voice for this pad and set loop
      for (int v = 0; v < MAX_VOICES; v++) {
        if (voices[v].active && voices[v].padIndex == padIndex) {
          voices[v].loop = true;
          voices[v].loopStart = 0;
          voices[v].loopEnd = stutterSamples;
          break;
        }
      }
    }
  } else {
    // Remove loop from voices of this pad
    for (int v = 0; v < MAX_VOICES; v++) {
      if (voices[v].active && voices[v].padIndex == padIndex) {
        voices[v].loop = false;
        break;
      }
    }
  }
  
  Serial.printf("[AudioEngine] Stutter %s pad %d interval %dms\n", active ? "ON" : "OFF", padIndex, intervalMs);
}

// ============= SCRATCH & TURNTABLISM CONFIGURABLE PARAMS =============

void AudioEngine::setScratchParams(int padIndex, bool active, float rate, float depth, float filterCutoff, float crackle) {
  if (padIndex < 0 || padIndex >= MAX_PADS) return;
  
  if (active) {
    // Activate scratch as pad filter
    padFilters[padIndex].filterType = FILTER_SCRATCH;
    padFilterActive[padIndex] = true;
    
    // Configure scratch state with provided parameters
    ScratchState& ss = scratchState[padIndex];
    ss.lfoPhase = 0.0f;
    ss.lfoRate = constrain(rate, 0.5f, 25.0f);
    ss.depth = constrain(depth, 0.1f, 1.0f);
    ss.filterCutoff = constrain(filterCutoff, 200.0f, 12000.0f);
    ss.crackleAmount = constrain(crackle, 0.0f, 1.0f);
    ss.lpState1 = 0.0f;
    ss.lpState2 = 0.0f;
    
    Serial.printf("[AudioEngine] Pad %d SCRATCH ON (rate:%.1fHz depth:%.0f%% filter:%.0fHz crackle:%.0f%%)\n",
                  padIndex, ss.lfoRate, ss.depth * 100.0f, ss.filterCutoff, ss.crackleAmount * 100.0f);
  } else {
    // Only clear if current filter is scratch
    if (padFilters[padIndex].filterType == FILTER_SCRATCH) {
      padFilters[padIndex].filterType = FILTER_NONE;
      padFilterActive[padIndex] = false;
    }
    Serial.printf("[AudioEngine] Pad %d SCRATCH OFF\n", padIndex);
  }
}

void AudioEngine::setTurntablismParams(int padIndex, bool active, bool autoMode, int mode, int brakeMs, int backspinMs, float transformRate, float vinylNoise) {
  if (padIndex < 0 || padIndex >= MAX_PADS) return;
  
  if (active) {
    // Activate turntablism as pad filter
    padFilters[padIndex].filterType = FILTER_TURNTABLISM;
    padFilterActive[padIndex] = true;
    
    // Configure turntablism state
    TurntablismState& ts = turntablismState[padIndex];
    ts.autoMode = autoMode;
    ts.brakeLen = (uint32_t)((SAMPLE_RATE * constrain(brakeMs, 100, 2000)) / 1000);
    ts.backspinLen = (uint32_t)((SAMPLE_RATE * constrain(backspinMs, 100, 2000)) / 1000);
    ts.transformRate = constrain(transformRate, 2.0f, 30.0f);
    ts.vinylNoise = constrain(vinylNoise, 0.0f, 1.0f);
    
    // Manual mode trigger: set specific mode immediately
    if (mode >= 0 && mode <= 3) {
      ts.mode = mode;
      switch (mode) {
        case 0: ts.modeTimer = 33075; break;                           // Normal ~750ms
        case 1: ts.modeTimer = ts.brakeLen; break;                     // Brake
        case 2: ts.modeTimer = ts.backspinLen; break;                  // Backspin
        case 3: ts.modeTimer = (uint32_t)(SAMPLE_RATE * 0.55f); ts.gatePhase = 0; break; // Transform ~550ms
      }
    } else if (ts.modeTimer == 0) {
      // Fresh start
      ts.mode = 0;
      ts.modeTimer = 33075;
    }
    
    ts.lpState1 = 0.0f;
    ts.lpState2 = 0.0f;
    
    Serial.printf("[AudioEngine] Pad %d TURNTABLISM ON (auto:%d brake:%dms backspin:%dms tRate:%.1fHz noise:%.0f%%)\n",
                  padIndex, autoMode, brakeMs, backspinMs, ts.transformRate, ts.vinylNoise * 100.0f);
  } else {
    if (padFilters[padIndex].filterType == FILTER_TURNTABLISM) {
      padFilters[padIndex].filterType = FILTER_NONE;
      padFilterActive[padIndex] = false;
    }
    Serial.printf("[AudioEngine] Pad %d TURNTABLISM OFF\n", padIndex);
  }
}

// ============= EXTENDED BIQUAD COEFFICIENT CALCULATION =============

void AudioEngine::calculateBiquadCoeffs(FXParams& fxParam) {
  if (fxParam.filterType == FILTER_NONE) return;
  
  float omega = 2.0f * PI * fxParam.cutoff / SAMPLE_RATE;
  float sn = sinf(omega);
  float cs = cosf(omega);
  float alpha = sn / (2.0f * fxParam.resonance);
  float A = powf(10.0f, fxParam.gain / 40.0f); // For shelf/peaking filters
  
  switch (fxParam.filterType) {
    case FILTER_LOWPASS:
      fxParam.coeffs.b0 = (1.0f - cs) / 2.0f;
      fxParam.coeffs.b1 = 1.0f - cs;
      fxParam.coeffs.b2 = (1.0f - cs) / 2.0f;
      fxParam.coeffs.a1 = -2.0f * cs;
      fxParam.coeffs.a2 = 1.0f - alpha;
      break;
      
    case FILTER_HIGHPASS:
      fxParam.coeffs.b0 = (1.0f + cs) / 2.0f;
      fxParam.coeffs.b1 = -(1.0f + cs);
      fxParam.coeffs.b2 = (1.0f + cs) / 2.0f;
      fxParam.coeffs.a1 = -2.0f * cs;
      fxParam.coeffs.a2 = 1.0f - alpha;
      break;
      
    case FILTER_BANDPASS:
      fxParam.coeffs.b0 = alpha;
      fxParam.coeffs.b1 = 0.0f;
      fxParam.coeffs.b2 = -alpha;
      fxParam.coeffs.a1 = -2.0f * cs;
      fxParam.coeffs.a2 = 1.0f - alpha;
      break;
      
    case FILTER_NOTCH:
      fxParam.coeffs.b0 = 1.0f;
      fxParam.coeffs.b1 = -2.0f * cs;
      fxParam.coeffs.b2 = 1.0f;
      fxParam.coeffs.a1 = -2.0f * cs;
      fxParam.coeffs.a2 = 1.0f - alpha;
      break;
      
    case FILTER_ALLPASS:
      fxParam.coeffs.b0 = 1.0f - alpha;
      fxParam.coeffs.b1 = -2.0f * cs;
      fxParam.coeffs.b2 = 1.0f + alpha;
      fxParam.coeffs.a1 = -2.0f * cs;
      fxParam.coeffs.a2 = 1.0f - alpha;
      break;
      
    case FILTER_PEAKING: {
      float beta = sqrtf(A) / fxParam.resonance;
      fxParam.coeffs.b0 = 1.0f + alpha * A;
      fxParam.coeffs.b1 = -2.0f * cs;
      fxParam.coeffs.b2 = 1.0f - alpha * A;
      fxParam.coeffs.a1 = -2.0f * cs;
      fxParam.coeffs.a2 = 1.0f - alpha / A;
      break;
    }
      
    case FILTER_LOWSHELF: {
      float sqrtA = sqrtf(A);
      fxParam.coeffs.b0 = A * ((A + 1.0f) - (A - 1.0f) * cs + 2.0f * sqrtA * alpha);
      fxParam.coeffs.b1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cs);
      fxParam.coeffs.b2 = A * ((A + 1.0f) - (A - 1.0f) * cs - 2.0f * sqrtA * alpha);
      fxParam.coeffs.a1 = -2.0f * ((A - 1.0f) + (A + 1.0f) * cs);
      fxParam.coeffs.a2 = (A + 1.0f) + (A - 1.0f) * cs - 2.0f * sqrtA * alpha;
      break;
    }
      
    case FILTER_HIGHSHELF: {
      float sqrtA = sqrtf(A);
      fxParam.coeffs.b0 = A * ((A + 1.0f) + (A - 1.0f) * cs + 2.0f * sqrtA * alpha);
      fxParam.coeffs.b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cs);
      fxParam.coeffs.b2 = A * ((A + 1.0f) + (A - 1.0f) * cs - 2.0f * sqrtA * alpha);
      fxParam.coeffs.a1 = 2.0f * ((A - 1.0f) - (A + 1.0f) * cs);
      fxParam.coeffs.a2 = (A + 1.0f) - (A - 1.0f) * cs - 2.0f * sqrtA * alpha;
      break;
    }
      
    case FILTER_RESONANT:
      // High resonance lowpass
      fxParam.coeffs.b0 = (1.0f - cs) / 2.0f;
      fxParam.coeffs.b1 = 1.0f - cs;
      fxParam.coeffs.b2 = (1.0f - cs) / 2.0f;
      fxParam.coeffs.a1 = -2.0f * cs;
      fxParam.coeffs.a2 = 1.0f - alpha;
      break;
      
    default:
      break;
  }
  
  // Normalize by a0
  float a0 = (fxParam.filterType == FILTER_LOWSHELF || fxParam.filterType == FILTER_HIGHSHELF) 
             ? ((fxParam.filterType == FILTER_LOWSHELF) 
                ? ((powf(10.0f, fxParam.gain / 40.0f) + 1.0f) + (powf(10.0f, fxParam.gain / 40.0f) - 1.0f) * cs + 2.0f * sqrtf(powf(10.0f, fxParam.gain / 40.0f)) * alpha)
                : ((powf(10.0f, fxParam.gain / 40.0f) + 1.0f) - (powf(10.0f, fxParam.gain / 40.0f) - 1.0f) * cs + 2.0f * sqrtf(powf(10.0f, fxParam.gain / 40.0f)) * alpha))
             : (1.0f + alpha);
             
  fxParam.coeffs.b0 /= a0;
  fxParam.coeffs.b1 /= a0;
  fxParam.coeffs.b2 /= a0;
  fxParam.coeffs.a1 /= a0;
  fxParam.coeffs.a2 /= a0;
}

// ============= EXTENDED FILTER PROCESSING =============

inline int16_t AudioEngine::applyFilter(int16_t input, FXParams& fxParam) {
  if (fxParam.filterType == FILTER_NONE) return input;
  
  float x = (float)input;
  float y = fxParam.coeffs.b0 * x + fxParam.state.x1;
  
  fxParam.state.x1 = fxParam.coeffs.b1 * x - fxParam.coeffs.a1 * y + fxParam.state.x2;
  fxParam.state.x2 = fxParam.coeffs.b2 * x - fxParam.coeffs.a2 * y;
  
  // Clamp to prevent overflow
  if (y > 32767.0f) y = 32767.0f;
  else if (y < -32768.0f) y = -32768.0f;
  
  return (int16_t)y;
}
