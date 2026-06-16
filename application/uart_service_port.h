#ifndef UART_SERVICE_PORT_H
#define UART_SERVICE_PORT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "uart_service.h"

uart_service_status_t uart_service_port_init(void);
uart_service_status_t shell_interface_printf(const char *format, ...);
uart_service_status_t mqtt_interface_printf(const char *format, ...);
uart_service_status_t ota_interface_send(const uint8_t *data, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* UART_SERVICE_PORT_H */
