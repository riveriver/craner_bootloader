#include "ota_flash_service.h"

#include <stddef.h>
#include <string.h>

#include "log_service.h"
#include "stm32h7xx_hal.h"

#define OTA_FLASH_META_MAGIC          0x4F54414DUL
#define OTA_FLASH_META_STRUCT_VERSION 2UL
#define OTA_FLASH_SECTOR_SIZE         0x00020000UL
#define OTA_FLASH_BANK1_BASE          0x08000000UL
#define OTA_FLASH_BANK2_BASE          0x08100000UL
#define OTA_FLASH_BANK_SIZE           0x00100000UL
#define OTA_FLASH_PROGRAM_UNIT        32U
#define OTA_FLASH_ERASE_VOLTAGE       FLASH_VOLTAGE_RANGE_3

#define OTA_FLASH_LOG_I(format, ...) log_printf("[I][OTA_FLASH] " format "\r\n", ##__VA_ARGS__)
#define OTA_FLASH_LOG_E(format, ...) log_printf("[E][OTA_FLASH] " format "\r\n", ##__VA_ARGS__)

typedef struct
{
  ota_flash_slot_t slot;
  uint32_t base;
  uint32_t size;
  uint32_t written;
  uint32_t crc32;
  uint8_t active;
  uint8_t program_buf[OTA_FLASH_PROGRAM_UNIT];
  uint8_t program_buf_len;
} ota_flash_write_context_t;

static ota_flash_write_context_t g_flash_write;

static const char *ota_flash_slot_name(ota_flash_slot_t slot)
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

static uint8_t ota_flash_slot_index(ota_flash_slot_t slot)
{
  return (slot == OTA_FLASH_SLOT_B) ? 1U : 0U;
}

static uint32_t ota_flash_meta_crc(const ota_flash_meta_t *meta)
{
  ota_flash_meta_t temp;

  if (meta == NULL)
  {
    return 0U;
  }

  temp = *meta;
  temp.meta_crc32 = 0U;
  return ota_flash_crc32_finish(ota_flash_crc32_update(0xFFFFFFFFUL,
                                                       (const uint8_t *)&temp,
                                                       (uint32_t)sizeof(temp)));
}

static uint8_t ota_flash_addr_in_flash(uint32_t address)
{
  return (uint8_t)((address >= OTA_FLASH_BANK1_BASE) &&
                   (address < (OTA_FLASH_BANK1_BASE + (OTA_FLASH_BANK_SIZE * 2UL))));
}

static ota_flash_status_t ota_flash_get_bank_sector(uint32_t address,
                                                    uint32_t *bank,
                                                    uint32_t *sector)
{
  uint32_t bank_base;

  if ((bank == NULL) || (sector == NULL) || (ota_flash_addr_in_flash(address) == 0U))
  {
    return OTA_FLASH_ERR_PARAM;
  }

  if (address < OTA_FLASH_BANK2_BASE)
  {
    *bank = FLASH_BANK_1;
    bank_base = OTA_FLASH_BANK1_BASE;
  }
  else
  {
    *bank = FLASH_BANK_2;
    bank_base = OTA_FLASH_BANK2_BASE;
  }

  *sector = (address - bank_base) / OTA_FLASH_SECTOR_SIZE;
  return OTA_FLASH_OK;
}

static ota_flash_status_t ota_flash_erase_range(uint32_t address, uint32_t len)
{
  FLASH_EraseInitTypeDef erase = {0};
  uint32_t error = 0U;
  uint32_t bank;
  uint32_t first_sector;
  uint32_t last_sector;
  uint32_t end_addr;

  if ((len == 0U) || ((address % OTA_FLASH_SECTOR_SIZE) != 0U))
  {
    OTA_FLASH_LOG_E("erase param invalid: addr=0x%08lx len=%lu",
                    (unsigned long)address,
                    (unsigned long)len);
    return OTA_FLASH_ERR_PARAM;
  }

  end_addr = address + len - 1U;
  if ((end_addr < address) || (ota_flash_get_bank_sector(address, &bank, &first_sector) != OTA_FLASH_OK) ||
      (ota_flash_get_bank_sector(end_addr, &bank, &last_sector) != OTA_FLASH_OK))
  {
    OTA_FLASH_LOG_E("erase range invalid: addr=0x%08lx len=%lu end=0x%08lx",
                    (unsigned long)address,
                    (unsigned long)len,
                    (unsigned long)end_addr);
    return OTA_FLASH_ERR_RANGE;
  }

  erase.TypeErase = FLASH_TYPEERASE_SECTORS;
  erase.Banks = bank;
  erase.Sector = first_sector;
  erase.NbSectors = (last_sector - first_sector) + 1U;
  erase.VoltageRange = OTA_FLASH_ERASE_VOLTAGE;

  OTA_FLASH_LOG_I("erase start: addr=0x%08lx len=%lu bank=%lu sector=%lu count=%lu",
                  (unsigned long)address,
                  (unsigned long)len,
                  (unsigned long)bank,
                  (unsigned long)first_sector,
                  (unsigned long)erase.NbSectors);

  if (HAL_FLASH_Unlock() != HAL_OK)
  {
    OTA_FLASH_LOG_E("flash unlock failed before erase: err=0x%08lx",
                    (unsigned long)HAL_FLASH_GetError());
    return OTA_FLASH_ERR_HAL;
  }

  if (HAL_FLASHEx_Erase(&erase, &error) != HAL_OK)
  {
    OTA_FLASH_LOG_E("erase failed: addr=0x%08lx error_sector=%lu hal_err=0x%08lx",
                    (unsigned long)address,
                    (unsigned long)error,
                    (unsigned long)HAL_FLASH_GetError());
    (void)HAL_FLASH_Lock();
    return OTA_FLASH_ERR_HAL;
  }

  if (HAL_FLASH_Lock() != HAL_OK)
  {
    OTA_FLASH_LOG_E("flash lock failed after erase: err=0x%08lx",
                    (unsigned long)HAL_FLASH_GetError());
    return OTA_FLASH_ERR_HAL;
  }

  OTA_FLASH_LOG_I("erase done: addr=0x%08lx len=%lu",
                  (unsigned long)address,
                  (unsigned long)len);
  return OTA_FLASH_OK;
}

static ota_flash_status_t ota_flash_program_flashword(uint32_t address, const uint8_t *data)
{
  uint8_t aligned[OTA_FLASH_PROGRAM_UNIT] __attribute__((aligned(32)));

  if ((data == NULL) || ((address % OTA_FLASH_PROGRAM_UNIT) != 0U))
  {
    OTA_FLASH_LOG_E("program param invalid: addr=0x%08lx data=%p",
                    (unsigned long)address,
                    (void *)data);
    return OTA_FLASH_ERR_PARAM;
  }

  (void)memcpy(aligned, data, OTA_FLASH_PROGRAM_UNIT);

  if (HAL_FLASH_Unlock() != HAL_OK)
  {
    OTA_FLASH_LOG_E("flash unlock failed before program: addr=0x%08lx err=0x%08lx",
                    (unsigned long)address,
                    (unsigned long)HAL_FLASH_GetError());
    return OTA_FLASH_ERR_HAL;
  }

  if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_FLASHWORD, address, (uint32_t)aligned) != HAL_OK)
  {
    OTA_FLASH_LOG_E("program failed: addr=0x%08lx hal_err=0x%08lx",
                    (unsigned long)address,
                    (unsigned long)HAL_FLASH_GetError());
    (void)HAL_FLASH_Lock();
    return OTA_FLASH_ERR_HAL;
  }

  if (HAL_FLASH_Lock() != HAL_OK)
  {
    OTA_FLASH_LOG_E("flash lock failed after program: addr=0x%08lx err=0x%08lx",
                    (unsigned long)address,
                    (unsigned long)HAL_FLASH_GetError());
    return OTA_FLASH_ERR_HAL;
  }

  return OTA_FLASH_OK;
}

static ota_flash_status_t ota_flash_flush_program_buf(uint8_t final_flush)
{
  uint32_t address;

  if (g_flash_write.program_buf_len == 0U)
  {
    return OTA_FLASH_OK;
  }

  if ((g_flash_write.program_buf_len < OTA_FLASH_PROGRAM_UNIT) && (final_flush == 0U))
  {
    return OTA_FLASH_OK;
  }

  while (g_flash_write.program_buf_len < OTA_FLASH_PROGRAM_UNIT)
  {
    g_flash_write.program_buf[g_flash_write.program_buf_len] = 0xFFU;
    ++g_flash_write.program_buf_len;
  }

  address = g_flash_write.base + g_flash_write.written;
  if (ota_flash_program_flashword(address, g_flash_write.program_buf) != OTA_FLASH_OK)
  {
    OTA_FLASH_LOG_E("flush program buffer failed: addr=0x%08lx written=%lu pending=%u final=%u",
                    (unsigned long)address,
                    (unsigned long)g_flash_write.written,
                    (unsigned int)g_flash_write.program_buf_len,
                    (unsigned int)final_flush);
    return OTA_FLASH_ERR_HAL;
  }

  g_flash_write.written += OTA_FLASH_PROGRAM_UNIT;
  g_flash_write.program_buf_len = 0U;
  return OTA_FLASH_OK;
}

ota_flash_status_t ota_flash_service_init(void)
{
  ota_flash_meta_t meta;
  ota_flash_status_t status;

  OTA_FLASH_LOG_I("service init");
  (void)memset(&g_flash_write, 0, sizeof(g_flash_write));

  status = ota_flash_read_meta(&meta);
  if (status == OTA_FLASH_OK)
  {
    OTA_FLASH_LOG_I("meta ok: active_slot=%s request=%lu target=%s boot_count=%lu",
                    ota_flash_slot_name((ota_flash_slot_t)meta.active_slot),
                    (unsigned long)meta.ota_request,
                    ota_flash_slot_name((ota_flash_slot_t)meta.target_slot),
                    (unsigned long)meta.boot_count);
    return OTA_FLASH_OK;
  }

  OTA_FLASH_LOG_E("meta invalid, write default meta: status=%d", (int)status);
  ota_flash_make_default_meta(&meta);
  status = ota_flash_write_meta(&meta);
  if (status != OTA_FLASH_OK)
  {
    OTA_FLASH_LOG_E("write default meta failed: status=%d", (int)status);
  }
  else
  {
    OTA_FLASH_LOG_I("default meta written");
  }

  return status;
}

void ota_flash_make_default_meta(ota_flash_meta_t *meta)
{
  if (meta == NULL)
  {
    return;
  }

  (void)memset(meta, 0, sizeof(*meta));
  meta->magic = OTA_FLASH_META_MAGIC;
  meta->struct_version = OTA_FLASH_META_STRUCT_VERSION;
  meta->ota_request = OTA_FLASH_OTA_REQUEST_NONE;
  meta->target_slot = OTA_FLASH_SLOT_NONE;
  meta->active_slot = OTA_FLASH_SLOT_A;
  meta->image[0].address = OTA_FLASH_SLOT_A_ADDR;
  meta->image[0].state = OTA_FLASH_IMAGE_EMPTY;
  meta->image[1].address = OTA_FLASH_SLOT_B_ADDR;
  meta->image[1].state = OTA_FLASH_IMAGE_EMPTY;
  meta->meta_crc32 = ota_flash_meta_crc(meta);
}

ota_flash_status_t ota_flash_read_meta(ota_flash_meta_t *meta)
{
  const ota_flash_meta_t *stored = (const ota_flash_meta_t *)OTA_FLASH_META_ADDR;

  if (meta == NULL)
  {
    OTA_FLASH_LOG_E("read meta param invalid");
    return OTA_FLASH_ERR_PARAM;
  }

  (void)memcpy(meta, stored, sizeof(*meta));

  if ((meta->magic != OTA_FLASH_META_MAGIC) ||
      (meta->struct_version != OTA_FLASH_META_STRUCT_VERSION) ||
      (meta->meta_crc32 != ota_flash_meta_crc(meta)))
  {
    OTA_FLASH_LOG_E("read meta invalid: magic=0x%08lx ver=%lu crc=0x%08lx calc=0x%08lx",
                    (unsigned long)meta->magic,
                    (unsigned long)meta->struct_version,
                    (unsigned long)meta->meta_crc32,
                    (unsigned long)ota_flash_meta_crc(meta));
    return OTA_FLASH_ERR_META;
  }

  if ((meta->active_slot != OTA_FLASH_SLOT_A) && (meta->active_slot != OTA_FLASH_SLOT_B))
  {
    OTA_FLASH_LOG_E("read meta invalid active_slot=%lu", (unsigned long)meta->active_slot);
    return OTA_FLASH_ERR_META;
  }

  if ((meta->ota_request != OTA_FLASH_OTA_REQUEST_NONE) &&
      (meta->ota_request != OTA_FLASH_OTA_REQUEST_UPDATE))
  {
    OTA_FLASH_LOG_E("read meta invalid request=%lu", (unsigned long)meta->ota_request);
    return OTA_FLASH_ERR_META;
  }

  return OTA_FLASH_OK;
}

ota_flash_status_t ota_flash_write_meta(const ota_flash_meta_t *meta)
{
  ota_flash_meta_t writable;
  uint8_t buffer[OTA_FLASH_PROGRAM_UNIT];
  uint32_t offset = 0U;

  if (meta == NULL)
  {
    OTA_FLASH_LOG_E("write meta param invalid");
    return OTA_FLASH_ERR_PARAM;
  }

  writable = *meta;
  writable.magic = OTA_FLASH_META_MAGIC;
  writable.struct_version = OTA_FLASH_META_STRUCT_VERSION;
  writable.meta_crc32 = ota_flash_meta_crc(&writable);

  OTA_FLASH_LOG_I("write meta: active_slot=%s request=%lu target=%s boot_count=%lu",
                  ota_flash_slot_name((ota_flash_slot_t)writable.active_slot),
                  (unsigned long)writable.ota_request,
                  ota_flash_slot_name((ota_flash_slot_t)writable.target_slot),
                  (unsigned long)writable.boot_count);

  if (ota_flash_erase_range(OTA_FLASH_META_ADDR, OTA_FLASH_META_SIZE) != OTA_FLASH_OK)
  {
    OTA_FLASH_LOG_E("write meta failed: erase meta sector failed");
    return OTA_FLASH_ERR_HAL;
  }

  while (offset < sizeof(writable))
  {
    uint32_t copy_len = sizeof(writable) - offset;

    if (copy_len > OTA_FLASH_PROGRAM_UNIT)
    {
      copy_len = OTA_FLASH_PROGRAM_UNIT;
    }

    (void)memset(buffer, 0xFF, sizeof(buffer));
    (void)memcpy(buffer, ((const uint8_t *)&writable) + offset, copy_len);

    if (ota_flash_program_flashword(OTA_FLASH_META_ADDR + offset, buffer) != OTA_FLASH_OK)
    {
      OTA_FLASH_LOG_E("write meta failed: program offset=%lu", (unsigned long)offset);
      return OTA_FLASH_ERR_HAL;
    }

    offset += OTA_FLASH_PROGRAM_UNIT;
  }

  OTA_FLASH_LOG_I("write meta done: crc=0x%08lx", (unsigned long)writable.meta_crc32);
  return OTA_FLASH_OK;
}

ota_flash_slot_t ota_flash_get_active_slot(void)
{
  ota_flash_meta_t meta;

  if (ota_flash_read_meta(&meta) != OTA_FLASH_OK)
  {
    OTA_FLASH_LOG_E("active slot fallback to A, meta read failed");
    return OTA_FLASH_SLOT_A;
  }

  return (ota_flash_slot_t)meta.active_slot;
}

ota_flash_slot_t ota_flash_get_inactive_slot(void)
{
  return (ota_flash_get_active_slot() == OTA_FLASH_SLOT_A) ? OTA_FLASH_SLOT_B : OTA_FLASH_SLOT_A;
}

uint8_t ota_flash_is_valid_slot(ota_flash_slot_t slot)
{
  return (uint8_t)((slot == OTA_FLASH_SLOT_A) || (slot == OTA_FLASH_SLOT_B));
}

uint32_t ota_flash_get_slot_address(ota_flash_slot_t slot)
{
  if (slot == OTA_FLASH_SLOT_A)
  {
    return OTA_FLASH_SLOT_A_ADDR;
  }

  if (slot == OTA_FLASH_SLOT_B)
  {
    return OTA_FLASH_SLOT_B_ADDR;
  }

  return 0U;
}

uint32_t ota_flash_get_slot_size(ota_flash_slot_t slot)
{
  if ((slot == OTA_FLASH_SLOT_A) || (slot == OTA_FLASH_SLOT_B))
  {
    return OTA_FLASH_SLOT_SIZE;
  }

  return 0U;
}

uint8_t ota_flash_is_valid_app(uint32_t app_addr)
{
  uint32_t sp = *((const uint32_t *)app_addr);
  uint32_t reset = *((const uint32_t *)(app_addr + 4U));
  uint8_t sp_in_ram = 0U;

  if (((sp > 0x20000000UL) && (sp <= 0x20020000UL)) ||
      ((sp > 0x24000000UL) && (sp <= 0x24080000UL)) ||
      ((sp > 0x30000000UL) && (sp <= 0x30048000UL)) ||
      ((sp > 0x38000000UL) && (sp <= 0x38010000UL)))
  {
    sp_in_ram = 1U;
  }

  return (uint8_t)((sp_in_ram != 0U) &&
                   (reset >= app_addr) &&
                   (reset < (app_addr + OTA_FLASH_SLOT_SIZE)));
}

ota_flash_status_t ota_flash_request_ota(ota_flash_slot_t target_slot)
{
  ota_flash_meta_t meta;
  ota_flash_status_t status;

  if (ota_flash_is_valid_slot(target_slot) == 0U)
  {
    OTA_FLASH_LOG_E("request OTA invalid slot=%s", ota_flash_slot_name(target_slot));
    return OTA_FLASH_ERR_PARAM;
  }

  status = ota_flash_read_meta(&meta);
  if (status != OTA_FLASH_OK)
  {
    OTA_FLASH_LOG_E("request OTA read meta failed, use default: status=%d", (int)status);
    ota_flash_make_default_meta(&meta);
  }

  meta.ota_request = OTA_FLASH_OTA_REQUEST_UPDATE;
  meta.target_slot = target_slot;
  OTA_FLASH_LOG_I("request OTA: target_slot=%s", ota_flash_slot_name(target_slot));

  status = ota_flash_write_meta(&meta);
  if (status != OTA_FLASH_OK)
  {
    OTA_FLASH_LOG_E("request OTA write meta failed: status=%d", (int)status);
  }

  return status;
}

ota_flash_status_t ota_flash_clear_ota_request(void)
{
  ota_flash_meta_t meta;
  ota_flash_status_t status;

  status = ota_flash_read_meta(&meta);
  if (status != OTA_FLASH_OK)
  {
    OTA_FLASH_LOG_E("clear OTA request read meta failed: status=%d", (int)status);
    return OTA_FLASH_ERR_META;
  }

  meta.ota_request = OTA_FLASH_OTA_REQUEST_NONE;
  meta.target_slot = OTA_FLASH_SLOT_NONE;
  OTA_FLASH_LOG_I("clear OTA request");

  status = ota_flash_write_meta(&meta);
  if (status != OTA_FLASH_OK)
  {
    OTA_FLASH_LOG_E("clear OTA request write meta failed: status=%d", (int)status);
  }

  return status;
}

ota_flash_status_t ota_flash_begin_write(ota_flash_slot_t slot, uint32_t image_size)
{
  uint32_t base = ota_flash_get_slot_address(slot);
  uint32_t slot_size = ota_flash_get_slot_size(slot);

  OTA_FLASH_LOG_I("begin write: slot=%s base=0x%08lx image_size=%lu slot_size=%lu",
                  ota_flash_slot_name(slot),
                  (unsigned long)base,
                  (unsigned long)image_size,
                  (unsigned long)slot_size);

  if ((base == 0U) || (image_size == 0U) || (image_size > slot_size))
  {
    OTA_FLASH_LOG_E("begin write range invalid: slot=%s base=0x%08lx image_size=%lu slot_size=%lu",
                    ota_flash_slot_name(slot),
                    (unsigned long)base,
                    (unsigned long)image_size,
                    (unsigned long)slot_size);
    return OTA_FLASH_ERR_RANGE;
  }

  if (ota_flash_erase_range(base, slot_size) != OTA_FLASH_OK)
  {
    OTA_FLASH_LOG_E("begin write failed: erase slot=%s base=0x%08lx size=%lu",
                    ota_flash_slot_name(slot),
                    (unsigned long)base,
                    (unsigned long)slot_size);
    return OTA_FLASH_ERR_HAL;
  }

  (void)memset(&g_flash_write, 0, sizeof(g_flash_write));
  g_flash_write.slot = slot;
  g_flash_write.base = base;
  g_flash_write.size = image_size;
  g_flash_write.crc32 = 0xFFFFFFFFUL;
  g_flash_write.active = 1U;
  OTA_FLASH_LOG_I("begin write done: slot=%s", ota_flash_slot_name(slot));
  return OTA_FLASH_OK;
}

ota_flash_status_t ota_flash_write(uint32_t offset, const uint8_t *data, uint32_t len)
{
  uint32_t pos = 0U;

  if ((g_flash_write.active == 0U) || (data == NULL) || (offset != (g_flash_write.written + g_flash_write.program_buf_len)))
  {
    OTA_FLASH_LOG_E("write state invalid: active=%u data=%p off=%lu expect=%lu",
                    (unsigned int)g_flash_write.active,
                    (void *)data,
                    (unsigned long)offset,
                    (unsigned long)(g_flash_write.written + g_flash_write.program_buf_len));
    return OTA_FLASH_ERR_STATE;
  }

  if ((offset + len < offset) || ((offset + len) > g_flash_write.size))
  {
    OTA_FLASH_LOG_E("write range invalid: off=%lu len=%u size=%lu",
                    (unsigned long)offset,
                    (unsigned int)len,
                    (unsigned long)g_flash_write.size);
    return OTA_FLASH_ERR_RANGE;
  }

  g_flash_write.crc32 = ota_flash_crc32_update(g_flash_write.crc32, data, len);

  while (pos < len)
  {
    uint32_t room = OTA_FLASH_PROGRAM_UNIT - g_flash_write.program_buf_len;
    uint32_t copy_len = len - pos;

    if (copy_len > room)
    {
      copy_len = room;
    }

    (void)memcpy(&g_flash_write.program_buf[g_flash_write.program_buf_len], &data[pos], copy_len);
    g_flash_write.program_buf_len += (uint8_t)copy_len;
    pos += copy_len;

    if (ota_flash_flush_program_buf(0U) != OTA_FLASH_OK)
    {
      OTA_FLASH_LOG_E("write failed during flush: off=%lu len=%u pos=%lu",
                      (unsigned long)offset,
                      (unsigned int)len,
                      (unsigned long)pos);
      return OTA_FLASH_ERR_HAL;
    }
  }

  return OTA_FLASH_OK;
}

ota_flash_status_t ota_flash_end_write(uint32_t image_size, uint32_t image_crc32)
{
  if ((g_flash_write.active == 0U) || (image_size != g_flash_write.size))
  {
    OTA_FLASH_LOG_E("end write state invalid: active=%u image_size=%lu expect=%lu",
                    (unsigned int)g_flash_write.active,
                    (unsigned long)image_size,
                    (unsigned long)g_flash_write.size);
    return OTA_FLASH_ERR_STATE;
  }

  if (ota_flash_flush_program_buf(1U) != OTA_FLASH_OK)
  {
    OTA_FLASH_LOG_E("end write failed: final flush failed");
    return OTA_FLASH_ERR_HAL;
  }

  if (ota_flash_crc32_finish(g_flash_write.crc32) != image_crc32)
  {
    OTA_FLASH_LOG_E("end write CRC mismatch: calc=0x%08lx expect=0x%08lx",
                    (unsigned long)ota_flash_crc32_finish(g_flash_write.crc32),
                    (unsigned long)image_crc32);
    g_flash_write.active = 0U;
    return OTA_FLASH_ERR_META;
  }

  if (ota_flash_is_valid_app(g_flash_write.base) == 0U)
  {
    OTA_FLASH_LOG_E("end write invalid app vector: base=0x%08lx sp=0x%08lx reset=0x%08lx",
                    (unsigned long)g_flash_write.base,
                    (unsigned long)(*((const uint32_t *)g_flash_write.base)),
                    (unsigned long)(*((const uint32_t *)(g_flash_write.base + 4U))));
    g_flash_write.active = 0U;
    return OTA_FLASH_ERR_META;
  }

  g_flash_write.active = 0U;
  OTA_FLASH_LOG_I("end write done: slot=%s size=%lu crc=0x%08lx",
                  ota_flash_slot_name(g_flash_write.slot),
                  (unsigned long)image_size,
                  (unsigned long)image_crc32);
  return OTA_FLASH_OK;
}

ota_flash_status_t ota_flash_mark_pending(ota_flash_slot_t slot,
                                          uint32_t image_size,
                                          uint32_t image_crc32)
{
  ota_flash_meta_t meta;
  uint8_t index;

  if ((slot != OTA_FLASH_SLOT_A) && (slot != OTA_FLASH_SLOT_B))
  {
    OTA_FLASH_LOG_E("mark pending invalid slot=%s", ota_flash_slot_name(slot));
    return OTA_FLASH_ERR_PARAM;
  }

  if (ota_flash_read_meta(&meta) != OTA_FLASH_OK)
  {
    OTA_FLASH_LOG_E("mark pending read meta failed, use default");
    ota_flash_make_default_meta(&meta);
  }

  index = ota_flash_slot_index(slot);
  meta.image[index].address = ota_flash_get_slot_address(slot);
  meta.image[index].size = image_size;
  meta.image[index].crc32 = image_crc32;
  meta.image[index].state = OTA_FLASH_IMAGE_VALID;
  meta.active_slot = slot;
  meta.ota_request = OTA_FLASH_OTA_REQUEST_NONE;
  meta.target_slot = OTA_FLASH_SLOT_NONE;
  ++meta.boot_count;

  OTA_FLASH_LOG_I("mark pending: slot=%s size=%lu crc=0x%08lx boot_count=%lu",
                  ota_flash_slot_name(slot),
                  (unsigned long)image_size,
                  (unsigned long)image_crc32,
                  (unsigned long)meta.boot_count);

  return ota_flash_write_meta(&meta);
}

uint32_t ota_flash_crc32_update(uint32_t crc, const uint8_t *data, uint32_t len)
{
  for (uint32_t i = 0U; i < len; ++i)
  {
    crc ^= data[i];
    for (uint8_t bit = 0U; bit < 8U; ++bit)
    {
      if ((crc & 1UL) != 0UL)
      {
        crc = (crc >> 1U) ^ 0xEDB88320UL;
      }
      else
      {
        crc >>= 1U;
      }
    }
  }

  return crc;
}

uint32_t ota_flash_crc32_finish(uint32_t crc)
{
  return crc ^ 0xFFFFFFFFUL;
}
