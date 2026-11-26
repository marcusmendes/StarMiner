#include "coinbase_decoder.h"
#include "utils.h"
#include "segwit_addr.h"
#include "libbase58.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "mbedtls/sha256.h"

#define BIP110_SIGNAL_BIT 4
#define BIP110_SIGNAL_EXPIRY_BLOCK 965664

// Wrapper for SHA256 to match libbase58's expected signature
static bool my_sha256(void *digest, const void *data, size_t datasz) {
    mbedtls_sha256(data, datasz, digest, 0);
    return true;
}

static void ensure_base58_init(void) {
    if (b58_sha256_impl == NULL) {
        b58_sha256_impl = my_sha256;
    }
}

uint64_t coinbase_decode_varint(const uint8_t *data, int *offset) {
    uint8_t first_byte = data[*offset];
    (*offset)++;
    
    if (first_byte < 0xFD) {
        return first_byte;
    } else if (first_byte == 0xFD) {
        uint64_t value = data[*offset] | (data[*offset + 1] << 8);
        *offset += 2;
        return value;
    } else if (first_byte == 0xFE) {
        uint64_t value = data[*offset] | (data[*offset + 1] << 8) | 
                        (data[*offset + 2] << 16) | (data[*offset + 3] << 24);
        *offset += 4;
        return value;
    } else { // 0xFF
        uint64_t value = 0;
        for (int i = 0; i < 8; i++) {
            value |= ((uint64_t)data[*offset + i]) << (i * 8);
        }
        *offset += 8;
        return value;
    }
}

static bool script_matches_user_payload(const uint8_t *script, size_t script_len, const CoinAddrPayload *payload)
{
    if (script == NULL || payload == NULL || payload->kind == COIN_ADDR_PAYLOAD_NONE) {
        return false;
    }

    if (payload->kind == COIN_ADDR_PAYLOAD_P2PKH_HASH160 && payload->data_len == 20 &&
        script_len == 25 && script[0] == OP_DUP && script[1] == OP_HASH160 &&
        script[2] == OP_PUSHDATA_20 && script[23] == OP_EQUALVERIFY && script[24] == OP_CHECKSIG) {
        return memcmp(script + 3, payload->data, 20) == 0;
    }

    if (payload->kind == COIN_ADDR_PAYLOAD_P2SH_HASH160 && payload->data_len == 20 &&
        script_len == 23 && script[0] == OP_HASH160 && script[1] == OP_PUSHDATA_20 && script[22] == OP_EQUAL) {
        return memcmp(script + 2, payload->data, 20) == 0;
    }

    if (payload->kind == COIN_ADDR_PAYLOAD_WITNESS) {
        if (payload->witness_ver == 0 && payload->data_len == 20 &&
            script_len == 22 && script[0] == OP_0 && script[1] == OP_PUSHDATA_20) {
            return memcmp(script + 2, payload->data, 20) == 0;
        }
        if (payload->witness_ver == 0 && payload->data_len == 32 &&
            script_len == 34 && script[0] == OP_0 && script[1] == OP_PUSHDATA_32) {
            return memcmp(script + 2, payload->data, 32) == 0;
        }
        if (payload->witness_ver == 1 && payload->data_len == 32 &&
            script_len == 34 && script[0] == OP_1 && script[1] == OP_PUSHDATA_32) {
            return memcmp(script + 2, payload->data, 32) == 0;
        }
    }

    return false;
}

void coinbase_decode_address_with_params(const uint8_t *script, size_t script_len,
                                         char *output, size_t output_len,
                                         const CoinParams *params) {
    if (output == NULL || output_len == 0) {
        return;
    }
    if (script == NULL || script_len == 0 || output_len < 65) {
        snprintf(output, output_len, "unknown");
        return;
    }

    ensure_base58_init();

    CoinParams fallback_btc;
    if (params == NULL) {
        coin_get_params_by_id(COIN_PROFILE_BTC, &fallback_btc);
        params = &fallback_btc;
    }

    uint8_t p2pkh_version = params->p2pkh_version;
    uint8_t p2sh_version  = params->p2sh_version;
    const char *bech32_hrp = (params->has_bech32 && params->bech32_hrp[0] != '\0') ? params->bech32_hrp : NULL;

    // P2PKH: OP_DUP OP_HASH160 <20 bytes> OP_EQUALVERIFY OP_CHECKSIG
    if (script_len == 25 && script[0] == OP_DUP && script[1] == OP_HASH160 && 
        script[2] == OP_PUSHDATA_20 && script[23] == OP_EQUALVERIFY && script[24] == OP_CHECKSIG) {
        size_t b58sz = output_len;
        if (b58check_enc(output, &b58sz, p2pkh_version, script + 3, 20)) {
            return;
        }
        // Fallback
        snprintf(output, output_len, "P2PKH:");
        bin2hex(script + 3, 20, output + 6, output_len - 6);
        return;
    }
    
    // P2SH: OP_HASH160 <20 bytes> OP_EQUAL
    if (script_len == 23 && script[0] == OP_HASH160 && script[1] == OP_PUSHDATA_20 && script[22] == OP_EQUAL) {
        size_t b58sz = output_len;
        if (b58check_enc(output, &b58sz, p2sh_version, script + 2, 20)) {
            return;
        }
        // Fallback
        snprintf(output, output_len, "P2SH:");
        bin2hex(script + 2, 20, output + 5, output_len - 5);
        return;
    }
    
    // P2WPKH: OP_0 <20 bytes>
    if (script_len == 22 && script[0] == OP_0 && script[1] == OP_PUSHDATA_20) {
        if (bech32_hrp && segwit_addr_encode(output, bech32_hrp, 0, script + 2, 20)) {
            return;
        }
        // Fallback to hex if encoding fails or chain has no Bech32
        snprintf(output, output_len, "P2WPKH:");
        bin2hex(script + 2, 20, output + 7, output_len - 7);
        return;
    }
    
    // P2WSH: OP_0 <32 bytes>
    if (script_len == 34 && script[0] == OP_0 && script[1] == OP_PUSHDATA_32) {
        if (bech32_hrp && segwit_addr_encode(output, bech32_hrp, 0, script + 2, 32)) {
            return;
        }
        // Fallback to hex if encoding fails or chain has no Bech32
        snprintf(output, output_len, "P2WSH:");
        bin2hex(script + 2, 32, output + 6, output_len - 6);
        return;
    }
    
    // P2TR: OP_1 <32 bytes>
    if (script_len == 34 && script[0] == OP_1 && script[1] == OP_PUSHDATA_32) {
        if (bech32_hrp && segwit_addr_encode(output, bech32_hrp, 1, script + 2, 32)) {
            return;
        }
        // Fallback to hex if encoding fails or chain has no Bech32
        snprintf(output, output_len, "P2TR:");
        bin2hex(script + 2, 32, output + 5, output_len - 5);
        return;
    }

    // OP_RETURN: OP_RETURN <data>
    if (script_len > 0 && script[0] == OP_RETURN) {
        snprintf(output, output_len, "OP_RETURN: ");
        size_t offset = 1;
        
        // Simple check for small pushdata to skip the length byte
        // If script[1] is the length of the remaining data
        if (script_len > 1 && script[1] > 0 && script[1] <= 0x4b && (size_t)script[1] + 2 == script_len) {
            offset = 2;
        }
        
        size_t out_idx = strlen(output);
        for (size_t i = offset; i < script_len && out_idx < output_len - 1; i++) {
            unsigned char c = script[i];
            output[out_idx++] = isprint(c) ? c : '.';
        }
        output[out_idx] = '\0';
        return;
    }
    
    // Unknown format - just show hex
    snprintf(output, output_len, "UNKNOWN:");
    size_t hex_len = script_len < 32 ? script_len : 32; // Limit to 32 bytes
    bin2hex(script, hex_len, output + 8, output_len - 8);
}

void coinbase_decode_address_from_scriptpubkey(const uint8_t *script, size_t script_len, 
                                                char *output, size_t output_len,
                                                const char *bech32_hrp, bool is_testnet) {
    CoinParams params;
    coin_get_params_by_id(COIN_PROFILE_BTC, &params);
    if (is_testnet) {
        params.p2pkh_version = 0x6F;
        params.p2sh_version = 0xC4;
    }
    if (bech32_hrp != NULL && bech32_hrp[0] != '\0') {
        params.has_bech32 = true;
        snprintf(params.bech32_hrp, sizeof(params.bech32_hrp), "%s", bech32_hrp);
    }
    coinbase_decode_address_with_params(script, script_len, output, output_len, &params);
}

esp_err_t coinbase_process_notification_ex(const mining_notify *notification,
                                           const char *extranonce1,
                                           int extranonce2_len,
                                           const char *user_address,
                                           const char *stratum_url,
                                           CoinProfileId coin_profile,
                                           bool decode_coinbase_tx,
                                           mining_notification_result_t *result) {
    if (!notification || !extranonce1 || !result) return ESP_ERR_INVALID_ARG;

    // Initialize result
    result->total_value_satoshis = 0;
    result->user_value_satoshis = 0;
    result->decode_coinbase_tx = decode_coinbase_tx;

    coin_resolve_params(coin_profile, user_address, stratum_url, &result->resolved_coin, &result->user_is_address);

    char user_addr_part[MAX_ADDRESS_STRING_LEN];
    coin_extract_address_part(user_address, user_addr_part, sizeof(user_addr_part));

    CoinAddrPayload user_payload;
    bool has_user_payload = coin_decode_user_address_payload(user_address, &result->resolved_coin, &user_payload);

    // 1. Calculate difficulty
    result->network_difficulty = networkDifficulty(notification->target);

    // 2. Parse Coinbase 1 for ScriptSig info
    int coinbase_1_len = strlen(notification->coinbase_1) / 2;
    int coinbase_1_offset = 41; // Skip version (4), inputcount (1), prevhash (32), vout (4)
    
    if (coinbase_1_len < coinbase_1_offset) return ESP_ERR_INVALID_ARG;

    uint8_t scriptsig_len;
    hex2bin(notification->coinbase_1 + (coinbase_1_offset * 2), &scriptsig_len, 1);
    coinbase_1_offset++;

    if (coinbase_1_len < coinbase_1_offset) return ESP_ERR_INVALID_ARG;
    
    uint8_t block_height_len;
    hex2bin(notification->coinbase_1 + (coinbase_1_offset * 2), &block_height_len, 1);
    coinbase_1_offset++;

    if (coinbase_1_len < coinbase_1_offset || block_height_len == 0 || block_height_len > 4) return ESP_ERR_INVALID_ARG;

    result->block_height = 0;
    hex2bin(notification->coinbase_1 + (coinbase_1_offset * 2), (uint8_t *)&result->block_height, block_height_len);
    coinbase_1_offset += block_height_len;

    // Detect BIP-110 signaling only on Bitcoin family chains
    result->bip110_signaling = decode_coinbase_tx &&
                               result->resolved_coin.is_btc_family &&
                               result->block_height < BIP110_SIGNAL_EXPIRY_BLOCK &&
                               (notification->version & (1U << BIP110_SIGNAL_BIT)) != 0;

    // Calculate remaining scriptsig length (excluding block height part)
    int scriptsig_length = scriptsig_len - 1 - block_height_len;
    size_t extranonce1_len = strlen(extranonce1) / 2;
    
    // Check if scriptsig extends into coinbase_2 (meaning it covers the extranonces)
    // If so, subtract extranonce lengths to get just the miner tag length
    if (coinbase_1_len - coinbase_1_offset < scriptsig_length) {
        scriptsig_length -= (extranonce1_len + extranonce2_len);
    }
    
    // Extract miner tag if present
    if (scriptsig_length > 0) {
        char *tag = malloc(scriptsig_length + 1);
        if (tag) {
            int coinbase_1_tag_len = coinbase_1_len - coinbase_1_offset;
            if (coinbase_1_tag_len > scriptsig_length) {
                coinbase_1_tag_len = scriptsig_length;
            }

            hex2bin(notification->coinbase_1 + (coinbase_1_offset * 2), (uint8_t *)tag, coinbase_1_tag_len);

            int coinbase_2_tag_len = scriptsig_length - coinbase_1_tag_len;
            int coinbase_2_len = strlen(notification->coinbase_2) / 2;
            
            if (coinbase_2_len >= coinbase_2_tag_len) {
                if (coinbase_2_tag_len > 0) {
                    hex2bin(notification->coinbase_2, (uint8_t *)tag + coinbase_1_tag_len, coinbase_2_tag_len);
                }
                
                // Filter non-printable characters
                for (int i = 0; i < scriptsig_length; i++) {
                    if (!isprint((unsigned char)tag[i])) {
                        tag[i] = '.';
                    }                }
                tag[scriptsig_length] = '\0';
                result->scriptsig = tag;
            } else {
                free(tag);
                // Tag extraction failed due to length mismatch, but we can continue
            }
        }
    }

    // 3. Parse Coinbase 2 for Outputs
    // Calculate offset in coinbase_2 where outputs start
    // Re-calculate raw remainder length without subtracting extranonces
    int raw_scriptsig_remainder = (scriptsig_len - 1 - block_height_len) - (coinbase_1_len - coinbase_1_offset);
    
    int coinbase_2_offset = 0;
    if (raw_scriptsig_remainder > 0) {
        // Subtract extranonce lengths to see what's left for coinbase_2
        int remainder_in_coinbase_2 = raw_scriptsig_remainder - (extranonce1_len + extranonce2_len);
        if (remainder_in_coinbase_2 > 0) {
            coinbase_2_offset = remainder_in_coinbase_2;
        }
    }
    
    int coinbase_2_len = strlen(notification->coinbase_2) / 2;
    uint8_t *coinbase_2_bin = malloc(coinbase_2_len);
    if (!coinbase_2_bin) {
        return ESP_ERR_NO_MEM; // Memory error is fatal
    }
    
    hex2bin(notification->coinbase_2, coinbase_2_bin, coinbase_2_len);
    
    int offset = coinbase_2_offset;
    
    // Read sequence (4 bytes) for BIP-54 detection
    if (offset + 4 > coinbase_2_len) {
        free(coinbase_2_bin);
        return ESP_ERR_INVALID_ARG; // No room for outputs, but valid notification processed so far
    }
    uint32_t nSequence = 0;
    for (int i = 0; i < 4; i++) {
        nSequence |= ((uint32_t)coinbase_2_bin[offset + i]) << (i * 8);
    }
    offset += 4;
    
    // Decode output count
    if (offset >= coinbase_2_len) {
        free(coinbase_2_bin);
        return ESP_ERR_INVALID_ARG;
    }
    
    uint64_t num_outputs = coinbase_decode_varint(coinbase_2_bin, &offset);
    result->output_count = 0;
    
    // Parse each output
    for (uint64_t i = 0; i < num_outputs && offset < coinbase_2_len; i++) {
        // Read value (8 bytes, little-endian)
        if (offset + 8 > coinbase_2_len) break;

        uint64_t value_satoshis = 0;
        for (int i = 0; i < 8; i++) {
            value_satoshis |= ((uint64_t)coinbase_2_bin[offset + i]) << (i * 8);
        }
        offset += 8;

        // Add to total value
        result->total_value_satoshis += value_satoshis;

        // Read scriptPubKey length
        if (offset >= coinbase_2_len) break;
        uint64_t script_len = coinbase_decode_varint(coinbase_2_bin, &offset);

        if (offset + script_len > coinbase_2_len) break;

        if (decode_coinbase_tx) {
            const uint8_t *script_ptr = coinbase_2_bin + offset;
            if (value_satoshis > 0) {
                char output_address[MAX_ADDRESS_STRING_LEN];
                coinbase_decode_address_with_params(script_ptr, script_len, output_address, MAX_ADDRESS_STRING_LEN, &result->resolved_coin);
                bool is_user_address = (user_addr_part[0] != '\0' && strcmp(user_addr_part, output_address) == 0) ||
                                       (has_user_payload && script_matches_user_payload(script_ptr, script_len, &user_payload));

                if (is_user_address) result->user_value_satoshis += value_satoshis;

                if (i < MAX_COINBASE_TX_OUTPUTS) {
                    strncpy(result->outputs[i].address, output_address, MAX_ADDRESS_STRING_LEN - 1);
                    result->outputs[i].address[MAX_ADDRESS_STRING_LEN - 1] = '\0';
                    result->outputs[i].value_satoshis = value_satoshis;
                    result->outputs[i].is_user_output = is_user_address;
                    result->output_count++;
                }
            } else {
                if (i < MAX_COINBASE_TX_OUTPUTS) {
                    coinbase_decode_address_with_params(script_ptr, script_len, result->outputs[i].address, MAX_ADDRESS_STRING_LEN, &result->resolved_coin);
                    result->outputs[i].value_satoshis = 0;
                    result->outputs[i].is_user_output = false;
                    result->output_count++;
                }
            }
        }

        offset += script_len;
    }
    
    // Read nLockTime (4 bytes at the end of the transaction) for BIP-54 detection
    uint32_t nLockTime = 0;
    if (offset + 4 <= coinbase_2_len) {
        for (int i = 0; i < 4; i++) {
            nLockTime |= ((uint32_t)coinbase_2_bin[offset + i]) << (i * 8);
        }
    }
    
    // Detect BIP-54 signaling only on Bitcoin family chains
    result->bip54_signaling = decode_coinbase_tx &&
                              result->resolved_coin.is_btc_family &&
                              (result->block_height > 0) &&
                              (nLockTime == result->block_height - 1) &&
                              (nSequence != 0xffffffff);
    
    free(coinbase_2_bin);
    return ESP_OK;
}

esp_err_t coinbase_process_notification(const mining_notify *notification,
                                        const char *extranonce1,
                                        int extranonce2_len,
                                        const char *user_address,
                                        bool decode_coinbase_tx,
                                        mining_notification_result_t *result) {
    return coinbase_process_notification_ex(notification,
                                            extranonce1,
                                            extranonce2_len,
                                            user_address,
                                            NULL,
                                            COIN_PROFILE_AUTO,
                                            decode_coinbase_tx,
                                            result);
}
