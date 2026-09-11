CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -O2 -pthread

all: caredispatch send_alarm

caredispatch: caredispatch.c
	$(CC) $(CFLAGS) caredispatch.c -o caredispatch

send_alarm: send_alarm.c
	$(CC) $(CFLAGS) send_alarm.c -o send_alarm

test: all
	./test.sh

clean:
	rm -f caredispatch send_alarm alarms.log stats.bin

.PHONY: all test clean
