#include <linux/module.h>       /* Needed by all modules */
#include <linux/kernel.h>       /* Needed for KERN_INFO  */
#include <linux/init.h>         /* Needed for the macros */
#include <linux/fs.h>           /* libfs stuff           */
#include <linux/buffer_head.h>  /* buffer_head           */
#include <linux/slab.h>         /* kmem_cache            */
#include "assoofs.h"

MODULE_LICENSE("GPL");

/*
 *  Prototipos de funciones
 */
static struct dentry *assoofs_mount(struct file_system_type *fs_type, int flags, const char *dev_name, void *data);
int assoofs_fill_super(struct super_block *sb, void *data, int silent);
ssize_t assoofs_read(struct file * filp, char __user * buf, size_t len, loff_t * ppos);
ssize_t assoofs_write(struct file * filp, const char __user * buf, size_t len, loff_t * ppos);
static int assoofs_iterate(struct file *filp, struct dir_context *ctx);
static int assoofs_create(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, umode_t mode, bool excl); //MODIFICADO MIGRACION el primer argumento
struct dentry *assoofs_lookup(struct inode *parent_inode, struct dentry *child_dentry, unsigned int flags);
static int assoofs_mkdir(struct mnt_idmap *idmap, struct inode *dir , struct dentry *dentry, umode_t mode); //MODIFICADO MIGRACION el primer argumento cambia
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
};

// Operaciones sobre dircctorios
const struct file_operations assoofs_dir_operations = {
    .owner = THIS_MODULE,
    .iterate_shared = assoofs_iterate, //MODIFICADO MIGRACION
};
// Operaciones sobre inodos
static struct inode_operations assoofs_inode_ops = {
    .create = assoofs_create,
    .lookup = assoofs_lookup,
    .mkdir = assoofs_mkdir,
    .unlink = assoofs_remove,
    .rmdir = assoofs_remove,
};
// Operaciones sobre el superbloque
static const struct super_operations assoofs_sops = {
    .drop_inode = generic_delete_inode,
};


/*
 *  Funciones que realizan operaciones sobre ficheros
 */

 ssize_t assoofs_read(struct file *filp, char __user *buf, size_t len, loff_t *ppos) {
    struct inode *inode = file_inode(filp);
    struct super_block *sb = inode->i_sb;
    struct assoofs_inode_info *inode_info = inode->i_private;
    struct buffer_head *bh;
    char *data;
    size_t nbytes;

    if (*ppos >= inode_info->file_size)
        return 0;

    bh = sb_bread(sb, inode_info->data_block_number);
    data = (char *)bh->b_data;
    nbytes = min(len, (size_t)(inode_info->file_size - *ppos));

    if (copy_to_user(buf, data + *ppos, nbytes)) {
        brelse(bh);
        return -EFAULT;
    }

    *ppos += nbytes;
    brelse(bh);
    return nbytes;
}


ssize_t assoofs_write(struct file *filp, const char __user *buf, size_t len, loff_t *ppos) {
    struct inode *inode = file_inode(filp);
    struct super_block *sb = inode->i_sb;
    struct assoofs_inode_info *inode_info = inode->i_private;
    struct buffer_head *bh;
    char *data;

    if (len > ASSOOFS_DEFAULT_BLOCK_SIZE) {
        printk(KERN_ERR "assoofs_write: write size exceeds block size\n");
        return -ENOSPC;
    }

    bh = sb_bread(sb, inode_info->data_block_number);
    data = (char *)bh->b_data;

    if (copy_from_user(data + *ppos, buf, len)) {
        brelse(bh);
        return -EFAULT;
    }

    *ppos += len;
    inode_info->file_size = *ppos;
    mark_buffer_dirty(bh);
    brelse(bh);
    mark_inode_dirty(inode);

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

    if (ctx->pos)
        return 0;

    bh = sb_bread(sb, inode_info->data_block_number);
    record = (struct assoofs_dir_record_entry *)bh->b_data;

    for (i = 0; i < inode_info->dir_children_count; i++) {
        if (record[i].entry_removed == ASSOOFS_FALSE) {
            dir_emit(ctx, record[i].filename, strlen(record[i].filename),
                     record[i].inode_no, DT_UNKNOWN);
            ctx->pos += sizeof(struct assoofs_dir_record_entry);
        }
    }

    brelse(bh);
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

    bh = sb_bread(sb, parent_info->data_block_number);
    record = (struct assoofs_dir_record_entry *)bh->b_data;

    for (i = 0; i < parent_info->dir_children_count; i++) {
        if (!strcmp(record[i].filename, child_dentry->d_name.name) && record[i].entry_removed == ASSOOFS_FALSE) {
            struct inode *inode = new_inode(sb);
            struct assoofs_inode_info *info = kmalloc(sizeof(struct assoofs_inode_info), GFP_KERNEL);
            struct buffer_head *inode_bh = sb_bread(sb, ASSOOFS_INODESTORE_BLOCK_NUMBER);
            memcpy(info, ((struct assoofs_inode_info *)inode_bh->b_data) + record[i].inode_no, sizeof(*info));
            brelse(inode_bh);
            inode->i_ino = info->inode_no;
            inode_init_owner(&nop_mnt_idmap, inode, parent_inode, info->mode);
            inode->i_sb = sb;
            inode->i_op = &assoofs_inode_ops;
            inode->i_fop = S_ISDIR(info->mode) ? &assoofs_dir_operations : &assoofs_file_operations;
            inode->i_private = info;
            d_add(child_dentry, inode);
            brelse(bh);
            return NULL;
        }
    }

    brelse(bh);
    return NULL;
}



static int assoofs_create(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, umode_t mode, bool excl) {
    struct super_block *sb = dir->i_sb;
    struct inode *inode = new_inode(sb);
    struct assoofs_inode_info *inode_info = kmalloc(sizeof(struct assoofs_inode_info), GFP_KERNEL);
    struct assoofs_inode_info *parent_info = dir->i_private;
    struct buffer_head *bh;
    struct assoofs_dir_record_entry *record;
    struct assoofs_super_block_info *sb_info = sb->s_fs_info;

    inode->i_sb = sb;
    inode->i_op = &assoofs_inode_ops;
    inode->i_fop = &assoofs_file_operations;
    inode_init_owner(idmap, inode, dir, mode);
    inode->i_ino = sb_info->inodes_count;

    inode_info->inode_no = inode->i_ino;
    inode_info->mode = mode;
    inode_info->file_size = 0;
    inode_info->data_block_number = sb_info->free_blocks;
    inode->i_private = inode_info;

    bh = sb_bread(sb, ASSOOFS_INODESTORE_BLOCK_NUMBER);
    memcpy(((struct assoofs_inode_info *)bh->b_data) + inode_info->inode_no, inode_info, sizeof(*inode_info));
    mark_buffer_dirty(bh);
    brelse(bh);

    bh = sb_bread(sb, parent_info->data_block_number);
    record = (struct assoofs_dir_record_entry *)bh->b_data + parent_info->dir_children_count;
    strcpy(record->filename, dentry->d_name.name);
    record->inode_no = inode_info->inode_no;
    record->entry_removed = ASSOOFS_FALSE;
    mark_buffer_dirty(bh);
    brelse(bh);

    parent_info->dir_children_count++;
    sb_info->inodes_count++;
    sb_info->free_blocks++;

    mark_inode_dirty(dir);
    d_add(dentry, inode);

    return 0;
}

static int assoofs_mkdir(struct mnt_idmap *idmap, struct inode *dir , struct dentry *dentry, umode_t mode) {
    return assoofs_create(idmap, dir, dentry, S_IFDIR | mode, 0);
}

static int assoofs_remove(struct inode *dir, struct dentry *dentry) {
    struct super_block *sb = dir->i_sb;
    struct assoofs_inode_info *parent_info = dir->i_private;
    struct buffer_head *bh;
    struct assoofs_dir_record_entry *record;
    int i;

    bh = sb_bread(sb, parent_info->data_block_number);
    record = (struct assoofs_dir_record_entry *)bh->b_data;

    for (i = 0; i < parent_info->dir_children_count; i++) {
        if (!strcmp(record[i].filename, dentry->d_name.name)) {
            record[i].entry_removed = ASSOOFS_TRUE;
            mark_buffer_dirty(bh);
            break;
        }
    }

    brelse(bh);
    return 0;
}


/*
 *  Inicialización del superbloque
 */
int assoofs_fill_super(struct super_block *sb, void *data, int silent) {
    struct buffer_head *bh;
    struct assoofs_super_block_info *assoofs_sb;
    struct inode *root_inode;

    printk(KERN_INFO "assoofs_fill_super: reading superblock\n");

    // 1. Leer el superbloque del disco (bloque 0)
    bh = sb_bread(sb, ASSOOFS_SUPERBLOCK_BLOCK_NUMBER);
    assoofs_sb = (struct assoofs_super_block_info *)bh->b_data;

    // 2. Validar el número mágico
    if (assoofs_sb->magic != ASSOOFS_MAGIC) {
        printk(KERN_ERR "assoofs_fill_super: invalid magic number\n");        brelse(bh);
        return -EINVAL;
    }

    // 3. Asignar la info leída al superblock
    sb->s_magic = ASSOOFS_MAGIC;
    sb->s_fs_info = assoofs_sb;
    sb->s_op = &assoofs_sops;

    brelse(bh);  // liberar buffer ya usado

    // 4. Crear el inodo raíz
    root_inode = new_inode(sb);
    inode_init_owner(&nop_mnt_idmap, root_inode, NULL, S_IFDIR);
    root_inode->i_ino = ASSOOFS_ROOTDIR_INODE_NUMBER;
    root_inode->i_sb = sb;
    root_inode->i_op = &assoofs_inode_ops;
    root_inode->i_fop = &assoofs_dir_operations;

    // Obtener la info persistente del inodo raíz
    bh = sb_bread(sb, ASSOOFS_INODESTORE_BLOCK_NUMBER);
    root_inode->i_private = kmalloc(sizeof(struct assoofs_inode_info), GFP_KERNEL);
    memcpy(root_inode->i_private, bh->b_data, sizeof(struct assoofs_inode_info));
    brelse(bh);

    sb->s_root = d_make_root(root_inode);

    printk(KERN_INFO "assoofs_fill_super: root inode created\n");

    return 0;
}


/*
 *  Función auxiliar para obtener info de inodos
 */
struct assoofs_inode_info *assoofs_get_inode_info(struct super_block *sb, uint64_t inode_no) {
    struct buffer_head *bh;
    struct assoofs_inode_info *inode_info;
    struct assoofs_super_block_info *afs_sb = sb->s_fs_info;
    struct assoofs_inode_info *buffer = NULL;
    int i;

    bh = sb_bread(sb, ASSOOFS_INODESTORE_BLOCK_NUMBER);
    if (!bh) {
        printk(KERN_ERR "assoofs: error reading inode store\n");
        return NULL;
    }

    inode_info = (struct assoofs_inode_info *)bh->b_data;

    for (i = 0; i < afs_sb->inodes_count; i++) {
        if (inode_info->inode_no == inode_no) {
            buffer = kmalloc(sizeof(struct assoofs_inode_info), GFP_KERNEL);
            memcpy(buffer, inode_info, sizeof(*buffer));
            break;
        }
        inode_info++;
    }

    brelse(bh);
    return buffer;
}


/*
 *  Montaje de dispositivos assoofs
 */
static struct dentry *assoofs_mount(struct file_system_type *fs_type, int flags, const char *dev_name, void *data) {
    struct dentry *ret;
    printk(KERN_INFO "assoofs_mount request\n");
    ret = mount_bdev(fs_type, flags, dev_name, data, assoofs_fill_super);
    if (IS_ERR(ret)) {
        printk(KERN_ERR "assoofs_mount: error mounting filesystem\n");
    }
    return ret;
}



static int __init assoofs_init(void) {
    int ret;
    printk(KERN_INFO "assoofs_init request\n");
    ret = register_filesystem(&assoofs_type);
    // Control de errores a partir del valor de retorno
    return ret;
}

static void __exit assoofs_exit(void) {
    int ret;
    printk(KERN_INFO "assoofs_exit request\n");
    ret = unregister_filesystem(&assoofs_type);
    // Control de errores a partir del valor de retorno
}

module_init(assoofs_init);
module_exit(assoofs_exit);


static struct inode *assoofs_get_inode(struct super_block *sb, int ino) {
    struct inode *inode;
    struct timespec64 ts;
    struct assoofs_inode_info *inode_info;

    inode_info = assoofs_get_inode_info(sb, ino);
    if (!inode_info) {
        printk(KERN_ERR "assoofs_get_inode: failed to get inode info\n");
        return NULL;
    }

    inode = new_inode(sb);
    if (!inode) {
        printk(KERN_ERR "assoofs_get_inode: failed to allocate new inode\n");
        kfree(inode_info);
        return NULL;
    }

    inode->i_ino = ino;
    inode->i_sb = sb;
    inode->i_op = &assoofs_inode_ops;

    if (S_ISDIR(inode_info->mode)) {
        inode->i_fop = &assoofs_dir_operations;
    } else if (S_ISREG(inode_info->mode)) {
        inode->i_fop = &assoofs_file_operations;
    } else {
        printk(KERN_ERR "assoofs_get_inode: unknown inode type\n");
    }

    ts = current_time(inode);
    inode_set_ctime(inode, ts.tv_sec, ts.tv_nsec);
    inode_set_mtime(inode, ts.tv_sec, ts.tv_nsec);
    inode_set_atime(inode, ts.tv_sec, ts.tv_nsec);

    inode->i_private = inode_info;
    return inode;
}