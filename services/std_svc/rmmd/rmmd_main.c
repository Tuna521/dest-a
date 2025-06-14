/*
 * Copyright (c) 2021-2024, Arm Limited and Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <string.h>

#include <arch_helpers.h>
#include <arch_features.h>
#include <bl31/bl31.h>
#include <common/debug.h>
#include <common/runtime_svc.h>
#include <context.h>
#include <lib/el3_runtime/context_mgmt.h>
#include <lib/el3_runtime/cpu_data.h>
#include <lib/el3_runtime/pubsub.h>
#include <lib/extensions/pmuv3.h>
#include <lib/extensions/sys_reg_trace.h>
#include <lib/gpt_rme/gpt_rme.h>

#include <lib/spinlock.h>
#include <lib/utils.h>
#include <lib/xlat_tables/xlat_tables_v2.h>
#include <plat/common/common_def.h>
#include <plat/common/platform.h>
#include <platform_def.h>
#include <services/rmmd_svc.h>
#include <smccc_helpers.h>
#include <lib/extensions/sme.h>
#include <lib/extensions/sve.h>
#include "rmmd_initial_context.h"
#include "rmmd_private.h"

#include <bl31/interrupt_mgmt.h>

/*******************************************************************************
 * RMM boot failure flag
 ******************************************************************************/
static bool rmm_boot_failed;

/*******************************************************************************
 * RMM context information.
 ******************************************************************************/
rmmd_rmm_context_t rmm_context[PLATFORM_CORE_COUNT];

/*******************************************************************************
 * RMM entry point information. Discovered on the primary core and reused
 * on secondary cores.
 ******************************************************************************/
static entry_point_info_t *rmm_ep_info;

/*******************************************************************************
 * Static function declaration.
 ******************************************************************************/
static int32_t rmm_init(void);

/*******************************************************************************
 * DEST: Define list to contain realm variables.
 ******************************************************************************/
static realm_info_t realm_values[MAX_REALM_NUMS];
static uint32_t realm_count = 0;
static bool realm_created = false;
static uint64_t realm_created_id = 0;
static bool realm_getting_rpv = false;
static uint64_t realm_getting_rpv_id = 0;

/*******************************************************************************
 * This function takes an RMM context pointer and performs a synchronous entry
 * into it.
 ******************************************************************************/
uint64_t rmmd_rmm_sync_entry(rmmd_rmm_context_t *rmm_ctx)
{
	uint64_t rc;

	assert(rmm_ctx != NULL);

	cm_set_context(&(rmm_ctx->cpu_ctx), REALM);

	/* Restore the realm context assigned above */
	cm_el2_sysregs_context_restore(REALM);
	cm_set_next_eret_context(REALM);

	/* Enter RMM */
	rc = rmmd_rmm_enter(&rmm_ctx->c_rt_ctx);

	/*
	 * Save realm context. EL2 Non-secure context will be restored
	 * before exiting Non-secure world, therefore there is no need
	 * to clear EL2 context registers.
	 */
	cm_el2_sysregs_context_save(REALM);

	return rc;
}

/*******************************************************************************
 * This function returns to the place where rmmd_rmm_sync_entry() was
 * called originally.
 ******************************************************************************/
__dead2 void rmmd_rmm_sync_exit(uint64_t rc)
{
	rmmd_rmm_context_t *ctx = &rmm_context[plat_my_core_pos()];

	/* Get context of the RMM in use by this CPU. */
	assert(cm_get_context(REALM) == &(ctx->cpu_ctx));

	/*
	 * The RMMD must have initiated the original request through a
	 * synchronous entry into RMM. Jump back to the original C runtime
	 * context with the value of rc in x0;
	 */
	rmmd_rmm_exit(ctx->c_rt_ctx, rc);

	panic();
}

static void rmm_el2_context_init(el2_sysregs_t *regs)
{
	write_el2_ctx_common(regs, spsr_el2, REALM_SPSR_EL2);
	write_el2_ctx_common(regs, sctlr_el2, SCTLR_EL2_RES1);
}

/*******************************************************************************
 * Enable architecture extensions on first entry to Realm world.
 ******************************************************************************/

static void manage_extensions_realm(cpu_context_t *ctx)
{
	pmuv3_enable(ctx);

	/*
	 * Enable access to TPIDR2_EL0 if SME/SME2 is enabled for Non Secure world.
	 */
	if (is_feat_sme_supported()) {
		sme_enable(ctx);
	}
}

static void manage_extensions_realm_per_world(void)
{
	cm_el3_arch_init_per_world(&per_world_context[CPU_CONTEXT_REALM]);

	if (is_feat_sve_supported()) {
	/*
	 * Enable SVE and FPU in realm context when it is enabled for NS.
	 * Realm manager must ensure that the SVE and FPU register
	 * contexts are properly managed.
	 */
		sve_enable_per_world(&per_world_context[CPU_CONTEXT_REALM]);
	}

	/* NS can access this but Realm shouldn't */
	if (is_feat_sys_reg_trace_supported()) {
		sys_reg_trace_disable_per_world(&per_world_context[CPU_CONTEXT_REALM]);
	}

	/*
	 * If SME/SME2 is supported and enabled for NS world, then disable trapping
	 * of SME instructions for Realm world. RMM will save/restore required
	 * registers that are shared with SVE/FPU so that Realm can use FPU or SVE.
	 */
	if (is_feat_sme_supported()) {
		sme_enable_per_world(&per_world_context[CPU_CONTEXT_REALM]);
	}
}

/*******************************************************************************
 * Jump to the RMM for the first time.
 ******************************************************************************/
static void el3_timer_irq_init(void);

static int32_t rmm_init(void)
{
	long rc;
	rmmd_rmm_context_t *ctx = &rmm_context[plat_my_core_pos()];

	INFO("RMM init start.\n");

	/* Enable architecture extensions */
	manage_extensions_realm(&ctx->cpu_ctx);
	manage_extensions_realm_per_world();

	/* Initialize RMM EL2 context */
	rmm_el2_context_init(&ctx->cpu_ctx.el2_sysregs_ctx);

	INFO("Set up the enable\n");
	el3_timer_irq_init();

	rc = rmmd_rmm_sync_entry(ctx);
	if (rc != E_RMM_BOOT_SUCCESS) {
		ERROR("RMM init failed: %ld\n", rc);
		rmm_boot_failed = true;
		return 0;
	}

	INFO("RMM init end.\n");
	return 1;
}

static void el3_timer_irq_init(void)
{
	INFO("INIT: Registering EL3 timer interrupt handler\n");
	// CCA_TRACE_START;
	// CCA_MARKER_TIMER_SETUP_START();
	// CCA_TRACE_STOP;

    plat_ic_set_interrupt_type(EL3_TIMER_IRQ, INTR_TYPE_EL3);
	plat_ic_set_interrupt_priority(EL3_TIMER_IRQ, GIC_HIGHEST_SEC_PRIORITY);
    plat_ic_enable_interrupt(EL3_TIMER_IRQ);

	// CCA_TRACE_START;
	// CCA_MARKER_TIMER_SETUP_END();
	// CCA_TRACE_STOP;
}

/*******************************************************************************
 * Load and read RMM manifest, setup RMM.
 ******************************************************************************/
static void el3_timer_irq_setup(void);

int rmmd_setup(void)
{
	size_t shared_buf_size __unused;
	uintptr_t shared_buf_base;
	uint32_t ep_attr;
	unsigned int linear_id = plat_my_core_pos();
	rmmd_rmm_context_t *rmm_ctx = &rmm_context[linear_id];
	struct rmm_manifest *manifest;
	int rc;

	/* Make sure RME is supported. */
	if (is_feat_rme_present() == 0U) {
		/* Mark the RMM boot as failed for all the CPUs */
		rmm_boot_failed = true;
		return -ENOTSUP;
	}

	rmm_ep_info = bl31_plat_get_next_image_ep_info(REALM);
	if ((rmm_ep_info == NULL) || (rmm_ep_info->pc == 0)) {
		WARN("No RMM image provided by BL2 boot loader, Booting "
		     "device without RMM initialization. SMCs destined for "
		     "RMM will return SMC_UNK\n");

		/* Mark the boot as failed for all the CPUs */
		rmm_boot_failed = true;
		return -ENOENT;
	}

	/* Initialise an entrypoint to set up the CPU context */
	ep_attr = EP_REALM;
	if ((read_sctlr_el3() & SCTLR_EE_BIT) != 0U) {
		ep_attr |= EP_EE_BIG;
	}

	SET_PARAM_HEAD(rmm_ep_info, PARAM_EP, VERSION_1, ep_attr);
	rmm_ep_info->spsr = SPSR_64(MODE_EL2,
					MODE_SP_ELX,
					DISABLE_ALL_EXCEPTIONS);

	shared_buf_size =
			plat_rmmd_get_el3_rmm_shared_mem(&shared_buf_base);

	assert((shared_buf_size == SZ_4K) &&
					((void *)shared_buf_base != NULL));

	/* Zero out and load the boot manifest at the beginning of the share area */
	manifest = (struct rmm_manifest *)shared_buf_base;
	(void)memset((void *)manifest, 0, sizeof(struct rmm_manifest));

	rc = plat_rmmd_load_manifest(manifest);
	if (rc != 0) {
		ERROR("Error loading RMM Boot Manifest (%i)\n", rc);
		/* Mark the boot as failed for all the CPUs */
		rmm_boot_failed = true;
		return rc;
	}
	flush_dcache_range((uintptr_t)shared_buf_base, shared_buf_size);

	/*
	 * Prepare coldboot arguments for RMM:
	 * arg0: This CPUID (primary processor).
	 * arg1: Version for this Boot Interface.
	 * arg2: PLATFORM_CORE_COUNT.
	 * arg3: Base address for the EL3 <-> RMM shared area. The boot
	 *       manifest will be stored at the beginning of this area.
	 */
	rmm_ep_info->args.arg0 = linear_id;
	rmm_ep_info->args.arg1 = RMM_EL3_INTERFACE_VERSION;
	rmm_ep_info->args.arg2 = PLATFORM_CORE_COUNT;
	rmm_ep_info->args.arg3 = shared_buf_base;

	/* Initialise RMM context with this entry point information */
	cm_setup_context(&rmm_ctx->cpu_ctx, rmm_ep_info);

	INFO("RMM setup done.\n");
	el3_timer_irq_setup();

	/* Register init function for deferred init.  */
	bl31_register_rmm_init(&rmm_init);

	return 0;
}

static void el3_timer_irq_setup(void) {
	INFO("SETUP: Registering EL3 timer interrupt handler\n");
	// CCA_TRACE_START;
	// CCA_MARKER_TIMER_SETUP_START();
	// CCA_TRACE_STOP;

	uint64_t flags = 0;
    set_interrupt_rm_flag(flags, SECURE);
    set_interrupt_rm_flag(flags, NON_SECURE);

    int rc = register_interrupt_type_handler(INTR_TYPE_S_EL1, rmmd_timer_handler, flags);
    if (rc == -EALREADY) {
        ERROR("Failed to register EL3 timer handler: %d\n", rc);
    }
	INFO("SETUP: rc = %d\n", rc);
	
	// CCA_TRACE_START;
	// CCA_MARKER_TIMER_SETUP_END();
	// CCA_TRACE_STOP;
}

/*******************************************************************************
 * Forward SMC to the other security state
 ******************************************************************************/
static uint64_t	rmmd_smc_forward(uint32_t src_sec_state,
				 uint32_t dst_sec_state, uint64_t x0,
				 uint64_t x1, uint64_t x2, uint64_t x3,
				 uint64_t x4, void *handle)
{
	cpu_context_t *ctx = cm_get_context(dst_sec_state);

	/* Save incoming security state */
	cm_el2_sysregs_context_save(src_sec_state);

	/* Restore outgoing security state */
	cm_el2_sysregs_context_restore(dst_sec_state);
	cm_set_next_eret_context(dst_sec_state);

	/*
	 * As per SMCCCv1.2, we need to preserve x4 to x7 unless
	 * being used as return args. Hence we differentiate the
	 * onward and backward path. Support upto 8 args in the
	 * onward path and 4 args in return path.
	 * Register x4 will be preserved by RMM in case it is not
	 * used in return path.
	 */
	if (src_sec_state == NON_SECURE) {
		rmmd_smc_save_values(ctx, x0, x1, x2, x3, x4, handle);
	}

	SMC_RET5(ctx, x0, x1, x2, x3, x4);
}


/*******************************************************************************
 * DEST: Save values in the realm.
 *******************************************************************************/
 static realm_info_t *get_realm_info_by_rd(uint64_t rd)
{
    for (uint32_t i = 0; i < realm_count; i++) {
        if (realm_values[i].rd == rd) {
            return &realm_values[i];
        }
    }
    return NULL;
}

static void print_realm_info(const realm_info_t *r) {
    INFO("Realm rd = 0x%lx\n", r->rd);
}

uint64_t rmmd_smc_save_values(cpu_context_t *ctx, 
	uint64_t x0, uint64_t x1, uint64_t x2, uint64_t x3,
	uint64_t x4, void *handle) 
{
	if (x0 == RMI_DATA_DESTROY_ALL_FID) {
		INFO("RMI_DATA_DESTROY_ALL called\n");
		INFO("x1 = 0x%lx\n", x1);
		realm_info_t *r = get_realm_info_by_rd(x1);
		print_realm_info(r);
	}
	
	/* This will now be called with either the original function ID or our changed one */
	SMC_RET8(ctx, x0, x1, x2, x3, x4,
		SMC_GET_GP(handle, CTX_GPREG_X5),
		SMC_GET_GP(handle, CTX_GPREG_X6),
		SMC_GET_GP(handle, CTX_GPREG_X7));
}

/*******************************************************************************
 * This function handles all SMCs in the range reserved for RMI. Each call is
 * either forwarded to the other security state or handled by the RMM dispatcher
 ******************************************************************************/
uint64_t rmmd_rmi_handler(uint32_t smc_fid, uint64_t x1, uint64_t x2,
			  uint64_t x3, uint64_t x4, void *cookie,
			  void *handle, uint64_t flags)
{
	uint32_t src_sec_state;
	
	/* If RMM failed to boot, treat any RMI SMC as unknown */
	if (rmm_boot_failed) {
		WARN("RMMD: Failed to boot up RMM. Ignoring RMI call\n");
		SMC_RET1(handle, SMC_UNK);
	}

	/* Determine which security state this SMC originated from */
	src_sec_state = caller_sec_state(flags);

	/* RMI must not be invoked by the Secure world */
	if (src_sec_state == SMC_FROM_SECURE) {
		WARN("RMMD: RMI invoked by secure world.\n");
		SMC_RET1(handle, SMC_UNK);
	}

	/*
	 * Forward an RMI call from the Normal world to the Realm world as it
	 * is.
	 */

	if (smc_fid == RMI_REALM_CREATE_FID) {
		INFO("RMI_REALM_CREATE called in tf-a\n");

		if (realm_count >= MAX_REALM_NUMS) {
			ERROR("Too many realms!\n");
		} else {
			realm_info_t *r = &realm_values[realm_count++];
			r->rd = x1;
			r->timer_expiration = 0;
			print_realm_info(r);
			
			realm_created_id = r->rd;
			realm_created = true;
		}
	} else if (smc_fid == RMI_RPV_GET_FID) {
		INFO("RMI_RPV_GET called in tf-a\n");
		realm_getting_rpv = true;
	} else if (smc_fid == RMI_REALM_ACTIVATE_FID) {
		INFO("RMI_REALM_ACTIVATE called in tf-a\n");
		rmmd_timer_init(x1, false);
	} 

	if (src_sec_state == SMC_FROM_NON_SECURE) {
		/*
		 * If SVE hint bit is set in the flags then update the SMC
		 * function id and pass it on to the lower EL.
		 */
		if (is_sve_hint_set(flags)) {
			smc_fid |= (FUNCID_SVE_HINT_MASK <<
				    FUNCID_SVE_HINT_SHIFT);
		}
		
		VERBOSE("RMMD: RMI call from non-secure world.\n");
		return rmmd_smc_forward(NON_SECURE, REALM, smc_fid,
					x1, x2, x3, x4, handle);
	}
	// here maybe we should forward to RMM froms ecure statee

	if (src_sec_state != SMC_FROM_REALM) {
		SMC_RET1(handle, SMC_UNK);
	}

	switch (smc_fid) {
	case RMM_RMI_REQ_COMPLETE: {
		uint64_t x5 = SMC_GET_GP(handle, CTX_GPREG_X5);
		if (realm_created) {
			// after RMI_REALM_CREATE
			realm_created = false;
			realm_getting_rpv_id = realm_created_id;
			// set timer to call RMI_RPV_GET
			rmmd_timer_init(realm_created_id, true);
			realm_created_id = 0;

		} else if (realm_getting_rpv) {
			// after RMI_RPV_GET
			INFO("RMI_RPV_GET_COMPLETE called in tf-a\n");
			INFO("Timer setting to %lu\n", x1);
			// get_realm_info_by_rd(realm_created_id)->timer_expiration = x1;
			rmmd_timer_set_expiration(x1);
			realm_getting_rpv = false;
		}
	
		return rmmd_smc_forward(REALM, NON_SECURE, x1,
					x2, x3, x4, x5, handle);
	}
	default:
		WARN("RMMD: Unsupported RMM call 0x%08x\n", smc_fid);
		SMC_RET1(handle, SMC_UNK);
	}
}

/*******************************************************************************
 * This cpu has been turned on. Enter RMM to initialise R-EL2.  Entry into RMM
 * is done after initialising minimal architectural state that guarantees safe
 * execution.
 ******************************************************************************/
static void *rmmd_cpu_on_finish_handler(const void *arg)
{
	long rc;
	uint32_t linear_id = plat_my_core_pos();
	rmmd_rmm_context_t *ctx = &rmm_context[linear_id];

	if (rmm_boot_failed) {
		/* RMM Boot failed on a previous CPU. Abort. */
		ERROR("RMM Failed to initialize. Ignoring for CPU%d\n",
								linear_id);
		return NULL;
	}

	/*
	 * Prepare warmboot arguments for RMM:
	 * arg0: This CPUID.
	 * arg1 to arg3: Not used.
	 */
	rmm_ep_info->args.arg0 = linear_id;
	rmm_ep_info->args.arg1 = 0ULL;
	rmm_ep_info->args.arg2 = 0ULL;
	rmm_ep_info->args.arg3 = 0ULL;

	/* Initialise RMM context with this entry point information */
	cm_setup_context(&ctx->cpu_ctx, rmm_ep_info);

	/* Enable architecture extensions */
	manage_extensions_realm(&ctx->cpu_ctx);

	/* Initialize RMM EL2 context. */
	rmm_el2_context_init(&ctx->cpu_ctx.el2_sysregs_ctx);

	rc = rmmd_rmm_sync_entry(ctx);

	if (rc != E_RMM_BOOT_SUCCESS) {
		ERROR("RMM init failed on CPU%d: %ld\n", linear_id, rc);
		/* Mark the boot as failed for any other booting CPU */
		rmm_boot_failed = true;
	}

	return NULL;
}

/* Subscribe to PSCI CPU on to initialize RMM on secondary */
SUBSCRIBE_TO_EVENT(psci_cpu_on_finish, rmmd_cpu_on_finish_handler);

/* Convert GPT lib error to RMMD GTS error */
static int gpt_to_gts_error(int error, uint32_t smc_fid, uint64_t address)
{
	int ret;

	if (error == 0) {
		return E_RMM_OK;
	}

	if (error == -EINVAL) {
		ret = E_RMM_BAD_ADDR;
	} else {
		/* This is the only other error code we expect */
		assert(error == -EPERM);
		ret = E_RMM_BAD_PAS;
	}

	ERROR("RMMD: PAS Transition failed. GPT ret = %d, PA: 0x%"PRIx64 ", FID = 0x%x\n",
				error, address, smc_fid);
	return ret;
}

static int rmm_el3_ifc_get_feat_register(uint64_t feat_reg_idx,
					 uint64_t *feat_reg)
{
	if (feat_reg_idx != RMM_EL3_FEAT_REG_0_IDX) {
		ERROR("RMMD: Failed to get feature register %ld\n", feat_reg_idx);
		return E_RMM_INVAL;
	}

	*feat_reg = 0UL;
#if RMMD_ENABLE_EL3_TOKEN_SIGN
	*feat_reg |= RMM_EL3_FEAT_REG_0_EL3_TOKEN_SIGN_MASK;
#endif
	return E_RMM_OK;
}

/*******************************************************************************
 * This function handles RMM-EL3 interface SMCs
 ******************************************************************************/
uint64_t rmmd_rmm_el3_handler(uint32_t smc_fid, uint64_t x1, uint64_t x2,
				uint64_t x3, uint64_t x4, void *cookie,
				void *handle, uint64_t flags)
{
	uint64_t remaining_len = 0UL;
	uint32_t src_sec_state;
	int ret;

	/* If RMM failed to boot, treat any RMM-EL3 interface SMC as unknown */
	if (rmm_boot_failed) {
		WARN("RMMD: Failed to boot up RMM. Ignoring RMM-EL3 call\n");
		SMC_RET1(handle, SMC_UNK);
	}

	/* Determine which security state this SMC originated from */
	src_sec_state = caller_sec_state(flags);

	if (src_sec_state != SMC_FROM_REALM) {
		WARN("RMMD: RMM-EL3 call originated from secure or normal world\n");
		SMC_RET1(handle, SMC_UNK);
	}

	switch (smc_fid) {
	case RMM_GTSI_DELEGATE:
		ret = gpt_delegate_pas(x1, PAGE_SIZE_4KB, SMC_FROM_REALM);
		SMC_RET1(handle, gpt_to_gts_error(ret, smc_fid, x1));
	case RMM_GTSI_UNDELEGATE:
		ret = gpt_undelegate_pas(x1, PAGE_SIZE_4KB, SMC_FROM_REALM);
		SMC_RET1(handle, gpt_to_gts_error(ret, smc_fid, x1));
	case RMM_ATTEST_GET_PLAT_TOKEN:
		ret = rmmd_attest_get_platform_token(x1, &x2, x3, &remaining_len);
		SMC_RET3(handle, ret, x2, remaining_len);
	case RMM_ATTEST_GET_REALM_KEY:
		ret = rmmd_attest_get_signing_key(x1, &x2, x3);
		SMC_RET2(handle, ret, x2);
	case RMM_EL3_FEATURES:
		ret = rmm_el3_ifc_get_feat_register(x1, &x2);
		SMC_RET2(handle, ret, x2);
#if RMMD_ENABLE_EL3_TOKEN_SIGN
	case RMM_EL3_TOKEN_SIGN:
		return rmmd_el3_token_sign(handle, x1, x2, x3, x4);
#endif
	case RMM_BOOT_COMPLETE:
		VERBOSE("RMMD: running rmmd_rmm_sync_exit\n");
		rmmd_rmm_sync_exit(x1);

	default:
		WARN("RMMD: Unsupported RMM-EL3 call 0x%08x\n", smc_fid);
		SMC_RET1(handle, SMC_UNK);
	}
}
