/* v0.72: the PC stand-in for PSL1GHT's <audio/audio.h>, for host/sfx_test.c: the port as src/sfx.c uses it.
 * The two address fields are 64-bit here (a PC address does not fit the console's 32). */
#ifndef FAKE_AUDIO_H
#define FAKE_AUDIO_H
#include <ppu-types.h>
#define AUDIO_BLOCK_SAMPLES	256
#define AUDIO_PORT_2CH		2
#define AUDIO_BLOCK_8		8
#define AUDIO_BLOCK_16		16
#define AUDIO_PORT_INITLEVEL	0x1000
typedef struct { u64 numChannels, numBlocks, attrib; float level; } audioPortParam;
typedef struct { u64 readIndex; u32 status; u64 channelCount, numBlocks; u32 portSize; u64 audioDataStart; } audioPortConfig;
s32 audioInit(void);
s32 audioQuit(void);
s32 audioPortOpen(audioPortParam *param, u32 *portNum);
s32 audioPortStart(u32 portNum);
s32 audioPortStop(u32 portNum);
s32 audioPortClose(u32 portNum);
s32 audioGetPortConfig(u32 portNum, audioPortConfig *config);
#endif
