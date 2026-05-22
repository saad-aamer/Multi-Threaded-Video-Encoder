/*
 * shared.h — IPC definitions shared by orchestrator, worker, and GUI monitor
 *
 * All three components include this header so the layout of SharedMem,
 * the POSIX shared-memory name, and the semaphore naming scheme stay
 * consistent without duplication.
 */

#ifndef SHARED_H
#define SHARED_H

#include <stdint.h>
#include <stdatomic.h>

/* ── limits ──────────────────────────────────────────────────── */
#define MAX_WORKERS  16

/* ── POSIX IPC names ─────────────────────────────────────────── */
#define SHM_NAME    "/vidpipe_shm"      /* shm_open key              */
#define SEM_PREFIX  "/vidpipe_sem_"     /* appended with worker id   */
                                        /* e.g. /vidpipe_sem_0       */

/* ── Worker status codes stored in SharedMem::worker_status[] ── */
typedef enum {
    WORKER_IDLE      = 0,
    WORKER_ENCODING  = 1,
    WORKER_DONE      = 2,
    WORKER_CRASHED   = 3
} WorkerStatus;

/*
 * SharedMem — the struct that lives in the POSIX shared-memory segment.
 *
 * Layout (one shared-memory page is enough for MAX_WORKERS = 16):
 *
 *  ┌──────────────────────────────────────────────────────────┐
 *  │  num_workers    (int)   number of active worker slots    │
 *  │  total_frames   (int)   total frames in the video        │
 *  │  pipeline_done  (int)   set to 1 by orchestrator at end  │
 *  │──────────────────────────────────────────────────────────│
 *  │  worker_frames[MAX_WORKERS]  frames encoded so far       │
 *  │  worker_total [MAX_WORKERS]  frames assigned to worker   │
 *  │  worker_status[MAX_WORKERS]  WorkerStatus enum value     │
 *  └──────────────────────────────────────────────────────────┘
 *
 * Workers write worker_frames[id] and worker_status[id].
 * Orchestrator writes everything else.
 * GUI monitor reads everything (read-only after setup).
 */
typedef struct {
    /* set by orchestrator at startup */
    int num_workers;
    int total_frames;

    /* set by orchestrator after final merge */
    volatile int pipeline_done;

    /* updated by each worker as it encodes frames */
    volatile int worker_frames[MAX_WORKERS];   /* frames done so far  */
    int          worker_total [MAX_WORKERS];   /* frames assigned     */
    volatile int worker_status[MAX_WORKERS];   /* WorkerStatus        */
} SharedMem;

/* ── tiny inline helper used in orchestrator.c ───────────────── */
#ifdef __cplusplus
extern "C" {
#endif

/* Marks pipeline_done = 1 in the shared segment if it still exists.
 * Called by orchestrator after the final merge step.
 * Defined in orchestrator.c; declared here so shared.h is the single
 * source of truth for the function signature. */
void shm_mark_complete_if_exists(void);

#ifdef __cplusplus
}
#endif

#endif /* SHARED_H */
