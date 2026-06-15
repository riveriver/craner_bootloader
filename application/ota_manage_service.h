#ifndef OTA_MANAGE_SERVICE_H
#define OTA_MANAGE_SERVICE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef enum
{
  OTA_MANAGE_OK = 0,
  OTA_MANAGE_ERR = -1,
} ota_manage_status_t;

typedef enum
{
  OTA_MANAGE_STATE_INIT = 0,
  OTA_MANAGE_STATE_WAIT_TRANSFER,
  OTA_MANAGE_STATE_RECEIVING,
  OTA_MANAGE_STATE_COMPLETE,
  OTA_MANAGE_STATE_JUMP_APP,
  OTA_MANAGE_STATE_ERROR,
} ota_manage_state_t;

ota_manage_status_t ota_manage_service_init(void);
ota_manage_status_t ota_manage_service_start(void);
void ota_manage_service_process(uint32_t now_ms);
ota_manage_state_t ota_manage_service_get_state(void);
void ota_manage_service_jump_to_active_app(void);

#ifdef __cplusplus
}
#endif

#endif /* OTA_MANAGE_SERVICE_H */
