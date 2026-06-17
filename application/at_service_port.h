#ifndef AT_SERVICE_PORT_H
#define AT_SERVICE_PORT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "at_service.h"

int32_t at_otasloa_read(const at_service_request_t *request);
int32_t at_otasloa_describe(const at_service_request_t *request);

int32_t at_service_port_process(const uint8_t *buf,
                                uint16_t len,
                                at_service_reply_send_fn_t reply_fn);

#ifdef __cplusplus
}
#endif

#endif /* AT_SERVICE_PORT_H */
