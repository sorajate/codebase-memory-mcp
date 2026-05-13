#include "foundation/process_lock.h"

#include "foundation/constants.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

struct cbm_process_lock {
    HANDLE handle;
};

static int acquire_windows_lock(const char *path, cbm_process_lock_t **out, DWORD flags) {
    if (!path || !out) {
        return CBM_NOT_FOUND;
    }
    *out = NULL;

    HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return CBM_NOT_FOUND;
    }

    OVERLAPPED ov = {0};
    if (!LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | flags, 0, 1, 0, &ov)) {
        CloseHandle(h);
        return CBM_NOT_FOUND;
    }

    cbm_process_lock_t *lock = calloc(CBM_ALLOC_ONE, sizeof(*lock));
    if (!lock) {
        UnlockFileEx(h, 0, 1, 0, &ov);
        CloseHandle(h);
        return CBM_NOT_FOUND;
    }
    lock->handle = h;
    *out = lock;
    return 0;
}

int cbm_process_lock_acquire(const char *path, cbm_process_lock_t **out) {
    return acquire_windows_lock(path, out, 0);
}

int cbm_process_lock_try_acquire(const char *path, cbm_process_lock_t **out) {
    return acquire_windows_lock(path, out, LOCKFILE_FAIL_IMMEDIATELY);
}

void cbm_process_lock_release(cbm_process_lock_t *lock) {
    if (!lock) {
        return;
    }
    OVERLAPPED ov = {0};
    UnlockFileEx(lock->handle, 0, 1, 0, &ov);
    CloseHandle(lock->handle);
    free(lock);
}

#else

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

struct cbm_process_lock {
    int fd;
};

static int acquire_posix_lock(const char *path, cbm_process_lock_t **out, int flags) {
    if (!path || !out) {
        return CBM_NOT_FOUND;
    }
    *out = NULL;

    int fd = open(path, O_CREAT | O_RDWR, 0666);
    if (fd < 0) {
        return CBM_NOT_FOUND;
    }

    int rc;
    do {
        rc = flock(fd, LOCK_EX | flags);
    } while (rc != 0 && errno == EINTR);
    if (rc != 0) {
        close(fd);
        return CBM_NOT_FOUND;
    }

    cbm_process_lock_t *lock = calloc(CBM_ALLOC_ONE, sizeof(*lock));
    if (!lock) {
        flock(fd, LOCK_UN);
        close(fd);
        return CBM_NOT_FOUND;
    }
    lock->fd = fd;
    *out = lock;
    return 0;
}

int cbm_process_lock_acquire(const char *path, cbm_process_lock_t **out) {
    return acquire_posix_lock(path, out, 0);
}

int cbm_process_lock_try_acquire(const char *path, cbm_process_lock_t **out) {
    return acquire_posix_lock(path, out, LOCK_NB);
}

void cbm_process_lock_release(cbm_process_lock_t *lock) {
    if (!lock) {
        return;
    }
    flock(lock->fd, LOCK_UN);
    close(lock->fd);
    free(lock);
}

#endif
