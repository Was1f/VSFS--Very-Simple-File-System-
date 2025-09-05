// Build: gcc -O2 -std=c17 -Wall -Wextra mkfs_adder.c -o mkfs_adder

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

#define BS 4096u
#define INODE_SIZE 128u
#define ROOT_INO 1u
#define DIRECT_MAX 12
#define DIRENT_SIZE 64u
#define MAX_NAME_LEN 57  // 58 bytes including null terminator

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
    uint32_t proj_id;            // Your group ID (set to 1 for consistency with builder)
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

// Helper to find first free bit in bitmap (returns bit index or -1 if none)
static int find_first_free_bit(uint8_t *bitmap, uint64_t max_bits, uint64_t start_bit) {
    for (uint64_t bit = start_bit; bit < max_bits; bit++) {
        uint64_t byte_idx = bit / 8;
        uint64_t bit_idx = bit % 8;
        if ((bitmap[byte_idx] & (1 << bit_idx)) == 0) {
            return (int)bit;
        }
    }
    return -1;
}

// Helper to set bit in bitmap
static void set_bit(uint8_t *bitmap, uint64_t bit, int value) {
    uint64_t byte_idx = bit / 8;
    uint64_t bit_idx = bit % 8;
    if (value) {
        bitmap[byte_idx] |= (1 << bit_idx);
    } else {
        bitmap[byte_idx] &= ~(1 << bit_idx);
    }
}

int main(int argc, char *argv[]) {
    crc32_init();
    
    // Parse CLI parameters
    char *input_name = NULL;
    char *output_name = NULL;
    char *file_to_add = NULL;
    
    for (int i = 1; i < argc; i += 2) {
        if (strcmp(argv[i], "--input") == 0 && i + 1 < argc) {
            input_name = argv[i + 1];
        } else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            output_name = argv[i + 1];
        } else if (strcmp(argv[i], "--file") == 0 && i + 1 < argc) {
            file_to_add = argv[i + 1];
        } else {
            fprintf(stderr, "Error: Invalid argument: %s\n", argv[i]);
            fprintf(stderr, "Usage: mkfs_adder --input in.img --output out.img --file <file>\n");
            return 1;
        }
    }
    
    // Validate parameters
    if (input_name == NULL || output_name == NULL || file_to_add == NULL) {
        fprintf(stderr, "Error: Missing required parameters.\n");
        fprintf(stderr, "Usage: mkfs_adder --input in.img --output out.img --file <file>\n");
        return 1;
    }
    
    // Open and read input image
    FILE *input_fp = fopen(input_name, "rb");
    if (input_fp == NULL) {
        fprintf(stderr, "Error: Failed to open input file '%s': %s\n", input_name, strerror(errno));
        return 1;
    }
    fseek(input_fp, 0, SEEK_END);
    long image_size = ftell(input_fp);
    rewind(input_fp);
    if (image_size % BS != 0) {
        fprintf(stderr, "Error: Input image size is not a multiple of block size (%u).\n", BS);
        fclose(input_fp);
        return 1;
    }
    uint64_t total_blocks = image_size / BS;
    uint8_t *image = malloc(image_size);
    if (image == NULL) {
        fprintf(stderr, "Error: Failed to allocate memory for disk image.\n");
        fclose(input_fp);
        return 1;
    }
    size_t read_blocks = fread(image, BS, total_blocks, input_fp);
    fclose(input_fp);
    if (read_blocks != total_blocks) {
        fprintf(stderr, "Error: Failed to read input image '%s'.\n", input_name);
        free(image);
        return 1;
    }
    
    // Access superblock
    superblock_t *sb = (superblock_t *)image;
    if (sb->magic != 0x4D565346) {
        fprintf(stderr, "Error: Invalid MiniVSFS image (bad magic).\n");
        free(image);
        return 1;
    }
    
    // Get current time for updates
    time_t now = time(NULL);
    
    // Open file to add
    FILE *file_fp = fopen(file_to_add, "rb");
    if (file_fp == NULL) {
        fprintf(stderr, "Error: Failed to open file to add '%s': %s\n", file_to_add, strerror(errno));
        free(image);
        return 1;
    }
    fseek(file_fp, 0, SEEK_END);
    uint64_t file_size = ftell(file_fp);
    rewind(file_fp);
    uint8_t *file_content = malloc(file_size);
    if (file_content == NULL) {
        fprintf(stderr, "Error: Failed to allocate memory for file content.\n");
        fclose(file_fp);
        free(image);
        return 1;
    }
    size_t read_bytes = fread(file_content, 1, file_size, file_fp);
    fclose(file_fp);
    if (read_bytes != file_size) {
        fprintf(stderr, "Error: Failed to read file '%s'.\n", file_to_add);
        free(file_content);
        free(image);
        return 1;
    }
    
    // Check file name length
    const char *filename = strrchr(file_to_add, '/');
    filename = (filename) ? filename + 1 : file_to_add;
    if (strlen(filename) > MAX_NAME_LEN) {
        fprintf(stderr, "Error: File name '%s' is too long (max %d characters).\n", filename, MAX_NAME_LEN);
        free(file_content);
        free(image);
        return 1;
    }
    
    // Calculate needed data blocks
    uint64_t needed_blocks = (file_size + BS - 1) / BS;
    if (needed_blocks > DIRECT_MAX) {
        fprintf(stderr, "Warning: File '%s' is too large (requires %llu blocks, max %d). Cannot add.\n", filename, (unsigned long long)needed_blocks, DIRECT_MAX);
        free(file_content);
        free(image);
        return 1;
    }
    
    // Access bitmaps and inode table
    uint8_t *inode_bitmap = image + sb->inode_bitmap_start * BS;
    uint8_t *data_bitmap = image + sb->data_bitmap_start * BS;
    inode_t *inode_table = (inode_t *)(image + sb->inode_table_start * BS);
    inode_t *root = &inode_table[ROOT_INO - 1];  // Root inode at index 0
    
    // Find free inode (first-fit, starting after root)
    uint64_t max_inode_bits = sb->inode_count;  // Since 1-indexed up to inode_count
    int free_inode_bit = find_first_free_bit(inode_bitmap, max_inode_bits, 1);  // Start from bit 1 (inode 2)
    if (free_inode_bit == -1) {
        fprintf(stderr, "Error: No free inodes available.\n");
        free(file_content);
        free(image);
        return 1;
    }
    uint64_t new_inode_num = free_inode_bit + 1;  // 1-indexed
    set_bit(inode_bitmap, free_inode_bit, 1);
    
    // Find free data blocks (first-fit, allocate needed_blocks)
    uint64_t max_data_bits = sb->data_region_blocks;
    uint32_t allocated_blocks[DIRECT_MAX] = {0};
    uint64_t allocated_count = 0;
    for (uint64_t i = 0; allocated_count < needed_blocks; i++) {  // Start from 0, but root may have taken some
        int free_data_bit = find_first_free_bit(data_bitmap, max_data_bits, i);
        if (free_data_bit == -1) {
            fprintf(stderr, "Error: Not enough free data blocks (need %llu, found %llu).\n", (unsigned long long)needed_blocks, (unsigned long long)allocated_count);
            // Rollback inode allocation
            set_bit(inode_bitmap, free_inode_bit, 0);
            free(file_content);
            free(image);
            return 1;
        }
        set_bit(data_bitmap, free_data_bit, 1);
        allocated_blocks[allocated_count++] = sb->data_region_start + free_data_bit;  // Absolute block number
        i = free_data_bit;  // Continue from next
    }
    
    // Set up new inode
    inode_t *new_inode = &inode_table[new_inode_num - 1];
    memset(new_inode, 0, INODE_SIZE);
    new_inode->mode = 0100000;  // Octal for file
    new_inode->links = 1;
    new_inode->uid = 0;
    new_inode->gid = 0;
    new_inode->size_bytes = file_size;
    new_inode->atime = (uint64_t)now;
    new_inode->mtime = (uint64_t)now;
    new_inode->ctime = (uint64_t)now;
    for (uint64_t i = 0; i < needed_blocks; i++) {
        new_inode->direct[i] = allocated_blocks[i];
    }
    new_inode->reserved_0 = 0;
    new_inode->reserved_1 = 0;
    new_inode->reserved_2 = 0;
    new_inode->proj_id = 1; 
    new_inode->uid16_gid16 = 0;
    new_inode->xattr_ptr = 0;
    inode_crc_finalize(new_inode);
    
    // Copy file content to allocated blocks
    for (uint64_t i = 0; i < needed_blocks; i++) {
        uint8_t *block_ptr = image + allocated_blocks[i] * BS;
        uint64_t copy_size = (i == needed_blocks - 1) ? (file_size % BS) : BS;
        if (copy_size == 0) copy_size = BS;  // Full last block if exact multiple
        memcpy(block_ptr, file_content + i * BS, copy_size);
    }
    
    // Update root inode: Increase links by 1 (as per PDF), update size, times
    root->links += 1;
    root->size_bytes += DIRENT_SIZE;
    root->mtime = (uint64_t)now;
    root->atime = (uint64_t)now;
    
    // Find free dirent slot in root's data blocks and check for duplicate name
    int found_slot = 0;
    dirent64_t *new_de = NULL;
    for (int d = 0; d < DIRECT_MAX; d++) {

        if (root->direct[d] == 0) continue;  // Unused direct
        uint8_t *dir_block = image + root->direct[d] * BS;
        for (uint64_t e = 0; e < BS / DIRENT_SIZE; e++) {
            dirent64_t *de = (dirent64_t *)(dir_block + e * DIRENT_SIZE);
            if (de->inode_no == 0) {  // Free slot
                if (!found_slot) {
                    new_de = de;
                    found_slot = 1;
                }
            } else if (strcmp(de->name, filename, 58) == 0) {
                fprintf(stderr, "Error: File '%s' already exists in root directory.\n", filename);
                // Rollback
                set_bit(inode_bitmap, free_inode_bit, 0);
                for (uint64_t i = 0; i < allocated_count; i++) {
                    uint64_t bit = allocated_blocks[i] - sb->data_region_start;
                    set_bit(data_bitmap, bit, 0);
                }
                free(file_content);
                free(image);
                return 1;
            }
        }
        if (found_slot) break;
    }
    if (!found_slot) {
        // Need to allocate new data block for root (first-fit)
        int free_data_bit = find_first_free_bit(data_bitmap, max_data_bits, 0);
        if (free_data_bit == -1) {
            fprintf(stderr, "Error: No free data blocks for root directory expansion.\n");
            // Rollback
            set_bit(inode_bitmap, free_inode_bit, 0);
            for (uint64_t i = 0; i < allocated_count; i++) {
                uint64_t bit = allocated_blocks[i] - sb->data_region_start;
                set_bit(data_bitmap, bit, 0);
            }
            free(file_content);
            free(image);
            return 1;
        }
        set_bit(data_bitmap, free_data_bit, 1);
        uint32_t new_dir_block_num = sb->data_region_start + free_data_bit;
        
        // Find free direct slot in root
        int free_direct = -1;
        for (int d = 0; d < DIRECT_MAX; d++) {
            if (root->direct[d] == 0) {
                free_direct = d;
                break;
            }
        }
        if (free_direct == -1) {
            fprintf(stderr, "Error: Root directory has no free direct pointers.\n");
            // Rollback
            set_bit(inode_bitmap, free_inode_bit, 0);
            set_bit(data_bitmap, free_data_bit, 0);
            for (uint64_t i = 0; i < allocated_count; i++) {
                uint64_t bit = allocated_blocks[i] - sb->data_region_start;
                set_bit(data_bitmap, bit, 0);
            }
            free(file_content);
            free(image);
            return 1;
        }
        root->direct[free_direct] = new_dir_block_num;
        uint8_t *new_dir_block = image + new_dir_block_num * BS;
        memset(new_dir_block, 0, BS);  // Zero the new block
        new_de = (dirent64_t *)new_dir_block;  // First slot
        root->size_bytes += BS;  // Adding a full block, but adjust if needed (per PDF initial logic)
    }
    
    // Set up new dirent
    new_de->inode_no = new_inode_num;
    new_de->type = 1;  // File
    strncpy(new_de->name, filename, 58);
    new_de->name[57] = '\0';  // Ensure null-terminated
    dirent_checksum_finalize(new_de);
    
    // Finalize root inode
    inode_crc_finalize(root);
    
    // Update superblock mtime and checksum
    sb->mtime_epoch = (uint64_t)now;
    superblock_crc_finalize(sb);
    
    // Write to output
    FILE *output_fp = fopen(output_name, "wb");
    if (output_fp == NULL) {
        fprintf(stderr, "Error: Failed to open output file '%s': %s\n", output_name, strerror(errno));
        free(file_content);
        free(image);
        return 1;
    }
    size_t written = fwrite(image, BS, total_blocks, output_fp);
    if (written != total_blocks) {
        fprintf(stderr, "Error: Failed to write output image to '%s'.\n", output_name);
        fclose(output_fp);
        free(file_content);
        free(image);
        return 1;
    }
    fclose(output_fp);
    
    free(file_content);
    free(image);
    return 0;
}
