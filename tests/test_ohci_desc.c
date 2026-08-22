#include "test_util.h"
#include "fw/ohci_desc.h"
#include "fw/fw_packet.h"

static uint32_t le32_at(const uint8_t *p, size_t off)
{
    return (uint32_t)p[off] | ((uint32_t)p[off+1] << 8) |
           ((uint32_t)p[off+2] << 16) | ((uint32_t)p[off+3] << 24);
}

/* The transaction we actually want to send first: a quadlet read of node 0's
 * config ROM header. */
static void test_quadlet_read_descriptor(void)
{
    fw_packet_header_t hdr = {
        .tcode       = FW_TCODE_READ_QUADLET_REQUEST,
        .tlabel      = 0x01,
        .retry       = 1,
        .destination = 0xffc0,     /* node 0 */
        .source      = 0xffc1,     /* us */
        .offset      = FW_CONFIG_ROM_BASE,
    };

    fw_quadlet_t q[FW_MAX_HEADER_QUADLETS];
    int n = fw_packet_encode_header(&hdr, q, FW_MAX_HEADER_QUADLETS);
    CHECK_EQ_U(n, 3, "quadlet read header is 3 quadlets");

    uint8_t block[64] __attribute__((aligned(16)));
    int z = ohci_build_at_request(q, (size_t)n, 0, 0, block, sizeof(block));

    /* One descriptor unit + one immediate-data unit: a 3-quadlet header still
     * occupies a whole 16-byte unit. */
    CHECK_EQ_U(z, 2, "Z counts descriptor + immediate data units");

    uint32_t control = le32_at(block, 0);
    CHECK_EQ_U(control >> OHCI_DESC_CMD_SHIFT, OHCI_DESC_OUTPUT_LAST,
               "no payload means the header descriptor is OUTPUT_LAST");
    CHECK_EQ_U((control >> OHCI_DESC_KEY_SHIFT) & 0x7, OHCI_DESC_KEY_IMMEDIATE,
               "key marks the data as immediate");
    CHECK_EQ_U(control & 0xffff, 12, "reqCount is the header byte count");
    CHECK_EQ_U((control >> OHCI_DESC_INTERRUPT_SHIFT) & 0x3,
               OHCI_DESC_INTERRUPT_ALWAYS, "interrupt on completion");

    /* The header quadlets follow the descriptor, little-endian. */
    CHECK_EQ_U(le32_at(block, 16), q[0], "immediate quadlet 0");
    CHECK_EQ_U(le32_at(block, 20), q[1], "immediate quadlet 1");
    CHECK_EQ_U(le32_at(block, 24), q[2], "immediate quadlet 2");

    CHECK_EQ_U(le32_at(block, 12), 0, "status starts clear");
    CHECK(!ohci_desc_is_complete(block), "not complete until hardware writes status");
}

/* A block write carries a payload, so the block gains a second descriptor. */
static void test_block_write_descriptor(void)
{
    fw_packet_header_t hdr = {
        .tcode       = FW_TCODE_WRITE_BLOCK_REQUEST,
        .tlabel      = 0x02,
        .destination = 0xffc0,
        .source      = 0xffc1,
        .offset      = 0xfffff0001000ull,
        .data_length = 8,
    };

    fw_quadlet_t q[FW_MAX_HEADER_QUADLETS];
    int n = fw_packet_encode_header(&hdr, q, FW_MAX_HEADER_QUADLETS);
    CHECK_EQ_U(n, 4, "block write header is 4 quadlets");

    uint8_t block[96] __attribute__((aligned(16)));
    int z = ohci_build_at_request(q, (size_t)n, 0x80001000u, 8,
                                  block, sizeof(block));

    CHECK_EQ_U(z, 3, "descriptor + immediate unit + payload descriptor");

    uint32_t c0 = le32_at(block, 0);
    CHECK_EQ_U(c0 >> OHCI_DESC_CMD_SHIFT, OHCI_DESC_OUTPUT_MORE,
               "header descriptor is OUTPUT_MORE when a payload follows");

    /* The payload descriptor sits after the 16-byte immediate unit. */
    uint32_t c1 = le32_at(block, 32);
    CHECK_EQ_U(c1 >> OHCI_DESC_CMD_SHIFT, OHCI_DESC_OUTPUT_LAST,
               "payload descriptor terminates the block");
    CHECK_EQ_U((c1 >> OHCI_DESC_KEY_SHIFT) & 0x7, 0,
               "payload descriptor points at data rather than carrying it");
    CHECK_EQ_U(c1 & 0xffff, 8, "payload reqCount");
    CHECK_EQ_U(le32_at(block, 36), 0x80001000u, "payload DMA address");
}

static void test_receive_descriptor(void)
{
    uint8_t block[32] __attribute__((aligned(16)));
    int z = ohci_build_ar_buffer(0x80002000u, 4096, 0x80003001u,
                                 block, sizeof(block));

    CHECK_EQ_U(z, 1, "an AR buffer is a single descriptor unit");

    uint32_t c = le32_at(block, 0);
    CHECK_EQ_U(c >> OHCI_DESC_CMD_SHIFT, OHCI_DESC_INPUT_MORE, "INPUT_MORE");
    CHECK_EQ_U((c >> OHCI_DESC_BRANCH_SHIFT) & 0x3, OHCI_DESC_BRANCH_ALWAYS,
               "branch always, so the ring keeps running");
    CHECK_EQ_U(c & 0xffff, 4096, "buffer length");
    CHECK_EQ_U(le32_at(block, 4), 0x80002000u, "buffer DMA address");
    CHECK_EQ_U(le32_at(block, 8), 0x80003001u, "branch address carries its Z");
}

/*
 * Z shares the low four bits with the block address, which is why descriptor
 * blocks must be 16-byte aligned - otherwise the two would overlap.
 */
static void test_command_ptr_packing(void)
{
    CHECK_EQ_U(ohci_command_ptr(0x80001000u, 2), 0x80001002u,
               "address ORed with Z");
    CHECK_EQ_U(ohci_command_ptr(0x80001000u, 0), 0x80001000u, "Z of zero");
    CHECK_EQ_U(ohci_command_ptr(0x8000100fu, 3), 0x80001003u,
               "a misaligned address has its low bits replaced, not added");
}

static void test_status_readback(void)
{
    uint8_t block[32] __attribute__((aligned(16)));
    memset(block, 0, sizeof(block));

    CHECK(!ohci_desc_is_complete(block), "zero status means incomplete");

    /* Simulate the controller reporting ack_complete with 100 bytes unwritten. */
    uint32_t status = ((uint32_t)OHCI_ACK_COMPLETE << 16) | 100u;
    block[12] = (uint8_t)(status);
    block[13] = (uint8_t)(status >> 8);
    block[14] = (uint8_t)(status >> 16);
    block[15] = (uint8_t)(status >> 24);

    CHECK(ohci_desc_is_complete(block), "non-zero status means complete");
    CHECK_EQ_U(ohci_desc_xfer_status(block), OHCI_ACK_COMPLETE, "ack code");
    CHECK_EQ_U(ohci_desc_res_count(block), 100, "residual count");
    CHECK(ohci_ack_is_success(OHCI_ACK_COMPLETE), "ack_complete is success");
    CHECK(ohci_ack_is_success(OHCI_ACK_PENDING),
          "ack_pending is success - a split response follows");
    CHECK(!ohci_ack_is_success(OHCI_ACK_BUSY_X), "ack_busy is not success");
    CHECK_STR(ohci_event_name(OHCI_EVT_TIMEOUT), "timeout", "event name");
}

static void test_bounds(void)
{
    fw_quadlet_t q[4] = { 1, 2, 3, 4 };
    uint8_t small[16] __attribute__((aligned(16)));

    CHECK_EQ_U(ohci_build_at_request(q, 4, 0, 0, small, sizeof(small)),
               FW_ERR_NOSPACE, "undersized block refused");
    CHECK_EQ_U(ohci_build_at_request(NULL, 4, 0, 0, small, sizeof(small)),
               FW_ERR_INVAL, "NULL header refused");
    CHECK_EQ_U(ohci_build_at_request(q, 0, 0, 0, small, sizeof(small)),
               FW_ERR_INVAL, "zero-quadlet header refused");
    CHECK_EQ_U(ohci_build_at_request(q, 4, 0, 64, small, sizeof(small)),
               FW_ERR_INVAL, "payload length with no address refused");
    CHECK_EQ_U(ohci_build_ar_buffer(0, 4096, 0, small, sizeof(small)),
               FW_ERR_INVAL, "AR buffer with no address refused");
    CHECK_EQ_U(ohci_build_ar_buffer(0x1000, 0x10000, 0, small, sizeof(small)),
               FW_ERR_INVAL, "AR length beyond the reqCount field refused");
}

/*
 * OHCI's AT DMA header is not the 1394 wire header. Quadlet 0 carries speed
 * where the wire header carries destinationID, and quadlet 1 carries
 * destinationID where the wire header carries sourceID. Getting this wrong
 * addresses the packet to the wrong node and the controller rejects the
 * descriptor with evt_unknown - which is exactly what the hardware did.
 */
static void test_at_header_layout(void)
{
    fw_quadlet_t q[FW_MAX_HEADER_QUADLETS];
    int n = ohci_build_at_header(FW_TCODE_READ_QUADLET_REQUEST,
                                 0x05,          /* tlabel */
                                 1,             /* retry */
                                 FW_SPEED_S400,
                                 0xffc0,        /* node 0 */
                                 FW_CONFIG_ROM_BASE,
                                 0, 0, q, FW_MAX_HEADER_QUADLETS);

    CHECK_EQ_U(n, 3, "quadlet read is 3 quadlets");

    CHECK_EQ_U((q[0] >> 16) & 0x7, FW_SPEED_S400, "quadlet 0 carries speed");
    CHECK_EQ_U((q[0] >> 10) & 0x3f, 0x05, "quadlet 0 carries tlabel");
    CHECK_EQ_U((q[0] >> 8) & 0x3, 1, "quadlet 0 carries retry");
    CHECK_EQ_U((q[0] >> 4) & 0xf, FW_TCODE_READ_QUADLET_REQUEST,
               "quadlet 0 carries tcode");
    CHECK_EQ_U(q[0] >> 24, 0, "no destinationID in quadlet 0");

    CHECK_EQ_U(q[1] >> 16, 0xffc0, "quadlet 1 carries destinationID");
    CHECK_EQ_U(q[1] & 0xffff, 0xffff, "quadlet 1 carries offset high");
    CHECK_EQ_U(q[2], 0xf0000400u, "quadlet 2 carries offset low");

    /* The wire header would have put the destination in quadlet 0 and our own
     * node id in quadlet 1; assert we did not do that. */
    fw_packet_header_t wire = {
        .tcode = FW_TCODE_READ_QUADLET_REQUEST, .tlabel = 0x05, .retry = 1,
        .destination = 0xffc0, .source = 0xffc1, .offset = FW_CONFIG_ROM_BASE,
    };
    fw_quadlet_t w[FW_MAX_HEADER_QUADLETS];
    fw_packet_encode_header(&wire, w, FW_MAX_HEADER_QUADLETS);
    CHECK(q[0] != w[0], "OHCI quadlet 0 differs from the wire header");
    CHECK(q[1] != w[1], "OHCI quadlet 1 differs from the wire header");
}

static void test_at_header_block(void)
{
    fw_quadlet_t q[FW_MAX_HEADER_QUADLETS];
    int n = ohci_build_at_header(FW_TCODE_READ_BLOCK_REQUEST, 0x0a, 1,
                                 FW_SPEED_S800, 0xffc0,
                                 FW_CONFIG_ROM_BASE, 1024, 0,
                                 q, FW_MAX_HEADER_QUADLETS);
    CHECK_EQ_U(n, 4, "block read is 4 quadlets");
    CHECK_EQ_U((q[0] >> 16) & 0x7, FW_SPEED_S800, "S800 speed encoded");
    CHECK_EQ_U(q[3] >> 16, 1024, "quadlet 3 carries dataLength");
}

void test_ohci_desc(void)
{
    TEST_SUITE("ohci_desc");
    test_at_header_layout();
    test_at_header_block();
    test_quadlet_read_descriptor();
    test_block_write_descriptor();
    test_receive_descriptor();
    test_command_ptr_packing();
    test_status_readback();
    test_bounds();
}
