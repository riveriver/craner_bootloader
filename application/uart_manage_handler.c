#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <uart_manage_port.h>

#include "lwrb.h"

#define SHELL_INTERFACE_NAME "shell"
#define MQTT_INTERFACE_NAME  "mqtt"
#define SHELL_PRINTF_BUFFER_SIZE 1024U
#define MQTT_PRINTF_BUFFER_SIZE  (1024U - 2U)
#define MQTT_PREFIX_SIZE         2U
/* retry brief time when UART is busy to reduce lost messages */
#define UART_SEND_RETRY_TIMEOUT_MS 20UL

static uint8_t ota_parse_buffer[2048U];
static lwrb_t ota_data_ring;


void uart_service_port_init_ring_buffer(void)
{
  (void)lwrb_init(&ota_data_ring,
                  ota_parse_buffer,
                  (lwrb_sz_t)sizeof(ota_parse_buffer));
}

int32_t shell_interface_send(const uint8_t *data, uint16_t len)
{
  if ((data == NULL) || (len == 0U))
  {
    return -1;
  }
  return (int32_t)uart_manage_dma_send_by_name(SHELL_INTERFACE_NAME, data, len);
}

int32_t mqtt_interface_send(const uint8_t *data, uint16_t len)
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

int shell_interface_printf(const char *format, ...)
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

int mqtt_interface_printf(const char *format, ...)
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

int ota_ack_send(const uint8_t *data, uint16_t len)
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

uint32_t shell_interface_recv_callback(uint8_t *data, uint16_t len)
{
  (void)data;
  if ((data == NULL) || (len == 0U))
  {
    return 0U;
  }

  (void)shell_interface_send(data, len);
  return 0U;
}

uint32_t mqtt_interface_recv_callback(uint8_t *data, uint16_t len)
{
  if (data == NULL || len == 0U)
  {
    return 0U;
  }

  if ((len < 3U) && (data[0] == (uint8_t)'1') && (data[1] != (uint8_t)','))
  {
    (void)mqtt_interface_send(data, len);
    return 0U;
  }
  else if ((len >= 3U) && (data[0] == (uint8_t)'2') && (data[1] == (uint8_t)','))
  {
    const uint8_t *payload = &data[2];
    uint16_t payload_len = (uint16_t)(len - 2U);
    (void)lwrb_write(&ota_data_ring, payload, payload_len);
    return 0U;
  }
  else
  {
    (void)mqtt_interface_send(data, len);
    (void)shell_interface_send(data, len);
    return 0U;
  }
}

const lwrb_t *uart_service_port_get_ota_ring(void)
{
  return &ota_data_ring;
}
