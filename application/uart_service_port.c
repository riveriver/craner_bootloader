#include "uart_service_port.h"

#include <stddef.h>

#include "main.h"

#define LOG_E(format, ...) (void)shell_interface_printf("[E] " format "\r\n", ##__VA_ARGS__)

extern UART_HandleTypeDef huart5;
extern UART_HandleTypeDef huart1;

static const uart_service_config_t uart_service_table[] = {
  {
    .huart = &huart5,
    .name = "shell",
    .recv_callback = shell_interface_recv_callback,
  },
  {
    .huart = &huart1,
    .name = "mqtt",
    .recv_callback = mqtt_interface_recv_callback,
  },
};

uart_service_status_t uart_service_init_port(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  __HAL_RCC_GPIOE_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();

  HAL_GPIO_WritePin(GPIOE, GPIO_PIN_6, GPIO_PIN_SET);
  HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_SET);
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_15, GPIO_PIN_SET);

  GPIO_InitStruct.Pin = GPIO_PIN_6;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOE, &GPIO_InitStruct);

  GPIO_InitStruct.Pin = GPIO_PIN_13;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

  GPIO_InitStruct.Pin = GPIO_PIN_15;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  uart_service_port_init_ring_buffer();

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

    ret = uart_service_start_recv_it(uart_service_table[i].huart);
    if (ret != UART_SERVICE_OK)
    {
      LOG_E("uart service start rx failed: %d", (int)ret);
      continue;
    }
  }

  return UART_SERVICE_OK;
}

void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t size)
{
  uart_service_status_t ret = uart_service_on_recv_event(huart, size);
  if (ret != UART_SERVICE_OK)
  {
    LOG_E("rx event failed: %d", (int)ret);
  }
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
  uart_service_status_t ret = uart_service_on_send_complete(huart);
  if (ret != UART_SERVICE_OK)
  {
    LOG_E("tx complete failed: %d", (int)ret);
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

  status = uart_service_start_recv_it(huart);
  if (status != UART_SERVICE_OK)
  {
    LOG_E("restart rx failed: %d", (int)status);
    return;
  }  
}
