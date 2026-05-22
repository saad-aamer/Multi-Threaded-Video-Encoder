# Multi-Process Video Encoding System

## 🎬 Overview
The **Multi-Process Video Encoding System** is a simulated video encoding pipeline that demonstrates core Operating Systems concepts in a practical, real-world scenario. The project showcases process management, thread-based parallelism, mutual exclusion, and POSIX semaphore-based synchronization.

The system orchestrates multiple concurrent worker processes to encode individual chunks of a video using FFmpeg. A real-time web-based GUI monitor displays system-wide progress and metrics via POSIX shared memory.

## ✨ Features
- **Multi-Process Pipeline:** An orchestrator process manages multiple concurrent worker processes using `fork()` and `waitpid()`.
- **Multi-Threading:** Each worker process uses POSIX threads (`pthreads`) to simulate frame-level concurrent processing, protected by `pthread_mutex_t`.
- **Inter-Process Communication (IPC):** Uses POSIX Shared Memory (`shm_open`, `mmap`) to share live encoding progress with the GUI monitor.
- **Process Synchronization:** Named POSIX semaphores (`sem_open`, `sem_post`, `sem_wait`) coordinate completion across pipeline stages.
- **Fault Tolerance:** The orchestrator polls worker health every 100ms. If a worker crashes, the orchestrator seamlessly respawns it (up to 3 times per slot) without halting the entire pipeline.
- **Real-Time Web Dashboard:** A Python-based GUI dashboard (using HTTP Server and Glassmorphism CSS) reads shared memory to provide live progress bars, throughput metrics, and worker status.

## 🏗️ System Architecture
The system follows a producer-consumer pipeline model:
1. **Orchestrator (`orchestrator.c`):** The master process. It calculates total frames using `ffprobe`, allocates POSIX shared memory, creates semaphores, and forks `N` worker processes. It then monitors them and merges the final video chunks once all semaphores signal completion.
2. **Worker (`worker.c`):** The child processes. Each worker spawns multiple `pthreads` to handle frame processing logic. It invokes FFmpeg to encode its assigned video segment and updates its progress in shared memory. Once done, it signals its unique semaphore.
3. **Monitor (`gui_monitor.py`):** A lightweight Python web server that reads the shared memory block, displaying live encoding status, frames completed, and overall pipeline state on a beautiful web UI.
4. **Shared Definitions (`shared.h`):** Contains the data structures defining the shared memory layout, status enums, and macro constants.

## 🛠️ Technology Stack
- **Language:** C11 (Core System), Python 3 (GUI)
- **Concurrency & OS Primitives:** `fork()`, `waitpid()`, `pthreads`, `pthread_mutex`, POSIX Semaphores, POSIX Shared Memory.
- **Video Processing:** `FFmpeg`, `ffprobe`
- **Build System:** GNU Make
- **Frontend:** HTML5, Vanilla CSS (Glassmorphism design)

## 🚀 Getting Started

### Prerequisites
Ensure you have the following installed on your Linux (Ubuntu/Debian) system:
```bash
# Install GCC and build tools
sudo apt install build-essential

# Install FFmpeg and ffprobe for video encoding/parsing
sudo apt install ffmpeg

# Install a media player (optional, for auto-playing the output)
sudo apt install mpv
```
*(No external Python packages like pip are required for the GUI.)*

### Build Instructions
To compile the C source files (`orchestrator` and `worker`), simply run:
```bash
make
```

### Running the System
The easiest way to run the pipeline along with the graphical monitor is to use the provided Makefile target.

```bash
# Run the pipeline with default 4 workers
make run
```
This command will:
1. Create necessary directories (`frames`, `segments`).
2. Start the `orchestrator` process in the background.
3. Start the `gui_monitor.py` web server.
4. Open the live dashboard in your default web browser at `http://localhost:8080`.

**To run manually in two separate terminals:**
Terminal 1 (Monitor):
```bash
python3 gui_monitor.py
```
Terminal 2 (Orchestrator):
```bash
./orchestrator input.mp4 4
```
*(Make sure to provide an `input.mp4` file in the project root before running.)*

### Clean Up
To remove all compiled binaries, object files, and generated video segments/frames:
```bash
make clean
```
*Note: If you abruptly kill the orchestrator, you might need to clean up lingering IPC objects manually from `/dev/shm`.*

## 👨‍💻 Team Members
This project was developed for the **Operating Systems** course at FAST-NUCES by:
- **Faraz Ahmed** (24k0942)
- **Saad Aamer** (23k0921)
- **Muhammed Talha Aamir** (24k0729)

## 📄 License & Academic Integrity
This project is an academic submission for the FAST-NUCES Operating Systems course.
