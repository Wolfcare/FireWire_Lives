#include "fw/ohci_desc.h"

#include <string.h>

/*
 * Descriptors are little-endian in host memory regardless of CPU endianness,
 * because the controller reads them as a PCI device. Writing them byte-wise
 * keeps that explicit and portable.
 */
static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint32_t ohci_command_ptr(uint32_t block_address, int z)
{
    if (z < 0)
        z = 0;

    /* Z lives in the low four bits, so the block must be 16-byte aligned or
     * the address and the count would overlap. */
    return (block_address & ~OHCI_Z_MASK) | ((uint32_t)z & OHCI_Z_MASK);
}

int ohci_build_at_header(uint8_t tcode, uint8_t tlabel, uint8_t retry,
                         uint8_t speed, fw_nodeid_t destination,
                         fw_offset_t offset, uint16_t data_length,
                         uint16_t extended_tcode,
                         fw_quadlet_t *out, size_t out_quadlets)
{
    if (out == NULL)
        return FW_ERR_INVAL;

    int n = fw_packet_header_quadlets(tcode);
    if (n < 0)
        return n;
    if (out_quadlets < (size_t)n)
        return FW_ERR_NOSPACE;

    memset(out, 0, (size_t)n * sizeof(*out));

    /* Quadlet 0 carries speed where the wire header carries destinationID. */
    out[0] = ((uint32_t)(speed & 0x7u) << 16) |
             ((uint32_t)(tlabel & 0x3fu) << 10) |
             ((uint32_t)(retry & 0x3u) << 8) |
             ((uint32_t)(tcode & 0xfu) << 4);

    /* Quadlet 1 carries destinationID where the wire header carries sourceID;
     * the controller supplies sourceID itself. */
    out[1] = ((uint32_t)destination << 16) |
             (uint32_t)((offset >> 32) & 0xffffu);

    out[2] = (uint32_t)(offset & 0xffffffffu);

    if (n > 3) {
        /* Block requests and lock requests carry length and extended tcode. */
        if (tcode == FW_TCODE_WRITE_QUADLET_REQUEST)
            out[3] = 0;   /* filled by the caller with the payload quadlet */
        else
            out[3] = ((uint32_t)data_length << 16) | (uint32_t)extended_tcode;
    }

    return n;
}

int ohci_build_at_request(const fw_quadlet_t *header, size_t header_quadlets,
                          uint32_t payload_address, uint32_t payload_length,
                          void *block, size_t block_size)
{
    if (header == NULL || block == NULL)
        return FW_ERR_INVAL;
    if (header_quadlets == 0 ||
        header_quadlets * sizeof(fw_quadlet_t) > OHCI_IMMEDIATE_MAX_BYTES)
        return FW_ERR_INVAL;
    if (payload_length > 0 && payload_address == 0)
        return FW_ERR_INVAL;
    if (payload_length > 0xffffu)
        return FW_ERR_INVAL;

    /*
     * Immediate data must occupy a whole number of 16-byte units, because Z
     * counts units rather than bytes. A 3-quadlet header therefore still
     * consumes a full 16-byte unit, with the last quadlet unused.
     */
    size_t header_bytes = header_quadlets * sizeof(fw_quadlet_t);
    size_t immediate_units = (header_bytes + 15) / 16;
    size_t immediate_bytes = immediate_units * 16;

    bool has_payload = (payload_length > 0);

    /* One descriptor for the header, its immediate data, and optionally a
     * second descriptor pointing at the payload. */
    size_t needed = sizeof(ohci_descriptor_t) + immediate_bytes;
    if (has_payload)
        needed += sizeof(ohci_descriptor_t);

    if (block_size < needed)
        return FW_ERR_NOSPACE;

    uint8_t *p = (uint8_t *)block;
    memset(p, 0, needed);

    /*
     * Descriptor 0: carries the packet header as immediate data.
     *
     * When there is no payload this is the last descriptor in the block, so it
     * is OUTPUT_LAST and asks for an interrupt. With a payload it is
     * OUTPUT_MORE and the following descriptor terminates the block.
     */
    /*
     * OUTPUT_LAST must always carry branch=always, even when it ends the list:
     * end of list is signalled solely by a zero branch address, never by the
     * branch control field. Setting branch=never here makes the controller
     * reject the descriptor with evt_unknown.
     *
     * OUTPUT_MORE header descriptors take interrupt=never/branch=never,
     * because the controller relies on physical contiguity to reach the
     * descriptor that follows and ignores their branch word.
     */
    bool header_is_last = !has_payload;

    uint32_t cmd       = header_is_last ? OHCI_DESC_OUTPUT_LAST
                                        : OHCI_DESC_OUTPUT_MORE;
    uint32_t interrupt = header_is_last ? OHCI_DESC_INTERRUPT_ALWAYS : 0;
    uint32_t branch    = header_is_last ? OHCI_DESC_BRANCH_ALWAYS : 0;

    put_le32(p + 0, ohci_desc_control(cmd,
                                      OHCI_DESC_KEY_IMMEDIATE,
                                      interrupt,
                                      branch,
                                      (uint32_t)header_bytes));
    put_le32(p + 4, 0);   /* immediate data follows; no separate address */
    put_le32(p + 8, 0);   /* branch address filled in by the caller */
    put_le32(p + 12, 0);  /* status, written by the controller */

    /* The header quadlets themselves, immediately after the descriptor. */
    uint8_t *immediate = p + sizeof(ohci_descriptor_t);
    for (size_t i = 0; i < header_quadlets; i++)
        put_le32(immediate + (i * 4), header[i]);

    if (has_payload) {
        uint8_t *d1 = immediate + immediate_bytes;

        /* Same rule: branch=always, terminated by a zero branch address. */
        put_le32(d1 + 0, ohci_desc_control(OHCI_DESC_OUTPUT_LAST,
                                           0, /* key 0: address points at data */
                                           OHCI_DESC_INTERRUPT_ALWAYS,
                                           OHCI_DESC_BRANCH_ALWAYS,
                                           payload_length));
        put_le32(d1 + 4, payload_address);
        put_le32(d1 + 8, 0);
        put_le32(d1 + 12, 0);
    }

    /* Z counts 16-byte units: the header descriptor, its immediate data, and
     * the payload descriptor if present. The controller requires 2 <= Z <= 8. */
    int z = 1 + (int)immediate_units + (has_payload ? 1 : 0);
    if (z < 2 || z > 8)
        return FW_ERR_INVAL;
    return z;
}

int ohci_build_ar_buffer(uint32_t buffer_address, uint32_t buffer_length,
                         uint32_t branch_address,
                         void *block, size_t block_size)
{
    if (block == NULL || buffer_address == 0)
        return FW_ERR_INVAL;
    if (buffer_length == 0 || buffer_length > 0xffffu)
        return FW_ERR_INVAL;
    if (block_size < sizeof(ohci_descriptor_t))
        return FW_ERR_NOSPACE;

    uint8_t *p = (uint8_t *)block;
    memset(p, 0, sizeof(ohci_descriptor_t));

    /*
     * INPUT_MORE with branch-always: the controller fills this buffer then
     * follows the branch to the next block, so a ring of these keeps receive
     * running without further intervention.
     */
    put_le32(p + 0, ohci_desc_control(OHCI_DESC_INPUT_MORE,
                                      0,
                                      OHCI_DESC_INTERRUPT_ALWAYS,
                                      OHCI_DESC_BRANCH_ALWAYS,
                                      buffer_length));
    put_le32(p + 4, buffer_address);
    put_le32(p + 8, branch_address);
    put_le32(p + 12, 0);

    return 1;
}

uint16_t ohci_desc_res_count(const void *block)
{
    if (block == NULL)
        return 0;
    return (uint16_t)(get_le32((const uint8_t *)block + 12) & 0xffffu);
}

uint16_t ohci_desc_xfer_status(const void *block)
{
    if (block == NULL)
        return 0;
    return (uint16_t)(get_le32((const uint8_t *)block + 12) >> 16);
}

bool ohci_desc_is_complete(const void *block)
{
    /* The controller writes a non-zero event code into xferStatus when it
     * finishes with a descriptor; until then the field reads as zero. */
    return ohci_desc_xfer_status(block) != 0;
}

bool ohci_ack_is_success(uint8_t ack)
{
    /* ack_complete means the target accepted the packet outright.
     * ack_pending means it accepted it and will answer with a split
     * response, which is equally a success at this layer. */
    return ack == OHCI_ACK_COMPLETE || ack == OHCI_ACK_PENDING;
}

const char *ohci_event_name(uint8_t event)
{
    switch (event) {
    case OHCI_EVT_NO_STATUS:       return "no status";
    case OHCI_EVT_LONG_PACKET:     return "long packet";
    case OHCI_EVT_MISSING_ACK:     return "missing ack";
    case OHCI_EVT_UNDERRUN:        return "underrun";
    case OHCI_EVT_OVERRUN:         return "overrun";
    case OHCI_EVT_DESCRIPTOR_READ: return "descriptor read error";
    case OHCI_EVT_DATA_READ:       return "data read error";
    case OHCI_EVT_DATA_WRITE:      return "data write error";
    case OHCI_EVT_BUS_RESET:       return "bus reset";
    case OHCI_EVT_TIMEOUT:         return "timeout";
    case OHCI_EVT_TCODE_ERR:       return "tcode error";
    case OHCI_EVT_UNKNOWN:         return "unknown event";
    case OHCI_EVT_FLUSHED:         return "flushed by bus reset";
    case OHCI_ACK_COMPLETE:        return "ack_complete";
    case OHCI_ACK_PENDING:         return "ack_pending";
    case OHCI_ACK_BUSY_X:          return "ack_busy_X";
    case OHCI_ACK_BUSY_A:          return "ack_busy_A";
    case OHCI_ACK_BUSY_B:          return "ack_busy_B";
    case OHCI_ACK_TARDY:           return "ack_tardy";
    case OHCI_ACK_DATA_ERROR:      return "ack_data_error";
    case OHCI_ACK_TYPE_ERROR:      return "ack_type_error";
    default:                       return "reserved";
    }
}
