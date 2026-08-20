/*
 * ohci_regs.h - 1394 Open Host Controller Interface register map.
 *
 * Offsets and bit definitions follow the 1394 Open Host Controller Interface
 * Specification, release 1.1. All registers are 32-bit and little-endian in
 * the controller's BAR0 MMIO window.
 *
 * Registers whose name ends in Set/Clear are write-1-to-modify pairs: writing
 * a 1 to a bit in the Set register sets it, writing a 1 in the Clear register
 * clears it, and reading either returns the current value. Never
 * read-modify-write these.
 */

#ifndef OHCI_REGS_H
#define OHCI_REGS_H

#include "fw_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The OHCI controller is a PCI device of this class. Matching on class code
 * rather than vendor/device catches every OHCI part - TI, LSI/Agere, VIA,
 * NEC - including the one inside Apple's Thunderbolt-to-FireWire adapter. */
#define OHCI_PCI_CLASS_CODE     0x0c0010u  /* serial bus / FireWire / OHCI */
#define OHCI_PCI_CLASS_MASK     0xffffffu

/* ---- Core registers ---- */
#define OHCI_Version                    0x000
#define OHCI_GUID_ROM                   0x004
#define OHCI_ATRetries                  0x008
#define OHCI_CSRData                    0x00c
#define OHCI_CSRCompareData             0x010
#define OHCI_CSRControl                 0x014
#define OHCI_ConfigROMhdr               0x018
#define OHCI_BusID                      0x01c
#define OHCI_BusOptions                 0x020
#define OHCI_GUIDHi                     0x024
#define OHCI_GUIDLo                     0x028
#define OHCI_ConfigROMmap               0x034
#define OHCI_PostedWriteAddressLo       0x038
#define OHCI_PostedWriteAddressHi       0x03c
#define OHCI_VendorID                   0x040

#define OHCI_HCControlSet               0x050
#define OHCI_HCControlClear             0x054

#define OHCI_SelfIDBuffer               0x064
#define OHCI_SelfIDCount                0x068

#define OHCI_IRMultiChanMaskHiSet       0x070
#define OHCI_IRMultiChanMaskHiClear     0x074
#define OHCI_IRMultiChanMaskLoSet       0x078
#define OHCI_IRMultiChanMaskLoClear     0x07c

#define OHCI_IntEventSet                0x080
#define OHCI_IntEventClear              0x084
#define OHCI_IntMaskSet                 0x088
#define OHCI_IntMaskClear               0x08c

#define OHCI_IsoXmitIntEventSet         0x090
#define OHCI_IsoXmitIntEventClear       0x094
#define OHCI_IsoXmitIntMaskSet          0x098
#define OHCI_IsoXmitIntMaskClear        0x09c

#define OHCI_IsoRecvIntEventSet         0x0a0
#define OHCI_IsoRecvIntEventClear       0x0a4
#define OHCI_IsoRecvIntMaskSet          0x0a8
#define OHCI_IsoRecvIntMaskClear        0x0ac

#define OHCI_InitialBandwidthAvailable  0x0b0
#define OHCI_InitialChannelsAvailableHi 0x0b4
#define OHCI_InitialChannelsAvailableLo 0x0b8
#define OHCI_FairnessControl            0x0dc

#define OHCI_LinkControlSet             0x0e0
#define OHCI_LinkControlClear           0x0e4
#define OHCI_NodeID                     0x0e8
#define OHCI_PhyControl                 0x0ec
#define OHCI_IsochronousCycleTimer      0x0f0

#define OHCI_AsyncRequestFilterHiSet    0x100
#define OHCI_AsyncRequestFilterHiClear  0x104
#define OHCI_AsyncRequestFilterLoSet    0x108
#define OHCI_AsyncRequestFilterLoClear  0x10c
#define OHCI_PhysicalRequestFilterHiSet   0x110
#define OHCI_PhysicalRequestFilterHiClear 0x114
#define OHCI_PhysicalRequestFilterLoSet   0x118
#define OHCI_PhysicalRequestFilterLoClear 0x11c
#define OHCI_PhysicalUpperBound         0x120

/* ---- DMA context register blocks ----
 *
 * Each async context is a 16-byte block: ContextControlSet, ContextControlClear,
 * CommandPtr, reserved.
 */
#define OHCI_AsyncReqTrContextBase      0x180  /* AT request  */
#define OHCI_AsyncRspTrContextBase      0x1a0  /* AT response */
#define OHCI_AsyncReqRcvContextBase     0x1c0  /* AR request  */
#define OHCI_AsyncRspRcvContextBase     0x1e0  /* AR response */

#define OHCI_ContextControlSet          0x000
#define OHCI_ContextControlClear        0x004
#define OHCI_CommandPtr                 0x00c

/* Isochronous transmit contexts: 16 bytes each starting at 0x200. */
#define OHCI_IsoXmitContextBase(n)      (0x200u + (n) * 0x10u)
/* Isochronous receive contexts: 32 bytes each starting at 0x400. */
#define OHCI_IsoRecvContextBase(n)      (0x400u + (n) * 0x20u)
#define OHCI_IsoRecvContextMatch        0x010  /* offset within an IR block */

#define OHCI_MAX_ISO_CONTEXTS           32

/* ---- HCControl bits (OHCI 1.1 section 5.7) ---- */
#define OHCI_HCControl_softReset        (1u << 16)
#define OHCI_HCControl_linkEnable       (1u << 17)
#define OHCI_HCControl_postedWriteEnable (1u << 18)
#define OHCI_HCControl_LPS              (1u << 19)  /* link power status */
#define OHCI_HCControl_aPhyEnhanceEnable (1u << 22)
#define OHCI_HCControl_programPhyEnable (1u << 23)
#define OHCI_HCControl_noByteSwapData   (1u << 30)
#define OHCI_HCControl_BIBimageValid    (1u << 31)

/* ---- IntEvent / IntMask bits (OHCI 1.1 section 6.1) ---- */
#define OHCI_Int_reqTxComplete          (1u << 0)
#define OHCI_Int_respTxComplete         (1u << 1)
#define OHCI_Int_ARRQ                   (1u << 2)
#define OHCI_Int_ARRS                   (1u << 3)
#define OHCI_Int_RQPkt                  (1u << 4)
#define OHCI_Int_RSPkt                  (1u << 5)
#define OHCI_Int_isochTx                (1u << 6)
#define OHCI_Int_isochRx                (1u << 7)
#define OHCI_Int_postedWriteErr         (1u << 8)
#define OHCI_Int_lockRespErr            (1u << 9)
#define OHCI_Int_selfIDComplete          (1u << 16)
#define OHCI_Int_busReset               (1u << 17)
#define OHCI_Int_regAccessFail          (1u << 18)
#define OHCI_Int_phy                    (1u << 19)
#define OHCI_Int_cycleSynch             (1u << 20)
#define OHCI_Int_cycle64Seconds         (1u << 21)
#define OHCI_Int_cycleLost              (1u << 22)
#define OHCI_Int_cycleInconsistent      (1u << 23)
#define OHCI_Int_unrecoverableError     (1u << 24)
#define OHCI_Int_cycleTooLong           (1u << 25)
#define OHCI_Int_phyRegRcvd             (1u << 26)
#define OHCI_Int_ack_tardy              (1u << 27)
#define OHCI_Int_softInterrupt          (1u << 29)
#define OHCI_Int_vendorSpecific         (1u << 30)
#define OHCI_Int_masterIntEnable        (1u << 31)

/* ---- LinkControl bits ---- */
#define OHCI_LinkControl_cycleSynch     (1u << 6)
#define OHCI_LinkControl_cycleTimerEnable (1u << 20)
#define OHCI_LinkControl_cycleMaster    (1u << 21)
#define OHCI_LinkControl_cycleSource    (1u << 22)
#define OHCI_LinkControl_rcvSelfID      (1u << 9)
#define OHCI_LinkControl_rcvPhyPkt      (1u << 10)

/* ---- ContextControl bits ---- */
#define OHCI_ContextControl_run         (1u << 15)
#define OHCI_ContextControl_wake        (1u << 12)
#define OHCI_ContextControl_dead        (1u << 11)
#define OHCI_ContextControl_active      (1u << 10)
#define OHCI_ContextControl_eventCodeMask 0x1fu
/* IR-only bits. */
#define OHCI_ContextControl_bufferFill      (1u << 31)
#define OHCI_ContextControl_isochHeader     (1u << 30)
#define OHCI_ContextControl_cycleMatchEnable (1u << 29)
#define OHCI_ContextControl_multiChanMode   (1u << 28)
#define OHCI_ContextControl_dualBufferMode  (1u << 27)

/* ---- PhyControl ---- */
#define OHCI_PhyControl_Read(addr)      ((((uint32_t)(addr)) << 8) | (1u << 15))
#define OHCI_PhyControl_Write(addr, d)  ((((uint32_t)(addr)) << 8) | ((uint32_t)(d) & 0xffu) | (1u << 14))
#define OHCI_PhyControl_ReadDone        (1u << 31)
#define OHCI_PhyControl_WriteDone       (1u << 14)
#define OHCI_PhyControl_ReadDataShift   16

/* ---- PHY registers (accessed through PhyControl) ---- */
#define OHCI_PHY_REG_1                  1
#define OHCI_PHY1_IBR                   (1u << 6)   /* initiate bus reset */
#define OHCI_PHY1_RHB                   (1u << 7)   /* root hold-off */
#define OHCI_PHY1_GAP_COUNT_MASK        0x3fu

/* ---- NodeID ---- */
#define OHCI_NodeID_nodeNumberMask      0x3fu
#define OHCI_NodeID_busNumberShift      6
#define OHCI_NodeID_busNumberMask       0x3ffu
#define OHCI_NodeID_CPS                 (1u << 27)  /* cable power status */
#define OHCI_NodeID_root                (1u << 30)
#define OHCI_NodeID_idValid             (1u << 31)

/* ---- SelfIDCount ---- */
#define OHCI_SelfIDCount_selfIDError    (1u << 31)
/*
 * selfIDSize occupies bits [10:2] and counts QUADLETS written into the self-ID
 * buffer, including the one-quadlet header the controller writes first.
 *
 * Verified against real hardware: an LSI FW643 with two nodes on the bus
 * reported 0x00020014, i.e. 5 quadlets = header + 2 self-ID packets of 2
 * quadlets each (value followed by its inverse). Reading this field as [10:3]
 * yields 2, which then computes an odd quadlet count and fails to parse.
 */
#define OHCI_SelfIDCount_sizeShift      2
#define OHCI_SelfIDCount_sizeMask       0x1ffu  /* quadlets, including header */
#define OHCI_SelfIDCount_generationShift 16
#define OHCI_SelfIDCount_generationMask 0xffu

/* ---- DMA descriptor command types (OHCI 1.1 section 3.1) ---- */
#define OHCI_DESC_OUTPUT_MORE           0x0
#define OHCI_DESC_OUTPUT_LAST           0x1
#define OHCI_DESC_INPUT_MORE            0x2
#define OHCI_DESC_INPUT_LAST            0x3
#define OHCI_DESC_STORE_VALUE           0x8
#define OHCI_DESC_CMD_SHIFT             28

/* Descriptor control bits, in the first quadlet alongside the command. */
#define OHCI_DESC_KEY_SHIFT             24   /* key: 0 = data, 2 = immediate */
#define OHCI_DESC_KEY_IMMEDIATE         2
#define OHCI_DESC_INTERRUPT_SHIFT       20   /* 3 = interrupt always */
#define OHCI_DESC_INTERRUPT_ALWAYS      3
#define OHCI_DESC_BRANCH_SHIFT          18   /* 3 = branch always */
#define OHCI_DESC_BRANCH_ALWAYS         3
#define OHCI_DESC_WAIT_SHIFT            16
#define OHCI_DESC_PING                  (1u << 23)

/*
 * A 1394 OHCI DMA descriptor. Sixteen bytes, and the controller requires the
 * whole descriptor block to be 16-byte aligned and physically contiguous.
 * Fields are little-endian on the wire regardless of host endianness.
 */
typedef struct __attribute__((packed, aligned(16))) {
    uint32_t control;      /* command, key, interrupt, branch, reqCount */
    uint32_t data_address;  /* physical address of the payload */
    uint32_t branch_address; /* next descriptor | Z value in low 4 bits */
    uint32_t status;        /* xferStatus in high half, resCount in low */
} ohci_descriptor_t;

/* Build the control quadlet of a descriptor. */
static inline uint32_t ohci_desc_control(uint32_t cmd, uint32_t key,
                                         uint32_t interrupt, uint32_t branch,
                                         uint32_t req_count)
{
    return (cmd << OHCI_DESC_CMD_SHIFT) |
           (key << OHCI_DESC_KEY_SHIFT) |
           (interrupt << OHCI_DESC_INTERRUPT_SHIFT) |
           (branch << OHCI_DESC_BRANCH_SHIFT) |
           (req_count & 0xffffu);
}

#ifdef __cplusplus
}
#endif

#endif /* OHCI_REGS_H */
