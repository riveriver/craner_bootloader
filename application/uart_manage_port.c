#include <stddef.h>
#include <string.h>
#include <uart_manage_port.h>

#include "main.h"

#define LOG_E(format, ...) (void)shell_interface_printf("[E] " format "\r\n", ##__VA_ARGS__)
#define LOG_I(format, ...) (void)shell_interface_printf("[I] " format "\r\n", ##__VA_ARGS__)

extern UART_HandleTypeDef huart5;
extern UART_HandleTypeDef huart1;

/* DMA buffer placement */
#if defined(__GNUC__)
#define DMA_BUFFER __attribute__((section(".dma_buffer"), aligned(32)))
#else
#define DMA_BUFFER
#endif

extern DMA_HandleTypeDef hdma_uart5_rx;
extern DMA_HandleTypeDef hdma_usart1_rx;

static uint8_t uart5_recv_buff[512U] DMA_BUFFER;
static uint8_t uart5_send_buff[512U] DMA_BUFFER;
static uint8_t uart5_send_fifo_buff[512U] DMA_BUFFER;
static uint8_t uart5_process_buff[512U * 2] DMA_BUFFER;

static uint8_t uart1_send_buff[512U] DMA_BUFFER;
static uint8_t uart1_send_fifo_buff[512U] DMA_BUFFER;
static uint8_t uart1_recv_buff[2048U] DMA_BUFFER;
static uint8_t uart1_process_buff[2048U * 2] DMA_BUFFER;

const uart_inferface_t uart_manage_table[] = {
  {
    .name = "shell",
    .uart_h = &huart5,
    .dma_h = &hdma_uart5_rx,
    .recv_buffer = uart5_recv_buff,
    .recv_buffer_size = sizeof(uart5_recv_buff),
    .process_buffer = uart5_process_buff,
    .process_buffer_size = sizeof(uart5_process_buff),
    .recv_callback = shell_interface_recv_callback,
    .send_buffer = uart5_send_buff,
    .send_buffer_size = sizeof(uart5_send_buff),
    .send_fifo_buffer = uart5_send_fifo_buff,
    .send_fifo_size = sizeof(uart5_send_fifo_buff),
    .send_callback = NULL,
  },
  {
    .name = "mqtt",
    .uart_h = &huart1,
    .dma_h = &hdma_usart1_rx,
    .recv_buffer = uart1_recv_buff,
    .recv_buffer_size = sizeof(uart1_recv_buff),
    .process_buffer = uart1_process_buff,
    .process_buffer_size = sizeof(uart1_process_buff),
    .recv_callback = mqtt_interface_recv_callback,
    .send_buffer = uart1_send_buff,
    .send_buffer_size = sizeof(uart1_send_buff),
    .send_fifo_buffer = uart1_send_fifo_buff,
    .send_fifo_size = sizeof(uart1_send_fifo_buff),
    .send_callback = NULL,
  },
};

void uart_service_init_port(void)
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

  const uint16_t count = (uint16_t)(sizeof(uart_manage_table) / sizeof(uart_manage_table[0]));
  int interface_init_result[UART_MANAGE_MAX_OBJECTS] = {0};

  if ((count == 0U) || (count > UART_MANAGE_MAX_OBJECTS))
  {
    return;
  }

  for (uint16_t i = 0U; i < count; ++i)
  {
    int ret = uart_manage_register_interface((uart_inferface_t *)&uart_manage_table[i]);
    if (ret == UART_MANAGE_OK)
    {
      ret = uart_manage_enable_dma_recv(uart_manage_table[i].uart_h);
    }
    interface_init_result[i] = ret;
  }

  for (uint16_t i = 0U; i < count; ++i)
  {
    if (interface_init_result[i] == UART_MANAGE_OK)
    {
      LOG_I("uart manage init %s ok", uart_manage_table[i].name);
    }
    else
    {
      LOG_E("uart manage init %s failed: %d", uart_manage_table[i].name, interface_init_result[i]);
    }
  }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  (void)uart_manage_reset_dma_send(huart);
  (void)uart_manage_enable_dma_recv(huart);
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
  uart_manage_send_completed_hook(huart);
}


void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t size)
{
  uart_inferface_t *m_obj = uart_manage_get_obj(huart);

  if (m_obj != NULL)
  {
    if (size > 0U)
    {
      (void)uart_manage_recv_idle_hook(m_obj, INTERRUPT_TYPE_UART, size);
    }
    (void)uart_manage_enable_dma_recv(huart);
  }
}
