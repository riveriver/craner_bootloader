#ifndef AT_PROTOCOL_HANDLER_H
#define AT_PROTOCOL_HANDLER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef enum
{
  AT_OK = 0,
  AT_PREFIX_NOT_MATCH = 1,
  AT_ACTION_EXECUTION_FAILED = 2,
  AT_UNKNOWN_CMD = -1
} at_ret_t;

typedef int32_t (*at_reply_send_fn_t)(const uint8_t *buf, uint16_t len);

int32_t craner_at_handler(const uint8_t *buf, uint16_t len, at_reply_send_fn_t reply_fn);

#ifdef __cplusplus
}
#endif

#endif /* AT_PROTOCOL_HANDLER_H */
