/* Prism's boot disc program (v0.29). The SETTINGS switch "Start Prism with the console" has webMAN
 * mount its copy of USRDIR/boot (a PS3 disc folder, title PRSM00002; setup.c copies it to
 * /dev_hdd0/tmp/prism_boot, as webMAN mounts nothing under /dev_hdd0/game as a disc) after the sign-in and press the disc icon
 * (USRDIR/autostart.bat): the one XMB press that has been reliable, because the XMB moves to a newly
 * inserted disc by itself. This program only asks the system to start Prism - cellGameExecGame,
 * type 2 = a game on the hard disk, the call Prism uses for the PSP launcher (launch.c game_exec) -
 * and ends; the system then starts Prism, and Prism unmounts this disc (setup.c autostart_started).
 * One line per run goes to /dev_hdd0/tmp/prism_autostart.log. */
#include <stdio.h>
#include <sysmodule/sysmodule.h>

#define SYSMODULE_GAME_EXEC	0x37

extern int cellGameExecGame(unsigned int type, const char *dir_name, unsigned int options,
	unsigned int mem_container, unsigned int exec_data, unsigned int user_data);

int main(void)
{
	int lr, r = -1;
	FILE *f;

	/* v0.30: autostart.bat presses the disc icon only if this file is not there yet (the console
	 * may have started this disc by itself at the sign-in) */
	f = fopen("/dev_hdd0/tmp/prism_boot_ran", "w");
	if (f)
		fclose(f);
	lr = sysModuleLoad(SYSMODULE_GAME_EXEC);
	if (lr == 0)
		r = cellGameExecGame(2, "PRSM00001", 0, 0xFFFFFFFFu, 0, 0);
	f = fopen("/dev_hdd0/tmp/prism_autostart.log", "a");
	if (f)
	{
		fprintf(f, "boot disc: game exec module %08x, cellGameExecGame(2, PRSM00001) %08x\n", (unsigned)lr, (unsigned)r);
		fclose(f);
	}
	return 0;
}
