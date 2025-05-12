#include <linux/module.h>       /* Needed by all modules */
#include <linux/kernel.h>       /* Needed for KERN_INFO, KERN_ERR */
#include <linux/init.h>         /* Needed for the macros */
#include <linux/fs.h>           /* libfs stuff           */
#include <linux/buffer_head.h>  /* buffer_head           */
#include <linux/slab.h>         /* kmalloc, kfree        */
#include <linux/timekeeping.h>  /* current_time          */
#include <linux/stat.h>         /* S_IFDIR, S_IFREG constants */
#include <linux/string.h>       /* strcmp                */
#include <linux/uaccess.h>      /* copy_to_user, copy_from_user */
#include <linux/sched.h>        /* current task struct */
#include <linux/mount.h>        /* mnt_idmap needed for inode_init_owner */

#include "assoofs.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Based on ULE Document"); // Optional: Add your name if you modify it

/*
 * Helper Function Prototypes
 */
static struct assoofs_inode_info *assoofs_get_inode_info(struct super_block *sb, uint64_t inode_no);
static struct inode *assoofs_get_inode(struct super_block *sb, int ino);
static int assoofs_sb_get_a_freeinode(struct super_block *sb, unsigned long *inode);
static int assoofs_sb_get_a_freeblock(struct super_block *sb, uint64_t *block);
static void assoofs_save_sb_info(struct super_block *vsb);
static void assoofs_add_inode_info(struct super_block *sb, struct assoofs_inode_info *inode);
static int assoofs_save_inode_info(struct super_block *sb, struct assoofs_inode_info *inode_info);
static struct assoofs_inode_info *assoofs_search_inode_info(struct super_block *sb, struct assoofs_inode_info *start, struct assoofs_inode_info *search);
static int assoofs_sb_set_a_freeinode(struct super_block *sb, uint64_t inode_no);
static int assoofs_sb_set_a_freeblock(struct super_block *sb, uint64_t block);

/*
 * VFS Function Prototypes (already defined in skeleton, kept for clarity)
 */
static struct dentry *assoofs_mount(struct file_system_type *fs_type, int flags, const char *dev_name, void *data);
int assoofs_fill_super(struct super_block *sb, void *data, int silent);
ssize_t assoofs_read(struct file * filp, char __user * buf, size_t len, loff_t * ppos);
ssize_t assoofs_write(struct file * filp, const char __user * buf, size_t len, loff_t * ppos);
static int assoofs_iterate(struct file *filp, struct dir_context *ctx);
static int assoofs_create(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, umode_t mode, bool excl);
struct dentry *assoofs_lookup(struct inode *parent_inode, struct dentry *child_dentry, unsigned int flags);
static int assoofs_mkdir(struct mnt_idmap *idmap, struct inode *dir , struct dentry *dentry, umode_t mode);
static int assoofs_remove(struct inode *dir, struct dentry *dentry);

/*
 *  Estructuras de datos necesarias
 */

// Definicion del tipo de sistema de archivos assoofs
static struct file_system_type assoofs_type = {
    .owner   = THIS_MODULE,
    .name    = "assoofs",
    .mount   = assoofs_mount,
    .kill_sb = kill_block_super,
};

// Operaciones sobre ficheros
const struct file_operations assoofs_file_operations = {
    .read = assoofs_read,
    .write = assoofs_write,
    // .llseek = generic_file_llseek, // Optional: Add if needed, standard implementation
};

// Operaciones sobre directorios
const struct file_operations assoofs_dir_operations = {
    .owner = THIS_MODULE,
    .iterate_shared = assoofs_iterate, //MODIFICADO MIGRACION
    // .llseek = generic_file_llseek, // Optional: Add if needed, standard implementation
};

// Operaciones sobre inodos
static struct inode_operations assoofs_inode_ops = {
    .create = assoofs_create,
    .lookup = assoofs_lookup,
    .mkdir = assoofs_mkdir,
    .unlink = assoofs_remove, // unlink calls assoofs_remove for files
    .rmdir = assoofs_remove, // rmdir calls assoofs_remove for directories
};

// Operaciones sobre el superbloque
static const struct super_operations assoofs_sops = {
    .drop_inode = generic_delete_inode, // Use generic for basic cleanup
    // Add .put_super if specific cleanup needed beyond kill_block_super
    // Add .statfs if needed
};


/*
 *  Funciones que realizan operaciones sobre ficheros
 */

ssize_t assoofs_read(struct file * filp, char __user * buf, size_t len, loff_t * ppos) {
    struct assoofs_inode_info *inode_info = filp->f_path.dentry->d_inode->i_private;
    struct buffer_head *bh;
    struct super_block *sb = filp->f_path.dentry->d_inode->i_sb;
    char *buffer;
    int nbytes;

    printk(KERN_INFO "ASSOOFS: Read request for inode %llu, pos %lld, len %zu\n",
           inode_info->inode_no, *ppos, len);

    // Check if position is beyond file size
    if (*ppos >= inode_info->file_size) {
        printk(KERN_INFO "ASSOOFS: Read: Reached EOF for inode %llu\n", inode_info->inode_no);
        return 0; // End of File
    }

    bh = sb_bread(sb, inode_info->data_block_number);
    if (!bh) {
        printk(KERN_ERR "ASSOOFS: Read: Error reading block %llu from device\n", inode_info->data_block_number);
        return -EIO;
    }
    buffer = (char *)bh->b_data;

    // Calculate bytes to read
    nbytes = min(len, (size_t)(inode_info->file_size - *ppos));

    // Copy data to user space
    if (copy_to_user(buf, buffer + *ppos, nbytes)) {
        brelse(bh);
        printk(KERN_ERR "ASSOOFS: Read: Error copying data to user space\n");
        return -EFAULT;
    }

    // Update position
    *ppos += nbytes;

    brelse(bh);

    printk(KERN_INFO "ASSOOFS: Read: Copied %d bytes for inode %llu\n", nbytes, inode_info->inode_no);
    return nbytes;
}

ssize_t assoofs_write(struct file * filp, const char __user * buf, size_t len, loff_t * ppos) {
    struct inode *inode = filp->f_path.dentry->d_inode;
    struct assoofs_inode_info *inode_info = inode->i_private;
    struct super_block *sb = inode->i_sb;
    struct buffer_head *bh;
    char *buffer;

    printk(KERN_INFO "ASSOOFS: Write request for inode %llu, pos %lld, len %zu\n",
           inode_info->inode_no, *ppos, len);

    // Check if write exceeds block size limit
    if (*ppos + len > ASSOOFS_DEFAULT_BLOCK_SIZE) {
        printk(KERN_ERR "ASSOOFS: Write: Attempt to write past block boundary on inode %llu\n", inode_info->inode_no);
        return -ENOSPC; // No space left on device (simplification: only one block per file)
    }

    bh = sb_bread(sb, inode_info->data_block_number);
    if (!bh) {
        printk(KERN_ERR "ASSOOFS: Write: Error reading block %llu from device\n", inode_info->data_block_number);
        return -EIO;
    }
    buffer = (char *)bh->b_data;

    // Copy data from user space
    if (copy_from_user(buffer + *ppos, buf, len)) {
        brelse(bh);
        printk(KERN_ERR "ASSOOFS: Write: Error copying data from user space\n");
        return -EFAULT;
    }

    // Mark buffer dirty and sync
    mark_buffer_dirty(bh);
    sync_dirty_buffer(bh); // Write immediately for simplicity

    // Update position and persistent file size
    *ppos += len;
    if (*ppos > inode_info->file_size) {
        inode_info->file_size = *ppos;
        if (assoofs_save_inode_info(sb, inode_info) != 0) {
             printk(KERN_ERR "ASSOOFS: Write: Failed to save updated inode info for inode %llu\n", inode_info->inode_no);
             // Continue, but data size on disk might be inconsistent
        }
    }
    // Update inode timestamp
    inode_set_mtime_to_ts(inode, current_time(inode)); // MODIFICADO MIGRACION
    mark_inode_dirty(inode);


    brelse(bh);

    printk(KERN_INFO "ASSOOFS: Write: Wrote %zu bytes for inode %llu, new size %llu\n",
           len, inode_info->inode_no, inode_info->file_size);
    return len;
}

/*
 *  Funciones que realizan operaciones sobre directorios
 */

static int assoofs_iterate(struct file *filp, struct dir_context *ctx) {
    struct inode *inode = file_inode(filp);
    struct super_block *sb = inode->i_sb;
    struct assoofs_inode_info *inode_info = inode->i_private;
    struct buffer_head *bh;
    struct assoofs_dir_record_entry *record;
    int i;

    printk(KERN_INFO "ASSOOFS: Iterate request for inode %lu, pos %lld\n", inode->i_ino, ctx->pos);

    // Check if already iterated
    if (ctx->pos) {
        // We have filled the buffer once, subsequent calls depend on ctx->pos logic
        // For simplicity, ASSOOFS assumes one block directories, so we are done.
        printk(KERN_INFO "ASSOOFS: Iterate: Already iterated or assumes single block dir, pos > 0.\n");
        return 0;
    }

    // Check if it's a directory
    if (!S_ISDIR(inode_info->mode)) {
        printk(KERN_ERR "ASSOOFS: Iterate: Inode %lu is not a directory\n", inode->i_ino);
        return -ENOTDIR;
    }

    bh = sb_bread(sb, inode_info->data_block_number);
    if (!bh) {
        printk(KERN_ERR "ASSOOFS: Iterate: Error reading block %llu for directory inode %lu\n",
               inode_info->data_block_number, inode->i_ino);
        return -EIO;
    }
    record = (struct assoofs_dir_record_entry *)bh->b_data;

    // Iterate through directory entries
    for (i = 0; i < inode_info->dir_children_count; i++) {
        if (record->entry_removed == ASSOOFS_FALSE) {
             printk(KERN_INFO "ASSOOFS: Iterate: Emitting entry '%.*s' with inode %llu at pos %lld\n",
                   ASSOOFS_FILENAME_MAXLEN, record->filename, record->inode_no, ctx->pos);

            if (!dir_emit(ctx, record->filename, strnlen(record->filename, ASSOOFS_FILENAME_MAXLEN), record->inode_no, DT_UNKNOWN)) {
                brelse(bh);
                printk(KERN_WARNING "ASSOOFS: Iterate: dir_emit failed, buffer likely full.\n");
                return 0; // Buffer full, stop iteration
            }
             // ctx->pos is managed by dir_emit, do not manually increment by sizeof here
             ctx->pos++; // Increment logical position
        } else {
             printk(KERN_INFO "ASSOOFS: Iterate: Skipping removed entry at index %d\n", i);
        }
        record++; // Move to the next record
    }

    brelse(bh);
    printk(KERN_INFO "ASSOOFS: Iterate: Finished iterating directory inode %lu\n", inode->i_ino);
    return 0;
}

/*
 *  Funciones que realizan operaciones sobre inodos
 */
struct dentry *assoofs_lookup(struct inode *parent_inode, struct dentry *child_dentry, unsigned int flags) {
    struct assoofs_inode_info *parent_info = parent_inode->i_private;
    struct super_block *sb = parent_inode->i_sb;
    struct buffer_head *bh;
    struct assoofs_dir_record_entry *record;
    int i;
    struct inode *inode = NULL; // Initialize to NULL

    printk(KERN_INFO "ASSOOFS: Lookup request for name '%s' in parent inode %lu\n",
           child_dentry->d_name.name, parent_inode->i_ino);

    bh = sb_bread(sb, parent_info->data_block_number);
    if (!bh) {
        printk(KERN_ERR "ASSOOFS: Lookup: Error reading block %llu for parent inode %lu\n",
               parent_info->data_block_number, parent_inode->i_ino);
        return ERR_PTR(-EIO);
    }
    record = (struct assoofs_dir_record_entry *)bh->b_data;

    // Search for the entry in the directory block
    for (i = 0; i < parent_info->dir_children_count; i++) {
        if (record->entry_removed == ASSOOFS_FALSE &&
            strcmp(record->filename, child_dentry->d_name.name) == 0) {

            printk(KERN_INFO "ASSOOFS: Lookup: Found entry '%s' pointing to inode %llu\n",
                   record->filename, record->inode_no);

            // Get the inode for the found entry
            inode = assoofs_get_inode(sb, record->inode_no);
            if (IS_ERR(inode)) {
                brelse(bh);
                printk(KERN_ERR "ASSOOFS: Lookup: Failed to get inode %llu\n", record->inode_no);
                return ERR_CAST(inode); // Propagate error from assoofs_get_inode
            }

            // Add the found inode to the dentry cache
            d_add(child_dentry, inode); // Deprecated, use d_splice_alias
            //d_splice_alias(inode, child_dentry); // Preferred way

            brelse(bh);
            return NULL; // VFS expects NULL when lookup is successful and dentry is filled
        }
        record++;
    }

    // Entry not found
    brelse(bh);
    printk(KERN_INFO "ASSOOFS: Lookup: Entry '%s' not found in inode %lu\n",
           child_dentry->d_name.name, parent_inode->i_ino);
    // Tell VFS the entry doesn't exist
    d_add(child_dentry, NULL); // Or d_splice_alias(NULL, child_dentry);
    return NULL;
}


static int assoofs_create(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, umode_t mode, bool excl) {
    struct super_block *sb = dir->i_sb;
    struct assoofs_inode_info *parent_inode_info = dir->i_private;
    struct inode *inode;
    struct assoofs_inode_info *inode_info;
    unsigned long ino;
    uint64_t block_no;
    struct buffer_head *bh;
    struct assoofs_dir_record_entry *dir_contents;
    int ret;

    printk(KERN_INFO "ASSOOFS: Create request for '%s' in parent inode %lu with mode %o\n",
           dentry->d_name.name, dir->i_ino, mode);

    // 1. Get a free inode number
    ret = assoofs_sb_get_a_freeinode(sb, &ino);
    if (ret < 0) {
        printk(KERN_ERR "ASSOOFS: Create: No free inodes available\n");
        return ret;
    }
    printk(KERN_INFO "ASSOOFS: Create: Got free inode number %lu\n", ino);

    // 2. Create the new inode structure
    inode = new_inode(sb);
    if (!inode) {
        assoofs_sb_set_a_freeinode(sb, ino); // Release inode number
        printk(KERN_ERR "ASSOOFS: Create: Failed to create new inode structure\n");
        return -ENOMEM;
    }

    inode->i_ino = ino;
    inode->i_sb = sb;
    inode->i_op = &assoofs_inode_ops;
    inode->i_fop = &assoofs_file_operations; // File operations for a file
    inode_init_owner(idmap, inode, dir, S_IFREG | mode); // Set owner, permissions (Regular File)
    inode_set_atime_to_ts(inode, current_time(inode)); // MODIFICADO MIGRACION
    inode_set_mtime_to_ts(inode, current_time(inode)); // MODIFICADO MIGRACION
    inode_set_ctime_to_ts(inode, current_time(inode)); // MODIFICADO MIGRACION

    // 3. Allocate persistent inode info
    inode_info = kmalloc(sizeof(struct assoofs_inode_info), GFP_KERNEL);
    if (!inode_info) {
        iput(inode); // Release inode structure
        assoofs_sb_set_a_freeinode(sb, ino);
        printk(KERN_ERR "ASSOOFS: Create: Failed to allocate memory for persistent inode info\n");
        return -ENOMEM;
    }
    inode_info->inode_no = ino;
    inode_info->mode = S_IFREG | mode;
    inode_info->file_size = 0; // Initial size is 0
    inode->i_private = inode_info; // Link persistent info to inode

    // 4. Get a free data block
    ret = assoofs_sb_get_a_freeblock(sb, &block_no);
    if (ret < 0) {
        kfree(inode_info);
        iput(inode);
        assoofs_sb_set_a_freeinode(sb, ino);
        printk(KERN_ERR "ASSOOFS: Create: No free data blocks available\n");
        return ret;
    }
    inode_info->data_block_number = block_no;
    printk(KERN_INFO "ASSOOFS: Create: Got free data block number %llu\n", block_no);

    // 5. Write persistent inode info to disk
    assoofs_add_inode_info(sb, inode_info); // This function handles saving the SB count too

    // 6. Add entry to parent directory
    bh = sb_bread(sb, parent_inode_info->data_block_number);
    if (!bh) {
        // Cleanup required: free block, free inode, kfree inode_info, iput inode
        assoofs_sb_set_a_freeblock(sb, block_no);
        assoofs_sb_set_a_freeinode(sb, ino);
        // Need a way to remove the inode info from disk store too... complex cleanup
        kfree(inode_info);
        iput(inode);
        printk(KERN_ERR "ASSOOFS: Create: Error reading parent directory block %llu\n", parent_inode_info->data_block_number);
        return -EIO;
    }
    dir_contents = (struct assoofs_dir_record_entry *)bh->b_data;
    // Find the next slot (simple append)
    dir_contents += parent_inode_info->dir_children_count;
    dir_contents->inode_no = ino;
    strncpy(dir_contents->filename, dentry->d_name.name, ASSOOFS_FILENAME_MAXLEN);
    dir_contents->filename[ASSOOFS_FILENAME_MAXLEN] = '\0'; // Ensure null termination
    dir_contents->entry_removed = ASSOOFS_FALSE;

    mark_buffer_dirty(bh);
    sync_dirty_buffer(bh);
    brelse(bh);

    // 7. Update parent inode's children count
    parent_inode_info->dir_children_count++;
    ret = assoofs_save_inode_info(sb, parent_inode_info);
    if (ret != 0) {
        // Inconsistency: Entry added, but parent count not updated on disk
        printk(KERN_ERR "ASSOOFS: Create: Failed to update parent inode %lu count on disk\n", dir->i_ino);
        // No easy rollback here without more complex journaling/transaction logic
    }

    // 8. Instantiate the dentry
    d_instantiate(dentry, inode);

    printk(KERN_INFO "ASSOOFS: Create: Successfully created file '%s' with inode %lu\n", dentry->d_name.name, ino);
    return 0;
}

static int assoofs_mkdir(struct mnt_idmap *idmap, struct inode *dir , struct dentry *dentry, umode_t mode) {
    struct super_block *sb = dir->i_sb;
    struct assoofs_inode_info *parent_inode_info = dir->i_private;
    struct inode *inode;
    struct assoofs_inode_info *inode_info;
    unsigned long ino;
    uint64_t block_no;
    struct buffer_head *bh;
    struct assoofs_dir_record_entry *dir_contents;
    int ret;

    printk(KERN_INFO "ASSOOFS: Mkdir request for '%s' in parent inode %lu with mode %o\n",
           dentry->d_name.name, dir->i_ino, mode);

    // 1. Get a free inode number
    ret = assoofs_sb_get_a_freeinode(sb, &ino);
    if (ret < 0) return ret;
    printk(KERN_INFO "ASSOOFS: Mkdir: Got free inode number %lu\n", ino);

    // 2. Create the new inode structure
    inode = new_inode(sb);
    if (!inode) {
        assoofs_sb_set_a_freeinode(sb, ino);
        return -ENOMEM;
    }

    inode->i_ino = ino;
    inode->i_sb = sb;
    inode->i_op = &assoofs_inode_ops;
    inode->i_fop = &assoofs_dir_operations; // Directory operations
    inode_init_owner(idmap, inode, dir, S_IFDIR | mode); // Set owner, permissions (Directory)
    inode_set_atime_to_ts(inode, current_time(inode)); // MODIFICADO MIGRACION
    inode_set_mtime_to_ts(inode, current_time(inode)); // MODIFICADO MIGRACION
    inode_set_ctime_to_ts(inode, current_time(inode)); // MODIFICADO MIGRACION
    // For directories, link count starts at 2 ('.' and parent's entry) -> handled by VFS? Let's check. new_inode sets it to 1.
    // Let's explicitly set it if needed: set_nlink(inode, 2); // For '.' and the entry in parent

    // 3. Allocate persistent inode info
    inode_info = kmalloc(sizeof(struct assoofs_inode_info), GFP_KERNEL);
    if (!inode_info) {
        iput(inode);
        assoofs_sb_set_a_freeinode(sb, ino);
        return -ENOMEM;
    }
    inode_info->inode_no = ino;
    inode_info->mode = S_IFDIR | mode;
    inode_info->dir_children_count = 0; // Directory starts empty
    inode->i_private = inode_info;

    // 4. Get a free data block for the directory contents
    ret = assoofs_sb_get_a_freeblock(sb, &block_no);
    if (ret < 0) {
        kfree(inode_info);
        iput(inode);
        assoofs_sb_set_a_freeinode(sb, ino);
        return ret;
    }
    inode_info->data_block_number = block_no;
    printk(KERN_INFO "ASSOOFS: Mkdir: Got free data block number %llu\n", block_no);

    // 5. Write persistent inode info to disk
    assoofs_add_inode_info(sb, inode_info);

    // 6. Add entry to parent directory
    bh = sb_bread(sb, parent_inode_info->data_block_number);
    if (!bh) {
        // Complex cleanup needed
        assoofs_sb_set_a_freeblock(sb, block_no);
        assoofs_sb_set_a_freeinode(sb, ino);
        kfree(inode_info);
        iput(inode);
        printk(KERN_ERR "ASSOOFS: Mkdir: Error reading parent directory block\n");
        return -EIO;
    }
    dir_contents = (struct assoofs_dir_record_entry *)bh->b_data;
    dir_contents += parent_inode_info->dir_children_count; // Append
    dir_contents->inode_no = ino;
    strncpy(dir_contents->filename, dentry->d_name.name, ASSOOFS_FILENAME_MAXLEN);
    dir_contents->filename[ASSOOFS_FILENAME_MAXLEN] = '\0';
    dir_contents->entry_removed = ASSOOFS_FALSE;

    mark_buffer_dirty(bh);
    sync_dirty_buffer(bh);
    brelse(bh);

    // 7. Update parent inode's children count
    parent_inode_info->dir_children_count++;
    ret = assoofs_save_inode_info(sb, parent_inode_info);
    if (ret != 0) {
         printk(KERN_ERR "ASSOOFS: Mkdir: Failed to update parent inode %lu count on disk\n", dir->i_ino);
         // Inconsistency
    }

    // 8. Instantiate the dentry
    d_instantiate(dentry, inode);

    printk(KERN_INFO "ASSOOFS: Mkdir: Successfully created directory '%s' with inode %lu\n", dentry->d_name.name, ino);
    return 0;
}

static int assoofs_remove(struct inode *dir, struct dentry *dentry){
    struct super_block *sb = dir->i_sb;
    struct inode *inode_remove = d_inode(dentry); // Get inode to remove from dentry
    struct assoofs_inode_info *inode_info_remove = inode_remove->i_private;
    struct assoofs_inode_info *parent_inode_info = dir->i_private;
    struct buffer_head *bh;
    struct assoofs_dir_record_entry *record;
    int i;
    int ret;

    printk(KERN_INFO "ASSOOFS: Remove request for '%s' (inode %lu) in parent inode %lu\n",
           dentry->d_name.name, inode_remove->i_ino, dir->i_ino);

    // Optional: Add check for removing non-empty directory
    if (S_ISDIR(inode_info_remove->mode) && inode_info_remove->dir_children_count > 0) {
         printk(KERN_ERR "ASSOOFS: Remove: Attempt to remove non-empty directory inode %lu\n", inode_remove->i_ino);
         //return -ENOTEMPTY; // Standard error for this case
    }


    // 1. Mark directory entry as removed
    bh = sb_bread(sb, parent_inode_info->data_block_number);
    if (!bh) {
        printk(KERN_ERR "ASSOOFS: Remove: Error reading parent directory block %llu\n", parent_inode_info->data_block_number);
        return -EIO;
    }
    record = (struct assoofs_dir_record_entry *)bh->b_data;

    for (i = 0; i < parent_inode_info->dir_children_count; i++) {
        if (record->entry_removed == ASSOOFS_FALSE &&
            strcmp(record->filename, dentry->d_name.name) == 0 &&
            record->inode_no == inode_remove->i_ino) {

            printk(KERN_INFO "ASSOOFS: Remove: Found dir entry for inode %lu, marking removed.\n", inode_remove->i_ino);
            record->entry_removed = ASSOOFS_TRUE;
            mark_buffer_dirty(bh);
            sync_dirty_buffer(bh);
            // Optional: Could decrement parent_inode_info->dir_children_count here
            // and save parent inode info, but simple removal just marks.
            goto found;
        }
        record++;
    }
    // Entry not found (shouldn't happen if VFS called remove correctly)
    brelse(bh);
    printk(KERN_ERR "ASSOOFS: Remove: Could not find directory entry for '%s' in parent %lu\n", dentry->d_name.name, dir->i_ino);
    return -ENOENT;

found:
    brelse(bh);

    // 2. Free the inode number in the superblock bitmap
    ret = assoofs_sb_set_a_freeinode(sb, inode_info_remove->inode_no);
    if (ret < 0) {
        printk(KERN_ERR "ASSOOFS: Remove: Failed to free inode %lu in bitmap\n", inode_info_remove->inode_no);
        // Inconsistency: entry marked removed, but inode not freed
        return ret;
    }
    printk(KERN_INFO "ASSOOFS: Remove: Freed inode number %lu\n", inode_info_remove->inode_no);

    // 3. Free the data block in the superblock bitmap
    ret = assoofs_sb_set_a_freeblock(sb, inode_info_remove->data_block_number);
    if (ret < 0) {
        printk(KERN_ERR "ASSOOFS: Remove: Failed to free block %llu in bitmap\n", inode_info_remove->data_block_number);
        // Inconsistency: inode freed, but block not freed
        return ret;
    }
    printk(KERN_INFO "ASSOOFS: Remove: Freed block number %llu\n", inode_info_remove->data_block_number);

    // VFS handles dropping the inode structure and its i_private data via drop_inode

    printk(KERN_INFO "ASSOOFS: Remove: Successfully removed entry '%s'\n", dentry->d_name.name);
    return 0;
}

/*
 *  Inicialización del superbloque
 */
int assoofs_fill_super(struct super_block *sb, void *data, int silent) {
    struct buffer_head *bh;
    struct assoofs_super_block_info *assoofs_sb_disk;
    struct assoofs_super_block_info *assoofs_sb_mem;
    struct inode *root_inode;
    int ret = -EINVAL; // Default error

    printk(KERN_INFO "ASSOOFS: fill_super request\n");

    // 1. Leer la información persistente del superbloque (Block 0)
    bh = sb_bread(sb, ASSOOFS_SUPERBLOCK_BLOCK_NUMBER);
    if (!bh) {
        printk(KERN_ERR "ASSOOFS: Error reading superblock from device\n");
        return -EIO;
    }
    assoofs_sb_disk = (struct assoofs_super_block_info *)bh->b_data;

    // 2. Comprobar los parámetros (magic number, block size)
    if (assoofs_sb_disk->magic != ASSOOFS_MAGIC) {
        printk(KERN_ERR "ASSOOFS: Invalid magic number: %llx\n", assoofs_sb_disk->magic);
        goto release_bh;
    }
    if (assoofs_sb_disk->block_size != ASSOOFS_DEFAULT_BLOCK_SIZE) {
        printk(KERN_ERR "ASSOOFS: Invalid block size: %llu\n", assoofs_sb_disk->block_size);
        goto release_bh;
    }
    printk(KERN_INFO "ASSOOFS: Superblock read successfully (magic: %llx, blocksize: %llu)\n",
           assoofs_sb_disk->magic, assoofs_sb_disk->block_size);

    // 3. Escribir la información en el superbloque en memoria (sb)
    sb->s_magic = ASSOOFS_MAGIC;
    sb->s_maxbytes = ASSOOFS_DEFAULT_BLOCK_SIZE; // Max file size
    sb->s_op = &assoofs_sops; // Superblock operations

    // Allocate memory for in-memory superblock info and copy from disk version
    assoofs_sb_mem = kmalloc(sizeof(struct assoofs_super_block_info), GFP_KERNEL);
    if (!assoofs_sb_mem) {
         printk(KERN_ERR "ASSOOFS: Failed to allocate memory for superblock info\n");
         ret = -ENOMEM;
         goto release_bh;
    }
    memcpy(assoofs_sb_mem, assoofs_sb_disk, sizeof(struct assoofs_super_block_info));
    sb->s_fs_info = assoofs_sb_mem; // Store our persistent info copy

    brelse(bh); // Release buffer head for superblock after copying data

    // 4. Crear el inodo raíz
    root_inode = assoofs_get_inode(sb, ASSOOFS_ROOTDIR_INODE_NUMBER);
    if (IS_ERR(root_inode)) {
        printk(KERN_ERR "ASSOOFS: Failed to get root inode\n");
        kfree(sb->s_fs_info); // Free allocated memory
        sb->s_fs_info = NULL;
        return PTR_ERR(root_inode);
    }

    sb->s_root = d_make_root(root_inode);
    if (!sb->s_root) {
        printk(KERN_ERR "ASSOOFS: Failed to create root dentry\n");
        iput(root_inode); // Decrease ref count for root inode
        kfree(sb->s_fs_info);
        sb->s_fs_info = NULL;
        return -ENOMEM;
    }

    printk(KERN_INFO "ASSOOFS: Filesystem mounted successfully with root inode %lu\n", root_inode->i_ino);
    return 0; // Success

release_bh:
    brelse(bh);
    return ret;
}

/*
 *  Montaje de dispositivos assoofs
 */
static struct dentry *assoofs_mount(struct file_system_type *fs_type, int flags, const char *dev_name, void *data) {
    struct dentry *ret;

    printk(KERN_INFO "ASSOOFS: mount request. Device: %s\n", dev_name);

    // Mount the block device
    ret = mount_bdev(fs_type, flags, dev_name, data, assoofs_fill_super);

    // Control de errores
    if (IS_ERR(ret)) {
        printk(KERN_ERR "ASSOOFS: Error mounting block device %s: %ld\n", dev_name, PTR_ERR(ret));
    } else {
        printk(KERN_INFO "ASSOOFS: Block device %s mounted successfully\n", dev_name);
    }

    return ret;
}


/*
 * Module Initialization and Exit
 */

static int __init assoofs_init(void) {
    int ret;
    printk(KERN_INFO "ASSOOFS: Initializing assoofs module\n");
    ret = register_filesystem(&assoofs_type);
    // Control de errores
    if (ret != 0) {
        printk(KERN_ERR "ASSOOFS: Failed to register filesystem: %d\n", ret);
    } else {
        printk(KERN_INFO "ASSOOFS: Filesystem registered successfully\n");
    }
    return ret;
}

static void __exit assoofs_exit(void) {
    int ret;
    printk(KERN_INFO "ASSOOFS: Exiting assoofs module\n");
    ret = unregister_filesystem(&assoofs_type);
     // Control de errores
    if (ret != 0) {
        printk(KERN_ERR "ASSOOFS: Failed to unregister filesystem: %d\n", ret);
    } else {
        printk(KERN_INFO "ASSOOFS: Filesystem unregistered successfully\n");
    }
     // Note: If mount points still exist, unregister might fail.
}

/*
 * Helper Function Implementations
 */

// Reads persistent inode info from disk
static struct assoofs_inode_info *assoofs_get_inode_info(struct super_block *sb, uint64_t inode_no) {
    struct buffer_head *bh;
    struct assoofs_inode_info *inode_disk;
    struct assoofs_inode_info *inode_info = NULL; // Return value
    struct assoofs_super_block_info *afs_sb = sb->s_fs_info;
    int i;

    printk(KERN_INFO "ASSOOFS: get_inode_info for inode number %llu\n", inode_no);

    bh = sb_bread(sb, ASSOOFS_INODESTORE_BLOCK_NUMBER);
    if (!bh) {
        printk(KERN_ERR "ASSOOFS: get_inode_info: Failed to read inode store block\n");
        return NULL;
    }
    inode_disk = (struct assoofs_inode_info *)bh->b_data;

    // Iterate through the inode store
    for (i = 0; i < afs_sb->inodes_count; i++) {
        // Check if inode number matches (assumes inodes are contiguous for now)
        if (inode_disk->inode_no == inode_no) {
            // Found the inode, allocate memory and copy
            inode_info = kmalloc(sizeof(struct assoofs_inode_info), GFP_KERNEL);
            if (!inode_info) {
                printk(KERN_ERR "ASSOOFS: get_inode_info: Failed to allocate memory\n");
                break; // Exit loop
            }
            memcpy(inode_info, inode_disk, sizeof(*inode_info));
            printk(KERN_INFO "ASSOOFS: get_inode_info: Found inode %llu (mode: %o)\n", inode_no, inode_info->mode);
            break; // Exit loop
        }
        inode_disk++; // Move to next inode on disk
    }

    brelse(bh);
    if (!inode_info) {
         printk(KERN_WARNING "ASSOOFS: get_inode_info: Inode %llu not found in store.\n", inode_no);
    }
    return inode_info; // Returns NULL if not found or allocation failed
}

// Gets or creates a VFS inode structure
static struct inode *assoofs_get_inode(struct super_block *sb, int ino) {
    struct inode *inode;
    struct assoofs_inode_info *inode_info;

    printk(KERN_INFO "ASSOOFS: get_inode for inode number %d\n", ino);

    // Get inode from VFS cache or create a new one
    inode = iget_locked(sb, ino);
    if (!inode) {
        printk(KERN_ERR "ASSOOFS: get_inode: Failed to get inode %d from VFS\n", ino);
        return ERR_PTR(-ENOMEM);
    }

    // If inode is new, fill it from disk
    if (inode->i_state & I_NEW) {
        printk(KERN_INFO "ASSOOFS: get_inode: Inode %d is new, initializing...\n", ino);
        inode_info = assoofs_get_inode_info(sb, ino);
        if (!inode_info) {
            iget_failed(inode); // Tell VFS iget failed
            printk(KERN_ERR "ASSOOFS: get_inode: Failed to get persistent info for new inode %d\n", ino);
            return ERR_PTR(-EIO); // Or another appropriate error
        }

        // Fill VFS inode structure
        inode->i_private = inode_info; // Attach persistent info
        inode->i_op = &assoofs_inode_ops; // Set inode operations

        // Set file operations based on type
        if (S_ISDIR(inode_info->mode)) {
            inode->i_fop = &assoofs_dir_operations;
            // set_nlink(inode, 2); // Directories usually start with link count 2 ('.' and parent entry) - VFS might handle this.
        } else if (S_ISREG(inode_info->mode)) {
            inode->i_fop = &assoofs_file_operations;
             // set_nlink(inode, 1); // Regular files start with link count 1
        } else {
             printk(KERN_ERR "ASSOOFS: get_inode: Unknown inode type for inode %d (mode: %o)\n", ino, inode_info->mode);
             // Handle error - maybe set default ops or fail?
             iget_failed(inode);
             kfree(inode_info); // Free allocated persistent info
             return ERR_PTR(-EINVAL);
        }

        // Set times (usually read from persistent info if available, but mkassoofs doesn't store them)
        inode_set_atime_to_ts(inode, current_time(inode)); // MODIFICADO MIGRACION
        inode_set_mtime_to_ts(inode, current_time(inode)); // MODIFICADO MIGRACION
        inode_set_ctime_to_ts(inode, current_time(inode)); // MODIFICADO MIGRACION

        // Unlock the new inode for VFS
        unlock_new_inode(inode);
    } else {
         printk(KERN_INFO "ASSOOFS: get_inode: Found inode %d in VFS cache\n", ino);
    }

    return inode;
}

// Finds and allocates a free inode number from the superblock bitmap
static int assoofs_sb_get_a_freeinode(struct super_block *sb, unsigned long *inode) {
    struct assoofs_super_block_info *assoofs_sb = sb->s_fs_info;
    int i;

    // Add locking here if concurrent access is possible (e.g., mutex_lock)
    printk(KERN_INFO "ASSOOFS: Searching for a free inode...\n");

    for (i = ASSOOFS_ROOTDIR_INODE_NUMBER + 1; i < ASSOOFS_MAX_FILESYSTEM_OBJECTS_SUPPORTED; i++) {
         // Check if the i-th bit is 0 (meaning free)
        if (!((assoofs_sb->free_inodes) & (1 << i))) {
            *inode = i;
             // Mark inode as used (set the i-th bit to 1)
            assoofs_sb->free_inodes |= (1 << i);
            assoofs_save_sb_info(sb); // Save changes to disk
            // Unlock mutex here
            printk(KERN_INFO "ASSOOFS: Found free inode: %d\n", i);
            return 0; // Success
        }
    }

    // No free inodes found
    // Unlock mutex here
    printk(KERN_ERR "ASSOOFS: No free inodes found.\n");
    return -ENOSPC;
}

// Finds and allocates a free data block number from the superblock bitmap
static int assoofs_sb_get_a_freeblock(struct super_block *sb, uint64_t *block) {
    struct assoofs_super_block_info *assoofs_sb = sb->s_fs_info;
    int i;

    // Add locking here
    printk(KERN_INFO "ASSOOFS: Searching for a free block...\n");

    // Start searching after reserved blocks
    for (i = ASSOOFS_LAST_RESERVED_BLOCK + 1; i < ASSOOFS_MAX_FILESYSTEM_OBJECTS_SUPPORTED; i++) {
         // Check if the i-th bit is 0
        if (!((assoofs_sb->free_blocks) & (1 << i))) {
            *block = i;
             // Mark block as used
            assoofs_sb->free_blocks |= (1 << i);
            assoofs_save_sb_info(sb);
            // Unlock mutex here
            printk(KERN_INFO "ASSOOFS: Found free block: %d\n", i);
            return 0;
        }
    }

    // No free blocks
    // Unlock mutex here
    printk(KERN_ERR "ASSOOFS: No free blocks found.\n");
    return -ENOSPC;
}

// Saves the in-memory superblock info (sb->s_fs_info) back to disk (Block 0)
static void assoofs_save_sb_info(struct super_block *vsb) {
    struct buffer_head *bh;
    struct assoofs_super_block_info *assoofs_sb_mem = vsb->s_fs_info; // Get in-memory copy

    printk(KERN_INFO "ASSOOFS: Saving superblock info to disk...\n");

    bh = sb_bread(vsb, ASSOOFS_SUPERBLOCK_BLOCK_NUMBER);
    if (!bh) {
        printk(KERN_ERR "ASSOOFS: save_sb_info: Failed to read superblock block for writing.\n");
        return; // Cannot save
    }

    // Copy in-memory data to the buffer
    memcpy(bh->b_data, assoofs_sb_mem, sizeof(struct assoofs_super_block_info));

    mark_buffer_dirty(bh);
    sync_dirty_buffer(bh); // Write immediately
    brelse(bh);
    printk(KERN_INFO "ASSOOFS: Superblock info saved.\n");
}

// Adds new persistent inode info to the inode store block
static void assoofs_add_inode_info(struct super_block *sb, struct assoofs_inode_info *inode) {
    struct assoofs_super_block_info *assoofs_sb = sb->s_fs_info;
    struct buffer_head *bh;
    struct assoofs_inode_info *inode_info_disk;

    printk(KERN_INFO "ASSOOFS: Adding inode info for inode %llu to store...\n", inode->inode_no);

    // Increment inode count if this inode number is at or beyond the current count
    // This handles reusing inode numbers correctly without always incrementing.
    // Add locking here
    if (assoofs_sb->inodes_count <= inode->inode_no) {
        assoofs_sb->inodes_count = inode->inode_no + 1;
        assoofs_save_sb_info(sb); // Save updated count
    }
    // Unlock mutex here

    bh = sb_bread(sb, ASSOOFS_INODESTORE_BLOCK_NUMBER);
    if (!bh) {
        printk(KERN_ERR "ASSOOFS: add_inode_info: Failed to read inode store block.\n");
        return; // Cannot add
    }

    inode_info_disk = (struct assoofs_inode_info *)bh->b_data;
    // Go to the position corresponding to the inode number (assumes contiguous storage)
    inode_info_disk += inode->inode_no; // Navigate to the correct slot

    // Copy the new inode data to the buffer
    memcpy(inode_info_disk, inode, sizeof(struct assoofs_inode_info));

    mark_buffer_dirty(bh);
    sync_dirty_buffer(bh);
    brelse(bh);
    printk(KERN_INFO "ASSOOFS: Inode info for inode %llu added/updated.\n", inode->inode_no);
}

// Saves existing persistent inode info back to the inode store block
static int assoofs_save_inode_info(struct super_block *sb, struct assoofs_inode_info *inode_info) {
    struct buffer_head *bh;
    struct assoofs_inode_info *inode_pos;

    printk(KERN_INFO "ASSOOFS: Saving inode info for inode %llu...\n", inode_info->inode_no);

    bh = sb_bread(sb, ASSOOFS_INODESTORE_BLOCK_NUMBER);
    if (!bh) {
        printk(KERN_ERR "ASSOOFS: save_inode_info: Failed to read inode store block.\n");
        return -EIO;
    }

    // Find the position of the inode in the store block
    inode_pos = assoofs_search_inode_info(sb, (struct assoofs_inode_info *)bh->b_data, inode_info);

    if (inode_pos) {
        // Found it, copy updated data
        memcpy(inode_pos, inode_info, sizeof(*inode_pos));
        mark_buffer_dirty(bh);
        sync_dirty_buffer(bh);
        brelse(bh);
        printk(KERN_INFO "ASSOOFS: Inode info for %llu saved.\n", inode_info->inode_no);
        return 0;
    } else {
        // Inode not found in the store - should not happen if called correctly
        brelse(bh);
        printk(KERN_ERR "ASSOOFS: save_inode_info: Inode %llu not found in store to save.\n", inode_info->inode_no);
        return -EIO; // Or another error
    }
}

// Helper to find a specific inode within the inode store block data
static struct assoofs_inode_info *assoofs_search_inode_info(struct super_block *sb, struct assoofs_inode_info *start, struct assoofs_inode_info *search) {
    struct assoofs_super_block_info *afs_sb = sb->s_fs_info;
    uint64_t count = 0;

    printk(KERN_INFO "ASSOOFS: Searching for inode %llu in inode store...\n", search->inode_no);
    while (start->inode_no != search->inode_no && count < afs_sb->inodes_count) {
        count++;
        start++;
    }

    if (start->inode_no == search->inode_no) {
        printk(KERN_INFO "ASSOOFS: Found inode %llu in store at position %llu\n", search->inode_no, count);
        return start; // Return pointer to the location in the buffer
    }

    printk(KERN_WARNING "ASSOOFS: search_inode_info: Inode %llu not found.\n", search->inode_no);
    return NULL;
}

// Frees an inode number in the superblock bitmap
static int assoofs_sb_set_a_freeinode(struct super_block *sb, uint64_t inode_no) {
    struct assoofs_super_block_info *assoofs_sb = sb->s_fs_info;

    // Add locking here
    printk(KERN_INFO "ASSOOFS: Freeing inode number %llu...\n", inode_no);

    // Check if inode is actually used
    if (!((assoofs_sb->free_inodes) & (NULL << inode_no))) {
         printk(KERN_WARNING "ASSOOFS: Attempt to free already free inode %llu\n", inode_no);
         // Unlock mutex here
         // return -EINVAL; // Or just ignore
         return 0;
    }

    // Mark inode as free (clear the bit)
    assoofs_sb->free_inodes &= ~(NULL << inode_no);
    assoofs_save_sb_info(sb); // Save changes

    // Unlock mutex here
    printk(KERN_INFO "ASSOOFS: Inode %llu marked as free.\n", inode_no);
    return 0;
}

// Frees a data block number in the superblock bitmap
static int assoofs_sb_set_a_freeblock(struct super_block *sb, uint64_t block) {
    struct assoofs_super_block_info *assoofs_sb = sb->s_fs_info;

    // Add locking here
    printk(KERN_INFO "ASSOOFS: Freeing block number %llu...\n", block);

    // Check if block is actually used
     if (!((assoofs_sb->free_blocks) & (NULL << block))) {
         printk(KERN_WARNING "ASSOOFS: Attempt to free already free block %llu\n", block);
         // Unlock mutex here
         // return -EINVAL;
         return 0;
     }

    // Mark block as free (clear the bit)
    assoofs_sb->free_blocks &= ~(NULL << block);
    assoofs_save_sb_info(sb);

    // Unlock mutex here
    printk(KERN_INFO "ASSOOFS: Block %llu marked as free.\n", block);
    return 0;
}


module_init(assoofs_init);
module_exit(assoofs_exit);
