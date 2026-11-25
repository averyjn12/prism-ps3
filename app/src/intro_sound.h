#ifndef PRISM_APP_INTRO_SOUND_H
#define PRISM_APP_INTRO_SOUND_H
/* v0.30: plays intro_synth's sound through the console's audio port (intro_sound.c) */
int  intro_sound_prepare(void);	/* load or compute it (before the intro); 1 loaded, 0 computed, <0 none */
int  intro_sound_play(void);	/* start it, at the intro's t = 0; 0 = playing */
void intro_sound_end(int quick);	/* quick: fade out in 150 ms. v0.72: returns at once; the port is sfx.c's and stays open */
void intro_sound_tidy(void);	/* v0.72: free the sound once it has been played out; call now and then */
#endif
