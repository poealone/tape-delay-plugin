/**
 * Tape Delay — PocketDAW Effect Plugin Example
 * 
 * Warm stereo delay with low-pass filtering on the feedback path,
 * simulating analog tape echo characteristics.
 * 
 * Build:
 *   aarch64-none-linux-gnu-gcc -shared -fPIC -O2 -o tape-delay.so tape-delay.c -lm
 */

#include <SDL2/SDL.h>
#include "pocketdaw.h"
#include "pd_text.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#define MAX_DELAY_SAMPLES (44100 * 2)  /* 2 seconds max */
#define NUM_PARAMS 5
#define NUM_PRESETS 4

typedef struct {
    float sampleRate;
    
    /* Parameters (0.0-1.0 normalized) */
    float params[NUM_PARAMS];
    
    /* Delay buffers */
    float delayL[MAX_DELAY_SAMPLES];
    float delayR[MAX_DELAY_SAMPLES];
    int writePos;
    
    /* Low-pass filter state (feedback path) */
    float lpStateL;
    float lpStateR;
    
    int currentPreset;
} TapeDelay;

/* Parameter indices */
enum {
    P_TIME = 0,       /* 0.0=50ms, 1.0=2000ms */
    P_FEEDBACK,        /* 0.0=0%, 1.0=90% */
    P_MIX,             /* 0.0=dry, 1.0=wet */
    P_TONE,            /* 0.0=dark, 1.0=bright */
    P_STEREO_SPREAD    /* 0.0=mono, 1.0=wide */
};

static const char* param_names[NUM_PARAMS] = {
    "Time", "Feedback", "Mix", "Tone", "Spread"
};

static const float presets[NUM_PRESETS][NUM_PARAMS] = {
    /* Slapback */     { 0.08f, 0.15f, 0.35f, 0.7f, 0.3f },
    /* Tape Echo */    { 0.35f, 0.45f, 0.30f, 0.4f, 0.5f },
    /* Dub Delay */    { 0.50f, 0.65f, 0.40f, 0.25f, 0.7f },
    /* Ambient Wash */ { 0.75f, 0.55f, 0.50f, 0.3f, 0.9f }
};

static const char* preset_names[NUM_PRESETS] = {
    "Slapback", "Tape Echo", "Dub Delay", "Ambient Wash"
};

/* ── Required API ──────────────────────────────────────── */

int pdfx_api_version(void) { return PDFX_API_VERSION; }
const char* pdfx_name(void) { return "Tape Delay"; }
int pdfx_param_count(void) { return NUM_PARAMS; }

PdFxInstance pdfx_create(float sampleRate) {
    TapeDelay* td = (TapeDelay*)calloc(1, sizeof(TapeDelay));
    if (!td) return NULL;
    td->sampleRate = sampleRate;
    td->currentPreset = 0;
    /* Load default preset */
    for (int i = 0; i < NUM_PARAMS; i++)
        td->params[i] = presets[0][i];
    return td;
}

void pdfx_destroy(PdFxInstance inst) {
    free(inst);
}

void pdfx_process(PdFxInstance inst, PdFxAudio* audio) {
    TapeDelay* td = (TapeDelay*)inst;
    if (!td) return;

    /* Pick up sample rate changes from host */
    if (audio->sampleRate > 0.0f && audio->sampleRate != td->sampleRate)
        td->sampleRate = audio->sampleRate;

    /* Map parameters */
    float timeMs = 50.0f + td->params[P_TIME] * 1950.0f;      /* 50-2000ms */
    float feedback = td->params[P_FEEDBACK] * 0.9f;             /* 0-90% */
    float mix = td->params[P_MIX];
    float tone = td->params[P_TONE];
    float spread = td->params[P_STEREO_SPREAD];

    int delaySamples = (int)(timeMs * td->sampleRate / 1000.0f);
    if (delaySamples >= MAX_DELAY_SAMPLES) delaySamples = MAX_DELAY_SAMPLES - 1;
    if (delaySamples < 1) delaySamples = 1;

    /* Stereo spread: offset R channel delay */
    int delaySamplesR = delaySamples + (int)(spread * delaySamples * 0.3f);
    if (delaySamplesR >= MAX_DELAY_SAMPLES) delaySamplesR = MAX_DELAY_SAMPLES - 1;

    /* Low-pass coefficient (tone control on feedback) */
    float lpCoeff = 0.05f + tone * 0.9f;  /* dark → bright */

    float dry = 1.0f - mix * 0.5f;  /* Keep some dry signal */
    float wet = mix;

    for (int i = 0; i < audio->bufferSize; i++) {
        float inL = audio->inputL[i];
        float inR = audio->inputR[i];

        /* Read from delay lines */
        int readL = td->writePos - delaySamples;
        if (readL < 0) readL += MAX_DELAY_SAMPLES;
        int readR = td->writePos - delaySamplesR;
        if (readR < 0) readR += MAX_DELAY_SAMPLES;

        float delL = td->delayL[readL];
        float delR = td->delayR[readR];

        /* Low-pass filter on feedback (tape warmth) */
        td->lpStateL += lpCoeff * (delL - td->lpStateL);
        td->lpStateR += lpCoeff * (delR - td->lpStateR);

        /* Flush denormals in feedback path — prevents CPU stall */
        if (fabsf(td->lpStateL) < 1e-20f) td->lpStateL = 0.0f;
        if (fabsf(td->lpStateR) < 1e-20f) td->lpStateR = 0.0f;

        /* Write to delay lines (input + filtered feedback) */
        td->delayL[td->writePos] = inL + td->lpStateL * feedback;
        td->delayR[td->writePos] = inR + td->lpStateR * feedback;

        /* Soft-clip the delay buffer to prevent runaway */
        td->delayL[td->writePos] = tanhf(td->delayL[td->writePos]);
        td->delayR[td->writePos] = tanhf(td->delayR[td->writePos]);

        /* Mix output — clamp to prevent downstream clipping */
        float outL = inL * dry + delL * wet;
        float outR = inR * dry + delR * wet;
        if (outL > 1.0f) outL = 1.0f; else if (outL < -1.0f) outL = -1.0f;
        if (outR > 1.0f) outR = 1.0f; else if (outR < -1.0f) outR = -1.0f;
        audio->outputL[i] = outL;
        audio->outputR[i] = outR;

        td->writePos = (td->writePos + 1) % MAX_DELAY_SAMPLES;
    }
}

float pdfx_get_param(PdFxInstance inst, int index) {
    TapeDelay* td = (TapeDelay*)inst;
    if (!td || index < 0 || index >= NUM_PARAMS) return 0.0f;
    return td->params[index];
}

void pdfx_set_param(PdFxInstance inst, int index, float value) {
    TapeDelay* td = (TapeDelay*)inst;
    if (!td || index < 0 || index >= NUM_PARAMS) return;
    td->params[index] = value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
    td->currentPreset = -1;
}

/* ── Optional API ──────────────────────────────────────── */

const char* pdfx_param_name(int index) {
    if (index < 0 || index >= NUM_PARAMS) return NULL;
    return param_names[index];
}

void pdfx_reset(PdFxInstance inst) {
    TapeDelay* td = (TapeDelay*)inst;
    if (!td) return;
    memset(td->delayL, 0, sizeof(td->delayL));
    memset(td->delayR, 0, sizeof(td->delayR));
    td->lpStateL = 0.0f;
    td->lpStateR = 0.0f;
    td->writePos = 0;
}

int pdfx_preset_count(void) { return NUM_PRESETS; }

const char* pdfx_preset_name(int index) {
    if (index < 0 || index >= NUM_PRESETS) return NULL;
    return preset_names[index];
}

void pdfx_load_preset(PdFxInstance inst, int index) {
    TapeDelay* td = (TapeDelay*)inst;
    if (!td || index < 0 || index >= NUM_PRESETS) return;
    for (int i = 0; i < NUM_PARAMS; i++)
        td->params[i] = presets[index][i];
    td->currentPreset = index;
}

int pdfx_get_preset(PdFxInstance inst) {
    TapeDelay* td = (TapeDelay*)inst;
    return td ? td->currentPreset : -1;
}

/* ══════════════════════════════════════════════════════════════
 * Page 0 custom draw — tape delay hero page.
 *
 * Left side: a stack of echo "ticks" fanning out from the input —
 * each echo spaced by the time parameter, decaying by feedback.
 * Reads like a performance delay pedal.
 *
 * Right side / bottom: live input/output oscilloscope + peak meters.
 * ══════════════════════════════════════════════════════════════ */

static void td_fillRect(SDL_Renderer* r, int x, int y, int w, int h) {
    SDL_Rect rc = {x, y, w, h}; SDL_RenderFillRect(r, &rc);
}
static void td_drawRect(SDL_Renderer* r, int x, int y, int w, int h) {
    SDL_Rect rc = {x, y, w, h}; SDL_RenderDrawRect(r, &rc);
}

int pdfx_draw(PdFxInstance inst, void* renderer, PdDrawContext* ctx) {
    TapeDelay* td = (TapeDelay*)inst;
    SDL_Renderer* r = (SDL_Renderer*)renderer;
    if (!td || !r || !ctx) return 0;

    int X = ctx->x, Y = ctx->y, W = ctx->w, H = ctx->h;
    int sel = ctx->selectedParam;
    int editing = ctx->editMode;

    /* Tape delay accent — warm cyan-blue */
    const uint8_t AR = 60, AG = 180, AB = 255;

    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);

    /* Background */
    SDL_SetRenderDrawColor(r, 4, 10, 16, 255);
    td_fillRect(r, X, Y, W, H);
    if (ctx->peak > 0.01f) {
        int glowH = (int)(ctx->peak * 50);
        if (glowH > H / 3) glowH = H / 3;
        for (int g = 0; g < glowH; g++) {
            int a = (int)((1.0f - (float)g / glowH) * ctx->peak * 55);
            SDL_SetRenderDrawColor(r, AR, AG, AB, a);
            SDL_RenderDrawLine(r, X, Y + H - 1 - g, X + W, Y + H - 1 - g);
        }
    }

    /* Accent band with "TAPE DELAY" title */
    {
        int bandH = 14;
        SDL_SetRenderDrawColor(r, 4, 14, 22, 255);
        td_fillRect(r, X, Y, W, bandH);
        pdt_drawStrC(r, X + 6, Y + 4, 180, 230, 255, 255, "TAPE DELAY");
        SDL_SetRenderDrawColor(r, AR, AG, AB, 220);
        SDL_RenderDrawLine(r, X, Y + bandH, X + W - 1, Y + bandH);
        /* Tape reel dots */
        for (int i = 0; i < 2; i++) {
            int cx = X + W - 12 - i * 9;
            SDL_SetRenderDrawColor(r, AR, AG, AB, 220);
            for (int a = 0; a < 360; a += 12) {
                float ang = (float)a * 0.01745329f;
                int px = cx + (int)(cosf(ang) * 4);
                int py = Y + 6 + (int)(sinf(ang) * 4);
                SDL_RenderDrawPoint(r, px, py);
            }
        }
    }

    /* ── Echo pulse visualization (Y=18..104, 86 px, compressed from 112) ── */
    {
        int fx = X + 8, fy = Y + 18, fw = W - 16, fh = 86;
        SDL_SetRenderDrawColor(r, 4, 12, 18, 255);
        td_fillRect(r, fx, fy, fw, fh);
        SDL_SetRenderDrawColor(r, AR, AG, AB, ctx->peak > 0.01f ? 200 : 100);
        td_drawRect(r, fx, fy, fw, fh);

        float timeN = td->params[P_TIME];
        float feedback = td->params[P_FEEDBACK] * 0.9f;
        float mix = td->params[P_MIX];
        float tone = td->params[P_TONE];
        float spread = td->params[P_STEREO_SPREAD];

        /* Left channel baseline */
        int midL = fy + fh / 3;
        int midR = fy + (2 * fh) / 3;
        SDL_SetRenderDrawColor(r, AR / 6, AG / 6, AB / 6, 160);
        SDL_RenderDrawLine(r, fx + 3, midL, fx + fw - 4, midL);
        SDL_RenderDrawLine(r, fx + 3, midR, fx + fw - 4, midR);

        /* Compute up to 8 echo pulses fanning out from the left edge. Each
         * echo's X is spaced by timeN * (fw/8). Height decays by feedback^n.
         * Spread offsets the R channel echo position for a visible stereo
         * image. Tone tints the echo brightness. */
        int startX = fx + 10;
        float spacing = 6.0f + timeN * (float)(fw - 30) / 8.0f;
        float amp = 0.9f;
        for (int e = 0; e < 8; e++) {
            int exL = startX + (int)(e * spacing);
            int exR = startX + (int)(e * spacing * (1.0f + spread * 0.3f));
            if (exL >= fx + fw - 4) break;

            int lh = (int)(amp * (fh / 3 - 4));
            int rh = (int)(amp * (fh / 3 - 4));
            /* Alpha scales with amplitude AND mix */
            uint8_t la = (uint8_t)(amp * mix * 255);
            /* Tone tint — bright echoes fade toward white, dark fade toward dim */
            uint8_t lr = (uint8_t)(AR + (255 - AR) * tone * 0.5f);
            uint8_t lg = (uint8_t)(AG + (255 - AG) * tone * 0.3f);
            uint8_t lb = AB;

            SDL_SetRenderDrawColor(r, lr, lg, lb, la);
            /* Left channel pulse — vertical line above/below mid */
            SDL_RenderDrawLine(r, exL, midL - lh, exL, midL + lh);
            SDL_RenderDrawLine(r, exL + 1, midL - lh, exL + 1, midL + lh);
            /* R channel */
            if (exR < fx + fw - 4) {
                SDL_SetRenderDrawColor(r, lr, lg, lb, (uint8_t)(la * 0.8f));
                SDL_RenderDrawLine(r, exR, midR - rh, exR, midR + rh);
                SDL_RenderDrawLine(r, exR + 1, midR - rh, exR + 1, midR + rh);
            }

            /* Glow trail — dim horizontal line from pulse to next position */
            SDL_SetRenderDrawColor(r, lr, lg, lb, (uint8_t)(la * 0.25f));
            SDL_RenderDrawLine(r, exL + 2, midL, exL + (int)spacing - 2, midL);

            amp *= feedback;
            if (amp < 0.05f) break;
        }

        /* Dry-signal indicator at the very left — small solid block labelled
         * as the "dry tap". */
        {
            int dryH = (int)((1.0f - mix * 0.5f) * (fh / 3 - 4));
            SDL_SetRenderDrawColor(r, 200, 220, 240, 220);
            td_fillRect(r, fx + 3, midL - dryH / 2, 4, dryH);
            td_fillRect(r, fx + 3, midR - dryH / 2, 4, dryH);
        }

        /* Time-parameter readout — a slim bar across the top showing what
         * fraction of the 50-2000 ms window is currently selected */
        {
            int barY = fy + 4;
            int barW = fw - 8;
            int fill = (int)(timeN * (barW - 2));
            SDL_SetRenderDrawColor(r, AR / 6, AG / 6, AB / 6, 200);
            td_fillRect(r, fx + 4, barY, barW, 2);
            SDL_SetRenderDrawColor(r, AR, AG, AB, 220);
            td_fillRect(r, fx + 4, barY, fill, 2);
        }
    }

    /* ── Live oscilloscope (Y=108..134, 26 px) — post-FX output ─────── */
    {
        int ox = X + 8, oy = Y + 108, ow = W - 16, oh = 26;
        SDL_SetRenderDrawColor(r, 4, 10, 18, 255);
        td_fillRect(r, ox, oy, ow, oh);
        SDL_SetRenderDrawColor(r, AR, AG, AB, ctx->peak > 0.01f ? 200 : 80);
        td_drawRect(r, ox, oy, ow, oh);
        int mid = oy + oh / 2;
        SDL_SetRenderDrawColor(r, AR / 6, AG / 6, AB / 6, 140);
        SDL_RenderDrawLine(r, ox + 3, mid, ox + ow - 4, mid);

        if (ctx->scopeLen > 0 && ctx->scopeBufL) {
            SDL_SetRenderDrawColor(r, AR, AG, AB, 230);
            int px = ox + 2, py = mid;
            for (int i = 1; i < ow - 4; i++) {
                int si = i * ctx->scopeLen / (ow - 4);
                if (si >= ctx->scopeLen) break;
                int nx = ox + 2 + i;
                int ny = mid - (int)(ctx->scopeBufL[si] * (oh / 2 - 4));
                if (ny < oy + 2) ny = oy + 2;
                if (ny > oy + oh - 2) ny = oy + oh - 2;
                SDL_RenderDrawLine(r, px, py, nx, ny);
                px = nx; py = ny;
            }
            if (ctx->scopeBufR) {
                SDL_SetRenderDrawColor(r, 200, 240, 255, 180);
                px = ox + 2; py = mid;
                for (int i = 1; i < ow - 4; i++) {
                    int si = i * ctx->scopeLen / (ow - 4);
                    if (si >= ctx->scopeLen) break;
                    int nx = ox + 2 + i;
                    int ny = mid - (int)(ctx->scopeBufR[si] * (oh / 2 - 4));
                    if (ny < oy + 2) ny = oy + 2;
                    if (ny > oy + oh - 2) ny = oy + oh - 2;
                    SDL_RenderDrawLine(r, px, py, nx, ny);
                    px = nx; py = ny;
                }
            }
        }
    }

    /* ── Peak meters L/R (Y=138..150) ── */
    {
        int mx = X + 8, my = Y + 138, mh = 12;
        int mw = (W - 20) / 2;
        for (int ch = 0; ch < 2; ch++) {
            float pk = (ch == 0) ? ctx->peakL : ctx->peakR;
            if (pk < 0.0f) pk = 0.0f; if (pk > 1.0f) pk = 1.0f;
            int x0 = mx + ch * (mw + 4);
            SDL_SetRenderDrawColor(r, 8, 14, 22, 255);
            td_fillRect(r, x0, my, mw, mh);
            int fill = (int)(pk * (mw - 2));
            uint8_t mcR = pk > 0.7f ? 255 : 60;
            uint8_t mcG = pk < 0.7f ? 200 : (uint8_t)((1.0f - pk) * 290);
            uint8_t mcB = pk < 0.7f ? 255 : 80;
            SDL_SetRenderDrawColor(r, mcR, mcG, mcB, 255);
            td_fillRect(r, x0 + 1, my + 1, fill, mh - 2);
            SDL_SetRenderDrawColor(r, AR, AG, AB, 120);
            td_drawRect(r, x0, my, mw, mh);
        }
    }

    /* ── Interactive param strip (Y=154..224, 70 px) — 5 params ── */
    {
        int stripY = Y + 154;
        int stripH = H - 154 - 2;
        if (stripH < 30) stripH = 30;
        int pCount = 5;
        int stripPadX = 6;
        int cellW = (W - stripPadX * 2) / pCount;

        float vals[5] = {
            td->params[P_TIME], td->params[P_FEEDBACK], td->params[P_MIX],
            td->params[P_TONE], td->params[P_STEREO_SPREAD]
        };
        static const char* labels[5] = {
            "TIM", "FBK", "MIX", "TON", "SPR"
        };

        SDL_SetRenderDrawColor(r, AR / 8, AG / 8, AB / 8, 200);
        SDL_RenderDrawLine(r, X + 4, stripY, X + W - 4, stripY);

        for (int p = 0; p < pCount; p++) {
            int cx = X + stripPadX + p * cellW;
            int cy = stripY + 4;
            int cw = cellW - 1;
            int ch = stripH - 6;
            int isSel = (sel == p);
            int isEdit = isSel && editing;
            pdt_drawParamCell(r, ctx, p, cx, cy, cw, ch, labels[p], vals[p],
                              isSel, isEdit, AR, AG, AB);
        }
    }

    SDL_SetRenderDrawColor(r, AR, AG, AB, 80);
    td_drawRect(r, X, Y, W, H);
    return 1;
}
