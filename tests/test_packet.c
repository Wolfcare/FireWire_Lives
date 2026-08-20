#include "test_util.h"
#include "fw/fw_packet.h"

/*
 * The canonical transaction the stack performs first: a quadlet read of the
 * config ROM header at 0xfffff0000400 from node 0xffc0 to node 0xffc1.
 */
static void test_read_quadlet_request(void)
{
    fw_packet_header_t hdr = {
        .tcode       = FW_TCODE_READ_QUADLET_REQUEST,
        .tlabel      = 0x2a,
        .retry       = 1,
        .priority    = 0,
        .destination = 0xffc1,
        .source      = 0xffc0,
        .offset      = FW_CONFIG_ROM_BASE,
    };

    fw_quadlet_t q[FW_MAX_HEADER_QUADLETS];
    int n = fw_packet_encode_header(&hdr, q, FW_MAX_HEADER_QUADLETS);

    CHECK_EQ_U(n, 3, "read_quadlet_request header length (quadlets)");

    /* dest=0xffc1, tlabel=0x2a (<<10), rt=1 (<<8), tcode=4 (<<4), pri=0 */
    CHECK_EQ_U(q[0], 0xffc1a940u, "quadlet 0 (dest|tlabel|rt|tcode)");
    /* source=0xffc0, offset high 16 = 0xffff */
    CHECK_EQ_U(q[1], 0xffc0ffffu, "quadlet 1 (source|offset_hi)");
    CHECK_EQ_U(q[2], 0xf0000400u, "quadlet 2 (offset_lo)");

    /* Round-trip must reproduce every field. */
    fw_packet_header_t back;
    int m = fw_packet_decode_header(q, (size_t)n, &back);

    CHECK_EQ_U(m, n, "decode consumes what encode produced");
    CHECK_EQ_U(back.tcode, hdr.tcode, "round-trip tcode");
    CHECK_EQ_U(back.tlabel, hdr.tlabel, "round-trip tlabel");
    CHECK_EQ_U(back.retry, hdr.retry, "round-trip retry");
    CHECK_EQ_U(back.destination, hdr.destination, "round-trip destination");
    CHECK_EQ_U(back.source, hdr.source, "round-trip source");
    CHECK_EQ_U(back.offset, hdr.offset, "round-trip 48-bit offset");
}

static void test_block_request(void)
{
    fw_packet_header_t hdr = {
        .tcode       = FW_TCODE_READ_BLOCK_REQUEST,
        .tlabel      = 0x05,
        .destination = 0xffc2,
        .source      = 0xffc0,
        .offset      = FW_CONFIG_ROM_BASE + 0x20,
        .data_length = 256,
    };

    fw_quadlet_t q[FW_MAX_HEADER_QUADLETS];
    int n = fw_packet_encode_header(&hdr, q, FW_MAX_HEADER_QUADLETS);

    CHECK_EQ_U(n, 4, "read_block_request header length");
    CHECK_EQ_U(q[3] >> 16, 256, "data_length field");

    fw_packet_header_t back;
    fw_packet_decode_header(q, (size_t)n, &back);
    CHECK_EQ_U(back.data_length, 256, "round-trip data_length");
    CHECK_EQ_U(back.offset, hdr.offset, "round-trip block offset");
}

static void test_response(void)
{
    fw_packet_header_t hdr = {
        .tcode       = FW_TCODE_READ_QUADLET_RESPONSE,
        .tlabel      = 0x2a,
        .destination = 0xffc0,
        .source      = 0xffc1,
        .rcode       = FW_RCODE_COMPLETE,
        .quadlet_data = 0x0404b8f5u,
    };

    fw_quadlet_t q[FW_MAX_HEADER_QUADLETS];
    int n = fw_packet_encode_header(&hdr, q, FW_MAX_HEADER_QUADLETS);

    CHECK_EQ_U(n, 4, "read_quadlet_response header length");

    fw_packet_header_t back;
    fw_packet_decode_header(q, (size_t)n, &back);

    CHECK_EQ_U(back.rcode, FW_RCODE_COMPLETE, "round-trip rcode");
    CHECK_EQ_U(back.quadlet_data, 0x0404b8f5u, "round-trip payload quadlet");
    CHECK(fw_packet_is_response(back.tcode), "tcode classified as a response");

    /* A response must not be mistaken for carrying an offset. */
    CHECK_EQ_U(back.offset, 0, "response carries no offset");
}

static void test_classification_and_errors(void)
{
    CHECK_EQ_U(fw_packet_response_tcode(FW_TCODE_READ_BLOCK_REQUEST),
               FW_TCODE_READ_BLOCK_RESPONSE, "read block -> read block response");
    CHECK_EQ_U(fw_packet_response_tcode(FW_TCODE_WRITE_BLOCK_REQUEST),
               FW_TCODE_WRITE_RESPONSE, "write block -> write response");
    CHECK_EQ_U(fw_packet_response_tcode(FW_TCODE_LOCK_REQUEST),
               FW_TCODE_LOCK_RESPONSE, "lock -> lock response");

    CHECK(fw_packet_has_block_payload(FW_TCODE_WRITE_BLOCK_REQUEST),
          "write_block_request carries a payload");
    CHECK(!fw_packet_has_block_payload(FW_TCODE_READ_BLOCK_REQUEST),
          "read_block_request carries no payload");

    /* Bounds and argument checking. */
    fw_packet_header_t hdr = { .tcode = FW_TCODE_READ_BLOCK_REQUEST };
    fw_quadlet_t small[2];
    CHECK(fw_packet_encode_header(&hdr, small, 2) == FW_ERR_NOSPACE,
          "encode rejects an undersized output buffer");
    CHECK(fw_packet_encode_header(NULL, small, 2) == FW_ERR_INVAL,
          "encode rejects a NULL header");

    fw_quadlet_t truncated[1] = { 0xffc10050u };  /* tcode 5, needs 4 quadlets */
    fw_packet_header_t out;
    CHECK(fw_packet_decode_header(truncated, 1, &out) == FW_ERR_TOOSMALL,
          "decode rejects a truncated header");

    /* Reserved tcode 0x3 has no defined header length. */
    fw_quadlet_t reserved[4] = { 0x00000030u, 0, 0, 0 };
    CHECK(fw_packet_decode_header(reserved, 4, &out) == FW_ERR_UNSUPPORTED,
          "decode rejects a reserved tcode");
}

void test_packet(void)
{
    TEST_SUITE("fw_packet");
    test_read_quadlet_request();
    test_block_request();
    test_response();
    test_classification_and_errors();
}
