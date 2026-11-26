#include "coin_params.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "libbase58.h"
#include "mbedtls/sha256.h"
#include "segwit_addr.h"

static bool coin_sha256(void *digest, const void *data, size_t datasz)
{
    mbedtls_sha256((const unsigned char *) data, datasz, (unsigned char *) digest, 0);
    return true;
}

static void ensure_b58_sha256(void)
{
    if (b58_sha256_impl == NULL) {
        b58_sha256_impl = coin_sha256;
    }
}

static const CoinParams COIN_CATALOG[] = {
    [COIN_PROFILE_BTC] = {
        .id = COIN_PROFILE_BTC,
        .ticker = "BTC",
        .decimals = 8,
        .block_time_s = 600,
        .p2pkh_version = 0x00,
        .p2sh_version = 0x05,
        .has_bech32 = true,
        .bech32_hrp = "bc",
        .is_btc_family = true,
    },
    [COIN_PROFILE_DGB] = {
        .id = COIN_PROFILE_DGB,
        .ticker = "DGB",
        .decimals = 8,
        .block_time_s = 15,
        .p2pkh_version = 0x1E,
        .p2sh_version = 0x3F,
        .has_bech32 = true,
        .bech32_hrp = "dgb",
        .is_btc_family = false,
    },
    [COIN_PROFILE_BCH] = {
        .id = COIN_PROFILE_BCH,
        .ticker = "BCH",
        .decimals = 8,
        .block_time_s = 600,
        .p2pkh_version = 0x00,
        .p2sh_version = 0x05,
        .has_bech32 = false,
        .bech32_hrp = "",
        .is_btc_family = false,
    },
    [COIN_PROFILE_BSV] = {
        .id = COIN_PROFILE_BSV,
        .ticker = "BSV",
        .decimals = 8,
        .block_time_s = 600,
        .p2pkh_version = 0x00,
        .p2sh_version = 0x05,
        .has_bech32 = false,
        .bech32_hrp = "",
        .is_btc_family = false,
    },
    [COIN_PROFILE_FB] = {
        .id = COIN_PROFILE_FB,
        .ticker = "FB",
        .decimals = 8,
        .block_time_s = 30,
        .p2pkh_version = 0x00,
        .p2sh_version = 0x05,
        .has_bech32 = true,
        .bech32_hrp = "bc",
        .is_btc_family = false,
    },
    [COIN_PROFILE_BC2] = {
        .id = COIN_PROFILE_BC2,
        .ticker = "BC2",
        .decimals = 8,
        .block_time_s = 600,
        .p2pkh_version = 0x00,
        .p2sh_version = 0x05,
        .has_bech32 = true,
        .bech32_hrp = "bc",
        .is_btc_family = false,
    },
    [COIN_PROFILE_XEC] = {
        .id = COIN_PROFILE_XEC,
        .ticker = "XEC",
        .decimals = 2,
        .block_time_s = 600,
        .p2pkh_version = 0x00,
        .p2sh_version = 0x05,
        .has_bech32 = false,
        .bech32_hrp = "",
        .is_btc_family = false,
    },
    [COIN_PROFILE_PPC] = {
        .id = COIN_PROFILE_PPC,
        .ticker = "PPC",
        .decimals = 6,
        .block_time_s = 600,
        .p2pkh_version = 0x37,
        .p2sh_version = 0x75,
        .has_bech32 = true,
        .bech32_hrp = "pc",
        .is_btc_family = false,
    },
    [COIN_PROFILE_NMC] = {
        .id = COIN_PROFILE_NMC,
        .ticker = "NMC",
        .decimals = 8,
        .block_time_s = 600,
        .p2pkh_version = 0x34,
        .p2sh_version = 0x0D,
        .has_bech32 = true,
        .bech32_hrp = "nc",
        .is_btc_family = false,
    },
    [COIN_PROFILE_GENERIC] = {
        .id = COIN_PROFILE_GENERIC,
        .ticker = "SHA256",
        .decimals = 8,
        .block_time_s = 600,
        .p2pkh_version = 0x00,
        .p2sh_version = 0x05,
        .has_bech32 = true,
        .bech32_hrp = "bc",
        .is_btc_family = false,
    },
};

void coin_get_params_by_id(CoinProfileId profile, CoinParams *out_params)
{
    if (out_params == NULL) {
        return;
    }
    if (profile < COIN_PROFILE_BTC || profile > COIN_PROFILE_GENERIC) {
        profile = COIN_PROFILE_BTC;
    }
    *out_params = COIN_CATALOG[profile];
}

void coin_extract_address_part(const char *stratum_user, char *out_addr, size_t out_len)
{
    if (out_addr == NULL || out_len == 0) {
        return;
    }
    out_addr[0] = '\0';
    if (stratum_user == NULL) {
        return;
    }

    const char *p = stratum_user;
    while (*p != '\0' && isspace((unsigned char) *p)) {
        p++;
    }

    size_t i = 0;
    while (p[i] != '\0' && i + 1 < out_len) {
        unsigned char c = (unsigned char) p[i];
        if (c == '.' || c == '_' || c == '/' || isspace(c)) {
            break;
        }
        out_addr[i] = (char) c;
        i++;
    }
    out_addr[i] = '\0';
}

static bool decode_bech32_addr(const char *addr,
                               char *out_hrp,
                               size_t out_hrp_len,
                               int *out_ver,
                               uint8_t *out_prog,
                               size_t *out_prog_len)
{
    if (addr == NULL || addr[0] == '\0') {
        return false;
    }
    size_t len = strlen(addr);
    if (len < 8 || len > 90) {
        return false;
    }

    char hrp_buf[96];
    uint8_t data_buf[96];
    size_t data_len = sizeof(data_buf);
    if (bech32_decode(hrp_buf, data_buf, &data_len, addr) == BECH32_ENCODING_NONE) {
        return false;
    }

    int ver = 0;
    uint8_t prog[40];
    size_t prog_len = sizeof(prog);
    if (!segwit_addr_decode(&ver, prog, &prog_len, hrp_buf, addr)) {
        return false;
    }

    if (out_hrp != NULL && out_hrp_len > 0) {
        size_t hrp_len = strlen(hrp_buf);
        size_t copy_len = (hrp_len < out_hrp_len) ? hrp_len : (out_hrp_len - 1);
        memcpy(out_hrp, hrp_buf, copy_len);
        out_hrp[copy_len] = '\0';
    }
    if (out_ver != NULL) {
        *out_ver = ver;
    }
    if (out_prog != NULL && out_prog_len != NULL) {
        memcpy(out_prog, prog, prog_len);
        *out_prog_len = prog_len;
    }
    return true;
}

static bool decode_b58check_addr(const char *addr, uint8_t *out_ver, uint8_t out_hash160[20])
{
    if (addr == NULL || addr[0] == '\0') {
        return false;
    }
    size_t len = strlen(addr);
    if (len < 25 || len > 36) {
        return false;
    }

    ensure_b58_sha256();
    uint8_t bin[25];
    size_t binsz = sizeof(bin);
    if (!b58tobin(bin, &binsz, addr, len) || binsz != 25) {
        return false;
    }
    int ver = b58check(bin, sizeof(bin), addr, len);
    if (ver < 0) {
        return false;
    }
    if (out_ver != NULL) {
        *out_ver = (uint8_t) ver;
    }
    if (out_hash160 != NULL) {
        memcpy(out_hash160, bin + 1, 20);
    }
    return true;
}

static bool is_url_delim(unsigned char c)
{
    return c == '\0' || c == '.' || c == '-' || c == '_' || c == '/' || c == ':' || isspace(c);
}

static bool contains_token_ci(const char *haystack, const char *token)
{
    if (haystack == NULL || token == NULL || token[0] == '\0') {
        return false;
    }
    size_t hlen = strlen(haystack);
    size_t tlen = strlen(token);
    if (hlen < tlen) {
        return false;
    }

    for (size_t i = 0; i + tlen <= hlen; i++) {
        bool match = true;
        for (size_t j = 0; j < tlen; j++) {
            if (tolower((unsigned char) haystack[i + j]) != tolower((unsigned char) token[j])) {
                match = false;
                break;
            }
        }
        if (match) {
            bool left_ok = (i == 0) || is_url_delim((unsigned char) haystack[i - 1]);
            bool right_ok = is_url_delim((unsigned char) haystack[i + tlen]);
            if (left_ok && right_ok) {
                return true;
            }
        }
    }
    return false;
}

static bool contains_substr_ci(const char *haystack, const char *needle)
{
    if (haystack == NULL || needle == NULL || needle[0] == '\0') {
        return false;
    }
    size_t hlen = strlen(haystack);
    size_t nlen = strlen(needle);
    if (hlen < nlen) {
        return false;
    }
    for (size_t i = 0; i + nlen <= hlen; i++) {
        size_t j = 0;
        while (j < nlen && tolower((unsigned char) haystack[i + j]) == tolower((unsigned char) needle[j])) {
            j++;
        }
        if (j == nlen) {
            return true;
        }
    }
    return false;
}

static CoinProfileId detect_coin_from_url(const char *stratum_url)
{
    if (stratum_url == NULL || stratum_url[0] == '\0') {
        return COIN_PROFILE_AUTO;
    }
    if (contains_substr_ci(stratum_url, "digibyte") || contains_token_ci(stratum_url, "dgb")) {
        return COIN_PROFILE_DGB;
    }
    if (contains_substr_ci(stratum_url, "bitcoincash") || contains_token_ci(stratum_url, "bch")) {
        return COIN_PROFILE_BCH;
    }
    if (contains_substr_ci(stratum_url, "bitcoinsv") || contains_token_ci(stratum_url, "bsv")) {
        return COIN_PROFILE_BSV;
    }
    if (contains_substr_ci(stratum_url, "fractal") || contains_token_ci(stratum_url, "fb")) {
        return COIN_PROFILE_FB;
    }
    if (contains_token_ci(stratum_url, "bc2")) {
        return COIN_PROFILE_BC2;
    }
    if (contains_substr_ci(stratum_url, "ecash") || contains_token_ci(stratum_url, "xec")) {
        return COIN_PROFILE_XEC;
    }
    if (contains_substr_ci(stratum_url, "peercoin") || contains_token_ci(stratum_url, "ppc")) {
        return COIN_PROFILE_PPC;
    }
    if (contains_substr_ci(stratum_url, "namecoin") || contains_token_ci(stratum_url, "nmc")) {
        return COIN_PROFILE_NMC;
    }
    if (contains_substr_ci(stratum_url, "bitcoin") ||
        contains_substr_ci(stratum_url, "ckpool") ||
        contains_substr_ci(stratum_url, "public-pool") ||
        contains_substr_ci(stratum_url, "ocean") ||
        contains_substr_ci(stratum_url, "braiins") ||
        contains_token_ci(stratum_url, "btc")) {
        return COIN_PROFILE_BTC;
    }
    return COIN_PROFILE_AUTO;
}

static bool has_ci_prefix(const char *str, const char *prefix)
{
    size_t plen = strlen(prefix);
    if (strlen(str) < plen) {
        return false;
    }
    for (size_t i = 0; i < plen; i++) {
        if (tolower((unsigned char) str[i]) != tolower((unsigned char) prefix[i])) {
            return false;
        }
    }
    return true;
}

static void apply_btc_network_variant(CoinParams *params, bool is_regtest)
{
    params->p2pkh_version = 0x6F;
    params->p2sh_version = 0xC4;
    params->has_bech32 = true;
    snprintf(params->bech32_hrp, sizeof(params->bech32_hrp), "%s", is_regtest ? "bcrt" : "tb");
}

bool coin_decode_user_address_payload(const char *stratum_user,
                                      const CoinParams *params,
                                      CoinAddrPayload *out_payload)
{
    if (out_payload == NULL) {
        return false;
    }
    memset(out_payload, 0, sizeof(*out_payload));

    char addr[128];
    coin_extract_address_part(stratum_user, addr, sizeof(addr));
    if (addr[0] == '\0') {
        return false;
    }

    int witness_ver = 0;
    uint8_t prog[40];
    size_t prog_len = sizeof(prog);
    if (decode_bech32_addr(addr, NULL, 0, &witness_ver, prog, &prog_len)) {
        out_payload->kind = COIN_ADDR_PAYLOAD_WITNESS;
        out_payload->witness_ver = witness_ver;
        out_payload->data_len = prog_len;
        memcpy(out_payload->data, prog, prog_len);
        return true;
    }

    uint8_t ver = 0;
    uint8_t hash160[20];
    if (decode_b58check_addr(addr, &ver, hash160)) {
        bool is_p2sh = (params != NULL && ver == params->p2sh_version) ||
                       ver == 0x05 || ver == 0x3F || ver == 0xC4 || ver == 0x75 || ver == 0x0D;
        out_payload->kind = is_p2sh ? COIN_ADDR_PAYLOAD_P2SH_HASH160 : COIN_ADDR_PAYLOAD_P2PKH_HASH160;
        out_payload->data_len = 20;
        memcpy(out_payload->data, hash160, 20);
        return true;
    }

    return false;
}

void coin_resolve_params(CoinProfileId override_profile,
                         const char *stratum_user,
                         const char *stratum_url,
                         CoinParams *out_params,
                         bool *out_user_is_address)
{
    CoinParams resolved;
    coin_get_params_by_id(COIN_PROFILE_BTC, &resolved);

    char addr[128];
    coin_extract_address_part(stratum_user, addr, sizeof(addr));

    char bech32_hrp[COIN_BECH32_HRP_MAX_LEN] = {0};
    bool is_bech32 = decode_bech32_addr(addr, bech32_hrp, sizeof(bech32_hrp), NULL, NULL, NULL);

    uint8_t b58_ver = 0;
    bool is_b58 = !is_bech32 && decode_b58check_addr(addr, &b58_ver, NULL);

    bool user_is_addr = is_bech32 || is_b58;
    CoinProfileId url_hint = detect_coin_from_url(stratum_url);

    if (override_profile >= COIN_PROFILE_BTC && override_profile <= COIN_PROFILE_GENERIC) {
        coin_get_params_by_id(override_profile, &resolved);
        if (override_profile == COIN_PROFILE_BTC) {
            if (is_bech32 && strcmp(bech32_hrp, "bcrt") == 0) {
                apply_btc_network_variant(&resolved, true);
            } else if ((is_bech32 && strcmp(bech32_hrp, "tb") == 0) || (is_b58 && (b58_ver == 0x6F || b58_ver == 0xC4))) {
                apply_btc_network_variant(&resolved, false);
            }
        }
    } else if (is_bech32) {
        if (strcmp(bech32_hrp, "dgb") == 0) {
            coin_get_params_by_id(COIN_PROFILE_DGB, &resolved);
        } else if (strcmp(bech32_hrp, "pc") == 0) {
            coin_get_params_by_id(COIN_PROFILE_PPC, &resolved);
        } else if (strcmp(bech32_hrp, "nc") == 0) {
            coin_get_params_by_id(COIN_PROFILE_NMC, &resolved);
        } else if (strcmp(bech32_hrp, "tb") == 0) {
            coin_get_params_by_id(COIN_PROFILE_BTC, &resolved);
            apply_btc_network_variant(&resolved, false);
        } else if (strcmp(bech32_hrp, "bcrt") == 0) {
            coin_get_params_by_id(COIN_PROFILE_BTC, &resolved);
            apply_btc_network_variant(&resolved, true);
        } else if (strcmp(bech32_hrp, "bc") == 0) {
            if (url_hint == COIN_PROFILE_FB || url_hint == COIN_PROFILE_BC2) {
                coin_get_params_by_id(url_hint, &resolved);
            } else {
                coin_get_params_by_id(COIN_PROFILE_BTC, &resolved);
            }
        } else {
            coin_get_params_by_id(COIN_PROFILE_GENERIC, &resolved);
            resolved.has_bech32 = true;
            snprintf(resolved.bech32_hrp, sizeof(resolved.bech32_hrp), "%s", bech32_hrp);
        }
    } else if (is_b58) {
        if (b58_ver == 0x1E || b58_ver == 0x3F) {
            coin_get_params_by_id(COIN_PROFILE_DGB, &resolved);
        } else if (b58_ver == 0x37 || b58_ver == 0x75) {
            coin_get_params_by_id(COIN_PROFILE_PPC, &resolved);
        } else if (b58_ver == 0x34 || b58_ver == 0x0D) {
            coin_get_params_by_id(COIN_PROFILE_NMC, &resolved);
        } else if (b58_ver == 0x6F || b58_ver == 0xC4) {
            coin_get_params_by_id(COIN_PROFILE_BTC, &resolved);
            apply_btc_network_variant(&resolved, false);
        } else if (b58_ver == 0x00 || b58_ver == 0x05) {
            if (url_hint == COIN_PROFILE_BCH || url_hint == COIN_PROFILE_BSV ||
                url_hint == COIN_PROFILE_FB || url_hint == COIN_PROFILE_BC2 ||
                url_hint == COIN_PROFILE_XEC || url_hint == COIN_PROFILE_DGB) {
                coin_get_params_by_id(url_hint, &resolved);
            } else {
                coin_get_params_by_id(COIN_PROFILE_BTC, &resolved);
            }
        } else {
            coin_get_params_by_id(COIN_PROFILE_GENERIC, &resolved);
        }
    } else if (has_ci_prefix(addr, "bitcoincash:")) {
        coin_get_params_by_id(COIN_PROFILE_BCH, &resolved);
        user_is_addr = true;
    } else if (has_ci_prefix(addr, "ecash:")) {
        coin_get_params_by_id(COIN_PROFILE_XEC, &resolved);
        user_is_addr = true;
    } else if (url_hint != COIN_PROFILE_AUTO) {
        coin_get_params_by_id(url_hint, &resolved);
    } else if (addr[0] == '\0' && (stratum_url == NULL || stratum_url[0] == '\0')) {
        coin_get_params_by_id(COIN_PROFILE_BTC, &resolved);
    } else {
        coin_get_params_by_id(COIN_PROFILE_GENERIC, &resolved);
    }

    if (out_params != NULL) {
        *out_params = resolved;
    }
    if (out_user_is_address != NULL) {
        *out_user_is_address = user_is_addr;
    }
}
