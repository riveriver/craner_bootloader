#include "uart_service.h"

#include <string.h>

static uart_service_t uart_service_ports[8U] = {0};

static const uint32_t uart_service_max_ports =
    (uint32_t)(sizeof(uart_service_ports) / sizeof(uart_service_ports[0]));
static const uint32_t uart_service_send_default_timeout_ms = 1000U;

static uart_service_status_t uart_service_mapping_hal_status(HAL_StatusTypeDef status,
                                                   uart_service_status_t error)
{
  if (status == HAL_OK)
  {
    return UART_SERVICE_OK;
  }

  if (status == HAL_BUSY)
  {
    return UART_SERVICE_ERR_BUSY;
  }

  if (status == HAL_TIMEOUT)
  {
    return UART_SERVICE_ERR_TIMEOUT;
  }

  return error;
}

uart_service_t *uart_service_get_obj(UART_HandleTypeDef *huart)
{
  if (huart == NULL)
  {
    return NULL;
  }

  for (uint32_t i = 0U; i < uart_service_max_ports; ++i)
  {
    if ((uart_service_ports[i].used != 0U) && (uart_service_ports[i].huart == huart))
    {
      return &uart_service_ports[i];
    }
  }

  return NULL;
}

uart_service_t *uart_service_get_obj_by_name(const char *name)
{
  if (name == NULL)
  {
    return NULL;
  }

  for (uint32_t i = 0U; i < uart_service_max_ports; ++i)
  {
    if ((uart_service_ports[i].used != 0U) &&
        (uart_service_ports[i].name != NULL) &&
        (strcmp(uart_service_ports[i].name, name) == 0))
    {
      return &uart_service_ports[i];
    }
  }

  return NULL;
}

uart_service_status_t uart_service_register_obj(const uart_service_config_t *config)
{
  uart_service_t *obj;

  if ((config == NULL) || (config->huart == NULL))
  {
    return UART_SERVICE_ERR_PARAM;
  }

  obj = uart_service_get_obj(config->huart);
  if (obj == NULL)
  {
    for (uint32_t i = 0U; i < uart_service_max_ports; ++i)
    {
      if (uart_service_ports[i].used == 0U)
      {
        obj = &uart_service_ports[i];
        break;
      }
    }

    if (obj == NULL)
    {
      return UART_SERVICE_ERR_NO_SLOT;
    }
  }

  obj->huart = config->huart;
  obj->name = config->name;
  obj->recv_callback = config->recv_callback;
  (void)memset(obj->recv_dma_buffer, 0, sizeof(obj->recv_dma_buffer));
  (void)memset(obj->send_dma_buffer, 0, sizeof(obj->send_dma_buffer));
  obj->send_dma_busy = 0U;
  obj->ready = 0U;
  obj->used = 1U;

  return UART_SERVICE_OK;
}

uart_service_status_t uart_service_send(UART_HandleTypeDef *huart, const uint8_t *data, uint16_t len)
{
  uart_service_t *obj;
  HAL_StatusTypeDef status;

  if ((huart == NULL) || (data == NULL) || (len == 0U))
  {
    return UART_SERVICE_ERR_PARAM;
  }

  obj = uart_service_get_obj(huart);
  if (obj == NULL)
  {
    return UART_SERVICE_ERR_NOT_FOUND;
  }

  if (obj->ready == 0U)
  {
    return UART_SERVICE_ERR_NOT_INIT;
  }

  if (len > UART_SERVICE_SEND_DMA_BUFFER_SIZE)
  {
    return UART_SERVICE_ERR_PARAM;
  }

  if (obj->send_dma_busy != 0U)
  {
    return UART_SERVICE_ERR_BUSY;
  }

  (void)memcpy(obj->send_dma_buffer, data, len);
  obj->send_dma_busy = 1U;

  status = HAL_UART_Transmit_DMA(obj->huart, obj->send_dma_buffer, len);
  if (status != HAL_OK)
  {
    obj->send_dma_busy = 0U;
    return uart_service_mapping_hal_status(status, UART_SERVICE_ERR_TX);
  }

  return UART_SERVICE_OK;
}

uart_service_status_t uart_service_send_by_name(const char *name, const uint8_t *data, uint16_t len)
{
  uart_service_t *obj;

  if ((name == NULL) || (data == NULL) || (len == 0U))
  {
    return UART_SERVICE_ERR_PARAM;
  }

  obj = uart_service_get_obj_by_name(name);
  if (obj == NULL)
  {
    return UART_SERVICE_ERR_NOT_FOUND;
  }

  return uart_service_send(obj->huart, data, len);
}

uart_service_status_t uart_service_start_recv_it(UART_HandleTypeDef *huart)
{
  
  if (huart == NULL)
  {
    return UART_SERVICE_ERR_PARAM;
  }

  uart_service_t *obj;
  obj = uart_service_get_obj(huart);
  if (obj == NULL)
  {
    return UART_SERVICE_ERR_NOT_FOUND;
  }

  HAL_StatusTypeDef ret;
  ret = HAL_UARTEx_ReceiveToIdle_DMA(obj->huart,
                                     obj->recv_dma_buffer,
                                     (uint16_t)sizeof(obj->recv_dma_buffer));
  if (ret != HAL_OK)
  {
    return uart_service_mapping_hal_status(ret, UART_SERVICE_ERR_RECV_START);
  }

  if (obj->huart->hdmarx != NULL)
  {
    __HAL_DMA_DISABLE_IT(obj->huart->hdmarx, DMA_IT_HT);
  }

  obj->ready = 1U;
  return ret;
}

uart_service_status_t uart_service_start_recv_it_by_name(const char *name)
{
  if (name == NULL)
  {
    return UART_SERVICE_ERR_PARAM;
  }
  
  uart_service_t *obj;
  obj = uart_service_get_obj_by_name(name);
  if (obj == NULL)
  {
    return UART_SERVICE_ERR_NOT_FOUND;
  }

  return uart_service_start_recv_it(obj->huart);
}

uart_service_status_t uart_service_on_recv_event(UART_HandleTypeDef *huart, uint16_t size)
{
  uart_service_t *obj;

  if (huart == NULL)
  {
    return UART_SERVICE_ERR_PARAM;
  }

  obj = uart_service_get_obj(huart);
  if (obj == NULL)
  {
    return UART_SERVICE_OK;
  }

  if (obj->ready == 0U)
  {
    return UART_SERVICE_OK;
  }

  uart_service_status_t ret;

  if (size <= 0U)
  {
    ret = uart_service_start_recv_it(obj->huart);
    if (ret != UART_SERVICE_OK)
    {
      obj->ready = 0U;
    }

    return ret;
  }
  
  if (size > (uint16_t)sizeof(obj->recv_dma_buffer))
  {
    size = (uint16_t)sizeof(obj->recv_dma_buffer);
  }

  if (obj->recv_callback != NULL)
  {
    obj->recv_callback(obj, obj->recv_dma_buffer, size);
  }

  ret = uart_service_start_recv_it(obj->huart);
  if (ret != UART_SERVICE_OK)
  {
    obj->ready = 0U;
  }

  return ret;
}

uart_service_status_t uart_service_on_send_complete(UART_HandleTypeDef *huart)
{
  uart_service_t *obj;

  if (huart == NULL)
  {
    return UART_SERVICE_ERR_PARAM;
  }

  obj = uart_service_get_obj(huart);
  if (obj == NULL)
  {
    return UART_SERVICE_OK;
  }

  if (obj->ready == 0U)
  {
    return UART_SERVICE_ERR_NOT_INIT;
  }

  obj->send_dma_busy = 0U;

  return UART_SERVICE_OK;
}

uart_service_status_t uart_service_on_error(UART_HandleTypeDef *huart)
{
  uart_service_t *obj;
  HAL_StatusTypeDef status;

  if (huart == NULL)
  {
    return UART_SERVICE_ERR_PARAM;
  }

  obj = uart_service_get_obj(huart);
  if (obj == NULL)
  {
    return UART_SERVICE_OK;
  }

  if (obj->ready == 0U)
  {
    return UART_SERVICE_ERR_NOT_INIT;
  }

  if (obj->send_dma_busy != 0U)
  {
    (void)HAL_UART_AbortTransmit(obj->huart);
    obj->send_dma_busy = 0U;
  }

  status = HAL_UART_AbortReceive(obj->huart);
  if (status != HAL_OK)
  {
    return uart_service_mapping_hal_status(status, UART_SERVICE_ERR_RECV_ABORT);
  }

  return UART_SERVICE_ERR_UART;
}
