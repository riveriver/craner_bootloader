#include "ota_manage_service.h"

#include <stddef.h>
#include <string.h>

#include "main.h"
#include "ota_flash_service.h"
#include "ota_ymodem_protocol.h"
#include "uart_service.h"

#define OTA_MANAGE_UART_NAME          "ota"
#define OTA_MANAGE_CRC_REQUEST_MS     5000UL
#define OTA_MANAGE_PACKET_TIMEOUT_MS  1000UL

typedef void (*ota_app_entry_t)(void);

typedef struct
{
  ota_manage_state_t state;
  ota_flash_slot_t write_slot;
  ota_flash_slot_t requested_slot;
  uint32_t image_size;
  uint32_t image_crc32;
  uint32_t last_crc_request_ms;
  uint8_t ota_requested;
  uint8_t file_started;
} ota_manage_context_t;

static ota_manage_context_t g_ota_manage;

static int ota_manage_ymodem_send(const uint8_t *data, uint16_t len, void *user)
{
  (void)user;
  return (uart_service_send_by_name(OTA_MANAGE_UART_NAME, data, len) == UART_SERVICE_OK) ? 0 : -1;
}

static void ota_manage_send_cancel(void)
{
  static const uint8_t cancel[2] = {OTA_YMODEM_CAN, OTA_YMODEM_CAN};

  (void)uart_service_send_by_name(OTA_MANAGE_UART_NAME, cancel, (uint16_t)sizeof(cancel));
}

static int ota_manage_on_file_begin(const ota_ymodem_file_info_t *file, void *user)
{
  ota_manage_context_t *ctx = (ota_manage_context_t *)user;

  if ((ctx == NULL) || (file == NULL) || (file->size == 0U))
  {
    return -1;
  }

  if ((file->size_valid == 0U) || (file->size > ota_flash_get_slot_size(ctx->requested_slot)))
  {
    ctx->state = OTA_MANAGE_STATE_ERROR;
    return -1;
  }

  ctx->write_slot = ctx->requested_slot;
  ctx->image_size = file->size;
  ctx->image_crc32 = 0xFFFFFFFFUL;

  if (ota_flash_begin_write(ctx->write_slot, ctx->image_size) != OTA_FLASH_OK)
  {
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

  if ((ctx == NULL) || (data == NULL) || (ctx->file_started == 0U))
  {
    return -1;
  }

  if (ota_flash_write(offset, data, len) != OTA_FLASH_OK)
  {
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

  if ((ctx == NULL) || (file == NULL) || (ctx->file_started == 0U))
  {
    return;
  }

  crc32 = ota_flash_crc32_finish(ctx->image_crc32);

  if ((file->size_valid == 0U) || (file->bytes_received != ctx->image_size) ||
      (ota_flash_end_write(ctx->image_size, crc32) != OTA_FLASH_OK) ||
      (ota_flash_mark_pending(ctx->write_slot, ctx->image_size, crc32) != OTA_FLASH_OK))
  {
    ctx->state = OTA_MANAGE_STATE_ERROR;
    return;
  }

  ctx->state = OTA_MANAGE_STATE_COMPLETE;
}

static void ota_manage_on_abort(ota_ymodem_status_t reason, void *user)
{
  ota_manage_context_t *ctx = (ota_manage_context_t *)user;

  (void)reason;

  if (ctx != NULL)
  {
    ctx->state = OTA_MANAGE_STATE_ERROR;
  }
}

ota_manage_status_t ota_manage_service_init(void)
{
  ota_ymodem_protocol_config_t ymodem_config;
  ota_flash_meta_t meta;

  (void)memset(&g_ota_manage, 0, sizeof(g_ota_manage));
  g_ota_manage.state = OTA_MANAGE_STATE_INIT;
  g_ota_manage.write_slot = OTA_FLASH_SLOT_NONE;
  g_ota_manage.requested_slot = OTA_FLASH_SLOT_NONE;

  if (ota_flash_service_init() != OTA_FLASH_OK)
  {
    g_ota_manage.state = OTA_MANAGE_STATE_ERROR;
    return OTA_MANAGE_ERR;
  }

  if ((ota_flash_read_meta(&meta) == OTA_FLASH_OK) &&
      (meta.ota_request == OTA_FLASH_OTA_REQUEST_UPDATE))
  {
    g_ota_manage.ota_requested = 1U;
    g_ota_manage.requested_slot = (ota_flash_slot_t)meta.target_slot;
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

  if (g_ota_manage.ota_requested == 0U)
  {
    g_ota_manage.state = OTA_MANAGE_STATE_JUMP_APP;
    return OTA_MANAGE_OK;
  }

  if ((ota_flash_is_valid_slot(g_ota_manage.requested_slot) == 0U) ||
      (g_ota_manage.requested_slot == active_slot))
  {
    ota_manage_send_cancel();
    (void)ota_flash_clear_ota_request();
    g_ota_manage.state = OTA_MANAGE_STATE_JUMP_APP;
    return OTA_MANAGE_ERR;
  }

  if (ota_ymodem_protocol_start_receive() != OTA_YMODEM_STATUS_OK)
  {
    g_ota_manage.state = OTA_MANAGE_STATE_ERROR;
    return OTA_MANAGE_ERR;
  }

  g_ota_manage.last_crc_request_ms = HAL_GetTick();
  g_ota_manage.state = OTA_MANAGE_STATE_WAIT_TRANSFER;
  return OTA_MANAGE_OK;
}

void ota_manage_service_process(uint32_t now_ms)
{
  if ((g_ota_manage.state == OTA_MANAGE_STATE_WAIT_TRANSFER) &&
      (ota_ymodem_protocol_get_state() != OTA_YMODEM_RX_FIND_HEADER))
  {
    g_ota_manage.state = OTA_MANAGE_STATE_RECEIVING;
  }

  if ((g_ota_manage.state == OTA_MANAGE_STATE_WAIT_TRANSFER) &&
      ((uint32_t)(now_ms - g_ota_manage.last_crc_request_ms) >= OTA_MANAGE_CRC_REQUEST_MS))
  {
    uint8_t crc_request = OTA_YMODEM_CRC_REQUEST;

    (void)uart_service_send_by_name(OTA_MANAGE_UART_NAME, &crc_request, 1U);
    g_ota_manage.last_crc_request_ms = now_ms;
  }

  if ((g_ota_manage.state == OTA_MANAGE_STATE_WAIT_TRANSFER) ||
      (g_ota_manage.state == OTA_MANAGE_STATE_RECEIVING))
  {
    (void)ota_ymodem_protocol_tick(now_ms);
  }

  if (g_ota_manage.state == OTA_MANAGE_STATE_COMPLETE)
  {
    g_ota_manage.state = OTA_MANAGE_STATE_JUMP_APP;
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
  uint32_t app_addr = ota_flash_get_slot_address(ota_flash_get_active_slot());
  uint32_t app_stack;
  uint32_t app_reset;
  ota_app_entry_t app_entry;

  if ((app_addr == 0U) || (ota_flash_is_valid_app(app_addr) == 0U))
  {
    g_ota_manage.state = OTA_MANAGE_STATE_ERROR;
    return;
  }

  app_stack = *((const uint32_t *)app_addr);
  app_reset = *((const uint32_t *)(app_addr + 4U));
  app_entry = (ota_app_entry_t)app_reset;

  __disable_irq();

  HAL_UART_DeInit(&huart1);
  HAL_UART_DeInit(&huart8);
  HAL_RCC_DeInit();
  HAL_DeInit();

  SysTick->CTRL = 0U;
  SysTick->LOAD = 0U;
  SysTick->VAL = 0U;

  SCB->VTOR = app_addr;
  __set_MSP(app_stack);
  __enable_irq();

  app_entry();

  while (1)
  {
  }
}
