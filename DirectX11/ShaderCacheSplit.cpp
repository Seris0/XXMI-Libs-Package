#include "ShaderCacheSplit.h"
#include "log.h"
#include <algorithm>

// Utility functions for shader type encoding/decoding
uint32_t EncodeShaderType(const wchar_t* type) {
    if (!type || wcslen(type) == 0)
        return 0;

    // Convert to lowercase ASCII and pack into uint32
    uint32_t encoded = 0;
    for (int i = 0; i < 4 && type[i] != 0; i++) {
        wchar_t c = type[i];
        if (c >= L'A' && c <= L'Z')
            c = c - L'A' + L'a';
        encoded |= (c & 0xFF) << (i * 8);
    }
    return encoded;
}

void DecodeShaderType(uint32_t encoded, wchar_t* out_type, size_t out_size) {
    if (!out_type || out_size < 5)
        return;

    for (int i = 0; i < 4; i++) {
        uint8_t byte = (encoded >> (i * 8)) & 0xFF;
        if (byte == 0)
            break;
        out_type[i] = (wchar_t)byte;
    }
    out_type[4] = 0; // Null terminate
}

// Helper: Create directory recursively
static bool CreateDirectoryRecursive(const wchar_t* path) {
    wchar_t dir_path[MAX_PATH];
    wcsncpy_s(dir_path, MAX_PATH, path, _TRUNCATE);

    DWORD attribs = GetFileAttributesW(dir_path);
    if (attribs != INVALID_FILE_ATTRIBUTES && (attribs & FILE_ATTRIBUTE_DIRECTORY)) {
        return true;
    }

    if (CreateDirectoryW(dir_path, NULL)) {
        return true;
    }

    DWORD error = GetLastError();
    if (error == ERROR_PATH_NOT_FOUND) {
        wchar_t parent_path[MAX_PATH];
        wcsncpy_s(parent_path, MAX_PATH, dir_path, _TRUNCATE);
        wchar_t* last_slash = wcsrchr(parent_path, L'\\');
        if (last_slash) {
            *last_slash = L'\0';
            if (CreateDirectoryRecursive(parent_path)) {
                if (CreateDirectoryW(dir_path, NULL)) {
                    return true;
                }
            }
        }
    }
    else if (error == ERROR_ALREADY_EXISTS) {
        return true;
    }

    return false;
}

// Helper: Get block file path
static void GetBlockFilePath(const SplitShaderCache* cache, uint32_t block_id,
    wchar_t* out_path, size_t out_size) {
    swprintf_s(out_path, out_size, L"%ls\\ShaderCache_%04u.bin",
        cache->block_dir, block_id);
}

// Forward declaration for EvictLRUBlockFile
static void EvictLRUBlockFile(SplitShaderCache* cache);

// Helper: Open or get cached block file handle
// NOTE: Must be called while holding cache->lock
static HANDLE GetBlockFileHandle(SplitShaderCache* cache, uint32_t block_id,
    bool create_if_missing) {
    // If we need write access, we might need to reopen the file with write permissions
    if (create_if_missing) {
        auto it = cache->open_block_files.find(block_id);
        if (it != cache->open_block_files.end()) {
            // File is already open. For writes, we need read+write access.
            // The current handle might be read-only. We need to close and reopen.
            CloseHandle(it->second);
            cache->open_block_files.erase(it);
            // Remove from LRU list
            cache->block_file_lru.erase(
                std::remove(cache->block_file_lru.begin(),
                    cache->block_file_lru.end(), block_id),
                cache->block_file_lru.end());
            // Fall through to reopen with write access
        }
    }
    else {
        // Check if already open
        auto it = cache->open_block_files.find(block_id);
        if (it != cache->open_block_files.end()) {
            // Move to front of LRU if not already at front
            if (!cache->block_file_lru.empty() && cache->block_file_lru.front() != block_id) {
                cache->block_file_lru.erase(
                    std::remove(cache->block_file_lru.begin(),
                        cache->block_file_lru.end(), block_id),
                    cache->block_file_lru.end());
                cache->block_file_lru.insert(cache->block_file_lru.begin(), block_id);
            }
            return it->second;
        }
    }

    // If we have too many open files, evict the least recently used
    while (cache->open_block_files.size() >= cache->max_open_block_files &&
        !cache->block_file_lru.empty()) {
        EvictLRUBlockFile(cache);
    }

    // Need to open new file
    wchar_t block_path[MAX_PATH];
    GetBlockFilePath(cache, block_id, block_path, MAX_PATH);

    DWORD access = create_if_missing ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ;
    DWORD creation = create_if_missing ? OPEN_ALWAYS : OPEN_EXISTING;
    HANDLE handle = CreateFileW(block_path, access, FILE_SHARE_READ, NULL,
        creation, FILE_ATTRIBUTE_NORMAL, NULL);

    if (handle == INVALID_HANDLE_VALUE) {
        return INVALID_HANDLE_VALUE;
    }

    cache->block_file_opens++;

    // Add to cache
    cache->open_block_files[block_id] = handle;
    cache->block_file_lru.insert(cache->block_file_lru.begin(), block_id);

    return handle;
}

// Helper: Get or create memory-mapped view of a block file
// Returns pointer to mapped memory, or NULL on failure
// NOTE: This function must be called while holding the cache->lock
static void* GetBlockMemoryMapping(SplitShaderCache* cache, uint32_t block_id) {
    if (!cache->use_memory_mapping)
        return NULL;

    // Check if already mapped
    auto it = cache->block_mappings.find(block_id);
    if (it != cache->block_mappings.end()) {
        return it->second.view;
    }

    // Get file handle (this will open it if needed)
    HANDLE file_handle = GetBlockFileHandle(cache, block_id, false);
    if (file_handle == INVALID_HANDLE_VALUE)
        return NULL;

    // Get file size
    LARGE_INTEGER file_size;
    if (!GetFileSizeEx(file_handle, &file_size))
        return NULL;

    if (file_size.QuadPart == 0)
        return NULL; // Empty file

    // Create file mapping
    HANDLE file_mapping = CreateFileMappingW(
        file_handle, NULL, PAGE_READONLY, 0, 0, NULL);

    if (file_mapping == NULL)
        return NULL;

    // Map view of file
    void* view = MapViewOfFile(file_mapping, FILE_MAP_READ, 0, 0, 0);

    if (view == NULL) {
        CloseHandle(file_mapping);
        return NULL;
    }

    // Store mapping info
    SplitShaderCache::BlockMapping mapping;
    mapping.file_mapping = file_mapping;
    mapping.view = view;
    mapping.view_size = (SIZE_T)file_size.QuadPart;

    cache->block_mappings[block_id] = mapping;

    return view;
}

// Helper: Unmap a block file (called when closing or evicting from cache)
static void UnmapBlockFile(SplitShaderCache* cache, uint32_t block_id) {
    auto it = cache->block_mappings.find(block_id);
    if (it != cache->block_mappings.end()) {
        UnmapViewOfFile(it->second.view);
        CloseHandle(it->second.file_mapping);
        cache->block_mappings.erase(it);
    }
}

// Helper: Update block file header after writing shader data
// NOTE: Must be called while holding cache->lock
static bool UpdateBlockFileHeader(SplitShaderCache* cache, HANDLE block_file,
    uint32_t block_id, uint32_t shader_count,
    uint32_t data_size) {
    SplitCacheBlockFileHeader header;
    memset(&header, 0, sizeof(SplitCacheBlockFileHeader));
    memcpy(header.magic, SHADER_CACHE_SPLIT_BLOCK_MAGIC, 8);
    header.version = SHADER_CACHE_SPLIT_VERSION;
    header.block_id = block_id;
    header.shader_count = shader_count;
    header.data_size = data_size;

    DWORD written;
    SetFilePointer(block_file, 0, NULL, FILE_BEGIN);
    if (!WriteFile(block_file, &header, sizeof(SplitCacheBlockFileHeader), &written, NULL) ||
        written != sizeof(SplitCacheBlockFileHeader)) {
        LogInfo("SplitCache: Failed to update block file header for block %u\n", block_id);
        return false;
    }
    return true;
}

// Helper: Evict LRU block file and its memory mapping
static void EvictLRUBlockFile(SplitShaderCache* cache) {
    if (cache->block_file_lru.empty())
        return;

    uint32_t lru_block_id = cache->block_file_lru.back();

    // Unmap memory mapping if exists
    UnmapBlockFile(cache, lru_block_id);

    // Close file handle
    auto it = cache->open_block_files.find(lru_block_id);
    if (it != cache->open_block_files.end()) {
        CloseHandle(it->second);
        cache->open_block_files.erase(it);
    }

    cache->block_file_lru.pop_back();
}

// Memory pool: Allocate bytecode buffer from pool or heap
// Returns allocated buffer that must be freed with FreePoolMemory()
static uint8_t* AllocPoolMemory(SplitShaderCache* cache, uint32_t size) {
    if (!cache->use_memory_pool || size > cache->pool_block_size) {
        // Size too large for pool, use heap
        cache->heap_allocs++;
        return new uint8_t[size];
    }

    // Try to find free block in pool
    for (auto& block : cache->memory_pool) {
        if (!block.in_use && block.size >= size) {
            block.in_use = true;
            cache->pool_allocs++;
            return block.data;
        }
    }

    // No free block available, check if we can grow pool
    if (cache->memory_pool.size() < cache->max_pool_blocks) {
        SplitShaderCache::MemoryBlock new_block;
        new_block.data = new uint8_t[cache->pool_block_size];
        new_block.size = cache->pool_block_size;
        new_block.in_use = true;
        cache->memory_pool.push_back(new_block);
        cache->pool_allocs++;
        return new_block.data;
    }

    // Pool is full, fallback to heap
    cache->heap_allocs++;
    return new uint8_t[size];
}

// Memory pool: Free bytecode buffer (return to pool or heap)
static void FreePoolMemory(SplitShaderCache* cache, uint8_t* ptr) {
    if (!ptr)
        return;

    // Check if pointer belongs to pool
    for (auto& block : cache->memory_pool) {
        if (block.data == ptr) {
            block.in_use = false;
            return;
        }
    }

    // Not in pool, must be heap allocation
    delete[] ptr;
}

// Initialize split shader cache
SplitShaderCache* InitSplitShaderCache(const wchar_t* cache_dir,
    uint32_t regex_hash,
    uint32_t shaders_per_block,
    uint32_t max_open_files,
    uint32_t pool_block_size,
    uint32_t max_pool_blocks,
    bool use_mmap,
    bool use_pool) {
    if (!cache_dir) {
        LogInfo("SplitCache: Invalid cache directory\n");
        return NULL;
    }

    LogInfo("SplitCache: Initializing at %ls\n", cache_dir);

    // Create cache directory
    if (!CreateDirectoryRecursive(cache_dir)) {
        LogInfo("SplitCache: Failed to create cache directory\n");
        return NULL;
    }

    SplitShaderCache* cache = new SplitShaderCache();
    // Initialize fields (don't use memset on C++ objects!)
    wcsncpy_s(cache->block_dir, MAX_PATH, cache_dir, _TRUNCATE);
    swprintf_s(cache->index_path, MAX_PATH, L"%ls\\ShaderCache.idx", cache_dir);

    cache->index_file_handle = INVALID_HANDLE_VALUE;
    cache->max_open_block_files = max_open_files > 0 ? max_open_files : DEFAULT_MAX_OPEN_BLOCK_FILES;
    cache->dirty = false;
    cache->read_only = false;
    cache->use_memory_mapping = use_mmap;

    // Initialize memory pool
    cache->pool_block_size = pool_block_size > 0 ? pool_block_size : DEFAULT_POOL_BLOCK_SIZE;
    cache->max_pool_blocks = max_pool_blocks > 0 ? max_pool_blocks : DEFAULT_MAX_POOL_BLOCKS;
    cache->use_memory_pool = use_pool;

    // Initialize statistics
    cache->query_count = 0;
    cache->insert_count = 0;
    cache->hit_count = 0;
    cache->miss_count = 0;
    cache->block_file_opens = 0;
    cache->mmap_reads = 0;
    cache->file_reads = 0;
    cache->pool_allocs = 0;
    cache->heap_allocs = 0;

    LogInfo("SplitCache: Initializing critical section...\n");
    InitializeCriticalSection(&cache->lock);

    LogInfo("SplitCache: Opening index file...\n");

    // Try to open existing index file
    cache->index_file_handle = CreateFileW(
        cache->index_path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

    bool need_new_index = false;

    if (cache->index_file_handle != INVALID_HANDLE_VALUE) {
        // Read existing index
        DWORD read;
        if (!ReadFile(cache->index_file_handle, &cache->header,
            sizeof(SplitCacheIndexHeader), &read, NULL) ||
            read != sizeof(SplitCacheIndexHeader)) {
            LogInfo("SplitCache: Failed to read index header\n");
            CloseHandle(cache->index_file_handle);
            need_new_index = true;
        }
        else if (strncmp(cache->header.magic, SHADER_CACHE_SPLIT_MAGIC, 8) != 0) {
            LogInfo("SplitCache: Invalid magic number\n");
            CloseHandle(cache->index_file_handle);
            need_new_index = true;
        }
        else if (cache->header.version != SHADER_CACHE_SPLIT_VERSION) {
            LogInfo("SplitCache: Version mismatch (got %d, expected %d)\n",
                cache->header.version, SHADER_CACHE_SPLIT_VERSION);
            CloseHandle(cache->index_file_handle);
            need_new_index = true;
        }
        else {
            // Load index entries
            cache->index.resize(cache->header.index_count);
            if (cache->header.index_count > 0) {
                DWORD bytes_to_read = cache->header.index_count * sizeof(SplitCacheIndexEntry);
                if (!ReadFile(cache->index_file_handle, cache->index.data(),
                    bytes_to_read, &read, NULL) || read != bytes_to_read) {
                    LogInfo("SplitCache: Failed to read index entries\n");
                    cache->index.clear();
                    CloseHandle(cache->index_file_handle);
                    need_new_index = true;
                }
                else {
                    // Build hash map
                    for (size_t i = 0; i < cache->index.size(); i++) {
                        auto& entry = cache->index[i];
                        uint64_t key = entry.hash | ((uint64_t)entry.type << 32);
                        cache->hash_map[key] = i;
                    }
                    LogInfo("SplitCache: Loaded %u shaders from %u block files\n",
                        cache->header.shader_count, cache->header.block_file_count);
                }
            }
            LogInfo("SplitCache: Finished loading index\n");
        }
    }
    else {
        LogInfo("SplitCache: Index file doesn't exist\n");
        need_new_index = true;
    }

    LogInfo("SplitCache: Checking if new index needed (need_new_index=%d)...\n", need_new_index);

    // Create new index if needed
    if (need_new_index) {
        LogInfo("SplitCache: Creating new index file\n");
        cache->index_file_handle = CreateFileW(
            cache->index_path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
            NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

        if (cache->index_file_handle == INVALID_HANDLE_VALUE) {
            LogInfo("SplitCache: Failed to create index file (error=%lu)\n", GetLastError());
            DeleteCriticalSection(&cache->lock);
            delete cache;
            return NULL;
        }

        // Initialize header
        memset(&cache->header, 0, sizeof(SplitCacheIndexHeader));
        memcpy(cache->header.magic, SHADER_CACHE_SPLIT_MAGIC, 8);  // Copy exactly 8 bytes, no null terminator
        cache->header.version = SHADER_CACHE_SPLIT_VERSION;
        cache->header.shader_count = 0;
        cache->header.index_count = 0;
        cache->header.shaders_per_block = shaders_per_block;
        cache->header.block_file_count = 0;
        cache->header.shader_regex_hash = regex_hash;

        // Write header
        DWORD written;
        SetFilePointer(cache->index_file_handle, 0, NULL, FILE_BEGIN);
        if (!WriteFile(cache->index_file_handle, &cache->header,
            sizeof(SplitCacheIndexHeader), &written, NULL) ||
            written != sizeof(SplitCacheIndexHeader)) {
            LogInfo("SplitCache: Failed to write index header\n");
            CloseHandle(cache->index_file_handle);
            DeleteCriticalSection(&cache->lock);
            delete cache;
            return NULL;
        }

        LogInfo("SplitCache: Created new index file\n");
    }

    // Update regex hash if changed
    if (cache->header.shader_regex_hash != regex_hash) {
        LogInfo("SplitCache: Regex hash changed (0x%08x -> 0x%08x) - invalidating cached regex shaders\n",
            cache->header.shader_regex_hash, regex_hash);
        cache->header.shader_regex_hash = regex_hash;
        cache->dirty = true;
        // Clear regex flag on all shaders since regex patterns have changed
        for (size_t i = 0; i < cache->index.size(); i++) {
            if (cache->index[i].flags & BLOCK_FLAG_REGEX_PATCH) {
                cache->index[i].flags &= ~BLOCK_FLAG_REGEX_PATCH;
            }
        }
    }

    return cache;
}

// Close split shader cache
void CloseSplitShaderCache(SplitShaderCache* cache) {
    if (!cache)
        return;

    // Flush index if dirty
    if (cache->dirty) {
        FlushSplitCacheIndex(cache);
    }

    // Unmap all memory-mapped block files
    for (auto& pair : cache->block_mappings) {
        UnmapViewOfFile(pair.second.view);
        CloseHandle(pair.second.file_mapping);
    }
    cache->block_mappings.clear();

    // Close all open block files
    for (auto& pair : cache->open_block_files) {
        CloseHandle(pair.second);
    }
    cache->open_block_files.clear();

    // Close index file
    if (cache->index_file_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(cache->index_file_handle);
    }

    // Free memory pool
    // Check for memory leaks (blocks still in use)
    uint32_t blocks_in_use = 0;
    for (auto& block : cache->memory_pool) {
        if (block.in_use) {
            blocks_in_use++;
            LogInfo("SplitCache: WARNING - Memory pool block still in use during cleanup (potential memory leak)\n");
        }
        delete[] block.data;
    }
    if (blocks_in_use > 0) {
        LogInfo("SplitCache: WARNING - %u memory pool blocks were still in use during cleanup\n", blocks_in_use);
    }
    cache->memory_pool.clear();

    DeleteCriticalSection(&cache->lock);
    delete cache;
}

// Flush index to disk
bool FlushSplitCacheIndex(SplitShaderCache* cache) {
    if (!cache || !cache->dirty)
        return true;

    EnterCriticalSection(&cache->lock);

    // Write header
    DWORD written;
    SetFilePointer(cache->index_file_handle, 0, NULL, FILE_BEGIN);
    if (!WriteFile(cache->index_file_handle, &cache->header,
        sizeof(SplitCacheIndexHeader), &written, NULL) ||
        written != sizeof(SplitCacheIndexHeader)) {
        LogInfo("SplitCache: Failed to write index header\n");
        LeaveCriticalSection(&cache->lock);
        return false;
    }

    // Write index entries
    if (cache->index.size() > 0) {
        DWORD bytes_to_write = cache->index.size() * sizeof(SplitCacheIndexEntry);
        if (!WriteFile(cache->index_file_handle, cache->index.data(),
            bytes_to_write, &written, NULL) || written != bytes_to_write) {
            LogInfo("SplitCache: Failed to write index entries\n");
            LeaveCriticalSection(&cache->lock);
            return false;
        }
    }

    FlushFileBuffers(cache->index_file_handle);
    cache->dirty = false;

    LeaveCriticalSection(&cache->lock);
    return true;
}

// Query shader bytecode from cache
const void* QuerySplitShaderBytecode(SplitShaderCache* cache, uint64_t hash,
    const wchar_t* type, uint32_t* out_size) {
    if (!cache || !type || !out_size)
        return NULL;

    uint32_t type_encoded = EncodeShaderType(type);
    uint64_t key = hash | ((uint64_t)type_encoded << 32);

    EnterCriticalSection(&cache->lock);
    cache->query_count++;

    // Look up in hash map
    auto it = cache->hash_map.find(key);
    if (it == cache->hash_map.end()) {
        cache->miss_count++;
        LeaveCriticalSection(&cache->lock);
        return NULL;
    }

    SplitCacheIndexEntry* entry = &cache->index[it->second];
    cache->hit_count++;

    // Try memory-mapped I/O first (fast path)
    void* mapped_view = GetBlockMemoryMapping(cache, entry->block_file_id);
    if (mapped_view != NULL) {
        // Verify offset is within mapped region
        auto mapping_it = cache->block_mappings.find(entry->block_file_id);
        if (mapping_it != cache->block_mappings.end() &&
            entry->block_offset + sizeof(ShaderBlockHeader) <= mapping_it->second.view_size) {

            // Read block header directly from memory
            uint8_t* block_ptr = (uint8_t*)mapped_view + entry->block_offset;
            ShaderBlockHeader* block_header = (ShaderBlockHeader*)block_ptr;

            // Validate block header
            if (block_header->magic == SHADER_BLOCK_MAGIC && block_header->shader_hash == hash) {
                // Validate bytecode size
                if (block_header->bytecode_size > MAX_SHADER_BYTECODE_SIZE) {
                    LogInfo("SplitCache: Invalid bytecode size (%u > %u) - possible corrupted cache\n",
                        block_header->bytecode_size, MAX_SHADER_BYTECODE_SIZE);
                    LeaveCriticalSection(&cache->lock);
                    return NULL;
                }

                // Calculate bytecode offset
                size_t data_offset = sizeof(ShaderBlockHeader);

                // Skip regex metadata if present
                if (block_header->flags & BLOCK_FLAG_REGEX_PATCH) {
                    // Validate num_matches bounds before skipping
                    uint32_t* num_matches_ptr = (uint32_t*)(block_ptr + data_offset);
                    uint32_t num_matches = *num_matches_ptr;

                    // SECURITY: Validate num_matches against reasonable maximum
                    if (num_matches > MAX_REGEX_MATCHES) {
                        LogInfo("SplitCache: num_matches exceeds maximum (%u > %u) - possible corrupted cache\n",
                            num_matches, MAX_REGEX_MATCHES);
                        LeaveCriticalSection(&cache->lock);
                        return NULL;
                    }

                    // Bounds check: ensure we have enough data
                    size_t match_ids_size = (size_t)num_matches * sizeof(uint32_t);
                    if (data_offset + sizeof(uint32_t) + match_ids_size + block_header->bytecode_size > mapping_it->second.view_size - entry->block_offset) {
                        LogInfo("SplitCache: Insufficient data for regex metadata\n");
                        LeaveCriticalSection(&cache->lock);
                        return NULL;
                    }

                    data_offset += sizeof(uint32_t) + match_ids_size;
                }

                // Verify we have enough data
                if (entry->block_offset + data_offset + block_header->bytecode_size <= mapping_it->second.view_size) {
                    // Allocate and copy bytecode from pool
                    uint8_t* bytecode = AllocPoolMemory(cache, block_header->bytecode_size);
                    memcpy(bytecode, block_ptr + data_offset, block_header->bytecode_size);

                    *out_size = block_header->bytecode_size;
                    cache->mmap_reads++;  // Count memory-mapped read
                    LeaveCriticalSection(&cache->lock);
                    return bytecode;
                }
            }
        }
    }

    // Fallback to traditional file I/O if memory mapping failed
    cache->file_reads++;  // Count traditional file I/O read
    HANDLE block_file = GetBlockFileHandle(cache, entry->block_file_id, false);
    if (block_file == INVALID_HANDLE_VALUE) {
        LogInfo("SplitCache: Failed to open block file %u\n", entry->block_file_id);
        LeaveCriticalSection(&cache->lock);
        return NULL;
    }

    // Seek to shader block
    LARGE_INTEGER offset;
    offset.QuadPart = entry->block_offset;
    if (!SetFilePointerEx(block_file, offset, NULL, FILE_BEGIN)) {
        LogInfo("SplitCache: Failed to seek in block file\n");
        LeaveCriticalSection(&cache->lock);
        return NULL;
    }

    // Read shader block header
    ShaderBlockHeader block_header;
    DWORD read;
    if (!ReadFile(block_file, &block_header, sizeof(ShaderBlockHeader), &read,
        NULL) ||
        read != sizeof(ShaderBlockHeader)) {
        LogInfo("SplitCache: Failed to read shader block header\n");
        LeaveCriticalSection(&cache->lock);
        return NULL;
    }

    // Validate block header
    if (block_header.magic != SHADER_BLOCK_MAGIC || block_header.shader_hash != hash) {
        LogInfo("SplitCache: Invalid shader block header\n");
        LeaveCriticalSection(&cache->lock);
        return NULL;
    }

    // Validate bytecode size
    if (block_header.bytecode_size > MAX_SHADER_BYTECODE_SIZE) {
        LogInfo("SplitCache: Invalid bytecode size (%u > %u) - possible corrupted cache\n",
            block_header.bytecode_size, MAX_SHADER_BYTECODE_SIZE);
        LeaveCriticalSection(&cache->lock);
        return NULL;
    }

    // Allocate buffer for bytecode from pool
    uint8_t* bytecode = AllocPoolMemory(cache, block_header.bytecode_size);

    // Skip regex metadata if present
    if (block_header.flags & BLOCK_FLAG_REGEX_PATCH) {
        uint32_t num_matches;
        if (!ReadFile(block_file, &num_matches, sizeof(uint32_t), &read, NULL) ||
            read != sizeof(uint32_t)) {
            FreePoolMemory(cache, bytecode);
            LeaveCriticalSection(&cache->lock);
            return NULL;
        }
        // SECURITY: Validate num_matches against reasonable maximum
        if (num_matches > MAX_REGEX_MATCHES) {
            LogInfo("SplitCache: num_matches exceeds maximum (%u > %u) - possible corrupted cache\n",
                num_matches, MAX_REGEX_MATCHES);
            FreePoolMemory(cache, bytecode);
            LeaveCriticalSection(&cache->lock);
            return NULL;
        }
        // Skip match IDs
        if (num_matches > 0) {
            SetFilePointer(block_file, num_matches * sizeof(uint32_t), NULL,
                FILE_CURRENT);
        }
    }

    // Read bytecode
    if (!ReadFile(block_file, bytecode, block_header.bytecode_size, &read, NULL) ||
        read != block_header.bytecode_size) {
        LogInfo("SplitCache: Failed to read shader bytecode\n");
        FreePoolMemory(cache, bytecode);
        LeaveCriticalSection(&cache->lock);
        return NULL;
    }

    *out_size = block_header.bytecode_size;
    LeaveCriticalSection(&cache->lock);
    return bytecode;
}

// Free bytecode buffer allocated by query functions
void FreeSplitShaderBytecode(SplitShaderCache* cache, const void* bytecode) {
    if (!cache || !bytecode)
        return;

    EnterCriticalSection(&cache->lock);
    FreePoolMemory(cache, (uint8_t*)bytecode);
    LeaveCriticalSection(&cache->lock);
}

// Insert shader to cache
// NOTE: Holds lock throughout operation to prevent race conditions on block file writes
bool InsertSplitShaderToCache(SplitShaderCache* cache, uint64_t hash,
    const wchar_t* type, const void* bytecode,
    uint32_t bytecode_size, FILETIME timestamp) {
    if (!cache || !type || !bytecode || bytecode_size == 0)
        return false;

    uint32_t type_encoded = EncodeShaderType(type);
    uint64_t key = hash | ((uint64_t)type_encoded << 32);

    EnterCriticalSection(&cache->lock);

    // Check if already exists
    auto it = cache->hash_map.find(key);
    if (it != cache->hash_map.end()) {
        // Already cached, skip
        LeaveCriticalSection(&cache->lock);
        return true;
    }

    cache->insert_count++;

    // Determine which block file to use.
    // Use shader_count (unique shaders) NOT index.size() — index may contain ghost entries
    // from regex upgrades (old non-regex entry stays in vector when replaced by regex version).
    uint32_t block_file_id = cache->header.shader_count / cache->header.shaders_per_block;

    // Update block file count if needed
    if (block_file_id >= cache->header.block_file_count) {
        cache->header.block_file_count = block_file_id + 1;
    }

    // Invalidate memory mapping for this block (file will be modified)
    UnmapBlockFile(cache, block_file_id);

    // Get block file handle (lock must be held - GetBlockFileHandle no longer takes lock)
    HANDLE block_file = GetBlockFileHandle(cache, block_file_id, true);
    if (block_file == INVALID_HANDLE_VALUE) {
        LogInfo("SplitCache: Failed to open/create block file %u\n", block_file_id);
        LeaveCriticalSection(&cache->lock);
        return false;
    }

    // Seek to end of file
    LARGE_INTEGER file_size;
    if (!GetFileSizeEx(block_file, &file_size)) {
        file_size.QuadPart = 0;
    }

    // If new file, write block file header
    if (file_size.QuadPart == 0) {
        SplitCacheBlockFileHeader block_file_header;
        memset(&block_file_header, 0, sizeof(SplitCacheBlockFileHeader));
        memcpy(block_file_header.magic, SHADER_CACHE_SPLIT_BLOCK_MAGIC, 8);
        block_file_header.version = SHADER_CACHE_SPLIT_VERSION;
        block_file_header.block_id = block_file_id;
        block_file_header.shader_count = 0;
        block_file_header.data_size = 0;

        DWORD written;
        if (!WriteFile(block_file, &block_file_header,
            sizeof(SplitCacheBlockFileHeader), &written, NULL)) {
            LogInfo("SplitCache: Failed to write block file header\n");
            LeaveCriticalSection(&cache->lock);
            return false;
        }
        file_size.QuadPart = sizeof(SplitCacheBlockFileHeader);
    }

    // Seek to end
    SetFilePointerEx(block_file, file_size, NULL, FILE_BEGIN);

    // Write shader block header
    ShaderBlockHeader block_header;
    block_header.magic = SHADER_BLOCK_MAGIC;
    block_header.flags = BLOCK_FLAG_USED;
    block_header.shader_hash = hash;
    block_header.shader_type = type_encoded;
    block_header.bytecode_size = bytecode_size;
    block_header.total_size = ALIGN_4(sizeof(ShaderBlockHeader) + bytecode_size);

    DWORD written;
    if (!WriteFile(block_file, &block_header, sizeof(ShaderBlockHeader), &written,
        NULL) ||
        written != sizeof(ShaderBlockHeader)) {
        LogInfo("SplitCache: Failed to write shader block header\n");
        LeaveCriticalSection(&cache->lock);
        return false;
    }

    // Write bytecode
    if (!WriteFile(block_file, bytecode, bytecode_size, &written, NULL) ||
        written != bytecode_size) {
        LogInfo("SplitCache: Failed to write shader bytecode\n");
        LeaveCriticalSection(&cache->lock);
        return false;
    }

    // Write padding if needed
    uint32_t padding_size = block_header.total_size - sizeof(ShaderBlockHeader) - bytecode_size;
    if (padding_size > 0) {
        uint8_t padding[4] = { 0 };
        if (!WriteFile(block_file, padding, padding_size, &written, NULL) ||
            written != padding_size) {
            LogInfo("SplitCache: Failed to write padding\n");
            LeaveCriticalSection(&cache->lock);
            return false;
        }
    }

    FlushFileBuffers(block_file);

    // Update block file header with current counts
    uint32_t block_shader_count = 1;  // Count shaders in this block (we just added one)
    uint32_t block_data_size = (uint32_t)(file_size.QuadPart - sizeof(SplitCacheBlockFileHeader)) + block_header.total_size;

    // Read existing header to get accumulated count
    LARGE_INTEGER zero_pos = { 0 };
    SplitCacheBlockFileHeader old_header;
    DWORD read;
    SetFilePointerEx(block_file, zero_pos, NULL, FILE_BEGIN);
    if (ReadFile(block_file, &old_header, sizeof(SplitCacheBlockFileHeader), &read, NULL) &&
        read == sizeof(SplitCacheBlockFileHeader) &&
        memcmp(old_header.magic, SHADER_CACHE_SPLIT_BLOCK_MAGIC, 8) == 0) {
        block_shader_count = old_header.shader_count + 1;
        block_data_size = old_header.data_size + block_header.total_size;
    }

    UpdateBlockFileHeader(cache, block_file, block_file_id, block_shader_count, block_data_size);

    // Add to index (still holding lock)
    SplitCacheIndexEntry new_entry;
    new_entry.hash = hash;
    new_entry.type = type_encoded;
    new_entry.bytecode_size = bytecode_size;
    new_entry.block_file_id = block_file_id;
    new_entry.block_offset = (uint32_t)file_size.QuadPart;
    new_entry.flags = block_header.flags;
    new_entry.reserved = 0;

    cache->index.push_back(new_entry);
    cache->hash_map[key] = cache->index.size() - 1;

    // Update header
    cache->header.shader_count++;
    cache->header.index_count++;
    cache->dirty = true;

    LeaveCriticalSection(&cache->lock);
    return true;
}

// Query regex-patched shader bytecode
const void* QuerySplitShaderRegexBytecode(SplitShaderCache* cache,
    uint64_t hash,
    const wchar_t* type,
    uint32_t expected_regex_hash,
    uint32_t* out_size,
    uint32_t* out_num_matches,
    uint32_t** out_match_ids) {
    if (!cache || !type || !out_size)
        return NULL;

    // Check if regex hash matches
    if (cache->header.shader_regex_hash != expected_regex_hash) {
        LogInfo("SplitCache: Regex hash mismatch (cache: 0x%08X, expected: 0x%08X)\n",
            cache->header.shader_regex_hash, expected_regex_hash);
        return NULL;
    }

    uint32_t type_encoded = EncodeShaderType(type);
    uint64_t key = hash | ((uint64_t)type_encoded << 32);

    EnterCriticalSection(&cache->lock);
    cache->query_count++;

    // Find in hash map
    auto it = cache->hash_map.find(key);
    if (it == cache->hash_map.end()) {
        cache->miss_count++;
        LeaveCriticalSection(&cache->lock);
        return NULL;
    }

    cache->hit_count++;
    SplitCacheIndexEntry* entry = &cache->index[it->second];

    // Check if it has regex patch flag
    if (!(entry->flags & BLOCK_FLAG_REGEX_PATCH)) {
        LeaveCriticalSection(&cache->lock);
        return NULL;
    }

    // Try memory-mapped I/O first (fast path)
    void* mapped_view = GetBlockMemoryMapping(cache, entry->block_file_id);
    if (mapped_view != NULL) {
        auto mapping_it = cache->block_mappings.find(entry->block_file_id);
        if (mapping_it != cache->block_mappings.end() &&
            entry->block_offset + sizeof(ShaderBlockHeader) <= mapping_it->second.view_size) {

            uint8_t* block_ptr = (uint8_t*)mapped_view + entry->block_offset;
            ShaderBlockHeader* block_header = (ShaderBlockHeader*)block_ptr;

            // Verify block header
            if (block_header->magic == SHADER_BLOCK_MAGIC && block_header->shader_hash == hash &&
                block_header->shader_type == type_encoded) {

                // Validate bytecode size
                if (block_header->bytecode_size > MAX_SHADER_BYTECODE_SIZE) {
                    LogInfo("SplitCache: Invalid bytecode size (%u > %u) - possible corrupted cache\n",
                        block_header->bytecode_size, MAX_SHADER_BYTECODE_SIZE);
                    LeaveCriticalSection(&cache->lock);
                    return NULL;
                }

                size_t data_offset = sizeof(ShaderBlockHeader);

                // Validate num_matches bounds before reading match IDs
                // We need at least 4 bytes for num_matches, and then num_matches * 4 bytes for match IDs
                uint32_t* num_matches_ptr = (uint32_t*)(block_ptr + data_offset);
                uint32_t num_matches = *num_matches_ptr;

                // SECURITY: Validate num_matches against reasonable maximum
                if (num_matches > MAX_REGEX_MATCHES) {
                    LogInfo("SplitCache: num_matches exceeds maximum (%u > %u) - possible corrupted cache\n",
                        num_matches, MAX_REGEX_MATCHES);
                    LeaveCriticalSection(&cache->lock);
                    return NULL;
                }

                // Bounds check: ensure all data (metadata + bytecode) fits within the block
                size_t match_ids_size = (size_t)num_matches * sizeof(uint32_t);
                size_t required_size = data_offset + sizeof(uint32_t) + match_ids_size + block_header->bytecode_size;

                if (required_size > mapping_it->second.view_size - entry->block_offset) {
                    LogInfo("SplitCache: Block data exceeds available space (required=%zu, available=%zu, block_offset=%u, bytecode_size=%u, num_matches=%u)\n",
                        required_size, mapping_it->second.view_size - entry->block_offset, entry->block_offset, block_header->bytecode_size, num_matches);
                    LeaveCriticalSection(&cache->lock);
                    return NULL;
                }

                // Advance past num_matches field only
                data_offset += sizeof(uint32_t);

                uint32_t* match_ids = NULL;

                if (num_matches > 0) {
                    match_ids = new uint32_t[num_matches];
                    memcpy(match_ids, block_ptr + data_offset, match_ids_size);
                    data_offset += match_ids_size; // advance past match IDs
                }

                // Verify we have enough data for bytecode
                if (entry->block_offset + data_offset + block_header->bytecode_size <= mapping_it->second.view_size) {
                    // Allocate and copy bytecode from pool
                    uint8_t* bytecode = AllocPoolMemory(cache, block_header->bytecode_size);
                    memcpy(bytecode, block_ptr + data_offset, block_header->bytecode_size);

                    *out_size = block_header->bytecode_size;
                    if (out_num_matches)
                        *out_num_matches = num_matches;
                    if (out_match_ids)
                        *out_match_ids = match_ids;
                    else if (match_ids)
                        delete[] match_ids;

                    cache->mmap_reads++;  // Count memory-mapped read
                    LeaveCriticalSection(&cache->lock);
                    return bytecode;
                }

                // Cleanup on failure
                if (match_ids)
                    delete[] match_ids;
            }
        }
    }

    // Fallback to traditional file I/O
    cache->file_reads++;  // Count traditional file I/O read
    HANDLE block_file = GetBlockFileHandle(cache, entry->block_file_id, false);
    if (block_file == INVALID_HANDLE_VALUE) {
        LogInfo("SplitCache: Failed to open block file %u\n", entry->block_file_id);
        LeaveCriticalSection(&cache->lock);
        return NULL;
    }

    // Seek to block offset
    LARGE_INTEGER seek_pos;
    seek_pos.QuadPart = entry->block_offset;
    if (!SetFilePointerEx(block_file, seek_pos, NULL, FILE_BEGIN)) {
        LogInfo("SplitCache: Failed to seek to block offset\n");
        LeaveCriticalSection(&cache->lock);
        return NULL;
    }

    // Read block header
    ShaderBlockHeader block_header;
    DWORD read;
    if (!ReadFile(block_file, &block_header, sizeof(ShaderBlockHeader), &read,
        NULL) ||
        read != sizeof(ShaderBlockHeader)) {
        LogInfo("SplitCache: Failed to read shader block header\n");
        LeaveCriticalSection(&cache->lock);
        return NULL;
    }

    // Verify block header
    if (block_header.magic != SHADER_BLOCK_MAGIC || block_header.shader_hash != hash ||
        block_header.shader_type != type_encoded) {
        LogInfo("SplitCache: Block header mismatch\n");
        LeaveCriticalSection(&cache->lock);
        return NULL;
    }

    // Validate bytecode size
    if (block_header.bytecode_size > MAX_SHADER_BYTECODE_SIZE) {
        LogInfo("SplitCache: Invalid bytecode size (%u > %u) - possible corrupted cache\n",
            block_header.bytecode_size, MAX_SHADER_BYTECODE_SIZE);
        LeaveCriticalSection(&cache->lock);
        return NULL;
    }

    // Read regex metadata (num_matches and match IDs)
    uint32_t num_matches = 0;
    if (!ReadFile(block_file, &num_matches, sizeof(uint32_t), &read, NULL) ||
        read != sizeof(uint32_t)) {
        LogInfo("SplitCache: Failed to read regex match count\n");
        LeaveCriticalSection(&cache->lock);
        return NULL;
    }

    // SECURITY: Validate num_matches against reasonable maximum
    // A shader regex section shouldn't have more than a few thousand matches

    if (num_matches > MAX_REGEX_MATCHES) {
        LogInfo("SplitCache: num_matches exceeds maximum (%u > %u) - possible corrupted cache\n",
            num_matches, MAX_REGEX_MATCHES);
        LeaveCriticalSection(&cache->lock);
        return NULL;
    }

    uint32_t* match_ids = NULL;
    if (num_matches > 0) {
        match_ids = new uint32_t[num_matches];
        if (!ReadFile(block_file, match_ids, num_matches * sizeof(uint32_t), &read,
            NULL) ||
            read != num_matches * sizeof(uint32_t)) {
            LogInfo("SplitCache: Failed to read regex match IDs\n");
            delete[] match_ids;
            LeaveCriticalSection(&cache->lock);
            return NULL;
        }
    }

    // Allocate buffer for bytecode from pool
    uint8_t* bytecode = AllocPoolMemory(cache, block_header.bytecode_size);

    // Read bytecode
    if (!ReadFile(block_file, bytecode, block_header.bytecode_size, &read, NULL) ||
        read != block_header.bytecode_size) {
        LogInfo("SplitCache: Failed to read shader bytecode\n");
        FreePoolMemory(cache, bytecode);
        delete[] match_ids;
        LeaveCriticalSection(&cache->lock);
        return NULL;
    }

    *out_size = block_header.bytecode_size;
    if (out_num_matches)
        *out_num_matches = num_matches;
    if (out_match_ids)
        *out_match_ids = match_ids;
    else if (match_ids)
        delete[] match_ids;

    LeaveCriticalSection(&cache->lock);
    return bytecode;
}

// Store regex-patched shader bytecode (or match-only with bytecode_size=0)
// NOTE: Holds lock throughout operation to prevent race conditions on block file writes
bool StoreSplitShaderRegexBytecode(SplitShaderCache* cache,
    uint64_t hash,
    const wchar_t* type,
    const void* bytecode,
    uint32_t bytecode_size,
    uint32_t num_matches,
    const uint32_t* match_ids) {
    if (!cache || !type)
        return false;

    // Allow NULL bytecode for match-only shaders (bytecode_size must be 0)
    if ((bytecode == NULL || bytecode_size == 0)) {
        if (bytecode != NULL || bytecode_size != 0) {
            // Mismatch: either both NULL/0 or both non-NULL/non-zero
            return false;
        }
        // Match-only shader: bytecode == NULL and bytecode_size == 0
        // Continue to store with regex metadata
    }

    uint32_t type_encoded = EncodeShaderType(type);
    uint64_t key = hash | ((uint64_t)type_encoded << 32);

    EnterCriticalSection(&cache->lock);

    // Check if already exists
    bool is_regex_upgrade = false;
    auto it = cache->hash_map.find(key);
    if (it != cache->hash_map.end()) {
        // Get the index entry
        size_t entry_index = it->second;
        if (entry_index < cache->index.size()) {
            SplitCacheIndexEntry& entry = cache->index[entry_index];

            // Check if existing entry already has regex flag
            if (entry.flags & BLOCK_FLAG_REGEX_PATCH) {
                // Already has regex-patched version, skip
                LeaveCriticalSection(&cache->lock);
                return true;
            }
        }
        // Existing entry is original shader — upgrade it to regex-patched version.
        // The old index slot becomes a ghost (stays in vector, removed from hash_map).
        // Track this so we don't inflate shader_count for a replacement.
        cache->hash_map.erase(it);
        is_regex_upgrade = true;
        LogInfo("SplitCache: Updating %ls %016llx with regex-patched version\n", type, hash);
    }

    cache->insert_count++;

    // Determine which block file to use.
    // Use shader_count (unique shaders) NOT index.size() — a regex upgrade leaves a ghost
    // entry in the index vector (old non-regex entry is never removed), so index.size()
    // is inflated and would place this shader in the wrong (or a brand-new) block file.
    uint32_t block_file_id = cache->header.shader_count / cache->header.shaders_per_block;

    // Update block file count if needed
    if (block_file_id >= cache->header.block_file_count) {
        cache->header.block_file_count = block_file_id + 1;
    }

    // Get block file handle (lock must be held - GetBlockFileHandle no longer takes lock)
    // Invalidate memory mapping for this block (file will be modified)
    UnmapBlockFile(cache, block_file_id);

    HANDLE block_file = GetBlockFileHandle(cache, block_file_id, true);
    if (block_file == INVALID_HANDLE_VALUE) {
        LogInfo("SplitCache: Failed to open/create block file %u\n", block_file_id);
        LeaveCriticalSection(&cache->lock);
        return false;
    }

    // Seek to end of file
    LARGE_INTEGER file_size;
    if (!GetFileSizeEx(block_file, &file_size)) {
        file_size.QuadPart = 0;
    }

    // If new file, write block file header
    if (file_size.QuadPart == 0) {
        SplitCacheBlockFileHeader block_file_header;
        memset(&block_file_header, 0, sizeof(SplitCacheBlockFileHeader));
        memcpy(block_file_header.magic, SHADER_CACHE_SPLIT_BLOCK_MAGIC, 8);
        block_file_header.version = SHADER_CACHE_SPLIT_VERSION;
        block_file_header.block_id = block_file_id;
        block_file_header.shader_count = 0;
        block_file_header.data_size = 0;

        DWORD written;
        if (!WriteFile(block_file, &block_file_header,
            sizeof(SplitCacheBlockFileHeader), &written, NULL)) {
            LogInfo("SplitCache: Failed to write block file header\n");
            LeaveCriticalSection(&cache->lock);
            return false;
        }
        file_size.QuadPart = sizeof(SplitCacheBlockFileHeader);
    }

    // Seek to end
    SetFilePointerEx(block_file, file_size, NULL, FILE_BEGIN);

    // Calculate total block size (header + regex metadata + bytecode + padding)
    uint32_t regex_metadata_size = sizeof(uint32_t) + (num_matches * sizeof(uint32_t));
    uint32_t total_data_size = sizeof(ShaderBlockHeader) + regex_metadata_size + bytecode_size;
    uint32_t aligned_size = ALIGN_4(total_data_size);

    // Write shader block header
    ShaderBlockHeader block_header;
    block_header.magic = SHADER_BLOCK_MAGIC;
    block_header.flags = BLOCK_FLAG_USED | BLOCK_FLAG_REGEX_PATCH;
    block_header.shader_hash = hash;
    block_header.shader_type = type_encoded;
    block_header.bytecode_size = bytecode_size;
    block_header.total_size = aligned_size;

    DWORD written;
    if (!WriteFile(block_file, &block_header, sizeof(ShaderBlockHeader), &written,
        NULL) ||
        written != sizeof(ShaderBlockHeader)) {
        LogInfo("SplitCache: Failed to write shader block header\n");
        LeaveCriticalSection(&cache->lock);
        return false;
    }

    // Write regex metadata
    if (!WriteFile(block_file, &num_matches, sizeof(uint32_t), &written, NULL) ||
        written != sizeof(uint32_t)) {
        LogInfo("SplitCache: Failed to write regex match count\n");
        LeaveCriticalSection(&cache->lock);
        return false;
    }

    if (num_matches > 0) {
        if (!WriteFile(block_file, match_ids, num_matches * sizeof(uint32_t),
            &written, NULL) ||
            written != num_matches * sizeof(uint32_t)) {
            LogInfo("SplitCache: Failed to write regex match IDs\n");
            LeaveCriticalSection(&cache->lock);
            return false;
        }
    }

    // Write bytecode (skip if match-only with zero size)
    if (bytecode_size > 0 && bytecode != NULL) {
        if (!WriteFile(block_file, bytecode, bytecode_size, &written, NULL) ||
            written != bytecode_size) {
            LogInfo("SplitCache: Failed to write shader bytecode\n");
            LeaveCriticalSection(&cache->lock);
            return false;
        }
    }

    // Write padding if needed
    uint32_t padding_size = aligned_size - total_data_size;
    if (padding_size > 0) {
        uint8_t padding[4] = { 0 };
        if (!WriteFile(block_file, padding, padding_size, &written, NULL) ||
            written != padding_size) {
            LogInfo("SplitCache: Failed to write padding\n");
            LeaveCriticalSection(&cache->lock);
            return false;
        }
    }

    FlushFileBuffers(block_file);

    // Update block file header with current counts
    uint32_t block_shader_count = 1;
    uint32_t block_data_size = (uint32_t)(file_size.QuadPart - sizeof(SplitCacheBlockFileHeader)) + aligned_size;

    LARGE_INTEGER zero_pos = { 0 };
    SplitCacheBlockFileHeader old_header;
    DWORD read;
    SetFilePointerEx(block_file, zero_pos, NULL, FILE_BEGIN);
    if (ReadFile(block_file, &old_header, sizeof(SplitCacheBlockFileHeader), &read, NULL) &&
        read == sizeof(SplitCacheBlockFileHeader) &&
        memcmp(old_header.magic, SHADER_CACHE_SPLIT_BLOCK_MAGIC, 8) == 0) {
        block_shader_count = old_header.shader_count + 1;
        block_data_size = old_header.data_size + aligned_size;
    }

    UpdateBlockFileHeader(cache, block_file, block_file_id, block_shader_count, block_data_size);

    // Add to index (still holding lock)
    SplitCacheIndexEntry new_entry;
    new_entry.hash = hash;
    new_entry.type = type_encoded;
    new_entry.bytecode_size = bytecode_size;
    new_entry.block_file_id = block_file_id;
    new_entry.block_offset = (uint32_t)file_size.QuadPart;
    new_entry.flags = block_header.flags;
    new_entry.reserved = 0;

    cache->index.push_back(new_entry);
    cache->hash_map[key] = cache->index.size() - 1;

    // Update header.
    // On a regex upgrade the unique shader count hasn't changed (we replaced an existing
    // entry), only index_count grows because the ghost slot is still in the vector.
    // On a fresh insert both counts grow.
    if (!is_regex_upgrade) {
        cache->header.shader_count++;
    }
    cache->header.index_count++;
    cache->dirty = true;

    LeaveCriticalSection(&cache->lock);

    if (bytecode_size > 0) {
        LogInfo("SplitCache: Stored regex-patched %ls %016llx (%u bytes, %u matches)\n",
            type, hash, bytecode_size, num_matches);
    }
    else {
        LogInfo("SplitCache: Stored match-only %ls %016llx (%u matches)\n",
            type, hash, num_matches);
    }

    // Flush index to disk immediately to persist regex-processed flag
    FlushSplitCacheIndex(cache);

    return true;
}

// Mark shader as regex-processed (for match-only shaders that don't get patched)
bool MarkSplitShaderRegexProcessed(SplitShaderCache* cache,
    uint64_t hash,
    const wchar_t* type) {
    if (!cache || !type)
        return false;

    uint32_t type_encoded = EncodeShaderType(type);
    uint64_t key = hash | ((uint64_t)type_encoded << 32);

    EnterCriticalSection(&cache->lock);

    // Find existing entry
    auto it = cache->hash_map.find(key);
    if (it == cache->hash_map.end()) {
        LeaveCriticalSection(&cache->lock);
        return false;  // Shader not in cache
    }

    size_t entry_index = it->second;
    if (entry_index >= cache->index.size()) {
        LeaveCriticalSection(&cache->lock);
        return false;
    }

    SplitCacheIndexEntry& entry = cache->index[entry_index];

    // Check if already marked
    if (entry.flags & BLOCK_FLAG_REGEX_PATCH) {
        LeaveCriticalSection(&cache->lock);
        return true;  // Already marked
    }

    // Set the flag
    entry.flags |= BLOCK_FLAG_REGEX_PATCH;
    cache->dirty = true;

    LeaveCriticalSection(&cache->lock);

    LogInfo("SplitCache: Marked %ls %016llx as regex-processed (match-only)\n", type, hash);

    // Flush index to disk immediately to persist regex-processed flag
    FlushSplitCacheIndex(cache);

    return true;
}

// Get cache statistics
void GetSplitCacheStatistics(SplitShaderCache* cache,
    uint32_t* out_shader_count,
    uint32_t* out_block_file_count) {
    if (!cache)
        return;

    EnterCriticalSection(&cache->lock);

    if (out_shader_count)
        *out_shader_count = cache->header.shader_count;
    if (out_block_file_count)
        *out_block_file_count = cache->header.block_file_count;

    LeaveCriticalSection(&cache->lock);
}

// Log detailed cache statistics
void LogSplitCacheStatistics(SplitShaderCache* cache) {
    if (!cache)
        return;

    EnterCriticalSection(&cache->lock);

    LogInfo("SplitCache: %u shaders in %u block files",
        cache->header.shader_count, cache->header.block_file_count);
    if (cache->query_count > 0) {
        double hit_rate = (double)cache->hit_count / cache->query_count * 100.0;
        LogInfo(" (%.1f%% hit rate)\n", hit_rate);
    }
    else {
        LogInfo("\n");
    }

    LeaveCriticalSection(&cache->lock);
}

// Validate cache integrity
uint32_t ValidateSplitCacheIntegrity(SplitShaderCache* cache) {
    if (!cache)
        return 0;

    uint32_t error_count = 0;
    EnterCriticalSection(&cache->lock);

    LogInfo("Validating split cache integrity...\n");

    // Validate header
    if (cache->header.shader_count != cache->header.index_count) {
        LogInfo("ERROR: Shader count (%u) != Index count (%u)\n",
            cache->header.shader_count, cache->header.index_count);
        error_count++;
    }

    if (cache->header.shader_count != cache->index.size()) {
        LogInfo("ERROR: Header shader count (%u) != Index vector size (%zu)\n",
            cache->header.shader_count, cache->index.size());
        error_count++;
    }

    // Validate each index entry
    for (size_t i = 0; i < cache->index.size(); i++) {
        SplitCacheIndexEntry* entry = &cache->index[i];

        // Check if block file exists
        wchar_t block_path[MAX_PATH];
        GetBlockFilePath(cache, entry->block_file_id, block_path, MAX_PATH);

        DWORD attribs = GetFileAttributesW(block_path);
        if (attribs == INVALID_FILE_ATTRIBUTES) {
            LogInfo("ERROR: Block file %u doesn't exist for index entry %zu\n",
                entry->block_file_id, i);
            error_count++;
            continue;
        }

        // Open block file directly (don't use GetBlockFileHandle to avoid LRU cache pollution)
        HANDLE block_file = CreateFileW(block_path, GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (block_file == INVALID_HANDLE_VALUE) {
            LogInfo("ERROR: Cannot open block file %u for validation\n",
                entry->block_file_id);
            error_count++;
            continue;
        }

        // Seek to block offset
        LARGE_INTEGER seek_pos;
        seek_pos.QuadPart = entry->block_offset;
        if (!SetFilePointerEx(block_file, seek_pos, NULL, FILE_BEGIN)) {
            LogInfo("ERROR: Cannot seek to offset %u in block file %u\n",
                entry->block_offset, entry->block_file_id);
            error_count++;
            continue;
        }

        // Read and verify block header
        ShaderBlockHeader block_header;
        DWORD read;
        if (!ReadFile(block_file, &block_header, sizeof(ShaderBlockHeader), &read,
            NULL) ||
            read != sizeof(ShaderBlockHeader)) {
            LogInfo("ERROR: Cannot read block header at index %zu\n", i);
            error_count++;
            continue;
        }

        if (block_header.magic != SHADER_BLOCK_MAGIC) {
            LogInfo("ERROR: Invalid block magic at index %zu (expected SHADER_BLOCK_MAGIC, got 0x%08X)\n",
                i, block_header.magic);
            error_count++;
        }

        if (block_header.shader_hash != entry->hash) {
            LogInfo("ERROR: Hash mismatch at index %zu (expected 0x%016llX, got 0x%016llX)\n",
                i, entry->hash, block_header.shader_hash);
            error_count++;
        }

        if (block_header.shader_type != entry->type) {
            LogInfo("ERROR: Type mismatch at index %zu\n", i);
            error_count++;
        }

        CloseHandle(block_file);  // Close validation handle (opened directly, not via LRU cache)
    }

    if (error_count == 0) {
        LogInfo("Cache validation PASSED - no errors found\n");
    }
    else {
        LogInfo("Cache validation FAILED - %u errors found\n", error_count);
    }

    LeaveCriticalSection(&cache->lock);
    return error_count;
}

// Enumerate all cached shaders and invoke a callback for each one.
// Intended for warm-up at game startup: iterate the cache, load each shader's bytecode,
// and submit it to the GPU driver so it can pre-compile before gameplay begins.
//
// Callback signature:
//   bool callback(uint64_t hash, const wchar_t *type, uint32_t flags,
//                 const void *bytecode, uint32_t bytecode_size, void *userdata)
//   Return false to stop enumeration early.
//
// Skips ghost entries (superseded non-regex slots left behind by regex upgrades).
// Thread-safe: holds the lock only while reading each entry; releases before the callback.
void EnumerateSplitShaderCache(SplitShaderCache* cache,
    bool (*callback)(uint64_t hash,
        const wchar_t* type,
        uint32_t flags,
        const void* bytecode,
        uint32_t bytecode_size,
        void* userdata),
    void* userdata) {
    if (!cache || !callback)
        return;

    EnterCriticalSection(&cache->lock);
    // Snapshot the index entries we care about so we can release the lock before
    // each potentially slow disk read and before calling user code.
    struct SnapEntry {
        uint64_t hash;
        uint32_t type_encoded;
        uint32_t flags;
        size_t   index_pos;   // position in cache->index (for hash_map validation)
    };
    std::vector<SnapEntry> snap;
    snap.reserve(cache->hash_map.size());

    for (auto& kv : cache->hash_map) {
        size_t idx = kv.second;
        if (idx >= cache->index.size())
            continue;
        SplitCacheIndexEntry& entry = cache->index[idx];
        SnapEntry se;
        se.hash = entry.hash;
        se.type_encoded = entry.type;
        se.flags = entry.flags;
        se.index_pos = idx;
        snap.push_back(se);
    }
    LeaveCriticalSection(&cache->lock);

    wchar_t type_str[5];
    for (auto& se : snap) {
        // Decode type
        memset(type_str, 0, sizeof(type_str));
        DecodeShaderType(se.type_encoded, type_str, 5);

        // Load bytecode via the normal query path (handles mmap + file I/O, thread-safe)
        uint32_t bytecode_size = 0;
        const void* bytecode = NULL;

        if (se.flags & BLOCK_FLAG_REGEX_PATCH) {
            uint32_t num_matches = 0;
            uint32_t* match_ids = NULL;
            // Pass the cache's own stored regex hash — we're enumerating what IS cached.
            bytecode = QuerySplitShaderRegexBytecode(
                cache, se.hash, type_str,
                cache->header.shader_regex_hash,
                &bytecode_size, &num_matches, &match_ids);
            if (match_ids)
                delete[] match_ids;
        }
        else {
            bytecode = QuerySplitShaderBytecode(cache, se.hash, type_str, &bytecode_size);
        }

        if (!bytecode)
            continue;  // Unreadable entry — skip gracefully

        bool keep_going = callback(se.hash, type_str, se.flags, bytecode, bytecode_size, userdata);
        FreeSplitShaderBytecode(cache, bytecode);

        if (!keep_going)
            break;
    }

    LogInfo("SplitCache: Enumeration complete (%zu entries visited)\n", snap.size());
}

// Migration: Convert old monolithic cache to split format
bool MigrateMonolithicToSplit(const wchar_t* old_cache_path,
    const wchar_t* new_cache_dir) {
    if (!old_cache_path || !new_cache_dir)
        return false;

    LogInfo("========== Migrating Monolithic Cache to Split Format ==========\n");
    LogInfo("Source: %ls\n", old_cache_path);
    LogInfo("Destination: %ls\n", new_cache_dir);

    // Open old cache file
    HANDLE old_file = CreateFileW(old_cache_path, GENERIC_READ, FILE_SHARE_READ,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (old_file == INVALID_HANDLE_VALUE) {
        LogInfo("ERROR: Cannot open old cache file (error: %lu)\n", GetLastError());
        return false;
    }

    // Read old cache header (64 bytes)
    struct OldShaderCacheFileHeader {
        char magic[8];
        uint32_t version;
        uint32_t shader_count;
        uint64_t file_size;
        uint64_t index_offset;
        uint32_t index_count;
        uint32_t initial_size;
        uint32_t growth_increment;
        uint32_t shader_regex_hash;
        uint8_t reserved[20];
    };

    OldShaderCacheFileHeader old_header;
    DWORD read;
    if (!ReadFile(old_file, &old_header, sizeof(OldShaderCacheFileHeader), &read,
        NULL) ||
        read != sizeof(OldShaderCacheFileHeader)) {
        LogInfo("ERROR: Cannot read old cache header\n");
        CloseHandle(old_file);
        return false;
    }

    // Verify magic and version
    if (strncmp(old_header.magic, "3DMCACHE", 8) != 0) {
        LogInfo("ERROR: Invalid cache magic\n");
        CloseHandle(old_file);
        return false;
    }

    if (old_header.version != 3) {
        LogInfo("ERROR: Unsupported cache version %u (expected 3)\n",
            old_header.version);
        CloseHandle(old_file);
        return false;
    }

    LogInfo("Old cache has %u shaders (regex hash: 0x%08X)\n",
        old_header.shader_count, old_header.shader_regex_hash);

    // Read old index
    struct OldShaderIndexEntry {
        uint64_t hash;
        uint32_t type;
        uint64_t block_offset;
        uint32_t bytecode_size;
    };

    std::vector<OldShaderIndexEntry> old_index;
    old_index.resize(old_header.index_count);

    LARGE_INTEGER seek_pos;
    seek_pos.QuadPart = old_header.index_offset;
    if (!SetFilePointerEx(old_file, seek_pos, NULL, FILE_BEGIN)) {
        LogInfo("ERROR: Cannot seek to old index\n");
        CloseHandle(old_file);
        return false;
    }

    DWORD index_size = old_header.index_count * sizeof(OldShaderIndexEntry);
    if (!ReadFile(old_file, old_index.data(), index_size, &read, NULL) ||
        read != index_size) {
        LogInfo("ERROR: Cannot read old index\n");
        CloseHandle(old_file);
        return false;
    }

    // Create new split cache with defaults (migration doesn't need custom tuning)
    SplitShaderCache* new_cache = InitSplitShaderCache(
        new_cache_dir, old_header.shader_regex_hash, SHADERS_PER_BLOCK_FILE,
        0, 0, 0, true, true);
    if (!new_cache) {
        LogInfo("ERROR: Cannot initialize split cache\n");
        CloseHandle(old_file);
        return false;
    }

    LogInfo("Migrating %u shaders...\n", old_header.index_count);

    // Migrate each shader
    uint32_t migrated_count = 0;
    uint32_t error_count = 0;

    for (uint32_t i = 0; i < old_header.index_count; i++) {
        OldShaderIndexEntry* old_entry = &old_index[i];

        // Seek to old block
        seek_pos.QuadPart = old_entry->block_offset;
        if (!SetFilePointerEx(old_file, seek_pos, NULL, FILE_BEGIN)) {
            LogInfo("ERROR: Cannot seek to block %u\n", i);
            error_count++;
            continue;
        }

        // Read old block header
        ShaderBlockHeader block_header;
        if (!ReadFile(old_file, &block_header, sizeof(ShaderBlockHeader), &read,
            NULL) ||
            read != sizeof(ShaderBlockHeader)) {
            LogInfo("ERROR: Cannot read block header %u\n", i);
            error_count++;
            continue;
        }

        // Verify block header
        if (block_header.magic != SHADER_BLOCK_MAGIC) {
            LogInfo("ERROR: Invalid block magic at index %u\n", i);
            error_count++;
            continue;
        }

        // Decode shader type
        wchar_t type_str[5];
        DecodeShaderType(old_entry->type, type_str, 5);

        // Check if regex-patched
        bool is_regex = (block_header.flags & BLOCK_FLAG_REGEX_PATCH) != 0;

        if (is_regex) {
            // Read regex metadata
            uint32_t num_matches = 0;
            if (!ReadFile(old_file, &num_matches, sizeof(uint32_t), &read, NULL) ||
                read != sizeof(uint32_t)) {
                LogInfo("ERROR: Cannot read regex match count at index %u\n", i);
                error_count++;
                continue;
            }

            // SECURITY: Validate num_matches against reasonable maximum

            if (num_matches > MAX_REGEX_MATCHES) {
                LogInfo("ERROR: num_matches exceeds maximum at index %u (%u > %u) - possible corrupted source cache\n",
                    i, num_matches, MAX_REGEX_MATCHES);
                error_count++;
                continue;
            }

            uint32_t* match_ids = NULL;
            if (num_matches > 0) {
                match_ids = new uint32_t[num_matches];
                if (!ReadFile(old_file, match_ids, num_matches * sizeof(uint32_t),
                    &read, NULL) ||
                    read != num_matches * sizeof(uint32_t)) {
                    LogInfo("ERROR: Cannot read regex match IDs at index %u\n", i);
                    delete[] match_ids;
                    error_count++;
                    continue;
                }
            }

            // Read bytecode
            uint8_t* bytecode = new uint8_t[block_header.bytecode_size];
            if (!ReadFile(old_file, bytecode, block_header.bytecode_size, &read,
                NULL) ||
                read != block_header.bytecode_size) {
                LogInfo("ERROR: Cannot read bytecode at index %u\n", i);
                delete[] bytecode;
                delete[] match_ids;
                error_count++;
                continue;
            }

            // Store in new cache
            if (StoreSplitShaderRegexBytecode(new_cache, old_entry->hash, type_str,
                bytecode, block_header.bytecode_size,
                num_matches, match_ids)) {
                migrated_count++;
            }
            else {
                LogInfo("ERROR: Failed to store regex shader at index %u\n", i);
                error_count++;
            }

            delete[] bytecode;
            delete[] match_ids;
        }
        else {
            // Regular shader - read bytecode
            uint8_t* bytecode = new uint8_t[block_header.bytecode_size];
            if (!ReadFile(old_file, bytecode, block_header.bytecode_size, &read,
                NULL) ||
                read != block_header.bytecode_size) {
                LogInfo("ERROR: Cannot read bytecode at index %u\n", i);
                delete[] bytecode;
                error_count++;
                continue;
            }

            // Get timestamp (use current time)
            FILETIME timestamp;
            GetSystemTimeAsFileTime(&timestamp);

            // Store in new cache
            if (InsertSplitShaderToCache(new_cache, old_entry->hash, type_str,
                bytecode, block_header.bytecode_size,
                timestamp)) {
                migrated_count++;
            }
            else {
                LogInfo("ERROR: Failed to store shader at index %u\n", i);
                error_count++;
            }

            delete[] bytecode;
        }

        // Progress indicator
        if ((i + 1) % 100 == 0) {
            LogInfo("Progress: %u / %u shaders migrated\n", i + 1,
                old_header.index_count);
        }
    }

    // Close old cache
    CloseHandle(old_file);

    // Flush and close new cache
    FlushSplitCacheIndex(new_cache);
    CloseSplitShaderCache(new_cache);

    LogInfo("================================================================\n");
    LogInfo("Migration complete:\n");
    LogInfo("  Migrated: %u shaders\n", migrated_count);
    LogInfo("  Errors: %u\n", error_count);
    LogInfo("================================================================\n");

    return error_count == 0;
}