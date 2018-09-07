/******************************************************************************
 * xenguest.h
 *
 * A library for guest domain management in Xen.
 *
 * Copyright (c) 2003-2004, K A Fraser.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef XENGUEST_H
#define XENGUEST_H

#define XC_NUMA_NO_NODE   (~0U)

#define XCFLAGS_LIVE      (1 << 0)
#define XCFLAGS_DEBUG     (1 << 1)
#define XCFLAGS_HVM       (1 << 2)
#define XCFLAGS_STDVGA    (1 << 3)
#define XCFLAGS_CHECKPOINT_COMPRESS    (1 << 4)

#define X86_64_B_SIZE   64 
#define X86_32_B_SIZE   32

/*
 * User not using xc_suspend_* / xc_await_suspent may not want to
 * include the full libxenevtchn API here.
 */
struct xenevtchn_handle;

/* callbacks provided by xc_domain_save */
struct save_callbacks {
    /* Called after expiration of checkpoint interval,
     * to suspend the guest.
     */
    int (*suspend)(void* data);

    /* Called after the guest's dirty pages have been
     *  copied into an output buffer.
     * Callback function resumes the guest & the device model,
     *  returns to xc_domain_save.
     * xc_domain_save then flushes the output buffer, while the
     *  guest continues to run.
     */
    int (*postcopy)(void* data);

    /* Called after the memory checkpoint has been flushed
     * out into the network. Typical actions performed in this
     * callback include:
     *   (a) send the saved device model state (for HVM guests),
     *   (b) wait for checkpoint ack
     *   (c) release the network output buffer pertaining to the acked checkpoint.
     *   (c) sleep for the checkpoint interval.
     *
     * returns:
     * 0: terminate checkpointing gracefully
     * 1: take another checkpoint */
    int (*checkpoint)(void* data);

    /*
     * Called after the checkpoint callback.
     *
     * returns:
     * 0: terminate checkpointing gracefully
     * 1: take another checkpoint
     */
    int (*wait_checkpoint)(void* data);

    /* Enable qemu-dm logging dirty pages to xen */
    int (*switch_qemu_logdirty)(int domid, unsigned enable, void *data); /* HVM only */

    /* to be provided as the last argument to each callback function */
    void* data;
};

typedef enum {
    XC_MIG_STREAM_NONE, /* plain stream */
    XC_MIG_STREAM_REMUS,
    XC_MIG_STREAM_COLO,
} xc_migration_stream_t;

/**
 * This function will save a running domain.
 *
 * @parm xch a handle to an open hypervisor interface
 * @parm fd the file descriptor to save a domain to
 * @parm dom the id of the domain
 * @param stream_type XC_MIG_STREAM_NONE if the far end of the stream
 *        doesn't use checkpointing
 * @return 0 on success, -1 on failure
 */
int xc_domain_save(xc_interface *xch, int io_fd, uint32_t dom, uint32_t max_iters,
                   uint32_t max_factor, uint32_t flags /* XCFLAGS_xxx */,
                   struct save_callbacks* callbacks, int hvm,
                   xc_migration_stream_t stream_type, int recv_fd);

/* callbacks provided by xc_domain_restore */
struct restore_callbacks {
    /* Called after a new checkpoint to suspend the guest.
     */
    int (*suspend)(void* data);

    /* Called after the secondary vm is ready to resume.
     * Callback function resumes the guest & the device model,
     * returns to xc_domain_restore.
     */
    int (*postcopy)(void* data);

    /* A checkpoint record has been found in the stream.
     * returns: */
#define XGR_CHECKPOINT_ERROR    0 /* Terminate processing */
#define XGR_CHECKPOINT_SUCCESS  1 /* Continue reading more data from the stream */
#define XGR_CHECKPOINT_FAILOVER 2 /* Failover and resume VM */
    int (*checkpoint)(void* data);

    /*
     * Called after the checkpoint callback.
     *
     * returns:
     * 0: terminate checkpointing gracefully
     * 1: take another checkpoint
     */
    int (*wait_checkpoint)(void* data);

    /*
     * callback to send store gfn and console gfn to xl
     * if we want to resume vm before xc_domain_save()
     * exits.
     */
    void (*restore_results)(xen_pfn_t store_gfn, xen_pfn_t console_gfn,
                            void *data);

    /* to be provided as the last argument to each callback function */
    void* data;
};

/**
 * This function will restore a saved domain.
 *
 * Domain is restored in a suspended state ready to be unpaused.
 *
 * @parm xch a handle to an open hypervisor interface
 * @parm fd the file descriptor to restore a domain from
 * @parm dom the id of the domain
 * @parm store_evtchn the store event channel for this domain to use
 * @parm store_mfn returned with the mfn of the store page
 * @parm hvm non-zero if this is a HVM restore
 * @parm pae non-zero if this HVM domain has PAE support enabled
 * @parm superpages non-zero to allocate guest memory with superpages
 * @parm stream_type non-zero if the far end of the stream is using checkpointing
 * @parm callbacks non-NULL to receive a callback to restore toolstack
 *       specific data
 * @return 0 on success, -1 on failure
 */
int xc_domain_restore(xc_interface *xch, int io_fd, uint32_t dom,
                      unsigned int store_evtchn, unsigned long *store_mfn,
                      domid_t store_domid, unsigned int console_evtchn,
                      unsigned long *console_mfn, domid_t console_domid,
                      unsigned int hvm, unsigned int pae, int superpages,
                      xc_migration_stream_t stream_type,
                      struct restore_callbacks *callbacks, int send_back_fd);

/**
 * This function will create a domain for a paravirtualized Linux
 * using file names pointing to kernel and ramdisk
 *
 * @parm xch a handle to an open hypervisor interface
 * @parm domid the id of the domain
 * @parm mem_mb memory size in megabytes
 * @parm image_name name of the kernel image file
 * @parm ramdisk_name name of the ramdisk image file
 * @parm cmdline command line string
 * @parm flags domain creation flags
 * @parm store_evtchn the store event channel for this domain to use
 * @parm store_mfn returned with the mfn of the store page
 * @parm console_evtchn the console event channel for this domain to use
 * @parm conole_mfn returned with the mfn of the console page
 * @return 0 on success, -1 on failure
 */
int xc_linux_build(xc_interface *xch,
                   uint32_t domid,
                   unsigned int mem_mb,
                   const char *image_name,
                   const char *ramdisk_name,
                   const char *cmdline,
                   const char *features,
                   unsigned long flags,
                   unsigned int store_evtchn,
                   unsigned long *store_mfn,
                   unsigned int console_evtchn,
                   unsigned long *console_mfn);

struct xc_hvm_firmware_module {
    uint8_t  *data;
    uint32_t  length;
    uint64_t  guest_addr_out;
};

/*
 * Sets *lockfd to -1.
 * Has deallocated everything even on error.
 */
int xc_suspend_evtchn_release(xc_interface *xch,
                              struct xenevtchn_handle *xce,
                              int domid, int suspend_evtchn, int *lockfd);

/**
 * This function eats the initial notification.
 * xce must not be used for anything else
 * See xc_suspend_evtchn_init_sane re lockfd.
 */
int xc_suspend_evtchn_init_exclusive(xc_interface *xch,
                                     struct xenevtchn_handle *xce,
                                     int domid, int port, int *lockfd);

/* xce must not be used for anything else */
int xc_await_suspend(xc_interface *xch, struct xenevtchn_handle *xce,
                     int suspend_evtchn);

/**
 * The port will be signaled immediately after this call
 * The caller should check the domain status and look for the next event
 * On success, *lockfd will be set to >=0 and *lockfd must be preserved
 * and fed to xc_suspend_evtchn_release.  (On error *lockfd is
 * undefined and xc_suspend_evtchn_release is not allowed.)
 */
int xc_suspend_evtchn_init_sane(xc_interface *xch,
                                struct xenevtchn_handle *xce,
                                int domid, int port, int *lockfd);

int xc_mark_page_online(xc_interface *xch, unsigned long start,
                        unsigned long end, uint32_t *status);

int xc_mark_page_offline(xc_interface *xch, unsigned long start,
                          unsigned long end, uint32_t *status);

int xc_query_page_offline_status(xc_interface *xch, unsigned long start,
                                 unsigned long end, uint32_t *status);

int xc_exchange_page(xc_interface *xch, int domid, xen_pfn_t mfn);


/**
 * Memory related information, such as PFN types, the P2M table,
 * the guest word width and the guest page table levels.
 */
struct xc_domain_meminfo {
    unsigned int pt_levels;
    unsigned int guest_width;
    xen_pfn_t *pfn_type;
    xen_pfn_t *p2m_table;
    unsigned long p2m_size;
};

int xc_map_domain_meminfo(xc_interface *xch, int domid,
                          struct xc_domain_meminfo *minfo);

int xc_unmap_domain_meminfo(xc_interface *xch, struct xc_domain_meminfo *mem);

/**
 * This function map m2p table
 * @parm xch a handle to an open hypervisor interface
 * @parm max_mfn the max pfn
 * @parm prot the flags to map, such as read/write etc
 * @parm mfn0 return the first mfn, can be NULL
 * @return mapped m2p table on success, NULL on failure
 */
xen_pfn_t *xc_map_m2p(xc_interface *xch,
                      unsigned long max_mfn,
                      int prot,
                      unsigned long *mfn0);
#endif /* XENGUEST_H */
