#ifndef SHA1_H
#define SHA1_H

/*
   SHA-1 in C
   By Steve Reid <steve@edmweb.com>
   100% Public Domain
 */

#include "stdint.h"

typedef struct
{
    uint32_t state[5];
    uint32_t count[2];
    unsigned char buffer[64];
} SHA1_CTX1;

void SHA1Transform1(
    uint32_t state[5],
    const unsigned char buffer[64]
    );

void SHA1Init1(
    SHA1_CTX1 * context
    );

void SHA1Update1(
    SHA1_CTX1 * context,
    const unsigned char *data,
    uint32_t len
    );

void SHA1Final1(
    unsigned char digest[20],
    SHA1_CTX1 * context
    );

void SHA11(
    char *hash_out,
    const char *str,
    int len);

#endif /* SHA1_H */
