// compat.h — Small portability shims for PES-VCS
//
// This repo is primarily POSIX-oriented, but can be built under
// Windows toolchains like MSYS2/MinGW where some functions differ.

#ifndef PES_COMPAT_H
#define PES_COMPAT_H

#ifdef _WIN32
// MSYS2/MinGW: mkdir() takes a single argument, fsync() is named _commit().
#include <direct.h> // _mkdir
#include <io.h>     // _commit

#ifndef mkdir
#define mkdir(path, mode) _mkdir(path)
#endif

#ifndef fsync
#define fsync(fd) _commit(fd)
#endif
#endif

#endif // PES_COMPAT_H
