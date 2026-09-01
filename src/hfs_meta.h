#ifndef HFS_META_H
#define HFS_META_H

#include <stdint.h>

#include "hfs.h"

/* Magic value stored in the super block ('HFSB'). */
#define HFS_SUPER_MAGIC	0x48465342U
/* Super block format version. */
#define HFS_SUPER_VERSION	1U
/* Maximum number of inodes held in the in-memory table. */
#define HFS_MAX_INODES	65536U

/* Inode types. */
#define HFS_INODE_DIR	1
#define HFS_INODE_FILE	2

/*
 * Super block: persisted in the blobstore super blob.
 * Points to the metadata blob which stores the serialized inode table.
 */
struct hfs_super_block {
	uint64_t magic;		/* HFS_SUPER_MAGIC, detects unformatted storage */
	uint32_t version;	/* HFS_SUPER_VERSION */
	uint64_t metaBlobId;	/* blob id holding the serialized inode table */
	uint64_t rootInodeId;	/* inode id of the root directory */
	uint32_t checksum;	/* simple checksum of the super block */
};

/*
 * Inode: one entry per directory or file.
 * Directories and files share one flat table; hierarchy is expressed
 * through parentId.
 */
struct hfs_inode {
	uint64_t id;		/* unique inode number */
	uint8_t type;		/* HFS_INODE_DIR or HFS_INODE_FILE */
	uint64_t parentId;	/* inode id of the parent directory */
	uint64_t blobId;	/* data blob (file) or metadata blob (dir) */
	uint64_t size;		/* file size in bytes */
	uint64_t mtimeNs;	/* last modification time, ns since epoch */
	uint64_t ctimeNs;	/* creation time, ns since epoch */
	char name[FILENAME_LENGTH];	/* name within the parent directory */
};

/*
 * Load or format the metadata: allocates the inode table, then either
 * reads it back from the metadata blob or formats a fresh one.
 * Returns 0 on success, -1 on failure.
 */
int hfsMetaInit(Hfs *fs);

/* Read the super blob id from the blobstore. */
int hfsMetaLoad(Hfs *fs);

/* Create the metadata blob and register it as the blobstore super blob. */
int hfsMetaFormat(Hfs *fs);

/* Persist the super block (currently a no-op stub). */
int hfsMetaSave(Hfs *fs);

/* Serialize the in-memory inode table into the metadata blob. */
int hfsMetaSaveAll(Hfs *fs);

/* Read the metadata blob and rebuild the in-memory inode table. */
int hfsMetaLoadAll(Hfs *fs);

/* Append a zeroed inode to the table and return it, or NULL if full. */
struct hfs_inode *hfsMetaAddInode(Hfs *fs);

/* Look up an inode by (parentId, name), or NULL if not found. */
struct hfs_inode *hfsMetaFindByName(Hfs *fs, uint64_t parentId, const char *name);

/* Look up an inode by its id, or NULL if not found. */
struct hfs_inode *hfsMetaFindById(Hfs *fs, uint64_t id);

/* Root directory inode id (created at format time). */
#define HFS_ROOT_INODE_ID	1

/*
 * Resolve an absolute path (e.g. "/hfs/a/b.txt") into its inode by
 * walking each component with findByName. Returns the inode or NULL;
 * on error errno is set to ENOENT (missing component) or ENOTDIR
 * (an intermediate component is not a directory).
 */
struct hfs_inode *hfsMetaResolvePath(Hfs *fs, const char *path);

/*
 * Split an absolute path into its parent directory inode and the leaf
 * name. "*leafName" points into 'path' (or a static buffer for "/").
 * Returns -1 and sets errno if the parent directory does not exist.
 */
int hfsMetaResolveParent(Hfs *fs, const char *path,
			 struct hfs_inode **parent, const char **leafName);

/*
 * Create a new directory inode under 'parent' with the given name.
 * Persists the inode table. Fails with EEXIST if the name is taken.
 */
int hfsMetaMkdir(Hfs *fs, struct hfs_inode *parent, const char *name);

/*
 * Remove an empty directory resolved from 'path'.
 * Fails with ENOTEMPTY if it still contains children.
 */
int hfsMetaRmdir(Hfs *fs, const char *path);

/*
 * Create a new file: allocate a data blob, add a FILE inode under
 * 'parent' and persist the table. Fails with EEXIST if the name is taken.
 */
int hfsMetaCreateFile(Hfs *fs, struct hfs_inode *parent, const char *name);

/*
 * Remove a file inode from the table without touching the data blob.
 * The removed blob id is returned via '*blobId' for later deletion.
 * Fails with EISDIR if the name refers to a directory.
 */
int hfsMetaRemoveFile(Hfs *fs, struct hfs_inode *parent, const char *name,
		      uint64_t *blobId);

/*
 * Rename 'oldPath' to 'newPath' (same or different directory).
 * POSIX semantics: a plain-file target is overwritten; a directory
 * target must be empty. Persists the table once (atomic).
 */
int hfsMetaRename(Hfs *fs, const char *oldPath, const char *newPath);

#endif /* HFS_META_H */