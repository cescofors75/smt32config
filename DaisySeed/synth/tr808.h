/* ═══════════════════════════════════════════════════════════════════
 *  TR808 — Roland TR-808 Drum Synthesis Library
 * ─────────────────────────────────────────────────────────────────
 *  Síntesis analógica matemática: sin(), exp(), tanh(), noise
 *  Cada instrumento = clase independiente con Trigger() + Process()
 *  48 kHz · float32 · header-only
 *
 *  Instrumentos:
 *    Kick, Snare, Clap, HiHatClosed, HiHatOpen,
 *    LowTom, MidTom, HiTom, LowConga, MidConga, HiConga,
 *    Claves, Maracas, RimShot, Cowbell, Cymbal
 * ═══════════════════════════════════════════════════════════════════ */
#pragma once

#include <math.h>
#include <stdint.h>

#ifndef TWOPI_F
#define TWOPI_F 6.283185307f
#endif

namespace TR808 {

/* ── PRNG rápido para ruido ──────────────────────────────────── */
static inline float Noise(uint32_t& state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return ((float)(int32_t)state) / 2147483648.0f;
}

/* ── Clamp helper ────────────────────────────────────────────── */
static inline float Clamp(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* ═══════════════════════════════════════════════════════════════
 *  KICK 808
 *  Sine + pitch envelope exponencial + saturación tanh
 *  El sonido más icónico: "boom" profundo con caída de pitch
 * ═══════════════════════════════════════════════════════════════ */
class Kick {
public:
    /* Parámetros (knobs) */
    float decay      = 0.45f;   /* 0.1 - 0.8 s    duración del boom     */
    float pitch      = 55.0f;   /* 40  - 80 Hz     frecuencia base       */
    float pitchDecay = 0.08f;   /* 0.02 - 0.5 s    caída del pitch       */
    float attack     = 0.005f;  /* 0.001 - 0.02 s  click inicial         */
    float saturation = 0.3f;    /* 0.0 - 1.0       suciedad analógica    */
    float volume     = 1.0f;    /* 0.0 - 1.0       nivel de salida       */

    void Init(float sampleRate) {
        sr_ = sampleRate;
        dt_ = 1.0f / sr_;
        active_ = false;
        time_ = 0.0f;
        phase_ = 0.0f;
    }

    void Trigger(float velocity = 1.0f) {
        active_ = true;
        time_ = 0.0f;
        phase_ = 0.0f;
        vel_ = Clamp(velocity, 0.0f, 1.0f);
    }

    float Process() {
        if (!active_) return 0.0f;

        /* 1. Pitch actual: cae exponencialmente */
        float currentPitch = pitch + pitch * 8.0f * expf(-time_ / pitchDecay);

        /* 2. Avanzar fase del oscilador sine */
        phase_ += currentPitch * dt_;
        if (phase_ >= 1.0f) phase_ -= 1.0f;
        float sine = sinf(TWOPI_F * phase_);

        /* 3. Click de ataque (transitorio) */
        float clickEnv = expf(-time_ / attack);
        float click = clickEnv * sinf(TWOPI_F * 1200.0f * time_) * 0.3f;

        /* 4. Envelope de amplitud */
        float amp = expf(-time_ / decay);

        /* 5. Mezclar sine + click */
        float output = sine + click;

        /* 6. Saturación suave (carácter analógico) */
        output = tanhf(output * (1.0f + saturation * 3.0f));

        /* 7. Avanzar tiempo */
        time_ += dt_;

        /* 8. Desactivar cuando es inaudible */
        if (amp < 0.001f) active_ = false;

        return output * amp * volume * vel_;
    }

    bool IsActive() const { return active_; }

    void SetDecay(float d)  { decay = Clamp(d, 0.05f, 2.0f); }
    void SetPitch(float p)  { pitch = Clamp(p, 30.0f, 120.0f); }

private:
    float sr_ = 48000.0f;
    float dt_ = 1.0f / 48000.0f;
    bool  active_ = false;
    float time_ = 0.0f;
    float phase_ = 0.0f;
    float vel_ = 1.0f;
};

/* ═══════════════════════════════════════════════════════════════
 *  SNARE 808
 *  Dos tonos (180 Hz + 330 Hz) + ruido filtrado bandpass
 *  El snare original mezcla componente tonal con noise
 * ═══════════════════════════════════════════════════════════════ */
class Snare {
public:
    float decay     = 0.2f;     /* 0.1 - 0.5 s     decay total           */
    float tone      = 0.5f;     /* 0.0 - 1.0        mezcla tono/ruido    */
    float snappy    = 0.5f;     /* 0.0 - 1.0        cantidad de noise    */
    float pitch     = 180.0f;   /* 100 - 300 Hz     tono fundamental     */
    float volume    = 1.0f;

    void Init(float sampleRate) {
        sr_ = sampleRate;
        dt_ = 1.0f / sr_;
        active_ = false;
        noiseState_ = 0xDEADBEEF;
    }

    void Trigger(float velocity = 1.0f) {
        active_ = true;
        time_ = 0.0f;
        phase1_ = 0.0f;
        phase2_ = 0.0f;
        vel_ = Clamp(velocity, 0.0f, 1.0f);
        /* Reset filtro */
        nfZ1_ = nfZ2_ = 0.0f;
    }

    float Process() {
        if (!active_) return 0.0f;

        /* Tone 1: fundamental */
        phase1_ += pitch * dt_;
        if (phase1_ >= 1.0f) phase1_ -= 1.0f;
        float t1 = sinf(TWOPI_F * phase1_);

        /* Tone 2: armónico ~1.8x */
        float pitch2 = pitch * 1.833f;
        phase2_ += pitch2 * dt_;
        if (phase2_ >= 1.0f) phase2_ -= 1.0f;
        float t2 = sinf(TWOPI_F * phase2_);

        /* Mezcla tonal con pitch decay */
        float toneEnv = expf(-time_ / (decay * 0.6f));
        float toneOut = (t1 * 0.6f + t2 * 0.4f) * toneEnv * tone;

        /* Ruido bandpass (1kHz - 8kHz) */
        float n = Noise(noiseState_);
        /* Filtro bandpass simple 2-pole */
        float fc = 5000.0f;
        float w = TWOPI_F * fc / sr_;
        float q = 1.5f;
        float alpha = sinf(w) / (2.0f * q);
        float a0 = 1.0f + alpha;
        float filtered = (alpha * n + (-alpha) * nfZ2_
                         + (-2.0f * cosf(w)) * (-nfZ1_)
                         + (1.0f - alpha) * (-nfZ2_)) / a0;
        /* Simplified: usar state variable */
        nfZ2_ = nfZ1_;
        nfZ1_ = n;
        filtered = n * 0.3f + filtered * 0.7f; /* mezcla para más cuerpo */

        float noiseEnv = expf(-time_ / decay);
        float noiseOut = filtered * noiseEnv * snappy;

        /* Mezclar */
        float output = toneOut + noiseOut;
        output = tanhf(output * 1.5f);

        time_ += dt_;
        if (noiseEnv < 0.001f) active_ = false;

        return output * volume * vel_;
    }

    bool IsActive() const { return active_; }
    void SetDecay(float d) { decay = Clamp(d, 0.05f, 1.0f); }
    void SetTone(float t)  { tone = Clamp(t, 0.0f, 1.0f); }
    void SetSnappy(float s){ snappy = Clamp(s, 0.0f, 1.0f); }

private:
    float sr_ = 48000.0f, dt_ = 1.0f / 48000.0f;
    bool  active_ = false;
    float time_ = 0.0f;
    float phase1_ = 0.0f, phase2_ = 0.0f;
    float vel_ = 1.0f;
    uint32_t noiseState_ = 0xDEADBEEF;
    float nfZ1_ = 0.0f, nfZ2_ = 0.0f;
};

/* ═══════════════════════════════════════════════════════════════
 *  CLAP 808
 *  4 ráfagas de ruido rápidas + filtro bandpass + reverb corto
 *  Simula múltiples manos aplaudiendo con delays micro
 * ═══════════════════════════════════════════════════════════════ */
class Clap {
public:
    float decay   = 0.3f;    /* 0.1 - 0.6 s */
    float tone    = 0.5f;    /* 0.0 - 1.0 brillantez */
    float volume  = 1.0f;

    void Init(float sampleRate) {
        sr_ = sampleRate;
        dt_ = 1.0f / sr_;
        active_ = false;
        noiseState_ = 0xCAFEBABE;
    }

    void Trigger(float velocity = 1.0f) {
        active_ = true;
        time_ = 0.0f;
        vel_ = Clamp(velocity, 0.0f, 1.0f);
        bpZ1_ = bpZ2_ = 0.0f;
    }

    float Process() {
        if (!active_) return 0.0f;

        float n = Noise(noiseState_);

        /* 4 ráfagas separadas ~7ms cada una */
        float burstT = 0.007f;
        float env = 0.0f;
        for (int i = 0; i < 4; i++) {
            float t = time_ - i * burstT;
            if (t >= 0.0f && t < burstT) {
                env += expf(-t / 0.002f) * 0.5f;
            }
        }
        /* Tail: decay largo después de las ráfagas */
        float tailStart = 4.0f * burstT;
        if (time_ >= tailStart) {
            env += expf(-(time_ - tailStart) / decay);
        }

        /* Bandpass filter ~1.2kHz */
        float fc = 1200.0f + tone * 3000.0f;
        float w = TWOPI_F * fc / sr_;
        float sw = sinf(w);
        float cw = cosf(w);
        float q = 2.0f;
        float alpha = sw / (2.0f * q);
        float a0i = 1.0f / (1.0f + alpha);
        float out = (alpha * n - alpha * bpZ2_) * a0i
                  - (-2.0f * cw) * a0i * bpZ1_
                  - (1.0f - alpha) * a0i * bpZ2_;
        bpZ2_ = bpZ1_;
        bpZ1_ = out;

        float output = out * env;
        output = tanhf(output * 2.0f);

        time_ += dt_;
        if (time_ > decay + 0.05f && env < 0.001f) active_ = false;

        return output * volume * vel_;
    }

    bool IsActive() const { return active_; }
    void SetDecay(float d) { decay = Clamp(d, 0.05f, 1.0f); }

private:
    float sr_ = 48000.0f, dt_ = 1.0f / 48000.0f;
    bool  active_ = false;
    float time_ = 0.0f;
    float vel_ = 1.0f;
    uint32_t noiseState_ = 0xCAFEBABE;
    float bpZ1_ = 0.0f, bpZ2_ = 0.0f;
};

/* ═══════════════════════════════════════════════════════════════
 *  HIHAT BASE (compartido entre Closed y Open)
 *  6 ondas cuadradas metálicas con frecuencias no armónicas
 *  Estas frecuencias son las del circuito original Roland
 * ═══════════════════════════════════════════════════════════════ */
class HiHatBase {
protected:
    /* Frecuencias metálicas originales del 808 */
    static constexpr float METAL_FREQS[6] = {
        204.0f, 298.5f, 366.5f, 522.0f, 540.0f, 800.0f
    };

    float sr_ = 48000.0f, dt_ = 1.0f / 48000.0f;
    bool  active_ = false;
    float time_ = 0.0f;
    float vel_ = 1.0f;
    float phase_[6] = {};
    uint32_t noiseState_ = 0xBAADF00D;
    /* Filtro highpass state */
    float hpZ1_ = 0.0f, hpOut_ = 0.0f;

    float MetallicNoise() {
        float sum = 0.0f;
        for (int i = 0; i < 6; i++) {
            phase_[i] += METAL_FREQS[i] * dt_;
            if (phase_[i] >= 1.0f) phase_[i] -= 1.0f;
            /* Square wave */
            sum += (phase_[i] < 0.5f) ? 1.0f : -1.0f;
        }
        /* Mezclar con un poco de ruido real */
        float n = Noise(noiseState_) * 0.15f;
        return (sum / 6.0f + n);
    }

    /* Highpass simple ~6kHz para brillo metálico */
    float Highpass(float in) {
        float rc = 1.0f / (TWOPI_F * 6000.0f);
        float alpha = rc / (rc + dt_);
        hpOut_ = alpha * (hpOut_ + in - hpZ1_);
        hpZ1_ = in;
        return hpOut_;
    }
};

constexpr float HiHatBase::METAL_FREQS[6];

/* ═══════════════════════════════════════════════════════════════
 *  HIHAT CLOSED
 * ═══════════════════════════════════════════════════════════════ */
class HiHatClosed : public HiHatBase {
public:
    float decay  = 0.04f;   /* 0.02 - 0.15 s  (corto) */
    float tone   = 0.5f;    /* 0.0 - 1.0 */
    float volume = 1.0f;

    void Init(float sampleRate) {
        sr_ = sampleRate;
        dt_ = 1.0f / sr_;
        active_ = false;
    }

    void Trigger(float velocity = 1.0f) {
        active_ = true;
        time_ = 0.0f;
        vel_ = Clamp(velocity, 0.0f, 1.0f);
        for (int i = 0; i < 6; i++) phase_[i] = 0.0f;
        hpZ1_ = hpOut_ = 0.0f;
    }

    float Process() {
        if (!active_) return 0.0f;

        float metal = MetallicNoise();
        float hp = Highpass(metal);
        float env = expf(-time_ / decay);

        float output = hp * env * (0.5f + tone * 0.5f);
        output = tanhf(output * 2.0f);

        time_ += dt_;
        if (env < 0.001f) active_ = false;

        return output * volume * vel_;
    }

    bool IsActive() const { return active_; }
    void SetDecay(float d) { decay = Clamp(d, 0.01f, 0.3f); }
};

/* ═══════════════════════════════════════════════════════════════
 *  HIHAT OPEN
 * ═══════════════════════════════════════════════════════════════ */
class HiHatOpen : public HiHatBase {
public:
    float decay  = 0.25f;   /* 0.1 - 0.8 s (largo) */
    float tone   = 0.5f;
    float volume = 1.0f;

    void Init(float sampleRate) {
        sr_ = sampleRate;
        dt_ = 1.0f / sr_;
        active_ = false;
    }

    void Trigger(float velocity = 1.0f) {
        active_ = true;
        time_ = 0.0f;
        vel_ = Clamp(velocity, 0.0f, 1.0f);
        for (int i = 0; i < 6; i++) phase_[i] = 0.0f;
        hpZ1_ = hpOut_ = 0.0f;
    }

    /* Choke: cerrar el hihat abierto */
    void Choke() { active_ = false; }

    float Process() {
        if (!active_) return 0.0f;

        float metal = MetallicNoise();
        float hp = Highpass(metal);
        float env = expf(-time_ / decay);

        float output = hp * env * (0.5f + tone * 0.5f);
        output = tanhf(output * 2.0f);

        time_ += dt_;
        if (env < 0.001f) active_ = false;

        return output * volume * vel_;
    }

    bool IsActive() const { return active_; }
    void SetDecay(float d) { decay = Clamp(d, 0.05f, 2.0f); }
};

/* ═══════════════════════════════════════════════════════════════
 *  TOM BASE (compartido entre Low, Mid, Hi Tom)
 *  Sine con pitch envelope — como el kick pero más corto
 * ═══════════════════════════════════════════════════════════════ */
class TomBase {
public:
    float decay      = 0.25f;
    float pitch      = 100.0f;
    float pitchDecay = 0.05f;
    float volume     = 1.0f;

    void Init(float sampleRate) {
        sr_ = sampleRate;
        dt_ = 1.0f / sr_;
        active_ = false;
    }

    void Trigger(float velocity = 1.0f) {
        active_ = true;
        time_ = 0.0f;
        phase_ = 0.0f;
        vel_ = Clamp(velocity, 0.0f, 1.0f);
    }

    float Process() {
        if (!active_) return 0.0f;

        float cp = pitch + pitch * 2.0f * expf(-time_ / pitchDecay);
        phase_ += cp * dt_;
        if (phase_ >= 1.0f) phase_ -= 1.0f;
        float sine = sinf(TWOPI_F * phase_);

        float amp = expf(-time_ / decay);
        float output = tanhf(sine * 1.2f) * amp;

        time_ += dt_;
        if (amp < 0.001f) active_ = false;

        return output * volume * vel_;
    }

    bool IsActive() const { return active_; }
    void SetDecay(float d) { decay = Clamp(d, 0.05f, 1.0f); }
    void SetPitch(float p) { pitch = Clamp(p, 40.0f, 500.0f); }

protected:
    float sr_ = 48000.0f, dt_ = 1.0f / 48000.0f;
    bool  active_ = false;
    float time_ = 0.0f;
    float phase_ = 0.0f;
    float vel_ = 1.0f;
};

/* ═══════════════════════════════════════════════════════════════ */
class LowTom : public TomBase {
public:
    LowTom() { pitch = 80.0f; decay = 0.3f; pitchDecay = 0.06f; }
};

class MidTom : public TomBase {
public:
    MidTom() { pitch = 120.0f; decay = 0.25f; pitchDecay = 0.05f; }
};

class HiTom : public TomBase {
public:
    HiTom() { pitch = 180.0f; decay = 0.2f; pitchDecay = 0.04f; }
};

/* ═══════════════════════════════════════════════════════════════
 *  CONGA BASE (sine corto, más "seco" que los toms)
 * ═══════════════════════════════════════════════════════════════ */
class CongaBase {
public:
    float decay  = 0.15f;
    float pitch  = 200.0f;
    float volume = 1.0f;

    void Init(float sampleRate) {
        sr_ = sampleRate;
        dt_ = 1.0f / sr_;
        active_ = false;
    }

    void Trigger(float velocity = 1.0f) {
        active_ = true;
        time_ = 0.0f;
        phase_ = 0.0f;
        vel_ = Clamp(velocity, 0.0f, 1.0f);
    }

    float Process() {
        if (!active_) return 0.0f;

        /* Pitch drop corto */
        float cp = pitch + pitch * 1.5f * expf(-time_ / 0.015f);
        phase_ += cp * dt_;
        if (phase_ >= 1.0f) phase_ -= 1.0f;
        float sine = sinf(TWOPI_F * phase_);

        float amp = expf(-time_ / decay);
        float output = tanhf(sine * 1.1f) * amp;

        time_ += dt_;
        if (amp < 0.001f) active_ = false;

        return output * volume * vel_;
    }

    bool IsActive() const { return active_; }
    void SetDecay(float d) { decay = Clamp(d, 0.03f, 0.5f); }

protected:
    float sr_ = 48000.0f, dt_ = 1.0f / 48000.0f;
    bool  active_ = false;
    float time_ = 0.0f;
    float phase_ = 0.0f;
    float vel_ = 1.0f;
};

class LowConga : public CongaBase {
public:
    LowConga() { pitch = 170.0f; decay = 0.18f; }
};

class MidConga : public CongaBase {
public:
    MidConga() { pitch = 250.0f; decay = 0.15f; }
};

class HiConga : public CongaBase {
public:
    HiConga() { pitch = 370.0f; decay = 0.12f; }
};

/* ═══════════════════════════════════════════════════════════════
 *  CLAVES 808
 *  Click seco + sine cortísimo (~2500 Hz)
 * ═══════════════════════════════════════════════════════════════ */
class Claves {
public:
    float decay  = 0.02f;    /* muy corto */
    float pitch  = 2500.0f;
    float volume = 1.0f;

    void Init(float sampleRate) {
        sr_ = sampleRate;
        dt_ = 1.0f / sr_;
        active_ = false;
    }

    void Trigger(float velocity = 1.0f) {
        active_ = true;
        time_ = 0.0f;
        phase_ = 0.0f;
        vel_ = Clamp(velocity, 0.0f, 1.0f);
    }

    float Process() {
        if (!active_) return 0.0f;

        phase_ += pitch * dt_;
        if (phase_ >= 1.0f) phase_ -= 1.0f;
        float sine = sinf(TWOPI_F * phase_);

        float amp = expf(-time_ / decay);
        float output = sine * amp;

        time_ += dt_;
        if (amp < 0.001f) active_ = false;

        return output * volume * vel_;
    }

    bool IsActive() const { return active_; }

private:
    float sr_ = 48000.0f, dt_ = 1.0f / 48000.0f;
    bool  active_ = false;
    float time_ = 0.0f;
    float phase_ = 0.0f;
    float vel_ = 1.0f;
};

/* ═══════════════════════════════════════════════════════════════
 *  MARACAS 808
 *  Burst de ruido corto highpass-filtered
 * ═══════════════════════════════════════════════════════════════ */
class Maracas {
public:
    float decay  = 0.035f;
    float tone   = 0.7f;
    float volume = 1.0f;

    void Init(float sampleRate) {
        sr_ = sampleRate;
        dt_ = 1.0f / sr_;
        active_ = false;
        noiseState_ = 0xF00DFACE;
    }

    void Trigger(float velocity = 1.0f) {
        active_ = true;
        time_ = 0.0f;
        vel_ = Clamp(velocity, 0.0f, 1.0f);
        hpZ1_ = hpOut_ = 0.0f;
    }

    float Process() {
        if (!active_) return 0.0f;

        float n = Noise(noiseState_);

        /* Highpass ~7kHz */
        float fc = 5000.0f + tone * 5000.0f;
        float rc = 1.0f / (TWOPI_F * fc);
        float alpha = rc / (rc + dt_);
        hpOut_ = alpha * (hpOut_ + n - hpZ1_);
        hpZ1_ = n;

        float env = expf(-time_ / decay);
        float output = hpOut_ * env;

        time_ += dt_;
        if (env < 0.001f) active_ = false;

        return output * volume * vel_;
    }

    bool IsActive() const { return active_; }

private:
    float sr_ = 48000.0f, dt_ = 1.0f / 48000.0f;
    bool  active_ = false;
    float time_ = 0.0f;
    float vel_ = 1.0f;
    uint32_t noiseState_ = 0xF00DFACE;
    float hpZ1_ = 0.0f, hpOut_ = 0.0f;
};

/* ═══════════════════════════════════════════════════════════════
 *  RIMSHOT 808
 *  Click agudo + tono corto (~820 Hz)
 * ═══════════════════════════════════════════════════════════════ */
class RimShot {
public:
    float decay  = 0.025f;
    float pitch  = 820.0f;
    float volume = 1.0f;

    void Init(float sampleRate) {
        sr_ = sampleRate;
        dt_ = 1.0f / sr_;
        active_ = false;
        noiseState_ = 0xABCDEF01;
    }

    void Trigger(float velocity = 1.0f) {
        active_ = true;
        time_ = 0.0f;
        phase_ = 0.0f;
        vel_ = Clamp(velocity, 0.0f, 1.0f);
    }

    float Process() {
        if (!active_) return 0.0f;

        /* Click: burst de ruido HP muy corto */
        float n = Noise(noiseState_);
        float clickEnv = expf(-time_ / 0.0008f);
        float click = n * clickEnv * 0.5f;

        /* Tono */
        phase_ += pitch * dt_;
        if (phase_ >= 1.0f) phase_ -= 1.0f;
        float sine = sinf(TWOPI_F * phase_);
        float toneEnv = expf(-time_ / decay);

        float output = click + sine * toneEnv;
        output = tanhf(output * 1.8f);

        time_ += dt_;
        if (toneEnv < 0.001f && clickEnv < 0.001f) active_ = false;

        return output * volume * vel_;
    }

    bool IsActive() const { return active_; }

private:
    float sr_ = 48000.0f, dt_ = 1.0f / 48000.0f;
    bool  active_ = false;
    float time_ = 0.0f;
    float phase_ = 0.0f;
    float vel_ = 1.0f;
    uint32_t noiseState_ = 0xABCDEF01;
};

/* ═══════════════════════════════════════════════════════════════
 *  COWBELL 808
 *  Dos ondas cuadradas desafinadas (540 Hz + 800 Hz)
 *  Sonido metálico característico
 * ═══════════════════════════════════════════════════════════════ */
class Cowbell {
public:
    float decay  = 0.08f;
    float volume = 1.0f;

    void Init(float sampleRate) {
        sr_ = sampleRate;
        dt_ = 1.0f / sr_;
        active_ = false;
    }

    void Trigger(float velocity = 1.0f) {
        active_ = true;
        time_ = 0.0f;
        phase1_ = 0.0f;
        phase2_ = 0.0f;
        vel_ = Clamp(velocity, 0.0f, 1.0f);
    }

    float Process() {
        if (!active_) return 0.0f;

        /* Dos square waves desafinadas */
        phase1_ += 540.0f * dt_;
        if (phase1_ >= 1.0f) phase1_ -= 1.0f;
        float sq1 = (phase1_ < 0.5f) ? 1.0f : -1.0f;

        phase2_ += 800.0f * dt_;
        if (phase2_ >= 1.0f) phase2_ -= 1.0f;
        float sq2 = (phase2_ < 0.5f) ? 1.0f : -1.0f;

        float mix = (sq1 + sq2) * 0.5f;

        /* Envelope con dos fases: attack rápido, decay */
        float env1 = expf(-time_ / 0.003f);  /* click */
        float env2 = expf(-time_ / decay);    /* body  */
        float env = env2 + (env1 - env2) * 0.3f;

        /* Bandpass ligero para suavizar */
        float output = tanhf(mix * env * 1.5f);

        time_ += dt_;
        if (env2 < 0.001f) active_ = false;

        return output * volume * vel_;
    }

    bool IsActive() const { return active_; }
    void SetDecay(float d) { decay = Clamp(d, 0.03f, 0.5f); }

private:
    float sr_ = 48000.0f, dt_ = 1.0f / 48000.0f;
    bool  active_ = false;
    float time_ = 0.0f;
    float phase1_ = 0.0f, phase2_ = 0.0f;
    float vel_ = 1.0f;
};

/* ═══════════════════════════════════════════════════════════════
 *  CYMBAL 808
 *  Metallic noise (6 square waves) + bandpass largo
 *  Como el hihat pero con decay mucho más largo
 * ═══════════════════════════════════════════════════════════════ */
class Cymbal : public HiHatBase {
public:
    float decay  = 0.8f;    /* 0.3 - 3.0 s */
    float tone   = 0.6f;
    float volume = 1.0f;

    void Init(float sampleRate) {
        sr_ = sampleRate;
        dt_ = 1.0f / sr_;
        active_ = false;
    }

    void Trigger(float velocity = 1.0f) {
        active_ = true;
        time_ = 0.0f;
        vel_ = Clamp(velocity, 0.0f, 1.0f);
        for (int i = 0; i < 6; i++) phase_[i] = 0.0f;
        hpZ1_ = hpOut_ = 0.0f;
    }

    float Process() {
        if (!active_) return 0.0f;

        float metal = MetallicNoise();
        float hp = Highpass(metal);

        /* Decay largo con onset rápido */
        float env = expf(-time_ / decay);
        float attack = 1.0f - expf(-time_ / 0.002f);

        float output = hp * env * attack * (0.4f + tone * 0.6f);
        output = tanhf(output * 1.8f);

        time_ += dt_;
        if (env < 0.0005f) active_ = false;

        return output * volume * vel_;
    }

    bool IsActive() const { return active_; }
    void SetDecay(float d) { decay = Clamp(d, 0.1f, 5.0f); }
};

/* ═══════════════════════════════════════════════════════════════
 *  KIT 808 COMPLETO — contenedor para acceso indexado
 * ═══════════════════════════════════════════════════════════════ */
enum InstrumentId {
    INST_KICK = 0,
    INST_SNARE,
    INST_CLAP,
    INST_HIHAT_C,
    INST_HIHAT_O,
    INST_LOW_TOM,
    INST_MID_TOM,
    INST_HI_TOM,
    INST_LOW_CONGA,
    INST_MID_CONGA,
    INST_HI_CONGA,
    INST_CLAVES,
    INST_MARACAS,
    INST_RIMSHOT,
    INST_COWBELL,
    INST_CYMBAL,
    INST_COUNT
};

class Kit {
public:
    Kick       kick;
    Snare      snare;
    Clap       clap;
    HiHatClosed hihatC;
    HiHatOpen   hihatO;
    LowTom     lowTom;
    MidTom     midTom;
    HiTom      hiTom;
    LowConga   lowConga;
    MidConga   midConga;
    HiConga    hiConga;
    Claves     claves;
    Maracas    maracas;
    RimShot    rimshot;
    Cowbell    cowbell;
    Cymbal     cymbal;

    void Init(float sampleRate) {
        kick.Init(sampleRate);
        snare.Init(sampleRate);
        clap.Init(sampleRate);
        hihatC.Init(sampleRate);
        hihatO.Init(sampleRate);
        lowTom.Init(sampleRate);
        midTom.Init(sampleRate);
        hiTom.Init(sampleRate);
        lowConga.Init(sampleRate);
        midConga.Init(sampleRate);
        hiConga.Init(sampleRate);
        claves.Init(sampleRate);
        maracas.Init(sampleRate);
        rimshot.Init(sampleRate);
        cowbell.Init(sampleRate);
        cymbal.Init(sampleRate);
    }

    void Trigger(uint8_t instrument, float velocity = 1.0f) {
        switch (instrument) {
            case INST_KICK:       kick.Trigger(velocity); break;
            case INST_SNARE:      snare.Trigger(velocity); break;
            case INST_CLAP:       clap.Trigger(velocity); break;
            case INST_HIHAT_C:
                hihatO.Choke();  /* cerrar el open al tocar closed */
                hihatC.Trigger(velocity);
                break;
            case INST_HIHAT_O:   hihatO.Trigger(velocity); break;
            case INST_LOW_TOM:   lowTom.Trigger(velocity); break;
            case INST_MID_TOM:   midTom.Trigger(velocity); break;
            case INST_HI_TOM:    hiTom.Trigger(velocity); break;
            case INST_LOW_CONGA: lowConga.Trigger(velocity); break;
            case INST_MID_CONGA: midConga.Trigger(velocity); break;
            case INST_HI_CONGA:  hiConga.Trigger(velocity); break;
            case INST_CLAVES:    claves.Trigger(velocity); break;
            case INST_MARACAS:   maracas.Trigger(velocity); break;
            case INST_RIMSHOT:   rimshot.Trigger(velocity); break;
            case INST_COWBELL:   cowbell.Trigger(velocity); break;
            case INST_CYMBAL:    cymbal.Trigger(velocity); break;
        }
    }

    /* Procesa TODOS los instrumentos y devuelve la mezcla */
    float Process() {
        float mix = 0.0f;
        mix += kick.Process();
        mix += snare.Process();
        mix += clap.Process();
        mix += hihatC.Process();
        mix += hihatO.Process();
        mix += lowTom.Process();
        mix += midTom.Process();
        mix += hiTom.Process();
        mix += lowConga.Process();
        mix += midConga.Process();
        mix += hiConga.Process();
        mix += claves.Process();
        mix += maracas.Process();
        mix += rimshot.Process();
        mix += cowbell.Process();
        mix += cymbal.Process();
        return mix;
    }

    /* Número de instrumentos activos */
    uint8_t ActiveCount() const {
        uint8_t c = 0;
        if (kick.IsActive())     c++;
        if (snare.IsActive())    c++;
        if (clap.IsActive())     c++;
        if (hihatC.IsActive())   c++;
        if (hihatO.IsActive())   c++;
        if (lowTom.IsActive())   c++;
        if (midTom.IsActive())   c++;
        if (hiTom.IsActive())    c++;
        if (lowConga.IsActive()) c++;
        if (midConga.IsActive()) c++;
        if (hiConga.IsActive())  c++;
        if (claves.IsActive())   c++;
        if (maracas.IsActive())  c++;
        if (rimshot.IsActive())  c++;
        if (cowbell.IsActive())  c++;
        if (cymbal.IsActive())   c++;
        return c;
    }
};

} /* namespace TR808 */
