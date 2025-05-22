/*
 * Copyright (c) 2021-2024, Arm Limited and Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef RMMD_PRIVATE_H
#define RMMD_PRIVATE_H

#include <context.h>

/*******************************************************************************
 * Constants that allow assembler code to preserve callee-saved registers of the
 * C runtime context while performing a security state switch.
 ******************************************************************************/
#define RMMD_C_RT_CTX_X19		0x0
#define RMMD_C_RT_CTX_X20		0x8
#define RMMD_C_RT_CTX_X21		0x10
#define RMMD_C_RT_CTX_X22		0x18
#define RMMD_C_RT_CTX_X23		0x20
#define RMMD_C_RT_CTX_X24		0x28
#define RMMD_C_RT_CTX_X25		0x30
#define RMMD_C_RT_CTX_X26		0x38
#define RMMD_C_RT_CTX_X27		0x40
#define RMMD_C_RT_CTX_X28		0x48
#define RMMD_C_RT_CTX_X29		0x50
#define RMMD_C_RT_CTX_X30		0x58

#define RMMD_C_RT_CTX_SIZE		0x60
#define RMMD_C_RT_CTX_ENTRIES		(RMMD_C_RT_CTX_SIZE >> DWORD_SHIFT)

/*******************************************************************************
 * DEST: Constant assumption on max number of values per Realm.
 ******************************************************************************/
#define MAX_REALM_NUMS      4    // Max number of realms active at one time
#define MAX_DATA_GRANULES   32   // Expected max number of data granules
#define MAX_RECS            4    // Expected REC count per Realm
#define MAX_RTT_PAGES       128  // Expected max number of RTT pages

/*******************************************************************************
 * DEST: Function fid of the calls RMM.
 ******************************************************************************/
#define RMI_REALM_CREATE_FID     0xc4000158
#define RMI_RTT_CREATE_FID       0xc400015d
#define RMI_DATA_CREATE_FID      0xc4000153
#define RMI_REC_CREATE_FID       0xc400015a
#define RMI_REALM_ACTIVATE_FID   0xc4000157
#define RMI_DATA_DESTROY_ALL_FID 0xc400016a

#define RMI_REC_DESTROY_FID    0xc400015b
#define RMI_DATA_DESTROY_FID   0xc4000155
#define RMI_RTT_DESTROY_FID    0xc400015e
#define RMI_REALM_DESTROY_FID  0xc4000159

#ifndef __ASSEMBLER__
#include <stdint.h>

/*
 * Data structure used by the RMM dispatcher (RMMD) in EL3 to track context of
 * the RMM at R-EL2.
 */
typedef struct rmmd_rmm_context {
	uint64_t c_rt_ctx;
	cpu_context_t cpu_ctx;
} rmmd_rmm_context_t;

/*
 * DEST: Data stcture that contains info on the realm addr space mapping.
 * There can be at most MAX_REALM_NUMS ammount of realm_info_t.
 * Define Variables per realm. Each realm contain the following values:
 * rd_addr (defines which realm),
 * rtt_lvl_addrs[] (list of other rtt on other levels),
 * data_lvl_addrs[] (list of all data granules allocated),
 * rec_addr[] (list of recs used for in the run)
 */
typedef struct rtt_info {
	uint64_t rtt_addr;
	uint64_t ipa;
	uint64_t lvl;
} rtt_info_t;

typedef struct data_info {
	uint64_t data_addr;
	uint64_t ipa;
} data_info_t;

typedef struct realm_info {
    uint64_t rd; 			// Realm Descriptor
	rtt_info_t rtt_info[MAX_RTT_PAGES];
    uint32_t num_rtt;
	data_info_t data_addrs[MAX_DATA_GRANULES];  // Allocated data granules
	uint32_t num_data;
    uint64_t rec_addrs[MAX_RECS];               // REC addresses used during runtime
    uint32_t num_recs;
} realm_info_t;

/* Functions used to enter/exit the RMM synchronously */
uint64_t rmmd_rmm_sync_entry(rmmd_rmm_context_t *ctx);
__dead2 void rmmd_rmm_sync_exit(uint64_t rc);

/* DEST: Functions to save realm info */
uint64_t rmmd_smc_save_values(cpu_context_t *ctx, uint64_t x0, 
					uint64_t x1, uint64_t x2, uint64_t x3,
					uint64_t x4, void *handle);

/* Functions implementing attestation utilities for RMM */
int rmmd_attest_get_platform_token(uint64_t buf_pa, uint64_t *buf_size,
				   uint64_t c_size,
				   uint64_t *remaining_len);
int rmmd_attest_get_signing_key(uint64_t buf_pa, uint64_t *buf_size,
				uint64_t ecc_curve);
uint64_t rmmd_el3_token_sign(void *handle, uint64_t x1, uint64_t x2,
				    uint64_t x3, uint64_t x4);

/* Timer functions for realm destruction */
void rmmd_timer_init(uint64_t rd);

/* Assembly helpers */
uint64_t rmmd_rmm_enter(uint64_t *c_rt_ctx);
void __dead2 rmmd_rmm_exit(uint64_t c_rt_ctx, uint64_t ret);

/* Reference to PM ops for the RMMD */
extern const spd_pm_ops_t rmmd_pm;

#endif /* __ASSEMBLER__ */

#endif /* RMMD_PRIVATE_H */
