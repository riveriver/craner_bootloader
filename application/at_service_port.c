#include "at_service_port.h"

#include <stddef.h>

static const at_service_command_t craner_at_command_table[] = {
  {
    .name = "OTASLOA",
    .write = NULL,
    .read = at_otasloa_read,
    .describe = at_otasloa_describe,
    .user = NULL,
  },
};

static const at_service_group_t at_group_table[] = {
  {
    .prefix = "craner",
    .commands = craner_at_command_table,
    .command_count = (uint16_t)(sizeof(craner_at_command_table) / sizeof(craner_at_command_table[0])),
    .user = NULL,
  },
};

int32_t at_service_port_process(const uint8_t *buf,
                                uint16_t len,
                                at_service_reply_send_fn_t reply_fn)
{
  return at_service_process(at_group_table,
                            (uint16_t)(sizeof(at_group_table) / sizeof(at_group_table[0])),
                            buf,
                            len,
                            reply_fn);
}
