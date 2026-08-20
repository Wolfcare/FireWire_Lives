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
    uint32_t cmd = has_payload ? OHCI_DESC_OUTPUT_MORE : OHCI_DESC_OUTPUT_LAST;
    uint32_t interrupt = has_payload ? 0 : OHCI_DESC_INTERRUPT_ALWAYS;

    put_le32(p + 0, ohci_desc_control(cmd,
                                      OHCI_DESC_KEY_IMMEDIATE,
                                      interrupt,
                                      0, /* branch handled by CommandPtr/Z */
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

        put_le32(d1 + 0, ohci_desc_control(OHCI_DESC_OUTPUT_LAST,
                                           0, /* key 0: address points at data */
                                           OHCI_DESC_INTERRUPT_ALWAYS,
                                           0,
                                           payload_length));
        put_le32(d1 + 4, payload_address);
        put_le32(d1 + 8, 0);
        put_le32(d1 + 12, 0);
    }

    /* Z counts 16-byte units: the header descriptor, its immediate data, and
     * the payload descriptor if present. */
    int z = 1 + (int)immediate_units + (has_payload ? 1 : 0);
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
