/******************************************************************************
 * hypercall.h
 */

#ifndef __XEN_HYPERCALL_H__
#define __XEN_HYPERCALL_H__

#include <xen/types.h>
#include <xen/time.h>
#include <public/xen.h>
#include <public/domctl.h>
#include <public/sysctl.h>
#include <public/platform.h>
#include <public/event_channel.h>
#include <public/tmem.h>
#include <public/version.h>
#include <public/pmu.h>
#include <public/hvm/dm_op.h>
#include <asm/hypercall.h>
#include <xsm/xsm.h>

extern long
do_sched_op(
    int cmd,
    XEN_GUEST_HANDLE_PARAM(void) arg);

extern long
do_domctl(
    XEN_GUEST_HANDLE_PARAM(xen_domctl_t) u_domctl);

extern long
arch_do_domctl(
    struct xen_domctl *domctl, struct domain *d,
    XEN_GUEST_HANDLE_PARAM(xen_domctl_t) u_domctl);

extern long
do_sysctl(
    XEN_GUEST_HANDLE_PARAM(xen_sysctl_t) u_sysctl);

extern long
arch_do_sysctl(
    struct xen_sysctl *sysctl,
    XEN_GUEST_HANDLE_PARAM(xen_sysctl_t) u_sysctl);

extern long
do_platform_op(
    XEN_GUEST_HANDLE_PARAM(xen_platform_op_t) u_xenpf_op);

/*
 * To allow safe resume of do_memory_op() after preemption, we need to know
 * at what point in the page list to resume. For this purpose I steal the
 * high-order bits of the @cmd parameter, which are otherwise unused and zero.
 *
 * Note that both of these values are effectively part of the ABI, even if
 * we don't need to make them a formal part of it: A guest suspended for
 * migration in the middle of a continuation would fail to work if resumed on
 * a hypervisor using different values.
 */
#define MEMOP_EXTENT_SHIFT 6 /* cmd[:6] == start_extent */
#define MEMOP_CMD_MASK     ((1 << MEMOP_EXTENT_SHIFT) - 1)

extern long
do_memory_op(
    unsigned long cmd,
    XEN_GUEST_HANDLE_PARAM(void) arg);

extern long
do_multicall(
    XEN_GUEST_HANDLE_PARAM(multicall_entry_t) call_list,
    unsigned int nr_calls);

extern long
do_set_timer_op(
    s_time_t timeout);

extern long
do_event_channel_op(
    int cmd, XEN_GUEST_HANDLE_PARAM(void) arg);

extern long
do_xen_version(
    int cmd,
    XEN_GUEST_HANDLE_PARAM(void) arg);

extern long
do_console_io(
    int cmd,
    int count,
    XEN_GUEST_HANDLE_PARAM(char) buffer);

extern long
do_grant_table_op(
    unsigned int cmd,
    XEN_GUEST_HANDLE_PARAM(void) uop,
    unsigned int count);

extern long
do_vm_assist(
    unsigned int cmd,
    unsigned int type);

extern long
do_vcpu_op(
    int cmd,
    unsigned int vcpuid,
    XEN_GUEST_HANDLE_PARAM(void) arg);

struct vcpu;
extern long
arch_do_vcpu_op(int cmd,
    struct vcpu *v,
    XEN_GUEST_HANDLE_PARAM(void) arg);

extern long
do_nmi_op(
    unsigned int cmd,
    XEN_GUEST_HANDLE_PARAM(void) arg);

extern long
do_hvm_op(
    unsigned long op,
    XEN_GUEST_HANDLE_PARAM(void) arg);

extern long
do_kexec_op(
    unsigned long op,
    XEN_GUEST_HANDLE_PARAM(void) uarg);

extern long
do_xsm_op(
    XEN_GUEST_HANDLE_PARAM(xsm_op_t) u_xsm_op);

#ifdef CONFIG_TMEM
extern long
do_tmem_op(
    XEN_GUEST_HANDLE_PARAM(tmem_op_t) uops);
#endif

extern long
do_xenoprof_op(int op, XEN_GUEST_HANDLE_PARAM(void) arg);

extern long
do_xenpmu_op(unsigned int op, XEN_GUEST_HANDLE_PARAM(xen_pmu_params_t) arg);

extern long
do_dm_op(
    domid_t domid,
    unsigned int nr_bufs,
    XEN_GUEST_HANDLE_PARAM(xen_dm_op_buf_t) bufs);

extern long
do_vmcs_op(
    uint16_t domain_id,
    unsigned long field,
    unsigned long value,
    bool enable,
    bool rdtsc_alert_flag);

extern long
do_vmwrite_2(
    unsigned int op1,
    unsigned int op2);

#ifdef CONFIG_COMPAT

extern int
compat_memory_op(
    unsigned int cmd,
    XEN_GUEST_HANDLE_PARAM(void) arg);

extern int
compat_grant_table_op(
    unsigned int cmd,
    XEN_GUEST_HANDLE_PARAM(void) uop,
    unsigned int count);

extern int
compat_vcpu_op(
    int cmd,
    unsigned int vcpuid,
    XEN_GUEST_HANDLE_PARAM(void) arg);

extern int
compat_xenoprof_op(int op, XEN_GUEST_HANDLE_PARAM(void) arg);

extern int
compat_xen_version(
    int cmd,
    XEN_GUEST_HANDLE_PARAM(void) arg);

extern int
compat_sched_op(
    int cmd,
    XEN_GUEST_HANDLE_PARAM(void) arg);

extern int
compat_set_timer_op(
    u32 lo,
    s32 hi);

extern int compat_xsm_op(
    XEN_GUEST_HANDLE_PARAM(xsm_op_t) op);

extern int compat_kexec_op(unsigned long op, XEN_GUEST_HANDLE_PARAM(void) uarg);

extern int compat_vm_assist(unsigned int cmd, unsigned int type);

DEFINE_XEN_GUEST_HANDLE(multicall_entry_compat_t);
extern int compat_multicall(
    XEN_GUEST_HANDLE_PARAM(multicall_entry_compat_t) call_list,
    uint32_t nr_calls);

#include <compat/hvm/dm_op.h>

extern int
compat_dm_op(
    domid_t domid,
    unsigned int nr_bufs,
    XEN_GUEST_HANDLE_PARAM(void) bufs);

#endif

void arch_get_xen_caps(xen_capabilities_info_t *info);

#endif /* __XEN_HYPERCALL_H__ */
