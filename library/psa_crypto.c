/*
 *  PSA crypto layer on top of Mbed TLS crypto
 */
/*  Copyright (C) 2018, ARM Limited, All Rights Reserved
 *  SPDX-License-Identifier: Apache-2.0
 *
 *  Licensed under the Apache License, Version 2.0 (the "License"); you may
 *  not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 *  WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 *  This file is part of mbed TLS (https://tls.mbed.org)
 */

#if !defined(MBEDTLS_CONFIG_FILE)
#include "mbedtls/config.h"
#else
#include MBEDTLS_CONFIG_FILE
#endif

#if defined(MBEDTLS_PSA_CRYPTO_C)

#include "psa/crypto.h"

#include <stdlib.h>
#include <string.h>
#if defined(MBEDTLS_PLATFORM_C)
#include "mbedtls/platform.h"
#else
#define mbedtls_calloc calloc
#define mbedtls_free   free
#endif

#include "mbedtls/arc4.h"
#include "mbedtls/blowfish.h"
#include "mbedtls/camellia.h"
#include "mbedtls/cipher.h"
#include "mbedtls/ccm.h"
#include "mbedtls/cmac.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/des.h"
#include "mbedtls/ecp.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/gcm.h"
#include "mbedtls/md2.h"
#include "mbedtls/md4.h"
#include "mbedtls/md5.h"
#include "mbedtls/md.h"
#include "mbedtls/md_internal.h"
#include "mbedtls/pk.h"
#include "mbedtls/pk_internal.h"
#include "mbedtls/ripemd160.h"
#include "mbedtls/rsa.h"
#include "mbedtls/sha1.h"
#include "mbedtls/sha256.h"
#include "mbedtls/sha512.h"
#include "mbedtls/xtea.h"



/* Implementation that should never be optimized out by the compiler */
static void mbedtls_zeroize( void *v, size_t n )
{
    volatile unsigned char *p = v; while( n-- ) *p++ = 0;
}

/* constant-time buffer comparison */
static inline int safer_memcmp( const uint8_t *a, const uint8_t *b, size_t n )
{
    size_t i;
    unsigned char diff = 0;

    for( i = 0; i < n; i++ )
        diff |= a[i] ^ b[i];

    return( diff );
}



/****************************************************************/
/* Global data, support functions and library management */
/****************************************************************/

/* Number of key slots (plus one because 0 is not used).
 * The value is a compile-time constant for now, for simplicity. */
#define MBEDTLS_PSA_KEY_SLOT_COUNT 32

typedef struct {
    psa_key_type_t type;
    union {
        struct raw_data {
            uint8_t *data;
            size_t bytes;
        } raw;
#if defined(MBEDTLS_RSA_C)
        mbedtls_rsa_context *rsa;
#endif /* MBEDTLS_RSA_C */
#if defined(MBEDTLS_ECP_C)
        mbedtls_ecp_keypair *ecp;
#endif /* MBEDTLS_ECP_C */
    } data;
} key_slot_t;

typedef struct {
    int initialized;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    key_slot_t key_slots[MBEDTLS_PSA_KEY_SLOT_COUNT];
} psa_global_data_t;

static psa_global_data_t global_data;

static psa_status_t mbedtls_to_psa_error( int ret )
{
    /* If there's both a high-level code and low-level code, dispatch on
     * the high-level code. */
    switch( ret < -0x7f ? - ( -ret & 0x7f80 ) : ret )
    {
        case 0:
            return( PSA_SUCCESS );

        case MBEDTLS_ERR_AES_INVALID_KEY_LENGTH:
        case MBEDTLS_ERR_AES_INVALID_INPUT_LENGTH:
        case MBEDTLS_ERR_AES_FEATURE_UNAVAILABLE:
            return( PSA_ERROR_NOT_SUPPORTED );
        case MBEDTLS_ERR_AES_HW_ACCEL_FAILED:
            return( PSA_ERROR_HARDWARE_FAILURE );

        case MBEDTLS_ERR_ARC4_HW_ACCEL_FAILED:
            return( PSA_ERROR_HARDWARE_FAILURE );

        case MBEDTLS_ERR_BLOWFISH_INVALID_KEY_LENGTH:
        case MBEDTLS_ERR_BLOWFISH_INVALID_INPUT_LENGTH:
            return( PSA_ERROR_NOT_SUPPORTED );
        case MBEDTLS_ERR_BLOWFISH_HW_ACCEL_FAILED:
            return( PSA_ERROR_HARDWARE_FAILURE );

        case MBEDTLS_ERR_CAMELLIA_INVALID_KEY_LENGTH:
        case MBEDTLS_ERR_CAMELLIA_INVALID_INPUT_LENGTH:
            return( PSA_ERROR_NOT_SUPPORTED );
        case MBEDTLS_ERR_CAMELLIA_HW_ACCEL_FAILED:
            return( PSA_ERROR_HARDWARE_FAILURE );

        case MBEDTLS_ERR_CCM_BAD_INPUT:
            return( PSA_ERROR_INVALID_ARGUMENT );
        case MBEDTLS_ERR_CCM_AUTH_FAILED:
            return( PSA_ERROR_INVALID_SIGNATURE );
        case MBEDTLS_ERR_CCM_HW_ACCEL_FAILED:
            return( PSA_ERROR_HARDWARE_FAILURE );

        case MBEDTLS_ERR_CIPHER_FEATURE_UNAVAILABLE:
            return( PSA_ERROR_NOT_SUPPORTED );
        case MBEDTLS_ERR_CIPHER_BAD_INPUT_DATA:
            return( PSA_ERROR_INVALID_ARGUMENT );
        case MBEDTLS_ERR_CIPHER_ALLOC_FAILED:
            return( PSA_ERROR_INSUFFICIENT_MEMORY );
        case MBEDTLS_ERR_CIPHER_INVALID_PADDING:
            return( PSA_ERROR_INVALID_PADDING );
        case MBEDTLS_ERR_CIPHER_FULL_BLOCK_EXPECTED:
            return( PSA_ERROR_BAD_STATE );
        case MBEDTLS_ERR_CIPHER_AUTH_FAILED:
            return( PSA_ERROR_INVALID_SIGNATURE );
        case MBEDTLS_ERR_CIPHER_INVALID_CONTEXT:
            return( PSA_ERROR_TAMPERING_DETECTED );
        case MBEDTLS_ERR_CIPHER_HW_ACCEL_FAILED:
            return( PSA_ERROR_HARDWARE_FAILURE );

        case MBEDTLS_ERR_CMAC_HW_ACCEL_FAILED:
            return( PSA_ERROR_HARDWARE_FAILURE );

        case MBEDTLS_ERR_CTR_DRBG_ENTROPY_SOURCE_FAILED:
            return( PSA_ERROR_INSUFFICIENT_ENTROPY );
        case MBEDTLS_ERR_CTR_DRBG_REQUEST_TOO_BIG:
        case MBEDTLS_ERR_CTR_DRBG_INPUT_TOO_BIG:
            return( PSA_ERROR_NOT_SUPPORTED );
        case MBEDTLS_ERR_CTR_DRBG_FILE_IO_ERROR:
            return( PSA_ERROR_INSUFFICIENT_ENTROPY );

        case MBEDTLS_ERR_DES_INVALID_INPUT_LENGTH:
            return( PSA_ERROR_NOT_SUPPORTED );
        case MBEDTLS_ERR_DES_HW_ACCEL_FAILED:
            return( PSA_ERROR_HARDWARE_FAILURE );

        case MBEDTLS_ERR_ENTROPY_NO_SOURCES_DEFINED:
        case MBEDTLS_ERR_ENTROPY_NO_STRONG_SOURCE:
        case MBEDTLS_ERR_ENTROPY_SOURCE_FAILED:
            return( PSA_ERROR_INSUFFICIENT_ENTROPY );

        case MBEDTLS_ERR_GCM_AUTH_FAILED:
            return( PSA_ERROR_INVALID_SIGNATURE );
        case MBEDTLS_ERR_GCM_BAD_INPUT:
            return( PSA_ERROR_NOT_SUPPORTED );
        case MBEDTLS_ERR_GCM_HW_ACCEL_FAILED:
            return( PSA_ERROR_HARDWARE_FAILURE );

        case MBEDTLS_ERR_MD2_HW_ACCEL_FAILED:
        case MBEDTLS_ERR_MD4_HW_ACCEL_FAILED:
        case MBEDTLS_ERR_MD5_HW_ACCEL_FAILED:
            return( PSA_ERROR_HARDWARE_FAILURE );

        case MBEDTLS_ERR_MD_FEATURE_UNAVAILABLE:
            return( PSA_ERROR_NOT_SUPPORTED );
        case MBEDTLS_ERR_MD_BAD_INPUT_DATA:
            return( PSA_ERROR_INVALID_ARGUMENT );
        case MBEDTLS_ERR_MD_ALLOC_FAILED:
            return( PSA_ERROR_INSUFFICIENT_MEMORY );
        case MBEDTLS_ERR_MD_FILE_IO_ERROR:
            return( PSA_ERROR_STORAGE_FAILURE );
        case MBEDTLS_ERR_MD_HW_ACCEL_FAILED:
            return( PSA_ERROR_HARDWARE_FAILURE );

        case MBEDTLS_ERR_PK_ALLOC_FAILED:
            return( PSA_ERROR_INSUFFICIENT_MEMORY );
        case MBEDTLS_ERR_PK_TYPE_MISMATCH:
        case MBEDTLS_ERR_PK_BAD_INPUT_DATA:
            return( PSA_ERROR_INVALID_ARGUMENT );
        case MBEDTLS_ERR_PK_FILE_IO_ERROR:
            return( PSA_ERROR_STORAGE_FAILURE );
        case MBEDTLS_ERR_PK_KEY_INVALID_VERSION:
        case MBEDTLS_ERR_PK_KEY_INVALID_FORMAT:
            return( PSA_ERROR_INVALID_ARGUMENT );
        case MBEDTLS_ERR_PK_UNKNOWN_PK_ALG:
            return( PSA_ERROR_NOT_SUPPORTED );
        case MBEDTLS_ERR_PK_PASSWORD_REQUIRED:
        case MBEDTLS_ERR_PK_PASSWORD_MISMATCH:
            return( PSA_ERROR_NOT_PERMITTED );
        case MBEDTLS_ERR_PK_INVALID_PUBKEY:
            return( PSA_ERROR_INVALID_ARGUMENT );
        case MBEDTLS_ERR_PK_INVALID_ALG:
        case MBEDTLS_ERR_PK_UNKNOWN_NAMED_CURVE:
        case MBEDTLS_ERR_PK_FEATURE_UNAVAILABLE:
            return( PSA_ERROR_NOT_SUPPORTED );
        case MBEDTLS_ERR_PK_SIG_LEN_MISMATCH:
            return( PSA_ERROR_INVALID_SIGNATURE );
        case MBEDTLS_ERR_PK_HW_ACCEL_FAILED:
            return( PSA_ERROR_HARDWARE_FAILURE );

        case MBEDTLS_ERR_RIPEMD160_HW_ACCEL_FAILED:
            return( PSA_ERROR_HARDWARE_FAILURE );

        case MBEDTLS_ERR_RSA_BAD_INPUT_DATA:
            return( PSA_ERROR_INVALID_ARGUMENT );
        case MBEDTLS_ERR_RSA_INVALID_PADDING:
            return( PSA_ERROR_INVALID_PADDING );
        case MBEDTLS_ERR_RSA_KEY_GEN_FAILED:
            return( PSA_ERROR_HARDWARE_FAILURE );
        case MBEDTLS_ERR_RSA_KEY_CHECK_FAILED:
            return( PSA_ERROR_INVALID_ARGUMENT );
        case MBEDTLS_ERR_RSA_PUBLIC_FAILED:
        case MBEDTLS_ERR_RSA_PRIVATE_FAILED:
            return( PSA_ERROR_TAMPERING_DETECTED );
        case MBEDTLS_ERR_RSA_VERIFY_FAILED:
            return( PSA_ERROR_INVALID_SIGNATURE );
        case MBEDTLS_ERR_RSA_OUTPUT_TOO_LARGE:
            return( PSA_ERROR_BUFFER_TOO_SMALL );
        case MBEDTLS_ERR_RSA_RNG_FAILED:
            return( PSA_ERROR_INSUFFICIENT_MEMORY );
        case MBEDTLS_ERR_RSA_UNSUPPORTED_OPERATION:
            return( PSA_ERROR_NOT_SUPPORTED );
        case MBEDTLS_ERR_RSA_HW_ACCEL_FAILED:
            return( PSA_ERROR_HARDWARE_FAILURE );

        case MBEDTLS_ERR_SHA1_HW_ACCEL_FAILED:
        case MBEDTLS_ERR_SHA256_HW_ACCEL_FAILED:
        case MBEDTLS_ERR_SHA512_HW_ACCEL_FAILED:
            return( PSA_ERROR_HARDWARE_FAILURE );

        case MBEDTLS_ERR_XTEA_INVALID_INPUT_LENGTH:
            return( PSA_ERROR_INVALID_ARGUMENT );
        case MBEDTLS_ERR_XTEA_HW_ACCEL_FAILED:
            return( PSA_ERROR_HARDWARE_FAILURE );

        default:
            return( PSA_ERROR_UNKNOWN_ERROR );
    }
}



/****************************************************************/
/* Key management */
/****************************************************************/

psa_status_t psa_import_key(psa_key_slot_t key,
                            psa_key_type_t type,
                            const uint8_t *data,
                            size_t data_length)
{
    key_slot_t *slot;

    if( key == 0 || key > MBEDTLS_PSA_KEY_SLOT_COUNT )
        return( PSA_ERROR_INVALID_ARGUMENT );
    slot = &global_data.key_slots[key];
    if( slot->type != PSA_KEY_TYPE_NONE )
        return( PSA_ERROR_OCCUPIED_SLOT );

    if( type == PSA_KEY_TYPE_RAW_DATA )
    {
        if( data_length > SIZE_MAX / 8 )
            return( PSA_ERROR_NOT_SUPPORTED );
        slot->data.raw.data = mbedtls_calloc( 1, data_length );
        if( slot->data.raw.data == NULL )
            return( PSA_ERROR_INSUFFICIENT_MEMORY );
        memcpy( slot->data.raw.data, data, data_length );
        slot->data.raw.bytes = data_length;
    }
    else
#if defined(MBEDTLS_PK_PARSE_C)
    if( type == PSA_KEY_TYPE_RSA_PUBLIC_KEY ||
        type == PSA_KEY_TYPE_RSA_KEYPAIR ||
        PSA_KEY_TYPE_IS_ECC( type ) )
    {
        int ret;
        mbedtls_pk_context pk;
        mbedtls_pk_init( &pk );
        if( PSA_KEY_TYPE_IS_KEYPAIR( type ) )
            ret = mbedtls_pk_parse_key( &pk, data, data_length, NULL, 0 );
        else
            ret = mbedtls_pk_parse_public_key( &pk, data, data_length );
        if( ret != 0 )
            return( mbedtls_to_psa_error( ret ) );
        switch( mbedtls_pk_get_type( &pk ) )
        {
#if defined(MBEDTLS_RSA_C)
            case MBEDTLS_PK_RSA:
                if( type == PSA_KEY_TYPE_RSA_PUBLIC_KEY ||
                    type == PSA_KEY_TYPE_RSA_KEYPAIR )
                    slot->data.rsa = pk.pk_ctx;
                else
                    return( PSA_ERROR_INVALID_ARGUMENT );
                break;
#endif /* MBEDTLS_RSA_C */
#if defined(MBEDTLS_ECP_C)
            case MBEDTLS_PK_ECKEY:
                if( PSA_KEY_TYPE_IS_ECC( type ) )
                {
                    // TODO: check curve
                    slot->data.ecp = pk.pk_ctx;
                }
                else
                    return( PSA_ERROR_INVALID_ARGUMENT );
                break;
#endif /* MBEDTLS_ECP_C */
            default:
                return( PSA_ERROR_INVALID_ARGUMENT );
        }
    }
    else
#endif /* defined(MBEDTLS_PK_PARSE_C) */
    {
        return( PSA_ERROR_NOT_SUPPORTED );
    }

    slot->type = type;
    return( PSA_SUCCESS );
}

psa_status_t psa_destroy_key(psa_key_slot_t key)
{
    key_slot_t *slot;

    if( key == 0 || key > MBEDTLS_PSA_KEY_SLOT_COUNT )
        return( PSA_ERROR_INVALID_ARGUMENT );
    slot = &global_data.key_slots[key];
    if( slot->type == PSA_KEY_TYPE_NONE )
        return( PSA_ERROR_EMPTY_SLOT );

    if( slot->type == PSA_KEY_TYPE_RAW_DATA )
    {
        mbedtls_free( slot->data.raw.data );
    }
    else
#if defined(MBEDTLS_RSA_C)
    if( slot->type == PSA_KEY_TYPE_RSA_PUBLIC_KEY ||
        slot->type == PSA_KEY_TYPE_RSA_KEYPAIR )
    {
        mbedtls_rsa_free( slot->data.rsa );
    }
    else
#endif /* defined(MBEDTLS_RSA_C) */
#if defined(MBEDTLS_ECP_C)
    if( PSA_KEY_TYPE_IS_ECC( slot->type ) )
    {
        mbedtls_ecp_keypair_free( slot->data.ecp );
    }
    else
#endif /* defined(MBEDTLS_ECP_C) */
    {
        /* Shouldn't happen: the key type is not any type that we
         * put it. */
        return( PSA_ERROR_TAMPERING_DETECTED );
    }

    mbedtls_zeroize( slot, sizeof( *slot ) );
    return( PSA_SUCCESS );
}

psa_status_t psa_get_key_information(psa_key_slot_t key,
                                     psa_key_type_t *type,
                                     size_t *bits)
{
    key_slot_t *slot;

    if( key == 0 || key > MBEDTLS_PSA_KEY_SLOT_COUNT )
        return( PSA_ERROR_EMPTY_SLOT );
    slot = &global_data.key_slots[key];
    if( type != NULL )
        *type = slot->type;
    if( bits != NULL )
        *bits = 0;
    if( slot->type == PSA_KEY_TYPE_NONE )
        return( PSA_ERROR_EMPTY_SLOT );

    if( slot->type == PSA_KEY_TYPE_RAW_DATA )
    {
        if( bits != NULL )
            *bits = slot->data.raw.bytes * 8;
    }
    else
#if defined(MBEDTLS_RSA_C)
    if( slot->type == PSA_KEY_TYPE_RSA_PUBLIC_KEY ||
        slot->type == PSA_KEY_TYPE_RSA_KEYPAIR )
    {
        if( bits != NULL )
            *bits = mbedtls_rsa_get_bitlen( slot->data.rsa );
    }
    else
#endif /* defined(MBEDTLS_RSA_C) */
#if defined(MBEDTLS_ECP_C)
    if( PSA_KEY_TYPE_IS_ECC( slot->type ) )
    {
        if( bits != NULL )
            *bits = slot->data.ecp->grp.pbits;
    }
    else
#endif /* defined(MBEDTLS_ECP_C) */
    {
        /* Shouldn't happen: the key type is not any type that we
         * put it. */
        return( PSA_ERROR_TAMPERING_DETECTED );
    }

    return( PSA_SUCCESS );
}

psa_status_t psa_export_key(psa_key_slot_t key,
                            uint8_t *data,
                            size_t data_size,
                            size_t *data_length)
{
    key_slot_t *slot;

    if( key == 0 || key > MBEDTLS_PSA_KEY_SLOT_COUNT )
        return( PSA_ERROR_EMPTY_SLOT );
    slot = &global_data.key_slots[key];
    if( slot->type == PSA_KEY_TYPE_NONE )
        return( PSA_ERROR_EMPTY_SLOT );

    if( slot->type == PSA_KEY_TYPE_RAW_DATA )
    {
        if( slot->data.raw.bytes > data_size )
            return( PSA_ERROR_BUFFER_TOO_SMALL );
        memcpy( data, slot->data.raw.data, slot->data.raw.bytes );
        *data_length = slot->data.raw.bytes;
        return( PSA_SUCCESS );
    }
    else
#if defined(MBEDTLS_PK_WRITE_C)
    if( slot->type == PSA_KEY_TYPE_RSA_PUBLIC_KEY ||
        slot->type == PSA_KEY_TYPE_RSA_KEYPAIR ||
        PSA_KEY_TYPE_IS_ECC( slot->type ) )
    {
        mbedtls_pk_context pk;
        int ret;
        mbedtls_pk_init( &pk );
        if( slot->type == PSA_KEY_TYPE_RSA_PUBLIC_KEY ||
            slot->type == PSA_KEY_TYPE_RSA_KEYPAIR )
        {
            pk.pk_info = &mbedtls_rsa_info;
            pk.pk_ctx = slot->data.rsa;
        }
        else
        {
            pk.pk_info = &mbedtls_eckey_info;
            pk.pk_ctx = slot->data.ecp;
        }
        if( PSA_KEY_TYPE_IS_KEYPAIR( slot->type ) )
            ret = mbedtls_pk_write_key_der( &pk, data, data_size );
        else
            ret = mbedtls_pk_write_pubkey_der( &pk, data, data_size );
        if( ret < 0 )
            return( mbedtls_to_psa_error( ret ) );
        *data_length = ret;
        return( PSA_SUCCESS );
    }
    else
#endif /* definedMBEDTLS_PK_WRITE_C) */
    {
        return( PSA_ERROR_NOT_SUPPORTED );
    }
}



/****************************************************************/
/* Message digests */
/****************************************************************/

static const mbedtls_md_info_t *mbedtls_md_info_of_psa( psa_algorithm_t alg )
{
    switch( alg )
    {
#if defined(MBEDTLS_MD2_C)
        case PSA_ALG_MD2:
            return( &mbedtls_md2_info );
#endif
#if defined(MBEDTLS_MD4_C)
        case PSA_ALG_MD4:
            return( &mbedtls_md4_info );
#endif
#if defined(MBEDTLS_MD5_C)
        case PSA_ALG_MD5:
            return( &mbedtls_md5_info );
#endif
#if defined(MBEDTLS_RIPEMD160_C)
        case PSA_ALG_RIPEMD160:
            return( &mbedtls_ripemd160_info );
#endif
#if defined(MBEDTLS_SHA1_C)
        case PSA_ALG_SHA_1:
            return( &mbedtls_sha1_info );
#endif
#if defined(MBEDTLS_SHA256_C)
        case PSA_ALG_SHA_224:
            return( &mbedtls_sha224_info );
        case PSA_ALG_SHA_256:
            return( &mbedtls_sha256_info );
#endif
#if defined(MBEDTLS_SHA512_C)
        case PSA_ALG_SHA_384:
            return( &mbedtls_sha384_info );
        case PSA_ALG_SHA_512:
            return( &mbedtls_sha512_info );
#endif
        default:
            return( NULL );
    }
}

#if 0
static psa_algorithm_t mbedtls_md_alg_to_psa( mbedtls_md_type_t md_alg )
{
    switch( md_alg )
    {
        case MBEDTLS_MD_NONE:
            return( 0 );
        case MBEDTLS_MD_MD2:
            return( PSA_ALG_MD2 );
        case MBEDTLS_MD_MD4:
            return( PSA_ALG_MD4 );
        case MBEDTLS_MD_MD5:
            return( PSA_ALG_MD5 );
        case MBEDTLS_MD_SHA1:
            return( PSA_ALG_SHA_1 );
        case MBEDTLS_MD_SHA224:
            return( PSA_ALG_SHA_224 );
        case MBEDTLS_MD_SHA256:
            return( PSA_ALG_SHA_256 );
        case MBEDTLS_MD_SHA384:
            return( PSA_ALG_SHA_384 );
        case MBEDTLS_MD_SHA512:
            return( PSA_ALG_SHA_512 );
        case MBEDTLS_MD_RIPEMD160:
            return( PSA_ALG_RIPEMD160 );
        default:
            return( MBEDTLS_MD_NOT_SUPPORTED );
    }
}
#endif

psa_status_t psa_hash_abort( psa_hash_operation_t *operation )
{
    switch( operation->alg )
    {
#if defined(MBEDTLS_MD2_C)
        case PSA_ALG_MD2:
            mbedtls_md2_free( &operation->ctx.md2 );
            break;
#endif
#if defined(MBEDTLS_MD4_C)
        case PSA_ALG_MD4:
            mbedtls_md4_free( &operation->ctx.md4 );
            break;
#endif
#if defined(MBEDTLS_MD5_C)
        case PSA_ALG_MD5:
            mbedtls_md5_free( &operation->ctx.md5 );
            break;
#endif
#if defined(MBEDTLS_RIPEMD160_C)
        case PSA_ALG_RIPEMD160:
            mbedtls_ripemd160_free( &operation->ctx.ripemd160 );
            break;
#endif
#if defined(MBEDTLS_SHA1_C)
        case PSA_ALG_SHA_1:
            mbedtls_sha1_free( &operation->ctx.sha1 );
            break;
#endif
#if defined(MBEDTLS_SHA256_C)
        case PSA_ALG_SHA_224:
        case PSA_ALG_SHA_256:
            mbedtls_sha256_free( &operation->ctx.sha256 );
            break;
#endif
#if defined(MBEDTLS_SHA512_C)
        case PSA_ALG_SHA_384:
        case PSA_ALG_SHA_512:
            mbedtls_sha512_free( &operation->ctx.sha512 );
            break;
#endif
        default:
            return( PSA_ERROR_NOT_SUPPORTED );
    }
    operation->alg = 0;
    return( PSA_SUCCESS );
}

psa_status_t psa_hash_start( psa_hash_operation_t *operation,
                             psa_algorithm_t alg )
{
    int ret;
    operation->alg = 0;
    switch( alg )
    {
#if defined(MBEDTLS_MD2_C)
        case PSA_ALG_MD2:
            mbedtls_md2_init( &operation->ctx.md2 );
            ret = mbedtls_md2_starts_ret( &operation->ctx.md2 );
            break;
#endif
#if defined(MBEDTLS_MD4_C)
        case PSA_ALG_MD4:
            mbedtls_md4_init( &operation->ctx.md4 );
            ret = mbedtls_md4_starts_ret( &operation->ctx.md4 );
            break;
#endif
#if defined(MBEDTLS_MD5_C)
        case PSA_ALG_MD5:
            mbedtls_md5_init( &operation->ctx.md5 );
            ret = mbedtls_md5_starts_ret( &operation->ctx.md5 );
            break;
#endif
#if defined(MBEDTLS_RIPEMD160_C)
        case PSA_ALG_RIPEMD160:
            mbedtls_ripemd160_init( &operation->ctx.ripemd160 );
            ret = mbedtls_ripemd160_starts_ret( &operation->ctx.ripemd160 );
            break;
#endif
#if defined(MBEDTLS_SHA1_C)
        case PSA_ALG_SHA_1:
            mbedtls_sha1_init( &operation->ctx.sha1 );
            ret = mbedtls_sha1_starts_ret( &operation->ctx.sha1 );
            break;
#endif
#if defined(MBEDTLS_SHA256_C)
        case PSA_ALG_SHA_224:
            mbedtls_sha256_init( &operation->ctx.sha256 );
            ret = mbedtls_sha256_starts_ret( &operation->ctx.sha256, 1 );
            break;
        case PSA_ALG_SHA_256:
            mbedtls_sha256_init( &operation->ctx.sha256 );
            ret = mbedtls_sha256_starts_ret( &operation->ctx.sha256, 0 );
            break;
#endif
#if defined(MBEDTLS_SHA512_C)
        case PSA_ALG_SHA_384:
            mbedtls_sha512_init( &operation->ctx.sha512 );
            ret = mbedtls_sha512_starts_ret( &operation->ctx.sha512, 1 );
            break;
        case PSA_ALG_SHA_512:
            mbedtls_sha512_init( &operation->ctx.sha512 );
            ret = mbedtls_sha512_starts_ret( &operation->ctx.sha512, 0 );
            break;
#endif
        default:
            return( PSA_ERROR_NOT_SUPPORTED );
    }
    if( ret == 0 )
        operation->alg = alg;
    else
        psa_hash_abort( operation );
    return( mbedtls_to_psa_error( ret ) );
}

psa_status_t psa_hash_update( psa_hash_operation_t *operation,
                              const uint8_t *input,
                              size_t input_length )
{
    int ret;
    switch( operation->alg )
    {
#if defined(MBEDTLS_MD2_C)
        case PSA_ALG_MD2:
            ret = mbedtls_md2_update_ret( &operation->ctx.md2,
                                          input, input_length );
            break;
#endif
#if defined(MBEDTLS_MD4_C)
        case PSA_ALG_MD4:
            ret = mbedtls_md4_update_ret( &operation->ctx.md4,
                                          input, input_length );
            break;
#endif
#if defined(MBEDTLS_MD5_C)
        case PSA_ALG_MD5:
            ret = mbedtls_md5_update_ret( &operation->ctx.md5,
                                          input, input_length );
            break;
#endif
#if defined(MBEDTLS_RIPEMD160_C)
        case PSA_ALG_RIPEMD160:
            ret = mbedtls_ripemd160_update_ret( &operation->ctx.ripemd160,
                                                input, input_length );
            break;
#endif
#if defined(MBEDTLS_SHA1_C)
        case PSA_ALG_SHA_1:
            ret = mbedtls_sha1_update_ret( &operation->ctx.sha1,
                                           input, input_length );
            break;
#endif
#if defined(MBEDTLS_SHA256_C)
        case PSA_ALG_SHA_224:
        case PSA_ALG_SHA_256:
            ret = mbedtls_sha256_update_ret( &operation->ctx.sha256,
                                             input, input_length );
            break;
#endif
#if defined(MBEDTLS_SHA512_C)
        case PSA_ALG_SHA_384:
        case PSA_ALG_SHA_512:
            ret = mbedtls_sha512_update_ret( &operation->ctx.sha512,
                                             input, input_length );
            break;
#endif
        default:
            ret = MBEDTLS_ERR_MD_BAD_INPUT_DATA;
            break;
    }
    if( ret != 0 )
        psa_hash_abort( operation );
    return( mbedtls_to_psa_error( ret ) );
}

psa_status_t psa_hash_finish( psa_hash_operation_t *operation,
                              uint8_t *hash,
                              size_t hash_size,
                              size_t *hash_length )
{
    int ret;
    size_t actual_hash_length = PSA_HASH_FINAL_SIZE( operation->alg );

    /* Fill the output buffer with something that isn't a valid hash
     * (barring an attack on the hash and deliberately-crafted input),
     * in case the caller doesn't check the return status properly. */
    *hash_length = actual_hash_length;
    memset( hash, '!', hash_size );

    if( hash_size < actual_hash_length )
        return( PSA_ERROR_BUFFER_TOO_SMALL );

    switch( operation->alg )
    {
#if defined(MBEDTLS_MD2_C)
        case PSA_ALG_MD2:
            ret = mbedtls_md2_finish_ret( &operation->ctx.md2, hash );
            break;
#endif
#if defined(MBEDTLS_MD4_C)
        case PSA_ALG_MD4:
            ret = mbedtls_md4_finish_ret( &operation->ctx.md4, hash );
            break;
#endif
#if defined(MBEDTLS_MD5_C)
        case PSA_ALG_MD5:
            ret = mbedtls_md5_finish_ret( &operation->ctx.md5, hash );
            break;
#endif
#if defined(MBEDTLS_RIPEMD160_C)
        case PSA_ALG_RIPEMD160:
            ret = mbedtls_ripemd160_finish_ret( &operation->ctx.ripemd160, hash );
            break;
#endif
#if defined(MBEDTLS_SHA1_C)
        case PSA_ALG_SHA_1:
            ret = mbedtls_sha1_finish_ret( &operation->ctx.sha1, hash );
            break;
#endif
#if defined(MBEDTLS_SHA256_C)
        case PSA_ALG_SHA_224:
        case PSA_ALG_SHA_256:
            ret = mbedtls_sha256_finish_ret( &operation->ctx.sha256, hash );
            break;
#endif
#if defined(MBEDTLS_SHA512_C)
        case PSA_ALG_SHA_384:
        case PSA_ALG_SHA_512:
            ret = mbedtls_sha512_finish_ret( &operation->ctx.sha512, hash );
            break;
#endif
        default:
            ret = MBEDTLS_ERR_MD_BAD_INPUT_DATA;
            break;
    }

    if( ret == 0 )
    {
        return( psa_hash_abort( operation ) );
    }
    else
    {
        psa_hash_abort( operation );
        return( mbedtls_to_psa_error( ret ) );
    }
}

psa_status_t psa_hash_verify(psa_hash_operation_t *operation,
                             const uint8_t *hash,
                             size_t hash_length)
{
    uint8_t actual_hash[MBEDTLS_MD_MAX_SIZE];
    size_t actual_hash_length;
    psa_status_t status = psa_hash_finish( operation,
                                           actual_hash, sizeof( actual_hash ),
                                           &actual_hash_length );
    if( status != PSA_SUCCESS )
        return( status );
    if( actual_hash_length != hash_length )
        return( PSA_ERROR_INVALID_SIGNATURE );
    if( safer_memcmp( hash, actual_hash, actual_hash_length ) != 0 )
        return( PSA_ERROR_INVALID_SIGNATURE );
    return( PSA_SUCCESS );
}




/****************************************************************/


/****************************************************************/
/* Asymmetric cryptography */
/****************************************************************/

psa_status_t psa_asymmetric_sign(psa_key_slot_t key,
                                 psa_algorithm_t alg,
                                 const uint8_t *hash,
                                 size_t hash_length,
                                 const uint8_t *salt,
                                 size_t salt_length,
                                 uint8_t *signature,
                                 size_t signature_size,
                                 size_t *signature_length)
{
    key_slot_t *slot;

    *signature_length = 0;
    (void) salt;
    (void) salt_length;

    if( key == 0 || key > MBEDTLS_PSA_KEY_SLOT_COUNT )
        return( PSA_ERROR_EMPTY_SLOT );
    slot = &global_data.key_slots[key];
    if( slot->type == PSA_KEY_TYPE_NONE )
        return( PSA_ERROR_EMPTY_SLOT );
    if( ! PSA_KEY_TYPE_IS_KEYPAIR( slot->type ) )
        return( PSA_ERROR_INVALID_ARGUMENT );

#if defined(MBEDTLS_RSA_C)
    if( slot->type == PSA_KEY_TYPE_RSA_KEYPAIR )
    {
        mbedtls_rsa_context *rsa = slot->data.rsa;
        int ret;
        psa_algorithm_t hash_alg = PSA_ALG_RSA_GET_HASH( alg );
        const mbedtls_md_info_t *md_info = mbedtls_md_info_of_psa( hash_alg );
        mbedtls_md_type_t md_alg =
            hash_alg == 0 ? MBEDTLS_MD_NONE : mbedtls_md_get_type( md_info );
        if( md_alg == MBEDTLS_MD_NONE )
        {
#if SIZE_MAX > UINT_MAX
            if( hash_length > UINT_MAX )
                return( PSA_ERROR_INVALID_ARGUMENT );
#endif
        }
        else
        {
            if( mbedtls_md_get_size( md_info ) != hash_length )
                return( PSA_ERROR_INVALID_ARGUMENT );
            if( md_info == NULL )
                return( PSA_ERROR_NOT_SUPPORTED );
        }
        if( signature_size < rsa->len )
            return( PSA_ERROR_BUFFER_TOO_SMALL );
#if defined(MBEDTLS_PKCS1_V15)
        if( PSA_ALG_IS_RSA_PKCS1V15( alg ) )
        {
            mbedtls_rsa_set_padding( rsa, MBEDTLS_RSA_PKCS_V15,
                                     MBEDTLS_MD_NONE );
            ret = mbedtls_rsa_pkcs1_sign( rsa,
                                          mbedtls_ctr_drbg_random,
                                          &global_data.ctr_drbg,
                                          MBEDTLS_RSA_PRIVATE,
                                          md_alg, hash_length, hash,
                                          signature );
        }
        else
#endif /* MBEDTLS_PKCS1_V15 */
#if defined(MBEDTLS_PKCS1_V21)
        if( alg == PSA_ALG_RSA_PSS_MGF1 )
        {
            mbedtls_rsa_set_padding( rsa, MBEDTLS_RSA_PKCS_V21, md_alg );
            ret = mbedtls_rsa_rsassa_pss_sign( rsa,
                                               mbedtls_ctr_drbg_random,
                                               &global_data.ctr_drbg,
                                               MBEDTLS_RSA_PRIVATE,
                                               md_alg, hash_length, hash,
                                               signature );
        }
        else
#endif /* MBEDTLS_PKCS1_V21 */
        {
            return( PSA_ERROR_INVALID_ARGUMENT );
        }
        if( ret == 0 )
            *signature_length = rsa->len;
        return( mbedtls_to_psa_error( ret ) );
    }
    else
#endif /* defined(MBEDTLS_RSA_C) */
#if defined(MBEDTLS_ECP_C)
    if( PSA_KEY_TYPE_IS_ECC( slot->type ) )
    {
        // TODO
        return( PSA_ERROR_NOT_SUPPORTED );
    }
    else
#endif /* defined(MBEDTLS_ECP_C) */
    {
        return( PSA_ERROR_NOT_SUPPORTED );
    }
}



/****************************************************************/
/* Module setup */
/****************************************************************/

void mbedtls_psa_crypto_free( void )
{
    size_t key;
    for( key = 1; key < MBEDTLS_PSA_KEY_SLOT_COUNT; key++ )
        psa_destroy_key( key );
    mbedtls_ctr_drbg_free( &global_data.ctr_drbg );
    mbedtls_entropy_free( &global_data.entropy );
    mbedtls_zeroize( &global_data, sizeof( global_data ) );
}

psa_status_t psa_crypto_init( void )
{
    int ret;
    const unsigned char drbg_seed[] = "PSA";

    if( global_data.initialized != 0 )
        return( PSA_SUCCESS );

    mbedtls_zeroize( &global_data, sizeof( global_data ) );
    mbedtls_entropy_init( &global_data.entropy );
    mbedtls_ctr_drbg_init( &global_data.ctr_drbg );

    ret = mbedtls_ctr_drbg_seed( &global_data.ctr_drbg,
                                 mbedtls_entropy_func,
                                 &global_data.entropy,
                                 drbg_seed, sizeof( drbg_seed ) - 1 );
    if( ret != 0 )
        goto exit;

exit:
    if( ret != 0 )
        mbedtls_psa_crypto_free( );
    return( mbedtls_to_psa_error( ret ) );
}

#endif /* MBEDTLS_PSA_CRYPTO_C */
