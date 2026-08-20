#include "test_util.h"
#include "fw/fw_sbp2.h"

/* Read a big-endian quadlet out of an encoded ORB, so assertions describe the
 * on-the-wire layout rather than any host representation. */
static uint32_t be32_at(const uint8_t *buf, size_t offset)
{
    return ((uint32_t)buf[offset] << 24) | ((uint32_t)buf[offset + 1] << 16) |
           ((uint32_t)buf[offset + 2] << 8) | (uint32_t)buf[offset + 3];
}

static void test_login_orb(void)
{
    sbp2_management_params_t p;
    memset(&p, 0, sizeof(p));
    p.function            = SBP2_FUNC_LOGIN;
    p.lun                 = 0;
    p.login_response_addr = 0xfffff0012000ull;
    p.login_response_size = SBP2_LOGIN_RESPONSE_SIZE;
    p.status_fifo_addr    = 0xfffff0013000ull;
    p.exclusive           = true;
    p.reconnect           = 4;      /* 2^4 = 16 seconds of reconnect hold */
    p.notify              = true;

    uint8_t orb[SBP2_MANAGEMENT_ORB_SIZE];
    int n = sbp2_encode_management_orb(&p, orb, sizeof(orb));

    CHECK_EQ_U(n, SBP2_MANAGEMENT_ORB_SIZE, "login ORB size");

    CHECK_EQ_U(be32_at(orb, 0), 0, "password high (unused)");
    CHECK_EQ_U(be32_at(orb, 4), 0, "password low (unused)");
    CHECK_EQ_U(be32_at(orb, 8), 0xffffu, "login response address high");
    CHECK_EQ_U(be32_at(orb, 12), 0xf0012000u, "login response address low");

    /* notify(31) | exclusive(28) | reconnect=4(20-23) | function=0(16-19) | lun=0 */
    uint32_t misc = be32_at(orb, 16);
    CHECK_EQ_U(misc >> 31, 1, "notify bit set");
    CHECK_EQ_U((misc >> 28) & 1, 1, "exclusive bit set");
    CHECK_EQ_U((misc >> 20) & 0xf, 4, "reconnect field");
    CHECK_EQ_U((misc >> 16) & 0xf, SBP2_FUNC_LOGIN, "function = LOGIN");
    CHECK_EQ_U(misc & 0xffff, 0, "logical unit number");

    CHECK_EQ_U(be32_at(orb, 20), SBP2_LOGIN_RESPONSE_SIZE,
               "response buffer length");
    CHECK_EQ_U(be32_at(orb, 24), 0xffffu, "status FIFO address high");
    CHECK_EQ_U(be32_at(orb, 28), 0xf0013000u, "status FIFO address low");
}

/*
 * LOGOUT and RECONNECT reuse the LUN field to carry the login_id from the
 * earlier LOGIN. Getting this wrong logs out the wrong session.
 */
static void test_logout_reuses_lun_field_for_login_id(void)
{
    sbp2_management_params_t p;
    memset(&p, 0, sizeof(p));
    p.function = SBP2_FUNC_LOGOUT;
    p.lun      = 0x0007;     /* must be ignored */
    p.login_id = 0x1234;
    p.notify   = true;

    uint8_t orb[SBP2_MANAGEMENT_ORB_SIZE];
    sbp2_encode_management_orb(&p, orb, sizeof(orb));

    uint32_t misc = be32_at(orb, 16);
    CHECK_EQ_U((misc >> 16) & 0xf, SBP2_FUNC_LOGOUT, "function = LOGOUT");
    CHECK_EQ_U(misc & 0xffff, 0x1234, "login_id occupies the low 16 bits");

    /* And RECONNECT behaves the same way. */
    p.function = SBP2_FUNC_RECONNECT;
    sbp2_encode_management_orb(&p, orb, sizeof(orb));
    CHECK_EQ_U(be32_at(orb, 16) & 0xffff, 0x1234,
               "RECONNECT also carries login_id there");

    /* A LOGIN, by contrast, must send the real LUN. */
    p.function = SBP2_FUNC_LOGIN;
    sbp2_encode_management_orb(&p, orb, sizeof(orb));
    CHECK_EQ_U(be32_at(orb, 16) & 0xffff, 0x0007,
               "LOGIN sends the logical unit number, not the login id");
}

static void test_login_response_parsing(void)
{
    /* A response as a target would write it: length 16, login_id 0x0003,
     * command block agent at 0xfffff0020000, reconnect hold 15 seconds. */
    uint8_t buf[SBP2_LOGIN_RESPONSE_SIZE] = {
        0x00, 0x10, 0x00, 0x03,     /* length | login_id */
        0x00, 0x00, 0xff, 0xff,     /* agent address high */
        0xf0, 0x02, 0x00, 0x00,     /* agent address low */
        0x00, 0x00, 0x00, 0x0f,     /* reconnect hold */
    };

    sbp2_login_response_t r;
    CHECK_EQ_U(sbp2_parse_login_response(buf, sizeof(buf), &r), FW_OK,
               "login response parses");
    CHECK_EQ_U(r.length, 0x10, "length");
    CHECK_EQ_U(r.login_id, 0x0003, "login id");
    CHECK_EQ_U(r.command_block_agent, 0xfffff0020000ull,
               "command block agent address");
    CHECK_EQ_U(r.reconnect_hold, 15, "reconnect hold seconds");

    CHECK_EQ_U(sbp2_parse_login_response(buf, 8, &r), FW_ERR_TOOSMALL,
               "a truncated login response is rejected");
    CHECK_EQ_U(sbp2_parse_login_response(NULL, 16, &r), FW_ERR_INVAL,
               "NULL buffer rejected");
}

/*
 * A SCSI INQUIRY, which is the exact command that will read the model and
 * serial number off the test drive. See docs/test-hardware.md.
 */
static void test_inquiry_command_orb(void)
{
    sbp2_command_params_t p;
    memset(&p, 0, sizeof(p));
    p.next_orb        = 0;                    /* end of chain */
    p.data_descriptor = 0xfffff0030000ull;
    p.data_size       = 36;                   /* standard INQUIRY reply */
    p.max_payload     = 9;                    /* 2^11 = 2048 bytes */
    p.speed           = FW_SPEED_S400;
    p.direction       = SBP2_DIR_FROM_TARGET; /* data flows target -> us */
    p.notify          = true;
    p.cdb_length      = 6;
    p.cdb[0] = 0x12;   /* INQUIRY */
    p.cdb[4] = 36;     /* allocation length */

    uint8_t orb[SBP2_COMMAND_ORB_SIZE];
    int n = sbp2_encode_command_orb(&p, orb, sizeof(orb));

    CHECK_EQ_U(n, SBP2_COMMAND_ORB_SIZE, "command ORB size");

    /* A null next-ORB is the MSB of the pointer, not an all-zero address:
     * address zero is a legal 1394 address. */
    CHECK_EQ_U(be32_at(orb, 0), 0x80000000u, "null next-ORB pointer");
    CHECK_EQ_U(be32_at(orb, 4), 0, "null pointer low quadlet");

    CHECK_EQ_U(be32_at(orb, 8), 0xffffu, "data descriptor high");
    CHECK_EQ_U(be32_at(orb, 12), 0xf0030000u, "data descriptor low");

    uint32_t misc = be32_at(orb, 16);
    CHECK_EQ_U(misc & 0xffff, 36, "data size");
    CHECK_EQ_U((misc >> 19) & 1, 0, "no page table for a single buffer");
    CHECK_EQ_U((misc >> 20) & 0xf, 9, "max payload");
    CHECK_EQ_U((misc >> 24) & 0x7, FW_SPEED_S400, "speed");
    CHECK_EQ_U((misc >> 27) & 1, 1, "direction bit set for a read");
    CHECK_EQ_U(misc >> 31, 1, "notify bit set");

    CHECK_EQ_U(orb[20], 0x12, "CDB byte 0 is INQUIRY");
    CHECK_EQ_U(orb[24], 36, "CDB allocation length");
}

static void test_write_direction_and_page_table(void)
{
    sbp2_command_params_t p;
    memset(&p, 0, sizeof(p));
    p.next_orb           = 0xfffff0040000ull;
    p.data_descriptor    = 0xfffff0050000ull;
    p.data_size          = 4;      /* four page table entries */
    p.page_table_present = true;
    p.page_size          = 0;
    p.direction          = SBP2_DIR_TO_TARGET;
    p.cdb_length         = 10;
    p.cdb[0] = 0x2a;   /* WRITE(10) */

    uint8_t orb[SBP2_COMMAND_ORB_SIZE];
    sbp2_encode_command_orb(&p, orb, sizeof(orb));

    /* A real next-ORB address must not be flagged null. */
    CHECK_EQ_U(be32_at(orb, 0), 0xffffu, "chained next-ORB high quadlet");
    CHECK_EQ_U(be32_at(orb, 4), 0xf0040000u, "chained next-ORB low quadlet");

    uint32_t misc = be32_at(orb, 16);
    CHECK_EQ_U((misc >> 19) & 1, 1, "page table present bit");
    CHECK_EQ_U((misc >> 27) & 1, 0, "direction bit clear for a write");
    CHECK_EQ_U(orb[20], 0x2a, "CDB byte 0 is WRITE(10)");
}

static void test_status_block(void)
{
    /* A normal completion: src=0, resp=0, dead=0, len=1, sbp_status=0,
     * referring to an ORB at 0xfffff0060000. */
    uint8_t ok[8] = {
        0x01, 0x00, 0xff, 0xff,
        0xf0, 0x06, 0x00, 0x00,
    };

    sbp2_status_t s;
    CHECK_EQ_U(sbp2_parse_status(ok, sizeof(ok), &s), FW_OK, "status parses");
    CHECK_EQ_U(s.resp, SBP2_RESP_REQUEST_COMPLETE, "response field");
    CHECK_EQ_U(s.dead, 0, "fetch agent alive");
    CHECK_EQ_U(s.len, 1, "length field");
    CHECK_EQ_U(s.sbp_status, 0, "sbp status");
    CHECK_EQ_U(s.orb_offset, 0xfffff0060000ull, "ORB this status refers to");
    CHECK(sbp2_status_is_success(&s), "classified as success");

    /* A dead fetch agent: the agent has stopped and needs an explicit reset
     * before it will accept further ORBs. */
    uint8_t dead[8] = { 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    CHECK_EQ_U(sbp2_parse_status(dead, sizeof(dead), &s), FW_OK,
               "dead status parses");
    CHECK(s.dead, "dead bit decoded");
    CHECK(!sbp2_status_is_success(&s), "a dead agent is not success");

    /* Transport failure. */
    uint8_t fail[8] = { 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    sbp2_parse_status(fail, sizeof(fail), &s);
    CHECK_EQ_U(s.resp, SBP2_RESP_TRANSPORT_FAILURE, "transport failure decoded");
    CHECK(!sbp2_status_is_success(&s), "transport failure is not success");
    CHECK_STR(sbp2_response_name(s.resp), "transport failure", "response name");
}

/* Sense data rides along in the status block; len counts header quadlets too. */
static void test_status_sense_data(void)
{
    uint8_t buf[SBP2_STATUS_SIZE];
    memset(buf, 0, sizeof(buf));
    buf[0] = 0x04;              /* len = 4 quadlets total -> 2 of payload */
    buf[2] = 0x00; buf[3] = 0x00;
    for (int i = 0; i < 8; i++)
        buf[8 + i] = (uint8_t)(0xa0 + i);

    sbp2_status_t s;
    CHECK_EQ_U(sbp2_parse_status(buf, sizeof(buf), &s), FW_OK,
               "status with sense data parses");
    CHECK_EQ_U(s.len, 4, "len field");
    CHECK_EQ_U(s.data_length, 8, "two payload quadlets extracted");
    CHECK_EQ_U(s.data[0], 0xa0, "first sense byte");
    CHECK_EQ_U(s.data[7], 0xa7, "last sense byte");

    /* A target claiming more payload than it actually wrote must not cause an
     * overread of the caller's buffer. */
    uint8_t liar[12];
    memset(liar, 0, sizeof(liar));
    liar[0] = 0x07;             /* claims 7 quadlets, only 1 present */
    CHECK_EQ_U(sbp2_parse_status(liar, sizeof(liar), &s), FW_OK,
               "an over-claiming status block still parses");
    CHECK_EQ_U(s.data_length, 4,
               "payload is clamped to what the buffer actually holds");
}

static void test_encoding_bounds(void)
{
    sbp2_management_params_t m;
    memset(&m, 0, sizeof(m));
    uint8_t small[16];

    CHECK_EQ_U(sbp2_encode_management_orb(&m, small, sizeof(small)),
               FW_ERR_NOSPACE, "management ORB refuses an undersized buffer");
    CHECK_EQ_U(sbp2_encode_management_orb(NULL, small, sizeof(small)),
               FW_ERR_INVAL, "NULL params rejected");

    sbp2_command_params_t c;
    memset(&c, 0, sizeof(c));
    uint8_t orb[SBP2_COMMAND_ORB_SIZE];

    CHECK_EQ_U(sbp2_encode_command_orb(&c, small, sizeof(small)),
               FW_ERR_NOSPACE, "command ORB refuses an undersized buffer");

    /* A CDB longer than the inline space would overrun the ORB. */
    c.cdb_length = SBP2_MAX_CDB_SIZE + 1;
    CHECK_EQ_U(sbp2_encode_command_orb(&c, orb, sizeof(orb)), FW_ERR_INVAL,
               "an over-long CDB is refused rather than truncated");
}

void test_sbp2(void)
{
    TEST_SUITE("fw_sbp2");
    test_login_orb();
    test_logout_reuses_lun_field_for_login_id();
    test_login_response_parsing();
    test_inquiry_command_orb();
    test_write_direction_and_page_table();
    test_status_block();
    test_status_sense_data();
    test_encoding_bounds();
}
