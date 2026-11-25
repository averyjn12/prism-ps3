/* v0.72: Prism's sound for as long as it runs: one audio port and one mixing thread (sfx.c). The opening's
 * music goes through it (intro_sound.c), and so do the menu sounds (Bananafuzzbuddy, 11-25: "when you scroll the xmb
 * on the ps3 it makes a noise everytime you move to something different. we need a noise everytime you
 * move"). Every sound is computed here from sine waves: nothing recorded, no Sony sound. */
#ifndef PRISM_APP_SFX_H
#define PRISM_APP_SFX_H

enum { SFX_MOVE, SFX_OK, SFX_BACK, SFX_N };

int  sound_open(void);		/* 0 = open; negative = no sound this run (everything below then does nothing) */
void sound_close(void);		/* before Prism ends or hands the console to another system */
void sfx(int which);		/* play a menu sound over whatever else is playing */
/* a long sound (the opening): n frames of 48 kHz stereo float, kept by the caller until sound_music_done() */
void sound_music(const float *pcm, int n);
void sound_music_fade(void);	/* out in 150 ms */
int  sound_music_done(void);	/* 1 = nothing of it is playing any more: the caller may free it */

/* v0.73 (Bananafuzzbuddy, 11-25: "the ability to turn the vfx sound on or off incase people don't like it"): the
 * menu sounds only; the opening keeps its sound. Kept as a file (there = off), read by sound_open(). */
#define SFX_OFF_FILE	"/dev_hdd0/game/PRSM00001/USRDIR/menu_sounds_off.txt"
int  menu_sounds_on(void);
int  menu_sounds_set(int on);	/* 0 = saved */
#endif
