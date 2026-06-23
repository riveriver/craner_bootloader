#include "uart_service_port.h"

#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "ring_buffer.h"

#define SHELL_INTERFACE_NAME "shell"
#define MQTT_INTERFACE_NAME  "mqtt"
#define SHELL_PRINTF_BUFFER_SIZE 1024U
#define MQTT_PRINTF_BUFFER_SIZE  (1024U - 2U)
#define MQTT_PREFIX_SIZE         2U
#define UART_SEND_RETRY_TIMEOUT_MS 20UL

static uint8_t ota_parse_buffer[2048U];
static ring_buffer_t ota_data_ring;

static uart_service_status_t uart_service_send_by_name_with_retry(const char *name,
                                                                  const uint8_t *data,
                                                                  uint16_t len)
{
  uart_service_status_t ret;
  uint32_t start_ms;

  if ((name == NULL) || (data == NULL) || (len == 0U))
  {
    return UART_SERVICE_ERR_PARAM;
  }

  start_ms = HAL_GetTick();

  do
  {
    ret = uart_service_send_by_name(name, data, len);
    if (ret != UART_SERVICE_ERR_BUSY)
    {
      return ret;
    }
  } while ((uint32_t)(HAL_GetTick() - start_ms) < UART_SEND_RETRY_TIMEOUT_MS);

  return ret;
}

void uart_service_port_init_ring_buffer(void)
{
  ring_buffer_init(&ota_data_ring,
                   ota_parse_buffer,
                   (uint16_t)sizeof(ota_parse_buffer));
}

int32_t shell_interface_send(const uint8_t *data, uint16_t len)
{
  if ((data == NULL) || (len == 0U))
  {
    return (int32_t)UART_SERVICE_ERR_PARAM;
  }

  return (int32_t)uart_service_send_by_name_with_retry(SHELL_INTERFACE_NAME, data, len);
}

int32_t mqtt_interface_send(const uint8_t *data, uint16_t len)
{
  uint8_t mqtt_send_buffer[256U];

  if ((data == NULL) || (len == 0U) || ((uint32_t)len > (sizeof(mqtt_send_buffer) - MQTT_PREFIX_SIZE)))
  {
    return (int32_t)UART_SERVICE_ERR_PARAM;
  }

  mqtt_send_buffer[0] = '1';
  mqtt_send_buffer[1] = ',';
  (void)memcpy(&mqtt_send_buffer[MQTT_PREFIX_SIZE], data, len);

  return (int32_t)uart_service_send_by_name_with_retry(MQTT_INTERFACE_NAME,
                                                       mqtt_send_buffer,
                                                       (uint16_t)(len + MQTT_PREFIX_SIZE));
}

uart_service_status_t shell_interface_printf(const char *format, ...)
{
  char buffer[256U];
  va_list args;
  int len;

  if (format == NULL)
  {
    return UART_SERVICE_ERR_PARAM;
  }

  va_start(args, format);
  len = vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);

  if (len <= 0)
  {
    return UART_SERVICE_ERR_PARAM;
  }

  if ((uint32_t)len >= sizeof(buffer))
  {
    len = (int)sizeof(buffer) - 1;
  }

  return uart_service_send_by_name_with_retry(SHELL_INTERFACE_NAME, (const uint8_t *)buffer, (uint16_t)len);
}

uart_service_status_t mqtt_interface_printf(const char *format, ...)
{
  char buffer[256U];
  va_list args;
  int len;

  if (format == NULL)
  {
    return UART_SERVICE_ERR_PARAM;
  }

  buffer[0] = '1';
  buffer[1] = ',';

  va_start(args, format);
  len = vsnprintf(&buffer[2], sizeof(buffer) - 2U, format, args);
  va_end(args);

  if (len <= 0)
  {
    return UART_SERVICE_ERR_PARAM;
  }

  if ((uint32_t)len >= (sizeof(buffer) - 2U))
  {
    len = (int)sizeof(buffer) - 3;
  }

  return uart_service_send_by_name_with_retry(MQTT_INTERFACE_NAME, (const uint8_t *)buffer, (uint16_t)(len + 2));
}

uart_service_status_t ota_ack_send(const uint8_t *data, uint16_t len)
{
  uint8_t ota_ack_buffer[256U];

  if ((data == NULL) || (len == 0U))
  {
    return UART_SERVICE_ERR_PARAM;
  }

  if ((uint32_t)len > (sizeof(ota_ack_buffer) - MQTT_PREFIX_SIZE))
  {
    return UART_SERVICE_ERR_PARAM;
  }

  ota_ack_buffer[0] = '2';
  ota_ack_buffer[1] = ',';
  (void)memcpy(&ota_ack_buffer[MQTT_PREFIX_SIZE], data, len);

  return uart_service_send_by_name_with_retry(MQTT_INTERFACE_NAME,
                                              ota_ack_buffer,
                                              (uint16_t)(len + MQTT_PREFIX_SIZE));
}

uart_service_status_t shell_interface_recv_callback(uart_service_t *uart,
                                            const uint8_t *data,
                                            uint16_t len)
{
  (void)uart;

  if ((data == NULL) || (len == 0U))
  {
    return UART_SERVICE_ERR_PARAM;
  }

  return shell_interface_send(data, len);
}

uart_service_status_t mqtt_interface_recv_callback(uart_service_t *uart,
                                           const uint8_t *data,
                                           uint16_t len)
{
  if (data == NULL|| len == 0U)
  {
    return UART_SERVICE_ERR_PARAM;
  }

  if ((len < 3U) &&  (data[0] == (uint8_t)'1') && (data[1] != (uint8_t)','))
  {
    mqtt_interface_send(data, len);
    return UART_SERVICE_OK;
  }else if ((len >= 3U) && (data[0] == (uint8_t)'2') && (data[1] == (uint8_t)','))
  {
    const uint8_t *payload = &data[2];
    uint16_t payload_len = (uint16_t)(len - 2U);
    uint16_t head = ota_data_ring.head;
    uint16_t first = (uint16_t)(ota_data_ring.size - head);
    if (first > payload_len)
    {
      first = payload_len;
    }

    (void)memcpy(&ota_data_ring.data[head], payload, first);
    if (payload_len > first)
    {
      (void)memcpy(ota_data_ring.data, &payload[first], (uint16_t)(payload_len - first));
    }

    head = (uint16_t)(head + payload_len);
    if (head >= ota_data_ring.size)
    {
      head = (uint16_t)(head - ota_data_ring.size);
    }
    ring_buffer_update_write_ptr(&ota_data_ring, head);
    return UART_SERVICE_OK;
  }else
  {
    mqtt_interface_send(data, len);
    shell_interface_send(data, len);
    return UART_SERVICE_OK;
  }
  (void)uart;
}

const uint8_t *uart_service_port_get_ota_read_ptr(uint16_t *len)
{
  return ring_buffer_get_read_ptr(&ota_data_ring, len);
}

void uart_service_port_consume_ota_data(uint16_t len)
{
  ring_buffer_update_read_ptr(&ota_data_ring, len);
}
