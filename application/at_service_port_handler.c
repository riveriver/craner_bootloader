#include "at_service_port.h"

#include <stddef.h>

#include "ota_flash_service.h"

int32_t at_otasloa_read(const at_service_request_t *request)
{
  ota_flash_slot_t active_slot;
  const char *slot_name = "NONE";

  if (request == NULL)
  {
    return AT_SERVICE_ACTION_EXECUTION_FAILED;
  }

  if (ota_flash_service_init() != OTA_FLASH_OK)
  {
    (void)at_service_reply_error(request);
    return AT_SERVICE_ACTION_EXECUTION_FAILED;
  }

  active_slot = ota_flash_get_active_slot();
  if (active_slot == OTA_FLASH_SLOT_A)
  {
    slot_name = "A";
  }
  else if (active_slot == OTA_FLASH_SLOT_B)
  {
    slot_name = "B";
  }

  (void)at_service_reply_value(request, slot_name);
  return AT_SERVICE_OK;
}

int32_t at_otasloa_describe(const at_service_request_t *request)
{
  if (request == NULL)
  {
    return AT_SERVICE_ACTION_EXECUTION_FAILED;
  }

  (void)at_service_reply_options(request, "A,B");
  return AT_SERVICE_OK;
}
