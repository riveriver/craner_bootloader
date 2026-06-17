#ifndef UART_SERVICE_H
#define UART_SERVICE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "stm32h7xx_hal.h"
#include "stm32h7xx_hal_uart.h"
#include "stm32h7xx_hal_uart_ex.h"

#ifndef UART_SERVICE_RECV_DMA_BUFFER_SIZE
#define UART_SERVICE_RECV_DMA_BUFFER_SIZE 2048U
#endif

#ifndef UART_SERVICE_SEND_DMA_BUFFER_SIZE
#define UART_SERVICE_SEND_DMA_BUFFER_SIZE 1024U
#endif

typedef enum
{
  UART_SERVICE_OK = 0,
  UART_SERVICE_ERR_PARAM = -1,
  UART_SERVICE_ERR_NOT_FOUND = -2,
  UART_SERVICE_ERR_NO_SLOT = -3,
  UART_SERVICE_ERR_TX = -4,
  UART_SERVICE_ERR_RECV = -5,
  UART_SERVICE_ERR_RECV_START = -6,
  UART_SERVICE_ERR_RECV_ABORT = -7,
  UART_SERVICE_ERR_BUSY = -8,
  UART_SERVICE_ERR_TIMEOUT = -9,
  UART_SERVICE_ERR_UART = -10,
  UART_SERVICE_ERR_NOT_INIT = -11,
} uart_service_status_t;

typedef struct uart_service_t uart_service_t;
typedef uart_service_status_t (*uart_service_recv_callback_t)(uart_service_t *uart,
                                                            const uint8_t *data,
                                                            uint16_t len);

typedef struct
{
  UART_HandleTypeDef *huart;
  const char *name;
  uart_service_recv_callback_t recv_callback;
} uart_service_config_t;

struct uart_service_t
{
  UART_HandleTypeDef *huart;
  const char *name;
  uart_service_recv_callback_t recv_callback;
  uint8_t used;
  uint8_t ready;
  uint8_t recv_dma_buffer[UART_SERVICE_RECV_DMA_BUFFER_SIZE];
  uint8_t send_dma_buffer[UART_SERVICE_SEND_DMA_BUFFER_SIZE];
  volatile uint8_t send_dma_busy;
};

uart_service_status_t uart_service_register_obj(const uart_service_config_t *config);
uart_service_t *uart_service_get_obj(UART_HandleTypeDef *huart);
uart_service_t *uart_service_get_obj_by_name(const char *name);
uart_service_status_t uart_service_send(UART_HandleTypeDef *huart, const uint8_t *data, uint16_t len);
uart_service_status_t uart_service_send_by_name(const char *name, const uint8_t *data, uint16_t len);
uart_service_status_t uart_service_start_recv_it(UART_HandleTypeDef *huart);
uart_service_status_t uart_service_start_recv_it_by_name(const char *name);
uart_service_status_t uart_service_on_recv_event(UART_HandleTypeDef *huart, uint16_t size);
uart_service_status_t uart_service_on_send_complete(UART_HandleTypeDef *huart);
uart_service_status_t uart_service_on_error(UART_HandleTypeDef *huart);

#ifdef __cplusplus
}
#endif

#endif // UART_SERVICE_H
