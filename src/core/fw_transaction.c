#include "fw/fw_transaction.h"

#include <string.h>

void fw_txn_mgr_init(fw_txn_mgr_t *mgr, fw_nodeid_t local_node,
                     uint32_t generation, uint32_t split_timeout_us)
{
    if (mgr == NULL)
        return;

    memset(mgr, 0, sizeof(*mgr));
    mgr->local_node = local_node;
    mgr->generation = generation;
    mgr->split_timeout_us = split_timeout_us ? split_timeout_us
                                             : FW_DEFAULT_SPLIT_TIMEOUT_US;
}

unsigned fw_txn_pending(const fw_txn_mgr_t *mgr)
{
    if (mgr == NULL)
        return 0;

    unsigned n = 0;
    for (int i = 0; i < FW_MAX_TLABELS; i++) {
        if (mgr->txn[i].state == FW_TXN_PENDING)
            n++;
    }
    return n;
}

/*
 * Allocate a tlabel round-robin rather than lowest-free.
 *
 * Immediately reusing a just-released label is dangerous: a slow responder may
 * still answer the previous transaction, and that late response would match
 * the new one and deliver the wrong payload. Cycling through all 64 before
 * reusing any gives late responses time to drain.
 */
static int alloc_tlabel(fw_txn_mgr_t *mgr)
{
    for (int i = 0; i < FW_MAX_TLABELS; i++) {
        uint8_t candidate = (uint8_t)((mgr->next_tlabel + i) % FW_MAX_TLABELS);
        if ((mgr->allocated & (1ull << candidate)) == 0) {
            mgr->allocated |= (1ull << candidate);
            mgr->next_tlabel = (uint8_t)((candidate + 1) % FW_MAX_TLABELS);
            return candidate;
        }
    }
    return -1;
}

static void release_tlabel(fw_txn_mgr_t *mgr, uint8_t tlabel)
{
    mgr->allocated &= ~(1ull << tlabel);
    mgr->txn[tlabel].state = FW_TXN_FREE;
}

fw_err_t fw_txn_begin(fw_txn_mgr_t *mgr, fw_nodeid_t destination,
                      uint8_t tcode, fw_offset_t offset,
                      uint16_t data_length, fw_quadlet_t quadlet_data,
                      uint64_t now_us, void *context,
                      fw_packet_header_t *out_hdr)
{
    if (mgr == NULL || out_hdr == NULL)
        return FW_ERR_INVAL;

    /* Only requests get transaction tracking; a response is the end of one. */
    if (fw_packet_is_response(tcode))
        return FW_ERR_INVAL;

    if (fw_packet_header_quadlets(tcode) < 0)
        return FW_ERR_UNSUPPORTED;

    int tlabel = alloc_tlabel(mgr);
    if (tlabel < 0)
        return FW_ERR_NOSPACE;

    fw_txn_t *t = &mgr->txn[tlabel];
    memset(t, 0, sizeof(*t));
    t->state       = FW_TXN_PENDING;
    t->tlabel      = (uint8_t)tlabel;
    t->tcode       = tcode;
    t->destination = destination;
    t->offset      = offset;
    t->generation  = mgr->generation;
    t->deadline_us = now_us + mgr->split_timeout_us;
    t->context     = context;

    memset(out_hdr, 0, sizeof(*out_hdr));
    out_hdr->tcode        = tcode;
    out_hdr->tlabel       = (uint8_t)tlabel;
    out_hdr->retry        = 1;            /* retry_X */
    out_hdr->destination  = destination;
    out_hdr->source       = mgr->local_node;
    out_hdr->offset       = offset;
    out_hdr->data_length  = data_length;
    out_hdr->quadlet_data = quadlet_data;

    mgr->stat_started++;
    return FW_OK;
}

fw_err_t fw_txn_match_response(fw_txn_mgr_t *mgr,
                               const fw_packet_header_t *response,
                               fw_txn_t *out)
{
    if (mgr == NULL || response == NULL)
        return FW_ERR_INVAL;

    if (!fw_packet_is_response(response->tcode)) {
        mgr->stat_unmatched++;
        return FW_ERR_INVAL;
    }

    uint8_t tlabel = response->tlabel & 0x3fu;
    fw_txn_t *t = &mgr->txn[tlabel];

    if (t->state != FW_TXN_PENDING) {
        /* Nothing outstanding under this label: a late duplicate, or a
         * response to something we already timed out. */
        mgr->stat_unmatched++;
        return FW_ERR_NOTFOUND;
    }

    /* The responder must be the node we sent to. Without this check a
     * different device reusing the tlabel could satisfy our request. */
    if (response->source != t->destination) {
        mgr->stat_unmatched++;
        return FW_ERR_NOTFOUND;
    }

    /* And it must be addressed to us. */
    if (response->destination != mgr->local_node) {
        mgr->stat_unmatched++;
        return FW_ERR_NOTFOUND;
    }

    /* A response must be the right kind for the request that provoked it. */
    int expected = fw_packet_response_tcode(t->tcode);
    if (expected < 0 || response->tcode != (uint8_t)expected) {
        mgr->stat_unmatched++;
        return FW_ERR_NOTFOUND;
    }

    t->completion       = FW_TXN_DONE_RESPONSE;
    t->rcode            = (fw_rcode_t)response->rcode;
    t->response_quadlet = response->quadlet_data;
    t->response_length  = response->data_length;

    if (out)
        *out = *t;

    release_tlabel(mgr, tlabel);
    mgr->stat_responses++;
    return FW_OK;
}

int fw_txn_expire(fw_txn_mgr_t *mgr, uint64_t now_us,
                  fw_txn_t *expired, size_t max)
{
    if (mgr == NULL)
        return FW_ERR_INVAL;

    int count = 0;

    for (int i = 0; i < FW_MAX_TLABELS; i++) {
        fw_txn_t *t = &mgr->txn[i];
        if (t->state != FW_TXN_PENDING)
            continue;
        if (now_us < t->deadline_us)
            continue;

        t->completion = FW_TXN_DONE_TIMEOUT;
        t->rcode      = FW_RCODE_TYPE_ERROR;

        if (expired && (size_t)count < max)
            expired[count] = *t;

        release_tlabel(mgr, (uint8_t)i);
        count++;
        mgr->stat_timeouts++;
    }

    return count;
}

int fw_txn_bus_reset(fw_txn_mgr_t *mgr, uint32_t new_generation,
                     fw_txn_t *aborted, size_t max)
{
    if (mgr == NULL)
        return FW_ERR_INVAL;

    int count = 0;

    for (int i = 0; i < FW_MAX_TLABELS; i++) {
        fw_txn_t *t = &mgr->txn[i];
        if (t->state != FW_TXN_PENDING)
            continue;

        t->completion = FW_TXN_DONE_BUS_RESET;
        t->rcode      = FW_RCODE_CONFLICT_ERROR;

        if (aborted && (size_t)count < max)
            aborted[count] = *t;

        release_tlabel(mgr, (uint8_t)i);
        count++;
    }

    mgr->generation = new_generation;
    mgr->stat_bus_resets++;

    /* Start the label cursor afresh so post-reset transactions do not collide
     * with any straggling pre-reset response. */
    mgr->next_tlabel = 0;

    return count;
}

const char *fw_txn_completion_name(fw_txn_completion_t c)
{
    switch (c) {
    case FW_TXN_DONE_RESPONSE:  return "response";
    case FW_TXN_DONE_TIMEOUT:   return "timeout";
    case FW_TXN_DONE_BUS_RESET: return "bus reset";
    default:                    return "unknown";
    }
}
