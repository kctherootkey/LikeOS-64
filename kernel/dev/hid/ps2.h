// LikeOS - PS/2 Controller Initialization
//
// Copyright (C) 2026 The LikeOS Project

#pragma once
typedef unsigned char uint8_t; /* minimal fixed-width for freestanding env */
int ps2_init(void);
/* Disable the controller's auxiliary (mouse) port: command 0xA7, which
 * also sets the "aux clock disabled" bit of the configuration byte.  The
 * keyboard port is untouched.  0, -1 when there is no controller or it
 * does not accept the command.  Any context (polls a few microseconds). */
int ps2_disable_aux_port(void);
