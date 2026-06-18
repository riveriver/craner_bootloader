#ifndef UART_SERVICE_PORT_H
#define UART_SERVICE_PORT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "uart_service.h"
#include "ring_buffer.h"

uart_service_status_t uart_service_init_port(void);
void uart_service_port_init_ring_buffer(void);
uart_service_status_t shell_interface_send(const uint8_t *data, uint16_t len);
uart_service_status_t mqtt_interface_send(const uint8_t *data, uint16_t len);
uart_service_status_t shell_interface_printf(const char *format, ...);
uart_service_status_t mqtt_interface_printf(const char *format, ...);
uart_service_status_t ota_ack_send(const uint8_t *data, uint16_t len);

uart_service_status_t shell_interface_recv_callback(uart_service_t *uart,
                                                    const uint8_t *data,
                                                    uint16_t len);
uart_service_status_t mqtt_interface_recv_callback(uart_service_t *uart,
                                                   const uint8_t *data,
                                                   uint16_t len);
const uint8_t *uart_service_port_get_ota_read_ptr(uint16_t *len);
void uart_service_port_consume_ota_data(uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* UART_SERVICE_PORT_H */
