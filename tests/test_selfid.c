#include "test_util.h"
#include "fw/fw_selfid.h"

/*
 * Helpers that build self-ID packet #0 exactly as a PHY would emit it, so the
 * fixtures below read as topologies rather than as magic constants.
 */
static fw_quadlet_t selfid0(uint8_t phy_id, bool link_active, uint8_t gap_count,
                            uint8_t speed, bool contender, uint8_t p0,
                            uint8_t p1, uint8_t p2, bool initiated_reset,
                            bool more)
{
    fw_quadlet_t q = 0x2u << 30;               /* 10b tag */
    q |= (fw_quadlet_t)phy_id << 24;
    /* bit 23 stays 0: this is packet #0 */
    if (link_active) q |= 1u << 22;
    q |= (fw_quadlet_t)(gap_count & 0x3f) << 16;
    q |= (fw_quadlet_t)(speed & 0x3) << 14;
    if (contender) q |= 1u << 11;
    q |= (fw_quadlet_t)(p0 & 0x3) << 6;
    q |= (fw_quadlet_t)(p1 & 0x3) << 4;
    q |= (fw_quadlet_t)(p2 & 0x3) << 2;
    if (initiated_reset) q |= 1u << 1;
    if (more) q |= 1u;
    return q;
}

/* Interleave each quadlet with its inverse, the way OHCI DMAs the buffer. */
static size_t add_inverses(const fw_quadlet_t *in, size_t n, fw_quadlet_t *out)
{
    for (size_t i = 0; i < n; i++) {
        out[i * 2]     = in[i];
        out[i * 2 + 1] = ~in[i];
    }
    return n * 2;
}

/* Two nodes: a leaf on phy 0 hanging off the root on phy 1. */
static void test_two_node_bus(void)
{
    fw_quadlet_t sid[2] = {
        selfid0(0, true, 0x3f, FW_SPEED_S400, true,
                FW_PORT_PARENT, FW_PORT_NONE, FW_PORT_NONE, false, false),
        selfid0(1, true, 0x3f, FW_SPEED_S400, true,
                FW_PORT_CHILD, FW_PORT_NCONN, FW_PORT_NONE, true, false),
    };

    fw_topology_t topo;
    fw_err_t err = fw_selfid_parse(sid, 2, false, &topo);

    CHECK_EQ_U(err, FW_OK, "two-node bus parses");
    CHECK_EQ_U(topo.node_count, 2, "node count");
    CHECK_EQ_U(topo.root_id, 1, "root is the highest phy_id");
    CHECK_EQ_U(topo.node[0].parent, 1, "node 0's parent is node 1");
    CHECK_EQ_U(topo.node[1].parent, FW_NO_PARENT, "root has no parent");
    CHECK_EQ_U(topo.node[1].child_count, 1, "root has one child");
    CHECK_EQ_U(topo.node[0].hop_count, 1, "leaf is one hop from root");
    CHECK_EQ_U(topo.node[1].hop_count, 0, "root is zero hops from itself");
    CHECK_EQ_U(topo.node[0].speed, FW_SPEED_S400, "node 0 speed");
    CHECK(topo.node[0].link_active, "node 0 reports an active link");
    CHECK(topo.gap_count_consistent, "gap counts agree");
    CHECK_EQ_U(topo.gap_count, 0x3f, "gap count value");
    CHECK(topo.node[1].initiated_reset, "root initiated the reset");

    /* Trailing absent ports are trimmed, real ones kept. */
    CHECK_EQ_U(topo.node[0].port_count, 1, "node 0 has one real port");
    CHECK_EQ_U(topo.node[1].port_count, 2, "node 1 has two real ports");
}

/* Same bus, but fed through the OHCI inverse-quadlet format. */
static void test_ohci_inverse_format(void)
{
    fw_quadlet_t sid[2] = {
        selfid0(0, true, 0x3f, FW_SPEED_S400, true,
                FW_PORT_PARENT, FW_PORT_NONE, FW_PORT_NONE, false, false),
        selfid0(1, true, 0x3f, FW_SPEED_S400, true,
                FW_PORT_CHILD, FW_PORT_NCONN, FW_PORT_NONE, true, false),
    };
    fw_quadlet_t raw[4];
    size_t n = add_inverses(sid, 2, raw);

    fw_topology_t topo;
    CHECK_EQ_U(fw_selfid_parse(raw, n, true, &topo), FW_OK,
               "OHCI inverse-quadlet buffer parses");
    CHECK_EQ_U(topo.node_count, 2, "node count from OHCI buffer");

    /* Corrupt one inverse: a torn DMA write must be caught. */
    raw[1] ^= 0x00000001u;
    CHECK_EQ_U(fw_selfid_parse(raw, n, true, &topo), FW_ERR_TOPOLOGY,
               "a bad inverse quadlet is rejected");
}

/* A linear chain 0 - 1 - 2 with the root at the far end. */
static void test_chain_topology(void)
{
    fw_quadlet_t sid[3] = {
        selfid0(0, true, 0x20, FW_SPEED_S400, false,
                FW_PORT_PARENT, FW_PORT_NONE, FW_PORT_NONE, false, false),
        selfid0(1, true, 0x20, FW_SPEED_S800, false,
                FW_PORT_CHILD, FW_PORT_PARENT, FW_PORT_NONE, false, false),
        selfid0(2, true, 0x20, FW_SPEED_S800, true,
                FW_PORT_CHILD, FW_PORT_NONE, FW_PORT_NONE, false, false),
    };

    fw_topology_t topo;
    CHECK_EQ_U(fw_selfid_parse(sid, 3, false, &topo), FW_OK, "chain parses");
    CHECK_EQ_U(topo.root_id, 2, "chain root");
    CHECK_EQ_U(topo.node[0].parent, 1, "0's parent is 1");
    CHECK_EQ_U(topo.node[1].parent, 2, "1's parent is 2");
    CHECK_EQ_U(topo.node[0].hop_count, 2, "0 is two hops from the root");
    CHECK_EQ_U(topo.node[1].hop_count, 1, "1 is one hop from the root");
}

/* A root with two leaf children - the branch case the stack walker must
 * handle by popping twice. */
static void test_branch_topology(void)
{
    fw_quadlet_t sid[3] = {
        selfid0(0, true, 0x10, FW_SPEED_S400, false,
                FW_PORT_PARENT, FW_PORT_NONE, FW_PORT_NONE, false, false),
        selfid0(1, true, 0x10, FW_SPEED_S400, false,
                FW_PORT_PARENT, FW_PORT_NONE, FW_PORT_NONE, false, false),
        selfid0(2, true, 0x10, FW_SPEED_S800, true,
                FW_PORT_CHILD, FW_PORT_CHILD, FW_PORT_NONE, false, false),
    };

    fw_topology_t topo;
    CHECK_EQ_U(fw_selfid_parse(sid, 3, false, &topo), FW_OK, "branch parses");
    CHECK_EQ_U(topo.node[2].child_count, 2, "root has two children");
    CHECK_EQ_U(topo.node[0].parent, 2, "leaf 0 hangs off the root");
    CHECK_EQ_U(topo.node[1].parent, 2, "leaf 1 hangs off the root");
    CHECK_EQ_U(topo.node[0].hop_count, 1, "leaf 0 hop count");
    CHECK_EQ_U(topo.node[1].hop_count, 1, "leaf 1 hop count");
}

/* A PHY with more than three ports emits extended packets. */
static void test_extended_packets(void)
{
    fw_quadlet_t p0 = selfid0(0, true, 0x3f, FW_SPEED_S800, true,
                              FW_PORT_CHILD, FW_PORT_NCONN, FW_PORT_NCONN,
                              false, true /* more packets follow */);

    /* Extended packet #0 for phy 0: eight more port fields, all unconnected
     * except the last which is absent. */
    fw_quadlet_t ext = (0x2u << 30) | (0u << 24) | (1u << 23) | (0u << 20);
    for (int i = 0; i < 8; i++)
        ext |= (fw_quadlet_t)FW_PORT_NCONN << (16 - i * 2);

    fw_quadlet_t child = selfid0(0, true, 0x3f, FW_SPEED_S800, true,
                                 FW_PORT_PARENT, FW_PORT_NONE, FW_PORT_NONE,
                                 false, false);
    /* Rebuild as phy 0 = leaf child, phy 1 = the multi-port root. */
    fw_quadlet_t sid[3];
    sid[0] = child;
    sid[1] = (p0 & ~(0x3fu << 24)) | (1u << 24);   /* retag as phy 1 */
    sid[2] = (ext & ~(0x3fu << 24)) | (1u << 24);  /* retag as phy 1 */

    fw_topology_t topo;
    CHECK_EQ_U(fw_selfid_parse(sid, 3, false, &topo), FW_OK,
               "extended self-ID packets parse");
    CHECK_EQ_U(topo.node_count, 2, "extended packets do not create extra nodes");
    CHECK_EQ_U(topo.node[1].port_count, 11,
               "3 base ports + 8 extended ports are all recorded");
    CHECK_EQ_U(topo.node[0].parent, 1, "child attaches to the multi-port root");
}

static void test_malformed_input(void)
{
    fw_topology_t topo;

    CHECK_EQ_U(fw_selfid_parse(NULL, 2, false, &topo), FW_ERR_INVAL,
               "NULL buffer rejected");
    CHECK_EQ_U(fw_selfid_parse((fw_quadlet_t[]){0}, 0, false, &topo),
               FW_ERR_TOPOLOGY, "empty burst rejected");

    /* Not a self-ID packet: top bits are not 10b. */
    fw_quadlet_t not_selfid[1] = { 0x00000000u };
    CHECK_EQ_U(fw_selfid_parse(not_selfid, 1, false, &topo), FW_ERR_TOPOLOGY,
               "non-self-ID quadlet rejected");

    /* phy_ids must start at 0 and not skip. */
    fw_quadlet_t gap[1] = {
        selfid0(3, true, 0x3f, FW_SPEED_S400, false,
                FW_PORT_NONE, FW_PORT_NONE, FW_PORT_NONE, false, false)
    };
    CHECK_EQ_U(fw_selfid_parse(gap, 1, false, &topo), FW_ERR_TOPOLOGY,
               "a gap in the phy_id sequence is rejected");

    /* `more` set but the burst ends: truncated. */
    fw_quadlet_t truncated[1] = {
        selfid0(0, true, 0x3f, FW_SPEED_S400, false,
                FW_PORT_NONE, FW_PORT_NONE, FW_PORT_NONE, false, true)
    };
    CHECK_EQ_U(fw_selfid_parse(truncated, 1, false, &topo), FW_ERR_TOPOLOGY,
               "a burst truncated mid-node is rejected");

    /* A child port with nothing below it to claim is impossible. */
    fw_quadlet_t bad_tree[1] = {
        selfid0(0, true, 0x3f, FW_SPEED_S400, false,
                FW_PORT_CHILD, FW_PORT_NONE, FW_PORT_NONE, false, false)
    };
    CHECK_EQ_U(fw_selfid_parse(bad_tree, 1, false, &topo), FW_ERR_TOPOLOGY,
               "an unsatisfiable child port is rejected");

    /* Odd quadlet count in OHCI mode means a half-written buffer. */
    fw_quadlet_t odd[3] = { 0x80000000u, 0x7fffffffu, 0x81000000u };
    CHECK_EQ_U(fw_selfid_parse(odd, 3, true, &topo), FW_ERR_TOPOLOGY,
               "odd quadlet count in OHCI mode is rejected");
}

static void test_gap_count_mismatch(void)
{
    fw_quadlet_t sid[2] = {
        selfid0(0, true, 0x3f, FW_SPEED_S400, false,
                FW_PORT_PARENT, FW_PORT_NONE, FW_PORT_NONE, false, false),
        selfid0(1, true, 0x05, FW_SPEED_S400, true,
                FW_PORT_CHILD, FW_PORT_NONE, FW_PORT_NONE, false, false),
    };

    fw_topology_t topo;
    CHECK_EQ_U(fw_selfid_parse(sid, 2, false, &topo), FW_OK,
               "mismatched gap counts still parse");
    CHECK(!topo.gap_count_consistent,
          "gap count mismatch is flagged so the bus manager can reconfigure");
}

void test_selfid(void)
{
    TEST_SUITE("fw_selfid");
    test_two_node_bus();
    test_ohci_inverse_format();
    test_chain_topology();
    test_branch_topology();
    test_extended_packets();
    test_gap_count_mismatch();
    test_malformed_input();
}
