// WAV, MP3 and Ogg Vorbis decoders (single-file libraries, public domain / MIT-0), kept in their own
// translation unit: stb_vorbis defines short macro names.
#define DR_WAV_IMPLEMENTATION
#define DR_WAV_NO_STDIO
#include "audio/dr_wav.h"
#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#include "audio/dr_mp3.h"
#define STB_VORBIS_NO_STDIO
#include "audio/stb_vorbis.c"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

// decodes a whole file into interleaved stereo float frames; returns the frame count (0: not audio we read)
int audio_decode(const unsigned char *data, unsigned size, float **out, int *rate) {
    *out = NULL; *rate = 0;
    float *pcm = NULL; int channels = 0; unsigned long long frames = 0;
    if (size >= 12 && !memcmp(data, "RIFF", 4) && !memcmp(data + 8, "WAVE", 4)) {
        unsigned int ch, sr; drwav_uint64 n;
        pcm = drwav_open_memory_and_read_pcm_frames_f32(data, size, &ch, &sr, &n, NULL);
        channels = (int)ch; *rate = (int)sr; frames = n;
    } else if (size >= 4 && !memcmp(data, "OggS", 4)) {
        int ch, sr; short *s16 = NULL;
        int n = stb_vorbis_decode_memory(data, (int)size, &ch, &sr, &s16);
        if (n > 0 && s16) {
            pcm = (float *)malloc(sizeof(float) * (size_t)n * (size_t)ch);
            for (long long i = 0; i < (long long)n * ch; i++) pcm[i] = s16[i] / 32768.0f;
            channels = ch; *rate = sr; frames = (unsigned long long)n;
        }
        free(s16);
    } else {
        drmp3_config cfg; drmp3_uint64 n;
        pcm = drmp3_open_memory_and_read_pcm_frames_f32(data, size, &cfg, &n, NULL);
        channels = (int)cfg.channels; *rate = (int)cfg.sampleRate; frames = n;
    }
    if (!pcm || !frames || channels < 1 || *rate <= 0) { if (pcm) free(pcm); return 0; }
    float *st = (float *)malloc(sizeof(float) * 2 * (size_t)frames);
    for (unsigned long long i = 0; i < frames; i++) {
        float l = pcm[i * channels], r = channels > 1 ? pcm[i * channels + 1] : l;
        st[2 * i] = l; st[2 * i + 1] = r;
    }
    free(pcm);   // the dr_libs allocate with malloc by default, as stb_vorbis does
    *out = st;
    return (int)frames;
}
