// LikeOS-64 - USB Mass Storage BOT + SCSI minimal scaffolding
#ifndef LIKEOS_USB_MSD_H
#define LIKEOS_USB_MSD_H

#include "types.h"

#pragma pack(push,1)
typedef struct {
    uint32_t signature;   // 'USBC' 0x43425355
    uint32_t tag;         // echoed in CSW
    uint32_t data_len;    // expected data length
    uint8_t  flags;       // bit7: direction (1=in)
    uint8_t  lun;         // logical unit
    uint8_t  cb_len;      // length of command block
    uint8_t  cb[16];      // command block (SCSI)
} usb_msd_cbw_t;

typedef struct {
    uint32_t signature;   // 'USBS' 0x53425355
    uint32_t tag;         // must match CBW tag
    uint32_t residue;     // difference between expected and actual
    uint8_t  status;      // 0=Pass 1=Fail 2=PhaseError
} usb_msd_csw_t;
#pragma pack(pop)

// SCSI opcodes used
#define SCSI_INQUIRY          0x12
#define SCSI_READ_CAPACITY_10 0x25
#define SCSI_TEST_UNIT_READY  0x00
#define SCSI_REQUEST_SENSE    0x03

int usb_msd_try_init(void* ctrl_ptr); // controller pointer to access rings
void usb_msd_poll(void* ctrl_ptr);

#endif
