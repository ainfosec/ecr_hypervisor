/*
 * Copyright 2009-2017 Citrix Ltd and other contributors
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

#include <ctype.h>
#include <inttypes.h>
#include <stdlib.h>
#include <unistd.h>

#include <libxl.h>
#include <libxl_utils.h>
#include <libxlutil.h>

#include "xl.h"
#include "xl_utils.h"
#include "xl_parse.h"

#ifdef LIBXL_HAVE_PSR_CMT
static int psr_cmt_hwinfo(void)
{
    int rc;
    int enabled;
    uint32_t total_rmid;

    printf("Cache Monitoring Technology (CMT):\n");

    enabled = libxl_psr_cmt_enabled(ctx);
    printf("%-16s: %s\n", "Enabled", enabled ? "1" : "0");
    if (!enabled)
        return 0;

    rc = libxl_psr_cmt_get_total_rmid(ctx, &total_rmid);
    if (rc) {
        fprintf(stderr, "Failed to get max RMID value\n");
        return rc;
    }
    printf("%-16s: %u\n", "Total RMID", total_rmid);

    printf("Supported monitor types:\n");
    if (libxl_psr_cmt_type_supported(ctx, LIBXL_PSR_CMT_TYPE_CACHE_OCCUPANCY))
        printf("cache-occupancy\n");
    if (libxl_psr_cmt_type_supported(ctx, LIBXL_PSR_CMT_TYPE_TOTAL_MEM_COUNT))
        printf("total-mem-bandwidth\n");
    if (libxl_psr_cmt_type_supported(ctx, LIBXL_PSR_CMT_TYPE_LOCAL_MEM_COUNT))
        printf("local-mem-bandwidth\n");

    return rc;
}

#define MBM_SAMPLE_RETRY_MAX 4
static int psr_cmt_get_mem_bandwidth(uint32_t domid,
                                     libxl_psr_cmt_type type,
                                     uint32_t socketid,
                                     uint64_t *bandwidth_r)
{
    uint64_t sample1, sample2;
    uint64_t tsc1, tsc2;
    int retry_attempts = 0;
    int rc;

    while (1) {
        rc = libxl_psr_cmt_get_sample(ctx, domid, type, socketid,
                                      &sample1, &tsc1);
        if (rc < 0)
            return rc;

        usleep(10000);

        rc = libxl_psr_cmt_get_sample(ctx, domid, type, socketid,
                                      &sample2, &tsc2);
        if (rc < 0)
            return rc;

        if (tsc2 <= tsc1)
            return -1;

        /*
         * Hardware guarantees at most 1 overflow can happen if the duration
         * between two samples is less than 1 second. Note that tsc returned
         * from hypervisor is already-scaled time(ns).
         */
        if (tsc2 - tsc1 < 1000000000 && sample2 >= sample1)
            break;

        if (retry_attempts < MBM_SAMPLE_RETRY_MAX) {
            retry_attempts++;
        } else {
            fprintf(stderr, "event counter overflowed\n");
            return -1;
        }
    }

    *bandwidth_r = (sample2 - sample1) * 1000000000 / (tsc2 - tsc1) / 1024;
    return 0;
}

static void psr_cmt_print_domain_info(libxl_dominfo *dominfo,
                                      libxl_psr_cmt_type type,
                                      libxl_bitmap *socketmap)
{
    char *domain_name;
    uint32_t socketid;
    uint64_t monitor_data;

    if (!libxl_psr_cmt_domain_attached(ctx, dominfo->domid))
        return;

    domain_name = libxl_domid_to_name(ctx, dominfo->domid);
    printf("%-40s %5d", domain_name, dominfo->domid);
    free(domain_name);

    libxl_for_each_set_bit(socketid, *socketmap) {
        switch (type) {
        case LIBXL_PSR_CMT_TYPE_CACHE_OCCUPANCY:
            if (!libxl_psr_cmt_get_sample(ctx, dominfo->domid, type, socketid,
                                          &monitor_data, NULL))
                printf("%13"PRIu64" KB", monitor_data / 1024);
            break;
        case LIBXL_PSR_CMT_TYPE_TOTAL_MEM_COUNT:
        case LIBXL_PSR_CMT_TYPE_LOCAL_MEM_COUNT:
            if (!psr_cmt_get_mem_bandwidth(dominfo->domid, type, socketid,
                                           &monitor_data))
                printf("%11"PRIu64" KB/s", monitor_data);
            break;
        default:
            return;
        }
    }

    printf("\n");
}

static int psr_cmt_show(libxl_psr_cmt_type type, uint32_t domid)
{
    uint32_t i, socketid, total_rmid;
    uint32_t l3_cache_size;
    libxl_bitmap socketmap;
    int rc, nr_domains;

    if (!libxl_psr_cmt_enabled(ctx)) {
        fprintf(stderr, "CMT is disabled in the system\n");
        return -1;
    }

    if (!libxl_psr_cmt_type_supported(ctx, type)) {
        fprintf(stderr, "Monitor type '%s' is not supported in the system\n",
                libxl_psr_cmt_type_to_string(type));
        return -1;
    }

    libxl_bitmap_init(&socketmap);
    libxl_socket_bitmap_alloc(ctx, &socketmap, 0);
    rc = libxl_get_online_socketmap(ctx, &socketmap);
    if (rc < 0) {
        fprintf(stderr, "Failed getting available sockets, rc: %d\n", rc);
        goto out;
    }

    rc = libxl_psr_cmt_get_total_rmid(ctx, &total_rmid);
    if (rc < 0) {
        fprintf(stderr, "Failed to get max RMID value\n");
        goto out;
    }

    printf("Total RMID: %d\n", total_rmid);

    /* Header */
    printf("%-40s %5s", "Name", "ID");
    libxl_for_each_set_bit(socketid, socketmap)
        printf("%14s %d", "Socket", socketid);
    printf("\n");

    if (type == LIBXL_PSR_CMT_TYPE_CACHE_OCCUPANCY) {
            /* Total L3 cache size */
            printf("%-46s", "Total L3 Cache Size");
            libxl_for_each_set_bit(socketid, socketmap) {
                rc = libxl_psr_cmt_get_l3_cache_size(ctx, socketid,
                                                     &l3_cache_size);
                if (rc < 0) {
                    fprintf(stderr,
                            "Failed to get system l3 cache size for socket:%d\n",
                            socketid);
                    goto out;
                }
                printf("%13u KB", l3_cache_size);
            }
            printf("\n");
    }

    /* Each domain */
    if (domid != INVALID_DOMID) {
        libxl_dominfo dominfo;

        libxl_dominfo_init(&dominfo);
        if (libxl_domain_info(ctx, &dominfo, domid)) {
            fprintf(stderr, "Failed to get domain info for %d\n", domid);
            rc = -1;
            goto out;
        }
        psr_cmt_print_domain_info(&dominfo, type, &socketmap);
        libxl_dominfo_dispose(&dominfo);
    }
    else
    {
        libxl_dominfo *list;
        if (!(list = libxl_list_domain(ctx, &nr_domains))) {
            fprintf(stderr, "Failed to get domain info for domain list.\n");
            rc = -1;
            goto out;
        }
        for (i = 0; i < nr_domains; i++)
            psr_cmt_print_domain_info(list + i, type, &socketmap);
        libxl_dominfo_list_free(list, nr_domains);
    }

out:
    libxl_bitmap_dispose(&socketmap);
    return rc;
}

int main_psr_cmt_attach(int argc, char **argv)
{
    uint32_t domid;
    int opt, ret = 0;

    SWITCH_FOREACH_OPT(opt, "", NULL, "psr-cmt-attach", 1) {
        /* No options */
    }

    domid = find_domain(argv[optind]);
    ret = libxl_psr_cmt_attach(ctx, domid);

    return ret;
}

int main_psr_cmt_detach(int argc, char **argv)
{
    uint32_t domid;
    int opt, ret = 0;

    SWITCH_FOREACH_OPT(opt, "", NULL, "psr-cmt-detach", 1) {
        /* No options */
    }

    domid = find_domain(argv[optind]);
    ret = libxl_psr_cmt_detach(ctx, domid);

    return ret;
}

int main_psr_cmt_show(int argc, char **argv)
{
    int opt, ret = 0;
    uint32_t domid;
    libxl_psr_cmt_type type;

    SWITCH_FOREACH_OPT(opt, "", NULL, "psr-cmt-show", 1) {
        /* No options */
    }

    if (!strcmp(argv[optind], "cache-occupancy"))
        type = LIBXL_PSR_CMT_TYPE_CACHE_OCCUPANCY;
    else if (!strcmp(argv[optind], "total-mem-bandwidth"))
        type = LIBXL_PSR_CMT_TYPE_TOTAL_MEM_COUNT;
    else if (!strcmp(argv[optind], "local-mem-bandwidth"))
        type = LIBXL_PSR_CMT_TYPE_LOCAL_MEM_COUNT;
    else {
        help("psr-cmt-show");
        return 2;
    }

    if (optind + 1 >= argc)
        domid = INVALID_DOMID;
    else if (optind + 1 == argc - 1)
        domid = find_domain(argv[optind + 1]);
    else {
        help("psr-cmt-show");
        return 2;
    }

    ret = psr_cmt_show(type, domid);

    return ret;
}
#endif

#ifdef LIBXL_HAVE_PSR_CAT
static int psr_cat_hwinfo(void)
{
    int rc;
    int i, nr;
    uint32_t l3_cache_size;
    libxl_psr_cat_info *info;

    printf("Cache Allocation Technology (CAT):\n");

    rc = libxl_psr_cat_get_l3_info(ctx, &info, &nr);
    if (rc) {
        fprintf(stderr, "Failed to get cat info\n");
        return rc;
    }

    for (i = 0; i < nr; i++) {
        rc = libxl_psr_cmt_get_l3_cache_size(ctx, info[i].id, &l3_cache_size);
        if (rc) {
            fprintf(stderr, "Failed to get l3 cache size for socket:%d\n",
                    info[i].id);
            goto out;
        }
        printf("%-16s: %u\n", "Socket ID", info[i].id);
        printf("%-16s: %uKB\n", "L3 Cache", l3_cache_size);
        printf("%-16s: %s\n", "CDP Status",
               info[i].cdp_enabled ? "Enabled" : "Disabled");
        printf("%-16s: %u\n", "Maximum COS", info[i].cos_max);
        printf("%-16s: %u\n", "CBM length", info[i].cbm_len);
        printf("%-16s: %#llx\n", "Default CBM",
               (1ull << info[i].cbm_len) - 1);
    }

out:
    libxl_psr_cat_info_list_free(info, nr);
    return rc;
}

static void psr_cat_print_one_domain_cbm_type(uint32_t domid, uint32_t socketid,
                                              libxl_psr_cbm_type type)
{
    uint64_t cbm;

    if (!libxl_psr_cat_get_cbm(ctx, domid, type, socketid, &cbm))
        printf("%#16"PRIx64, cbm);
    else
        printf("%16s", "error");
}

static void psr_cat_print_one_domain_cbm(uint32_t domid, uint32_t socketid,
                                         bool cdp_enabled)
{
    char *domain_name;

    domain_name = libxl_domid_to_name(ctx, domid);
    printf("%5d%25s", domid, domain_name);
    free(domain_name);

    if (!cdp_enabled) {
        psr_cat_print_one_domain_cbm_type(domid, socketid,
                                          LIBXL_PSR_CBM_TYPE_L3_CBM);
    } else {
        psr_cat_print_one_domain_cbm_type(domid, socketid,
                                          LIBXL_PSR_CBM_TYPE_L3_CBM_CODE);
        psr_cat_print_one_domain_cbm_type(domid, socketid,
                                          LIBXL_PSR_CBM_TYPE_L3_CBM_DATA);
    }

    printf("\n");
}

static int psr_cat_print_domain_cbm(uint32_t domid, uint32_t socketid,
                                    bool cdp_enabled)
{
    int i, nr_domains;
    libxl_dominfo *list;

    if (domid != INVALID_DOMID) {
        psr_cat_print_one_domain_cbm(domid, socketid, cdp_enabled);
        return 0;
    }

    if (!(list = libxl_list_domain(ctx, &nr_domains))) {
        fprintf(stderr, "Failed to get domain list for cbm display\n");
        return -1;
    }

    for (i = 0; i < nr_domains; i++)
        psr_cat_print_one_domain_cbm(list[i].domid, socketid, cdp_enabled);
    libxl_dominfo_list_free(list, nr_domains);

    return 0;
}

static int psr_cat_print_socket(uint32_t domid, libxl_psr_cat_info *info)
{
    int rc;
    uint32_t l3_cache_size;

    rc = libxl_psr_cmt_get_l3_cache_size(ctx, info->id, &l3_cache_size);
    if (rc) {
        fprintf(stderr, "Failed to get l3 cache size for socket:%d\n",
                info->id);
        return -1;
    }

    printf("%-16s: %u\n", "Socket ID", info->id);
    printf("%-16s: %uKB\n", "L3 Cache", l3_cache_size);
    printf("%-16s: %#llx\n", "Default CBM", (1ull << info->cbm_len) - 1);
    if (info->cdp_enabled)
        printf("%5s%25s%16s%16s\n", "ID", "NAME", "CBM (code)", "CBM (data)");
    else
        printf("%5s%25s%16s\n", "ID", "NAME", "CBM");

    return psr_cat_print_domain_cbm(domid, info->id, info->cdp_enabled);
}

static int psr_cat_show(uint32_t domid)
{
    int i, nr;
    int rc;
    libxl_psr_cat_info *info;

    rc = libxl_psr_cat_get_l3_info(ctx, &info, &nr);
    if (rc) {
        fprintf(stderr, "Failed to get cat info\n");
        return rc;
    }

    for (i = 0; i < nr; i++) {
        rc = psr_cat_print_socket(domid, info + i);
        if (rc)
            goto out;
    }

out:
    libxl_psr_cat_info_list_free(info, nr);
    return rc;
}

int main_psr_cat_cbm_set(int argc, char **argv)
{
    uint32_t domid;
    libxl_psr_cbm_type type;
    uint64_t cbm;
    int ret, opt = 0;
    int opt_data = 0, opt_code = 0;
    libxl_bitmap target_map;
    char *value;
    libxl_string_list socket_list;
    unsigned long start, end;
    int i, j, len;

    static struct option opts[] = {
        {"socket", 1, 0, 's'},
        {"data", 0, 0, 'd'},
        {"code", 0, 0, 'c'},
        COMMON_LONG_OPTS
    };

    libxl_socket_bitmap_alloc(ctx, &target_map, 0);
    libxl_bitmap_set_none(&target_map);

    SWITCH_FOREACH_OPT(opt, "s:cd", opts, "psr-cat-cbm-set", 2) {
    case 's':
        trim(isspace, optarg, &value);
        split_string_into_string_list(value, ",", &socket_list);
        len = libxl_string_list_length(&socket_list);
        for (i = 0; i < len; i++) {
            parse_range(socket_list[i], &start, &end);
            for (j = start; j <= end; j++)
                libxl_bitmap_set(&target_map, j);
        }

        libxl_string_list_dispose(&socket_list);
        free(value);
        break;
    case 'd':
        opt_data = 1;
        break;
    case 'c':
        opt_code = 1;
        break;
    }

    if (opt_data && opt_code) {
        fprintf(stderr, "Cannot handle -c and -d at the same time\n");
        return -1;
    } else if (opt_data) {
        type = LIBXL_PSR_CBM_TYPE_L3_CBM_DATA;
    } else if (opt_code) {
        type = LIBXL_PSR_CBM_TYPE_L3_CBM_CODE;
    } else {
        type = LIBXL_PSR_CBM_TYPE_L3_CBM;
    }

    if (libxl_bitmap_is_empty(&target_map))
        libxl_bitmap_set_any(&target_map);

    if (argc != optind + 2) {
        help("psr-cat-cbm-set");
        return 2;
    }

    domid = find_domain(argv[optind]);
    cbm = strtoll(argv[optind + 1], NULL , 0);

    ret = libxl_psr_cat_set_cbm(ctx, domid, type, &target_map, cbm);

    libxl_bitmap_dispose(&target_map);
    return ret;
}

int main_psr_cat_show(int argc, char **argv)
{
    int opt;
    uint32_t domid;

    SWITCH_FOREACH_OPT(opt, "", NULL, "psr-cat-show", 0) {
        /* No options */
    }

    if (optind >= argc)
        domid = INVALID_DOMID;
    else if (optind == argc - 1)
        domid = find_domain(argv[optind]);
    else {
        help("psr-cat-show");
        return 2;
    }

    return psr_cat_show(domid);
}

int main_psr_hwinfo(int argc, char **argv)
{
    int opt, ret = 0;
    bool all = true, cmt = false, cat = false;
    static struct option opts[] = {
        {"cmt", 0, 0, 'm'},
        {"cat", 0, 0, 'a'},
        COMMON_LONG_OPTS
    };

    SWITCH_FOREACH_OPT(opt, "ma", opts, "psr-hwinfo", 0) {
    case 'm':
        all = false; cmt = true;
        break;
    case 'a':
        all = false; cat = true;
        break;
    }

    if (!ret && (all || cmt))
        ret = psr_cmt_hwinfo();

    if (!ret && (all || cat))
        ret = psr_cat_hwinfo();

    return ret;
}

#endif

/*
 * Local variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
