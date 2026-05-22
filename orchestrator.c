/*
 * orchestrator.c — Faraz's Part
 * ─────────────────────────────
 * Responsibilities:
 *   1. Split input video into PNG frames via FFmpeg
 *   2. Fork N worker processes, assign frame batches
 *   3. Monitor workers with waitpid(WNOHANG), respawn on crash
 *   4. Wait for all semaphores, merge segments with FFmpeg
 *   5. Launch media player (vlc or mpv)
 *
 * Build:
 *   gcc -O2 -Wall -o orchestrator orchestrator.c -lrt -lpthread
 *
 * Usage:
 *   ./orchestrator input.mp4 [num_workers]
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <semaphore.h>
#include <errno.h>
#include <signal.h>
#include <time.h>

#include "shared.h"   /* SharedMem, SHM_NAME, SEM_PREFIX, MAX_WORKERS */

/* ── tunables ────────────────────────────────────────────────── */
#define FRAMES_DIR   "frames"
#define SEGMENTS_DIR "segments"
#define OUTPUT_FILE  "output_final.mp4"
#define FRAMERATE    "30"          /* must match worker encoder setting  */
#define RESPAWN_MAX  3             /* max crash-respawns per worker slot */

/* ── helpers ─────────────────────────────────────────────────── */
static void die(const char *msg) { perror(msg); exit(EXIT_FAILURE); }

static void shell(const char *cmd)
{
    printf("[ORC] $ %s\n", cmd);
    int rc = system(cmd);
    if (rc != 0) {
        fprintf(stderr, "[ORC] command failed (exit %d): %s\n", rc, cmd);
        exit(EXIT_FAILURE);
    }
}

/* Count total frames from the video directly using ffprobe */
static int count_frames(const char *input_video)
{
    char cmd[512];
    snprintf(cmd, sizeof cmd,
             "ffprobe -v error -select_streams v:0 -show_entries stream=nb_frames "
             "-of default=nokey=1:noprint_wrappers=1 \"%s\"",
             input_video);
    FILE *fp = popen(cmd, "r");
    int n = 0;
    if (fp) {
        if (fscanf(fp, "%d", &n) == 1 && n > 0) {
            pclose(fp);
            return n;
        }
        pclose(fp);
    }

    /* Fallback if nb_frames is missing in container header */
    snprintf(cmd, sizeof cmd,
             "ffprobe -v error -select_streams v:0 -count_packets "
             "-show_entries stream=nb_read_packets -of csv=p=0 \"%s\"",
             input_video);
    fp = popen(cmd, "r");
    if (fp) {
        if (fscanf(fp, "%d", &n) != 1) {
            n = 0;
        }
        pclose(fp);
    }
    return n;
}

/* ── step 2 : fork one worker ─────────────────────────────────── */
static pid_t spawn_worker(int worker_id, int frame_start, int frame_count, const char *input_video)
{
    pid_t pid = fork();
    if (pid < 0) die("fork");

    if (pid == 0) {
        /* child — exec worker binary */
        char sid[16], sstart[16], scount[16];
        snprintf(sid,    sizeof sid,    "%d", worker_id);
        snprintf(sstart, sizeof sstart, "%d", frame_start);
        snprintf(scount, sizeof scount, "%d", frame_count);

        execl("./worker", "./worker", sid, sstart, scount, input_video, NULL);
        perror("execl worker");
        _exit(EXIT_FAILURE);
    }

    return pid;   /* parent gets child PID */
}

/* ── step 3 : monitor & respawn ──────────────────────────────── */
typedef struct {
    pid_t  pid;
    int    worker_id;
    int    frame_start;
    int    frame_count;
    int    respawns;
    int    done;          /* 1 after semaphore posted */
    char   input_video[256];
} WorkerSlot;

static void monitor_workers(WorkerSlot *slots, int nw, SharedMem *shm)
{
    int finished = 0;

    while (finished < nw) {
        for (int i = 0; i < nw; i++) {
            if (slots[i].done) continue;

            /* non-blocking wait */
            int status;
            pid_t r = waitpid(slots[i].pid, &status, WNOHANG);

            if (r == 0) continue;   /* still running */

            if (r < 0) { perror("waitpid"); continue; }

            /* worker exited — check how */
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                /* clean exit; semaphore already posted by worker */
                printf("[ORC] Worker %d exited cleanly.\n", slots[i].worker_id);
                slots[i].done = 1;
                finished++;
            } else {
                /* crash / non-zero exit */
                int sig = WIFSIGNALED(status) ? WTERMSIG(status) : -1;
                fprintf(stderr, "[ORC] Worker %d crashed (sig=%d exit=%d).\n",
                        slots[i].worker_id, sig,
                        WIFEXITED(status) ? WEXITSTATUS(status) : -1);

                if (slots[i].respawns >= RESPAWN_MAX) {
                    fprintf(stderr,
                            "[ORC] Worker %d exceeded respawn limit — giving up.\n",
                            slots[i].worker_id);
                    slots[i].done = 1;   /* skip it to avoid infinite loop */
                    finished++;
                } else {
                    slots[i].respawns++;
                    printf("[ORC] Respawning worker %d (attempt %d).\n",
                           slots[i].worker_id, slots[i].respawns);
                    /* Reset shared-mem counter so GUI doesn't show stale data */
                    shm->worker_frames[slots[i].worker_id] = 0;
                    slots[i].pid = spawn_worker(slots[i].worker_id,
                                                slots[i].frame_start,
                                                slots[i].frame_count,
                                                slots[i].input_video);
                }
            }
        }

        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = 100000000; /* 100 ms */
        nanosleep(&ts, NULL);
    }
}

/* ── step 4 : wait for semaphores ────────────────────────────── */
static void wait_all_semaphores(int nw)
{
    char sem_name[64];
    for (int i = 0; i < nw; i++) {
        snprintf(sem_name, sizeof sem_name, "%s%d", SEM_PREFIX, i);
        sem_t *s = sem_open(sem_name, 0);
        if (s == SEM_FAILED) { perror("sem_open"); continue; }

        /* Wait with a generous timeout (60 s per worker) */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 60;

        if (sem_timedwait(s, &ts) != 0) {
            fprintf(stderr, "[ORC] Timeout waiting for semaphore %d\n", i);
        }
        sem_close(s);
        sem_unlink(sem_name);
        printf("[ORC] Semaphore %d cleared.\n", i);
    }
}

/* ── step 5 : merge segments ─────────────────────────────────── */
static void merge_segments(int nw)
{
    /* Build a concat list file */
    FILE *f = fopen("segments/concat.txt", "w");
    if (!f) die("fopen concat.txt");
    for (int i = 0; i < nw; i++)
        fprintf(f, "file 'segment_%02d.mp4'\n", i);
    fclose(f);

    /* Clean up old output before merge to ensure it plays right */
    remove(OUTPUT_FILE);

    /* Use mp4 container to save overhead and keep it readable */
    shell("ffmpeg -y -f concat -safe 0 "
          "-i " SEGMENTS_DIR "/concat.txt "
          "-c copy " OUTPUT_FILE " > /dev/null 2>&1");

    printf("[ORC] Merge complete → %s\n", OUTPUT_FILE);

    /* Signal GUI monitor that we are done */
    shm_mark_complete_if_exists();   /* see shared.h for inline helper */
}

/* ── step 6 : launch player ──────────────────────────────────── */
/* GUI handles launching the player */

/* ── shared-memory helpers (implemented here for convenience) ── */

static SharedMem *shm_create(int nw)
{
    int fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (fd < 0) die("shm_open");
    if (ftruncate(fd, sizeof(SharedMem)) < 0) die("ftruncate");

    SharedMem *shm = mmap(NULL, sizeof(SharedMem),
                          PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shm == MAP_FAILED) die("mmap");
    close(fd);

    memset(shm, 0, sizeof(SharedMem));
    shm->num_workers   = nw;
    shm->pipeline_done = 0;
    return shm;
}

static void shm_destroy(SharedMem *shm)
{
    munmap(shm, sizeof(SharedMem));
    shm_unlink(SHM_NAME);
}

/* Inline helper declared in shared.h needs a definition */
void shm_mark_complete_if_exists(void)
{
    int fd = shm_open(SHM_NAME, O_RDWR, 0666);
    if (fd < 0) return;
    SharedMem *shm = mmap(NULL, sizeof(SharedMem),
                          PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shm != MAP_FAILED) {
        shm->pipeline_done = 1;
        munmap(shm, sizeof(SharedMem));
    }
    close(fd);
}

static void create_semaphores(int nw)
{
    char sem_name[64];
    for (int i = 0; i < nw; i++) {
        snprintf(sem_name, sizeof sem_name, "%s%d", SEM_PREFIX, i);
        sem_unlink(sem_name);   /* clean up any leftover */
        sem_t *s = sem_open(sem_name, O_CREAT | O_EXCL, 0666, 0);
        if (s == SEM_FAILED) die("sem_open create");
        sem_close(s);
    }
}

/* ── main ────────────────────────────────────────────────────── */
SharedMem *global_shm = NULL;

static void handle_sigint(int sig)
{
    (void)sig;
    if (global_shm) {
        shm_destroy(global_shm);
        global_shm = NULL;
    }
    if (sig == SIGTSTP) {
        printf("\n[ORC] Ctrl+Z received. Cleaning up IPC and terminating.\n");
    } else {
        printf("\n[ORC] Interrupted. Cleaned up IPC. Exiting.\n");
    }
    exit(EXIT_FAILURE);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s input.mp4 [num_workers]\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char *input_video = argv[1];
    int nw = (argc >= 3) ? atoi(argv[2]) : 4;
    if (nw < 1 || nw > MAX_WORKERS) {
        fprintf(stderr, "num_workers must be 1..%d\n", MAX_WORKERS);
        return EXIT_FAILURE;
    }

    mkdir(SEGMENTS_DIR, 0755);

    /* ── 1. Count frames ── */
    int total_frames = count_frames(input_video);
    if (total_frames <= 0) {
        fprintf(stderr, "[ORC] No frames found in video — aborting.\n");
        return EXIT_FAILURE;
    }
    printf("[ORC] Total frames: %d, workers: %d\n", total_frames, nw);

    /* ── Set up IPC ── */
    signal(SIGINT, handle_sigint);
    signal(SIGTERM, handle_sigint);
    signal(SIGTSTP, handle_sigint);
    
    SharedMem *shm = shm_create(nw);
    global_shm = shm;
    shm->total_frames = total_frames;
    create_semaphores(nw);

    /* ── 2. Fork workers ── */
    WorkerSlot slots[MAX_WORKERS];
    int base  = total_frames / nw;
    int extra = total_frames % nw;
    int offset = 1;   /* FFmpeg frame files are 1-indexed */

    for (int i = 0; i < nw; i++) {
        int count = base + (i < extra ? 1 : 0);
        slots[i].worker_id   = i;
        slots[i].frame_start = offset;
        slots[i].frame_count = count;
        slots[i].respawns    = 0;
        slots[i].done        = 0;
        strncpy(slots[i].input_video, input_video, sizeof(slots[i].input_video)-1);
        slots[i].input_video[sizeof(slots[i].input_video)-1] = '\0';
        slots[i].pid         = spawn_worker(i, offset, count, slots[i].input_video);
        offset += count;
    }

    /* ── 3. Monitor / respawn ── */
    monitor_workers(slots, nw, shm);

    /* ── 4. Wait semaphores ── */
    wait_all_semaphores(nw);

    /* ── 5. Merge ── */
    merge_segments(nw);

    shm_destroy(shm);
    printf("[ORC] Pipeline complete.\n");
    return EXIT_SUCCESS;
}
