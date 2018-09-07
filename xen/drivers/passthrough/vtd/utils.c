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
 */

#include <xen/sched.h>
#include <xen/delay.h>
#include <xen/iommu.h>
#include <xen/time.h>
#include <xen/pci.h>
#include <xen/pci_regs.h>
#include "iommu.h"
#include "dmar.h"
#include "vtd.h"
#include "extern.h"
#include <asm/io_apic.h>

/* Disable vt-d protected memory registers. */
void disable_pmr(struct iommu *iommu)
{
    u32 val;
    unsigned long flags;

    val = dmar_readl(iommu->reg, DMAR_PMEN_REG);
    if ( !(val & DMA_PMEN_PRS) )
        return;

    spin_lock_irqsave(&iommu->register_lock, flags);
    dmar_writel(iommu->reg, DMAR_PMEN_REG, val & ~DMA_PMEN_EPM);

    IOMMU_WAIT_OP(iommu, DMAR_PMEN_REG, dmar_readl,
                  !(val & DMA_PMEN_PRS), val);
    spin_unlock_irqrestore(&iommu->register_lock, flags);

    dprintk(XENLOG_INFO VTDPREFIX,
            "Disabled protected memory registers\n");
}

void print_iommu_regs(struct acpi_drhd_unit *drhd)
{
    struct iommu *iommu = drhd->iommu;
    u64 cap;

    printk("---- print_iommu_regs ----\n");
    printk(" drhd->address = %"PRIx64"\n", drhd->address);
    printk(" VER = %x\n", dmar_readl(iommu->reg, DMAR_VER_REG));
    printk(" CAP = %"PRIx64"\n", cap = dmar_readq(iommu->reg, DMAR_CAP_REG));
    printk(" n_fault_reg = %"PRIx64"\n", cap_num_fault_regs(cap));
    printk(" fault_recording_offset = %"PRIx64"\n", cap_fault_reg_offset(cap));
    if ( cap_fault_reg_offset(cap) < PAGE_SIZE )
    {
        printk(" fault_recording_reg_l = %"PRIx64"\n",
               dmar_readq(iommu->reg, cap_fault_reg_offset(cap)));
        printk(" fault_recording_reg_h = %"PRIx64"\n",
               dmar_readq(iommu->reg, cap_fault_reg_offset(cap) + 8));
    }
    printk(" ECAP = %"PRIx64"\n", dmar_readq(iommu->reg, DMAR_ECAP_REG));
    printk(" GCMD = %x\n", dmar_readl(iommu->reg, DMAR_GCMD_REG));
    printk(" GSTS = %x\n", dmar_readl(iommu->reg, DMAR_GSTS_REG));
    printk(" RTADDR = %"PRIx64"\n", dmar_readq(iommu->reg,DMAR_RTADDR_REG));
    printk(" CCMD = %"PRIx64"\n", dmar_readq(iommu->reg, DMAR_CCMD_REG));
    printk(" FSTS = %x\n", dmar_readl(iommu->reg, DMAR_FSTS_REG));
    printk(" FECTL = %x\n", dmar_readl(iommu->reg, DMAR_FECTL_REG));
    printk(" FEDATA = %x\n", dmar_readl(iommu->reg, DMAR_FEDATA_REG));
    printk(" FEADDR = %x\n", dmar_readl(iommu->reg, DMAR_FEADDR_REG));
    printk(" FEUADDR = %x\n", dmar_readl(iommu->reg, DMAR_FEUADDR_REG));
}

static u32 get_level_index(unsigned long gmfn, int level)
{
    while ( --level )
        gmfn = gmfn >> LEVEL_STRIDE;

    return gmfn & LEVEL_MASK;
}

void print_vtd_entries(struct iommu *iommu, int bus, int devfn, u64 gmfn)
{
    struct context_entry *ctxt_entry;
    struct root_entry *root_entry;
    struct dma_pte pte;
    u64 *l, val;
    u32 l_index, level;

    printk("print_vtd_entries: iommu #%u dev %04x:%02x:%02x.%u gmfn %"PRI_gfn"\n",
           iommu->index, iommu->intel->drhd->segment, bus,
           PCI_SLOT(devfn), PCI_FUNC(devfn), gmfn);

    if ( iommu->root_maddr == 0 )
    {
        printk("    iommu->root_maddr = 0\n");
        return;
    }

    root_entry = (struct root_entry *)map_vtd_domain_page(iommu->root_maddr);
    if ( root_entry == NULL )
    {
        printk("    root_entry == NULL\n");
        return;
    }

    printk("    root_entry[%02x] = %"PRIx64"\n", bus, root_entry[bus].val);
    if ( !root_present(root_entry[bus]) )
    {
        unmap_vtd_domain_page(root_entry);
        printk("    root_entry[%02x] not present\n", bus);
        return;
    }

    val = root_entry[bus].val;
    unmap_vtd_domain_page(root_entry);
    ctxt_entry = map_vtd_domain_page(val);
    if ( ctxt_entry == NULL )
    {
        printk("    ctxt_entry == NULL\n");
        return;
    }

    val = ctxt_entry[devfn].lo;
    printk("    context[%02x] = %"PRIx64"_%"PRIx64"\n",
           devfn, ctxt_entry[devfn].hi, val);
    if ( !context_present(ctxt_entry[devfn]) )
    {
        unmap_vtd_domain_page(ctxt_entry);
        printk("    ctxt_entry[%02x] not present\n", devfn);
        return;
    }

    level = agaw_to_level(context_address_width(ctxt_entry[devfn]));
    unmap_vtd_domain_page(ctxt_entry);
    if ( level != VTD_PAGE_TABLE_LEVEL_3 &&
         level != VTD_PAGE_TABLE_LEVEL_4)
    {
        printk("Unsupported VTD page table level (%d)!\n", level);
        return;
    }

    do
    {
        l = map_vtd_domain_page(val);
        if ( l == NULL )
        {
            printk("    l%u == NULL\n", level);
            break;
        }
        l_index = get_level_index(gmfn, level);
        pte.val = l[l_index];
        unmap_vtd_domain_page(l);
        printk("    l%u[%03x] = %"PRIx64"\n", level, l_index, pte.val);

        if ( !dma_pte_present(pte) )
        {
            printk("    l%u[%03x] not present\n", level, l_index);
            break;
        }
        if ( dma_pte_superpage(pte) )
            break;
        val = dma_pte_addr(pte);
    } while ( --level );
}

void vtd_dump_iommu_info(unsigned char key)
{
    struct acpi_drhd_unit *drhd;
    struct iommu *iommu;
    int i;

    for_each_drhd_unit ( drhd )
    {
        u32 status = 0;

        iommu = drhd->iommu;
        printk("\niommu %x: nr_pt_levels = %x.\n", iommu->index,
            iommu->nr_pt_levels);

        if ( ecap_queued_inval(iommu->ecap) ||  ecap_intr_remap(iommu->ecap) )
            status = dmar_readl(iommu->reg, DMAR_GSTS_REG);

        printk("  Queued Invalidation: %ssupported%s.\n",
            ecap_queued_inval(iommu->ecap) ? "" : "not ",
           (status & DMA_GSTS_QIES) ? " and enabled" : "" );


        printk("  Interrupt Remapping: %ssupported%s.\n",
            ecap_intr_remap(iommu->ecap) ? "" : "not ",
            (status & DMA_GSTS_IRES) ? " and enabled" : "" );

        printk("  Interrupt Posting: %ssupported.\n",
               cap_intr_post(iommu->cap) ? "" : "not ");

        if ( status & DMA_GSTS_IRES )
        {
            /* Dump interrupt remapping table. */
            u64 iremap_maddr = dmar_readq(iommu->reg, DMAR_IRTA_REG);
            int nr_entry = 1 << ((iremap_maddr & 0xF) + 1);
            struct iremap_entry *iremap_entries = NULL;
            int print_cnt = 0;

            printk("  Interrupt remapping table (nr_entry=%#x. "
                "Only dump P=1 entries here):\n", nr_entry);
            printk("R means remapped format, P means posted format.\n");
            printk("R:       SVT  SQ   SID  V  AVL FPD      DST DLM TM RH DM P\n");
            printk("P:       SVT  SQ   SID  V  AVL FPD              PDA  URG P\n");
            for ( i = 0; i < nr_entry; i++ )
            {
                struct iremap_entry *p;
                if ( i % (1 << IREMAP_ENTRY_ORDER) == 0 )
                {
                    /* This entry across page boundry */
                    if ( iremap_entries )
                        unmap_vtd_domain_page(iremap_entries);

                    GET_IREMAP_ENTRY(iremap_maddr, i,
                                     iremap_entries, p);
                }
                else
                    p = &iremap_entries[i % (1 << IREMAP_ENTRY_ORDER)];

                if ( !p->remap.p )
                    continue;
                if ( !p->remap.im )
                    printk("R:  %04x:  %x   %x  %04x %02x    %x   %x %08x   %x  %x  %x  %x %x\n",
                           i,
                           p->remap.svt, p->remap.sq, p->remap.sid,
                           p->remap.vector, p->remap.avail, p->remap.fpd,
                           p->remap.dst, p->remap.dlm, p->remap.tm, p->remap.rh,
                           p->remap.dm, p->remap.p);
                else
                    printk("P:  %04x:  %x   %x  %04x %02x    %x   %x %16lx    %x %x\n",
                           i,
                           p->post.svt, p->post.sq, p->post.sid, p->post.vector,
                           p->post.avail, p->post.fpd,
                           ((u64)p->post.pda_h << 32) | (p->post.pda_l << 6),
                           p->post.urg, p->post.p);

                print_cnt++;
            }
            if ( iremap_entries )
                unmap_vtd_domain_page(iremap_entries);
            if ( iommu_ir_ctrl(iommu)->iremap_num != print_cnt )
                printk("Warning: Print %d IRTE (actually have %d)!\n",
                        print_cnt, iommu_ir_ctrl(iommu)->iremap_num);

        }
    }

    /* Dump the I/O xAPIC redirection table(s). */
    if ( iommu_enabled )
    {
        int apic;
        union IO_APIC_reg_01 reg_01;
        struct IO_APIC_route_remap_entry *remap;
        struct ir_ctrl *ir_ctrl;

        for ( apic = 0; apic < nr_ioapics; apic++ )
        {
            iommu = ioapic_to_iommu(mp_ioapics[apic].mpc_apicid);
            ir_ctrl = iommu_ir_ctrl(iommu);
            if ( !ir_ctrl || !ir_ctrl->iremap_maddr || !ir_ctrl->iremap_num )
                continue;

            printk( "\nRedirection table of IOAPIC %x:\n", apic);

            /* IO xAPIC Version Register. */
            reg_01.raw = __io_apic_read(apic, 1);

            printk("  #entry IDX FMT MASK TRIG IRR POL STAT DELI  VECTOR\n");
            for ( i = 0; i <= reg_01.bits.entries; i++ )
            {
                struct IO_APIC_route_entry rte =
                    __ioapic_read_entry(apic, i, TRUE);

                remap = (struct IO_APIC_route_remap_entry *) &rte;
                if ( !remap->format )
                    continue;

                printk("   %02x:  %04x   %x    %x   %x   %x   %x    %x"
                    "    %x     %02x\n", i,
                    remap->index_0_14 | (remap->index_15 << 15),
                    remap->format, remap->mask, remap->trigger, remap->irr,
                    remap->polarity, remap->delivery_status, remap->delivery_mode,
                    remap->vector);
            }
        }
    }
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
