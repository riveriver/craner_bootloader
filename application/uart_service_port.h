#ifndef UART_SERVICE_PORT_H
#define UART_SERVICE_PORT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "uart_service.h"

typedef void (*uart_service_port_ota_rx_callback_t)(uint8_t byte, void *user);

uart_service_status_t uart_service_port_init(void);

#ifdef __cplusplus
}
#endif

#endif /* UART_SERVICE_PORT_H */
