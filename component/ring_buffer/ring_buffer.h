#ifndef RING_BUFFER_H
#define RING_BUFFER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef struct
{
  uint8_t *data;
  uint16_t size;
  volatile uint16_t head;
  volatile uint16_t tail;
} ring_buffer_t;

void ring_buffer_init(ring_buffer_t *ring, uint8_t *data, uint16_t size);
void ring_buffer_reset(ring_buffer_t *ring);
void ring_buffer_update_write_ptr(ring_buffer_t *ring, uint16_t write_pos);
const uint8_t *ring_buffer_get_read_ptr(const ring_buffer_t *ring, uint16_t *len);
void ring_buffer_update_read_ptr(ring_buffer_t *ring, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* RING_BUFFER_H */
