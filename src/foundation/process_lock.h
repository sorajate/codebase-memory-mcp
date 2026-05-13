#ifndef CBM_PROCESS_LOCK_H
#define CBM_PROCESS_LOCK_H

typedef struct cbm_process_lock cbm_process_lock_t;

int cbm_process_lock_acquire(const char *path, cbm_process_lock_t **out);
int cbm_process_lock_try_acquire(const char *path, cbm_process_lock_t **out);
void cbm_process_lock_release(cbm_process_lock_t *lock);

#endif /* CBM_PROCESS_LOCK_H */
