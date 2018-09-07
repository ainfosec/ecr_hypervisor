/*
 * Copyright (c) 2006, Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; If not, see <http://www.gnu.org/licenses/>.
 *
 * Copyright (C) Allen Kay <allen.m.kay@intel.com>
 * Copyright (C) Xiaohui Xin <xiaohui.xin@intel.com>
 */


#include <xen/sched.h>
#include <xen/iommu.h>
#include <xen/time.h>
#include <xen/pci.h>
#include <xen/pci_regs.h>
#include "iommu.h"
#include "dmar.h"
#include "vtd.h"
#include "extern.h"
#include "../ats.h"

#define VTD_QI_TIMEOUT	1

static int __must_check invalidate_sync(struct iommu *iommu);

static void print_qi_regs(struct iommu *iommu)
{
    u64 val;

    val = dmar_readq(iommu->reg, DMAR_IQA_REG);
    printk("DMAR_IQA_REG = %"PRIx64"\n", val);

    val = dmar_readq(iommu->reg, DMAR_IQH_REG);
    printk("DMAR_IQH_REG = %"PRIx64"\n", val);

    val = dmar_readq(iommu->reg, DMAR_IQT_REG);
    printk("DMAR_IQT_REG = %"PRIx64"\n", val);
}

static unsigned int qinval_next_index(struct iommu *iommu)
{
    u64 tail;

    tail = dmar_readq(iommu->reg, DMAR_IQT_REG);
    tail >>= QINVAL_INDEX_SHIFT;

    /* (tail+1 == head) indicates a full queue, wait for HW */
    while ( ( tail + 1 ) % QINVAL_ENTRY_NR ==
            ( dmar_readq(iommu->reg, DMAR_IQH_REG) >> QINVAL_INDEX_SHIFT ) )
        cpu_relax();

    return tail;
}

static void qinval_update_qtail(struct iommu *iommu, unsigned int index)
{
    u64 val;

    /* Need hold register lock when update tail */
    ASSERT( spin_is_locked(&iommu->register_lock) );
    val = (index + 1) % QINVAL_ENTRY_NR;
    dmar_writeq(iommu->reg, DMAR_IQT_REG, (val << QINVAL_INDEX_SHIFT));
}

static int __must_check queue_invalidate_context_sync(struct iommu *iommu,
                                                      u16 did, u16 source_id,
                                                      u8 function_mask,
                                                      u8 granu)
{
    unsigned long flags;
    unsigned int index;
    u64 entry_base;
    struct qinval_entry *qinval_entry, *qinval_entries;

    spin_lock_irqsave(&iommu->register_lock, flags);
    index = qinval_next_index(iommu);
    entry_base = iommu_qi_ctrl(iommu)->qinval_maddr +
                 ((index >> QINVAL_ENTRY_ORDER) << PAGE_SHIFT);
    qinval_entries = map_vtd_domain_page(entry_base);
    qinval_entry = &qinval_entries[index % (1 << QINVAL_ENTRY_ORDER)];

    qinval_entry->q.cc_inv_dsc.lo.type = TYPE_INVAL_CONTEXT;
    qinval_entry->q.cc_inv_dsc.lo.granu = granu;
    qinval_entry->q.cc_inv_dsc.lo.res_1 = 0;
    qinval_entry->q.cc_inv_dsc.lo.did = did;
    qinval_entry->q.cc_inv_dsc.lo.sid = source_id;
    qinval_entry->q.cc_inv_dsc.lo.fm = function_mask;
    qinval_entry->q.cc_inv_dsc.lo.res_2 = 0;
    qinval_entry->q.cc_inv_dsc.hi.res = 0;

    qinval_update_qtail(iommu, index);
    spin_unlock_irqrestore(&iommu->register_lock, flags);

    unmap_vtd_domain_page(qinval_entries);

    return invalidate_sync(iommu);
}

static int __must_check queue_invalidate_iotlb_sync(struct iommu *iommu,
                                                    u8 granu, u8 dr, u8 dw,
                                                    u16 did, u8 am, u8 ih,
                                                    u64 addr)
{
    unsigned long flags;
    unsigned int index;
    u64 entry_base;
    struct qinval_entry *qinval_entry, *qinval_entries;

    spin_lock_irqsave(&iommu->register_lock, flags);
    index = qinval_next_index(iommu);
    entry_base = iommu_qi_ctrl(iommu)->qinval_maddr +
                 ((index >> QINVAL_ENTRY_ORDER) << PAGE_SHIFT);
    qinval_entries = map_vtd_domain_page(entry_base);
    qinval_entry = &qinval_entries[index % (1 << QINVAL_ENTRY_ORDER)];

    qinval_entry->q.iotlb_inv_dsc.lo.type = TYPE_INVAL_IOTLB;
    qinval_entry->q.iotlb_inv_dsc.lo.granu = granu;
    qinval_entry->q.iotlb_inv_dsc.lo.dr = dr;
    qinval_entry->q.iotlb_inv_dsc.lo.dw = dw;
    qinval_entry->q.iotlb_inv_dsc.lo.res_1 = 0;
    qinval_entry->q.iotlb_inv_dsc.lo.did = did;
    qinval_entry->q.iotlb_inv_dsc.lo.res_2 = 0;

    qinval_entry->q.iotlb_inv_dsc.hi.am = am;
    qinval_entry->q.iotlb_inv_dsc.hi.ih = ih;
    qinval_entry->q.iotlb_inv_dsc.hi.res_1 = 0;
    qinval_entry->q.iotlb_inv_dsc.hi.addr = addr >> PAGE_SHIFT_4K;

    unmap_vtd_domain_page(qinval_entries);
    qinval_update_qtail(iommu, index);
    spin_unlock_irqrestore(&iommu->register_lock, flags);

    return invalidate_sync(iommu);
}

static int __must_check queue_invalidate_wait(struct iommu *iommu,
                                              u8 iflag, u8 sw, u8 fn,
                                              bool_t flush_dev_iotlb)
{
    volatile u32 poll_slot = QINVAL_STAT_INIT;
    unsigned int index;
    unsigned long flags;
    u64 entry_base;
    struct qinval_entry *qinval_entry, *qinval_entries;

    spin_lock_irqsave(&iommu->register_lock, flags);
    index = qinval_next_index(iommu);
    entry_base = iommu_qi_ctrl(iommu)->qinval_maddr +
                 ((index >> QINVAL_ENTRY_ORDER) << PAGE_SHIFT);
    qinval_entries = map_vtd_domain_page(entry_base);
    qinval_entry = &qinval_entries[index % (1 << QINVAL_ENTRY_ORDER)];

    qinval_entry->q.inv_wait_dsc.lo.type = TYPE_INVAL_WAIT;
    qinval_entry->q.inv_wait_dsc.lo.iflag = iflag;
    qinval_entry->q.inv_wait_dsc.lo.sw = sw;
    qinval_entry->q.inv_wait_dsc.lo.fn = fn;
    qinval_entry->q.inv_wait_dsc.lo.res_1 = 0;
    qinval_entry->q.inv_wait_dsc.lo.sdata = QINVAL_STAT_DONE;
    qinval_entry->q.inv_wait_dsc.hi.res_1 = 0;
    qinval_entry->q.inv_wait_dsc.hi.saddr = virt_to_maddr(&poll_slot) >> 2;

    unmap_vtd_domain_page(qinval_entries);
    qinval_update_qtail(iommu, index);
    spin_unlock_irqrestore(&iommu->register_lock, flags);

    /* Now we don't support interrupt method */
    if ( sw )
    {
        s_time_t timeout;

        /* In case all wait descriptor writes to same addr with same data */
        timeout = NOW() + MILLISECS(flush_dev_iotlb ?
                                    iommu_dev_iotlb_timeout : VTD_QI_TIMEOUT);

        while ( poll_slot != QINVAL_STAT_DONE )
        {
            if ( NOW() > timeout )
            {
                print_qi_regs(iommu);
                printk(XENLOG_WARNING VTDPREFIX
                       " Queue invalidate wait descriptor timed out\n");
                return -ETIMEDOUT;
            }
            cpu_relax();
        }
        return 0;
    }

    return -EOPNOTSUPP;
}

static int __must_check invalidate_sync(struct iommu *iommu)
{
    struct qi_ctrl *qi_ctrl = iommu_qi_ctrl(iommu);

    ASSERT(qi_ctrl->qinval_maddr);

    return queue_invalidate_wait(iommu, 0, 1, 1, 0);
}

static int __must_check dev_invalidate_sync(struct iommu *iommu,
                                            struct pci_dev *pdev, u16 did)
{
    struct qi_ctrl *qi_ctrl = iommu_qi_ctrl(iommu);
    int rc;

    ASSERT(qi_ctrl->qinval_maddr);
    rc = queue_invalidate_wait(iommu, 0, 1, 1, 1);
    if ( rc == -ETIMEDOUT )
    {
        struct domain *d = NULL;

        if ( test_bit(did, iommu->domid_bitmap) )
            d = rcu_lock_domain_by_id(iommu->domid_map[did]);

        /*
         * In case the domain has been freed or the IOMMU domid bitmap is
         * not valid, the device no longer belongs to this domain.
         */
        if ( d == NULL )
            return rc;

        iommu_dev_iotlb_flush_timeout(d, pdev);
        rcu_unlock_domain(d);
    }

    return rc;
}

int qinval_device_iotlb_sync(struct iommu *iommu, struct pci_dev *pdev,
                             u16 did, u16 size, u64 addr)
{
    unsigned long flags;
    unsigned int index;
    u64 entry_base;
    struct qinval_entry *qinval_entry, *qinval_entries;

    ASSERT(pdev);
    spin_lock_irqsave(&iommu->register_lock, flags);
    index = qinval_next_index(iommu);
    entry_base = iommu_qi_ctrl(iommu)->qinval_maddr +
                 ((index >> QINVAL_ENTRY_ORDER) << PAGE_SHIFT);
    qinval_entries = map_vtd_domain_page(entry_base);
    qinval_entry = &qinval_entries[index % (1 << QINVAL_ENTRY_ORDER)];

    qinval_entry->q.dev_iotlb_inv_dsc.lo.type = TYPE_INVAL_DEVICE_IOTLB;
    qinval_entry->q.dev_iotlb_inv_dsc.lo.res_1 = 0;
    qinval_entry->q.dev_iotlb_inv_dsc.lo.max_invs_pend = pdev->ats.queue_depth;
    qinval_entry->q.dev_iotlb_inv_dsc.lo.res_2 = 0;
    qinval_entry->q.dev_iotlb_inv_dsc.lo.sid = PCI_BDF2(pdev->bus, pdev->devfn);
    qinval_entry->q.dev_iotlb_inv_dsc.lo.res_3 = 0;

    qinval_entry->q.dev_iotlb_inv_dsc.hi.size = size;
    qinval_entry->q.dev_iotlb_inv_dsc.hi.res_1 = 0;
    qinval_entry->q.dev_iotlb_inv_dsc.hi.addr = addr >> PAGE_SHIFT_4K;

    unmap_vtd_domain_page(qinval_entries);
    qinval_update_qtail(iommu, index);
    spin_unlock_irqrestore(&iommu->register_lock, flags);

    return dev_invalidate_sync(iommu, pdev, did);
}

static int __must_check queue_invalidate_iec_sync(struct iommu *iommu,
                                                  u8 granu, u8 im, u16 iidx)
{
    unsigned long flags;
    unsigned int index;
    u64 entry_base;
    struct qinval_entry *qinval_entry, *qinval_entries;
    int ret;

    spin_lock_irqsave(&iommu->register_lock, flags);
    index = qinval_next_index(iommu);
    entry_base = iommu_qi_ctrl(iommu)->qinval_maddr +
                 ((index >> QINVAL_ENTRY_ORDER) << PAGE_SHIFT);
    qinval_entries = map_vtd_domain_page(entry_base);
    qinval_entry = &qinval_entries[index % (1 << QINVAL_ENTRY_ORDER)];

    qinval_entry->q.iec_inv_dsc.lo.type = TYPE_INVAL_IEC;
    qinval_entry->q.iec_inv_dsc.lo.granu = granu;
    qinval_entry->q.iec_inv_dsc.lo.res_1 = 0;
    qinval_entry->q.iec_inv_dsc.lo.im = im;
    qinval_entry->q.iec_inv_dsc.lo.iidx = iidx;
    qinval_entry->q.iec_inv_dsc.lo.res_2 = 0;
    qinval_entry->q.iec_inv_dsc.hi.res = 0;

    unmap_vtd_domain_page(qinval_entries);
    qinval_update_qtail(iommu, index);
    spin_unlock_irqrestore(&iommu->register_lock, flags);

    ret = invalidate_sync(iommu);

    /*
     * reading vt-d architecture register will ensure
     * draining happens in implementation independent way.
     */
    (void)dmar_readq(iommu->reg, DMAR_CAP_REG);

    return ret;
}

int iommu_flush_iec_global(struct iommu *iommu)
{
    return queue_invalidate_iec_sync(iommu, IEC_GLOBAL_INVL, 0, 0);
}

int iommu_flush_iec_index(struct iommu *iommu, u8 im, u16 iidx)
{
    return queue_invalidate_iec_sync(iommu, IEC_INDEX_INVL, im, iidx);
}

static int __must_check flush_context_qi(void *_iommu, u16 did,
                                         u16 sid, u8 fm, u64 type,
                                         bool_t flush_non_present_entry)
{
    struct iommu *iommu = (struct iommu *)_iommu;
    struct qi_ctrl *qi_ctrl = iommu_qi_ctrl(iommu);

    ASSERT(qi_ctrl->qinval_maddr);

    /*
     * In the non-present entry flush case, if hardware doesn't cache
     * non-present entry we do nothing and if hardware cache non-present
     * entry, we flush entries of domain 0 (the domain id is used to cache
     * any non-present entries)
     */
    if ( flush_non_present_entry )
    {
        if ( !cap_caching_mode(iommu->cap) )
            return 1;
        else
            did = 0;
    }

    return queue_invalidate_context_sync(iommu, did, sid, fm,
                                         type >> DMA_CCMD_INVL_GRANU_OFFSET);
}

static int __must_check flush_iotlb_qi(void *_iommu, u16 did, u64 addr,
                                       unsigned int size_order, u64 type,
                                       bool_t flush_non_present_entry,
                                       bool_t flush_dev_iotlb)
{
    u8 dr = 0, dw = 0;
    int ret = 0, rc;
    struct iommu *iommu = (struct iommu *)_iommu;
    struct qi_ctrl *qi_ctrl = iommu_qi_ctrl(iommu);

    ASSERT(qi_ctrl->qinval_maddr);

    /*
     * In the non-present entry flush case, if hardware doesn't cache
     * non-present entry we do nothing and if hardware cache non-present
     * entry, we flush entries of domain 0 (the domain id is used to cache
     * any non-present entries)
     */
    if ( flush_non_present_entry )
    {
        if ( !cap_caching_mode(iommu->cap) )
            return 1;
        else
            did = 0;
    }

    /* use queued invalidation */
    if (cap_write_drain(iommu->cap))
        dw = 1;
    if (cap_read_drain(iommu->cap))
        dr = 1;
    /* Need to conside the ih bit later */
    rc = queue_invalidate_iotlb_sync(iommu,
                                     type >> DMA_TLB_FLUSH_GRANU_OFFSET,
                                     dr, dw, did, size_order, 0, addr);
    if ( !ret )
        ret = rc;

    if ( flush_dev_iotlb )
    {
        rc = dev_invalidate_iotlb(iommu, did, addr, size_order, type);
        if ( !ret )
            ret = rc;
    }
    return ret;
}

int enable_qinval(struct iommu *iommu)
{
    struct acpi_drhd_unit *drhd;
    struct qi_ctrl *qi_ctrl;
    struct iommu_flush *flush;
    u32 sts;
    unsigned long flags;

    if ( !ecap_queued_inval(iommu->ecap) || !iommu_qinval )
        return -ENOENT;

    qi_ctrl = iommu_qi_ctrl(iommu);
    flush = iommu_get_flush(iommu);

    /* Return if already enabled by Xen */
    sts = dmar_readl(iommu->reg, DMAR_GSTS_REG);
    if ( (sts & DMA_GSTS_QIES) && qi_ctrl->qinval_maddr )
        return 0;

    if ( qi_ctrl->qinval_maddr == 0 )
    {
        drhd = iommu_to_drhd(iommu);
        qi_ctrl->qinval_maddr = alloc_pgtable_maddr(drhd, QINVAL_ARCH_PAGE_NR);
        if ( qi_ctrl->qinval_maddr == 0 )
        {
            dprintk(XENLOG_WARNING VTDPREFIX,
                    "Cannot allocate memory for qi_ctrl->qinval_maddr\n");
            return -ENOMEM;
        }
    }

    flush->context = flush_context_qi;
    flush->iotlb = flush_iotlb_qi;

    /* Setup Invalidation Queue Address(IQA) register with the
     * address of the page we just allocated.  QS field at
     * bits[2:0] to indicate size of queue is one 4KB page.
     * That's 256 entries.  Queued Head (IQH) and Queue Tail (IQT)
     * registers are automatically reset to 0 with write
     * to IQA register.
     */
    qi_ctrl->qinval_maddr |= QINVAL_PAGE_ORDER;

    spin_lock_irqsave(&iommu->register_lock, flags);
    dmar_writeq(iommu->reg, DMAR_IQA_REG, qi_ctrl->qinval_maddr);

    dmar_writeq(iommu->reg, DMAR_IQT_REG, 0);

    /* enable queued invalidation hardware */
    sts = dmar_readl(iommu->reg, DMAR_GSTS_REG);
    dmar_writel(iommu->reg, DMAR_GCMD_REG, sts | DMA_GCMD_QIE);

    /* Make sure hardware complete it */
    IOMMU_WAIT_OP(iommu, DMAR_GSTS_REG, dmar_readl,
                  (sts & DMA_GSTS_QIES), sts);
    spin_unlock_irqrestore(&iommu->register_lock, flags);

    return 0;
}

void disable_qinval(struct iommu *iommu)
{
    u32 sts;
    unsigned long flags;

    if ( !ecap_queued_inval(iommu->ecap) )
        return;

    spin_lock_irqsave(&iommu->register_lock, flags);
    sts = dmar_readl(iommu->reg, DMAR_GSTS_REG);
    if ( !(sts & DMA_GSTS_QIES) )
        goto out;

    dmar_writel(iommu->reg, DMAR_GCMD_REG, sts & (~DMA_GCMD_QIE));

    /* Make sure hardware complete it */
    IOMMU_WAIT_OP(iommu, DMAR_GSTS_REG, dmar_readl,
                  !(sts & DMA_GSTS_QIES), sts);
out:
    spin_unlock_irqrestore(&iommu->register_lock, flags);
}
