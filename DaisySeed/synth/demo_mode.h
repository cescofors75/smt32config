/* ═══════════════════════════════════════════════════════════════════
 *  DEMO MODE — Secuencia automática de demostración
 * ─────────────────────────────────────────────────────────────────
 *  3 minutos de audio generado en tiempo real.
 *  Sin display. Sin pads. Sin SPI. Solo audio out por el jack.
 *
 *  GUIÓN:
 *    0:00  Kick 808 solo. 90 BPM.
 *    0:15  Entra snare 808.
 *    0:25  Entran hi-hats. Swing 56%.
 *    0:40  Entra línea 303 filtro cerrado.
 *    1:00  Filter sweep 303. Cutoff sube solo.
 *    1:30  MORPHING: 808→909, BPM 90→145, swing→0, cutoff→4000
 *    2:10  Detroit completo. 145 BPM.
 *    2:50  Fade out lento.
 *    3:00  Silencio. Reinicia demo.
 *
 *  Dependencias: synth/tr808.h, synth/tr909.h, synth/tb303.h
 * ═══════════════════════════════════════════════════════════════════ */
#pragma once

#include <math.h>
#include <stdint.h>

/* ═══════════════════════════════════════════════════════════════════
 *  FUNCIONES MATEMÁTICAS BASE
 * ═══════════════════════════════════════════════════════════════════ */

namespace Demo {

/* ── lerp: interpolación lineal ─── */
static inline float lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

/* ── clamp01 ─── */
static inline float clamp01(float v) {
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

/* ── Conversión ms → samples ─── */
static inline uint32_t msToSamples(float ms, float sr) {
    return (uint32_t)(ms * sr / 1000.0f);
}

/* ── Conversión seconds → samples ─── */
static inline uint32_t secToSamples(float sec, float sr) {
    return (uint32_t)(sec * sr);
}

/* ═══════════════════════════════════════════════════════════════════
 *  AutoParam — parámetro que cambia solo en el tiempo
 *  Interpola linealmente de startVal a endVal durante durationSamples
 * ═══════════════════════════════════════════════════════════════════ */
struct AutoParam {
    float    startVal       = 0.0f;
    float    endVal         = 1.0f;
    float    currentVal     = 0.0f;
    uint32_t startSample    = 0;
    uint32_t durationSamples= 0;
    bool     active         = false;

    void Set(float from, float to, uint32_t start, uint32_t duration) {
        startVal        = from;
        endVal          = to;
        currentVal      = from;
        startSample     = start;
        durationSamples = duration;
        active          = true;
    }

    float Update(uint32_t now) {
        if (!active) return currentVal;
        if (now < startSample) { currentVal = startVal; return currentVal; }
        if (durationSamples == 0) { currentVal = endVal; active = false; return currentVal; }
        float t = (float)(now - startSample) / (float)durationSamples;
        if (t >= 1.0f) { t = 1.0f; active = false; }
        currentVal = lerp(startVal, endVal, t);
        return currentVal;
    }
};

/* ═══════════════════════════════════════════════════════════════════
 *  SECUENCIA DE NOTAS 303
 *  Patrón acid clásico: 16 steps, notas MIDI, accent/slide
 * ═══════════════════════════════════════════════════════════════════ */
struct AcidStep {
    uint8_t note;       /* MIDI note (0 = rest/tie) */
    bool    accent;
    bool    slide;
    bool    noteOff;    /* true = cortar la nota en este step */
};

/* Patrón acid clásico en Am */
static const AcidStep ACID_PATTERN[16] = {
    { 36, false, false, false },   /* C2  */
    {  0, false, false, false },   /* tie */
    { 36, false, false, true  },   /* C2  corto */
    { 39, true,  false, false },   /* Eb2 accent */
    { 36, false, true,  false },   /* C2  slide from Eb */
    {  0, false, false, false },   /* tie */
    { 48, true,  false, true  },   /* C3  accent corto */
    { 36, false, false, false },   /* C2  */
    { 43, false, false, false },   /* G2  */
    {  0, false, false, false },   /* tie */
    { 41, true,  true,  false },   /* F2  accent slide */
    { 36, false, true,  false },   /* C2  slide from F */
    {  0, false, false, true  },   /* rest + noteOff */
    { 36, false, false, false },   /* C2  */
    { 44, true,  false, true  },   /* Ab2 accent corto */
    { 36, false, false, false },   /* C2  */
};

/* ═══════════════════════════════════════════════════════════════════
 *  DemoSequencer — gestiona el guión completo
 * ═══════════════════════════════════════════════════════════════════ */
class DemoSequencer {
public:

    void Init(float sampleRate,
              TR808::Kit*   kit808,
              TR909::Kit*   kit909,
              TB303::Synth* synth303)
    {
        sr_       = sampleRate;
        kit808_   = kit808;
        kit909_   = kit909;
        synth303_ = synth303;
        Reset();
    }

    void Reset() {
        globalSample_ = 0;
        stepCounter_  = 0;
        acidStep_     = 0;
        nextTrigger_  = 0;
        nextAcid_     = 0;
        fadeGain_     = 1.0f;

        /* Estado inicial */
        bpm_          = 90.0f;
        swing_        = 0.56f;
        morphT_       = 0.0f;
        kickMix808_   = 1.0f;  /* 100% 808 */
        kickMix909_   = 0.0f;  /* 0% 909   */

        /* Flags de sección */
        kickOn_       = false;
        snareOn_      = false;
        hihatOn_      = false;
        acidOn_       = false;
        sweepOn_      = false;
        morphOn_      = false;
        detroitOn_    = false;
        fadeOut_       = false;

        /* AutoParams */
        autoSweep_.active = false;
        autoMorph_.active = false;
        autoFade_.active  = false;
        autoBpm_.active   = false;
        autoSwing_.active = false;
        autoKick808_.active = false;
        autoKick909_.active = false;

        /* 303 setup inicial */
        if (synth303_) {
            synth303_->SetCutoff(200.0f);
            synth303_->SetResonance(0.7f);
            synth303_->SetEnvMod(0.4f);
            synth303_->SetDecay(0.2f);
            synth303_->SetAccent(0.6f);
            synth303_->SetWaveform(TB303::WAVE_SAW);
            synth303_->volume = 0.6f;
        }

        /* Volúmenes iniciales */
        if (kit808_) {
            kit808_->kick.volume    = 0.85f;
            kit808_->snare.volume   = 0.7f;
            kit808_->hihatC.volume  = 0.45f;
            kit808_->hihatO.volume  = 0.4f;
        }
        if (kit909_) {
            kit909_->kick.volume    = 0.0f;    /* empieza mudo */
        }

        /* Calcular timestamps de sección (en samples) */
        sec_00_ = 0;
        sec_15_ = secToSamples(15.0f, sr_);
        sec_25_ = secToSamples(25.0f, sr_);
        sec_40_ = secToSamples(40.0f, sr_);
        sec_60_ = secToSamples(60.0f, sr_);
        sec_90_ = secToSamples(90.0f, sr_);
        sec_130_ = secToSamples(130.0f, sr_);
        sec_170_ = secToSamples(170.0f, sr_);
        sec_180_ = secToSamples(180.0f, sr_);

        RecalcStepLen();
    }

    /* ───────────────────────────────────────────────────────────
     *  ProcessSample — llamar 48000 veces/segundo
     *  Devuelve el gain (fade) que se aplica al mix total
     * ─────────────────────────────────────────────────────────── */
    float ProcessSample() {
        uint32_t g = globalSample_;

        /* ══════════════════════════════════════
         *  SECCIONES — activar en el momento justo
         * ══════════════════════════════════════ */

        /* 0:00 — Kick 808 */
        if (g == sec_00_) {
            kickOn_ = true;
        }

        /* 0:15 — Snare */
        if (g == sec_15_) {
            snareOn_ = true;
        }

        /* 0:25 — Hi-hats con swing */
        if (g == sec_25_) {
            hihatOn_ = true;
        }

        /* 0:40 — 303 entra, filtro cerrado */
        if (g == sec_40_) {
            acidOn_ = true;
            if (synth303_) {
                synth303_->SetCutoff(200.0f);
                synth303_->SetResonance(0.7f);
            }
        }

        /* 1:00 — Filter sweep 303 */
        if (g == sec_60_) {
            sweepOn_ = true;
            /* Cutoff: 200 → 3000 durante 30 segundos */
            autoSweep_.Set(200.0f, 3000.0f, g, secToSamples(30.0f, sr_));
        }

        /* 1:30 — MORPHING (40 segundos) */
        if (g == sec_90_) {
            morphOn_ = true;
            uint32_t morphDur = secToSamples(40.0f, sr_);
            /* BPM 90 → 145 */
            autoBpm_.Set(90.0f, 145.0f, g, morphDur);
            /* Swing 56% → 0% */
            autoSwing_.Set(0.56f, 0.0f, g, morphDur);
            /* Kick: 808 fade out, 909 fade in */
            autoKick808_.Set(1.0f, 0.0f, g, morphDur);
            autoKick909_.Set(0.0f, 1.0f, g, morphDur);
            /* 303 cutoff 200 → 4000 durante morph */
            autoMorph_.Set(200.0f, 4000.0f, g, morphDur);
        }

        /* 2:10 — Detroit completo */
        if (g == sec_130_) {
            detroitOn_ = true;
            morphOn_ = false;
            bpm_ = 145.0f;
            swing_ = 0.0f;
            kickMix808_ = 0.0f;
            kickMix909_ = 1.0f;
            if (synth303_) synth303_->SetCutoff(4000.0f);
            RecalcStepLen();
        }

        /* 2:50 — Fade out */
        if (g == sec_170_) {
            fadeOut_ = true;
            autoFade_.Set(1.0f, 0.0f, g, secToSamples(10.0f, sr_));
        }

        /* 3:00 — Reset y reiniciar */
        if (g >= sec_180_) {
            /* Silenciar 303 antes del reset */
            if (synth303_) synth303_->NoteOff();
            Reset();
            return 0.0f;
        }

        /* ══════════════════════════════════════
         *  ACTUALIZAR AutoParams
         * ══════════════════════════════════════ */
        if (sweepOn_ && autoSweep_.active) {
            float c = autoSweep_.Update(g);
            if (synth303_) synth303_->SetCutoff(c);
        }

        if (morphOn_) {
            if (autoBpm_.active) {
                bpm_ = autoBpm_.Update(g);
                RecalcStepLen();
            }
            if (autoSwing_.active) {
                swing_ = autoSwing_.Update(g);
            }
            if (autoKick808_.active) {
                kickMix808_ = autoKick808_.Update(g);
                if (kit808_) kit808_->kick.volume = 0.85f * kickMix808_;
            }
            if (autoKick909_.active) {
                kickMix909_ = autoKick909_.Update(g);
                if (kit909_) kit909_->kick.volume = 0.85f * kickMix909_;
            }
            if (autoMorph_.active) {
                float c = autoMorph_.Update(g);
                if (synth303_) synth303_->SetCutoff(c);
            }
        }

        if (fadeOut_ && autoFade_.active) {
            fadeGain_ = autoFade_.Update(g);
        }

        /* ══════════════════════════════════════
         *  SECUENCIADOR DE TRIGGERS
         *  16 steps por compás, con swing
         * ══════════════════════════════════════ */
        if (g >= nextTrigger_) {
            uint8_t step = stepCounter_ % 16;
            (void)step; /* suppress unused if no Detroit section */

            /* ── KICK (en beats 0) ── */
            if (kickOn_ && (step % 4 == 0)) {
                /* Trigger ambos kicks, el volumen controla el crossfade */
                if (kickMix808_ > 0.01f && kit808_)
                    kit808_->kick.Trigger(0.9f);
                if (kickMix909_ > 0.01f && kit909_)
                    kit909_->kick.Trigger(0.9f);
            }

            /* ── SNARE (en beats 1 y 3 = steps 4, 12) ── */
            if (snareOn_ && (step == 4 || step == 12)) {
                if (kit808_) kit808_->snare.Trigger(0.85f);
            }

            /* ── HI-HATS (cada step, alternando closed/open) ── */
            if (hihatOn_) {
                if (step % 4 == 2) {
                    /* Open hihat en offbeats */
                    if (kit808_) kit808_->hihatO.Trigger(0.6f);
                } else {
                    /* Closed en el resto */
                    if (kit808_) kit808_->hihatC.Trigger(0.55f);
                }
            }

            /* ── Detroit extras (909 snare, más hihats) ── */
            if (detroitOn_) {
                /* 909 snare en beats 1 y 3 */
                if (step == 4 || step == 12) {
                    if (kit909_) kit909_->snare.Trigger(0.8f);
                }
                /* 909 hihats rápidos */
                if (step % 2 == 0 && kit909_) {
                    kit909_->hihatC.Trigger(0.5f);
                }
                /* Clap en beat 1 */
                if (step == 4 && kit909_) {
                    kit909_->clap.Trigger(0.6f);
                }
            }

            /* Calcular próximo trigger con swing */
            stepCounter_++;
            uint32_t baseLen = stepLen16th_;
            /* Swing: steps impares se retrasan */
            if ((stepCounter_ % 2) == 1) {
                /* swingAmount: 0.5 = sin swing, >0.5 = retrasado */
                float swingMs = (swing_ - 0.5f) * 2.0f;
                if (swingMs < 0.0f) swingMs = 0.0f;
                uint32_t swingOffset = (uint32_t)((float)baseLen * swingMs);
                nextTrigger_ = g + baseLen + swingOffset;
            } else {
                nextTrigger_ = g + baseLen;
            }
        }

        /* ══════════════════════════════════════
         *  SECUENCIADOR 303 (16 steps)
         * ══════════════════════════════════════ */
        if (acidOn_ && g >= nextAcid_) {
            const AcidStep& s = ACID_PATTERN[acidStep_ % 16];

            if (synth303_) {
                if (s.noteOff) {
                    synth303_->NoteOff();
                }
                if (s.note > 0) {
                    synth303_->NoteOn(s.note, s.accent, s.slide);
                }
            }

            acidStep_++;
            /* El 303 va a la misma velocidad que los drums */
            nextAcid_ = g + stepLen16th_;
        }

        globalSample_++;
        return fadeGain_;
    }

    bool IsRunning() const { return globalSample_ < sec_180_; }

private:
    /* ── Referencias a los engines ── */
    float         sr_       = 48000.0f;
    TR808::Kit*   kit808_   = nullptr;
    TR909::Kit*   kit909_   = nullptr;
    TB303::Synth* synth303_ = nullptr;

    /* ── Timer global (en samples) ── */
    uint32_t globalSample_ = 0;

    /* ── Secuenciador ── */
    uint32_t stepCounter_  = 0;
    uint32_t acidStep_     = 0;
    uint32_t nextTrigger_  = 0;
    uint32_t nextAcid_     = 0;
    uint32_t stepLen16th_  = 0;

    /* ── Parámetros animados ── */
    float bpm_        = 90.0f;
    float swing_      = 0.56f;
    float morphT_     = 0.0f;
    float kickMix808_ = 1.0f;
    float kickMix909_ = 0.0f;
    float fadeGain_    = 1.0f;

    /* ── Flags de sección ── */
    bool kickOn_    = false;
    bool snareOn_   = false;
    bool hihatOn_   = false;
    bool acidOn_    = false;
    bool sweepOn_   = false;
    bool morphOn_   = false;
    bool detroitOn_ = false;
    bool fadeOut_    = false;

    /* ── AutoParams ── */
    AutoParam autoSweep_;     /* 303 cutoff sweep         */
    AutoParam autoMorph_;     /* 303 cutoff morph         */
    AutoParam autoFade_;      /* fade out final           */
    AutoParam autoBpm_;       /* BPM 90 → 145             */
    AutoParam autoSwing_;     /* swing 56% → 0%           */
    AutoParam autoKick808_;   /* kick 808 volume fade     */
    AutoParam autoKick909_;   /* kick 909 volume fade     */

    /* ── Timestamps de sección (en samples) ── */
    uint32_t sec_00_  = 0;
    uint32_t sec_15_  = 0;
    uint32_t sec_25_  = 0;
    uint32_t sec_40_  = 0;
    uint32_t sec_60_  = 0;
    uint32_t sec_90_  = 0;   /* 1:30 */
    uint32_t sec_130_ = 0;   /* 2:10 */
    uint32_t sec_170_ = 0;   /* 2:50 */
    uint32_t sec_180_ = 0;   /* 3:00 */

    /* ── Recalcular duración de 1/16th note ── */
    void RecalcStepLen() {
        /* BPM → duración de 1 beat = 60/BPM segundos
         * 1/16th note = 1 beat / 4 */
        float beatSec = 60.0f / bpm_;
        float stepSec = beatSec / 4.0f;
        stepLen16th_ = (uint32_t)(stepSec * sr_);
        if (stepLen16th_ < 1) stepLen16th_ = 1;
    }
};

} /* namespace Demo */
