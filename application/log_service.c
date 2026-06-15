#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "main.h"

extern UART_HandleTypeDef huart1;

void log_printf(char *format, ...)
{
  char str[128];
  va_list args;

  va_start(args, format);
  vsnprintf(str, sizeof(str), format, args);
  va_end(args);

  HAL_UART_Transmit(&huart1, (uint8_t *)str, strlen(str), 100U);
}