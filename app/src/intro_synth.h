#ifndef PRISM_APP_INTRO_SYNTH_H
#define PRISM_APP_INTRO_SYNTH_H
/* v0.30: the opening's sound, computed (intro_synth.c). Stereo, interleaved L R floats. */
int  intro_synth_frames(int rate);			/* how many frames intro_synth writes */
void intro_synth(float *out, int frames, int rate);	/* out: 2 * frames floats */
#endif
