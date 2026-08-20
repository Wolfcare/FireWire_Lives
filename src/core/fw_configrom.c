#include "fw/fw_configrom.h"
#include "fw/fw_crc.h"

#include <string.h>

#define BUS_NAME_1394 0x31333934u  /* "1394" in ASCII */

fw_rom_entry_t fw_configrom_decode_entry(fw_quadlet_t q, uint32_t entry_offset)
{
    fw_rom_entry_t e;

    e.key_type   = (uint8_t)((q >> 30) & 0x3u);
    e.key_value  = (uint8_t)((q >> 24) & 0x3fu);
    e.value      = q & 0x00ffffffu;

    /* Leaf and directory entries store an offset in quadlets, relative to the
     * position of the entry itself. */
    if (e.key_type == FW_KEY_TYPE_LEAF || e.key_type == FW_KEY_TYPE_DIRECTORY)
        e.abs_offset = entry_offset + e.value;
    else
        e.abs_offset = 0;

    return e;
}

uint32_t fw_configrom_max_payload(const fw_bus_info_t *info)
{
    if (info == NULL || info->max_rec == 0)
        return 512;  /* the spec's floor when max_rec is unspecified */

    /* max_rec encodes payload as 2^(max_rec+1), capped at 4096 in practice. */
    if (info->max_rec >= 12)
        return 8192;

    return 1u << (info->max_rec + 1);
}

fw_unit_protocol_t fw_configrom_classify_unit(const fw_unit_t *unit)
{
    if (unit == NULL || !unit->has_specifier_id || !unit->has_version)
        return FW_UNIT_UNKNOWN;

    if (unit->specifier_id == FW_SPECIFIER_SBP2 &&
        unit->version == FW_VERSION_SBP2)
        return FW_UNIT_SBP2;

    if (unit->specifier_id == FW_SPECIFIER_1394TA) {
        if (unit->version == FW_VERSION_AVC)
            return FW_UNIT_AVC;
        if (unit->version == FW_VERSION_IIDC_100 ||
            unit->version == FW_VERSION_IIDC_101 ||
            unit->version == FW_VERSION_IIDC_102)
            return FW_UNIT_IIDC;
    }

    /* Audio interfaces are identified by vendor-specific specifier_ids rather
     * than a single registered protocol; fw_audio.c owns that table and
     * refines FW_UNIT_UNKNOWN into FW_UNIT_AUDIO where it recognises one. */
    return FW_UNIT_UNKNOWN;
}

const char *fw_unit_protocol_name(fw_unit_protocol_t p)
{
    switch (p) {
    case FW_UNIT_SBP2:  return "SBP-2 (storage)";
    case FW_UNIT_AVC:   return "AV/C (DV/HDV)";
    case FW_UNIT_IIDC:  return "IIDC (camera)";
    case FW_UNIT_AUDIO: return "audio";
    default:            return "unknown";
    }
}

static fw_err_t parse_bus_info(const fw_quadlet_t *rom, size_t length,
                               fw_bus_info_t *info, bool *crc_valid)
{
    if (length < 5)
        return FW_ERR_TOOSMALL;

    fw_quadlet_t hdr = rom[0];
    info->info_length = (uint8_t)((hdr >> 24) & 0xffu);
    info->crc_length  = (uint8_t)((hdr >> 16) & 0xffu);
    info->rom_crc     = (uint16_t)(hdr & 0xffffu);
    info->bus_name    = rom[1];

    if (info->bus_name != BUS_NAME_1394)
        return FW_ERR_UNSUPPORTED;

    /* A general-format ROM has info_length >= 4. Anything less is a minimal
     * ROM carrying only a 24-bit vendor id, which has no directories. */
    if (info->info_length < 4)
        return FW_ERR_UNSUPPORTED;

    fw_quadlet_t opts = rom[2];
    info->irmc        = (opts & (1u << 31)) != 0;
    info->cmc         = (opts & (1u << 30)) != 0;
    info->isc         = (opts & (1u << 29)) != 0;
    info->bmc         = (opts & (1u << 28)) != 0;
    info->pmc         = (opts & (1u << 27)) != 0;
    info->cyc_clk_acc = (uint8_t)((opts >> 16) & 0xffu);
    info->max_rec     = (uint8_t)((opts >> 12) & 0xfu);
    info->max_rom     = (uint8_t)((opts >> 8) & 0x3u);
    info->generation  = (uint8_t)((opts >> 4) & 0xfu);
    info->link_speed  = (uint8_t)(opts & 0x7u);

    info->guid = ((uint64_t)rom[3] << 32) | (uint64_t)rom[4];
    info->vendor_id = (uint32_t)(rom[3] >> 8);

    /* The stored CRC covers crc_length quadlets starting immediately after
     * the header quadlet. */
    *crc_valid = false;
    if (info->crc_length > 0 && (size_t)info->crc_length + 1 <= length) {
        uint16_t computed = fw_crc16(&rom[1], info->crc_length);
        *crc_valid = (computed == info->rom_crc);
    }

    return FW_OK;
}

/*
 * Copy a textual descriptor leaf into `dst`.
 *
 * Layout: a leaf header quadlet (length + CRC), then two quadlets of
 * specifier/width/character-set metadata, then the text itself. We only
 * handle the minimal-ASCII case (specifier 0, character set 0), which is what
 * essentially every real device ships.
 */
static void extract_text(const fw_quadlet_t *rom, size_t length,
                         uint32_t leaf_offset, char *dst, size_t dst_size)
{
    if (leaf_offset == 0 || leaf_offset >= length)
        return;

    uint16_t leaf_len = (uint16_t)((rom[leaf_offset] >> 16) & 0xffffu);
    if (leaf_len < 3)
        return;

    /* Bounds: header + leaf_len quadlets must fit. */
    if ((size_t)leaf_offset + 1 + leaf_len > length)
        return;

    const fw_quadlet_t *body = &rom[leaf_offset + 1];

    /* body[0] = descriptor/specifier, body[1] = width/character set/language.
     * Non-zero specifier means a vendor-defined encoding we do not decode. */
    if (body[0] != 0 || body[1] != 0)
        return;

    size_t text_quadlets = (size_t)leaf_len - 2;
    size_t out = 0;

    for (size_t i = 0; i < text_quadlets && out + 1 < dst_size; i++) {
        fw_quadlet_t q = body[2 + i];
        for (int b = 3; b >= 0 && out + 1 < dst_size; b--) {
            char c = (char)((q >> (b * 8)) & 0xffu);
            if (c == '\0')
                goto done;
            dst[out++] = c;
        }
    }

done:
    dst[out] = '\0';

    /* Devices commonly pad with trailing spaces. */
    while (out > 0 && dst[out - 1] == ' ')
        dst[--out] = '\0';
}

static void parse_unit_directory(const fw_quadlet_t *rom, size_t length,
                                 uint32_t dir_offset, fw_unit_t *unit,
                                 char *model_text, size_t model_text_size)
{
    if (dir_offset >= length)
        return;

    uint16_t dir_len = (uint16_t)((rom[dir_offset] >> 16) & 0xffffu);
    if ((size_t)dir_offset + 1 + dir_len > length)
        return;

    unit->directory_offset = dir_offset;

    for (uint16_t i = 0; i < dir_len; i++) {
        uint32_t entry_off = dir_offset + 1 + i;
        fw_rom_entry_t e = fw_configrom_decode_entry(rom[entry_off], entry_off);

        switch (e.key_value) {
        case FW_KEY_SPECIFIER_ID:
            if (e.key_type == FW_KEY_TYPE_IMMEDIATE) {
                unit->specifier_id = e.value;
                unit->has_specifier_id = true;
            }
            break;

        case FW_KEY_VERSION:
            if (e.key_type == FW_KEY_TYPE_IMMEDIATE) {
                unit->version = e.value;
                unit->has_version = true;
            }
            break;

        case FW_KEY_MODEL:
            if (e.key_type == FW_KEY_TYPE_IMMEDIATE)
                unit->model_id = e.value;
            break;

        case FW_KEY_VENDOR:
            if (e.key_type == FW_KEY_TYPE_IMMEDIATE)
                unit->vendor_id = e.value;
            break;

        case FW_KEY_DEPENDENT_INFO:
            /* In an SBP-2 unit directory this key appears twice: as a CSR
             * offset it locates the management agent, as an immediate it
             * carries the logical unit number. */
            if (e.key_type == FW_KEY_TYPE_CSR_OFFSET) {
                unit->sbp2_management_agent =
                    FW_CSR_REGISTER_BASE + ((uint64_t)e.value * 4u);
                unit->has_sbp2_management_agent = true;
            } else if (e.key_type == FW_KEY_TYPE_IMMEDIATE) {
                unit->sbp2_lun = (uint16_t)(e.value & 0xffffu);
            }
            break;

        case FW_KEY_DESCRIPTOR:
            if (e.key_type == FW_KEY_TYPE_LEAF && model_text != NULL &&
                model_text[0] == '\0')
                extract_text(rom, length, e.abs_offset, model_text,
                             model_text_size);
            break;

        default:
            break;
        }
    }

    unit->protocol = fw_configrom_classify_unit(unit);
}

fw_err_t fw_configrom_parse(const fw_quadlet_t *rom, size_t length,
                            fw_config_rom_t *out)
{
    if (rom == NULL || out == NULL)
        return FW_ERR_INVAL;

    memset(out, 0, sizeof(*out));

    bool crc_valid = false;
    fw_err_t err = parse_bus_info(rom, length, &out->bus_info, &crc_valid);
    if (err != FW_OK)
        return err;

    out->bus_info_crc_valid = crc_valid;

    /* The root directory begins right after the bus info block. */
    uint32_t root_offset = out->bus_info.info_length + 1u;
    if (root_offset >= length)
        return FW_ERR_TOOSMALL;

    uint16_t root_len = (uint16_t)((rom[root_offset] >> 16) & 0xffffu);
    if ((size_t)root_offset + 1 + root_len > length)
        return FW_ERR_TOOSMALL;

    for (uint16_t i = 0; i < root_len; i++) {
        uint32_t entry_off = root_offset + 1 + i;
        fw_rom_entry_t e = fw_configrom_decode_entry(rom[entry_off], entry_off);

        switch (e.key_value) {
        case FW_KEY_VENDOR:
            if (e.key_type == FW_KEY_TYPE_IMMEDIATE)
                out->root_vendor_id = e.value;
            else if (e.key_type == FW_KEY_TYPE_LEAF)
                extract_text(rom, length, e.abs_offset, out->vendor_text,
                             sizeof(out->vendor_text));
            break;

        case FW_KEY_MODEL:
            if (e.key_type == FW_KEY_TYPE_IMMEDIATE)
                out->root_model_id = e.value;
            break;

        case FW_KEY_DESCRIPTOR:
            if (e.key_type == FW_KEY_TYPE_LEAF && out->vendor_text[0] == '\0')
                extract_text(rom, length, e.abs_offset, out->vendor_text,
                             sizeof(out->vendor_text));
            break;

        case FW_KEY_UNIT:
            if (e.key_type == FW_KEY_TYPE_DIRECTORY &&
                out->unit_count < FW_MAX_UNITS) {
                fw_unit_t *unit = &out->unit[out->unit_count];
                parse_unit_directory(rom, length, e.abs_offset, unit,
                                     out->model_text, sizeof(out->model_text));
                out->unit_count++;
            }
            break;

        default:
            break;
        }
    }

    return crc_valid ? FW_OK : FW_ERR_CRC;
}
