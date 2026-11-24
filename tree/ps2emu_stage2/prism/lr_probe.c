/* LR-G1: read-only census. Nothing in this file stores to a device register. */
#include <ps2emu/ps2emu.h>
#include <ps2emu/libc.h>
#include <lv1/lv1.h>
#include "lr.h"

#define MAX_PORTS	8

#if defined(PS2HWEMU)
/* ps2_emu: three code words of the binary this build was made for. Its USB pages sit at fixed
 * EAs (HW_EA_*), so there is no table of addresses to compare; lr_usb_mapped() is the gate. */
int lr_sig_check(void)
{
	return *(u32 *)(u64)GX_SIG1_ADDR == GX_SIG1_WORD && *(u32 *)(u64)GX_SIG2_ADDR == GX_SIG2_WORD &&
		*(u32 *)(u64)HW_SIG3_ADDR == HW_SIG3_WORD;
}

/* The four pages are mapped by one routine (0x01D2E0) before the EHCI init, which reads
 * 0x20023000, sets the op base: so op base != 0 means all four EAs are live. */
int lr_usb_mapped(void)
{
	return HW_LPAR_OHCI(0) && HW_LPAR_EHCI(0) && HW_LPAR_OHCI(1) && HW_LPAR_EHCI(1) &&
		*(u64 *)(u64)GX_EHCI_OPBASE_PTR != 0;
}
#define EA_EHCI(i)	HW_EA_EHCI(i)
#define EA_OHCI(i)	HW_EA_OHCI(i)
#else
int lr_sig_check(void)
{
	lr_gx_usbdev *d = LR_DEVTAB;

	if (*(u32 *)(u64)GX_SIG1_ADDR != GX_SIG1_WORD || *(u32 *)(u64)GX_SIG2_ADDR != GX_SIG2_WORD)
		return 0;
	if (d[0].ea_ohci != 0x60000000 || d[0].ea_ehci != 0x60200000)
		return 0;
	if (d[1].ea_ohci != 0x60100000 || d[1].ea_ehci != 0x60300000)
		return 0;
	if (d[0].n_dma > 8 || d[1].n_dma > 8)
		return 0;

	return 1;
}

int lr_usb_mapped(void)
{
	lr_gx_usbdev *d = LR_DEVTAB;

	return d[0].lpar_ohci && d[0].lpar_ehci && d[1].lpar_ohci && d[1].lpar_ehci;
}
#endif

int lr_emu_usb_started(void)
{
	return lr_usb_mapped() && *(u64 *)(u64)GX_EHCI_OPBASE_PTR != 0;
}

/* -------------------------------------------------------------- repository */

static s64 repo(u64 n1, u64 n2, u64 n3, u64 n4, u64 *v)
{
	u64 v2;

	return lv1_get_repository_node_value(PS3_LPAR_ID_PME, n1, n2, n3, n4, v, &v2);
}

void lr_repo_dump(void)
{
	u64 b, d;

	for (b = 0; b < 10; b++)
	{
		u64 type = 0, id = 0, ndev = 0;

		if (repo(FIELD_FIRST("bus", b), FIELD("type", 0), 0, 0, &type))
			continue;
		repo(FIELD_FIRST("bus", b), FIELD("id", 0), 0, 0, &id);
		repo(FIELD_FIRST("bus", b), FIELD("num_dev", 0), 0, 0, &ndev);
		lr_log("repo bus#%d id %d type %d ndev %d\n", (int)b, (int)id, (int)type, (int)ndev);

		for (d = 0; d < ndev && d < 16; d++)
		{
			u64 did = 99, dtype = 99, intr = 99, nreg = 0;

			repo(FIELD_FIRST("bus", b), FIELD("dev", d), FIELD("id", 0), 0, &did);
			repo(FIELD_FIRST("bus", b), FIELD("dev", d), FIELD("type", 0), 0, &dtype);
			repo(FIELD_FIRST("bus", b), FIELD("dev", d), FIELD("intr", 0), 0, &intr);
			repo(FIELD_FIRST("bus", b), FIELD("dev", d), FIELD("n_regs", 0), 0, &nreg);
			lr_log(" dev#%d id %d type %d intr %d nreg %d\n", (int)d, (int)did, (int)dtype, (int)intr, (int)nreg);
		}
	}
}

/* ----------------------------------------------------------------- dumps */

static void dump_ehci(int n, u64 cap)
{
	u32 w0 = lr_rd(cap, 0);
	u32 hcs = lr_rd(cap, 4);
	u64 op = cap + (w0 & 0xff);
	int np = hcs & 0xf, i;

	lr_log("E%d cap %08x hcs %08x hcc %08x | cmd %08x sts %08x intr %08x frx %08x\n", n, w0, hcs,
		lr_rd(cap, 8), lr_rd(op, 0x00), lr_rd(op, 0x04), lr_rd(op, 0x08), lr_rd(op, 0x0c));
	lr_log("E%d seg %08x per %08x async %08x cfg %08x\n", n, lr_rd(op, 0x10), lr_rd(op, 0x14),
		lr_rd(op, 0x18), lr_rd(op, 0x40));

	if (np > MAX_PORTS)
		np = MAX_PORTS;
	for (i = 0; i < np; i++)
		lr_log("E%d p%d %08x\n", n, i + 1, lr_rd(op, 0x44 + 4 * i));
}

static void dump_ohci(int n, u64 base)
{
	u32 rha = lr_rd(base, 0x48);
	int np = rha & 0xff, i;

	lr_log("O%d rev %08x ctl %08x cs %08x ist %08x ien %08x hcca %08x\n", n, lr_rd(base, 0x00),
		lr_rd(base, 0x04), lr_rd(base, 0x08), lr_rd(base, 0x0c), lr_rd(base, 0x10), lr_rd(base, 0x18));
	lr_log("O%d chd %08x bhd %08x fmi %08x rha %08x rhb %08x rhs %08x\n", n, lr_rd(base, 0x20),
		lr_rd(base, 0x28), lr_rd(base, 0x34), rha, lr_rd(base, 0x4c), lr_rd(base, 0x50));

	if (np > MAX_PORTS)
		np = MAX_PORTS;
	for (i = 0; i < np; i++)
		lr_log("O%d p%d %08x\n", n, i + 1, lr_rd(base, 0x54 + 4 * i));
}

#if defined(PS2HWEMU)
void lr_snapshot(const char *tag)
{
	int i;

	lr_log("== snap %s tb %lx\n", tag, (unsigned long)lr_tb());
	lr_log(LR_EMU_NAME " ehci cap %lx op %lx nports %d\n", (unsigned long)*(u64 *)(u64)GX_EHCI_CAPBASE_PTR,
		(unsigned long)*(u64 *)(u64)GX_EHCI_OPBASE_PTR, *(u8 *)(u64)GX_EHCI_NPORTS);
	for (i = 0; i < 2; i++)
		lr_log("uhc%d lpar %lx/%lx\n", i, (unsigned long)HW_LPAR_OHCI(i), (unsigned long)HW_LPAR_EHCI(i));

	if (!lr_usb_mapped())
	{
		lr_log("mmio not mapped yet\n");
		return;
	}

	for (i = 0; i < 2; i++)
	{
		dump_ohci(i, EA_OHCI(i));
		dump_ehci(i, EA_EHCI(i));
	}
}
#else
void lr_snapshot(const char *tag)
{
	lr_gx_usbdev *d = LR_DEVTAB;
	int i, j;

	lr_log("== snap %s tb %lx\n", tag, (unsigned long)lr_tb());
	lr_log("gx ehci cap %lx op %lx nports %d\n", (unsigned long)*(u64 *)(u64)GX_EHCI_CAPBASE_PTR,
		(unsigned long)*(u64 *)(u64)GX_EHCI_OPBASE_PTR, *(u8 *)(u64)GX_EHCI_NPORTS);

	for (i = 0; i < 2; i++)
	{
		lr_gx_dma *m = (lr_gx_dma *)d[i].dma_desc;

		lr_log("dev%d virq %d/%d lpar %lx/%lx outlet %lx/%lx\n", i, (int)d[i].virq_ohci, (int)d[i].virq_ehci,
			(unsigned long)d[i].lpar_ohci, (unsigned long)d[i].lpar_ehci,
			(unsigned long)d[i].outlet_ohci, (unsigned long)d[i].outlet_ehci);

		for (j = 0; j < (int)d[i].n_dma; j++)
			lr_log(" dma%d size %lx fl %d ty %d bus %lx lpar %lx\n", j, (unsigned long)m[j].size, (int)m[j].flags,
				(int)m[j].type, (unsigned long)m[j].bus, (unsigned long)m[j].lpar);
	}

	if (!lr_usb_mapped())
	{
		lr_log("mmio not mapped yet\n");
		return;
	}

	for (i = 0; i < 2; i++)
	{
		dump_ohci(i, d[i].ea_ohci);
		dump_ehci(i, d[i].ea_ehci);
	}
}
#endif

/* --------------------------------------------- cheap change detection */

static int port_words(u32 *w, int max)
{
#if defined(PS2HWEMU)
	int n = 0, i, j;

	for (i = 0; i < 2; i++)
	{
		u64 cap = EA_EHCI(i), ohci = EA_OHCI(i);
#else
	lr_gx_usbdev *d = LR_DEVTAB;
	int n = 0, i, j;

	for (i = 0; i < 2; i++)
	{
		u64 cap = d[i].ea_ehci;
#endif
		u64 op = cap + (lr_rd(cap, 0) & 0xff);
		int np = lr_rd(cap, 4) & 0xf;

		if (np > MAX_PORTS)
			np = MAX_PORTS;
		if (n < max)
			w[n++] = lr_rd(op, 0x00) & 0x31;	/* run / periodic / async */
		if (n < max)
			w[n++] = lr_rd(op, 0x40);
		for (j = 0; j < np && n < max; j++)
			w[n++] = lr_rd(op, 0x44 + 4 * j) & 0x3d05;	/* ccs pe reset line pp owner */

#if defined(PS2HWEMU)
		np = lr_rd(ohci, 0x48) & 0xff;
		if (np > MAX_PORTS)
			np = MAX_PORTS;
		if (n < max)
			w[n++] = lr_rd(ohci, 0x04) & 0xff;
		for (j = 0; j < np && n < max; j++)
			w[n++] = lr_rd(ohci, 0x54 + 4 * j) & 0x303;	/* ccs pes pps lsda */
#else
		np = lr_rd(d[i].ea_ohci, 0x48) & 0xff;
		if (np > MAX_PORTS)
			np = MAX_PORTS;
		if (n < max)
			w[n++] = lr_rd(d[i].ea_ohci, 0x04) & 0xff;
		for (j = 0; j < np && n < max; j++)
			w[n++] = lr_rd(d[i].ea_ohci, 0x54 + 4 * j) & 0x303;	/* ccs pes pps lsda */
#endif
	}

	return n;
}

u32 lr_port_sig(void)
{
	u32 w[48], s = 0x9e3779b9;
	int n, i;

	if (!lr_usb_mapped())
		return 0;

	n = port_words(w, 48);
	for (i = 0; i < n; i++)
		s = (s ^ w[i]) * 0x01000193;

	return s | 1;
}

void lr_port_line(const char *tag)
{
	u32 w[48];
	char line[190];
	int n, i, l = 0;

	if (!lr_usb_mapped())
		return;

	n = port_words(w, 48);
	for (i = 0; i < n && l < 178; i++)
	{
		int k;

		for (k = 28; k >= 0; k -= 4)	/* all 32 bits: the change bits are in the top half */
		{
			int dgt = (w[i] >> k) & 0xf;

			line[l++] = (char)(dgt < 10 ? '0' + dgt : 'a' + dgt - 10);
		}
		line[l++] = ' ';
	}
	line[l] = 0;
	lr_log("chg %s tb %lx: %s\n", tag, (unsigned long)lr_tb(), line);
}
