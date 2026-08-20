/*
 * fw_configrom.h - Configuration ROM parsing.
 *
 * Every 1394 node exposes a config ROM at FW_CONFIG_ROM_BASE describing who
 * it is and which protocols its units speak. Reading and parsing it is how we
 * decide whether a device is a disk (SBP-2), a camcorder (AV/C), or an audio
 * interface, and therefore which upper layer should claim it.
 */

#ifndef FW_CONFIGROM_H
#define FW_CONFIGROM_H

#include "fw_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Directory entry key types, from the top two bits of an entry. */
typedef enum {
    FW_KEY_TYPE_IMMEDIATE = 0,  /* value is inline in the low 24 bits */
    FW_KEY_TYPE_CSR_OFFSET = 1, /* value is an offset from the CSR base */
    FW_KEY_TYPE_LEAF = 2,       /* value is a relative offset to a leaf */
    FW_KEY_TYPE_DIRECTORY = 3,  /* value is a relative offset to a directory */
} fw_key_type_t;

/* Key values we care about (IEEE 1212 and the 1394 TA registry). */
#define FW_KEY_VENDOR              0x03
#define FW_KEY_HARDWARE_VERSION    0x04
#define FW_KEY_NODE_CAPABILITIES   0x0c
#define FW_KEY_UNIT                0x11  /* with type=directory -> 0xd1 */
#define FW_KEY_SPECIFIER_ID        0x12
#define FW_KEY_VERSION             0x13
#define FW_KEY_DEPENDENT_INFO      0x14
#define FW_KEY_MODEL               0x17
#define FW_KEY_DESCRIPTOR          0x01  /* with type=leaf -> 0x81 */

/* SBP-2 specific keys, used to find the login agent and enumerate LUNs. */
#define FW_KEY_SBP2_COMMAND_SET_SPEC_ID  0x38
#define FW_KEY_SBP2_COMMAND_SET          0x39
#define FW_KEY_SBP2_UNIT_CHARACTERISTICS 0x3a
#define FW_KEY_SBP2_COMMAND_SET_REVISION 0x3b
#define FW_KEY_SBP2_FIRMWARE_REVISION    0x3c

/*
 * Well-known (specifier_id, version) pairs that identify a unit's protocol.
 * These are what let us route a device to the right upper layer.
 */
#define FW_SPECIFIER_SBP2          0x00609eu
#define FW_VERSION_SBP2            0x010483u

#define FW_SPECIFIER_1394TA        0x00a02du
#define FW_VERSION_AVC             0x010001u  /* AV/C - DV and HDV cameras */
#define FW_VERSION_IIDC_100        0x000100u  /* IIDC machine vision 1.04 */
#define FW_VERSION_IIDC_101        0x000101u
#define FW_VERSION_IIDC_102        0x000102u

/* Protocol classification produced by fw_configrom_classify_unit(). */
typedef enum {
    FW_UNIT_UNKNOWN = 0,
    FW_UNIT_SBP2,       /* storage */
    FW_UNIT_AVC,        /* DV / HDV camcorder */
    FW_UNIT_IIDC,       /* machine vision camera */
    FW_UNIT_AUDIO,      /* an audio interface (see fw_audio_flavor_t) */
} fw_unit_protocol_t;

/* The bus info block: the first five quadlets of every config ROM. */
typedef struct {
    uint8_t  info_length;     /* bus info block length, in quadlets */
    uint8_t  crc_length;
    uint16_t rom_crc;
    uint32_t bus_name;        /* must be 0x31333934, i.e. "1394" */

    bool     irmc;            /* isochronous resource manager capable */
    bool     cmc;             /* cycle master capable */
    bool     isc;             /* isochronous capable */
    bool     bmc;             /* bus manager capable */
    bool     pmc;             /* power manager capable */
    uint8_t  cyc_clk_acc;
    uint8_t  max_rec;         /* max payload = 2^(max_rec+1) bytes */
    uint8_t  max_rom;
    uint8_t  generation;
    uint8_t  link_speed;

    uint64_t guid;            /* EUI-64: vendor id + chip id */
    uint32_t vendor_id;       /* top 24 bits of the GUID */
} fw_bus_info_t;

/* One entry from a directory. */
typedef struct {
    uint8_t  key_type;
    uint8_t  key_value;
    uint32_t value;           /* low 24 bits, meaning depends on key_type */
    uint32_t abs_offset;      /* for leaf/directory: index into the ROM */
} fw_rom_entry_t;

/* A parsed unit directory. */
typedef struct {
    uint32_t specifier_id;
    uint32_t version;
    uint32_t model_id;
    uint32_t vendor_id;
    bool     has_specifier_id;
    bool     has_version;

    /* SBP-2: absolute CSR address of the management agent, and the LUN. */
    uint64_t sbp2_management_agent;
    uint16_t sbp2_lun;
    bool     has_sbp2_management_agent;

    uint32_t directory_offset;  /* where this unit directory starts */
    fw_unit_protocol_t protocol;
} fw_unit_t;

#define FW_MAX_UNITS 8

/* Everything we extract from one node's ROM. */
typedef struct {
    fw_bus_info_t bus_info;
    uint32_t      root_vendor_id;
    uint32_t      root_model_id;
    bool          bus_info_crc_valid;

    fw_unit_t     unit[FW_MAX_UNITS];
    uint8_t       unit_count;

    /* Text pulled from the first textual descriptor leaf under the root and
     * under each unit, when the leaf uses minimal ASCII. */
    char          vendor_text[64];
    char          model_text[64];
} fw_config_rom_t;

/*
 * Parse a config ROM image.
 *
 * `rom` is the ROM in host byte order, `length` its size in quadlets. The
 * parser is bounds-checked throughout: a malicious or damaged ROM can cause a
 * parse failure but never a read outside the buffer.
 *
 * Returns FW_OK on success. FW_ERR_CRC means the bus info block failed its
 * CRC but the structure was still parsed, so the caller may choose to
 * continue; every other error means `out` is not usable.
 */
fw_err_t fw_configrom_parse(const fw_quadlet_t *rom, size_t length,
                            fw_config_rom_t *out);

/* Decode a single directory entry quadlet. */
fw_rom_entry_t fw_configrom_decode_entry(fw_quadlet_t q, uint32_t entry_offset);

/* Classify a unit from its specifier_id/version pair. */
fw_unit_protocol_t fw_configrom_classify_unit(const fw_unit_t *unit);

/* Maximum async payload this node accepts, derived from max_rec. */
uint32_t fw_configrom_max_payload(const fw_bus_info_t *info);

const char *fw_unit_protocol_name(fw_unit_protocol_t p);

#ifdef __cplusplus
}
#endif

#endif /* FW_CONFIGROM_H */
