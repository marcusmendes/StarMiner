#ifndef COIN_PARAMS_H
#define COIN_PARAMS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COIN_TICKER_MAX_LEN     12
#define COIN_BECH32_HRP_MAX_LEN 8

typedef enum {
    COIN_PROFILE_AUTO    = 0,
    COIN_PROFILE_BTC     = 1,
    COIN_PROFILE_DGB     = 2,
    COIN_PROFILE_BCH     = 3,
    COIN_PROFILE_BSV     = 4,
    COIN_PROFILE_FB      = 5,
    COIN_PROFILE_BC2     = 6,
    COIN_PROFILE_XEC     = 7,
    COIN_PROFILE_PPC     = 8,
    COIN_PROFILE_NMC     = 9,
    COIN_PROFILE_GENERIC = 10,
    COIN_PROFILE_MAX     = 10,
} CoinProfileId;

typedef struct {
    CoinProfileId id;
    char ticker[COIN_TICKER_MAX_LEN];
    uint8_t decimals;
    uint16_t block_time_s;
    uint8_t p2pkh_version;
    uint8_t p2sh_version;
    bool has_bech32;
    char bech32_hrp[COIN_BECH32_HRP_MAX_LEN];
    bool is_btc_family;
} CoinParams;

typedef enum {
    COIN_ADDR_PAYLOAD_NONE = 0,
    COIN_ADDR_PAYLOAD_P2PKH_HASH160,
    COIN_ADDR_PAYLOAD_P2SH_HASH160,
    COIN_ADDR_PAYLOAD_WITNESS,
} CoinAddrPayloadKind;

typedef struct {
    CoinAddrPayloadKind kind;
    int witness_ver;
    uint8_t data[40];
    size_t data_len;
} CoinAddrPayload;

/**
 * @brief Populate CoinParams for an explicit profile ID (1..10). Unknown/AUTO defaults to BTC parameters.
 */
void coin_get_params_by_id(CoinProfileId profile, CoinParams *out_params);

/**
 * @brief Extract the address/account prefix from a stratum user string, stripping .worker/_worker suffixes.
 */
void coin_extract_address_part(const char *stratum_user, char *out_addr, size_t out_len);

/**
 * @brief Extract raw hash160 or witness program payload from stratum_user for scriptPubKey comparison.
 */
bool coin_decode_user_address_payload(const char *stratum_user,
                                      const CoinParams *params,
                                      CoinAddrPayload *out_payload);

/**
 * @brief Resolve effective CoinParams using explicit override or Auto detection from address and pool URL.
 */
void coin_resolve_params(CoinProfileId override_profile,
                         const char *stratum_user,
                         const char *stratum_url,
                         CoinParams *out_params,
                         bool *out_user_is_address);

#ifdef __cplusplus
}
#endif

#endif /* COIN_PARAMS_H */
