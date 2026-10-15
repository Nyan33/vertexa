// Audio playback for audio layers (the sound bank itself is declared in doc/doc.h).
#pragma once
#include "doc/doc.h"

bool audio_play(Doc *d, int frame);   // starts every audible audio layer at that frame; false: nothing to hear
void audio_stop(void);
bool audio_playing(void);
double audio_time(void);              // timeline seconds being heard
float sound_peak(const Sound *s, double t0, double t1);
void audio_shutdown(void);
