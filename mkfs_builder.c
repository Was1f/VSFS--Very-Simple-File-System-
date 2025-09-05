// Build: gcc -O2 -std=c17 -Wall -Wextra mkfs_minivsfs.c -o mkfs_builder

//to do remove errno.h logics


#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <time.h>
#include <assert.h>

#define BS 4096u               // block size
#define INODE_SIZE 128u
#define ROOT_INO 1u

// uint64_t g_random_seed = 0; // This should be replaced by seed value from the CLI.


#pragma pack(push, 1)
typedef struct {
    uint32_t magic;              // 0x4D565346
    uint32_t version;            // 1
    uint32_t block_size;         // 4096
    uint64_t total_blocks;       // Calculated from size_kib
    uint64_t inode_count;        // From CLI
    uint64_t inode_bitmap_start; // 1
    uint64_t inode_bitmap_blocks;// 1
    uint64_t data_bitmap_start;  // 2
    uint64_t data_bitmap_blocks; // 1
    uint64_t inode_table_start;  // 3
    uint64_t inode_table_blocks; // Calculated
    uint64_t data_region_start;  // 3 + inode_table_blocks
    uint64_t data_region_blocks; // total_blocks - data_region_start
    uint64_t root_inode;         // 1
    uint64_t mtime_epoch;        // Build time (Unix Epoch)
    uint32_t flags;              // 0
    
    // THIS FIELD SHOULD STAY AT THE END
    // ALL OTHER FIELDS SHOULD BE ABOVE THIS
    uint32_t checksum;           // crc32(superblock[0..4091])
} superblock_t;
#pragma pack(pop)
_Static_assert(sizeof(superblock_t) == 116, "superblock must fit in one block");

#pragma pack(push,1)
typedef struct {
    uint16_t mode;               // (0100000)8 for files, (0040000)8 for directories
    uint16_t links;              // Number of links
    uint32_t uid;                // 0
    uint32_t gid;                // 0
    uint64_t size_bytes;         // Size in bytes
    uint64_t atime;              // Access time (Unix Epoch)
    uint64_t mtime;              // Modification time (Unix Epoch)
    uint64_t ctime;              // Creation time (Unix Epoch)
    uint32_t direct[12];         // Direct block pointers
    uint32_t reserved_0;         // 0
    uint32_t reserved_1;         // 0
    uint32_t reserved_2;         // 0
    uint32_t proj_id;            // Your group ID (set to 0 as not specified in CLI)
    uint32_t uid16_gid16;        // 0
    uint64_t xattr_ptr;          // 0
    
    // THIS FIELD SHOULD STAY AT THE END
    // ALL OTHER FIELDS SHOULD BE ABOVE THIS
    uint64_t inode_crc;          // low 4 bytes store crc32 of bytes [0..119]; high 4 bytes 0
} inode_t;
#pragma pack(pop)
_Static_assert(sizeof(inode_t)==INODE_SIZE, "inode size mismatch");

#pragma pack(push,1)
typedef struct {
    uint32_t inode_no;           // Inode number (0 if free)
    uint8_t type;                // 1=file, 2=dir
    char name[58];               // Name (null-terminated)
    
    uint8_t checksum;            // XOR of bytes 0..62
} dirent64_t;
#pragma pack(pop)
_Static_assert(sizeof(dirent64_t)==64, "dirent size mismatch");

// ==========================DO NOT CHANGE THIS PORTION=========================
// These functions are there for your help. You should refer to the specifications to see how you can use them.
// ====================================CRC32====================================
uint32_t CRC32_TAB[256];
void crc32_init(void){
    for (uint32_t i=0;i<256;i++){
        uint32_t c=i;
        for(int j=0;j<8;j++) c = (c&1)?(0xEDB88320u^(c>>1)):(c>>1);
        CRC32_TAB[i]=c;
    }
}
uint32_t crc32(const void* data, size_t n){
    const uint8_t* p=(const uint8_t*)data; uint32_t c=0xFFFFFFFFu;
    for(size_t i=0;i<n;i++) c = CRC32_TAB[(c^p[i])&0xFF] ^ (c>>8);
    return c ^ 0xFFFFFFFFu;
}
// ====================================CRC32====================================

// WARNING: CALL THIS ONLY AFTER ALL OTHER SUPERBLOCK ELEMENTS HAVE BEEN FINALIZED
static uint32_t superblock_crc_finalize(superblock_t *sb) {
    sb->checksum = 0;
    uint32_t s = crc32((void *) sb, BS - 4);
    sb->checksum = s;
    return s;
}

// WARNING: CALL THIS ONLY AFTER ALL OTHER SUPERBLOCK ELEMENTS HAVE BEEN FINALIZED
void inode_crc_finalize(inode_t* ino){
    uint8_t tmp[INODE_SIZE]; memcpy(tmp, ino, INODE_SIZE);
    // zero crc area before computing
    memset(&tmp[120], 0, 8);
    uint32_t c = crc32(tmp, 120);
    ino->inode_crc = (uint64_t)c; // low 4 bytes carry the crc
}

// WARNING: CALL THIS ONLY AFTER ALL OTHER SUPERBLOCK ELEMENTS HAVE BEEN FINALIZED
void dirent_checksum_finalize(dirent64_t* de) {
    const uint8_t* p = (const uint8_t*)de;
    uint8_t x = 0;
    for (int i = 0; i < 63; i++) x ^= p[i];   // covers ino(4) + type(1) + name(58)
    de->checksum = x;
}

int main(int argc, char *argv[]) {
    crc32_init();
    
    // Parse CLI parameters
    char *image_name = NULL;
    uint64_t size_kib = 0;
    uint64_t inodes = 0;
    
    for (int i = 1; i < argc; i += 2) {
        if (strcmp(argv[i], "--image") == 0 && i + 1 < argc) {
            image_name = argv[i + 1];
        } else if (strcmp(argv[i], "--size-kib") == 0 && i + 1 < argc) {
            size_kib = strtoull(argv[i + 1], NULL, 10);
        } else if (strcmp(argv[i], "--inodes") == 0 && i + 1 < argc) {
            inodes = strtoull(argv[i + 1], NULL, 10);
        } else {
            fprintf(stderr, "Error in parsing arguments.\n");
            fprintf(stderr, "Usage: mkfs_builder --image out.img --size-kib <180..4096> --inodes <128..512>\n");
            return 1;
        }
    }
    
    // Validate parameters
    if (image_name == NULL || size_kib == 0 || inodes == 0) {
        fprintf(stderr, "Error: Missing required parameters.\n");
        fprintf(stderr, "Usage: mkfs_builder --image out.img --size-kib <180..4096> --inodes <128..512>\n");
        return 1;
    }
    if (size_kib < 180 || size_kib > 4096 || size_kib % 4 != 0) {
        fprintf(stderr, "Error: --size-kib must be between 180 and 4096 (inclusive) and a multiple of 4.\n");
        return 1;
    }
    if (inodes < 128 || inodes > 512) {
        fprintf(stderr, "Error: --inodes must be between 128 and 512 (inclusive).\n");
        return 1;
    }
    
    // Calculate file system parameters
    uint64_t total_blocks = size_kib * (1024 / 4096);  
    uint64_t inode_table_blocks = (inodes * INODE_SIZE + BS - 1) / BS; // Ceiling division
    uint64_t data_region_start = 3 + inode_table_blocks;
    if (data_region_start >= total_blocks) {
        fprintf(stderr, "Error: Specified size is too small to accommodate the file system structures.\n");
        return 1;
    }
    uint64_t data_region_blocks = total_blocks - data_region_start;
    
    // Allocate memory for the entire disk image (zero-initialized)
    uint8_t *image = calloc(total_blocks, BS);
    if (image == NULL) {
        fprintf(stderr, "Error: Failed to allocate memory for disk image.\n");
        return 1;
    }
    
    // Set up superblock (at block 0)
    superblock_t *sb = (superblock_t *)image;
    sb->magic = 0x4D565346;
    sb->version = 1;
    sb->block_size = BS;
    sb->total_blocks = total_blocks;
    sb->inode_count = inodes;
    sb->inode_bitmap_start = 1;
    sb->inode_bitmap_blocks = 1;
    sb->data_bitmap_start = 2;
    sb->data_bitmap_blocks = 1;
    sb->inode_table_start = 3;
    sb->inode_table_blocks = inode_table_blocks;
    sb->data_region_start = data_region_start;
    sb->data_region_blocks = data_region_blocks;
    sb->root_inode = ROOT_INO;
    time_t now = time(NULL);
    sb->mtime_epoch = (uint64_t)now;
    sb->flags = 0;
    superblock_crc_finalize(sb);
    
    // Set up inode table (starting at block 3)
    inode_t *inode_table = (inode_t *)(image + 3 * BS);
    inode_t *root = &inode_table[0];  // Inode 1 is at index 0 (1-indexed)
    
    root->mode = 040000;  // (0040000)8 for directory
    root->links = 2;      // . and ..
    root->uid = 0;
    root->gid = 0;
    root->size_bytes = 128;  // 2 dirents * 64 bytes
    root->atime = (uint64_t)now;
    root->mtime = (uint64_t)now;
    root->ctime = (uint64_t)now;
    root->direct[0] = (uint32_t)data_region_start;  // First data block for root dir
    for (int i = 1; i < 12; i++) {
        root->direct[i] = 0;
    }
    root->reserved_0 = 0;
    root->reserved_1 = 0;
    root->reserved_2 = 0;
    root->proj_id = 1;    
    root->uid16_gid16 = 0;
    root->xattr_ptr = 0;
    inode_crc_finalize(root);
    
    // Set up inode bitmap (at block 1): Mark inode 1 as allocated (bit 0)
    uint8_t *inode_bitmap = image + 1 * BS;
    inode_bitmap[0] |= (1 << 0);  // Bit 0 for inode 1
    
    // Set up data bitmap (at block 2): Mark first data block as allocated (bit 0)
    uint8_t *data_bitmap = image + 2 * BS;
    data_bitmap[0] |= (1 << 0);  // Bit 0 for first data block
    
    // Set up root directory data (in first data block)
    uint8_t *root_data = image + data_region_start * BS;
    dirent64_t *de_dot = (dirent64_t *)root_data;
    de_dot->inode_no = ROOT_INO;
    de_dot->type = 2;  // Directory
    strcpy(de_dot->name, ".");
    dirent_checksum_finalize(de_dot);
    
    dirent64_t *de_dotdot = (dirent64_t *)(root_data + 64);
    de_dotdot->inode_no = ROOT_INO;
    de_dotdot->type = 2;  // Directory
    strcpy(de_dotdot->name, "..");
    dirent_checksum_finalize(de_dotdot);
    
    // Write the image to file
    FILE *fp = fopen(image_name, "wb");
    if (fp == NULL) {
        fprintf(stderr, "Error: Failed to open output file '%s': %s\n", image_name, strerror(errno));
        free(image);
        return 1;
    }

    //change the logic here to something simple
    
    size_t written = fwrite(image, BS, total_blocks, fp);
    if (written != total_blocks) {
        fprintf(stderr, "Error: Failed to write disk image to '%s'.\n", image_name);
        fclose(fp);
        free(image);
        return 1;
    }
    fclose(fp);
    free(image);
    
    return 0;
}
