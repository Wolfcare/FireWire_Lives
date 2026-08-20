#include "fw/fw_packet.h"

#include <string.h>

/*
 * Async header layout (IEEE 1394-2008 clause 6.2). Quadlet 0 is common to
 * every tcode:
 *
 *   31..16       | 15..10 | 9 8 | 7..4  | 3..0
 *   destination  | tlabel | rt  | tcode | priority
 *
 * Quadlet 1 carries the source ID in its top half; the bottom half is either
 * the high 16 bits of a 48-bit offset (requests) or the rcode (responses).
 */

int fw_packet_header_quadlets(uint8_t tcode)
{
    switch (tcode) {
    case FW_TCODE_WRITE_QUADLET_REQUEST:
    case FW_TCODE_READ_BLOCK_REQUEST:
    case FW_TCODE_READ_QUADLET_RESPONSE:
        return 4;
    case FW_TCODE_WRITE_BLOCK_REQUEST:
    case FW_TCODE_LOCK_REQUEST:
    case FW_TCODE_READ_BLOCK_RESPONSE:
    case FW_TCODE_LOCK_RESPONSE:
        return 4;
    case FW_TCODE_READ_QUADLET_REQUEST:
        return 3;
    case FW_TCODE_WRITE_RESPONSE:
        return 3;
    default:
        return FW_ERR_UNSUPPORTED;
    }
}

bool fw_packet_is_response(uint8_t tcode)
{
    return tcode == FW_TCODE_WRITE_RESPONSE ||
           tcode == FW_TCODE_READ_QUADLET_RESPONSE ||
           tcode == FW_TCODE_READ_BLOCK_RESPONSE ||
           tcode == FW_TCODE_LOCK_RESPONSE;
}

bool fw_packet_has_block_payload(uint8_t tcode)
{
    return tcode == FW_TCODE_WRITE_BLOCK_REQUEST ||
           tcode == FW_TCODE_READ_BLOCK_RESPONSE ||
           tcode == FW_TCODE_LOCK_REQUEST ||
           tcode == FW_TCODE_LOCK_RESPONSE;
}

int fw_packet_response_tcode(uint8_t request_tcode)
{
    switch (request_tcode) {
    case FW_TCODE_WRITE_QUADLET_REQUEST:
    case FW_TCODE_WRITE_BLOCK_REQUEST:
        return FW_TCODE_WRITE_RESPONSE;
    case FW_TCODE_READ_QUADLET_REQUEST:
        return FW_TCODE_READ_QUADLET_RESPONSE;
    case FW_TCODE_READ_BLOCK_REQUEST:
        return FW_TCODE_READ_BLOCK_RESPONSE;
    case FW_TCODE_LOCK_REQUEST:
        return FW_TCODE_LOCK_RESPONSE;
    default:
        return FW_ERR_UNSUPPORTED;
    }
}

int fw_packet_encode_header(const fw_packet_header_t *hdr, fw_quadlet_t *out,
                            size_t out_quadlets)
{
    if (hdr == NULL || out == NULL)
        return FW_ERR_INVAL;

    int n = fw_packet_header_quadlets(hdr->tcode);
    if (n < 0)
        return n;
    if (out_quadlets < (size_t)n)
        return FW_ERR_NOSPACE;

    memset(out, 0, (size_t)n * sizeof(*out));

    out[0] = ((uint32_t)hdr->destination << 16) |
             ((uint32_t)(hdr->tlabel & 0x3fu) << 10) |
             ((uint32_t)(hdr->retry & 0x3u) << 8) |
             ((uint32_t)(hdr->tcode & 0xfu) << 4) |
             (uint32_t)(hdr->priority & 0xfu);

    if (fw_packet_is_response(hdr->tcode)) {
        out[1] = ((uint32_t)hdr->source << 16) |
                 ((uint32_t)(hdr->rcode & 0xfu) << 12);
    } else {
        out[1] = ((uint32_t)hdr->source << 16) |
                 (uint32_t)((hdr->offset >> 32) & 0xffffu);
        out[2] = (uint32_t)(hdr->offset & 0xffffffffu);
    }

    switch (hdr->tcode) {
    case FW_TCODE_WRITE_QUADLET_REQUEST:
        out[3] = hdr->quadlet_data;
        break;

    case FW_TCODE_READ_QUADLET_RESPONSE:
        /* Responses have no offset quadlet, so the payload lands in [3] with
         * [2] reserved. */
        out[2] = 0;
        out[3] = hdr->quadlet_data;
        break;

    case FW_TCODE_READ_BLOCK_REQUEST:
    case FW_TCODE_WRITE_BLOCK_REQUEST:
    case FW_TCODE_LOCK_REQUEST:
        out[3] = ((uint32_t)hdr->data_length << 16) |
                 (uint32_t)hdr->extended_tcode;
        break;

    case FW_TCODE_READ_BLOCK_RESPONSE:
    case FW_TCODE_LOCK_RESPONSE:
        out[2] = 0;
        out[3] = ((uint32_t)hdr->data_length << 16) |
                 (uint32_t)hdr->extended_tcode;
        break;

    default:
        break;
    }

    return n;
}

int fw_packet_decode_header(const fw_quadlet_t *in, size_t in_quadlets,
                            fw_packet_header_t *hdr)
{
    if (in == NULL || hdr == NULL)
        return FW_ERR_INVAL;
    if (in_quadlets < 1)
        return FW_ERR_TOOSMALL;

    memset(hdr, 0, sizeof(*hdr));

    hdr->destination = (fw_nodeid_t)(in[0] >> 16);
    hdr->tlabel      = (uint8_t)((in[0] >> 10) & 0x3fu);
    hdr->retry       = (uint8_t)((in[0] >> 8) & 0x3u);
    hdr->tcode       = (uint8_t)((in[0] >> 4) & 0xfu);
    hdr->priority    = (uint8_t)(in[0] & 0xfu);

    int n = fw_packet_header_quadlets(hdr->tcode);
    if (n < 0)
        return n;
    if (in_quadlets < (size_t)n)
        return FW_ERR_TOOSMALL;

    hdr->source = (fw_nodeid_t)(in[1] >> 16);

    if (fw_packet_is_response(hdr->tcode)) {
        hdr->rcode = (uint8_t)((in[1] >> 12) & 0xfu);
    } else {
        hdr->offset = ((uint64_t)(in[1] & 0xffffu) << 32) | (uint64_t)in[2];
    }

    switch (hdr->tcode) {
    case FW_TCODE_WRITE_QUADLET_REQUEST:
    case FW_TCODE_READ_QUADLET_RESPONSE:
        hdr->quadlet_data = in[3];
        break;

    case FW_TCODE_READ_BLOCK_REQUEST:
    case FW_TCODE_WRITE_BLOCK_REQUEST:
    case FW_TCODE_LOCK_REQUEST:
    case FW_TCODE_READ_BLOCK_RESPONSE:
    case FW_TCODE_LOCK_RESPONSE:
        hdr->data_length    = (uint16_t)(in[3] >> 16);
        hdr->extended_tcode = (uint16_t)(in[3] & 0xffffu);
        break;

    default:
        break;
    }

    return n;
}

const char *fw_tcode_name(uint8_t tcode)
{
    switch (tcode) {
    case FW_TCODE_WRITE_QUADLET_REQUEST: return "write_quadlet_request";
    case FW_TCODE_WRITE_BLOCK_REQUEST:   return "write_block_request";
    case FW_TCODE_WRITE_RESPONSE:        return "write_response";
    case FW_TCODE_READ_QUADLET_REQUEST:  return "read_quadlet_request";
    case FW_TCODE_READ_BLOCK_REQUEST:    return "read_block_request";
    case FW_TCODE_READ_QUADLET_RESPONSE: return "read_quadlet_response";
    case FW_TCODE_READ_BLOCK_RESPONSE:   return "read_block_response";
    case FW_TCODE_CYCLE_START:           return "cycle_start";
    case FW_TCODE_LOCK_REQUEST:          return "lock_request";
    case FW_TCODE_STREAM_DATA:           return "stream_data";
    case FW_TCODE_LOCK_RESPONSE:         return "lock_response";
    default:                             return "reserved";
    }
}

const char *fw_rcode_name(uint8_t rcode)
{
    switch (rcode) {
    case FW_RCODE_COMPLETE:       return "complete";
    case FW_RCODE_CONFLICT_ERROR: return "conflict_error";
    case FW_RCODE_DATA_ERROR:     return "data_error";
    case FW_RCODE_TYPE_ERROR:     return "type_error";
    case FW_RCODE_ADDRESS_ERROR:  return "address_error";
    default:                      return "reserved";
    }
}
