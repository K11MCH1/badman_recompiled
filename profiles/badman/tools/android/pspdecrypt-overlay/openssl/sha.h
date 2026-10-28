/* Minimal OpenSSL-compatible SHA-256 subset used to build the external,
 * separately licensed pspdecrypt (GPLv3) where OpenSSL is unavailable, such
 * as the Android NDK. Independent implementation of FIPS 180-4; exposes the
 * public h[8] state words that pspdecrypt's IPL code adjusts. MIT licensed
 * like PSPRecomp; the combined decryptor binary remains under GPLv3. */
#ifndef PSPRECOMP_OPENSSL_SHA_SHIM_H
#define PSPRECOMP_OPENSSL_SHA_SHIM_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define SHA256_DIGEST_LENGTH 32
typedef struct {
    uint32_t h[8];
    uint64_t length;
    unsigned char block[64];
    unsigned int used;
} SHA256_CTX;

static inline uint32_t psprecomp_sha_rotr(uint32_t x, unsigned n) { return (x >> n) | (x << (32u - n)); }

static inline void psprecomp_sha256_block(SHA256_CTX *c, const unsigned char *p) {
    static const uint32_t k[64] = {
        0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
        0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
        0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
        0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
        0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
        0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
        0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
        0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u};
    uint32_t w[64], a, b, d, e, f, g, h, cc, t1, t2;
    unsigned i;
    for (i = 0; i < 16; ++i)
        w[i] = (uint32_t)p[i*4] << 24 | (uint32_t)p[i*4+1] << 16 | (uint32_t)p[i*4+2] << 8 | p[i*4+3];
    for (i = 16; i < 64; ++i) {
        const uint32_t s0 = psprecomp_sha_rotr(w[i-15], 7) ^ psprecomp_sha_rotr(w[i-15], 18) ^ (w[i-15] >> 3);
        const uint32_t s1 = psprecomp_sha_rotr(w[i-2], 17) ^ psprecomp_sha_rotr(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3]; e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
    for (i = 0; i < 64; ++i) {
        t1 = h + (psprecomp_sha_rotr(e, 6) ^ psprecomp_sha_rotr(e, 11) ^ psprecomp_sha_rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
        t2 = (psprecomp_sha_rotr(a, 2) ^ psprecomp_sha_rotr(a, 13) ^ psprecomp_sha_rotr(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}
static inline int SHA256_Init(SHA256_CTX *c) {
    static const uint32_t initial[8] = {0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u};
    memcpy(c->h, initial, sizeof initial); c->length = 0; c->used = 0; return 1;
}
static inline int SHA256_Update(SHA256_CTX *c, const void *data, size_t size) {
    const unsigned char *p = (const unsigned char *)data;
    c->length += (uint64_t)size;
    while (size) {
        const size_t take = size < 64u - c->used ? size : 64u - c->used;
        memcpy(c->block + c->used, p, take); c->used += (unsigned)take; p += take; size -= take;
        if (c->used == 64u) { psprecomp_sha256_block(c, c->block); c->used = 0; }
    }
    return 1;
}
static inline int SHA256_Final(unsigned char *md, SHA256_CTX *c) {
    const uint64_t bits = c->length * 8u;
    unsigned i;
    c->block[c->used++] = 0x80u;
    if (c->used > 56u) { memset(c->block + c->used, 0, 64u - c->used); psprecomp_sha256_block(c, c->block); c->used = 0; }
    memset(c->block + c->used, 0, 56u - c->used);
    for (i = 0; i < 8; ++i) c->block[56 + i] = (unsigned char)(bits >> (56u - i * 8u));
    psprecomp_sha256_block(c, c->block);
    for (i = 0; i < 8; ++i) {
        md[i*4] = (unsigned char)(c->h[i] >> 24); md[i*4+1] = (unsigned char)(c->h[i] >> 16);
        md[i*4+2] = (unsigned char)(c->h[i] >> 8); md[i*4+3] = (unsigned char)c->h[i];
    }
    return 1;
}
#endif
