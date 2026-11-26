#include <stdio.h>
#include <string.h>
#include "unity.h"
#include "coinbase_decoder.h"

TEST_CASE("Varint decode single byte", "[coinbase_decoder]")
{
    uint8_t data[] = {0x42};
    int offset = 0;
    uint64_t result = coinbase_decode_varint(data, &offset);
    TEST_ASSERT_TRUE(0x42 == result);
    TEST_ASSERT_EQUAL_INT(1, offset);
}

TEST_CASE("Varint decode FD format", "[coinbase_decoder]")
{
    uint8_t data[] = {0xFD, 0x34, 0x12};  // 0x1234 in little-endian
    int offset = 0;
    uint64_t result = coinbase_decode_varint(data, &offset);
    TEST_ASSERT_TRUE(0x1234 == result);
    TEST_ASSERT_EQUAL_INT(3, offset);
}

TEST_CASE("Varint decode FE format", "[coinbase_decoder]")
{
    uint8_t data[] = {0xFE, 0x78, 0x56, 0x34, 0x12};  // 0x12345678 in little-endian
    int offset = 0;
    uint64_t result = coinbase_decode_varint(data, &offset);
    TEST_ASSERT_TRUE(0x12345678 == result);
    TEST_ASSERT_EQUAL_INT(5, offset);
}

TEST_CASE("Varint decode FF format", "[coinbase_decoder]")
{
    uint8_t data[] = {0xFF, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    int offset = 0;
    uint64_t result = coinbase_decode_varint(data, &offset);
    TEST_ASSERT_TRUE(0x0807060504030201ULL == result);
    TEST_ASSERT_EQUAL_INT(9, offset);
}

TEST_CASE("Decode P2PKH address", "[coinbase_decoder]")
{
    // P2PKH: OP_DUP OP_HASH160 <20 bytes> OP_EQUALVERIFY OP_CHECKSIG
    uint8_t script[] = {
        0x76, 0xa9, 0x14,
        0x89, 0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67, 0x89, 0xab,
        0xcd, 0xef, 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
        0x88, 0xac
    };
    char output[MAX_ADDRESS_STRING_LEN];
    
    coinbase_decode_address_from_scriptpubkey(script, sizeof(script), output, sizeof(output), "bc", false);
    
    TEST_ASSERT_EQUAL_STRING("1DYwPTnC4NgEmoqbLbcRqoSzVeH3ehmGbV", output);
}

TEST_CASE("Decode P2SH address", "[coinbase_decoder]")
{
    // P2SH: OP_HASH160 <20 bytes> OP_EQUAL
    uint8_t script[] = {
        0xa9, 0x14,
        0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0, 0x12, 0x34,
        0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0, 0x12, 0x34, 0x56, 0x78,
        0x87
    };
    char output[MAX_ADDRESS_STRING_LEN];
    
    coinbase_decode_address_from_scriptpubkey(script, sizeof(script), output, sizeof(output), "bc", false);
    
    TEST_ASSERT_EQUAL_STRING("33MGnVL6rnKqt6Jjt3HbRqWJrhwy65dMhS", output);
}

TEST_CASE("Decode P2WPKH address", "[coinbase_decoder]")
{
    // P2WPKH: OP_0 <20 bytes>
    uint8_t script[] = {
        0x00, 0x14,
        0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00, 0x11, 0x22, 0x33,
        0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd
    };
    char output[MAX_ADDRESS_STRING_LEN];
    
    coinbase_decode_address_from_scriptpubkey(script, sizeof(script), output, sizeof(output), "bc", false);
    
    TEST_ASSERT_EQUAL_STRING("bc1q42aueh0wluqpzg3ng32kvaugnx4thnxa7y625x", output);
}

TEST_CASE("Decode P2WSH address", "[coinbase_decoder]")
{
    // P2WSH: OP_0 <32 bytes>
    uint8_t script[] = {
        0x00, 0x20,
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
        0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
        0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20
    };
    char output[MAX_ADDRESS_STRING_LEN];
    
    coinbase_decode_address_from_scriptpubkey(script, sizeof(script), output, sizeof(output), "bc", false);
    
    TEST_ASSERT_EQUAL_STRING("bc1qqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7rusqyp0mu0", output);
}

TEST_CASE("Decode P2TR address", "[coinbase_decoder]")
{
    // P2TR: OP_1 <32 bytes>
    uint8_t script[] = {
        0x51, 0x20,
        0xff, 0xee, 0xdd, 0xcc, 0xbb, 0xaa, 0x99, 0x88,
        0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x00,
        0xff, 0xee, 0xdd, 0xcc, 0xbb, 0xaa, 0x99, 0x88,
        0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x00
    };
    char output[MAX_ADDRESS_STRING_LEN];
    
    coinbase_decode_address_from_scriptpubkey(script, sizeof(script), output, sizeof(output), "bc", false);
    
    TEST_ASSERT_EQUAL_STRING("bc1pllhdmn9m42vcsamx24zrxgs3qrl7ahwvhw4fnzrhve25gvezzyqqc0cgpt", output);
}

// Testnet address tests

TEST_CASE("Decode testnet P2PKH address", "[coinbase_decoder]")
{
    // Same hash as mainnet P2PKH test, but with testnet version byte (0x6F)
    uint8_t script[] = {
        0x76, 0xa9, 0x14,
        0x89, 0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67, 0x89, 0xab,
        0xcd, 0xef, 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
        0x88, 0xac
    };
    char output[MAX_ADDRESS_STRING_LEN];

    coinbase_decode_address_from_scriptpubkey(script, sizeof(script), output, sizeof(output), "tb", true);

    // Testnet P2PKH addresses start with 'm' or 'n'
    TEST_ASSERT_TRUE(output[0] == 'm' || output[0] == 'n');
}

TEST_CASE("Decode testnet P2SH address", "[coinbase_decoder]")
{
    // Same hash as mainnet P2SH test, but with testnet version byte (0xC4)
    uint8_t script[] = {
        0xa9, 0x14,
        0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0, 0x12, 0x34,
        0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0, 0x12, 0x34, 0x56, 0x78,
        0x87
    };
    char output[MAX_ADDRESS_STRING_LEN];

    coinbase_decode_address_from_scriptpubkey(script, sizeof(script), output, sizeof(output), "tb", true);

    // Testnet P2SH addresses start with '2'
    TEST_ASSERT_EQUAL_CHAR('2', output[0]);
}

TEST_CASE("Decode testnet P2WPKH address", "[coinbase_decoder]")
{
    // Same hash as mainnet P2WPKH test, but with "tb" HRP
    uint8_t script[] = {
        0x00, 0x14,
        0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00, 0x11, 0x22, 0x33,
        0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd
    };
    char output[MAX_ADDRESS_STRING_LEN];

    coinbase_decode_address_from_scriptpubkey(script, sizeof(script), output, sizeof(output), "tb", true);

    TEST_ASSERT_TRUE(strncmp(output, "tb1q", 4) == 0);
}

TEST_CASE("Decode testnet P2TR address", "[coinbase_decoder]")
{
    // Same hash as mainnet P2TR test, but with "tb" HRP
    uint8_t script[] = {
        0x51, 0x20,
        0xff, 0xee, 0xdd, 0xcc, 0xbb, 0xaa, 0x99, 0x88,
        0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x00,
        0xff, 0xee, 0xdd, 0xcc, 0xbb, 0xaa, 0x99, 0x88,
        0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x00
    };
    char output[MAX_ADDRESS_STRING_LEN];

    coinbase_decode_address_from_scriptpubkey(script, sizeof(script), output, sizeof(output), "tb", true);

    TEST_ASSERT_TRUE(strncmp(output, "tb1p", 4) == 0);
}

TEST_CASE("Decode regtest P2WPKH address", "[coinbase_decoder]")
{
    // Same hash as mainnet P2WPKH test, but with "bcrt" HRP
    uint8_t script[] = {
        0x00, 0x14,
        0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00, 0x11, 0x22, 0x33,
        0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd
    };
    char output[MAX_ADDRESS_STRING_LEN];

    coinbase_decode_address_from_scriptpubkey(script, sizeof(script), output, sizeof(output), "bcrt", true);

    TEST_ASSERT_TRUE(strncmp(output, "bcrt1q", 6) == 0);
}

// Network auto-detection tests via coinbase_process_notification are
// integration-level — the detection logic is tested implicitly through
// the address prefix matching in the full processing pipeline.

TEST_CASE("BIP-110 signaling not detected", "[coinbase_decoder]")
{
    // Create a mining_notify without BIP-110 bit set
    mining_notify notify = { 0 };
    notify.version = 0x20000000;  // No BIP-110 signaling
    notify.job_id = "test_job";
    notify.coinbase_1 = "01000000010000000000000000000000000000000000000000000000000000000000000000ffffffff4b03a5020cfabe6d6d379ae882651f6469f2ed6b8b40a4f9a4b41fd838a3ad6de8cba775f4e8f1d3080100000000000000";
    notify.coinbase_2 = "41903d4c1b2f736c7573682f0000000003ca890d27000000001976a9147c154ed1dc59609e3d26abb2df2ea3d587cd8c4188ac00000000000000002c6a4c2952534b424c4f434b3a4cb4cb2ddfc37c41baf5ef6b6b4899e3253a8f1dfc7e5dd68a5b5b27005014ef0000000000000000266a24aa21a9ed5caa249f1af9fbf71c986fea8e076ca34ae3514fb2f86400561b28c7b15949bf00000000";
    
    mining_notification_result_t result = { 0 };
    
    // Use valid extranonce1 (8 hex chars = 4 bytes)
    esp_err_t err = coinbase_process_notification(&notify, "01020304", 8, "", true, &result);
    TEST_ASSERT_EQUAL(ESP_OK, err);
    TEST_ASSERT_FALSE(result.bip110_signaling);
}

TEST_CASE("BIP-110 signaling detected", "[coinbase_decoder]")
{
    // Create a mining_notify with BIP-110 bit set (bit 4 = 0x00000010)
    mining_notify notify = { 0 };
    notify.version = 0x20000010;  // Version with BIP-110 signaling
    notify.job_id = "test_job";
    notify.coinbase_1 = "01000000010000000000000000000000000000000000000000000000000000000000000000ffffffff4b03a5020cfabe6d6d379ae882651f6469f2ed6b8b40a4f9a4b41fd838a3ad6de8cba775f4e8f1d3080100000000000000";
    notify.coinbase_2 = "41903d4c1b2f736c7573682f0000000003ca890d27000000001976a9147c154ed1dc59609e3d26abb2df2ea3d587cd8c4188ac00000000000000002c6a4c2952534b424c4f434b3a4cb4cb2ddfc37c41baf5ef6b6b4899e3253a8f1dfc7e5dd68a5b5b27005014ef0000000000000000266a24aa21a9ed5caa249f1af9fbf71c986fea8e076ca34ae3514fb2f86400561b28c7b15949bf00000000";
    
    mining_notification_result_t result = { 0 };
    
    // Use valid extranonce1 (8 hex chars = 4 bytes)
    esp_err_t err = coinbase_process_notification(&notify, "01020304", 8, "", true, &result);
    TEST_ASSERT_EQUAL(ESP_OK, err);
    TEST_ASSERT_TRUE(result.bip110_signaling);
}

TEST_CASE("BIP-110 signaling last block", "[coinbase_decoder]")
{
    // Create a mining_notify with BIP-110 bit set (bit 4 = 0x00000010)
    mining_notify notify = { 0 };
    notify.version = 0x20000010;  // Version with BIP-110 signaling
    notify.job_id = "test_job";
    notify.coinbase_1 = "01000000010000000000000000000000000000000000000000000000000000000000000000ffffffff4b031fbc0efabe6d6d379ae882651f6469f2ed6b8b40a4f9a4b41fd838a3ad6de8cba775f4e8f1d3080100000000000000";
    notify.coinbase_2 = "41903d4c1b2f736c7573682f0000000003ca890d27000000001976a9147c154ed1dc59609e3d26abb2df2ea3d587cd8c4188ac00000000000000002c6a4c2952534b424c4f434b3a4cb4cb2ddfc37c41baf5ef6b6b4899e3253a8f1dfc7e5dd68a5b5b27005014ef0000000000000000266a24aa21a9ed5caa249f1af9fbf71c986fea8e076ca34ae3514fb2f86400561b28c7b15949bf00000000";
    
    mining_notification_result_t result = { 0 };
    
    // Use valid extranonce1 (8 hex chars = 4 bytes)
    esp_err_t err = coinbase_process_notification(&notify, "01020304", 8, "", true, &result);
    TEST_ASSERT_EQUAL(ESP_OK, err);
    TEST_ASSERT_EQUAL(965663, result.block_height);
    TEST_ASSERT_TRUE(result.bip110_signaling);
}

TEST_CASE("BIP-110 signaling expired", "[coinbase_decoder]")
{
    // Create a mining_notify with BIP-110 bit set (bit 4 = 0x00000010)
    mining_notify notify = { 0 };
    notify.version = 0x20000010;  // Version with BIP-110 signaling
    notify.job_id = "test_job";
    notify.coinbase_1 = "01000000010000000000000000000000000000000000000000000000000000000000000000ffffffff4b0320bc0efabe6d6d379ae882651f6469f2ed6b8b40a4f9a4b41fd838a3ad6de8cba775f4e8f1d3080100000000000000";
    notify.coinbase_2 = "41903d4c1b2f736c7573682f0000000003ca890d27000000001976a9147c154ed1dc59609e3d26abb2df2ea3d587cd8c4188ac00000000000000002c6a4c2952534b424c4f434b3a4cb4cb2ddfc37c41baf5ef6b6b4899e3253a8f1dfc7e5dd68a5b5b27005014ef0000000000000000266a24aa21a9ed5caa249f1af9fbf71c986fea8e076ca34ae3514fb2f86400561b28c7b15949bf00000000";
    
    mining_notification_result_t result = { 0 };
    
    // Use valid extranonce1 (8 hex chars = 4 bytes)
    esp_err_t err = coinbase_process_notification(&notify, "01020304", 8, "", true, &result);
    TEST_ASSERT_EQUAL(ESP_OK, err);
    TEST_ASSERT_EQUAL(965664, result.block_height);
    TEST_ASSERT_FALSE(result.bip110_signaling);
}

TEST_CASE("Multi-coin registry and auto-detection resolve DGB, PPC, NMC, XEC, BTC, and Generic", "[coinbase_decoder]")
{
    CoinParams params = {0};
    bool user_is_addr = false;

    // 1. DGB P2PKH with .worker suffix
    coin_resolve_params(COIN_PROFILE_AUTO, "DJKYme7vLUcgDAJGqjXbcmqdRVWnH5xQnU.Bitaxe01", "lat-dgb.solofury.com", &params, &user_is_addr);
    TEST_ASSERT_EQUAL(COIN_PROFILE_DGB, params.id);
    TEST_ASSERT_EQUAL_STRING("DGB", params.ticker);
    TEST_ASSERT_EQUAL_UINT8(8, params.decimals);
    TEST_ASSERT_EQUAL_UINT16(15, params.block_time_s);
    TEST_ASSERT_EQUAL_HEX8(0x1E, params.p2pkh_version);
    TEST_ASSERT_EQUAL_HEX8(0x3F, params.p2sh_version);
    TEST_ASSERT_EQUAL_STRING("dgb", params.bech32_hrp);
    TEST_ASSERT_FALSE(params.is_btc_family);
    TEST_ASSERT_TRUE(user_is_addr);

    // 2. DGB Bech32
    coin_resolve_params(COIN_PROFILE_AUTO, "dgb1q42aueh0wluqpzg3ng32kvaugnx4thnxafl7jvg", "solo.digibyte.org", &params, &user_is_addr);
    TEST_ASSERT_EQUAL(COIN_PROFILE_DGB, params.id);
    TEST_ASSERT_EQUAL_STRING("DGB", params.ticker);
    TEST_ASSERT_TRUE(user_is_addr);

    // 3. PPC & NMC Base58 versions
    coin_resolve_params(COIN_PROFILE_AUTO, "PAdonateF5tG3mW8N2Q9p6sV4L9wK1Z3bX", "ppc.pool.example", &params, &user_is_addr);
    TEST_ASSERT_EQUAL(COIN_PROFILE_PPC, params.id);
    TEST_ASSERT_EQUAL_STRING("PPC", params.ticker);
    TEST_ASSERT_FALSE(user_is_addr); // synthetic invalid checksum triggers URL hint instead

    coin_resolve_params(COIN_PROFILE_AUTO, "NMcAccount.01", "nmc.pool.example", &params, &user_is_addr);
    TEST_ASSERT_EQUAL(COIN_PROFILE_NMC, params.id);
    TEST_ASSERT_EQUAL_STRING("NMC", params.ticker);
    TEST_ASSERT_FALSE(user_is_addr);

    // 4. Explicit override beats URL hint
    coin_resolve_params(COIN_PROFILE_XEC, "1DYwPTnC4NgEmoqbLbcRqoSzVeH3ehmGbV.rig1", "btc.pool.example", &params, &user_is_addr);
    TEST_ASSERT_EQUAL(COIN_PROFILE_XEC, params.id);
    TEST_ASSERT_EQUAL_STRING("XEC", params.ticker);
    TEST_ASSERT_EQUAL_UINT8(2, params.decimals);
    TEST_ASSERT_TRUE(user_is_addr);

    // 5. Generic fallback for non-address pool username
    coin_resolve_params(COIN_PROFILE_AUTO, "alice.rig1", "stratum.unknownpool.org", &params, &user_is_addr);
    TEST_ASSERT_EQUAL(COIN_PROFILE_GENERIC, params.id);
    TEST_ASSERT_EQUAL_STRING("SHA256", params.ticker);
    TEST_ASSERT_FALSE(user_is_addr);
}

TEST_CASE("DGB solo coinbase decodes native addresses and matches user payout", "[coinbase_decoder]")
{
    // Output 0: 99,000,000,000 sats (990 DGB) = 0x170cdc1e00 -> LE: 001edc0c17000000
    // Output 1:  1,000,000,000 sats (10 DGB) = 0x3b9aca00 -> LE: 00ca9a3b00000000
    mining_notify notify = { 0 };
    notify.version = 0x20000010;
    notify.job_id = "dgb_job";
    notify.coinbase_1 = "01000000010000000000000000000000000000000000000000000000000000000000000000ffffffff0b02a502736f6c6f646762";
    // coinbase_2: nSequence(feffffff) + num_outputs(02) +
    // out0: 001edc0c17000000 + len(19) + 76a914 9093b0ed1d457129f65dff4c4d3669c8c8b8acfe 88ac +
    // out1: 00ca9a3b00000000 + len(16) + 0014 aabbccddeeff00112233445566778899aabbccdd +
    // nLockTime(a4020000 = 676 = height - 1)
    notify.coinbase_2 = "feffffff02"
                        "001edc0c170000001976a9149093b0ed1d457129f65dff4c4d3669c8c8b8acfe88ac"
                        "00ca9a3b00000000160014aabbccddeeff00112233445566778899aabbccdd"
                        "a4020000";

    mining_notification_result_t result = { 0 };
    esp_err_t err = coinbase_process_notification_ex(
        &notify, "01020304", 0,
        "DJKYme7vLUcgDAJGqjXbcmqdRVWnH5xQnU.Bitaxe01",
        "lat-dgb.solofury.com",
        COIN_PROFILE_AUTO,
        true,
        &result);
    if (result.scriptsig) free(result.scriptsig);

    TEST_ASSERT_EQUAL(ESP_OK, err);
    TEST_ASSERT_EQUAL(COIN_PROFILE_DGB, result.resolved_coin.id);
    TEST_ASSERT_EQUAL_STRING("DGB", result.resolved_coin.ticker);
    TEST_ASSERT_EQUAL_UINT16(15, result.resolved_coin.block_time_s);
    TEST_ASSERT_TRUE(result.user_is_address);
    TEST_ASSERT_EQUAL_INT(2, result.output_count);
    TEST_ASSERT_EQUAL_STRING("DJKYme7vLUcgDAJGqjXbcmqdRVWnH5xQnU", result.outputs[0].address);
    TEST_ASSERT_TRUE(result.outputs[0].is_user_output);
    TEST_ASSERT_TRUE(strncmp(result.outputs[1].address, "dgb1q", 5) == 0);
    TEST_ASSERT_FALSE(result.outputs[1].is_user_output);
    TEST_ASSERT_TRUE(result.user_value_satoshis == 99000000000ULL);
    TEST_ASSERT_TRUE(result.total_value_satoshis == 100000000000ULL);
    // BIP-54 and BIP-110 must both stay false for non-BTC coins
    TEST_ASSERT_FALSE(result.bip54_signaling);
    TEST_ASSERT_FALSE(result.bip110_signaling);
}

TEST_CASE("Raw script payload matches user payout even with Generic prefix and BTC keeps BIP signaling", "[coinbase_decoder]")
{
    mining_notify notify = { 0 };
    notify.version = 0x20000010;
    notify.job_id = "payload_job";
    notify.coinbase_1 = "01000000010000000000000000000000000000000000000000000000000000000000000000ffffffff0b02a502736f6c6f646762";
    notify.coinbase_2 = "feffffff02"
                        "001edc0c170000001976a9149093b0ed1d457129f65dff4c4d3669c8c8b8acfe88ac"
                        "00ca9a3b00000000160014aabbccddeeff00112233445566778899aabbccdd"
                        "a4020000";

    // Force Generic profile (uses 0x00 prefix -> 1EBTEPBH...), payload matching must still credit DJKYme7v...
    mining_notification_result_t gen_res = { 0 };
    esp_err_t err = coinbase_process_notification_ex(
        &notify, "01020304", 0,
        "DJKYme7vLUcgDAJGqjXbcmqdRVWnH5xQnU.Bitaxe01",
        "custom.pool.org",
        COIN_PROFILE_GENERIC,
        true,
        &gen_res);
    if (gen_res.scriptsig) free(gen_res.scriptsig);

    TEST_ASSERT_EQUAL(ESP_OK, err);
    TEST_ASSERT_EQUAL_STRING("1EBTEPBH34iPgA7g79Y351g2YMnUwsZ1Cp", gen_res.outputs[0].address);
    TEST_ASSERT_TRUE(gen_res.outputs[0].is_user_output);
    TEST_ASSERT_TRUE(gen_res.user_value_satoshis == 99000000000ULL);
    TEST_ASSERT_FALSE(gen_res.bip54_signaling);
    TEST_ASSERT_FALSE(gen_res.bip110_signaling);

    // Same template processed under BTC profile emits BIP-54 and BIP-110
    mining_notification_result_t btc_res = { 0 };
    err = coinbase_process_notification_ex(
        &notify, "01020304", 0,
        "bc1q42aueh0wluqpzg3ng32kvaugnx4thnxa7y625x",
        "solo.ckpool.org",
        COIN_PROFILE_BTC,
        true,
        &btc_res);
    if (btc_res.scriptsig) free(btc_res.scriptsig);

    TEST_ASSERT_EQUAL(ESP_OK, err);
    TEST_ASSERT_TRUE(btc_res.resolved_coin.is_btc_family);
    TEST_ASSERT_TRUE(btc_res.bip54_signaling);
    TEST_ASSERT_TRUE(btc_res.bip110_signaling);
    TEST_ASSERT_FALSE(btc_res.outputs[0].is_user_output);
    TEST_ASSERT_TRUE(btc_res.outputs[1].is_user_output);
    TEST_ASSERT_TRUE(btc_res.user_value_satoshis == 1000000000ULL);
}