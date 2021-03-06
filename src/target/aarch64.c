/***************************************************************************
 *   Copyright (C) 2015 by David Ung                                       *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *                                                                         *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "breakpoints.h"
#include "aarch64.h"
#include "register.h"
#include "target_request.h"
#include "target_type.h"
#include "arm_opcodes.h"
#include <helper/time_support.h>
#include "armv8_opcodes.h"

static int aarch64_poll(struct target *target);
static int aarch64_debug_entry(struct target *target);
static int aarch64_restore_context(struct target *target, bool bpwp);
static int aarch64_set_breakpoint(struct target *target,
	struct breakpoint *breakpoint, uint8_t matchmode);
static int aarch64_set_context_breakpoint(struct target *target,
	struct breakpoint *breakpoint, uint8_t matchmode);
static int aarch64_set_hybrid_breakpoint(struct target *target,
	struct breakpoint *breakpoint);
static int aarch64_unset_breakpoint(struct target *target,
	struct breakpoint *breakpoint);
static int aarch64_mmu(struct target *target, int *enabled);
static int aarch64_virt2phys(struct target *target,
	uint32_t virt, uint32_t *phys);
static int aarch64_read_apb_ab_memory(struct target *target,
	uint64_t address, uint32_t size, uint32_t count, uint8_t *buffer);

static int aarch64_instr_write_data_r0(struct arm_dpm *dpm,
				       uint32_t opcode, uint32_t data);

static int aarch64_restore_system_control_reg(struct target *target)
{
	struct aarch64_common *aarch64 = target_to_aarch64(target);
	struct armv8_common *armv8 = &aarch64->armv8_common;
	int retval = ERROR_OK;

	if (aarch64->system_control_reg != aarch64->system_control_reg_curr) {
		aarch64->system_control_reg_curr = aarch64->system_control_reg;
		/* LOG_INFO("cp15_control_reg: %8.8" PRIx32, cortex_v8->cp15_control_reg); */

		switch (armv8->arm.core_mode) {
			case ARMV8_64_EL0T:
			case ARMV8_64_EL1T:
			case ARMV8_64_EL1H:
				retval = armv8->arm.msr(target, 3, /*op 0*/
						0, 1,	/* op1, op2 */
						0, 0,	/* CRn, CRm */
						aarch64->system_control_reg);
				if (retval != ERROR_OK)
					return retval;
			break;
			case ARMV8_64_EL2T:
			case ARMV8_64_EL2H:
				retval = armv8->arm.msr(target, 3, /*op 0*/
						4, 1,	/* op1, op2 */
						0, 0,	/* CRn, CRm */
						aarch64->system_control_reg);
				if (retval != ERROR_OK)
					return retval;
			break;
			case ARMV8_64_EL3H:
			case ARMV8_64_EL3T:
				retval = armv8->arm.msr(target, 3, /*op 0*/
						6, 1,	/* op1, op2 */
						0, 0,	/* CRn, CRm */
						aarch64->system_control_reg);
				if (retval != ERROR_OK)
					return retval;
			break;
			default:
				LOG_DEBUG("unknown cpu state 0x%x" PRIx32, armv8->arm.core_state);
			}
	}
	return retval;
}


/*  check address before aarch64_apb read write access with mmu on
 *  remove apb predictible data abort */
static int aarch64_check_address(struct target *target, uint32_t address)
{
	/* TODO */
	return ERROR_OK;
}
/*  modify system_control_reg in order to enable or disable mmu for :
 *  - virt2phys address conversion
 *  - read or write memory in phys or virt address */
static int aarch64_mmu_modify(struct target *target, int enable)
{
	struct aarch64_common *aarch64 = target_to_aarch64(target);
	struct armv8_common *armv8 = &aarch64->armv8_common;
	int retval = ERROR_OK;
	if (enable) {
		/*	if mmu enabled at target stop and mmu not enable */
		if (!(aarch64->system_control_reg & 0x1U)) {
			LOG_ERROR("trying to enable mmu on stopped target with mmu disabled");
			return ERROR_FAIL;
		}
		if (!(aarch64->system_control_reg_curr & 0x1U)) {
			aarch64->system_control_reg_curr |= 0x1U;
			switch (armv8->arm.core_mode) {
				case ARMV8_64_EL0T:
				case ARMV8_64_EL1T:
				case ARMV8_64_EL1H:
					retval = armv8->arm.msr(target, 3, /*op 0*/
							0, 0,	/* op1, op2 */
							1, 0,	/* CRn, CRm */
							aarch64->system_control_reg_curr);
					if (retval != ERROR_OK)
						return retval;
				break;
				case ARMV8_64_EL2T:
				case ARMV8_64_EL2H:
					retval = armv8->arm.msr(target, 3, /*op 0*/
							4, 0,	/* op1, op2 */
							1, 0,	/* CRn, CRm */
							aarch64->system_control_reg_curr);
					if (retval != ERROR_OK)
						return retval;
				break;
				case ARMV8_64_EL3H:
				case ARMV8_64_EL3T:
					retval = armv8->arm.msr(target, 3, /*op 0*/
							6, 0,	/* op1, op2 */
							1, 0,	/* CRn, CRm */
							aarch64->system_control_reg_curr);
					if (retval != ERROR_OK)
						return retval;
				break;
				default:
					LOG_DEBUG("unknown cpu state 0x%x" PRIx32, armv8->arm.core_state);
			}
		}
	} else {
		if (aarch64->system_control_reg_curr & 0x4U) {
			/*	data cache is active */
			aarch64->system_control_reg_curr &= ~0x4U;
			/* flush data cache armv7 function to be called */
			if (armv8->armv8_mmu.armv8_cache.flush_all_data_cache)
				armv8->armv8_mmu.armv8_cache.flush_all_data_cache(target);
		}
		if ((aarch64->system_control_reg_curr & 0x1U)) {
			aarch64->system_control_reg_curr &= ~0x1U;
			switch (armv8->arm.core_mode) {
				case ARMV8_64_EL0T:
				case ARMV8_64_EL1T:
				case ARMV8_64_EL1H:
					retval = armv8->arm.msr(target, 3, /*op 0*/
							0, 0,	/* op1, op2 */
							1, 0,	/* CRn, CRm */
							aarch64->system_control_reg_curr);
					if (retval != ERROR_OK)
						return retval;
					break;
				case ARMV8_64_EL2T:
				case ARMV8_64_EL2H:
					retval = armv8->arm.msr(target, 3, /*op 0*/
							4, 0,	/* op1, op2 */
							1, 0,	/* CRn, CRm */
							aarch64->system_control_reg_curr);
					if (retval != ERROR_OK)
						return retval;
					break;
				case ARMV8_64_EL3H:
				case ARMV8_64_EL3T:
					retval = armv8->arm.msr(target, 3, /*op 0*/
							6, 0,	/* op1, op2 */
							1, 0,	/* CRn, CRm */
							aarch64->system_control_reg_curr);
					if (retval != ERROR_OK)
						return retval;
					break;
				default:
					LOG_DEBUG("unknown cpu state 0x%x" PRIx32, armv8->arm.core_state);
					break;
			}
		}
	}
	return retval;
}


/*
 * Basic debug access, very low level assumes state is saved
 */
static int aarch64_init_debug_access(struct target *target)
{
	struct armv8_common *armv8 = target_to_armv8(target);
	struct adiv5_dap *swjdp = armv8->arm.dap;
	int retval;
	uint32_t dummy;

	LOG_DEBUG(" ");

	/* Unlocking the debug registers for modification
	 * The debugport might be uninitialised so try twice */
	retval = mem_ap_sel_write_atomic_u32(swjdp, armv8->debug_ap,
			     armv8->debug_base + CPUV8_DBG_LOCKACCESS, 0xC5ACCE55);
	if (retval != ERROR_OK) {
		/* try again */
		retval = mem_ap_sel_write_atomic_u32(swjdp, armv8->debug_ap,
			     armv8->debug_base + CPUV8_DBG_LOCKACCESS, 0xC5ACCE55);
		if (retval == ERROR_OK)
			LOG_USER("Locking debug access failed on first, but succeeded on second try.");
	}
	if (retval != ERROR_OK)
		return retval;
	/* Clear Sticky Power Down status Bit in PRSR to enable access to
	   the registers in the Core Power Domain */
	retval = mem_ap_sel_read_atomic_u32(swjdp, armv8->debug_ap,
			armv8->debug_base + CPUV8_DBG_PRSR, &dummy);
	if (retval != ERROR_OK)
		return retval;

	/* Enabling of instruction execution in debug mode is done in debug_entry code */

	/*
	  Establish the CTM/CTI/PE handling
	 */
# if 1
	{
	  uint32_t scratch;

	  /* enable CTI */
	  retval = mem_ap_sel_write_atomic_u32(swjdp, armv8->debug_ap,
					       armv8->cti_base + CTI_CTR, 1);
	  if (retval != ERROR_OK)
	    return retval;

	  retval = mem_ap_sel_read_atomic_u32(swjdp, armv8->debug_ap,
					       armv8->cti_base + CTI_CTR, &scratch);
	  LOG_DEBUG("CTI_CTR:    0x%08" PRIx32, scratch);

	  /* 4 signals enabled */
	  retval = mem_ap_sel_write_atomic_u32(swjdp, armv8->debug_ap,
					       armv8->cti_base + CTI_GATE, 0xf);
	  if (retval != ERROR_OK)
	    return retval;

	  retval = mem_ap_sel_read_atomic_u32(swjdp, armv8->debug_ap,
					      armv8->cti_base + CTI_GATE, &scratch);
	  LOG_DEBUG("CTI_GATE:   0x%08" PRIx32, scratch);
	  /*
	    0: debug request trigger event
	  */
	  retval = mem_ap_sel_write_atomic_u32(swjdp, armv8->debug_ap,
					       armv8->cti_base + CTI_OUTEN0, 1);
	  if (retval != ERROR_OK)
	    return retval;

	  retval = mem_ap_sel_read_atomic_u32(swjdp, armv8->debug_ap,
					      armv8->cti_base + CTI_OUTEN0, &scratch);
	  LOG_DEBUG("CTI_OUTEN0: 0x%08" PRIx32, scratch);

	  /*
	    1: restart request trigger event
	  */
	  retval = mem_ap_sel_write_atomic_u32(swjdp, armv8->debug_ap,
					       armv8->cti_base + CTI_OUTEN1, 2);
	  if (retval != ERROR_OK)
	    return retval;

	  retval = mem_ap_sel_read_atomic_u32(swjdp, armv8->debug_ap,
					      armv8->cti_base + CTI_OUTEN1, &scratch);
	  LOG_DEBUG("CTI_OUTEN1: 0x%08" PRIx32, scratch);

	  /*
	   * add HDE in halting debug mode
	   */
	  retval = mem_ap_sel_read_atomic_u32(swjdp, armv8->debug_ap,
					      armv8->debug_base + CPUV8_DBG_DSCR, &scratch);
	  if (retval != ERROR_OK)
	    return retval;
	  
	  retval = mem_ap_sel_write_atomic_u32(swjdp, armv8->debug_ap,
					       armv8->debug_base + CPUV8_DBG_DSCR, scratch | DSCR_HDE);
	  if (retval != ERROR_OK)
	    return retval;
	  
	    retval = mem_ap_sel_read_atomic_u32(swjdp, armv8->debug_ap,
						armv8->cti_base + CTI_TROUT_STATUS, &scratch);
	    if (retval != ERROR_OK) {
	      return retval;
	    }

	    LOG_DEBUG("CTI trigger out status: 0x%08" PRIx32, scratch);

	}
# endif

	/* Resync breakpoint registers */

	/* Since this is likely called from init or reset, update target state information*/
	return aarch64_poll(target);
}

/* To reduce needless round-trips, pass in a pointer to the current
 * DSCR value.  Initialize it to zero if you just need to know the
 * value on return from this function; or DSCR_ITE if you
 * happen to know that no instruction is pending.
 */
static int aarch64_exec_opcode(struct target *target,
	uint32_t opcode, uint32_t *dscr_p)
{
	uint32_t dscr;
	int retval;
	struct armv8_common *armv8 = target_to_armv8(target);
	struct adiv5_dap *swjdp = armv8->arm.dap;

	dscr = dscr_p ? *dscr_p : 0;

	LOG_DEBUG("exec opcode 0x%08" PRIx32, opcode);

	/* Wait for InstrCompl bit to be set */
	long long then = timeval_ms();
	while ((dscr & DSCR_ITE) == 0) {
		retval = mem_ap_sel_read_atomic_u32(swjdp, armv8->debug_ap,
				armv8->debug_base + CPUV8_DBG_DSCR, &dscr);
		if (retval != ERROR_OK) {
			LOG_ERROR("Could not read DSCR register, opcode = 0x%08" PRIx32, opcode);
			return retval;
		}
		if (timeval_ms() > then + 2000) {
			LOG_ERROR("Timeout waiting for aarch64_exec_opcode");
			return ERROR_FAIL;
		}
	}

	retval = mem_ap_sel_write_u32(swjdp, armv8->debug_ap,
			armv8->debug_base + CPUV8_DBG_ITR, opcode);
	if (retval != ERROR_OK)
		return retval;

	then = timeval_ms();
	do {
		retval = mem_ap_sel_read_atomic_u32(swjdp, armv8->debug_ap,
				armv8->debug_base + CPUV8_DBG_DSCR, &dscr);
		if (retval != ERROR_OK) {
			LOG_ERROR("Could not read DSCR register");
			return retval;
		}
		if (timeval_ms() > then + 1000) {
			LOG_ERROR("Timeout waiting for aarch64_exec_opcode");
			return ERROR_FAIL;
		}
	} while ((dscr & DSCR_ITE) == 0);	/* Wait for InstrCompl bit to be set */

	if (dscr_p)
		*dscr_p = dscr;

	return retval;
}

/* Write to memory mapped registers directly with no cache or mmu handling */
static int aarch64_dap_write_memap_register_u32(struct target *target,
	uint32_t address,
	uint32_t value)
{
	int retval;
	struct armv8_common *armv8 = target_to_armv8(target);
	struct adiv5_dap *swjdp = armv8->arm.dap;

	retval = mem_ap_sel_write_atomic_u32(swjdp, armv8->debug_ap, address, value);

	return retval;
}

/*
 * AARCH64 implementation of Debug Programmer's Model
 *
 * NOTE the invariant:  these routines return with DSCR_ITE set,
 * so there's no need to poll for it before executing an instruction.
 *
 * NOTE that in several of these cases the "stall" mode might be useful.
 * It'd let us queue a few operations together... prepare/finish might
 * be the places to enable/disable that mode.
 */

static inline struct aarch64_common *dpm_to_a8(struct arm_dpm *dpm)
{
	return container_of(dpm, struct aarch64_common, armv8_common.dpm);
}

static int aarch64_write_dcc(struct aarch64_common *a8, uint32_t data)
{
	LOG_DEBUG("write DCC 0x%08" PRIx32, data);
	return mem_ap_sel_write_u32(a8->armv8_common.arm.dap,
		a8->armv8_common.debug_ap, a8->armv8_common.debug_base + CPUV8_DBG_DTRRX, data);
}

static int aarch64_write_dcc_64(struct aarch64_common *a8, uint64_t data)
{
	int ret;
	LOG_DEBUG("write DCC Low word  0x%08" PRIx32, (unsigned)data);
	LOG_DEBUG("write DCC High word 0x%08" PRIx32, (unsigned)(data >> 32));
	ret = mem_ap_sel_write_u32(a8->armv8_common.arm.dap,
		a8->armv8_common.debug_ap, a8->armv8_common.debug_base + CPUV8_DBG_DTRRX, data);
	ret += mem_ap_sel_write_u32(a8->armv8_common.arm.dap,
		a8->armv8_common.debug_ap, a8->armv8_common.debug_base + CPUV8_DBG_DTRTX, data >> 32);
	return ret;
}

static int aarch64_read_dcc(struct aarch64_common *a8, uint32_t *data,
	uint32_t *dscr_p)
{
	struct adiv5_dap *swjdp = a8->armv8_common.arm.dap;
	uint32_t dscr = DSCR_ITE;
	int retval;

	if (dscr_p)
		dscr = *dscr_p;

	/* Wait for DTRRXfull */
	long long then = timeval_ms();
	while ((dscr & DSCR_DTR_TX_FULL) == 0) {
		retval = mem_ap_sel_read_atomic_u32(swjdp, a8->armv8_common.debug_ap,
				a8->armv8_common.debug_base + CPUV8_DBG_DSCR,
				&dscr);
		if (retval != ERROR_OK)
			return retval;
		if (timeval_ms() > then + 1000) {
			LOG_ERROR("Timeout waiting for read dcc");
			return ERROR_FAIL;
		}
	}

	retval = mem_ap_sel_read_atomic_u32(swjdp, a8->armv8_common.debug_ap,
					    a8->armv8_common.debug_base + CPUV8_DBG_DTRTX,
					    data);
	if (retval != ERROR_OK)
		return retval;
	LOG_DEBUG("read DCC 0x%08" PRIx32, *data);

	if (dscr_p)
		*dscr_p = dscr;

	return retval;
}
static int aarch64_read_dcc_64(struct aarch64_common *a8, uint64_t *data,
	uint32_t *dscr_p)
{
	struct adiv5_dap *swjdp = a8->armv8_common.arm.dap;
	uint32_t dscr = DSCR_ITE;
	uint32_t higher;
	int retval;

	if (dscr_p)
		dscr = *dscr_p;

	/* Wait for DTRRXfull */
	long long then = timeval_ms();
	while ((dscr & DSCR_DTR_TX_FULL) == 0) {
		retval = mem_ap_sel_read_atomic_u32(swjdp, a8->armv8_common.debug_ap,
				a8->armv8_common.debug_base + CPUV8_DBG_DSCR,
				&dscr);
		if (retval != ERROR_OK)
			return retval;
		if (timeval_ms() > then + 1000) {
			LOG_ERROR("Timeout waiting for read dcc");
			return ERROR_FAIL;
		}
	}

	retval = mem_ap_sel_read_atomic_u32(swjdp, a8->armv8_common.debug_ap,
					    a8->armv8_common.debug_base + CPUV8_DBG_DTRTX,
					    (uint32_t *)data);
	if (retval != ERROR_OK)
		return retval;

	retval = mem_ap_sel_read_atomic_u32(swjdp, a8->armv8_common.debug_ap,
					    a8->armv8_common.debug_base + CPUV8_DBG_DTRRX,
					    &higher);
	if (retval != ERROR_OK)
		return retval;

	*data = *(uint32_t *)data | (uint64_t)higher << 32;
	LOG_DEBUG("read DCC 0x%16.16" PRIx64, *data);

	if (dscr_p)
		*dscr_p = dscr;

	return retval;
}

static int aarch64_dpm_prepare(struct arm_dpm *dpm)
{
	struct aarch64_common *a8 = dpm_to_a8(dpm);
	struct adiv5_dap *swjdp = a8->armv8_common.arm.dap;
	uint32_t dscr;
	int retval;

	/* set up invariant:  INSTR_COMP is set after ever DPM operation */
	long long then = timeval_ms();
	for (;; ) {
		retval = mem_ap_sel_read_atomic_u32(swjdp, a8->armv8_common.debug_ap,
				a8->armv8_common.debug_base + CPUV8_DBG_DSCR,
				&dscr);
		if (retval != ERROR_OK)
			return retval;
		if ((dscr & DSCR_ITE) != 0)
			break;
		if (timeval_ms() > then + 2000) {
			LOG_ERROR("Timeout waiting for dpm prepare");
			return ERROR_FAIL;
		}
	}

	/* this "should never happen" ... */
	if (dscr & DSCR_DTR_RX_FULL) {
		LOG_ERROR("DSCR_DTR_RX_FULL, dscr 0x%08" PRIx32, dscr);
		/* Clear DCCRX */
		retval = mem_ap_sel_read_u32(swjdp, a8->armv8_common.debug_ap,
			a8->armv8_common.debug_base + CPUV8_DBG_DTRRX, &dscr);
		if (retval != ERROR_OK)
			return retval;

		/* Clear sticky error */
		retval = mem_ap_sel_write_u32(swjdp, a8->armv8_common.debug_ap,
			a8->armv8_common.debug_base + CPUV8_DBG_DRCR, DRCR_CSE);
		if (retval != ERROR_OK)
			return retval;

	}

	return retval;
}

static int aarch64_dpm_finish(struct arm_dpm *dpm)
{
	/* REVISIT what could be done here? */
	return ERROR_OK;
}

static int aarch64_instr_execute(struct arm_dpm *dpm,
	uint32_t opcode)
{
	struct aarch64_common *a8 = dpm_to_a8(dpm);
	uint32_t dscr = DSCR_ITE;

	return aarch64_exec_opcode(
			a8->armv8_common.arm.target,
			opcode,
			&dscr);
}

static int aarch64_instr_write_data_dcc(struct arm_dpm *dpm,
	uint32_t opcode, uint32_t data)
{
	struct aarch64_common *a8 = dpm_to_a8(dpm);
	int retval;
	uint32_t dscr = DSCR_ITE;

	retval = aarch64_write_dcc(a8, data);
	if (retval != ERROR_OK)
		return retval;

	return aarch64_exec_opcode(
			a8->armv8_common.arm.target,
			opcode,
			&dscr);
}

static int aarch64_instr_write_data_dcc_64(struct arm_dpm *dpm,
	uint32_t opcode, uint64_t data)
{
	struct aarch64_common *a8 = dpm_to_a8(dpm);
	int retval;
	uint32_t dscr = DSCR_ITE;

	retval = aarch64_write_dcc_64(a8, data);
	if (retval != ERROR_OK)
		return retval;

	return aarch64_exec_opcode(
			a8->armv8_common.arm.target,
			opcode,
			&dscr);
}

static int aarch64_instr_write_data_r0(struct arm_dpm *dpm,
	uint32_t opcode, uint32_t data)
{
	struct aarch64_common *a8 = dpm_to_a8(dpm);
	uint32_t dscr = DSCR_ITE;
	int retval;

	retval = aarch64_write_dcc(a8, data);
	if (retval != ERROR_OK)
		return retval;

	retval = aarch64_exec_opcode(
			a8->armv8_common.arm.target,
			ARMV8_MRS(SYSTEM_DBG_DTRRX_EL0, 0),
			&dscr);
	if (retval != ERROR_OK)
		return retval;

	/* then the opcode, taking data from R0 */
	retval = aarch64_exec_opcode(
			a8->armv8_common.arm.target,
			opcode,
			&dscr);

	return retval;
}

static int aarch64_instr_write_data_r0_64(struct arm_dpm *dpm,
	uint32_t opcode, uint64_t data)
{
	struct aarch64_common *a8 = dpm_to_a8(dpm);
	uint32_t dscr = DSCR_ITE;
	int retval;

	retval = aarch64_write_dcc_64(a8, data);
	if (retval != ERROR_OK)
		return retval;

	retval = aarch64_exec_opcode(
			a8->armv8_common.arm.target,
			ARMV8_MRS(SYSTEM_DBG_DBGDTR_EL0, 0),
			&dscr);
	if (retval != ERROR_OK)
		return retval;

	/* then the opcode, taking data from R0 */
	retval = aarch64_exec_opcode(
			a8->armv8_common.arm.target,
			opcode,
			&dscr);

	return retval;
}

static int aarch64_instr_cpsr_sync(struct arm_dpm *dpm)
{
	struct target *target = dpm->arm->target;
	uint32_t dscr = DSCR_ITE;

	/* "Prefetch flush" after modifying execution status in CPSR */
	return aarch64_exec_opcode(target,
			DSB_SY,
			&dscr);
}

static int aarch64_instr_read_data_dcc(struct arm_dpm *dpm,
	uint32_t opcode, uint32_t *data)
{
	struct aarch64_common *a8 = dpm_to_a8(dpm);
	int retval;
	uint32_t dscr = DSCR_ITE;

	/* the opcode, writing data to DCC */
	retval = aarch64_exec_opcode(
			a8->armv8_common.arm.target,
			opcode,
			&dscr);
	if (retval != ERROR_OK)
		return retval;

	return aarch64_read_dcc(a8, data, &dscr);
}

static int aarch64_instr_read_data_dcc_64(struct arm_dpm *dpm,
	uint32_t opcode, uint64_t *data)
{
	struct aarch64_common *a8 = dpm_to_a8(dpm);
	int retval;
	uint32_t dscr = DSCR_ITE;

	/* the opcode, writing data to DCC */
	retval = aarch64_exec_opcode(
			a8->armv8_common.arm.target,
			opcode,
			&dscr);
	if (retval != ERROR_OK)
		return retval;

	return aarch64_read_dcc_64(a8, data, &dscr);
}

static int aarch64_instr_read_data_r0(struct arm_dpm *dpm,
	uint32_t opcode, uint32_t *data)
{
	struct aarch64_common *a8 = dpm_to_a8(dpm);
	uint32_t dscr = DSCR_ITE;
	int retval;

	/* the opcode, writing data to R0 */
	retval = aarch64_exec_opcode(
			a8->armv8_common.arm.target,
			opcode,
			&dscr);
	if (retval != ERROR_OK)
		return retval;

	/* write R0 to DCC */
	retval = aarch64_exec_opcode(
			a8->armv8_common.arm.target,
			ARMV8_MSR_GP(SYSTEM_DBG_DTRTX_EL0, 0),  /* msr dbgdtr_el0, x0 */
			&dscr);
	if (retval != ERROR_OK)
		return retval;

	return aarch64_read_dcc(a8, data, &dscr);
}

static int aarch64_instr_read_data_r0_64(struct arm_dpm *dpm,
	uint32_t opcode, uint64_t *data)
{
	struct aarch64_common *a8 = dpm_to_a8(dpm);
	uint32_t dscr = DSCR_ITE;
	int retval;

	/* the opcode, writing data to R0 */
	retval = aarch64_exec_opcode(
			a8->armv8_common.arm.target,
			opcode,
			&dscr);
	if (retval != ERROR_OK)
		return retval;

	/* write R0 to DCC */
	retval = aarch64_exec_opcode(
			a8->armv8_common.arm.target,
			ARMV8_MSR_GP(SYSTEM_DBG_DBGDTR_EL0, 0),  /* msr dbgdtr_el0, x0 */
			&dscr);
	if (retval != ERROR_OK)
		return retval;

	return aarch64_read_dcc_64(a8, data, &dscr);
}

static int aarch64_bpwp_enable(struct arm_dpm *dpm, unsigned index_t,
	uint32_t addr, uint32_t control)
{
	struct aarch64_common *a8 = dpm_to_a8(dpm);
	uint32_t vr = a8->armv8_common.debug_base;
	uint32_t cr = a8->armv8_common.debug_base;
	int retval;

	switch (index_t) {
		case 0 ... 15:	/* breakpoints */
			vr += CPUV8_DBG_BVR_BASE;
			cr += CPUV8_DBG_BCR_BASE;
			break;
		case 16 ... 31:	/* watchpoints */
			vr += CPUV8_DBG_WVR_BASE;
			cr += CPUV8_DBG_WCR_BASE;
			index_t -= 16;
			break;
		default:
			return ERROR_FAIL;
	}
	vr += 16 * index_t;
	cr += 16 * index_t;

	LOG_DEBUG("A8: bpwp enable, vr %08x cr %08x",
		(unsigned) vr, (unsigned) cr);

	retval = aarch64_dap_write_memap_register_u32(dpm->arm->target,
			vr, addr);
	if (retval != ERROR_OK)
		return retval;
	retval = aarch64_dap_write_memap_register_u32(dpm->arm->target,
			cr, control);
	return retval;
}

static int aarch64_bpwp_disable(struct arm_dpm *dpm, unsigned index_t)
{
	struct aarch64_common *a = dpm_to_a8(dpm);
	uint32_t cr;

	switch (index_t) {
		case 0 ... 15:
			cr = a->armv8_common.debug_base + CPUV8_DBG_BCR_BASE;
			break;
		case 16 ... 31:
			cr = a->armv8_common.debug_base + CPUV8_DBG_WCR_BASE;
			index_t -= 16;
			break;
		default:
			return ERROR_FAIL;
	}
	cr += 16 * index_t;

	LOG_DEBUG("A: bpwp disable, cr %08x", (unsigned) cr);

	/* clear control register */
	return aarch64_dap_write_memap_register_u32(dpm->arm->target, cr, 0);

}

static int aarch64_dpm_setup(struct aarch64_common *a8, uint64_t debug)
{
	struct arm_dpm *dpm = &a8->armv8_common.dpm;
	int retval;

	dpm->arm = &a8->armv8_common.arm;
	dpm->didr = debug;

	dpm->prepare = aarch64_dpm_prepare;
	dpm->finish = aarch64_dpm_finish;

	dpm->instr_execute = aarch64_instr_execute;
	dpm->instr_write_data_dcc = aarch64_instr_write_data_dcc;
	dpm->instr_write_data_dcc_64 = aarch64_instr_write_data_dcc_64;
	dpm->instr_write_data_r0 = aarch64_instr_write_data_r0;
	dpm->instr_write_data_r0_64 = aarch64_instr_write_data_r0_64;

	dpm->instr_cpsr_sync = aarch64_instr_cpsr_sync;

	dpm->instr_read_data_dcc = aarch64_instr_read_data_dcc;
	dpm->instr_read_data_dcc_64 = aarch64_instr_read_data_dcc_64;
	dpm->instr_read_data_r0 = aarch64_instr_read_data_r0;
	dpm->instr_read_data_r0_64 = aarch64_instr_read_data_r0_64;

/*	dpm->arm_reg_current = armv8_reg_current;*/

	dpm->bpwp_enable = aarch64_bpwp_enable;
	dpm->bpwp_disable = aarch64_bpwp_disable;

	retval = armv8_dpm_setup(dpm);
	if (retval == ERROR_OK)
		retval = armv8_dpm_initialize(dpm);

	return retval;
}
static struct target *get_aarch64(struct target *target, int32_t coreid)
{
	struct target_list *head;
	struct target *curr;

	head = target->head;
	while (head != (struct target_list *)NULL) {
		curr = head->target;
		if ((curr->coreid == coreid) && (curr->state == TARGET_HALTED))
			return curr;
		head = head->next;
	}
	return target;
}
static int aarch64_halt(struct target *target);

static int aarch64_halt_smp(struct target *target)
{
	int retval = 0;
	struct target_list *head;
	struct target *curr;
	head = target->head;
	while (head != (struct target_list *)NULL) {
		curr = head->target;
		if ((curr != target) && (curr->state != TARGET_HALTED))
			retval += aarch64_halt(curr);
		head = head->next;
	}
	return retval;
}

static int update_halt_gdb(struct target *target)
{
	int retval = 0;
	if (target->gdb_service && target->gdb_service->core[0] == -1) {
		target->gdb_service->target = target;
		target->gdb_service->core[0] = target->coreid;
		retval += aarch64_halt_smp(target);
	}
	return retval;
}

/*
 * Cortex-A8 Run control
 */

static int aarch64_poll(struct target *target)
{
	int retval = ERROR_OK;
	uint32_t dscr;
	struct aarch64_common *aarch64 = target_to_aarch64(target);
	struct armv8_common *armv8 = &aarch64->armv8_common;
	struct adiv5_dap *swjdp = armv8->arm.dap;
	enum target_state prev_target_state = target->state;
	/*  toggle to another core is done by gdb as follow */
	/*  maint packet J core_id */
	/*  continue */
	/*  the next polling trigger an halt event sent to gdb */
	if ((target->state == TARGET_HALTED) && (target->smp) &&
		(target->gdb_service) &&
		(target->gdb_service->target == NULL)) {
		target->gdb_service->target =
			get_aarch64(target, target->gdb_service->core[1]);
		target_call_event_callbacks(target, TARGET_EVENT_HALTED);
		return retval;
	}
	retval = mem_ap_sel_read_atomic_u32(swjdp, armv8->debug_ap,
			armv8->debug_base + CPUV8_DBG_DSCR, &dscr);
	if (retval != ERROR_OK)
		return retval;
	aarch64->cpudbg_dscr = dscr;

	if (DSCR_RUN_MODE(dscr) != 0) {
		if (prev_target_state != TARGET_HALTED) {
			/* We have a halting debug event */
			LOG_DEBUG("Target halted");
			target->state = TARGET_HALTED;
			if ((prev_target_state == TARGET_RUNNING)
				|| (prev_target_state == TARGET_UNKNOWN)
				|| (prev_target_state == TARGET_RESET)) {
				retval = aarch64_debug_entry(target);
				if (retval != ERROR_OK)
					return retval;
				if (target->smp) {
					retval = update_halt_gdb(target);
					if (retval != ERROR_OK)
						return retval;
				}
				target_call_event_callbacks(target,
					TARGET_EVENT_HALTED);
			}
			if (prev_target_state == TARGET_DEBUG_RUNNING) {
				LOG_DEBUG(" ");

				retval = aarch64_debug_entry(target);
				if (retval != ERROR_OK)
					return retval;
				if (target->smp) {
					retval = update_halt_gdb(target);
					if (retval != ERROR_OK)
						return retval;
				}

				target_call_event_callbacks(target,
					TARGET_EVENT_DEBUG_HALTED);
			}
		}
	} else if ((DSCR_RUN_MODE(dscr) & DSCR_HALT_MASK) == 0)
		target->state = TARGET_RUNNING;
	else {
		LOG_DEBUG("Unknown target state dscr = 0x%08" PRIx32, dscr);
		target->state = TARGET_UNKNOWN;
	}

	return retval;
}

static int aarch64_halt(struct target *target)
{
	int retval = ERROR_OK;
	uint32_t dscr;
	struct armv8_common *armv8 = target_to_armv8(target);
	struct adiv5_dap *swjdp = armv8->arm.dap;

	/* enable CTI */
	retval = mem_ap_sel_write_atomic_u32(swjdp, armv8->debug_ap,
			armv8->cti_base + CTI_CTR, 1);
	if (retval != ERROR_OK)
		return retval;

	retval = mem_ap_sel_write_atomic_u32(swjdp, armv8->debug_ap,
			armv8->cti_base + CTI_GATE, 3);
	if (retval != ERROR_OK)
		return retval;

	retval = mem_ap_sel_write_atomic_u32(swjdp, armv8->debug_ap,
			armv8->cti_base + CTI_OUTEN0, 1);
	if (retval != ERROR_OK)
		return retval;

	retval = mem_ap_sel_write_atomic_u32(swjdp, armv8->debug_ap,
			armv8->cti_base + CTI_OUTEN1, 2);
	if (retval != ERROR_OK)
		return retval;

	/*
	 * add HDE in halting debug mode
	 */
	retval = mem_ap_sel_read_atomic_u32(swjdp, armv8->debug_ap,
			armv8->debug_base + CPUV8_DBG_DSCR, &dscr);
	if (retval != ERROR_OK)
		return retval;

	retval = mem_ap_sel_write_atomic_u32(swjdp, armv8->debug_ap,
			armv8->debug_base + CPUV8_DBG_DSCR, dscr | DSCR_HDE);
	if (retval != ERROR_OK)
		return retval;

	retval = mem_ap_sel_write_atomic_u32(swjdp, armv8->debug_ap,
			armv8->cti_base + CTI_APPPULSE, 1);
	if (retval != ERROR_OK)
		return retval;

	/*
	  Read trigger out status
	*/
	{
	  uint32_t trigstat = 0;

	  while (trigstat == 0) {
	    retval = mem_ap_sel_read_atomic_u32(swjdp, armv8->debug_ap,
						armv8->cti_base + CTI_TROUT_STATUS, &trigstat);
	    if (retval != ERROR_OK) {
	      return retval;
	    }

	    LOG_DEBUG("CTI trigger out status: 0x%08" PRIx32, trigstat);
	  }
	}

	retval = mem_ap_sel_write_atomic_u32(swjdp, armv8->debug_ap,
			armv8->cti_base + CTI_INACK, 1);
	if (retval != ERROR_OK)
		return retval;


	long long then = timeval_ms();
	for (;; ) {
		retval = mem_ap_sel_read_atomic_u32(swjdp, armv8->debug_ap,
				armv8->debug_base + CPUV8_DBG_DSCR, &dscr);
		if (retval != ERROR_OK)
			return retval;
		if ((dscr & DSCR_HALT_MASK) != 0)
			break;
		if (timeval_ms() > then + 1000) {
			LOG_ERROR("Timeout waiting for halt");
			return ERROR_FAIL;
		}
	}

	target->debug_reason = DBG_REASON_DBGRQ;

	return ERROR_OK;
}

static int aarch64_internal_restore(struct target *target, int current,
	uint64_t *address, int handle_breakpoints, int debug_execution)
{
	struct armv8_common *armv8 = target_to_armv8(target);
	struct arm *arm = &armv8->arm;
	int retval;
	uint64_t resume_pc;

	if (!debug_execution)
		target_free_all_working_areas(target);

	/* current = 1: continue on current pc, otherwise continue at <address> */
	resume_pc = buf_get_u64(arm->pc->value, 0, 64);
	if (!current)
		resume_pc = *address;
	else
		*address = resume_pc;

	/* Make sure that the Armv7 gdb thumb fixups does not
	 * kill the return address
	 */
	switch (arm->core_state) {
		case ARM_STATE_ARM:
			resume_pc &= 0xFFFFFFFC;
			break;
		case ARM_STATE_AARCH64:
			resume_pc &= 0xFFFFFFFFFFFFFFFC;
			break;
		case ARM_STATE_THUMB:
		case ARM_STATE_THUMB_EE:
			/* When the return address is loaded into PC
			 * bit 0 must be 1 to stay in Thumb state
			 */
			resume_pc |= 0x1;
			break;
		case ARM_STATE_JAZELLE:
			LOG_ERROR("How do I resume into Jazelle state??");
			return ERROR_FAIL;
	}
	LOG_DEBUG("resume pc = 0x%16" PRIx64, resume_pc);
	buf_set_u64(arm->pc->value, 0, 64, resume_pc);
	arm->pc->dirty = 1;
	arm->pc->valid = 1;
# if 0 /* PJA ? */
	dpmv8_modeswitch(&armv8->dpm, ARM_MODE_ANY);
# endif
	/* called it now before restoring context because it uses cpu
	 * register r0 for restoring system control register */
	retval = aarch64_restore_system_control_reg(target);
	if (retval != ERROR_OK)
		return retval;
	retval = aarch64_restore_context(target, handle_breakpoints);
	if (retval != ERROR_OK)
		return retval;
	target->debug_reason = DBG_REASON_NOTHALTED;
	target->state = TARGET_RUNNING;

	/* registers are now invalid */
	register_cache_invalidate(arm->core_cache);

#if 0
	/* the front-end may request us not to handle breakpoints */
	if (handle_breakpoints) {
		/* Single step past breakpoint at current address */
		breakpoint = breakpoint_find(target, resume_pc);
		if (breakpoint) {
			LOG_DEBUG("unset breakpoint at 0x%8.8x", breakpoint->address);
			cortex_m3_unset_breakpoint(target, breakpoint);
			cortex_m3_single_step_core(target);
			cortex_m3_set_breakpoint(target, breakpoint);
		}
	}
#endif

	return retval;
}

static int aarch64_internal_restart(struct target *target)
{
	struct armv8_common *armv8 = target_to_armv8(target);
	struct arm *arm = &armv8->arm;
	struct adiv5_dap *swjdp = arm->dap;
	int retval;
	uint32_t dscr;
	uint32_t trigstat;
	/*
	 * * Restart core and wait for it to be started.  Clear ITRen and sticky
	 * * exception flags: see ARMv7 ARM, C5.9.
	 *
	 * REVISIT: for single stepping, we probably want to
	 * disable IRQs by default, with optional override...
	 */

	retval = mem_ap_sel_read_atomic_u32(swjdp, armv8->debug_ap,
			armv8->debug_base + CPUV8_DBG_DSCR, &dscr);
	if (retval != ERROR_OK)
		return retval;

	if ((dscr & DSCR_ITE) == 0) {
	  if (target->state != TARGET_HALTED) {
	    return(ERROR_OK);
	    LOG_ERROR("DSCR InstrCompl must be set before leaving debug!");
	  }
	  else { /* fire a NOP down the pipe */
	    retval = aarch64_instr_execute(arm->dpm, 0xd503201f); /* NOP */
	  }
	}

	{
	  retval = mem_ap_sel_read_atomic_u32(swjdp, armv8->debug_ap,
					       armv8->cti_base + CTI_TROUT_STATUS, &trigstat);
	  LOG_DEBUG("CTI trigger out status: 0x%08" PRIx32, trigstat);
	}

# if 1
	retval = mem_ap_sel_write_atomic_u32(swjdp, armv8->debug_ap,
					     armv8->debug_base + CPUV8_DBG_DRCR, DRCR_CLEAR_SPA |
					     DRCR_CLEAR_EXCEPTIONS);
	if (retval != ERROR_OK)
		return retval;

	/* CTI_INTACK */
	retval = mem_ap_sel_write_atomic_u32(swjdp, armv8->debug_ap,
					     armv8->cti_base + CTI_INACK, 1);

	if (retval != ERROR_OK)
		return retval;
# endif

	/*
	  Signal restart
	*/
	retval = mem_ap_sel_write_atomic_u32(swjdp, armv8->debug_ap,
					     armv8->cti_base + CTI_APPPULSE, 2);
	if (retval != ERROR_OK)
		return retval;

	long long then = timeval_ms();
	for (;; ) {
		retval = mem_ap_sel_read_atomic_u32(swjdp, armv8->debug_ap,
				armv8->debug_base + CPUV8_DBG_DSCR, &dscr);
		if (retval != ERROR_OK)
			return retval;
# if 0
		LOG_DEBUG("dscr: 0x%08" PRIx32, dscr);
		if ((dscr & DSCR_DEBUG_STATUS_MASK) == DSCR_CORE_RESTARTED) {
		  break;
		}
# else
		if ((dscr & DSCR_HDE) != 0)
			break;
# endif
		if (timeval_ms() > then + 1000) {
			LOG_ERROR("Timeout waiting for resume");
			return ERROR_FAIL;
		}
	}

	target->debug_reason = DBG_REASON_NOTHALTED;
	target->state = TARGET_RUNNING;

	/* registers are now invalid */
	register_cache_invalidate(arm->core_cache);

	return ERROR_OK;
}

static int aarch64_restore_smp(struct target *target, int handle_breakpoints)
{
	int retval = 0;
	struct target_list *head;
	struct target *curr;
	uint64_t address;
	head = target->head;
	while (head != (struct target_list *)NULL) {
		curr = head->target;
		if ((curr != target) && (curr->state != TARGET_RUNNING)) {
			/*  resume current address , not in step mode */
			retval += aarch64_internal_restore(curr, 1, &address,
					handle_breakpoints, 0);
			retval += aarch64_internal_restart(curr);
		}
		head = head->next;

	}
	return retval;
}

static int aarch64_resume(struct target *target, int current,
	uint32_t address, int handle_breakpoints, int debug_execution)
{
	int retval = 0;
	uint64_t resume_addr;

	if (address) {
		LOG_DEBUG("resuming with custom address not supported");
		return ERROR_FAIL;
	}

	/* dummy resume for smp toggle in order to reduce gdb impact  */
	if ((target->smp) && (target->gdb_service->core[1] != -1)) {
		/*   simulate a start and halt of target */
		target->gdb_service->target = NULL;
		target->gdb_service->core[0] = target->gdb_service->core[1];
		/*  fake resume at next poll we play the  target core[1], see poll*/
		target_call_event_callbacks(target, TARGET_EVENT_RESUMED);
		return 0;
	}
	aarch64_internal_restore(target, current, &resume_addr, handle_breakpoints, debug_execution);
	if (target->smp) {
		target->gdb_service->core[0] = -1;
		retval = aarch64_restore_smp(target, handle_breakpoints);
		if (retval != ERROR_OK)
			return retval;
	}
	aarch64_internal_restart(target);

	if (!debug_execution) {
		target->state = TARGET_RUNNING;
		target_call_event_callbacks(target, TARGET_EVENT_RESUMED);
		LOG_DEBUG("target resumed at 0x%" PRIx64, resume_addr);
	} else {
		target->state = TARGET_DEBUG_RUNNING;
		target_call_event_callbacks(target, TARGET_EVENT_DEBUG_RESUMED);
		LOG_DEBUG("target debug resumed at 0x%" PRIx64, resume_addr);
	}

	return ERROR_OK;
}

static int aarch64_read_memory_64(struct target *target, uint64_t address,
				  uint32_t size, uint32_t count, uint8_t *buffer);

static int aarch64_debug_entry(struct target *target)
{
	uint32_t dscr;
	uint32_t edesr;
	uint32_t edprsr;
	int retval = ERROR_OK;
	struct aarch64_common *aarch64 = target_to_aarch64(target);
	struct armv8_common *armv8 = target_to_armv8(target);
	struct adiv5_dap *swjdp = armv8->arm.dap;

	LOG_DEBUG("dscr = 0x%08" PRIx32, aarch64->cpudbg_dscr);

	/* REVISIT surely we should not re-read DSCR !! */
	retval = mem_ap_sel_read_atomic_u32(swjdp, armv8->debug_ap,
			armv8->debug_base + CPUV8_DBG_DSCR, &dscr);
	if (retval != ERROR_OK)
		return retval;

	retval = mem_ap_sel_read_atomic_u32(swjdp, armv8->debug_ap,
					    armv8->debug_base + CPUV8_DBG_PRSR, &edprsr);

	if (retval != ERROR_OK)
		return retval;

	LOG_DEBUG("edprsr = 0x%08" PRIx32, edprsr);
	if (edprsr & 0x10) {
	  LOG_DEBUG("Halted: edprsr = 0x%08" PRIx32, edprsr);
	}

	retval = mem_ap_sel_read_atomic_u32(swjdp, armv8->debug_ap,
					    armv8->debug_base + CPUV8_DBG_EDESR, &edesr);

	if (retval != ERROR_OK)
		return retval;

	LOG_DEBUG("edesr = 0x%08" PRIx32, edesr);

	if (edesr & 0x4) {
	  LOG_DEBUG("edesr = 0x%08" PRIx32 " halting debug pending", edesr);
	}

	/* REVISIT see A8 TRM 12.11.4 steps 2..3 -- make sure that any
	 * imprecise data aborts get discarded by issuing a Data
	 * Synchronization Barrier:  ARMV4_5_MCR(15, 0, 0, 7, 10, 4).
	 */

	/* Examine debug reason */
	armv8_dpm_report_dscr(&armv8->dpm, aarch64->cpudbg_dscr);

# if 0
	if ((edesr & 0x7) == 0x4) { /* halted */
	  target->debug_reason = DBG_REASON_SINGLESTEP;
	}
# endif

	/* save address of instruction that triggered the watchpoint? */
	if (target->debug_reason == DBG_REASON_WATCHPOINT) {
		uint32_t tmp;
		uint64_t wfar = 0;

		retval = mem_ap_sel_read_atomic_u32(swjdp, armv8->debug_ap,
				armv8->debug_base + CPUV8_DBG_WFAR1,
				&tmp);
		if (retval != ERROR_OK)
			return retval;
		wfar = tmp;
		wfar = (wfar << 32);
		retval = mem_ap_sel_read_atomic_u32(swjdp, armv8->debug_ap,
				armv8->debug_base + CPUV8_DBG_WFAR0,
				&tmp);
		if (retval != ERROR_OK)
			return retval;
		wfar |= tmp;
		armv8_dpm_report_wfar(&armv8->dpm, wfar);
	}


	retval = armv8_dpm_read_current_registers(&armv8->dpm);

	{
	  struct arm_dpm *dpm = &armv8->dpm;
	  struct arm *arm = dpm->arm;
	  struct reg *pc;
	  uint32_t insn, insn1, insn2;
	  uint64_t pc_val;

	  pc = arm->core_cache->reg_list + 32; /* PC */
	  memcpy(&pc_val, pc->value, sizeof(pc_val));
	  (void)aarch64_read_memory_64(target,
				       pc_val,
				       sizeof(insn), 1,
				       (uint8_t *)&insn);
	  (void)aarch64_read_memory_64(target,
				       pc_val-4,
				       sizeof(insn1), 1,
				       (uint8_t *)&insn1);
	  (void)aarch64_read_memory_64(target,
				       pc_val+4,
				       sizeof(insn2), 1,
				       (uint8_t *)&insn2);
	  LOG_DEBUG("PC = 0x%0" PRIx64 " insn: 0x%08" PRIx32 " insn[-1]: 0x%08" PRIx32 " insn[+1]: 0x%08" PRIx32,
		    pc_val,
		    insn,
		    insn1,
		    insn2);
	}

	if (armv8->arm.core_state == ARM_STATE_AARCH64)
		target->type->pc_size = 64;
	else if (armv8->arm.core_state == ARM_STATE_ARM)
		target->type->pc_size = 32;

	if (armv8->post_debug_entry) {
		retval = armv8->post_debug_entry(target);
		if (retval != ERROR_OK)
			return retval;
	}

	return retval;
}

static int aarch64_post_debug_entry(struct target *target)
{
	struct aarch64_common *aarch64 = target_to_aarch64(target);
	struct armv8_common *armv8 = &aarch64->armv8_common;
	int retval;

	mem_ap_sel_write_atomic_u32(armv8->arm.dap, armv8->debug_ap,
				    armv8->debug_base + CPUV8_DBG_DRCR, 1<<2);
	switch (armv8->arm.core_mode) {
		case ARMV8_64_EL0T:
		case ARMV8_64_EL1T:
		case ARMV8_64_EL1H:
			retval = armv8->arm.mrs(target, 3, /*op 0*/
					0, 0,	/* op1, op2 */
					1, 0,	/* CRn, CRm */
					&aarch64->system_control_reg);
			if (retval != ERROR_OK)
				return retval;
		break;
		case ARMV8_64_EL2T:
		case ARMV8_64_EL2H:
			retval = armv8->arm.mrs(target, 3, /*op 0*/
					4, 0,	/* op1, op2 */
					1, 0,	/* CRn, CRm */
					&aarch64->system_control_reg);
			if (retval != ERROR_OK)
				return retval;
		break;
		case ARMV8_64_EL3H:
		case ARMV8_64_EL3T:
			retval = armv8->arm.mrs(target, 3, /*op 0*/
					6, 0,	/* op1, op2 */
					1, 0,	/* CRn, CRm */
					&aarch64->system_control_reg);
			if (retval != ERROR_OK)
				return retval;
		break;
		default:
			LOG_DEBUG("unknown cpu state 0x%x" PRIx32, armv8->arm.core_state);
	}
	LOG_DEBUG("System_register: %8.8" PRIx32, aarch64->system_control_reg);
	aarch64->system_control_reg_curr = aarch64->system_control_reg;

	if (armv8->armv8_mmu.armv8_cache.ctype == -1)
		armv8_identify_cache(target);

	armv8->armv8_mmu.mmu_enabled =
			(aarch64->system_control_reg & 0x1U) ? 1 : 0;
	armv8->armv8_mmu.armv8_cache.d_u_cache_enabled =
		(aarch64->system_control_reg & 0x4U) ? 1 : 0;
	armv8->armv8_mmu.armv8_cache.i_cache_enabled =
		(aarch64->system_control_reg & 0x1000U) ? 1 : 0;
	aarch64->curr_mode = armv8->arm.core_mode;
	return ERROR_OK;
}

static int aarch64_step(struct target *target, int current, uint32_t address,
	int handle_breakpoints)
{
	struct armv8_common *armv8 = target_to_armv8(target);
	struct adiv5_dap *swjdp = armv8->arm.dap;
	int retval;
	uint32_t tmp;

	if (target->state != TARGET_HALTED) {
		LOG_WARNING("target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	retval = mem_ap_sel_read_atomic_u32(swjdp, armv8->debug_ap,
			armv8->debug_base + CPUV8_DBG_EDECR, &tmp);
	if (retval != ERROR_OK)
		return retval;

	retval = mem_ap_sel_write_atomic_u32(swjdp, armv8->debug_ap,
			armv8->debug_base + CPUV8_DBG_EDECR, (tmp | EDECR_SS_HALTING_STEP_ENABLE));
	if (retval != ERROR_OK)
		return retval;

	retval = aarch64_resume(target, 1, address, 0, 0);
	if (retval != ERROR_OK)
		return retval;

	long long then = timeval_ms();
	while (target->state != TARGET_HALTED) {
		retval = aarch64_poll(target);
		if (retval != ERROR_OK)
			return retval;
		if (timeval_ms() > then + 2000) {
			LOG_ERROR("timeout waiting for target halt");
			return ERROR_FAIL;
		}
	}

	target->debug_reason = DBG_REASON_BREAKPOINT;
	retval = mem_ap_sel_write_atomic_u32(swjdp, armv8->debug_ap,
			armv8->debug_base + CPUV8_DBG_EDECR, (tmp&(~EDECR_SS_HALTING_STEP_ENABLE)));
	if (retval != ERROR_OK)
		return retval;

	if (target->state != TARGET_HALTED)
		LOG_DEBUG("target stepped");

	return ERROR_OK;
}

static int aarch64_restore_context(struct target *target, bool bpwp)
{
	struct armv8_common *armv8 = target_to_armv8(target);
	LOG_DEBUG(" ");
	if (armv8->pre_restore_context)
		armv8->pre_restore_context(target);

	return armv8_dpm_write_dirty_registers(&armv8->dpm, bpwp);

}

/*
 * Cortex-A8 Breakpoint and watchpoint functions
 */

static int aarch64_write_memory_64(struct target *target, uint64_t address,
				  uint32_t size, uint32_t count, const uint8_t *buffer);

/* Setup hardware Breakpoint Register Pair */
static int aarch64_set_breakpoint(struct target *target,
	struct breakpoint *breakpoint, uint8_t matchmode)
{
	int retval;
	int brp_i = 0;
	uint32_t control;
	uint8_t byte_addr_select = 0x0F;
	struct aarch64_common *aarch64 = target_to_aarch64(target);
	struct armv8_common *armv8 = &aarch64->armv8_common;
	struct aarch64_brp *brp_list = aarch64->brp_list;

	if (breakpoint->set) {
	  LOG_WARNING("breakpoint already set");
		return ERROR_OK;
	}

	if (breakpoint->type == BKPT_HARD) {
		while (brp_list[brp_i].used && (brp_i < aarch64->brp_num))
			brp_i++;
		if (brp_i >= aarch64->brp_num) {
			LOG_ERROR("ERROR Can not find free Breakpoint Register Pair");
			return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
		}
		breakpoint->set = brp_i + 1;
		if (breakpoint->length == 2)
			byte_addr_select = (3 << (breakpoint->address & 0x02));
		control = ((matchmode & 0x7) << 20)
			| (byte_addr_select << 5)
			| (3 << 1) | (1 << 13) | 1;
		brp_list[brp_i].used = 1;
		brp_list[brp_i].value = (breakpoint->address_64 & 0xFFFFFFFFFFFFFFFC);
		brp_list[brp_i].control = control;
		retval = aarch64_dap_write_memap_register_u32(target, armv8->debug_base
				+ CPUV8_DBG_BVR_BASE + 16 * brp_list[brp_i].BRPn,
				(uint32_t)(brp_list[brp_i].value & 0xFFFFFFFF));
		if (retval != ERROR_OK)
			return retval;

		retval = aarch64_dap_write_memap_register_u32(target, armv8->debug_base
				+ CPUV8_DBG_BVR_BASE + 4 + 16 * brp_list[brp_i].BRPn,
				(uint32_t)(brp_list[brp_i].value >> 32));
		if (retval != ERROR_OK)
			return retval;
		retval = aarch64_dap_write_memap_register_u32(target, armv8->debug_base
				+ CPUV8_DBG_BCR_BASE + 16 * brp_list[brp_i].BRPn,
				brp_list[brp_i].control);
		if (retval != ERROR_OK)
			return retval;
		LOG_DEBUG("brp %i control 0x%0" PRIx32 " value 0x%0" PRIx64, brp_i,
			brp_list[brp_i].control,
			brp_list[brp_i].value);
	} else if (breakpoint->type == BKPT_SOFT) {
		uint8_t code[4];

		LOG_DEBUG("BKPT_SOFT: address: 0x%0" PRIx64 " length: %d",
			  breakpoint->address_64,
			  breakpoint->length);

		buf_set_u32(code, 0, 32, ARMV8_HALT(0x11));
# if 1
		retval = aarch64_read_memory_64(target,
# else
		retval = target_read_memory(target,
# endif
# if 1
					    (breakpoint->address_64 & 0xFFFFFFFFFFFFFFFE), /* don't wipe upper 32 bits of address */
# else
					    breakpoint->address_64 & 0xFFFFFFFE,
# endif
					    sizeof(code), 1,
					    breakpoint->orig_instr);
		if (retval != ERROR_OK) {
		  return retval;
		}
		{
		  uint32_t old;
		  uint32_t halt;
		  memcpy(&old, breakpoint->orig_instr, 4);
		  memcpy(&halt, &code[0], 4);
		  LOG_DEBUG("BKPT_SOFT: old: 0x%08" PRIx32 " halt: 0x%08" PRIx32, old, halt);
		}
# if 1
		retval = aarch64_write_memory_64(target,
						(breakpoint->address_64 & 0xFFFFFFFFFFFFFFFE), /* don't wipe upper 32 bits of address */
						sizeof(code), 1, code);
# else
		retval = target_write_memory(target,
# if 1
				breakpoint->address_64 & 0xFFFFFFFFFFFFFFFE, /* don't wipe upper 32 bits of address */
# else
				breakpoint->address_64 & 0xFFFFFFFE,
# endif
				breakpoint->length, 1, code);
# endif
		if (retval != ERROR_OK)
			return retval;

		/*
		  Invalidate i-cache
		 */
		{
		  struct arm_dpm *dpm = armv8->arm.dpm;

		  retval = aarch64_instr_execute(dpm, 0xd5087500);
		}

		breakpoint->set = 0x11; /* Any nice value but 0 */
	}

	return ERROR_OK;
}


static int aarch64_set_context_breakpoint(struct target *target,
	struct breakpoint *breakpoint, uint8_t matchmode)
{
	int retval = ERROR_FAIL;
	int brp_i = 0;
	uint32_t control;
	uint8_t byte_addr_select = 0x0F;
	struct aarch64_common *aarch64 = target_to_aarch64(target);
	struct armv8_common *armv8 = &aarch64->armv8_common;
	struct aarch64_brp *brp_list = aarch64->brp_list;

	if (breakpoint->set) {
		LOG_WARNING("breakpoint already set");
		return retval;
	}
	/*check available context BRPs*/
	while ((brp_list[brp_i].used ||
		(brp_list[brp_i].type != BRP_CONTEXT)) && (brp_i < aarch64->brp_num))
		brp_i++;

	if (brp_i >= aarch64->brp_num) {
		LOG_ERROR("ERROR Can not find free Breakpoint Register Pair");
		return ERROR_FAIL;
	}

	breakpoint->set = brp_i + 1;
	control = ((matchmode & 0x7) << 20)
			| (byte_addr_select << 5)
			| (3 << 1) | (1 << 13) | 1;
	brp_list[brp_i].used = 1;
	brp_list[brp_i].value = (breakpoint->asid);
	brp_list[brp_i].control = control;
	retval = aarch64_dap_write_memap_register_u32(target, armv8->debug_base
			+ CPUV8_DBG_BVR_BASE + 16 * brp_list[brp_i].BRPn,
			(uint32_t)(brp_list[brp_i].value & 0xFFFFFFFF));
	if (retval != ERROR_OK)
		return retval;
	retval = aarch64_dap_write_memap_register_u32(target, armv8->debug_base
			+ CPUV8_DBG_BCR_BASE + 16 * brp_list[brp_i].BRPn,
			brp_list[brp_i].control);
	if (retval != ERROR_OK)
		return retval;
	LOG_DEBUG("brp %i control 0x%0" PRIx32 " value 0x%0" PRIx64, brp_i,
		brp_list[brp_i].control,
		brp_list[brp_i].value);
	return ERROR_OK;

}


static int aarch64_set_hybrid_breakpoint(struct target *target, struct breakpoint *breakpoint)
{
	int retval = ERROR_FAIL;
	int brp_1 = 0;	/* holds the contextID pair */
	int brp_2 = 0;	/* holds the IVA pair */
	uint32_t control_CTX, control_IVA;
	uint8_t CTX_byte_addr_select = 0x0F;
	uint8_t IVA_byte_addr_select = 0x0F;
	uint8_t CTX_machmode = 0x03;
	uint8_t IVA_machmode = 0x01;
	struct aarch64_common *aarch64 = target_to_aarch64(target);
	struct armv8_common *armv8 = &aarch64->armv8_common;
	struct aarch64_brp *brp_list = aarch64->brp_list;

	if (breakpoint->set) {
		LOG_WARNING("breakpoint already set");
		return retval;
	}
	/*check available context BRPs*/
	while ((brp_list[brp_1].used ||
		(brp_list[brp_1].type != BRP_CONTEXT)) && (brp_1 < aarch64->brp_num))
		brp_1++;

	printf("brp(CTX) found num: %d\n", brp_1);
	if (brp_1 >= aarch64->brp_num) {
		LOG_ERROR("ERROR Can not find free Breakpoint Register Pair");
		return ERROR_FAIL;
	}

	while ((brp_list[brp_2].used ||
		(brp_list[brp_2].type != BRP_NORMAL)) && (brp_2 < aarch64->brp_num))
		brp_2++;

	printf("brp(IVA) found num: %d\n", brp_2);
	if (brp_2 >= aarch64->brp_num) {
		LOG_ERROR("ERROR Can not find free Breakpoint Register Pair");
		return ERROR_FAIL;
	}

	breakpoint->set = brp_1 + 1;
	breakpoint->linked_BRP = brp_2;
	control_CTX = ((CTX_machmode & 0x7) << 20)
		| (brp_2 << 16)
		| (0 << 14)
		| (CTX_byte_addr_select << 5)
		| (3 << 1) | (1 << 13) | 1;
	brp_list[brp_1].used = 1;
	brp_list[brp_1].value = (breakpoint->asid);
	brp_list[brp_1].control = control_CTX;
	retval = aarch64_dap_write_memap_register_u32(target, armv8->debug_base
			+ CPUV8_DBG_BVR_BASE + 16 * brp_list[brp_1].BRPn,
			brp_list[brp_1].value);
	if (retval != ERROR_OK)
		return retval;
	retval = aarch64_dap_write_memap_register_u32(target, armv8->debug_base
			+ CPUV8_DBG_BCR_BASE + 16 * brp_list[brp_1].BRPn,
			brp_list[brp_1].control);
	if (retval != ERROR_OK)
		return retval;

	control_IVA = ((IVA_machmode & 0x7) << 20)
		| (brp_1 << 16)
		| (IVA_byte_addr_select << 5)
		| (3 << 1) | (1 << 13) | 1;
	brp_list[brp_2].used = 1;
	brp_list[brp_2].value = (breakpoint->address_64 & 0xFFFFFFFFFFFFFFFC);
	brp_list[brp_2].control = control_IVA;
	retval = aarch64_dap_write_memap_register_u32(target, armv8->debug_base
			+ CPUV8_DBG_BVR_BASE + 16 * brp_list[brp_2].BRPn,
			(uint32_t)(brp_list[brp_2].value & 0xFFFFFFFF));
	if (retval != ERROR_OK)
		return retval;
	retval = aarch64_dap_write_memap_register_u32(target, armv8->debug_base
			+ CPUV8_DBG_BVR_BASE + 4 + 16 * brp_list[brp_2].BRPn,
			(uint32_t)(brp_list[brp_2].value >> 32));
	if (retval != ERROR_OK)
		return retval;

	retval = aarch64_dap_write_memap_register_u32(target, armv8->debug_base
			+ CPUV8_DBG_BCR_BASE + 16 * brp_list[brp_2].BRPn,
			brp_list[brp_2].control);
	if (retval != ERROR_OK)
		return retval;

	return ERROR_OK;
}


static int aarch64_unset_breakpoint(struct target *target, struct breakpoint *breakpoint)
{
	int retval;
	struct aarch64_common *aarch64 = target_to_aarch64(target);
	struct armv8_common *armv8 = &aarch64->armv8_common;
	struct aarch64_brp *brp_list = aarch64->brp_list;

	if (!breakpoint->set) {
		LOG_WARNING("breakpoint not set");
		return ERROR_OK;
	}

	if (breakpoint->type == BKPT_HARD) {
		if ((breakpoint->address != 0) && (breakpoint->asid != 0)) {
			int brp_i = breakpoint->set - 1;
			int brp_j = breakpoint->linked_BRP;
			if ((brp_i < 0) || (brp_i >= aarch64->brp_num)) {
				LOG_DEBUG("Invalid BRP number in breakpoint");
				return ERROR_OK;
			}
			LOG_DEBUG("rbp %i control 0x%0" PRIx32 " value 0x%0" PRIx64, brp_i,
				brp_list[brp_i].control, brp_list[brp_i].value);
			brp_list[brp_i].used = 0;
			brp_list[brp_i].value = 0;
			brp_list[brp_i].control = 0;
			retval = aarch64_dap_write_memap_register_u32(target, armv8->debug_base
					+ CPUV8_DBG_BCR_BASE + 16 * brp_list[brp_i].BRPn,
					brp_list[brp_i].control);
			if (retval != ERROR_OK)
				return retval;
			retval = aarch64_dap_write_memap_register_u32(target, armv8->debug_base
					+ CPUV8_DBG_BVR_BASE + 16 * brp_list[brp_i].BRPn,
					(uint32_t)brp_list[brp_i].value);
			if (retval != ERROR_OK)
				return retval;
			retval = aarch64_dap_write_memap_register_u32(target, armv8->debug_base
					+ CPUV8_DBG_BVR_BASE + 4 + 16 * brp_list[brp_i].BRPn,
					(uint32_t)brp_list[brp_i].value);
			if (retval != ERROR_OK)
				return retval;
			if ((brp_j < 0) || (brp_j >= aarch64->brp_num)) {
				LOG_DEBUG("Invalid BRP number in breakpoint");
				return ERROR_OK;
			}
			LOG_DEBUG("rbp %i control 0x%0" PRIx32 " value 0x%0" PRIx64, brp_j,
				brp_list[brp_j].control, brp_list[brp_j].value);
			brp_list[brp_j].used = 0;
			brp_list[brp_j].value = 0;
			brp_list[brp_j].control = 0;
			retval = aarch64_dap_write_memap_register_u32(target, armv8->debug_base
					+ CPUV8_DBG_BCR_BASE + 16 * brp_list[brp_j].BRPn,
					brp_list[brp_j].control);
			if (retval != ERROR_OK)
				return retval;
			retval = aarch64_dap_write_memap_register_u32(target, armv8->debug_base
					+ CPUV8_DBG_BVR_BASE + 16 * brp_list[brp_j].BRPn,
					(uint32_t)brp_list[brp_j].value);
			if (retval != ERROR_OK)
				return retval;
			retval = aarch64_dap_write_memap_register_u32(target, armv8->debug_base
					+ CPUV8_DBG_BVR_BASE + 4 + 16 * brp_list[brp_j].BRPn,
					(uint32_t)brp_list[brp_j].value);
			if (retval != ERROR_OK)
				return retval;

			breakpoint->linked_BRP = 0;
			breakpoint->set = 0;
			return ERROR_OK;

		} else {
			int brp_i = breakpoint->set - 1;
			if ((brp_i < 0) || (brp_i >= aarch64->brp_num)) {
				LOG_DEBUG("Invalid BRP number in breakpoint");
				return ERROR_OK;
			}
			LOG_DEBUG("rbp %i control 0x%0" PRIx32 " value 0x%0" PRIx64, brp_i,
				brp_list[brp_i].control, brp_list[brp_i].value);
			brp_list[brp_i].used = 0;
			brp_list[brp_i].value = 0;
			brp_list[brp_i].control = 0;
			retval = aarch64_dap_write_memap_register_u32(target, armv8->debug_base
					+ CPUV8_DBG_BCR_BASE + 16 * brp_list[brp_i].BRPn,
					brp_list[brp_i].control);
			if (retval != ERROR_OK)
				return retval;
			retval = aarch64_dap_write_memap_register_u32(target, armv8->debug_base
					+ CPUV8_DBG_BVR_BASE + 16 * brp_list[brp_i].BRPn,
					(uint32_t)brp_list[brp_i].value);
			if (retval != ERROR_OK)
				return retval;

			retval = aarch64_dap_write_memap_register_u32(target, armv8->debug_base
					+ CPUV8_DBG_BVR_BASE + 4 + 16 * brp_list[brp_i].BRPn,
					(uint32_t)brp_list[brp_i].value);
			if (retval != ERROR_OK)
				return retval;
			breakpoint->set = 0;
			return ERROR_OK;
		}
	} else {
		/* restore original instruction (kept in target endianness) */
		if (breakpoint->length == 4) {
# if 1
		  {
		    uint32_t old;
		    memcpy(&old, breakpoint->orig_instr, 4);
		    LOG_DEBUG("Unset BKPT_SOFT: old: 0x%08" PRIx32, old);
		  }
		  retval = aarch64_write_memory_64(target,
						   (breakpoint->address_64 & 0xFFFFFFFFFFFFFFFE), /* don't wipe upper 32 bits of address */
						  4, 1, breakpoint->orig_instr);
		  /*
		    Invalidate i-cache
		  */
		  {
		    struct arm_dpm *dpm = armv8->arm.dpm;

		    retval = aarch64_instr_execute(dpm, 0xd5087500); /* IC IALLU */
		  }

# else
		  retval = target_write_memory(target,
					       breakpoint->address_64 & 0xFFFFFFFE,
					       4, 1, breakpoint->orig_instr);
# endif
		  if (retval != ERROR_OK)
		    return retval;
		}
		else {
		  retval = target_write_memory(target,
					       breakpoint->address_64 & 0xFFFFFFFE,
					       2, 1, breakpoint->orig_instr);
		  if (retval != ERROR_OK)
		    return retval;
		}
	}
	breakpoint->set = 0;

	return ERROR_OK;
}


static int aarch64_add_breakpoint(struct target *target,
	struct breakpoint *breakpoint)
{
	struct aarch64_common *aarch64 = target_to_aarch64(target);

	if ((breakpoint->type == BKPT_HARD) && (aarch64->brp_num_available < 1)) {
		LOG_INFO("no hardware breakpoint available");
		return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
	}

	if (breakpoint->type == BKPT_HARD)
		aarch64->brp_num_available--;

	return aarch64_set_breakpoint(target, breakpoint, 0x00);	/* Exact match */
}

static int aarch64_add_context_breakpoint(struct target *target,
	struct breakpoint *breakpoint)
{
	struct aarch64_common *aarch64 = target_to_aarch64(target);

	if ((breakpoint->type == BKPT_HARD) && (aarch64->brp_num_available < 1)) {
		LOG_INFO("no hardware breakpoint available");
		return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
	}

	if (breakpoint->type == BKPT_HARD)
		aarch64->brp_num_available--;

	return aarch64_set_context_breakpoint(target, breakpoint, 0x02);	/* asid match */
}

static int aarch64_add_hybrid_breakpoint(struct target *target,
	struct breakpoint *breakpoint)
{
	struct aarch64_common *aarch64 = target_to_aarch64(target);

	if ((breakpoint->type == BKPT_HARD) && (aarch64->brp_num_available < 1)) {
		LOG_INFO("no hardware breakpoint available");
		return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
	}

	if (breakpoint->type == BKPT_HARD)
		aarch64->brp_num_available--;

	return aarch64_set_hybrid_breakpoint(target, breakpoint);	/* ??? */
}


static int aarch64_remove_breakpoint(struct target *target, struct breakpoint *breakpoint)
{
	struct aarch64_common *aarch64 = target_to_aarch64(target);

#if 0
/* It is perfectly possible to remove breakpoints while the target is running */
	if (target->state != TARGET_HALTED) {
		LOG_WARNING("target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}
#endif

	if (breakpoint->set) {
		aarch64_unset_breakpoint(target, breakpoint);
		if (breakpoint->type == BKPT_HARD)
			aarch64->brp_num_available++;
	}

	return ERROR_OK;
}

/*
 * Cortex-A8 Reset functions
 */

static int aarch64_assert_reset(struct target *target)
{
	struct armv8_common *armv8 = target_to_armv8(target);

	LOG_DEBUG(" ");

	/* FIXME when halt is requested, make it work somehow... */

	/* Issue some kind of warm reset. */
	if (target_has_event_action(target, TARGET_EVENT_RESET_ASSERT))
		target_handle_event(target, TARGET_EVENT_RESET_ASSERT);
	else if (jtag_get_reset_config() & RESET_HAS_SRST) {
		/* REVISIT handle "pulls" cases, if there's
		 * hardware that needs them to work.
		 */
		jtag_add_reset(0, 1);
	} else {
		LOG_ERROR("%s: how to reset?", target_name(target));
		return ERROR_FAIL;
	}

	/* registers are now invalid */
	register_cache_invalidate(armv8->arm.core_cache);

	target->state = TARGET_RESET;

	return ERROR_OK;
}

static int aarch64_deassert_reset(struct target *target)
{
	int retval;

	LOG_DEBUG(" ");

	/* be certain SRST is off */
	jtag_add_reset(0, 0);

	for (;;) {
	  retval = aarch64_poll(target);
	  if (retval == ERROR_OK) {
	    break;
	  }
	}
	if (retval != ERROR_OK)
		return retval;

	if (target->reset_halt) {
		if (target->state != TARGET_HALTED) {
			LOG_WARNING("%s: ran after reset and before halt ...",
				target_name(target));
			retval = target_halt(target);
			if (retval != ERROR_OK)
				return retval;
		}
	}

	return ERROR_OK;
}

static int aarch64_write_apb_ab_memory(struct target *target,
	uint32_t address, uint32_t size,
	uint32_t count, const uint8_t *buffer)
{
	/* write memory through APB-AP */
	int retval = ERROR_COMMAND_SYNTAX_ERROR;
	struct armv8_common *armv8 = target_to_armv8(target);
	struct arm *arm = &armv8->arm;
	struct adiv5_dap *swjdp = armv8->arm.dap;
	int total_bytes = count * size;
	int total_u32;
	int start_byte = address & 0x3;
	int end_byte   = (address + total_bytes) & 0x3;
	struct reg *reg;
	uint32_t dscr;
	uint8_t *tmp_buff = NULL;

	LOG_DEBUG("Writing APB-AP memory address 0x%" PRIx32 " size %"	PRIu32 " count %"  PRIu32,
			  address, size, count);
	if (target->state != TARGET_HALTED) {
		LOG_WARNING("target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	total_u32 = DIV_ROUND_UP((address & 3) + total_bytes, 4);

	/* Mark register R0 as dirty, as it will be used
	 * for transferring the data.
	 * It will be restored automatically when exiting
	 * debug mode
	 */
	reg = armv8_reg_current(arm, 1);
	reg->dirty = true;

	reg = armv8_reg_current(arm, 0);
	reg->dirty = true;

	/*	clear any abort  */
	retval = mem_ap_sel_write_atomic_u32(swjdp, armv8->debug_ap, armv8->debug_base + CPUV8_DBG_DRCR, 1<<2);
	if (retval != ERROR_OK)
		return retval;

	/* This algorithm comes from either :
	 * Cortex-A TRM Example 12-25
	 * Cortex-R4 TRM Example 11-26
	 * (slight differences)
	 */

	/* The algorithm only copies 32 bit words, so the buffer
	 * should be expanded to include the words at either end.
	 * The first and last words will be read first to avoid
	 * corruption if needed.
	 */
	tmp_buff = malloc(total_u32 * 4);

	if ((start_byte != 0) && (total_u32 > 1)) {
		/* First bytes not aligned - read the 32 bit word to avoid corrupting
		 * the other bytes in the word.
		 */
		retval = aarch64_read_apb_ab_memory(target, (address & ~0x3), 4, 1, tmp_buff);
		if (retval != ERROR_OK)
			goto error_free_buff_w;
	}

	/* If end of write is not aligned, or the write is less than 4 bytes */
	if ((end_byte != 0) ||
		((total_u32 == 1) && (total_bytes != 4))) {

		/* Read the last word to avoid corruption during 32 bit write */
		int mem_offset = (total_u32-1) * 4;
		retval = aarch64_read_apb_ab_memory(target, (address & ~0x3) + mem_offset, 4, 1, &tmp_buff[mem_offset]);
		if (retval != ERROR_OK)
			goto error_free_buff_w;
	}

	/* Copy the write buffer over the top of the temporary buffer */
	memcpy(&tmp_buff[start_byte], buffer, total_bytes);

	/* We now have a 32 bit aligned buffer that can be written */

	/* Read DSCR */
	retval = mem_ap_sel_read_atomic_u32(swjdp, armv8->debug_ap,
			armv8->debug_base + CPUV8_DBG_DSCR, &dscr);
	if (retval != ERROR_OK)
		goto error_free_buff_w;

	/* Set DTR mode to Normal*/
	dscr = (dscr & ~DSCR_MA);
	retval = mem_ap_sel_write_atomic_u32(swjdp, armv8->debug_ap,
			armv8->debug_base + CPUV8_DBG_DSCR, dscr);
	if (retval != ERROR_OK)
		goto error_free_buff_w;

	/* Write X0 with value 'address' using write procedure */
	/*	 - Write the address for read access into DTRRX */
	retval += mem_ap_sel_write_atomic_u32(swjdp, armv8->debug_ap,
			armv8->debug_base + CPUV8_DBG_DTRRX, address & ~0x3);
	/*	- Copy value from DTRRX to R0 using instruction mrs DCCRX, x0 */
	aarch64_exec_opcode(target, ARMV8_MRS(SYSTEM_DBG_DTRRX_EL0, 0), &dscr);

	/* change DCC to memory mode
	 * in one combined write (since they are adjacent registers)
	 */
	dscr = (dscr & ~DSCR_MA) | DSCR_MA;
	retval +=  mem_ap_sel_write_atomic_u32(swjdp, armv8->debug_ap,
			armv8->debug_base + CPUV8_DBG_DSCR, dscr);

	if (retval != ERROR_OK)
		goto error_unset_dtr_w;


	/* Do the write */
	retval = mem_ap_sel_write_buf_noincr(swjdp, armv8->debug_ap,
					tmp_buff, 4, total_u32, armv8->debug_base + CPUV8_DBG_DTRRX);
	if (retval != ERROR_OK)
		goto error_unset_dtr_w;


	/* Switch DTR mode back to Normal mode */
	dscr = (dscr & ~DSCR_MA);
	retval = mem_ap_sel_write_atomic_u32(swjdp, armv8->debug_ap,
				armv8->debug_base + CPUV8_DBG_DSCR, dscr);
	if (retval != ERROR_OK)
		goto error_unset_dtr_w;

	/* Check for sticky abort flags in the DSCR */
	retval = mem_ap_sel_read_atomic_u32(swjdp, armv8->debug_ap,
				armv8->debug_base + CPUV8_DBG_DSCR, &dscr);
	if (retval != ERROR_OK)
		goto error_free_buff_w;
	if (dscr & (DSCR_ERR | DSCR_SYS_ERROR_PEND)) {
		/* Abort occurred - clear it and exit */
		LOG_ERROR("abort occurred - dscr = 0x%08" PRIx32, dscr);
		mem_ap_sel_write_atomic_u32(swjdp, armv8->debug_ap,
					armv8->debug_base + CPUV8_DBG_DRCR, 1<<2);
		goto error_free_buff_w;
	}

	/* Done */
	free(tmp_buff);
	return ERROR_OK;

error_unset_dtr_w:
	/* Unset DTR mode */
	mem_ap_sel_read_atomic_u32(swjdp, armv8->debug_ap,
				armv8->debug_base + CPUV8_DBG_DSCR, &dscr);
	dscr = (dscr & ~DSCR_MA);
	mem_ap_sel_write_atomic_u32(swjdp, armv8->debug_ap,
				armv8->debug_base + CPUV8_DBG_DSCR, dscr);
error_free_buff_w:
	LOG_ERROR("error");
	free(tmp_buff);
	return ERROR_FAIL;
}

static int aarch64_write_apb_ab_memory64(struct target *target,
					 uint64_t address, uint32_t size,
					 uint32_t count, const uint8_t *buffer)
{
	/* write memory through APB-AP */
	int retval = ERROR_COMMAND_SYNTAX_ERROR;
	struct armv8_common *armv8 = target_to_armv8(target);
	struct arm *arm = &armv8->arm;
	struct adiv5_dap *swjdp = armv8->arm.dap;
	int total_bytes = count * size;
	int total_u32;
	int start_byte = address & 0x3;
	int end_byte   = (address + total_bytes) & 0x3;
	struct reg *reg;
	uint32_t dscr;
	uint8_t *tmp_buff = NULL;
	uint32_t i = 0;

	LOG_DEBUG("Writing APB-AP memory address 0x%" PRIx64 " size %"  PRIu32 " count %"  PRIu32,
			  address, size, count);
	if (target->state != TARGET_HALTED) {
		LOG_WARNING("target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	total_u32 = DIV_ROUND_UP((address & 3) + total_bytes, 4);

	/* Mark register R0 as dirty, as it will be used
	 * for transferring the data.
	 * It will be restored automatically when exiting
	 * debug mode
	 */
	reg = armv8_reg_current(arm, 1);
	reg->dirty = true;

	reg = armv8_reg_current(arm, 0);
	reg->dirty = true;

	/*  clear any abort  */
	retval = mem_ap_sel_write_atomic_u32(swjdp, armv8->debug_ap, armv8->debug_base + CPUV8_DBG_DRCR, 1<<2);
	if (retval != ERROR_OK)
		return retval;

	/* This algorithm comes from either :
	 * Cortex-A8 TRM Example 12-25
	 * Cortex-R4 TRM Example 11-26
	 * (slight differences)
	 */

	/* The algorithm only copies 32 bit words, so the buffer
	 * should be expanded to include the words at either end.
	 * The first and last words will be read first to avoid
	 * corruption if needed.
	 */
	tmp_buff = malloc(total_u32 * 4);

	if ((start_byte != 0) && (total_u32 > 1)) {
		/* First bytes not aligned - read the 32 bit word to avoid corrupting
		 * the other bytes in the word.
		 */
		retval = aarch64_read_apb_ab_memory(target, (address & ~0x3), 4, 1, tmp_buff);
		if (retval != ERROR_OK)
			goto error_free_buff_w;
	}

	/* If end of write is not aligned, or the write is less than 4 bytes */
	if ((end_byte != 0) ||
		((total_u32 == 1) && (total_bytes != 4))) {

		/* Read the last word to avoid corruption during 32 bit write */
		int mem_offset = (total_u32-1) * 4;
		retval = aarch64_read_apb_ab_memory(target, (address & ~0x3) + mem_offset, 4, 1, &tmp_buff[mem_offset]);
		if (retval != ERROR_OK)
			goto error_free_buff_w;
	}

	/* Copy the write buffer over the top of the temporary buffer */
	memcpy(&tmp_buff[start_byte], buffer, total_bytes);

	/* We now have a 32 bit aligned buffer that can be written */

	/* Read DSCR */
	retval = mem_ap_sel_read_atomic_u32(swjdp, armv8->debug_ap,
			armv8->debug_base + CPUV8_DBG_DSCR, &dscr);
	if (retval != ERROR_OK)
		goto error_free_buff_w;

	/* Set DTR mode to Normal*/
	dscr = (dscr & ~DSCR_EXT_DCC_MASK) | DSCR_EXT_DCC_NON_BLOCKING;
	retval = mem_ap_sel_write_atomic_u32(swjdp, armv8->debug_ap,
					     armv8->debug_base + CPUV8_DBG_DSCR, dscr);
	if (retval != ERROR_OK)
		goto error_free_buff_w;

	if (size > 4) {
		LOG_WARNING("reading size >4 bytes not yet supported");
		goto error_unset_dtr_w;
	}

	retval = aarch64_instr_write_data_dcc_64(arm->dpm, 0xd5330401, address + 4);
	if (retval != ERROR_OK)
		goto error_unset_dtr_w;

	dscr = DSCR_INSTR_COMP;
	while (i < count * size) {
		uint32_t val;

		memcpy(&val, &buffer[i], size);
		retval = aarch64_instr_write_data_dcc(arm->dpm, 0xd5330500, val);
		if (retval != ERROR_OK)
			goto error_unset_dtr_w;

		retval = aarch64_exec_opcode(target, 0xb81fc020, &dscr);
		if (retval != ERROR_OK)
			goto error_unset_dtr_w;

		retval = aarch64_exec_opcode(target, 0x91001021, &dscr);
		if (retval != ERROR_OK)
			goto error_unset_dtr_w;

		i += 4;
	}

	/* Check for sticky abort flags in the DSCR */
	retval = mem_ap_sel_read_atomic_u32(swjdp, armv8->debug_ap,
				armv8->debug_base + CPUV8_DBG_DSCR, &dscr);
	if (retval != ERROR_OK)
		goto error_free_buff_w;
	if (dscr & (DSCR_STICKY_ABORT_PRECISE | DSCR_STICKY_ABORT_IMPRECISE)) {
		/* Abort occurred - clear it and exit */
		LOG_ERROR("abort occurred - dscr = 0x%08" PRIx32, dscr);
		mem_ap_sel_write_atomic_u32(swjdp, armv8->debug_ap,
					armv8->debug_base + CPUV8_DBG_DRCR, 1<<2);
		goto error_free_buff_w;
	}

	/* Done */
	free(tmp_buff);
	return ERROR_OK;

error_unset_dtr_w:
	/* Unset DTR mode */
	mem_ap_sel_read_atomic_u32(swjdp, armv8->debug_ap,
				armv8->debug_base + CPUV8_DBG_DSCR, &dscr);
	dscr = (dscr & ~DSCR_EXT_DCC_MASK) | DSCR_EXT_DCC_NON_BLOCKING;
	mem_ap_sel_write_atomic_u32(swjdp, armv8->debug_ap,
				armv8->debug_base + CPUV8_DBG_DSCR, dscr);
error_free_buff_w:
	LOG_ERROR("error");
	free(tmp_buff);
	return ERROR_FAIL;
}

# if 1 /* new */
# define DSCR_INSTR_COMP             (0x1 << 24)
static int aarch64_read_apb_ab_memory(struct target *target,
	uint64_t address, uint32_t size,
	uint32_t count, uint8_t *buffer)
{
	/* read memory through APB-AP */

	int retval = ERROR_COMMAND_SYNTAX_ERROR;
	struct armv8_common *armv8 = target_to_armv8(target);
	struct adiv5_dap *swjdp = armv8->arm.dap;
	struct arm *arm = &armv8->arm;
	struct reg *reg;
	uint32_t dscr, val;
	uint8_t *tmp_buff = NULL;
	uint32_t i = 0;
	uint32_t o_size = size;

	LOG_DEBUG("Reading APB-AP memory address 0x%" PRIx64 " size %"  PRIu32 " count %"  PRIu32,
			  address, size, count);
	if (target->state != TARGET_HALTED) {
		LOG_WARNING("target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	/* Mark register R0 as dirty, as it will be used
	 * for transferring the data.
	 * It will be restored automatically when exiting
	 * debug mode
	 */
	reg = armv8_reg_current(arm, 0);
	reg->dirty = true;

	/*  clear any abort  */
	retval = mem_ap_sel_write_atomic_u32(swjdp, armv8->debug_ap,
		armv8->debug_base + CPUV8_DBG_DRCR, 1<<2);
	if (retval != ERROR_OK)
		goto error_free_buff_r;

	retval = mem_ap_sel_read_atomic_u32(swjdp, armv8->debug_ap,
			armv8->debug_base + CPUV8_DBG_DSCR, &dscr);
	if (retval != ERROR_OK)
		goto error_unset_dtr_r;

	if (size > 4) {
	  if (size == 8) {
	    o_size = 4;
	    count = count * 2;	    
	  }
	  else {
	    LOG_WARNING("reading size %d bytes not yet supported", size);
	    goto error_unset_dtr_r;
	  }
	}

	while (i < count * o_size) {

		retval = aarch64_instr_write_data_dcc_64(arm->dpm, 0xd5330400, address+4);
		if (retval != ERROR_OK)
			goto error_unset_dtr_r;
		retval = mem_ap_sel_read_atomic_u32(swjdp, armv8->debug_ap,
			armv8->debug_base + CPUV8_DBG_DSCR, &dscr);

		dscr = DSCR_INSTR_COMP;
		retval = aarch64_exec_opcode(target, 0xb85fc000, &dscr);
		if (retval != ERROR_OK)
			goto error_unset_dtr_r;
		retval = mem_ap_sel_read_atomic_u32(swjdp, armv8->debug_ap,
			armv8->debug_base + CPUV8_DBG_DSCR, &dscr);

		retval = aarch64_instr_read_data_dcc(arm->dpm, 0xd5130400, &val);
		if (retval != ERROR_OK)
			goto error_unset_dtr_r;
		memcpy(&buffer[i], &val, o_size);
		i += 4;
		address += 4;
	}

	/* Clear any sticky error */
	mem_ap_sel_write_atomic_u32(swjdp, armv8->debug_ap,
		armv8->debug_base + CPUV8_DBG_DRCR, 1<<2);

	/* Done */
	return ERROR_OK;

error_unset_dtr_r:
	LOG_WARNING("DSCR = 0x%" PRIx32, dscr);
	/* Todo: Unset DTR mode */

error_free_buff_r:
	LOG_ERROR("error");
	free(tmp_buff);

	/* Clear any sticky error */
	mem_ap_sel_write_atomic_u32(swjdp, armv8->debug_ap,
		armv8->debug_base + CPUV8_DBG_DRCR, 1<<2);

	return ERROR_FAIL;
}
# else /* old */
static int aarch64_read_apb_ab_memory(struct target *target,
	uint64_t address, uint32_t size,
	uint32_t count, uint8_t *buffer)
{
	/* read memory through APB-AP */
	int retval = ERROR_COMMAND_SYNTAX_ERROR;
	struct armv8_common *armv8 = target_to_armv8(target);
	struct adiv5_dap *swjdp = armv8->arm.dap;
	struct arm *arm = &armv8->arm;
	int total_bytes = count * size;
	int total_u32;
	int start_byte = address & 0x3;
	int end_byte   = (address + total_bytes) & 0x3;
	struct reg *reg;
	uint32_t dscr;
	uint8_t *tmp_buff = NULL;
	uint8_t *u8buf_ptr;

	LOG_DEBUG("Reading APB-AP memory address 0x%" PRIx64 " size %"	PRIu32 " count %"  PRIu32,
			  address, size, count);
	if (target->state != TARGET_HALTED) {
		LOG_WARNING("target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	total_u32 = DIV_ROUND_UP((address & 3) + total_bytes, 4);
	/* Mark register X0, X1 as dirty, as it will be used
	 * for transferring the data.
	 * It will be restored automatically when exiting
	 * debug mode
	 */
	reg = armv8_reg_current(arm, 1);
	reg->dirty = true;

	reg = armv8_reg_current(arm, 0);
	reg->dirty = true;

	/*	clear any abort  */
	retval =
		mem_ap_sel_write_atomic_u32(swjdp, armv8->debug_ap, armv8->debug_base + CPUV8_DBG_DRCR, 1<<2);
	if (retval != ERROR_OK)
		goto error_free_buff_r;

	/* Read DSCR */
	retval = mem_ap_sel_read_atomic_u32(swjdp, armv8->debug_ap,
			armv8->debug_base + CPUV8_DBG_DSCR, &dscr);

	/* This algorithm comes from either :
	 * Cortex-A TRM Example 12-24
	 * Cortex-R4 TRM Example 11-25
	 * (slight differences)
	 */

	/* Set Normal access mode  */
	dscr = (dscr & ~DSCR_MA);
	retval +=  mem_ap_sel_write_atomic_u32(swjdp, armv8->debug_ap,
			armv8->debug_base + CPUV8_DBG_DSCR, dscr);

	/* Write X0 with value 'address' using write procedure */
	/*	 - Write the address for read access into DTRRX */
	retval += mem_ap_sel_write_atomic_u32(swjdp, armv8->debug_ap,
			armv8->debug_base + CPUV8_DBG_DTRRX, address & ~0x3);

	/*
	  Top 32 bits
	*/
	retval += mem_ap_sel_write_atomic_u32(swjdp, armv8->debug_ap,
			armv8->debug_base + CPUV8_DBG_DTRTX, (address >> 32) & 0xffffffff );

	/*	- Copy value from DTRRX to R0 using instruction mrs DCCRX, x0 */
	aarch64_exec_opcode(target, ARMV8_MRS(SYSTEM_DBG_DTRRX_EL0, 0), &dscr);

	/*
	  Discard 1st read
	*/
	{
	  uint32_t scratch = 0;

	  retval = aarch64_exec_opcode(target,
				       ARMV8_MSR_GP(SYSTEM_DBG_DTRTX_EL0, 0),  /* msr dbgdtrtx_el0, x0 */
				       &scratch);  

	  scratch = 0;

	  retval = mem_ap_sel_read_atomic_u32(swjdp, armv8->debug_ap,
					      armv8->debug_base + CPUV8_DBG_DTRTX, &scratch);
	  LOG_DEBUG("dummy read - scratch = 0x%08" PRIx32, scratch);
	}

	/* change DCC to memory mode
	 * in one combined write (since they are adjacent registers)
	 */
	dscr = (dscr & ~DSCR_MA)|DSCR_MA;
	retval +=  mem_ap_sel_write_atomic_u32(swjdp, armv8->debug_ap,
			armv8->debug_base + CPUV8_DBG_DSCR, dscr);
	if (retval != ERROR_OK)
		goto error_unset_dtr_r;

	/* Optimize the read as much as we can, either way we read in a single pass  */
	if ((start_byte) || (end_byte)) {
		/* The algorithm only copies 32 bit words, so the buffer
		 * should be expanded to include the words at either end.
		 * The first and last words will be read into a temp buffer
		 * to avoid corruption
		 */
		tmp_buff = malloc(total_u32 * 4);
		if (!tmp_buff)
			goto error_unset_dtr_r;

		/* use the tmp buffer to read the entire data */
		u8buf_ptr = tmp_buff;
	} else
		/* address and read length are aligned so read directely into the passed buffer */
		u8buf_ptr = buffer;

	/* Read the data - Each read of the DTRTX register causes the instruction to be reissued
	 * Abort flags are sticky, so can be read at end of transactions
	 *
	 * This data is read in aligned to 32 bit boundary.
	 */
	retval = mem_ap_sel_read_buf_noincr(swjdp, armv8->debug_ap, u8buf_ptr, 4, total_u32,
									armv8->debug_base + CPUV8_DBG_DTRTX);
	if (retval != ERROR_OK)
			goto error_unset_dtr_r;

	/* set DTR access mode back to Normal mode	*/
	dscr = (dscr & ~DSCR_MA);
	retval =  mem_ap_sel_write_atomic_u32(swjdp, armv8->debug_ap,
					armv8->debug_base + CPUV8_DBG_DSCR, dscr);
	if (retval != ERROR_OK)
		goto error_free_buff_r;

	/* Wait for the final read instruction to finish */
	do {
		retval = mem_ap_sel_read_atomic_u32(swjdp, armv8->debug_ap,
					armv8->debug_base + CPUV8_DBG_DSCR, &dscr);
		if (retval != ERROR_OK)
			goto error_free_buff_r;
	} while ((dscr & DSCR_ITE) == 0);

	/* Check for sticky abort flags in the DSCR */
	retval = mem_ap_sel_read_atomic_u32(swjdp, armv8->debug_ap,
				armv8->debug_base + CPUV8_DBG_DSCR, &dscr);
	if (retval != ERROR_OK)
		goto error_free_buff_r;
	if (dscr & (DSCR_ERR | DSCR_SYS_ERROR_PEND)) {
		/* Abort occurred - clear it and exit */
		LOG_ERROR("abort occurred - dscr = 0x%08" PRIx32, dscr);
		mem_ap_sel_write_atomic_u32(swjdp, armv8->debug_ap,
					armv8->debug_base + CPUV8_DBG_DRCR, 1<<2);
		goto error_free_buff_r;
	}

	/* check if we need to copy aligned data by applying any shift necessary */
	if (tmp_buff) {
		memcpy(buffer, tmp_buff + start_byte, total_bytes);
		free(tmp_buff);
	}

	/* Done */
	return ERROR_OK;

error_unset_dtr_r:
	/* Unset DTR mode */
	mem_ap_sel_read_atomic_u32(swjdp, armv8->debug_ap,
				armv8->debug_base + CPUV8_DBG_DSCR, &dscr);
	dscr = (dscr & ~DSCR_MA);
	mem_ap_sel_write_atomic_u32(swjdp, armv8->debug_ap,
				armv8->debug_base + CPUV8_DBG_DSCR, dscr);
error_free_buff_r:
	LOG_ERROR("error");
	free(tmp_buff);
	return ERROR_FAIL;
}
# endif


/*
 * Cortex-A8 Memory access
 *
 * This is same Cortex M3 but we must also use the correct
 * ap number for every access.
 */

static int aarch64_read_phys_memory(struct target *target,
	uint32_t address, uint32_t size,
	uint32_t count, uint8_t *buffer)
{
	struct armv8_common *armv8 = target_to_armv8(target);
	struct adiv5_dap *swjdp = armv8->arm.dap;
	int retval = ERROR_COMMAND_SYNTAX_ERROR;
	uint8_t apsel = swjdp->apsel;
	LOG_DEBUG("Reading memory at real address 0x%" PRIx32 "; size %" PRId32 "; count %" PRId32,
		address, size, count);

	if (count && buffer) {

		if (armv8->memory_ap_available && (apsel == armv8->memory_ap)) {

			/* read memory through AHB-AP */
			retval = mem_ap_sel_read_buf(swjdp, armv8->memory_ap, buffer, size, count, address);
		} else {
			/* read memory through APB-AP */
			retval = aarch64_mmu_modify(target, 0);
			if (retval != ERROR_OK)
				return retval;
			retval = aarch64_read_apb_ab_memory(target, address, size, count, buffer);
		}
	}
	return retval;
}

static int aarch64_read_phys_memory_64(struct target *target,
	uint64_t address, uint32_t size,
	uint32_t count, uint8_t *buffer)
{
	LOG_DEBUG("Reading memory at real address 0x%" PRIx64 "; size %" PRId32 "; count %" PRId32,
		address, size, count);
	LOG_ERROR("physical memory access not supportted");

	return ERROR_FAIL;
}

static int aarch64_read_memory(struct target *target, uint32_t address,
	uint32_t size, uint32_t count, uint8_t *buffer)
{
	int mmu_enabled = 0;
	uint32_t virt, phys;
	int retval;
	struct armv8_common *armv8 = target_to_armv8(target);
	struct adiv5_dap *swjdp = armv8->arm.dap;
	uint8_t apsel = swjdp->apsel;

	/* aarch64 handles unaligned memory access */
	LOG_DEBUG("Reading memory at address 0x%" PRIx32 "; size %" PRId32 "; count %" PRId32, address,
		size, count);

	/* determine if MMU was enabled on target stop */
	if (!armv8->is_armv7r) {
		retval = aarch64_mmu(target, &mmu_enabled);
		if (retval != ERROR_OK)
			return retval;
	}

	if (armv8->memory_ap_available && (apsel == armv8->memory_ap)) {
		if (mmu_enabled) {
			virt = address;
			retval = aarch64_virt2phys(target, virt, &phys);
			if (retval != ERROR_OK)
				return retval;

			LOG_DEBUG("Reading at virtual address. Translating v:0x%" PRIx32 " to r:0x%" PRIx32,
				  virt, phys);
			address = phys;
		}
		retval = aarch64_read_phys_memory(target, address, size, count, buffer);
	} else {
		if (mmu_enabled) {
			retval = aarch64_check_address(target, address);
			if (retval != ERROR_OK)
				return retval;
			/* enable MMU as we could have disabled it for phys access */
			retval = aarch64_mmu_modify(target, 1);
			if (retval != ERROR_OK)
				return retval;
		}
		retval = aarch64_read_apb_ab_memory(target, address, size, count, buffer);
	}
	return retval;
}

static int aarch64_read_memory_64(struct target *target, uint64_t address,
	uint32_t size, uint32_t count, uint8_t *buffer)
{
	int mmu_enabled = 0;
	uint32_t virt, phys;
	int retval;
	struct armv8_common *armv8 = target_to_armv8(target);
	struct adiv5_dap *swjdp = armv8->arm.dap;
	uint8_t apsel = swjdp->apsel;

	/* aarch64 handles unaligned memory access */
	LOG_DEBUG("Reading memory at address 0x%" PRIx64 "; size %" PRId32 "; count %" PRId32, address,
		size, count);

	/* determine if MMU was enabled on target stop */
	if (!armv8->is_armv7r) {
		retval = aarch64_mmu(target, &mmu_enabled);
		if (retval != ERROR_OK)
			return retval;
	}

	if (armv8->memory_ap_available && (apsel == armv8->memory_ap)) {
		if (mmu_enabled) {
			virt = address;
			retval = aarch64_virt2phys(target, virt, &phys);
			if (retval != ERROR_OK)
				return retval;

			LOG_DEBUG("Reading at virtual address. Translating v:0x%" PRIx32 " to r:0x%" PRIx32,
				  virt, phys);
			address = phys;
		}
		retval = aarch64_read_phys_memory(target, address, size, count, buffer);
	} else {
		if (mmu_enabled) {
			retval = aarch64_check_address(target, address);
			if (retval != ERROR_OK)
				return retval;
			/* enable MMU as we could have disabled it for phys access */
			retval = aarch64_mmu_modify(target, 1);
			if (retval != ERROR_OK)
				return retval;
		}
		retval = aarch64_read_apb_ab_memory(target, address, size, count, buffer);
	}
	return retval;
}

static int aarch64_write_phys_memory(struct target *target,
	uint32_t address, uint32_t size,
	uint32_t count, const uint8_t *buffer)
{
	struct armv8_common *armv8 = target_to_armv8(target);
	struct adiv5_dap *swjdp = armv8->arm.dap;
	int retval = ERROR_COMMAND_SYNTAX_ERROR;
	uint8_t apsel = swjdp->apsel;

	LOG_DEBUG("Writing memory to real address 0x%" PRIx32 "; size %" PRId32 "; count %" PRId32, address,
		size, count);

	if (count && buffer) {

		if (armv8->memory_ap_available && (apsel == armv8->memory_ap)) {

			/* write memory through AHB-AP */
			retval = mem_ap_sel_write_buf(swjdp, armv8->memory_ap, buffer, size, count, address);
		} else {

			/* write memory through APB-AP */
			retval = aarch64_mmu_modify(target, 0);
			if (retval != ERROR_OK)
				return retval;
			return aarch64_write_apb_ab_memory(target, address, size, count, buffer);
		}
	}


	/* REVISIT this op is generic ARMv7-A/R stuff */
	if (retval == ERROR_OK && target->state == TARGET_HALTED) {
		struct arm_dpm *dpm = armv8->arm.dpm;

		retval = dpm->prepare(dpm);
		if (retval != ERROR_OK)
			return retval;

		/* The Cache handling will NOT work with MMU active, the
		 * wrong addresses will be invalidated!
		 *
		 * For both ICache and DCache, walk all cache lines in the
		 * address range. Cortex-A has fixed 64 byte line length.
		 *
		 * REVISIT per ARMv7, these may trigger watchpoints ...
		 */

		/* invalidate I-Cache */
		if (armv8->armv8_mmu.armv8_cache.i_cache_enabled) {
			/* ICIMVAU - Invalidate Cache single entry
			 * with MVA to PoU
			 *		MCR p15, 0, r0, c7, c5, 1
			 */
			for (uint32_t cacheline = 0;
				cacheline < size * count;
				cacheline += 64) {
				retval = dpm->instr_write_data_r0(dpm,
						ARMV8_MSR_GP(SYSTEM_ICIVAU, 0),
						address + cacheline);
				if (retval != ERROR_OK)
					return retval;
			}
		}

		/* invalidate D-Cache */
		if (armv8->armv8_mmu.armv8_cache.d_u_cache_enabled) {
			/* DCIMVAC - Invalidate data Cache line
			 * with MVA to PoC
			 *		MCR p15, 0, r0, c7, c6, 1
			 */
			for (uint32_t cacheline = 0;
				cacheline < size * count;
				cacheline += 64) {
				retval = dpm->instr_write_data_r0(dpm,
						ARMV8_MSR_GP(SYSTEM_DCCVAU, 0),
						address + cacheline);
				if (retval != ERROR_OK)
					return retval;
			}
		}

		/* (void) */ dpm->finish(dpm);
	}

	return retval;
}


static int aarch64_write_memory(struct target *target, uint32_t address,
	uint32_t size, uint32_t count, const uint8_t *buffer)
{
	int mmu_enabled = 0;
	uint32_t virt, phys;
	int retval;
	struct armv8_common *armv8 = target_to_armv8(target);
	struct adiv5_dap *swjdp = armv8->arm.dap;
	uint8_t apsel = swjdp->apsel;

	/* aarch64 handles unaligned memory access */
	LOG_DEBUG("Writing memory at address 0x%" PRIx32 "; size %" PRId32 "; count %" PRId32, address,
		size, count);

	/* determine if MMU was enabled on target stop */
	if (!armv8->is_armv7r) {
		retval = aarch64_mmu(target, &mmu_enabled);
		if (retval != ERROR_OK)
			return retval;
	}

	if (armv8->memory_ap_available && (apsel == armv8->memory_ap)) {
		LOG_DEBUG("Writing memory to address 0x%" PRIx32 "; size %" PRId32 "; count %" PRId32, address, size,
			count);
		if (mmu_enabled) {
			virt = address;
			retval = aarch64_virt2phys(target, virt, &phys);
			if (retval != ERROR_OK)
				return retval;

			LOG_DEBUG("Writing to virtual address. Translating v:0x%" PRIx32 " to r:0x%" PRIx32,
				  virt,
				  phys);
			address = phys;
		}
		retval = aarch64_write_phys_memory(target, address, size,
				count, buffer);
	} else {
		if (mmu_enabled) {
			retval = aarch64_check_address(target, address);
			if (retval != ERROR_OK)
				return retval;
			/* enable MMU as we could have disabled it for phys access */
			retval = aarch64_mmu_modify(target, 1);
			if (retval != ERROR_OK)
				return retval;
		}
		retval = aarch64_write_apb_ab_memory(target, address, size, count, buffer);
	}
	return retval;
}

static int aarch64_write_phys_memory64(struct target *target,
				       uint64_t address, uint32_t size,
				       uint32_t count, const uint8_t *buffer)
{
	struct armv8_common *armv8 = target_to_armv8(target);
	struct adiv5_dap *swjdp = armv8->arm.dap;
	int retval = ERROR_COMMAND_SYNTAX_ERROR;
	uint8_t apsel = swjdp->apsel;

	LOG_DEBUG("Writing memory to real address 0x%" PRIx64 "; size %" PRId32 "; count %" PRId32, address,
		size, count);

	if (count && buffer) {

		if (armv8->memory_ap_available && (apsel == armv8->memory_ap)) {

			/* write memory through AHB-AP */
			retval = mem_ap_sel_write_buf(swjdp, armv8->memory_ap, buffer, size, count, address);
		} else {

			/* write memory through APB-AP */
			retval = aarch64_mmu_modify(target, 0);
			if (retval != ERROR_OK)
				return retval;
			return aarch64_write_apb_ab_memory(target, address, size, count, buffer);
		}
	}


	/* REVISIT this op is generic ARMv7-A/R stuff */
	if (retval == ERROR_OK && target->state == TARGET_HALTED) {
		struct arm_dpm *dpm = armv8->arm.dpm;

		retval = dpm->prepare(dpm);
		if (retval != ERROR_OK)
			return retval;

		/* The Cache handling will NOT work with MMU active, the
		 * wrong addresses will be invalidated!
		 *
		 * For both ICache and DCache, walk all cache lines in the
		 * address range. Cortex-A has fixed 64 byte line length.
		 *
		 * REVISIT per ARMv7, these may trigger watchpoints ...
		 */

		/* invalidate I-Cache */
		if (armv8->armv8_mmu.armv8_cache.i_cache_enabled) {
			/* ICIMVAU - Invalidate Cache single entry
			 * with MVA to PoU
			 *		MCR p15, 0, r0, c7, c5, 1
			 */
			for (uint32_t cacheline = 0;
				cacheline < size * count;
				cacheline += 64) {
				retval = dpm->instr_write_data_r0(dpm,
						ARMV8_MSR_GP(SYSTEM_ICIVAU, 0),
						address + cacheline);
				if (retval != ERROR_OK)
					return retval;
			}
		}

		/* invalidate D-Cache */
		if (armv8->armv8_mmu.armv8_cache.d_u_cache_enabled) {
			/* DCIMVAC - Invalidate data Cache line
			 * with MVA to PoC
			 *		MCR p15, 0, r0, c7, c6, 1
			 */
			for (uint32_t cacheline = 0;
				cacheline < size * count;
				cacheline += 64) {
				retval = dpm->instr_write_data_r0(dpm,
						ARMV8_MSR_GP(SYSTEM_DCCVAU, 0),
						address + cacheline);
				if (retval != ERROR_OK)
					return retval;
			}
		}

		/* (void) */ dpm->finish(dpm);
	}

	return retval;
}

static int aarch64_virt2phys64(struct target *target,
			       uint64_t virt, uint64_t *phys)
{
  int retval = ERROR_FAIL;
  struct armv8_common *armv8 = target_to_armv8(target);
  struct adiv5_dap *swjdp = armv8->arm.dap;
  uint8_t apsel = swjdp->apsel;

  if (armv8->memory_ap_available && (apsel == armv8->memory_ap)) {
    uint32_t ret;
    retval = armv8_mmu_translate_va(target,
				    virt, &ret);
    if (retval != ERROR_OK)
      goto done;
    *phys = ret;
  } 
  else {
    LOG_ERROR("AAR64 processor not support translate va to pa");
  }
 done:
  return retval;
}

static int aarch64_write_memory_64(struct target *target, uint64_t address,
				   uint32_t size, uint32_t count, const uint8_t *buffer)
{
	int mmu_enabled = 0;
	uint64_t virt, phys;
	int retval;
	struct armv8_common *armv8 = target_to_armv8(target);
	struct adiv5_dap *swjdp = armv8->arm.dap;
	uint8_t apsel = swjdp->apsel;

	/* aarch64 handles unaligned memory access */
	LOG_DEBUG("Writing memory at address 0x%" PRIx64 "; size %" PRId32 "; count %" PRId32, address,
		size, count);

	/* determine if MMU was enabled on target stop */
	if (!armv8->is_armv7r) {
		retval = aarch64_mmu(target, &mmu_enabled);
		if (retval != ERROR_OK)
			return retval;
	}

	if (armv8->memory_ap_available && (apsel == armv8->memory_ap)) {
		LOG_DEBUG("Writing memory to address 0x%" PRIx64 "; size %" PRId32 "; count %" PRId32, address, size,
			count);
		if (mmu_enabled) {
			virt = address;
			retval = aarch64_virt2phys64(target, virt, &phys);
			if (retval != ERROR_OK)
				return retval;

			LOG_DEBUG("Writing to virtual address. Translating v:0x%" PRIx64 " to r:0x%" PRIx64,
				  virt,
				  phys);
			address = phys;
		}
		retval = aarch64_write_phys_memory64(target, address, size,
						     count, buffer);
	} else {
		if (mmu_enabled) {
			retval = aarch64_check_address(target, address);
			if (retval != ERROR_OK)
				return retval;
			/* enable MMU as we could have disabled it for phys access */
			retval = aarch64_mmu_modify(target, 1);
			if (retval != ERROR_OK)
				return retval;
		}
		retval = aarch64_write_apb_ab_memory64(target, address, size, count, buffer);
	}
	return retval;
}

static int aarch64_handle_target_request(void *priv)
{
	struct target *target = priv;
	struct armv8_common *armv8 = target_to_armv8(target);
	struct adiv5_dap *swjdp = armv8->arm.dap;
	int retval;

	if (!target_was_examined(target))
		return ERROR_OK;
	if (!target->dbg_msg_enabled)
		return ERROR_OK;

	if (target->state == TARGET_RUNNING) {
		uint32_t request;
		uint32_t dscr;
		retval = mem_ap_sel_read_atomic_u32(swjdp, armv8->debug_ap,
				armv8->debug_base + CPUV8_DBG_DSCR, &dscr);

		/* check if we have data */
		while ((dscr & DSCR_DTR_TX_FULL) && (retval == ERROR_OK)) {
			retval = mem_ap_sel_read_atomic_u32(swjdp, armv8->debug_ap,
					armv8->debug_base + CPUV8_DBG_DTRTX, &request);
			if (retval == ERROR_OK) {
				target_request(target, request);
				retval = mem_ap_sel_read_atomic_u32(swjdp, armv8->debug_ap,
						armv8->debug_base + CPUV8_DBG_DSCR, &dscr);
			}
		}
	}

	return ERROR_OK;
}

static int aarch64_examine_first(struct target *target)
{
	struct aarch64_common *aarch64 = target_to_aarch64(target);
	struct armv8_common *armv8 = &aarch64->armv8_common;
	struct adiv5_dap *swjdp = armv8->arm.dap;
	int i;
	int retval = ERROR_OK;
	uint64_t debug, ttypr, cpuid;
	uint32_t tmp0, tmp1;
	debug = ttypr = cpuid = 0;

	/* We do one extra read to ensure DAP is configured,
	 * we call ahbap_debugport_init(swjdp) instead
	 */
	retval = ahbap_debugport_init(swjdp);
	if (retval != ERROR_OK)
		return retval;

	/* Search for the APB-AB - it is needed for access to debug registers */
	retval = dap_find_ap(swjdp, AP_TYPE_APB_AP, &armv8->debug_ap);
	if (retval != ERROR_OK) {
		LOG_ERROR("Could not find APB-AP for debug access");
		return retval;
	}
	/* Search for the AHB-AB */
	retval = dap_find_ap(swjdp, AP_TYPE_AHB_AP, &armv8->memory_ap);
	if (retval != ERROR_OK) {
		/* AHB-AP not found - use APB-AP */
		LOG_DEBUG("Could not find AHB-AP - using APB-AP for memory access");
		armv8->memory_ap_available = false;
	} else {
		armv8->memory_ap_available = true;
	}


	if (!target->dbgbase_set) {
		uint32_t dbgbase;
		/* Get ROM Table base */
		uint32_t apid;
		int32_t coreidx = target->coreid;
		retval = dap_get_debugbase(swjdp, 1, &dbgbase, &apid);
		if (retval != ERROR_OK)
			return retval;
		/* Lookup 0x15 -- Processor DAP */
		retval = dap_lookup_cs_component(swjdp, 1, dbgbase, 0x15,
				&armv8->debug_base, &coreidx);
		if (retval != ERROR_OK)
			return retval;
		LOG_DEBUG("Detected core %" PRId32 " dbgbase: %08" PRIx32,
			  coreidx, armv8->debug_base);
	} else
		armv8->debug_base = target->dbgbase;

	LOG_DEBUG("Target ctibase is 0x%x", target->ctibase);
	if (target->ctibase == 0)
		armv8->cti_base = target->ctibase = armv8->debug_base + 0x1000;
	else
		armv8->cti_base = target->ctibase;

	retval = mem_ap_sel_write_atomic_u32(swjdp, armv8->debug_ap,
			armv8->debug_base + CPUV8_DBG_LOCKACCESS, 0xC5ACCE55);
	if (retval != ERROR_OK) {
		LOG_DEBUG("LOCK debug access fail");
		return retval;
	}

	retval = mem_ap_sel_write_atomic_u32(swjdp, armv8->debug_ap,
			armv8->cti_base + CTI_UNLOCK , 0xC5ACCE55);
	if (retval != ERROR_OK)
		return retval;

	retval = mem_ap_sel_write_atomic_u32(swjdp, armv8->debug_ap,
			armv8->debug_base + CPUV8_DBG_OSLAR, 0);
	if (retval != ERROR_OK) {
		LOG_DEBUG("Examine %s failed", "oslock");
		return retval;
	}

	retval = mem_ap_sel_read_atomic_u32(swjdp, armv8->debug_ap,
			armv8->debug_base + CPUV8_DBG_MAINID0, &tmp0);
	retval += mem_ap_sel_read_atomic_u32(swjdp, armv8->debug_ap,
			armv8->debug_base + CPUV8_DBG_MAINID0 + 4, &tmp1);
	if (retval != ERROR_OK) {
		LOG_DEBUG("Examine %s failed", "CPUID");
		return retval;
	}
	cpuid |= tmp1;
	cpuid = (cpuid << 32) | tmp0;

	retval = mem_ap_sel_read_atomic_u32(swjdp, armv8->debug_ap,
			armv8->debug_base + CPUV8_DBG_MEMFEATURE0, &tmp0);
	retval += mem_ap_sel_read_atomic_u32(swjdp, armv8->debug_ap,
			armv8->debug_base + CPUV8_DBG_MEMFEATURE0 + 4, &tmp1);
	if (retval != ERROR_OK) {
		LOG_DEBUG("Examine %s failed", "Memory Model Type");
		return retval;
	}
	ttypr |= tmp1;
	ttypr = (ttypr << 32) | tmp0;

	retval = mem_ap_sel_read_atomic_u32(swjdp, armv8->debug_ap,
			armv8->debug_base + CPUV8_DBG_DBGFEATURE0, &tmp0);
	retval += mem_ap_sel_read_atomic_u32(swjdp, armv8->debug_ap,
			armv8->debug_base + CPUV8_DBG_DBGFEATURE0 + 4, &tmp1);
	if (retval != ERROR_OK) {
		LOG_DEBUG("Examine %s failed", "ID_AA64DFR0_EL1");
		return retval;
	}
	debug |= tmp1;
	debug = (debug << 32) | tmp0;

	LOG_DEBUG("cpuid = 0x%08" PRIx64, cpuid);
	LOG_DEBUG("ttypr = 0x%08" PRIx64, ttypr);
	LOG_DEBUG("debug = 0x%08" PRIx64, debug);

	armv8->arm.core_type = ARM_MODE_MON;
	retval = aarch64_dpm_setup(aarch64, debug);
	if (retval != ERROR_OK)
		return retval;

	/* Setup Breakpoint Register Pairs */
	aarch64->brp_num = (uint32_t)((debug >> 12) & 0x0F) + 1;
	aarch64->brp_num_context = (uint32_t)((debug >> 28) & 0x0F) + 1;
	aarch64->brp_num_available = aarch64->brp_num;
	aarch64->brp_list = calloc(aarch64->brp_num, sizeof(struct aarch64_brp));
	for (i = 0; i < aarch64->brp_num; i++) {
		aarch64->brp_list[i].used = 0;
		if (i < (aarch64->brp_num-aarch64->brp_num_context))
			aarch64->brp_list[i].type = BRP_NORMAL;
		else
			aarch64->brp_list[i].type = BRP_CONTEXT;
		aarch64->brp_list[i].value = 0;
		aarch64->brp_list[i].control = 0;
		aarch64->brp_list[i].BRPn = i;
	}

	LOG_DEBUG("Configured %i hw breakpoints", aarch64->brp_num);

	target_set_examined(target);
	return ERROR_OK;
}

static int aarch64_examine(struct target *target)
{
	int retval = ERROR_OK;

	/* don't re-probe hardware after each reset */
	if (!target_was_examined(target))
		retval = aarch64_examine_first(target);

	/* Configure core debug access */
	if (retval == ERROR_OK)
		retval = aarch64_init_debug_access(target);

	return retval;
}

/*
 *	Cortex-A8 target creation and initialization
 */

static int aarch64_init_target(struct command_context *cmd_ctx,
	struct target *target)
{
	/* examine_first() does a bunch of this */
# if 0
	if (!target_was_examined(target))
	  (void)aarch64_examine_first(target);
# endif
	return ERROR_OK;
}

static int aarch64_init_arch_info(struct target *target,
	struct aarch64_common *aarch64, struct jtag_tap *tap)
{
	struct armv8_common *armv8 = &aarch64->armv8_common;
	struct adiv5_dap *dap = &armv8->dap;

	armv8->arm.dap = dap;

	/* Setup struct aarch64_common */
	aarch64->common_magic = AARCH64_COMMON_MAGIC;
	/*  tap has no dap initialized */
	if (!tap->dap) {
		armv8->arm.dap = dap;
		/* Setup struct aarch64_common */

		/* prepare JTAG information for the new target */
		aarch64->jtag_info.tap = tap;
		aarch64->jtag_info.scann_size = 4;

		/* Leave (only) generic DAP stuff for debugport_init() */
		dap->jtag_info = &aarch64->jtag_info;

		/* Number of bits for tar autoincrement, impl. dep. at least 10 */
		dap->tar_autoincr_block = (1 << 10);
		dap->memaccess_tck = 80;
		tap->dap = dap;
	} else
		armv8->arm.dap = tap->dap;

	aarch64->fast_reg_read = 0;

	/* register arch-specific functions */
	armv8->examine_debug_reason = NULL;

	armv8->post_debug_entry = aarch64_post_debug_entry;

	armv8->pre_restore_context = NULL;

	armv8->armv8_mmu.read_physical_memory = aarch64_read_phys_memory_64;

	/* REVISIT v7a setup should be in a v7a-specific routine */
	armv8_init_arch_info(target, armv8);
	target_register_timer_callback(aarch64_handle_target_request, 1, 1, target);

	return ERROR_OK;
}

static int aarch64_target_create(struct target *target, Jim_Interp *interp)
{
	struct aarch64_common *aarch64 = calloc(1, sizeof(struct aarch64_common));

	aarch64->armv8_common.is_armv7r = false;

	return aarch64_init_arch_info(target, aarch64, target->tap);
}

static int aarch64_mmu(struct target *target, int *enabled)
{
	if (target->state != TARGET_HALTED) {
		LOG_ERROR("%s: target not halted", __func__);
		return ERROR_TARGET_INVALID;
	}

	*enabled = target_to_aarch64(target)->armv8_common.armv8_mmu.mmu_enabled;
	return ERROR_OK;
}

static int aarch64_virt2phys(struct target *target,
	uint32_t virt, uint32_t *phys)
{
	int retval = ERROR_FAIL;
	struct armv8_common *armv8 = target_to_armv8(target);
	struct adiv5_dap *swjdp = armv8->arm.dap;
	uint8_t apsel = swjdp->apsel;
	if (armv8->memory_ap_available && (apsel == armv8->memory_ap)) {
		uint32_t ret;
		retval = armv8_mmu_translate_va(target,
				virt, &ret);
		if (retval != ERROR_OK)
			goto done;
		*phys = ret;
	} else {
		LOG_ERROR("AAR64 processor not support translate va to pa");
	}
done:
	return retval;
}

COMMAND_HANDLER(aarch64_handle_cache_info_command)
{
	struct target *target = get_current_target(CMD_CTX);
	struct armv8_common *armv8 = target_to_armv8(target);

	return armv8_handle_cache_info_command(CMD_CTX,
			&armv8->armv8_mmu.armv8_cache);
}


COMMAND_HANDLER(aarch64_handle_dbginit_command)
{
	struct target *target = get_current_target(CMD_CTX);
	if (!target_was_examined(target)) {
		LOG_ERROR("target not examined yet");
		return ERROR_FAIL;
	}

	return aarch64_init_debug_access(target);
}

COMMAND_HANDLER(aarch64_handle_mmu_info_command)
{
  struct target *target = get_current_target(CMD_CTX);
  int ret_value;

  ret_value = armv8_arch_state(target);
  
  return ret_value;
}

COMMAND_HANDLER(aarch64_handle_registers_command)
{
  struct target *target = get_current_target(CMD_CTX);
  struct target_list *head;
  struct target *curr;

  head = target->head;
  if (head != (struct target_list *)NULL) {
    while (head != (struct target_list *)NULL) {
      curr = head->target;
      {
	struct reg_cache *cache = curr->reg_cache;
	struct reg_cache *cache2 = cache;
	unsigned count = 0;
	struct reg *reg = NULL;
	char *value;
	
	count = 0;
	while (cache) {
	  unsigned i;
	  
	  LOG_USER("===== %s: %s", target_name(curr), cache->name);
	  
	  for (i = 0, reg = cache->reg_list;
	       i < cache->num_regs;
	       i++, reg++, count++) {
	    /* only print cached values if they are valid */
	    if (reg->valid) {
	      value = buf_to_str(reg->value,
				 reg->size, 16);
	      LOG_USER("(%i) %s (/%" PRIu32 "): 0x%s%s",
		       count, reg->name,
		       reg->size, value,
		       reg->dirty
		       ? " (dirty)"
		       : "");
	      free(value);
	    } 
	    else {
	      LOG_USER("(%i) %s (/%" PRIu32 ")",
		       count, reg->name,
		       reg->size) ;
	    }
	  }
	  cache = cache->next;
	  if (cache == cache2) { /* in loop ... */
	    break;
	  }
	}
      }
      head = head->next;
    }
  }
  return ERROR_OK;
}

COMMAND_HANDLER(aarch64_handle_states_command)
{
  struct target *target = get_current_target(CMD_CTX);
  struct target_list *head;
  struct target *curr;

  head = target->head;
  if (head != (struct target_list *)NULL) {
    while (head != (struct target_list *)NULL) {
      curr = head->target;
      {
	struct reg_cache *cache = curr->reg_cache;
	struct reg_cache *cache2 = NULL;
	unsigned count = 0;
	struct reg *reg = NULL;
	char *value;
	
	count = 0;
	while (cache) {
	  unsigned i;
	  
	  LOG_USER("===== %s: sp/pc/cpsr", target_name(curr));
	  if (curr->state == TARGET_HALTED) {
	    (void)armv8_arch_state(curr);
	  }
	  else {
	    LOG_USER("Not halted");
	  }
	  
	  for (i = 0, reg = cache->reg_list;
	       i < cache->num_regs;
	       i++, reg++, count++) {
	    if (i < 31) { /* only want sp, pc and spsr */
	      continue;
	    }
	    /* only print cached values if they are valid */
	    if (reg->valid) {
	      value = buf_to_str(reg->value,
				 reg->size, 16);
	      LOG_USER("(%i) %s (/%" PRIu32 "): 0x%s%s",
		       count, reg->name,
		       reg->size, value,
		       reg->dirty
		       ? " (dirty)"
		       : "");
	      free(value);
	    } 
	    else {
	      LOG_USER("(%i) %s (/%" PRIu32 ")",
		       count, reg->name,
		       reg->size) ;
	    }
	  }
	  cache2 = cache;
	  cache = cache->next;
	  if (cache == cache2) { /* in loop ... */
	    break;
	  }
	}
      }
      head = head->next;
    }
  }
  return ERROR_OK;
}

COMMAND_HANDLER(aarch64_handle_state_command)
{
  struct target *curr = get_current_target(CMD_CTX);
  struct reg_cache *cache = curr->reg_cache;
  struct reg_cache *cache2 = NULL;
  unsigned count = 0;
  struct reg *reg = NULL;
  char *value;
  
  count = 0;
  while (cache) {
    unsigned i;
    
    LOG_USER("===== %s: sp/pc/cpsr", target_name(curr));
    if (curr->state == TARGET_HALTED) {
      (void)armv8_arch_state(curr);
    }
    else {
      LOG_USER("Not halted");
    }
    
    for (i = 0, reg = cache->reg_list;
	 i < cache->num_regs;
	 i++, reg++, count++) {
      if (i < 31) { /* only want sp, pc and spsr */
	continue;
      }
      /* only print cached values if they are valid */
      if (reg->valid) {
	value = buf_to_str(reg->value,
			   reg->size, 16);
	LOG_USER("(%i) %s (/%" PRIu32 "): 0x%s%s",
		 count, reg->name,
		 reg->size, value,
		 reg->dirty
		 ? " (dirty)"
		 : "");
	free(value);
      } 
      else {
	LOG_USER("(%i) %s (/%" PRIu32 ")",
		 count, reg->name,
		 reg->size) ;
      }
    }
    cache2 = cache;
    cache = cache->next;
    if (cache == cache2) { /* in loop ... */
      break;
    }
  }
  return ERROR_OK;
}

COMMAND_HANDLER(aarch64_handle_smp_off_command)
{
	struct target *target = get_current_target(CMD_CTX);
	/* check target is an smp target */
	struct target_list *head;
	struct target *curr;
	head = target->head;
	target->smp = 0;
	if (head != (struct target_list *)NULL) {
		while (head != (struct target_list *)NULL) {
			curr = head->target;
			curr->smp = 0;
			head = head->next;
		}
		/*  fixes the target display to the debugger */
		target->gdb_service->target = target;
	}
	return ERROR_OK;
}

COMMAND_HANDLER(aarch64_handle_smp_on_command)
{
	struct target *target = get_current_target(CMD_CTX);
	struct target_list *head;
	struct target *curr;
	head = target->head;
	if (head != (struct target_list *)NULL) {
		target->smp = 1;
		while (head != (struct target_list *)NULL) {
			curr = head->target;
			curr->smp = 1;
			head = head->next;
		}
	}
	return ERROR_OK;
}

COMMAND_HANDLER(aarch64_handle_smp_gdb_command)
{
	struct target *target = get_current_target(CMD_CTX);
	int retval = ERROR_OK;
	struct target_list *head;
	head = target->head;
	if (head != (struct target_list *)NULL) {
		if (CMD_ARGC == 1) {
			int coreid = 0;
			COMMAND_PARSE_NUMBER(int, CMD_ARGV[0], coreid);
			if (ERROR_OK != retval)
				return retval;
			target->gdb_service->core[1] = coreid;

		}
		command_print(CMD_CTX, "gdb coreid  %" PRId32 " -> %" PRId32, target->gdb_service->core[0]
			, target->gdb_service->core[1]);
	}
	return ERROR_OK;
}

# if 0 /* not used */
static char *dscr_status_tab(uint32_t status)
{
  switch(status) {
  case 0x01:
    return("PE is restarting");
    
  case 0x02:
    return("PE is in non-debug state");

  case 0x07:
    return("breakpoint");

  case 0x13:
    return("external debug request");

  case 0x1b:
    return("halting step, normal");

  case 0x1f:
    return("halting step, exclusive");

  case 0x23:
    return("OS unlock catch");

  case 0x27:
    return("reset catch");

  case 0x2b:
    return("watchpoint");

  case 0x2f:
    return("HLT instruction");

  case 0x33:
    return("software access to debug register");

  case 0x37:
    return("exception catch");

  case 0x3b:
    return("halting step, no syndrome");

  default:
    return("UNKNOWN");
  }
}

static char *decode_dscr(struct target *target, uint32_t dscr)
{
  static char buff[128];

  buff[0] = '\0';

  sprintf(buff, "%s: 0x%08x ns: %d sdd: %d hde: %d err: %d mode: %s",
	  target_name(target),
	  dscr,
	  dscr & (1 << 18) ? 1 : 0,
	  dscr & (1 << 16) ? 1 : 0,
	  dscr & (1 << 14) ? 1 : 0,
	  dscr & (1 << 6)  ? 1 : 0,
	  dscr_status_tab(dscr & 0x3f));

  return(buff);
}
# endif

static const struct command_registration aarch64_exec_command_handlers[] = {
	{
		.name = "cache_info",
		.handler = aarch64_handle_cache_info_command,
		.mode = COMMAND_EXEC,
		.help = "display information about target caches",
		.usage = "",
	},
	{
		.name = "dbginit",
		.handler = aarch64_handle_dbginit_command,
		.mode = COMMAND_EXEC,
		.help = "Initialize core debug",
		.usage = "",
	},
	{
		.name = "mmu_info",
		.handler = aarch64_handle_mmu_info_command,
		.mode = COMMAND_EXEC,
		.help = "MMU and cache summary",
		.usage = "",
	},
	{
		.name = "regs",
		.handler = aarch64_handle_registers_command,
		.mode = COMMAND_EXEC,
		.help = "Register summary, all cores if in SMP",
		.usage = "",
	},
	{
		.name = "registers",
		.handler = aarch64_handle_registers_command,
		.mode = COMMAND_EXEC,
		.help = "Register summary, all cores if in SMP",
		.usage = "",
	},
	{   .name = "smp_off",
	    .handler = aarch64_handle_smp_off_command,
	    .mode = COMMAND_EXEC,
	    .help = "Stop smp handling",
	    .usage = "",
	},
	{
		.name = "smp_on",
		.handler = aarch64_handle_smp_on_command,
		.mode = COMMAND_EXEC,
		.help = "Restart smp handling",
		.usage = "",
	},
	{
		.name = "smp_gdb",
		.handler = aarch64_handle_smp_gdb_command,
		.mode = COMMAND_EXEC,
		.help = "display/fix current core played to gdb",
		.usage = "",
	},
	{
		.name = "state",
		.handler = aarch64_handle_state_command,
		.mode = COMMAND_EXEC,
		.help = "State register summary: SP, PC, CPSR",
		.usage = "",
	},
	{
		.name = "states",
		.handler = aarch64_handle_states_command,
		.mode = COMMAND_EXEC,
		.help = "State register summary: SP, PC, CPSR; SMP",
		.usage = "",
	},

	COMMAND_REGISTRATION_DONE
};
static const struct command_registration aarch64_command_handlers[] = {
	{
		.chain = arm_command_handlers,
	},
	{
		.chain = armv8_command_handlers,
	},
	{
		.name = "aarch64",
		.mode = COMMAND_ANY,
		.help = "aarch64 command group",
		.usage = "",
		.chain = aarch64_exec_command_handlers,
	},
	COMMAND_REGISTRATION_DONE
};

struct target_type aarch64_target = {
	.name = "aarch64",
	.pc_size = 64,

	.poll = aarch64_poll,
	.arch_state = armv8_arch_state,

	.halt = aarch64_halt,
	.resume = aarch64_resume,
	.step = aarch64_step,

	.assert_reset = aarch64_assert_reset,
	.deassert_reset = aarch64_deassert_reset,

	/* REVISIT allow exporting VFP3 registers ... */
	.get_gdb_reg_list = armv8_get_gdb_reg_list,

	.read_memory = aarch64_read_memory,
	.read_memory_64 = aarch64_read_memory_64,
	.write_memory = aarch64_write_memory,

	.checksum_memory = arm_checksum_memory,
	.blank_check_memory = arm_blank_check_memory,

	.run_algorithm = armv4_5_run_algorithm,

	.add_breakpoint = aarch64_add_breakpoint,
	.add_context_breakpoint = aarch64_add_context_breakpoint,
	.add_hybrid_breakpoint = aarch64_add_hybrid_breakpoint,
	.remove_breakpoint = aarch64_remove_breakpoint,
	.add_watchpoint = NULL,
	.remove_watchpoint = NULL,

	.commands = aarch64_command_handlers,
	.target_create = aarch64_target_create,
	.init_target = aarch64_init_target,
	.examine = aarch64_examine,

	.read_phys_memory = aarch64_read_phys_memory,
	.read_phys_memory_64 = aarch64_read_phys_memory_64,
	.write_phys_memory = aarch64_write_phys_memory,
	.mmu = aarch64_mmu,
	.virt2phys = aarch64_virt2phys,
};
