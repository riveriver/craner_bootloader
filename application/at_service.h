#ifndef AT_SERVICE_H
#define AT_SERVICE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef enum
{
  AT_SERVICE_OK = 0,
  AT_SERVICE_PREFIX_NOT_MATCH = 1,
  AT_SERVICE_ACTION_EXECUTION_FAILED = 2,
  AT_SERVICE_UNKNOWN_CMD = -1,
  AT_SERVICE_ERR_PARAM = -2,
  AT_SERVICE_ERR_UNSUPPORTED = -3,
} at_service_ret_t;

typedef int32_t (*at_service_reply_send_fn_t)(uint8_t *buf, uint16_t len);

typedef struct at_service_request_t at_service_request_t;
typedef int32_t (*at_service_handler_fn_t)(const at_service_request_t *request);

typedef struct
{
  const char *name;
  at_service_handler_fn_t write;
  at_service_handler_fn_t read;
  at_service_handler_fn_t describe;
  void *user;
} at_service_command_t;

typedef struct
{
  const char *prefix;
  const at_service_command_t *commands;
  uint16_t command_count;
  void *user;
} at_service_group_t;

struct at_service_request_t
{
  const uint8_t *buffer;
  uint16_t buffer_len;
  const char *group_prefix;
  const char *command_name;
  const uint8_t *value;
  uint16_t value_len;
  at_service_reply_send_fn_t reply_fn;
  void *group_user;
  void *command_user;
};

int32_t at_service_process(const at_service_group_t *groups,
                           uint16_t group_count,
                           const uint8_t *buf,
                           uint16_t len,
                           at_service_reply_send_fn_t reply_fn);
int32_t at_service_send_reply(at_service_reply_send_fn_t send_fn, const char *reply);
int32_t at_service_send_ok(at_service_reply_send_fn_t send_fn, const char *prefix);
int32_t at_service_send_error(at_service_reply_send_fn_t send_fn, const char *prefix);
int32_t at_service_reply_ok(const at_service_request_t *request);
int32_t at_service_reply_error(const at_service_request_t *request);
int32_t at_service_reply_value(const at_service_request_t *request, const char *value);
int32_t at_service_reply_options(const at_service_request_t *request, const char *options);
uint8_t at_service_value_equals(const at_service_request_t *request, const char *value);
uint16_t at_service_copy_value(const at_service_request_t *request, char *buffer, uint16_t buffer_size);

#ifdef __cplusplus
}
#endif

#endif /* AT_SERVICE_H */
