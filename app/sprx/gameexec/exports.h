#ifndef __EXPORTS_H__
#define __EXPORTS_H__

/* FNID = first 4 bytes of SHA-1(name + ps3 NID suffix), byte-reversed (psdevwiki "NID").
 * cellGameSetExitParam = 0x38579ec9 matches RPCS3's table, which checks the calculation; all three
 * are present in this firmware's libsysutil_game_exec.sprx export table (2026-11-23). */
EXPORT(cellGameExecGame, 0xca872c41);
EXPORT(cellGameSetExitParam, 0x38579ec9);
EXPORT(cellGameGetBootGameInfo, 0xf6acd0bc);

#endif
