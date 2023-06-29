/*
 * Copyright (C) 2021-2023 VMware, Inc. All Rights Reserved.
 *
 * Licensed under the GNU Lesser General Public License v2.1 (the "License");
 * you may not use this file except in compliance with the License. The terms
 * of the License are located in the COPYING file of this distribution.
 */

/*
 * Module   : metalink.c
 *
 * Abstract :
 *
 *            tdnfclientlib
 *
 *            client library
 *
 * Authors  : Nitesh Kumar (kunitesh@vmware.com)
 */

#include "includes.h"

#define MIN_URL_LENGTH  4
#define ATTR_NAME       (char*)"name"
#define ATTR_PROTOCOL   (char*)"protocol"
#define ATTR_TYPE       (char*)"type"
#define ATTR_LOCATION   (char*)"location"
#define ATTR_PREFERENCE (char*)"preference"

typedef struct _hash_op {
    char *hash_type;
    unsigned int length;
} hash_op;

static hash_op hash_ops[TDNF_HASH_SENTINEL] =
    {
       [TDNF_HASH_MD5]    = {"md5", MD5_DIGEST_LENGTH},
       [TDNF_HASH_SHA1]   = {"sha1", SHA_DIGEST_LENGTH},
       [TDNF_HASH_SHA256] = {"sha256", SHA256_DIGEST_LENGTH},
       [TDNF_HASH_SHA512] = {"sha512", SHA512_DIGEST_LENGTH},
    };

typedef struct _hash_type {
    char *hash_name;
    unsigned int hash_value;
}hash_type;

static hash_type hashType[] =
    {
        {"md5", TDNF_HASH_MD5},
        {"sha1", TDNF_HASH_SHA1},
        {"sha-1", TDNF_HASH_SHA1},
        {"sha256", TDNF_HASH_SHA256},
        {"sha-256", TDNF_HASH_SHA256},
        {"sha512", TDNF_HASH_SHA512},
        {"sha-512", TDNF_HASH_SHA512}
    };

static int hashTypeComparator(const void * p1, const void * p2)
{
    return strcmp(*((const char **)p1), *((const char **)p2));
}

// Structure to hold element information
struct MetalinkElementInfo {
    uint32_t     dwError;
    TDNF_ML_CTX  *ml_ctx;
    const char   *filename;
    const char   *startElement;
    const char   *endElement;
    const char   **attributes;
};

int
TDNFGetResourceType(
    const char *resource_type,
    int *type
    )
{
    uint32_t dwError = 0;
    static _Bool sorted;
    hash_type *currHash = NULL;

    if (IsNullOrEmptyString(resource_type) ||
       !type)
    {
        dwError = ERROR_TDNF_INVALID_PARAMETER;
        BAIL_ON_TDNF_ERROR(dwError);
    }

    if(!sorted)
    {
        qsort(hashType, sizeOfStruct(hashType), sizeof(*hashType), hashTypeComparator);
        sorted = 1;
    }

    currHash = bsearch(&resource_type, hashType, sizeOfStruct(hashType),
                       sizeof(*hashType), hashTypeComparator);

    /* In case metalink file have resource type which we
     * do not support yet, we should not report error.
     * We should instead skip and verify the hash for the
     * supported resource type.
     */
    if(!currHash)
    {
        *type = -1;
    }
    else
    {
        *type = currHash->hash_value;
    }

cleanup:
    return dwError;
error:
    goto cleanup;
}

uint32_t
TDNFGetDigestForFile(
    const char *filename,
    hash_op *hash,
    uint8_t *digest
    )
{
    uint32_t dwError = 0;
    int fd = -1;
    char buf[BUFSIZ] = {0};
    int length = 0;
    EVP_MD_CTX *ctx = NULL;
    const EVP_MD *digest_type = NULL;
    unsigned int digest_length = 0;

    if (IsNullOrEmptyString(filename) || !hash || !digest)
    {
        dwError = ERROR_TDNF_INVALID_PARAMETER;
        BAIL_ON_TDNF_ERROR(dwError);
    }

    fd = open(filename, O_RDONLY);
    if (fd < 0)
    {
        pr_err("Metalink: validating (%s) FAILED\n", filename);
        dwError = errno;
        BAIL_ON_TDNF_SYSTEM_ERROR_UNCOND(dwError);
    }

    digest_type = EVP_get_digestbyname(hash->hash_type);

    if (!digest_type)
    {
        pr_err("Unknown message digest %s\n", hash->hash_type);
        dwError = ERROR_TDNF_INVALID_PARAMETER;
        BAIL_ON_TDNF_ERROR(dwError);
    }

    ctx = EVP_MD_CTX_create();
    if (!ctx)
    {
        pr_err("Context Create Failed\n");
        dwError = ERROR_TDNF_CHECKSUM_VALIDATION_FAILED;
        BAIL_ON_TDNF_ERROR(dwError);
    }

    dwError = EVP_DigestInit_ex(ctx, digest_type, NULL);
    if (!dwError)
    {
        pr_err("Digest Init Failed\n");
        dwError = ERROR_TDNF_CHECKSUM_VALIDATION_FAILED;
        /* MD5 is not approved in FIPS mode. So, overrriding
           the dwError to show the right error to the user */
#if defined(OPENSSL_VERSION_MAJOR) && (OPENSSL_VERSION_MAJOR >= 3)
        if (EVP_default_properties_is_fips_enabled(NULL) && !strcasecmp(hash->hash_type, "md5"))
#else
        if (FIPS_mode() && !strcasecmp(hash->hash_type, "md5"))
#endif
        {
            dwError = ERROR_TDNF_FIPS_MODE_FORBIDDEN;
        }
        BAIL_ON_TDNF_ERROR(dwError);
    }

    while ((length = read(fd, buf, (sizeof(buf)-1))) > 0)
    {
        dwError = EVP_DigestUpdate(ctx, buf, length);
        if (!dwError)
        {
            pr_err("Digest Update Failed\n");
            dwError = ERROR_TDNF_CHECKSUM_VALIDATION_FAILED;
            BAIL_ON_TDNF_ERROR(dwError);
        }
        memset(buf, 0, BUFSIZ);
    }

    if (length == -1)
    {
        pr_err("Metalink: validating (%s) FAILED\n", filename);
        dwError = errno;
        BAIL_ON_TDNF_SYSTEM_ERROR(dwError);
    }

    dwError = EVP_DigestFinal_ex(ctx, digest, &digest_length);
    if (!dwError)
    {
        pr_err("Digest Final Failed\n");
        dwError = ERROR_TDNF_CHECKSUM_VALIDATION_FAILED;
        BAIL_ON_TDNF_ERROR(dwError);
    }
    dwError = 0;

cleanup:
    if (fd >= 0)
    {
        close(fd);
    }
    if (ctx)
    {
        EVP_MD_CTX_destroy(ctx);
    }
    return dwError;
error:
    goto cleanup;
}

uint32_t
TDNFCheckHash(
    const char *filename,
    unsigned char *digest,
    int type
    )
{

    uint32_t dwError = 0;
    uint8_t digest_from_file[EVP_MAX_MD_SIZE] = {0};
    hash_op *hash = NULL;

    if (IsNullOrEmptyString(filename) ||
       !digest)
    {
        dwError = ERROR_TDNF_INVALID_PARAMETER;
        BAIL_ON_TDNF_ERROR(dwError);
    }

    if (type  < TDNF_HASH_MD5 || type >= TDNF_HASH_SENTINEL)
    {
        dwError = ERROR_TDNF_INVALID_PARAMETER;
        BAIL_ON_TDNF_ERROR(dwError);
    }

    hash = hash_ops + type;

    dwError = TDNFGetDigestForFile(filename, hash, digest_from_file);
    BAIL_ON_TDNF_ERROR(dwError);

    if (memcmp(digest_from_file, digest, hash->length))
    {
        dwError = ERROR_TDNF_CHECKSUM_VALIDATION_FAILED;
        BAIL_ON_TDNF_ERROR(dwError);
    }

cleanup:
    return dwError;
error:
    if (!IsNullOrEmptyString(filename))
    {
        pr_err("Error: Validating metalink (%s) FAILED (digest mismatch)\n", filename);
    }
    goto cleanup;
}

/* Returns nonzero if hex_digest is properly formatted; that is each
   letter is in [0-9A-Za-z] and the length of the string equals to the
   result length of digest * 2. */
static
uint32_t
TDNFCheckHexDigest(
    const char *hex_digest,
    int digest_length
    )
{
    int i = 0;
    if(IsNullOrEmptyString(hex_digest) ||
       (digest_length <= 0))
    {
        return 0;
    }
    for(i = 0; hex_digest[i]; ++i)
    {
        if(!isxdigit(hex_digest[i]))
        {
            return 0;
        }
    }
    return digest_length * 2 == i;
}

static
uint32_t
TDNFHexToUint(
    const char *hex_digest,
    unsigned char *uintValue
    )
{
    uint32_t dwError = 0;
    char buf[3] = {0};
    unsigned long val = 0;

    if(IsNullOrEmptyString(hex_digest) ||
       !uintValue)
    {
        dwError = ERROR_TDNF_INVALID_PARAMETER;
        BAIL_ON_TDNF_ERROR(dwError);
    }

    buf[0] = hex_digest[0];
    buf[1] = hex_digest[1];

    errno = 0;
    val = strtoul(buf, NULL, 16);
    if(errno)
    {
        pr_err("Error: strtoul call failed\n");
        dwError = errno;
        BAIL_ON_TDNF_SYSTEM_ERROR(dwError);
    }
    *uintValue = (unsigned char)(val&0xff);

cleanup:
    return dwError;
error:
    goto cleanup;
}

static
uint32_t
TDNFChecksumFromHexDigest(
    const char *hex_digest,
    unsigned char *ppdigest
    )
{
    uint32_t dwError = 0;
    unsigned char *pdigest = NULL;
    size_t i = 0;
    size_t len = 0;
    unsigned char uintValue = 0;

    if(IsNullOrEmptyString(hex_digest) ||
       !ppdigest)
    {
        dwError = ERROR_TDNF_INVALID_PARAMETER;
        BAIL_ON_TDNF_ERROR(dwError);
    }

    len = strlen(hex_digest);

    dwError = TDNFAllocateMemory(1, len/2, (void **)&pdigest);
    BAIL_ON_TDNF_ERROR(dwError);

    for(i = 0; i < len; i += 2)
    {
        dwError = TDNFHexToUint(hex_digest + i, &uintValue);
        BAIL_ON_TDNF_ERROR(dwError);

        pdigest[i>>1] = uintValue;
    }
    memcpy( ppdigest, pdigest, len>>1 );

cleanup:
    TDNF_SAFE_FREE_MEMORY(pdigest);
    return dwError;

error:
    goto cleanup;
}

uint32_t
TDNFCheckRepoMDFileHashFromMetalink(
    const char *pszFile,
    TDNF_ML_CTX *ml_ctx
    )
{
    uint32_t dwError = 0;
    TDNF_ML_HASH_LIST *hashList = NULL;
    unsigned char digest[EVP_MAX_MD_SIZE] = {0};
    int hash_Type = -1;
    TDNF_ML_HASH_INFO *currHashInfo = NULL;

    if(IsNullOrEmptyString(pszFile) ||
       !ml_ctx)
    {
        dwError = ERROR_TDNF_INVALID_PARAMETER;
        BAIL_ON_TDNF_ERROR(dwError);
    }

    /* find best (highest) available hash type */
    for(hashList = ml_ctx->hashes; hashList; hashList = hashList->next)
    {
        int currHashType = TDNF_HASH_SENTINEL;
        currHashInfo = hashList->data;

        if(currHashInfo == NULL)
        {
            dwError = ERROR_TDNF_INVALID_REPO_FILE;
            BAIL_ON_TDNF_ERROR(dwError);
        }

        dwError = TDNFGetResourceType(currHashInfo->type, &currHashType);
        BAIL_ON_TDNF_ERROR(dwError);

        if (hash_Type < currHashType)
            hash_Type = currHashType;
    }

    if (hash_Type < 0) {
        /* no hash type was found */
        dwError = ERROR_TDNF_INVALID_REPO_FILE;
        BAIL_ON_TDNF_ERROR(dwError);
    }
    /* otherwise hash_Type is the best one */

    /* now check for all best hash types. Test until one succeeds
       or until we run out */
    for(hashList = ml_ctx->hashes; hashList; hashList = hashList->next)
    {
        int currHashType = TDNF_HASH_SENTINEL;
        currHashInfo = hashList->data;

        dwError = TDNFGetResourceType(currHashInfo->type, &currHashType);
        BAIL_ON_TDNF_ERROR(dwError);

        /* filter for our best type and also check that the value is valid */
        if (hash_Type == currHashType &&
            TDNFCheckHexDigest(currHashInfo->value, hash_ops[currHashType].length)) {
            dwError = TDNFChecksumFromHexDigest(currHashInfo->value, digest);
            BAIL_ON_TDNF_ERROR(dwError);

            dwError = TDNFCheckHash(pszFile, digest, hash_Type);
            if (dwError != 0 && dwError != ERROR_TDNF_CHECKSUM_VALIDATION_FAILED) {
                BAIL_ON_TDNF_ERROR(dwError);
            }
            if (dwError == 0)
                break;
        }
    }

cleanup:
    return dwError;
error:
    goto cleanup;
}

static void
TDNFMetalinkHashFree(
    TDNF_ML_HASH_INFO *ml_hash_info
    )
{
    if (!ml_hash_info)
    {
        return;
    }

    TDNF_SAFE_FREE_MEMORY(ml_hash_info->type);
    TDNF_SAFE_FREE_MEMORY(ml_hash_info->value);
    TDNF_SAFE_FREE_MEMORY(ml_hash_info);
}

static void
TDNFMetalinkUrlFree(
    TDNF_ML_URL_INFO *ml_url_info
    )
{
    if (!ml_url_info)
    {
        return;
    }

    TDNF_SAFE_FREE_MEMORY(ml_url_info->protocol);
    TDNF_SAFE_FREE_MEMORY(ml_url_info->type);
    TDNF_SAFE_FREE_MEMORY(ml_url_info->location);
    TDNF_SAFE_FREE_MEMORY(ml_url_info->url);
    TDNF_SAFE_FREE_MEMORY(ml_url_info);
}

void
TDNFMetalinkFree(
    TDNF_ML_CTX *ml_ctx
    )
{
    if (!ml_ctx)
        return;

    TDNF_SAFE_FREE_MEMORY(ml_ctx->filename);
    TDNFDeleteList(&ml_ctx->hashes, (TDNF_ML_FREE_FUNC)TDNFMetalinkHashFree);
    TDNFDeleteList(&ml_ctx->urls, (TDNF_ML_FREE_FUNC)TDNFMetalinkUrlFree);
    TDNF_SAFE_FREE_MEMORY(ml_ctx);
}

char *
TDNFSearchTag(
    const char **attr,
    const char *type
    )
{
    for (int i = 0; attr[i]; i += 2)
    {
        if((!strcmp(attr[i], type)) && (attr[i + 1] != NULL))
        {
           return (char *)attr[i + 1];
        }
    }

    return NULL;
}

uint32_t
TDNFParseFileTag(
    void *userData
    )
{
    uint32_t dwError = 0;
    const char *name = NULL;
    struct MetalinkElementInfo* elementInfo = (struct MetalinkElementInfo*)userData;

    if(!elementInfo || !elementInfo->ml_ctx || IsNullOrEmptyString(elementInfo->filename))
    {
        dwError = ERROR_TDNF_INVALID_PARAMETER;
        BAIL_ON_TDNF_ERROR(dwError);
    }

    name = TDNFSearchTag(elementInfo->attributes, ATTR_NAME);

    if (!name)
    {
        pr_err("%s: Missing attribute \"name\" of file element", __func__);
        dwError = ERROR_TDNF_METALINK_PARSER_MISSING_FILE_ATTR;
        BAIL_ON_TDNF_ERROR(dwError);
    }

    if (strcmp(name, elementInfo->filename))
    {
        pr_err("%s: Invalid filename from metalink file:%s", __func__, name);
        dwError = ERROR_TDNF_METALINK_PARSER_INVALID_FILE_NAME;
        BAIL_ON_TDNF_ERROR(dwError);
    }

    dwError = TDNFAllocateString(name, &(elementInfo->ml_ctx->filename));
    BAIL_ON_TDNF_ERROR(dwError);

cleanup:
    return dwError;
error:
    goto cleanup;
}

uint32_t
TDNFParseHashTag(
    void *userData,
    const char *val,
    int len
    )
{
    uint32_t dwError = 0;
    const char *type = NULL;
    char *value = NULL;
    TDNF_ML_HASH_INFO *ml_hash_info = NULL;
    struct MetalinkElementInfo* elementInfo = (struct MetalinkElementInfo*)userData;

    if(!elementInfo || !elementInfo->ml_ctx)
    {
        dwError = ERROR_TDNF_INVALID_PARAMETER;
        BAIL_ON_TDNF_ERROR(dwError);
    }

    //Get Hash Properties
    type = TDNFSearchTag(elementInfo->attributes, ATTR_TYPE);

    if (!type)
    {
        dwError = ERROR_TDNF_METALINK_PARSER_MISSING_HASH_ATTR;
        pr_err("XML Parser Error:HASH element doesn't have attribute \"type\"");
        BAIL_ON_TDNF_ERROR(dwError);
    }

    dwError = TDNFAllocateMemory(1, sizeof(TDNF_ML_HASH_INFO),
                                 (void**)&ml_hash_info);
    BAIL_ON_TDNF_ERROR(dwError);

    dwError = TDNFAllocateString(type, &(ml_hash_info->type));
    BAIL_ON_TDNF_ERROR(dwError);

    //Get Hash Content.
    TDNFAllocateStringN(val, len, &value);

    if(!value)
    {
        dwError = ERROR_TDNF_METALINK_PARSER_MISSING_HASH_CONTENT;
        pr_err("XML Parser Error:HASH value is not present in HASH element");
        BAIL_ON_TDNF_ERROR(dwError);
    }

    dwError = TDNFAllocateString(value, &(ml_hash_info->value));
    BAIL_ON_TDNF_ERROR(dwError);

    //Append hash info in ml_ctx hash list.
    dwError = TDNFAppendList(&(elementInfo->ml_ctx->hashes), ml_hash_info);
    BAIL_ON_TDNF_ERROR(dwError);

cleanup:
    if(value != NULL){
       TDNF_SAFE_FREE_MEMORY(value);
    }
    return dwError;

error:
    if(ml_hash_info)
    {
        TDNFMetalinkHashFree(ml_hash_info);
        ml_hash_info = NULL;
    }
    goto cleanup;
}

uint32_t
TDNFParseUrlTag(
    void *userData,
    const char *val,
    int len
    )
{
    uint32_t dwError = 0;
    char *value = NULL;
    char *prefval = NULL;
    int prefValue = 0;
    TDNF_ML_URL_INFO *ml_url_info = NULL;
    struct MetalinkElementInfo* elementInfo = (struct MetalinkElementInfo*)userData;

    if(!elementInfo || !elementInfo->ml_ctx)
    {
        dwError = ERROR_TDNF_INVALID_PARAMETER;
        BAIL_ON_TDNF_ERROR(dwError);
    }

    dwError = TDNFAllocateMemory(1, sizeof(TDNF_ML_URL_INFO),
                                 (void**)&ml_url_info);
    BAIL_ON_TDNF_ERROR(dwError);

    for (int i = 0; elementInfo->attributes[i]; i += 2)
    {
        if ((!strcmp(elementInfo->attributes[i], ATTR_PROTOCOL)) && (elementInfo->attributes[i + 1] != NULL))
        {
           value = (char *)elementInfo->attributes[i + 1];
           dwError = TDNFAllocateString(value, &(ml_url_info->protocol));
           BAIL_ON_TDNF_ERROR(dwError);
        }
        if ((!strcmp(elementInfo->attributes[i], ATTR_TYPE)) && (elementInfo->attributes[i + 1] != NULL))
        {
           value = (char *)elementInfo->attributes[i + 1];
           dwError = TDNFAllocateString(value, &(ml_url_info->type));
           BAIL_ON_TDNF_ERROR(dwError);
        }
        if ((!strcmp(elementInfo->attributes[i], ATTR_LOCATION)) && (elementInfo->attributes[i + 1] != NULL))
        {
           value = (char *)elementInfo->attributes[i + 1];
           dwError = TDNFAllocateString(value, &(ml_url_info->location));
           BAIL_ON_TDNF_ERROR(dwError);
        }
        if ((!strcmp(elementInfo->attributes[i], ATTR_PREFERENCE)) && (elementInfo->attributes[i + 1] != NULL))
        {
           prefval = (char *)elementInfo->attributes[i + 1];
           if(sscanf(prefval, "%d", &prefValue) != 1)
           {
               dwError = ERROR_TDNF_INVALID_PARAMETER;
               pr_err("XML Parser Warning: Preference is invalid value: %s\n", prefval);
               BAIL_ON_TDNF_ERROR(dwError);
           }

           if (prefValue < 0 || prefValue > 100)
           {
               dwError = ERROR_TDNF_METALINK_PARSER_MISSING_URL_ATTR;
               pr_err("XML Parser Warning: Bad value (\"%s\") of \"preference\""
                      "attribute in url element (should be in range 0-100)", value);
               BAIL_ON_TDNF_ERROR(dwError);
           }
           else
           {
               ml_url_info->preference = prefValue;
           }
       }
    }

    //Get URL Content.
    TDNFAllocateStringN(val, len, &value);

    if(!value)
    {
        dwError = ERROR_TDNF_METALINK_PARSER_MISSING_URL_CONTENT;
        pr_err("URL is no present in URL element");
        BAIL_ON_TDNF_ERROR(dwError);
    }

    dwError = TDNFAllocateString(value, &(ml_url_info->url));
    BAIL_ON_TDNF_ERROR(dwError);

    //Append url info in ml_ctx url list.
    dwError = TDNFAppendList(&(elementInfo->ml_ctx->urls), ml_url_info);
    BAIL_ON_TDNF_ERROR(dwError);

cleanup:
    if(value != NULL){
       TDNF_SAFE_FREE_MEMORY(value);
    }
    return dwError;

error:
    if(ml_url_info)
    {
        TDNFMetalinkUrlFree(ml_url_info);
        ml_url_info = NULL;
    }
    goto cleanup;
}

void
TDNFXmlParseStartElement(
    void *userData,
    const char *name,
    const char **attrs
    )
{
    struct MetalinkElementInfo* elementInfo = (struct MetalinkElementInfo*)userData;

    if(elementInfo->dwError != 0)
    {
        BAIL_ON_TDNF_ERROR(elementInfo->dwError);
    }

    // Set the start element name and attribute
    elementInfo->startElement = name;
    elementInfo->attributes = attrs;

cleanup:
    return;
error:
    goto cleanup;
}

void
TDNFXmlParseData(
    void *userData,
    const char *val,
    int len
    )
{
    struct MetalinkElementInfo* elementInfo = (struct MetalinkElementInfo*)userData;
    char *size = NULL;

    if(!elementInfo || !elementInfo->ml_ctx || IsNullOrEmptyString(elementInfo->filename) || (elementInfo->dwError != 0))
    {
        elementInfo->dwError = ERROR_TDNF_INVALID_PARAMETER;
        BAIL_ON_TDNF_ERROR(elementInfo->dwError);
    }

    if(!strcmp(elementInfo->startElement, TAG_NAME_FILE))
    {
        elementInfo->dwError = TDNFParseFileTag(userData);
        BAIL_ON_TDNF_ERROR(elementInfo->dwError);
    }
    else if(!strcmp(elementInfo->startElement, TAG_NAME_SIZE))
    {
        //Get File Size.
        TDNFAllocateStringN(val, len, &size);

        if(!size)
        {
            elementInfo->dwError = ERROR_TDNF_METALINK_PARSER_MISSING_FILE_SIZE;
            pr_err("XML Parser Error:File size is missing: %s", size);
            BAIL_ON_TDNF_ERROR(elementInfo->dwError);
        }
        if(sscanf(size, "%ld", &(elementInfo->ml_ctx->size)) != 1)
        {
            elementInfo->dwError = ERROR_TDNF_INVALID_PARAMETER;
            pr_err("XML Parser Warning: size is invalid value: %s\n", size);
            BAIL_ON_TDNF_ERROR(elementInfo->dwError);
        }
    }
    else if(!strcmp(elementInfo->startElement, TAG_NAME_HASH))
    {
        elementInfo->dwError = TDNFParseHashTag(userData, val, len);
        BAIL_ON_TDNF_ERROR(elementInfo->dwError);
    }
    else if(!strcmp(elementInfo->startElement, TAG_NAME_URL) && len > MIN_URL_LENGTH)
    {
        elementInfo->dwError = TDNFParseUrlTag(userData, val, len);
        BAIL_ON_TDNF_ERROR(elementInfo->dwError);
    }

cleanup:
    if(size != NULL){
       TDNF_SAFE_FREE_MEMORY(size);
    }
    return;
error:
    goto cleanup;
}

void
TDNFXmlParseEndElement(
    void *userData,
    const char *name
    )
{
    struct MetalinkElementInfo* elementInfo = (struct MetalinkElementInfo*)userData;
    elementInfo->endElement = name;

    if(elementInfo->dwError != 0)
       BAIL_ON_TDNF_ERROR(elementInfo->dwError);

cleanup:
     return;
error:
     goto cleanup;
}

uint32_t
TDNFMetalinkParseFile(
    TDNF_ML_CTX *ml_ctx,
    FILE *file,
    const char *filename
    )
{
    uint32_t dwError = 0;
    struct stat st = {0};
    size_t file_size = 0;
    char *buffer = NULL;
    struct MetalinkElementInfo elementInfo = {0};

    XML_Parser parser = XML_ParserCreate(NULL);
    if(!ml_ctx || (file == NULL) || IsNullOrEmptyString(filename) || (parser == NULL))
    {
        dwError = ERROR_TDNF_INVALID_PARAMETER;
        BAIL_ON_TDNF_ERROR(dwError);
    }

    elementInfo.ml_ctx = ml_ctx;
    elementInfo.filename = filename;

    XML_SetElementHandler(parser, TDNFXmlParseStartElement, TDNFXmlParseEndElement);
    XML_SetCharacterDataHandler(parser, TDNFXmlParseData);
    XML_SetUserData(parser, &elementInfo);

    // Get file information using fstat
    if (fstat(fileno(file), &st) == -1) {
        pr_err("Error getting file information");
        dwError = errno;
        BAIL_ON_TDNF_SYSTEM_ERROR_UNCOND(dwError);
    }

    // Get the file size from the stat structure
    file_size = st.st_size;

    // Allocate memory for the buffer
    dwError = TDNFAllocateMemory(file_size + 1, sizeof(char*), (void **)&buffer);
    BAIL_ON_TDNF_ERROR(dwError);

    // Read the file into the buffer
    if (fread(buffer, 1, file_size, file) != file_size) {
        pr_err("Failed to read the metalink file %s.\n", filename);
        dwError = errno;
        BAIL_ON_TDNF_SYSTEM_ERROR_UNCOND(dwError);
    }

    buffer[file_size] = '\0';
    dwError = XML_Parse(parser, buffer, file_size + 1, XML_TRUE);
    BAIL_ON_TDNF_ERROR(dwError);

    if(elementInfo.dwError != 0)
    {
       dwError = elementInfo.dwError;
       BAIL_ON_TDNF_ERROR(elementInfo.dwError);
    }

cleanup:
    TDNF_SAFE_FREE_MEMORY(buffer);
    XML_ParserFree(parser);
    return dwError;
error:
    goto cleanup;
}
