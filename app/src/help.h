/* v0.46: the HELP pages and the terms of use (help.c). */
#ifndef PRISM_APP_HELP_H
#define PRISM_APP_HELP_H

#define TERMS_VER	1	/* raise it when TERMS.md changes: every console is asked again */

void help_open(void);
void help_draw(void);
int  help_pad(unsigned pressed);	/* 1 = back to SETTINGS */

int  terms_accepted(void);		/* 1 = this console agreed to TERMS_VER or later */
void terms_accept(void);
void terms_draw(int armed);
#endif
