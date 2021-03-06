/*
 * MCA implementation for AMD Family10 CPUs
 * Copyright (c) 2007 Advanced Micro Devices, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */


/* K8 common MCA documentation published at
 *
 * AMD64 Architecture Programmer's Manual Volume 2:
 * System Programming
 * Publication # 24593 Revision: 3.12
 * Issue Date: September 2006
 */

/* Family10 MCA documentation published at
 *
 * BIOS and Kernel Developer's Guide
 * For AMD Family 10h Processors
 * Publication # 31116 Revision: 1.08
 * Isse Date: June 10, 2007
 */


#include <xen/init.h>
#include <xen/types.h>
#include <xen/kernel.h>
#include <xen/config.h>
#include <xen/smp.h>

#include <asm/processor.h>
#include <asm/system.h>
#include <asm/msr.h>

#include "mce.h"
#include "x86_mca.h"


static enum mca_extinfo
amd_f10_handler(struct mc_info *mi, uint16_t bank, uint64_t status)
{
	struct mcinfo_extended mc_ext;

	/* Family 0x10 introduced additional MSR that belong to the
	 * northbridge bank (4). */
	if (mi == NULL || bank != 4)
		return MCA_EXTINFO_IGNORED;

	if (!(status & MCi_STATUS_VAL))
		return MCA_EXTINFO_IGNORED;

	if (!(status & MCi_STATUS_MISCV))
		return MCA_EXTINFO_IGNORED;

	memset(&mc_ext, 0, sizeof(mc_ext));
	mc_ext.common.type = MC_TYPE_EXTENDED;
	mc_ext.common.size = sizeof(mc_ext);
	mc_ext.mc_msrs = 3;

	mc_ext.mc_msr[0].reg = MSR_F10_MC4_MISC1;
	mc_ext.mc_msr[1].reg = MSR_F10_MC4_MISC2;
	mc_ext.mc_msr[2].reg = MSR_F10_MC4_MISC3;

	mca_rdmsrl(MSR_F10_MC4_MISC1, mc_ext.mc_msr[0].value);
	mca_rdmsrl(MSR_F10_MC4_MISC2, mc_ext.mc_msr[1].value);
	mca_rdmsrl(MSR_F10_MC4_MISC3, mc_ext.mc_msr[2].value);
	
	x86_mcinfo_add(mi, &mc_ext);
	return MCA_EXTINFO_LOCAL;
}


extern void k8_machine_check(struct cpu_user_regs *regs, long error_code);

/* AMD Family10 machine check */
int amd_f10_mcheck_init(struct cpuinfo_x86 *c) 
{ 
	uint64_t value;
	uint32_t i;
	int cpu_nr;

	if (!cpu_has(c, X86_FEATURE_MCA))
		return 0;

	x86_mce_vector_register(k8_machine_check);
	x86_mce_callback_register(amd_f10_handler);
	cpu_nr = smp_processor_id();

	rdmsrl(MSR_IA32_MCG_CAP, value);
	if (value & MCG_CTL_P)	/* Control register present ? */
		wrmsrl (MSR_IA32_MCG_CTL, 0xffffffffffffffffULL);
	nr_mce_banks = value & MCG_CAP_COUNT;

	for (i = 0; i < nr_mce_banks; i++) {
		switch (i) {
		case 4: /* Northbridge */
			/* Enable error reporting of all errors */
			wrmsrl(MSR_IA32_MC4_CTL, 0xffffffffffffffffULL);
			wrmsrl(MSR_IA32_MC4_STATUS, 0x0ULL);
			break;

		default:
			/* Enable error reporting of all errors */
			wrmsrl(MSR_IA32_MC0_CTL + 4 * i, 0xffffffffffffffffULL);
			wrmsrl(MSR_IA32_MC0_STATUS + 4 * i, 0x0ULL);
			break;
		}
	}

	set_in_cr4(X86_CR4_MCE);
	printk("CPU%i: AMD Family10h machine check reporting enabled.\n", cpu_nr);
	return 1;
}
