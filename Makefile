
CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -std=c11
LDFLAGS = -lrt -lpthread

WORKERS = 4      

.PHONY: all clean run monitor dirs

all: orchestrator worker

orchestrator: orchestrator.c shared.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

worker: worker.c shared.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

dirs:
	mkdir -p frames segments

run: all dirs
	./orchestrator input.mp4 $(WORKERS) > orchestrator.log 2>&1 & python3 gui_monitor.py

monitor:
	python3 gui_monitor.py

clean:
	rm -f orchestrator worker
	rm -rf frames segments output_final.mp4
	rm -f segments/concat.txt

