// LikeOS-64 VMware vmxnet3 paravirtualized NIC Driver
//
// Supports the VMware vmxnet3 paravirtual NIC (vendor 0x15AD, device
// 0x07B0) as exposed by VMware ESXi/Workstation/Fusion and emulated by
// QEMU as `-device vmxnet3`.  The driver implements UPT/Vmxnet3
// revision 1 — sufficient for basic packet I/O on every supported host.

#ifndef _KERNEL_VMXNET3_H_
#define _KERNEL_VMXNET3_H_

#include "types.h"
#include "pci.h"
#include "net.h"
#include "sched.h"

// ============================================================================
// PCI Device IDs
// ============================================================================
#define VMXNET3_VENDOR_ID       0x15AD
#define VMXNET3_DEV_ID          0x07B0

// ============================================================================
// MMIO register offsets — BAR0 is "PT" registers (per-queue/per-vector
// doorbells), BAR1 is "VD" registers (control plane).
// ============================================================================
// BAR1 (control)
#define VMXNET3_REG_VRRS    0x000   // Revision Report / Select
#define VMXNET3_REG_UVRS    0x008   // UPT Revision Report / Select
#define VMXNET3_REG_DSAL    0x010   // Driver Shared Address Lo
#define VMXNET3_REG_DSAH    0x018   // Driver Shared Address Hi
#define VMXNET3_REG_CMD     0x020   // Command write / status read
#define VMXNET3_REG_MACL    0x028   // MAC Address Lo
#define VMXNET3_REG_MACH    0x030   // MAC Address Hi
#define VMXNET3_REG_ICR     0x038   // Interrupt Cause Register
#define VMXNET3_REG_ECR     0x040   // Event Cause Register

// BAR0 (data plane doorbells / mask)
#define VMXNET3_REG_IMR     0x000   // Per-vector intr mask, stride 8
#define VMXNET3_REG_TXPROD  0x600   // TX producer index, stride 8
#define VMXNET3_REG_RXPROD  0x800   // RX producer ring 0, stride 8
#define VMXNET3_REG_RXPROD2 0xA00   // RX producer ring 1, stride 8

// ============================================================================
// Commands (write to VMXNET3_REG_CMD)
// ============================================================================
#define VMXNET3_CMD_FIRST_SET           0xCAFE0000
#define VMXNET3_CMD_ACTIVATE_DEV        0xCAFE0000
#define VMXNET3_CMD_QUIESCE_DEV         0xCAFE0001
#define VMXNET3_CMD_RESET_DEV           0xCAFE0002
#define VMXNET3_CMD_UPDATE_RX_MODE      0xCAFE0003
#define VMXNET3_CMD_UPDATE_MAC_FILTERS  0xCAFE0004
#define VMXNET3_CMD_UPDATE_VLAN_FILTERS 0xCAFE0005
#define VMXNET3_CMD_UPDATE_FEATURE      0xCAFE0009

#define VMXNET3_CMD_FIRST_GET           0xF00D0000
#define VMXNET3_CMD_GET_QUEUE_STATUS    0xF00D0000
#define VMXNET3_CMD_GET_STATS           0xF00D0001
#define VMXNET3_CMD_GET_LINK            0xF00D0002
#define VMXNET3_CMD_GET_PERM_MAC_LO     0xF00D0003
#define VMXNET3_CMD_GET_PERM_MAC_HI     0xF00D0004

// ============================================================================
// Magic + version
// ============================================================================
#define VMXNET3_REV1_MAGIC          0xbabefee1
#define VMXNET3_VERSION_SELECT      1
#define VMXNET3_UPT_VERSION_SELECT  1

// ============================================================================
// RX mode bits (write into rxFilterConf.rxMode then UPDATE_RX_MODE)
// ============================================================================
#define VMXNET3_RXM_UCAST       0x01
#define VMXNET3_RXM_MCAST       0x02
#define VMXNET3_RXM_BCAST       0x04
#define VMXNET3_RXM_ALL_MULTI   0x08
#define VMXNET3_RXM_PROMISC     0x10

// ============================================================================
// IntrConf.intrCtrl bit 0 — disable all interrupts (0 = enabled)
// ============================================================================
#define VMXNET3_IC_DISABLE_ALL  0x1

// ============================================================================
// Driver constants (deliberately small — keep memory footprint modest)
// ============================================================================
// Ring sizes must be non-zero multiples of VMXNET3_RING_SIZE_ALIGN (32) —
// the host validates this in vmxnet3_validate_queues() and rejects the
// whole DriverShared blob with "Device configuration ... is invalid"
// otherwise.  We don't actually use ring 2, but the host still validates
// its size, so it gets the minimum legal value.
#define VMXNET3_NUM_TX_DESC     32
#define VMXNET3_NUM_RX_DESC     32
#define VMXNET3_NUM_RX2_DESC    32
#define VMXNET3_NUM_TX_COMP     VMXNET3_NUM_TX_DESC
#define VMXNET3_NUM_RX_COMP     (VMXNET3_NUM_RX_DESC + VMXNET3_NUM_RX2_DESC)

#define VMXNET3_TX_BUF_SIZE     2048
#define VMXNET3_RX_BUF_SIZE     2048

// ============================================================================
// Descriptor structures (16 bytes each, must be packed and ABI-stable).
// We use plain u32 words rather than bitfields to avoid GCC bitfield
// ordering issues — the bit layouts below match VMware's vmxnet3_defs.h.
// ============================================================================

// TX descriptor word 1 (val1):
//   bits  0-13  msscof   (LSO MSS / checksum offset)
//   bit   14    ext1
//   bit   15    dtype
//   bit   16    rsvd
//   bit   17    gen
//   bits 18-31  len      (0 means 16384)
// TX descriptor word 2 (val2):
//   bits  0-9   hlen
//   bits 10-11  om       (offload mode)
//   bit  12     eop
//   bit  13     cq       (completion request)
//   bit  14     ext2
//   bit  15     ti       (tag insert)
//   bits 16-31  tci
typedef struct {
    uint64_t addr;
    uint32_t val1;
    uint32_t val2;
} __attribute__((packed)) vmxnet3_tx_desc_t;

// RX descriptor word val:
//   bits  0-13  len      (buffer size, 0 = 16384)
//   bit  14     btype    (0 = head, 1 = body)
//   bit  15     dtype
//   bits 16-30  rsvd
//   bit  31     gen
typedef struct {
    uint64_t addr;
    uint32_t val;
    uint32_t reserved;
} __attribute__((packed)) vmxnet3_rx_desc_t;

// TX completion descriptor:
//   word0 bits 0-11   txdIdx
//   word3 bits 0-7    type   (UPT1_CDTYPE_TXCOMP=0)
//   word3 bit  31     gen
typedef struct {
    uint32_t word0;
    uint32_t word1;
    uint32_t word2;
    uint32_t word3;
} __attribute__((packed)) vmxnet3_tx_comp_desc_t;

// RX completion descriptor:
//   word0 bits  0-11  rxdIdx
//   word0 bit  12     ext1
//   word0 bits 13-22  rqID
//   word0 bits 24-26  rssType
//   word0 bit  27     err
//   word0 bit  28     eop
//   word0 bit  29     sop
//   word2 bits  0-13  len
//   word2 bit  31     ?
//   word3 bits  0-7   type (UPT1_CDTYPE_RXCOMP=3)
//   word3 bit  31     gen
typedef struct {
    uint32_t word0;
    uint32_t word1;
    uint32_t word2;
    uint32_t word3;
} __attribute__((packed)) vmxnet3_rx_comp_desc_t;

// ============================================================================
// DriverShared sub-structures.  Field ordering and sizes must match
// VMware's vmxnet3_defs.h byte-for-byte; the host validates the layout.
// ============================================================================
typedef struct {
    uint32_t version;               // driver version (any non-zero)
    uint32_t gos;                   // gos info packed (see init code)
    uint32_t vmxnet3RevSpt;         // bitmap: rev1 supported = bit 0
    uint32_t uptVerSpt;             // bitmap: UPT1 supported = bit 0
} __attribute__((packed)) vmxnet3_driver_info_t;

typedef struct {
    vmxnet3_driver_info_t driverInfo;
    uint64_t uptFeatures;           // 0 — no LRO/RSS/etc.
    uint64_t ddPA;                  // PA of the DriverShared itself (back-ptr)
    uint64_t queueDescPA;           // PA of TxQueueDesc[0] (then RxQueueDesc[0])
    uint32_t ddLen;                 // sizeof(DriverShared)
    uint32_t queueDescLen;          // total bytes of queue descs
    uint32_t mtu;
    uint16_t maxNumRxSG;
    uint8_t  numTxQueues;
    uint8_t  numRxQueues;
    uint32_t reserved[4];
} __attribute__((packed)) vmxnet3_misc_conf_t;

#define VMXNET3_MAX_INTRS 25
typedef struct {
    uint8_t  autoMask;
    uint8_t  numIntrs;
    uint8_t  eventIntrIdx;
    uint8_t  modLevels[VMXNET3_MAX_INTRS];
    uint32_t intrCtrl;
    uint32_t reserved[2];
} __attribute__((packed)) vmxnet3_intr_conf_t;

// VMXNET3_VFT_SIZE = 4096 bits / 32 = 128 u32 words.  This is fixed by
// the on-the-wire ABI; making it smaller shifts every following field
// (rssConfDesc / pmConfDesc / pluginConfDesc) and the host reads garbage.
#define VMXNET3_VFT_SIZE        128
typedef struct {
    uint32_t rxMode;
    uint16_t mfTableLen;
    uint16_t pad1;
    uint64_t mfTablePA;
    uint32_t vfTable[VMXNET3_VFT_SIZE];
} __attribute__((packed)) vmxnet3_rx_filter_conf_t;

typedef struct {
    uint32_t confVer;
    uint32_t confLen;
    uint64_t confPA;
} __attribute__((packed)) vmxnet3_var_len_conf_t;

typedef struct {
    vmxnet3_misc_conf_t       misc;
    vmxnet3_intr_conf_t       intrConf;
    vmxnet3_rx_filter_conf_t  rxFilterConf;
    vmxnet3_var_len_conf_t    rssConfDesc;
    vmxnet3_var_len_conf_t    pmConfDesc;
    vmxnet3_var_len_conf_t    pluginConfDesc;
} __attribute__((packed)) vmxnet3_dev_read_t;

typedef struct {
    uint32_t magic;
    uint32_t pad;
    vmxnet3_dev_read_t devRead;
    uint32_t ecr;
    uint32_t reserved[5];
} __attribute__((packed)) vmxnet3_driver_shared_t;

// ----------------------------------------------------------------------------
// Per-queue descriptor blobs (one each for TX and RX, written by us, read
// by the host on ACTIVATE_DEV).
// ----------------------------------------------------------------------------
// Spec layout: u32 + u32 + u64 reserved = 16 bytes.  Required size,
// not optional padding — the host computes Vmxnet3_TxQueueDesc layout
// using the spec sizes and reads at the wrong offsets if we shrink it.
typedef struct {
    uint32_t txNumDeferred;
    uint32_t txThreshold;
    uint64_t reserved;
} __attribute__((packed)) vmxnet3_tx_queue_ctrl_t;

typedef struct {
    uint64_t txRingBasePA;
    uint64_t dataRingBasePA;
    uint64_t compRingBasePA;
    uint64_t ddPA;
    uint64_t reserved;
    uint32_t txRingSize;
    uint32_t dataRingSize;
    uint32_t compRingSize;
    uint32_t ddLen;
    uint8_t  intrIdx;
    uint8_t  pad1[1];
    uint16_t txDataRingDescSize;
    uint8_t  pad2[4];
} __attribute__((packed)) vmxnet3_tx_queue_conf_t;

// Spec layout: u32 + u32[7] = 32 bytes.
// Spec layout (vmxnet3.h): bool updateRxProd + u8 _pad[7] + __le64 reserved
// = 16 bytes total.  The host computes the offset of `conf` from this
// size, so it MUST be 16 (not 32).
typedef struct {
    uint8_t  updateRxProd;
    uint8_t  pad[7];
    uint64_t reserved;
} __attribute__((packed)) vmxnet3_rx_queue_ctrl_t;

typedef struct {
    uint64_t rxRingBasePA[2];
    uint64_t compRingBasePA;
    uint64_t ddPA;
    uint64_t reserved;
    uint32_t rxRingSize[2];
    uint32_t compRingSize;
    uint32_t ddLen;
    uint8_t  intrIdx;
    uint8_t  pad[7];
} __attribute__((packed)) vmxnet3_rx_queue_conf_t;

typedef struct {
    uint8_t  stopped;
    uint8_t  pad[3];
    uint32_t error;
} __attribute__((packed)) vmxnet3_queue_status_t;

typedef struct { uint64_t s[10]; } __attribute__((packed)) vmxnet3_upt_tx_stats_t;
typedef struct { uint64_t s[10]; } __attribute__((packed)) vmxnet3_upt_rx_stats_t;

// Spec order:  ctrl, conf, status, stats, _pad[88]
// Sizes:       16 + 64 + 8 + 80 + 88 = 256 bytes (matches sizeof on host).
typedef struct {
    vmxnet3_tx_queue_ctrl_t  ctrl;     //  16
    vmxnet3_tx_queue_conf_t  conf;     //  64
    vmxnet3_queue_status_t   status;   //   8
    vmxnet3_upt_tx_stats_t   stats;    //  80
    uint8_t pad[88];                   //  88
} __attribute__((packed)) vmxnet3_tx_queue_desc_t;

// Spec order:  ctrl, conf, status, stats, _pad[88]   (same order as TX!)
// Sizes:       16 + 64 + 8 + 80 + 88 = 256 bytes
typedef struct {
    vmxnet3_rx_queue_ctrl_t  ctrl;     //  16
    vmxnet3_rx_queue_conf_t  conf;     //  64
    vmxnet3_queue_status_t   status;   //   8
    vmxnet3_upt_rx_stats_t   stats;    //  80
    uint8_t pad[88];                   //  88
} __attribute__((packed)) vmxnet3_rx_queue_desc_t;

// ============================================================================
// Per-device state
// ============================================================================
typedef struct {
    const pci_device_t* pci_dev;

    volatile uint8_t* bar0;     // PT regs (4 KiB)
    volatile uint8_t* bar1;     // VD regs (4 KiB)
    uint64_t bar0_phys, bar1_phys;

    uint8_t  mac_addr[6];

    // DriverShared + queue descs (one contiguous page)
    vmxnet3_driver_shared_t* shared;
    uint64_t shared_phys;
    vmxnet3_tx_queue_desc_t* txqd;
    uint64_t txqd_phys;
    vmxnet3_rx_queue_desc_t* rxqd;
    uint64_t rxqd_phys;

    // Rings
    vmxnet3_tx_desc_t*       tx_ring;
    uint64_t                 tx_ring_phys;
    vmxnet3_tx_comp_desc_t*  tx_comp_ring;
    uint64_t                 tx_comp_ring_phys;
    vmxnet3_rx_desc_t*       rx_ring;
    uint64_t                 rx_ring_phys;
    vmxnet3_rx_desc_t*       rx_ring2;
    uint64_t                 rx_ring2_phys;
    vmxnet3_rx_comp_desc_t*  rx_comp_ring;
    uint64_t                 rx_comp_ring_phys;

    // Buffers
    uint8_t*  tx_bufs[VMXNET3_NUM_TX_DESC];
    uint64_t  tx_bufs_phys[VMXNET3_NUM_TX_DESC];
    uint8_t*  rx_bufs[VMXNET3_NUM_RX_DESC];
    uint64_t  rx_bufs_phys[VMXNET3_NUM_RX_DESC];

    // Software ring state
    uint16_t tx_prod;       // next slot to fill
    uint8_t  tx_gen;        // generation we write into new TX descs
    uint16_t tx_comp_next;
    uint8_t  tx_comp_gen;   // generation we expect on TX completion
    spinlock_t tx_lock;     // Serializes vmxnet3_send() across CPUs
    uint16_t rx_prod;
    uint8_t  rx_gen;
    uint16_t rx_comp_next;
    uint8_t  rx_comp_gen;

    // Multicast filter table (1-byte placeholder; we accept all multicast)
    uint8_t* mf_table;
    uint64_t mf_table_phys;

    int link_up;
    uint8_t msi_vector;     // 0 if MSI not in use

    net_device_t net_dev;
} vmxnet3_dev_t;

// ============================================================================
// Public API
// ============================================================================
void vmxnet3_init(void);
void vmxnet3_irq_handler(void);

#endif // _KERNEL_VMXNET3_H_
