#include "uart_service_port.h"

#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "main.h"
#include "log_service.h"
#include "ota_ymodem_protocol.h"

#define LOG_E(format, ...) log_printf("[E] " format "\r\n", ##__VA_ARGS__)

extern UART_HandleTypeDef huart1;
extern UART_HandleTypeDef huart8;

#define SHELL_INTERFACE_NAME "shell"
#define OTA_INTERFACE_NAME "ota"

static uart_service_status_t shell_interface_on_rx(uart_service_t *uart, uint8_t byte)
{
  uart_service_send_by_name(SHELL_INTERFACE_NAME, &byte, 1U);
  return UART_SERVICE_OK;
}

static uart_service_status_t ota_interface_on_rx(uart_service_t *uart, uint8_t byte)
{
  (void)uart;
  (void)ota_ymodem_protocol_input_byte(byte);
  return UART_SERVICE_OK;
}

static const uart_service_config_t uart_service_table[] = {
  {
    .name = SHELL_INTERFACE_NAME,
    .huart = &huart8,
    .rx_callback = shell_interface_on_rx,
    .tx_timeout_ms = 100U,
    .rx_timeout_ms = 0U,
  },
  {
    .name = OTA_INTERFACE_NAME,
    .huart = &huart1,
    .rx_callback = ota_interface_on_rx,
    .tx_timeout_ms = 100U,
    .rx_timeout_ms = 0U,
  },
};

uart_service_status_t uart_service_port_init(void)
{
  const uint16_t count = (uint16_t)(sizeof(uart_service_table) / sizeof(uart_service_table[0]));

  uart_service_status_t ret =uart_service_init(uart_service_table, count);
  if(ret != UART_SERVICE_OK)
  {
    LOG_E("uart service init failed: %d", (int)ret);
    return UART_SERVICE_ERR;
  }
  
  (void)uart_service_start_rx_it_by_name("shell");
  (void)uart_service_start_rx_it_by_name("ota");
  return UART_SERVICE_OK;
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  uart_service_status_t ret = uart_service_on_rx_complete(huart);
  if (ret != UART_SERVICE_OK)
  {
    LOG_E("rx complete failed: %d",(int)ret);
  }
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

  uart_service_t *obj;
  obj = uart_service_get_obj(huart);
  if (obj == NULL)
  {
    return;
  }
  HAL_StatusTypeDef ret;
  ret = HAL_UART_AbortReceive(obj->huart);
  if (ret != HAL_OK)
  {
    LOG_E("abort rx failed: %d", (int)ret);
    return;
  }
 
  ret = HAL_UART_Receive_IT(obj->huart, &obj->rx_byte, 1U);
  if (ret != HAL_OK)
  {
    LOG_E("restart rx failed: %d", (int)ret);
    return;
  }  
}
