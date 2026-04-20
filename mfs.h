/*
 *  .mfs - (.muckrat's) minimal filesystem
 * 
 *  Licensed under the zlib license.
 * 
 *  Copyright (c) 2026 .muckrat
 * 
 *  This software is provided 'as-is', without any express or implied
 *  warranty. In no event will the authors be held liable for any damages
 *  arising from the use of this software.
 * 
 *  Permission is granted to anyone to use this software for any purpose,
 *  including commercial applications, and to alter it and redistribute it
 *  freely, subject to the following restrictions:
 *
 *   1. The origin of this software must not be misrepresented; you must not
 *      claim that you wrote the original software. If you use this software
 *      in a product, an acknowledgment in the product documentation would be
 *      appreciated but is not required.
 *   2. Altered source versions must be plainly marked as such, and must not be
 *      misrepresented as being the original software.
 *   3. This notice may not be removed or altered from any source distribution.
*/

#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdbool.h>
#include <assert.h>
#include <errno.h>
#include <unistd.h>

// Entry types
#define MFS_NULL_ENTRY  0b00
#define MFS_FILE        0b01
#define MFS_DIR         0b10

// Permissions
#define MFS_ROOT_R      0b100000
#define MFS_ROOT_W      0b010000
#define MFS_ROOT_X      0b001000
#define MFS_USER_R      0b000100
#define MFS_USER_W      0b000010
#define MFS_USER_X      0b000001

#define MFS_USER_ALL    (MFS_USER_R | MFS_USER_W | MFS_USER_X)
#define MFS_ROOT_ALL    (MFS_ROOT_R | MFS_ROOT_W | MFS_ROOT_X)
#define MFS_PERMS_ALL   (MFS_USER_ALL | MFS_ROOT_ALL)

// File modes
#define MFS_MODE_READ    0b0001
#define MFS_MODE_WRITE   0b0010
#define MFS_MODE_APPEND  0b0100
#define MFS_MODE_CREATE  0b1000

#define MFS_CMODE_READ      'r'
#define MFS_CMODE_WRITE     'w'
#define MFS_CMODE_APPEND    'a'

// Note: all of these are complete arbitrary right now (consider some order)
#define MFS_E_NAME_TOO_LONG -1
#define MFS_E_NO_NAME       -2
#define MFS_E_FAILED_CREATE -3
#define MFS_E_WRITE_ERROR   -4
#define MFS_E_NOT_FOUND     -5
#define MFS_E_TYPE_MISMATCH -6
#define MFS_E_READ_OOB      -7  // Attempted to read out of bounds
#define MFS_E_NO_DESCRIPTOR -8  // No descriptor was available
#define MFS_E_MODE_CONFLICT -9 
#define MFS_E_NO_MODE       -10
#define MFS_APPEND_ERROR    -11
#define MFS_INCORRECT_MODE  -12
#define MFS_E_EXISTS        -13

#define MFS_SEEK_SET 0  // Seek from start
#define MFS_SEEK_CUR 1  // Seek from current position
#define MFS_SEEK_END 2  // Seek from end of file

// == User defines ==

// Block table extension improves block allocation speed for eligible part layouts (block count must be <= (block size - 8) * 8)
#ifdef MFS_EXTENSION_BLKTAB
    #error "MFS Block tables are not yet implemented"
#endif

#ifndef MFS_MAX_DESCRIPTORS
#define MFS_MAX_DESCRIPTORS 16
#endif

#ifndef MFS_LOG
#ifdef MFS_STDOUT
#define MFS_LOG(...) printf(__VA_ARGS__)
#else
#define MFS_LOG(...)
#endif
#endif

#ifndef MFS_WRITE
// Defaults to memcpy, changing this and MFS_READ lets you write a filesystem to essentially anything that can store bytes
#define MFS_WRITE(address, data_in, size) \
    memcpy((void*)(address), data_in, size)
#endif

#ifndef MFS_READ
// Defaults to memcpy, changing this and MFS_WRITE lets you write a filesystem to essentially anything that can store bytes
#define MFS_READ(address, data_out, size) \
    memcpy(data_out, (void*)(address), size)
#endif

// == MFS util functions ==

uint8_t _mfsutil_ilog2(int i) {
    int res = 0;
    while(i >>= 1) ++res;
    return res;
}

#define _mfsutil_encode_to_int(text) ((int)text[3] << 24) | ((int)text[2] << 16) | ((int)text[1] << 8) | (int)text[0];

#define _mfsutil_min(a, b) (a < b ? a : b)

// == MFS type disambiguations ==

// Block index
typedef uint32_t _mfs_block_t;
typedef uintptr_t _mfs_address_t;

// == MFS block structs ==

typedef struct mfs_state {
    uint8_t     block_expo;   // Exponent of 2 for block size
    uint16_t    block_size;
    uint32_t    block_count;
    _mfs_address_t   address;
    uint32_t    offset;              // Current write offset (int blocks)
} mfs_state;

typedef struct __attribute__((packed)) _mfs_header {
    uint8_t jmp[4];    // Jump to boot sector instruction (for devices that have it)
    uint32_t magic;
} _mfs_header;


// Block types
typedef struct __attribute__((packed)) _mfs_file_header {
    uint8_t type : 2;
    uint8_t perm : 6;

    uint32_t sectionSize : 24;  // Blocks in this section
    uint32_t nextSection;       // Next section in file after this one
    uint32_t sectionBytes;      // Size of the section in bytes

    char name[16];
    uint32_t size;          // Size of the whole file (in bytes)
} _mfs_file_header;
static_assert(sizeof(_mfs_file_header) == 32);

// Consider optimising for 8 byte
typedef struct __attribute__((packed)) _mfs_section_header {
    uint8_t res;
    
    uint32_t sectionSize : 24;  // Blocks in this section
    uint32_t nextSection;
    uint32_t sectionBytes;      // Bytes of data in this section
} _mfs_section_header;
static_assert(sizeof(_mfs_section_header) == 12);

// == Internal mfs system ==

_mfs_address_t _mfs_get_block(mfs_state * state, _mfs_block_t block);
_mfs_file_header _mfs_get_file_header(mfs_state * state, _mfs_block_t block);
_mfs_section_header _mfs_get_section_header(mfs_state * state, _mfs_block_t block);
_mfs_block_t _mfs_find_empty_block(mfs_state * state);                      // Find a single empty block (0 for none)
_mfs_block_t _mfs_find_empty_section(mfs_state * state, uint32_t blocks);   // Find a continous space of [blocks] (0 for none)

// Write data from a entry to a section and link to [nextSection] (returns block of section start)
_mfs_block_t _mfs_create_section(mfs_state * state, void * data, uint32_t size, _mfs_block_t nextSection);
_mfs_block_t _mfs_create_empty_section(mfs_state * state, uint32_t size, _mfs_block_t nextSection);

_mfs_block_t _mfs_section_append(mfs_state * state, _mfs_block_t section, const void * data, uint32_t size);  // Append [size] bytes to an entry from section
_mfs_block_t _mfs_entry_append(mfs_state * state, _mfs_block_t entry, const void * data, uint32_t size);  // Append [size] bytes to [entry]

int _mfs_entry_read(mfs_state * state, _mfs_block_t block, uint32_t offset, void * data, uint32_t size);
int _mfs_section_read(mfs_state * state, _mfs_block_t block, uint32_t offset, void * data, uint32_t size);

int _mfs_entry_write(mfs_state * state, _mfs_block_t block, uint32_t offset, const void * data, uint32_t size);
int _mfs_section_write(mfs_state * state, _mfs_block_t block, uint32_t offset, const void * data, uint32_t size);

int _mfs_entry_alloc(mfs_state * state, _mfs_block_t block, uint32_t size);
int _mfs_section_alloc(mfs_state * state, _mfs_block_t block, uint32_t size);

int _mfs_entry_dealloc(mfs_state * state, _mfs_block_t block, uint32_t size);
int _mfs_section_dealloc(mfs_state * state, _mfs_block_t block, uint32_t size);

int _mfs_entry_remove(mfs_state * state, _mfs_block_t block);
int _mfs_section_remove(mfs_state * state, _mfs_block_t block);

// == Function definitions ==

_mfs_address_t _mfs_get_block(mfs_state * state, _mfs_block_t block) {
    if(block > state->block_count) return 0;
    return state->address + (block << state->block_expo);
}

_mfs_file_header _mfs_get_file_header(mfs_state * state, _mfs_block_t block) {
    _mfs_file_header header;
    MFS_READ(_mfs_get_block(state, block), &header, sizeof(_mfs_file_header));
    return header;
}

_mfs_section_header _mfs_get_section_header(mfs_state * state, _mfs_block_t block) {
    _mfs_section_header header;
    MFS_READ(_mfs_get_block(state, block), &header, sizeof(_mfs_section_header));
    return header;
}

_mfs_block_t _mfs_find_empty_block(mfs_state * state) {
    for(_mfs_block_t i = 1; i < state->block_count;) {
        _mfs_address_t blockAddr = _mfs_get_block(state, i);
        uint8_t res;
        MFS_READ(blockAddr, &res, 1);
        if(res == 0) {
            return i;
        }
        // Go to end of section
        _mfs_section_header header = _mfs_get_section_header(state, i);
        i += header.sectionSize;
    }
    return 0;
}

_mfs_block_t _mfs_find_empty_section(mfs_state * state, uint32_t count) {
    for(_mfs_block_t i = 1; i < state->block_count;) {
        _mfs_address_t blockAddr = _mfs_get_block(state, i);
        uint8_t res;
        MFS_READ(blockAddr, &res, 1);
        if(res == 0) {
            // Check if enough blocks
            uint32_t c = count - 1;
            int section = i;
            while(c > 0) {
                MFS_READ(_mfs_get_block(state, i), &res, 1);
                if(res != 0) break;
                c--;
                i++;
            }
            if(c == 0) return section;
        }
        // Go to end of section
        _mfs_section_header header = _mfs_get_section_header(state, i);
        i += header.sectionSize;
    }
    return 0;
}

_mfs_block_t _mfs_create_section(mfs_state * state, void * data, uint32_t size, _mfs_block_t nextSection) {
    uint32_t trueSize = (size + sizeof(_mfs_section_header));
    const uint32_t blocks = (trueSize >> state->block_expo) + 1;
    uint32_t pos = _mfs_find_empty_section(state, blocks);

    // If not found, split section
    if(!pos) {
        if(trueSize < state->block_size) return 0;
        const uint32_t sectionSize = (size >> 1);

        // Create sections backwards so that we dont have to go back and link them
        uint32_t second = _mfs_create_section(state, (uint8_t*)data + sectionSize, sectionSize, 0);
        if(!second)
            return 0;
        return _mfs_create_section(state, data, sectionSize, second);
    }

    // Create header
    _mfs_section_header header = {
        0xFF, blocks, 0, size
    };

    // Write to section
    _mfs_address_t block = _mfs_get_block(state, pos);
    MFS_WRITE(block, &header, size);
    MFS_WRITE(block + sizeof(_mfs_section_header), data, size);
    return pos;
}

_mfs_block_t _mfs_create_empty_section(mfs_state * state, uint32_t size, _mfs_block_t nextSection) {
    uint32_t trueSize = (size + sizeof(_mfs_section_header));
    const uint32_t blocks = (trueSize >> state->block_expo) + 1;
    uint32_t pos = _mfs_find_empty_section(state, blocks);

    // If not found, split section
    if(!pos) {
        if(trueSize < state->block_size) return 0;
        const uint32_t sectionSize = (size >> 1);

        // Create sections backwards so that we dont have to go back and link them
        uint32_t second = _mfs_create_empty_section(state, sectionSize, 0);
        if(!second)
            return 0;
        return _mfs_create_empty_section(state, sectionSize, second);
    }

    // Create header
    _mfs_section_header header = {
        0xFF, blocks, 0, size
    };

    // Write to section
    _mfs_address_t block = _mfs_get_block(state, pos);
    MFS_WRITE(block, &header, size);
    return pos;
}

_mfs_block_t _mfs_section_append(mfs_state * state, _mfs_block_t section, const void * data, uint32_t size) {
    _mfs_section_header header = _mfs_get_section_header(state, section);

    // If next section, go there
    if(header.nextSection) {
        return _mfs_section_append(state, header.nextSection, data, size);
    }

    const uint32_t maxSize = (header.sectionSize << state->block_expo) - sizeof(_mfs_section_header);

    if(header.sectionBytes + size > maxSize) {
        // Write as much as possible
        const uint32_t writeSize = maxSize - header.sectionBytes;
        if(writeSize > 0) {
            MFS_WRITE(_mfs_get_block(state, section) + sizeof(_mfs_section_header) + header.sectionBytes, data, writeSize);
            header.sectionBytes += writeSize;
            MFS_WRITE(_mfs_get_block(state, section), &header, sizeof(_mfs_section_header));
            data = (uint8_t*)data + writeSize;
            size -= writeSize;
        }

        // If next block empty, expand by 1
        uint8_t res;
        MFS_READ(_mfs_get_block(state, section + header.sectionSize), &res, 1);
        if(res == 0) {
            header.sectionSize++;
            MFS_WRITE(_mfs_get_block(state, section), &header, sizeof(_mfs_section_header));
            return _mfs_section_append(state, section, data, size);
        }

        uint32_t second = _mfs_create_section(state, (uint8_t*)data, size, 0);
        if(!second)
            return 0;
        header.nextSection = second;
        MFS_WRITE(_mfs_get_block(state, section), &header, sizeof(_mfs_section_header));
        return second;
    }

    // Write to section
    MFS_WRITE(_mfs_get_block(state, section) + sizeof(_mfs_section_header) + header.sectionBytes, data, size);
    header.sectionBytes += size;
    MFS_WRITE(_mfs_get_block(state, section), &header, sizeof(_mfs_section_header));
    return section;
}

_mfs_block_t _mfs_entry_append(mfs_state * state, _mfs_block_t entry, const void * data, uint32_t size) {
    _mfs_file_header header = _mfs_get_file_header(state, entry);
    header.size += size;

    // If next section, go there
    if(header.nextSection) {
        return _mfs_section_append(state, header.nextSection, data, size);
    }

    const uint32_t maxSize = (header.sectionSize << state->block_expo) - sizeof(_mfs_file_header);
    if(header.sectionBytes + size > maxSize) {
        // Write as much as we can to current section
        const int writeSize = maxSize - header.sectionBytes;
        if(writeSize > 0) {
            MFS_WRITE(_mfs_get_block(state, entry) + sizeof(_mfs_file_header) + header.sectionBytes, data, writeSize);
            header.sectionBytes += writeSize;
            MFS_WRITE(_mfs_get_block(state, entry), &header, sizeof(_mfs_file_header));
            data = (uint8_t*)data + writeSize;
            size -= writeSize;
        }

        // If next block empty, expand by 1
        uint8_t res;
        MFS_READ(_mfs_get_block(state, entry + header.sectionSize), &res, 1);
        if(res == 0) {
            header.sectionSize++;
            MFS_WRITE(_mfs_get_block(state, entry), &header, sizeof(_mfs_file_header));
            return _mfs_entry_append(state, entry, data, size);
        }

        // Create new section
        const uint32_t newSectionSize = size;
        _mfs_block_t newSection = _mfs_create_section(state, (uint8_t*)data, newSectionSize, 0);
        if(!newSection) return 0;

        // Write header
        header.nextSection = newSection;
        header.size += size;
        MFS_WRITE(_mfs_get_block(state, entry), &header, sizeof(_mfs_file_header));
        return newSection;
    }

    // Write to section
    MFS_WRITE(_mfs_get_block(state, entry) + sizeof(_mfs_file_header) + header.sectionBytes, data, size);
    header.sectionBytes += size;
    MFS_WRITE(_mfs_get_block(state, entry), &header, sizeof(_mfs_file_header));
    return entry;
}

// Create an empty fs entry
uint32_t _mfs_create_entry(mfs_state * state, const char * name, uint8_t type, uint8_t perm) {
    // Create header
    _mfs_file_header header;
    header.type = type;
    header.perm = perm;
    memset(header.name, 0, 16);
    memcpy(header.name, name, strlen(name));
    header.size = 0;
    header.sectionBytes = 0;
    header.sectionSize = 1;
    header.nextSection = 0;

    // Find empty spot
    _mfs_block_t pos = _mfs_find_empty_block(state);
    if(!pos) return 0;

    // Write header
    MFS_WRITE(_mfs_get_block(state, pos), &header, sizeof(_mfs_file_header));
    return pos;
}

int _mfs_section_read(mfs_state * state, _mfs_block_t block, uint32_t offset, void * data, uint32_t size) {
    const _mfs_section_header header = _mfs_get_section_header(state, block);

    // If offset larger that entry data, go next
    if(offset > header.sectionBytes) {
        if(!header.nextSection) return MFS_E_READ_OOB;
        return _mfs_section_read(state, header.nextSection, offset - header.sectionBytes, data, size);
    }

    // Read as much as can from entry data
    const uint32_t dataOffset = offset + sizeof(_mfs_section_header);
    const uint32_t readSize = _mfsutil_min(size, header.sectionBytes - offset);
    MFS_READ(dataOffset + _mfs_get_block(state, block), data, readSize);

    // Read rest from next section if exists
    if(size != readSize) {
        if(!header.nextSection) return MFS_E_READ_OOB;
        size -= readSize;
        offset += readSize;
        data = (uint8_t*)data + readSize;
        return _mfs_section_read(state, header.nextSection, offset - header.sectionBytes, data, size);
    }
    return 0;
}

int _mfs_entry_read(mfs_state * state, _mfs_block_t block, uint32_t offset, void * data, uint32_t size) {
    const _mfs_file_header header = _mfs_get_file_header(state, block);

    // If offset larger that entry data, go next
    if(offset > header.sectionBytes) {
        if(!header.nextSection) return MFS_E_READ_OOB;
        return _mfs_section_read(state, header.nextSection, offset - header.sectionBytes, data, size);
    }

    // Read as much as can from entry data
    const uint32_t dataOffset = offset + sizeof(_mfs_file_header);
    const uint32_t readSize = _mfsutil_min(size, header.sectionBytes - offset);
    MFS_READ(dataOffset + _mfs_get_block(state, block), data, readSize);

    // Read rest from next section if exists
    if(size != readSize) {
        if(!header.nextSection) return MFS_E_READ_OOB;
        size -= readSize;
        offset += readSize;
        data = (uint8_t*)data + readSize;
        return _mfs_section_read(state, header.nextSection, offset - header.sectionBytes, data, size);
    }
    return 0;
}

int _mfs_section_write(mfs_state * state, _mfs_block_t block, uint32_t offset, const void * data, uint32_t size) {
    const _mfs_section_header header = _mfs_get_section_header(state, block);

    // If offset larger that entry data, go next
    if(offset > header.sectionBytes) {
        if(!header.nextSection) return MFS_E_READ_OOB;
        return _mfs_section_write(state, header.nextSection, offset - header.sectionBytes, data, size);
    }

    // Read as much as can from entry data
    const uint32_t dataOffset = offset + sizeof(_mfs_section_header);
    const uint32_t readSize = _mfsutil_min(size, header.sectionBytes - offset);
    MFS_WRITE(dataOffset + _mfs_get_block(state, block), data, readSize);

    // Read rest from next section if exists
    if(size != readSize) {
        if(!header.nextSection) return MFS_E_READ_OOB;
        size -= readSize;
        offset += readSize;
        data = (uint8_t*)data + readSize;
        return _mfs_section_write(state, header.nextSection, offset - header.sectionBytes, data, size);
    }
    return 0;
}

int _mfs_entry_write(mfs_state * state, _mfs_block_t block, uint32_t offset, const void * data, uint32_t size) {
    const _mfs_file_header header = _mfs_get_file_header(state, block);

    // If offset larger that entry data, go next
    if(offset > header.sectionBytes) {
        if(!header.nextSection) return MFS_E_READ_OOB;
        return _mfs_section_write(state, header.nextSection, offset - header.sectionBytes, data, size);
    }

    // Read as much as can from entry data
    const uint32_t dataOffset = offset + sizeof(_mfs_file_header);
    const uint32_t readSize = _mfsutil_min(size, header.sectionBytes - offset);
    MFS_WRITE(dataOffset + _mfs_get_block(state, block), data, readSize);

    // Read rest from next section if exists
    if(size != readSize) {
        if(!header.nextSection) return MFS_E_READ_OOB;
        size -= readSize;
        offset += readSize;
        data = (uint8_t*)data + readSize;
        return _mfs_section_write(state, header.nextSection, offset - header.sectionBytes, data, size);
    }
    return 0;
}

int _mfs_section_alloc(mfs_state * state, _mfs_block_t block, uint32_t size) {
    _mfs_section_header header = _mfs_get_section_header(state, block);

    // If already has next, go there
    if(header.nextSection) {
        return _mfs_section_alloc(state, header.nextSection, size);
    }

    // If room in section, use that
    const uint32_t maxSize = (header.sectionSize << state->block_expo) - sizeof(_mfs_section_header);
    if(header.sectionBytes < maxSize) {
        const uint32_t addSize = _mfsutil_min(size, maxSize - header.sectionBytes);
        header.sectionBytes += addSize;
        MFS_WRITE(_mfs_get_block(state, block), &header, sizeof(_mfs_section_header));
        size -= addSize;
        if(size == 0) return 0;
    }

    // Expand section if next block empty
    uint8_t res;
    MFS_READ(_mfs_get_block(state, block + header.sectionSize), &res, 1);
    if(res == 0) {
        header.sectionSize++;
        MFS_WRITE(_mfs_get_block(state, block), &header, sizeof(_mfs_section_header));
        return _mfs_section_alloc(state, block, size);
    }

    // Add section
    uint32_t second = _mfs_create_empty_section(state, size, 0);
    if(!second)
        return MFS_APPEND_ERROR;
    header.nextSection = second;
    MFS_WRITE(_mfs_get_block(state, block), &header, sizeof(_mfs_section_header));
    return 0;
}

int _mfs_entry_alloc(mfs_state * state, _mfs_block_t block, uint32_t size) {
    _mfs_file_header header = _mfs_get_file_header(state, block);
    header.size += size;

    // If already has next, go there
    if(header.nextSection) {
        MFS_WRITE(_mfs_get_block(state, block), &header, sizeof(_mfs_file_header));
        return _mfs_section_alloc(state, header.nextSection, size);
    }

    // If room in section, use that
    const uint32_t maxSize = (header.sectionSize << state->block_expo) - sizeof(_mfs_file_header);
    if(header.sectionBytes < maxSize) {
        const uint32_t addSize = _mfsutil_min(size, maxSize - header.sectionBytes);
        header.sectionBytes += addSize;
        MFS_WRITE(_mfs_get_block(state, block), &header, sizeof(_mfs_file_header));
        size -= addSize;
        if(size == 0) return 0;
    }

    // Expand section if next block empty
    uint8_t res;
    MFS_READ(_mfs_get_block(state, block + header.sectionSize), &res, 1);
    if(res == 0) {
        header.sectionSize++;
        MFS_WRITE(_mfs_get_block(state, block), &header, sizeof(_mfs_file_header));
        return _mfs_section_alloc(state, block, size);
    }

    // Add section
    uint32_t second = _mfs_create_empty_section(state, size, 0);
    if(!second)
        return MFS_APPEND_ERROR;
    header.nextSection = second;
    MFS_WRITE(_mfs_get_block(state, block), &header, sizeof(_mfs_file_header));
    return 0;
}

int _mfs_entry_dealloc(mfs_state * state, _mfs_block_t block, uint32_t size) {
    _mfs_file_header header = _mfs_get_file_header(state, block);

    header.size -= _mfsutil_min(size, header.size);
    MFS_WRITE(_mfs_get_block(state, block), &header, sizeof(_mfs_file_header));

    // If has next go there
    if(header.nextSection) {
        size = _mfs_section_dealloc(state, block, size);
        if(size == 0) return 0;
    }

    // Remove in entry
    if(size > header.size) {
        header.sectionBytes = 0;
        MFS_WRITE(_mfs_get_block(state, block), &header, sizeof(_mfs_file_header));
        return 0;
    }

    header.sectionBytes -= size;
    MFS_WRITE(_mfs_get_block(state, block), &header, sizeof(_mfs_file_header));
    return 0;
}

int _mfs_section_dealloc(mfs_state * state, _mfs_block_t block, uint32_t size) {
    _mfs_section_header header = _mfs_get_section_header(state, block);

    // If has next go there
    if(header.nextSection) {
        size = _mfs_section_dealloc(state, block, size);
        if(size == 0) return 0;
    }

    const int removeSize = _mfsutil_min(size, header.sectionBytes);
    header.sectionBytes -= removeSize;
    
    // If no bytes, delete section
    if(header.sectionBytes == 0) {
        _mfs_section_remove(state, block);
    }
    else {
        MFS_WRITE(_mfs_get_block(state, block), &header, sizeof(_mfs_section_header));
    }
    return size - removeSize;
}

// Note: _mfs_section_remove would work fine on entries, this is here for parity
int _mfs_entry_remove(mfs_state * state, _mfs_block_t block) {
    _mfs_file_header header = _mfs_get_file_header(state, block);

    // If has sub-section, remove that first
    if(header.nextSection) {
        int res = _mfs_section_remove(state, header.nextSection);
        if(res) return res;
    }

    // Mark as deletable
    header.type = 0;
    header.perm = 0;
    MFS_WRITE(_mfs_get_block(state, block), &header, sizeof(_mfs_file_header));
    return 0;
}

int _mfs_section_remove(mfs_state * state, _mfs_block_t block) {
    _mfs_section_header header = _mfs_get_section_header(state, block);

    // If has sub-section, remove that first
    if(header.nextSection) {
        int res = _mfs_section_remove(state, header.nextSection);
        if(res) return res;
    }

    // Mark as deletable
    header.res = 0;
    MFS_WRITE(_mfs_get_block(state, block), &header, sizeof(_mfs_section_header));
    return 0;
}

// == MFS interface ==

typedef struct mfs_descriptor {
    int flags;
    _mfs_file_header header;
    uint32_t offset;    // Offset inside the file
    _mfs_block_t block;

    // Section data for optimization later (if i ever do it :P)
    _mfs_block_t currentSection;
    _mfs_block_t nextSection;
} mfs_descriptor;

mfs_descriptor mfs_descriptor_table[MFS_MAX_DESCRIPTORS];

struct mfs_stat {
    uint8_t type : 2;
    uint8_t perms : 6;
    uint32_t size;
};

typedef struct mfs_dir {
    _mfs_file_header header;
    uint32_t offset;    // Offset inside the directory
    _mfs_block_t block;
    uint32_t entriesLeft;
} mfs_dir;

struct mfs_dirent {
    char name[17];
    uint32_t block;
    struct mfs_stat stat;
};

// == MFS globals ==
mfs_state mfs_global_state;
const char * mfs_current_dir = "/";

// == MFS exclusive functions ==

bool mfs_init(uint64_t address, uint32_t blocks, uint16_t block_size);
mfs_dir mfs_open_root();
int mfs_touch(const char * path);

// == Essential posix functions ==

int     mfs_open(const char * path, const char * mode);
int     mfs_close(int fd);
int     mfs_read(int fd, void * buf, uint32_t size);
int     mfs_write(int fd, const void * data, uint32_t size);
int     mfs_lseek(int fd, int offset, int whence);

// Metadata
int     mfs_stat(const char * path, struct mfs_stat * st);

// Directory
int     mfs_mkdir(const char * path);
int     mfs_opendir(mfs_dir * dir, const char * path);
int     mfs_closedir(mfs_dir * dir);
int     mfs_readdir(mfs_dir * dir, struct mfs_dirent * out);    // Returns entries left to read
int     mfs_rmdir(const char * path);

// Management
int     mfs_remove(const char * path);
int     mfs_rename(const char * old, const char * next);

/* Possibly coming to MFS later */
//int     mfs_truncate(const char *path, uint32_t size);


// == MFS interface under-the-hood implementations ==

int _mfs_get_free_descriptor() {
    for(int i = 0; i < MFS_MAX_DESCRIPTORS; ++i) {
        if(mfs_descriptor_table[i].flags) continue;
        return i;
    }
    return -1;
}

struct mfs_stat _mfs_stat(_mfs_block_t block) {
    _mfs_file_header header = _mfs_get_file_header(&mfs_global_state, block);
    return (struct mfs_stat){
        header.type, 
        header.perm,
        header.size
    };
}

mfs_dir _mfs_open_dir(_mfs_block_t block) {
    mfs_dir d;
    d.offset = 0;
    d.header = _mfs_get_file_header(&mfs_global_state, block);
    d.block = block;
    d.entriesLeft = d.header.size / 4;
    return d;
}

// Get the starting block of an item from a path
_mfs_block_t _mfs_resolve_path(const char * path, mfs_dir * current) {
    char pathBuf[strlen(path)+1];
    strcpy(pathBuf, path);
    path = pathBuf;

    // If no current search from root
    if(current == NULL) {
        if(strcmp(path, "/") == 0) {
            return 1;
        }
        char nextPath[strlen(path) + 1 + strlen(mfs_current_dir)];
        if(path[0] == '/') {
            strcpy(nextPath, path + 1);
        }
        else {
            strcpy(nextPath, mfs_current_dir);
            strcat(nextPath, path);
        }
        mfs_dir root = mfs_open_root();
        return _mfs_resolve_path(nextPath, &root);
    }

    // Get first item in path
    char * p = strchr((char*)path, '/');
    if(p) {
        *p = '\0';
        ++p;
    }

    // Search for item inside dir
    struct mfs_dirent ent;
    int error = 0;
    while((error = mfs_readdir(current, &ent)) > 0) {
        if(strcmp(path, ent.name) == 0) {
            // If more in path continue on
            if(p) {
                // Error out if file
                if(ent.stat.type != MFS_DIR)
                    return MFS_E_TYPE_MISMATCH;

                // Open dir
                mfs_dir next = _mfs_open_dir(ent.block);
                uint32_t res = _mfs_resolve_path(p, &next);
                mfs_closedir(&next);
                return res;
            }
            // Otherwise, done
            return ent.block;
        }
    }
    return 0;
}

// Get the parent directory of an entry (Warning: this is very expensive)
_mfs_block_t _mfs_get_parent(_mfs_block_t entry, _mfs_block_t start) {
    if(start == 0) {
        start = 1;  // Start at root
    }

    struct mfs_dirent ent;
    int error = 0;
    mfs_dir current = _mfs_open_dir(start);
    while((error = mfs_readdir(&current, &ent)) > 0) {
        if(ent.block == entry) return start;
        if(ent.stat.type != MFS_DIR) continue;
        
        // Check this path
        _mfs_block_t res = _mfs_get_parent(entry, ent.block);
        if(res) return res;
    }
    return 0;
}

int _mfs_remove_from_dir(_mfs_block_t entry, _mfs_block_t directory) {
    _mfs_file_header header = _mfs_get_file_header(&mfs_global_state, directory);
    const int directoryLen = header.size / 4;
    if(directory == 0) return MFS_E_NOT_FOUND;

    // Search for entry in directory
    bool found = false;
    for(int i = 0; i < directoryLen; ++i) {
        uint32_t res;
        _mfs_entry_read(&mfs_global_state, directory, i * 4, &res, 4);
        if(found) {
            // Overwrite prior
            _mfs_entry_write(&mfs_global_state, directory, (i-1) * 4, &res, 4);
        }
        if(res == entry) {
            found = true;
        }
    }
    if(!found) return MFS_E_NOT_FOUND;

    // Unallocate
    _mfs_entry_dealloc(&mfs_global_state, directory, 4);

    return 0;
}

int _mfs_mkent(const char * path, uint8_t type, _mfs_block_t * entry) {


    if(!mfs_stat(path, NULL)) return MFS_E_EXISTS;
    if(entry) *entry = 0;

    char buf[strlen(path) + 1];
    strcpy(buf, path);
    
    // Get path until last
    char * name = strrchr((char*)buf, '/');
    if(name != NULL) {
        *name = '\0';
        ++name;
    }
    uint32_t block = 1; // Root
    // If not root, find path
    if(strlen(buf)) {
        block = _mfs_resolve_path((char*)buf, NULL);
    }
    if(block == 0)
        return MFS_E_NOT_FOUND;

    if(strlen(name) == 0)
        return MFS_E_NO_NAME;
    if(strlen(name) > 16) 
        return MFS_E_NAME_TOO_LONG;

    // Create entry
    _mfs_block_t e = _mfs_create_entry(&mfs_global_state, name, type, MFS_PERMS_ALL); 
    if(e == 0) return MFS_E_FAILED_CREATE;

    // Add entry to previous path
    _mfs_block_t res = _mfs_entry_append(&mfs_global_state, block, &e, sizeof(uint32_t));
    if(res == 0) return MFS_E_WRITE_ERROR;
    if(entry) *entry = e;
    return 0;
}

int _mfs_open(const char * path, int flags) {
    if((flags & MFS_MODE_WRITE) && (flags & MFS_MODE_APPEND)) {
        return MFS_E_MODE_CONFLICT;
    }

    bool exists = !mfs_stat(path, 0);

    _mfs_block_t block;

    // If create flag, create file
    if(!exists) {
        if(!(flags & MFS_MODE_CREATE)) {
            return MFS_E_NOT_FOUND;
        }

        int error = _mfs_mkent(path, MFS_FILE, &block);
        if(error < 0) return error;
        if(block == 0) return MFS_E_FAILED_CREATE;
    }
    else {
        block = _mfs_resolve_path(path, NULL);
        if(block == 0) return MFS_E_NOT_FOUND;
    }

    int descriptorIndex = _mfs_get_free_descriptor();
    if(descriptorIndex == -1) return MFS_E_NO_DESCRIPTOR;

    mfs_descriptor fd;
    fd.header = _mfs_get_file_header(&mfs_global_state, block);
    fd.flags = flags;
    fd.block = block;
    fd.currentSection = block;
    fd.nextSection = fd.header.nextSection;
    fd.offset = 0;

    mfs_descriptor_table[descriptorIndex] = fd;

    return descriptorIndex;
}

void _mfs_update_descriptor(int fd) {
    mfs_descriptor_table[fd].header = _mfs_get_file_header(&mfs_global_state, mfs_descriptor_table[fd].block);
}

int _mfs_create(const char * path, uint8_t type) {
    return _mfs_mkent(path, type, 0);
}

// == MFS exclusive function definitions ==

// Create an mfs filesystem at address (returns false on error)
bool mfs_init(uint64_t address, uint32_t blocks, uint16_t block_size) {
    // Verify block table extension
    #ifdef MFS_EXTENSION_BLKTAB
    if(blocks > (block_size - sizeof(_mfs_header)) * 8) {
        MFS_LOG("E: [mfs_create] insufficient block layout for block table extension\n");
        return false;
    }
    #endif

    mfs_state state = {
        _mfsutil_ilog2(block_size),
        block_size,
        blocks,
        address,
        0
    };

    // Write header
    const int magic = _mfsutil_encode_to_int("MFS1");
    _mfs_header header = { {}, magic };
    MFS_WRITE(state.address, &header, sizeof(_mfs_header));

    // Write root directory
    uint32_t entry = _mfs_create_entry(&state, "/", MFS_DIR, MFS_ROOT_ALL | MFS_USER_R);

    mfs_global_state = state;
    return true;
}

mfs_dir mfs_open_root() {
    return _mfs_open_dir(1);
}

int mfs_touch(const char * path) {
    return _mfs_mkent(path, MFS_DIR, 0);
}

// == Definitions ==

int mfs_open(const char * path, const char * mode) {
    const int flagCount = strlen(mode);
    if(flagCount == 0) return MFS_E_NO_MODE;
    
    int flags = 0;
    // Parse mode
    for(int i = 0; i < strlen(mode); ++i) {
        if(mode[i] == MFS_CMODE_READ) flags |= MFS_MODE_READ;
        if(mode[i] == MFS_CMODE_WRITE) flags |= MFS_MODE_WRITE;
        if(mode[i] == MFS_CMODE_APPEND) flags |= MFS_MODE_APPEND;
    }

    // If writing add create flag
    if((flags & MFS_MODE_WRITE) || (flags & MFS_MODE_APPEND)) {
        flags |= MFS_MODE_CREATE;
    }

    return _mfs_open(path, flags);
}

int mfs_close(int fd) {
    mfs_descriptor_table[fd].flags = 0;
    return 0;
}

int mfs_read(int fd, void * buf, uint32_t size) {
    mfs_descriptor d = mfs_descriptor_table[fd];
    int res = _mfs_entry_read(&mfs_global_state, d.block, d.offset, buf, size);
    d.offset += size;
    return res;
}

int mfs_write(int fd, const void * data, uint32_t size) {
    mfs_descriptor d = mfs_descriptor_table[fd];
    if(d.flags | MFS_MODE_APPEND) {
        _mfs_block_t res = _mfs_entry_append(&mfs_global_state, d.block, data, size);
        _mfs_update_descriptor(fd);
        if(res == 0) return MFS_APPEND_ERROR;   // Should be more descriptive
        return 0;
    }
    // Write as much as can fit in data
    const long int writeData = min(d.header.size - d.offset, size);
    int res;
    if(writeData > 0) {
        res = _mfs_entry_write(&mfs_global_state, d.block, d.offset, data, size);
        if(res < 0) return res;
        d.offset += writeData;
        data = (uint8_t*)data + writeData;
        size -= writeData;
        _mfs_update_descriptor(fd);
        if(writeData == size) return 0;
    }

    // Allocate for rest
    res = _mfs_entry_alloc(&mfs_global_state, d.block, d.offset + size - d.header.size);
    if(res != 0) return res;
    return mfs_write(fd, data, size);
}

int mfs_lseek(int fd, int offset, int whence) {
    if(whence == MFS_SEEK_SET) {
        mfs_descriptor_table[fd].offset = 0;
    }
    else if(whence == MFS_SEEK_END) {
        mfs_descriptor_table[fd].offset = mfs_descriptor_table[fd].header.size;
    }
    mfs_descriptor_table[fd].offset += offset;

    return mfs_descriptor_table[fd].offset;
}

// Get info about a file (return true if file exists, false otherwise)
int mfs_stat(const char * path, struct mfs_stat * st) {
    _mfs_block_t block = _mfs_resolve_path(path, 0);
    if(!block) return MFS_E_NOT_FOUND;
    if(st != NULL) {
        *st = _mfs_stat(block);
    }
    return 0;
}

int mfs_mkdir(const char * path) {
    return _mfs_mkent(path, MFS_DIR, 0);
}

int mfs_opendir(mfs_dir * dir, const char * path) {
    _mfs_block_t block = _mfs_resolve_path(path, NULL);
    if(block == 0) return MFS_E_NOT_FOUND;
    *dir = _mfs_open_dir(block);
    return 0;
}

// MFS close dir does not do anything, it is just here for standardization :)
int mfs_closedir(mfs_dir * dir) {
    return 0;
}

int mfs_readdir(mfs_dir * dir, struct mfs_dirent * out) {
    // If dir empty, 0
    if(dir->entriesLeft == 0) return 0;

    // Get entry
    _mfs_block_t block;
    int res = _mfs_entry_read(&mfs_global_state, dir->block, dir->offset, &block, 4);
    dir->offset += 4;
    if(res < 0) return res;

    // Make dirent
    struct mfs_dirent ent;
    ent.block = block;
    _mfs_file_header header = _mfs_get_file_header(&mfs_global_state, block);
    memcpy(ent.name, header.name, sizeof(header.name));
    ent.name[16] = 0;
    ent.stat = _mfs_stat(block);

    *out = ent;

    return dir->entriesLeft--;
}

int mfs_rmdir(const char * path) {
    return mfs_remove(path);
}

int mfs_remove(const char * path) {
    _mfs_block_t block = _mfs_resolve_path(path, NULL);
    if(block == 0) return MFS_E_NOT_FOUND;

    // Remove from parent directory
    int res = _mfs_remove_from_dir(block, _mfs_get_parent(block, 0));
    if(res) return res;

    return _mfs_entry_remove(&mfs_global_state, block);
}

int mfs_rename(const char * old, const char * next) {
    _mfs_block_t block = _mfs_resolve_path(old, NULL);
    if(block == 0) return MFS_E_NOT_FOUND;

    _mfs_file_header header = _mfs_get_file_header(&mfs_global_state, block);

    // Remove leading path from filename
    const char * name = strchr(next, '/');
    if(name == 0) {
        name = next;
    }
    else {
        name++;
    }
    memset(header.name, 0, 16);
    memcpy(header.name, name, min(strlen(name), 16));
    // Write header
    MFS_WRITE(_mfs_get_block(&mfs_global_state, block), &header, sizeof(_mfs_file_header));
    return 0;
}