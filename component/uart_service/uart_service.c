#include "uart_service.h"

#include <string.h>

static uart_service_t uart_service_ports[8U] = {0};

static const uint32_t uart_service_max_ports =
    (uint32_t)(sizeof(uart_service_ports) / sizeof(uart_service_ports[0]));
static const uint32_t uart_service_tx_default_timeout_ms = 1000U;
static const uint32_t uart_service_rx_default_timeout_ms = 1000U;

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

  obj->name = config->name;
  obj->huart = config->huart;
  obj->rx_callback = config->rx_callback;
  obj->tx_timeout_ms = config->tx_timeout_ms;
  obj->rx_timeout_ms = config->rx_timeout_ms;
  obj->rx_byte = 0U;
  obj->used = 1U;

  return UART_SERVICE_OK;
}

uart_service_status_t uart_service_init(const uart_service_config_t *table, uint16_t count)
{
  uart_service_status_t status;

  if ((table == NULL) || (count == 0U) || (count > uart_service_max_ports))
  {
    return UART_SERVICE_ERR_PARAM;
  }

  for (uint16_t i = 0U; i < count; ++i)
  {
    status = uart_service_register_obj(&table[i]);
    if (status != UART_SERVICE_OK)
    {
      return status;
    }
  }

  return UART_SERVICE_OK;
}

uart_service_status_t uart_service_set_rx_callback(UART_HandleTypeDef *huart,
                                                 uart_service_rx_callback_t callback)
{
  uart_service_t *obj;

  if (huart == NULL)
  {
    return UART_SERVICE_ERR_PARAM;
  }

  obj = uart_service_get_obj(huart);
  if (obj == NULL)
  {
    return UART_SERVICE_ERR_NOT_FOUND;
  }

  obj->rx_callback = callback;
  return UART_SERVICE_OK;
}

static uint32_t uart_service_tx_timeout(const uart_service_t *obj)
{
  return (obj->tx_timeout_ms == 0U) ? uart_service_tx_default_timeout_ms : obj->tx_timeout_ms;
}

static uint32_t uart_service_rx_timeout(const uart_service_t *obj, uint32_t timeout_ms)
{
  if (timeout_ms != 0U)
  {
    return timeout_ms;
  }

  return (obj->rx_timeout_ms == 0U) ? uart_service_rx_default_timeout_ms : obj->rx_timeout_ms;
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

  status = HAL_UART_Transmit(obj->huart, (uint8_t *)data, len, uart_service_tx_timeout(obj));
  return uart_service_mapping_hal_status(status, UART_SERVICE_ERR_TX);
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

uart_service_status_t uart_service_receive(UART_HandleTypeDef *huart,
                                         uint8_t *data,
                                         uint16_t len,
                                         uint32_t timeout_ms)
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

  status = HAL_UART_Receive(obj->huart, data, len, uart_service_rx_timeout(obj, timeout_ms));
  return uart_service_mapping_hal_status(status, UART_SERVICE_ERR_RX);
}

uart_service_status_t uart_service_receive_by_name(const char *name,
                                                 uint8_t *data,
                                                 uint16_t len,
                                                 uint32_t timeout_ms)
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

  return uart_service_receive(obj->huart, data, len, timeout_ms);
}

static uart_service_status_t uart_service_start_rx_it_impl(uart_service_t *obj)
{
  HAL_StatusTypeDef status;

  if ((obj == NULL) || (obj->huart == NULL))
  {
    return UART_SERVICE_ERR_PARAM;
  }

  status = HAL_UART_Receive_IT(obj->huart, &obj->rx_byte, 1U);
  if (status != HAL_OK)
  {
    return uart_service_mapping_hal_status(status, UART_SERVICE_ERR_RX_START);
  }

  return UART_SERVICE_OK;
}

uart_service_status_t uart_service_start_rx_it(UART_HandleTypeDef *huart)
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
  ret = HAL_UART_Receive_IT(obj->huart, &obj->rx_byte, 1U);
  if (ret != HAL_OK)
  {
    return uart_service_mapping_hal_status(ret, UART_SERVICE_ERR_RX_START);
  }

  return UART_SERVICE_OK;
}

uart_service_status_t uart_service_start_rx_it_by_name(const char *name)
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

  return uart_service_start_rx_it(obj->huart);
}

uart_service_status_t uart_service_on_rx_complete(UART_HandleTypeDef *huart)
{
  uart_service_t *obj;
  uart_service_status_t status;
  uart_service_status_t callback_status;
  uint8_t byte;

  if (huart == NULL)
  {
    return UART_SERVICE_ERR_PARAM;
  }

  obj = uart_service_get_obj(huart);
  if (obj == NULL)
  {
    return UART_SERVICE_OK;
  }

  byte = obj->rx_byte;
  status = uart_service_start_rx_it_impl(obj);

  if (obj->rx_callback != NULL)
  {
    callback_status = obj->rx_callback(obj, byte);
    if (callback_status != UART_SERVICE_OK)
    {
      return callback_status;
    }
  }

  return status;
}

uart_service_status_t uart_service_on_error(UART_HandleTypeDef *huart)
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

  return UART_SERVICE_ERR_UART;
}
