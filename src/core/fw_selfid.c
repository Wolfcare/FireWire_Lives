#include "fw/fw_selfid.h"

#include <string.h>

/*
 * Self-ID packet #0 (IEEE 1394-2008 figure 16-21):
 *
 *   31 30 | 29..24 | 23 | 22 | 21..16  | 15 14 | 13 12 | 11 | 10..8 |
 *    1  0 | phy_id |  0 |  L | gap_cnt |  sp   |  del  |  c |  pwr  |
 *   7 6 | 5 4 | 3 2 | 1 | 0
 *    p0 |  p1 |  p2 | i | m
 *
 * Extended packets #1..#3 replace everything below bit 23 with eight further
 * port fields:
 *
 *   31 30 | 29..24 | 23 | 22..20 | 19 18 | 17..2          | 1 | 0
 *    1  0 | phy_id |  1 |    n   |  rsv  | pa..ph (2b ea) |rsv| m
 */

#define SELFID_TAG_SHIFT        30
#define SELFID_TAG_VALUE        0x2u    /* 10b marks a self-ID packet */
#define SELFID_PHY_ID_SHIFT     24
#define SELFID_PHY_ID_MASK      0x3fu
#define SELFID_EXTENDED_BIT     (1u << 23)
#define SELFID_MORE_PACKETS_BIT (1u << 0)

/* Packet #0 fields. */
#define SELFID0_LINK_ACTIVE_BIT (1u << 22)
#define SELFID0_GAP_CNT_SHIFT   16
#define SELFID0_GAP_CNT_MASK    0x3fu
#define SELFID0_SPEED_SHIFT     14
#define SELFID0_SPEED_MASK      0x3u
#define SELFID0_CONTENDER_BIT   (1u << 11)
#define SELFID0_POWER_SHIFT     8
#define SELFID0_POWER_MASK      0x7u
#define SELFID0_INITIATED_RESET (1u << 1)
#define SELFID0_PORT_COUNT      3

/* Extended packet fields. */
#define SELFIDN_SEQ_SHIFT       20
#define SELFIDN_SEQ_MASK        0x7u
#define SELFIDN_PORT_COUNT      8

bool fw_selfid_is_selfid(fw_quadlet_t q)
{
    return ((q >> SELFID_TAG_SHIFT) & 0x3u) == SELFID_TAG_VALUE;
}

static uint8_t selfid_phy_id(fw_quadlet_t q)
{
    return (uint8_t)((q >> SELFID_PHY_ID_SHIFT) & SELFID_PHY_ID_MASK);
}

/*
 * Port fields are packed two bits each, most significant first. `first_bit` is
 * the LOW bit of the first field: packet #0 puts p0 in bits [7:6] so it passes
 * 6, and extended packets put pa in bits [17:16] so they pass 16.
 */
static void unpack_ports(fw_quadlet_t q, int first_bit, int count,
                         fw_node_t *node)
{
    for (int i = 0; i < count; i++) {
        int shift = first_bit - (i * 2);
        if (node->port_count >= FW_MAX_PORTS)
            return;
        node->port[node->port_count++] = (uint8_t)((q >> shift) & 0x3u);
    }
}

/*
 * A PHY reports a fixed number of port fields regardless of how many ports it
 * physically has, padding the remainder with FW_PORT_NONE. Trimming the
 * trailing NONE entries gives us the real port count.
 */
static void trim_absent_ports(fw_node_t *node)
{
    while (node->port_count > 0 &&
           node->port[node->port_count - 1] == FW_PORT_NONE)
        node->port_count--;
}

fw_err_t fw_selfid_parse(const fw_quadlet_t *quadlets, size_t count,
                         bool has_inverses, fw_topology_t *topo)
{
    if (quadlets == NULL || topo == NULL)
        return FW_ERR_INVAL;

    memset(topo, 0, sizeof(*topo));
    for (size_t i = 0; i < FW_MAX_NODES; i++)
        topo->node[i].parent = FW_NO_PARENT;

    const size_t stride = has_inverses ? 2u : 1u;
    if (has_inverses && (count % 2) != 0)
        return FW_ERR_TOPOLOGY;

    fw_node_t *current = NULL;
    int expected_seq = 0;

    for (size_t i = 0; i + stride <= count; i += stride) {
        fw_quadlet_t q = quadlets[i];

        /* OHCI writes each self-ID quadlet twice, the second time inverted,
         * so that a torn DMA write is detectable. */
        if (has_inverses && (q ^ quadlets[i + 1]) != 0xffffffffu)
            return FW_ERR_TOPOLOGY;

        if (!fw_selfid_is_selfid(q))
            return FW_ERR_TOPOLOGY;

        uint8_t phy_id = selfid_phy_id(q);
        if (phy_id >= FW_MAX_NODES)
            return FW_ERR_TOPOLOGY;

        if ((q & SELFID_EXTENDED_BIT) == 0) {
            /* Packet #0 - a new node begins here. Self-ID packets arrive in
             * ascending phy_id order with no gaps. */
            if (phy_id != topo->node_count)
                return FW_ERR_TOPOLOGY;

            current = &topo->node[phy_id];
            current->phy_id          = phy_id;
            current->link_active     = (q & SELFID0_LINK_ACTIVE_BIT) != 0;
            current->gap_count       = (uint8_t)((q >> SELFID0_GAP_CNT_SHIFT) & SELFID0_GAP_CNT_MASK);
            current->speed           = (uint8_t)((q >> SELFID0_SPEED_SHIFT) & SELFID0_SPEED_MASK);
            current->contender       = (q & SELFID0_CONTENDER_BIT) != 0;
            current->power_class     = (uint8_t)((q >> SELFID0_POWER_SHIFT) & SELFID0_POWER_MASK);
            current->initiated_reset = (q & SELFID0_INITIATED_RESET) != 0;
            current->port_count      = 0;

            unpack_ports(q, 6, SELFID0_PORT_COUNT, current);

            topo->node_count++;
            expected_seq = 0;
        } else {
            /* Extended packet - must continue the node we are already on, and
             * its sequence numbers must run 0, 1, 2 in order. */
            if (current == NULL || current->phy_id != phy_id)
                return FW_ERR_TOPOLOGY;

            int seq = (int)((q >> SELFIDN_SEQ_SHIFT) & SELFIDN_SEQ_MASK);
            if (seq != expected_seq)
                return FW_ERR_TOPOLOGY;
            expected_seq++;

            unpack_ports(q, 16, SELFIDN_PORT_COUNT, current);
        }

        /* The `m` bit says another packet for this node follows. */
        bool more = (q & SELFID_MORE_PACKETS_BIT) != 0;
        if (!more) {
            trim_absent_ports(current);
            current = NULL;
        }
    }

    /* A burst that ends mid-node is truncated. */
    if (current != NULL)
        return FW_ERR_TOPOLOGY;

    if (topo->node_count == 0)
        return FW_ERR_TOPOLOGY;

    return fw_topology_build(topo);
}

fw_err_t fw_topology_build(fw_topology_t *topo)
{
    if (topo == NULL || topo->node_count == 0)
        return FW_ERR_INVAL;

    /* The node with the highest phy_id is always the root: phy_ids are handed
     * out in the order nodes finish self-identifying, and the root goes last. */
    topo->root_id = (uint8_t)(topo->node_count - 1);

    /* Gap count must agree across the bus or timing is undefined; a mismatch
     * means the bus manager needs to issue a PHY config packet. */
    uint8_t gap = topo->node[0].gap_count;
    topo->gap_count_consistent = true;
    for (uint8_t i = 1; i < topo->node_count; i++) {
        if (topo->node[i].gap_count != gap) {
            topo->gap_count_consistent = false;
            break;
        }
    }
    topo->gap_count = topo->gap_count_consistent ? gap : 0x3f;

    /*
     * Reconstruct the tree. Self-ID ordering guarantees that when node i
     * reports a child port, that child is the highest-numbered node below i
     * that has not yet been claimed. Walking upward from phy_id 0 and keeping
     * a stack of unclaimed subtrees resolves every edge in one pass.
     */
    uint8_t stack[FW_MAX_NODES];
    int sp = 0;

    for (uint8_t i = 0; i < topo->node_count; i++) {
        fw_node_t *n = &topo->node[i];
        n->child_count = 0;

        for (uint8_t p = 0; p < n->port_count; p++) {
            if (n->port[p] != FW_PORT_CHILD)
                continue;

            if (sp == 0)
                return FW_ERR_TOPOLOGY;  /* child port with no subtree to claim */

            uint8_t child = stack[--sp];
            topo->node[child].parent = i;
            n->child_count++;
        }

        if (sp >= (int)FW_MAX_NODES)
            return FW_ERR_TOPOLOGY;
        stack[sp++] = i;
    }

    /* Exactly one subtree - the root's - must be left unclaimed. */
    if (sp != 1 || stack[0] != topo->root_id)
        return FW_ERR_TOPOLOGY;

    topo->node[topo->root_id].parent = FW_NO_PARENT;

    /* A node's parent always has a higher phy_id than the node itself, so
     * iterating from the root down to phy_id 0 visits every parent before its
     * children and hop counts fall out in a single pass. */
    topo->node[topo->root_id].hop_count = 0;
    for (int i = (int)topo->node_count - 1; i >= 0; i--) {
        fw_node_t *n = &topo->node[i];
        if (n->parent == FW_NO_PARENT)
            continue;
        n->hop_count = (uint8_t)(topo->node[n->parent].hop_count + 1);
    }

    return FW_OK;
}

const char *fw_speed_name(uint8_t speed)
{
    switch (speed) {
    case FW_SPEED_S100:  return "S100";
    case FW_SPEED_S200:  return "S200";
    case FW_SPEED_S400:  return "S400";
    case FW_SPEED_S800:  return "S800";
    case FW_SPEED_S1600: return "S1600";
    case FW_SPEED_S3200: return "S3200";
    default:             return "unknown";
    }
}

const char *fw_port_status_name(uint8_t status)
{
    switch (status) {
    case FW_PORT_NONE:   return "absent";
    case FW_PORT_NCONN:  return "unconnected";
    case FW_PORT_PARENT: return "parent";
    case FW_PORT_CHILD:  return "child";
    default:             return "invalid";
    }
}
