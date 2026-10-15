// Sounds and playback. Audio layers point at sounds in a bank that lives for the whole session, so undo
// snapshots share them; documents embed the original files. Playback mixes every audible audio layer
// through sokol_audio (WASAPI) and, while it runs, its clock drives the frame, so lip sync doesn't drift.
#include "doc/doc.h"
#include "audio/audio.h"
#include "sokol/sokol_audio.h"
#include "sokol/sokol_log.h"
#include <stdatomic.h>

int audio_decode(const unsigned char *data, unsigned size, float **out, int *rate);

/* ── the bank ── */

static Sound *BANK[256];
static int NB, NEXT_ID = 1;

int sound_add(const char *name, const u8 *data, u32 size, char *err, int cap) {
    if (NB >= 256) { snprintf(err, (size_t)cap, "Too many sounds in this session"); return 0; }
    float *pcm; int rate;
    int frames = audio_decode(data, size, &pcm, &rate);
    if (!frames) { snprintf(err, (size_t)cap, "Vertexa reads WAV, MP3 and OGG sound files"); return 0; }
    Sound *s = calloc(1, sizeof(Sound));
    s->id = NEXT_ID++;
    snprintf(s->name, sizeof(s->name), "%s", name);
    s->data = malloc(size); memcpy(s->data, data, size); s->size = size;
    s->pcm = pcm; s->nframes = frames; s->rate = rate;
    // waveform: the loudest sample of every SOUND_PEAK frames
    s->npeaks = frames / SOUND_PEAK + 1;
    s->peaks = calloc((size_t)s->npeaks, sizeof(float));
    for (int i = 0; i < frames; i++) {
        float a = MAX(fabsf(pcm[2 * i]), fabsf(pcm[2 * i + 1]));
        float *pk = &s->peaks[i / SOUND_PEAK];
        if (a > *pk) *pk = a;
    }
    BANK[NB++] = s;
    return s->id;
}

Sound *sound_get(int id) {
    for (int i = 0; i < NB; i++) if (BANK[i]->id == id) return BANK[i];
    return NULL;
}

float sound_peak(const Sound *s, double t0, double t1) {   // loudest between two times (seconds)
    int a = (int)(t0 * s->rate) / SOUND_PEAK, b = (int)(t1 * s->rate) / SOUND_PEAK;
    a = MAX(a, 0); b = MIN(b, s->npeaks - 1);
    float m = 0;
    for (int i = a; i <= b; i++) m = MAX(m, s->peaks[i]);
    return m;
}

/* ── playback ── */

typedef struct { const Sound *s; double start; } Voice;   // start: timeline seconds where the sound begins

static struct {
    bool ready;
    int rate;
    Voice v[32]; int nv;
    double t0;                 // timeline seconds at the first frame sent
    atomic_llong sent;         // frames sent to the device since t0
    atomic_bool on;
} MX;

static void stream(float *buf, int frames, int channels) {
    if (!atomic_load(&MX.on)) { memset(buf, 0, sizeof(float) * (size_t)frames * (size_t)channels); return; }
    long long p0 = atomic_load(&MX.sent);
    for (int i = 0; i < frames; i++) {
        double t = MX.t0 + (double)(p0 + i) / MX.rate;
        float l = 0, r = 0;
        for (int k = 0; k < MX.nv; k++) {
            const Sound *s = MX.v[k].s;
            double x = (t - MX.v[k].start) * s->rate;
            if (x < 0 || x >= s->nframes - 1) continue;
            int j = (int)x; float f = (float)(x - j);
            l += s->pcm[2 * j] + (s->pcm[2 * j + 2] - s->pcm[2 * j]) * f;
            r += s->pcm[2 * j + 1] + (s->pcm[2 * j + 3] - s->pcm[2 * j + 1]) * f;
        }
        l = clampf(l, -1, 1); r = clampf(r, -1, 1);
        if (channels == 1) buf[i] = (l + r) * 0.5f;
        else { buf[i * channels] = l; buf[i * channels + 1] = r; for (int c = 2; c < channels; c++) buf[i * channels + c] = 0; }
    }
    atomic_store(&MX.sent, p0 + frames);
}

static bool device(void) {
    if (MX.ready) return saudio_isvalid();
    MX.ready = true;
    saudio_setup(&(saudio_desc){.num_channels = 2, .stream_cb = stream, .logger.func = slog_func});
    MX.rate = saudio_sample_rate();
    return saudio_isvalid() && MX.rate > 0;
}

bool audio_play(Doc *d, int frame) {
    audio_stop();
    Timeline *t = &d->scene;
    MX.nv = 0;
    for (ptrdiff_t i = 0; i < arrlen(t->layers) && MX.nv < 32; i++) {
        Layer *l = &t->layers[i];
        if (l->type != LT_AUDIO || !l->sound || !arrlen(l->keys)) continue;
        bool hidden = l->hidden;
        for (int pid = l->parent, g = 0; pid && !hidden && g < 64; g++) {
            Layer *p = NULL;
            for (ptrdiff_t k = 0; k < arrlen(t->layers); k++) if (t->layers[k].id == pid) p = &t->layers[k];
            if (!p) break;
            hidden = p->hidden; pid = p->parent;
        }
        Sound *s = sound_get(l->sound);
        if (hidden || !s) continue;
        MX.v[MX.nv++] = (Voice){s, (l->keys[0].f - 1) / (double)d->fps};   // the sound starts on the layer's first key
    }
    if (!MX.nv || !device()) return false;
    MX.t0 = (frame - 1) / (double)d->fps;
    atomic_store(&MX.sent, 0);
    atomic_store(&MX.on, true);
    return true;
}

void audio_stop(void) { atomic_store(&MX.on, false); }
bool audio_playing(void) { return atomic_load(&MX.on); }

// timeline seconds being heard right now (what was sent, minus what still waits in the device buffer)
double audio_time(void) {
    double lag = MX.ready && saudio_isvalid() ? saudio_buffer_frames() / (double)MAX(1, MX.rate) : 0;
    return MX.t0 + MAX(0.0, atomic_load(&MX.sent) / (double)MAX(1, MX.rate) - lag);
}

void audio_shutdown(void) { if (MX.ready && saudio_isvalid()) saudio_shutdown(); }
