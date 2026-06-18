#include "at_service.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

typedef enum
{
  AT_SERVICE_OP_READ = 0,
  AT_SERVICE_OP_WRITE,
  AT_SERVICE_OP_DESCRIBE,
} at_service_operation_t;

static uint16_t at_service_skip_space(const uint8_t *buf, uint16_t len, uint16_t index)
{
  while (index < len)
  {
    if ((buf[index] != (uint8_t)' ') &&
        (buf[index] != (uint8_t)'\t') &&
        (buf[index] != (uint8_t)'\r') &&
        (buf[index] != (uint8_t)'\n'))
    {
      break;
    }

    ++index;
  }

  return index;
}

static uint16_t at_service_trim_tail(const uint8_t *buf, uint16_t start, uint16_t end)
{
  while (end > start)
  {
    uint8_t value = buf[end - 1U];

    if ((value != (uint8_t)' ') &&
        (value != (uint8_t)'\t') &&
        (value != (uint8_t)'\r') &&
        (value != (uint8_t)'\n'))
    {
      break;
    }

    --end;
  }

  return end;
}

static const at_service_group_t *at_service_find_group(const at_service_group_t *groups,
                                                       uint16_t group_count,
                                                       const uint8_t *buf,
                                                       uint16_t len,
                                                       uint16_t *offset)
{
  uint16_t index;

  if ((groups == NULL) || (buf == NULL) || (offset == NULL))
  {
    return NULL;
  }

  index = at_service_skip_space(buf, len, 0U);

  for (uint16_t i = 0U; i < group_count; ++i)
  {
    uint16_t prefix_len;
    static const char at_token[] = "#AT+";
    uint16_t at_token_len = (uint16_t)(sizeof(at_token) - 1U);

    if (groups[i].prefix == NULL)
    {
      continue;
    }

    prefix_len = (uint16_t)strlen(groups[i].prefix);
    if (((uint16_t)(len - index) < (uint16_t)(prefix_len + at_token_len)) ||
        (memcmp(&buf[index], groups[i].prefix, prefix_len) != 0) ||
        (memcmp(&buf[index + prefix_len], at_token, at_token_len) != 0))
    {
      continue;
    }

    *offset = (uint16_t)(index + prefix_len + at_token_len);
    return &groups[i];
  }

  return NULL;
}

static const at_service_command_t *at_service_find_command(const at_service_group_t *group,
                                                           const uint8_t *name,
                                                           uint16_t name_len)
{
  if ((group == NULL) || (name == NULL) || (name_len == 0U))
  {
    return NULL;
  }

  for (uint16_t i = 0U; i < group->command_count; ++i)
  {
    uint16_t cmd_len;

    if (group->commands[i].name == NULL)
    {
      continue;
    }

    cmd_len = (uint16_t)strlen(group->commands[i].name);
    if ((cmd_len == name_len) && (memcmp(name, group->commands[i].name, name_len) == 0))
    {
      return &group->commands[i];
    }
  }

  return NULL;
}

int32_t at_service_send_reply(at_service_reply_send_fn_t send_fn, const char *reply)
{
  if ((send_fn == NULL) || (reply == NULL))
  {
    return AT_SERVICE_ERR_PARAM;
  }

  return send_fn((const uint8_t *)reply, (uint16_t)strlen(reply));
}

int32_t at_service_send_ok(at_service_reply_send_fn_t send_fn, const char *prefix)
{
  char reply[48];
  int len;

  if ((send_fn == NULL) || (prefix == NULL))
  {
    return AT_SERVICE_ERR_PARAM;
  }

  len = snprintf(reply, sizeof(reply), "%s#OK\r\n", prefix);
  if ((len <= 0) || ((uint32_t)len >= sizeof(reply)))
  {
    return AT_SERVICE_ERR_PARAM;
  }

  return at_service_send_reply(send_fn, reply);
}

int32_t at_service_send_error(at_service_reply_send_fn_t send_fn, const char *prefix)
{
  char reply[48];
  int len;

  if ((send_fn == NULL) || (prefix == NULL))
  {
    return AT_SERVICE_ERR_PARAM;
  }

  len = snprintf(reply, sizeof(reply), "%s#ERROR\r\n", prefix);
  if ((len <= 0) || ((uint32_t)len >= sizeof(reply)))
  {
    return AT_SERVICE_ERR_PARAM;
  }

  return at_service_send_reply(send_fn, reply);
}

int32_t at_service_reply_ok(const at_service_request_t *request)
{
  if (request == NULL)
  {
    return AT_SERVICE_ERR_PARAM;
  }

  return at_service_send_ok(request->reply_fn, request->group_prefix);
}

int32_t at_service_reply_error(const at_service_request_t *request)
{
  if (request == NULL)
  {
    return AT_SERVICE_ERR_PARAM;
  }

  return at_service_send_error(request->reply_fn, request->group_prefix);
}

int32_t at_service_reply_value(const at_service_request_t *request, const char *value)
{
  char reply[128];
  int len;

  if ((request == NULL) || (value == NULL))
  {
    return AT_SERVICE_ERR_PARAM;
  }

  len = snprintf(reply, sizeof(reply), "%s#+%s:%s\r\n",
                 request->group_prefix,
                 request->command_name,
                 value);
  if ((len <= 0) || ((uint32_t)len >= sizeof(reply)))
  {
    return AT_SERVICE_ERR_PARAM;
  }

  return at_service_send_reply(request->reply_fn, reply);
}

int32_t at_service_reply_options(const at_service_request_t *request, const char *options)
{
  char reply[160];
  int len;

  if ((request == NULL) || (options == NULL))
  {
    return AT_SERVICE_ERR_PARAM;
  }

  len = snprintf(reply, sizeof(reply), "%s#+%s:%s\r\n%s#OK\r\n",
                 request->group_prefix,
                 request->command_name,
                 options,
                 request->group_prefix);
  if ((len <= 0) || ((uint32_t)len >= sizeof(reply)))
  {
    return AT_SERVICE_ERR_PARAM;
  }

  return at_service_send_reply(request->reply_fn, reply);
}

uint8_t at_service_value_equals(const at_service_request_t *request, const char *value)
{
  uint16_t value_len;

  if ((request == NULL) || (value == NULL))
  {
    return 0U;
  }

  value_len = (uint16_t)strlen(value);
  if (request->value_len != value_len)
  {
    return 0U;
  }

  return (uint8_t)(memcmp(request->value, value, value_len) == 0);
}

uint16_t at_service_copy_value(const at_service_request_t *request, char *buffer, uint16_t buffer_size)
{
  uint16_t copy_len;

  if ((request == NULL) || (buffer == NULL) || (buffer_size == 0U))
  {
    return 0U;
  }

  copy_len = request->value_len;
  if (copy_len >= buffer_size)
  {
    copy_len = (uint16_t)(buffer_size - 1U);
  }

  if (copy_len > 0U)
  {
    (void)memcpy(buffer, request->value, copy_len);
  }

  buffer[copy_len] = '\0';
  return copy_len;
}

int32_t at_service_process(const at_service_group_t *groups,
                           uint16_t group_count,
                           const uint8_t *buf,
                           uint16_t len,
                           at_service_reply_send_fn_t reply_fn)
{
  const at_service_group_t *group;
  const at_service_command_t *command;
  at_service_request_t request;
  at_service_operation_t operation = AT_SERVICE_OP_READ;
  at_service_handler_fn_t handler = NULL;
  uint16_t index;
  uint16_t command_start;
  uint16_t command_end;
  uint16_t value_start = 0U;
  uint16_t value_end = 0U;

  if ((groups == NULL) || (group_count == 0U) || (buf == NULL) || (len == 0U))
  {
    return AT_SERVICE_ERR_PARAM;
  }

  group = at_service_find_group(groups, group_count, buf, len, &index);
  if (group == NULL)
  {
    return AT_SERVICE_PREFIX_NOT_MATCH;
  }

  command_start = index;
  while (index < len)
  {
    uint8_t value = buf[index];

    if ((value == (uint8_t)'?') ||
        (value == (uint8_t)'=') ||
        (value == (uint8_t)' ') ||
        (value == (uint8_t)'\t') ||
        (value == (uint8_t)'\r') ||
        (value == (uint8_t)'\n'))
    {
      break;
    }

    ++index;
  }

  command_end = index;
  if (command_end <= command_start)
  {
    return AT_SERVICE_UNKNOWN_CMD;
  }

  index = at_service_skip_space(buf, len, index);
  if (index < len)
  {
    if (buf[index] == (uint8_t)'?')
    {
      ++index;
      index = at_service_skip_space(buf, len, index);
      if (index != len)
      {
        return AT_SERVICE_ERR_PARAM;
      }

      operation = AT_SERVICE_OP_READ;
    }
    else if (buf[index] == (uint8_t)'=')
    {
      ++index;
      index = at_service_skip_space(buf, len, index);
      if ((index < len) && (buf[index] == (uint8_t)'?'))
      {
        ++index;
        index = at_service_skip_space(buf, len, index);
        if (index != len)
        {
          return AT_SERVICE_ERR_PARAM;
        }

        operation = AT_SERVICE_OP_DESCRIBE;
      }
      else
      {
        operation = AT_SERVICE_OP_WRITE;
        value_start = index;
        value_end = at_service_trim_tail(buf, value_start, len);
        if (value_end <= value_start)
        {
          return AT_SERVICE_ERR_PARAM;
        }
      }
    }
    else if (index != len)
    {
      return AT_SERVICE_ERR_PARAM;
    }
  }

  command = at_service_find_command(group, &buf[command_start], (uint16_t)(command_end - command_start));
  if (command == NULL)
  {
    return AT_SERVICE_UNKNOWN_CMD;
  }

  switch (operation)
  {
    case AT_SERVICE_OP_WRITE:
      handler = command->write;
      break;

    case AT_SERVICE_OP_DESCRIBE:
      handler = command->describe;
      break;

    case AT_SERVICE_OP_READ:
    default:
      handler = command->read;
      break;
  }

  if (handler == NULL)
  {
    return AT_SERVICE_ERR_UNSUPPORTED;
  }

  request.buffer = buf;
  request.buffer_len = len;
  request.group_prefix = group->prefix;
  request.command_name = command->name;
  request.value = (operation == AT_SERVICE_OP_WRITE) ? &buf[value_start] : NULL;
  request.value_len = (operation == AT_SERVICE_OP_WRITE) ? (uint16_t)(value_end - value_start) : 0U;
  request.reply_fn = reply_fn;
  request.group_user = group->user;
  request.command_user = command->user;

  return handler(&request);
}
