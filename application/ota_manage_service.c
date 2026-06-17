#include "ota_manage_service.h"

#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "main.h"
#include "ota_flash_service.h"
#include "ota_ymodem_protocol.h"
#include "uart_service_port.h"

extern UART_HandleTypeDef huart1;
extern UART_HandleTypeDef huart5;
extern UART_HandleTypeDef huart8;

#define OTA_MANAGE_REQUEST_CRC_REQUEST_MS            5000UL
#define OTA_MANAGE_REQUEST_WAIT_TRANSFER_TIMEOUT_MS  60000UL
#define OTA_MANAGE_PROBE_CRC_REQUEST_MS              2000UL
#define OTA_MANAGE_PROBE_WAIT_TRANSFER_TIMEOUT_MS    6000UL
#define OTA_MANAGE_PACKET_TIMEOUT_MS                 1000UL

static void ota_manage_log(const char *level, const char *format, ...)
{
  char buffer[192];
  va_list args;
  int len;

  if ((level == NULL) || (format == NULL))
  {
    return;
  }

  len = snprintf(buffer, sizeof(buffer), "[%s][OTA] ", level);
  if ((len <= 0) || ((uint32_t)len >= sizeof(buffer)))
  {
    return;
  }

  va_start(args, format);
  len += vsnprintf(&buffer[len], sizeof(buffer) - (uint32_t)len, format, args);
  va_end(args);

  if (len <= 0)
  {
    return;
  }

  (void)mqtt_interface_printf("%s", buffer);
}

#define OTA_LOG_I(format, ...) ota_manage_log("I", format, ##__VA_ARGS__)
#define OTA_LOG_E(format, ...) ota_manage_log("E", format, ##__VA_ARGS__)

typedef void (*ota_app_entry_t)(void);

typedef struct
{
  ota_manage_state_t state;
  ota_flash_slot_t write_slot;
  ota_flash_slot_t requested_slot;
  uint32_t image_size;
  uint32_t image_crc32;
  uint32_t last_crc_request_ms;
  uint32_t wait_transfer_start_ms;
  uint32_t crc_request_interval_ms;
  uint32_t wait_transfer_timeout_ms;
  uint8_t ota_requested;
  uint8_t file_started;
} ota_manage_context_t;

static ota_manage_context_t g_ota_manage;

static void ota_manage_disable_all_irqs(void)
{
  uint32_t index;

  __disable_irq();

  for (index = 0U; index < 8U; ++index)
  {
    NVIC->ICER[index] = 0xFFFFFFFFUL;
    NVIC->ICPR[index] = 0xFFFFFFFFUL;
  }

  SysTick->CTRL = 0U;
  SysTick->LOAD = 0U;
  SysTick->VAL = 0U;
  SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk | SCB_ICSR_PENDSVCLR_Msk;
}

static const char *ota_manage_slot_name(ota_flash_slot_t slot)
{
  if (slot == OTA_FLASH_SLOT_A)
  {
    return "A";
  }

  if (slot == OTA_FLASH_SLOT_B)
  {
    return "B";
  }

  return "NONE";
}

static void ota_manage_notify_error(const char *format, ...)
{
  char buffer[128];
  va_list args;
  int len;

  if (format == NULL)
  {
    return;
  }

  va_start(args, format);
  len = vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);

  if (len <= 0)
  {
    return;
  }

  if ((uint32_t)len >= sizeof(buffer))
  {
    len = (int)sizeof(buffer) - 1;
  }

  (void)mqtt_interface_printf("OTA ERROR: %s", buffer);
}

static const char *ota_manage_flash_status_name(ota_flash_status_t status)
{
  switch (status)
  {
    case OTA_FLASH_OK:
      return "OK";
    case OTA_FLASH_ERR_PARAM:
      return "PARAM";
    case OTA_FLASH_ERR_RANGE:
      return "RANGE";
    case OTA_FLASH_ERR_HAL:
      return "HAL";
    case OTA_FLASH_ERR_META:
      return "META";
    case OTA_FLASH_ERR_STATE:
      return "STATE";
    default:
      return "UNKNOWN";
  }
}

static const char *ota_manage_ymodem_status_name(ota_ymodem_status_t status)
{
  switch (status)
  {
    case OTA_YMODEM_STATUS_OK:
      return "OK";
    case OTA_YMODEM_STATUS_ERR_PARAM:
      return "PARAM";
    case OTA_YMODEM_STATUS_ERR_SEND:
      return "SEND";
    case OTA_YMODEM_STATUS_ERR_SEQUENCE:
      return "SEQUENCE";
    case OTA_YMODEM_STATUS_ERR_CRC:
      return "CRC";
    case OTA_YMODEM_STATUS_ERR_PACKET:
      return "PACKET";
    case OTA_YMODEM_STATUS_ERR_CALLBACK:
      return "CALLBACK";
    case OTA_YMODEM_STATUS_ERR_ABORTED:
      return "ABORTED";
    case OTA_YMODEM_STATUS_ERR_TIMEOUT:
      return "TIMEOUT";
    default:
      return "UNKNOWN";
  }
}

static uint8_t ota_manage_find_bootable_slot(ota_flash_slot_t preferred_slot,
                                             ota_flash_slot_t *boot_slot)
{
  ota_flash_slot_t candidates[2];

  if (boot_slot == NULL)
  {
    return 0U;
  }

  candidates[0] = preferred_slot;
  candidates[1] = (preferred_slot == OTA_FLASH_SLOT_A) ? OTA_FLASH_SLOT_B : OTA_FLASH_SLOT_A;

  for (uint8_t i = 0U; i < 2U; ++i)
  {
    uint32_t addr;

    if (ota_flash_is_valid_slot(candidates[i]) == 0U)
    {
      continue;
    }

    addr = ota_flash_get_slot_address(candidates[i]);
    if ((addr != 0U) && (ota_flash_is_valid_app(addr) != 0U))
    {
      *boot_slot = candidates[i];
      return 1U;
    }
  }

  return 0U;
}

static uint8_t ota_manage_recover_boot_slot(ota_flash_slot_t failed_slot)
{
  ota_flash_meta_t meta;
  ota_flash_slot_t active_slot;
  ota_flash_slot_t boot_slot;
  uint8_t failed_index;

  if (ota_flash_read_meta(&meta) != OTA_FLASH_OK)
  {
    OTA_LOG_E("recover boot slot failed: read meta failed");
    return 0U;
  }

  active_slot = (ota_flash_slot_t)meta.active_slot;
  if (ota_manage_find_bootable_slot(active_slot, &boot_slot) == 0U)
  {
    OTA_LOG_E("recover boot slot failed: no bootable slot from active=%s",
              ota_manage_slot_name(active_slot));
    return 0U;
  }

  meta.active_slot = boot_slot;
  meta.ota_request = OTA_FLASH_OTA_REQUEST_NONE;
  meta.target_slot = OTA_FLASH_SLOT_NONE;

  if (ota_flash_is_valid_slot(failed_slot) != 0U)
  {
    failed_index = (failed_slot == OTA_FLASH_SLOT_B) ? 1U : 0U;
    meta.image[failed_index].size = 0U;
    meta.image[failed_index].crc32 = 0U;
    meta.image[failed_index].state = OTA_FLASH_IMAGE_EMPTY;
  }

  if (ota_flash_write_meta(&meta) != OTA_FLASH_OK)
  {
    OTA_LOG_E("recover boot slot failed: write meta failed");
    return 0U;
  }

  OTA_LOG_I("recover boot slot: failed_slot=%s boot_slot=%s",
            ota_manage_slot_name(failed_slot),
            ota_manage_slot_name(boot_slot));
  return 1U;
}

static uint8_t ota_manage_sp_in_ram(uint32_t sp)
{
  return (uint8_t)((((sp > 0x20000000UL) && (sp <= 0x20020000UL)) ||
                    ((sp > 0x24000000UL) && (sp <= 0x24080000UL)) ||
                    ((sp > 0x30000000UL) && (sp <= 0x30048000UL)) ||
                    ((sp > 0x38000000UL) && (sp <= 0x38010000UL))) ? 1U : 0U);
}

static int ota_manage_ymodem_send(const uint8_t *data, uint16_t len, void *user)
{
  (void)user;
  if (ota_ack_send(data, len) != UART_SERVICE_OK)
  {
    OTA_LOG_E("YMODEM send failed, len=%u", (unsigned int)len);
    return -1;
  }

  return 0;
}

static void ota_manage_send_cancel(void)
{
  static const uint8_t cancel[2] = {OTA_YMODEM_CAN, OTA_YMODEM_CAN};

  (void)ota_ack_send(cancel, (uint16_t)sizeof(cancel));
}

static int ota_manage_on_file_begin(const ota_ymodem_file_info_t *file, void *user)
{
  ota_manage_context_t *ctx = (ota_manage_context_t *)user;
  ota_flash_status_t flash_status;

  if ((ctx == NULL) || (file == NULL) || (file->size == 0U))
  {
    OTA_LOG_E("file begin invalid param, ctx=%p file=%p",
              (void *)ctx,
              (void *)file);
    return -1;
  }

  OTA_LOG_I("file begin: name=%s size=%lu target_slot=%s",
            file->name,
            (unsigned long)file->size,
            ota_manage_slot_name(ctx->requested_slot));

  if ((file->size_valid == 0U) || (file->size > ota_flash_get_slot_size(ctx->requested_slot)))
  {
    OTA_LOG_E("file size invalid: size=%lu size_valid=%u slot_size=%lu",
              (unsigned long)file->size,
              (unsigned int)file->size_valid,
              (unsigned long)ota_flash_get_slot_size(ctx->requested_slot));
    ctx->state = OTA_MANAGE_STATE_ERROR;
    return -1;
  }

  ctx->write_slot = ctx->requested_slot;
  ctx->image_size = file->size;
  ctx->image_crc32 = 0xFFFFFFFFUL;
  flash_status = ota_flash_begin_write(ctx->write_slot, ctx->image_size);
  if (flash_status != OTA_FLASH_OK)
  {
    OTA_LOG_E("flash begin write failed: status=%s(%d) slot=%s size=%lu",
              ota_manage_flash_status_name(flash_status),
              (int)flash_status,
              ota_manage_slot_name(ctx->write_slot),
              (unsigned long)ctx->image_size);
    ctx->state = OTA_MANAGE_STATE_ERROR;
    return -1;
  }

  ctx->file_started = 1U;
  ctx->state = OTA_MANAGE_STATE_RECEIVING;
  return 0;
}

static int ota_manage_on_file_data(const uint8_t *data, uint16_t len, uint32_t offset, void *user)
{
  ota_manage_context_t *ctx = (ota_manage_context_t *)user;
  ota_flash_status_t flash_status;
  if ((ctx == NULL) || (data == NULL) || (ctx->file_started == 0U))
  {
    OTA_LOG_E("file data invalid: ctx=%p data=%p started=%u off=%lu len=%u",
              (void *)ctx,
              (void *)data,
              (ctx == NULL) ? 0U : (unsigned int)ctx->file_started,
              (unsigned long)offset,
              (unsigned int)len);
    return -1;
  }

  flash_status = ota_flash_write(offset, data, len);
  if (flash_status != OTA_FLASH_OK)
  {
    OTA_LOG_E("flash write failed: status=%s(%d) off=%lu len=%u",
              ota_manage_flash_status_name(flash_status),
              (int)flash_status,
              (unsigned long)offset,
              (unsigned int)len);
    ctx->state = OTA_MANAGE_STATE_ERROR;
    return -1;
  }

  ctx->image_crc32 = ota_flash_crc32_update(ctx->image_crc32, data, len);

  return 0;
}

static void ota_manage_on_file_end(const ota_ymodem_file_info_t *file, void *user)
{
  ota_manage_context_t *ctx = (ota_manage_context_t *)user;
  uint32_t crc32;
  ota_flash_status_t flash_status;

  if ((ctx == NULL) || (file == NULL) || (ctx->file_started == 0U))
  {
    OTA_LOG_E("file end ignored: ctx=%p file=%p started=%u",
              (void *)ctx,
              (void *)file,
              (ctx == NULL) ? 0U : (unsigned int)ctx->file_started);
    return;
  }

  crc32 = ota_flash_crc32_finish(ctx->image_crc32);

  OTA_LOG_I("file end: received=%lu expect=%lu crc=0x%08lx",
            (unsigned long)file->bytes_received,
            (unsigned long)ctx->image_size,
            (unsigned long)crc32);

  if (file->size_valid == 0U)
  {
    OTA_LOG_E("file end failed: size invalid");
    ctx->state = OTA_MANAGE_STATE_ERROR;
    return;
  }

  if (file->bytes_received != ctx->image_size)
  {
    OTA_LOG_E("file end failed: size mismatch, got=%lu expect=%lu",
              (unsigned long)file->bytes_received,
              (unsigned long)ctx->image_size);
    ctx->state = OTA_MANAGE_STATE_ERROR;
    return;
  }

  flash_status = ota_flash_end_write(ctx->image_size, crc32);
  if (flash_status != OTA_FLASH_OK)
  {
    OTA_LOG_E("flash end write failed: status=%s(%d) size=%lu crc=0x%08lx",
              ota_manage_flash_status_name(flash_status),
              (int)flash_status,
              (unsigned long)ctx->image_size,
              (unsigned long)crc32);
    ota_manage_notify_error("invalid image in slot %s, rollback",
                            ota_manage_slot_name(ctx->write_slot));
    ctx->file_started = 0U;
    if (ota_manage_recover_boot_slot(ctx->write_slot) != 0U)
    {
      ctx->state = OTA_MANAGE_STATE_JUMP_APP;
    }
    else
    {
      ctx->state = OTA_MANAGE_STATE_ERROR;
    }
    return;
  }

  flash_status = ota_flash_mark_pending(ctx->write_slot, ctx->image_size, crc32);
  if (flash_status != OTA_FLASH_OK)
  {
    OTA_LOG_E("mark pending failed: status=%s(%d) slot=%s",
              ota_manage_flash_status_name(flash_status),
              (int)flash_status,
              ota_manage_slot_name(ctx->write_slot));
    ctx->state = OTA_MANAGE_STATE_ERROR;
    return;
  }

  ctx->state = OTA_MANAGE_STATE_COMPLETE;
  ctx->file_started = 0U;
  OTA_LOG_I("OTA image ready: active_slot=%s size=%lu crc=0x%08lx",
            ota_manage_slot_name(ctx->write_slot),
            (unsigned long)ctx->image_size,
            (unsigned long)crc32);
}

static void ota_manage_on_abort(ota_ymodem_status_t reason, void *user)
{
  ota_manage_context_t *ctx = (ota_manage_context_t *)user;

  OTA_LOG_E("YMODEM aborted: reason=%s(%d)",
            ota_manage_ymodem_status_name(reason),
            (int)reason);

  if (ctx != NULL)
  {
    ctx->state = OTA_MANAGE_STATE_ERROR;
  }
}

ota_manage_status_t ota_manage_service_init(void)
{
  ota_ymodem_protocol_config_t ymodem_config;
  ota_flash_meta_t meta;
  ota_flash_status_t flash_status;

  (void)memset(&g_ota_manage, 0, sizeof(g_ota_manage));
  g_ota_manage.state = OTA_MANAGE_STATE_INIT;
  g_ota_manage.write_slot = OTA_FLASH_SLOT_NONE;
  g_ota_manage.requested_slot = OTA_FLASH_SLOT_NONE;

  flash_status = ota_flash_service_init();
  if (flash_status != OTA_FLASH_OK)
  {
    OTA_LOG_E("flash service init failed: status=%s(%d)",
              ota_manage_flash_status_name(flash_status),
              (int)flash_status);
    g_ota_manage.state = OTA_MANAGE_STATE_ERROR;
    return OTA_MANAGE_ERR;
  }

  if ((ota_flash_read_meta(&meta) == OTA_FLASH_OK) &&
      (meta.ota_request == OTA_FLASH_OTA_REQUEST_UPDATE))
  {
    g_ota_manage.ota_requested = 1U;
    g_ota_manage.requested_slot = (ota_flash_slot_t)meta.target_slot;
    OTA_LOG_I("OTA request found: active_slot=%s target_slot=%s",
              ota_manage_slot_name((ota_flash_slot_t)meta.active_slot),
              ota_manage_slot_name(g_ota_manage.requested_slot));
  }

  (void)memset(&ymodem_config, 0, sizeof(ymodem_config));
  ymodem_config.send = ota_manage_ymodem_send;
  ymodem_config.on_file_begin = ota_manage_on_file_begin;
  ymodem_config.on_file_data = ota_manage_on_file_data;
  ymodem_config.on_file_end = ota_manage_on_file_end;
  ymodem_config.on_abort = ota_manage_on_abort;
  ymodem_config.user = &g_ota_manage;
  ymodem_config.max_errors = 10U;
  ymodem_config.packet_timeout_ms = OTA_MANAGE_PACKET_TIMEOUT_MS;
  ota_ymodem_protocol_init(&ymodem_config);

  return OTA_MANAGE_OK;
}

ota_manage_status_t ota_manage_service_start(void)
{
  ota_flash_slot_t active_slot = ota_flash_get_active_slot();
  ota_flash_status_t flash_status;
  ota_ymodem_status_t ymodem_status;

  if (g_ota_manage.ota_requested == 0U)
  {
    g_ota_manage.requested_slot = ota_flash_get_inactive_slot();
    g_ota_manage.crc_request_interval_ms = OTA_MANAGE_PROBE_CRC_REQUEST_MS;
    g_ota_manage.wait_transfer_timeout_ms = OTA_MANAGE_PROBE_WAIT_TRANSFER_TIMEOUT_MS;
  }
  else
  {
    g_ota_manage.crc_request_interval_ms = OTA_MANAGE_REQUEST_CRC_REQUEST_MS;
    g_ota_manage.wait_transfer_timeout_ms = OTA_MANAGE_REQUEST_WAIT_TRANSFER_TIMEOUT_MS;
  }

  if ((ota_flash_is_valid_slot(g_ota_manage.requested_slot) == 0U) ||
      (g_ota_manage.requested_slot == active_slot))
  {
    OTA_LOG_E("invalid OTA target: active_slot=%s target_slot=%s",
              ota_manage_slot_name(active_slot),
              ota_manage_slot_name(g_ota_manage.requested_slot));
    ota_manage_send_cancel();
    if (g_ota_manage.ota_requested != 0U)
    {
      flash_status = ota_flash_clear_ota_request();
      if (flash_status != OTA_FLASH_OK)
      {
        OTA_LOG_E("clear OTA request failed: status=%s(%d)",
                  ota_manage_flash_status_name(flash_status),
                  (int)flash_status);
      }
    }
    g_ota_manage.state = OTA_MANAGE_STATE_JUMP_APP;
    return OTA_MANAGE_ERR;
  }

  ymodem_status = ota_ymodem_protocol_start_receive();
  if (ymodem_status != OTA_YMODEM_STATUS_OK)
  {
    OTA_LOG_E("YMODEM start failed: status=%s(%d)",
              ota_manage_ymodem_status_name(ymodem_status),
              (int)ymodem_status);
    g_ota_manage.state = OTA_MANAGE_STATE_ERROR;
    return OTA_MANAGE_ERR;
  }

  g_ota_manage.last_crc_request_ms = HAL_GetTick();
  g_ota_manage.wait_transfer_start_ms = g_ota_manage.last_crc_request_ms;
  g_ota_manage.state = OTA_MANAGE_STATE_WAIT_TRANSFER;
  OTA_LOG_I("waiting YMODEM: request=%u target_slot=%s interval_ms=%lu timeout_ms=%lu",
            (unsigned int)g_ota_manage.ota_requested,
            ota_manage_slot_name(g_ota_manage.requested_slot),
            (unsigned long)g_ota_manage.crc_request_interval_ms,
            (unsigned long)g_ota_manage.wait_transfer_timeout_ms);
  return OTA_MANAGE_OK;
}

void ota_manage_service_process(uint32_t now_ms)
{
  const uint8_t *data;
  uint16_t len;

  while ((data = uart_service_port_get_ota_read_ptr(&len)) != NULL)
  {
    for (uint16_t i = 0U; i < len; ++i)
    {
      (void)ota_ymodem_protocol_input_byte(data[i]);
    }

    uart_service_port_consume_ota_data(len);
  }

  if ((g_ota_manage.state == OTA_MANAGE_STATE_WAIT_TRANSFER) &&
      (ota_ymodem_protocol_get_state() != OTA_YMODEM_RX_FIND_HEADER))
  {
    g_ota_manage.state = OTA_MANAGE_STATE_RECEIVING;
  }

  if (g_ota_manage.state == OTA_MANAGE_STATE_WAIT_TRANSFER)
  {
    uint32_t wait_elapsed_ms = now_ms - g_ota_manage.wait_transfer_start_ms;

    if (wait_elapsed_ms >= g_ota_manage.wait_transfer_timeout_ms)
    {
      OTA_LOG_E("waiting sender timeout: waited_ms=%lu timeout_ms=%lu",
                (unsigned long)wait_elapsed_ms,
                (unsigned long)g_ota_manage.wait_transfer_timeout_ms);
      ota_manage_send_cancel();

      if (g_ota_manage.ota_requested != 0U)
      {
        ota_flash_status_t flash_status = ota_flash_clear_ota_request();
        if (flash_status != OTA_FLASH_OK)
        {
          OTA_LOG_E("clear OTA request failed after wait timeout: status=%s(%d)",
                    ota_manage_flash_status_name(flash_status),
                    (int)flash_status);
        }
      }

      g_ota_manage.state = OTA_MANAGE_STATE_JUMP_APP;
    }
  }

  if ((g_ota_manage.state == OTA_MANAGE_STATE_WAIT_TRANSFER) &&
      ((uint32_t)(now_ms - g_ota_manage.last_crc_request_ms) >= g_ota_manage.crc_request_interval_ms))
  {
    uint8_t crc_request = OTA_YMODEM_CRC_REQUEST;
    (void)ota_ack_send(&crc_request, 1U);
    g_ota_manage.last_crc_request_ms = now_ms;
  }

  if ((g_ota_manage.state == OTA_MANAGE_STATE_WAIT_TRANSFER) ||
      (g_ota_manage.state == OTA_MANAGE_STATE_RECEIVING))
  {
    ota_ymodem_status_t ymodem_status = ota_ymodem_protocol_tick(now_ms);

    if (ymodem_status != OTA_YMODEM_STATUS_OK)
    {
      OTA_LOG_E("YMODEM tick failed: status=%s(%d)",
                ota_manage_ymodem_status_name(ymodem_status),
                (int)ymodem_status);
    }
  }

  if (g_ota_manage.state == OTA_MANAGE_STATE_COMPLETE)
  {
    g_ota_manage.state = OTA_MANAGE_STATE_JUMP_APP;
    OTA_LOG_I("OTA complete, jump to updated app");
  }

  if (g_ota_manage.state == OTA_MANAGE_STATE_JUMP_APP)
  {
    ota_manage_service_jump_to_active_app();
  }
}

ota_manage_state_t ota_manage_service_get_state(void)
{
  return g_ota_manage.state;
}

void ota_manage_service_jump_to_active_app(void)
{
  ota_flash_slot_t active_slot = ota_flash_get_active_slot();
  ota_flash_slot_t boot_slot;
  uint32_t app_addr = ota_flash_get_slot_address(active_slot);
  uint32_t app_stack;
  uint32_t app_reset;
  ota_app_entry_t app_entry;

  if (ota_manage_find_bootable_slot(active_slot, &boot_slot) != 0U)
  {
    if (boot_slot != active_slot)
    {
      OTA_LOG_E("active slot invalid, fallback to slot=%s",
                ota_manage_slot_name(boot_slot));
      (void)ota_manage_recover_boot_slot(OTA_FLASH_SLOT_NONE);
      active_slot = boot_slot;
      app_addr = ota_flash_get_slot_address(active_slot);
    }
  }
  else
  {
    OTA_LOG_E("no bootable app slot found");
    ota_manage_notify_error("no bootable app slot");
    g_ota_manage.state = OTA_MANAGE_STATE_ERROR;
    return;
  }

  if ((app_addr == 0U) || (ota_flash_is_valid_app(app_addr) == 0U))
  {
    uint32_t initial_sp = (app_addr == 0U) ? 0U : *((const uint32_t *)app_addr);
    uint32_t reset_raw = (app_addr == 0U) ? 0U : *((const uint32_t *)(app_addr + 4U));
    uint32_t reset_addr = reset_raw & ~1UL;
    uint32_t slot_size = ota_flash_get_slot_size(active_slot);
    uint32_t slot_end = app_addr + slot_size;

    OTA_LOG_E("active app invalid: slot=%s vector_base=0x%08lx sp=0x%08lx reset_raw=0x%08lx reset_addr=0x%08lx",
              ota_manage_slot_name(active_slot),
              (unsigned long)app_addr,
              (unsigned long)initial_sp,
              (unsigned long)reset_raw,
              (unsigned long)reset_addr);
    OTA_LOG_E("active app check: base_valid=%u sp_in_ram=%u reset_thumb=%u reset_in_slot=%u slot_end=0x%08lx",
              (unsigned int)(app_addr != 0U),
              (unsigned int)ota_manage_sp_in_ram(initial_sp),
              (unsigned int)((reset_raw & 1UL) != 0UL),
              (unsigned int)((slot_size != 0U) && (reset_addr >= app_addr) && (reset_addr < slot_end)),
              (unsigned long)slot_end);
    g_ota_manage.state = OTA_MANAGE_STATE_ERROR;
    return;
  }

  app_stack = *((const uint32_t *)app_addr);
  app_reset = *((const uint32_t *)(app_addr + 4U));
  app_entry = (ota_app_entry_t)app_reset;

  ota_manage_disable_all_irqs();

  HAL_UART_DeInit(&huart5);
  HAL_UART_DeInit(&huart1);
  HAL_UART_DeInit(&huart8);
  HAL_RCC_DeInit();
  HAL_DeInit();

  SCB->VTOR = app_addr;
  __DSB();
  __ISB();
  __set_MSP(app_stack);

  app_entry();

  while (1)
  {
  }
}
