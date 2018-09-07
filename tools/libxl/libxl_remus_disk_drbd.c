/*
 * Copyright (C) 2014 FUJITSU LIMITED
 * Author Lai Jiangshan <laijs@cn.fujitsu.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; version 2.1 only. with the special
 * exception on linking described in file LICENSE.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 */

#include "libxl_osdeps.h" /* must come before any other headers */

#include "libxl_internal.h"

/*** drbd implementation ***/
const int DRBD_SEND_CHECKPOINT = 20;
const int DRBD_WAIT_CHECKPOINT_ACK = 30;

typedef struct libxl__remus_drbd_disk {
    int ctl_fd;
    int ackwait;
} libxl__remus_drbd_disk;

int init_subkind_drbd_disk(libxl__checkpoint_devices_state *cds)
{
    libxl__remus_state *rs = cds->concrete_data;
    STATE_AO_GC(cds->ao);

    rs->drbd_probe_script = GCSPRINTF("%s/block-drbd-probe",
                                      libxl__xen_script_dir_path());

    return 0;
}

void cleanup_subkind_drbd_disk(libxl__checkpoint_devices_state *cds)
{
    return;
}

/*----- match(), setup() and teardown() -----*/

/* callbacks */
static void match_async_exec_cb(libxl__egc *egc,
                                libxl__async_exec_state *aes,
                                int rc, int status);

/* implementations */

static void match_async_exec(libxl__egc *egc, libxl__checkpoint_device *dev);

static void drbd_setup(libxl__egc *egc, libxl__checkpoint_device *dev)
{
    STATE_AO_GC(dev->cds->ao);

    match_async_exec(egc, dev);
}

static void match_async_exec(libxl__egc *egc, libxl__checkpoint_device *dev)
{
    int arraysize, nr = 0, rc;
    const libxl_device_disk *disk = dev->backend_dev;
    libxl__async_exec_state *aes = &dev->aodev.aes;
    libxl__remus_state *rs = dev->cds->concrete_data;
    STATE_AO_GC(dev->cds->ao);

    /* setup env & args */
    arraysize = 1;
    GCNEW_ARRAY(aes->env, arraysize);
    aes->env[nr++] = NULL;
    assert(nr <= arraysize);

    arraysize = 3;
    nr = 0;
    GCNEW_ARRAY(aes->args, arraysize);
    aes->args[nr++] = rs->drbd_probe_script;
    aes->args[nr++] = disk->pdev_path;
    aes->args[nr++] = NULL;
    assert(nr <= arraysize);

    aes->ao = dev->cds->ao;
    aes->what = GCSPRINTF("%s %s", aes->args[0], aes->args[1]);
    aes->timeout_ms = LIBXL_HOTPLUG_TIMEOUT * 1000;
    aes->callback = match_async_exec_cb;
    aes->stdfds[0] = -1;
    aes->stdfds[1] = -1;
    aes->stdfds[2] = -1;

    rc = libxl__async_exec_start(aes);
    if (rc)
        goto out;

    return;

out:
    dev->aodev.rc = rc;
    dev->aodev.callback(egc, &dev->aodev);
}

static void match_async_exec_cb(libxl__egc *egc,
                                libxl__async_exec_state *aes,
                                int rc, int status)
{
    libxl__ao_device *aodev = CONTAINER_OF(aes, *aodev, aes);
    libxl__checkpoint_device *dev = CONTAINER_OF(aodev, *dev, aodev);
    libxl__remus_drbd_disk *drbd_disk;
    const libxl_device_disk *disk = dev->backend_dev;

    STATE_AO_GC(aodev->ao);

    if (rc)
        goto out;

    if (status) {
        rc = ERROR_CHECKPOINT_DEVOPS_DOES_NOT_MATCH;
        /* BUG: seems to assume that any exit status means `no match' */
        /* BUG: exit status will have been logged as an error */
        goto out;
    }

    /* ops matched */
    dev->matched = true;

    GCNEW(drbd_disk);
    dev->concrete_data = drbd_disk;
    drbd_disk->ackwait = 0;
    drbd_disk->ctl_fd = open(disk->pdev_path, O_RDONLY);
    if (drbd_disk->ctl_fd < 0) {
        rc = ERROR_FAIL;
        goto out;
    }

    rc = 0;

out:
    aodev->rc = rc;
    aodev->callback(egc, aodev);
}

static void drbd_teardown(libxl__egc *egc, libxl__checkpoint_device *dev)
{
    libxl__remus_drbd_disk *drbd_disk = dev->concrete_data;
    STATE_AO_GC(dev->cds->ao);

    close(drbd_disk->ctl_fd);
    dev->aodev.rc = 0;
    dev->aodev.callback(egc, &dev->aodev);
}

/*----- checkpointing APIs -----*/

/* callbacks */
static void checkpoint_async_call_done(libxl__egc *egc,
                                       libxl__ev_child *child,
                                       pid_t pid, int status);

/* API implementations */

/* this op will not wait and block, so implement as sync op */
static void drbd_postsuspend(libxl__egc *egc, libxl__checkpoint_device *dev)
{
    STATE_AO_GC(dev->cds->ao);

    libxl__remus_drbd_disk *rdd = dev->concrete_data;

    if (!rdd->ackwait) {
        if (ioctl(rdd->ctl_fd, DRBD_SEND_CHECKPOINT, 0) <= 0)
            rdd->ackwait = 1;
    }

    dev->aodev.rc = 0;
    dev->aodev.callback(egc, &dev->aodev);
}


static void drbd_preresume_async(libxl__checkpoint_device *dev);

static void drbd_preresume(libxl__egc *egc, libxl__checkpoint_device *dev)
{
    ASYNC_CALL(egc, dev->cds->ao, &dev->aodev.child, dev,
               drbd_preresume_async,
               checkpoint_async_call_done);
}

static void drbd_preresume_async(libxl__checkpoint_device *dev)
{
    libxl__remus_drbd_disk *rdd = dev->concrete_data;
    int ackwait = rdd->ackwait;

    if (ackwait) {
        ioctl(rdd->ctl_fd, DRBD_WAIT_CHECKPOINT_ACK, 0);
        ackwait = 0;
    }

    _exit(ackwait);
}

static void checkpoint_async_call_done(libxl__egc *egc,
                                       libxl__ev_child *child,
                                       pid_t pid, int status)
{
    int rc;
    libxl__ao_device *aodev = CONTAINER_OF(child, *aodev, child);
    libxl__checkpoint_device *dev = CONTAINER_OF(aodev, *dev, aodev);
    libxl__remus_drbd_disk *rdd = dev->concrete_data;

    STATE_AO_GC(aodev->ao);

    if (!WIFEXITED(status)) {
        rc = ERROR_FAIL;
        goto out;
    }

    rdd->ackwait = WEXITSTATUS(status);
    rc = 0;

out:
    aodev->rc = rc;
    aodev->callback(egc, aodev);
}

const libxl__checkpoint_device_instance_ops remus_device_drbd_disk = {
    .kind = LIBXL__DEVICE_KIND_VBD,
    .setup = drbd_setup,
    .teardown = drbd_teardown,
    .postsuspend = drbd_postsuspend,
    .preresume = drbd_preresume,
};
