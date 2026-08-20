/*
 * fw_packet.h - Asynchronous packet header encode/decode.
 *
 * Async packets carry every non-streaming transaction on the bus: config ROM
 * reads, SBP-2 ORB pointers, AV/C commands, audio register pokes. The OHCI
 * controller wants these headers as quadlets in a DMA descriptor, so this
 * layer deals purely in quadlet arrays and leaves DMA to the dext.
 */

#ifndef FW_PACKET_H
#define FW_PACKET_H

#include "fw_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A decoded async packet header. Payload is referenced, never copied. */
typedef struct {
    uint8_t     tcode;
    uint8_t     tlabel;       /* transaction label, matches response to request */
    uint8_t     retry;
    uint8_t     priority;
    fw_nodeid_t destination;
    fw_nodeid_t source;
    fw_offset_t offset;       /* 48-bit destination offset (requests only) */
    uint16_t    data_length;  /* block payload length in bytes */
    uint16_t    extended_tcode;
    uint8_t     rcode;        /* responses only */
    fw_quadlet_t quadlet_data; /* quadlet request/response payload */
} fw_packet_header_t;

/* Maximum async header size in quadlets (block request: 4 header + CRC). */
#define FW_MAX_HEADER_QUADLETS 4

/*
 * Encode a header into `out`, which must have room for at least
 * FW_MAX_HEADER_QUADLETS quadlets.
 *
 * Returns the number of quadlets written, or a negative fw_err_t. The header
 * CRC quadlet is not emitted: OHCI generates it in hardware.
 */
int fw_packet_encode_header(const fw_packet_header_t *hdr, fw_quadlet_t *out,
                            size_t out_quadlets);

/*
 * Decode a header from `in`. Returns the number of quadlets consumed, or a
 * negative fw_err_t if the buffer is too short for the tcode's header.
 */
int fw_packet_decode_header(const fw_quadlet_t *in, size_t in_quadlets,
                            fw_packet_header_t *hdr);

/* How many header quadlets a given tcode uses. */
int fw_packet_header_quadlets(uint8_t tcode);

/* True if this tcode is a response rather than a request. */
bool fw_packet_is_response(uint8_t tcode);

/* True if this tcode carries a block payload after the header. */
bool fw_packet_has_block_payload(uint8_t tcode);

/* The tcode of the response that answers this request. */
int fw_packet_response_tcode(uint8_t request_tcode);

const char *fw_tcode_name(uint8_t tcode);
const char *fw_rcode_name(uint8_t rcode);

#ifdef __cplusplus
}
#endif

#endif /* FW_PACKET_H */
