/*
 * fw_sbp2.h - Serial Bus Protocol 2, the storage protocol for FireWire.
 *
 * SBP-2 tunnels SCSI over 1394. The initiator writes Operation Request Blocks
 * (ORBs) into its own memory and hands the target a pointer; the target then
 * DMAs the ORB out, executes it, and writes a status block back to an address
 * the initiator nominated. Almost all of the traffic is target-initiated,
 * which is why the transaction layer underneath has to be correct.
 *
 * Everything an SBP-2 target reads or writes is big-endian in host memory.
 * These encoders therefore emit bytes, not host-order quadlets, so the
 * on-the-wire layout is explicit and testable.
 */

#ifndef FW_SBP2_H
#define FW_SBP2_H

#include "fw_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Management functions (SBP-2 table 6-1). */
typedef enum {
    SBP2_FUNC_LOGIN              = 0x0,
    SBP2_FUNC_QUERY_LOGINS       = 0x1,
    SBP2_FUNC_RECONNECT          = 0x3,
    SBP2_FUNC_SET_PASSWORD       = 0x4,
    SBP2_FUNC_LOGOUT             = 0x7,
    SBP2_FUNC_ABORT_TASK         = 0xb,
    SBP2_FUNC_ABORT_TASK_SET     = 0xc,
    SBP2_FUNC_LOGICAL_UNIT_RESET = 0xe,
    SBP2_FUNC_TARGET_RESET       = 0xf,
} sbp2_function_t;

/*
 * Fetch-agent CSR offsets, relative to the command block agent address the
 * target returns in its login response.
 */
#define SBP2_AGENT_STATE                0x00
#define SBP2_AGENT_RESET                0x04
#define SBP2_ORB_POINTER                0x08
#define SBP2_DOORBELL                   0x10
#define SBP2_UNSOLICITED_STATUS_ENABLE  0x14

/* Sizes fixed by the specification. */
#define SBP2_MANAGEMENT_ORB_SIZE  32
#define SBP2_COMMAND_ORB_SIZE     32
#define SBP2_LOGIN_RESPONSE_SIZE  16
#define SBP2_STATUS_SIZE          32
#define SBP2_MAX_CDB_SIZE         12   /* inline CDB space in a command ORB */

/* A 64-bit 1394 address, as ORBs carry it. */
typedef struct {
    uint64_t address;
} sbp2_pointer_t;

/* ---- Management ORB ---- */

typedef struct {
    sbp2_function_t function;
    uint16_t lun;
    uint64_t password;            /* 0 when unused */
    uint16_t password_length;     /* 0 = the password field is a literal */
    uint64_t login_response_addr; /* where the target writes its response */
    uint16_t login_response_size;
    uint64_t status_fifo_addr;    /* where the target writes status blocks */
    bool     exclusive;           /* refuse other initiators */
    uint8_t  reconnect;           /* 2^reconnect seconds of reconnect hold */
    bool     notify;
    uint16_t login_id;            /* for LOGOUT and RECONNECT */
} sbp2_management_params_t;

/*
 * Encode a management ORB into `buf`, which must be at least
 * SBP2_MANAGEMENT_ORB_SIZE bytes. Returns bytes written or a negative
 * fw_err_t.
 */
int sbp2_encode_management_orb(const sbp2_management_params_t *params,
                               uint8_t *buf, size_t buf_size);

/* ---- Login response ---- */

typedef struct {
    uint16_t length;
    uint16_t login_id;
    uint64_t command_block_agent;  /* base of the fetch-agent registers */
    uint16_t reconnect_hold;       /* seconds we may reconnect after a reset */
} sbp2_login_response_t;

fw_err_t sbp2_parse_login_response(const uint8_t *buf, size_t buf_size,
                                   sbp2_login_response_t *out);

/* ---- Command ORB ---- */

typedef enum {
    SBP2_DIR_TO_TARGET = 0,    /* write: data flows initiator -> target */
    SBP2_DIR_FROM_TARGET = 1,  /* read */
} sbp2_direction_t;

typedef struct {
    uint64_t next_orb;          /* 0 for none; encoded as the null pointer */
    uint64_t data_descriptor;   /* buffer address, or page table address */
    uint16_t data_size;         /* bytes, or page table entry count */
    bool     page_table_present;
    uint8_t  page_size;         /* 0 = pages are 2^(page_size+8) bytes */
    uint8_t  max_payload;       /* 2^(max_payload+2) bytes per packet */
    uint8_t  speed;             /* fw_speed_t */
    sbp2_direction_t direction;
    bool     notify;
    uint8_t  cdb[SBP2_MAX_CDB_SIZE];
    uint8_t  cdb_length;
} sbp2_command_params_t;

int sbp2_encode_command_orb(const sbp2_command_params_t *params,
                            uint8_t *buf, size_t buf_size);

/* ---- Status block ---- */

/* The `resp` field: how to interpret the rest of the block. */
typedef enum {
    SBP2_RESP_REQUEST_COMPLETE = 0,
    SBP2_RESP_TRANSPORT_FAILURE = 1,
    SBP2_RESP_ILLEGAL_REQUEST = 2,
    SBP2_RESP_VENDOR_DEPENDENT = 3,
} sbp2_response_t;

typedef struct {
    uint8_t  src;
    uint8_t  resp;         /* sbp2_response_t */
    bool     dead;         /* the fetch agent has stopped and needs a reset */
    uint8_t  len;          /* quadlets of meaningful data, including the header */
    uint8_t  sbp_status;
    uint64_t orb_offset;   /* which ORB this status refers to */
    uint8_t  data[24];
    size_t   data_length;
} sbp2_status_t;

fw_err_t sbp2_parse_status(const uint8_t *buf, size_t buf_size,
                           sbp2_status_t *out);

/* True when the status block reports a normally completed request. */
bool sbp2_status_is_success(const sbp2_status_t *status);

const char *sbp2_function_name(sbp2_function_t f);
const char *sbp2_response_name(uint8_t resp);

#ifdef __cplusplus
}
#endif

#endif /* FW_SBP2_H */
