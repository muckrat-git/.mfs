# .mfs  -  (.muckrat's) minimal filesystem
.mfs is a lightweight, single-header embedded filesystem library with a POSIX-like API.
It provides an easy-to-use, POSIX-like file API to interface with the filesystem that operates on an entirely user-defined underlying storage backend.

This means an .mfs filesystem can reside on:
- RAM buffers
- Flash storage
- EEPROM storage
- custom hardware interfaces
- or any memory-mapped region

## Features
- Single-header with only standard dependencies
- POSIX-like file api (open, read, write, lseek, etc.)
- Pluggable storage backed (through user-defined read/write implementation)
- Designed for embedded systems
- No OS needed
- Lightweight and minimal memory overhead
- High data efficiency (can operate with as little as 128 bytes!)

## Design Philosophy
.mfs is by design, not tied to any physical disk, memory, or operating system. It instead abstracts storage into a simple underlying backend interface.
By overriding the read/write layer (MFS_WRITE and MFS_READ), a filesystem can be made to operate on essentially anything with an input and output!
It has been tested on:
- Memory regions
- Integrated flash storage
- 512KB FRAM
- And even a 1024-byte Arduino EEPROM!

## Core API
Setup
```
bool   mfs_init(uint64_t address, uint32_t blocks, uint16_t block_size);
```
File operations
```
int     mfs_open(const char * path, const char * mode);
int     mfs_close(int fd);
int     mfs_read(int fd, void * buf, uint32_t size);
int     mfs_write(int fd, const void * data, uint32_t size);
int     mfs_lseek(int fd, int offset, int whence);
```
Metadata
```
int     mfs_stat(const char * path, struct mfs_stat * st);
```
Directory operations
```
int     mfs_mkdir(const char * path);
int     mfs_opendir(mfs_dir * dir, const char * path);
int     mfs_closedir(mfs_dir * dir);
int     mfs_readdir(mfs_dir * dir, struct mfs_dirent * out);    // Returns entries left to read
int     mfs_rmdir(const char * path);
```
File management
```
int     mfs_remove(const char * path);
int     mfs_rename(const char * old, const char * next);
```
## Backend Model
.mfs can be configured by replacing the low-level storage macros MFS_WRITE and MFS_READ, which default to memcpy otherwise.
## Limitations
.mfs is, by design, very minimal and, as a result, has a few quirks.
While it is reasonably efficient at most operations, the lack of full paths and directory references in files means that file finding and file removal are both quite slow at scale. This is mostly avoidable in practice, however, as .mfs is made to run on small data sizes and thus will often rarely deal with too many nested directories.
Additionally, file information has been greatly reduced for space optimisation, and as a result, the permission system is extremely simplistic.

## License
This project is licensed under the zlib/libpng license.
You are free to:
- use
- modify
- distribute
- include in commercial or closed-source projects
Subject to the following conditions:
- The origin of this software must not be misrepresented
- Modified versions must be clearly marked
- This notice must remain intact
