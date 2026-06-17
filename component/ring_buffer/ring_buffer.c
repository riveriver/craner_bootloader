#include "ring_buffer.h"

#include <stddef.h>

static uint16_t ring_buffer_advance(uint16_t value, uint16_t step, uint16_t size)
{
  uint32_t next = (uint32_t)value + (uint32_t)step;

  if (next >= size)
  {
    next -= size;
  }

  return (uint16_t)next;
}

void ring_buffer_init(ring_buffer_t *ring, uint8_t *data, uint16_t size)
{
  if (ring == NULL)
  {
    return;
  }

  ring->data = data;
  ring->size = size;
  ring_buffer_reset(ring);
}

void ring_buffer_reset(ring_buffer_t *ring)
{
  if (ring == NULL)
  {
    return;
  }

  ring->head = 0U;
  ring->tail = 0U;
}

void ring_buffer_update_write_ptr(ring_buffer_t *ring, uint16_t write_pos)
{
  if ((ring == NULL) || (ring->size == 0U))
  {
    return;
  }

  ring->head = (write_pos < ring->size) ? write_pos : 0U;
}

const uint8_t *ring_buffer_get_read_ptr(const ring_buffer_t *ring, uint16_t *len)
{
  uint16_t head;
  uint16_t tail;
  uint16_t linear;

  if (len != NULL)
  {
    *len = 0U;
  }

  if ((ring == NULL) || (ring->data == NULL) || (ring->size == 0U) || (len == NULL))
  {
    return NULL;
  }

  head = ring->head;
  tail = ring->tail;
  if (head == tail)
  {
    return NULL;
  }

  linear = (head > tail) ? (uint16_t)(head - tail)
                         : (uint16_t)(ring->size - tail);
  *len = linear;
  return &ring->data[tail];
}

void ring_buffer_update_read_ptr(ring_buffer_t *ring, uint16_t len)
{
  if ((ring == NULL) || (ring->size == 0U) || (len == 0U))
  {
    return;
  }

  ring->tail = ring_buffer_advance(ring->tail, len, ring->size);
}
