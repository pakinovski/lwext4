/*
 * Copyright (c) 2015 Grzegorz Kostka (kostka.grzegorz@gmail.com)
 * Copyright (c) 2015 Kaho Ng (ngkaho1234@gmail.com)
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 * - The name of the author may not be used to endorse or promote products
 *   derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/** @addtogroup lwext4
 * @{
 */
/**
 * @file  ext4_journal.c
 * @brief Journal handle functions
 */

#include "ext4_config.h"
#include "ext4_types.h"
#include "ext4_fs.h"
#include "ext4_super.h"
#include "ext4_journal.h"
#include "ext4_errno.h"
#include "ext4_blockdev.h"
#include "ext4_crc32c.h"
#include "ext4_debug.h"

#include <string.h>
#include <stdlib.h>

/**@brief  Revoke entry during journal replay.*/
struct revoke_entry {
	/**@brief  Block number not to be replayed.*/
	ext4_fsblk_t block;

	/**@brief  For any transaction id smaller
	 *         than trans_id, records of @block
	 *         in those transactions should not
	 *         be replayed.*/
	uint32_t trans_id;

	/**@brief  Revoke tree node.*/
	RB_ENTRY(revoke_entry) revoke_node;
};

/**@brief  Valid journal replay information.*/
struct recover_info {
	/**@brief  Starting transaction id.*/
	uint32_t start_trans_id;

	/**@brief  Ending transaction id.*/
	uint32_t last_trans_id;

	/**@brief  Used as internal argument.*/
	uint32_t this_trans_id;

	/**@brief  RB-Tree storing revoke entries.*/
	RB_HEAD(jbd_revoke, revoke_entry) revoke_root;
};

/**@brief  Journal replay internal arguments.*/
struct replay_arg {
	/**@brief  Journal replay information.*/
	struct recover_info *info;

	/**@brief  Current block we are on.*/
	uint32_t *this_block;

	/**@brief  Current trans_id we are on.*/
	uint32_t this_trans_id;
};

static int
jbd_revoke_entry_cmp(struct revoke_entry *a, struct revoke_entry *b)
{
	if (a->block > b->block)
		return 1;
	else if (a->block < b->block)
		return -1;
	return 0;
}

static int
jbd_block_rec_cmp(struct jbd_block_rec *a, struct jbd_block_rec *b)
{
	if (a->lba > b->lba)
		return 1;
	else if (a->lba < b->lba)
		return -1;
	return 0;
}

RB_GENERATE_INTERNAL(jbd_revoke, revoke_entry, revoke_node,
		     jbd_revoke_entry_cmp, static inline)
RB_GENERATE_INTERNAL(jbd_block, jbd_block_rec, block_rec_node,
		     jbd_block_rec_cmp, static inline)

#define jbd_alloc_revoke_entry() calloc(1, sizeof(struct revoke_entry))
#define jbd_free_revoke_entry(addr) free(addr)

/**@brief  Write jbd superblock to disk.
 * @param  jbd_fs jbd filesystem
 * @param  s jbd superblock
 * @return standard error code*/
static int jbd_sb_write(struct jbd_fs *jbd_fs, struct jbd_sb *s)
{
	int rc;
	struct ext4_fs *fs = jbd_fs->inode_ref.fs;
	uint64_t offset;
	ext4_fsblk_t fblock;
	rc = jbd_inode_bmap(jbd_fs, 0, &fblock);
	if (rc != EOK)
		return rc;

	offset = fblock * ext4_sb_get_block_size(&fs->sb);
	return ext4_block_writebytes(fs->bdev, offset, s,
				     EXT4_SUPERBLOCK_SIZE);
}

/**@brief  Read jbd superblock from disk.
 * @param  jbd_fs jbd filesystem
 * @param  s jbd superblock
 * @return standard error code*/
static int jbd_sb_read(struct jbd_fs *jbd_fs, struct jbd_sb *s)
{
	int rc;
	struct ext4_fs *fs = jbd_fs->inode_ref.fs;
	uint64_t offset;
	ext4_fsblk_t fblock;
	rc = jbd_inode_bmap(jbd_fs, 0, &fblock);
	if (rc != EOK)
		return rc;

	offset = fblock * ext4_sb_get_block_size(&fs->sb);
	return ext4_block_readbytes(fs->bdev, offset, s,
				    EXT4_SUPERBLOCK_SIZE);
}

/**@brief  Verify jbd superblock.
 * @param  sb jbd superblock
 * @return true if jbd superblock is valid */
static bool jbd_verify_sb(struct jbd_sb *sb)
{
	struct jbd_bhdr *header = &sb->header;
	if (jbd_get32(header, magic) != JBD_MAGIC_NUMBER)
		return false;

	if (jbd_get32(header, blocktype) != JBD_SUPERBLOCK &&
	    jbd_get32(header, blocktype) != JBD_SUPERBLOCK_V2)
		return false;

	return true;
}

/**@brief  Write back dirty jbd superblock to disk.
 * @param  jbd_fs jbd filesystem
 * @return standard error code*/
static int jbd_write_sb(struct jbd_fs *jbd_fs)
{
	int rc = EOK;
	if (jbd_fs->dirty) {
		rc = jbd_sb_write(jbd_fs, &jbd_fs->sb);
		if (rc != EOK)
			return rc;

		jbd_fs->dirty = false;
	}
	return rc;
}

/**@brief  Get reference to jbd filesystem.
 * @param  fs Filesystem to load journal of
 * @param  jbd_fs jbd filesystem
 * @return standard error code*/
int jbd_get_fs(struct ext4_fs *fs,
	       struct jbd_fs *jbd_fs)
{
	int rc;
	uint32_t journal_ino;

	memset(jbd_fs, 0, sizeof(struct jbd_fs));
	/* See if there is journal inode on this filesystem.*/
	/* FIXME: detection on existance ofbkejournal bdev is
	 *        missing.*/
	journal_ino = ext4_get32(&fs->sb, journal_inode_number);

	rc = ext4_fs_get_inode_ref(fs,
				   journal_ino,
				   &jbd_fs->inode_ref);
	if (rc != EOK) {
		memset(jbd_fs, 0, sizeof(struct jbd_fs));
		return rc;
	}
	rc = jbd_sb_read(jbd_fs, &jbd_fs->sb);
	if (rc != EOK) {
		memset(jbd_fs, 0, sizeof(struct jbd_fs));
		ext4_fs_put_inode_ref(&jbd_fs->inode_ref);
		return rc;
	}
	if (!jbd_verify_sb(&jbd_fs->sb)) {
		memset(jbd_fs, 0, sizeof(struct jbd_fs));
		ext4_fs_put_inode_ref(&jbd_fs->inode_ref);
		rc = EIO;
	}

	return rc;
}

/**@brief  Put reference of jbd filesystem.
 * @param  jbd_fs jbd filesystem
 * @return standard error code*/
int jbd_put_fs(struct jbd_fs *jbd_fs)
{
	int rc = EOK;
	rc = jbd_write_sb(jbd_fs);

	ext4_fs_put_inode_ref(&jbd_fs->inode_ref);
	return rc;
}

/**@brief  Data block lookup helper.
 * @param  jbd_fs jbd filesystem
 * @param  iblock block index
 * @param  fblock logical block address
 * @return standard error code*/
int jbd_inode_bmap(struct jbd_fs *jbd_fs,
		   ext4_lblk_t iblock,
		   ext4_fsblk_t *fblock)
{
	int rc = ext4_fs_get_inode_dblk_idx(
			&jbd_fs->inode_ref,
			iblock,
			fblock,
			false);
	return rc;
}

/**@brief   jbd block get function (through cache).
 * @param   jbd_fs jbd filesystem
 * @param   block block descriptor
 * @param   fblock jbd logical block address
 * @return  standard error code*/
static int jbd_block_get(struct jbd_fs *jbd_fs,
		  struct ext4_block *block,
		  ext4_fsblk_t fblock)
{
	/* TODO: journal device. */
	int rc;
	ext4_lblk_t iblock = (ext4_lblk_t)fblock;

	/* Lookup the logical block address of
	 * fblock.*/
	rc = jbd_inode_bmap(jbd_fs, iblock,
			    &fblock);
	if (rc != EOK)
		return rc;

	struct ext4_blockdev *bdev = jbd_fs->inode_ref.fs->bdev;
	rc = ext4_block_get(bdev, block, fblock);

	/* If succeeded, mark buffer as BC_FLUSH to indicate
	 * that data should be written to disk immediately.*/
	if (rc == EOK)
		ext4_bcache_set_flag(block->buf, BC_FLUSH);

	return rc;
}

/**@brief   jbd block get function (through cache, don't read).
 * @param   jbd_fs jbd filesystem
 * @param   block block descriptor
 * @param   fblock jbd logical block address
 * @return  standard error code*/
static int jbd_block_get_noread(struct jbd_fs *jbd_fs,
			 struct ext4_block *block,
			 ext4_fsblk_t fblock)
{
	/* TODO: journal device. */
	int rc;
	ext4_lblk_t iblock = (ext4_lblk_t)fblock;
	rc = jbd_inode_bmap(jbd_fs, iblock,
			    &fblock);
	if (rc != EOK)
		return rc;

	struct ext4_blockdev *bdev = jbd_fs->inode_ref.fs->bdev;
	rc = ext4_block_get_noread(bdev, block, fblock);
	if (rc == EOK)
		ext4_bcache_set_flag(block->buf, BC_FLUSH);

	return rc;
}

/**@brief   jbd block set procedure (through cache).
 * @param   jbd_fs jbd filesystem
 * @param   block block descriptor
 * @return  standard error code*/
static int jbd_block_set(struct jbd_fs *jbd_fs,
		  struct ext4_block *block)
{
	return ext4_block_set(jbd_fs->inode_ref.fs->bdev,
			      block);
}

/**@brief  helper functions to calculate
 *         block tag size, not including UUID part.
 * @param  jbd_fs jbd filesystem
 * @return tag size in bytes*/
static int jbd_tag_bytes(struct jbd_fs *jbd_fs)
{
	int size;

	/* It is very easy to deal with the case which
	 * JBD_FEATURE_INCOMPAT_CSUM_V3 is enabled.*/
	if (JBD_HAS_INCOMPAT_FEATURE(&jbd_fs->sb,
				     JBD_FEATURE_INCOMPAT_CSUM_V3))
		return sizeof(struct jbd_block_tag3);

	size = sizeof(struct jbd_block_tag);

	/* If JBD_FEATURE_INCOMPAT_CSUM_V2 is enabled,
	 * add 2 bytes to size.*/
	if (JBD_HAS_INCOMPAT_FEATURE(&jbd_fs->sb,
				     JBD_FEATURE_INCOMPAT_CSUM_V2))
		size += sizeof(uint16_t);

	if (JBD_HAS_INCOMPAT_FEATURE(&jbd_fs->sb,
				     JBD_FEATURE_INCOMPAT_64BIT))
		return size;

	/* If block number is 4 bytes in size,
	 * minus 4 bytes from size */
	return size - sizeof(uint32_t);
}

/**@brief  Tag information. */
struct tag_info {
	/**@brief  Tag size in bytes, including UUID part.*/
	int tag_bytes;

	/**@brief  block number stored in this tag.*/
	ext4_fsblk_t block;

	/**@brief  whether UUID part exists or not.*/
	bool uuid_exist;

	/**@brief  UUID content if UUID part exists.*/
	uint8_t uuid[UUID_SIZE];

	/**@brief  Is this the last tag? */
	bool last_tag;
};

/**@brief  Extract information from a block tag.
 * @param  __tag pointer to the block tag
 * @param  tag_bytes block tag size of this jbd filesystem
 * @param  remaining size in buffer containing the block tag
 * @param  tag_info information of this tag.
 * @return  EOK when succeed, otherwise return EINVAL.*/
static int
jbd_extract_block_tag(struct jbd_fs *jbd_fs,
		      void *__tag,
		      int tag_bytes,
		      int32_t remain_buf_size,
		      struct tag_info *tag_info)
{
	char *uuid_start;
	tag_info->tag_bytes = tag_bytes;
	tag_info->uuid_exist = false;
	tag_info->last_tag = false;

	/* See whether it is possible to hold a valid block tag.*/
	if (remain_buf_size - tag_bytes < 0)
		return EINVAL;

	if (JBD_HAS_INCOMPAT_FEATURE(&jbd_fs->sb,
				     JBD_FEATURE_INCOMPAT_CSUM_V3)) {
		struct jbd_block_tag3 *tag = __tag;
		tag_info->block = jbd_get32(tag, blocknr);
		if (JBD_HAS_INCOMPAT_FEATURE(&jbd_fs->sb,
					     JBD_FEATURE_INCOMPAT_64BIT))
			 tag_info->block |=
				 (uint64_t)jbd_get32(tag, blocknr_high) << 32;

		if (jbd_get32(tag, flags) & JBD_FLAG_ESCAPE)
			tag_info->block = 0;

		if (!(jbd_get32(tag, flags) & JBD_FLAG_SAME_UUID)) {
			/* See whether it is possible to hold UUID part.*/
			if (remain_buf_size - tag_bytes < UUID_SIZE)
				return EINVAL;

			uuid_start = (char *)tag + tag_bytes;
			tag_info->uuid_exist = true;
			tag_info->tag_bytes += UUID_SIZE;
			memcpy(tag_info->uuid, uuid_start, UUID_SIZE);
		}

		if (jbd_get32(tag, flags) & JBD_FLAG_LAST_TAG)
			tag_info->last_tag = true;

	} else {
		struct jbd_block_tag *tag = __tag;
		tag_info->block = jbd_get32(tag, blocknr);
		if (JBD_HAS_INCOMPAT_FEATURE(&jbd_fs->sb,
					     JBD_FEATURE_INCOMPAT_64BIT))
			 tag_info->block |=
				 (uint64_t)jbd_get32(tag, blocknr_high) << 32;

		if (jbd_get16(tag, flags) & JBD_FLAG_ESCAPE)
			tag_info->block = 0;

		if (!(jbd_get16(tag, flags) & JBD_FLAG_SAME_UUID)) {
			/* See whether it is possible to hold UUID part.*/
			if (remain_buf_size - tag_bytes < UUID_SIZE)
				return EINVAL;

			uuid_start = (char *)tag + tag_bytes;
			tag_info->uuid_exist = true;
			tag_info->tag_bytes += UUID_SIZE;
			memcpy(tag_info->uuid, uuid_start, UUID_SIZE);
		}

		if (jbd_get16(tag, flags) & JBD_FLAG_LAST_TAG)
			tag_info->last_tag = true;

	}
	return EOK;
}

/**@brief  Write information to a block tag.
 * @param  __tag pointer to the block tag
 * @param  remaining size in buffer containing the block tag
 * @param  tag_info information of this tag.
 * @return  EOK when succeed, otherwise return EINVAL.*/
static int
jbd_write_block_tag(struct jbd_fs *jbd_fs,
		    void *__tag,
		    int32_t remain_buf_size,
		    struct tag_info *tag_info)
{
	char *uuid_start;
	int tag_bytes = jbd_tag_bytes(jbd_fs);

	tag_info->tag_bytes = tag_bytes;

	/* See whether it is possible to hold a valid block tag.*/
	if (remain_buf_size - tag_bytes < 0)
		return EINVAL;

	if (JBD_HAS_INCOMPAT_FEATURE(&jbd_fs->sb,
				     JBD_FEATURE_INCOMPAT_CSUM_V3)) {
		struct jbd_block_tag3 *tag = __tag;
		memset(tag, 0, sizeof(struct jbd_block_tag3));
		jbd_set32(tag, blocknr, tag_info->block);
		if (JBD_HAS_INCOMPAT_FEATURE(&jbd_fs->sb,
					     JBD_FEATURE_INCOMPAT_64BIT))
			jbd_set32(tag, blocknr_high, tag_info->block >> 32);

		if (tag_info->uuid_exist) {
			/* See whether it is possible to hold UUID part.*/
			if (remain_buf_size - tag_bytes < UUID_SIZE)
				return EINVAL;

			uuid_start = (char *)tag + tag_bytes;
			tag_info->tag_bytes += UUID_SIZE;
			memcpy(uuid_start, tag_info->uuid, UUID_SIZE);
		} else
			jbd_set32(tag, flags,
				  jbd_get32(tag, flags) | JBD_FLAG_SAME_UUID);

		if (tag_info->last_tag)
			jbd_set32(tag, flags,
				  jbd_get32(tag, flags) | JBD_FLAG_LAST_TAG);

	} else {
		struct jbd_block_tag *tag = __tag;
		memset(tag, 0, sizeof(struct jbd_block_tag));
		jbd_set32(tag, blocknr, tag_info->block);
		if (JBD_HAS_INCOMPAT_FEATURE(&jbd_fs->sb,
					     JBD_FEATURE_INCOMPAT_64BIT))
			jbd_set32(tag, blocknr_high, tag_info->block >> 32);

		if (tag_info->uuid_exist) {
			/* See whether it is possible to hold UUID part.*/
			if (remain_buf_size - tag_bytes < UUID_SIZE)
				return EINVAL;

			uuid_start = (char *)tag + tag_bytes;
			tag_info->tag_bytes += UUID_SIZE;
			memcpy(uuid_start, tag_info->uuid, UUID_SIZE);
		} else
			jbd_set16(tag, flags,
				  jbd_get16(tag, flags) | JBD_FLAG_SAME_UUID);

		if (tag_info->last_tag)
			jbd_set16(tag, flags,
				  jbd_get16(tag, flags) | JBD_FLAG_LAST_TAG);

	}
	return EOK;
}

/**@brief  Iterate all block tags in a block.
 * @param  jbd_fs jbd filesystem
 * @param  __tag_start pointer to the block
 * @param  tag_tbl_size size of the block
 * @param  func callback routine to indicate that
 *         a block tag is found
 * @param  arg additional argument to be passed to func */
static void
jbd_iterate_block_table(struct jbd_fs *jbd_fs,
			void *__tag_start,
			int32_t tag_tbl_size,
			void (*func)(struct jbd_fs * jbd_fs,
					ext4_fsblk_t block,
					uint8_t *uuid,
					void *arg),
			void *arg)
{
	char *tag_start, *tag_ptr;
	int tag_bytes = jbd_tag_bytes(jbd_fs);
	tag_start = __tag_start;
	tag_ptr = tag_start;

	/* Cut off the size of block tail storing checksum. */
	if (JBD_HAS_INCOMPAT_FEATURE(&jbd_fs->sb,
				     JBD_FEATURE_INCOMPAT_CSUM_V2) ||
	    JBD_HAS_INCOMPAT_FEATURE(&jbd_fs->sb,
				     JBD_FEATURE_INCOMPAT_CSUM_V3))
		tag_tbl_size -= sizeof(struct jbd_block_tail);

	while (tag_tbl_size) {
		struct tag_info tag_info;
		int rc = jbd_extract_block_tag(jbd_fs,
				      tag_ptr,
				      tag_bytes,
				      tag_tbl_size,
				      &tag_info);
		if (rc != EOK)
			break;

		if (func)
			func(jbd_fs, tag_info.block, tag_info.uuid, arg);

		/* Stop the iteration when we reach the last tag. */
		if (tag_info.last_tag)
			break;

		tag_ptr += tag_info.tag_bytes;
		tag_tbl_size -= tag_info.tag_bytes;
	}
}

static void jbd_display_block_tags(struct jbd_fs *jbd_fs,
				   ext4_fsblk_t block,
				   uint8_t *uuid,
				   void *arg)
{
	uint32_t *iblock = arg;
	ext4_dbg(DEBUG_JBD, "Block in block_tag: %" PRIu64 "\n", block);
	(*iblock)++;
	(void)jbd_fs;
	(void)uuid;
	return;
}

static struct revoke_entry *
jbd_revoke_entry_lookup(struct recover_info *info, ext4_fsblk_t block)
{
	struct revoke_entry tmp = {
		.block = block
	};

	return RB_FIND(jbd_revoke, &info->revoke_root, &tmp);
}

/**@brief  Replay a block in a transaction.
 * @param  jbd_fs jbd filesystem
 * @param  block  block address to be replayed.*/
static void jbd_replay_block_tags(struct jbd_fs *jbd_fs,
				  ext4_fsblk_t block,
				  uint8_t *uuid __unused,
				  void *__arg)
{
	int r;
	struct replay_arg *arg = __arg;
	struct recover_info *info = arg->info;
	uint32_t *this_block = arg->this_block;
	struct revoke_entry *revoke_entry;
	struct ext4_block journal_block, ext4_block;
	struct ext4_fs *fs = jbd_fs->inode_ref.fs;

	(*this_block)++;

	/* We replay this block only if the current transaction id
	 * is equal or greater than that in revoke entry.*/
	revoke_entry = jbd_revoke_entry_lookup(info, block);
	if (revoke_entry &&
	    arg->this_trans_id < revoke_entry->trans_id)
		return;

	ext4_dbg(DEBUG_JBD,
		 "Replaying block in block_tag: %" PRIu64 "\n",
		 block);

	r = jbd_block_get(jbd_fs, &journal_block, *this_block);
	if (r != EOK)
		return;

	/* We need special treatment for ext4 superblock. */
	if (block) {
		r = ext4_block_get_noread(fs->bdev, &ext4_block, block);
		if (r != EOK) {
			jbd_block_set(jbd_fs, &journal_block);
			return;
		}

		memcpy(ext4_block.data,
			journal_block.data,
			jbd_get32(&jbd_fs->sb, blocksize));

		ext4_bcache_set_dirty(ext4_block.buf);
		ext4_block_set(fs->bdev, &ext4_block);
	} else {
		uint16_t mount_count, state;
		mount_count = ext4_get16(&fs->sb, mount_count);
		state = ext4_get16(&fs->sb, state);

		memcpy(&fs->sb,
			journal_block.data + EXT4_SUPERBLOCK_OFFSET,
			EXT4_SUPERBLOCK_SIZE);

		/* Mark system as mounted */
		ext4_set16(&fs->sb, state, state);
		r = ext4_sb_write(fs->bdev, &fs->sb);
		if (r != EOK)
			return;

		/*Update mount count*/
		ext4_set16(&fs->sb, mount_count, mount_count);
	}

	jbd_block_set(jbd_fs, &journal_block);
	
	return;
}

/**@brief  Add block address to revoke tree, along with
 *         its transaction id.
 * @param  info  journal replay info
 * @param  block  block address to be replayed.*/
static void jbd_add_revoke_block_tags(struct recover_info *info,
				      ext4_fsblk_t block)
{
	struct revoke_entry *revoke_entry;

	ext4_dbg(DEBUG_JBD, "Add block %" PRIu64 " to revoke tree\n", block);
	/* If the revoke entry with respect to the block address
	 * exists already, update its transaction id.*/
	revoke_entry = jbd_revoke_entry_lookup(info, block);
	if (revoke_entry) {
		revoke_entry->trans_id = info->this_trans_id;
		return;
	}

	revoke_entry = jbd_alloc_revoke_entry();
	ext4_assert(revoke_entry);
	revoke_entry->block = block;
	revoke_entry->trans_id = info->this_trans_id;
	RB_INSERT(jbd_revoke, &info->revoke_root, revoke_entry);

	return;
}

static void jbd_destroy_revoke_tree(struct recover_info *info)
{
	while (!RB_EMPTY(&info->revoke_root)) {
		struct revoke_entry *revoke_entry =
			RB_MIN(jbd_revoke, &info->revoke_root);
		ext4_assert(revoke_entry);
		RB_REMOVE(jbd_revoke, &info->revoke_root, revoke_entry);
		jbd_free_revoke_entry(revoke_entry);
	}
}

/* Make sure we wrap around the log correctly! */
#define wrap(sb, var)						\
do {									\
	if (var >= jbd_get32((sb), maxlen))					\
		var -= (jbd_get32((sb), maxlen) - jbd_get32((sb), first));	\
} while (0)

#define ACTION_SCAN 0
#define ACTION_REVOKE 1
#define ACTION_RECOVER 2

/**@brief  Add entries in a revoke block to revoke tree.
 * @param  jbd_fs jbd filesystem
 * @param  header revoke block header
 * @param  recover_info  journal replay info*/
static void jbd_build_revoke_tree(struct jbd_fs *jbd_fs,
				  struct jbd_bhdr *header,
				  struct recover_info *info)
{
	char *blocks_entry;
	struct jbd_revoke_header *revoke_hdr =
		(struct jbd_revoke_header *)header;
	uint32_t i, nr_entries, record_len = 4;

	/* If we are working on a 64bit jbd filesystem, */
	if (JBD_HAS_INCOMPAT_FEATURE(&jbd_fs->sb,
				     JBD_FEATURE_INCOMPAT_64BIT))
		record_len = 8;

	nr_entries = (jbd_get32(revoke_hdr, count) -
			sizeof(struct jbd_revoke_header)) /
			record_len;

	blocks_entry = (char *)(revoke_hdr + 1);

	for (i = 0;i < nr_entries;i++) {
		if (record_len == 8) {
			uint64_t *blocks =
				(uint64_t *)blocks_entry;
			jbd_add_revoke_block_tags(info, to_be64(*blocks));
		} else {
			uint32_t *blocks =
				(uint32_t *)blocks_entry;
			jbd_add_revoke_block_tags(info, to_be32(*blocks));
		}
		blocks_entry += record_len;
	}
}

static void jbd_debug_descriptor_block(struct jbd_fs *jbd_fs,
				       struct jbd_bhdr *header,
				       uint32_t *iblock)
{
	jbd_iterate_block_table(jbd_fs,
				header + 1,
				jbd_get32(&jbd_fs->sb, blocksize) -
					sizeof(struct jbd_bhdr),
				jbd_display_block_tags,
				iblock);
}

static void jbd_replay_descriptor_block(struct jbd_fs *jbd_fs,
					struct jbd_bhdr *header,
					struct replay_arg *arg)
{
	jbd_iterate_block_table(jbd_fs,
				header + 1,
				jbd_get32(&jbd_fs->sb, blocksize) -
					sizeof(struct jbd_bhdr),
				jbd_replay_block_tags,
				arg);
}

/**@brief  The core routine of journal replay.
 * @param  jbd_fs jbd filesystem
 * @param  recover_info  journal replay info
 * @param  action action needed to be taken
 * @return standard error code*/
static int jbd_iterate_log(struct jbd_fs *jbd_fs,
			   struct recover_info *info,
			   int action)
{
	int r = EOK;
	bool log_end = false;
	struct jbd_sb *sb = &jbd_fs->sb;
	uint32_t start_trans_id, this_trans_id;
	uint32_t start_block, this_block;

	/* We start iterating valid blocks in the whole journal.*/
	start_trans_id = this_trans_id = jbd_get32(sb, sequence);
	start_block = this_block = jbd_get32(sb, start);

	ext4_dbg(DEBUG_JBD, "Start of journal at trans id: %" PRIu32 "\n",
			    start_trans_id);

	while (!log_end) {
		struct ext4_block block;
		struct jbd_bhdr *header;
		/* If we are not scanning for the last
		 * valid transaction in the journal,
		 * we will stop when we reach the end of
		 * the journal.*/
		if (action != ACTION_SCAN)
			if (this_trans_id > info->last_trans_id) {
				log_end = true;
				continue;
			}

		r = jbd_block_get(jbd_fs, &block, this_block);
		if (r != EOK)
			break;

		header = (struct jbd_bhdr *)block.data;
		/* This block does not have a valid magic number,
		 * so we have reached the end of the journal.*/
		if (jbd_get32(header, magic) != JBD_MAGIC_NUMBER) {
			jbd_block_set(jbd_fs, &block);
			log_end = true;
			continue;
		}

		/* If the transaction id we found is not expected,
		 * we may have reached the end of the journal.
		 *
		 * If we are not scanning the journal, something
		 * bad might have taken place. :-( */
		if (jbd_get32(header, sequence) != this_trans_id) {
			if (action != ACTION_SCAN)
				r = EIO;

			jbd_block_set(jbd_fs, &block);
			log_end = true;
			continue;
		}

		switch (jbd_get32(header, blocktype)) {
		case JBD_DESCRIPTOR_BLOCK:
			ext4_dbg(DEBUG_JBD, "Descriptor block: %" PRIu32", "
					    "trans_id: %" PRIu32"\n",
					    this_block, this_trans_id);
			if (action == ACTION_RECOVER) {
				struct replay_arg replay_arg;
				replay_arg.info = info;
				replay_arg.this_block = &this_block;
				replay_arg.this_trans_id = this_trans_id;

				jbd_replay_descriptor_block(jbd_fs,
						header, &replay_arg);
			} else
				jbd_debug_descriptor_block(jbd_fs,
						header, &this_block);

			break;
		case JBD_COMMIT_BLOCK:
			ext4_dbg(DEBUG_JBD, "Commit block: %" PRIu32", "
					    "trans_id: %" PRIu32"\n",
					    this_block, this_trans_id);
			/* This is the end of a transaction,
			 * we may now proceed to the next transaction.
			 */
			this_trans_id++;
			break;
		case JBD_REVOKE_BLOCK:
			ext4_dbg(DEBUG_JBD, "Revoke block: %" PRIu32", "
					    "trans_id: %" PRIu32"\n",
					    this_block, this_trans_id);
			if (action == ACTION_REVOKE) {
				info->this_trans_id = this_trans_id;
				jbd_build_revoke_tree(jbd_fs,
						header, info);
			}
			break;
		default:
			log_end = true;
			break;
		}
		jbd_block_set(jbd_fs, &block);
		this_block++;
		wrap(sb, this_block);
		if (this_block == start_block)
			log_end = true;

	}
	ext4_dbg(DEBUG_JBD, "End of journal.\n");
	if (r == EOK && action == ACTION_SCAN) {
		/* We have finished scanning the journal. */
		info->start_trans_id = start_trans_id;
		if (this_trans_id > start_trans_id)
			info->last_trans_id = this_trans_id - 1;
		else
			info->last_trans_id = this_trans_id;
	}

	return r;
}

/**@brief  Replay journal.
 * @param  jbd_fs jbd filesystem
 * @return standard error code*/
int jbd_recover(struct jbd_fs *jbd_fs)
{
	int r;
	struct recover_info info;
	struct jbd_sb *sb = &jbd_fs->sb;
	if (!sb->start)
		return EOK;

	RB_INIT(&info.revoke_root);

	r = jbd_iterate_log(jbd_fs, &info, ACTION_SCAN);
	if (r != EOK)
		return r;

	r = jbd_iterate_log(jbd_fs, &info, ACTION_REVOKE);
	if (r != EOK)
		return r;

	r = jbd_iterate_log(jbd_fs, &info, ACTION_RECOVER);
	if (r == EOK) {
		/* If we successfully replay the journal,
		 * clear EXT4_FINCOM_RECOVER flag on the
		 * ext4 superblock, and set the start of
		 * journal to 0.*/
		uint32_t features_incompatible =
			ext4_get32(&jbd_fs->inode_ref.fs->sb,
				   features_incompatible);
		jbd_set32(&jbd_fs->sb, start, 0);
		features_incompatible &= ~EXT4_FINCOM_RECOVER;
		ext4_set32(&jbd_fs->inode_ref.fs->sb,
			   features_incompatible,
			   features_incompatible);
		jbd_fs->dirty = true;
		r = ext4_sb_write(jbd_fs->inode_ref.fs->bdev,
				  &jbd_fs->inode_ref.fs->sb);
	}
	jbd_destroy_revoke_tree(&info);
	return r;
}

static void jbd_journal_write_sb(struct jbd_journal *journal)
{
	struct jbd_fs *jbd_fs = journal->jbd_fs;
	jbd_set32(&jbd_fs->sb, start, journal->start);
	jbd_set32(&jbd_fs->sb, sequence, journal->trans_id);
	jbd_fs->dirty = true;
}

/**@brief  Start accessing the journal.
 * @param  jbd_fs jbd filesystem
 * @param  journal current journal session
 * @return standard error code*/
int jbd_journal_start(struct jbd_fs *jbd_fs,
		      struct jbd_journal *journal)
{
	int r;
	uint32_t features_incompatible =
			ext4_get32(&jbd_fs->inode_ref.fs->sb,
				   features_incompatible);
	features_incompatible |= EXT4_FINCOM_RECOVER;
	ext4_set32(&jbd_fs->inode_ref.fs->sb,
			features_incompatible,
			features_incompatible);
	r = ext4_sb_write(jbd_fs->inode_ref.fs->bdev,
			&jbd_fs->inode_ref.fs->sb);
	if (r != EOK)
		return r;

	journal->first = jbd_get32(&jbd_fs->sb, first);
	journal->start = journal->first;
	journal->last = journal->first;
	journal->trans_id = 1;
	journal->alloc_trans_id = 1;

	journal->block_size = jbd_get32(&jbd_fs->sb, blocksize);

	TAILQ_INIT(&journal->trans_queue);
	TAILQ_INIT(&journal->cp_queue);
	RB_INIT(&journal->block_rec_root);
	journal->jbd_fs = jbd_fs;
	jbd_journal_write_sb(journal);
	return jbd_write_sb(jbd_fs);
}

static void jbd_journal_flush_trans(struct jbd_trans *trans)
{
	struct jbd_buf *jbd_buf, *tmp;
	struct jbd_journal *journal = trans->journal;
	struct ext4_fs *fs = journal->jbd_fs->inode_ref.fs;
	LIST_FOREACH_SAFE(jbd_buf, &trans->buf_list, buf_node,
			tmp) {
		struct ext4_block block = jbd_buf->block;
		ext4_block_flush_buf(fs->bdev, block.buf);
	}
}

static void
jbd_journal_skip_pure_revoke(struct jbd_journal *journal,
			     struct jbd_trans *trans)
{
	journal->start = trans->start_iblock +
		trans->alloc_blocks;
	wrap(&journal->jbd_fs->sb, journal->start);
	journal->trans_id = trans->trans_id + 1;
	jbd_journal_free_trans(journal,
			trans, false);
	jbd_journal_write_sb(journal);
}

static void jbd_journal_flush_all_trans(struct jbd_journal *journal)
{
	struct jbd_trans *trans;
	while ((trans = TAILQ_FIRST(&journal->cp_queue))) {
		if (!trans->data_cnt) {
			TAILQ_REMOVE(&journal->cp_queue,
					trans,
					trans_node);
			jbd_journal_skip_pure_revoke(journal, trans);
		} else
			jbd_journal_flush_trans(trans);

	}
}

/**@brief  Stop accessing the journal.
 * @param  journal current journal session
 * @return standard error code*/
int jbd_journal_stop(struct jbd_journal *journal)
{
	int r;
	struct jbd_fs *jbd_fs = journal->jbd_fs;
	uint32_t features_incompatible;

	/* Commit all the transactions to the journal.*/
	jbd_journal_commit_all(journal);

	/* Make sure that journalled content have reached
	 * the disk.*/
	jbd_journal_flush_all_trans(journal);

	/* There should be no block record in this journal
	 * session. */
	if (!RB_EMPTY(&journal->block_rec_root))
		ext4_dbg(DEBUG_JBD,
			 DBG_WARN "There are still block records "
			 	  "in this journal session!\n");

	features_incompatible =
		ext4_get32(&jbd_fs->inode_ref.fs->sb,
			   features_incompatible);
	features_incompatible &= ~EXT4_FINCOM_RECOVER;
	ext4_set32(&jbd_fs->inode_ref.fs->sb,
			features_incompatible,
			features_incompatible);
	r = ext4_sb_write(jbd_fs->inode_ref.fs->bdev,
			&jbd_fs->inode_ref.fs->sb);
	if (r != EOK)
		return r;

	journal->start = 0;
	journal->trans_id = 0;
	jbd_journal_write_sb(journal);
	return jbd_write_sb(journal->jbd_fs);
}

/**@brief  Allocate a block in the journal.
 * @param  journal current journal session
 * @param  trans transaction
 * @return allocated block address*/
static uint32_t jbd_journal_alloc_block(struct jbd_journal *journal,
					struct jbd_trans *trans)
{
	uint32_t start_block;

	start_block = journal->last++;
	trans->alloc_blocks++;
	wrap(&journal->jbd_fs->sb, journal->last);
	
	/* If there is no space left, flush all journalled
	 * blocks to disk first.*/
	if (journal->last == journal->start)
		jbd_journal_flush_all_trans(journal);

	return start_block;
}

/**@brief  Allocate a new transaction
 * @param  journal current journal session
 * @return transaction allocated*/
struct jbd_trans *
jbd_journal_new_trans(struct jbd_journal *journal)
{
	struct jbd_trans *trans = calloc(1, sizeof(struct jbd_trans));
	if (!trans)
		return NULL;

	/* We will assign a trans_id to this transaction,
	 * once it has been committed.*/
	trans->journal = journal;
	trans->error = EOK;
	return trans;
}

static void jbd_trans_end_write(struct ext4_bcache *bc __unused,
			  struct ext4_buf *buf __unused,
			  int res,
			  void *arg);

/**@brief  gain access to it before making any modications.
 * @param  journal current journal session
 * @param  trans transaction
 * @param  block descriptor
 * @return standard error code.*/
int jbd_trans_get_access(struct jbd_journal *journal,
			 struct jbd_trans *trans,
			 struct ext4_block *block)
{
	int r = EOK;
	struct ext4_fs *fs = journal->jbd_fs->inode_ref.fs;
	struct jbd_buf *jbd_buf = block->buf->end_write_arg;

	/* If the buffer has already been modified, we should
	 * flush dirty data in this buffer to disk.*/
	if (ext4_bcache_test_flag(block->buf, BC_DIRTY) &&
	    block->buf->end_write == jbd_trans_end_write) {
		ext4_assert(jbd_buf);
		if (jbd_buf->trans != trans)
			r = ext4_block_flush_buf(fs->bdev, block->buf);

	}
	return r;
}

static struct jbd_block_rec *
jbd_trans_block_rec_lookup(struct jbd_journal *journal,
			   ext4_fsblk_t lba)
{
	struct jbd_block_rec tmp = {
		.lba = lba
	};

	return RB_FIND(jbd_block,
		       &journal->block_rec_root,
		       &tmp);
}

static inline struct jbd_block_rec *
jbd_trans_insert_block_rec(struct jbd_trans *trans,
			   ext4_fsblk_t lba,
			   struct ext4_buf *buf)
{
	struct jbd_block_rec *block_rec;
	block_rec = jbd_trans_block_rec_lookup(trans->journal, lba);
	if (block_rec) {
		/* Data should be flushed to disk already. */
		ext4_assert(!block_rec->buf);
		/* Now this block record belongs to this transaction. */
		block_rec->trans = trans;
		return block_rec;
	}
	block_rec = calloc(1, sizeof(struct jbd_block_rec));
	if (!block_rec)
		return NULL;

	block_rec->lba = lba;
	block_rec->buf = buf;
	block_rec->trans = trans;
	RB_INSERT(jbd_block, &trans->journal->block_rec_root, block_rec);
	return block_rec;
}

static inline void
jbd_trans_remove_block_rec(struct jbd_journal *journal,
			   struct jbd_buf *jbd_buf)
{
	struct jbd_block_rec *block_rec = jbd_buf->block_rec;
	/* If this block record doesn't belong to this transaction,
	 * give up.*/
	if (block_rec->trans == jbd_buf->trans) {
		RB_REMOVE(jbd_block,
				&journal->block_rec_root,
				block_rec);
		free(block_rec);
	}
}

/**@brief  Add block to a transaction and mark it dirty.
 * @param  trans transaction
 * @param  block block descriptor
 * @return standard error code*/
int jbd_trans_set_block_dirty(struct jbd_trans *trans,
			      struct ext4_block *block)
{
	struct jbd_buf *buf;

	if (!ext4_bcache_test_flag(block->buf, BC_DIRTY) &&
	    block->buf->end_write != jbd_trans_end_write) {
		struct jbd_block_rec *block_rec;
		buf = calloc(1, sizeof(struct jbd_buf));
		if (!buf)
			return ENOMEM;

		if ((block_rec = jbd_trans_insert_block_rec(trans,
					block->lb_id,
					block->buf)) == NULL) {
			free(buf);
			return ENOMEM;
		}

		buf->block_rec = block_rec;
		buf->trans = trans;
		buf->block = *block;
		ext4_bcache_inc_ref(block->buf);

		/* If the content reach the disk, notify us
		 * so that we may do a checkpoint. */
		block->buf->end_write = jbd_trans_end_write;
		block->buf->end_write_arg = buf;

		trans->data_cnt++;
		LIST_INSERT_HEAD(&trans->buf_list, buf, buf_node);

		ext4_bcache_set_dirty(block->buf);
	}
	return EOK;
}

/**@brief  Add block to be revoked to a transaction
 * @param  trans transaction
 * @param  lba logical block address
 * @return standard error code*/
int jbd_trans_revoke_block(struct jbd_trans *trans,
			   ext4_fsblk_t lba)
{
	struct jbd_revoke_rec *rec =
		calloc(1, sizeof(struct jbd_revoke_rec));
	if (!rec)
		return ENOMEM;

	rec->lba = lba;
	LIST_INSERT_HEAD(&trans->revoke_list, rec, revoke_node);
	return EOK;
}

/**@brief  Try to add block to be revoked to a transaction.
 *         If @lba still remains in an transaction on checkpoint
 *         queue, add @lba as a revoked block to the transaction.
 * @param  trans transaction
 * @param  lba logical block address
 * @return standard error code*/
int jbd_trans_try_revoke_block(struct jbd_trans *trans,
			       ext4_fsblk_t lba)
{
	int r = EOK;
	struct jbd_journal *journal = trans->journal;
	struct ext4_fs *fs = journal->jbd_fs->inode_ref.fs;
	struct jbd_block_rec *block_rec =
		jbd_trans_block_rec_lookup(journal, lba);

	/* Make sure we don't flush any buffers belong to this transaction. */
	if (block_rec && block_rec->trans != trans) {
		/* If the buffer has not been flushed yet, flush it now. */
		if (block_rec->buf) {
			r = ext4_block_flush_buf(fs->bdev, block_rec->buf);
			if (r != EOK)
				return r;

		}

		jbd_trans_revoke_block(trans, lba);
	}

	return EOK;
}

/**@brief  Free a transaction
 * @param  journal current journal session
 * @param  trans transaction
 * @param  abort discard all the modifications on the block?
 * @return standard error code*/
void jbd_journal_free_trans(struct jbd_journal *journal,
			    struct jbd_trans *trans,
			    bool abort)
{
	struct jbd_buf *jbd_buf, *tmp;
	struct jbd_revoke_rec *rec, *tmp2;
	struct ext4_fs *fs = journal->jbd_fs->inode_ref.fs;
	LIST_FOREACH_SAFE(jbd_buf, &trans->buf_list, buf_node,
			  tmp) {
		if (abort) {
			jbd_buf->block.buf->end_write = NULL;
			jbd_buf->block.buf->end_write_arg = NULL;
			ext4_bcache_clear_dirty(jbd_buf->block.buf);
			ext4_block_set(fs->bdev, &jbd_buf->block);
		}

		jbd_trans_remove_block_rec(journal, jbd_buf);
		LIST_REMOVE(jbd_buf, buf_node);
		free(jbd_buf);
	}
	LIST_FOREACH_SAFE(rec, &trans->revoke_list, revoke_node,
			  tmp2) {
		LIST_REMOVE(rec, revoke_node);
		free(rec);
	}

	free(trans);
}

/**@brief  Write commit block for a transaction
 * @param  trans transaction
 * @return standard error code*/
static int jbd_trans_write_commit_block(struct jbd_trans *trans)
{
	int rc;
	struct jbd_commit_header *header;
	uint32_t commit_iblock = 0;
	struct ext4_block commit_block;
	struct jbd_journal *journal = trans->journal;

	commit_iblock = jbd_journal_alloc_block(journal, trans);
	rc = jbd_block_get_noread(journal->jbd_fs,
			&commit_block, commit_iblock);
	if (rc != EOK)
		return rc;

	header = (struct jbd_commit_header *)commit_block.data;
	jbd_set32(&header->header, magic, JBD_MAGIC_NUMBER);
	jbd_set32(&header->header, blocktype, JBD_COMMIT_BLOCK);
	jbd_set32(&header->header, sequence, trans->trans_id);

	ext4_bcache_set_dirty(commit_block.buf);
	rc = jbd_block_set(journal->jbd_fs, &commit_block);
	if (rc != EOK)
		return rc;

	return EOK;
}

/**@brief  Write descriptor block for a transaction
 * @param  journal current journal session
 * @param  trans transaction
 * @return standard error code*/
static int jbd_journal_prepare(struct jbd_journal *journal,
			       struct jbd_trans *trans)
{
	int rc = EOK, i = 0;
	int32_t tag_tbl_size;
	uint32_t desc_iblock = 0;
	uint32_t data_iblock = 0;
	char *tag_start = NULL, *tag_ptr = NULL;
	struct jbd_buf *jbd_buf, *tmp;
	struct ext4_block desc_block, data_block;
	struct ext4_fs *fs = journal->jbd_fs->inode_ref.fs;

	LIST_FOREACH_SAFE(jbd_buf, &trans->buf_list, buf_node, tmp) {
		struct tag_info tag_info;
		bool uuid_exist = false;
		if (!ext4_bcache_test_flag(jbd_buf->block.buf,
					   BC_DIRTY)) {
			/* The buffer has not been modified, just release
			 * that jbd_buf. */
			jbd_buf->block.buf->end_write = NULL;
			jbd_buf->block.buf->end_write_arg = NULL;
			ext4_block_set(fs->bdev, &jbd_buf->block);
			LIST_REMOVE(jbd_buf, buf_node);
			free(jbd_buf);
			continue;
		}
again:
		if (!desc_iblock) {
			struct jbd_bhdr *bhdr;
			desc_iblock = jbd_journal_alloc_block(journal, trans);
			rc = jbd_block_get_noread(journal->jbd_fs,
					   &desc_block, desc_iblock);
			if (rc != EOK)
				break;

			ext4_bcache_set_dirty(desc_block.buf);

			bhdr = (struct jbd_bhdr *)desc_block.data;
			jbd_set32(bhdr, magic, JBD_MAGIC_NUMBER);
			jbd_set32(bhdr, blocktype, JBD_DESCRIPTOR_BLOCK);
			jbd_set32(bhdr, sequence, trans->trans_id);

			tag_start = (char *)(bhdr + 1);
			tag_ptr = tag_start;
			uuid_exist = true;
			tag_tbl_size = journal->block_size -
				sizeof(struct jbd_bhdr);

			if (!trans->start_iblock)
				trans->start_iblock = desc_iblock;

		}
		tag_info.block = jbd_buf->block.lb_id;
		tag_info.uuid_exist = uuid_exist;
		if (i == trans->data_cnt - 1)
			tag_info.last_tag = true;
		else
			tag_info.last_tag = false;

		if (uuid_exist)
			memcpy(tag_info.uuid, journal->jbd_fs->sb.uuid,
					UUID_SIZE);

		rc = jbd_write_block_tag(journal->jbd_fs,
				tag_ptr,
				tag_tbl_size,
				&tag_info);
		if (rc != EOK) {
			jbd_block_set(journal->jbd_fs, &desc_block);
			desc_iblock = 0;
			goto again;
		}

		data_iblock = jbd_journal_alloc_block(journal, trans);
		rc = jbd_block_get_noread(journal->jbd_fs,
				&data_block, data_iblock);
		if (rc != EOK)
			break;

		ext4_bcache_set_dirty(data_block.buf);

		memcpy(data_block.data, jbd_buf->block.data,
			journal->block_size);

		rc = jbd_block_set(journal->jbd_fs, &data_block);
		if (rc != EOK)
			break;

		tag_ptr += tag_info.tag_bytes;
		tag_tbl_size -= tag_info.tag_bytes;

		i++;
	}
	if (rc == EOK && desc_iblock)
		jbd_block_set(journal->jbd_fs, &desc_block);

	return rc;
}

/**@brief  Write revoke block for a transaction
 * @param  journal current journal session
 * @param  trans transaction
 * @return standard error code*/
static int
jbd_journal_prepare_revoke(struct jbd_journal *journal,
			   struct jbd_trans *trans)
{
	int rc = EOK, i = 0;
	int32_t tag_tbl_size;
	uint32_t desc_iblock = 0;
	char *blocks_entry = NULL;
	struct jbd_revoke_rec *rec, *tmp;
	struct ext4_block desc_block;
	struct jbd_revoke_header *header = NULL;
	int32_t record_len = 4;

	if (JBD_HAS_INCOMPAT_FEATURE(&journal->jbd_fs->sb,
				     JBD_FEATURE_INCOMPAT_64BIT))
		record_len = 8;

	LIST_FOREACH_SAFE(rec, &trans->revoke_list, revoke_node,
			  tmp) {
again:
		if (!desc_iblock) {
			struct jbd_bhdr *bhdr;
			desc_iblock = jbd_journal_alloc_block(journal, trans);
			rc = jbd_block_get_noread(journal->jbd_fs,
					   &desc_block, desc_iblock);
			if (rc != EOK) {
				break;
			}

			ext4_bcache_set_dirty(desc_block.buf);

			bhdr = (struct jbd_bhdr *)desc_block.data;
			jbd_set32(bhdr, magic, JBD_MAGIC_NUMBER);
			jbd_set32(bhdr, blocktype, JBD_REVOKE_BLOCK);
			jbd_set32(bhdr, sequence, trans->trans_id);
			
			header = (struct jbd_revoke_header *)bhdr;
			blocks_entry = (char *)(header + 1);
			tag_tbl_size = journal->block_size -
				sizeof(struct jbd_revoke_header);

			if (!trans->start_iblock)
				trans->start_iblock = desc_iblock;

		}

		if (tag_tbl_size < record_len) {
			jbd_set32(header, count,
				  journal->block_size - tag_tbl_size);
			jbd_block_set(journal->jbd_fs, &desc_block);
			desc_iblock = 0;
			header = NULL;
			goto again;
		}
		if (record_len == 8) {
			uint64_t *blocks =
				(uint64_t *)blocks_entry;
			*blocks = to_be64(rec->lba);
		} else {
			uint32_t *blocks =
				(uint32_t *)blocks_entry;
			*blocks = to_be32(rec->lba);
		}
		blocks_entry += record_len;
		tag_tbl_size -= record_len;

		i++;
	}
	if (rc == EOK && desc_iblock) {
		if (header != NULL)
			jbd_set32(header, count,
				  journal->block_size - tag_tbl_size);

		jbd_block_set(journal->jbd_fs, &desc_block);
	}

	return rc;
}

/**@brief  Submit the transaction to transaction queue.
 * @param  journal current journal session
 * @param  trans transaction*/
void
jbd_journal_submit_trans(struct jbd_journal *journal,
			 struct jbd_trans *trans)
{
	TAILQ_INSERT_TAIL(&journal->trans_queue,
			  trans,
			  trans_node);
}

/**@brief  Put references of block descriptors in a transaction.
 * @param  journal current journal session
 * @param  trans transaction*/
void jbd_journal_cp_trans(struct jbd_journal *journal, struct jbd_trans *trans)
{
	struct jbd_buf *jbd_buf, *tmp;
	struct ext4_fs *fs = journal->jbd_fs->inode_ref.fs;
	LIST_FOREACH_SAFE(jbd_buf, &trans->buf_list, buf_node,
			tmp) {
		struct ext4_block block = jbd_buf->block;
		ext4_block_set(fs->bdev, &block);
	}
}

/**@brief  Update the start block of the journal when
 *         all the contents in a transaction reach the disk.*/
static void jbd_trans_end_write(struct ext4_bcache *bc __unused,
			  struct ext4_buf *buf,
			  int res,
			  void *arg)
{
	struct jbd_buf *jbd_buf = arg;
	struct jbd_trans *trans = jbd_buf->trans;
	struct jbd_journal *journal = trans->journal;
	bool first_in_queue =
		trans == TAILQ_FIRST(&journal->cp_queue);
	if (res != EOK)
		trans->error = res;

	LIST_REMOVE(jbd_buf, buf_node);
	jbd_buf->block_rec->buf = NULL;
	jbd_trans_remove_block_rec(journal, jbd_buf);
	free(jbd_buf);

	/* Clear the end_write and end_write_arg fields. */
	buf->end_write = NULL;
	buf->end_write_arg = NULL;

	trans->written_cnt++;
	if (trans->written_cnt == trans->data_cnt) {
		TAILQ_REMOVE(&journal->cp_queue, trans, trans_node);

		if (first_in_queue) {
			journal->start = trans->start_iblock +
				trans->alloc_blocks;
			wrap(&journal->jbd_fs->sb, journal->start);
			journal->trans_id = trans->trans_id + 1;
		}
		jbd_journal_free_trans(journal, trans, false);

		if (first_in_queue) {
			while ((trans = TAILQ_FIRST(&journal->cp_queue))) {
				if (!trans->data_cnt) {
					TAILQ_REMOVE(&journal->cp_queue,
						     trans,
						     trans_node);
					jbd_journal_skip_pure_revoke(journal,
								     trans);
				} else {
					journal->start = trans->start_iblock;
					wrap(&journal->jbd_fs->sb, journal->start);
					journal->trans_id = trans->trans_id;
					break;
				}
			}
			jbd_journal_write_sb(journal);
			jbd_write_sb(journal->jbd_fs);
		}
	}
}

/**@brief  Commit a transaction to the journal immediately.
 * @param  journal current journal session
 * @param  trans transaction
 * @return standard error code*/
int jbd_journal_commit_trans(struct jbd_journal *journal,
			     struct jbd_trans *trans)
{
	int rc = EOK;
	uint32_t last = journal->last;

	trans->trans_id = journal->alloc_trans_id;
	rc = jbd_journal_prepare(journal, trans);
	if (rc != EOK)
		goto Finish;

	rc = jbd_journal_prepare_revoke(journal, trans);
	if (rc != EOK)
		goto Finish;

	if (LIST_EMPTY(&trans->buf_list) &&
	    LIST_EMPTY(&trans->revoke_list)) {
		/* Since there are no entries in both buffer list
		 * and revoke entry list, we do not consider trans as
		 * complete transaction and just return EOK.*/
		jbd_journal_free_trans(journal, trans, false);
		goto Finish;
	}

	rc = jbd_trans_write_commit_block(trans);
	if (rc != EOK)
		goto Finish;

	journal->alloc_trans_id++;
	if (TAILQ_EMPTY(&journal->cp_queue)) {
		if (trans->data_cnt) {
			journal->start = trans->start_iblock;
			wrap(&journal->jbd_fs->sb, journal->start);
			journal->trans_id = trans->trans_id;
			jbd_journal_write_sb(journal);
			jbd_write_sb(journal->jbd_fs);
			TAILQ_INSERT_TAIL(&journal->cp_queue, trans,
					trans_node);
			jbd_journal_cp_trans(journal, trans);
		} else {
			journal->start = trans->start_iblock +
				trans->alloc_blocks;
			wrap(&journal->jbd_fs->sb, journal->start);
			journal->trans_id = trans->trans_id + 1;
			jbd_journal_write_sb(journal);
			jbd_journal_free_trans(journal, trans, false);
		}
	} else {
		TAILQ_INSERT_TAIL(&journal->cp_queue, trans,
				trans_node);
		if (trans->data_cnt)
			jbd_journal_cp_trans(journal, trans);

	}
Finish:
	if (rc != EOK) {
		journal->last = last;
		jbd_journal_free_trans(journal, trans, true);
	}
	return rc;
}

/**@brief  Commit one transaction on transaction queue
 *         to the journal.
 * @param  journal current journal session.*/
void jbd_journal_commit_one(struct jbd_journal *journal)
{
	struct jbd_trans *trans;

	if ((trans = TAILQ_FIRST(&journal->trans_queue))) {
		TAILQ_REMOVE(&journal->trans_queue, trans, trans_node);
		jbd_journal_commit_trans(journal, trans);
	}
}

/**@brief  Commit all the transactions on transaction queue
 *         to the journal.
 * @param  journal current journal session.*/
void jbd_journal_commit_all(struct jbd_journal *journal)
{
	while (!TAILQ_EMPTY(&journal->trans_queue)) {
		jbd_journal_commit_one(journal);
	}
}

/**
 * @}
 */
