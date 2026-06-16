#include "uart_service_port.h"

#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "main.h"
#include "log_service.h"
#include "ota_ymodem_protocol.h"

#define LOG_E(format, ...) log_printf("[E] " format "\r\n", ##__VA_ARGS__)

extern UART_HandleTypeDef huart5;
extern UART_HandleTypeDef huart1;

#define SHELL_INTERFACE_NAME "shell"
#define MQTT_INTERFACE_NAME "mqtt"
#define SHELL_INTERFACE_TX_BUFFER_SIZE 256U

uart_service_status_t shell_interface_printf(const char *format, ...)
{
  char buffer[SHELL_INTERFACE_TX_BUFFER_SIZE];
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

  return uart_service_send_by_name(SHELL_INTERFACE_NAME, (const uint8_t *)buffer, (uint16_t)len);
}

uart_service_status_t mqtt_interface_printf(const char *format, ...)
{
  char buffer[SHELL_INTERFACE_TX_BUFFER_SIZE];
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

  return uart_service_send_by_name(MQTT_INTERFACE_NAME, (const uint8_t *)buffer, (uint16_t)(len + 2));
}

static uart_service_status_t shell_interface_on_rx(uart_service_t *uart,
                                                   const uint8_t *data,
                                                   uint16_t len)
{
  (void)uart;

  if (data == NULL)
  {
    return UART_SERVICE_ERR_PARAM;
  }

  return uart_service_send_by_name(SHELL_INTERFACE_NAME, data, len);
}

static uart_service_status_t mqtt_interface_on_rx(uart_service_t *uart,
                                                 const uint8_t *data,
                                                 uint16_t len)
{
  (void)uart;
  if (data == NULL)
  {
    return UART_SERVICE_ERR_PARAM;
  }
  
  if (len < 2U)
  {
    return UART_SERVICE_ERR_RX;
  }

  if(data[0] == '1' && data[1] == ',')
  {
    shell_interface_printf("mqtt rx: %.*s", len - 2U, &data[2]);
  }
  else if(data[0] == '2' && data[1] == ',')
  {
    for (uint16_t i = 2U; i < len; ++i)
    {
      (void)ota_ymodem_protocol_input_byte(data[i]);
    }
  }

  return UART_SERVICE_OK;
}

static const uart_service_config_t uart_service_table[] = {
  {
    .name = SHELL_INTERFACE_NAME,
    .huart = &huart5,
    .rx_callback = shell_interface_on_rx,
    .tx_timeout_ms = 100U,
    .rx_timeout_ms = 0U,
  },
  {
    .name = MQTT_INTERFACE_NAME,
    .huart = &huart1,
    .rx_callback = mqtt_interface_on_rx,
    .tx_timeout_ms = 100U,
    .rx_timeout_ms = 0U,
  },
};

uart_service_status_t uart_service_port_init(void)
{
  const uint16_t count = (uint16_t)(sizeof(uart_service_table) / sizeof(uart_service_table[0]));
  uart_service_status_t ret;

  if (count == 0U)
  {
    LOG_E("uart service table is empty");
    return UART_SERVICE_ERR_PARAM;
  }

  for (uint16_t i = 0U; i < count; ++i)
  {
    ret = uart_service_register_obj(&uart_service_table[i]);
    if (ret != UART_SERVICE_OK)
    {
      LOG_E("uart service register failed: %d", (int)ret);
      continue;
    }

    ret = uart_service_start_rx_it(uart_service_table[i].huart);
    if (ret != UART_SERVICE_OK)
    {
      LOG_E("uart service start rx failed: %d", (int)ret);
      continue;
    }
  }

  return UART_SERVICE_OK;
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
#if !UART_SEVICE_ENABLE_DMA
  uart_service_status_t ret = uart_service_on_rx_complete(huart);
  if (ret != UART_SERVICE_OK)
  {
    LOG_E("rx complete failed: %d",(int)ret);
  }
#endif
}

void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t size)
{
#if UART_SEVICE_ENABLE_DMA
  uart_service_status_t ret = uart_service_on_rx_event(huart, size);
  if (ret != UART_SERVICE_OK)
  {
    LOG_E("rx event failed: %d", (int)ret);
  }
#endif
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
#if UART_SEVICE_ENABLE_DMA
  uart_service_status_t ret = uart_service_on_tx_complete(huart);
  if (ret != UART_SERVICE_OK)
  {
    LOG_E("tx complete failed: %d", (int)ret);
  }
#endif
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  if ((huart == NULL) || (huart->Instance == NULL))
  {
    LOG_E("invalid huart pointer");
    return;
  }

  LOG_E("uart error: err=0x%08lx g=%lu rx=%lu isr=0x%08lx",
             (unsigned long)huart->ErrorCode,
             (unsigned long)huart->gState,
             (unsigned long)huart->RxState,
             (unsigned long)huart->Instance->ISR);

  uart_service_status_t status = uart_service_on_error(huart);
  if (status == UART_SERVICE_ERR_NOT_INIT)
  {
    return;
  }

  if ((status != UART_SERVICE_OK) && (status != UART_SERVICE_ERR_UART))
  {
    LOG_E("uart service error failed: %d", (int)status);
    return;
  }

  status = uart_service_start_rx_it(huart);
  if (status != UART_SERVICE_OK)
  {
    LOG_E("restart rx failed: %d", (int)status);
    return;
  }  
}
