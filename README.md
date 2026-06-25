# Multi-Process Video Encoding System

## Overview

The **Multi-Process Video Encoding System** is a simulated video encoding pipeline that demonstrates core Operating Systems concepts within a practical, real-world scenario. The project implements process management, thread-based parallelism, mutual exclusion, and POSIX semaphore-based synchronisation.

The system orchestrates multiple concurrent worker processes to encode discrete chunks of a video file using FFmpeg. A real-time, web-based dashboard monitors system-wide progress and metrics by reading from POSIX shared memory.

---

## Features

**Multi-Process Pipeline**
An orchestrator process governs multiple concurrent worker processes, spawning and reaping them via `fork()` and `waitpid()`.

**Multi-Threading**
Each worker process employs POSIX threads (`pthreads`) to simulate frame-level concurrent processing, with all shared state protected by `pthread_mutex_t`.

**Inter-Process Communication**
Utilises POSIX Shared Memory (`shm_open`, `mmap`) to expose live encoding progress to the GUI monitor without requiring inter-process messaging.

**Process Synchronisation**
Named POSIX semaphores (`sem_open`, `sem_post`, `sem_wait`) coordinate completion signals across pipeline stages, ensuring ordered, race-free progression.

**Fault Tolerance**
The orchestrator polls worker health at 100ms intervals. Should a worker terminate unexpectedly, the orchestrator transparently respawns it — up to three times per slot — without halting the broader pipeline.

**Real-Time Web Dashboard**
A lightweight Python-based dashboard (served via HTTP Server with Glassmorphism CSS) reads the shared memory block directly to render live progress bars, throughput metrics, and per-worker status.

---

## System Architecture

The system implements a producer-consumer pipeline model across four components:

1. **Orchestrator (`orchestrator.c`)** — The master process. Calculates total frame count using `ffprobe`, allocates POSIX shared memory, initialises semaphores, and forks `N` worker processes. Continuously monitors worker health and, upon completion of all workers, merges the encoded video chunks into the final output.

2. **Worker (`worker.c`)** — The child processes. Each worker spawns multiple `pthreads` to handle frame processing logic, invokes FFmpeg to encode its assigned video segment, and writes incremental progress back into shared memory. Signals its unique semaphore upon completion.

3. **Monitor (`gui_monitor.py`)** — A lightweight Python web server that reads the shared memory block and renders live encoding status, frame completion counts, and overall pipeline state on a web-based dashboard.

4. **Shared Definitions (`shared.h`)** — Declares the data structures governing the shared memory layout, status enumerations, and macro constants used across all components.

---

## Technology Stack

| Category | Technology |
| :--- | :--- |
| Core Language | C11 |
| GUI Language | Python 3 |
| Concurrency and OS Primitives | `fork()`, `waitpid()`, `pthreads`, `pthread_mutex`, POSIX Semaphores, POSIX Shared Memory |
| Video Processing | FFmpeg, ffprobe |
| Build System | GNU Make |
| Frontend | HTML5, Vanilla CSS (Glassmorphism) |

---

## Getting Started

### Prerequisites

Ensure the following are installed on a Linux (Ubuntu / Debian) system:

```bash
# Install GCC and build tools
sudo apt install build-essential

# Install FFmpeg and ffprobe
sudo apt install ffmpeg

# Install a media player (optional, for auto-playing the output)
sudo apt install mpv
```

No external Python packages are required for the GUI monitor.

### Build

Compile the C source files (`orchestrator` and `worker`) by running:

```bash
make
```

### Running the System

The simplest way to launch the full pipeline alongside the dashboard is via the provided Makefile target:

```bash
# Run the pipeline with the default 4 workers
make run
```

This command will:
1. Create the required working directories (`frames`, `segments`).
2. Start the `orchestrator` process in the background.
3. Start the `gui_monitor.py` web server.
4. Open the live dashboard in the default browser at `http://localhost:8080`.

**To run components manually in two separate terminals:**

Terminal 1 — Monitor:
```bash
python3 gui_monitor.py
```

Terminal 2 — Orchestrator:
```bash
./orchestrator input.mp4 4
```

> Ensure an `input.mp4` file is present in the project root before launching the orchestrator.

### Clean Up

Remove all compiled binaries, object files, and generated video segments and frames:

```bash
make clean
```

> If the orchestrator is terminated abruptly, any lingering IPC objects in `/dev/shm` must be cleaned up manually.

---


## License and Academic Integrity

This project is an academic submission for the FAST-NUCES Operating Systems course.
