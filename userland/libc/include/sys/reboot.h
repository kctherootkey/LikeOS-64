#ifndef _SYS_REBOOT_H
#define _SYS_REBOOT_H

/* Commands for the reboot() system call */
#define RB_AUTOBOOT     0x01234567  /* Restart system */
#define RB_HALT_SYSTEM  0xCDEF0123  /* Halt system, stay powered */
#define RB_ENABLE_CAD   0x89ABCDEF  /* Enable Ctrl-Alt-Del reboot */
#define RB_DISABLE_CAD  0x00000000  /* Disable Ctrl-Alt-Del reboot */
#define RB_POWER_OFF    0x4321FEDC  /* Power off system */

int reboot(int cmd);

#endif /* _SYS_REBOOT_H */
