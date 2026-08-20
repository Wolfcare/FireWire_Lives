/*
 * fw_crc.h - CRC-16 as defined by IEEE 1394 for config ROM validation.
 */

#ifndef FW_CRC_H
#define FW_CRC_H

#include "fw_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Compute the IEEE 1394 CRC-16 over `length` quadlets.
 *
 * The polynomial is the ITU-T one, x^16 + x^12 + x^5 + 1, applied MSB-first
 * over each quadlet with a zero seed. `quadlets` must already be in host byte
 * order - callers reading a ROM off the wire byte-swap first.
 */
uint16_t fw_crc16(const fw_quadlet_t *quadlets, size_t length);

#ifdef __cplusplus
}
#endif

#endif /* FW_CRC_H */
