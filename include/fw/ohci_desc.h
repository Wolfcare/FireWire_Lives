/*
 * ohci_desc.h - OHCI DMA descriptor construction.
 *
 * The controller does not take packets directly; it walks linked lists of
 * 16-byte descriptors in host memory, called descriptor blocks. Each context
 * (async transmit, async receive, isochronous) has a CommandPtr register
 * holding the address of the next block plus a Z value counting the 16-byte
 * units in it.
 *
 * Building these correctly is fiddly and unforgiving: a wrong Z value or a
 * misplaced branch address makes the controller walk into arbitrary memory.
 * That is why this layer is portable and unit-tested rather than living in the
 * dext where it could only be exercised against hardware.
 *
 * All descriptor fields are little-endian, matching the controller's view of
 * host memory on both x86 and Apple Silicon.
 */

#ifndef OHCI_DESC_H
#define OHCI_DESC_H

#include "fw_types.h"
#include "ohci_regs.h"
#include "fw_packet.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A descriptor block address is handed to the controller as address | Z. */
#define OHCI_Z_MASK 0xfu

/*
 * Maximum descriptors in one async transmit block: an immediate descriptor
 * carrying the packet header, plus one pointing at the payload.
 */
#define OHCI_AT_MAX_DESCRIPTORS 2

/* An immediate descriptor carries at most 8 quadlets inline. */
#define OHCI_IMMEDIATE_MAX_BYTES 32

/*
 * Build the packet header OHCI's async transmit DMA expects.
 *
 * This is NOT the IEEE 1394 wire header. The controller synthesises the wire
 * header itself - notably it fills in sourceID - and expects a different
 * layout in the descriptor's immediate data (OHCI 1.1 figure 7-1):
 *
 *   quadlet 0:  srcBusID(23) | spd(18:16) | tLabel(15:10) | rt(9:8) | tCode(7:4)
 *   quadlet 1:  destinationID(31:16) | destinationOffsetHigh(15:0)
 *   quadlet 2:  destinationOffsetLow
 *   quadlet 3:  dataLength(31:16) | extendedTcode(15:0)     [block only]
 *
 * The trap is that quadlet 0 of the wire header carries destinationID where
 * OHCI carries speed, and quadlet 1 carries sourceID where OHCI carries
 * destinationID. Feeding a wire header to the AT context produces a packet
 * addressed to the wrong node at the wrong speed, and the controller rejects
 * the descriptor with evt_unknown.
 *
 * Returns the number of quadlets written, or a negative fw_err_t.
 */
int ohci_build_at_header(uint8_t tcode, uint8_t tlabel, uint8_t retry,
                         uint8_t speed, fw_nodeid_t destination,
                         fw_offset_t offset, uint16_t data_length,
                         uint16_t extended_tcode,
                         fw_quadlet_t *out, size_t out_quadlets);

/*
 * Build an asynchronous transmit descriptor block for one request.
 *
 * `header` holds the async packet header quadlets already encoded by
 * fw_packet_encode_header(), in host byte order; they are written into the
 * descriptor block as immediate data.
 *
 * `payload_address` is the DMA address (an IOVA under the DART) of the block
 * payload, or 0 when the request carries none. `payload_length` is in bytes.
 *
 * `block` receives the descriptor block and must be 16-byte aligned with room
 * for `block_size` bytes. The block is self-contained: the immediate header
 * data is copied in, so the caller need not keep `header` alive.
 *
 * On success returns the Z value - the number of 16-byte units in the block,
 * which the caller ORs into CommandPtr or a branch address. On failure
 * returns a negative fw_err_t.
 */
int ohci_build_at_request(const fw_quadlet_t *header, size_t header_quadlets,
                          uint32_t payload_address, uint32_t payload_length,
                          void *block, size_t block_size);

/*
 * Build an asynchronous receive descriptor: a single INPUT_MORE pointing at a
 * buffer the controller fills with incoming packets.
 *
 * `buffer_address` is the DMA address of the receive buffer and
 * `buffer_length` its size in bytes (max 65535, the reqCount field width).
 * `branch_address` is the DMA address of the next descriptor block, already
 * ORed with its Z value, or 0 to terminate the chain.
 *
 * Returns the Z value (always 1) or a negative fw_err_t.
 */
int ohci_build_ar_buffer(uint32_t buffer_address, uint32_t buffer_length,
                         uint32_t branch_address,
                         void *block, size_t block_size);

/*
 * Read back how much of an AR buffer the controller filled.
 *
 * The status quadlet holds xferStatus in its high half and resCount - the
 * number of bytes still unwritten - in its low half. Bytes received is
 * therefore reqCount minus resCount.
 */
uint16_t ohci_desc_res_count(const void *block);
uint16_t ohci_desc_xfer_status(const void *block);

/* True once the controller has written a completion status into a descriptor. */
bool ohci_desc_is_complete(const void *block);

/*
 * Combine a descriptor block address with its Z value for CommandPtr or a
 * branch address. The address must be 16-byte aligned; the low four bits carry
 * Z, which is why alignment is not merely a performance concern.
 */
uint32_t ohci_command_ptr(uint32_t block_address, int z);

/* Event codes reported in xferStatus, for turning failures into diagnostics. */
#define OHCI_EVT_NO_STATUS      0x00
#define OHCI_EVT_LONG_PACKET    0x02
#define OHCI_EVT_MISSING_ACK    0x03
#define OHCI_EVT_UNDERRUN       0x04
#define OHCI_EVT_OVERRUN        0x05
#define OHCI_EVT_DESCRIPTOR_READ 0x06
#define OHCI_EVT_DATA_READ      0x07
#define OHCI_EVT_DATA_WRITE     0x08
#define OHCI_EVT_BUS_RESET      0x09
#define OHCI_EVT_TIMEOUT        0x0a
#define OHCI_EVT_TCODE_ERR      0x0b
#define OHCI_EVT_UNKNOWN        0x0e
#define OHCI_EVT_FLUSHED        0x0f
#define OHCI_ACK_COMPLETE       0x11
#define OHCI_ACK_PENDING        0x12
#define OHCI_ACK_BUSY_X         0x14
#define OHCI_ACK_BUSY_A         0x15
#define OHCI_ACK_BUSY_B         0x16
#define OHCI_ACK_TARDY          0x1b
#define OHCI_ACK_DATA_ERROR     0x1d
#define OHCI_ACK_TYPE_ERROR     0x1e

const char *ohci_event_name(uint8_t event);

/* True when an ack code means the packet reached the target intact. */
bool ohci_ack_is_success(uint8_t ack);

#ifdef __cplusplus
}
#endif

#endif /* OHCI_DESC_H */
