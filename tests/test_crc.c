#include "test_util.h"
#include "fw/fw_crc.h"

/*
 * Independent reference implementation: bit-at-a-time CRC-16 with the ITU
 * polynomial x^16 + x^12 + x^5 + 1, MSB-first over each quadlet, zero seed.
 *
 * fw_crc16() processes four bits per step for speed. Cross-checking the two
 * over a large pseudo-random corpus is what proves the fast version is right,
 * rather than merely self-consistent.
 */
static uint16_t crc16_reference(const uint32_t *quadlets, size_t length)
{
    uint32_t crc = 0;

    for (size_t i = 0; i < length; i++) {
        uint32_t data = quadlets[i];
        for (int bit = 31; bit >= 0; bit--) {
            uint32_t msb = (crc >> 15) & 1u;
            uint32_t in  = (data >> bit) & 1u;
            crc = (crc << 1) & 0xffffu;
            if (msb ^ in)
                crc ^= 0x1021u;
        }
    }

    return (uint16_t)crc;
}

/* xorshift32 - deterministic, so a failure is always reproducible. */
static uint32_t rng_state = 0x1394f00du;
static uint32_t next_rand(void)
{
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}

void test_crc(void)
{
    TEST_SUITE("fw_crc16");

    /* Degenerate cases. */
    CHECK_EQ_U(fw_crc16(NULL, 0), 0, "crc of NULL");
    {
        uint32_t zero = 0;
        CHECK_EQ_U(fw_crc16(&zero, 0), 0, "crc over zero quadlets");
        CHECK_EQ_U(fw_crc16(&zero, 1), crc16_reference(&zero, 1),
                   "crc of a single zero quadlet");
    }

    /* Cross-check against the reference over random data of varying length. */
    int mismatches = 0;
    uint32_t buf[64];

    for (int trial = 0; trial < 2000; trial++) {
        size_t len = (size_t)(next_rand() % 64u) + 1u;
        for (size_t i = 0; i < len; i++)
            buf[i] = next_rand();

        if (fw_crc16(buf, len) != crc16_reference(buf, len))
            mismatches++;
    }

    CHECK(mismatches == 0,
          "nibble-wise CRC matches bitwise reference over 2000 random vectors "
          "(%d mismatches)", mismatches);

    /* The CRC must actually detect corruption: flipping any single bit in a
     * payload should change the result. */
    uint32_t payload[4] = { 0x31333934u, 0xf0006400u, 0x08002700u, 0x12345678u };
    uint16_t good = fw_crc16(payload, 4);
    int undetected = 0;

    for (int q = 0; q < 4; q++) {
        for (int b = 0; b < 32; b++) {
            uint32_t saved = payload[q];
            payload[q] ^= (1u << b);
            if (fw_crc16(payload, 4) == good)
                undetected++;
            payload[q] = saved;
        }
    }

    CHECK(undetected == 0,
          "every single-bit flip across 128 positions changes the CRC "
          "(%d undetected)", undetected);
}
