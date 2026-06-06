// LikeOS-64 - USB serial logging backend

#ifndef LIKEOS_USB_SERIAL_H
#define LIKEOS_USB_SERIAL_H

#include "types.h"
#include "status.h"
#include "xhci.h"

void usbserial_init(void);
int usbserial_probe(xhci_controller_t* ctrl, usb_device_t* dev,
                    uint8_t* config_desc, uint16_t config_len);
void usbserial_disconnect(usb_device_t* dev);
void usbserial_log_write(const char* data, uint32_t len);
int usbserial_is_active(void);

#endif // LIKEOS_USB_SERIAL_H