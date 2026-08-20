#include "test_util.h"
#include "fw/fw_transaction.h"

#define LOCAL_NODE 0xffc0
#define PEER_NODE  0xffc1
#define OTHER_NODE 0xffc2

/* Build the response a well-behaved peer would send for a request. */
static fw_packet_header_t make_response(const fw_packet_header_t *req,
                                        fw_rcode_t rcode,
                                        fw_quadlet_t payload)
{
    fw_packet_header_t r;
    memset(&r, 0, sizeof(r));
    r.tcode        = (uint8_t)fw_packet_response_tcode(req->tcode);
    r.tlabel       = req->tlabel;
    r.destination  = req->source;
    r.source       = req->destination;
    r.rcode        = (uint8_t)rcode;
    r.quadlet_data = payload;
    return r;
}

static void test_basic_round_trip(void)
{
    fw_txn_mgr_t mgr;
    fw_txn_mgr_init(&mgr, LOCAL_NODE, 1, 0);

    CHECK_EQ_U(mgr.split_timeout_us, FW_DEFAULT_SPLIT_TIMEOUT_US,
               "default split timeout adopted");

    int marker = 42;
    fw_packet_header_t req;
    fw_err_t err = fw_txn_begin(&mgr, PEER_NODE,
                                FW_TCODE_READ_QUADLET_REQUEST,
                                FW_CONFIG_ROM_BASE, 0, 0,
                                1000, &marker, &req);

    CHECK_EQ_U(err, FW_OK, "transaction starts");
    CHECK_EQ_U(req.source, LOCAL_NODE, "request carries our node id");
    CHECK_EQ_U(req.destination, PEER_NODE, "request addressed to the peer");
    CHECK_EQ_U(req.offset, FW_CONFIG_ROM_BASE, "request offset");
    CHECK_EQ_U(fw_txn_pending(&mgr), 1, "one transaction outstanding");

    fw_packet_header_t resp = make_response(&req, FW_RCODE_COMPLETE, 0x04040000u);
    fw_txn_t done;
    err = fw_txn_match_response(&mgr, &resp, &done);

    CHECK_EQ_U(err, FW_OK, "response matches the request");
    CHECK_EQ_U(done.rcode, FW_RCODE_COMPLETE, "rcode surfaced");
    CHECK_EQ_U(done.response_quadlet, 0x04040000u, "payload surfaced");
    CHECK_EQ_U(done.completion, FW_TXN_DONE_RESPONSE, "completion reason");
    CHECK(done.context == &marker, "caller context returned intact");
    CHECK_EQ_U(fw_txn_pending(&mgr), 0, "tlabel released");
}

/* tlabels must be unique while outstanding, and the pool must be finite. */
static void test_tlabel_allocation(void)
{
    fw_txn_mgr_t mgr;
    fw_txn_mgr_init(&mgr, LOCAL_NODE, 1, 0);

    bool seen[FW_MAX_TLABELS] = { false };
    int duplicates = 0;

    for (int i = 0; i < FW_MAX_TLABELS; i++) {
        fw_packet_header_t req;
        fw_err_t err = fw_txn_begin(&mgr, PEER_NODE,
                                    FW_TCODE_READ_QUADLET_REQUEST,
                                    FW_CONFIG_ROM_BASE, 0, 0, 0, NULL, &req);
        if (err != FW_OK)
            break;
        if (seen[req.tlabel])
            duplicates++;
        seen[req.tlabel] = true;
    }

    CHECK(duplicates == 0, "all 64 outstanding tlabels are distinct (%d dupes)",
          duplicates);
    CHECK_EQ_U(fw_txn_pending(&mgr), FW_MAX_TLABELS, "pool fully allocated");

    /* The 65th must be refused rather than silently reusing a label. */
    fw_packet_header_t overflow;
    CHECK_EQ_U(fw_txn_begin(&mgr, PEER_NODE, FW_TCODE_READ_QUADLET_REQUEST,
                            FW_CONFIG_ROM_BASE, 0, 0, 0, NULL, &overflow),
               FW_ERR_NOSPACE, "exhausted pool refuses new transactions");
}

/*
 * Releasing a label must not make it the immediate next pick. A slow responder
 * could still answer the old transaction, and that late reply would otherwise
 * be matched to the new one.
 */
static void test_tlabel_not_immediately_reused(void)
{
    fw_txn_mgr_t mgr;
    fw_txn_mgr_init(&mgr, LOCAL_NODE, 1, 0);

    fw_packet_header_t first;
    fw_txn_begin(&mgr, PEER_NODE, FW_TCODE_READ_QUADLET_REQUEST,
                 FW_CONFIG_ROM_BASE, 0, 0, 0, NULL, &first);
    uint8_t first_label = first.tlabel;

    fw_packet_header_t resp = make_response(&first, FW_RCODE_COMPLETE, 0);
    fw_txn_match_response(&mgr, &resp, NULL);

    fw_packet_header_t second;
    fw_txn_begin(&mgr, PEER_NODE, FW_TCODE_READ_QUADLET_REQUEST,
                 FW_CONFIG_ROM_BASE, 0, 0, 0, NULL, &second);

    CHECK(second.tlabel != first_label,
          "a freed tlabel (%u) is not handed straight back out (got %u)",
          first_label, second.tlabel);
}

/* Responses that do not belong to us, or to this request, must be dropped. */
static void test_response_validation(void)
{
    fw_txn_mgr_t mgr;
    fw_txn_mgr_init(&mgr, LOCAL_NODE, 1, 0);

    fw_packet_header_t req;
    fw_txn_begin(&mgr, PEER_NODE, FW_TCODE_READ_QUADLET_REQUEST,
                 FW_CONFIG_ROM_BASE, 0, 0, 0, NULL, &req);

    /* Right tlabel, wrong responder. */
    fw_packet_header_t imposter = make_response(&req, FW_RCODE_COMPLETE, 0xdead);
    imposter.source = OTHER_NODE;
    CHECK_EQ_U(fw_txn_match_response(&mgr, &imposter, NULL), FW_ERR_NOTFOUND,
               "response from the wrong node is rejected");

    /* Right tlabel, addressed to somebody else. */
    fw_packet_header_t misaddressed = make_response(&req, FW_RCODE_COMPLETE, 0);
    misaddressed.destination = OTHER_NODE;
    CHECK_EQ_U(fw_txn_match_response(&mgr, &misaddressed, NULL), FW_ERR_NOTFOUND,
               "response addressed elsewhere is rejected");

    /* Right tlabel, but the wrong kind of response for a quadlet read. */
    fw_packet_header_t wrong_kind = make_response(&req, FW_RCODE_COMPLETE, 0);
    wrong_kind.tcode = FW_TCODE_WRITE_RESPONSE;
    CHECK_EQ_U(fw_txn_match_response(&mgr, &wrong_kind, NULL), FW_ERR_NOTFOUND,
               "mismatched response tcode is rejected");

    /* A request is not a response. */
    CHECK_EQ_U(fw_txn_match_response(&mgr, &req, NULL), FW_ERR_INVAL,
               "a request packet is not accepted as a response");

    CHECK_EQ_U(fw_txn_pending(&mgr), 1,
               "the real transaction is still outstanding after all of that");

    /* The genuine article still works. */
    fw_packet_header_t good = make_response(&req, FW_RCODE_COMPLETE, 0x1234);
    CHECK_EQ_U(fw_txn_match_response(&mgr, &good, NULL), FW_OK,
               "the legitimate response still matches");

    /* Replaying it must not match a second time. */
    CHECK_EQ_U(fw_txn_match_response(&mgr, &good, NULL), FW_ERR_NOTFOUND,
               "a duplicated response is dropped");
}

static void test_split_timeout(void)
{
    fw_txn_mgr_t mgr;
    fw_txn_mgr_init(&mgr, LOCAL_NODE, 1, 1000 /* 1 ms */);

    fw_packet_header_t req;
    fw_txn_begin(&mgr, PEER_NODE, FW_TCODE_READ_QUADLET_REQUEST,
                 FW_CONFIG_ROM_BASE, 0, 0, 5000, NULL, &req);

    CHECK_EQ_U(fw_txn_expire(&mgr, 5500, NULL, 0), 0,
               "nothing expires before the deadline");
    CHECK_EQ_U(fw_txn_pending(&mgr), 1, "still outstanding");

    fw_txn_t expired[4];
    int n = fw_txn_expire(&mgr, 6000, expired, 4);

    CHECK_EQ_U(n, 1, "the transaction expires at its deadline");
    CHECK_EQ_U(expired[0].completion, FW_TXN_DONE_TIMEOUT, "completion reason");
    CHECK_EQ_U(fw_txn_pending(&mgr), 0, "tlabel reclaimed on timeout");

    /* A response arriving after the timeout must not resurrect it. */
    fw_packet_header_t late = make_response(&req, FW_RCODE_COMPLETE, 0xbeef);
    CHECK_EQ_U(fw_txn_match_response(&mgr, &late, NULL), FW_ERR_NOTFOUND,
               "a response arriving after timeout is dropped");
}

/*
 * The invariant that matters most. After a reset, node IDs are reassigned, so
 * a pre-reset response could arrive from a physically different device.
 */
static void test_bus_reset_aborts_everything(void)
{
    fw_txn_mgr_t mgr;
    fw_txn_mgr_init(&mgr, LOCAL_NODE, 7, 0);

    fw_packet_header_t reqs[3];
    for (int i = 0; i < 3; i++)
        fw_txn_begin(&mgr, PEER_NODE, FW_TCODE_READ_QUADLET_REQUEST,
                     FW_CONFIG_ROM_BASE, 0, 0, 0, NULL, &reqs[i]);

    CHECK_EQ_U(fw_txn_pending(&mgr), 3, "three transactions in flight");

    fw_txn_t aborted[8];
    int n = fw_txn_bus_reset(&mgr, 8, aborted, 8);

    CHECK_EQ_U(n, 3, "every in-flight transaction is aborted");
    CHECK_EQ_U(fw_txn_pending(&mgr), 0, "nothing left outstanding");
    CHECK_EQ_U(mgr.generation, 8, "generation advanced");
    CHECK_EQ_U(aborted[0].completion, FW_TXN_DONE_BUS_RESET, "completion reason");
    CHECK_EQ_U(aborted[0].generation, 7, "aborted entry records the old generation");

    /* A response for a pre-reset request must not be honoured, even though its
     * tlabel is now free and could be reallocated. */
    fw_packet_header_t stale = make_response(&reqs[0], FW_RCODE_COMPLETE, 0xbad);
    CHECK_EQ_U(fw_txn_match_response(&mgr, &stale, NULL), FW_ERR_NOTFOUND,
               "a pre-reset response cannot complete anything");

    CHECK_EQ_U(mgr.stat_bus_resets, 1, "bus reset counted");
}

static void test_argument_validation(void)
{
    fw_txn_mgr_t mgr;
    fw_txn_mgr_init(&mgr, LOCAL_NODE, 1, 0);
    fw_packet_header_t hdr;

    CHECK_EQ_U(fw_txn_begin(NULL, PEER_NODE, FW_TCODE_READ_QUADLET_REQUEST,
                            0, 0, 0, 0, NULL, &hdr), FW_ERR_INVAL,
               "NULL manager rejected");
    CHECK_EQ_U(fw_txn_begin(&mgr, PEER_NODE, FW_TCODE_READ_QUADLET_REQUEST,
                            0, 0, 0, 0, NULL, NULL), FW_ERR_INVAL,
               "NULL output header rejected");
    CHECK_EQ_U(fw_txn_begin(&mgr, PEER_NODE, FW_TCODE_READ_QUADLET_RESPONSE,
                            0, 0, 0, 0, NULL, &hdr), FW_ERR_INVAL,
               "a response tcode cannot start a transaction");
    CHECK_EQ_U(fw_txn_begin(&mgr, PEER_NODE, 0x3 /* reserved */,
                            0, 0, 0, 0, NULL, &hdr), FW_ERR_UNSUPPORTED,
               "a reserved tcode is refused");
    CHECK_EQ_U(fw_txn_pending(&mgr), 0,
               "no tlabel leaked by any rejected call");
}

void test_transaction(void)
{
    TEST_SUITE("fw_transaction");
    test_basic_round_trip();
    test_tlabel_allocation();
    test_tlabel_not_immediately_reused();
    test_response_validation();
    test_split_timeout();
    test_bus_reset_aborts_everything();
    test_argument_validation();
}
