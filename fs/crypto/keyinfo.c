/*
 * key management facility for FS encryption support.
 *
 * Copyright (C) 2015, Google, Inc.
 *
 * This contains encryption key functions.
 *
 * Written by Michael Halcrow, Ildar Muslukhov, and Uday Savagaonkar, 2015.
 */

#include <keys/user-type.h>
#include <linux/scatterlist.h>
#include <linux/fscrypto.h>
#include "ext4_ice.h"

#ifndef CONFIG_FS_CRYPTO_SEC_EXTENSION
static void derive_crypt_complete(struct crypto_async_request *req, int rc)
{
	struct fscrypt_completion_result *ecr = req->data;

	if (rc == -EINPROGRESS)
		return;

	ecr->res = rc;
	complete(&ecr->completion);
}

/**
 * derive_key_aes() - Derive a key using AES-128-ECB
 * @deriving_key: Encryption key used for derivation.
 * @source_key:   Source key to which to apply derivation.
 * @derived_key:  Derived key.
 *
 * Return: Zero on success; non-zero otherwise.
 */
static int derive_key_aes(u8 deriving_key[FS_AES_128_ECB_KEY_SIZE],
				u8 source_key[FS_AES_256_XTS_KEY_SIZE],
				u8 derived_key[FS_AES_256_XTS_KEY_SIZE])
{
	int res = 0;
	struct skcipher_request *req = NULL;
	DECLARE_FS_COMPLETION_RESULT(ecr);
	struct scatterlist src_sg, dst_sg;
	struct crypto_skcipher *tfm = crypto_alloc_skcipher("ecb(aes)", 0, 0);

	if (IS_ERR(tfm)) {
		res = PTR_ERR(tfm);
		tfm = NULL;
		goto out;
	}
	crypto_skcipher_set_flags(tfm, CRYPTO_TFM_REQ_WEAK_KEY);
	req = skcipher_request_alloc(tfm, GFP_NOFS);
	if (!req) {
		res = -ENOMEM;
		goto out;
	}
	skcipher_request_set_callback(req,
			CRYPTO_TFM_REQ_MAY_BACKLOG | CRYPTO_TFM_REQ_MAY_SLEEP,
			derive_crypt_complete, &ecr);
	res = crypto_skcipher_setkey(tfm, deriving_key,
					FS_AES_128_ECB_KEY_SIZE);
	if (res < 0)
		goto out;

	sg_init_one(&src_sg, source_key, FS_AES_256_XTS_KEY_SIZE);
	sg_init_one(&dst_sg, derived_key, FS_AES_256_XTS_KEY_SIZE);
	skcipher_request_set_crypt(req, &src_sg, &dst_sg,
					FS_AES_256_XTS_KEY_SIZE, NULL);
	res = crypto_skcipher_encrypt(req);
	if (res == -EINPROGRESS || res == -EBUSY) {
		wait_for_completion(&ecr.completion);
		res = ecr.res;
	}
out:
	skcipher_request_free(req);
	crypto_free_skcipher(tfm);
	return res;
}
#endif /* !CONFIG FS_CRYPTO_SEC_EXTENSION */

static inline int get_fe_key(char *nonce, char *src_key, char *fe_key)
{
#ifdef CONFIG_FS_CRYPTO_SEC_EXTENSION
	return fscrypt_sec_get_key_aes(nonce, src_key, fe_key);
#else
	return derive_key_aes(nonce, src_key, fe_key);
#endif /* CONFIG FS_CRYPTO_SEC_EXTENSION */
}

static int validate_user_key(struct fscrypt_info *crypt_info,
			struct fscrypt_context *ctx, u8 *raw_key,
			u8 *prefix, int prefix_size)
{
	u8 *full_key_descriptor;
	struct key *keyring_key;
	struct fscrypt_key *master_key;
	const struct user_key_payload *ukp;
	int full_key_len = prefix_size + (FS_KEY_DESCRIPTOR_SIZE * 2) + 1;
	int res;

	full_key_descriptor = kmalloc(full_key_len, GFP_NOFS);
	if (!full_key_descriptor)
		return -ENOMEM;

	memcpy(full_key_descriptor, prefix, prefix_size);
	sprintf(full_key_descriptor + prefix_size,
			"%*phN", FS_KEY_DESCRIPTOR_SIZE,
			ctx->master_key_descriptor);
	full_key_descriptor[full_key_len - 1] = '\0';
	keyring_key = request_key(&key_type_logon, full_key_descriptor, NULL);
	kfree(full_key_descriptor);
	if (IS_ERR(keyring_key))
		return PTR_ERR(keyring_key);
	down_read(&keyring_key->sem);

	if (keyring_key->type != &key_type_logon) {
		printk_once(KERN_WARNING
				"%s: key type must be logon\n", __func__);
		res = -ENOKEY;
		goto out;
	}
	ukp = user_key_payload(keyring_key);
	if (!ukp) {
		/* key was revoked before we acquired its semaphore */
		res = -EKEYREVOKED;
		goto out;
	}
	if (ukp->datalen != sizeof(struct fscrypt_key)) {
		res = -EINVAL;
		goto out;
	}
	master_key = (struct fscrypt_key *)ukp->data;
	BUILD_BUG_ON(FS_ESTIMATED_NONCE_SIZE != FS_KEY_DERIVATION_NONCE_SIZE);

	if (master_key->size != FS_AES_256_XTS_KEY_SIZE) {
		printk_once(KERN_WARNING
				"%s: key size incorrect: %d\n",
				__func__, master_key->size);
		res = -ENOKEY;
		goto out;
	}
	res = get_fe_key(ctx->nonce, master_key->raw, raw_key);
out:
	up_read(&keyring_key->sem);
	key_put(keyring_key);
	return res;
}

static int determine_cipher_type(struct fscrypt_info *ci, struct inode *inode,
		const char **cipher_str_ret, int *keysize_ret, int *fname)
{
	if (S_ISREG(inode->i_mode)) {
		if (ci->ci_data_mode == FS_ENCRYPTION_MODE_AES_256_XTS) {
			*cipher_str_ret = "xts(aes)";
			*keysize_ret = FS_AES_256_XTS_KEY_SIZE;
			return 0;
		} else if (ci->ci_data_mode == FS_ENCRYPTION_MODE_PRIVATE) {
			*cipher_str_ret = "bugon";
			*keysize_ret = FS_AES_256_XTS_KEY_SIZE;
			return 0;
		}
		pr_warn_once("fscrypto: unsupported contents encryption mode "
			     "%d for inode %lu\n",
			     ci->ci_data_mode, inode->i_ino);
		return -ENOKEY;
	}

	if (S_ISDIR(inode->i_mode) || S_ISLNK(inode->i_mode)) {
		if (ci->ci_filename_mode == FS_ENCRYPTION_MODE_AES_256_CTS) {
			*cipher_str_ret = "cts(cbc(aes))";
			*keysize_ret = FS_AES_256_CTS_KEY_SIZE;
			*fname = 1;
			return 0;
		}
		pr_warn_once("fscrypto: unsupported filenames encryption mode "
			     "%d for inode %lu\n",
			     ci->ci_filename_mode, inode->i_ino);
		return -ENOKEY;
	}

	pr_warn_once("fscrypto: unsupported file type %d for inode %lu\n",
		     (inode->i_mode & S_IFMT), inode->i_ino);
	return -ENOKEY;
}

static void put_crypt_info(struct fscrypt_info *ci)
{
	if (!ci)
		return;

	crypto_free_skcipher(ci->ci_ctfm);
	memzero_explicit(ci->ci_raw_key,
		sizeof(ci->ci_raw_key));
	kmem_cache_free(fscrypt_info_cachep, ci);
}

static int fs_data_encryption_mode(void)
{
	return ext4_is_ice_enabled() ? FS_ENCRYPTION_MODE_PRIVATE :
		FS_ENCRYPTION_MODE_AES_256_XTS;
}

int fs_using_hardware_encryption(struct inode *inode)
{
	struct fscrypt_info *ci = inode->i_crypt_info;

	return S_ISREG(inode->i_mode) && ci &&
		ci->ci_data_mode == FS_ENCRYPTION_MODE_PRIVATE;
}
EXPORT_SYMBOL(fs_using_hardware_encryption);

int fscrypt_get_encryption_info(struct inode *inode)
{
	struct fscrypt_info *crypt_info;
	struct fscrypt_context ctx;
	struct crypto_skcipher *ctfm;
	const char *cipher_str;
	int keysize;
	int res;
	int fname = 0;

	if (inode->i_crypt_info)
		return 0;

	res = fscrypt_initialize();
	if (res)
		return res;

	if (!inode->i_sb->s_cop->get_context)
		return -EOPNOTSUPP;

	res = inode->i_sb->s_cop->get_context(inode, &ctx, sizeof(ctx));
	if (res < 0) {
		if (!fscrypt_dummy_context_enabled(inode))
			return res;
		ctx.format = FS_ENCRYPTION_CONTEXT_FORMAT_V1;
		ctx.contents_encryption_mode = fs_data_encryption_mode();
		ctx.filenames_encryption_mode = FS_ENCRYPTION_MODE_AES_256_CTS;
		ctx.flags = 0;
	} else if (res != sizeof(ctx)) {
		return -EINVAL;
	}

	if (ctx.format != FS_ENCRYPTION_CONTEXT_FORMAT_V1)
		return -EINVAL;

	if (ctx.flags & ~FS_POLICY_FLAGS_VALID)
		return -EINVAL;

	crypt_info = kmem_cache_alloc(fscrypt_info_cachep, GFP_NOFS);
	if (!crypt_info)
		return -ENOMEM;

	crypt_info->ci_flags = ctx.flags;
	crypt_info->ci_data_mode = ctx.contents_encryption_mode;
	crypt_info->ci_filename_mode = ctx.filenames_encryption_mode;
	crypt_info->ci_ctfm = NULL;
	memcpy(crypt_info->ci_master_key, ctx.master_key_descriptor,
				sizeof(crypt_info->ci_master_key));

	res = determine_cipher_type(crypt_info, inode, &cipher_str, &keysize,
				&fname);
	if (res)
		goto out;

	/*
	 * This cannot be a stack buffer because it is passed to the scatterlist
	 * crypto API as part of key derivation.
	 */
	res = -ENOMEM;

	if (fscrypt_dummy_context_enabled(inode)) {
		memset(crypt_info->ci_raw_key, 0x42, FS_AES_256_XTS_KEY_SIZE);
		goto got_key;
	}

	res = validate_user_key(crypt_info, &ctx, crypt_info->ci_raw_key,
			FS_KEY_DESC_PREFIX, FS_KEY_DESC_PREFIX_SIZE);
	if (res && inode->i_sb->s_cop->key_prefix) {
		u8 *prefix = NULL;
		int prefix_size, res2;

		prefix_size = inode->i_sb->s_cop->key_prefix(inode, &prefix);
		res2 = validate_user_key(crypt_info, &ctx,
				crypt_info->ci_raw_key,	prefix, prefix_size);
		if (res2) {
			if (res2 == -ENOKEY)
				res = -ENOKEY;
			goto out;
		}
	} else if (res) {
		goto out;
	}
got_key:
	if (crypt_info->ci_data_mode != FS_ENCRYPTION_MODE_PRIVATE || fname) {
		ctfm = crypto_alloc_skcipher(cipher_str, 0, 0);
		if (!ctfm || IS_ERR(ctfm)) {
			res = ctfm ? PTR_ERR(ctfm) : -ENOMEM;
			pr_err("%s: error %d inode %u allocating crypto tfm\n",
				__func__, res, (unsigned int) inode->i_ino);
			goto out;
		}
		crypt_info->ci_ctfm = ctfm;
		crypto_skcipher_clear_flags(ctfm, ~0);
		crypto_skcipher_set_flags(ctfm, CRYPTO_TFM_REQ_WEAK_KEY);
		res = crypto_skcipher_setkey(ctfm, crypt_info->ci_raw_key,
						keysize);
		if (res)
			goto out;
	} else if (!ext4_is_ice_enabled()) {
		pr_warn("%s: ICE support not available\n",
			__func__);
		res = -EINVAL;
		goto out;
	}
	if (cmpxchg(&inode->i_crypt_info, NULL, crypt_info) == NULL)
		crypt_info = NULL;
out:
	if (res == -ENOKEY)
		res = 0;
	put_crypt_info(crypt_info);
	return res;
}
EXPORT_SYMBOL(fscrypt_get_encryption_info);

void fscrypt_put_encryption_info(struct inode *inode, struct fscrypt_info *ci)
{
	struct fscrypt_info *prev;

	if (ci == NULL)
		ci = ACCESS_ONCE(inode->i_crypt_info);
	if (ci == NULL)
		return;

	prev = cmpxchg(&inode->i_crypt_info, ci, NULL);
	if (prev != ci)
		return;

	put_crypt_info(ci);
}
EXPORT_SYMBOL(fscrypt_put_encryption_info);
