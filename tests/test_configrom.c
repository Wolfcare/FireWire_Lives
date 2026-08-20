#include "test_util.h"
#include "fw/fw_configrom.h"
#include "fw/fw_crc.h"

/*
 * Config ROM fixtures.
 *
 * CRCs are filled in with fw_crc16(), which is safe to rely on here because
 * test_crc.c independently validates it against a bitwise reference
 * implementation before this suite runs.
 */

#define ENTRY(type, key, value) \
    (((uint32_t)(type) << 30) | ((uint32_t)(key) << 24) | ((value) & 0xffffffu))

/*
 * An SBP-2 hard disk, laid out as a real bridge chip would present it:
 *
 *   [0]  bus info header      [10] vendor text leaf header
 *   [1]  "1394"               [11]   specifier (0)
 *   [2]  bus options          [12]   charset  (0)
 *   [3]  GUID hi              [13]   "ACME"
 *   [4]  GUID lo              [14] unit directory header
 *   [5]  root dir header      [15]   specifier_id = 0x00609e
 *   [6]    vendor id          [16]   version      = 0x010483
 *   [7]    model id           [17]   management agent offset
 *   [8]    vendor text leaf   [18]   logical unit number
 *   [9]    unit directory
 */
static size_t build_sbp2_rom(fw_quadlet_t *rom)
{
    rom[1] = 0x31333934u;                    /* "1394" */
    rom[2] = (1u << 29)                      /* isc */
           | (10u << 12)                     /* max_rec = 10 -> 2048 bytes */
           | 2u;                             /* link_speed = S400 */
    rom[3] = 0x00d04b12u;                    /* vendor 0x00d04b, chip hi 0x12 */
    rom[4] = 0x3456789au;                    /* chip lo */

    /* Bus info header: info_length = 4, crc_length = 4. */
    rom[0] = (4u << 24) | (4u << 16) | fw_crc16(&rom[1], 4);

    /* Root directory: 4 entries. */
    rom[6] = ENTRY(FW_KEY_TYPE_IMMEDIATE, FW_KEY_VENDOR, 0x00d04bu);
    rom[7] = ENTRY(FW_KEY_TYPE_IMMEDIATE, FW_KEY_MODEL,  0x001234u);
    rom[8] = ENTRY(FW_KEY_TYPE_LEAF,      FW_KEY_DESCRIPTOR, 2);  /* 8 + 2 = 10 */
    rom[9] = ENTRY(FW_KEY_TYPE_DIRECTORY, FW_KEY_UNIT,       5);  /* 9 + 5 = 14 */
    rom[5] = (4u << 16) | fw_crc16(&rom[6], 4);

    /* Vendor textual descriptor leaf: 3 quadlets of body. */
    rom[11] = 0;                             /* descriptor type / specifier */
    rom[12] = 0;                             /* width, character set, language */
    rom[13] = 0x41434d45u;                   /* "ACME" */
    rom[10] = (3u << 16) | fw_crc16(&rom[11], 3);

    /* Unit directory: 4 entries. */
    rom[15] = ENTRY(FW_KEY_TYPE_IMMEDIATE,  FW_KEY_SPECIFIER_ID, FW_SPECIFIER_SBP2);
    rom[16] = ENTRY(FW_KEY_TYPE_IMMEDIATE,  FW_KEY_VERSION,      FW_VERSION_SBP2);
    rom[17] = ENTRY(FW_KEY_TYPE_CSR_OFFSET, FW_KEY_DEPENDENT_INFO, 0x000400u);
    rom[18] = ENTRY(FW_KEY_TYPE_IMMEDIATE,  FW_KEY_DEPENDENT_INFO, 0x000000u);
    rom[14] = (4u << 16) | fw_crc16(&rom[15], 4);

    return 19;
}

static void test_sbp2_disk(void)
{
    fw_quadlet_t rom[32];
    size_t len = build_sbp2_rom(rom);

    fw_config_rom_t cr;
    fw_err_t err = fw_configrom_parse(rom, len, &cr);

    CHECK_EQ_U(err, FW_OK, "SBP-2 ROM parses with a valid CRC");
    CHECK(cr.bus_info_crc_valid, "bus info block CRC validates");
    CHECK_EQ_U(cr.bus_info.bus_name, 0x31333934u, "bus name is \"1394\"");
    CHECK_EQ_U(cr.bus_info.guid, 0x00d04b123456789aull, "GUID assembled from both quadlets");
    CHECK_EQ_U(cr.bus_info.vendor_id, 0x00d04bu, "vendor id from GUID high bits");
    CHECK_EQ_U(cr.bus_info.max_rec, 10, "max_rec");
    CHECK_EQ_U(fw_configrom_max_payload(&cr.bus_info), 2048,
               "max payload derived from max_rec");
    CHECK(cr.bus_info.isc, "isochronous capable bit");
    CHECK(!cr.bus_info.bmc, "not bus manager capable");

    CHECK_EQ_U(cr.root_vendor_id, 0x00d04bu, "root directory vendor id");
    CHECK_EQ_U(cr.root_model_id, 0x001234u, "root directory model id");
    CHECK_STR(cr.vendor_text, "ACME", "vendor textual descriptor");

    CHECK_EQ_U(cr.unit_count, 1, "one unit directory found");
    CHECK_EQ_U(cr.unit[0].specifier_id, FW_SPECIFIER_SBP2, "unit specifier_id");
    CHECK_EQ_U(cr.unit[0].version, FW_VERSION_SBP2, "unit version");
    CHECK_EQ_U(cr.unit[0].protocol, FW_UNIT_SBP2, "unit classified as SBP-2");
    CHECK(cr.unit[0].has_sbp2_management_agent, "management agent located");
    CHECK_EQ_U(cr.unit[0].sbp2_management_agent,
               FW_CSR_REGISTER_BASE + 0x400ull * 4,
               "management agent CSR address");
    CHECK_EQ_U(cr.unit[0].sbp2_lun, 0, "logical unit number");
}

static void test_crc_corruption_detected(void)
{
    fw_quadlet_t rom[32];
    size_t len = build_sbp2_rom(rom);

    rom[3] ^= 0x00000010u;   /* flip a bit in the GUID */

    fw_config_rom_t cr;
    fw_err_t err = fw_configrom_parse(rom, len, &cr);

    CHECK_EQ_U(err, FW_ERR_CRC, "a corrupted bus info block is reported");
    CHECK(!cr.bus_info_crc_valid, "CRC flagged invalid");
    /* The parse still populated the structure so a caller can choose to
     * proceed with a device whose ROM CRC is known-bad. */
    CHECK_EQ_U(cr.unit_count, 1, "structure still parsed despite bad CRC");
}

/* An AV/C camcorder unit directory - the DV path. */
static void test_avc_camcorder(void)
{
    fw_quadlet_t rom[32];
    size_t len = build_sbp2_rom(rom);

    /* Retarget the unit to the AV/C protocol. */
    rom[15] = ENTRY(FW_KEY_TYPE_IMMEDIATE, FW_KEY_SPECIFIER_ID, FW_SPECIFIER_1394TA);
    rom[16] = ENTRY(FW_KEY_TYPE_IMMEDIATE, FW_KEY_VERSION,      FW_VERSION_AVC);
    rom[14] = (4u << 16) | fw_crc16(&rom[15], 4);

    fw_config_rom_t cr;
    CHECK_EQ_U(fw_configrom_parse(rom, len, &cr), FW_OK, "AV/C ROM parses");
    CHECK_EQ_U(cr.unit[0].protocol, FW_UNIT_AVC, "unit classified as AV/C");
    CHECK_STR(fw_unit_protocol_name(cr.unit[0].protocol), "AV/C (DV/HDV)",
              "protocol name");
}

static void test_iidc_camera(void)
{
    fw_quadlet_t rom[32];
    size_t len = build_sbp2_rom(rom);

    rom[15] = ENTRY(FW_KEY_TYPE_IMMEDIATE, FW_KEY_SPECIFIER_ID, FW_SPECIFIER_1394TA);
    rom[16] = ENTRY(FW_KEY_TYPE_IMMEDIATE, FW_KEY_VERSION,      FW_VERSION_IIDC_102);
    rom[14] = (4u << 16) | fw_crc16(&rom[15], 4);

    fw_config_rom_t cr;
    CHECK_EQ_U(fw_configrom_parse(rom, len, &cr), FW_OK, "IIDC ROM parses");
    CHECK_EQ_U(cr.unit[0].protocol, FW_UNIT_IIDC, "unit classified as IIDC");
}

static void test_entry_decoding(void)
{
    /* A unit directory entry: type 3 (directory), key 0x11, offset 5. */
    fw_rom_entry_t e = fw_configrom_decode_entry(0xd1000005u, 9);
    CHECK_EQ_U(e.key_type, FW_KEY_TYPE_DIRECTORY, "key type");
    CHECK_EQ_U(e.key_value, FW_KEY_UNIT, "key value");
    CHECK_EQ_U(e.value, 5, "raw value");
    CHECK_EQ_U(e.abs_offset, 14, "offset resolved relative to the entry");

    /* An immediate entry has no absolute offset. */
    fw_rom_entry_t imm = fw_configrom_decode_entry(0x1200609eu, 15);
    CHECK_EQ_U(imm.key_type, FW_KEY_TYPE_IMMEDIATE, "immediate key type");
    CHECK_EQ_U(imm.value, 0x00609eu, "immediate value");
    CHECK_EQ_U(imm.abs_offset, 0, "immediate entry has no offset");
}

/*
 * Truncation and hostile input. A device can present any bytes it likes; the
 * parser must fail cleanly rather than read past the buffer. Run this under
 * ASan (make asan) to prove the bounds checks hold.
 */
static void test_truncation_and_bounds(void)
{
    fw_quadlet_t rom[32];
    size_t len = build_sbp2_rom(rom);
    fw_config_rom_t cr;

    CHECK_EQ_U(fw_configrom_parse(NULL, len, &cr), FW_ERR_INVAL, "NULL ROM");
    CHECK_EQ_U(fw_configrom_parse(rom, 0, &cr), FW_ERR_TOOSMALL, "zero length");
    CHECK_EQ_U(fw_configrom_parse(rom, 4, &cr), FW_ERR_TOOSMALL,
               "ROM shorter than the bus info block");

    /* Every truncation point must be handled without a crash or overread. */
    int bad_returns = 0;
    for (size_t cut = 5; cut < len; cut++) {
        fw_err_t err = fw_configrom_parse(rom, cut, &cr);
        if (err != FW_OK && err != FW_ERR_CRC && err != FW_ERR_TOOSMALL)
            bad_returns++;
    }
    CHECK(bad_returns == 0,
          "every truncation length returns a clean error (%d unexpected)",
          bad_returns);

    /* Wrong bus name is not a 1394 ROM at all. */
    fw_quadlet_t wrong[32];
    build_sbp2_rom(wrong);
    wrong[1] = 0xdeadbeefu;
    CHECK_EQ_U(fw_configrom_parse(wrong, len, &cr), FW_ERR_UNSUPPORTED,
               "non-1394 bus name rejected");

    /* A directory whose length runs past the end of the ROM. */
    fw_quadlet_t overrun[32];
    size_t olen = build_sbp2_rom(overrun);
    overrun[5] = (0x7fffu << 16) | 0;   /* root dir claims 32767 entries */
    CHECK_EQ_U(fw_configrom_parse(overrun, olen, &cr), FW_ERR_TOOSMALL,
               "oversized root directory length rejected");

    /* A leaf pointing outside the ROM must be ignored, not followed. */
    fw_quadlet_t badleaf[32];
    size_t blen = build_sbp2_rom(badleaf);
    badleaf[8] = ENTRY(FW_KEY_TYPE_LEAF, FW_KEY_DESCRIPTOR, 0xfffff0u);
    badleaf[5] = (4u << 16) | fw_crc16(&badleaf[6], 4);
    fw_err_t err = fw_configrom_parse(badleaf, blen, &cr);
    CHECK(err == FW_OK || err == FW_ERR_CRC,
          "out-of-range leaf offset is ignored rather than followed");
    CHECK_STR(cr.vendor_text, "", "no text extracted from an invalid leaf");
}

void test_configrom(void)
{
    TEST_SUITE("fw_configrom");
    test_entry_decoding();
    test_sbp2_disk();
    test_crc_corruption_detected();
    test_avc_camcorder();
    test_iidc_camera();
    test_truncation_and_bounds();
}
