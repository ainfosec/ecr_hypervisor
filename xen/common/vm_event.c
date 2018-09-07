/******************************************************************************
 * vm_event.c
 *
 * VM event support.
 *
 * Copyright (c) 2009 Citrix Systems, Inc. (Patrick Colp)
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
 * along with this program; If not, see <http://www.gnu.org/licenses/>.
 */


#include <xen/sched.h>
#include <xen/event.h>
#include <xen/wait.h>
#include <xen/vm_event.h>
#include <xen/mem_access.h>
#include <asm/p2m.h>
#include <asm/monitor.h>
#include <asm/vm_event.h>
#include <xsm/xsm.h>

/* for public/io/ring.h macros */
#define xen_mb()   smp_mb()
#define xen_rmb()  smp_rmb()
#define xen_wmb()  smp_wmb()

#define vm_event_ring_lock_init(_ved)  spin_lock_init(&(_ved)->ring_lock)
#define vm_event_ring_lock(_ved)       spin_lock(&(_ved)->ring_lock)
#define vm_event_ring_unlock(_ved)     spin_unlock(&(_ved)->ring_lock)

static int vm_event_enable(
    struct domain *d,
    xen_domctl_vm_event_op_t *vec,
    struct vm_event_domain *ved,
    int pause_flag,
    int param,
    xen_event_channel_notification_t notification_fn)
{
    int rc;
    unsigned long ring_gfn = d->arch.hvm_domain.params[param];

    /* Only one helper at a time. If the helper crashed,
     * the ring is in an undefined state and so is the guest.
     */
    if ( ved->ring_page )
        return -EBUSY;

    /* The parameter defaults to zero, and it should be
     * set to something */
    if ( ring_gfn == 0 )
        return -ENOSYS;

    vm_event_ring_lock_init(ved);
    vm_event_ring_lock(ved);

    rc = vm_event_init_domain(d);

    if ( rc < 0 )
        goto err;

    rc = prepare_ring_for_helper(d, ring_gfn, &ved->ring_pg_struct,
                                    &ved->ring_page);
    if ( rc < 0 )
        goto err;

    /* Set the number of currently blocked vCPUs to 0. */
    ved->blocked = 0;

    /* Allocate event channel */
    rc = alloc_unbound_xen_event_channel(d, 0, current->domain->domain_id,
                                         notification_fn);
    if ( rc < 0 )
        goto err;

    ved->xen_port = vec->port = rc;

    /* Prepare ring buffer */
    FRONT_RING_INIT(&ved->front_ring,
                    (vm_event_sring_t *)ved->ring_page,
                    PAGE_SIZE);

    /* Save the pause flag for this particular ring. */
    ved->pause_flag = pause_flag;

    /* Initialize the last-chance wait queue. */
    init_waitqueue_head(&ved->wq);

    vm_event_ring_unlock(ved);
    return 0;

 err:
    destroy_ring_for_helper(&ved->ring_page,
                            ved->ring_pg_struct);
    vm_event_ring_unlock(ved);

    return rc;
}

static unsigned int vm_event_ring_available(struct vm_event_domain *ved)
{
    int avail_req = RING_FREE_REQUESTS(&ved->front_ring);
    avail_req -= ved->target_producers;
    avail_req -= ved->foreign_producers;

    BUG_ON(avail_req < 0);

    return avail_req;
}

/*
 * vm_event_wake_blocked() will wakeup vcpus waiting for room in the
 * ring. These vCPUs were paused on their way out after placing an event,
 * but need to be resumed where the ring is capable of processing at least
 * one event from them.
 */
static void vm_event_wake_blocked(struct domain *d, struct vm_event_domain *ved)
{
    struct vcpu *v;
    unsigned int avail_req = vm_event_ring_available(ved);

    if ( avail_req == 0 || ved->blocked == 0 )
        return;

    /* We remember which vcpu last woke up to avoid scanning always linearly
     * from zero and starving higher-numbered vcpus under high load */
    if ( d->vcpu )
    {
        int i, j, k;

        for (i = ved->last_vcpu_wake_up + 1, j = 0; j < d->max_vcpus; i++, j++)
        {
            k = i % d->max_vcpus;
            v = d->vcpu[k];
            if ( !v )
                continue;

            if ( !(ved->blocked) || avail_req == 0 )
               break;

            if ( test_and_clear_bit(ved->pause_flag, &v->pause_flags) )
            {
                vcpu_unpause(v);
                avail_req--;
                ved->blocked--;
                ved->last_vcpu_wake_up = k;
            }
        }
    }
}

/*
 * In the event that a vCPU attempted to place an event in the ring and
 * was unable to do so, it is queued on a wait queue.  These are woken as
 * needed, and take precedence over the blocked vCPUs.
 */
static void vm_event_wake_queued(struct domain *d, struct vm_event_domain *ved)
{
    unsigned int avail_req = vm_event_ring_available(ved);

    if ( avail_req > 0 )
        wake_up_nr(&ved->wq, avail_req);
}

/*
 * vm_event_wake() will wakeup all vcpus waiting for the ring to
 * become available.  If we have queued vCPUs, they get top priority. We
 * are guaranteed that they will go through code paths that will eventually
 * call vm_event_wake() again, ensuring that any blocked vCPUs will get
 * unpaused once all the queued vCPUs have made it through.
 */
void vm_event_wake(struct domain *d, struct vm_event_domain *ved)
{
    if (!list_empty(&ved->wq.list))
        vm_event_wake_queued(d, ved);
    else
        vm_event_wake_blocked(d, ved);
}

static int vm_event_disable(struct domain *d, struct vm_event_domain *ved)
{
    if ( ved->ring_page )
    {
        struct vcpu *v;

        vm_event_ring_lock(ved);

        if ( !list_empty(&ved->wq.list) )
        {
            vm_event_ring_unlock(ved);
            return -EBUSY;
        }

        /* Free domU's event channel and leave the other one unbound */
        free_xen_event_channel(d, ved->xen_port);

        /* Unblock all vCPUs */
        for_each_vcpu ( d, v )
        {
            if ( test_and_clear_bit(ved->pause_flag, &v->pause_flags) )
            {
                vcpu_unpause(v);
                ved->blocked--;
            }
        }

        destroy_ring_for_helper(&ved->ring_page,
                                ved->ring_pg_struct);

        vm_event_cleanup_domain(d);

        vm_event_ring_unlock(ved);
    }

    return 0;
}

static inline void vm_event_release_slot(struct domain *d,
                                         struct vm_event_domain *ved)
{
    /* Update the accounting */
    if ( current->domain == d )
        ved->target_producers--;
    else
        ved->foreign_producers--;

    /* Kick any waiters */
    vm_event_wake(d, ved);
}

/*
 * vm_event_mark_and_pause() tags vcpu and put it to sleep.
 * The vcpu will resume execution in vm_event_wake_blocked().
 */
void vm_event_mark_and_pause(struct vcpu *v, struct vm_event_domain *ved)
{
    if ( !test_and_set_bit(ved->pause_flag, &v->pause_flags) )
    {
        vcpu_pause_nosync(v);
        ved->blocked++;
    }
}

/*
 * This must be preceded by a call to claim_slot(), and is guaranteed to
 * succeed.  As a side-effect however, the vCPU may be paused if the ring is
 * overly full and its continued execution would cause stalling and excessive
 * waiting.  The vCPU will be automatically unpaused when the ring clears.
 */
void vm_event_put_request(struct domain *d,
                          struct vm_event_domain *ved,
                          vm_event_request_t *req)
{
    vm_event_front_ring_t *front_ring;
    int free_req;
    unsigned int avail_req;
    RING_IDX req_prod;
    struct vcpu *curr = current;

    if ( curr->domain != d )
    {
        req->flags |= VM_EVENT_FLAG_FOREIGN;
#ifndef NDEBUG
        if ( !(req->flags & VM_EVENT_FLAG_VCPU_PAUSED) )
            gdprintk(XENLOG_G_WARNING, "d%dv%d was not paused.\n",
                     d->domain_id, req->vcpu_id);
#endif
    }

    req->version = VM_EVENT_INTERFACE_VERSION;

    vm_event_ring_lock(ved);

    /* Due to the reservations, this step must succeed. */
    front_ring = &ved->front_ring;
    free_req = RING_FREE_REQUESTS(front_ring);
    ASSERT(free_req > 0);

    /* Copy request */
    req_prod = front_ring->req_prod_pvt;
    memcpy(RING_GET_REQUEST(front_ring, req_prod), req, sizeof(*req));
    req_prod++;

    /* Update ring */
    front_ring->req_prod_pvt = req_prod;
    RING_PUSH_REQUESTS(front_ring);

    /* We've actually *used* our reservation, so release the slot. */
    vm_event_release_slot(d, ved);

    /* Give this vCPU a black eye if necessary, on the way out.
     * See the comments above wake_blocked() for more information
     * on how this mechanism works to avoid waiting. */
    avail_req = vm_event_ring_available(ved);
    if( curr->domain == d && avail_req < d->max_vcpus &&
        !atomic_read(&curr->vm_event_pause_count) )
        vm_event_mark_and_pause(curr, ved);

    vm_event_ring_unlock(ved);

    notify_via_xen_event_channel(d, ved->xen_port);
}

int vm_event_get_response(struct domain *d, struct vm_event_domain *ved,
                          vm_event_response_t *rsp)
{
    vm_event_front_ring_t *front_ring;
    RING_IDX rsp_cons;

    vm_event_ring_lock(ved);

    front_ring = &ved->front_ring;
    rsp_cons = front_ring->rsp_cons;

    if ( !RING_HAS_UNCONSUMED_RESPONSES(front_ring) )
    {
        vm_event_ring_unlock(ved);
        return 0;
    }

    /* Copy response */
    memcpy(rsp, RING_GET_RESPONSE(front_ring, rsp_cons), sizeof(*rsp));
    rsp_cons++;

    /* Update ring */
    front_ring->rsp_cons = rsp_cons;
    front_ring->sring->rsp_event = rsp_cons + 1;

    /* Kick any waiters -- since we've just consumed an event,
     * there may be additional space available in the ring. */
    vm_event_wake(d, ved);

    vm_event_ring_unlock(ved);

    return 1;
}

/*
 * Pull all responses from the given ring and unpause the corresponding vCPU
 * if required. Based on the response type, here we can also call custom
 * handlers.
 *
 * Note: responses are handled the same way regardless of which ring they
 * arrive on.
 */
void vm_event_resume(struct domain *d, struct vm_event_domain *ved)
{
    vm_event_response_t rsp;

    /*
     * vm_event_resume() runs in either XEN_DOMCTL_VM_EVENT_OP_*, or
     * EVTCHN_send context from the introspection consumer. Both contexts
     * are guaranteed not to be the subject of vm_event responses.
     * While we could ASSERT(v != current) for each VCPU in d in the loop
     * below, this covers the case where we would need to iterate over all
     * of them more succintly.
     */
    ASSERT(d != current->domain);

    /* Pull all responses off the ring. */
    while ( vm_event_get_response(d, ved, &rsp) )
    {
        struct vcpu *v;

        if ( rsp.version != VM_EVENT_INTERFACE_VERSION )
        {
            printk(XENLOG_G_WARNING "vm_event interface version mismatch\n");
            continue;
        }

        /* Validate the vcpu_id in the response. */
        if ( (rsp.vcpu_id >= d->max_vcpus) || !d->vcpu[rsp.vcpu_id] )
            continue;

        v = d->vcpu[rsp.vcpu_id];

        /*
         * In some cases the response type needs extra handling, so here
         * we call the appropriate handlers.
         */

        /* Check flags which apply only when the vCPU is paused */
        if ( atomic_read(&v->vm_event_pause_count) )
        {
#ifdef CONFIG_HAS_MEM_PAGING
            if ( rsp.reason == VM_EVENT_REASON_MEM_PAGING )
                p2m_mem_paging_resume(d, &rsp);
#endif

            /*
             * Check emulation flags in the arch-specific handler only, as it
             * has to set arch-specific flags when supported, and to avoid
             * bitmask overhead when it isn't supported.
             */
            vm_event_emulate_check(v, &rsp);

            /*
             * Check in arch-specific handler to avoid bitmask overhead when
             * not supported.
             */
            vm_event_register_write_resume(v, &rsp);

            /*
             * Check in arch-specific handler to avoid bitmask overhead when
             * not supported.
             */
            vm_event_toggle_singlestep(d, v, &rsp);

            /* Check for altp2m switch */
            if ( rsp.flags & VM_EVENT_FLAG_ALTERNATE_P2M )
                p2m_altp2m_check(v, rsp.altp2m_idx);

            if ( rsp.flags & VM_EVENT_FLAG_SET_REGISTERS )
                vm_event_set_registers(v, &rsp);

            if ( rsp.flags & VM_EVENT_FLAG_GET_NEXT_INTERRUPT )
                vm_event_monitor_next_interrupt(v);

            if ( rsp.flags & VM_EVENT_FLAG_VCPU_PAUSED )
                vm_event_vcpu_unpause(v);
        }
    }
}

void vm_event_cancel_slot(struct domain *d, struct vm_event_domain *ved)
{
    vm_event_ring_lock(ved);
    vm_event_release_slot(d, ved);
    vm_event_ring_unlock(ved);
}

static int vm_event_grab_slot(struct vm_event_domain *ved, int foreign)
{
    unsigned int avail_req;

    if ( !ved->ring_page )
        return -ENOSYS;

    vm_event_ring_lock(ved);

    avail_req = vm_event_ring_available(ved);
    if ( avail_req == 0 )
    {
        vm_event_ring_unlock(ved);
        return -EBUSY;
    }

    if ( !foreign )
        ved->target_producers++;
    else
        ved->foreign_producers++;

    vm_event_ring_unlock(ved);

    return 0;
}

/* Simple try_grab wrapper for use in the wait_event() macro. */
static int vm_event_wait_try_grab(struct vm_event_domain *ved, int *rc)
{
    *rc = vm_event_grab_slot(ved, 0);
    return *rc;
}

/* Call vm_event_grab_slot() until the ring doesn't exist, or is available. */
static int vm_event_wait_slot(struct vm_event_domain *ved)
{
    int rc = -EBUSY;
    wait_event(ved->wq, vm_event_wait_try_grab(ved, &rc) != -EBUSY);
    return rc;
}

bool_t vm_event_check_ring(struct vm_event_domain *ved)
{
    return (ved->ring_page != NULL);
}

/*
 * Determines whether or not the current vCPU belongs to the target domain,
 * and calls the appropriate wait function.  If it is a guest vCPU, then we
 * use vm_event_wait_slot() to reserve a slot.  As long as there is a ring,
 * this function will always return 0 for a guest.  For a non-guest, we check
 * for space and return -EBUSY if the ring is not available.
 *
 * Return codes: -ENOSYS: the ring is not yet configured
 *               -EBUSY: the ring is busy
 *               0: a spot has been reserved
 *
 */
int __vm_event_claim_slot(struct domain *d, struct vm_event_domain *ved,
                          bool_t allow_sleep)
{
    if ( (current->domain == d) && allow_sleep )
        return vm_event_wait_slot(ved);
    else
        return vm_event_grab_slot(ved, (current->domain != d));
}

#ifdef CONFIG_HAS_MEM_PAGING
/* Registered with Xen-bound event channel for incoming notifications. */
static void mem_paging_notification(struct vcpu *v, unsigned int port)
{
    if ( likely(v->domain->vm_event->paging.ring_page != NULL) )
        vm_event_resume(v->domain, &v->domain->vm_event->paging);
}
#endif

/* Registered with Xen-bound event channel for incoming notifications. */
static void monitor_notification(struct vcpu *v, unsigned int port)
{
    if ( likely(v->domain->vm_event->monitor.ring_page != NULL) )
        vm_event_resume(v->domain, &v->domain->vm_event->monitor);
}

#ifdef CONFIG_HAS_MEM_SHARING
/* Registered with Xen-bound event channel for incoming notifications. */
static void mem_sharing_notification(struct vcpu *v, unsigned int port)
{
    if ( likely(v->domain->vm_event->share.ring_page != NULL) )
        vm_event_resume(v->domain, &v->domain->vm_event->share);
}
#endif

/* Clean up on domain destruction */
void vm_event_cleanup(struct domain *d)
{
#ifdef CONFIG_HAS_MEM_PAGING
    if ( d->vm_event->paging.ring_page )
    {
        /* Destroying the wait queue head means waking up all
         * queued vcpus. This will drain the list, allowing
         * the disable routine to complete. It will also drop
         * all domain refs the wait-queued vcpus are holding.
         * Finally, because this code path involves previously
         * pausing the domain (domain_kill), unpausing the
         * vcpus causes no harm. */
        destroy_waitqueue_head(&d->vm_event->paging.wq);
        (void)vm_event_disable(d, &d->vm_event->paging);
    }
#endif
    if ( d->vm_event->monitor.ring_page )
    {
        destroy_waitqueue_head(&d->vm_event->monitor.wq);
        (void)vm_event_disable(d, &d->vm_event->monitor);
    }
#ifdef CONFIG_HAS_MEM_SHARING
    if ( d->vm_event->share.ring_page )
    {
        destroy_waitqueue_head(&d->vm_event->share.wq);
        (void)vm_event_disable(d, &d->vm_event->share);
    }
#endif
}

int vm_event_domctl(struct domain *d, xen_domctl_vm_event_op_t *vec,
                    XEN_GUEST_HANDLE_PARAM(void) u_domctl)
{
    int rc;

    rc = xsm_vm_event_control(XSM_PRIV, d, vec->mode, vec->op);
    if ( rc )
        return rc;

    if ( unlikely(d == current->domain) ) /* no domain_pause() */
    {
        gdprintk(XENLOG_INFO, "Tried to do a memory event op on itself.\n");
        return -EINVAL;
    }

    if ( unlikely(d->is_dying) )
    {
        gdprintk(XENLOG_INFO, "Ignoring memory event op on dying domain %u\n",
                 d->domain_id);
        return 0;
    }

    if ( unlikely(d->vcpu == NULL) || unlikely(d->vcpu[0] == NULL) )
    {
        gdprintk(XENLOG_INFO,
                 "Memory event op on a domain (%u) with no vcpus\n",
                 d->domain_id);
        return -EINVAL;
    }

    rc = -ENOSYS;

    switch ( vec->mode )
    {
#ifdef CONFIG_HAS_MEM_PAGING
    case XEN_DOMCTL_VM_EVENT_OP_PAGING:
    {
        struct vm_event_domain *ved = &d->vm_event->paging;
        rc = -EINVAL;

        switch( vec->op )
        {
        case XEN_VM_EVENT_ENABLE:
        {
            struct p2m_domain *p2m = p2m_get_hostp2m(d);

            rc = -EOPNOTSUPP;
            /* hvm fixme: p2m_is_foreign types need addressing */
            if ( is_hvm_domain(hardware_domain) )
                break;

            rc = -ENODEV;
            /* Only HAP is supported */
            if ( !hap_enabled(d) )
                break;

            /* No paging if iommu is used */
            rc = -EMLINK;
            if ( unlikely(need_iommu(d)) )
                break;

            rc = -EXDEV;
            /* Disallow paging in a PoD guest */
            if ( p2m->pod.entry_count )
                break;

            /* domain_pause() not required here, see XSA-99 */
            rc = vm_event_enable(d, vec, ved, _VPF_mem_paging,
                                 HVM_PARAM_PAGING_RING_PFN,
                                 mem_paging_notification);
        }
        break;

        case XEN_VM_EVENT_DISABLE:
            if ( ved->ring_page )
            {
                domain_pause(d);
                rc = vm_event_disable(d, ved);
                domain_unpause(d);
            }
            break;

        case XEN_VM_EVENT_RESUME:
            if ( ved->ring_page )
                vm_event_resume(d, ved);
            else
                rc = -ENODEV;
            break;

        default:
            rc = -ENOSYS;
            break;
        }
    }
    break;
#endif

    case XEN_DOMCTL_VM_EVENT_OP_MONITOR:
    {
        struct vm_event_domain *ved = &d->vm_event->monitor;
        rc = -EINVAL;

        switch( vec->op )
        {
        case XEN_VM_EVENT_ENABLE:
            /* domain_pause() not required here, see XSA-99 */
            rc = arch_monitor_init_domain(d);
            if ( rc )
                break;
            rc = vm_event_enable(d, vec, ved, _VPF_mem_access,
                                 HVM_PARAM_MONITOR_RING_PFN,
                                 monitor_notification);
            break;

        case XEN_VM_EVENT_DISABLE:
            if ( ved->ring_page )
            {
                domain_pause(d);
                rc = vm_event_disable(d, ved);
                arch_monitor_cleanup_domain(d);
                domain_unpause(d);
            }
            break;

        case XEN_VM_EVENT_RESUME:
            if ( ved->ring_page )
                vm_event_resume(d, ved);
            else
                rc = -ENODEV;
            break;

        default:
            rc = -ENOSYS;
            break;
        }
    }
    break;

#ifdef CONFIG_HAS_MEM_SHARING
    case XEN_DOMCTL_VM_EVENT_OP_SHARING:
    {
        struct vm_event_domain *ved = &d->vm_event->share;
        rc = -EINVAL;

        switch( vec->op )
        {
        case XEN_VM_EVENT_ENABLE:
            rc = -EOPNOTSUPP;
            /* hvm fixme: p2m_is_foreign types need addressing */
            if ( is_hvm_domain(hardware_domain) )
                break;

            rc = -ENODEV;
            /* Only HAP is supported */
            if ( !hap_enabled(d) )
                break;

            /* domain_pause() not required here, see XSA-99 */
            rc = vm_event_enable(d, vec, ved, _VPF_mem_sharing,
                                 HVM_PARAM_SHARING_RING_PFN,
                                 mem_sharing_notification);
            break;

        case XEN_VM_EVENT_DISABLE:
            if ( ved->ring_page )
            {
                domain_pause(d);
                rc = vm_event_disable(d, ved);
                domain_unpause(d);
            }
            break;

        case XEN_VM_EVENT_RESUME:
            if ( ved->ring_page )
                vm_event_resume(d, ved);
            else
                rc = -ENODEV;
            break;

        default:
            rc = -ENOSYS;
            break;
        }
    }
    break;
#endif

    default:
        rc = -ENOSYS;
    }

    return rc;
}

void vm_event_vcpu_pause(struct vcpu *v)
{
    ASSERT(v == current);

    atomic_inc(&v->vm_event_pause_count);
    vcpu_pause_nosync(v);
}

void vm_event_vcpu_unpause(struct vcpu *v)
{
    int old, new, prev = v->vm_event_pause_count.counter;

    /*
     * All unpause requests as a result of toolstack responses.
     * Prevent underflow of the vcpu pause count.
     */
    do
    {
        old = prev;
        new = old - 1;

        if ( new < 0 )
        {
            printk(XENLOG_G_WARNING
                   "%pv vm_event: Too many unpause attempts\n", v);
            return;
        }

        prev = cmpxchg(&v->vm_event_pause_count.counter, old, new);
    } while ( prev != old );

    vcpu_unpause(v);
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
