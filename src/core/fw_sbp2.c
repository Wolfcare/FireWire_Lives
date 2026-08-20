#include "fw/fw_sbp2.h"

#include <string.h>

/* ORBs are big-endian in host memory, because the target DMAs them out and
 * reads them as a 1394 device. Encode explicitly rather than relying on any
 * host layout. */
static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v);
}

static void put_be64(uint8_t *p, uint64_t v)
{
    put_be32(p, (uint32_t)(v >> 32));
    put_be32(p + 4, (uint32_t)(v & 0xffffffffu));
}

static uint32_t get_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

/*
 * A null ORB pointer is signalled by the most significant bit of the pointer's
 * first quadlet, not by an all-zero address - address zero is a legal 1394
 * address.
 */
#define SBP2_ORB_NULL_POINTER 0x80000000u

int sbp2_encode_management_orb(const sbp2_management_params_t *params,
                               uint8_t *buf, size_t buf_size)
{
    if (params == NULL || buf == NULL)
        return FW_ERR_INVAL;
    if (buf_size < SBP2_MANAGEMENT_ORB_SIZE)
        return FW_ERR_NOSPACE;

    memset(buf, 0, SBP2_MANAGEMENT_ORB_SIZE);

    /* Quadlets 0-1: password, either a literal or an address to fetch it from. */
    put_be64(buf + 0, params->password);

    /* Quadlets 2-3: where the target writes its login response. */
    put_be64(buf + 8, params->login_response_addr);

    /*
     * Quadlet 4. LOGIN carries a logical unit number here; LOGOUT and
     * RECONNECT reuse the same 16 bits to carry the login_id issued by the
     * earlier LOGIN.
     */
    uint16_t low16 = params->lun;
    if (params->function == SBP2_FUNC_LOGOUT ||
        params->function == SBP2_FUNC_RECONNECT)
        low16 = params->login_id;

    uint32_t misc = (uint32_t)low16;
    misc |= ((uint32_t)params->function & 0xfu) << 16;
    misc |= ((uint32_t)params->reconnect & 0xfu) << 20;
    if (params->exclusive)
        misc |= 1u << 28;
    if (params->notify)
        misc |= 1u << 31;
    put_be32(buf + 16, misc);

    /* Quadlet 5: password length above, response buffer length below. */
    put_be32(buf + 20, ((uint32_t)params->password_length << 16) |
                       (uint32_t)params->login_response_size);

    /* Quadlets 6-7: the address the target writes status blocks to. */
    put_be64(buf + 24, params->status_fifo_addr);

    return SBP2_MANAGEMENT_ORB_SIZE;
}

fw_err_t sbp2_parse_login_response(const uint8_t *buf, size_t buf_size,
                                   sbp2_login_response_t *out)
{
    if (buf == NULL || out == NULL)
        return FW_ERR_INVAL;
    if (buf_size < SBP2_LOGIN_RESPONSE_SIZE)
        return FW_ERR_TOOSMALL;

    memset(out, 0, sizeof(*out));

    uint32_t misc = get_be32(buf + 0);
    out->length   = (uint16_t)(misc >> 16);
    out->login_id = (uint16_t)(misc & 0xffffu);

    /* The command block agent base: every fetch-agent register is an offset
     * from here. */
    out->command_block_agent = ((uint64_t)get_be32(buf + 4) << 32) |
                               (uint64_t)get_be32(buf + 8);

    out->reconnect_hold = (uint16_t)(get_be32(buf + 12) & 0xffffu);

    return FW_OK;
}

int sbp2_encode_command_orb(const sbp2_command_params_t *params,
                            uint8_t *buf, size_t buf_size)
{
    if (params == NULL || buf == NULL)
        return FW_ERR_INVAL;
    if (buf_size < SBP2_COMMAND_ORB_SIZE)
        return FW_ERR_NOSPACE;
    if (params->cdb_length > SBP2_MAX_CDB_SIZE)
        return FW_ERR_INVAL;

    memset(buf, 0, SBP2_COMMAND_ORB_SIZE);

    /* Quadlets 0-1: next ORB in the chain, or the null pointer. */
    if (params->next_orb == 0)
        put_be32(buf + 0, SBP2_ORB_NULL_POINTER);
    else
        put_be64(buf + 0, params->next_orb);

    /* Quadlets 2-3: the data buffer, or a page table when scattered. */
    put_be64(buf + 8, params->data_descriptor);

    /* Quadlet 4. */
    uint32_t misc = (uint32_t)params->data_size;
    misc |= ((uint32_t)params->page_size & 0x7u) << 16;
    if (params->page_table_present)
        misc |= 1u << 19;
    misc |= ((uint32_t)params->max_payload & 0xfu) << 20;
    misc |= ((uint32_t)params->speed & 0x7u) << 24;
    if (params->direction == SBP2_DIR_FROM_TARGET)
        misc |= 1u << 27;
    if (params->notify)
        misc |= 1u << 31;
    put_be32(buf + 16, misc);

    /* Quadlets 5-7: the SCSI command block, inline. */
    if (params->cdb_length > 0)
        memcpy(buf + 20, params->cdb, params->cdb_length);

    return SBP2_COMMAND_ORB_SIZE;
}

fw_err_t sbp2_parse_status(const uint8_t *buf, size_t buf_size,
                           sbp2_status_t *out)
{
    if (buf == NULL || out == NULL)
        return FW_ERR_INVAL;
    /* A target may write a short status block; the header is the minimum. */
    if (buf_size < 8)
        return FW_ERR_TOOSMALL;

    memset(out, 0, sizeof(*out));

    uint32_t status = get_be32(buf + 0);

    out->src        = (uint8_t)((status >> 30) & 0x3u);
    out->resp       = (uint8_t)((status >> 28) & 0x3u);
    out->dead       = ((status >> 27) & 0x1u) != 0;
    out->len        = (uint8_t)((status >> 24) & 0x7u);
    out->sbp_status = (uint8_t)((status >> 16) & 0xffu);

    /* Which ORB this refers to: 16 bits here, 32 more in the next quadlet. */
    out->orb_offset = ((uint64_t)(status & 0xffffu) << 32) |
                      (uint64_t)get_be32(buf + 4);

    /*
     * `len` counts meaningful quadlets including the two header quadlets, so
     * anything beyond those two is payload - typically SCSI sense data.
     */
    size_t payload_quadlets = (out->len > 2) ? (size_t)(out->len - 2) : 0;
    size_t payload_bytes = payload_quadlets * 4;

    size_t available = (buf_size > 8) ? buf_size - 8 : 0;
    if (payload_bytes > available)
        payload_bytes = available;
    if (payload_bytes > sizeof(out->data))
        payload_bytes = sizeof(out->data);

    if (payload_bytes > 0)
        memcpy(out->data, buf + 8, payload_bytes);
    out->data_length = payload_bytes;

    return FW_OK;
}

bool sbp2_status_is_success(const sbp2_status_t *status)
{
    if (status == NULL)
        return false;

    /* A completed request that did not kill the fetch agent and reported no
     * SBP-level error. SCSI-level status lives in the sense data. */
    return status->resp == SBP2_RESP_REQUEST_COMPLETE &&
           !status->dead &&
           status->sbp_status == 0;
}

const char *sbp2_function_name(sbp2_function_t f)
{
    switch (f) {
    case SBP2_FUNC_LOGIN:              return "LOGIN";
    case SBP2_FUNC_QUERY_LOGINS:       return "QUERY LOGINS";
    case SBP2_FUNC_RECONNECT:          return "RECONNECT";
    case SBP2_FUNC_SET_PASSWORD:       return "SET PASSWORD";
    case SBP2_FUNC_LOGOUT:             return "LOGOUT";
    case SBP2_FUNC_ABORT_TASK:         return "ABORT TASK";
    case SBP2_FUNC_ABORT_TASK_SET:     return "ABORT TASK SET";
    case SBP2_FUNC_LOGICAL_UNIT_RESET: return "LOGICAL UNIT RESET";
    case SBP2_FUNC_TARGET_RESET:       return "TARGET RESET";
    default:                           return "unknown";
    }
}

const char *sbp2_response_name(uint8_t resp)
{
    switch (resp) {
    case SBP2_RESP_REQUEST_COMPLETE:  return "request complete";
    case SBP2_RESP_TRANSPORT_FAILURE: return "transport failure";
    case SBP2_RESP_ILLEGAL_REQUEST:   return "illegal request";
    case SBP2_RESP_VENDOR_DEPENDENT:  return "vendor dependent";
    default:                          return "unknown";
    }
}
