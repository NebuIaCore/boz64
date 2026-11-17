// Game data bundled inside the APK (assets/boz64/*, stored uncompressed), served to the game as if it
// were the data pack the game would otherwise download into Android/obb/<package>/.
#pragma once
#include <sys/stat.h>

#include <cstdio>
#include <string>

#include "common.h"

struct Bundled {
    std::string name;  // e.g. "blackops.dz"
    u64 off = 0;       // where the file's bytes start inside base.apk
    u64 size = 0;
};

void vfs_init(const std::string& libdir);
// The bundled file a guest path stands for, or nullptr. Only paths whose real file is missing match,
// so data the game downloads or the user copies in still wins.
const Bundled* vfs_lookup(const char* path);
void vfs_stat(const Bundled* b, struct stat* st);
int vfs_open(const Bundled* b);  // read-only fd, positioned at the file's start
FILE* vfs_fopen(const Bundled* b);

// fd translation for files opened with vfs_open (all are no-ops for other fds).
bool vfs_is_fd(int fd);
void vfs_close(int fd);
s64 vfs_lseek(int fd, s64 off, int whence);           // returns -2 if fd is not a bundled file
s64 vfs_read_limit(int fd, u64 n);                    // bytes that may be read now, or -2
s64 vfs_pread(int fd, void* buf, u64 n, s64 off);     // -2 if fd is not a bundled file
bool vfs_fix_fstat(int fd, struct stat* st);
