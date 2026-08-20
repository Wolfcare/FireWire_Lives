/*
 * fw_types.h - Base types and constants for the IEEE 1394 stack.
 *
 * This header is freestanding: it depends only on the C standard library so
 * that the protocol core can be compiled both into the PCIDriverKit dext and
 * into host-side unit tests.
 */

#ifndef FW_TYPES_H
#define FW_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A quadlet is the fundamental 32-bit unit of IEEE 1394. Everything on the
 * bus - config ROM, self-ID packets, async headers - is quadlet based and
 * transmitted big-endian. */
typedef uint32_t fw_quadlet_t;

/* 48-bit offset within a node's address space, carried in async packets. */
typedef uint64_t fw_offset_t;

/* Node IDs are 16 bits: 10-bit bus_id followed by 6-bit phy_id. */
typedef uint16_t fw_nodeid_t;

#define FW_NODE_ID_BUS_SHIFT    6
#define FW_NODE_ID_PHY_MASK     0x3fu
#define FW_LOCAL_BUS_ID         0x3ffu   /* "this bus" */
#define FW_MAX_NODES            63u      /* phy_id 63 is the broadcast ID */
#define FW_BROADCAST_PHY_ID     63u

/* Speed codes as they appear in self-ID packets and OHCI descriptors. */
typedef enum {
    FW_SPEED_S100 = 0,
    FW_SPEED_S200 = 1,
    FW_SPEED_S400 = 2,
    FW_SPEED_S800 = 3,
    FW_SPEED_S1600 = 4,
    FW_SPEED_S3200 = 5,
} fw_speed_t;

/* Transaction codes (IEEE 1394-2008 table 6-9). */
typedef enum {
    FW_TCODE_WRITE_QUADLET_REQUEST  = 0x0,
    FW_TCODE_WRITE_BLOCK_REQUEST    = 0x1,
    FW_TCODE_WRITE_RESPONSE         = 0x2,
    FW_TCODE_READ_QUADLET_REQUEST   = 0x4,
    FW_TCODE_READ_BLOCK_REQUEST     = 0x5,
    FW_TCODE_READ_QUADLET_RESPONSE  = 0x6,
    FW_TCODE_READ_BLOCK_RESPONSE    = 0x7,
    FW_TCODE_CYCLE_START            = 0x8,
    FW_TCODE_LOCK_REQUEST           = 0x9,
    FW_TCODE_STREAM_DATA            = 0xa,
    FW_TCODE_LOCK_RESPONSE          = 0xb,
} fw_tcode_t;

/* Response codes returned in the rcode field of a response packet. */
typedef enum {
    FW_RCODE_COMPLETE       = 0x0,
    FW_RCODE_CONFLICT_ERROR = 0x4,
    FW_RCODE_DATA_ERROR     = 0x5,
    FW_RCODE_TYPE_ERROR     = 0x6,
    FW_RCODE_ADDRESS_ERROR  = 0x7,
} fw_rcode_t;

/* Stack-internal error codes. Negative so they never collide with a byte
 * count returned from the same function. */
typedef enum {
    FW_OK               =  0,
    FW_ERR_INVAL        = -1,   /* caller passed nonsense */
    FW_ERR_TOOSMALL     = -2,   /* buffer ran out mid-structure */
    FW_ERR_CRC          = -3,   /* CRC mismatch */
    FW_ERR_NOTFOUND     = -4,   /* key or entry absent */
    FW_ERR_UNSUPPORTED  = -5,   /* valid but we do not implement it */
    FW_ERR_TOPOLOGY     = -6,   /* self-ID set is internally inconsistent */
    FW_ERR_NOSPACE      = -7,   /* caller's output array is too small */
} fw_err_t;

/* The CSR register space lives at this fixed base in every node's address
 * space; the config ROM begins one kilobyte further in. */
#define FW_CSR_REGISTER_BASE    0xfffff0000000ull
#define FW_CONFIG_ROM_BASE      (FW_CSR_REGISTER_BASE + 0x400)

#ifdef __cplusplus
}
#endif

#endif /* FW_TYPES_H */
