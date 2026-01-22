// LikeOS-64 - USB Mass Storage (Bulk-Only Transport) Driver
#ifndef LIKEOS_USB_MSD_H
#define LIKEOS_USB_MSD_H

#include "types.h"
#include "status.h"
#include "xhci.h"
#include "block.h"

// USB Mass Storage class codes
#define USB_CLASS_MASS_STORAGE      0x08
#define USB_SUBCLASS_SCSI           0x06
#define USB_PROTOCOL_BOT            0x50

// SCSI opcodes
#define SCSI_TEST_UNIT_READY        0x00
#define SCSI_REQUEST_SENSE          0x03
#define SCSI_INQUIRY                0x12
#define SCSI_READ_CAPACITY_10       0x25
#define SCSI_READ_10                0x28
#define SCSI_WRITE_10               0x2A

// Command Block Wrapper (CBW) - 31 bytes
#define CBW_SIGNATURE               0x43425355  // "USBC"
#define CBW_SIZE                    31
#define CBW_FLAG_DATA_IN            0x80
#define CBW_FLAG_DATA_OUT           0x00

typedef struct __attribute__((packed)) {
    uint32_t signature;         // 0x43425355 = "USBC"
    uint32_t tag;               // Transaction tag
    uint32_t data_transfer_len; // Number of bytes to transfer
    uint8_t flags;              // Direction: 0x80=in, 0x00=out
    uint8_t lun;                // Logical Unit Number
    uint8_t cb_length;          // Command block length (1-16)
    uint8_t cb[16];             // Command block (SCSI CDB)
} usb_msd_cbw_t;

// Command Status Wrapper (CSW) - 13 bytes
#define CSW_SIGNATURE               0x53425355  // "USBS"
#define CSW_SIZE                    13
#define CSW_STATUS_PASSED           0
#define CSW_STATUS_FAILED           1
#define CSW_STATUS_PHASE_ERROR      2

typedef struct __attribute__((packed)) {
    uint32_t signature;         // 0x53425355 = "USBS"
    uint32_t tag;               // Same as CBW tag
    uint32_t data_residue;      // Difference between expected and actual data
    uint8_t status;             // 0=passed, 1=failed, 2=phase error
} usb_msd_csw_t;

// SCSI Inquiry response
typedef struct __attribute__((packed)) {
    uint8_t device_type;        // Peripheral device type
    uint8_t rmb;                // Removable media bit
    uint8_t version;            // SCSI version
    uint8_t response_format;    // Response data format
    uint8_t additional_len;     // Additional length
    uint8_t reserved[3];
    char vendor[8];             // Vendor identification
    char product[16];           // Product identification
    char revision[4];           // Product revision
} scsi_inquiry_data_t;

// SCSI Read Capacity (10) response
typedef struct __attribute__((packed)) {
    uint32_t last_lba;          // Last logical block address (big-endian)
    uint32_t block_size;        // Block size in bytes (big-endian)
} scsi_read_capacity_data_t;

// SCSI Request Sense response
typedef struct __attribute__((packed)) {
    uint8_t response_code;
    uint8_t segment;
    uint8_t sense_key;
    uint8_t information[4];
    uint8_t additional_len;
    uint8_t reserved[4];
    uint8_t asc;                // Additional sense code
    uint8_t ascq;               // Additional sense code qualifier
    uint8_t reserved2[4];
} scsi_sense_data_t;

// USB Mass Storage device structure
typedef struct usb_msd_device {
    usb_device_t* usb_dev;      // Back-reference to USB device
    xhci_controller_t* ctrl;    // Controller reference
    
    // SCSI device info
    uint32_t block_count;
    uint32_t block_size;
    uint8_t ready;
    uint8_t removable;
    char vendor[9];
    char product[17];
    
    // Transaction tracking
    uint32_t next_tag;
    
    // Block device
    block_device_t blk;
} usb_msd_device_t;

// MSD state for boot integration
typedef struct {
    usb_msd_device_t* msd;
    int initialized;
} msd_state_t;

// Core MSD functions
int usb_msd_init(usb_msd_device_t* msd, usb_device_t* dev, xhci_controller_t* ctrl);
int usb_msd_test_unit_ready(usb_msd_device_t* msd);
int usb_msd_inquiry(usb_msd_device_t* msd, scsi_inquiry_data_t* data);
int usb_msd_read_capacity(usb_msd_device_t* msd, uint32_t* block_count, uint32_t* block_size);
int usb_msd_request_sense(usb_msd_device_t* msd, scsi_sense_data_t* data);
int usb_msd_read(usb_msd_device_t* msd, uint32_t lba, uint32_t count, void* buf);
int usb_msd_write(usb_msd_device_t* msd, uint32_t lba, uint32_t count, const void* buf);

// Low-level BOT protocol functions
int usb_msd_bot_transfer(usb_msd_device_t* msd, usb_msd_cbw_t* cbw,
                         void* data_buf, uint32_t data_len, usb_msd_csw_t* csw);

// Block device interface callbacks
int usb_msd_block_read(block_device_t* dev, unsigned long lba, unsigned long count, void* buf);
int usb_msd_block_write(block_device_t* dev, unsigned long lba, unsigned long count, const void* buf);

// Global MSD state
extern usb_msd_device_t* g_msd_devices[8];
extern int g_msd_count;

// Byte swapping for big-endian SCSI data
static inline uint32_t bswap32(uint32_t val) {
    return ((val >> 24) & 0xFF) |
           ((val >> 8) & 0xFF00) |
           ((val << 8) & 0xFF0000) |
           ((val << 24) & 0xFF000000);
}

static inline uint16_t bswap16(uint16_t val) {
    return (val >> 8) | (val << 8);
}

#endif // LIKEOS_USB_MSD_H
