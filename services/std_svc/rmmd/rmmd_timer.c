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

#include <arch.h>

#include "rmmd_private.h"

/* Timer configuration */
// how long after the realm is created that timer will be triggered
#define REALM_DESTROY_TIMER_SECONDS 1

/* Timer state */
static bool timer_triggered = false;
static uint64_t realm_descriptor = 0;

/*******************************************************************************
 * Timer interrupt handler that will trigger realm destruction
 ******************************************************************************/
uint64_t rmmd_timer_handler(uint32_t id,
                                 uint32_t flags,
                                 void *handle,
                                 void *cookie)
{
    uint32_t irq = plat_ic_acknowledge_interrupt();

    /* Acknowledge the interrupt */
    INFO("Inside timer interrupt handler\n");
    uint64_t rc = 0;
    timer_triggered = true;
    
    assert(get_cntp_ctl_istatus(read_cntps_ctl_el1()));

    assert(irq == EL3_TIMER_IRQ);

    /* Call the specified SMC with the realm descriptor */
    if (realm_descriptor != 0) {
        write_cntps_ctl_el1(0);
        INFO("Timer triggered\n");
        
        /* Get the RMM context */
        void *ctx_handle = cm_get_context(NON_SECURE);
        assert(ctx_handle != NULL);

        rc = rmmd_rmi_handler(RMI_DATA_DESTROY_ALL_FID, 
                realm_descriptor, 0, 0, 0, 
                NULL, ctx_handle, SMC_FROM_NON_SECURE);
    } else {
        INFO("No realm descriptor\n");
    }
    // ??? 
    plat_ic_end_of_interrupt(irq);

    /* If no realm descriptor, just return to the caller */
    return rc;
}



/*******************************************************************************
 * Initialize the timer for realm destruction
 ******************************************************************************/
void rmmd_timer_init(uint64_t rd)
{
    realm_descriptor = rd;

    assert(read_scr_el3() & SCR_FIQ_BIT);
    assert(read_scr_el3() & SCR_IRQ_BIT);

    uint64_t freq = read_cntfrq_el0();
    INFO("time now = %lu\n", read_cntpct_el0());
    uint64_t expire = read_cntpct_el0() + (REALM_DESTROY_TIMER_SECONDS * freq / 100);
    INFO("time expire = %lu\n", expire);
    write_cntps_cval_el1(expire);
    // isb();
    
    uint32_t ctl = CNTP_CTL_ENABLE_BIT;  // Enable
    // uint32_t ctl = 0;
    ctl &= ~CNTP_CTL_IMASK_BIT;         // Unmask
    write_cntps_ctl_el1(ctl);
    // isb();
    // write_cntps_ctl_el1(CNTP_CTL_ENABLE_BIT);  // Enable timer, unmasked

    assert(read_cntps_ctl_el1() > read_cntpct_el0());

    assert(read_cntps_ctl_el1() & CNTP_CTL_ENABLE_BIT);
    assert((read_cntps_ctl_el1() & CNTP_CTL_IMASK_BIT) == 0);

    INFO("EL3 timer armed: will fire in %d sec for realm 0x%lx\n",
         REALM_DESTROY_TIMER_SECONDS, rd);

    // INFO("Initializing timer for realm 0x%lx\n", rd);

    // if (timer_initialized) {
    //     INFO("Timer already initialized\n");
    //     return;
    // }

    // /* Ensure EL3 interrupts are supported */
    // assert(plat_ic_has_interrupt_type(INTR_TYPE_EL3));

    // /* Store the realm descriptor */
    // realm_descriptor = rd;

    // /* Set up the timer to fire after REALM_DESTROY_TIMER_SECONDS */
    // uint64_t timer_value = read_cntpct_el0() + 
    //                       (read_cntfrq_el0() * REALM_DESTROY_TIMER_SECONDS / 1000 );
    // write_cntps_cval_el1(timer_value);
    // INFO("Current time: %lu\n", read_cntpct_el0());
    // INFO("Timer value set to: %lu\n", timer_value);

    // /* Configure timer control register */
    // uint32_t ctl = CNTP_CTL_ENABLE_BIT;  // Enable
    // ctl &= ~CNTP_CTL_IMASK_BIT;         // Unmask
    // write_cntps_ctl_el1(ctl);

    // INFO("EL3 timer set to fire in %d seconds\n", REALM_DESTROY_TIMER_SECONDS);

    // /* Register the timer interrupt handler */
    // uint64_t flags = 0;
    // /* Route EL3 interrupts to EL3 from both secure and non-secure states */
    // set_interrupt_rm_flag(flags, SECURE);
    // set_interrupt_rm_flag(flags, NON_SECURE);

    // /* Register the interrupt handler */
    // int32_t rc = register_interrupt_type_handler(INTR_TYPE_EL3,
    //                                            rmmd_timer_handler,
    //                                            flags);
    // // // TODO: remove this for test only
    // // rmmd_timer_handler(0, flags, NULL, NULL);

    // if (rc != 0) {
    //     ERROR("Failed to register timer handler\n");
    //     return;
    // }

    // timer_initialized = true;
    // INFO("Realm timer initialized for realm 0x%lx\n", rd);
} 