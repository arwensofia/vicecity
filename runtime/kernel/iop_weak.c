/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Weak default implementations of the "IOP PROVIDES" half of vc_iop.h, so the EE runtime
 * links and boots (with SIF doing nothing) before runtime/iop provides the real ones.
 * Each function logs its first call.
 */
#include "vc_iop.h"

#define VC_WEAK __attribute__((weak))
#define ONCE(msg)                                                                           \
	do {                                                                                    \
		static int warned;                                                                  \
		if (!warned) {                                                                      \
			warned = 1;                                                                     \
			vc_log("iop", "stub: %s", msg);                                                 \
		}                                                                                   \
	} while (0)

VC_WEAK int vc_iop_init(const vc_iop_config *cfg)
{
	(void)cfg;
	ONCE("vc_iop_init (no IOP implementation linked)");
	return 0;
}

VC_WEAK void vc_iop_shutdown(void) {}

VC_WEAK int vc_iop_read_disc_file(const char *path, void **data, size_t *size)
{
	(void)path;
	(void)data;
	(void)size;
	ONCE("vc_iop_read_disc_file");
	return -1;
}

VC_WEAK void vc_iop_poll(void) {}
VC_WEAK void vc_iop_vblank(void) {}

VC_WEAK uint32_t vc_iop_sif_read32(uint32_t addr)
{
	(void)addr;
	ONCE("vc_iop_sif_read32");
	return 0;
}

VC_WEAK void vc_iop_sif_write32(uint32_t addr, uint32_t value)
{
	(void)addr;
	(void)value;
	ONCE("vc_iop_sif_write32");
}

VC_WEAK uint32_t vc_iop_sif_set_dma(ps2_cpu *c, uint32_t desc, uint32_t count)
{
	(void)c;
	(void)desc;
	(void)count;
	ONCE("vc_iop_sif_set_dma");
	return 1;
}

VC_WEAK int32_t vc_iop_sif_dma_stat(ps2_cpu *c, uint32_t id)
{
	(void)c;
	(void)id;
	return -1;
}

VC_WEAK void vc_iop_sif_set_dchain(ps2_cpu *c) { (void)c; }

VC_WEAK uint32_t vc_iop_sif_set_reg(ps2_cpu *c, uint32_t reg, uint32_t value)
{
	(void)c;
	(void)reg;
	(void)value;
	ONCE("vc_iop_sif_set_reg");
	return 0;
}

VC_WEAK uint32_t vc_iop_sif_get_reg(ps2_cpu *c, uint32_t reg)
{
	(void)c;
	(void)reg;
	ONCE("vc_iop_sif_get_reg");
	return 0;
}

VC_WEAK void vc_iop_sif_stop_dma(ps2_cpu *c) { (void)c; }

VC_WEAK void vc_iop_dma_start(int channel)
{
	(void)channel;
	ONCE("vc_iop_dma_start");
}
