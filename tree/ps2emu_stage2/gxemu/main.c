#include <ps2emu/ps2emu.h>
#include <ps2emu/libc.h>
#include <ps2emu/io.h>
#include <ps2emu/cdvd.h>
#include <ps2emu/memory.h>
#include <ps2emu/vuart.h>
#include <ps2emu/patch.h>
#include <ps2emu/symbols.h>

#include "common.h"

/* PRISM-WIRING-4 */
#ifdef PRISM_LR
#include "lr.h"
#endif

#if defined(FIRMWARE_CEX)
	#define AUTH_OFFSET			0x90120
	#define DATA_TOC_OFFSET		-0x5B00
	#define	LPAR_TOC_OFFSET		-0x5BC8
	#define FILESZ_TOC_OFFSET	-0xE68
#endif

typedef struct
{
	uint32_t address;
	uint32_t data;
	uint8_t *condition;
} Patch;

static uint8_t condition_true = 1;
static uint8_t condition_iso = 0;
static uint8_t condition_optical = 0;

static Patch patches[] =
{
	{ ps2_disc_auth_caller_symbol, LI(3, 0), &condition_true },
	{ ps2_disc_auth_caller_symbol + 4, BLR, &condition_true },
};

#define N_PATCHES	(sizeof(patches) / sizeof(Patch))

static INLINE void *get_data_buffer(void)
{
	return (void *) *(uint64_t *)(*(uint64_t *)((TOC + DATA_TOC_OFFSET)));
}

static INLINE uint8_t *get_buffer_from_lpar(uint64_t lpar)
{
	uint64_t lpar1 = *(uint64_t *)(*(uint64_t *)((TOC + LPAR_TOC_OFFSET)));
	uint64_t lpar2 = lpar1 + 0x1000;
	uint64_t lpar3 = lpar1 + 0x400;
	
	uint8_t *out1 = get_data_buffer();
	uint8_t *out2 = out1 + 0x1000;
	uint8_t *out3 = out1 + 0x400;
	
	if (lpar == lpar1)	
		return out1;	
	else if (lpar == lpar2)	
		return out2;	
	else if (lpar == lpar3)	
		return out3;	
	
	DPRINTF("Warning: cannot find buffer for lpar: 0x%lx, using default logic!!!\n", lpar);
	dump_stack_trace2(16);
	return (lpar-lpar1) + out1;
}

static INLINE uint64_t get_file_size(int fd)
{
	uint64_t *structure = (uint64_t *) ((0x1270 * fd) + *(uint64_t *)(TOC + FILESZ_TOC_OFFSET));
	return structure[0x178 / 8];
}

#include "common.c"

PS2EMU_HOOKED_FUNCTION_COND_POSTCALL_3(int, emu_cdvd_send_atapi_command, (uint8_t *cmd, uint64_t outlpar, uint64_t outlen))
{
#ifdef PRISM_LR
	lr_atapi_op(cmd[0]);
	lr_poll();
#endif
	int ret = DO_POSTCALL;
	uint8_t *out = get_buffer_from_lpar(outlpar);
	
	if (!out)
		return ret;
	
#if defined(PRISM_LR) && PRISM_LR >= 5
	if (lr_event(cmd, out))
		return 0;	/* a disc change in progress: answered by lr_card.c */
#endif
	int tri = process_scsi_cmd_iso(cmd, out, outlen);
	
	if (tri < 0)	
		ret = tri;	
	else if (tri > 0)	
		ret = 0;	
	
	return ret;	
}

PS2EMU_HOOKED_FUNCTION_COND_POSTCALL_1(int, emu_cdvd_send_device_command, (uint64_t cmd))
{
#ifdef PRISM_LR
	lr_atapi_op(0x100);	/* the emulator asks for the disc's size */
#endif
	if (cmd != STORAGE_COMMAND_GET_DEVICE_SIZE)
	{
		DPRINTF("Unexpected command: %lx\n", cmd);
		dump_stack_trace2(16);
		return DO_POSTCALL;
	}
	
	uint32_t *psize = get_data_buffer();	
	*psize = iso_size_sectors - 1;

	return 0;
}

static f_desc_t orig_send_atapi_command;

PS2EMU_HOOKED_FUNCTION(int, optical_cdvd_send_atapi_command, (uint8_t *cmd, uint64_t outlpar, uint64_t outlen))
{
	ScsiRead2064 read2064;
	uint8_t *out = get_buffer_from_lpar(outlpar);
	int is_2064 = (cmd[0] == SCSI_CMD_READ_2064);
	
	if (!out)
		return 0;
	
	if (is_2064 && device_type == DEVICE_TYPE_DVD)
	{
		ScsiRead10 *read10 = (ScsiRead10 *)cmd;
		
		memcpy(&read2064, cmd, sizeof(ScsiRead2064));		
		memset(read10, 0, sizeof(ScsiRead10));
		
		read10->opcode = SCSI_CMD_READ_10;
		read10->lba = read2064.lba;
		read10->length = read2064.length;
		read10->control = read2064.control;
	}
	
	void (* func)() = (void *)&orig_send_atapi_command;	
	func(cmd, outlpar, outlen);
	
	if (is_2064 && device_type == DEVICE_TYPE_DVD)	
		memcpy(cmd, &read2064, sizeof(ScsiRead2064));	
	
	return post_process_scsi_cmd_optical(cmd, out, outlen);	
}

#ifdef DEBUG

PS2EMU_HOOKED_FUNCTION_POSTCALL_4(void, ps2_disc_auth_hook, (uint64_t r3, uint64_t r4, uint64_t r5, uint64_t r6))
{
	DPRINTF("\n---------- ps2_disc_auth %lx %lx %lx %lx\n", r3, r4, r5, r6);
}

static INLINE void apply_debug_patches(void)
{			
	hook_function_with_postcall(ps2_disc_auth_symbol, ps2_disc_auth_hook, 4);
	//hook_function_with_precall(cdvd_send_atapi_command_symbol, hook_cdvd_send_atapi_command, 3);	
}

#endif

static INLINE void apply_patches(void)
{
	if (condition_iso)
	{
		hook_function_with_cond_postcall(cdvd_send_atapi_command_symbol, emu_cdvd_send_atapi_command, 3);
		hook_function_with_cond_postcall(cdvd_send_device_command_symbol, emu_cdvd_send_device_command, 1);			
	}
	else if (condition_optical)	
		hook_function(cdvd_send_atapi_command_symbol, optical_cdvd_send_atapi_command, &orig_send_atapi_command);
	
	for (int i = 0; i < N_PATCHES; i++)
	{
		if (*patches[i].condition)
		{		
			uint32_t *addr = (uint32_t *)(uint64_t)patches[i].address;
			*addr = patches[i].data;
			clear_icache(addr, 4);			
		}		
	}	
}

#if defined(PRISM_LR) && PRISM_LR >= 5
static ScsiTrackDescriptor lr_track;

/* Point Cobra's ISO state at the file on the card (the carrier ISO stays open, unused).
 * Called from main() and, in menu mode, from lr_iso_read() while the emulator is running. */
void lr_take_card(void)
{
	uint64_t size;
	int cd, raw;

	int r = lr_iso_take(&size, &cd, &raw);

	if (r == 2)
	{
		/* a launcher title: it keeps running from the hard disk, but says it is a full disc */
		iso_size_sectors = LR_MENU_SECTORS;
		layer0_size = iso_size_sectors;
		return;
	}
	if (!r)
		return;

	iso_size = size;
	base_offset = 0;
	device_type = cd ? DEVICE_TYPE_PS2_CD : DEVICE_TYPE_PS2_DVD;
	is_dual_layer = 0;
	num_tracks = 0;
	tracks = NULL;
	iso_size_sectors = size / 2048;

	if (raw)
	{
		/* 2352-byte sectors: one data track, the layout Cobra gives a single-track image */
		lr_track.adr_control = 0x14;
		lr_track.track_number = 1;
		lr_track.track_start_addr = 0;
		tracks = &lr_track;
		num_tracks = 1;
		iso_size_sectors = size / 2352;
	}

	layer0_size = iso_size_sectors;

	if (device_type == DEVICE_TYPE_PS2_DVD)
		check_double_layer();
}
#endif

int main(void)
{
#ifdef DEBUG
	debug_init();
	DPRINTF("Hello from ps2gxemu_stage2..\n");	
#endif
	
	int ret = setup_iso();
	
	if (ret == -2)	
		condition_optical = 1;	
	else if (ret == 0)	
		condition_iso = 1;	
	
#ifdef PRISM_LR
#if PRISM_LR >= 5
	if (condition_iso)
		lr_carrier(iso_fd);
#endif
	lr_init();
#if PRISM_LR >= 5
	if (condition_iso)
		lr_take_card();
#endif
#endif

	apply_patches();	
	
#ifdef DEBUG
	apply_debug_patches();
#endif	

	return 0;
}
