// LikeOS-64 Intel e1000e (PCIe gigabit) NIC Driver
//
// Targets (verified working on real hardware and/or hypervisor):
//   - Intel 82574L Gigabit Network Connection                 (PCI 0x10D3)
//       Used by QEMU `-device e1000e`.  Original target of this driver.
//   - Intel 82583V Gigabit Network Connection                 (PCI 0x150C)
//   - Intel I217-LM / I217-V                                  (PCH LPT LOM)
//   - Intel I218-LM / I218-V / I218-LM2 / I218-V2 / I218-LM3  (PCH LPT-LP/WPT LOM)
//   - Intel I219-LM / I219-V family, generations 1..23        (PCH SPT-H through MTP/RPL/ADP)
//       Verified on Lenovo ThinkPad P50 (PCH SPT-H, dev 0x156F).
//
// Note: The VirtualBox NIC choices "Intel PRO/1000 MT Desktop" (82540EM)
// and "Intel PRO/1000 MT Server" (82545EM) are e1000-class parts and are
// handled by the separate e1000 driver, not this one.
//
// ---------------------------------------------------------------------------
// Implementation status
// ---------------------------------------------------------------------------
// The 82574L register layout is largely backwards-compatible with the classic
// e1000 (82540EM/82545EM): we reuse the legacy RX/TX descriptor formats and
// the same MMIO offsets defined in <kernel/e1000.h>.  The extra PCIe / MSI-X
// facilities that distinguish e1000e proper from e1000 are NOT used here —
// the legacy programming model is sufficient for our purposes (UEFI desktop
// systems, hypervisor emulation, business-laptop LOMs) and matches the
// interrupt/DMA-handling style of the existing e1000 driver.
//
// What works:
//   * Probe / BAR0 MMIO mapping (uncached) for all supported parts.
//   * Full ICH8/9/10 + PCH (LPT/SPT/CNP/ICP/CMP/TGP/ADP/MTP/RPL) bring-up:
//       - PHY ULP exit (CV_SMB_CTRL / I218_ULP_CONFIG1 / FEXTNVM7 handshake)
//       - LANPHYPC toggle and LAN_INIT_DONE wait on cold boot
//       - PCIe master-disable drain + CTRL.RST sequenced reset
//       - CTRL_EXT (DRV_LOAD / PHY_PDEN / bit22) re-programmed post-reset
//       - PBA reprogrammed for SPT family
//       - HV_OEM_BITS LPLU / GBE_DIS cleared (BIOS hands the PHY off with
//         LPLU=1 on Lenovo P50; without this TX hangs on link-up)
//       - K1 disabled, KMRN HD_CTRL programmed for negotiated speed
//       - K1 workaround for LPT-LP / SPT / CNP+ at 100 Mb FD
//       - EEE disabled at MAC and PHY (EEER / IPCNFG / EMI 0x040E)
//       - I82577 PHY config (CRS_ON_TX, downshift=3)
//       - SPT TX-hang workaround: IOSFPC bit 16, FEXTNVM11.DISABLE_MULR_FIX,
//         TARC0 CB_MULTIQ_2_REQ
//       - TCTL.MULR + RRTHRESH set on PCH-LAN (the actual TX-stall gate;
//         without MULR the TX engine fetches one descriptor and stalls)
//       - KABGTXD.BGSQLBIAS, MAC DPG exit, ME H2ME ENFORCE_SETTINGS
//   * Legacy RX/TX descriptor rings, single-queue, contiguous DMA.
//   * Legacy INTx via ACPI _PRT GSI lookup, or MSI when available.
//   * Link state tracking via LSC interrupt + STATUS.LU polling.
//   * MAC address read from RAL0/RAH0 (UEFI-programmed) with EEPROM fallback.
//
// What is intentionally NOT implemented:
//   * MSI-X, multi-queue RX/TX, RSS, VMDq, SR-IOV.
//   * TX/RX checksum offload, TSO/LSO, header-split.
//   * VLAN tagging (CTRL.VME left at hardware default).
//   * Wake-on-LAN / D3-cold (CTRL.ADVD3WUC left at hardware default).
//   * Energy-Efficient Ethernet (explicitly disabled — saves debugging pain).
//   * NVM / EEPROM writes, firmware update path.
//
// Diagnostics:
//   By default the driver prints only the e1000-style essentials (probe,
//   MAC, IRQ/MSI selection, link state, init-success, allocation errors).
//   Set E1000E_DBG to 1 (just below the includes) to enable the full
//   verbose register-dump / state-trace firehose used during bring-up.
// ---------------------------------------------------------------------------
#include "../../../include/kernel/e1000e.h"
#include "../../../include/kernel/e1000.h"
#include "../../../include/kernel/net.h"
#include "../../../include/kernel/pci.h"
#include "../../../include/kernel/memory.h"
#include "../../../include/kernel/console.h"
#include "../../../include/kernel/interrupt.h"
#include "../../../include/kernel/slab.h"
#include "../../../include/kernel/lapic.h"
#include "../../../include/kernel/ioapic.h"
#include "../../../include/kernel/acpi.h"
#include "../../../include/kernel/smp.h"
#include "../../../include/kernel/timer.h"

// ============================================================================
// Debug / diagnostic logging.
//
// Set E1000E_DBG to 1 to enable verbose driver diagnostics (register dumps,
// PHY/MAC state transitions, TX/RX path tracing).  When 0 (the default)
// every e1000e_dbg() call collapses to a no-op and is fully eliminated by
// the compiler, leaving zero overhead and a silent driver.
//
// All informational/diagnostic output in this file goes through e1000e_dbg();
// only true error-recovery surface should ever call kprintf directly
// (currently nothing does).
// ============================================================================
#define E1000E_DBG 0

#if E1000E_DBG
static void e1000e_dbg(const char* fmt, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    kvprintf(fmt, ap);
    __builtin_va_end(ap);
}
#else
// When debug is off, e1000e_dbg() collapses to (void)0 via a macro so the
// variadic arguments are NEVER evaluated at the call site.  This is critical
// because most diagnostic calls feed live MMIO reads (e1000e_read(...)) as
// arguments — leaving those reads in the binary would defeat the purpose of
// disabling debug.  The do-while idiom keeps the call statement-safe in
// every context (if/else without braces, etc.).
#define e1000e_dbg(...) do { } while (0)
#endif

// ============================================================================
// Supported PCI device IDs.  Compiled from publicly documented
// hw.h (E1000_DEV_ID_*).  Covers the
// entire 8257x family plus 80003ES2LAN, ICH8/9/10, and the PCH (LPT, SPT,
// CNP, ICP, CMP, TGP, ADP, MTP, RPL) I217/I218/I219 LOM parts that ship
// in virtually every Intel-based business laptop and desktop board from
// 2012 onwards (including the Lenovo ThinkPad P50's I219-LM at 0x156F).
// ============================================================================
typedef struct { uint16_t did; const char* name; } e1000e_id_t;

static const e1000e_id_t e1000e_pci_ids[] = {
    // ---- 82571 family ----
    { 0x105E, "82571EB Copper (dual)" },
    { 0x105F, "82571EB Fiber (dual)"  },
    { 0x1060, "82571EB SerDes (dual)" },
    { 0x10A4, "82571EB Quad Copper"   },
    { 0x10A5, "82571EB Quad Fiber"    },
    { 0x10BC, "82571EB Quad LP"       },
    { 0x10D9, "82571EB SerDes Dual"   },
    { 0x10DA, "82571EB SerDes Quad"   },
    { 0x10D5, "82571PT Quad LP"       },
    // ---- 82572 family ----
    { 0x107D, "82572EI Copper"        },
    { 0x107E, "82572EI Fiber"         },
    { 0x107F, "82572EI SerDes"        },
    { 0x10B9, "82572EI"               },
    // ---- 82573 family ----
    { 0x108B, "82573V"                },
    { 0x108C, "82573E"                },
    { 0x109A, "82573L"                },
    // ---- 82574 / 82583 ----
    { 0x10D3, "82574L"                },  // QEMU -device e1000e
    { 0x10F6, "82574LA"               },
    { 0x150C, "82583V"                },
    // ---- 80003ES2LAN ----
    { 0x1096, "80003ES2LAN Copper Dual" },
    { 0x1098, "80003ES2LAN SerDes Dual" },
    { 0x10BA, "80003ES2LAN Copper"      },
    { 0x10BB, "80003ES2LAN SerDes"      },
    // ---- ICH8 ----
    { 0x1049, "ICH8 IGP M Amt"        },
    { 0x104A, "ICH8 IGP Amt"          },
    { 0x104B, "ICH8 IGP C"            },
    { 0x104C, "ICH8 IFE"              },
    { 0x104D, "ICH8 IGP M"            },
    { 0x10C4, "ICH8 IFE GT"           },
    { 0x10C5, "ICH8 IFE G"            },
    // ---- ICH9 ----
    { 0x10BD, "ICH9 IGP Amt"          },
    { 0x10BF, "ICH9 IGP M"            },
    { 0x10CB, "ICH9 IGP M V"          },
    { 0x10CC, "ICH9 IGP M Amt"        },
    { 0x10CD, "ICH9 BM LM"            },
    { 0x10CE, "ICH9 BM LF"            },
    { 0x10E5, "ICH9 BM"               },
    { 0x294C, "ICH9 IGP C"            },
    { 0x10F5, "ICH9 IGP M Amt (2)"    },
    { 0x10BD, "ICH9 IGP Amt"          },
    // ---- ICH10 ----
    { 0x10C0, "ICH10 D BM LM"         },
    { 0x10C2, "ICH10 D BM LF"         },
    { 0x10C3, "ICH10 D BM V"          },
    { 0x10DF, "ICH10 R BM LM"         },
    { 0x10F0, "ICH10 D BM LM (2)"     },
    // ---- PCH (Pineview) ----
    { 0x10EA, "PCH M HV LM"           },
    { 0x10EB, "PCH M HV LC"           },
    { 0x10EF, "PCH D HV DM"           },
    { 0x10F0, "PCH D HV DC"           },
    // ---- PCH2 (CougarPoint) ----
    { 0x1502, "PCH2 LV LM"            },
    { 0x1503, "PCH2 LV V"             },
    // ---- LPT / I217 (Lynx Point, Haswell PCH) ----
    { 0x153A, "I217-LM"               },
    { 0x153B, "I217-V"                },
    // ---- LPT-LP / I218 (Lynx Point LP, Haswell-ULT PCH) ----
    { 0x155A, "I218-LM"               },
    { 0x1559, "I218-V"                },
    { 0x15A0, "I218-LM (2)"           },
    { 0x15A1, "I218-V  (2)"           },
    { 0x15A2, "I218-LM (3)"           },
    { 0x15A3, "I218-V  (3)"           },
    // ---- SPT / I219 (Sunrise Point, Skylake PCH) ----  ← ThinkPad P50
    { 0x156F, "I219-LM (SPT)"         },
    { 0x1570, "I219-V  (SPT)"         },
    { 0x15B7, "I219-LM (SPT-H)"       },
    { 0x15B8, "I219-V  (SPT-H)"       },
    { 0x15B9, "I219-LM (SPT-LP)"      },
    // ---- CNP / I219 (Cannon/Coffee Lake PCH) ----
    { 0x15BB, "I219-LM (CNP)"         },
    { 0x15BC, "I219-V  (CNP)"         },
    { 0x15BD, "I219-LM (CNP-H)"       },
    { 0x15BE, "I219-V  (CNP-H)"       },
    { 0x15DF, "I219-LM (CNP-LP)"      },
    { 0x15E0, "I219-V  (CNP-LP)"      },
    { 0x15E1, "I219-LM (CNP-2)"       },
    { 0x15E2, "I219-V  (CNP-2)"       },
    // ---- ICP / I219 (Ice Lake / Comet Lake PCH) ----
    { 0x15D6, "I219-V  (ICP)"         },
    { 0x15D7, "I219-LM (ICP)"         },
    { 0x15D8, "I219-V  (ICP-2)"       },
    { 0x15E3, "I219-LM (ICP-2)"       },
    // ---- CMP / I219 (Comet Lake PCH) ----
    { 0x0D4E, "I219-LM (CMP)"         },
    { 0x0D4F, "I219-V  (CMP)"         },
    { 0x0D4C, "I219-LM (CMP-2)"       },
    { 0x0D4D, "I219-V  (CMP-2)"       },
    { 0x0D53, "I219-LM (CMP-LP)"      },
    { 0x0D55, "I219-V  (CMP-LP)"      },
    // ---- TGP / I219 (Tiger Lake PCH) ----
    { 0x15F9, "I219-LM (TGP-LP)"      },
    { 0x15FA, "I219-V  (TGP-LP)"      },
    { 0x15FB, "I219-LM (TGP-H)"       },
    { 0x15FC, "I219-V  (TGP-H)"       },
    // ---- ADP / RPL / MTP / LNL (Alder/Raptor/Meteor/Lunar Lake PCH) ----
    { 0x1A1C, "I219-LM (ADP)"         },
    { 0x1A1D, "I219-V  (ADP)"         },
    { 0x1A1E, "I219-LM (ADP-N)"       },
    { 0x1A1F, "I219-V  (ADP-N)"       },
    { 0x550A, "I219-LM (RPL)"         },
    { 0x550B, "I219-V  (RPL)"         },
    { 0x550C, "I219-LM (RPL-2)"       },
    { 0x550D, "I219-V  (RPL-2)"       },
    { 0x57A0, "I219-LM (MTP)"         },
    { 0x57A1, "I219-V  (MTP)"         },
    { 0,      NULL                    },
};

static const e1000e_id_t* e1000e_lookup(uint16_t did) {
    for (const e1000e_id_t* e = e1000e_pci_ids; e->name; e++) {
        if (e->did == did) return e;
    }
    return NULL;
}

// Returns 1 if `did` is an I217/I218/I219 PCH-LAN part that requires the
// PCH-LAN / I219-specific bring-up quirks below.  These parts integrate the
// MAC into the PCH and the PHY into the CPU package, so the OS must
// negotiate access with the Management Engine (ME) firmware via the
// SW/FW semaphore and clear several PCH power-saving features (K1 idle,
// EEE, ULP) before TX/RX will actually move bytes on the wire.
static int e1000e_is_pch_lan(uint16_t did) {
    switch (did) {
        // ICH8/9/10 PCH parts
        case 0x1049: case 0x104A: case 0x104B: case 0x104C: case 0x104D:
        case 0x10C4: case 0x10C5:
        case 0x10BD: case 0x10BF: case 0x10CB: case 0x10CC: case 0x10CD:
        case 0x10CE: case 0x10E5: case 0x294C: case 0x10F5:
        case 0x10C0: case 0x10C2: case 0x10C3: case 0x10DF: case 0x10F0:
        // PCH / PCH2
        case 0x10EA: case 0x10EB: case 0x10EF:
        case 0x1502: case 0x1503:
        // I217 / I218 / I219 (LPT, LPT-LP, SPT, CNP, ICP, CMP, TGP, ADP, RPL, MTP)
        case 0x153A: case 0x153B:
        case 0x155A: case 0x1559: case 0x15A0: case 0x15A1: case 0x15A2: case 0x15A3:
        case 0x156F: case 0x1570: case 0x15B7: case 0x15B8: case 0x15B9:
        case 0x15BB: case 0x15BC: case 0x15BD: case 0x15BE:
        case 0x15DF: case 0x15E0: case 0x15E1: case 0x15E2:
        case 0x15D6: case 0x15D7: case 0x15D8: case 0x15E3:
        case 0x0D4E: case 0x0D4F: case 0x0D4C: case 0x0D4D: case 0x0D53: case 0x0D55:
        case 0x15F9: case 0x15FA: case 0x15FB: case 0x15FC:
        case 0x1A1C: case 0x1A1D: case 0x1A1E: case 0x1A1F:
        case 0x550A: case 0x550B: case 0x550C: case 0x550D:
        case 0x57A0: case 0x57A1:
            return 1;
        default:
            return 0;
    }
}

static int e1000e_is_pch_spt(uint16_t did) {
    switch (did) {
        case 0x156F: case 0x1570: case 0x15B7: case 0x15B8: case 0x15B9:
            return 1;
        default:
            return 0;
    }
}

// Single-NIC global.
static e1000e_dev_t g_e1000e;
int g_e1000e_initialized = 0;
int g_e1000e_legacy_irq = -1;

// ============================================================================
// MMIO helpers
// ============================================================================
static inline uint32_t e1000e_read(e1000e_dev_t* dev, uint32_t reg) {
    return *(volatile uint32_t*)(dev->mmio_base + reg);
}

static inline void e1000e_write(e1000e_dev_t* dev, uint32_t reg, uint32_t val) {
    *(volatile uint32_t*)(dev->mmio_base + reg) = val;
    __asm__ volatile("mfence" ::: "memory");
}

// ============================================================================
// I217/I218/I219 (PCH-LAN) register set and bring-up quirks
// ============================================================================
// Registers exclusive to the PCH-LAN / I219 family.  Offsets and bit
// definitions taken from Intel's I218/I219 datasheets and EHL/CNL EDS.
#define E1000_CTRL_EXT          0x00018  // Extended Device Control
#define E1000_FEXTNVM           0x00028
#define E1000_FEXTNVM3          0x0003C
#define E1000_FEXTNVM4          0x00024
#define E1000_FEXTNVM5          0x00014
#define E1000_FEXTNVM6          0x00010
#define E1000_FEXTNVM7          0x000E4
#define E1000_FEXTNVM8          0x05BB0
#define E1000_FEXTNVM9          0x0005C
#define E1000_FEXTNVM11         0x05BBC  // SPT/CNP/TGP+ Future Extended NVM 11
#define E1000_PBA               0x01000
#define E1000_PBS               0x01008
#define E1000_PBECCSTS          0x0100C
#define E1000_KUMCTRLSTA        0x00034
#define E1000_FWSM              0x05B54
#define E1000_GCR               0x05B00  // PCIe control
#define E1000_EXTCNF_CTRL       0x00F00
#define E1000_PHY_CTRL          0x00F10
#define E1000_DPGFR             0x00FAC
#define E1000_IOSFPC            0x00F28  // SPT/KBL Tx corruption / hang workaround
#define E1000_IPCNFG            0x00E38  // EEE control
#define E1000_EEER              0x00E30  // Energy-Efficient Ethernet
#define E1000_PCH_LPT_PRA       0x05B58
#define E1000_TARC0             0x03840
#define E1000_TARC1             0x03940
#define E1000_TXDCTL1           0x03928
#define E1000_KABGTXD           0x03004  // AFE Band Gap Transmit Ref Data
#define E1000_KABGTXD_BGSQLBIAS 0x00050000u
#define E1000_TIDV              0x03820
#define E1000_TADV              0x0382C
#define E1000_TDFH              0x03410
#define E1000_TDFT              0x03418
#define E1000_TDFHS             0x03420
#define E1000_TDFTS             0x03428
#define E1000_FCRTL             0x02160  // Flow Control Receive Threshold Low
#define E1000_FCRTH             0x02168  // Flow Control Receive Threshold High
#define E1000_FCTTV             0x00170  // Flow Control Transmit Timer Value
#define E1000_FCAL              0x00028  // Flow Control Address Low
#define E1000_FCAH              0x0002C  // Flow Control Address High
#define E1000_FCT               0x00030  // Flow Control Type
#define E1000_MANC              0x05820  // Management Control
#define E1000_H2ME              0x05B50  // Host-to-ME
// NOTE: bit positions match the I218/I219 datasheet exactly.  Earlier we
// had these off by one (ULP at bit 10, ENFORCE_SETTINGS at bit 11), which
// caused the "exit ULP" handshake to actually REQUEST ULP (writing 0x800 ==
// ULP-indication bit), gating the TX descriptor-fetch DMA on real I219-LM
// hardware (TDH never advances, GPTC stays 0, no errors).
#define E1000_H2ME_ULP                  0x00000800u // Host requests ULP (bit 11)
#define E1000_H2ME_ENFORCE_SETTINGS     0x00001000u // Tell ME: host owns config (bit 12)

// GCR bits controlling PCIe no-snoop attributes for descriptor and buffer DMA.
// On PCH-LAN we bring the device up in normal snooped mode by clearing
// these bits rather than relying on reset state inherited from firmware.
#define E1000_GCR_RXD_NO_SNOOP          0x00000001u
#define E1000_GCR_RXDSCW_NO_SNOOP       0x00000002u
#define E1000_GCR_RXDSCR_NO_SNOOP       0x00000004u
#define E1000_GCR_TXD_NO_SNOOP          0x00000008u
#define E1000_GCR_TXDSCW_NO_SNOOP       0x00000010u
#define E1000_GCR_TXDSCR_NO_SNOOP       0x00000020u
#define E1000_GCR_NO_SNOOP_ALL          (E1000_GCR_RXD_NO_SNOOP \
                                       | E1000_GCR_RXDSCW_NO_SNOOP \
                                       | E1000_GCR_RXDSCR_NO_SNOOP \
                                       | E1000_GCR_TXD_NO_SNOOP \
                                       | E1000_GCR_TXDSCW_NO_SNOOP \
                                       | E1000_GCR_TXDSCR_NO_SNOOP)

// CTRL_EXT bits
#define E1000_CTRL_EXT_LSECCK   (1u << 5)
#define E1000_CTRL_EXT_DRV_LOAD (1u << 28)
#define E1000_CTRL_EXT_FORCE_SMBUS (1u << 11)
#define E1000_CTRL_EXT_DPG_EN   (1u << 3)
#define E1000_CTRL_EXT_DMA_DYN_CLK_EN (1u << 19)
#define E1000_CTRL_EXT_LPCD     (1u << 24)
#define E1000_CTRL_EXT_RO_DIS   (1u << 17)

// STATUS bits (PCH)
#define E1000_STATUS_LAN_INIT_DONE (1u << 9)
#define E1000_STATUS_PHYRA      (1u << 10)

#define E1000_ICH8_LAN_INIT_TIMEOUT 1500

// MDIC register (MAC-mediated MDIO PHY access)
#define E1000_MDIC              0x00020
#define E1000_MDIC_DATA_MASK    0x0000FFFFu
#define E1000_MDIC_REG_SHIFT    16
#define E1000_MDIC_PHY_SHIFT    21
#define E1000_MDIC_OP_WRITE     (1u << 26)
#define E1000_MDIC_OP_READ      (2u << 26)
#define E1000_MDIC_READY        (1u << 28)
#define E1000_MDIC_INT_EN       (1u << 29)
#define E1000_MDIC_ERROR        (1u << 30)

#define E1000E_MAX_PHY_REG_ADDRESS   0x1Fu
#define E1000E_IGP_PAGE_SHIFT        5
#define E1000E_IGP_PHY_PAGE_SELECT   0x1Fu
#define E1000E_PHY_REG(page, reg) \
    (((page) << E1000E_IGP_PAGE_SHIFT) | ((reg) & E1000E_MAX_PHY_REG_ADDRESS))

// PHY MMD register addresses on PCH-LAN.  On I219 the integrated PHY
// sits at MDIO address 2 (verified empirically on real hardware).
#define E1000E_PHY_ADDR         2
#define PHY_CONTROL_REG         0       // MII BMCR
#define E1000E_HV_INTC_FC_PAGE_START 778

// Paged HV PHY registers used by the PCH-LAN SMBus / ULP handoff.
#define E1000_CV_SMB_CTRL               E1000E_PHY_REG(769, 23)
#define E1000_CV_SMB_CTRL_FORCE_SMBUS   0x0001
#define E1000_I218_ULP_CONFIG1          E1000E_PHY_REG(779, 16)
#define E1000_I218_ULP_CONFIG1_START                    0x0001
#define E1000_I218_ULP_CONFIG1_IND                      0x0004
#define E1000_I218_ULP_CONFIG1_STICKY_ULP               0x0010
#define E1000_I218_ULP_CONFIG1_INBAND_EXIT              0x0020
#define E1000_I218_ULP_CONFIG1_WOL_HOST                 0x0040
#define E1000_I218_ULP_CONFIG1_RESET_TO_SMBUS           0x0100
#define E1000_I218_ULP_CONFIG1_EN_ULP_LANPHYPC          0x0400
#define E1000_I218_ULP_CONFIG1_DIS_CLR_STICKY_ON_PERST  0x0800
#define E1000_I218_ULP_CONFIG1_DISABLE_SMB_PERST        0x1000

// Intel I82577/I82578/I82579/I217/I218/I219 integrated-PHY copper setup
// register.  Page 0 register 22.  These bits configure the PHY's copper
// front-end before autonegotiation runs.  Per the I82577 datasheet, both
// bits must be set unconditionally before autoneg restart.
//
//   I82577_CFG_ASSERT_CRS_ON_TX (bit 15)
//     Force the PHY to assert internal carrier-sense whenever the MAC
//     is transmitting.  Without this the MAC's TX state machine on
//     PCH-LAN parts can lose CRS during a frame and abort the
//     transmission, presenting as TXQE with TDH stuck and GPTC=0 — our
//     exact symptom.
//
//   I82577_CFG_ENABLE_DOWNSHIFT (bits 11:10 = 0b11 = 3 retries)
//     Configure auto-downshift to 3 retries.  The NVM default on OEM
//     ThinkPad parts is 0 retries (= immediate downshift after a single
//     failed 100Mb training pulse), which causes the PHY to resolve at
//     10 Mb/s on its very first link attempt even when the partner
//     advertises 100FD with autoneg ACK — the *exact* symptom we see
//     in dmesg (LPAR=0xC1E1 with 100FD+Ack but BMCR resolves to 10FD).
#define I82577_CFG_REG                  22
#define I82577_CFG_ASSERT_CRS_ON_TX     0x8000
#define I82577_CFG_ENABLE_DOWNSHIFT     0x0C00

// I82577 PHY Status Register 2 (page 0 reg 26): ground-truth resolved
// speed/duplex from the PHY itself.  Trusting BMCR after autoneg is
// unsafe because IEEE 802.3 leaves BMCR.SPEED bits undefined when
// AUTONEG_EN=1; only this register is authoritative.
#define I82577_PHY_STATUS_2                 26
#define I82577_PHY_STATUS2_REV_POLARITY     0x0400
#define I82577_PHY_STATUS2_MDIX             0x0800
#define I82577_PHY_STATUS2_SPEED_MASK       0x0300
#define I82577_PHY_STATUS2_SPEED_1000MBPS   0x0200
#define I82577_PHY_STATUS2_SPEED_100MBPS    0x0100

// EMI (Extended MDI) interface for paged HV PHY registers used by the
// EEE block on PCH-LAN (I217/I218/I219).  EEE settings live in the
// "EMI" address space accessed via two paged PHY registers:
//   * I82579_EMI_ADDR — write the EMI register address here
//   * I82579_EMI_DATA — read/write the EMI data through this window
// The real EMI registers we need for EEE-disable are:
//   * I217_EEE_ADVERTISEMENT (0x040E) — local EEE ability  (must be 0)
//   * I217_EEE_LP_ABILITY    (0x040F) — link-partner EEE   (read-only)
//   * I217_EEE_PCS_STATUS    (0x06E8 on I218, 0x06E9 on I217) — PCS state
// Page 776 hosts the EMI window on I217/I218/I219.
#define I82579_EMI_ADDR             E1000E_PHY_REG(776, 22)
#define I82579_EMI_DATA             E1000E_PHY_REG(776, 23)
#define I217_EEE_ADVERTISEMENT      0x040E
#define I217_EEE_LP_ABILITY         0x040F

// HV LPI Control register (paged): bit 7 enables 100Mb LPI auto-shutoff,
// bit 16 (across the 32-bit MAC mirror) enables 1000Mb LPI.  On the
// PHY side it lives at page 776 reg 16 and we just clear it entirely.
#define I218_LPI_CTRL               E1000E_PHY_REG(776, 18)

// RX statistics counters (read-to-clear)
#define E1000_CRCERRS           0x04000
#define E1000_RXERRC            0x0400C
#define E1000_MPC               0x04010
#define E1000_GPRC              0x04074
#define E1000_RNBC              0x040A0
#define E1000_TPR               0x040D0
#define E1000_GPTC              0x04080
#define E1000_TPT               0x040D4

// EXTCNF_CTRL bits
#define E1000_EXTCNF_CTRL_SWFLAG (1u << 5)
#define E1000_EXTCNF_CTRL_OEM_WRITE_ENABLE (1u << 3)

// FEXTNVM6: K1 power-save controls (SPT/CNP/ICP)
#define E1000_FEXTNVM6_K1_OFF_ENABLE    (1u << 24)
#define E1000_FEXTNVM6_REQ_PLL_CLK      (1u << 8)
#define E1000_FEXTNVM6_ENABLE_K1_ENTRY_CONDITION  (1u << 1)

// FEXTNVM7
#define E1000_FEXTNVM7_SIDE_CLK_UNGATE          (1u << 2)
#define E1000_FEXTNVM7_DISABLE_PB_READ           (1u << 18)
#define E1000_FEXTNVM7_DISABLE_SMB_PERST         (1u << 5)
#define E1000_FEXTNVM7_DISABLE_TSYNC_CLK         (1u << 31)
#define E1000_FEXTNVM7_ENABLE_TSYNC_INTR         (1u << 0)

#define E1000_PBECCSTS_ECC_ENABLE                0x00010000u
#define E1000_CTRL_MEHE                          0x00080000u

// CTRL bits used for the LANPHYPC handoff on SPT/CNP/TGP+ (I218/I219).
// On these PCH-LAN parts the integrated PHY's clock and power gate is
// controlled by the Management Engine by default.  The driver MUST take
// ownership by toggling LANPHYPC_OVERRIDE / LANPHYPC_VALUE — otherwise
// the PHY analog frontend stays gated even though MDIO appears to work
// (the link LED can stay lit because the ME has its own link), and our
// BMCR autoneg restart never actually drives FLPs onto the wire.
//   E1000_CTRL_LANPHYPC_OVERRIDE = 0x00010000  (bit 16, SW takes control)
//   E1000_CTRL_LANPHYPC_VALUE    = 0x00020000  (bit 17, value while OVRD)
#define E1000_CTRL_LANPHYPC_OVERRIDE             0x00010000u
#define E1000_CTRL_LANPHYPC_VALUE                0x00020000u

// FEXTNVM9 / FEXTNVM11 quirks (CMP / TGP)
#define E1000_FEXTNVM9_IOSFSB_CLKGATE_DIS     (1u << 12)
#define E1000_FEXTNVM9_IOSFSB_CLKREQ_DIS      (1u << 11)
#define E1000_FEXTNVM11_DISABLE_MULR_FIX      0x00002000u
#define E1000_TARC0_CB_MULTIQ_3_REQ           0x30000000u
#define E1000_TARC0_CB_MULTIQ_2_REQ           0x20000000u
#define E1000_TXDCTL_PTHRESH                  0x0000003Fu
#define E1000_TXDCTL_HTHRESH                  0x00003F00u
#define E1000_TXDCTL_WTHRESH                  0x003F0000u
#define E1000_TXDCTL_GRAN                     0x01000000u
#define E1000_TXDCTL_COUNT_DESC               0x00400000u

// PHY_CTRL bits
#define E1000_PHY_CTRL_GBE_DISABLE      (1u << 6)
#define E1000_PHY_CTRL_NOND0A_LPLU      (1u << 3)
#define E1000_PHY_CTRL_D0A_LPLU         (1u << 2)

// PMA timer-based microsecond delay forward decl
static void e1000e_delay_us(uint32_t us);

static void __attribute__((unused))
e1000e_dump_pci_dma_state(e1000e_dev_t* dev, uint32_t tx_seq)
{
#if E1000E_DBG
    if (!dev || !dev->pci_dev) {
        return;
    }

    const pci_device_t* pdev = dev->pci_dev;
    uint32_t cmdsts = pci_cfg_read32(pdev->bus, pdev->device, pdev->function, 0x04);
    uint16_t cmd = (uint16_t)(cmdsts & 0xFFFFu);
    uint16_t sts = (uint16_t)(cmdsts >> 16);
    uint8_t pcie_cap = pci_find_capability(pdev, 0x10);

    e1000e_dbg("E1000E: tx#%u PCI CMD=%04x STS=%04x BUSM=%u MEM=%u MABRT=%u TABRT_R=%u TABRT_S=%u DPERR=%u SERR=%u\n",
            tx_seq, cmd, sts,
            (cmd >> 2) & 1,
            (cmd >> 1) & 1,
            (sts >> 13) & 1,
            (sts >> 12) & 1,
            (sts >> 11) & 1,
            (sts >> 15) & 1,
            (sts >> 14) & 1);

    if (pcie_cap) {
        uint32_t devctlsts = pci_cfg_read32(pdev->bus, pdev->device, pdev->function, pcie_cap + 0x08);
        uint32_t lnksts = pci_cfg_read32(pdev->bus, pdev->device, pdev->function, pcie_cap + 0x10);
        uint16_t devsta = (uint16_t)(devctlsts >> 16);
        uint16_t linksta = (uint16_t)(lnksts >> 16);

        e1000e_dbg("E1000E: tx#%u PCIe DEVSTA=%04x CORR=%u NFAT=%u FAT=%u UR=%u AUX=%u TPEND=%u LNKSTA=%04x speed=%u width=x%u\n",
                tx_seq, devsta,
                (devsta >> 0) & 1,
                (devsta >> 1) & 1,
                (devsta >> 2) & 1,
                (devsta >> 3) & 1,
                (devsta >> 4) & 1,
                (devsta >> 5) & 1,
                linksta,
                linksta & 0xF,
                (linksta >> 4) & 0x3F);
    } else {
        // The I219 is an LPC-attached integrated MAC and does not expose a
        // PCIe Express capability (Cap ID 0x10) in standard config space.
        // Nothing to do here.
        e1000e_dbg("E1000E: tx#%u no PCIe Express cap (expected on I219 LPC-integrated MAC)\n",
                tx_seq);
    }
#else
    (void)dev; (void)tx_seq;
#endif
}

// Wait for the SW flag to be released by ME firmware so we can safely
// touch PHY-side configuration.  Best-effort: if we can't get it within
// 100 ms we proceed anyway — the MAC-side workarounds below still help
// even when the PHY remains owned by ME.
static int __attribute__((unused)) e1000e_acquire_swflag(e1000e_dev_t* dev) {
    uint32_t ext = e1000e_read(dev, E1000_EXTCNF_CTRL);
    if (ext & E1000_EXTCNF_CTRL_SWFLAG) {
        // Already held — clear stale flag first
        e1000e_write(dev, E1000_EXTCNF_CTRL, ext & ~E1000_EXTCNF_CTRL_SWFLAG);
        e1000e_delay_us(2000);
    }
    for (int i = 0; i < 100; i++) {
        ext = e1000e_read(dev, E1000_EXTCNF_CTRL);
        e1000e_write(dev, E1000_EXTCNF_CTRL, ext | E1000_EXTCNF_CTRL_SWFLAG);
        e1000e_delay_us(1000);
        ext = e1000e_read(dev, E1000_EXTCNF_CTRL);
        if (ext & E1000_EXTCNF_CTRL_SWFLAG) return 0;
    }
    return -1;
}

static void __attribute__((unused)) e1000e_release_swflag(e1000e_dev_t* dev) {
    uint32_t ext = e1000e_read(dev, E1000_EXTCNF_CTRL);
    e1000e_write(dev, E1000_EXTCNF_CTRL, ext & ~E1000_EXTCNF_CTRL_SWFLAG);
}

// Minimal MDIO-via-MAC PHY register read.  Used on PCH-LAN parts to
// acknowledge the PHY after reset: until software performs at least one
// MDIO transaction, the PHY keeps STATUS.PHYRA asserted and gates its
// data interface to the MAC, so neither RX nor TX frames flow even
// though L1 link is up.
//
// Returns 0 on success and stores the 16-bit PHY register value in
// *val; returns -1 on timeout or MDIC.ERROR.
static int e1000e_phy_read_at(e1000e_dev_t* dev, uint8_t phy_addr,
                              uint8_t reg, uint16_t* val) {
    uint32_t mdic = ((uint32_t)reg << E1000_MDIC_REG_SHIFT)
                  | ((uint32_t)phy_addr << E1000_MDIC_PHY_SHIFT)
                  | E1000_MDIC_OP_READ;
    e1000e_write(dev, E1000_MDIC, mdic);

    // Spec: completes within ~64 us on a 25 MHz MDIO; allow up to 5 ms.
    for (int i = 0; i < 100; i++) {
        e1000e_delay_us(50);
        mdic = e1000e_read(dev, E1000_MDIC);
        if (mdic & E1000_MDIC_READY) {
            if (mdic & E1000_MDIC_ERROR) return -1;
            if (val) *val = (uint16_t)(mdic & E1000_MDIC_DATA_MASK);
            return 0;
        }
    }
    return -1;
}

static int e1000e_phy_write_at(e1000e_dev_t* dev, uint8_t phy_addr,
                               uint8_t reg, uint16_t val) {
    uint32_t mdic = ((uint32_t)val & E1000_MDIC_DATA_MASK)
                  | ((uint32_t)reg << E1000_MDIC_REG_SHIFT)
                  | ((uint32_t)phy_addr << E1000_MDIC_PHY_SHIFT)
                  | E1000_MDIC_OP_WRITE;
    e1000e_write(dev, E1000_MDIC, mdic);

    for (int i = 0; i < 100; i++) {
        e1000e_delay_us(50);
        mdic = e1000e_read(dev, E1000_MDIC);
        if (mdic & E1000_MDIC_READY) {
            return (mdic & E1000_MDIC_ERROR) ? -1 : 0;
        }
    }
    return -1;
}

static uint8_t e1000e_phy_addr_for_hv_page(uint16_t page) {
    return (page >= E1000E_HV_INTC_FC_PAGE_START) ? 1 : E1000E_PHY_ADDR;
}

static int e1000e_phy_read_paged_locked(e1000e_dev_t* dev, uint32_t offset,
                                        uint16_t* val) {
    uint16_t page = (uint16_t)(offset >> E1000E_IGP_PAGE_SHIFT);
    uint8_t reg = (uint8_t)(offset & E1000E_MAX_PHY_REG_ADDRESS);
    uint8_t phy_addr = e1000e_phy_addr_for_hv_page(page);

    if (offset > E1000E_MAX_PHY_REG_ADDRESS) {
        if (e1000e_phy_write_at(dev, phy_addr, E1000E_IGP_PHY_PAGE_SELECT,
                                (uint16_t)(page << E1000E_IGP_PAGE_SHIFT)) != 0) {
            return -1;
        }
    }

    return e1000e_phy_read_at(dev, phy_addr, reg, val);
}

static int e1000e_phy_write_paged_locked(e1000e_dev_t* dev, uint32_t offset,
                                         uint16_t val) {
    uint16_t page = (uint16_t)(offset >> E1000E_IGP_PAGE_SHIFT);
    uint8_t reg = (uint8_t)(offset & E1000E_MAX_PHY_REG_ADDRESS);
    uint8_t phy_addr = e1000e_phy_addr_for_hv_page(page);

    if (offset > E1000E_MAX_PHY_REG_ADDRESS) {
        if (e1000e_phy_write_at(dev, phy_addr, E1000E_IGP_PHY_PAGE_SELECT,
                                (uint16_t)(page << E1000E_IGP_PAGE_SHIFT)) != 0) {
            return -1;
        }
    }

    return e1000e_phy_write_at(dev, phy_addr, reg, val);
}

static void e1000e_force_exit_phy_ulp(e1000e_dev_t* dev) {
    if (e1000e_acquire_swflag(dev) != 0) {
        e1000e_dbg("E1000E: ULP exit: could not acquire SWFLAG\n");
        return;
    }

    uint32_t ctrl_ext_restore = e1000e_read(dev, E1000_CTRL_EXT);
    uint16_t smb_before = 0, smb_after = 0;
    uint16_t ulp_before = 0, ulp_after = 0;
    int smb_rc = e1000e_phy_read_paged_locked(dev, E1000_CV_SMB_CTRL, &smb_before);

    if (smb_rc != 0) {
        uint32_t ctrl_ext = ctrl_ext_restore | E1000_CTRL_EXT_FORCE_SMBUS;
        e1000e_write(dev, E1000_CTRL_EXT, ctrl_ext);
        e1000e_delay_us(50000);
        smb_rc = e1000e_phy_read_paged_locked(dev, E1000_CV_SMB_CTRL, &smb_before);
    }

    if (smb_rc == 0) {
        uint16_t smb_ctrl = (uint16_t)(smb_before & ~E1000_CV_SMB_CTRL_FORCE_SMBUS);
        if (e1000e_phy_write_paged_locked(dev, E1000_CV_SMB_CTRL, smb_ctrl) == 0) {
            (void)e1000e_phy_read_paged_locked(dev, E1000_CV_SMB_CTRL, &smb_after);
        }
    }

    if (e1000e_phy_read_paged_locked(dev, E1000_I218_ULP_CONFIG1, &ulp_before) == 0) {
        uint16_t ulp_cfg = ulp_before;
        ulp_cfg &= ~(E1000_I218_ULP_CONFIG1_IND |
                     E1000_I218_ULP_CONFIG1_STICKY_ULP |
                     E1000_I218_ULP_CONFIG1_RESET_TO_SMBUS |
                     E1000_I218_ULP_CONFIG1_WOL_HOST |
                     E1000_I218_ULP_CONFIG1_INBAND_EXIT |
                     E1000_I218_ULP_CONFIG1_EN_ULP_LANPHYPC |
                     E1000_I218_ULP_CONFIG1_DIS_CLR_STICKY_ON_PERST |
                     E1000_I218_ULP_CONFIG1_DISABLE_SMB_PERST);

        if (e1000e_phy_write_paged_locked(dev, E1000_I218_ULP_CONFIG1, ulp_cfg) == 0) {
            (void)e1000e_phy_write_paged_locked(dev, E1000_I218_ULP_CONFIG1,
                                                (uint16_t)(ulp_cfg | E1000_I218_ULP_CONFIG1_START));
            (void)e1000e_phy_read_paged_locked(dev, E1000_I218_ULP_CONFIG1, &ulp_after);
        }
    }

    e1000e_write(dev, E1000_CTRL_EXT, ctrl_ext_restore & ~E1000_CTRL_EXT_FORCE_SMBUS);

#if E1000E_DBG
    uint32_t fextnvm7_before = e1000e_read(dev, E1000_FEXTNVM7);
    e1000e_write(dev, E1000_FEXTNVM7,
                 fextnvm7_before & ~E1000_FEXTNVM7_DISABLE_SMB_PERST);
    uint32_t fextnvm7_after = e1000e_read(dev, E1000_FEXTNVM7);
#else
    {
        uint32_t fextnvm7_tmp = e1000e_read(dev, E1000_FEXTNVM7);
        e1000e_write(dev, E1000_FEXTNVM7,
                     fextnvm7_tmp & ~E1000_FEXTNVM7_DISABLE_SMB_PERST);
    }
#endif

    e1000e_release_swflag(dev);

    e1000e_dbg("E1000E: PHY SMB/ULP exit CV_SMB_CTRL=%04x->%04x ULP_CFG1=%04x->%04x FEXTNVM7=%08x->%08x\n",
            smb_before, smb_after, ulp_before, ulp_after,
            fextnvm7_before, fextnvm7_after);
}

static int __attribute__((unused))
e1000e_phy_read(e1000e_dev_t* dev, uint8_t reg, uint16_t* val) {
    return e1000e_phy_read_at(dev, E1000E_PHY_ADDR, reg, val);
}

// MII register addresses we touch directly.
#define MII_BMCR        0x00
#define MII_BMSR        0x01
#define MII_ANAR        0x04
#define MII_GBCR        0x09  // 1000BASE-T Control

#define MII_BMCR_RESET           0x8000
#define MII_BMCR_LOOPBACK        0x4000
#define MII_BMCR_SPEED_LSB       0x2000  // 100
#define MII_BMCR_AUTONEG_EN      0x1000
#define MII_BMCR_PWR_DOWN        0x0800
#define MII_BMCR_ISOLATE         0x0400
#define MII_BMCR_AUTONEG_RESTART 0x0200
#define MII_BMCR_DUPLEX_FD       0x0100
#define MII_BMCR_SPEED_MSB       0x0040

#define MII_BMSR_LSTATUS         0x0004
#define MII_BMSR_AN_COMPLETE     0x0020

// IEEE 802.3 ANAR fields: selector=00001 (802.3 CSMA/CD), advertise
// 10HD|10FD|100HD|100FD, and pause symmetric+asymmetric so the link
// partner knows we accept its pause frames (IEEE 802.3 defaults).
#define MII_ANAR_FULL_RANGE      0x05E1  // sel + 10/100 HD/FD + asym/sym pause

// 1000BASE-T Control: advertise 1000FD only (most modern partners
// reject 1000HD outright).
#define MII_GBCR_ADV_1000FD      0x0200
#define MII_GBCR_ADV_1000HD      0x0100

// Intel HV (Hidden-Vendor) PHY OEM control register.  On I217/I218/I219
// this is at PHY page 0, register 25 — i.e. plain MDIO register 0x19,
// no page select needed.  The NVM default for OEM-branded laptops
// (including ThinkPad P50) leaves LPLU set, which forces the PHY to
// auto-negotiate only up to 10 Mb/s when the host has not explicitly
// taken ownership.  We must therefore unconditionally clear LPLU and
// GBE_DIS and set RESTART_AN; without this we get exactly our symptom:
// BMSR.AN_COMPLETE=1, LSTATUS=1, but BMCR result bits show 10 Mb/s and
// MAC STATUS.SPEED reads 00.
// HV_OEM_BITS lives on PHY page 769 (BM_PORT_CTRL_PAGE), reg 25 — i.e.
//     PHY_REG(769, 25)
// Accessing it as raw reg 0x19 (page 0) — which we did before — reads a
// completely different MII register and any write is silently dropped.
// That is exactly the failure we observed:
//     E1000E: HV_OEM_BITS 0 -> 0 (... RESTART_AN set)
// — the read returned 0 and the write of 0x0400 had no effect, so the
// PHY's LPLU bit was NEVER cleared and autoneg never restarted.
#define HV_OEM_BITS              E1000E_PHY_REG(769, 25)
#define HV_OEM_BITS_LPLU         0x0004  // Low-Power Link-Up enable
#define HV_OEM_BITS_A1KDIS       0x0008  // 1Gb idle disable
#define HV_OEM_BITS_GBE_DIS      0x0040  // Gigabit ethernet disable
#define HV_OEM_BITS_RESTART_AN   0x0400  // Restart auto-negotiation

// Clear the MAC-side mirror of the PHY power-control bits.  PHY_CTRL is
// the MAC's view of the LPLU / GBE_DIS / SMBus signals it forwards to
// the integrated PHY; if D0A_LPLU stays set after reset the PHY will
// honor it and refuse to negotiate above 10 Mb/s even when we clear the
// PHY-side HV_OEM_BITS register.
static void e1000e_clear_mac_phy_ctrl_lplu(e1000e_dev_t* dev) {
    uint32_t before = e1000e_read(dev, E1000_PHY_CTRL);
    uint32_t after = before & ~(E1000_PHY_CTRL_GBE_DISABLE
                              | E1000_PHY_CTRL_NOND0A_LPLU
                              | E1000_PHY_CTRL_D0A_LPLU);
    e1000e_write(dev, E1000_PHY_CTRL, after);
    e1000e_dbg("E1000E: MAC PHY_CTRL %08x -> %08x (LPLU/GBE_DIS cleared)\n",
            before, e1000e_read(dev, E1000_PHY_CTRL));
}

// Clear the PHY-side LPLU and GBE_DIS bits and request an autoneg
// restart.  Required on every OEM-branded PCH-LAN part where the NVM
// ships LPLU=1 by default.
//
// CRITICAL: HV_OEM_BITS is paged (page 769 reg 25) AND its write requires
// the SW_FLAG semaphore + EXTCNF_CTRL.OEM_WRITE_ENABLE set before the
// write so the MAC routes the MDIO transaction to the OEM page rather
// than dropping it (per I218/I219 datasheet).
// `restart_an`: when nonzero, also set HV_OEM_BITS_RESTART_AN to force the
// PHY to re-run autoneg.  When zero (BIOS-already-up path), we leave the
// existing link intact and only clear the LPLU/GBE_DIS bits in place --
// this lifts the MII data-path throttle without dropping the live link.
//
// Empirically required on Lenovo P50 / I219-LM: the BIOS leaves the PHY
// in HV_OEM_BITS=0x544 (LPLU=1, GBE_DIS=1, RESTART_AN=1) post-handoff.
// LPLU=1 makes the PHY accept link pulses but stall the data path on
// MII -- the MAC fetches the descriptor (TDFH advances) but never pulls
// the buffer payload, so TDH stays 0 and GPTC stays 0.  Symptom is
// indistinguishable from a TX-arbiter gate.
static int e1000e_oem_bits_clear_lplu_ex(e1000e_dev_t* dev, int restart_an) {
    if (e1000e_acquire_swflag(dev) != 0) {
        e1000e_dbg("E1000E: HV_OEM_BITS: SWFLAG acquire failed\n");
        return -1;
    }

    // Set OEM_WRITE_ENABLE so the upcoming write to page 769 reg 25
    // is honored by the MAC's PHY-write filter.
    uint32_t ext = e1000e_read(dev, E1000_EXTCNF_CTRL);
    e1000e_write(dev, E1000_EXTCNF_CTRL,
                 ext | E1000_EXTCNF_CTRL_OEM_WRITE_ENABLE);

    uint16_t before = 0xFFFF, after = 0xFFFF;
    int rc = -1;
    if (e1000e_phy_read_paged_locked(dev, HV_OEM_BITS, &before) == 0) {
        uint16_t v = before;
        v &= ~(HV_OEM_BITS_LPLU | HV_OEM_BITS_GBE_DIS);
        if (restart_an)
            v |=  HV_OEM_BITS_RESTART_AN;
        else
            v &= ~HV_OEM_BITS_RESTART_AN;
        if (e1000e_phy_write_paged_locked(dev, HV_OEM_BITS, v) == 0) {
            // Spec: wait at least 1 ms after writing HV_OEM_BITS before
            // next PHY register access so the PHY internal state machine
            // restarts cleanly.
            e1000e_delay_us(2000);
            (void)e1000e_phy_read_paged_locked(dev, HV_OEM_BITS, &after);
            rc = 0;
        } else {
            e1000e_dbg("E1000E: HV_OEM_BITS write failed\n");
        }
    } else {
        e1000e_dbg("E1000E: HV_OEM_BITS read failed\n");
    }

    // Clear OEM_WRITE_ENABLE and release SW_FLAG.
    ext = e1000e_read(dev, E1000_EXTCNF_CTRL);
    e1000e_write(dev, E1000_EXTCNF_CTRL,
                 ext & ~E1000_EXTCNF_CTRL_OEM_WRITE_ENABLE);
    e1000e_release_swflag(dev);

    e1000e_dbg("E1000E: HV_OEM_BITS(769.25) %04x -> %04x (LPLU/GBE_DIS cleared, RESTART_AN=%d)\n",
            before, after, restart_an ? 1 : 0);
    return rc;
}

// Backwards-compatible wrapper: original callers always wanted the
// autoneg-restart variant.
static int e1000e_oem_bits_clear_lplu(e1000e_dev_t* dev) {
    return e1000e_oem_bits_clear_lplu_ex(dev, 1);
}

// Disable Energy-Efficient Ethernet (EEE / 802.3az LPI) before autoneg
// restart.  On OEM-branded NVMs (ThinkPad P50 included) the PHY's EEE
// advertisement is enabled by default and is exchanged via the autoneg
// Next-Page mechanism (LPAR.NP=1 in the dmesg confirms partner accepted
// our NP).  When EEE NextPage exchange fails or is incompatible with
// the link partner the PHY's HCD selection FSM falls back to the
// lowest common ability — exactly 10 Mb/s FD — instead of the IEEE
// 802.3 mandated highest common denominator.  Writing 0 to the PHY's
// EEE_ADVERTISEMENT register before autoneg avoids this misnegotiation.
//
// We do the same here, plus zero the MAC-side EEER/IPCNFG so the MAC
// doesn't re-enable EEE LPI on the internal MII bus after link.
static void e1000e_disable_eee(e1000e_dev_t* dev) {
    // 1. MAC-side disable: clear EEER (LPI enables, FRC_AN) and IPCNFG.
#if E1000E_DBG
    uint32_t eeer_before = e1000e_read(dev, E1000_EEER);
    uint32_t ipc_before  = e1000e_read(dev, E1000_IPCNFG);
#endif
    e1000e_write(dev, E1000_EEER,   0);
    e1000e_write(dev, E1000_IPCNFG, 0);
    e1000e_dbg("E1000E: MAC EEE disabled EEER=%08x->%08x IPCNFG=%08x->%08x\n",
            eeer_before, e1000e_read(dev, E1000_EEER),
            ipc_before,  e1000e_read(dev, E1000_IPCNFG));

    // 2. PHY-side disable: clear the EEE advertisement (EMI 0x040E).
    //    The EMI window is two paged PHY registers on page 776: write the
    //    EMI register address into I82579_EMI_ADDR, then the data into
    //    I82579_EMI_DATA.
    uint16_t lp = 0xFFFF, adv_after = 0xFFFF;
    if (e1000e_phy_write_paged_locked(dev, I82579_EMI_ADDR,
                                       I217_EEE_ADVERTISEMENT) == 0 &&
        e1000e_phy_write_paged_locked(dev, I82579_EMI_DATA, 0x0000) == 0) {
        // Read back to confirm
        if (e1000e_phy_write_paged_locked(dev, I82579_EMI_ADDR,
                                           I217_EEE_ADVERTISEMENT) == 0) {
            (void)e1000e_phy_read_paged_locked(dev, I82579_EMI_DATA, &adv_after);
        }
    }
    if (e1000e_phy_write_paged_locked(dev, I82579_EMI_ADDR,
                                       I217_EEE_LP_ABILITY) == 0) {
        (void)e1000e_phy_read_paged_locked(dev, I82579_EMI_DATA, &lp);
    }
    e1000e_dbg("E1000E: PHY EEE adv cleared (EMI[040E]=%04x) LP_ability(EMI[040F])=%04x\n",
            adv_after, lp);
    // NOTE: do NOT touch "LPI_CTRL" at page 776 reg 18 — that register on
    // this PHY is NOT the I82579-style I82579_LPI_CTRL (which lives at page
    // 772 reg 20).  Page 776 reg 18 is some PHY-internal config that
    // defaults to 0x8402 and clearing it breaks autoneg completely
    // (verified empirically: LPAR=0, BMSR unchanged, AN_COMPLETE=0).
}

// PHY-front-end copper config that MUST run before autoneg restart on
// every I82577/82578/82579/I217/I218/I219 PHY.  Without it, the PHY's
// NVM defaults leave CRS_ON_TX off and the downshift retry counter at
// 0, which on this exact board (ThinkPad P50, I219-LM PCH-SPT-H) causes
// the PHY to resolve at 10 Mb/s even when both ends advertise 100FD
// with autoneg ACK.
//
// Bits to set in I82577_CFG_REG (page 0 reg 22):
//   * ASSERT_CRS_ON_TX   - keep CRS asserted during MAC transmits so
//                          the MAC TX FSM doesn't abort mid-frame.
//   * ENABLE_DOWNSHIFT=3 - allow 3 training retries at the higher
//                          speed before downshifting; the NVM default
//                          of 0 retries is what's pinning us to 10Mb.
static int e1000e_copper_link_setup_82577(e1000e_dev_t* dev) {
    uint16_t before = 0, after = 0;
    if (e1000e_phy_read_at(dev, E1000E_PHY_ADDR, I82577_CFG_REG, &before) != 0) {
        e1000e_dbg("E1000E: I82577_CFG_REG read failed\n");
        return -1;
    }
    uint16_t v = before;
    v &= ~I82577_CFG_ENABLE_DOWNSHIFT;     // clear retry-count field first
    v |=  I82577_CFG_ASSERT_CRS_ON_TX;
    v |=  I82577_CFG_ENABLE_DOWNSHIFT;     // = 3 retries (bits 11:10 = 11b)
    if (e1000e_phy_write_at(dev, E1000E_PHY_ADDR, I82577_CFG_REG, v) != 0) {
        e1000e_dbg("E1000E: I82577_CFG_REG write failed\n");
        return -1;
    }
    e1000e_delay_us(1000);
    (void)e1000e_phy_read_at(dev, E1000E_PHY_ADDR, I82577_CFG_REG, &after);
    e1000e_dbg("E1000E: I82577_CFG_REG %04x -> %04x (CRS_ON_TX | DOWNSHIFT=3)\n",
            before, after);
    if (after != v) {
        e1000e_dbg("E1000E: WARNING I82577_CFG_REG write did not stick (wanted %04x got %04x)\n",
                v, after);
    }
    return 0;
}

// Restart copper auto-negotiation on the integrated PHY and block until
// it completes (or timeout).  This is the missing step that left the I219
// PHY without FLPs on the wire after our PHY_RST: BMSR.AN_COMPLETE = 0,
// BMSR.LSTATUS = 0, while MAC STATUS.LU = 1 reflected only the cached
// SMBus idle link.  Without a real copper link the MAC PCS never grants
// TX, so descriptors stay in the on-chip FIFO and GPTC stays at 0.
//
// Sequence is the IEEE 802.3 / Intel PCH-LAN canonical one:
//   1. Run the I82577 copper-link setup (PHY copper front-end config).
//   2. Program advertised abilities (ANAR, GBCR).
//   3. Set BMCR = AUTONEG_EN | AUTONEG_RESTART.
//   4. Poll BMSR.AN_COMPLETE for up to 5 s.
//   5. Read I82577_PHY_STATUS_2 for ground-truth resolved speed.
static int e1000e_phy_restart_autoneg(e1000e_dev_t* dev) {
    uint16_t bmsr_before = 0;
    (void)e1000e_phy_read_at(dev, E1000E_PHY_ADDR, MII_BMSR, &bmsr_before);
#if E1000E_DBG
    uint16_t bmcr_before = 0, anar_before = 0, gbcr_before = 0;
    (void)e1000e_phy_read_at(dev, E1000E_PHY_ADDR, MII_BMCR, &bmcr_before);
    (void)e1000e_phy_read_at(dev, E1000E_PHY_ADDR, MII_ANAR, &anar_before);
    (void)e1000e_phy_read_at(dev, E1000E_PHY_ADDR, MII_GBCR, &gbcr_before);
#endif

    // CRITICAL on real hardware: if the BIOS already brought up link
    // before we got here (BMSR.LSTATUS=1 AND BMSR.AN_COMPLETE=1), do
    // NOT restart autoneg.  Restarting forces the PHY to drop the
    // existing link, re-train from scratch, and on this ThinkPad P50
    // / I219-LM the partner switch's autoneg state machine takes
    // longer than our 5 s poll to re-converge — link goes down and
    // never comes back up during driver init.  Adopt the BIOS-
    // negotiated link as-is; it's already valid.
    if ((bmsr_before & MII_BMSR_LSTATUS) &&
        (bmsr_before & MII_BMSR_AN_COMPLETE)) {
        e1000e_dbg("E1000E: PHY link already UP (BMCR=%04x BMSR=%04x ANAR=%04x GBCR=%04x) - "
                "adopting BIOS-negotiated link, skipping autoneg restart\n",
                bmcr_before, bmsr_before, anar_before, gbcr_before);

        // Still clear the MAC-side LPLU mirror so the MAC won't gate
        // its own internal MII transmitter, but leave the PHY alone.
        e1000e_clear_mac_phy_ctrl_lplu(dev);

#if E1000E_DBG
        // Read PSS2 ground truth speed for the diagnostic line.
        uint16_t pss2 = 0;
        (void)e1000e_phy_read_at(dev, E1000E_PHY_ADDR, I82577_PHY_STATUS_2, &pss2);
        const char* phy_speed_str = "10";
        switch (pss2 & I82577_PHY_STATUS2_SPEED_MASK) {
            case I82577_PHY_STATUS2_SPEED_1000MBPS: phy_speed_str = "1000"; break;
            case I82577_PHY_STATUS2_SPEED_100MBPS:  phy_speed_str = "100";  break;
            default: phy_speed_str = "10"; break;
        }
        e1000e_dbg("E1000E: PHY ground-truth I82577_PHY_STATUS_2=%04x speed=%sMb/s MDIX=%u RevPol=%u\n",
                pss2, phy_speed_str,
                (pss2 & I82577_PHY_STATUS2_MDIX) ? 1 : 0,
                (pss2 & I82577_PHY_STATUS2_REV_POLARITY) ? 1 : 0);
#endif

        // CRITICAL on Lenovo P50 / I219-LM: BIOS leaves the PHY's OEM
        // bits at 0x544 (LPLU=1, GBE_DIS=1, RESTART_AN=1) post-handoff.
        // LPLU=1 with link UP makes the PHY accept link pulses but
        // throttles the MII data path -- the MAC fetches our descriptor
        // (TDFH advances by 16 bytes) but the buffer payload is never
        // pulled from host memory and the descriptor is never written
        // back, so TDH stays 0 and GPTC stays 0.  Indistinguishable
        // from a TX-arbiter gate without inspecting HV_OEM_BITS directly.
        //
        // Clear LPLU/GBE_DIS in place WITHOUT setting RESTART_AN, so we
        // lift the data-path throttle without dropping the live link.
        (void)e1000e_oem_bits_clear_lplu_ex(dev, 0);
        return 0;
    }

    // Clear the MAC-side LPLU / GBE_DIS mirror first.  If we don't, the
    // PHY will pick up the LPLU signal again on the next autoneg cycle
    // and resolve at 10 Mb/s no matter what we write into HV_OEM_BITS.
    e1000e_clear_mac_phy_ctrl_lplu(dev);

    // Then clear the PHY-side OEM bits so the PHY itself permits >10Mb
    // resolutions.  This both clears LPLU/GBE_DIS and triggers an
    // internal autoneg restart (RESTART_AN bit).
    (void)e1000e_oem_bits_clear_lplu(dev);

    // Disable EEE BEFORE programming the I82577 copper front-end and
    // BEFORE writing autoneg advertisements.  EEE NextPage exchange on
    // OEM-branded PCH-LAN NVMs causes the PHY's HCD selection to fall
    // back to 10Mb when the partner doesn't speak matching EEE NPs.
    e1000e_disable_eee(dev);

    // CRITICAL: program the I82577 PHY copper front-end (CRS_ON_TX +
    // ENABLE_DOWNSHIFT=3 retries) BEFORE writing ANAR/GBCR/BMCR.  Without
    // this the PHY uses NVM-default downshift count (0 retries) and
    // resolves at 10 Mb/s on the first 100Mb training failure.
    (void)e1000e_copper_link_setup_82577(dev);

    if (e1000e_phy_write_at(dev, E1000E_PHY_ADDR, MII_ANAR,
                            MII_ANAR_FULL_RANGE) != 0) {
        e1000e_dbg("E1000E: PHY ANAR write failed\n");
        return -1;
    }
    if (e1000e_phy_write_at(dev, E1000E_PHY_ADDR, MII_GBCR,
                            MII_GBCR_ADV_1000FD) != 0) {
        e1000e_dbg("E1000E: PHY GBCR write failed\n");
        return -1;
    }
    if (e1000e_phy_write_at(dev, E1000E_PHY_ADDR, MII_BMCR,
                            MII_BMCR_AUTONEG_EN | MII_BMCR_AUTONEG_RESTART) != 0) {
        e1000e_dbg("E1000E: PHY BMCR autoneg-restart write failed\n");
        return -1;
    }

    uint16_t bmsr = 0;
#if E1000E_DBG
    int ms_waited = 0;
#endif
    for (int i = 0; i < 5000; i++) {
        e1000e_delay_us(1000);
#if E1000E_DBG
        ms_waited = i + 1;
#endif
        if (e1000e_phy_read_at(dev, E1000E_PHY_ADDR, MII_BMSR, &bmsr) != 0) {
            continue;
        }
        if (bmsr & MII_BMSR_AN_COMPLETE) break;
    }

#if E1000E_DBG
    // BMSR.LSTATUS is latched-low; re-read so we see the live state.
    uint16_t bmsr2 = 0;
    (void)e1000e_phy_read_at(dev, E1000E_PHY_ADDR, MII_BMSR, &bmsr2);

    // Diagnostic: read the link-partner advertisement (MII reg 5 LPAR)
    // and the 1000BASE-T status (MII reg 10 GBSR).  If LPAR shows that
    // the partner only advertised 10 Mb/s, our resolved 10 Mb/s outcome
    // is correct and the partner-side cable/switch is the limit.  If
    // LPAR shows 100/1000 capability but BMCR result still reads 10 Mb/s,
    // the PHY itself is misbehaving.
    uint16_t lpar = 0, gbsr = 0;
    (void)e1000e_phy_read_at(dev, E1000E_PHY_ADDR, 5,  &lpar);
    (void)e1000e_phy_read_at(dev, E1000E_PHY_ADDR, 10, &gbsr);
    e1000e_dbg("E1000E: PHY LPAR=%04x (10HD=%u 10FD=%u 100HD=%u 100FD=%u T4=%u Pause=%u AsymPause=%u RemFault=%u Ack=%u)\n",
            lpar,
            (lpar >> 5) & 1, (lpar >> 6) & 1,
            (lpar >> 7) & 1, (lpar >> 8) & 1,
            (lpar >> 9) & 1,
            (lpar >> 10) & 1, (lpar >> 11) & 1,
            (lpar >> 13) & 1, (lpar >> 14) & 1);
    e1000e_dbg("E1000E: PHY GBSR=%04x (LP_1000HD=%u LP_1000FD=%u LRS=%u RRS=%u IDLE_ERR=%u)\n",
            gbsr,
            (gbsr >> 10) & 1, (gbsr >> 11) & 1,
            (gbsr >> 13) & 1, (gbsr >> 14) & 1,
            gbsr & 0xFF);

    uint16_t anar_after = 0, gbcr_after = 0, bmcr_after = 0;
    (void)e1000e_phy_read_at(dev, E1000E_PHY_ADDR, MII_BMCR, &bmcr_after);
    (void)e1000e_phy_read_at(dev, E1000E_PHY_ADDR, MII_ANAR, &anar_after);
    (void)e1000e_phy_read_at(dev, E1000E_PHY_ADDR, MII_GBCR, &gbcr_after);

    e1000e_dbg("E1000E: PHY autoneg restart ANAR %04x->%04x GBCR %04x->%04x BMCR %04x->%04x\n",
            anar_before, anar_after, gbcr_before, gbcr_after,
            bmcr_before, bmcr_after);
    e1000e_dbg("E1000E: PHY autoneg %s after %d ms (BMSR %04x -> %04x, LSTATUS=%u AN_COMPLETE=%u)\n",
            (bmsr & MII_BMSR_AN_COMPLETE) ? "complete" : "TIMEOUT",
            ms_waited, bmsr_before, bmsr2,
            (bmsr2 & MII_BMSR_LSTATUS) ? 1 : 0,
            (bmsr2 & MII_BMSR_AN_COMPLETE) ? 1 : 0);

    // Ground truth: read I82577_PHY_STATUS_2 (page 0 reg 26).  IEEE 802.3
    // leaves BMCR.SPEED bits undefined when AUTONEG_EN=1, so this is the
    // only authoritative source of the PHY's resolved speed.  If MAC
    // STATUS.SPEED disagrees with this register the fault is in the
    // MAC<->PHY internal interface, not in autoneg.
    uint16_t pss2 = 0;
    (void)e1000e_phy_read_at(dev, E1000E_PHY_ADDR, I82577_PHY_STATUS_2, &pss2);
    const char* phy_speed_str = "10";
    switch (pss2 & I82577_PHY_STATUS2_SPEED_MASK) {
        case I82577_PHY_STATUS2_SPEED_1000MBPS: phy_speed_str = "1000"; break;
        case I82577_PHY_STATUS2_SPEED_100MBPS:  phy_speed_str = "100";  break;
        default: phy_speed_str = "10"; break;
    }
    e1000e_dbg("E1000E: PHY ground-truth I82577_PHY_STATUS_2=%04x speed=%sMb/s MDIX=%u RevPol=%u\n",
            pss2, phy_speed_str,
            (pss2 & I82577_PHY_STATUS2_MDIX) ? 1 : 0,
            (pss2 & I82577_PHY_STATUS2_REV_POLARITY) ? 1 : 0);
#endif // E1000E_DBG

    return (bmsr & MII_BMSR_AN_COMPLETE) ? 0 : -1;
}

// LANPHYPC takeover sequence for SPT/CNP/TGP+ I218/I219.  On these parts,
// the PHY's clock and power gate is owned by the Management Engine until the OS driver explicitly
// takes ownership via CTRL.LANPHYPC_OVERRIDE.  When OVERRIDE=1 the
// LANPHYPC output pin is driven by VALUE; when VALUE=0 the pin is LOW
// which the PHY interprets as "power ON".  (VALUE=1 powers the PHY
// DOWN — getting that wrong manifests as "link LED was on at boot, goes
// off during driver init and stays off".)
static void __attribute__((unused))
e1000e_toggle_lanphypc_pch_lpt(e1000e_dev_t* dev) {
#if E1000E_DBG
    uint32_t before = e1000e_read(dev, E1000_CTRL);
    uint32_t mac_reg = before;
#else
    uint32_t mac_reg = e1000e_read(dev, E1000_CTRL);
#endif

    // Step 1: drop both OVERRIDE and VALUE so we don't glitch the gate
    //         to "off" while flipping OVERRIDE.
    mac_reg &= ~E1000_CTRL_LANPHYPC_OVERRIDE;
    mac_reg &= ~E1000_CTRL_LANPHYPC_VALUE;
    e1000e_write(dev, E1000_CTRL, mac_reg);
    (void)e1000e_read(dev, E1000_CTRL); // posting flush
    e1000e_delay_us(20);

    // Step 2: assert OVERRIDE alone (VALUE stays 0 = drive PHY power ON).
    mac_reg |= E1000_CTRL_LANPHYPC_OVERRIDE;
    e1000e_write(dev, E1000_CTRL, mac_reg);
    (void)e1000e_read(dev, E1000_CTRL); // posting flush
    e1000e_delay_us(50000); // 50 ms delay

    // Step 3: drop OVERRIDE — deliberately skipped.  On SPT-H the PHY clock
    //         domain is happiest with OVERRIDE left asserted after the
    //         50 ms wait so the ME does not perpetually contend for the
    //         gate input.

#if E1000E_DBG
    uint32_t after = e1000e_read(dev, E1000_CTRL);
    e1000e_dbg("E1000E: LANPHYPC toggled CTRL %08x -> %08x (OVERRIDE=1 VALUE=0 = PHY power ON)\n",
            before, after);
#endif
}

static void e1000e_wait_lan_init_done(e1000e_dev_t* dev) {
    uint32_t status = 0;
    uint32_t loops_left = E1000_ICH8_LAN_INIT_TIMEOUT;

    do {
        status = e1000e_read(dev, E1000_STATUS);
        if (status & E1000_STATUS_LAN_INIT_DONE) {
            break;
        }
        e1000e_delay_us(150);
    } while (--loops_left);

    if (!(status & E1000_STATUS_LAN_INIT_DONE)) {
        e1000e_dbg("E1000E: LAN_INIT_DONE timeout (STATUS=%08x)\n", status);
        return;
    }

    e1000e_dbg("E1000E: LAN_INIT_DONE after %u us (STATUS=%08x)\n",
            (E1000_ICH8_LAN_INIT_TIMEOUT - loops_left) * 150, status);

    e1000e_write(dev, E1000_STATUS, status & ~E1000_STATUS_LAN_INIT_DONE);
    e1000e_dbg("E1000E: LAN_INIT_DONE clear -> STATUS=%08x\n",
            e1000e_read(dev, E1000_STATUS));
}

// Apply the I217/I218/I219 (PCH-LAN) bring-up workarounds.
//
// Two things are required on these parts before RX/TX will flow:
//
//   1. CTRL_EXT.DRV_LOAD must be set so the Management Engine knows
//      an OS driver is now in charge of the NIC.  Without this the ME
//      considers itself the sole owner and silently filters frames.
//
//   2. Software MUST perform at least one MDIO transaction against the
//      integrated PHY to acknowledge the post-reset PHY state and
//      clear STATUS.PHYRA.  Until PHYRA is acknowledged the PHY gates
//      its data interface to the MAC and *no frames flow in either
//      direction* even though L1 link is up.  We do this under the ME
//      SW-flag so we don't race with ME firmware over MDIO ownership.
static void e1000e_pch_lan_init(e1000e_dev_t* dev) {
    e1000e_dbg("E1000E: applying PCH-LAN (I217/I218/I219) workarounds\n");

    // Configure CTRL_EXT correctly for an OS-driver scenario:
    //   * DRV_LOAD (bit 28)  : tell ME we now own the NIC
    //   * PHY_PDEN  (bit 20) : MUST be 0, otherwise the PHY is allowed
    //                          to enter low-power and the MAC sees no
    //                          link clock — RX silently dies.  NVM
    //                          default is 1b on PCH-LAN parts.
    //   * PBA_SUPPORT (bit 31): MUST be 0 in INTx/MSI mode (1b is the
    //                           MSI-X-only IMS->PBA-clear behaviour).
    //   * ASDCHK     (bit 12): self-clearing one-shot, force to 0 so a
    //                          stale value from BIOS doesn't re-fire
    //                          a speed-detection sequence on top of us.
    //   * RO_DIS     (bit 17): leave whatever NVM/BIOS programmed.
    //   * BIT(22)            : MUST be 1 per the I218/I219 datasheet.
    //                          On discrete 82574 this would be a LINK_MODE
    //                          bit, but on PCH-LAN parts the integrated PHY
    //                          makes LINK_MODE meaningless and BIT(22) is
    //                          re-purposed as a TX-arbitration mandatory bit
    //                          — without it the descriptor engine never
    //                          fetches even though TCTL.EN is honored.
    //   * PHY_PDEN  (bit 20) : SET on PCH-LAN per the datasheet. Only enables PHY
    //                          power-down when MAC is in D3, so harmless at
    //                          D0; required by spec for the PCH-LAN family.
    uint32_t ctrl_ext = e1000e_read(dev, E1000_CTRL_EXT);
    e1000e_dbg("E1000E: CTRL_EXT=%08x (before)\n", ctrl_ext);
    ctrl_ext |=  E1000_CTRL_EXT_DRV_LOAD;
    ctrl_ext &= ~((1u << 31)   // PBA_SUPPORT (MSI-X-only)
                | (1u << 12)   // ASDCHK (one-shot, force 0)
                | E1000_CTRL_EXT_FORCE_SMBUS
                | (1u << 23)); // LINK_MODE bit 1 (clear; bit 22 set below)
    ctrl_ext |= (1u << 22)     // datasheet-mandated TX-arbitration BIT(22)
              | (1u << 20);    // datasheet-mandated PHY_PDEN
    e1000e_write(dev, E1000_CTRL_EXT, ctrl_ext);
    e1000e_dbg("E1000E: CTRL_EXT=%08x (after DRV_LOAD/PHY_PDEN/BIT22/!PBA)\n",
            e1000e_read(dev, E1000_CTRL_EXT));

    e1000e_delay_us(2000);

    // ------------------------------------------------------------------
    // Hard MAC reset (CTRL.RST) AFTER asserting DRV_LOAD.
    //
    // On SPT-H / I219-LM the ME firmware leaves the MAC in a half-owned
    // state at boot: registers accept writes, link comes up, but the
    // TX descriptor-fetch DMA stays gated until the MAC is fully reset
    // *with the OS announcing ownership first*.  FWSM.FW_VALID=1 plus
    // FEXTNVM7 bit 31 set is the giveaway — that's "FW controls PHY
    // power management", and it disappears only after a CTRL.RST that
    // is performed under the SW-FLAG with DRV_LOAD already asserted.
    //
    // Per spec 4.6: CTRL.RST resets the MAC core but PRESERVES PCIe
    // config, CTRL_EXT, and the NVM-shadowed PHY config.  However it
    // DOES clear CTRL_EXT.DRV_LOAD on most PCH-LAN parts, so we have
    // to re-assert it after the reset completes.  We must also wait
    // for the bit to self-clear (typically <1 us, but spec allows up
    // to ~10 ms on PCH-LAN due to PHY re-link).
    {
        // ------------------------------------------------------------
        //  Full-chip reset — PCH-LAN reset sequence per Intel datasheet.
        //
        //  ROOT CAUSE of the I219-LM TX stall on the Lenovo P50:
        //  ME firmware on PCH-SPT-H is actively driving manageability
        //  TLPs through the LAN function's PCIe master at boot
        //  (FWSM.FW_VALID=1, MANC.EN_MNG2HOST=1, ICR.MNG fires
        //  spontaneously).  Per the datasheet:
        //
        //    "Prevent the PCI-E bus from sticking if there is no TLP
        //     connection on the last TLP read/write transaction when
        //     MAC is reset."
        //
        //  If CTRL.RST is asserted while an ME-driven TLP is in flight,
        //  the MAC's outbound DMA path gets wedged.  Symptom is exactly
        //  what we observe: TCTL.EN sticks, TDT tracks the doorbell,
        //  but TDH never advances, GPTC=0, TPT=0, and there are no
        //  PCI errors — because no TLP is ever issued from the wedged
        //  master.
        //
        //  Fix:
        //    1. Set CTRL.GIO_MASTER_DISABLE (bit 2) and poll
        //       STATUS.GIO_MASTER_EN (bit 19) to clear, draining all
        //       in-flight master TLPs.
        //    2. Mask interrupts, clear ICR.
        //    3. RCTL=0, TCTL=PSP (NOT 0 — PSP is the canonical PCH-LAN
        //       value during the quiesce window).
        //    4. Wait 10 ms for any pending transactions to retire.
        //    5. Issue full-chip reset: CTRL |= (PHY_RST | RST) in
        //       the SAME mmio write, under SWFLAG.  PHY_RST in the
        //       same write is what re-syncs the MAC<->PHY KMRN bus;
        //       without it the KMRN interface stays in whatever
        //       UEFI/ME left it and the MAC's TX scheduler can stay
        //       gated even though MDIO is responsive.
        //    6. Wait 20 ms (PHY reset path takes longer than MAC).
        // ------------------------------------------------------------

        // Step 1: Drain PCIe master.
        {
            uint32_t ctrl_drain = e1000e_read(dev, E1000_CTRL);
            e1000e_write(dev, E1000_CTRL,
                         ctrl_drain | (1u << 2) /* GIO_MASTER_DISABLE */);
            (void)e1000e_read(dev, E1000_STATUS);

            int drain_us = 0;
            uint32_t st_drain = 0;
            for (drain_us = 0; drain_us < 800000; drain_us += 100) {
                st_drain = e1000e_read(dev, E1000_STATUS);
                if (!(st_drain & (1u << 19) /* GIO_MASTER_EN */))
                    break;
                e1000e_delay_us(100);
            }
            e1000e_dbg("E1000E: PCIe master drain: %s after %d us (STATUS=%08x)\n",
                    (st_drain & (1u << 19)) ? "TIMEOUT" : "ok",
                    drain_us, st_drain);
        }

        // Step 2: mask interrupts, ack any pending.
        e1000e_write(dev, E1000_IMC, 0xFFFFFFFFu);
        (void)e1000e_read(dev, 0x000C0); // ICR

        // Step 3+4: quiesce TX/RX (TCTL=PSP per spec), wait 10 ms.
        e1000e_write(dev, E1000_RCTL, 0);
        e1000e_write(dev, E1000_TCTL, E1000_TCTL_PSP);
        (void)e1000e_read(dev, E1000_STATUS); // posting flush
        e1000e_delay_us(10000);

        // Step 5: full-chip reset under SWFLAG.
        if (e1000e_acquire_swflag(dev) != 0) {
            e1000e_dbg("E1000E: CTRL.RST: could not acquire SWFLAG, proceeding anyway\n");
        }
        uint32_t ctrl_pre = e1000e_read(dev, E1000_CTRL);
        e1000e_dbg("E1000E: CTRL.RST: asserting RST only (CTRL=%08x)\n", ctrl_pre);
        // PCH-LAN reset: write CTRL with E1000_CTRL_RST asserted.
        // We deliberately do NOT combine CTRL.PHY_RST with CTRL.RST on
        // PCH-LAN — doing so tears down the BIOS-negotiated PHY link in
        // the same cycle and leaves the PHY in a state from which only
        // the full PHY hardware-reset sequence (with HV PHY workarounds,
        // LCD config from NVM, MSE thresholds, etc.) can recover.  The
        // BIOS already brought up link before we got here; preserving
        // that link across our chip reset means the PHY just keeps
        // working.
        e1000e_write(dev, E1000_CTRL,
                     ctrl_pre | E1000_CTRL_RST);

        // Step 6: per the datasheet, msleep(20) — do NOT touch the chip
        // during this window; the spec says "cannot issue a flush
        // here because it hangs the hardware".
        e1000e_delay_us(20000);

        // Poll for RST self-clear, up to 20 ms more.
        int spun = 0;
        uint32_t ctrl_post = 0;
        for (spun = 0; spun < 2000; spun++) {
            ctrl_post = e1000e_read(dev, E1000_CTRL);
            if (!(ctrl_post & E1000_CTRL_RST)) break;
            e1000e_delay_us(10);
        }
        e1000e_dbg("E1000E: CTRL.RST: cleared after %d us (CTRL=%08x)\n",
                spun * 10, ctrl_post);

        // Reset clears interrupt mask state — re-mask everything for now.
        e1000e_write(dev, E1000_IMC, 0xFFFFFFFFu);
        (void)e1000e_read(dev, 0x000C0); // ICR read-to-clear
        e1000e_release_swflag(dev);

        // Re-assert DRV_LOAD (CTRL.RST cleared it on PCH-LAN) and
        // re-program CTRL_EXT per the I218/I219 datasheet:
        //   - DRV_LOAD (bit 28) : OS now owns the NIC
        //   - BIT(22)           : mandatory PCH-LAN TX-arbitration bit
        //   - PHY_PDEN (bit 20) : mandatory PCH-LAN+ bit
        //   - RO_DIS  (bit 17)  : disable PCIe relaxed-ordering for
        //                          descriptor coherency
        //   - clear PBA_SUPPORT (bit 31), ASDCHK (bit 12), FORCE_SMBUS
        //     (bit 11), and LINK_MODE bit 1 (bit 23).
        ctrl_ext = e1000e_read(dev, E1000_CTRL_EXT);
        ctrl_ext &= ~((1u << 31) | (1u << 12)
                | E1000_CTRL_EXT_FORCE_SMBUS
                | (1u << 23));
        ctrl_ext |=  E1000_CTRL_EXT_DRV_LOAD
                  |  E1000_CTRL_EXT_RO_DIS
                  |  (1u << 22)   // datasheet-mandated TX-arbitration BIT(22)
                  |  (1u << 20);  // datasheet-mandated PHY_PDEN
        e1000e_write(dev, E1000_CTRL_EXT, ctrl_ext);
        e1000e_dbg("E1000E: CTRL.RST: CTRL_EXT re-set to %08x (DRV_LOAD/PHY_PDEN/BIT22)\n",
            e1000e_read(dev, E1000_CTRL_EXT));

        // NOTE: do NOT call e1000e_toggle_lanphypc_pch_lpt here on this
        // ThinkPad P50 / I219-LM: empirically, before the toggle the PHY
        // is responsive (ID 0x154:0x00a6 readable at MDIO addr 2), and
        // after asserting CTRL.LANPHYPC_OVERRIDE the PHY goes silent
        // (every MDIO read returns 0xFFFF).  The toggle is needed
        // only when the ME left the PHY power-gated; the SPT-H BIOS on
        // this board does not.

        // PCH-LAN post-reset wait: after a full reset, wait until the
        // hardware finishes the basic LAN/PHY config sequence before
        // touching more PHY-facing state.
        e1000e_wait_lan_init_done(dev);

        // PCH-SPT family init reprograms PBA=26 after reset.  If the
        // reset/firmware default leaves the packet buffer split wrong,
        // the MAC can look alive while the TX datapath never actually
        // starts moving descriptors.
#if E1000E_DBG
        uint32_t pba_before = e1000e_read(dev, E1000_PBA);
#endif
        e1000e_write(dev, E1000_PBA, 26);
        e1000e_dbg("E1000E: PBA %08x -> %08x\n",
            pba_before, e1000e_read(dev, E1000_PBA));

        e1000e_delay_us(10000); // PHY re-link settling
    }
    // ------------------------------------------------------------------

    // Acknowledge the PHY via MDIO.  Hold the SW flag while we do it so
    // the ME doesn't race us on the MDIC bus.  Probe a few PHY addresses
    // because integrated PCH-LAN PHYs do not consistently sit at addr 1
    // (I219 frequently uses addr 2; some parts auto-select).  We read
    // the hardwired PHY-ID registers (MII regs 2 and 3) — a non-zero,
    // non-0xFFFF value identifies a real PHY at that address.
    if (e1000e_acquire_swflag(dev) == 0) {
        int found = -1;
        for (uint8_t addr = 0; addr <= 3; addr++) {
            uint16_t id1 = 0, id2 = 0;
            int rc1 = e1000e_phy_read_at(dev, addr, 2, &id1);
            int rc2 = e1000e_phy_read_at(dev, addr, 3, &id2);
            e1000e_dbg("E1000E: PHY probe addr=%u rc=%d/%d ID=%04x:%04x\n",
                    addr, rc1, rc2, id1, id2);
            if (rc1 == 0 && rc2 == 0 &&
                (id1 != 0x0000 && id1 != 0xFFFF)) {
                found = addr;
            }
        }
        if (found >= 0) {
            uint16_t bmcr = 0, bmsr = 0;
            e1000e_phy_read_at(dev, (uint8_t)found, 0, &bmcr);
            e1000e_phy_read_at(dev, (uint8_t)found, 1, &bmsr);
            e1000e_dbg("E1000E: PHY at addr %d BMCR=%04x BMSR=%04x\n",
                    found, bmcr, bmsr);
        } else {
            e1000e_dbg("E1000E: no PHY responded on MDIO addrs 0..3\n");
        }
        e1000e_release_swflag(dev);
    } else {
        e1000e_dbg("E1000E: could not acquire ME SW flag — skipping PHY probe\n");
    }

    // Clear STATUS.PHYRA.  Per 82574 datasheet 10.2.2.2: "This bit is
    // read/write.  Hardware sets this bit following the assertion of
    // PHY reset.  The bit is cleared on writing 0b to it."  So we
    // clear it by writing the rest of STATUS unchanged but with bit 10
    // set to 0 — NOT by W1C.  STATUS is otherwise read-only so writing
    // the whole register is safe.
    uint32_t st = e1000e_read(dev, E1000_STATUS);
    if (st & E1000_STATUS_PHYRA) {
        e1000e_write(dev, E1000_STATUS, st & ~E1000_STATUS_PHYRA);
    }

    // PCH-LAN TX-arbiter workaround.  TARC0 / TARC1 are the per-queue
    // TX-arbitration registers; on PCH-LAN parts (I217 / I218 / I219)
    // a few bits in these registers come out of reset cleared even
    // though the silicon REQUIRES them set in order to issue a TX
    // descriptor-fetch on the PCIe bus.  The symptom — and we hit it
    // exactly — is: TCTL.EN = 1, TXDCTL.ENABLE = 1, TDH != TDT, the
    // descriptor in host memory is well-formed (cmd has EOP|IFCS|RS,
    // length nonzero, status = 0), STATUS.GIO_MASTER_EN = 1, MSI
    // enabled — and yet TDH never advances and GPTC / TPT stay at
    // zero.  These magic bits are required by the I218/I219 datasheet's
    // hardware-init bit programming.
    //
    // TARC0 bits:
    //   23  — undocumented "TX queue arbitration count enable"
    //   24  — same family
    //   26  — same family (required on I219)
    //   27  — same family (required on I219)
    // TARC1 bits:
    //   24  set   — match TARC0
    //   28  clear — gates TX entirely on some I219 steppings
    {
        uint32_t tarc0 = e1000e_read(dev, E1000_TARC0);
#if E1000E_DBG
        uint32_t tarc0_before = tarc0;
#endif
        tarc0 |= (1u << 23) | (1u << 24) | (1u << 26) | (1u << 27);
        e1000e_write(dev, E1000_TARC0, tarc0);

        // TARC1: per the I218/I219 datasheet, set bits 24, 26, 30 (and
        // conditionally 28 based on TCTL.MULR).  We are not enabling MULR,
        // so we set bit 28 too.
        uint32_t tarc1 = e1000e_read(dev, E1000_TARC1);
#if E1000E_DBG
        uint32_t tarc1_before = tarc1;
#endif
        tarc1 |=  (1u << 24) | (1u << 26) | (1u << 28) | (1u << 30);
        e1000e_write(dev, E1000_TARC1, tarc1);

        e1000e_dbg("E1000E: TARC0 %08x -> %08x, TARC1 %08x -> %08x\n",
                tarc0_before, e1000e_read(dev, E1000_TARC0),
                tarc1_before, e1000e_read(dev, E1000_TARC1));
    }

    // PCH-LAN init also normalizes the PCIe DMA snoop policy through GCR.
    // Leaving the reset-time no-snoop state in place can make descriptor/
    // buffer DMA depend on platform firmware cache policy instead of the
    // host's normal coherent path.  For PCH-LAN the safe baseline is the
    // snooped mode: clear all no-snoop request bits.
    {
        uint32_t gcr_before = e1000e_read(dev, E1000_GCR);
        uint32_t gcr = gcr_before & ~E1000_GCR_NO_SNOOP_ALL;
        e1000e_write(dev, E1000_GCR, gcr);
        (void)e1000e_read(dev, E1000_GCR);
        e1000e_dbg("E1000E: GCR %08x -> %08x (PCIe DMA snoop policy)\n",
                gcr_before, e1000e_read(dev, E1000_GCR));
    }

    // ULP exit + "host owns config" handshake with the ME.
    //
    // On SPT-H / I219-LM the ME may have parked the LAN controller in
    // ULP (Ultra-Low-Power) before handing it to the OS.  In ULP the
    // MAC accepts register writes (TCTL.EN, TDT, etc. all stick) but
    // the TX descriptor-fetch DMA is gated off — exactly the symptom
    // we see: TDH stays 0, GPTC=0, descriptor.sta=0.  We exit ULP
    // by clearing H2ME.ULP and asserting H2ME.ENFORCE_SETTINGS so the
    // ME knows the host driver has taken over and must not re-arm ULP.
    {
        uint32_t h2me_before = e1000e_read(dev, E1000_H2ME);
        uint32_t h2me = h2me_before;
        h2me &= ~E1000_H2ME_ULP;
        h2me |=  E1000_H2ME_ENFORCE_SETTINGS;
        e1000e_write(dev, E1000_H2ME, h2me);
        e1000e_delay_us(10000);
        e1000e_dbg("E1000E: H2ME %08x -> %08x (ULP cleared, ENFORCE_SETTINGS set)\n",
                h2me_before, e1000e_read(dev, E1000_H2ME));
    }

    // PCH ULP exit is not just the H2ME handshake.  The PHY-side SMBus
    // latch and I218 ULP state must also be cleared, otherwise the MAC can
    // accept descriptor doorbells while the MAC<->PHY path still sits in a
    // reset-to-SMBus / ULP state and TX fetch never starts.
    e1000e_force_exit_phy_ulp(dev);

    // The remaining SPT/LPT init path has two MAC-side pieces required by
    // the datasheet that we did not previously carry over: enable packet-
    // buffer ECC / memory error handling, and explicitly exit the S0ix-
    // style dynamic power-gating state.  On the failing P50 the key clue
    // is FEXTNVM7 staying at 0x80000000, which is the exact "time-sync
    // clock disabled / still power-gated" pattern that must be cleared on
    // resume before allowing normal DMA again.
    {
        uint32_t pbeccsts_before = e1000e_read(dev, E1000_PBECCSTS);
        uint32_t pbeccsts = pbeccsts_before | E1000_PBECCSTS_ECC_ENABLE;
        e1000e_write(dev, E1000_PBECCSTS, pbeccsts);

        uint32_t ctrl_before = e1000e_read(dev, E1000_CTRL);
        uint32_t ctrl = ctrl_before | E1000_CTRL_MEHE;
        e1000e_write(dev, E1000_CTRL, ctrl);

        uint32_t dpgfr_before = e1000e_read(dev, E1000_DPGFR);
        uint32_t dpgfr = dpgfr_before & ~(1u << 2);
        e1000e_write(dev, E1000_DPGFR, dpgfr);

        uint32_t ctrl_ext_before = e1000e_read(dev, E1000_CTRL_EXT);
        uint32_t ctrl_ext = ctrl_ext_before &
                            ~(E1000_CTRL_EXT_DPG_EN |
                              E1000_CTRL_EXT_DMA_DYN_CLK_EN);
        ctrl_ext |= E1000_CTRL_EXT_RO_DIS;
        e1000e_write(dev, E1000_CTRL_EXT, ctrl_ext);

        uint32_t fextnvm7_before = e1000e_read(dev, E1000_FEXTNVM7);
        uint32_t fextnvm7 = fextnvm7_before;
        fextnvm7 &= ~E1000_FEXTNVM7_DISABLE_TSYNC_CLK;
        fextnvm7 |= E1000_FEXTNVM7_ENABLE_TSYNC_INTR;
        if (e1000e_is_pch_spt(dev->pci_dev->device_id)) {
            fextnvm7 |= E1000_FEXTNVM7_SIDE_CLK_UNGATE;
        }
        e1000e_write(dev, E1000_FEXTNVM7, fextnvm7);

        e1000e_dbg("E1000E: MAC DPG exit PBECCSTS=%08x->%08x CTRL=%08x->%08x DPGFR=%08x->%08x CTRL_EXT=%08x->%08x FEXTNVM7=%08x->%08x\n",
                pbeccsts_before, e1000e_read(dev, E1000_PBECCSTS),
                ctrl_before, e1000e_read(dev, E1000_CTRL),
                dpgfr_before, e1000e_read(dev, E1000_DPGFR),
                ctrl_ext_before, e1000e_read(dev, E1000_CTRL_EXT),
                fextnvm7_before, e1000e_read(dev, E1000_FEXTNVM7));
    }

    // The PCH-LAN reset path unconditionally sets KABGTXD.BGSQLBIAS after
    // the global reset.  Without this the AFE bandgap reference for the
    // Tx path can stay in a metastable bias state on PCH-LAN parts, which
    // silently gates the per-queue Tx DMA fetch even though every ring/
    // control register reads back as configured.  This is one of the two
    // writes that are unconditional in the datasheet's reset path; we
    // were missing it.
    {
        uint32_t kabgtxd_before = e1000e_read(dev, E1000_KABGTXD);
        uint32_t kabgtxd = kabgtxd_before | E1000_KABGTXD_BGSQLBIAS;
        e1000e_write(dev, E1000_KABGTXD, kabgtxd);
        e1000e_dbg("E1000E: KABGTXD %08x -> %08x (BGSQLBIAS set)\n",
                kabgtxd_before, e1000e_read(dev, E1000_KABGTXD));
    }

    // One-shot diagnostic dump of registers that commonly gate TX on
    // PCH-LAN parts when the driver looks otherwise correct.
    e1000e_dbg("E1000E: MANC=%08x FWSM=%08x FEXTNVM=%08x FEXTNVM6=%08x FEXTNVM7=%08x\n",
            e1000e_read(dev, E1000_MANC),
            e1000e_read(dev, E1000_FWSM),
            e1000e_read(dev, E1000_FEXTNVM),
            e1000e_read(dev, E1000_FEXTNVM6),
            e1000e_read(dev, E1000_FEXTNVM7));
}

// ============================================================================
// KMRN (Kumeran) MAC↔PHY interconnect access
// ============================================================================
//
// On PCH-LAN parts the MAC and the in-package PHY are not connected by
// a real GMII bus — instead they speak a serial side-band protocol
// called "Kumeran" over an internal interface.  KMRNCTRLSTA (MAC reg
// 0x00034) is the doorbell used by software to read/write Kumeran
// configuration registers that live on the PHY side of the link.
//
// Layout of KMRNCTRLSTA:
//   bits 15:0   data (read after REN completes; write payload when REN=0)
//   bits 22:16  REGADDR — Kumeran register offset
//   bit  21     REN     — Read ENable; set to start a read transaction
//
// (Often called read_kmrn_reg / write_kmrn_reg in vendor sample code.)
#define E1000_KMRNCTRLSTA               0x00034
#define E1000_KMRNCTRLSTA_OFFSET_SHIFT  16
#define E1000_KMRNCTRLSTA_OFFSET_MASK   0x001F0000u
#define E1000_KMRNCTRLSTA_REN           0x00200000u

// Kumeran register offsets we care about.
#define E1000_KMRN_K1_CONFIG_OFFSET     0x07
#define E1000_KMRN_K1_ENABLE            0x0002  // bit 1 of the 16-bit data
#define E1000_KMRN_HD_CTRL_OFFSET       0x10    // Half-Duplex / link timing
#define E1000_KMRN_HD_CTRL_10_100_DEFAULT  0x0004
#define E1000_KMRN_HD_CTRL_1000_DEFAULT    0x0000
#define E1000_KMRN_INBAND_PARAM_OFFSET  0x09    // Inband control parameters

static int e1000e_kmrn_read(e1000e_dev_t* dev, uint8_t kmrn_off, uint16_t* val) {
    uint32_t cmd = (((uint32_t)kmrn_off << E1000_KMRNCTRLSTA_OFFSET_SHIFT)
                    & E1000_KMRNCTRLSTA_OFFSET_MASK)
                 | E1000_KMRNCTRLSTA_REN;
    e1000e_write(dev, E1000_KMRNCTRLSTA, cmd);
    e1000e_delay_us(2);
    uint32_t r = e1000e_read(dev, E1000_KMRNCTRLSTA);
    if (val) *val = (uint16_t)(r & 0xFFFF);
    return 0;
}

static int e1000e_kmrn_write(e1000e_dev_t* dev, uint8_t kmrn_off, uint16_t val) {
    uint32_t cmd = (((uint32_t)kmrn_off << E1000_KMRNCTRLSTA_OFFSET_SHIFT)
                    & E1000_KMRNCTRLSTA_OFFSET_MASK)
                 | (uint32_t)val;
    e1000e_write(dev, E1000_KMRNCTRLSTA, cmd);
    e1000e_delay_us(2);
    return 0;
}

// Force the Kumeran K1 power-save state OFF.
//
// K1 is a low-power state that the Kumeran link enters between bursts.
// On many I217/I218/I219 steppings — and *consistently* on SPT-H I219-LM
// (dev 0x15B7) at 100 Mbps full-duplex — a hardware bug in the K1
// state machine causes EVERY frame received from the wire to be dropped
// inside the MAC↔PHY interconnect.  STATUS reports link UP, the PHY
// reports a clean autoneg, but TPR (Total Packets Received, which
// counts every frame at the MAC *before* address filtering) stays at
// zero.  Disabling K1 makes the interconnect always-on at the cost of
// a few mW and immediately fixes the RX path.  This is exactly the
// fix per the I218/I219 datasheet: configure K1 with k1_enable=false.
static void e1000e_disable_k1(e1000e_dev_t* dev) {
    uint16_t k1 = 0;
    e1000e_kmrn_read(dev, E1000_KMRN_K1_CONFIG_OFFSET, &k1);
#if E1000E_DBG
    uint16_t before = k1;
#endif
    k1 &= ~E1000_KMRN_K1_ENABLE;
    e1000e_kmrn_write(dev, E1000_KMRN_K1_CONFIG_OFFSET, k1);

    // Per Intel: after touching K1 we must briefly toggle CTRL_EXT.SPD_BYPS
    // around a CTRL.SPEED rewrite so the MAC re-syncs its internal clock
    // tree to the new K1 setting.  SPD_BYPS = CTRL_EXT bit 15.
    #define E1000_CTRL_EXT_SPD_BYPS (1u << 15)
    uint32_t ext = e1000e_read(dev, E1000_CTRL_EXT);
    e1000e_write(dev, E1000_CTRL_EXT, ext | E1000_CTRL_EXT_SPD_BYPS);
    (void)e1000e_read(dev, E1000_STATUS);  // posted-write flush
    e1000e_delay_us(20);

    uint32_t ctrl = e1000e_read(dev, E1000_CTRL);
    e1000e_write(dev, E1000_CTRL, ctrl);   // rewrite triggers the resync
    (void)e1000e_read(dev, E1000_STATUS);
    e1000e_delay_us(20);

    e1000e_write(dev, E1000_CTRL_EXT, ext & ~E1000_CTRL_EXT_SPD_BYPS);

#if E1000E_DBG
    uint16_t after = 0;
    e1000e_kmrn_read(dev, E1000_KMRN_K1_CONFIG_OFFSET, &after);
#endif
    e1000e_dbg("E1000E: K1 disabled (KMRN[0x07]: %04x -> %04x)\n", before, after);
}

// Configure the MAC↔PHY Kumeran interconnect for 10/100-Mbps timing.
//
// On PCH-LAN parts the MAC and PHY talk over an internal serial link
// ("Kumeran") whose RX clock domain timing differs between 10/100 and
// 1000 Mbps.  Out of reset the HD_CTRL register at KMRN offset 0x10
// holds whatever default the silicon picked — which for many
// SPT/CNP/ICP I219 steppings is the 1000-Mbps value (0x0000) even when
// the PHY auto-negotiates down to 100 Mbps.  In that mismatched state
// the MAC's RX deserializer rejects every nibble it sees from the PHY,
// so STATUS.LU is asserted, the PHY shows clean RX activity, but TPR
// stays at 0 and not a single frame ever reaches the descriptor ring.
//
// We fix this by configuring KMRN HD_CTRL for the resolved 10/100 timing.
// The HD_CTRL bits 2:0 carry the inter-packet-gap timing; 0x4 is the
// documented "10/100" default, 0x0 is the "1000" default.  We additionally
// keep TIPG.IPGT at the canonical 10/100 value (it is harmless at full-
// duplex but required at half-duplex which the spec says we must still
// support).
// TIPG is programmed once globally in e1000e_init_tx() with the canonical
// PCH-LAN value (IPGT=8, IPGR1=8, IPGR2=6 = 0x00602008).  The KMRN helpers
// only touch the KMRN HD_CTRL register's per-speed inter-packet-gap field.
// A previous version of this file forced TIPG.IPGT to 0xFF here on the
// (incorrect) belief that it was the canonical 10/100 value; in fact
// IPGT=0xFF parks the MAC's per-packet IPG timer for so long that the TX
// scheduler never dispatches a frame even though the descriptor is in
// the on-chip cache — hardware reports ICR.TXQE while TDH stays at 0
// and TDFH never advances past TDFHS, exactly the symptom we observed
// on real I219-LM hardware.
static void e1000e_configure_kmrn_for_10_100(e1000e_dev_t* dev) {
    uint16_t hd = 0;
    e1000e_kmrn_read(dev, E1000_KMRN_HD_CTRL_OFFSET, &hd);
#if E1000E_DBG
    uint16_t before = hd;
#endif
    hd &= ~0x0007u;                       // clear inter-packet-gap field
    hd |=  E1000_KMRN_HD_CTRL_10_100_DEFAULT;
    e1000e_kmrn_write(dev, E1000_KMRN_HD_CTRL_OFFSET, hd);

#if E1000E_DBG
    uint16_t after = 0;
    e1000e_kmrn_read(dev, E1000_KMRN_HD_CTRL_OFFSET, &after);
#endif
    e1000e_dbg("E1000E: KMRN HD_CTRL for 10/100 (KMRN[0x10]: %04x -> %04x)\n",
            before, after);
}

static void e1000e_configure_kmrn_for_1000(e1000e_dev_t* dev) {
    uint16_t hd = 0;
    e1000e_kmrn_read(dev, E1000_KMRN_HD_CTRL_OFFSET, &hd);
#if E1000E_DBG
    uint16_t before = hd;
#endif
    hd &= ~0x0007u;
    hd |=  E1000_KMRN_HD_CTRL_1000_DEFAULT;
    e1000e_kmrn_write(dev, E1000_KMRN_HD_CTRL_OFFSET, hd);

#if E1000E_DBG
    uint16_t after = 0;
    e1000e_kmrn_read(dev, E1000_KMRN_HD_CTRL_OFFSET, &after);
#endif
    e1000e_dbg("E1000E: KMRN HD_CTRL for 1000 (KMRN[0x10]: %04x -> %04x)\n",
            before, after);
}

// ============================================================================
// EEPROM Read
// ============================================================================
static uint16_t e1000e_read_eeprom(e1000e_dev_t* dev, uint8_t addr) {
    e1000e_write(dev, E1000_EERD,
                 (uint32_t)addr << E1000_EERD_ADDR_SHIFT | E1000_EERD_START);
    uint32_t val;
    for (int i = 0; i < 10000; i++) {
        val = e1000e_read(dev, E1000_EERD);
        if (val & E1000_EERD_DONE) {
            return (uint16_t)(val >> E1000_EERD_DATA_SHIFT);
        }
    }
    return 0;
}

// ============================================================================
// Read MAC Address
// ============================================================================
static void e1000e_read_mac(e1000e_dev_t* dev) {
    uint16_t w0 = e1000e_read_eeprom(dev, 0);
    uint16_t w1 = e1000e_read_eeprom(dev, 1);
    uint16_t w2 = e1000e_read_eeprom(dev, 2);

    if (w0 != 0 || w1 != 0 || w2 != 0) {
        dev->mac_addr[0] = w0 & 0xFF;
        dev->mac_addr[1] = (w0 >> 8) & 0xFF;
        dev->mac_addr[2] = w1 & 0xFF;
        dev->mac_addr[3] = (w1 >> 8) & 0xFF;
        dev->mac_addr[4] = w2 & 0xFF;
        dev->mac_addr[5] = (w2 >> 8) & 0xFF;
    } else {
        // Fallback: read from RAL/RAH (preprogrammed by firmware)
        uint32_t ral = e1000e_read(dev, E1000_RAL);
        uint32_t rah = e1000e_read(dev, E1000_RAH);
        dev->mac_addr[0] = ral & 0xFF;
        dev->mac_addr[1] = (ral >> 8) & 0xFF;
        dev->mac_addr[2] = (ral >> 16) & 0xFF;
        dev->mac_addr[3] = (ral >> 24) & 0xFF;
        dev->mac_addr[4] = rah & 0xFF;
        dev->mac_addr[5] = (rah >> 8) & 0xFF;
    }
}

// ============================================================================
// PM Timer-based microsecond delay
// ============================================================================
static void e1000e_delay_us(uint32_t us) {
    uint32_t t0 = timer_pmtimer_read_raw();
    if (t0 == 0) {
        for (volatile uint32_t i = 0; i < us * 4; i++)
            __asm__ volatile("pause");
        return;
    }
    while (timer_pmtimer_delta_us(t0, timer_pmtimer_read_raw()) < us)
        __asm__ volatile("pause");
}

static void e1000e_make_dma_pages_uncached(uint64_t phys_base,
                                           uint32_t page_count,
                                           const char* tag) {
    (void)tag;
    if (!phys_base || page_count == 0) {
        return;
    }

    uint64_t phys_last = phys_base + ((uint64_t)page_count - 1) * PAGE_SIZE;
    if (!is_phys_in_direct_map(phys_last)) {
        e1000e_dbg("E1000E: DMA region %s outside direct map (%llx..%llx)\n",
                tag,
                (unsigned long long)phys_base,
                (unsigned long long)phys_last);
        return;
    }

    uint64_t flags = PAGE_PRESENT | PAGE_WRITABLE | PAGE_WRITE_THROUGH |
                     PAGE_CACHE_DISABLE | PAGE_GLOBAL | PAGE_NO_EXECUTE;
    uint64_t virt_base = (uint64_t)phys_to_virt(phys_base);

    for (uint32_t page = 0; page < page_count; page++) {
        uint64_t page_phys = phys_base + (uint64_t)page * PAGE_SIZE;
        uint64_t page_virt = virt_base + (uint64_t)page * PAGE_SIZE;
        mm_map_page(page_virt, page_phys, flags);
    }

    if (smp_is_enabled()) {
        smp_tlb_shootdown_sync();
    }

    /*e1000e_dbg("E1000E: DMA region %s remapped UC (%llx..%llx)\n",
            tag,
            (unsigned long long)phys_base,
            (unsigned long long)phys_last);*/
}

// ============================================================================
// Soft Reset
// ============================================================================
static void e1000e_reset(e1000e_dev_t* dev) {
    // Mask + clear all interrupts
    e1000e_write(dev, E1000_IMC, 0xFFFFFFFF);
    (void)e1000e_read(dev, E1000_ICR);

    // Soft reset of the data path only — leave PHY/link untouched, the UEFI
    // firmware already negotiated.  A full CTRL.RST on e1000e parts can take many
    // seconds for link-up under hypervisors and breaks RX in some
    // some hypervisor builds, exactly as on the older e1000.
    e1000e_write(dev, E1000_RCTL, 0);
    e1000e_write(dev, E1000_TCTL, 0);

    e1000e_delay_us(10000);

    e1000e_write(dev, E1000_IMC, 0xFFFFFFFF);
    (void)e1000e_read(dev, E1000_ICR);
}

// ============================================================================
// RX Ring Initialization
// ============================================================================
static int e1000e_init_rx(e1000e_dev_t* dev) {
    uint32_t rx_ring_size = sizeof(e1000_rx_desc_legacy_t) * E1000E_NUM_RX_DESC;
    uint32_t rx_ring_pages = (rx_ring_size + PAGE_SIZE - 1) / PAGE_SIZE;

    uint64_t rx_phys = mm_allocate_contiguous_pages(rx_ring_pages);
    if (!rx_phys) {
        kprintf("E1000E: Failed to allocate RX descriptor ring\n");
        return -1;
    }

    dev->rx_descs = (e1000_rx_desc_legacy_t*)phys_to_virt(rx_phys);
    dev->rx_descs_phys = rx_phys;
    e1000e_make_dma_pages_uncached(rx_phys, rx_ring_pages, "rx-desc");

    for (uint32_t i = 0; i < rx_ring_size / 8; i++)
        ((uint64_t*)dev->rx_descs)[i] = 0;

    for (int i = 0; i < E1000E_NUM_RX_DESC; i++) {
        uint64_t buf_phys = mm_allocate_physical_page();
        if (!buf_phys) {
            kprintf("E1000E: Failed to allocate RX buffer %d\n", i);
            return -1;
        }
        dev->rx_bufs[i] = (uint8_t*)phys_to_virt(buf_phys);
        dev->rx_bufs_phys[i] = buf_phys;
        e1000e_make_dma_pages_uncached(buf_phys, 1, "rx-buf");

        dev->rx_descs[i].buffer_addr = buf_phys;
        dev->rx_descs[i].status = 0;
    }

    e1000e_write(dev, E1000_RDBAL, (uint32_t)(rx_phys & 0xFFFFFFFF));
    e1000e_write(dev, E1000_RDBAH, (uint32_t)(rx_phys >> 32));
    e1000e_write(dev, E1000_RDLEN, rx_ring_size);
    e1000e_write(dev, E1000_RDH, 0);
    e1000e_write(dev, E1000_RDT, E1000E_NUM_RX_DESC - 1);

    // Enable the per-queue RX descriptor fetch engine.  On all
    // post-ICH8 Intel parts (82571/2/4, 82576+, ICH9/10, PCH-LAN) the
    // legacy RCTL.EN bit only enables the MAC's RX framer — the actual
    // descriptor-DMA engine is gated by RXDCTL.ENABLE (bit 25), which
    // resets to 0.  Forgetting this bit is the textbook "link UP, RCTL
    // configured, but RDH never advances and TPR stays 0" failure mode
    // on I218/I219.  Intel datasheet 13.5.2: must be set BEFORE
    // RCTL.EN.  We also bump GRAN (bit 24, 1=descriptors not cache
    // lines) and leave the prefetch/host/writeback thresholds at their
    // hardware defaults which the I218/I219 datasheet considers sane.
    {
        uint32_t rxdctl = e1000e_read(dev, E1000_RXDCTL);
        rxdctl |= (1u << 24)   // GRAN: thresholds in descriptors
                | (1u << 25);  // ENABLE
        e1000e_write(dev, E1000_RXDCTL, rxdctl);
    }

    uint32_t rctl = E1000_RCTL_EN
                  | E1000_RCTL_BAM
                  | E1000_RCTL_BSIZE_2048
                  | E1000_RCTL_SECRC;
    e1000e_write(dev, E1000_RCTL, rctl);

    dev->rx_tail = E1000E_NUM_RX_DESC - 1;
    return 0;
}

// ============================================================================
// TX Ring Initialization
// ============================================================================
static int e1000e_init_tx(e1000e_dev_t* dev) {
    uint32_t tx_ring_size = sizeof(e1000_tx_desc_legacy_t) * E1000E_NUM_TX_DESC;
    uint32_t tx_ring_pages = (tx_ring_size + PAGE_SIZE - 1) / PAGE_SIZE;

    uint64_t tx_phys = mm_allocate_contiguous_pages(tx_ring_pages);
    if (!tx_phys) {
        kprintf("E1000E: Failed to allocate TX descriptor ring\n");
        return -1;
    }

    dev->tx_descs = (e1000_tx_desc_legacy_t*)phys_to_virt(tx_phys);
    dev->tx_descs_phys = tx_phys;
    e1000e_make_dma_pages_uncached(tx_phys, tx_ring_pages, "tx-desc");

    for (uint32_t i = 0; i < tx_ring_size / 8; i++)
        ((uint64_t*)dev->tx_descs)[i] = 0;

    for (int i = 0; i < E1000E_NUM_TX_DESC; i++) {
        uint64_t buf_phys = mm_allocate_physical_page();
        if (!buf_phys) {
            kprintf("E1000E: Failed to allocate TX buffer %d\n", i);
            return -1;
        }
        dev->tx_bufs[i] = (uint8_t*)phys_to_virt(buf_phys);
        dev->tx_bufs_phys[i] = buf_phys;
        e1000e_make_dma_pages_uncached(buf_phys, 1, "tx-buf");
    }

    e1000e_write(dev, E1000_TDBAL, (uint32_t)(tx_phys & 0xFFFFFFFF));
    e1000e_write(dev, E1000_TDBAH, (uint32_t)(tx_phys >> 32));
    e1000e_write(dev, E1000_TDLEN, tx_ring_size);
    e1000e_write(dev, E1000_TDH, 0);
    e1000e_write(dev, E1000_TDT, 0);

    // TIPG: canonical PCH-LAN value.
    //   IPGT  = 8   (bits  9:0 ) — inter-packet-gap transmit time
    //   IPGR1 = 8   (bits 19:10) — non-back-to-back IPG part 1
    //   IPGR2 = 6   (bits 29:20) — non-back-to-back IPG part 2
    // Per the I218/I219 datasheet, these exact values apply for PCH-LAN
    // regardless of negotiated speed.  IPGT must NOT be 0xFF — that parks
    // the IPG timer effectively forever, causing TXQE to assert and TDH
    // to never advance even with a valid descriptor cached on-chip.
    e1000e_write(dev, E1000_TIPG,
                 (8u <<  0) | (8u << 10) | (6u << 20));
    // TIDV — Transmit Interrupt Delay Value.  Use the canonical PCH-LAN
    // default of 8 (units of 1.024us, ~8.2us).  This batches Tx-completion
    // interrupts a little so we don't take one IRQ per packet under load,
    // matching what working hardware ground-truth shows on PCH-LAN.  Pure
    // latency tuning — does not gate TX.
    e1000e_write(dev, E1000_TIDV, 8);
    e1000e_write(dev, E1000_TADV, 0);

    // TXDCTL — use the canonical PCH-LAN values from the datasheet:
    //
    //   txdctl = (txdctl & ~TXDCTL_WTHRESH) | TXDCTL_FULL_TX_DESC_WB
    //   txdctl = (txdctl & ~TXDCTL_PTHRESH) | TXDCTL_MAX_TX_DESC_PREFETCH
    // with TXDCTL_FULL_TX_DESC_WB     = 0x01010000 (GRAN=1, WTHRESH=1)
    //      TXDCTL_MAX_TX_DESC_PREFETCH = 0x0100001F (GRAN=1, PTHRESH=31)
    // and the hardware-init bit programming additionally requires BIT(22).
    //
    // Final value: 0x0141001F.
    //
    // Earlier we ORed in BIT(25) believing it was a per-queue ENABLE bit
    // mirroring i350-family RXDCTL/TXDCTL.  PCH-LAN has no per-queue ENABLE;
    // TX is enabled exclusively by TCTL.EN.  Bit 25 on PCH-LAN belongs to
    // the LWTHRESH (low writeback-threshold) field, and writing 1 there
    // configures a Tx writeback policy that the datasheet explicitly does
    // not recommend.  This brings us back to the canonical PCH-LAN init
    // values byte-for-byte.
    {
        uint32_t txdctl = e1000e_read(dev, E1000_TXDCTL);
        txdctl &= ~(E1000_TXDCTL_PTHRESH | E1000_TXDCTL_WTHRESH);
        txdctl |= 0x01010000u    // FULL_TX_DESC_WB    (GRAN=1, WTHRESH=1)
               |  0x0100001Fu    // MAX_TX_DESC_PREFETCH (GRAN=1, PTHRESH=31)
               |  (1u << 22);    // datasheet hardware-init: TXDCTL |= BIT(22)
        e1000e_write(dev, E1000_TXDCTL, txdctl);

        // Mirror the same TXDCTL programming into queue 1 so the shared
        // TX arbitration block comes up in a known-good state.
        uint32_t txdctl1 = e1000e_read(dev, E1000_TXDCTL1);
        txdctl1 &= ~(E1000_TXDCTL_PTHRESH | E1000_TXDCTL_WTHRESH);
        txdctl1 |= 0x01010000u | 0x0100001Fu | (1u << 22);
        e1000e_write(dev, E1000_TXDCTL1, txdctl1);

        e1000e_dbg("E1000E: TXDCTL0=%08x TXDCTL1=%08x (PCH-LAN canonical)\n",
            e1000e_read(dev, E1000_TXDCTL),
            e1000e_read(dev, E1000_TXDCTL1));
    }

    uint32_t tctl = E1000_TCTL_EN
                  | E1000_TCTL_PSP
                  | (15 << E1000_TCTL_CT_SHIFT)
                  | E1000_TCTL_RTLC;
    e1000e_write(dev, E1000_TCTL, tctl);

    // Generic collision-distance config: use the default 63-slot COLD value
    // and force the write out before continuing.
    tctl = e1000e_read(dev, E1000_TCTL);
    tctl &= ~E1000_TCTL_COLD;
    tctl |= (63u << E1000_TCTL_COLD_SHIFT);
    e1000e_write(dev, E1000_TCTL, tctl);
    (void)e1000e_read(dev, E1000_TCTL);

    // CRITICAL on PCH-LAN (I217/I218/I219): enable MULR (Multiple Request,
    // bit 28) on TCTL.  Without it the TX engine issues a single descriptor
    // read request and waits forever for a completion-gating event that
    // never arrives on PCH-SPT, exactly matching the symptom we see on the
    // ThinkPad P50 / I219-LM: TDFH advances by 16 bytes (one descriptor's
    // worth fetched), then TDH/GPTC/TPT stay at 0 with no errors.
    //
    // Working register dumps from healthy hardware on the same board show
    // TCTL=0x3103F0FA — bit 28 (MULR) AND bit 29 (RRTHRESH low bit) set.
    // RRTHRESH is the hardware-default outstanding-read-request threshold.
    // We set both explicitly to match the known-good configuration byte-for-
    // byte.  We already disabled the buggy MULR-fix in FEXTNVM11 above, so
    // MULR is safe.
    if (e1000e_is_pch_lan(dev->device_id)) {
        tctl = e1000e_read(dev, E1000_TCTL);
#if E1000E_DBG
        uint32_t before = tctl;
#endif
        tctl |= (1u << 28)   // MULR — Multiple Request enable
              | (1u << 29);  // RRTHRESH[0] — match HW default
        e1000e_write(dev, E1000_TCTL, tctl);
        (void)e1000e_read(dev, E1000_TCTL);
#if E1000E_DBG
        e1000e_dbg("E1000E: TCTL %08x -> %08x (MULR + RRTHRESH set)\n",
                before, e1000e_read(dev, E1000_TCTL));
#endif
    }

    if (e1000e_is_pch_spt(dev->device_id)) {
        uint32_t iosfpc = e1000e_read(dev, E1000_IOSFPC);
        uint32_t fextnvm11 = e1000e_read(dev, E1000_FEXTNVM11);
        uint32_t tarc0 = e1000e_read(dev, E1000_TARC0);

        iosfpc |= (1u << 16);
        e1000e_write(dev, E1000_IOSFPC, iosfpc);

        fextnvm11 |= E1000_FEXTNVM11_DISABLE_MULR_FIX;
        e1000e_write(dev, E1000_FEXTNVM11, fextnvm11);

        tarc0 &= ~E1000_TARC0_CB_MULTIQ_3_REQ;
        tarc0 |= E1000_TARC0_CB_MULTIQ_2_REQ;
        e1000e_write(dev, E1000_TARC0, tarc0);

        e1000e_dbg("E1000E: SPT tx-hang WA IOSFPC=%08x FEXTNVM11=%08x TARC0=%08x\n",
                e1000e_read(dev, E1000_IOSFPC),
                e1000e_read(dev, E1000_FEXTNVM11),
                e1000e_read(dev, E1000_TARC0));
    }

    dev->tx_tail = 0;
    return 0;
}

// ============================================================================
// Send (net_device_t callback)
// ============================================================================
// Forward declaration: defined alongside e1000e_link_status() below.
static void e1000e_link_state_poll(e1000e_dev_t* dev);

static int e1000e_send(net_device_t* ndev, const uint8_t* data, uint16_t len) {
    e1000e_dev_t* dev = (e1000e_dev_t*)ndev->driver_data;

    // Always service the link state on the TX path.  After a cable
    // replug we must re-clear LPLU on the PHY or the descriptor will
    // be fetched but the buffer payload will never be pulled across
    // the MII interface (silent TX drop).  Polling unconditionally
    // (rather than gating on link_change_pending) closes the race
    // where userland calls send() before the LSC IRQ has been taken.
    e1000e_link_state_poll(dev);

    if (len > E1000E_RX_BUF_SIZE) return -1;

    // Serialize across CPUs: tx_tail, descriptor slot, and TDT MMIO
    // write form one critical section; without this two CPUs can
    // (a) read the same tx_tail, (b) write the same descriptor slot,
    // and (c) lose one packet to the duplicate-tail-bump.  Hold IRQs
    // disabled for the few MMIO cycles so an IRQ-tail TX (e.g. softirq
    // sending a TCP ACK) cannot preempt user-context TX from the same
    // CPU mid-slot.
    uint64_t txflags;
    spin_lock_irqsave(&dev->tx_lock, &txflags);

    uint16_t tail = dev->tx_tail;
    volatile e1000_tx_desc_legacy_t* desc = &dev->tx_descs[tail];
    if (tail != 0 || desc->cmd != 0) {
        if (!(desc->status & E1000_TXD_STAT_DD) && desc->cmd != 0) {
            ndev->tx_errors++;
            spin_unlock_irqrestore(&dev->tx_lock, txflags);
            return -1;
        }
    }

    for (uint16_t i = 0; i < len; i++)
        dev->tx_bufs[tail][i] = data[i];

    desc->buffer_addr = dev->tx_bufs_phys[tail];
    desc->length = len;
    desc->cso = 0;
    desc->cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS | E1000_TXD_CMD_RS;
    desc->status = 0;
    desc->css = 0;
    desc->special = 0;

    __asm__ volatile("mfence" ::: "memory");

    dev->tx_tail = (tail + 1) % E1000E_NUM_TX_DESC;
    e1000e_write(dev, E1000_TDT, dev->tx_tail);

    ndev->tx_packets++;
    ndev->tx_bytes += len;
    spin_unlock_irqrestore(&dev->tx_lock, txflags);

#if E1000E_DBG
    // Diagnostic: after each of the first 8 transmits, snapshot the MAC
    // RX stat counters.  Most of these registers are read-to-clear, so
    // each line shows what arrived since the previous dump — which for
    // the first call is everything since RCTL.EN was set, and for
    // subsequent calls is everything between back-to-back TX events.
    // This catches DHCP OFFER replies (or their absence) that arrive
    // 10-100 ms after our DISCOVER, long after the init path has
    // returned.
    {
        static uint32_t tx_dump_count = 0;
        if (tx_dump_count < 8) {
            tx_dump_count++;
            uint32_t s    = e1000e_read(dev, E1000_STATUS);
            uint32_t rdh  = e1000e_read(dev, E1000_RDH);
            uint32_t rdt  = e1000e_read(dev, E1000_RDT);
            uint32_t gprc = e1000e_read(dev, E1000_GPRC);
            uint32_t tpr  = e1000e_read(dev, E1000_TPR);
            uint32_t gptc = e1000e_read(dev, E1000_GPTC);
            uint32_t tpt  = e1000e_read(dev, E1000_TPT);
            uint32_t crc  = e1000e_read(dev, E1000_CRCERRS);
            uint32_t rxe  = e1000e_read(dev, E1000_RXERRC);
            uint32_t mpc  = e1000e_read(dev, E1000_MPC);
            uint32_t rnbc = e1000e_read(dev, E1000_RNBC);
            uint32_t tdh  = e1000e_read(dev, E1000_TDH);
            uint32_t tdt  = e1000e_read(dev, E1000_TDT);
            uint32_t tctl = e1000e_read(dev, E1000_TCTL);
            uint32_t txdctl = e1000e_read(dev, E1000_TXDCTL);
            uint32_t tipg = e1000e_read(dev, E1000_TIPG);
            uint32_t tidv = e1000e_read(dev, E1000_TIDV);
            uint32_t tadv = e1000e_read(dev, E1000_TADV);
            uint32_t tdbal = e1000e_read(dev, E1000_TDBAL);
            uint32_t tdbah = e1000e_read(dev, E1000_TDBAH);
            uint32_t tdlen = e1000e_read(dev, E1000_TDLEN);
            uint32_t ctrl = e1000e_read(dev, E1000_CTRL);
            const char *spd[] = { "10", "100", "1000", "rsv" };
            e1000e_dbg("E1000E: tx#%u STATUS=%08x CTRL=%08x speed=%s LU=%u GIO_MASTER=%u "
                    "RDH=%u RDT=%u TDH=%u TDT=%u "
                    "GPRC=%u TPR=%u GPTC=%u TPT=%u "
                    "CRCERRS=%u RXERRC=%u MPC=%u RNBC=%u\n",
                    tx_dump_count, s, ctrl, spd[(s >> 6) & 3],
                    (s & E1000_STATUS_LU) ? 1 : 0,
                    (s >> 19) & 1,
                    rdh, rdt, tdh, tdt,
                    gprc, tpr, gptc, tpt, crc, rxe, mpc, rnbc);
                e1000e_dbg("E1000E: tx#%u TCTL=%08x TXDCTL=%08x TIPG=%08x TIDV=%08x TADV=%08x TDBA=%08x:%08x TDLEN=%u\n",
                    tx_dump_count, tctl, txdctl, tipg, tidv, tadv,
                    tdbah, tdbal, tdlen);
            // Dump the descriptor we just wrote so we can confirm the
            // MAC sees a valid one (DEXT=0, EOP/IFCS/RS in cmd, status=0).
            volatile uint64_t* d = (volatile uint64_t*)&dev->tx_descs[tail];
            e1000e_dbg("E1000E: tx#%u desc[%u] @%016llx: %016llx %016llx (cmd=%02x sta=%02x len=%u)\n",
                    tx_dump_count, tail,
                    (unsigned long long)(dev->tx_descs_phys + tail * sizeof(e1000_tx_desc_legacy_t)),
                    (unsigned long long)d[0], (unsigned long long)d[1],
                    desc->cmd, desc->status, desc->length);

            // Poll for up to ~50 ms.  We sample TDFH separately from
            // TDH: TDFH advancing past TDFHS means the on-chip TX FIFO
            // popped the descriptor and queued the packet bytes for
            // the wire.  TDH advancing means the writeback completed.
            // If TDFH never advances either, the TX scheduler is gated
            // BEFORE the FIFO — a flow-control / arbiter / ME-ownership
            // problem, not a writeback / completion problem.
            uint8_t  sta_after = 0;
            uint32_t tdh_after = tdh;
            uint32_t gptc_after = 0, tpt_after = 0;
            uint32_t tdfh_after = 0, tdft_after = 0;
            uint32_t tdfhs_after = 0, tdfts_after = 0;
            uint32_t tdfh_first_change_us = 0;
            uint32_t tdh_first_change_us  = 0;
            uint32_t tdfh_initial = e1000e_read(dev, E1000_TDFH);
            for (int i = 0; i < 50; i++) {
                e1000e_delay_us(1000);
                sta_after  = desc->status;
                tdh_after  = e1000e_read(dev, E1000_TDH);
                uint32_t tdfh_now = e1000e_read(dev, E1000_TDFH);
                if (tdfh_first_change_us == 0 && tdfh_now != tdfh_initial)
                    tdfh_first_change_us = (uint32_t)((i + 1) * 1000);
                if (tdh_first_change_us == 0 && tdh_after != tdh)
                    tdh_first_change_us  = (uint32_t)((i + 1) * 1000);
                if (sta_after != 0 || tdh_after != tdh) break;
            }
            gptc_after = e1000e_read(dev, E1000_GPTC);
            tpt_after  = e1000e_read(dev, E1000_TPT);
            tdfh_after = e1000e_read(dev, E1000_TDFH);
            tdft_after = e1000e_read(dev, E1000_TDFT);
            tdfhs_after = e1000e_read(dev, E1000_TDFHS);
            tdfts_after = e1000e_read(dev, E1000_TDFTS);
            e1000e_dbg("E1000E: tx#%u after 50ms: desc.sta=%02x TDH=%u GPTC=%u TPT=%u TDFH=%08x TDFT=%08x TDFHS=%08x TDFTS=%08x\n",
                tx_dump_count, sta_after, tdh_after, gptc_after, tpt_after,
                tdfh_after, tdft_after, tdfhs_after, tdfts_after);
            e1000e_dbg("E1000E: tx#%u motion: TDFH_first_change_us=%u TDH_first_change_us=%u (0=never)\n",
                tx_dump_count, tdfh_first_change_us, tdh_first_change_us);

            // Gating-state dump: distinguishes flow-control pause,
            // arbiter mis-config, and ME-ownership gating.  All
            // read-only, no behaviour change.
            {
                uint32_t st       = e1000e_read(dev, E1000_STATUS);
                uint32_t tctl_rb  = e1000e_read(dev, E1000_TCTL);
                uint32_t txdctl_rb= e1000e_read(dev, E1000_TXDCTL);
                uint32_t tarc0_rb = e1000e_read(dev, E1000_TARC0);
                uint32_t tarc1_rb = e1000e_read(dev, E1000_TARC1);
                uint32_t fcrtl    = e1000e_read(dev, E1000_FCRTL);
                uint32_t fcrth    = e1000e_read(dev, E1000_FCRTH);
                uint32_t fcttv    = e1000e_read(dev, E1000_FCTTV);
                uint32_t fcal     = e1000e_read(dev, E1000_FCAL);
                uint32_t fcah     = e1000e_read(dev, E1000_FCAH);
                uint32_t fct      = e1000e_read(dev, E1000_FCT);
                uint32_t rfctl    = e1000e_read(dev, E1000_RFCTL);
                uint32_t manc     = e1000e_read(dev, E1000_MANC);
                uint32_t fwsm     = e1000e_read(dev, E1000_FWSM);
                uint32_t h2me     = e1000e_read(dev, E1000_H2ME);
                uint32_t ctrl_ext_rb = e1000e_read(dev, E1000_CTRL_EXT);
                e1000e_dbg("E1000E: tx#%u gate: STATUS=%08x TXOFF=%u TCTL=%08x(EN=%u) TXDCTL=%08x(EN=%u GRAN=%u) TARC0=%08x TARC1=%08x\n",
                    tx_dump_count, st, (st >> 4) & 1, tctl_rb,
                    (tctl_rb >> 1) & 1, txdctl_rb,
                    (txdctl_rb >> 25) & 1, (txdctl_rb >> 24) & 1,
                    tarc0_rb, tarc1_rb);
                e1000e_dbg("E1000E: tx#%u flow: FCRTL=%08x FCRTH=%08x FCTTV=%08x FCAL=%08x FCAH=%08x FCT=%08x RFCTL=%08x\n",
                    tx_dump_count, fcrtl, fcrth, fcttv, fcal, fcah, fct, rfctl);
                e1000e_dbg("E1000E: tx#%u me  : MANC=%08x FWSM=%08x H2ME=%08x CTRL_EXT=%08x\n",
                    tx_dump_count, manc, fwsm, h2me, ctrl_ext_rb);
            }

            if (sta_after == 0 && tdh_after == tdh && gptc_after == 0 && tpt_after == 0) {
                e1000e_dump_pci_dma_state(dev, tx_dump_count);
            }

            // NOTE: do NOT read ICR here.  ICR is read-to-clear; if we
            // peek it from non-IRQ context we destroy pending RX/LSC
            // causes that the IRQ handler hasn't had a chance to see
            // yet, and the RX ring is never drained.  IMS is safe (RW).
            uint32_t ims  = e1000e_read(dev, E1000_IMS);
            e1000e_dbg("E1000E: tx#%u IMS=%08x (ICR not peeked; would be destructive)\n",
                    tx_dump_count, ims);
        }
    }
#endif // E1000E_DBG

    return 0;
}

// ============================================================================
// Link Status (net_device_t callback)
// ============================================================================
//
// Cable hot-plug support.  When the cable is unplugged, the PHY drops
// link and the MAC's LSC interrupt fires; STATUS.LU goes to 0.  When
// the cable is replugged, the PHY runs a fresh autoneg cycle and on
// PCH-LAN parts the post-autoneg state may re-apply LPLU / GBE_DIS
// (the OEM NVM defaults on Lenovo ThinkPad NVMs are LPLU=1, GBE_DIS=1)
// — even though we cleared those bits at init time.  LPLU=1 with link
// UP throttles the MII data path: the MAC fetches the TX descriptor
// but the buffer payload is never pulled across the MAC<->PHY interface,
// so TX appears to "succeed" while no frame ever leaves the wire.
//
// We must therefore re-clear the LPLU / GBE_DIS bits on every observed
// link-down -> link-up edge.  This is too heavy to do from the LSC IRQ
// handler (the OEM-bits write needs the SW_FLAG semaphore, paged PHY
// access, and ms-scale delays), so the IRQ only sets a pending flag
// and the real edge handler runs in process context the next time
// either the link_status callback or the send path is invoked.
static void e1000e_link_state_poll(e1000e_dev_t* dev) {
    uint32_t status = e1000e_read(dev, E1000_STATUS);
    int now_up = (status & E1000_STATUS_LU) ? 1 : 0;
    int was_up = dev->link_up;

    int pending = dev->link_change_pending;
    dev->link_change_pending = 0;

    // Detect edge two ways: by direct STATUS.LU comparison (covers the
    // case where LSC is masked / hasn't fired yet) AND via the IRQ-set
    // pending flag (covers the case where the IRQ already updated
    // dev->link_up before we got here).
    int edge = (now_up != was_up) || pending;
    if (!edge) return;

    if (now_up != was_up) {
        dev->link_up = now_up;
        kprintf("E1000E: Link %s\n", now_up ? "UP" : "DOWN");
        if (!now_up) {
            // Link went down: invalidate any DHCP lease.  The network
            // we get back may be different (cable moved to another
            // switch, hypervisor toggled bridged<->NAT, ...) so the
            // cached IP/gateway/DHCP-server are no longer trustworthy.
            // Clearing them now makes the next dhclient invocation
            // do a full DISCOVER instead of a unicast renewal aimed
            // at the previous network's server.
            dhcp_invalidate(&dev->net_dev);
        }
    }

    if (now_up) {
        // Cable was just (re-)plugged and the PHY just finished a
        // fresh autoneg.  Re-clear the LPLU / GBE_DIS gates so the
        // MII data path is not throttled.  Do NOT request an autoneg
        // restart — link is already up and a restart would drop it.
        e1000e_clear_mac_phy_ctrl_lplu(dev);
        (void)e1000e_oem_bits_clear_lplu_ex(dev, 0);
    }
}

static int e1000e_link_status(net_device_t* ndev) {
    e1000e_dev_t* dev = (e1000e_dev_t*)ndev->driver_data;
    // Always poll: the LSC IRQ may not have fired yet (or may have
    // been collapsed into another cause), and STATUS.LU is the only
    // ground truth.  Cheap — single MMIO read in the no-change case.
    e1000e_link_state_poll(dev);
    return dev->link_up;
}

// Quiesce the NIC ahead of an ACPI S5 transition.  PCH-LAN integrated
// I218 / I219 controllers will hold PME# asserted (and the platform
// will refuse to fully power down — screen black, fans off, but rail
// still hot) if any of the following are still live when SLP_EN is
// written:
//   * IMS — any unmasked interrupt cause
//   * RCTL.EN / TCTL.EN — receiver / transmitter still enabled
//   * WUC.APME / WUFC — Wake-on-LAN advertised to the platform
//   * PCI Command.BME — bus-master DMA still active
//   * CTRL_EXT.DRV_LOAD — OS-ownership flag asserted to the ME
// Per the I218/I219 datasheet, all of these must be cleared before the
// chipset will release the platform to G2/S5.
static void e1000e_shutdown(net_device_t* ndev) {
    e1000e_dev_t* dev = (e1000e_dev_t*)ndev->driver_data;
    if (!dev || !dev->mmio_base) return;

    // Mask every interrupt cause and ack any pending ones.
    e1000e_write(dev, E1000_IMC, 0xFFFFFFFFu);
    (void)e1000e_read(dev, E1000_ICR);

    // Disable the receiver and transmitter so no further DMA descriptors
    // are fetched after this point.
    uint32_t rctl = e1000e_read(dev, E1000_RCTL);
    e1000e_write(dev, E1000_RCTL, rctl & ~E1000_RCTL_EN);
    uint32_t tctl = e1000e_read(dev, E1000_TCTL);
    e1000e_write(dev, E1000_TCTL, tctl & ~E1000_TCTL_EN);

    // Clear Wake-on-LAN: WUC.APME (offset 0x05800, bit 0) advertises
    // PME# capability to the platform; WUFC (0x05808) selects which
    // packet patterns assert the wake.  Zeroing both is mandatory for
    // a clean S5 on integrated PCH-LAN.
    e1000e_write(dev, 0x05808u /* WUFC */, 0);
    e1000e_write(dev, 0x05800u /* WUC  */, 0);

    // Tell the management engine we are no longer the owning driver.
    // Per the PCH-LAN spec, with DRV_LOAD deasserted the firmware will
    // run its own platform shutdown path and not wait for further OS
    // intervention before allowing the S5 transition.
    if (dev->pci_dev && e1000e_is_pch_lan(dev->pci_dev->device_id)) {
        // Drain the MAC master before D3 so no DMA/TLP remains outstanding
        // when ownership is returned to the firmware side.
        uint32_t ctrl = e1000e_read(dev, E1000_CTRL);
        e1000e_write(dev, E1000_CTRL, ctrl | (1u << 2));
        for (int waited_us = 0; waited_us < 100000; waited_us += 100) {
            if (!(e1000e_read(dev, E1000_STATUS) & (1u << 19))) {
                break;
            }
            e1000e_delay_us(100);
        }

        // Reverse the PCH-LAN power-gating exit that init applies in D0.
        // Leaving these clocks and MAC-side gates forced on into the S5
        // handoff can keep the integrated LAN function warm after the rest
        // of the platform has already started to power down.
        uint32_t pbeccsts = e1000e_read(dev, E1000_PBECCSTS);
        e1000e_write(dev, E1000_PBECCSTS,
                     pbeccsts & ~E1000_PBECCSTS_ECC_ENABLE);

        ctrl = e1000e_read(dev, E1000_CTRL);
        e1000e_write(dev, E1000_CTRL, ctrl & ~E1000_CTRL_MEHE);

        uint32_t dpgfr = e1000e_read(dev, E1000_DPGFR);
        e1000e_write(dev, E1000_DPGFR, dpgfr | (1u << 2));

        uint32_t pch_ctrl_ext = e1000e_read(dev, E1000_CTRL_EXT);
        pch_ctrl_ext |= E1000_CTRL_EXT_DPG_EN |
                        E1000_CTRL_EXT_DMA_DYN_CLK_EN;
        e1000e_write(dev, E1000_CTRL_EXT, pch_ctrl_ext);

        uint32_t fextnvm7 = e1000e_read(dev, E1000_FEXTNVM7);
        fextnvm7 |= E1000_FEXTNVM7_DISABLE_TSYNC_CLK;
        fextnvm7 &= ~(E1000_FEXTNVM7_ENABLE_TSYNC_INTR |
                      E1000_FEXTNVM7_SIDE_CLK_UNGATE);
        e1000e_write(dev, E1000_FEXTNVM7, fextnvm7);

        // Undo the bring-up ownership handshake so the firmware/ME can
        // park the integrated PHY in its shutdown state instead of keeping
        // the link side powered on our behalf.
        uint32_t h2me = e1000e_read(dev, E1000_H2ME);
        h2me &= ~E1000_H2ME_ENFORCE_SETTINGS;
        h2me |= E1000_H2ME_ULP;
        e1000e_write(dev, E1000_H2ME, h2me);

        if (e1000e_acquire_swflag(dev) == 0) {
            uint16_t ulp_cfg = 0;
            if (e1000e_phy_read_paged_locked(dev, E1000_I218_ULP_CONFIG1,
                                             &ulp_cfg) == 0) {
                ulp_cfg &= ~(E1000_I218_ULP_CONFIG1_WOL_HOST |
                             E1000_I218_ULP_CONFIG1_INBAND_EXIT);
                ulp_cfg |= E1000_I218_ULP_CONFIG1_RESET_TO_SMBUS |
                           E1000_I218_ULP_CONFIG1_EN_ULP_LANPHYPC;
                (void)e1000e_phy_write_paged_locked(dev, E1000_I218_ULP_CONFIG1,
                                                    ulp_cfg);
                (void)e1000e_phy_write_paged_locked(dev, E1000_I218_ULP_CONFIG1,
                    (uint16_t)(ulp_cfg | E1000_I218_ULP_CONFIG1_START));
            }
            e1000e_release_swflag(dev);
        }
    }

    uint32_t ctrl_ext = e1000e_read(dev, E1000_CTRL_EXT);
    ctrl_ext &= ~E1000_CTRL_EXT_DRV_LOAD;
    e1000e_write(dev, E1000_CTRL_EXT, ctrl_ext);

    // Drop PCI bus-mastering so any remaining in-flight TLPs are
    // discarded by the root complex rather than landing in memory
    // after the OS has handed off to the firmware.
    if (dev->pci_dev) {
        const pci_device_t* pdev = dev->pci_dev;

        uint8_t pm_cap = pci_find_capability(pdev, 0x01);
        if (pm_cap) {
            uint32_t pmcsr_dw = pci_cfg_read32(pdev->bus, pdev->device, pdev->function,
                                               (uint8_t)(pm_cap + 4));
            uint16_t pmcsr = (uint16_t)(pmcsr_dw & 0xFFFFu);
            pmcsr &= ~((uint16_t)0x0100u);  // PME Enable
            pmcsr |= 0x8000u;               // clear PME Status if latched
            pmcsr = (uint16_t)((pmcsr & ~0x0003u) | 0x0003u); // D3hot
            pmcsr_dw = (pmcsr_dw & 0xFFFF0000u) | pmcsr;
            pci_cfg_write32(pdev->bus, pdev->device, pdev->function,
                            (uint8_t)(pm_cap + 4), pmcsr_dw);
        }

        uint32_t cmd = pci_cfg_read32(pdev->bus, pdev->device, pdev->function, 0x04);
        cmd &= ~0x0006u; // clear Bus Master Enable and Memory Space Enable
        pci_cfg_write32(pdev->bus, pdev->device, pdev->function, 0x04, cmd);
    }
}

// ============================================================================
// IRQ Handler
// ============================================================================
void e1000e_irq_handler(void) {
    if (!g_e1000e_initialized) {
        lapic_eoi();
        return;
    }

    {
        extern void entropy_add_interrupt_timing(uint64_t extra);
        uint32_t lo, hi;
        __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
        entropy_add_interrupt_timing(((uint64_t)hi << 32) | lo);
    }

    e1000e_dev_t* dev = &g_e1000e;
    uint32_t icr = e1000e_read(dev, E1000_ICR);

#if E1000E_DBG
    // Count every IRQ entry so we can tell from userland (via the tx
    // diagnostic dump) whether the MAC is asserting interrupts at all.
    static uint32_t irq_total = 0;
    static uint32_t irq_with_cause = 0;
    irq_total++;

    if (icr != 0) {
        irq_with_cause++;
        if (irq_with_cause <= 8) {
            e1000e_dbg("E1000E: IRQ #%u ICR=%08x (total=%u)\n",
                    irq_with_cause, icr, irq_total);
        }
    }
#endif
    // NOTE: do not early-return on icr==0.  We still need to scan the
    // RX ring (DD bit) below in case the MAC posted MSI for a batch
    // whose descriptor became visible after our ICR read cleared it.

    if (icr & E1000_ICR_LSC) {
        // Throttle LSC: on some PCH-LAN parts the link can bounce
        // continuously after init and flood us with LSC interrupts,
        // starving the CPU.  After a handful of events in quick
        // succession, mask LSC permanently — link state is also
        // observable via the link_status callback so userland still
        // sees the truth.
        static uint32_t lsc_count = 0;
        if (++lsc_count > 16) {
            uint32_t ims = e1000e_read(dev, E1000_IMS);
            (void)ims;
            e1000e_write(dev, E1000_IMC, E1000_ICR_LSC);
            e1000e_dbg("E1000E: LSC interrupt storm — masking LSC\n");
        }
        uint32_t status = e1000e_read(dev, E1000_STATUS);
        int prev_up = dev->link_up;
        dev->link_up = (status & E1000_STATUS_LU) ? 1 : 0;
        if (prev_up != dev->link_up) {
            // Note for the process-context edge handler: on down->up
            // we must re-clear LPLU/GBE_DIS or TX will silently drop.
            dev->link_change_pending = 1;
        }
        if (lsc_count <= 4) {
            kprintf("E1000E: Link %s\n", dev->link_up ? "UP" : "DOWN");
        }
    }

    // Always drain the RX ring on every IRQ regardless of which ICR
    // bits survived to the handler.  ICR causes can be lost to read-
    // to-clear races (any MMIO read of ICR clears it) and to MSI
    // moderation collapsing several causes into a single edge.  The
    // DD bit in the descriptor itself is the only ground truth for
    // "a packet has arrived".  This matches the standard "poll-on-wake"
    // RX-completion handling pattern.
    {
        uint16_t tail = (dev->rx_tail + 1) % E1000E_NUM_RX_DESC;
        unsigned drained = 0;

        while (1) {
            volatile e1000_rx_desc_legacy_t* desc = &dev->rx_descs[tail];
            if (!(desc->status & E1000_RXD_STAT_DD))
                break;

            uint16_t len = desc->length;
            if ((desc->status & E1000_RXD_STAT_EOP) && len > 0 && len <= E1000E_RX_BUF_SIZE) {
                net_rx_packet(&dev->net_dev, dev->rx_bufs[tail], len);
            } else {
                dev->net_dev.rx_errors++;
            }

            desc->buffer_addr = dev->rx_bufs_phys[tail];
            desc->status = 0;

            dev->rx_tail = tail;
            e1000e_write(dev, E1000_RDT, tail);

            tail = (tail + 1) % E1000E_NUM_RX_DESC;
            drained++;
        }

#if E1000E_DBG
        static uint32_t rx_log = 0;
        if (drained && rx_log < 8) {
            rx_log++;
            e1000e_dbg("E1000E: RX drained %u pkt(s) (icr=%08x rdt=%u)\n",
                    drained, icr, dev->rx_tail);
        }
#else
        (void)drained; (void)icr;
#endif
    }

    // TXDW is observed via DD bit in send(); nothing else to do here.

    lapic_eoi();
}

// ============================================================================
// Map MMIO BAR
// ============================================================================
static int e1000e_map_bar(e1000e_dev_t* dev) {
    uint32_t bar0 = dev->pci_dev->bar[0];

    if (bar0 & 1) {
        kprintf("E1000E: BAR0 is I/O port, not MMIO\n");
        return -1;
    }

    uint64_t phys = bar0 & 0xFFFFFFF0ULL;
    if ((bar0 & 0x06) == 0x04) {
        phys |= ((uint64_t)dev->pci_dev->bar[1]) << 32;
    }

    dev->mmio_phys = phys;
    dev->mmio_size = 128 * 1024;

    uint32_t num_pages = (dev->mmio_size + PAGE_SIZE - 1) / PAGE_SIZE;

    // Map (or, if the BAR happens to lie inside the kernel direct map,
    // *re*map in place) the device's register window with strict
    // uncacheable attributes.
    //
    // The previous "if (!mm_is_page_mapped) mm_map_page(UC)" pattern
    // was the actual reason this driver worked under QEMU but stalled
    // on every real PCH-LAN system.  On QEMU the I219's BAR is placed
    // above the bootloader's identity/direct map (typically around
    // 0xFEB00000), so the conditional triggered and we got a UC
    // mapping.  On a real Lenovo P50 (and every other modern Intel
    // laptop) the integrated I219-LM's BAR sits below 4 GiB inside
    // the existing kernel direct map, which is mapped *write-back
    // cached*.  MMIO writes through a WB alias get coalesced and
    // posted out of order, so register state visible to the device
    // is not what the driver wrote — TDT may increment but TXDCTL,
    // TCTL or the TX doorbell ordering is silently broken, the
    // descriptor-fetch engine never starts, and the symptom is
    // exactly what we observe: link UP, TDT advancing, TDH stuck at
    // 0, GPTC=0, no PCI errors.  mm_map_device_mmio() handles both
    // cases correctly: it rewrites direct-map PTEs in place to UC
    // when the range is in the direct map, and falls back to
    // mm_map_mmio() otherwise.
    uint64_t virt_base = mm_map_device_mmio(phys, num_pages);
    if (!virt_base) {
        kprintf("E1000E: Failed to map BAR0 MMIO (phys=%llx, %u pages)\n",
                (unsigned long long)phys, num_pages);
        return -1;
    }

    dev->mmio_base = (volatile uint8_t*)virt_base;
    return 0;
}

// ============================================================================
// Driver Initialization
// ============================================================================
void e1000e_init(void) {
    int count;
    const pci_device_t* devs = pci_get_devices(&count);

    for (int i = 0; i < count; i++) {
        if (devs[i].vendor_id != E1000E_VENDOR_ID) continue;

        uint16_t did = devs[i].device_id;
        const e1000e_id_t* match = e1000e_lookup(did);
        if (!match) continue;
        const char* name = match->name;

        kprintf("E1000E: Found %s (PCI %02x:%02x.%x)\n",
                name, devs[i].bus, devs[i].device, devs[i].function);

        e1000e_dev_t* dev = &g_e1000e;
        dev->pci_dev = &devs[i];
        dev->device_id = did;

        pci_enable_busmaster_mem(dev->pci_dev);

        if (e1000e_map_bar(dev) < 0) {
            kprintf("E1000E: Failed to map BAR0\n");
            return;
        }

        e1000e_reset(dev);

        // I217/I218/I219 (PCH-LAN) bring-up workarounds — required on
        // every Intel-branded business laptop/desktop with an integrated
        // LOM since Skylake (incl. ThinkPad P50's I219-LM at 0x156F).
        if (e1000e_is_pch_lan(did)) {
            e1000e_pch_lan_init(dev);
        }

        e1000e_read_mac(dev);
        kprintf("E1000E: MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
                dev->mac_addr[0], dev->mac_addr[1], dev->mac_addr[2],
                dev->mac_addr[3], dev->mac_addr[4], dev->mac_addr[5]);

        // Set link up.  Per 82574 datasheet section 4.6.3.2 (and
        // confirmed for I217/I218/I219), the recommended config when
        // using the integrated PHY's auto-negotiation is:
        //   CTRL.FRCDPLX = 0  (let PHY tell MAC the duplex)
        //   CTRL.FRCSPD  = 0  (let PHY tell MAC the speed)
        //   CTRL.ASDE    = 0  (no separate auto-speed-detect needed —
        //                      datasheet explicitly says ASDE "must be
        //                      set to 0b in the 82574")
        //   CTRL.SLU     = 1  (enable MAC↔PHY communication)
        // The MAC then samples SPEED/FDX from the PHY's internal
        // SPD_IND/FDX signals on the rising edge of LINK and reflects
        // them in STATUS.  If we leave ASDE=1, the MAC additionally
        // tries to time RX_CLK and may latch the wrong frequency
        // when the PHY's link signal toggles, sending the MAC↔PHY
        // KMRN bus out of lock and dropping every frame.
        // Program flow-control registers to canonical IEEE 802.3x values
        // BEFORE asserting SLU.  The PCH-LAN setup_link path requires FCTTV
        // / FCRTL / FCRTH / FCAL / FCAH / FCT programming before transmitter
        // enable.
        // Chip default leaves FCT=0x80800000 / FCAH=0xC0000008 etc., which
        // can wedge the MAC's flow-control state machine.  We do not need
        // RX flow control (FCRTL/FCRTH=0 disables it), but the FCAL/FCAH/FCT
        // multicast-pause-frame-recogniser must hold the spec values, and
        // CTRL.RFCE/TFCE must be cleared so the MAC does not gate TX while
        // waiting for a XON it will never see.
        e1000e_write(dev, E1000_FCAL,  0x00C28001u); // 802.3x mc address lo
        e1000e_write(dev, E1000_FCAH,  0x00000100u); // 802.3x mc address hi
        e1000e_write(dev, E1000_FCT,   0x00008808u); // 802.3x ethertype
        e1000e_write(dev, E1000_FCTTV, 0x0000FFFFu); // pause-time, full
        e1000e_write(dev, E1000_FCRTL, 0);           // RX flow ctrl off
        e1000e_write(dev, E1000_FCRTH, 0);

        uint32_t ctrl = e1000e_read(dev, E1000_CTRL);
        ctrl &= ~(E1000_CTRL_FRCSPD | (1u << 12) /* FRCDPLX */
                | E1000_CTRL_ASDE
                | E1000_CTRL_PHY_RST
                | (1u << 7)    // ILOS — spec: must be 0 in normal mode
                | (1u << 20)   // ADVD3WUC — D3 wake; not in use, must be 0
                | (1u << 27)   // RFCE — RX flow control off
                | (1u << 28)   // TFCE — TX flow control off
                | (1u << 30)); // VME — VLAN tagging off
        ctrl |=  E1000_CTRL_SLU;
        e1000e_write(dev, E1000_CTRL, ctrl);
        e1000e_dbg("E1000E: CTRL=%08x (SLU set, FRCSPD/FRCDPLX/ASDE/RFCE/TFCE cleared)\n",
                e1000e_read(dev, E1000_CTRL));

        // Disable Kumeran K1 power-save on PCH-LAN parts.  Without this
        // the MAC↔PHY interconnect drops every received frame on
        // I219-LM at 100 Mbps full-duplex (TPR stays at 0).
        if (e1000e_is_pch_lan(did)) {
            e1000e_disable_k1(dev);

            // Restart copper auto-negotiation on the PHY.  The PHY_RST we
            // asserted in pch_lan_init left the integrated PHY in a state
            // where its 100BASE-T transmitter is up but no FLPs are on the
            // wire and BMSR.AN_COMPLETE = 0; the MAC's STATUS.LU bit then
            // reflects only the cached SMBus link and STATUS.SPEED reads as
            // 10 Mb/s.  Without a real copper link the MAC PCS never grants
            // TX, which is exactly the "TDH=0, GPTC=0, descriptor in FIFO"
            // symptom we observe.
            (void)e1000e_phy_restart_autoneg(dev);

            // Wait for STATUS.LU to assert AND for the speed bits to latch
            // before reading STATUS.SPEED for the KMRN configuration.
            // Reading STATUS immediately after asserting CTRL.SLU returns
            // the previously-latched speed (typically 10 Mb/s post-reset),
            // which then mis-times the MAC<->PHY KMRN bus and silently
            // drops every Tx fetch.  The check_for_link path always gates
            // speed-dependent KMRN reprogramming on a confirmed link
            // resolution.
            //
            // Poll up to ~2.5 s for link-up; that's the worst-case auto-neg
            // time the 802.3 spec allows for 10/100/1000BASE-T.  Then add a
            // 50 ms settle so STATUS.SPEED definitely reflects the resolved
            // speed before we sample it.
            {
                uint32_t status = 0;
                for (int ms = 0; ms < 2500; ms++) {
                    status = e1000e_read(dev, E1000_STATUS);
                    if (status & E1000_STATUS_LU) break;
                    e1000e_delay_us(1000);
                }
                e1000e_delay_us(50000);
                uint32_t st_spd = e1000e_read(dev, E1000_STATUS);
                uint32_t spd = (st_spd >> 6) & 0x3;
                e1000e_dbg("E1000E: link-resolve STATUS=%08x LU=%u SPEED=%u (%s)\n",
                        st_spd,
                        (st_spd & E1000_STATUS_LU) ? 1 : 0,
                        spd,
                        spd == 0 ? "10Mb/s" :
                        spd == 1 ? "100Mb/s" :
                        spd == 2 ? "1000Mb/s" : "reserved");
                if (spd == 0x2) {
                    e1000e_configure_kmrn_for_1000(dev);
                } else {
                    e1000e_configure_kmrn_for_10_100(dev);
                }

                // K1 workaround for PCH-LPT/SPT/CNP+ at 10/100 Mb FD:
                // Run AFTER link is resolved.  Re-programs K1 + the
                // PCIe-PLL clock-request gate based on the resolved
                // speed/duplex.  Without this, on SPT-H @ 100 Mb FD
                // the MAC's TX scheduler clock domain stays mistuned:
                // descriptor enters TX FIFO (TDFT advances) but TDH
                // never advances and no bytes hit the wire.  This is
                // exactly the dmesg symptom on the P50 — TDFH=0xd00
                // TDFT=0xd02 (one descriptor cached on-chip), TDH=0,
                // GPTC=0, TPT=0.
                //
                // Logic per the I218/I219 datasheet:
                //   if link && full-duplex:
                //     read KMRN K1_CONFIG
                //     if speed == 1000Mb: set K1_ENABLE
                //     else (10/100):      clear K1_ENABLE
                //                          udelay(10..20)
                //                          FEXTNVM6 |= REQ_PLL_CLK
                //   else: don't touch
                if (e1000e_is_pch_spt(did) &&
                    (st_spd & E1000_STATUS_LU) &&
                    (st_spd & 0x1) /* FD */) {
                    uint16_t k1cfg = 0;
                    e1000e_kmrn_read(dev, E1000_KMRN_K1_CONFIG_OFFSET, &k1cfg);
#if E1000E_DBG
                    uint16_t k1cfg_before = k1cfg;
#endif
                    if (spd == 0x2) {
                        k1cfg |=  E1000_KMRN_K1_ENABLE;
                        e1000e_kmrn_write(dev, E1000_KMRN_K1_CONFIG_OFFSET, k1cfg);
                    } else {
                        k1cfg &= ~E1000_KMRN_K1_ENABLE;
                        e1000e_kmrn_write(dev, E1000_KMRN_K1_CONFIG_OFFSET, k1cfg);
                        e1000e_delay_us(20);
                        uint32_t fx6_before = e1000e_read(dev, E1000_FEXTNVM6);
                        uint32_t fx6 = fx6_before | E1000_FEXTNVM6_REQ_PLL_CLK;
                        e1000e_write(dev, E1000_FEXTNVM6, fx6);
                        e1000e_dbg("E1000E: k1_workaround_lpt_lp 100Mb FD: "
                                "KMRN[0x07] %04x->%04x FEXTNVM6 %08x->%08x\n",
                                k1cfg_before, k1cfg,
                                fx6_before,
                                e1000e_read(dev, E1000_FEXTNVM6));
                    }
                }
            }
        }

        // Clear STATUS.PHYRA (PHY Reset Asserted).  Per the 82574
        // datasheet (section 10.2.2.2) this bit is RW and is cleared
        // by writing 0b to it (NOT W1C as on most other status bits).
        // The K1-disable sequence above re-asserts PHYRA so we must
        // clear it again here, after the SLU/CTRL writes have settled.
        {
            uint32_t st = e1000e_read(dev, E1000_STATUS);
            if (st & E1000_STATUS_PHYRA) {
                e1000e_write(dev, E1000_STATUS, st & ~E1000_STATUS_PHYRA);
                // Read back to confirm it actually cleared.
                st = e1000e_read(dev, E1000_STATUS);
                if (st & E1000_STATUS_PHYRA) {
                    e1000e_dbg("E1000E: WARNING PHYRA still set after clear "
                            "(STATUS=%08x) — PHY may still be held in reset\n",
                            st);
                }
            }
        }

        // Clear multicast table
        for (int m = 0; m < 128; m++)
            e1000e_write(dev, E1000_MTA + m * 4, 0);

        uint32_t ral = (uint32_t)dev->mac_addr[0]
                     | ((uint32_t)dev->mac_addr[1] << 8)
                     | ((uint32_t)dev->mac_addr[2] << 16)
                     | ((uint32_t)dev->mac_addr[3] << 24);
        uint32_t rah = (uint32_t)dev->mac_addr[4]
                     | ((uint32_t)dev->mac_addr[5] << 8)
                     | (1u << 31);
        e1000e_write(dev, E1000_RAL, ral);
        e1000e_write(dev, E1000_RAH, rah);

        if (e1000e_init_rx(dev) < 0) return;
        if (e1000e_init_tx(dev) < 0) return;

        // ------------------------------------------------------------------
        // MSI first; legacy INTx with full ACPI _PRT lookup as fallback.
        // Same logic as the older e1000 driver — see comments there for
        // the rationale (notably: PCI interrupt_line is NOT the IOAPIC GSI
        // in APIC mode; the BIOS-assigned PIC IRQ value must NOT be
        // trusted on ICH9-class systems).
        // ------------------------------------------------------------------
        dev->msi_vector = E1000E_MSI_VECTOR;
        int msi_ret = pci_enable_msi(dev->pci_dev, dev->msi_vector);
        if (msi_ret < 0) {
            dev->msi_vector = 0;

            uint8_t irq = 0xFF;
            uint32_t gsi = 0;
            uint8_t pin = dev->pci_dev->interrupt_pin;
            if (pin >= 1 && pin <= 4) {
                uint8_t acpi_pin = pin - 1;
                uint8_t lookup_dev = dev->pci_dev->device;
                uint8_t lookup_pin = acpi_pin;

                if (dev->pci_dev->bus != 0) {
                    const pci_device_t *bridge = pci_find_bridge_for_bus(dev->pci_dev->bus);
                    if (bridge) {
                        lookup_pin = (acpi_pin + dev->pci_dev->device) % 4;
                        lookup_dev = bridge->device;
                    }
                }

                if (acpi_pci_lookup_irq("\\\\_SB_.PCI0",
                                        lookup_dev, lookup_pin,
                                        &gsi) == 0 && gsi <= 23) {
                    irq = (uint8_t)gsi;
                    kprintf("E1000E: ACPI _PRT resolved INT%c -> GSI %u\n",
                            'A' + acpi_pin, gsi);
                }
            }

            if (irq == 0xFF) {
                irq = dev->pci_dev->interrupt_line;
                if (irq != 0xFF && irq <= 23) {
                    kprintf("E1000E: ACPI _PRT lookup failed, falling back to "
                            "PCI interrupt_line = %d\n", irq);
                } else {
                    kprintf("E1000E: WARNING: no valid IRQ (line=%d, _PRT failed)\n",
                            dev->pci_dev->interrupt_line);
                    kprintf("E1000E: Interrupts will not work\n");
                }
            }

            kprintf("E1000E: Using legacy IRQ %d\n", irq);
            g_e1000e_legacy_irq = irq;

            // Some firmware leaves PCI Command
            // bit 10 (INTx Disable) set, preventing the device from ever
            // raising its interrupt pin.  Clear it for the legacy path.
            {
                uint32_t cmd = pci_cfg_read32(dev->pci_dev->bus,
                                              dev->pci_dev->device,
                                              dev->pci_dev->function, 0x04);
                if (cmd & PCI_CMD_INTX_DISABLE) {
                    cmd &= ~PCI_CMD_INTX_DISABLE;
                    pci_cfg_write32(dev->pci_dev->bus,
                                    dev->pci_dev->device,
                                    dev->pci_dev->function, 0x04, cmd);
                    kprintf("E1000E: cleared PCI Command INTx Disable bit\n");
                }
            }

            if (irq <= 23) {
                uint8_t vector = 32 + irq;
                ioapic_configure_legacy_irq(irq, vector,
                                            IOAPIC_POLARITY_LOW,
                                            IOAPIC_TRIGGER_LEVEL);
            }
        } else {
            kprintf("E1000E: MSI enabled (vector %d)\n", dev->msi_vector);
        }

        // Mark the driver initialised before unmasking IMS — the IRQ
        // dispatcher only routes to e1000e_irq_handler() when this flag
        // is set; otherwise the vector may collide with the I2C LPSS
        // dispatch range and lock up the system in a level-triggered
        // IRQ storm (same problem we hit with the e1000 driver).
        dev->net_dev.lock = (spinlock_t)SPINLOCK_INIT("e1000e");
        dev->tx_lock = (spinlock_t)SPINLOCK_INIT("e1000e_tx");
        dev->net_dev.rx_packets = 0;
        dev->net_dev.tx_packets = 0;
        dev->net_dev.rx_bytes = 0;
        dev->net_dev.tx_bytes = 0;
        dev->net_dev.rx_errors = 0;
        dev->net_dev.tx_errors = 0;
        dev->net_dev.rx_dropped = 0;
        g_e1000e_initialized = 1;

        // Wait up to 2 s for link-up.  IMS is still masked here so we
        // cannot get an LSC storm during the poll.
        uint32_t status = e1000e_read(dev, E1000_STATUS);
        if (!(status & E1000_STATUS_LU)) {
            uint32_t t0 = timer_pmtimer_read_raw();
            while (timer_pmtimer_delta_us(t0, timer_pmtimer_read_raw()) < 2000000) {
                e1000e_delay_us(50000);
                status = e1000e_read(dev, E1000_STATUS);
                if (status & E1000_STATUS_LU) break;
            }
        }
        dev->link_up = (status & E1000_STATUS_LU) ? 1 : 0;
        kprintf("E1000E: Link %s\n", dev->link_up ? "UP" : "DOWN");

#if E1000E_DBG
        // One-shot diagnostic dump — printed BEFORE we unmask interrupts
        // so it always appears in the log even if an IRQ storm follows.
        {
            uint32_t s    = e1000e_read(dev, E1000_STATUS);
            uint32_t cext = e1000e_read(dev, E1000_CTRL_EXT);
            uint32_t rctl = e1000e_read(dev, E1000_RCTL);
            uint32_t rdh  = e1000e_read(dev, E1000_RDH);
            uint32_t rdt  = e1000e_read(dev, E1000_RDT);
            e1000e_dbg("E1000E: STATUS=%08x CTRL_EXT=%08x RCTL=%08x RDH=%u RDT=%u\n",
                    s, cext, rctl, rdh, rdt);
        }
#endif

        // (Stats dump moved to e1000e_send() so we see counters at the
        // time DHCP DISCOVER actually goes on the wire, not 2s into
        // boot when nothing has been transmitted yet.)

        // Unmask RX/TX and LSC.  LSC is required for prompt cable
        // hot-plug detection.  PCH-LAN parts can bounce link rapidly
        // during init; the IRQ handler throttles LSC and disables it
        // after 16 events in quick succession to prevent CPU starvation.
        // The link_status / send paths poll STATUS.LU directly as a
        // fall-back so userland still sees the truth even if LSC ends
        // up disabled.
        uint32_t ims = E1000_ICR_RXT0 | E1000_ICR_TXDW |
                       E1000_ICR_RXDMT0 | E1000_ICR_RXO |
                       E1000_ICR_LSC;
        e1000e_write(dev, E1000_IMS, ims);

        // net_device_t setup
        dev->net_dev.name = "eth0";
        for (int m = 0; m < ETH_ALEN; m++)
            dev->net_dev.mac_addr[m] = dev->mac_addr[m];
        dev->net_dev.mtu = NET_MTU_DEFAULT;
        dev->net_dev.ip_addr = 0;
        dev->net_dev.netmask = 0;
        dev->net_dev.gateway = 0;
        dev->net_dev.dns_server = 0;
        dev->net_dev.send = e1000e_send;
        dev->net_dev.link_status = e1000e_link_status;
        dev->net_dev.shutdown = e1000e_shutdown;
        dev->net_dev.driver_data = dev;

        net_register(&dev->net_dev);

        kprintf("E1000E: Driver initialized successfully\n");
        return;  // Only handle first matching device
    }
}
