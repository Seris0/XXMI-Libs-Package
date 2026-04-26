#pragma once

#include <stdint.h>
#include <windows.h>
#include <unordered_map>
#include <vector>

// Constants
#define SHADER_CACHE_SPLIT_MAGIC "3DMSPLIT"
#define SHADER_CACHE_SPLIT_BLOCK_MAGIC "3DMBLOCK"
#define SHADER_CACHE_SPLIT_VERSION 4
#define SHADERS_PER_BLOCK_FILE 100
#define SHADER_BLOCK_MAGIC 0x53444342
#define MAX_REGEX_MATCHES 10000
#define MAX_SHADER_BYTECODE_SIZE (256 * 1024 * 1024)
#define BLOCK_FLAG_USED 0x00000001
#define BLOCK_FLAG_HAS_ASM 0x00000002
#define BLOCK_FLAG_COMPRESSED 0x00000004
#define BLOCK_FLAG_REGEX_PATCH 0x00000008
#define ALIGN_4(x) (((x) + 3) & ~3)
#define ALIGN_16(x) (((x) + 15) & ~15)
#define DEFAULT_MAX_OPEN_BLOCK_FILES 10
#define DEFAULT_POOL_BLOCK_SIZE 65536
#define DEFAULT_MAX_POOL_BLOCKS 100

extern struct SplitShaderCache *G_SPLIT_SHADER_CACHE;

// Index file header (64 bytes)
#pragma pack(push, 4)
struct SplitCacheIndexHeader {
  char magic[8];
  uint32_t version;
  uint32_t shader_count;
  uint32_t index_count;
  uint32_t shaders_per_block;
  uint32_t block_file_count;
  uint32_t shader_regex_hash;
  uint8_t reserved[32];
};

struct SplitCacheIndexEntry {
  uint64_t hash;
  uint32_t type;
  uint32_t bytecode_size;
  uint32_t block_file_id;
  uint32_t block_offset;
  uint32_t flags;
  uint32_t reserved;
};

struct SplitCacheBlockFileHeader {
  char magic[8];
  uint32_t version;
  uint32_t block_id;
  uint32_t shader_count;
  uint32_t data_size;
  uint8_t reserved[8];
};

struct ShaderBlockHeader {
  uint32_t magic;
  uint32_t flags;
  uint64_t shader_hash;
  uint32_t shader_type;
  uint32_t bytecode_size;
  uint32_t total_size;
};
#pragma pack(pop)

struct SplitShaderCache {
  HANDLE index_file_handle;
  wchar_t index_path[MAX_PATH];
  wchar_t block_dir[MAX_PATH];
  
  SplitCacheIndexHeader header;
  
  std::vector<SplitCacheIndexEntry> index;
  std::unordered_map<uint64_t, size_t> hash_map;
  
  std::unordered_map<uint32_t, HANDLE> open_block_files;
  std::vector<uint32_t> block_file_lru;
  uint32_t max_open_block_files;
  
  struct BlockMapping {
    HANDLE file_mapping;
    void *view;
    SIZE_T view_size;
  };
  std::unordered_map<uint32_t, BlockMapping> block_mappings;
  bool use_memory_mapping;
  
  struct MemoryBlock {
    uint8_t *data;
    uint32_t size;
    bool in_use;
  };
  std::vector<MemoryBlock> memory_pool;
  uint32_t pool_block_size;
  uint32_t max_pool_blocks;
  bool use_memory_pool;
  
  bool dirty;
  bool read_only;
  
  CRITICAL_SECTION lock;
  
  uint64_t query_count;
  uint64_t insert_count;
  uint64_t hit_count;
  uint64_t miss_count;
  uint64_t block_file_opens;
  uint64_t mmap_reads;
  uint64_t file_reads;
  uint64_t pool_allocs;
  uint64_t heap_allocs;
};

// Function declarations
SplitShaderCache *InitSplitShaderCache(const wchar_t *cache_dir,
                                       uint32_t regex_hash, uint32_t shaders_per_block,
                                       uint32_t max_open_files, uint32_t pool_block_size,
                                       uint32_t max_pool_blocks, bool use_mmap, bool use_pool);

void CloseSplitShaderCache(SplitShaderCache *cache);

const void *QuerySplitShaderBytecode(SplitShaderCache *cache, uint64_t hash,
                                     const wchar_t *type, uint32_t *out_size);

const void *QuerySplitShaderRegexBytecode(SplitShaderCache *cache, uint64_t hash,
                                          const wchar_t *type, uint32_t expected_regex_hash,
                                          uint32_t *out_size, uint32_t *out_num_matches,
                                          uint32_t **out_match_ids);

void FreeSplitShaderBytecode(SplitShaderCache *cache, const void *bytecode);

bool InsertSplitShaderToCache(SplitShaderCache *cache, uint64_t hash,
                               const wchar_t *type, const void *bytecode,
                               uint32_t bytecode_size, FILETIME timestamp);

bool StoreSplitShaderRegexBytecode(SplitShaderCache *cache, uint64_t hash,
                                   const wchar_t *type, const void *bytecode,
                                   uint32_t bytecode_size, uint32_t num_matches,
                                   const uint32_t *match_ids);

bool MarkSplitShaderRegexProcessed(SplitShaderCache *cache, uint64_t hash,
                                   const wchar_t *type);

void GetSplitCacheStatistics(SplitShaderCache *cache,
                             uint32_t *out_shader_count,
                             uint32_t *out_block_file_count);

void LogSplitCacheStatistics(SplitShaderCache *cache);

uint32_t ValidateSplitCacheIntegrity(SplitShaderCache *cache);

bool FlushSplitCacheIndex(SplitShaderCache *cache);

uint32_t EncodeShaderType(const wchar_t *type);
