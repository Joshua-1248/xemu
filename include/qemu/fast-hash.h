#ifndef QEMU_FAST_HASH_H
#define QEMU_FAST_HASH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint64_t fast_hash(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* QEMU_FAST_HASH_H */
