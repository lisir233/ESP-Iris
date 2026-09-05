#pragma once
#include <stddef.h>
#include <stdint.h>
typedef struct { unsigned active; } psa_hash_operation_t;
#define PSA_SUCCESS 0
#define PSA_ALG_SHA_256 1
static inline psa_hash_operation_t psa_hash_operation_init(void) { return (psa_hash_operation_t){0}; }
static inline int psa_crypto_init(void) { return 0; }
static inline int psa_hash_setup(psa_hash_operation_t *p, unsigned alg) { p->active=1; return 0; }
static inline int psa_hash_update(psa_hash_operation_t *p, const uint8_t *b, size_t n) { return 0; }
static inline int psa_hash_finish(psa_hash_operation_t *p, uint8_t *b, size_t n, size_t *out) { memset(b, 0, n); *out=32; return 0; }
static inline int psa_hash_abort(psa_hash_operation_t *p) { return 0; }
typedef int psa_status_t;
#define PSA_ERROR_GENERIC_ERROR -1
static inline int psa_hash_compute(unsigned alg, const uint8_t *b, size_t n, uint8_t *o, size_t cap, size_t *out) { memset(o, 0, cap); *out=32; return 0; }
