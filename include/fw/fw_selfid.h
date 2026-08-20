/*
 * fw_selfid.h - Self-ID packet decoding and bus topology reconstruction.
 *
 * After every bus reset the PHY of each node broadcasts one to four self-ID
 * packets describing its port connections, speed and capabilities. The OHCI
 * controller DMAs the whole burst into the self-ID buffer; parsing it is how
 * we learn what is on the bus, who the root is, and which node is the
 * isochronous resource manager.
 */

#ifndef FW_SELFID_H
#define FW_SELFID_H

#include "fw_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Port status codes carried in the two-bit port fields. */
typedef enum {
    FW_PORT_NONE   = 0,  /* port does not exist on this PHY */
    FW_PORT_NCONN  = 1,  /* port exists but nothing is plugged in */
    FW_PORT_PARENT = 2,  /* connected, and it leads toward the root */
    FW_PORT_CHILD  = 3,  /* connected, and it leads away from the root */
} fw_port_status_t;

#define FW_MAX_PORTS 16  /* 3 in packet #0, then 8 per extended packet */

/* One decoded node, assembled from that node's self-ID packet(s). */
typedef struct {
    uint8_t  phy_id;
    bool     link_active;    /* L: a link layer is present and enabled */
    uint8_t  gap_count;
    uint8_t  speed;          /* fw_speed_t as reported by the PHY */
    bool     contender;      /* C: eligible to become bus/iso manager */
    uint8_t  power_class;    /* power supply/consumption class */
    bool     initiated_reset;
    uint8_t  port_count;
    uint8_t  port[FW_MAX_PORTS];  /* fw_port_status_t per port */

    /* Derived by fw_topology_build(). */
    uint8_t  parent;         /* phy_id of parent, or FW_NO_PARENT for root */
    uint8_t  child_count;
    uint8_t  hop_count;      /* distance from the root */
} fw_node_t;

#define FW_NO_PARENT 0xff

/* The whole bus as it stands after one reset. */
typedef struct {
    fw_node_t node[FW_MAX_NODES];
    uint8_t   node_count;
    uint8_t   root_id;        /* highest phy_id; the root by definition */
    uint8_t   gap_count;      /* consistent gap_count, or 0x3f if they disagree */
    bool      gap_count_consistent;
    uint32_t  generation;     /* bumped by the caller on every reset */
} fw_topology_t;

/*
 * Decode a self-ID burst into `topo`.
 *
 * `quadlets` points at the self-ID packets in host byte order with the OHCI
 * self-ID buffer header already stripped, and `count` is how many quadlets
 * are present. OHCI stores each self-ID quadlet followed by its bitwise
 * inverse for validation; pass `has_inverses = true` when handing this the
 * raw OHCI buffer so the check is performed and the copies discarded.
 *
 * Returns FW_OK, or FW_ERR_TOPOLOGY if the burst is inconsistent (bad
 * inverse, duplicate phy_id, gaps in the phy_id sequence).
 */
fw_err_t fw_selfid_parse(const fw_quadlet_t *quadlets, size_t count,
                         bool has_inverses, fw_topology_t *topo);

/*
 * Walk the decoded port arrays and fill in parent/child/hop_count, and
 * identify the root. Called automatically by fw_selfid_parse(); exposed
 * separately for tests.
 */
fw_err_t fw_topology_build(fw_topology_t *topo);

/* True if this quadlet is a self-ID packet (top two bits are 10b). */
bool fw_selfid_is_selfid(fw_quadlet_t q);

/* Human-readable helpers, handy for logging and the probe tool. */
const char *fw_speed_name(uint8_t speed);
const char *fw_port_status_name(uint8_t status);

#ifdef __cplusplus
}
#endif

#endif /* FW_SELFID_H */
