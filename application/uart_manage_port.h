#ifndef UART_SERVICE_PORT_H
#define UART_SERVICE_PORT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "uart_manage.h"
#include "lwrb.h"

void uart_service_init_port(void);
int32_t shell_interface_send(const uint8_t *data, uint16_t len);
int32_t mqtt_system_inform_send(const uint8_t *data, uint16_t len);
int32_t shell_interface_printf(const char *format, ...);
int32_t mqtt_system_inform_printf(const char *format, ...);
int32_t mqtt_ota_progress_printf(const uint8_t *data, uint16_t len);

int32_t shell_interface_recv_callback(uint8_t *data, uint16_t len);
int32_t mqtt_interface_recv_callback(uint8_t *data, uint16_t len);

const lwrb_t *uart_service_port_get_ota_ring(void);

#ifdef __cplusplus
}
#endif

#endif /* UART_SERVICE_PORT_H */
