#include "uart_service_port.h"

#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "at_service.h"
#include "at_service_port.h"
#include "ring_buffer.h"

#define SHELL_INTERFACE_NAME "shell"
#define MQTT_INTERFACE_NAME  "mqtt"
#define SHELL_PRINTF_BUFFER_SIZE 1024U
#define MQTT_PRINTF_BUFFER_SIZE  (1024U - 2U)
#define MQTT_PREFIX_SIZE         2U

static uint8_t ota_parse_buffer[2048U];
static ring_buffer_t ota_data_ring;
static uint8_t shell_printf_buffer[SHELL_PRINTF_BUFFER_SIZE];
static uint8_t mqtt_printf_buffer[MQTT_PRINTF_BUFFER_SIZE];
static uint8_t mqtt_send_buffer[MQTT_PRINTF_BUFFER_SIZE + MQTT_PREFIX_SIZE];
static uint8_t ota_ack_buffer[128U];

void uart_service_port_init_ring_buffer(void)
{
  ring_buffer_init(&ota_data_ring,
                   ota_parse_buffer,
                   (uint16_t)sizeof(ota_parse_buffer));
}

uart_service_status_t shell_interface_send(const uint8_t *data, uint16_t len)
{
  if ((data == NULL) || (len == 0U))
  {
    return UART_SERVICE_ERR_PARAM;
  }

  return uart_service_send_by_name(SHELL_INTERFACE_NAME, data, len);
}

uart_service_status_t mqtt_interface_send(const uint8_t *data, uint16_t len)
{
  if ((data == NULL) || (len == 0U) || ((uint32_t)len > (sizeof(mqtt_send_buffer) - MQTT_PREFIX_SIZE)))
  {
    return UART_SERVICE_ERR_PARAM;
  }

  mqtt_send_buffer[0] = '1';
  mqtt_send_buffer[1] = ',';
  (void)memcpy(&mqtt_send_buffer[MQTT_PREFIX_SIZE], data, len);

  return uart_service_send_by_name(MQTT_INTERFACE_NAME, mqtt_send_buffer, (uint16_t)(len + MQTT_PREFIX_SIZE));
}

static uint8_t uart_port_handle_at_command(const uint8_t *data, uint16_t len, uint8_t mqtt_reply)
{
  int32_t ret;

  if ((data == NULL) || (len == 0U))
  {
    return 0U;
  }

  ret = at_service_port_process(data,
                                len,
                                (mqtt_reply != 0U) ? mqtt_interface_send : shell_interface_send);
  if (ret == AT_SERVICE_PREFIX_NOT_MATCH)
  {
    return 0U;
  }

  if ((ret != AT_SERVICE_OK) && (ret != AT_SERVICE_ACTION_EXECUTION_FAILED))
  {
    if (mqtt_reply != 0U)
    {
      (void)at_service_send_error(mqtt_interface_send, "craner");
    }
    else
    {
      (void)at_service_send_error(shell_interface_send, "craner");
    }
  }

  return 1U;
}

uart_service_status_t shell_interface_printf(const char *format, ...)
{
  va_list args;
  int len;

  if (format == NULL)
  {
    return UART_SERVICE_ERR_PARAM;
  }

  va_start(args, format);
  len = vsnprintf((char *)shell_printf_buffer, sizeof(shell_printf_buffer), format, args);
  va_end(args);

  if (len <= 0)
  {
    return UART_SERVICE_ERR_PARAM;
  }

  if ((uint32_t)len >= sizeof(shell_printf_buffer))
  {
    len = (int)sizeof(shell_printf_buffer) - 1;
  }

  return shell_interface_send(shell_printf_buffer, (uint16_t)len);
}

uart_service_status_t mqtt_interface_printf(const char *format, ...)
{
  va_list args;
  int len;

  if (format == NULL)
  {
    return UART_SERVICE_ERR_PARAM;
  }

  va_start(args, format);
  len = vsnprintf((char *)mqtt_printf_buffer, sizeof(mqtt_printf_buffer), format, args);
  va_end(args);

  if (len <= 0)
  {
    return UART_SERVICE_ERR_PARAM;
  }

  if ((uint32_t)len >= sizeof(mqtt_printf_buffer))
  {
    len = (int)sizeof(mqtt_printf_buffer) - 1;
  }

  return mqtt_interface_send(mqtt_printf_buffer, (uint16_t)len);
}

uart_service_status_t ota_ack_send(const uint8_t *data, uint16_t len)
{
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

  return uart_service_send_by_name(MQTT_INTERFACE_NAME, ota_ack_buffer, (uint16_t)(len + MQTT_PREFIX_SIZE));
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

  if (uart_port_handle_at_command(data, len, 0U) != 0U)
  {
    return UART_SERVICE_OK;
  }

  return shell_interface_send(data, len);
}

uart_service_status_t mqtt_interface_recv_callback(uart_service_t *uart,
                                           const uint8_t *data,
                                           uint16_t len)
{
  const uint8_t *payload;
  uint16_t payload_len;
  uint16_t head;
  uint16_t first;

  (void)uart;

  if (data == NULL)
  {
    return UART_SERVICE_ERR_PARAM;
  }

  if ((len < 3U) || (data[1] != (uint8_t)','))
  {
    return UART_SERVICE_ERR_RECV;
  }

  payload = &data[2];
  payload_len = (uint16_t)(len - 2U);

  if (data[0] == (uint8_t)'1')
  {
    if (uart_port_handle_at_command(payload, payload_len, 1U) == 0U)
    {
      (void)shell_interface_printf("mqtt rx: %.*s", (int)payload_len, (const char *)payload);
    }

    return UART_SERVICE_OK;
  }

  if (data[0] != (uint8_t)'2')
  {
    return UART_SERVICE_ERR_RECV;
  }

  head = ota_data_ring.head;
  first = (uint16_t)(ota_data_ring.size - head);
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
}

const uint8_t *uart_service_port_get_ota_read_ptr(uint16_t *len)
{
  return ring_buffer_get_read_ptr(&ota_data_ring, len);
}

void uart_service_port_consume_ota_data(uint16_t len)
{
  ring_buffer_update_read_ptr(&ota_data_ring, len);
}
