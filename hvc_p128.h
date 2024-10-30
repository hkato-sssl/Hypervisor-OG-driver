// SPDX-License-Identifier: GPL-2.0-only
/*
 * hvc_p128.h
 *
 * (C) 2024 Hidekazu Kato
 */

#ifndef HYPERVISOR_HVC_P128
#define HYPERVISOR_HVC_P128

#ifdef __cplusplus
extern "C" {
#endif

#include "asm/types.h"

int hvc_p128_nr_interfaces(u32 id, u16 *nr_ifs);
int hvc_p128_get_interrupt_no(u32 id, u16 ifno, u16 *nr_ifs);
int hvc_p128_get_status(u32 id, u16 ifno, u32 *status);
int hvc_p128_get_event(u32 id, u16 ifno, u32 *event);
int hvc_p128_send(u32 id, u16 ifno, const void *buff);
int hvc_p128_receive(u32 id, u16 ifno, void *buff);

#ifdef __cplusplus
}
#endif

#endif /* HYPERVISOR_HVC_P128 */

