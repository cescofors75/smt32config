/* ═══════════════════════════════════════════════════════════════════
 *  TR909 — Roland TR-909 Drum Synthesis Library
 * ─────────────────────────────────────────────────────────────────
 *  Más agresiva que la 808: kick con más punch, snare más brillante,
 *  hihats con sampleo digital real (aquí sintetizado), clap más denso.
 *  La 909 es el sonido del techno, house y trance.
 *
 *  48 kHz · float32 · header-only
 *
 *  Instrumentos:
 *    Kick, Snare, Clap, HiHatClosed, HiHatOpen,
 *    LowTom, MidTom, HiTom, Ride, Crash, RimShot
 * ═══════════════════════════════════════════════════════════════════ */
#pragma once

#include <math.h>
#include <stdint.h>

#ifndef TWOPI_F
#define TWOPI_F 6.283185307f
#endif

namespace TR909 {

static inline float Noise(uint32_t& state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return ((float)(int32_t)state) / 2147483648.0f;
}

static inline float Clamp(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* ═══════════════════════════════════════════════════════════════
 *  KICK 909
 *  Más punch que la 808: click transitorio más fuerte,
 *  pitch envelope más agresivo, compresión interna
 *  El kick del techno por excelencia
 * ═══════════════════════════════════════════════════════════════ */
class Kick {
public:
    float decay      = 0.5f;    /* 0.15 - 1.2 s */
    float pitch      = 50.0f;   /* 35 - 90 Hz   */
    float pitchDecay = 0.04f;   /* más rápido que 808 */
    float attack     = 0.3f;    /* 0.0 - 1.0 click amount */
    float compression= 0.5f;    /* 0.0 - 1.0 punch */
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

        /* Pitch sweep más agresivo que 808 */
        float sweep = pitch * 12.0f * expf(-time_ / pitchDecay);
        float cp = pitch + sweep;

        phase_ += cp * dt_;
        if (phase_ >= 1.0f) phase_ -= 1.0f;
        float sine = sinf(TWOPI_F * phase_);

        /* Click transitorio punchy */
        float clickEnv = expf(-time_ / 0.001f);
        float click = clickEnv * sinf(TWOPI_F * 3000.0f * time_) * attack;

        /* Amp envelope con knee de compresión */
        float env = expf(-time_ / decay);
        /* Compresión: mantiene el cuerpo durante más tiempo */
        if (compression > 0.01f) {
            float sustain = expf(-time_ / (decay * 3.0f));
            env = env * (1.0f - compression * 0.5f) + sustain * compression * 0.5f;
        }

        float output = sine + click;
        /* Saturación más fuerte que 808 */
        output = tanhf(output * 1.8f);

        time_ += dt_;
        if (env < 0.001f) active_ = false;

        return output * env * volume * vel_;
    }

    bool IsActive() const { return active_; }
    void SetDecay(float d) { decay = Clamp(d, 0.1f, 2.0f); }
    void SetPitch(float p) { pitch = Clamp(p, 30.0f, 120.0f); }

private:
    float sr_ = 48000.0f, dt_ = 1.0f / 48000.0f;
    bool  active_ = false;
    float time_ = 0.0f, phase_ = 0.0f, vel_ = 1.0f;
};

/* ═══════════════════════════════════════════════════════════════
 *  SNARE 909
 *  Más brillante que 808: componente tonal más agudo,
 *  noise más presente, dos modos de decay
 * ═══════════════════════════════════════════════════════════════ */
class Snare {
public:
    float decay   = 0.18f;
    float tone    = 0.6f;      /* mezcla tono/ruido */
    float snappy  = 0.7f;      /* cantidad de noise */
    float pitch   = 200.0f;    /* Hz tono base */
    float volume  = 1.0f;

    void Init(float sampleRate) {
        sr_ = sampleRate;
        dt_ = 1.0f / sr_;
        active_ = false;
        noiseState_ = 0x909BEEF;
    }

    void Trigger(float velocity = 1.0f) {
        active_ = true;
        time_ = 0.0f;
        phase1_ = phase2_ = 0.0f;
        vel_ = Clamp(velocity, 0.0f, 1.0f);
        hpZ1_ = hpOut_ = 0.0f;
    }

    float Process() {
        if (!active_) return 0.0f;

        /* Dos tonos: fundamental + armónico */
        phase1_ += pitch * dt_;
        if (phase1_ >= 1.0f) phase1_ -= 1.0f;
        float t1 = sinf(TWOPI_F * phase1_);

        phase2_ += pitch * 1.6f * dt_;
        if (phase2_ >= 1.0f) phase2_ -= 1.0f;
        float t2 = sinf(TWOPI_F * phase2_);

        float toneEnv = expf(-time_ / (decay * 0.4f));
        float toneOut = (t1 * 0.7f + t2 * 0.3f) * toneEnv * tone;

        /* Ruido más blanco y brillante que 808 */
        float n = Noise(noiseState_);
        /* High-pass para más "snap" */
        float rc = 1.0f / (TWOPI_F * 3000.0f);
        float alpha = rc / (rc + dt_);
        hpOut_ = alpha * (hpOut_ + n - hpZ1_);
        hpZ1_ = n;

        float noiseEnv = expf(-time_ / decay);
        float noiseOut = hpOut_ * noiseEnv * snappy * 1.5f;

        float output = toneOut + noiseOut;
        output = tanhf(output * 2.0f);

        time_ += dt_;
        if (noiseEnv < 0.001f) active_ = false;

        return output * volume * vel_;
    }

    bool IsActive() const { return active_; }
    void SetDecay(float d)  { decay = Clamp(d, 0.05f, 0.8f); }
    void SetTone(float t)   { tone = Clamp(t, 0.0f, 1.0f); }
    void SetSnappy(float s) { snappy = Clamp(s, 0.0f, 1.0f); }

private:
    float sr_ = 48000.0f, dt_ = 1.0f / 48000.0f;
    bool  active_ = false;
    float time_ = 0.0f;
    float phase1_ = 0.0f, phase2_ = 0.0f;
    float vel_ = 1.0f;
    uint32_t noiseState_ = 0x909BEEF;
    float hpZ1_ = 0.0f, hpOut_ = 0.0f;
};

/* ═══════════════════════════════════════════════════════════════
 *  CLAP 909
 *  Similar a 808 pero con más ráfagas y más brillante
 * ═══════════════════════════════════════════════════════════════ */
class Clap {
public:
    float decay  = 0.25f;
    float tone   = 0.6f;
    float volume = 1.0f;

    void Init(float sampleRate) {
        sr_ = sampleRate;
        dt_ = 1.0f / sr_;
        active_ = false;
        noiseState_ = 0x909CAFE;
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

        /* 6 micro-ráfagas (más que 808) */
        float burstT = 0.005f;
        float env = 0.0f;
        for (int i = 0; i < 6; i++) {
            float t = time_ - i * burstT;
            if (t >= 0.0f && t < burstT)
                env += expf(-t / 0.0015f) * 0.4f;
        }
        float tailStart = 6.0f * burstT;
        if (time_ >= tailStart)
            env += expf(-(time_ - tailStart) / decay);

        /* Bandpass más brillante que 808 (~2kHz) */
        float fc = 1800.0f + tone * 4000.0f;
        float w = TWOPI_F * fc / sr_;
        float sw = sinf(w), cw = cosf(w);
        float q = 2.5f;
        float alpha = sw / (2.0f * q);
        float a0i = 1.0f / (1.0f + alpha);
        float out = (alpha * n - alpha * bpZ2_) * a0i
                  - (-2.0f * cw) * a0i * bpZ1_
                  - (1.0f - alpha) * a0i * bpZ2_;
        bpZ2_ = bpZ1_;
        bpZ1_ = out;

        float output = out * env;
        output = tanhf(output * 2.5f);

        time_ += dt_;
        if (time_ > decay + 0.06f && env < 0.001f) active_ = false;

        return output * volume * vel_;
    }

    bool IsActive() const { return active_; }

private:
    float sr_ = 48000.0f, dt_ = 1.0f / 48000.0f;
    bool  active_ = false;
    float time_ = 0.0f;
    float vel_ = 1.0f;
    uint32_t noiseState_ = 0x909CAFE;
    float bpZ1_ = 0.0f, bpZ2_ = 0.0f;
};

/* ═══════════════════════════════════════════════════════════════
 *  HIHAT 909 BASE
 *  6 pulsos metálicos — frecuencias diferentes a la 808
 *  Más brillante, más "digital" en carácter
 * ═══════════════════════════════════════════════════════════════ */
class HiHat909Base {
protected:
    static constexpr float METAL_FREQS[6] = {
        263.5f, 400.0f, 531.0f, 588.0f, 678.0f, 1043.0f
    };

    float sr_ = 48000.0f, dt_ = 1.0f / 48000.0f;
    bool  active_ = false;
    float time_ = 0.0f, vel_ = 1.0f;
    float phase_[6] = {};
    uint32_t noiseState_ = 0x909HHAT;
    float hpZ1_ = 0.0f, hpOut_ = 0.0f;

    float MetallicNoise() {
        float sum = 0.0f;
        for (int i = 0; i < 6; i++) {
            phase_[i] += METAL_FREQS[i] * dt_;
            if (phase_[i] >= 1.0f) phase_[i] -= 1.0f;
            sum += (phase_[i] < 0.5f) ? 1.0f : -1.0f;
        }
        float n = Noise(noiseState_) * 0.2f;
        return (sum / 6.0f + n);
    }

    float Highpass(float in) {
        float rc = 1.0f / (TWOPI_F * 7500.0f); /* más alto que 808 */
        float alpha = rc / (rc + dt_);
        hpOut_ = alpha * (hpOut_ + in - hpZ1_);
        hpZ1_ = in;
        return hpOut_;
    }
};

constexpr float HiHat909Base::METAL_FREQS[6];

class HiHatClosed : public HiHat909Base {
public:
    float decay  = 0.03f;
    float tone   = 0.6f;
    float volume = 1.0f;

    void Init(float sampleRate) {
        sr_ = sampleRate;
        dt_ = 1.0f / sr_;
        active_ = false;
        noiseState_ = 0x909AAAA;
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
        output = tanhf(output * 2.5f);

        time_ += dt_;
        if (env < 0.001f) active_ = false;

        return output * volume * vel_;
    }

    bool IsActive() const { return active_; }
    void SetDecay(float d) { decay = Clamp(d, 0.01f, 0.2f); }
};

class HiHatOpen : public HiHat909Base {
public:
    float decay  = 0.3f;
    float tone   = 0.6f;
    float volume = 1.0f;

    void Init(float sampleRate) {
        sr_ = sampleRate;
        dt_ = 1.0f / sr_;
        active_ = false;
        noiseState_ = 0x909BBBB;
    }

    void Trigger(float velocity = 1.0f) {
        active_ = true;
        time_ = 0.0f;
        vel_ = Clamp(velocity, 0.0f, 1.0f);
        for (int i = 0; i < 6; i++) phase_[i] = 0.0f;
        hpZ1_ = hpOut_ = 0.0f;
    }

    void Choke() { active_ = false; }

    float Process() {
        if (!active_) return 0.0f;

        float metal = MetallicNoise();
        float hp = Highpass(metal);
        float env = expf(-time_ / decay);

        float output = hp * env * (0.5f + tone * 0.5f);
        output = tanhf(output * 2.5f);

        time_ += dt_;
        if (env < 0.001f) active_ = false;

        return output * volume * vel_;
    }

    bool IsActive() const { return active_; }
    void SetDecay(float d) { decay = Clamp(d, 0.05f, 2.0f); }
};

/* ═══════════════════════════════════════════════════════════════
 *  TOM 909
 * ═══════════════════════════════════════════════════════════════ */
class Tom909 {
public:
    float decay      = 0.2f;
    float pitch      = 150.0f;
    float pitchDecay = 0.03f;
    float attack     = 0.4f;   /* click amount */
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

        float cp = pitch + pitch * 3.0f * expf(-time_ / pitchDecay);
        phase_ += cp * dt_;
        if (phase_ >= 1.0f) phase_ -= 1.0f;
        float sine = sinf(TWOPI_F * phase_);

        /* Click transitorio */
        float clickEnv = expf(-time_ / 0.001f);
        float click = clickEnv * sinf(TWOPI_F * 1500.0f * time_) * attack;

        float env = expf(-time_ / decay);
        float output = tanhf((sine + click * 0.3f) * 1.4f) * env;

        time_ += dt_;
        if (env < 0.001f) active_ = false;

        return output * volume * vel_;
    }

    bool IsActive() const { return active_; }
    void SetDecay(float d) { decay = Clamp(d, 0.05f, 0.8f); }
    void SetPitch(float p) { pitch = Clamp(p, 60.0f, 400.0f); }

private:
    float sr_ = 48000.0f, dt_ = 1.0f / 48000.0f;
    bool  active_ = false;
    float time_ = 0.0f, phase_ = 0.0f, vel_ = 1.0f;
};

class LowTom : public Tom909 {
public:
    LowTom()  { pitch = 90.0f;  decay = 0.3f;  }
};
class MidTom : public Tom909 {
public:
    MidTom()  { pitch = 140.0f; decay = 0.25f; }
};
class HiTom : public Tom909 {
public:
    HiTom()   { pitch = 210.0f; decay = 0.2f;  }
};

/* ═══════════════════════════════════════════════════════════════
 *  RIDE 909 — Metallic noise largo con shimmer
 * ═══════════════════════════════════════════════════════════════ */
class Ride : public HiHat909Base {
public:
    float decay  = 1.5f;
    float tone   = 0.5f;
    float volume = 1.0f;

    void Init(float sampleRate) {
        sr_ = sampleRate;
        dt_ = 1.0f / sr_;
        active_ = false;
        noiseState_ = 0x909RIDE;
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

        float attack = 1.0f - expf(-time_ / 0.001f);
        float env = expf(-time_ / decay);

        float output = hp * env * attack * (0.3f + tone * 0.4f);
        output = tanhf(output * 1.5f);

        time_ += dt_;
        if (env < 0.0005f) active_ = false;

        return output * volume * vel_;
    }

    bool IsActive() const { return active_; }
    void SetDecay(float d) { decay = Clamp(d, 0.5f, 4.0f); }
};

/* ═══════════════════════════════════════════════════════════════
 *  CRASH 909 — Cymbal largo y brillante
 * ═══════════════════════════════════════════════════════════════ */
class Crash : public HiHat909Base {
public:
    float decay  = 2.5f;
    float tone   = 0.7f;
    float volume = 1.0f;

    void Init(float sampleRate) {
        sr_ = sampleRate;
        dt_ = 1.0f / sr_;
        active_ = false;
        noiseState_ = 0x909CRSH;
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
        float n = Noise(noiseState_) * 0.2f;  /* extra noise para brillo */

        float attack = 1.0f - expf(-time_ / 0.003f);
        float env = expf(-time_ / decay);

        float output = (hp + n) * env * attack * (0.3f + tone * 0.5f);
        output = tanhf(output * 2.0f);

        time_ += dt_;
        if (env < 0.0005f) active_ = false;

        return output * volume * vel_;
    }

    bool IsActive() const { return active_; }
    void SetDecay(float d) { decay = Clamp(d, 0.5f, 6.0f); }
};

/* ═══════════════════════════════════════════════════════════════
 *  RIMSHOT 909
 * ═══════════════════════════════════════════════════════════════ */
class RimShot {
public:
    float decay  = 0.02f;
    float pitch  = 880.0f;
    float volume = 1.0f;

    void Init(float sampleRate) {
        sr_ = sampleRate;
        dt_ = 1.0f / sr_;
        active_ = false;
        noiseState_ = 0x909RIMS;
    }

    void Trigger(float velocity = 1.0f) {
        active_ = true;
        time_ = 0.0f;
        phase_ = 0.0f;
        vel_ = Clamp(velocity, 0.0f, 1.0f);
    }

    float Process() {
        if (!active_) return 0.0f;

        float n = Noise(noiseState_);
        float clickEnv = expf(-time_ / 0.0005f);
        float click = n * clickEnv * 0.6f;

        phase_ += pitch * dt_;
        if (phase_ >= 1.0f) phase_ -= 1.0f;
        float sine = sinf(TWOPI_F * phase_);
        float toneEnv = expf(-time_ / decay);

        float output = click + sine * toneEnv;
        output = tanhf(output * 2.0f);

        time_ += dt_;
        if (toneEnv < 0.001f) active_ = false;

        return output * volume * vel_;
    }

    bool IsActive() const { return active_; }

private:
    float sr_ = 48000.0f, dt_ = 1.0f / 48000.0f;
    bool  active_ = false;
    float time_ = 0.0f, phase_ = 0.0f, vel_ = 1.0f;
    uint32_t noiseState_ = 0x909RIMS;
};

/* ═══════════════════════════════════════════════════════════════
 *  KIT 909 COMPLETO
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
    INST_RIDE,
    INST_CRASH,
    INST_RIMSHOT,
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
    Ride       ride;
    Crash      crash;
    RimShot    rimshot;

    void Init(float sampleRate) {
        kick.Init(sampleRate);
        snare.Init(sampleRate);
        clap.Init(sampleRate);
        hihatC.Init(sampleRate);
        hihatO.Init(sampleRate);
        lowTom.Init(sampleRate);
        midTom.Init(sampleRate);
        hiTom.Init(sampleRate);
        ride.Init(sampleRate);
        crash.Init(sampleRate);
        rimshot.Init(sampleRate);
    }

    void Trigger(uint8_t instrument, float velocity = 1.0f) {
        switch (instrument) {
            case INST_KICK:     kick.Trigger(velocity); break;
            case INST_SNARE:    snare.Trigger(velocity); break;
            case INST_CLAP:     clap.Trigger(velocity); break;
            case INST_HIHAT_C:
                hihatO.Choke();
                hihatC.Trigger(velocity);
                break;
            case INST_HIHAT_O:  hihatO.Trigger(velocity); break;
            case INST_LOW_TOM:  lowTom.Trigger(velocity); break;
            case INST_MID_TOM:  midTom.Trigger(velocity); break;
            case INST_HI_TOM:   hiTom.Trigger(velocity); break;
            case INST_RIDE:     ride.Trigger(velocity); break;
            case INST_CRASH:    crash.Trigger(velocity); break;
            case INST_RIMSHOT:  rimshot.Trigger(velocity); break;
        }
    }

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
        mix += ride.Process();
        mix += crash.Process();
        mix += rimshot.Process();
        return mix;
    }

    uint8_t ActiveCount() const {
        uint8_t c = 0;
        if (kick.IsActive())    c++;
        if (snare.IsActive())   c++;
        if (clap.IsActive())    c++;
        if (hihatC.IsActive())  c++;
        if (hihatO.IsActive())  c++;
        if (lowTom.IsActive())  c++;
        if (midTom.IsActive())  c++;
        if (hiTom.IsActive())   c++;
        if (ride.IsActive())    c++;
        if (crash.IsActive())   c++;
        if (rimshot.IsActive()) c++;
        return c;
    }
};

} /* namespace TR909 */
