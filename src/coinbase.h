#ifndef SIMPLEPOOL_COINBASE_H
#define SIMPLEPOOL_COINBASE_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *cb1;
    size_t   cb1_len;
    uint8_t *cb2;
    size_t   cb2_len;
} coinbase_parts_t;

/* Build coinbase1/coinbase2 halves around the extranonce placeholder,
 * single-payout — the entire value_sats goes to payout_address.
 *
 * Equivalent to coinbase_build_split with operator_address=NULL / fee_bps=0.
 * Kept for tests and simple solo configurations.
 *
 * `witness_commitment_hex` may be NULL.
 * Returns 0 ok, negative on error (errbuf populated). */
int coinbase_build(uint32_t height, int64_t value_sats,
                   const char *payout_address,
                   const char *witness_commitment_hex,
                   const char *coinbase_tag,
                   size_t extranonce1_size, size_t extranonce2_size,
                   coinbase_parts_t *out, char *errbuf, size_t errlen);

/* Build coinbase1/coinbase2 with a two-way split:
 *   fee_sats   = value_sats * fee_bps / 10000  (rounded down)
 *   miner_sats = value_sats - fee_sats
 *
 * If `operator_address` is NULL/empty, fee_bps is 0, or `fee_sats` would
 * be below the dust threshold (~546 sats), the whole reward goes to the
 * miner and *out_fee_sats is set to 0. Otherwise both outputs are emitted.
 *
 * *out_miner_sats and *out_fee_sats receive the final split (may be NULL).
 *
 * Returns 0 ok, negative on error. */
int coinbase_build_split(uint32_t height, int64_t value_sats,
                         const char *miner_address,
                         const char *operator_address,
                         int fee_bps,
                         const char *witness_commitment_hex,
                         const char *coinbase_tag,
                         size_t extranonce1_size, size_t extranonce2_size,
                         coinbase_parts_t *out,
                         int64_t *out_miner_sats, int64_t *out_fee_sats,
                         char *errbuf, size_t errlen);

void coinbase_parts_free(coinbase_parts_t *p);

/* Internal helpers exposed for tests. */
int coinbase_address_to_script(const char *addr,
                               uint8_t *out, size_t cap, size_t *out_len,
                               char *errbuf, size_t errlen);

#endif
