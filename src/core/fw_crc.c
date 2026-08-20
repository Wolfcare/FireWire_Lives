#include "fw/fw_crc.h"

/*
 * Nibble-at-a-time CRC-16. Processing four bits per step keeps the inner loop
 * to eight iterations per quadlet without needing a lookup table, which
 * matters because this also runs inside the dext where we would rather not
 * carry static data.
 */
uint16_t fw_crc16(const fw_quadlet_t *quadlets, size_t length)
{
    uint32_t crc = 0;

    if (quadlets == NULL)
        return 0;

    for (size_t i = 0; i < length; i++) {
        uint32_t data = quadlets[i];

        for (int shift = 28; shift >= 0; shift -= 4) {
            uint32_t sum = ((crc >> 12) ^ (data >> shift)) & 0xf;
            crc = (crc << 4) ^ (sum << 12) ^ (sum << 5) ^ sum;
        }
        crc &= 0xffff;
    }

    return (uint16_t)crc;
}
