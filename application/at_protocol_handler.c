#include "at_protocol_handler.h"

#include <string.h>

#include "ota_flash_service.h"
#include "stm32h7xx_hal.h"
#include "uart_manage_port.h"

typedef int32_t (*craner_cmd_handler_t)(const uint8_t *cmd, uint16_t len, at_reply_send_fn_t reply_fn);

typedef struct
{
  const char *cmd;
  craner_cmd_handler_t handler;
} craner_cmd_entry_t;

typedef struct
{
  const uint8_t *cmd;
  uint16_t cmd_len;
} craner_at_command_t;

static uint8_t craner_is_blank(uint8_t ch)
{
  return (uint8_t)((ch == ' ') || (ch == '\t') || (ch == '\r') || (ch == '\n'));
}

static int32_t craner_find_at_command(const uint8_t *buf, uint16_t len, craner_at_command_t *at_cmd)
{
  static const char craner_prefix[] = "craner#";
  const uint16_t craner_prefix_len = (uint16_t)(sizeof(craner_prefix) - 1U);
  uint16_t index = 0U;
  uint16_t full_cmd_len;

  if ((buf == NULL) || (at_cmd == NULL))
  {
    return AT_PREFIX_NOT_MATCH;
  }

  while ((index < len) && (craner_is_blank(buf[index]) != 0U))
  {
    index++;
  }

  if ((len - index) < craner_prefix_len)
  {
    return AT_PREFIX_NOT_MATCH;
  }

  if (memcmp(&buf[index], craner_prefix, craner_prefix_len) != 0)
  {
    return AT_PREFIX_NOT_MATCH;
  }

  full_cmd_len = (uint16_t)(len - index);
  while ((full_cmd_len > craner_prefix_len) &&
         (craner_is_blank(buf[index + full_cmd_len - 1U]) != 0U))
  {
    full_cmd_len--;
  }

  at_cmd->cmd = &buf[index + craner_prefix_len];
  at_cmd->cmd_len = (uint16_t)(full_cmd_len - craner_prefix_len);

  return AT_OK;
}

static void craner_reply_ok(at_reply_send_fn_t reply_fn)
{
  static const uint8_t ack[] = "craner#OK\r\n";
  (void)reply_fn(ack, (uint16_t)(sizeof(ack) - 1U));
}

static void craner_reply_error(at_reply_send_fn_t reply_fn)
{
  static const uint8_t err[] = "craner#ERROR\r\n";
  (void)reply_fn(err, (uint16_t)(sizeof(err) - 1U));
}

static void craner_reply_cmd_error(at_reply_send_fn_t reply_fn, const uint8_t *cmd, uint16_t len)
{
  static const uint8_t prefix[] = "craner#";
  static const uint8_t suffix[] = " ERROR\r\n";

  (void)reply_fn(prefix, (uint16_t)(sizeof(prefix) - 1U));
  if ((len > 2U) && (cmd[0] == (uint8_t)'A') && (cmd[1] == (uint8_t)'T'))
  {
    (void)reply_fn(&cmd[2], (uint16_t)(len - 2U));
  }
  else
  {
    (void)reply_fn(cmd, len);
  }
  (void)reply_fn(suffix, (uint16_t)(sizeof(suffix) - 1U));
}

static int32_t craner_cmd_ota_cancel(const uint8_t *cmd, uint16_t len, at_reply_send_fn_t reply_fn)
{
  if (ota_flash_clear_ota_request() == OTA_FLASH_OK)
  {
    craner_reply_ok(reply_fn);
    NVIC_SystemReset();
    return AT_OK;
  }

  craner_reply_cmd_error(reply_fn, cmd, len);
  return AT_ACTION_EXECUTION_FAILED;
}

static int32_t craner_cmd_sys_reset(const uint8_t *cmd, uint16_t len, at_reply_send_fn_t reply_fn)
{
  (void)cmd;
  (void)len;
  craner_reply_ok(reply_fn);
  NVIC_SystemReset();
  return AT_OK;
}

static const craner_cmd_entry_t craner_cmd_table[] = {
  {"AT+OTACANCEL", craner_cmd_ota_cancel},
  {"AT+SYSRESET", craner_cmd_sys_reset},
};

int32_t craner_at_handler(const uint8_t *buf, uint16_t len, at_reply_send_fn_t reply_fn)
{
  at_reply_send_fn_t send_fn = (reply_fn != NULL) ? reply_fn : shell_interface_send;
  craner_at_command_t at_cmd;

  if (craner_find_at_command(buf, len, &at_cmd) != AT_OK)
  {
    return AT_PREFIX_NOT_MATCH;
  }

  for (uint16_t i = 0U; i < (uint16_t)(sizeof(craner_cmd_table) / sizeof(craner_cmd_table[0])); ++i)
  {
    uint16_t table_cmd_len = (uint16_t)strlen(craner_cmd_table[i].cmd);
    if ((at_cmd.cmd_len == table_cmd_len) && (memcmp(at_cmd.cmd, craner_cmd_table[i].cmd, table_cmd_len) == 0))
    {
      return craner_cmd_table[i].handler(at_cmd.cmd, at_cmd.cmd_len, send_fn);
    }
  }

  craner_reply_error(send_fn);
  return AT_UNKNOWN_CMD;
}
