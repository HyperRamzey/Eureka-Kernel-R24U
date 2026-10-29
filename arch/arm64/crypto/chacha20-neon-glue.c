/*
 * ChaCha20 256-bit cipher algorithm, RFC7539, arm64 NEON glue
 *
 * Ported from Linux 5.15 arch/arm64/crypto/chacha-neon-glue.c
 * (Copyright (C) 2016 - 2017 Linaro, Ltd. <ard.biesheuvel@linaro.org>),
 * adapted to the 4.4 blkcipher API used by crypto/chacha20_generic.c.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <crypto/algapi.h>
#include <crypto/chacha20.h>
#include <linux/crypto.h>
#include <linux/kernel.h>
#include <linux/module.h>

#include <asm/hwcap.h>
#include <asm/neon.h>
#include <asm/simd.h>

#define CHACHA20_NROUNDS	20

asmlinkage void chacha_block_xor_neon(u32 *state, u8 *dst, const u8 *src,
				      int nrounds);
asmlinkage void chacha_4block_xor_neon(u32 *state, u8 *dst, const u8 *src,
				       int nrounds, int bytes);

static void chacha20_doneon(u32 *state, u8 *dst, const u8 *src,
			    unsigned int bytes)
{
	u8 buf[CHACHA20_BLOCK_SIZE];

	while (bytes > 0) {
		unsigned int l = min_t(unsigned int, bytes, CHACHA20_BLOCK_SIZE * 5);

		if (l <= CHACHA20_BLOCK_SIZE) {
			memcpy(buf, src, l);
			chacha_block_xor_neon(state, buf, buf, CHACHA20_NROUNDS);
			memcpy(dst, buf, l);
			state[12] += 1;
			break;
		}

		chacha_4block_xor_neon(state, dst, src, CHACHA20_NROUNDS, l);
		bytes -= l;
		src += l;
		dst += l;
		state[12] += DIV_ROUND_UP(l, CHACHA20_BLOCK_SIZE);
	}
}

static int chacha20_neon_crypt(struct blkcipher_desc *desc,
			       struct scatterlist *dst, struct scatterlist *src,
			       unsigned int nbytes)
{
	struct blkcipher_walk walk;
	u32 state[16];
	int err;

	if (!may_use_simd())
		return crypto_chacha20_crypt(desc, dst, src, nbytes);

	blkcipher_walk_init(&walk, dst, src, nbytes);
	err = blkcipher_walk_virt_block(desc, &walk, CHACHA20_BLOCK_SIZE);
	if (err)
		return err;

	crypto_chacha20_init(state, crypto_blkcipher_ctx(desc->tfm), walk.iv);

	while (walk.nbytes >= CHACHA20_BLOCK_SIZE) {
		unsigned int len = rounddown(walk.nbytes, CHACHA20_BLOCK_SIZE);

		kernel_neon_begin();
		chacha20_doneon(state, walk.dst.virt.addr, walk.src.virt.addr,
				len);
		kernel_neon_end();

		err = blkcipher_walk_done(desc, &walk, walk.nbytes - len);
		if (err)
			return err;
	}

	if (walk.nbytes) {
		kernel_neon_begin();
		chacha20_doneon(state, walk.dst.virt.addr, walk.src.virt.addr,
				walk.nbytes);
		kernel_neon_end();
		err = blkcipher_walk_done(desc, &walk, 0);
	}

	return err;
}

static struct crypto_alg alg = {
	.cra_name		= "chacha20",
	.cra_driver_name	= "chacha20-neon",
	.cra_priority		= 300,
	.cra_flags		= CRYPTO_ALG_TYPE_BLKCIPHER,
	.cra_blocksize		= 1,
	.cra_type		= &crypto_blkcipher_type,
	.cra_ctxsize		= sizeof(struct chacha20_ctx),
	.cra_alignmask		= sizeof(u32) - 1,
	.cra_module		= THIS_MODULE,
	.cra_u			= {
		.blkcipher = {
			.min_keysize	= CHACHA20_KEY_SIZE,
			.max_keysize	= CHACHA20_KEY_SIZE,
			.ivsize		= CHACHA20_IV_SIZE,
			.geniv		= "seqiv",
			.setkey		= crypto_chacha20_setkey,
			.encrypt	= chacha20_neon_crypt,
			.decrypt	= chacha20_neon_crypt,
		},
	},
};

static int __init chacha20_neon_mod_init(void)
{
	if (!(elf_hwcap & HWCAP_ASIMD))
		return -ENODEV;

	return crypto_register_alg(&alg);
}

static void __exit chacha20_neon_mod_fini(void)
{
	crypto_unregister_alg(&alg);
}

module_init(chacha20_neon_mod_init);
module_exit(chacha20_neon_mod_fini);

MODULE_AUTHOR("Ard Biesheuvel <ard.biesheuvel@linaro.org>");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS_CRYPTO("chacha20");
MODULE_ALIAS_CRYPTO("chacha20-neon");
MODULE_DESCRIPTION("ChaCha20 cipher algorithm, RFC7539 (NEON accelerated)");
