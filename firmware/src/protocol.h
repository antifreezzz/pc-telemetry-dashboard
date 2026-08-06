#pragma once
#include <Arduino.h>

#define PROTO_SYNC 0xAA
#define PROTO_TYPE 0xF1
#define PROTO_MAX_PKT 256

#define FIELD_CPU 0x01
#define FIELD_RAM 0x02
#define FIELD_GPU 0x03
#define FIELD_NET 0x04
#define FIELD_DISK 0x05
#define FIELD_HEADER 0x06
#define FIELD_PROC 0x07

extern volatile uint32_t g_epoch_sec;
extern volatile uint32_t g_uptime_sec;
extern char g_hostname[24];

void protocol_init();
void protocol_poll();  // call from loop()