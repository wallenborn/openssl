/* ====================================================================
 * Copyright (c) 2011-2013 The OpenSSL Project.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. All advertising materials mentioning features or use of this
 *    software must display the following acknowledgment:
 *    "This product includes software developed by the OpenSSL Project
 *    for use in the OpenSSL Toolkit. (http://www.OpenSSL.org/)"
 *
 * 4. The names "OpenSSL Toolkit" and "OpenSSL Project" must not be used to
 *    endorse or promote products derived from this software without
 *    prior written permission. For written permission, please contact
 *    licensing@OpenSSL.org.
 *
 * 5. Products derived from this software may not be called "OpenSSL"
 *    nor may "OpenSSL" appear in their names without prior written
 *    permission of the OpenSSL Project.
 *
 * 6. Redistributions of any form whatsoever must retain the following
 *    acknowledgment:
 *    "This product includes software developed by the OpenSSL Project
 *    for use in the OpenSSL Toolkit (http://www.OpenSSL.org/)"
 *
 * THIS SOFTWARE IS PROVIDED BY THE OpenSSL PROJECT ``AS IS'' AND ANY
 * EXPRESSED OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE OpenSSL PROJECT OR
 * ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 * ====================================================================
 */

#include <openssl/opensslconf.h>

#include <stdio.h>
#include <string.h>

#if !defined(OPENSSL_NO_AES) && !defined(OPENSSL_NO_SHA1)

#include <openssl/evp.h>
#include <openssl/objects.h>
#include <openssl/aes.h>
#include <openssl/sha.h>
#include <openssl/rand.h>
#include "modes_lcl.h"

#ifndef EVP_CIPH_FLAG_AEAD_CIPHER
#define EVP_CIPH_FLAG_AEAD_CIPHER	0x200000
#define EVP_CTRL_AEAD_TLS1_AAD		0x16
#define EVP_CTRL_AEAD_SET_MAC_KEY	0x17
#endif

#if !defined(EVP_CIPH_FLAG_DEFAULT_ASN1)
#define EVP_CIPH_FLAG_DEFAULT_ASN1 0
#endif

#if !defined(EVP_CIPH_FLAG_TLS11_MULTI_BLOCK)
#define EVP_CIPH_FLAG_TLS11_MULTI_BLOCK 0
#endif

#define TLS1_1_VERSION 0x0302

typedef struct
    {
    AES_KEY		ks;
    SHA_CTX		head,tail,md;
    size_t		payload_length;	/* AAD length in decrypt case */
    union {
	unsigned int	tls_ver;
    	unsigned char	tls_aad[16];	/* 13 used */
    } aux;
    } EVP_AES_HMAC_SHA1;

#define NO_PAYLOAD_LENGTH	((size_t)-1)

#if	defined(AES_ASM) &&	( \
	defined(__x86_64)	|| defined(__x86_64__)	|| \
	defined(_M_AMD64)	|| defined(_M_X64)	|| \
	defined(__INTEL__)	)

extern unsigned int OPENSSL_ia32cap_P[2];
#define AESNI_CAPABLE   (1<<(57-32))

int aesni_set_encrypt_key(const unsigned char *userKey, int bits,
			      AES_KEY *key);
int aesni_set_decrypt_key(const unsigned char *userKey, int bits,
			      AES_KEY *key);

void aesni_cbc_encrypt(const unsigned char *in,
			   unsigned char *out,
			   size_t length,
			   const AES_KEY *key,
			   unsigned char *ivec, int enc);

void aesni_cbc_sha1_enc (const void *inp, void *out, size_t blocks,
		const AES_KEY *key, unsigned char iv[16],
		SHA_CTX *ctx,const void *in0);

#define data(ctx) ((EVP_AES_HMAC_SHA1 *)(ctx)->cipher_data)

static int aesni_cbc_hmac_sha1_init_key(EVP_CIPHER_CTX *ctx,
			const unsigned char *inkey,
			const unsigned char *iv, int enc)
	{
	EVP_AES_HMAC_SHA1 *key = data(ctx);
	int ret;

	if (enc)
		ret=aesni_set_encrypt_key(inkey,ctx->key_len*8,&key->ks);
	else
		ret=aesni_set_decrypt_key(inkey,ctx->key_len*8,&key->ks);

	SHA1_Init(&key->head);	/* handy when benchmarking */
	key->tail = key->head;
	key->md   = key->head;

	key->payload_length = NO_PAYLOAD_LENGTH;

	return ret<0?0:1;
	}

#define	STITCHED_CALL

#if !defined(STITCHED_CALL)
#define	aes_off 0
#endif

void sha1_block_data_order (void *c,const void *p,size_t len);

static void sha1_update(SHA_CTX *c,const void *data,size_t len)
{	const unsigned char *ptr = data;
	size_t res;

	if ((res = c->num)) {
		res = SHA_CBLOCK-res;
		if (len<res) res=len;
		SHA1_Update (c,ptr,res);
		ptr += res;
		len -= res;
	}

	res = len % SHA_CBLOCK;
	len -= res;

	if (len) {
		sha1_block_data_order(c,ptr,len/SHA_CBLOCK);

		ptr += len;
		c->Nh += len>>29;
		c->Nl += len<<=3;
		if (c->Nl<(unsigned int)len) c->Nh++;
	}

	if (res)
		SHA1_Update(c,ptr,res);
}

#ifdef SHA1_Update
#undef SHA1_Update
#endif
#define SHA1_Update sha1_update

#if EVP_CIPH_FLAG_TLS11_MULTI_BLOCK

typedef struct { unsigned int A[8],B[8],C[8],D[8],E[8]; } SHA1_MB_CTX;
typedef struct { const unsigned char *ptr; int blocks;  } HASH_DESC;

void sha1_multi_block(SHA1_MB_CTX *,const HASH_DESC *,int);

typedef struct { const unsigned char *inp; unsigned char *out;
		 int blocks; double iv[2]; } CIPH_DESC; 

void aesni_multi_cbc_encrypt(CIPH_DESC *,void *,int);

static size_t tls11_multi_block_encrypt(EVP_AES_HMAC_SHA1 *key,
	unsigned char *out, const unsigned char *inp, size_t inp_len,
	int n4x)	/* n4x is 1 or 2 */
{
	HASH_DESC	hash_d[8], edges[8];
	CIPH_DESC	ciph_d[8];
	unsigned char	storage[sizeof(SHA1_MB_CTX)+32];
	union {	u64	q[16];
		u32	d[32];
		u8	c[128];	} blocks[8];
	SHA1_MB_CTX	*ctx;
	unsigned int	frag, last, packlen, i, x4=4*n4x;
	size_t		ret = 0;

	ctx = (SHA1_MB_CTX *)(storage+32-((size_t)storage%32));	/* align */

	frag = (unsigned int)inp_len>>(1+n4x);
	last = (unsigned int)inp_len+frag-(frag<<(1+n4x));
	if (last>frag && ((last+13+9)%64)<(x4-1)) {
		frag++;
		last -= x4-1;
	}

	hash_d[0].ptr = inp;
	for (i=1;i<x4;i++)	hash_d[i].ptr = hash_d[i-1].ptr+frag;

	for (i=0;i<x4;i++) {
		unsigned int len = (i==(x4-1)?last:frag);

		ctx->A[i] = key->md.h0;
		ctx->B[i] = key->md.h1;
		ctx->C[i] = key->md.h2;
		ctx->D[i] = key->md.h3;
		ctx->E[i] = key->md.h4;

		/* fix seqnum */
#if defined(BSWAP8)
		blocks[i].q[0] = BSWAP8(BSWAP8(*(u64*)key->md.data)+i);
#else
		blocks[i].c[7] += key->md.data[7]+i;
		if (blocks[i].c[7] < i) {
			int j;

			for (j=6;j>=0;j--) {
				if (blocks[i].c[j]=key->md.data[j]+1) break;
			}
		}
#endif
		blocks[i].c[8] = key->md.data[8];
		blocks[i].c[9] = key->md.data[9];
		blocks[i].c[10] = key->md.data[10];
		/* fix length */
		blocks[i].c[11] = (unsigned char)(len>>8);
		blocks[i].c[12] = (unsigned char)(len);

		memcpy(blocks[i].c+13,hash_d[i].ptr,64-13);
		hash_d[i].ptr += 64-13;
		hash_d[i].blocks = (len-(64-13))/64;

		edges[i].ptr = blocks[i].c;
		edges[i].blocks = 1;
	}

	sha1_multi_block(ctx,edges,n4x);
	sha1_multi_block(ctx,hash_d,n4x);

	memset(blocks,0,sizeof(blocks));
	for (i=0;i<x4;i++) {
		unsigned int		len = (i==(x4-1)?last:frag),
					off = hash_d[i].blocks*64;
		const unsigned char    *ptr = hash_d[i].ptr+off;

		off = len-(64-13)-off;	/* remainder actually */
		memcpy(blocks[i].c,ptr,off);
		blocks[i].c[off]=0x80;
		len += 64+13;		/* 64 is HMAC header */
		len *= 8;		/* convert to bits */
		if (off<(64-8)) {
			blocks[i].d[15] = BSWAP4(len);
			edges[i].blocks = 1;			
		} else {
			blocks[i].d[31] = BSWAP4(len);
			edges[i].blocks = 2;
		}
		edges[i].ptr = blocks[i].c;
	}

	sha1_multi_block(ctx,edges,n4x);

	memset(blocks,0,sizeof(blocks));
	for (i=0;i<x4;i++) {
		blocks[i].d[0] = BSWAP4(ctx->A[i]);	ctx->A[i] = key->tail.h0;
		blocks[i].d[1] = BSWAP4(ctx->B[i]);	ctx->B[i] = key->tail.h1;
		blocks[i].d[2] = BSWAP4(ctx->C[i]);	ctx->C[i] = key->tail.h2;
		blocks[i].d[3] = BSWAP4(ctx->D[i]);	ctx->D[i] = key->tail.h3;
		blocks[i].d[4] = BSWAP4(ctx->E[i]);	ctx->E[i] = key->tail.h4;
		blocks[i].c[20] = 0x80;
		blocks[i].d[15] = BSWAP4((64+20)*8);
		edges[i].ptr = blocks[i].c;
		edges[i].blocks = 1;
	}

	sha1_multi_block(ctx,edges,n4x);

	packlen = 5+16+((frag+20+16)&-16);

	out += (packlen<<(1+n4x))-packlen;
	inp += (frag<<(1+n4x))-frag;

	for (i=x4-1;;i--) {
		unsigned int len = (i==(x4-1)?last:frag), pad, j;
		unsigned char *out0 = out;

		out += 5+16;		/* place for header and explicit IV */
		ciph_d[i].inp = out;
		ciph_d[i].out = out;

		memmove(out,inp,len);
		out += len;
		inp -= frag;

		((u32 *)out)[0] = BSWAP4(ctx->A[i]);
		((u32 *)out)[1] = BSWAP4(ctx->B[i]);
		((u32 *)out)[2] = BSWAP4(ctx->C[i]);
		((u32 *)out)[3] = BSWAP4(ctx->D[i]);
		((u32 *)out)[4] = BSWAP4(ctx->E[i]);
		out += 20;
		len += 20+16;

		pad = 15-len%16;
		for (j=0;j<=pad;j++) *(out++) = pad;
		len += pad+1;

		ciph_d[i].blocks = len/16;

		/* arrange header */
		out0[0] = key->md.data[8];
		out0[1] = key->md.data[9];
		out0[2] = key->md.data[10];
		out0[3] = (unsigned char)(len>>8);
		out0[4] = (unsigned char)(len);

		/* explicit iv */
		RAND_bytes((u8 *)ciph_d[i].iv, 16);
		memcpy(&out[5], ciph_d[i].iv, 16);

		ret += len+5;

		if (i==0) break;

		out = out0-packlen;
	}

	aesni_multi_cbc_encrypt(ciph_d,&key->ks,n4x);

	return ret;
}
#endif

static int aesni_cbc_hmac_sha1_cipher(EVP_CIPHER_CTX *ctx, unsigned char *out,
		      const unsigned char *in, size_t len)
	{
	EVP_AES_HMAC_SHA1 *key = data(ctx);
	unsigned int l;
	size_t	plen = key->payload_length,
		iv = 0,		/* explicit IV in TLS 1.1 and later */
		sha_off = 0;
#if defined(STITCHED_CALL)
	size_t	aes_off = 0,
		blocks;

	sha_off = SHA_CBLOCK-key->md.num;
#endif

	key->payload_length = NO_PAYLOAD_LENGTH;

	if (len%AES_BLOCK_SIZE) return 0;

	if (ctx->encrypt) {
		if (plen==NO_PAYLOAD_LENGTH)
			plen = len;
		else if (len!=((plen+SHA_DIGEST_LENGTH+AES_BLOCK_SIZE)&-AES_BLOCK_SIZE))
			return 0;
		else if (key->aux.tls_ver >= TLS1_1_VERSION)
			iv = AES_BLOCK_SIZE;

#if defined(STITCHED_CALL)
		if (plen>(sha_off+iv) && (blocks=(plen-(sha_off+iv))/SHA_CBLOCK)) {
			SHA1_Update(&key->md,in+iv,sha_off);

			aesni_cbc_sha1_enc(in,out,blocks,&key->ks,
				ctx->iv,&key->md,in+iv+sha_off);
			blocks *= SHA_CBLOCK;
			aes_off += blocks;
			sha_off += blocks;
			key->md.Nh += blocks>>29;
			key->md.Nl += blocks<<=3;
			if (key->md.Nl<(unsigned int)blocks) key->md.Nh++;
		} else {
			sha_off = 0;
		}
#endif
		sha_off += iv;
		SHA1_Update(&key->md,in+sha_off,plen-sha_off);

		if (plen!=len)	{	/* "TLS" mode of operation */
			if (in!=out)
				memcpy(out+aes_off,in+aes_off,plen-aes_off);

			/* calculate HMAC and append it to payload */
			SHA1_Final(out+plen,&key->md);
			key->md = key->tail;
			SHA1_Update(&key->md,out+plen,SHA_DIGEST_LENGTH);
			SHA1_Final(out+plen,&key->md);

			/* pad the payload|hmac */
			plen += SHA_DIGEST_LENGTH;
			for (l=len-plen-1;plen<len;plen++) out[plen]=l;
			/* encrypt HMAC|padding at once */
			aesni_cbc_encrypt(out+aes_off,out+aes_off,len-aes_off,
					&key->ks,ctx->iv,1);
		} else {
			aesni_cbc_encrypt(in+aes_off,out+aes_off,len-aes_off,
					&key->ks,ctx->iv,1);
		}
	} else {
		union { unsigned int  u[SHA_DIGEST_LENGTH/sizeof(unsigned int)];
			unsigned char c[32+SHA_DIGEST_LENGTH]; } mac, *pmac;

		/* arrange cache line alignment */
		pmac = (void *)(((size_t)mac.c+31)&((size_t)0-32));

		/* decrypt HMAC|padding at once */
		aesni_cbc_encrypt(in,out,len,
				&key->ks,ctx->iv,0);

		if (plen) {	/* "TLS" mode of operation */
			size_t inp_len, mask, j, i;
			unsigned int res, maxpad, pad, bitlen;
			int ret = 1;
			union {	unsigned int  u[SHA_LBLOCK];
				unsigned char c[SHA_CBLOCK]; }
				*data = (void *)key->md.data;

			if ((key->aux.tls_aad[plen-4]<<8|key->aux.tls_aad[plen-3])
			    >= TLS1_1_VERSION)
				iv = AES_BLOCK_SIZE;

			if (len<(iv+SHA_DIGEST_LENGTH+1))
				return 0;

			/* omit explicit iv */
			out += iv;
			len -= iv;

			/* figure out payload length */
			pad = out[len-1];
			maxpad = len-(SHA_DIGEST_LENGTH+1);
			maxpad |= (255-maxpad)>>(sizeof(maxpad)*8-8);
			maxpad &= 255;

			inp_len = len - (SHA_DIGEST_LENGTH+pad+1);
			mask = (0-((inp_len-len)>>(sizeof(inp_len)*8-1)));
			inp_len &= mask;
			ret &= (int)mask;

			key->aux.tls_aad[plen-2] = inp_len>>8;
			key->aux.tls_aad[plen-1] = inp_len;

			/* calculate HMAC */
			key->md = key->head;
			SHA1_Update(&key->md,key->aux.tls_aad,plen);

#if 1
			len -= SHA_DIGEST_LENGTH;		/* amend mac */
			if (len>=(256+SHA_CBLOCK)) {
				j = (len-(256+SHA_CBLOCK))&(0-SHA_CBLOCK);
				j += SHA_CBLOCK-key->md.num;
				SHA1_Update(&key->md,out,j);
				out += j;
				len -= j;
				inp_len -= j;
			}

			/* but pretend as if we hashed padded payload */
			bitlen = key->md.Nl+(inp_len<<3);	/* at most 18 bits */
#ifdef BSWAP4
			bitlen = BSWAP4(bitlen);
#else
			mac.c[0] = 0;
			mac.c[1] = (unsigned char)(bitlen>>16);
			mac.c[2] = (unsigned char)(bitlen>>8);
			mac.c[3] = (unsigned char)bitlen;
			bitlen = mac.u[0];
#endif

			pmac->u[0]=0;
			pmac->u[1]=0;
			pmac->u[2]=0;
			pmac->u[3]=0;
			pmac->u[4]=0;

			for (res=key->md.num, j=0;j<len;j++) {
				size_t c = out[j];
				mask = (j-inp_len)>>(sizeof(j)*8-8);
				c &= mask;
				c |= 0x80&~mask&~((inp_len-j)>>(sizeof(j)*8-8));
				data->c[res++]=(unsigned char)c;

				if (res!=SHA_CBLOCK) continue;

				/* j is not incremented yet */
				mask = 0-((inp_len+7-j)>>(sizeof(j)*8-1));
				data->u[SHA_LBLOCK-1] |= bitlen&mask;
				sha1_block_data_order(&key->md,data,1);
				mask &= 0-((j-inp_len-72)>>(sizeof(j)*8-1));
				pmac->u[0] |= key->md.h0 & mask;
				pmac->u[1] |= key->md.h1 & mask;
				pmac->u[2] |= key->md.h2 & mask;
				pmac->u[3] |= key->md.h3 & mask;
				pmac->u[4] |= key->md.h4 & mask;
				res=0;
			}

			for(i=res;i<SHA_CBLOCK;i++,j++) data->c[i]=0;

			if (res>SHA_CBLOCK-8) {
				mask = 0-((inp_len+8-j)>>(sizeof(j)*8-1));
				data->u[SHA_LBLOCK-1] |= bitlen&mask;
				sha1_block_data_order(&key->md,data,1);
				mask &= 0-((j-inp_len-73)>>(sizeof(j)*8-1));
				pmac->u[0] |= key->md.h0 & mask;
				pmac->u[1] |= key->md.h1 & mask;
				pmac->u[2] |= key->md.h2 & mask;
				pmac->u[3] |= key->md.h3 & mask;
				pmac->u[4] |= key->md.h4 & mask;

				memset(data,0,SHA_CBLOCK);
				j+=64;
			}
			data->u[SHA_LBLOCK-1] = bitlen;
			sha1_block_data_order(&key->md,data,1);
			mask = 0-((j-inp_len-73)>>(sizeof(j)*8-1));
			pmac->u[0] |= key->md.h0 & mask;
			pmac->u[1] |= key->md.h1 & mask;
			pmac->u[2] |= key->md.h2 & mask;
			pmac->u[3] |= key->md.h3 & mask;
			pmac->u[4] |= key->md.h4 & mask;

#ifdef BSWAP4
			pmac->u[0] = BSWAP4(pmac->u[0]);
			pmac->u[1] = BSWAP4(pmac->u[1]);
			pmac->u[2] = BSWAP4(pmac->u[2]);
			pmac->u[3] = BSWAP4(pmac->u[3]);
			pmac->u[4] = BSWAP4(pmac->u[4]);
#else
			for (i=0;i<5;i++) {
				res = pmac->u[i];
				pmac->c[4*i+0]=(unsigned char)(res>>24);
				pmac->c[4*i+1]=(unsigned char)(res>>16);
				pmac->c[4*i+2]=(unsigned char)(res>>8);
				pmac->c[4*i+3]=(unsigned char)res;
			}
#endif
			len += SHA_DIGEST_LENGTH;
#else
			SHA1_Update(&key->md,out,inp_len);
			res = key->md.num;
			SHA1_Final(pmac->c,&key->md);

			{
			unsigned int inp_blocks, pad_blocks;

			/* but pretend as if we hashed padded payload */
			inp_blocks = 1+((SHA_CBLOCK-9-res)>>(sizeof(res)*8-1));
			res += (unsigned int)(len-inp_len);
			pad_blocks = res / SHA_CBLOCK;
			res %= SHA_CBLOCK;
			pad_blocks += 1+((SHA_CBLOCK-9-res)>>(sizeof(res)*8-1));
			for (;inp_blocks<pad_blocks;inp_blocks++)
				sha1_block_data_order(&key->md,data,1);
			}
#endif
			key->md = key->tail;
			SHA1_Update(&key->md,pmac->c,SHA_DIGEST_LENGTH);
			SHA1_Final(pmac->c,&key->md);

			/* verify HMAC */
			out += inp_len;
			len -= inp_len;
#if 1
			{
			unsigned char *p = out+len-1-maxpad-SHA_DIGEST_LENGTH;
			size_t off = out-p;
			unsigned int c, cmask;

			maxpad += SHA_DIGEST_LENGTH;
			for (res=0,i=0,j=0;j<maxpad;j++) {
				c = p[j];
				cmask = ((int)(j-off-SHA_DIGEST_LENGTH))>>(sizeof(int)*8-1);
				res |= (c^pad)&~cmask;	/* ... and padding */
				cmask &= ((int)(off-1-j))>>(sizeof(int)*8-1);
				res |= (c^pmac->c[i])&cmask;
				i += 1&cmask;
			}
			maxpad -= SHA_DIGEST_LENGTH;

			res = 0-((0-res)>>(sizeof(res)*8-1));
			ret &= (int)~res;
			}
#else
			for (res=0,i=0;i<SHA_DIGEST_LENGTH;i++)
				res |= out[i]^pmac->c[i];
			res = 0-((0-res)>>(sizeof(res)*8-1));
			ret &= (int)~res;

			/* verify padding */
			pad = (pad&~res) | (maxpad&res);
			out = out+len-1-pad;
			for (res=0,i=0;i<pad;i++)
				res |= out[i]^pad;

			res = (0-res)>>(sizeof(res)*8-1);
			ret &= (int)~res;
#endif
			return ret;
		} else {
			SHA1_Update(&key->md,out,len);
		}
	}

	return 1;
	}

static int aesni_cbc_hmac_sha1_ctrl(EVP_CIPHER_CTX *ctx, int type, int arg, void *ptr)
	{
	EVP_AES_HMAC_SHA1 *key = data(ctx);

	switch (type)
		{
	case EVP_CTRL_AEAD_SET_MAC_KEY:
		{
		unsigned int  i;
		unsigned char hmac_key[64];

		memset (hmac_key,0,sizeof(hmac_key));

		if (arg > (int)sizeof(hmac_key)) {
			SHA1_Init(&key->head);
			SHA1_Update(&key->head,ptr,arg);
			SHA1_Final(hmac_key,&key->head);
		} else {
			memcpy(hmac_key,ptr,arg);
		}

		for (i=0;i<sizeof(hmac_key);i++)
			hmac_key[i] ^= 0x36;		/* ipad */
		SHA1_Init(&key->head);
		SHA1_Update(&key->head,hmac_key,sizeof(hmac_key));

		for (i=0;i<sizeof(hmac_key);i++)
			hmac_key[i] ^= 0x36^0x5c;	/* opad */
		SHA1_Init(&key->tail);
		SHA1_Update(&key->tail,hmac_key,sizeof(hmac_key));

		OPENSSL_cleanse(hmac_key,sizeof(hmac_key));

		return 1;
		}
	case EVP_CTRL_AEAD_TLS1_AAD:
		{
		unsigned char *p=ptr;
		unsigned int   len=p[arg-2]<<8|p[arg-1];

		if (ctx->encrypt)
			{
			key->payload_length = len;
			if ((key->aux.tls_ver=p[arg-4]<<8|p[arg-3]) >= TLS1_1_VERSION) {
				len -= AES_BLOCK_SIZE;
				p[arg-2] = len>>8;
				p[arg-1] = len;
			}
			key->md = key->head;
			SHA1_Update(&key->md,p,arg);

			return (int)(((len+SHA_DIGEST_LENGTH+AES_BLOCK_SIZE)&-AES_BLOCK_SIZE)
				- len);
			}
		else
			{
			if (arg>13) arg = 13;
			memcpy(key->aux.tls_aad,ptr,arg);
			key->payload_length = arg;

			return SHA_DIGEST_LENGTH;
			}
		}
#if EVP_EVP_CIPH_FLAG_TLS11_MULTI_BLOCK
	case EVP_CTRL_TLS11_MULTI_BLOCK_AAD:
		{
		EVP_CTRL_TLS11_MULTI_BLOCK_PARAM *param =
			(EVP_CTRL_TLS11_MULTI_BLOCK_PARAM *)ptr;
		unsigned int n4x=1, x4;
		unsigned int frag, last, packlen, inp_len;

		if (arg<sizeof(EVP_CTRL_TLS11_MULTI_BLOCK_PARAM)) return -1;

		inp_len = param->inp[11]<<8|param->inp[12];

		if (ctx->encrypt)
			{
			if ((param->inp[9]<<8|param->inp[10]) < TLS1_1_VERSION)
				return -1;
	
			if (inp_len<2048) return -1;	/* too short */

			if (inp_len>=6144) n4x=2;

			key->md = key->head;
			SHA1_Update(&key->md,param->inp,13);

			x4 = 4*n4x; n4x += 1;

			frag = inp_len>>n4x;
			last = inp_len+frag-(frag<<n4x);
			if (last>frag && ((last+13+9)%64<(x4-1))) {
				frag++;
				last -= x4-1;
			}

			packlen = 5+16+((frag+20+16)&-16);
			packlen = (packlen<<(1+n4x))-packlen;
			packlen += 5+16+((last+20+16)&-16);

			param->interleave = x4;

			return (int)packlen;
			}
		else
			return -1;	/* not yet */
		}
	case EVP_CTRL_TLS11_MULTI_BLOCK_ENCRYPT:
		{
		EVP_CTRL_TLS11_MULTI_BLOCK_PARAM *param =
			(EVP_CTRL_TLS11_MULTI_BLOCK_PARAM *)ptr;

		return tls11_multi_block_encrypt(key,param->out,param->inp,
						param->len,param->interleave/4);
		}
	case EVP_CTRL_TLS11_MULTI_BLOCK_DECRYPT:
#endif
	default:
		return -1;
		}
	}

static EVP_CIPHER aesni_128_cbc_hmac_sha1_cipher =
	{
#ifdef NID_aes_128_cbc_hmac_sha1
	NID_aes_128_cbc_hmac_sha1,
#else
	NID_undef,
#endif
	16,16,16,
	EVP_CIPH_CBC_MODE|EVP_CIPH_FLAG_DEFAULT_ASN1|
	EVP_CIPH_FLAG_AEAD_CIPHER|EVP_CIPH_FLAG_TLS11_MULTI_BLOCK,
	aesni_cbc_hmac_sha1_init_key,
	aesni_cbc_hmac_sha1_cipher,
	NULL,
	sizeof(EVP_AES_HMAC_SHA1),
	EVP_CIPH_FLAG_DEFAULT_ASN1?NULL:EVP_CIPHER_set_asn1_iv,
	EVP_CIPH_FLAG_DEFAULT_ASN1?NULL:EVP_CIPHER_get_asn1_iv,
	aesni_cbc_hmac_sha1_ctrl,
	NULL
	};

static EVP_CIPHER aesni_256_cbc_hmac_sha1_cipher =
	{
#ifdef NID_aes_256_cbc_hmac_sha1
	NID_aes_256_cbc_hmac_sha1,
#else
	NID_undef,
#endif
	16,32,16,
	EVP_CIPH_CBC_MODE|EVP_CIPH_FLAG_DEFAULT_ASN1|
	EVP_CIPH_FLAG_AEAD_CIPHER|EVP_CIPH_FLAG_TLS11_MULTI_BLOCK,
	aesni_cbc_hmac_sha1_init_key,
	aesni_cbc_hmac_sha1_cipher,
	NULL,
	sizeof(EVP_AES_HMAC_SHA1),
	EVP_CIPH_FLAG_DEFAULT_ASN1?NULL:EVP_CIPHER_set_asn1_iv,
	EVP_CIPH_FLAG_DEFAULT_ASN1?NULL:EVP_CIPHER_get_asn1_iv,
	aesni_cbc_hmac_sha1_ctrl,
	NULL
	};

const EVP_CIPHER *EVP_aes_128_cbc_hmac_sha1(void)
	{
	return(OPENSSL_ia32cap_P[1]&AESNI_CAPABLE?
		&aesni_128_cbc_hmac_sha1_cipher:NULL);
	}

const EVP_CIPHER *EVP_aes_256_cbc_hmac_sha1(void)
	{
	return(OPENSSL_ia32cap_P[1]&AESNI_CAPABLE?
		&aesni_256_cbc_hmac_sha1_cipher:NULL);
	}
#else
const EVP_CIPHER *EVP_aes_128_cbc_hmac_sha1(void)
	{
	return NULL;
	}
const EVP_CIPHER *EVP_aes_256_cbc_hmac_sha1(void)
	{
	return NULL;
	}
#endif
#endif
