/*
 *  Copyright (C) 2009 Ignacio Garcia Perez <iggarpe@gmail.com>
 *  Copyright (C) 2011 Paul Cercueil <paul@crapouillou.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <stdlib.h>

#include "config.h"

#include "board.h"
#include "nand.h"
#include "serial.h"
#include "ubi.h"
#include "mmc.h"
#include "fat.h"
#include "jz.h"
#include "utils.h"

/* Time how long UBIBoot takes to do its job.
 * Uses the JZ4770 OST, so won't work on JZ4740.
 */
#define BENCHMARK 0

/* Kernel parameters list */

enum {
	/* Arguments for the kernel itself. */
	PARAM_EXEC = 0,
#if defined(USE_UBI) && defined(UBI_ROOTFS_MTDNAME)
	PARAM_UBIMTD,
#endif
	PARAM_ROOTDEV,
	PARAM_ROOTTYPE,
	PARAM_ROOTWAIT,
	PARAM_LOGO,
	PARAM_VIDEO,
	PARAM_QUIET,
	PARAM_NOCURSOR_1,
	PARAM_NOCURSOR_2,
	/* Arguments for user space (init and later). */
	PARAM_SEPARATOR,
	PARAM_HWVARIANT,
	PARAM_KERNEL_BAK,
	PARAM_ROOTFS_BAK,
#if BENCHMARK
	PARAM_BOOTBENCH,
#endif
};

#define STRINGIFY(s) #s
#define STRINGIFY_IND(s) STRINGIFY(s)

static char *kernel_params[] = {
	[PARAM_EXEC] = "linux",
#if defined(USE_UBI) && defined(UBI_ROOTFS_MTDNAME)
	[PARAM_UBIMTD] = "",
#endif
	[PARAM_ROOTDEV] = "",
	[PARAM_ROOTTYPE] = "",
	[PARAM_ROOTWAIT] = "rootwait",
	[PARAM_LOGO] = "",
	[PARAM_VIDEO] = "", //"video=DPI-1:320x240",
	[PARAM_QUIET] = "", //"loglevel=7", //"quiet",
	[PARAM_NOCURSOR_1] = "vt.global_cursor_default=0",
	[PARAM_NOCURSOR_2] = "vt.cur_default=1",
	[PARAM_SEPARATOR] = "--",
	[PARAM_HWVARIANT] = "hwvariant=" VARIANT,
	[PARAM_KERNEL_BAK] = "",
	[PARAM_ROOTFS_BAK] = "",
#if BENCHMARK
	[PARAM_BOOTBENCH] = "bootbench=0x0000000000000000",
#endif
};

static void set_alt_param(void)
{
	kernel_params[PARAM_KERNEL_BAK] = "kernel_bak";
}

static void set_alt2_param(void)
{
	kernel_params[PARAM_ROOTFS_BAK] = "rootfs_bak";
}

static void set_logo_param(int show_logo)
{
	kernel_params[PARAM_LOGO] = show_logo ? "splash" : "logo.nologo";
}

typedef void (*kernel_main)(int, char**, char**, int*) __attribute__((noreturn));

void c_main(void)
{
	void *exec_addr = NULL;
	int mmc_inited, alt_kernel;
	extern unsigned int _bss_start, _bss_end;
	unsigned int *ptr;

	/* Clear the BSS section */
	for (ptr = &_bss_start; ptr < &_bss_end; ptr++)
		*ptr = 0;

#if BENCHMARK
	/* Setup 3 MHz timer, 64-bit wrap, abrupt stop. */
	REG_OST_OSTCSR = OSTCSR_CNT_MD | OSTCSR_SD
				   | OSTCSR_EXT_EN | OSTCSR_PRESCALE4;
	__tcu_stop_counter(15);
	REG_OST_OSTCNTL = 0;
	REG_OST_OSTCNTH = 0;
	__tcu_start_counter(15);
#endif

	board_init();

	if (!ram_works()) {
		SERIAL_PUTS("SDRAM does not work!\n");
		return;
	}

	SERIAL_PUTS("UBIBoot by Paul Cercueil <paul@crapouillou.net>\n");
#ifdef BKLIGHT_ON
	light(1);
#endif

#ifdef STAGE1_ONLY
	return;
#endif

	alt_kernel = alt_key_pressed();

	/* Tests on JZ4770 show that the data cache lines that contain the boot
	 * loader are not marked as dirty initially. Therefore, if those cache
	 * lines are evicted, the data is lost. To avoid that, we load to the
	 * uncached kseg1 virtual address region, so we never trigger a cache
	 * miss and therefore cause no evictions.
	 */

	int id = MMC_ID;
	mmc_inited = !mmc_init(MMC_ID);
	char *rootfs_dev = "root=/dev/mmcblk0p1";

#ifdef MMC_ID2
	if (!mmc_inited) {
		rootfs_dev = "root=/dev/mmcblk1p1";
		id = MMC_ID2;
		mmc_inited = !mmc_init(MMC_ID2);
	}
#endif

	if (mmc_inited) {
		if (mmc_load_kernel(
				id, (void *) (KSEG1 + LD_ADDR), alt_kernel,
				&exec_addr) == 1)
			set_alt_param();

		if (exec_addr) {
			kernel_params[PARAM_ROOTDEV] = rootfs_dev;
			kernel_params[PARAM_ROOTTYPE] = "rootfstype=vfat";
		}
	}

	if (!mmc_inited || !exec_addr) {
		SERIAL_PUTS("Unable to boot from SD."
#ifdef USE_NAND
					" Falling back to NAND."
#endif
					"\n");
#ifndef USE_NAND
		return;
#endif
	}

#ifdef USE_NAND
	if (!exec_addr) {
		nand_init();
#ifdef USE_UBI
		if (ubi_load_kernel((void *) (KSEG1 + LD_ADDR),
				    &exec_addr, alt_kernel)) {
			SERIAL_PUTS("Unable to boot from NAND.\n");
			return;
		} else {
			if (alt_kernel)
				set_alt_param();
#ifdef UBI_ROOTFS_MTDNAME
			kernel_params[PARAM_UBIMTD] = "ubi.mtd=" UBI_ROOTFS_MTDNAME;
#endif
			kernel_params[PARAM_ROOTDEV] = "root=ubi0:" UBI_ROOTFS_VOLUME;
			kernel_params[PARAM_ROOTTYPE] = "rootfstype=ubifs";
		}
#else /* USE_UBI */
#warning UBI is currently the only supported NAND file system and it was not selected.
#endif /* USE_UBI */
	}
#endif /* USE_NAND */

#if BENCHMARK
	/* Stop timer. */
	__tcu_stop_counter(15);
	/* Store timer count in kernel command line. */
	write_hex_digits(REG_OST_OSTCNTL,
			&kernel_params[PARAM_BOOTBENCH][27]);
	write_hex_digits(REG_OST_OSTCNTH_BUF,
			&kernel_params[PARAM_BOOTBENCH][27 - 8]);
#endif

	if (alt2_key_pressed())
		set_alt2_param();

	set_logo_param(!alt3_key_pressed());

	/* Since we load to kseg1, there is no data we want to keep in the cache,
	 * so no need to flush it to RAM.
	jz_flush_dcache();
	jz_flush_icache();
	*/

	SERIAL_PUTS("Kernel loaded. Executing...\n\n");

	/* Boot the kernel */
	((kernel_main) exec_addr) (
			ARRAY_SIZE(kernel_params), kernel_params, NULL, NULL );
}
