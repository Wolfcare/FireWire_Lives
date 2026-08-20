/*
 * fw_transaction.h - Split-transaction tracking for asynchronous 1394 requests.
 *
 * Every async request carries a 6-bit transaction label. The responder echoes
 * that label back, and the label is the only thing tying a response to the
 * request that caused it. This layer owns label allocation, response matching,
 * split-timeout expiry, and the bus-reset generation guard.
 *
 * It performs no I/O. Callers hand it a clock and receive packet headers to
 * transmit, which keeps the whole thing testable without hardware.
 */

#ifndef FW_TRANSACTION_H
#define FW_TRANSACTION_H

#include "fw_types.h"
#include "fw_packet.h"

#ifdef __cplusplus
extern "C" {
#endif

/* tlabel is 6 bits, so 64 transactions may be outstanding at once. */
#define FW_MAX_TLABELS 64

/* The spec's split timeout. A responder that cannot answer immediately must
 * do so within this window or the requester declares the transaction dead. */
#define FW_DEFAULT_SPLIT_TIMEOUT_US 100000u   /* 100 ms */

typedef enum {
    FW_TXN_FREE = 0,
    FW_TXN_PENDING,
} fw_txn_state_t;

/* Why a transaction stopped being pending. */
typedef enum {
    FW_TXN_DONE_RESPONSE = 0,  /* a matching response arrived */
    FW_TXN_DONE_TIMEOUT,       /* split timeout elapsed */
    FW_TXN_DONE_BUS_RESET,     /* generation changed underneath it */
} fw_txn_completion_t;

typedef struct {
    fw_txn_state_t state;
    uint8_t        tlabel;
    uint8_t        tcode;
    fw_nodeid_t    destination;
    fw_offset_t    offset;
    uint32_t       generation;   /* bus generation this was issued under */
    uint64_t       deadline_us;
    void          *context;      /* opaque, returned to the caller on completion */

    /* Filled in on completion. */
    fw_txn_completion_t completion;
    fw_rcode_t     rcode;
    fw_quadlet_t   response_quadlet;
    uint16_t       response_length;
} fw_txn_t;

typedef struct {
    fw_txn_t  txn[FW_MAX_TLABELS];
    uint64_t  allocated;            /* bitmap of in-use tlabels */
    uint8_t   next_tlabel;          /* round-robin cursor */
    uint32_t  generation;
    uint32_t  split_timeout_us;
    fw_nodeid_t local_node;

    /* Counters, useful for diagnostics and for asserting in tests. */
    uint32_t  stat_started;
    uint32_t  stat_responses;
    uint32_t  stat_timeouts;
    uint32_t  stat_bus_resets;
    uint32_t  stat_unmatched;
} fw_txn_mgr_t;

/*
 * Initialise a manager. `local_node` is our own node ID, used to sanity-check
 * that arriving responses were actually addressed to us. Pass 0 for
 * `split_timeout_us` to take the spec default.
 */
void fw_txn_mgr_init(fw_txn_mgr_t *mgr, fw_nodeid_t local_node,
                     uint32_t generation, uint32_t split_timeout_us);

/*
 * Start a transaction, allocating a tlabel and building the request header.
 *
 * Returns FW_OK and fills `out_hdr`, or FW_ERR_NOSPACE when all 64 labels are
 * outstanding. `context` is stored and handed back on completion.
 */
fw_err_t fw_txn_begin(fw_txn_mgr_t *mgr, fw_nodeid_t destination,
                      uint8_t tcode, fw_offset_t offset,
                      uint16_t data_length, fw_quadlet_t quadlet_data,
                      uint64_t now_us, void *context,
                      fw_packet_header_t *out_hdr);

/*
 * Match an arriving response against a pending request and retire it.
 *
 * On success `out` receives a copy of the completed transaction, including the
 * caller's context pointer, and the tlabel is released.
 *
 * Returns FW_ERR_NOTFOUND when the response does not correspond to anything
 * pending - a late duplicate, a response to a transaction we already timed
 * out, or a response from the wrong node. Such packets are dropped rather
 * than acted upon, because acting on them is how stale data reaches a caller.
 */
fw_err_t fw_txn_match_response(fw_txn_mgr_t *mgr,
                               const fw_packet_header_t *response,
                               fw_txn_t *out);

/*
 * Retire every transaction whose split timeout has elapsed.
 *
 * Completed entries are written to `expired` (up to `max`), and the number
 * retired is returned. Pass NULL/0 to simply discard them.
 */
int fw_txn_expire(fw_txn_mgr_t *mgr, uint64_t now_us,
                  fw_txn_t *expired, size_t max);

/*
 * Handle a bus reset: abort every outstanding transaction and adopt the new
 * generation.
 *
 * This is not optional cleanup. After a reset, node IDs are reassigned, so a
 * response arriving for a pre-reset request may come from a physically
 * different device that inherited the node ID. Failing these transactions
 * rather than retrying them is what stops one device's data being handed back
 * as another's.
 */
int fw_txn_bus_reset(fw_txn_mgr_t *mgr, uint32_t new_generation,
                     fw_txn_t *aborted, size_t max);

/* Number of currently outstanding transactions. */
unsigned fw_txn_pending(const fw_txn_mgr_t *mgr);

const char *fw_txn_completion_name(fw_txn_completion_t c);

#ifdef __cplusplus
}
#endif

#endif /* FW_TRANSACTION_H */
