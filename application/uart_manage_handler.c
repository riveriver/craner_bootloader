#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <uart_manage_port.h>

#include "at_protocol_handler.h"

#define SHELL_INTERFACE_NAME "shell"
#define MQTT_INTERFACE_NAME  "mqtt"
#define SHELL_PRINTF_BUFFER_SIZE 1024U
#define MQTT_PRINTF_BUFFER_SIZE  (1024U - 2U)
#define MQTT_PREFIX_SIZE         2U
/* retry brief time when UART is busy to reduce lost messages */
#define UART_SEND_RETRY_TIMEOUT_MS 20UL

static void mqtt_dump_payload_head(const uint8_t *data, uint16_t len)
{
  char line[96];
  int pos = 0;
  uint16_t head_len = len;

  if ((data == NULL) || (len == 0U))
  {
    return;
  }

  if (head_len > 8U)
  {
    head_len = 8U;
  }

  pos += snprintf(&line[pos], sizeof(line) - (uint32_t)pos, "[MQTT_RX] len=%u head:",
                  (unsigned int)len);
  for (uint16_t i = 0U; i < head_len; ++i)
  {
    pos += snprintf(&line[pos], sizeof(line) - (uint32_t)pos, " %02X", (unsigned int)data[i]);
  }
  (void)snprintf(&line[pos], sizeof(line) - (uint32_t)pos, "\r\n");
  (void)shell_interface_send((const uint8_t *)line, (uint16_t)strlen(line));
}

int32_t shell_interface_send(const uint8_t *data, uint16_t len)
{
  if ((data == NULL) || (len == 0U))
  {
    return -1;
  }
  return (int32_t)uart_manage_dma_send_by_name(SHELL_INTERFACE_NAME, data, len);
}

int32_t shell_interface_printf(const char *format, ...)
{
  char buffer[256U];
  va_list args;
  int len;

  if (format == NULL)
  {
    return -1;
  }

  va_start(args, format);
  len = vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);

  if (len <= 0)
  {
    return -1;
  }

  if ((uint32_t)len >= sizeof(buffer))
  {
    len = (int)sizeof(buffer) - 1;
  }

  return (int)uart_manage_dma_send_by_name(SHELL_INTERFACE_NAME, (const uint8_t *)buffer, (uint16_t)len);
}

int32_t mqtt_system_inform_send(const uint8_t *data, uint16_t len)
{
  uint8_t buffer[256U];

  if ((data == NULL) || (len == 0U) || ((uint32_t)len > (sizeof(buffer) - MQTT_PREFIX_SIZE)))
  {
    return -1;
  }

  buffer[0] = '1';
  buffer[1] = ',';
  (void)memcpy(&buffer[MQTT_PREFIX_SIZE], data, len);

  return (int32_t)uart_manage_dma_send_by_name(MQTT_INTERFACE_NAME,
                                                       buffer,
                                                       (uint16_t)(len + MQTT_PREFIX_SIZE));
}

int32_t mqtt_system_inform_printf(const char *format, ...)
{
  char buffer[256U];
  va_list args;
  int len;

  if (format == NULL)
  {
    return -1;
  }

  buffer[0] = '1';
  buffer[1] = ',';

  va_start(args, format);
  len = vsnprintf(&buffer[2], sizeof(buffer) - 2U, format, args);
  va_end(args);

  if (len <= 0)
  {
    return -1;
  }

  if ((uint32_t)len >= (sizeof(buffer) - 2U))
  {
    len = (int)sizeof(buffer) - 3;
  }

  return (int)uart_manage_dma_send_by_name(MQTT_INTERFACE_NAME, (const uint8_t *)buffer, (uint16_t)(len + 2));
}

int32_t mqtt_ota_progress_printf(const uint8_t *data, uint16_t len)
{
  uint8_t buffer[256U];

  if ((data == NULL) || (len == 0U))
  {
    return -1;
  }

  if ((uint32_t)len > (sizeof(buffer) - MQTT_PREFIX_SIZE))
  {
    return -1;
  }

  buffer[0] = '2';
  buffer[1] = ',';
  (void)memcpy(&buffer[MQTT_PREFIX_SIZE], data, len);

  return (int)uart_manage_dma_send_by_name(MQTT_INTERFACE_NAME,
                                              buffer,
                                              (uint16_t)(len + MQTT_PREFIX_SIZE));
}

int32_t shell_interface_recv_callback(uint8_t *data, uint16_t len)
{
  int32_t ret;

  if ((data == NULL) || (len == 0U))
  {
    return 0U;
  }

  ret = craner_at_handler(data, len, shell_interface_send);
  if (ret != AT_PREFIX_NOT_MATCH)
  {
    return (ret < 0) ? ret : 0;
  }

  (void)shell_interface_send(data, len);
  return 0U;
}

int32_t mqtt_interface_recv_callback(uint8_t *data, uint16_t len)
{
  int32_t ret;
  if (data == NULL || len == 0U)
  {
    return 0U;
  }

  if ((len >= 3U) && (data[1] == (uint8_t)','))
  {
    if (data[0] == (uint8_t)'1')
    {
      ret = craner_at_handler(&data[2], (uint16_t)(len - 2U), mqtt_system_inform_send);
      if (ret != AT_PREFIX_NOT_MATCH)
      {
        return (ret < 0) ? ret : 0;
      }
    }
    else if (data[0] == (uint8_t)'2')
    {
      uint8_t *payload = &data[2];
      uint16_t payload_len = (uint16_t)(len - 2U);
      (void)uart_manage_write_to_recv_ring(uart_manage_get_obj_by_name(MQTT_INTERFACE_NAME), payload, payload_len);
      return 0U;
    }
  }

  (void)mqtt_system_inform_send(data, len);
  (void)shell_interface_send(data, len);
  return 0U;
}

const lwrb_t *uart_service_port_get_ota_ring(void)
{
  uart_inferface_t *m_obj = uart_manage_get_obj_by_name(MQTT_INTERFACE_NAME);

  if (m_obj == NULL)
  {
    return NULL;
  }

  return &m_obj->process_ring_buffer;
}
