#ifndef FAKE_SYSMODULE_H
#define FAKE_SYSMODULE_H
#define SYSMODULE_AUDIO	0x11
static inline int sysModuleLoad(int id) { (void)id; return 0; }
#endif
