#ifndef PRISM_APP_LOGCOPY_H
#define PRISM_APP_LOGCOPY_H
/* v0.81: every Prism log in /dev_hdd0/tmp (and onxmb.bat) to PRISM_LOGS on the first USB drive. Writes a one-line
 * result for the screen into msg. Returns the files copied, or negative: -1 no USB drive, -2 no folder made. */
int logs_to_usb(char *msg, int size);
#endif
