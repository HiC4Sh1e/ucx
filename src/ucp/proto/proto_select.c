/**
 * Copyright (C) Mellanox Technologies Ltd. 2020.  ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "proto_select.h"
#include "proto_select.inl"

#include <ucp/core/ucp_context.h>
#include <ucp/core/ucp_worker.h>
#include <ucp/dt/dt.h>
#include <float.h>

#include <ucs/datastruct/array.inl>


/* Compare two protocols which intersect at point X, by examining their value
 * at point (X + UCP_PROTO_MSGLEN_EPSILON)
 */
#define UCP_PROTO_MSGLEN_EPSILON   0.5


/* Parameters structure for initializing protocols for a selection parameter */
typedef struct {
    const ucp_proto_select_param_t *select_param; /* Protocol selection parameter */
    ucp_proto_id_mask_t            mask;          /* Which protocols are valid */
    ucp_proto_caps_t               caps[UCP_PROTO_MAX_COUNT]; /* Protocols capabilities */
    void                           *priv_buf;     /* Protocols configuration buffer */
    size_t                         priv_offsets[UCP_PROTO_MAX_COUNT]; /* Offset of each
                                                                         protocol's private
                                                                         area in 'priv_buf' */
} ucp_proto_select_init_protocols_t;

/* Temporary list of constructed protocol thresholds */
typedef struct {
    size_t                         max_length; /* Maximal message size */
    ucp_proto_id_t                 proto_id;   /* Selected protocol up to 'max_length' */
} ucp_proto_threshold_tmp_elem_t;


UCS_ARRAY_DEFINE_INLINE(ucp_proto_thresh, unsigned,
                        ucp_proto_threshold_tmp_elem_t);

const ucp_proto_threshold_elem_t*
ucp_proto_thresholds_search_slow(const ucp_proto_threshold_elem_t *thresholds,
                                 size_t msg_length)
{
    unsigned idx;
    for (idx = 0; msg_length > thresholds[idx].max_msg_length; ++idx);
    return &thresholds[idx];
}

static ucs_status_t
ucp_proto_thresholds_append(ucs_array_t(ucp_proto_thresh) *thresh_list,
                            size_t max_length, ucp_proto_id_t proto_id)
{
    ucp_proto_threshold_tmp_elem_t *thresh_elem;
    ucs_status_t status;

    /* Consolidate with last protocol if possible */
    if (!ucs_array_is_empty(thresh_list)) {
        thresh_elem = ucs_array_last(thresh_list);
        ucs_assertv(max_length > thresh_elem->max_length,
                    "max_length=%zu last->max_length=%zu",
                    max_length, thresh_elem->max_length);
        if (thresh_elem->proto_id == proto_id) {
            thresh_elem->max_length = max_length;
            return UCS_OK;
        }
    }

    status = ucs_array_append(ucp_proto_thresh, thresh_list);
    if (status != UCS_OK) {
        return status;
    }

    thresh_elem             = ucs_array_last(thresh_list);
    thresh_elem->max_length = max_length;
    thresh_elem->proto_id   = proto_id;
    return UCS_OK;
}

static ucs_status_t
ucp_proto_thresholds_select_best(ucp_proto_id_mask_t proto_mask,
                                 const ucs_linear_func_t *proto_perf,
                                 ucs_array_t(ucp_proto_thresh) *thresh_list,
                                 size_t start, size_t end)
{
    struct {
        ucp_proto_id_t proto_id;
        double         result;
    } curr, best;
    ucs_status_t status;
    double x_intersect;
    size_t midpoint;
    char buf[64];

    ucs_trace("candidate protocols for [%s]:",
              ucs_memunits_range_str(start, end, buf, sizeof(buf)));
    ucs_for_each_bit(curr.proto_id, proto_mask) {
        ucs_trace("%24s %.0f+%.3f*X nsec",
                  ucp_proto_id_field(curr.proto_id, name),
                  proto_perf[curr.proto_id].c * UCS_NSEC_PER_SEC,
                  proto_perf[curr.proto_id].m * UCS_NSEC_PER_SEC);
    }

    do {
        ucs_assert(proto_mask != 0);

        /* Find best protocol at the 'start' point */
        best.result   = DBL_MAX;
        best.proto_id = UCP_PROTO_ID_INVALID;
        ucs_for_each_bit(curr.proto_id, proto_mask) {
            curr.result = ucs_linear_func_apply(proto_perf[curr.proto_id],
                                                start + UCP_PROTO_MSGLEN_EPSILON);
            ucs_assert(curr.result != DBL_MAX);
            if (curr.result < best.result) {
                best = curr;
            }
        }

        /* Since proto_mask != 0, we should find at least one protocol */
        ucs_assert(best.proto_id != UCP_PROTO_ID_INVALID);

        ucs_trace("  best protocol at %s is %s",
                  ucs_memunits_to_str(start, buf, sizeof(buf)),
                  ucp_proto_id_field(best.proto_id, name));

        /* Find first (smallest) intersection point between the current best
         * protocol and any other protocol. This would be the point where that
         * other protocol becomes the best one.
         */
        midpoint    = end;
        proto_mask &= ~UCS_BIT(best.proto_id);
        ucs_for_each_bit(curr.proto_id, proto_mask) {
            status = ucs_linear_func_intersect(proto_perf[curr.proto_id],
                                               proto_perf[best.proto_id],
                                               &x_intersect);
            if ((status == UCS_OK) && (x_intersect > start)) {
                /* We care only if the intersection is after 'start', since
                 * otherwise best.proto_id is better than curr.proto_id at
                 * 'end' as well as at 'start'.
                 */
                if (x_intersect < (double)SIZE_MAX) {
                    midpoint = ucs_min((size_t)x_intersect, midpoint);
                }
                ucs_trace("   - intersects with %s at %.2f, midpoint is %s",
                          ucp_proto_id_field(curr.proto_id, name), x_intersect,
                          ucs_memunits_to_str(midpoint, buf, sizeof(buf)));
            } else {
                ucs_trace("   - intersects with %s out of range",
                          ucp_proto_id_field(curr.proto_id, name));
            }
        }

        status = ucp_proto_thresholds_append(thresh_list, midpoint,
                                             best.proto_id);
        if (status != UCS_OK) {
            return status;
        }

        start = midpoint + 1;
    } while (midpoint < end);

    return UCS_OK;
}

/*
 * Select a protocol for 'msg_length', return last message length for the proto
 */
static ucs_status_t
ucp_proto_thresholds_select_next(ucp_proto_id_mask_t proto_mask,
                                 const ucp_proto_caps_t *proto_caps,
                                 ucs_array_t(ucp_proto_thresh) *thresh_list,
                                 size_t msg_length, size_t *max_length_p)
{
    ucs_linear_func_t proto_perf[UCP_PROTO_MAX_COUNT];
    ucp_proto_id_mask_t valid_proto_mask;  /* Valid protocols in the range */
    ucp_proto_id_mask_t forced_proto_mask; /* Protocols forced by user */
    const ucp_proto_caps_t *caps;
    ucp_proto_id_t proto_id;
    ucs_status_t status;
    size_t max_length;
    unsigned i;

    /*
     * Find the valid and configured protocols starting from 'msg_length'.
     * Start with endpoint at SIZE_MAX, and narrow it down whenever we encounter
     * a protocol with different configuration.
     */
    valid_proto_mask  = 0;
    forced_proto_mask = 0;
    max_length        = SIZE_MAX;
    ucs_for_each_bit(proto_id, proto_mask) {
        caps = &proto_caps[proto_id];

        /* Check if the protocol supports message length 'msg_length' */
        if (msg_length < caps->min_length) {
            ucs_trace("skipping proto %d with min_length %zu for msg_length %zu",
                      proto_id, caps->min_length, msg_length);
            continue;
        }

        /* Update 'max_length' by the maximal message length of the protocol */
        for (i = 0; i < caps->num_ranges; ++i) {
            /* Find first (and only) range which contains 'msg_length' */
            if (msg_length <= caps->ranges[i].max_length) {
                valid_proto_mask    |= UCS_BIT(proto_id);
                proto_perf[proto_id] = caps->ranges[i].perf;
                max_length           = ucs_min(max_length,
                                               caps->ranges[i].max_length);
                break;
            }
        }

        /* Apply user threshold configuration */
        if (caps->cfg_thresh != UCS_MEMUNITS_AUTO) {
            if (caps->cfg_thresh == UCS_MEMUNITS_INF) {
                /* 'inf' - protocol is disabled */
                valid_proto_mask  &= ~UCS_BIT(proto_id);
            } else if (caps->cfg_thresh <= msg_length) {
                /* The protocol is force-activated on 'msg_length' and above */
                forced_proto_mask |= UCS_BIT(proto_id);
            } else {
                /* The protocol is completely disabled up to 'cfg_thresh' - 1 */
                max_length         = ucs_min(max_length, caps->cfg_thresh - 1);
                valid_proto_mask  &= ~UCS_BIT(proto_id);
            }
        }
    }
    ucs_assert(msg_length <= max_length);

    if (valid_proto_mask == 0) {
        return UCS_ERR_UNSUPPORTED;
    }

    /* If we have forced protocols by user-configured threshold, use only those */
    forced_proto_mask &= valid_proto_mask;
    if (forced_proto_mask != 0) {
        valid_proto_mask = forced_proto_mask;
    }

    status = ucp_proto_thresholds_select_best(valid_proto_mask, proto_perf,
                                              thresh_list, msg_length,
                                              max_length);
    if (status != UCS_OK) {
        return status;
    }

    *max_length_p = max_length;
    return UCS_OK;
}

static ucs_status_t
ucp_proto_select_init_protocols(ucp_worker_h worker,
                                ucp_worker_cfg_index_t ep_cfg_index,
                                ucp_worker_cfg_index_t rkey_cfg_index,
                                const ucp_proto_select_param_t *select_param,
                                ucp_proto_select_init_protocols_t *proto_init)
{
    ucp_proto_init_params_t init_params;
    ucs_string_buffer_t strb;
    size_t priv_size, offset;
    ucp_proto_id_t proto_id;
    ucs_status_t status;
    void *tmp;

    ucs_assert(ep_cfg_index != UCP_WORKER_CFG_INDEX_NULL);

    init_params.worker        = worker;
    init_params.select_param  = select_param;
    init_params.ep_config_key = &worker->ep_config[ep_cfg_index].key;

    if (rkey_cfg_index == UCP_WORKER_CFG_INDEX_NULL) {
        init_params.rkey_config_key = NULL;
    } else {
        init_params.rkey_config_key = &worker->rkey_config[rkey_cfg_index].key;

        /* rkey configuration must be for the same ep */
        ucs_assertv_always(
                init_params.rkey_config_key->ep_cfg_index == ep_cfg_index,
                "rkey->ep_cfg_index=%d ep_cfg_index=%d",
                init_params.rkey_config_key->ep_cfg_index, ep_cfg_index);
    }

    proto_init->select_param = select_param;
    proto_init->mask         = 0;

    /* Initialize protocols and get their capabilities */
    proto_init->priv_buf = ucs_malloc(ucp_protocols_count * UCP_PROTO_PRIV_MAX,
                                      "ucp_proto_priv");
    if (proto_init->priv_buf == NULL) {
        status = UCS_ERR_NO_MEMORY;
        goto err;
    }

    offset = 0;
    for (proto_id = 0; proto_id < ucp_protocols_count; ++proto_id) {
        init_params.priv      = UCS_PTR_BYTE_OFFSET(proto_init->priv_buf,
                                                          offset);
        init_params.priv_size  = &priv_size;
        init_params.caps       = &proto_init->caps[proto_id];
        init_params.proto_name = ucp_proto_id_field(proto_id, name);

        status = ucp_proto_id_call(proto_id, init, &init_params);
        if (status != UCS_OK) {
            continue;
        }

        proto_init->mask                  |= UCS_BIT(proto_id);
        proto_init->priv_offsets[proto_id] = offset;
        offset                            += priv_size;
    }

    if (proto_init->mask == 0) {
        /* No protocol can support the given selection parameters */
        ucp_proto_select_param_str(select_param, &strb);
        ucs_debug("no protocols found for %s", ucs_string_buffer_cstr(&strb));
        ucs_string_buffer_cleanup(&strb);
        status = UCS_ERR_NO_ELEM;
        goto err_free_priv;
    }

    /* Finalize the shared priv buffer size */
    if (offset == 0) {
        ucs_free(proto_init->priv_buf);
        proto_init->priv_buf = NULL;
    } else {
        tmp = ucs_realloc(proto_init->priv_buf, offset, "ucp_proto_priv");
        if (tmp == NULL) {
            status = UCS_ERR_NO_MEMORY;
            goto err_free_priv;
        }

        proto_init->priv_buf = tmp;
    }

    return UCS_OK;

err_free_priv:
    ucs_free(proto_init->priv_buf);
err:
    return status;
}

static ucs_status_t
ucp_proto_select_elem_init_thresh(ucp_proto_select_elem_t *select_elem,
                                  const ucp_proto_select_init_protocols_t *proto_init,
                                  const char *select_param_str)
{

    ucp_proto_threshold_tmp_elem_t thresh_buffer[UCP_PROTO_MAX_COUNT];
    ucs_array_t(ucp_proto_thresh) tmp_thresh_list =
              UCS_ARRAY_FIXED_INITIALIZER(thresh_buffer, UCP_PROTO_MAX_COUNT);
    ucp_proto_threshold_tmp_elem_t *tmp_elem;
    ucp_proto_threshold_elem_t *thresh_elem;
    ucp_proto_config_t *proto_config;
    size_t msg_length, max_length;
    ucp_proto_id_t proto_id;
    ucs_status_t status;
    size_t priv_offset;

    /*
     * Select a protocol for every message size interval, until we cover all
     * possible message sizes until SIZE_MAX.
     */
    msg_length = 0;
    do {
        /* Select a protocol which can handle messages starting from 'msg_length',
         * and update max_length with the last message length for which this
         * protocol is selected.
         */
        status = ucp_proto_thresholds_select_next(proto_init->mask,
                                                  proto_init->caps,
                                                  &tmp_thresh_list, msg_length,
                                                  &max_length);
        if (status != UCS_OK) {
            if (status == UCS_ERR_UNSUPPORTED) {
                ucs_warn("no protocol for %s msg_length %zu", select_param_str,
                         msg_length);
            }
            return status;
        }

        msg_length = max_length + 1;
    } while (max_length < SIZE_MAX);

    ucs_assert_always(!ucs_array_is_empty(&tmp_thresh_list));

    /* Set pointer to priv buffer (to release it during cleanup) */
    select_elem->priv_buf   = proto_init->priv_buf;

    /* Allocate thresholds array */
    select_elem->thresholds = ucs_calloc(ucs_array_length(&tmp_thresh_list),
                                         sizeof(*select_elem->thresholds),
                                         "ucp_proto_thresholds");
    if (select_elem->thresholds == NULL) {
        return UCS_ERR_NO_MEMORY;
    }

    /* Copy the temporary thresholds list to an array inside select_elem */
    thresh_elem = select_elem->thresholds;
    ucs_array_for_each(tmp_elem, &tmp_thresh_list) {
        proto_id                    = tmp_elem->proto_id;
        priv_offset                 = proto_init->priv_offsets[proto_id];
        thresh_elem->max_msg_length = tmp_elem->max_length;
        proto_config                = &thresh_elem->proto_config;
        proto_config->select_param  = *proto_init->select_param;
        proto_config->proto         = ucp_protocols[proto_id];
        proto_config->priv          = UCS_PTR_BYTE_OFFSET(select_elem->priv_buf,
                                                          priv_offset);
        ++thresh_elem;
    }

    return UCS_OK;
}

static ucs_status_t
ucp_proto_select_elem_init(ucp_worker_h worker,
                           ucp_worker_cfg_index_t ep_cfg_index,
                           ucp_worker_cfg_index_t rkey_cfg_index,
                           const ucp_proto_select_param_t *select_param,
                           ucp_proto_select_elem_t *select_elem)
{
    ucp_proto_select_init_protocols_t *proto_init;
    ucs_string_buffer_t strb;
    ucs_status_t status;

    ucp_proto_select_param_str(select_param, &strb);

    ucs_trace("initialize selection for %s worker %p ep_config %d rkey_config %d",
              ucs_string_buffer_cstr(&strb), worker, ep_cfg_index, rkey_cfg_index);

    proto_init = ucs_malloc(sizeof(*proto_init), "proto_init");
    if (proto_init == NULL) {
        status = UCS_ERR_NO_MEMORY;
        goto out_free_strb;
    }

    status = ucp_proto_select_init_protocols(worker, ep_cfg_index, rkey_cfg_index,
                                             select_param, proto_init);
    if (status != UCS_OK) {
        goto out_free_proto_init;
    }

    status = ucp_proto_select_elem_init_thresh(select_elem, proto_init,
                                               ucs_string_buffer_cstr(&strb));
    if (status != UCS_OK) {
        goto err_cleanup_protocols;
    }

    status = UCS_OK;
    goto out_free_proto_init;

err_cleanup_protocols:
    ucs_free(proto_init->priv_buf);
out_free_proto_init:
    ucs_free(proto_init);
out_free_strb:
    ucs_string_buffer_cleanup(&strb);
    return status;
}

static void
ucp_proto_select_elem_cleanup(ucp_proto_select_elem_t *select_elem)
{
    ucs_free(select_elem->thresholds);
    ucs_free(select_elem->priv_buf);
}

static void  ucp_proto_select_cache_reset(ucp_proto_select_t *proto_select)
{
    proto_select->cache.key   = UINT64_MAX;
    proto_select->cache.value = NULL;
}

ucp_proto_select_elem_t *
ucp_proto_select_lookup_slow(ucp_worker_h worker,
                             ucp_proto_select_t *proto_select,
                             ucp_worker_cfg_index_t ep_cfg_index,
                             ucp_worker_cfg_index_t rkey_cfg_index,
                             const ucp_proto_select_param_t *select_param)
{
    ucp_proto_select_elem_t *select_elem;
    ucp_proto_select_key_t key;
    ucs_status_t status;
    khiter_t khiter;
    int khret;

    key.param = *select_param;
    khiter    = kh_put(ucp_proto_select_hash, &proto_select->hash, key.u64,
                       &khret);
    ucs_assert_always((khret == UCS_KH_PUT_BUCKET_EMPTY) ||
                      (khret == UCS_KH_PUT_BUCKET_CLEAR));

    /* Adding hash values may reallocate the array, so the cached pointer to
     * select_elem may not be valid anymore.
     */
    ucp_proto_select_cache_reset(proto_select);

    select_elem = &kh_value(&proto_select->hash, khiter);
    status      = ucp_proto_select_elem_init(worker, ep_cfg_index, rkey_cfg_index,
                                             select_param, select_elem);
    if (status != UCS_OK) {
        kh_del(ucp_proto_select_hash, &proto_select->hash, khiter);
        return NULL;
    }

    return select_elem;
}

ucs_status_t ucp_proto_select_init(ucp_proto_select_t *proto_select)
{
    kh_init_inplace(ucp_proto_select_hash, &proto_select->hash);
    ucp_proto_select_cache_reset(proto_select);
    return UCS_OK;
}

void ucp_proto_select_cleanup(ucp_proto_select_t *proto_select)
{
    ucp_proto_select_elem_t select_elem;

    kh_foreach_value(&proto_select->hash, select_elem,
         ucp_proto_select_elem_cleanup(&select_elem)
    )
    kh_destroy_inplace(ucp_proto_select_hash, &proto_select->hash);
}

static void
ucp_proto_select_dump_all(ucp_worker_h worker,
                          ucp_worker_cfg_index_t ep_cfg_index,
                          ucp_worker_cfg_index_t rkey_cfg_index,
                          const ucp_proto_select_param_t *select_param,
                          FILE *stream)
{
    static const char *proto_info_fmt =
                                "#     %-18s %-12s %-20s %-18s %-12s %s\n";
    ucp_proto_select_init_protocols_t *proto_init;
    ucs_string_buffer_t config_strb;
    size_t range_start, range_end;
    const ucp_proto_caps_t *caps;
    ucp_proto_id_t proto_id;
    ucs_status_t status;
    char range_str[64];
    char perf_str[64];
    char thresh_str[64];
    char bw_str[64];
    unsigned i;
    void *priv;

    /* Allocate on heap, since the structure is quite large */
    proto_init = ucs_malloc(sizeof(*proto_init), "proto_init");
    if (proto_init == NULL) {
        fprintf(stream, "<Could not allocate memory>\n");
        return;
    }

    status = ucp_proto_select_init_protocols(worker, ep_cfg_index, rkey_cfg_index,
                                             select_param, proto_init);
    if (status != UCS_OK) {
        fprintf(stream, "<%s>\n", ucs_status_string(status));
        goto out_free;
    }

    fprintf(stream, proto_info_fmt, "PROTOCOL", "SIZE", "TIME (nsec)",
            "BANDWIDTH (MiB/s)", "THRESHOLD", "CONIFURATION");

    ucs_for_each_bit(proto_id, proto_init->mask) {

        priv = UCS_PTR_BYTE_OFFSET(proto_init->priv_buf,
                                   proto_init->priv_offsets[proto_id]);
        caps = &proto_init->caps[proto_id];

        /* Get protocol configuration */
        ucp_proto_id_call(proto_id, config_str, priv, &config_strb);

        /* String for configured threshold */
        ucs_memunits_to_str(caps->cfg_thresh, thresh_str, sizeof(thresh_str));

        range_start = caps->min_length;
        for (i = 0; i < caps->num_ranges; ++i) {
            /* String for performance range */
            range_end = caps->ranges[i].max_length;
            ucs_memunits_range_str(range_start, range_end, range_str,
                                   sizeof(range_str));

            /* String for estimated performance */
            snprintf(perf_str, sizeof(perf_str), "%5.0f + %.3f * N",
                     caps->ranges[i].perf.c * 1e9,
                     caps->ranges[i].perf.m * 1e9);

            /* String for bandwidth */
            snprintf(bw_str, sizeof(bw_str), "%7.2f",
                     1.0 / (caps->ranges[i].perf.m * UCS_MBYTE));

            fprintf(stream, proto_info_fmt,
                    (i == 0) ? ucp_proto_id_field(proto_id, name) : "",
                    range_str, perf_str, bw_str,
                    (i == 0) ? thresh_str : "",
                    (i == 0) ? ucs_string_buffer_cstr(&config_strb) : "");

            range_start = range_end + 1;
        }

        ucs_string_buffer_cleanup(&config_strb);
    }
    fprintf(stream, "#\n");

    ucs_free(proto_init->priv_buf);
out_free:
    ucs_free(proto_init);
}

static void
ucp_proto_select_dump_thresholds(const ucp_proto_select_elem_t *select_elem,
                                 FILE *stream)
{
    static const char *proto_info_fmt = "#     %-16s %-18s %s\n";
    const ucp_proto_threshold_elem_t *thresh_elem;
    ucs_string_buffer_t strb;
    size_t range_start, range_end;
    char str[128];

    range_start = 0;
    thresh_elem = select_elem->thresholds;
    fprintf(stream, proto_info_fmt, "SIZE", "PROTOCOL", "CONFIGURATION");
    do {
        thresh_elem->proto_config.proto->config_str(
                thresh_elem->proto_config.priv, &strb);

        range_end = thresh_elem->max_msg_length;

        fprintf(stream, proto_info_fmt,
                ucs_memunits_range_str(range_start, range_end, str, sizeof(str)),
                thresh_elem->proto_config.proto->name,
                ucs_string_buffer_cstr(&strb));

        ucs_string_buffer_cleanup(&strb);

        range_start = range_end + 1;
        ++thresh_elem;
    } while (range_end != SIZE_MAX);
}

static void
ucp_proto_select_elem_dump(ucp_worker_h worker,
                           ucp_worker_cfg_index_t ep_cfg_index,
                           ucp_worker_cfg_index_t rkey_cfg_index,
                           const ucp_proto_select_param_t *select_param,
                           const ucp_proto_select_elem_t *select_elem,
                           FILE *stream)
{
    ucs_string_buffer_t strb;
    size_t i;

    fprintf(stream, "#\n");

    ucp_proto_select_param_str(select_param, &strb);
    fprintf(stream, "# %s:\n", ucs_string_buffer_cstr(&strb));
    fprintf(stream, "# ");
    for (i = 0; i < ucs_string_buffer_length(&strb); ++i) {
        fputc('=', stream);
    }
    fprintf(stream, "\n");
    ucs_string_buffer_cleanup(&strb);

    fprintf(stream, "#\n");
    fprintf(stream, "#   Selected protocols:\n");

    ucp_proto_select_dump_thresholds(select_elem, stream);

    fprintf(stream, "#\n");

    fprintf(stream, "#   Candidates:\n");
    ucp_proto_select_dump_all(worker, ep_cfg_index, rkey_cfg_index,
                              select_param, stream);
}

void ucp_proto_select_dump(ucp_worker_h worker,
                           ucp_worker_cfg_index_t ep_cfg_index,
                           ucp_worker_cfg_index_t rkey_cfg_index,
                           ucp_proto_select_t *proto_select, FILE *stream)
{
    ucp_proto_select_elem_t select_elem;
    ucp_proto_select_key_t key;

    fprintf(stream, "# \n");
    fprintf(stream, "# Protocols selection for ep_config[%d]/rkey_config[%d] "
            "(%d items)\n", ep_cfg_index, rkey_cfg_index,
            kh_size(&proto_select->hash));
    fprintf(stream, "# \n");
    kh_foreach(&proto_select->hash, key.u64, select_elem,
         ucp_proto_select_elem_dump(worker, ep_cfg_index, rkey_cfg_index,
                                    &key.param, &select_elem, stream);
    )
}

void ucp_proto_select_param_str(const ucp_proto_select_param_t *select_param,
                                ucs_string_buffer_t *strb)
{
    uint32_t op_attr_mask;

    ucs_string_buffer_init(strb);

    op_attr_mask = ucp_proto_select_op_attr_from_flags(select_param->op_flags);
    ucs_string_buffer_appendf(strb, "%s()",
                              ucp_operation_names[select_param->op_id]);
    ucs_string_buffer_appendf(strb, " on a %s data-type",
                              ucp_datatype_class_names[select_param->dt_class]);
    if (select_param->sg_count > 1) {
        ucs_string_buffer_appendf(strb, "with %u scatter-gather entries",
                                  select_param->sg_count);
    }
    ucs_string_buffer_appendf(strb, " in %s memory",
                              ucs_memory_type_names[select_param->mem_type]);

    if (op_attr_mask & UCP_OP_ATTR_FLAG_FAST_CMPL) {
        ucs_string_buffer_appendf(strb, " and fast completion");
    }
}
