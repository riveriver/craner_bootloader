#ifndef UART_SERVICE_H
#define UART_SERVICE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "stm32h7xx_hal.h"
#include "stm32h7xx_hal_uart.h"

typedef enum
{
  UART_SERVICE_OK = 0,
  UART_SERVICE_ERR_PARAM = -1,
  UART_SERVICE_ERR_NOT_FOUND = -2,
  UART_SERVICE_ERR_NO_SLOT = -3,
  UART_SERVICE_ERR_TX = -4,
  UART_SERVICE_ERR_RX = -5,
  UART_SERVICE_ERR_RX_START = -6,
  UART_SERVICE_ERR_RX_ABORT = -7,
  UART_SERVICE_ERR_BUSY = -8,
  UART_SERVICE_ERR_TIMEOUT = -9,
  UART_SERVICE_ERR_UART = -10,
} uart_service_status_t;

typedef struct uart_service_t uart_service_t;
typedef uart_service_status_t (*uart_service_rx_callback_t)(uart_service_t *uart, uint8_t byte);

typedef struct
{
  const char *name;
  UART_HandleTypeDef *huart;
  uart_service_rx_callback_t rx_callback;
  uint32_t tx_timeout_ms;
  uint32_t rx_timeout_ms;
} uart_service_config_t;

struct uart_service_t
{
  const char *name;
  uint8_t used;
  UART_HandleTypeDef *huart;
  uart_service_rx_callback_t rx_callback;
  uint32_t tx_timeout_ms;
  uint32_t rx_timeout_ms;
  uint8_t rx_byte;

};

uart_service_status_t uart_service_register_obj(const uart_service_config_t *config);
uart_service_status_t uart_service_init(const uart_service_config_t *table, uint16_t count);

uart_service_t *uart_service_get_obj(UART_HandleTypeDef *huart);
uart_service_t *uart_service_get_obj_by_name(const char *name);

uart_service_status_t uart_service_set_rx_callback(UART_HandleTypeDef *huart,
                                                 uart_service_rx_callback_t callback);

uart_service_status_t uart_service_send(UART_HandleTypeDef *huart, const uint8_t *data, uint16_t len);
uart_service_status_t uart_service_send_by_name(const char *name, const uint8_t *data, uint16_t len);

uart_service_status_t uart_service_receive(UART_HandleTypeDef *huart,
                                         uint8_t *data,
                                         uint16_t len,
                                         uint32_t timeout_ms);
uart_service_status_t uart_service_receive_by_name(const char *name,
                                                 uint8_t *data,
                                                 uint16_t len,
                                                 uint32_t timeout_ms);

uart_service_status_t uart_service_start_rx_it(UART_HandleTypeDef *huart);
uart_service_status_t uart_service_start_rx_it_by_name(const char *name);
uart_service_status_t uart_service_on_rx_complete(UART_HandleTypeDef *huart);
uart_service_status_t uart_service_on_error(UART_HandleTypeDef *huart);

#ifdef __cplusplus
}
#endif

#endif // UART_SERVICE_H
