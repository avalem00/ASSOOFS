#define ASSOOFS_MAGIC 0x20200406
#define ASSOOFS_DEFAULT_BLOCK_SIZE 4096
#define ASSOOFS_FILENAME_MAXLEN 255
#define ASSOOFS_LAST_RESERVED_BLOCK ASSOOFS_ROOTDIR_BLOCK_NUMBER
#define ASSOOFS_LAST_RESERVED_INODE ASSOOFS_ROOTDIR_INODE_NUMBER
const int ASSOOFS_TRUE = 1;
const int ASSOOFS_FALSE = 0;
const int ASSOOFS_SUPERBLOCK_BLOCK_NUMBER = 0;  
const int ASSOOFS_INODESTORE_BLOCK_NUMBER = 1;  
const int ASSOOFS_ROOTDIR_BLOCK_NUMBER = 2;     
const int ASSOOFS_ROOTDIR_INODE_NUMBER = 0;     
const int ASSOOFS_MAX_FILESYSTEM_OBJECTS_SUPPORTED = 64;  

struct assoofs_super_block_info {
    uint64_t version; 
    uint64_t magic;
    uint64_t block_size;    
    uint64_t inodes_count;
    uint64_t free_blocks;  
    uint64_t free_inodes;
    char padding[4048];     
};

struct assoofs_dir_record_entry {
    char filename[ASSOOFS_FILENAME_MAXLEN]; 
    uint64_t inode_no;  
    uint64_t entry_removed;
};


struct assoofs_inode_info {
    mode_t mode;    
    uint64_t inode_no; 
    uint64_t data_block_number; 

    union {                
        uint64_t file_size;
        uint64_t dir_children_count;  
    };
};

