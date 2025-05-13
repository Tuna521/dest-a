#include <arch_helpers.h>
#include <common/debug.h>
#include <drivers/delay_timer.h>
#include <lib/el3_runtime/context_mgmt.h>
#include <lib/el3_runtime/cpu_data.h>
#include <lib/el3_runtime/pubsub.h>
#include <plat/common/platform.h>
#include <services/rmmd_svc.h>
#include <smccc_helpers.h>
#include <bl31/interrupt_mgmt.h>

#include "rmmd_private.h"

/* Timer configuration */
// how long after the realm is created that timer will be triggered
#define REALM_DESTROY_TIMER_SECONDS 30
#define RMM_DATA_DESTROY_ALL_FID 0xc4000170

/* Timer state */
static bool timer_initialized = false;
static uint64_t realm_descriptor = 0;

/*******************************************************************************
 * Timer interrupt handler that will trigger realm destruction
 ******************************************************************************/
static uint64_t rmmd_timer_handler(uint32_t id,
                                 uint32_t flags,
                                 void *handle,
                                 void *cookie)
{
    /* Acknowledge the interrupt */
    INFO("Timer interrupt handler\n");
    uint32_t irq = plat_ic_acknowledge_interrupt();
    plat_ic_end_of_interrupt(irq);

    /* Call the specified SMC with the realm descriptor */
    if (realm_descriptor != 0) {
        INFO("Timer triggered\n");
        
        /* Get the RMM context */
        cpu_context_t *ctx = cm_get_context(REALM);
        assert(ctx != NULL);

        /* Save the current context */
        cm_el2_sysregs_context_save(SECURE);
        
        /* Set up the SMC call */
        write_ctx_reg(get_gpregs_ctx(ctx), CTX_GPREG_X0, RMM_DATA_DESTROY_ALL_FID);
        write_ctx_reg(get_gpregs_ctx(ctx), CTX_GPREG_X1, realm_descriptor);
        
        /* Switch to RMM context */
        cm_el2_sysregs_context_restore(REALM);
        cm_set_next_eret_context(REALM);
        
        realm_descriptor = 0;

        /* Make the SMC call */
        SMC_RET2(ctx, RMM_DATA_DESTROY_ALL_FID, realm_descriptor);

        /* Make the SMC call */
        // equivlent to SMC_RET2(ctx, RMM_DATA_DESTROY_ALL_FID, realm_descriptor);
        // write_ctx_reg((get_gpregs_ctx(ctx)), (CTX_GPREG_X1), (realm_descriptor));	
        // write_ctx_reg((get_gpregs_ctx(ctx)), (CTX_GPREG_X0), (RMM_DATA_DESTROY_ALL_FID));
        // return (void *)(uintptr_t) (ctx);	
    }

    /* If no realm descriptor, just return to the caller */
    return 0;
}

/*******************************************************************************
 * Initialize the timer for realm destruction
 ******************************************************************************/
void rmmd_timer_init(uint64_t rd)
{
    if (timer_initialized) {
        return;
    }

    /* Ensure EL3 interrupts are supported */
	assert(plat_ic_has_interrupt_type(INTR_TYPE_EL3));

    /* Store the realm descriptor */
    realm_descriptor = rd;

    /* Set up the timer to fire after REALM_DESTROY_TIMER_SECONDS */
    uint64_t timer_value = read_cntpct_el0() + 
                          (read_cntfrq_el0() * REALM_DESTROY_TIMER_SECONDS);
    write_cntps_cval_el1(timer_value);

    uint32_t ctl = 0;

    /* Enable the secure physical timer */
    set_cntp_ctl_enable(ctl);
    write_cntps_ctl_el1(ctl);

    /* Register the timer interrupt handler */
    uint64_t flags = 0;
    /* Route EL3 interrupts to EL3 from both secure and non-secure states */
    set_interrupt_rm_flag(flags, SECURE);
    set_interrupt_rm_flag(flags, NON_SECURE);
    int32_t rc = register_interrupt_type_handler(INTR_TYPE_EL3,
                                               rmmd_timer_handler,
                                               flags);
    if (rc != 0) {
        ERROR("Failed to register timer handler\n");
        return;
    }

    timer_initialized = true;
    INFO("Realm timer initialized for realm 0x%lx\n", rd);
} 