/* v0.67: host/vmcw_regress.sh builds vmc_test.c against the card writer AS IT WAS (backup_2026-11-24_pre67),
 * which has no .psu functions; vmc_test.c names them. These stand in there and are never called by the
 * regress run (it only copies and deletes). */
#include "vmc.h"
int vmc_export_psu(card_t *c, int si, const char *path, char *why, int why_n) { (void)c; (void)si; (void)path; (void)why; (void)why_n; return -1; }
int vmc_import_psu(const char *path, card_t *to, char *why, int why_n) { (void)path; (void)to; (void)why; (void)why_n; return -1; }
int vmc_psu_name(const char *path, char *name, unsigned *files) { (void)path; (void)name; (void)files; return -1; }
