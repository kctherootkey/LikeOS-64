// LikeOS-64 - HID over I2C Driver
// Intel DesignWare I2C controller + Microsoft HID-over-I2C protocol
//
// Supports Intel LPSS Serial IO I2C found on Tiger Lake through Arrow Lake.
// Discovers I2C HID devices (touchpads, keyboards) via bus scan and
// injects input into existing keyboard/mouse subsystems.
//
// References:
//   - Synopsys DesignWare DW_apb_i2c Databook
//   - Microsoft HID over I2C Protocol Specification v1.0
//   - Intel LPSS Serial IO I2C documentation

#ifndef LIKEOS_I2C_HID_H
#define LIKEOS_I2C_HID_H

#include "types.h"
#include "pci.h"

// ============================================================================
// Intel LPSS Serial IO I2C PCI Device IDs
// ============================================================================

#define INTEL_LPSS_I2C_VENDOR       0x8086

// Arrow Lake (ARL)
#define INTEL_LPSS_I2C_ARL_H_0     0x7778
#define INTEL_LPSS_I2C_ARL_H_1     0x7779
#define INTEL_LPSS_I2C_ARL_H_2     0x777A
#define INTEL_LPSS_I2C_ARL_H_3     0x777B

// Meteor Lake (MTL)
#define INTEL_LPSS_I2C_MTL_0       0x7E50
#define INTEL_LPSS_I2C_MTL_1       0x7E51
#define INTEL_LPSS_I2C_MTL_2       0x7E78
#define INTEL_LPSS_I2C_MTL_3       0x7E79

// Raptor Lake (RPL)
#define INTEL_LPSS_I2C_RPL_S_0     0x7A78
#define INTEL_LPSS_I2C_RPL_S_1     0x7A79
#define INTEL_LPSS_I2C_RPL_P_0     0x51E8
#define INTEL_LPSS_I2C_RPL_P_1     0x51E9

// Alder Lake (ADL)
#define INTEL_LPSS_I2C_ADL_S_0     0x7AF8
#define INTEL_LPSS_I2C_ADL_S_1     0x7AF9
#define INTEL_LPSS_I2C_ADL_P_0     0x51C5
#define INTEL_LPSS_I2C_ADL_P_1     0x51C6
#define INTEL_LPSS_I2C_ADL_N_0     0x54C5
#define INTEL_LPSS_I2C_ADL_N_1     0x54C6

// Tiger Lake (TGL)
#define INTEL_LPSS_I2C_TGL_0       0xA0C5
#define INTEL_LPSS_I2C_TGL_1       0xA0C6
#define INTEL_LPSS_I2C_TGL_2       0xA0D8
#define INTEL_LPSS_I2C_TGL_3       0xA0D9

// ============================================================================
// DesignWare I2C Controller Register Map
// ============================================================================

#define DW_IC_CON                  0x00    // Control
#define DW_IC_TAR                  0x04    // Target address
#define DW_IC_SAR                  0x08    // Slave address
#define DW_IC_DATA_CMD             0x10    // Data buffer and command
#define DW_IC_SS_SCL_HCNT         0x14    // Standard speed SCL high count
#define DW_IC_SS_SCL_LCNT         0x18    // Standard speed SCL low count
#define DW_IC_FS_SCL_HCNT         0x1C    // Fast speed SCL high count
#define DW_IC_FS_SCL_LCNT         0x20    // Fast speed SCL low count
#define DW_IC_INTR_STAT           0x2C    // Interrupt status (read-only)
#define DW_IC_INTR_MASK           0x30    // Interrupt mask
#define DW_IC_RAW_INTR_STAT       0x34    // Raw interrupt status
#define DW_IC_RX_TL               0x38    // RX FIFO threshold
#define DW_IC_TX_TL               0x3C    // TX FIFO threshold
#define DW_IC_CLR_INTR            0x40    // Clear combined interrupt
#define DW_IC_CLR_RX_UNDER        0x44
#define DW_IC_CLR_RX_OVER         0x48
#define DW_IC_CLR_TX_OVER         0x4C
#define DW_IC_CLR_RD_REQ          0x50
#define DW_IC_CLR_TX_ABRT         0x54
#define DW_IC_CLR_RX_DONE         0x58
#define DW_IC_CLR_ACTIVITY        0x5C
#define DW_IC_CLR_STOP_DET        0x60
#define DW_IC_CLR_START_DET       0x64
#define DW_IC_CLR_GEN_CALL        0x68
#define DW_IC_ENABLE              0x6C    // Enable
#define DW_IC_STATUS              0x70    // Status
#define DW_IC_TXFLR               0x74    // TX FIFO level
#define DW_IC_RXFLR               0x78    // RX FIFO level
#define DW_IC_SDA_HOLD            0x7C    // SDA hold time
#define DW_IC_TX_ABRT_SOURCE      0x80    // TX abort source
#define DW_IC_SDA_SETUP           0x94    // SDA setup time
#define DW_IC_ENABLE_STATUS       0x9C    // Enable status
#define DW_IC_FS_SPKLEN           0xA0    // SS and FS spike suppression limit
#define DW_IC_COMP_PARAM_1        0xF4    // Component parameter
#define DW_IC_COMP_VERSION        0xF8    // Component version
#define DW_IC_COMP_TYPE           0xFC    // Component type (expect 0x44570140)

// Intel LPSS private registers (at BAR + 0x200)
#define LPSS_PRIV_RESETS          0x204   // Reset control
#define LPSS_PRIV_GENERAL         0x208   // General config
#define LPSS_PRIV_ACTIVELTR       0x210   // Active LTR value
#define LPSS_PRIV_IDLELTR         0x214   // Idle LTR value
#define LPSS_PRIV_CLOCK_GATE      0x238   // Clock gate control
#define LPSS_PRIV_REMAP_ADDR_LO   0x240   // Remap address low
#define LPSS_PRIV_REMAP_ADDR_HI   0x244   // Remap address high
#define LPSS_PRIV_DEVIDLE_CTRL    0x24C   // Device idle control
#define LPSS_PRIV_CAPABILITIES    0x2FC   // Capabilities (read-only)

// Intel LPSS PCI config registers
#define LPSS_PCI_D0I3C            0x98    // SW LTR update MMIO location
#define LPSS_PCI_DEVIDLE_PTR      0x9C    // Device idle MMIO location
#define LPSS_PCI_PG_CONFIG        0xA0    // D0I3 / power-gate config

// PCI PMCSR bits (Power Management Control/Status Register)
#define PCI_PMCSR_POWER_STATE_MASK  0x0003
#define PCI_PMCSR_POWER_STATE_D0    0x0000
#define PCI_PMCSR_POWER_STATE_D3HOT 0x0003
#define PCI_PMCSR_PME_STATUS        0x8000

// Pointer register bits
#define LPSS_PCI_PTR_VALID        (1 << 0)

// Device power-gate config bits (PCI offset 0xA0)
#define LPSS_PCI_PGCFG_PMCRE      (1 << 16)
#define LPSS_PCI_PGCFG_I3_ENABLE  (1 << 17)
#define LPSS_PCI_PGCFG_PGE        (1 << 18)
#define LPSS_PCI_PGCFG_SLEEP_EN   (1 << 19)
#define LPSS_PCI_PGCFG_HAE        (1 << 21)  // Hardware Autonomous Enable
#define LPSS_PCI_PGCFG_ALL_GATE   (LPSS_PCI_PGCFG_PMCRE  | \
                                   LPSS_PCI_PGCFG_I3_ENABLE | \
                                   LPSS_PCI_PGCFG_PGE     | \
                                   LPSS_PCI_PGCFG_SLEEP_EN | \
                                   LPSS_PCI_PGCFG_HAE)

// PMC (D31:F2) PCI config
#define PMC_PCI_BUS               0
#define PMC_PCI_DEV               31
#define PMC_PCI_FUNC              2
#define PMC_PCI_PWRMBASE          0x10   // BAR for PMC MMIO (64-bit, bits [31:13])
#define PMC_PCI_PWRMBASE_HI       0x14
#define PMC_PCI_CMD               0x04

// PMC MMIO register offsets (from PWRMBASE)
#define PMC_PPASR0                0x1D80 // Power Gated ACK Status Register 0
#define PMC_ST_PG_FDIS2           0x1E24 // Static Function Disable 2 (I2C bits 0-5)
#define PMC_NST_PG_FDIS_1        0x1E28 // Chipset Init Register
#define PMC_FUSE_SS_DIS_RD_2     0x1E44 // Fuse/capability disable read

// CLOCK_GATE bits: 00=dynamic clock gate, 11=force clocks on
#define LPSS_CLOCK_GATE_IP_ON     0x03    // Force IP clock on (bits [1:0])
#define LPSS_CLOCK_GATE_DMA_ON    0x0C    // Force DMA clock on (bits [3:2])
#define LPSS_CLOCK_GATE_FORCE_ALL 0x0F    // Force all clocks on

// DEVIDLE_CONTROL bits
#define LPSS_DEVIDLE_CMD_PROGRESS (1 << 0) // Command in progress (RO)
#define LPSS_DEVIDLE_DEVIDLE      (1 << 2) // Device idle (1=idle, 0=active D0)
#define LPSS_DEVIDLE_RESTORE_REQ  (1 << 3) // Restore required (RW1C)

// GENERAL register bits
#define LPSS_GENERAL_LTR_MODE_SW  (1 << 2)

// LPSS CAPABILITIES decoding
#define LPSS_CAPS_NO_IDMA         (1 << 8)
#define LPSS_CAPS_TYPE_SHIFT      4
#define LPSS_CAPS_TYPE_MASK       (0xF << LPSS_CAPS_TYPE_SHIFT)
#define LPSS_DEV_TYPE_I2C         0

// IC_CON bits
#define DW_IC_CON_MASTER          (1 << 0)    // Master mode
#define DW_IC_CON_SPEED_SS        (1 << 1)    // Standard speed (100kHz)
#define DW_IC_CON_SPEED_FS        (2 << 1)    // Fast speed (400kHz)
#define DW_IC_CON_10BITADDR_SLAVE (1 << 3)
#define DW_IC_CON_10BITADDR_MSTR  (1 << 4)
#define DW_IC_CON_RESTART_EN      (1 << 5)    // Enable restart
#define DW_IC_CON_SLAVE_DISABLE   (1 << 6)    // Disable slave mode

// IC_DATA_CMD bits
#define DW_IC_DATA_CMD_READ       (1 << 8)    // Read from slave
#define DW_IC_DATA_CMD_STOP       (1 << 9)    // Send STOP after byte
#define DW_IC_DATA_CMD_RESTART    (1 << 10)   // Send RESTART before byte

// IC_STATUS bits
#define DW_IC_STATUS_ACTIVITY     (1 << 0)
#define DW_IC_STATUS_TFNF         (1 << 1)    // TX FIFO not full
#define DW_IC_STATUS_TFE          (1 << 2)    // TX FIFO empty
#define DW_IC_STATUS_RFNE         (1 << 3)    // RX FIFO not empty
#define DW_IC_STATUS_RFF          (1 << 4)    // RX FIFO full
#define DW_IC_STATUS_MST_ACTIVITY (1 << 5)

// IC_RAW_INTR_STAT / IC_INTR_STAT bits
#define DW_IC_INTR_RX_UNDER      (1 << 0)
#define DW_IC_INTR_RX_OVER       (1 << 1)
#define DW_IC_INTR_RX_FULL       (1 << 2)
#define DW_IC_INTR_TX_OVER       (1 << 3)
#define DW_IC_INTR_TX_EMPTY      (1 << 4)
#define DW_IC_INTR_RD_REQ        (1 << 5)
#define DW_IC_INTR_TX_ABRT       (1 << 6)
#define DW_IC_INTR_RX_DONE       (1 << 7)
#define DW_IC_INTR_ACTIVITY      (1 << 8)
#define DW_IC_INTR_STOP_DET      (1 << 9)
#define DW_IC_INTR_START_DET     (1 << 10)
#define DW_IC_INTR_GEN_CALL      (1 << 11)

// IC_ENABLE_STATUS bits
#define DW_IC_EN_STATUS_IC_EN    (1 << 0)

// Expected component type magic
#define DW_IC_COMP_TYPE_VALUE    0x44570140

// LPSS reset bits
// RESET_I2C is a 2-bit field: 00 = held in reset, 11 = released.
#define LPSS_RESETS_FUNC         0x3         // Function reset field released
#define LPSS_RESETS_IDMA         (1 << 2)    // iDMA reset released

// ============================================================================
// HID over I2C Protocol Definitions (Microsoft spec v1.0)
// ============================================================================

// HID Descriptor (30 bytes, fetched from device at a known register address)
typedef struct __attribute__((packed)) {
    uint16_t wHIDDescLength;       // Should be 30 (0x001E)
    uint16_t bcdVersion;           // HID version (0x0100)
    uint16_t wReportDescLength;    // Length of report descriptor
    uint16_t wReportDescRegister;  // Register for report descriptor
    uint16_t wInputRegister;       // Register for input reports
    uint16_t wMaxInputLength;      // Max input report length (including 2-byte length prefix)
    uint16_t wOutputRegister;      // Register for output reports
    uint16_t wMaxOutputLength;     // Max output report length
    uint16_t wCommandRegister;     // Register for commands
    uint16_t wDataRegister;        // Register for data
    uint16_t wVendorID;            // Vendor ID
    uint16_t wProductID;           // Product ID
    uint16_t wVersionID;           // Version ID
    uint32_t wReserved;            // Reserved
} i2c_hid_desc_t;

// HID-over-I2C opcodes (written to wCommandRegister)
#define I2C_HID_OPCODE_RESET       0x0001
#define I2C_HID_OPCODE_GET_REPORT  0x0002
#define I2C_HID_OPCODE_SET_REPORT  0x0003
#define I2C_HID_OPCODE_SET_POWER   0x0008

// Power states for SET_POWER
#define I2C_HID_POWER_ON           0x0000
#define I2C_HID_POWER_SLEEP        0x0100

// Common HID descriptor register addresses (tried during probe)
#define I2C_HID_DESC_REG_0001      0x0001
#define I2C_HID_DESC_REG_0020      0x0020
#define I2C_HID_DESC_REG_0030      0x0030

// Max input report size we support
#define I2C_HID_MAX_REPORT_SIZE    256

// ============================================================================
// Minimal HID Report Descriptor Parser structures
// ============================================================================

// Device types we detect from report descriptors
#define I2C_HID_DEV_UNKNOWN        0
#define I2C_HID_DEV_KEYBOARD       1
#define I2C_HID_DEV_MOUSE          2
#define I2C_HID_DEV_TOUCHPAD       3

// Parsed field info from report descriptor
typedef struct {
    uint8_t  report_id;           // Report ID (0 if none)
    uint16_t bit_offset;          // Bit offset within the report (after report ID)
    uint16_t bit_size;            // Total bits for this field
    uint16_t count;               // Number of values
    int32_t  logical_min;
    int32_t  logical_max;
    uint8_t  is_relative;         // 1 if relative, 0 if absolute
} i2c_hid_field_t;

// Parsed report layout for mouse/touchpad
typedef struct {
    uint8_t          report_id;        // Report ID for this report (0 if none)
    uint8_t          has_report_id;    // Device uses report IDs
    uint8_t          dev_type;         // I2C_HID_DEV_*
    // Button field
    i2c_hid_field_t  buttons;
    // X movement/position
    i2c_hid_field_t  x;
    // Y movement/position
    i2c_hid_field_t  y;
    // Scroll wheel (optional)
    i2c_hid_field_t  wheel;
    uint8_t          has_wheel;
    // Total report size in bytes (excluding report ID byte)
    uint16_t         report_bytes;
} i2c_hid_report_info_t;

// ============================================================================
// Driver Structures
// ============================================================================

#define I2C_DW_MAX_CONTROLLERS     4
#define I2C_HID_MAX_DEVICES        4

// DesignWare I2C controller instance
typedef struct {
    volatile uint32_t *base;        // MMIO base (virtual)
    uint64_t           bar_phys;    // BAR physical address
    const pci_device_t *pci_dev;    // PCI device reference
    uint8_t            bus_id;      // Logical I2C bus number
    uint16_t           rx_fifo_depth;
    uint16_t           tx_fifo_depth;
    uint8_t            active;      // Controller is initialized
    char               acpi_path[64]; // ACPI device path (e.g. "\\_SB.PCI0.I2C0")
    uint32_t           irq_gsi;     // ACPI GSI for this controller (from _CRS)
    uint8_t            irq_vector;  // IDT vector assigned for this controller
    uint8_t            use_interrupts; // 1 if interrupt mode active
    volatile uint8_t   irq_pending; // Set by ISR, cleared by handler
    volatile uint8_t   tx_complete; // TX complete flag (set by ISR)
    volatile uint8_t   rx_ready;    // RX data available (set by ISR)
    volatile uint8_t   xfer_error;  // Transfer error (set by ISR)
    volatile uint32_t  abort_source; // TX_ABRT_SOURCE captured by ISR
} i2c_dw_controller_t;

// I2C HID device instance
typedef struct {
    i2c_dw_controller_t *ctrl;       // I2C controller
    uint16_t             i2c_addr;   // I2C slave address (7-bit)
    uint16_t             hid_desc_reg; // Register address for HID descriptor
    i2c_hid_desc_t       desc;       // Parsed HID descriptor
    uint8_t             *report_desc; // Allocated report descriptor
    uint16_t             report_desc_len;
    uint8_t              dev_type;   // I2C_HID_DEV_*
    i2c_hid_report_info_t mouse_report; // For mouse/touchpad
    uint8_t              prev_buttons;
    uint8_t              active;        // Device is active
    uint8_t              input_buf[I2C_HID_MAX_REPORT_SIZE];
} i2c_hid_device_t;

// ============================================================================
// Public API
// ============================================================================

// Initialize all detected Intel LPSS I2C controllers and probe for HID devices
int i2c_hid_init(void);

// Poll all active I2C HID devices for input (call from main loop)
void i2c_hid_poll(void);

// Interrupt handler for I2C controller (called from IDT stub)
void i2c_hid_irq_handler(uint8_t vector);

// Query device status
int i2c_hid_has_touchpad(void);
int i2c_hid_device_count(void);

#endif // LIKEOS_I2C_HID_H
