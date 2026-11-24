#ifndef LOG_BUFFER_H_
#define LOG_BUFFER_H_

#include <stddef.h>
#include <stdint.h>
#include "sdkconfig.h"

#ifndef LOG_BUFFER_SIZE
#if defined(CONFIG_SPIRAM) && CONFIG_SPIRAM
#define LOG_BUFFER_SIZE  (512 * 1024)  /* 512 KB */
#else
#define LOG_BUFFER_SIZE  (16 * 1024)   /* 16 KB fallback when SPIRAM is disabled (e.g. QEMU unit tests) */
#endif
#endif

void log_buffer_init(void);
uint64_t log_buffer_get_total_written(void);

/**
 * @param abs_pos Pointer to a 64-bit absolute track position. 
 * @param dest Destination memory.
 * @param max_len Max chunk size to extract.
 * @return Bytes read into dest.
 */
size_t log_buffer_read_absolute(uint64_t *abs_pos, char *dest, size_t max_len);

#endif /* LOG_BUFFER_H_ */
