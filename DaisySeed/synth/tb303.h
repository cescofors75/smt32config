/* ═══════════════════════════════════════════════════════════════════
 *  TB303 — Roland TB-303 Acid Bass Synthesizer Library
 * ─────────────────────────────────────────────────────────────────
 *  El corazón del acid house: oscilador SAW/SQUARE → filtro ladder
 *  resonante 24dB/oct → VCA con accent y slide (portamento).
 *
 *  La magia está en el filtro: 4 polos en cascada con
 *  retroalimentación = resonancia que grita.
 *
 *  48 kHz · float32 · header-only
 *
 *  Uso típico:
 *    TB303::Synth acid;
 *    acid.Init(48000);
 *    acid.NoteOn(midiToFreq(36), true, false);
 *    float sample = acid.Process(); // en AudioCallback
 * ═══════════════════════════════════════════════════════════════════ */
#pragma once

#include <math.h>
#include <stdint.h>

#ifndef TWOPI_F
#define TWOPI_F 6.283185307f
#endif

namespace TB303 {

static inline float Clamp(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Conversión MIDI → Hz */
static inline float MidiToFreq(uint8_t note) {
    return 440.0f * powf(2.0f, ((float)note - 69.0f) / 12.0f);
}

/* ═══════════════════════════════════════════════════════════════
 *  Waveform selection
 * ═══════════════════════════════════════════════════════════════ */
enum Waveform {
    WAVE_SAW = 0,
    WAVE_SQUARE = 1
};

/* ═══════════════════════════════════════════════════════════════
 *  SYNTH 303 — Monofonía completa
 *
 *  Signal flow:
 *    OSC (saw/square) → LADDER FILTER (24dB/oct) → VCA
 *          ↑                    ↑                    ↑
 *      slide/pitch          env + accent          env + accent
 * ═══════════════════════════════════════════════════════════════ */
class Synth {
public:
    /* ─── PARÁMETROS (los knobs de la 303) ─── */
    float    cutoff     = 800.0f;   /* 20 Hz  - 20 kHz  freq del filtro    */
    float    resonance  = 0.5f;     /* 0.0    - 0.95    (cerca de 1 = acid)*/
    float    envMod     = 0.5f;     /* 0.0    - 1.0     cuánto env→cutoff  */
    float    decay      = 0.3f;     /* 0.05   - 2.0 s   decay del filtro   */
    float    accentAmt  = 0.5f;     /* 0.0    - 1.0     intensidad accent  */
    float    slideTime  = 0.06f;    /* 0.02   - 0.2 s   portamento         */
    Waveform waveform   = WAVE_SAW; /* SAW / SQUARE                        */
    float    volume     = 0.7f;     /* 0.0    - 1.0                        */

    void Init(float sampleRate) {
        sr_ = sampleRate;
        dt_ = 1.0f / sr_;
        active_ = false;
        phase_ = 0.0f;
        currentFreq_ = 220.0f;
        targetFreq_ = 220.0f;
        /* Reset filtro ladder */
        for (int i = 0; i < 4; i++) stage_[i] = 0.0f;
        delay_[0] = delay_[1] = delay_[2] = delay_[3] = 0.0f;
        filterEnv_ = 0.0f;
        ampEnv_ = 0.0f;
        gateOn_ = false;
        accent_ = false;
        sliding_ = false;
    }

    /* ─── NoteOn: dispara una nota ─── */
    void NoteOn(float freq, bool accent = false, bool slide = false) {
        targetFreq_ = Clamp(freq, 20.0f, 5000.0f);
        accent_ = accent;

        if (slide && active_) {
            /* Slide: suaviza la transición de nota */
            sliding_ = true;
        } else {
            /* Nota nueva sin slide */
            sliding_ = false;
            currentFreq_ = targetFreq_;
            /* Reset envelope del filtro */
            filterEnv_ = 1.0f;
        }

        gateOn_ = true;
        active_ = true;

        /* Accent aumenta la velocidad del envelope */
        if (accent_) {
            filterEnv_ = 1.2f; /* más excursión */
        }
    }

    void NoteOn(uint8_t midiNote, bool accent = false, bool slide = false) {
        NoteOn(MidiToFreq(midiNote), accent, slide);
    }

    /* ─── NoteOff: libera la nota ─── */
    void NoteOff() {
        gateOn_ = false;
    }

    /* ─── Process: genera 1 muestra de audio ─── */
    float Process() {
        if (!active_) return 0.0f;

        /* ── 1. SLIDE (portamento) ── */
        if (sliding_) {
            float slideRate = expf(-dt_ / slideTime);
            currentFreq_ = currentFreq_ * slideRate + targetFreq_ * (1.0f - slideRate);
            /* Llegó al destino? */
            if (fabsf(currentFreq_ - targetFreq_) < 0.1f) {
                currentFreq_ = targetFreq_;
                sliding_ = false;
            }
        }

        /* ── 2. OSCILADOR ── */
        phase_ += currentFreq_ * dt_;
        if (phase_ >= 1.0f) phase_ -= 1.0f;

        float osc;
        if (waveform == WAVE_SAW) {
            /* Saw: rampa de -1 a +1 */
            osc = 2.0f * phase_ - 1.0f;
            /* PolyBLEP anti-aliasing */
            osc -= PolyBlep(phase_, currentFreq_ * dt_);
        } else {
            /* Square: banda limitada */
            osc = (phase_ < 0.5f) ? 1.0f : -1.0f;
            osc += PolyBlep(phase_, currentFreq_ * dt_);
            float p2 = phase_ + 0.5f;
            if (p2 >= 1.0f) p2 -= 1.0f;
            osc -= PolyBlep(p2, currentFreq_ * dt_);
        }

        /* ── 3. ENVELOPES ── */
        /* Filter envelope: decay exponencial */
        float envDecay = accent_ ? decay * 0.7f : decay;
        filterEnv_ *= expf(-dt_ / envDecay);

        /* Amp envelope */
        if (gateOn_) {
            /* Attack rápido */
            ampEnv_ += (1.0f - ampEnv_) * 0.05f;
        } else {
            /* Release */
            float relTime = accent_ ? 0.01f : 0.005f;
            ampEnv_ *= expf(-dt_ / relTime);
            if (ampEnv_ < 0.001f) {
                active_ = false;
                return 0.0f;
            }
        }

        /* ── 4. FILTRO LADDER (el corazón del acid) ── */
        /* Calcular cutoff modulado por envelope */
        float accentBoost = accent_ ? accentAmt * 6000.0f : 0.0f;
        float envAmount = envMod * 10000.0f * filterEnv_;
        float fc = cutoff + envAmount + accentBoost;
        fc = Clamp(fc, 20.0f, sr_ * 0.45f);

        /* Resonancia: accent la aumenta */
        float res = resonance;
        if (accent_) res = Clamp(res + accentAmt * 0.3f, 0.0f, 0.95f);

        float filtered = LadderFilter(osc, fc, res);

        /* ── 5. VCA ── */
        float accentGain = accent_ ? 1.0f + accentAmt * 0.4f : 1.0f;
        float output = filtered * ampEnv_ * volume * accentGain;

        /* Soft clip final */
        output = tanhf(output * 1.5f);

        return output;
    }

    bool IsActive() const { return active_; }
    bool IsGateOn() const { return gateOn_; }

    /* ─── Setters para control en vivo ─── */
    void SetCutoff(float c)     { cutoff = Clamp(c, 20.0f, 20000.0f); }
    void SetResonance(float r)  { resonance = Clamp(r, 0.0f, 0.95f); }
    void SetEnvMod(float e)     { envMod = Clamp(e, 0.0f, 1.0f); }
    void SetDecay(float d)      { decay = Clamp(d, 0.02f, 3.0f); }
    void SetAccent(float a)     { accentAmt = Clamp(a, 0.0f, 1.0f); }
    void SetSlide(float s)      { slideTime = Clamp(s, 0.01f, 0.5f); }
    void SetWaveform(Waveform w){ waveform = w; }

private:
    float sr_ = 48000.0f;
    float dt_ = 1.0f / 48000.0f;

    /* Estado del oscilador */
    float phase_ = 0.0f;
    float currentFreq_ = 220.0f;
    float targetFreq_ = 220.0f;

    /* Estado activo */
    bool  active_ = false;
    bool  gateOn_ = false;
    bool  accent_ = false;
    bool  sliding_ = false;

    /* Envelopes */
    float filterEnv_ = 0.0f;
    float ampEnv_ = 0.0f;

    /* Filtro ladder: 4 stages */
    float stage_[4] = {};
    float delay_[4] = {};

    /* ═══════════════════════════════════════════════════════════
     *  LADDER FILTER — 4 polos en cascada (24 dB/octava)
     *  ─────────────────────────────────────────────────────────
     *  Cada stage = filtro de 1 polo (6dB/oct)
     *  4 stages en cascada = 24dB/oct
     *  Retroalimentación de la salida = resonancia
     *  
     *  Es la misma topología del Moog ladder original,
     *  adaptada para la 303 (diode ladder).
     *  La diferencia es que la 303 usa diodos en vez de
     *  transistores, dando un carácter más "chirpy".
     * ═══════════════════════════════════════════════════════════ */
    float LadderFilter(float input, float fc, float res) {
        /* Frecuencia normalizada */
        float f = 2.0f * fc / sr_;
        if (f > 0.99f) f = 0.99f;

        /* Coeficiente del filtro (tuning) */
        /* Aproximación: compensar la desviación no-lineal */
        float g = f * (1.0f + f * (-0.25f));

        /* Retroalimentación = resonancia × 4 (4 polos) */
        float fb = res * 4.0f;

        /* Compensación de ganancia para resonancia alta */
        float comp = 1.0f / (1.0f + fb * 0.25f);

        /* Entrada con retroalimentación */
        float fbSig = delay_[3];
        float in = (input - fb * fbSig) * comp;

        /* Saturación suave en la entrada (carácter diodo 303) */
        in = tanhf(in);

        /* 4 stages de filtro en cascada */
        for (int i = 0; i < 4; i++) {
            float prev = (i == 0) ? in : stage_[i - 1];
            stage_[i] = delay_[i] + g * (tanhf(prev) - tanhf(delay_[i]));
            delay_[i] = stage_[i];
        }

        return stage_[3];
    }

    /* ═══════════════════════════════════════════════════════════
     *  PolyBLEP — Anti-aliasing para osciladores
     *  Suaviza las discontinuidades de saw/square
     * ═══════════════════════════════════════════════════════════ */
    static float PolyBlep(float phase, float dt) {
        if (phase < dt) {
            float t = phase / dt;
            return t + t - t * t - 1.0f;
        }
        if (phase > 1.0f - dt) {
            float t = (phase - 1.0f) / dt;
            return t * t + t + t + 1.0f;
        }
        return 0.0f;
    }
};

} /* namespace TB303 */
