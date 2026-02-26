/* ═══════════════════════════════════════════════════════════════════
 *  TR505 — Roland TR-505 Drum Synthesis Library
 * ─────────────────────────────────────────────────────────────────
 *  Sonido más digital y lo-fi que 808/909.
 *  La 505 tenía samples PCM de 8 bits — aquí recreamos
 *  su carácter con síntesis simplificada + bit reduction.
 *  Sonido new wave, synth-pop, electro temprano.
 *
 *  48 kHz · float32 · header-only
 *
 *  Instrumentos:
 *    Kick, Snare, Clap, HiHatClosed, HiHatOpen,
 *    LowTom, MidTom, HiTom, Cowbell, Cymbal, RimShot
 * ═══════════════════════════════════════════════════════════════════ */
#pragma once

#include <math.h>
#include <stdint.h>

#ifndef TWOPI_F
#define TWOPI_F 6.283185307f
#endif

namespace TR505 {

static inline float Noise(uint32_t& state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return ((float)(int32_t)state) / 2147483648.0f;
}

static inline float Clamp(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Carácter lo-fi de la 505: reduce resolución */
static inline float LoFi(float s, float amount) {
    if (amount < 0.01f) return s;
    float levels = 256.0f / (1.0f + amount * 240.0f); /* 256 → 16 levels */
    return roundf(s * levels) / levels;
}

/* ═══════════════════════════════════════════════════════════════
 *  KICK 505
 *  Más corto y menos profundo que 808, carácter punchy digital
 * ═══════════════════════════════════════════════════════════════ */
class Kick {
public:
    float decay      = 0.25f;
    float pitch      = 60.0f;
    float pitchDecay = 0.03f;
    float lofi       = 0.15f;   /* cantidad de lo-fi */
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

        float cp = pitch + pitch * 5.0f * expf(-time_ / pitchDecay);
        phase_ += cp * dt_;
        if (phase_ >= 1.0f) phase_ -= 1.0f;
        float sine = sinf(TWOPI_F * phase_);

        float env = expf(-time_ / decay);
        float output = tanhf(sine * 1.3f) * env;
        output = LoFi(output, lofi);

        time_ += dt_;
        if (env < 0.001f) active_ = false;

        return output * volume * vel_;
    }

    bool IsActive() const { return active_; }
    void SetDecay(float d) { decay = Clamp(d, 0.05f, 0.8f); }

private:
    float sr_ = 48000.0f, dt_ = 1.0f / 48000.0f;
    bool  active_ = false;
    float time_ = 0.0f, phase_ = 0.0f, vel_ = 1.0f;
};

/* ═══════════════════════════════════════════════════════════════
 *  SNARE 505
 *  Más digital, tono más agudo, noise más suave
 * ═══════════════════════════════════════════════════════════════ */
class Snare {
public:
    float decay   = 0.15f;
    float tone    = 0.4f;
    float snappy  = 0.6f;
    float pitch   = 220.0f;
    float lofi    = 0.15f;
    float volume  = 1.0f;

    void Init(float sampleRate) {
        sr_ = sampleRate;
        dt_ = 1.0f / sr_;
        active_ = false;
        noiseState_ = 0x505BEEF;
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
        float t = sinf(TWOPI_F * phase_);
        float toneEnv = expf(-time_ / (decay * 0.5f));
        float toneOut = t * toneEnv * tone;

        float n = Noise(noiseState_);
        float noiseEnv = expf(-time_ / decay);
        float noiseOut = n * noiseEnv * snappy;

        float output = toneOut + noiseOut;
        output = tanhf(output * 1.5f);
        output = LoFi(output, lofi);

        time_ += dt_;
        if (noiseEnv < 0.001f) active_ = false;

        return output * volume * vel_;
    }

    bool IsActive() const { return active_; }

private:
    float sr_ = 48000.0f, dt_ = 1.0f / 48000.0f;
    bool  active_ = false;
    float time_ = 0.0f, phase_ = 0.0f, vel_ = 1.0f;
    uint32_t noiseState_ = 0x505BEEF;
};

/* ═══════════════════════════════════════════════════════════════
 *  CLAP 505
 * ═══════════════════════════════════════════════════════════════ */
class Clap {
public:
    float decay  = 0.2f;
    float lofi   = 0.15f;
    float volume = 1.0f;

    void Init(float sampleRate) {
        sr_ = sampleRate;
        dt_ = 1.0f / sr_;
        active_ = false;
        noiseState_ = 0x505C1A9;
    }

    void Trigger(float velocity = 1.0f) {
        active_ = true;
        time_ = 0.0f;
        vel_ = Clamp(velocity, 0.0f, 1.0f);
    }

    float Process() {
        if (!active_) return 0.0f;

        float n = Noise(noiseState_);

        /* 3 ráfagas (simpler que 808/909) */
        float burstT = 0.008f;
        float env = 0.0f;
        for (int i = 0; i < 3; i++) {
            float t = time_ - i * burstT;
            if (t >= 0.0f && t < burstT)
                env += expf(-t / 0.002f) * 0.5f;
        }
        float tailStart = 3.0f * burstT;
        if (time_ >= tailStart)
            env += expf(-(time_ - tailStart) / decay);

        float output = n * env;
        output = tanhf(output * 1.8f);
        output = LoFi(output, lofi);

        time_ += dt_;
        if (time_ > decay + 0.05f && env < 0.001f) active_ = false;

        return output * volume * vel_;
    }

    bool IsActive() const { return active_; }

private:
    float sr_ = 48000.0f, dt_ = 1.0f / 48000.0f;
    bool  active_ = false;
    float time_ = 0.0f, vel_ = 1.0f;
    uint32_t noiseState_ = 0x505C1A9;
};

/* ═══════════════════════════════════════════════════════════════
 *  HIHAT 505 — Más simple, solo noise filtrado
 * ═══════════════════════════════════════════════════════════════ */
class HiHatClosed {
public:
    float decay  = 0.03f;
    float tone   = 0.5f;
    float lofi   = 0.2f;
    float volume = 1.0f;

    void Init(float sampleRate) {
        sr_ = sampleRate;
        dt_ = 1.0f / sr_;
        active_ = false;
        noiseState_ = 0x505AA01;
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

        /* Highpass ~5kHz */
        float fc = 4000.0f + tone * 4000.0f;
        float rc = 1.0f / (TWOPI_F * fc);
        float alpha = rc / (rc + dt_);
        hpOut_ = alpha * (hpOut_ + n - hpZ1_);
        hpZ1_ = n;

        float env = expf(-time_ / decay);
        float output = hpOut_ * env;
        output = LoFi(output, lofi);

        time_ += dt_;
        if (env < 0.001f) active_ = false;

        return output * volume * vel_;
    }

    bool IsActive() const { return active_; }

private:
    float sr_ = 48000.0f, dt_ = 1.0f / 48000.0f;
    bool  active_ = false;
    float time_ = 0.0f, vel_ = 1.0f;
    uint32_t noiseState_ = 0x505AA01;
    float hpZ1_ = 0.0f, hpOut_ = 0.0f;
};

class HiHatOpen {
public:
    float decay  = 0.2f;
    float tone   = 0.5f;
    float lofi   = 0.2f;
    float volume = 1.0f;

    void Init(float sampleRate) {
        sr_ = sampleRate;
        dt_ = 1.0f / sr_;
        active_ = false;
        noiseState_ = 0x505BB02;
    }

    void Trigger(float velocity = 1.0f) {
        active_ = true;
        time_ = 0.0f;
        vel_ = Clamp(velocity, 0.0f, 1.0f);
        hpZ1_ = hpOut_ = 0.0f;
    }

    void Choke() { active_ = false; }

    float Process() {
        if (!active_) return 0.0f;

        float n = Noise(noiseState_);

        float fc = 4000.0f + tone * 4000.0f;
        float rc = 1.0f / (TWOPI_F * fc);
        float alpha = rc / (rc + dt_);
        hpOut_ = alpha * (hpOut_ + n - hpZ1_);
        hpZ1_ = n;

        float env = expf(-time_ / decay);
        float output = hpOut_ * env;
        output = LoFi(output, lofi);

        time_ += dt_;
        if (env < 0.001f) active_ = false;

        return output * volume * vel_;
    }

    bool IsActive() const { return active_; }

private:
    float sr_ = 48000.0f, dt_ = 1.0f / 48000.0f;
    bool  active_ = false;
    float time_ = 0.0f, vel_ = 1.0f;
    uint32_t noiseState_ = 0x505BB02;
    float hpZ1_ = 0.0f, hpOut_ = 0.0f;
};

/* ═══════════════════════════════════════════════════════════════
 *  TOM 505
 * ═══════════════════════════════════════════════════════════════ */
class Tom505 {
public:
    float decay      = 0.15f;
    float pitch      = 120.0f;
    float pitchDecay = 0.025f;
    float lofi       = 0.15f;
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

        float cp = pitch + pitch * 1.5f * expf(-time_ / pitchDecay);
        phase_ += cp * dt_;
        if (phase_ >= 1.0f) phase_ -= 1.0f;
        float sine = sinf(TWOPI_F * phase_);

        float env = expf(-time_ / decay);
        float output = sine * env;
        output = LoFi(output, lofi);

        time_ += dt_;
        if (env < 0.001f) active_ = false;

        return output * volume * vel_;
    }

    bool IsActive() const { return active_; }

protected:
    float sr_ = 48000.0f, dt_ = 1.0f / 48000.0f;
    bool  active_ = false;
    float time_ = 0.0f, phase_ = 0.0f, vel_ = 1.0f;
};

class LowTom : public Tom505 {
public:
    LowTom()  { pitch = 75.0f;  decay = 0.2f;  }
};
class MidTom : public Tom505 {
public:
    MidTom()  { pitch = 110.0f; decay = 0.18f; }
};
class HiTom : public Tom505 {
public:
    HiTom()   { pitch = 160.0f; decay = 0.15f; }
};

/* ═══════════════════════════════════════════════════════════════
 *  COWBELL 505
 * ═══════════════════════════════════════════════════════════════ */
class Cowbell {
public:
    float decay  = 0.06f;
    float lofi   = 0.2f;
    float volume = 1.0f;

    void Init(float sampleRate) {
        sr_ = sampleRate;
        dt_ = 1.0f / sr_;
        active_ = false;
    }

    void Trigger(float velocity = 1.0f) {
        active_ = true;
        time_ = 0.0f;
        phase1_ = phase2_ = 0.0f;
        vel_ = Clamp(velocity, 0.0f, 1.0f);
    }

    float Process() {
        if (!active_) return 0.0f;

        phase1_ += 560.0f * dt_;
        if (phase1_ >= 1.0f) phase1_ -= 1.0f;
        float sq1 = (phase1_ < 0.5f) ? 1.0f : -1.0f;

        phase2_ += 845.0f * dt_;
        if (phase2_ >= 1.0f) phase2_ -= 1.0f;
        float sq2 = (phase2_ < 0.5f) ? 1.0f : -1.0f;

        float mix = (sq1 + sq2) * 0.5f;
        float env = expf(-time_ / decay);

        float output = tanhf(mix * 1.3f) * env;
        output = LoFi(output, lofi);

        time_ += dt_;
        if (env < 0.001f) active_ = false;

        return output * volume * vel_;
    }

    bool IsActive() const { return active_; }

private:
    float sr_ = 48000.0f, dt_ = 1.0f / 48000.0f;
    bool  active_ = false;
    float time_ = 0.0f, vel_ = 1.0f;
    float phase1_ = 0.0f, phase2_ = 0.0f;
};

/* ═══════════════════════════════════════════════════════════════
 *  CYMBAL 505
 * ═══════════════════════════════════════════════════════════════ */
class Cymbal {
public:
    float decay  = 0.6f;
    float lofi   = 0.2f;
    float volume = 1.0f;

    void Init(float sampleRate) {
        sr_ = sampleRate;
        dt_ = 1.0f / sr_;
        active_ = false;
        noiseState_ = 0x505CC03;
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

        float rc = 1.0f / (TWOPI_F * 5000.0f);
        float alpha = rc / (rc + dt_);
        hpOut_ = alpha * (hpOut_ + n - hpZ1_);
        hpZ1_ = n;

        float attack = 1.0f - expf(-time_ / 0.003f);
        float env = expf(-time_ / decay);

        float output = hpOut_ * env * attack;
        output = LoFi(output, lofi);

        time_ += dt_;
        if (env < 0.001f) active_ = false;

        return output * volume * vel_;
    }

    bool IsActive() const { return active_; }

private:
    float sr_ = 48000.0f, dt_ = 1.0f / 48000.0f;
    bool  active_ = false;
    float time_ = 0.0f, vel_ = 1.0f;
    uint32_t noiseState_ = 0x505CC03;
    float hpZ1_ = 0.0f, hpOut_ = 0.0f;
};

/* ═══════════════════════════════════════════════════════════════
 *  RIMSHOT 505
 * ═══════════════════════════════════════════════════════════════ */
class RimShot {
public:
    float decay  = 0.02f;
    float lofi   = 0.15f;
    float volume = 1.0f;

    void Init(float sampleRate) {
        sr_ = sampleRate;
        dt_ = 1.0f / sr_;
        active_ = false;
        noiseState_ = 0x505DD04;
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
        float clickEnv = expf(-time_ / 0.0008f);
        float click = n * clickEnv * 0.4f;

        phase_ += 750.0f * dt_;
        if (phase_ >= 1.0f) phase_ -= 1.0f;
        float sine = sinf(TWOPI_F * phase_);
        float toneEnv = expf(-time_ / decay);

        float output = click + sine * toneEnv;
        output = tanhf(output * 1.5f);
        output = LoFi(output, lofi);

        time_ += dt_;
        if (toneEnv < 0.001f) active_ = false;

        return output * volume * vel_;
    }

    bool IsActive() const { return active_; }

private:
    float sr_ = 48000.0f, dt_ = 1.0f / 48000.0f;
    bool  active_ = false;
    float time_ = 0.0f, phase_ = 0.0f, vel_ = 1.0f;
    uint32_t noiseState_ = 0x505DD04;
};

/* ═══════════════════════════════════════════════════════════════
 *  KIT 505 COMPLETO
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
    INST_COWBELL,
    INST_CYMBAL,
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
    Cowbell    cowbell;
    Cymbal     cymbal;
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
        cowbell.Init(sampleRate);
        cymbal.Init(sampleRate);
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
            case INST_COWBELL:  cowbell.Trigger(velocity); break;
            case INST_CYMBAL:   cymbal.Trigger(velocity); break;
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
        mix += cowbell.Process();
        mix += cymbal.Process();
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
        if (cowbell.IsActive()) c++;
        if (cymbal.IsActive())  c++;
        if (rimshot.IsActive()) c++;
        return c;
    }
};

} /* namespace TR505 */
