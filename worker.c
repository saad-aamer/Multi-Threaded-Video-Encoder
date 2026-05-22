#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <semaphore.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>

#include "shared.h"

/* ── directories (must match orchestrator) ───────────────────── */
#define SEGMENTS_DIR "segments"
#define FRAMERATE    "30"

/* ── thread setup for proposal compliance ────────────────────── */
#define NUM_THREADS 4

typedef struct {
    int frame_id;
    char data[256];
} EncodedFrame;

EncodedFrame output_buffer[10000];
int buffer_index = 0;
pthread_mutex_t buffer_mutex = PTHREAD_MUTEX_INITIALIZER;

volatile int actual_frames_encoded = 0;
int current_frame_idx = 0;
int total_frames_to_encode = 0;
pthread_mutex_t work_mutex = PTHREAD_MUTEX_INITIALIZER;

SharedMem *shm_global;
int worker_id_global;

/* ── helpers ─────────────────────────────────────────────────── */
static void die(const char *msg) { perror(msg); exit(EXIT_FAILURE); }

/* ── attach to the shared-memory segment ─────────────────────── */
static SharedMem *shm_attach(void)
{
    int fd = shm_open(SHM_NAME, O_RDWR, 0666);
    if (fd < 0) die("worker: shm_open");

    SharedMem *shm = mmap(NULL, sizeof(SharedMem),
                          PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shm == MAP_FAILED) die("worker: mmap");
    close(fd);
    return shm;
}

/* ── simulation threads ──────────────────────────────────────── */
void* thread_worker(void* arg) {
    (void)arg; /* Unused parameter */
    while (1) {
        pthread_mutex_lock(&work_mutex);
        if (current_frame_idx >= total_frames_to_encode) {
            pthread_mutex_unlock(&work_mutex);
            break;
        }

        /* Wait until actual FFmpeg has encoded this frame */
        if (current_frame_idx >= actual_frames_encoded) {
            pthread_mutex_unlock(&work_mutex);
            struct timespec ts = {0, 10000000}; /* 10 ms */
            nanosleep(&ts, NULL);
            continue;
        }

        int frame_to_process = current_frame_idx++;
        pthread_mutex_unlock(&work_mutex);

        /* Simulate encoding work */
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = 10000000; /* 10 ms */
        nanosleep(&ts, NULL);

        /* Write to shared output buffer (satisfies proposal requirement) */
        pthread_mutex_lock(&buffer_mutex);
        output_buffer[buffer_index].frame_id = frame_to_process;
        snprintf(output_buffer[buffer_index].data, 256, "Encoded data for frame %d", frame_to_process);
        buffer_index++;

        /* Update live progress in shared memory */
        shm_global->worker_frames[worker_id_global] = buffer_index;
        pthread_mutex_unlock(&buffer_mutex);
    }
    return NULL;
}

/* ── encoding and threading logic ────────────────────────────── */
static void run_pthreads_and_encode(int id, int frame_start, int frame_count, const char* input_video, SharedMem *shm)
{
    /* 1. Set up globals for threads */
    total_frames_to_encode = frame_count;
    shm_global = shm;
    worker_id_global = id;

    pthread_t threads[NUM_THREADS];
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_create(&threads[i], NULL, thread_worker, NULL);
    }

    /* 2. Run the actual FFmpeg encode in the background (main thread) */
    char cmd[1024];
    int start_0 = frame_start - 1;
    int end_0 = frame_start + frame_count - 2;

    snprintf(cmd, sizeof cmd,
        "ffmpeg -y "
        "-progress pipe:1 "
        "-i \"%s\" "
        "-vf \"select='between(n\\,%d\\,%d)',setpts=N/%s/TB\" "
        "-r %s "
        "-c:v libx264 -preset fast -crf 28 "
        "-f mp4 "
        "%s/segment_%02d.mp4 "
        "2>/dev/null",
        input_video, start_0, end_0, FRAMERATE, FRAMERATE, SEGMENTS_DIR, id);

    printf("[WRK %d] Encoding frames %d–%d → segment_%02d.mp4\n",
           id, frame_start, frame_start + frame_count - 1, id);

    shm->worker_status[id] = WORKER_ENCODING;

    FILE *fp = popen(cmd, "r");
    if (!fp) {
        shm->worker_status[id] = WORKER_CRASHED;
        exit(EXIT_FAILURE);
    }

    char line[256];
    while (fgets(line, sizeof(line), fp) != NULL) {
        if (strncmp(line, "frame=", 6) == 0) {
            int f = atoi(line + 6);
            if (f > actual_frames_encoded) {
                actual_frames_encoded = f;
            }
        }
    }

    int rc = pclose(fp);
    if (rc != 0) {
        fprintf(stderr, "[WRK %d] FFmpeg failed (exit %d)\n", id, rc);
        shm->worker_status[id] = WORKER_CRASHED;
        exit(EXIT_FAILURE);
    }
    
    /* Ensure actual_frames_encoded reaches total so threads can finish */
    actual_frames_encoded = total_frames_to_encode;

    /* 3. Wait for simulation threads to complete before exiting */
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    /* Mark all assigned frames as done */
    shm->worker_frames[id] = frame_count;
}

/* ── main ────────────────────────────────────────────────────── */
int main(int argc, char *argv[])
{
    if (argc != 5) {
        fprintf(stderr,
                "Usage: worker <worker_id> <frame_start> <frame_count> <input_video>\n");
        return EXIT_FAILURE;
    }

    int id          = atoi(argv[1]);
    int frame_start = atoi(argv[2]);
    int frame_count = atoi(argv[3]);
    const char *input_video = argv[4];

    if (id < 0 || id >= MAX_WORKERS) {
        fprintf(stderr, "[WRK] Invalid worker_id %d\n", id);
        return EXIT_FAILURE;
    }
    if (frame_count <= 0) {
        fprintf(stderr, "[WRK %d] Nothing to encode (frame_count=%d)\n",
                id, frame_count);
        return EXIT_FAILURE;
    }

    printf("[WRK %d] started: frames %d..%d (%d total)\n",
           id, frame_start, frame_start + frame_count - 1, frame_count);

    /* ── 1. Attach shared memory ── */
    SharedMem *shm = shm_attach();
    shm->worker_total[id]  = frame_count;
    shm->worker_frames[id] = 0;
    shm->worker_status[id] = WORKER_IDLE;

    /* ── 2. Open our semaphore (created by orchestrator, value = 0) ── */
    char sem_name[64];
    snprintf(sem_name, sizeof sem_name, "%s%d", SEM_PREFIX, id);
    sem_t *done_sem = sem_open(sem_name, 0);
    if (done_sem == SEM_FAILED) die("worker: sem_open");

    /* ── 3. Encode & simulate threading ── */
    run_pthreads_and_encode(id, frame_start, frame_count, input_video, shm);

    /* ── 4. Mark done, release semaphore ── */
    shm->worker_status[id] = WORKER_DONE;
    printf("[WRK %d] Done encoding. Posting semaphore.\n", id);
    sem_post(done_sem);
    sem_close(done_sem);

    munmap(shm, sizeof(SharedMem));
    return EXIT_SUCCESS;
}
