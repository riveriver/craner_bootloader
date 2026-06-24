#include <stddef.h>
#include <string.h>
#include <uart_manage_port.h>

#include "main.h"

#define LOG_E(format, ...) (void)shell_interface_printf("[E] " format "\r\n", ##__VA_ARGS__)

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

static uint8_t uart5_recv_buff[256U] DMA_BUFFER;
static uint8_t uart5_send_buff[256U] DMA_BUFFER;
static uint8_t uart5_send_fifo_buff[256U] DMA_BUFFER;
static uint8_t uart5_process_buff[1024U] DMA_BUFFER;

static uint8_t uart1_recv_buff[256U] DMA_BUFFER;
static uint8_t uart1_send_buff[256U] DMA_BUFFER;
static uint8_t uart1_send_fifo_buff[256U] DMA_BUFFER;
static uint8_t uart1_process_buff[1024U] DMA_BUFFER;

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

int uart_service_init_port(void)
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

  const uint16_t count = (uint16_t)(sizeof(uart_manage_table) / sizeof(uart_manage_table[0]));
  if (count == 0U)
  {
    LOG_E("uart manage table is empty");
    return -1;
  }

  if (uart_manage_init_table(uart_manage_table, count) != 0)
  {
    LOG_E("uart_manage_init_table failed");
    return -1;
  }

  /* enable DMA receive for registered ports */
  uart_manage_enable_dma_recv_by_name("shell");
  uart_manage_enable_dma_recv_by_name("mqtt");

  return 0;
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

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
  uart_manage_send_completed_hook(huart);
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  (void)huart;
}
