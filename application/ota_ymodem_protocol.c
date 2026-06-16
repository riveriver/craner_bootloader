#include "ota_ymodem_protocol.h"

#include <stddef.h>
#include <string.h>

#include "log_service.h"

#define YMODEM_SOH_DATA_SIZE 128U
#define YMODEM_STX_DATA_SIZE 1024U
#define YMODEM_DEFAULT_MAX_ERRORS 10U
#define YMODEM_CAN_COUNT_TO_ABORT 2U
#define YMODEM_DEFAULT_PACKET_TIMEOUT_MS 1000U

#define YMODEM_LOG_I(format, ...) log_printf("[I][YMODEM] " format "\r\n", ##__VA_ARGS__)
#define YMODEM_LOG_E(format, ...) log_printf("[E][YMODEM] " format "\r\n", ##__VA_ARGS__)

typedef enum
{
  YMODEM_PHASE_WAIT_FILE_INFO = 0,
  YMODEM_PHASE_WAIT_FILE_DATA,
  YMODEM_PHASE_WAIT_FINISH_PACKET,
  YMODEM_PHASE_DONE,
  YMODEM_PHASE_ABORTED,
} ymodem_phase_t;

typedef struct
{
  ota_ymodem_protocol_config_t config;
  ota_ymodem_rx_state_t rx_state;
  ymodem_phase_t phase;
  ota_ymodem_file_info_t file;
  uint8_t packet_header;
  uint8_t packet_seq;
  uint8_t expected_seq;
  uint8_t error_count;
  uint8_t can_count;
  uint8_t eot_count;
  uint16_t packet_size;
  uint16_t data_index;
  uint16_t received_crc;
  uint16_t timeout_data_index;
  uint32_t last_progress_ms;
  ota_ymodem_rx_state_t timeout_state;
  uint8_t data[OTA_YMODEM_PACKET_DATA_MAX];
} ota_ymodem_context_t;

static ota_ymodem_context_t g_ymodem;

static const char *ymodem_status_name(ota_ymodem_status_t status)
{
  switch (status)
  {
    case OTA_YMODEM_STATUS_OK:
      return "OK";
    case OTA_YMODEM_STATUS_ERR_PARAM:
      return "PARAM";
    case OTA_YMODEM_STATUS_ERR_SEND:
      return "SEND";
    case OTA_YMODEM_STATUS_ERR_SEQUENCE:
      return "SEQUENCE";
    case OTA_YMODEM_STATUS_ERR_CRC:
      return "CRC";
    case OTA_YMODEM_STATUS_ERR_PACKET:
      return "PACKET";
    case OTA_YMODEM_STATUS_ERR_CALLBACK:
      return "CALLBACK";
    case OTA_YMODEM_STATUS_ERR_ABORTED:
      return "ABORTED";
    case OTA_YMODEM_STATUS_ERR_TIMEOUT:
      return "TIMEOUT";
    default:
      return "UNKNOWN";
  }
}

static const char *ymodem_rx_state_name(ota_ymodem_rx_state_t state)
{
  switch (state)
  {
    case OTA_YMODEM_RX_IDLE:
      return "IDLE";
    case OTA_YMODEM_RX_FIND_HEADER:
      return "FIND_HEADER";
    case OTA_YMODEM_RX_READ_PACKET_SEQ:
      return "READ_SEQ";
    case OTA_YMODEM_RX_READ_PACKET_SEQ_INV:
      return "READ_SEQ_INV";
    case OTA_YMODEM_RX_RECV_FILE_INFO:
      return "RECV_FILE_INFO";
    case OTA_YMODEM_RX_RECV_FILE_DATA:
      return "RECV_FILE_DATA";
    case OTA_YMODEM_RX_RECV_FINISH_PACKET:
      return "RECV_FINISH";
    case OTA_YMODEM_RX_DONE:
      return "DONE";
    case OTA_YMODEM_RX_ABORTED:
      return "ABORTED";
    default:
      return "UNKNOWN";
  }
}

static uint8_t ymodem_max_errors(void)
{
  return (g_ymodem.config.max_errors == 0U) ? YMODEM_DEFAULT_MAX_ERRORS
                                             : g_ymodem.config.max_errors;
}

static uint32_t ymodem_packet_timeout_ms(void)
{
  return (g_ymodem.config.packet_timeout_ms == 0U) ? YMODEM_DEFAULT_PACKET_TIMEOUT_MS
                                                    : g_ymodem.config.packet_timeout_ms;
}

static void ymodem_reset_session(void)
{
  ota_ymodem_protocol_config_t config = g_ymodem.config;

  (void)memset(&g_ymodem, 0, sizeof(g_ymodem));
  g_ymodem.config = config;
  g_ymodem.rx_state = OTA_YMODEM_RX_IDLE;
  g_ymodem.phase = YMODEM_PHASE_WAIT_FILE_INFO;
}

static ota_ymodem_status_t ymodem_send_bytes(const uint8_t *data, uint16_t len)
{
  if ((data == NULL) || (len == 0U))
  {
    YMODEM_LOG_E("send param invalid: data=%p len=%u",
                 (void *)data,
                 (unsigned int)len);
    return OTA_YMODEM_STATUS_ERR_PARAM;
  }

  if (g_ymodem.config.send == NULL)
  {
    YMODEM_LOG_E("send callback missing");
    return OTA_YMODEM_STATUS_ERR_SEND;
  }

  if (g_ymodem.config.send(data, len, g_ymodem.config.user) != 0)
  {
    YMODEM_LOG_E("send callback failed: len=%u", (unsigned int)len);
    return OTA_YMODEM_STATUS_ERR_SEND;
  }

  return OTA_YMODEM_STATUS_OK;
}

static ota_ymodem_status_t ymodem_send_control(uint8_t control)
{
  return ymodem_send_bytes(&control, 1U);
}

static ota_ymodem_status_t ymodem_send_abort(ota_ymodem_status_t reason, uint8_t send_cancel)
{
  static const uint8_t cancel[2] = {OTA_YMODEM_CAN, OTA_YMODEM_CAN};

  YMODEM_LOG_E("abort: reason=%s(%d) send_cancel=%u state=%s errors=%u",
               ymodem_status_name(reason),
               (int)reason,
               (unsigned int)send_cancel,
               ymodem_rx_state_name(g_ymodem.rx_state),
               (unsigned int)g_ymodem.error_count);

  if (send_cancel != 0U)
  {
    (void)ymodem_send_bytes(cancel, (uint16_t)sizeof(cancel));
  }

  g_ymodem.rx_state = OTA_YMODEM_RX_ABORTED;
  g_ymodem.phase = YMODEM_PHASE_ABORTED;

  if (g_ymodem.config.on_abort != NULL)
  {
    g_ymodem.config.on_abort(reason, g_ymodem.config.user);
  }

  return reason;
}

static uint16_t get_ymodem_crc16(const uint8_t *data, uint16_t len)
{
  uint16_t crc = 0U;

  for (uint16_t i = 0U; i < len; ++i)
  {
    crc ^= (uint16_t)data[i] << 8U;

    for (uint8_t bit = 0U; bit < 8U; ++bit)
    {
      if ((crc & 0x8000U) != 0U)
      {
        crc = (uint16_t)((crc << 1U) ^ 0x1021U);
      }
      else
      {
        crc = (uint16_t)(crc << 1U);
      }
    }
  }

  return crc;
}

static uint8_t ymodem_is_digit(uint8_t value)
{
  return (uint8_t)((value >= (uint8_t)'0') && (value <= (uint8_t)'9'));
}

static ota_ymodem_status_t ymodem_reject_packet(ota_ymodem_status_t reason)
{
  ++g_ymodem.error_count;
  YMODEM_LOG_E("reject packet: reason=%s(%d) count=%u/%u state=%s seq=%u expect=%u",
               ymodem_status_name(reason),
               (int)reason,
               (unsigned int)g_ymodem.error_count,
               (unsigned int)ymodem_max_errors(),
               ymodem_rx_state_name(g_ymodem.rx_state),
               (unsigned int)g_ymodem.packet_seq,
               (unsigned int)g_ymodem.expected_seq);

  if (g_ymodem.error_count >= ymodem_max_errors())
  {
    return ymodem_send_abort(reason, 1U);
  }

  g_ymodem.rx_state = OTA_YMODEM_RX_FIND_HEADER;
  g_ymodem.data_index = 0U;
  return ymodem_send_control(OTA_YMODEM_NAK);
}

static uint8_t ymodem_state_needs_timeout(ota_ymodem_rx_state_t state)
{
  return (uint8_t)((state == OTA_YMODEM_RX_READ_PACKET_SEQ) ||
                   (state == OTA_YMODEM_RX_READ_PACKET_SEQ_INV) ||
                   (state == OTA_YMODEM_RX_RECV_FILE_INFO) ||
                   (state == OTA_YMODEM_RX_RECV_FILE_DATA) ||
                   (state == OTA_YMODEM_RX_RECV_FINISH_PACKET));
}

static ota_ymodem_status_t ymodem_accept_file_info_packet(void)
{
  static const uint8_t ack_and_crc_request[2] = {OTA_YMODEM_ACK, OTA_YMODEM_CRC_REQUEST};
  uint16_t name_len = 0U;
  uint16_t copy_len;
  uint16_t pos;
  uint32_t size = 0U;
  uint8_t size_valid = 0U;

  if (g_ymodem.packet_seq != 0U)
  {
    YMODEM_LOG_E("file info seq invalid: seq=%u", (unsigned int)g_ymodem.packet_seq);
    return ymodem_reject_packet(OTA_YMODEM_STATUS_ERR_SEQUENCE);
  }

  (void)memset(&g_ymodem.file, 0, sizeof(g_ymodem.file));

  if (g_ymodem.data[0] == 0U)
  {
    g_ymodem.phase = YMODEM_PHASE_DONE;
    g_ymodem.rx_state = OTA_YMODEM_RX_DONE;
    YMODEM_LOG_I("empty file info packet, transfer done");
    return ymodem_send_control(OTA_YMODEM_ACK);
  }

  while ((name_len < g_ymodem.packet_size) && (g_ymodem.data[name_len] != 0U))
  {
    ++name_len;
  }

  if (name_len >= g_ymodem.packet_size)
  {
    YMODEM_LOG_E("file info invalid: missing name terminator");
    return ymodem_reject_packet(OTA_YMODEM_STATUS_ERR_PACKET);
  }

  copy_len = name_len;
  if (copy_len >= OTA_YMODEM_FILE_NAME_MAX)
  {
    copy_len = OTA_YMODEM_FILE_NAME_MAX - 1U;
  }

  if (copy_len > 0U)
  {
    (void)memcpy(g_ymodem.file.name, g_ymodem.data, copy_len);
  }
  g_ymodem.file.name[copy_len] = '\0';

  pos = (uint16_t)(name_len + 1U);
  while ((pos < g_ymodem.packet_size) && (g_ymodem.data[pos] == (uint8_t)' '))
  {
    ++pos;
  }

  while ((pos < g_ymodem.packet_size) && (ymodem_is_digit(g_ymodem.data[pos]) != 0U))
  {
    uint32_t digit = (uint32_t)(g_ymodem.data[pos] - (uint8_t)'0');

    if (size <= ((UINT32_MAX - digit) / 10U))
    {
      size = (size * 10U) + digit;
    }
    else
    {
      size = UINT32_MAX;
    }

    size_valid = 1U;
    ++pos;
  }

  g_ymodem.file.size = size;
  g_ymodem.file.size_valid = size_valid;
  g_ymodem.file.bytes_received = 0U;

  YMODEM_LOG_I("file info: name=%s size=%lu size_valid=%u",
               g_ymodem.file.name,
               (unsigned long)g_ymodem.file.size,
               (unsigned int)g_ymodem.file.size_valid);

  if (g_ymodem.config.on_file_begin != NULL)
  {
    if (g_ymodem.config.on_file_begin(&g_ymodem.file, g_ymodem.config.user) != 0)
    {
      YMODEM_LOG_E("file begin callback failed");
      return ymodem_send_abort(OTA_YMODEM_STATUS_ERR_CALLBACK, 1U);
    }
  }

  g_ymodem.expected_seq = 1U;
  g_ymodem.phase = YMODEM_PHASE_WAIT_FILE_DATA;
  g_ymodem.rx_state = OTA_YMODEM_RX_FIND_HEADER;
  g_ymodem.data_index = 0U;
  g_ymodem.eot_count = 0U;

  return ymodem_send_bytes(ack_and_crc_request, (uint16_t)sizeof(ack_and_crc_request));
}

static ota_ymodem_status_t ymodem_accept_data_packet(void)
{
  static const uint8_t ack_and_crc_request[2] = {OTA_YMODEM_ACK, OTA_YMODEM_CRC_REQUEST};
  uint8_t previous_seq = (uint8_t)(g_ymodem.expected_seq - 1U);
  uint32_t remaining;
  uint16_t payload_len;

  if (g_ymodem.packet_seq == previous_seq)
  {
    YMODEM_LOG_I("duplicate packet ACK: seq=%u", (unsigned int)g_ymodem.packet_seq);
    g_ymodem.rx_state = OTA_YMODEM_RX_FIND_HEADER;
    g_ymodem.data_index = 0U;

    if ((previous_seq == 0U) && (g_ymodem.file.bytes_received == 0U))
    {
      return ymodem_send_bytes(ack_and_crc_request, (uint16_t)sizeof(ack_and_crc_request));
    }

    return ymodem_send_control(OTA_YMODEM_ACK);
  }

  if (g_ymodem.packet_seq != g_ymodem.expected_seq)
  {
    YMODEM_LOG_E("data seq invalid: seq=%u expect=%u",
                 (unsigned int)g_ymodem.packet_seq,
                 (unsigned int)g_ymodem.expected_seq);
    return ymodem_reject_packet(OTA_YMODEM_STATUS_ERR_SEQUENCE);
  }

  if (g_ymodem.file.size_valid == 0U)
  {
    payload_len = g_ymodem.packet_size;
  }
  else if (g_ymodem.file.bytes_received >= g_ymodem.file.size)
  {
    payload_len = 0U;
  }
  else
  {
    remaining = g_ymodem.file.size - g_ymodem.file.bytes_received;
    if (remaining > (uint32_t)g_ymodem.packet_size)
    {
      payload_len = g_ymodem.packet_size;
    }
    else
    {
      payload_len = (uint16_t)remaining;
    }
  }

  if ((payload_len > 0U) && (g_ymodem.config.on_file_data != NULL))
  {
    if (g_ymodem.config.on_file_data(g_ymodem.data,
                                     payload_len,
                                     g_ymodem.file.bytes_received,
                                     g_ymodem.config.user) != 0)
    {
      YMODEM_LOG_E("file data callback failed: offset=%lu len=%u",
                   (unsigned long)g_ymodem.file.bytes_received,
                   (unsigned int)payload_len);
      return ymodem_send_abort(OTA_YMODEM_STATUS_ERR_CALLBACK, 1U);
    }
  }

  g_ymodem.file.bytes_received += payload_len;
  g_ymodem.expected_seq = (uint8_t)(g_ymodem.expected_seq + 1U);
  g_ymodem.rx_state = OTA_YMODEM_RX_FIND_HEADER;
  g_ymodem.data_index = 0U;

  return ymodem_send_control(OTA_YMODEM_ACK);
}

static ota_ymodem_status_t ymodem_accept_finish_packet(void)
{
  if (g_ymodem.packet_seq != 0U)
  {
    YMODEM_LOG_E("finish packet seq invalid: seq=%u", (unsigned int)g_ymodem.packet_seq);
    return ymodem_reject_packet(OTA_YMODEM_STATUS_ERR_SEQUENCE);
  }

  if (g_ymodem.data[0] != 0U)
  {
    YMODEM_LOG_E("finish packet invalid: first_byte=0x%02x", (unsigned int)g_ymodem.data[0]);
    return ymodem_reject_packet(OTA_YMODEM_STATUS_ERR_PACKET);
  }

  g_ymodem.phase = YMODEM_PHASE_DONE;
  g_ymodem.rx_state = OTA_YMODEM_RX_DONE;

  if (ymodem_send_control(OTA_YMODEM_ACK) != OTA_YMODEM_STATUS_OK)
  {
    YMODEM_LOG_E("finish packet ACK send failed");
    return OTA_YMODEM_STATUS_ERR_SEND;
  }

  if (g_ymodem.config.on_file_end != NULL)
  {
    YMODEM_LOG_I("finish packet accepted, call file end");
    g_ymodem.config.on_file_end(&g_ymodem.file, g_ymodem.config.user);
  }

  return OTA_YMODEM_STATUS_OK;
}

static ota_ymodem_status_t ymodem_finish_packet(ota_ymodem_status_t (*accept_packet)(void))
{
  uint16_t calc_crc = get_ymodem_crc16(g_ymodem.data, g_ymodem.packet_size);

  if (calc_crc != g_ymodem.received_crc)
  {
    YMODEM_LOG_E("CRC mismatch: seq=%u calc=0x%04x recv=0x%04x",
                 (unsigned int)g_ymodem.packet_seq,
                 (unsigned int)calc_crc,
                 (unsigned int)g_ymodem.received_crc);
    return ymodem_reject_packet(OTA_YMODEM_STATUS_ERR_CRC);
  }

  g_ymodem.error_count = 0U;
  g_ymodem.can_count = 0U;

  if (accept_packet == NULL)
  {
    return ymodem_reject_packet(OTA_YMODEM_STATUS_ERR_PACKET);
  }

  return accept_packet();
}

static ota_ymodem_status_t ymodem_receive_packet_byte(uint8_t byte,
                                                      ota_ymodem_status_t (*accept_packet)(void))
{
  if (g_ymodem.data_index < g_ymodem.packet_size)
  {
    g_ymodem.data[g_ymodem.data_index] = byte;
    ++g_ymodem.data_index;
    return OTA_YMODEM_STATUS_OK;
  }

  if (g_ymodem.data_index == g_ymodem.packet_size)
  {
    g_ymodem.received_crc = (uint16_t)byte << 8U;
    ++g_ymodem.data_index;
    return OTA_YMODEM_STATUS_OK;
  }

  g_ymodem.received_crc |= byte;
  return ymodem_finish_packet(accept_packet);
}

static ota_ymodem_status_t ymodem_handle_eot(void)
{
  static const uint8_t ack_and_crc_request[2] = {OTA_YMODEM_ACK, OTA_YMODEM_CRC_REQUEST};

  if (g_ymodem.phase != YMODEM_PHASE_WAIT_FILE_DATA)
  {
    YMODEM_LOG_E("EOT in invalid phase=%u", (unsigned int)g_ymodem.phase);
    return ymodem_reject_packet(OTA_YMODEM_STATUS_ERR_PACKET);
  }

  if (g_ymodem.eot_count == 0U)
  {
    g_ymodem.eot_count = 1U;
    YMODEM_LOG_I("first EOT received, send NAK");
    return ymodem_send_control(OTA_YMODEM_NAK);
  }

  g_ymodem.eot_count = 0U;
  g_ymodem.phase = YMODEM_PHASE_WAIT_FINISH_PACKET;
  g_ymodem.rx_state = OTA_YMODEM_RX_FIND_HEADER;

  YMODEM_LOG_I("second EOT received, wait finish packet");
  return ymodem_send_bytes(ack_and_crc_request, (uint16_t)sizeof(ack_and_crc_request));
}

void ota_ymodem_protocol_init(const ota_ymodem_protocol_config_t *config)
{
  (void)memset(&g_ymodem, 0, sizeof(g_ymodem));

  if (config != NULL)
  {
    g_ymodem.config = *config;
  }

  g_ymodem.rx_state = OTA_YMODEM_RX_IDLE;
  g_ymodem.phase = YMODEM_PHASE_WAIT_FILE_INFO;
  YMODEM_LOG_I("protocol init");
}

ota_ymodem_status_t ota_ymodem_protocol_start_receive(void)
{
  ymodem_reset_session();

  if (g_ymodem.config.send == NULL)
  {
    YMODEM_LOG_E("start receive failed: send callback missing");
    return OTA_YMODEM_STATUS_ERR_SEND;
  }

  g_ymodem.rx_state = OTA_YMODEM_RX_FIND_HEADER;
  g_ymodem.phase = YMODEM_PHASE_WAIT_FILE_INFO;
  g_ymodem.expected_seq = 0U;

  YMODEM_LOG_I("start receive, send CRC request");
  return ymodem_send_control(OTA_YMODEM_CRC_REQUEST);
}

void ota_ymodem_protocol_reset(void)
{
  ymodem_reset_session();
  YMODEM_LOG_I("protocol reset");
}

ota_ymodem_status_t ota_ymodem_protocol_input_byte(uint8_t byte)
{
  if ((g_ymodem.rx_state == OTA_YMODEM_RX_IDLE) ||
      (g_ymodem.rx_state == OTA_YMODEM_RX_DONE) ||
      (g_ymodem.rx_state == OTA_YMODEM_RX_ABORTED))
  {
    return OTA_YMODEM_STATUS_OK;
  }

  switch (g_ymodem.rx_state)
  {
    case OTA_YMODEM_RX_FIND_HEADER:
      if (byte == OTA_YMODEM_CAN)
      {
        ++g_ymodem.can_count;
        if (g_ymodem.can_count >= YMODEM_CAN_COUNT_TO_ABORT)
        {
          YMODEM_LOG_E("host cancelled transfer");
          return ymodem_send_abort(OTA_YMODEM_STATUS_ERR_ABORTED, 0U);
        }
        return OTA_YMODEM_STATUS_OK;
      }

      g_ymodem.can_count = 0U;

      if ((byte == OTA_YMODEM_SOH) || (byte == OTA_YMODEM_STX))
      {
        g_ymodem.packet_header = byte;
        g_ymodem.packet_size = (byte == OTA_YMODEM_SOH) ? YMODEM_SOH_DATA_SIZE
                                                        : YMODEM_STX_DATA_SIZE;
        g_ymodem.packet_seq = 0U;
        g_ymodem.data_index = 0U;
        g_ymodem.received_crc = 0U;
        g_ymodem.rx_state = OTA_YMODEM_RX_READ_PACKET_SEQ;
      }
      else if (byte == OTA_YMODEM_EOT)
      {
        return ymodem_handle_eot();
      }
      else
      {
        /* Ignore noise until a frame header is found. */
      }
      break;

    case OTA_YMODEM_RX_READ_PACKET_SEQ:
      g_ymodem.packet_seq = byte;
      g_ymodem.rx_state = OTA_YMODEM_RX_READ_PACKET_SEQ_INV;
      break;

    case OTA_YMODEM_RX_READ_PACKET_SEQ_INV:
      if ((uint8_t)(g_ymodem.packet_seq + byte) != 0xFFU)
      {
        g_ymodem.rx_state = OTA_YMODEM_RX_FIND_HEADER;
        g_ymodem.data_index = 0U;
        ++g_ymodem.error_count;
        YMODEM_LOG_E("seq inverse invalid: seq=%u inv=%u count=%u/%u",
                     (unsigned int)g_ymodem.packet_seq,
                     (unsigned int)byte,
                     (unsigned int)g_ymodem.error_count,
                     (unsigned int)ymodem_max_errors());

        if (g_ymodem.error_count >= ymodem_max_errors())
        {
          return ymodem_send_abort(OTA_YMODEM_STATUS_ERR_SEQUENCE, 1U);
        }

        return ymodem_send_control(OTA_YMODEM_NAK);
      }

      if (g_ymodem.phase == YMODEM_PHASE_WAIT_FILE_INFO)
      {
        g_ymodem.rx_state = OTA_YMODEM_RX_RECV_FILE_INFO;
      }
      else if (g_ymodem.phase == YMODEM_PHASE_WAIT_FILE_DATA)
      {
        g_ymodem.rx_state = OTA_YMODEM_RX_RECV_FILE_DATA;
      }
      else if (g_ymodem.phase == YMODEM_PHASE_WAIT_FINISH_PACKET)
      {
        g_ymodem.rx_state = OTA_YMODEM_RX_RECV_FINISH_PACKET;
      }
      else
      {
        return ymodem_reject_packet(OTA_YMODEM_STATUS_ERR_PACKET);
      }

      g_ymodem.data_index = 0U;
      break;

    case OTA_YMODEM_RX_RECV_FILE_INFO:
      return ymodem_receive_packet_byte(byte, ymodem_accept_file_info_packet);

    case OTA_YMODEM_RX_RECV_FILE_DATA:
      return ymodem_receive_packet_byte(byte, ymodem_accept_data_packet);

    case OTA_YMODEM_RX_RECV_FINISH_PACKET:
      return ymodem_receive_packet_byte(byte, ymodem_accept_finish_packet);

    case OTA_YMODEM_RX_IDLE:
    case OTA_YMODEM_RX_DONE:
    case OTA_YMODEM_RX_ABORTED:
    default:
      break;
  }

  return OTA_YMODEM_STATUS_OK;
}

ota_ymodem_status_t ota_ymodem_protocol_tick(uint32_t now_ms)
{
  if (ymodem_state_needs_timeout(g_ymodem.rx_state) == 0U)
  {
    g_ymodem.timeout_state = g_ymodem.rx_state;
    g_ymodem.timeout_data_index = g_ymodem.data_index;
    g_ymodem.last_progress_ms = now_ms;
    return OTA_YMODEM_STATUS_OK;
  }

  if ((g_ymodem.timeout_state != g_ymodem.rx_state) ||
      (g_ymodem.timeout_data_index != g_ymodem.data_index))
  {
    g_ymodem.timeout_state = g_ymodem.rx_state;
    g_ymodem.timeout_data_index = g_ymodem.data_index;
    g_ymodem.last_progress_ms = now_ms;
    return OTA_YMODEM_STATUS_OK;
  }

  if ((uint32_t)(now_ms - g_ymodem.last_progress_ms) < ymodem_packet_timeout_ms())
  {
    return OTA_YMODEM_STATUS_OK;
  }

  YMODEM_LOG_E("packet timeout: state=%s data_index=%u timeout=%lu",
               ymodem_rx_state_name(g_ymodem.rx_state),
               (unsigned int)g_ymodem.data_index,
               (unsigned long)ymodem_packet_timeout_ms());
  return ymodem_reject_packet(OTA_YMODEM_STATUS_ERR_TIMEOUT);
}

ota_ymodem_rx_state_t ota_ymodem_protocol_get_state(void)
{
  return g_ymodem.rx_state;
}

const ota_ymodem_file_info_t *ota_ymodem_protocol_get_file_info(void)
{
  return &g_ymodem.file;
}

uint8_t ota_ymodem_protocol_is_busy(void)
{
  return (uint8_t)((g_ymodem.rx_state != OTA_YMODEM_RX_IDLE) &&
                   (g_ymodem.rx_state != OTA_YMODEM_RX_DONE) &&
                   (g_ymodem.rx_state != OTA_YMODEM_RX_ABORTED));
}

uint8_t ota_ymodem_protocol_is_done(void)
{
  return (uint8_t)(g_ymodem.rx_state == OTA_YMODEM_RX_DONE);
}
