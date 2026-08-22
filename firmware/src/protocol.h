#pragma once
#include <Arduino.h>

#define PROTO_SYNC 0xAA
#define PROTO_TYPE 0xF1
#define PROTO_MAX_PKT 2048

#define FIELD_CPU 0x01
#define FIELD_RAM 0x02
#define FIELD_GPU 0x03
#define FIELD_NET 0x04
#define FIELD_DISK 0x05
#define FIELD_HEADER 0x06
#define FIELD_PROC 0x07
#define FIELD_LLM 0x08
#define FIELD_LLM_MODELS 0x09
#define FIELD_LLM_PROFILES 0x0A

#define PROC_KIND_CPU 0
#define PROC_KIND_RAM 1
#define PROC_KIND_GPU 2
#define PROC_KIND_DISK_RD 3
#define PROC_KIND_DISK_WR 4

#define LLM_STATUS_IDLE 0
#define LLM_STATUS_RUNNING 1
#define LLM_STATUS_STARTING 2
#define LLM_STATUS_OFFLINE 255

#define CMD_STOP_ALL 0x01
#define CMD_START_FAVORITE 0x02

extern volatile uint32_t g_epoch_sec;
extern volatile uint32_t g_uptime_sec;
extern char g_hostname[24];

void protocol_init();
void protocol_poll();  // call from loop()
void protocol_send_cmd(uint8_t cmd_id);
void protocol_send_start_model(const char *model_id);
void protocol_send_start_model_profile(const char *model_id, const char *profile);
void protocol_send_get_profiles(const char *model_id);