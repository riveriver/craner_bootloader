#ifndef OTA_YMODEM_PROTOCOL_H
#define OTA_YMODEM_PROTOCOL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#ifndef OTA_YMODEM_FILE_NAME_MAX
#define OTA_YMODEM_FILE_NAME_MAX 64U
#endif

#ifndef OTA_YMODEM_PACKET_DATA_MAX
#define OTA_YMODEM_PACKET_DATA_MAX 1024U
#endif

#if OTA_YMODEM_PACKET_DATA_MAX < 1024U
#error "OTA_YMODEM_PACKET_DATA_MAX must be at least 1024 bytes"
#endif

#if OTA_YMODEM_FILE_NAME_MAX < 2U
#error "OTA_YMODEM_FILE_NAME_MAX must be at least 2 bytes"
#endif

#define OTA_YMODEM_SOH 0x01U
#define OTA_YMODEM_STX 0x02U
#define OTA_YMODEM_EOT 0x04U
#define OTA_YMODEM_ACK 0x06U
#define OTA_YMODEM_NAK 0x15U
#define OTA_YMODEM_CAN 0x18U
#define OTA_YMODEM_CRC_REQUEST 0x43U

typedef enum
{
  OTA_YMODEM_STATUS_OK = 0,
  OTA_YMODEM_STATUS_ERR_PARAM = -1,
  OTA_YMODEM_STATUS_ERR_SEND = -2,
  OTA_YMODEM_STATUS_ERR_SEQUENCE = -3,
  OTA_YMODEM_STATUS_ERR_CRC = -4,
  OTA_YMODEM_STATUS_ERR_PACKET = -5,
  OTA_YMODEM_STATUS_ERR_CALLBACK = -6,
  OTA_YMODEM_STATUS_ERR_ABORTED = -7,
  OTA_YMODEM_STATUS_ERR_TIMEOUT = -8,
} ota_ymodem_status_t;

typedef enum
{
  OTA_YMODEM_RX_IDLE = 0,
  OTA_YMODEM_RX_FIND_HEADER,
  OTA_YMODEM_RX_READ_PACKET_SEQ,
  OTA_YMODEM_RX_READ_PACKET_SEQ_INV,
  OTA_YMODEM_RX_RECV_FILE_INFO,
  OTA_YMODEM_RX_RECV_FILE_DATA,
  OTA_YMODEM_RX_RECV_FINISH_PACKET,
  OTA_YMODEM_RX_DONE,
  OTA_YMODEM_RX_ABORTED,
} ota_ymodem_rx_state_t;

typedef struct
{
  char name[OTA_YMODEM_FILE_NAME_MAX];
  uint32_t size;
  uint32_t bytes_received;
  uint8_t size_valid;
} ota_ymodem_file_info_t;

typedef int (*ota_ymodem_send_fn_t)(const uint8_t *data, uint16_t len, void *user);
typedef int (*ota_ymodem_file_begin_fn_t)(const ota_ymodem_file_info_t *file, void *user);
typedef int (*ota_ymodem_file_data_fn_t)(const uint8_t *data,
                                         uint16_t len,
                                         uint32_t offset,
                                         void *user);
typedef void (*ota_ymodem_file_end_fn_t)(const ota_ymodem_file_info_t *file, void *user);
typedef void (*ota_ymodem_abort_fn_t)(ota_ymodem_status_t reason, void *user);

typedef struct
{
  ota_ymodem_send_fn_t send;
  ota_ymodem_file_begin_fn_t on_file_begin;
  ota_ymodem_file_data_fn_t on_file_data;
  ota_ymodem_file_end_fn_t on_file_end;
  ota_ymodem_abort_fn_t on_abort;
  void *user;
  uint8_t max_errors;
  uint32_t packet_timeout_ms;
} ota_ymodem_protocol_config_t;

void ota_ymodem_protocol_init(const ota_ymodem_protocol_config_t *config);
ota_ymodem_status_t ota_ymodem_protocol_start_receive(void);
void ota_ymodem_protocol_reset(void);
ota_ymodem_status_t ota_ymodem_protocol_input_byte(uint8_t byte);
ota_ymodem_status_t ota_ymodem_protocol_tick(uint32_t now_ms);

ota_ymodem_rx_state_t ota_ymodem_protocol_get_state(void);
const ota_ymodem_file_info_t *ota_ymodem_protocol_get_file_info(void);
uint8_t ota_ymodem_protocol_is_busy(void);
uint8_t ota_ymodem_protocol_is_done(void);

#ifdef __cplusplus
}
#endif

#endif /* OTA_YMODEM_PROTOCOL_H */
