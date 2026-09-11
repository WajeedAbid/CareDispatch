#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PORT 5050
#define QUEUE_CAPACITY 16
#define WORKER_COUNT 3
#define MESSAGE_SIZE 128
#define JOURNAL_FILE "alarms.log"
#define STATS_FILE "stats.bin"

typedef enum {
    PRIORITY_SERVICE = 1,
    PRIORITY_MEDICINE = 2,
    PRIORITY_FALL = 3
} Priority;

typedef struct {
    unsigned long id;
    Priority priority;
    time_t received_at;
    char resident[32];
    char message[MESSAGE_SIZE];
} Alarm;

typedef struct {
    Alarm *items[QUEUE_CAPACITY];
    size_t count;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    sem_t free_slots;
} AlarmQueue;

typedef struct {
    uint64_t received;
    uint64_t completed;
    uint64_t fall_alarms;
    uint64_t medicine_alarms;
    uint64_t service_alarms;
} PersistentStats;

typedef struct {
    AlarmQueue queue;
    pthread_mutex_t journal_mutex;
    pthread_mutex_t stats_mutex;
    PersistentStats *stats;
    size_t stats_size;
    int stats_fd;
    int listen_fd;
    int shutting_down;
    unsigned long next_id;
} App;

typedef struct {
    App *app;
    int client_fd;
} ClientJob;

static void fatal(const char *message) {
    perror(message);
    exit(EXIT_FAILURE);
}

static int write_all(int fd, const char *data, size_t length) {
    size_t sent = 0;
    while (sent < length) {
        ssize_t bytes = write(fd, data + sent, length - sent);
        if (bytes > 0) sent += (size_t)bytes;
        else if (bytes == -1 && errno == EINTR) continue;
        else return -1;
    }
    return 0;
}

static const char *priority_name(Priority priority) {
    switch (priority) {
        case PRIORITY_FALL: return "FALL";
        case PRIORITY_MEDICINE: return "MEDICINE";
        default: return "SERVICE";
    }
}

static Priority parse_priority(const char *text) {
    if (strcmp(text, "FALL") == 0) return PRIORITY_FALL;
    if (strcmp(text, "MEDICINE") == 0) return PRIORITY_MEDICINE;
    return PRIORITY_SERVICE;
}

static void queue_init(AlarmQueue *queue) {
    memset(queue, 0, sizeof(*queue));
    if (pthread_mutex_init(&queue->mutex, NULL) != 0 ||
        pthread_cond_init(&queue->not_empty, NULL) != 0 ||
        sem_init(&queue->free_slots, 0, QUEUE_CAPACITY) == -1) {
        fatal("queue_init");
    }
}

static void queue_destroy(AlarmQueue *queue) {
    pthread_mutex_destroy(&queue->mutex);
    pthread_cond_destroy(&queue->not_empty);
    sem_destroy(&queue->free_slots);
}

/* Inserts into a priority queue: higher priority is removed first. */
static int queue_push(App *app, Alarm *alarm) {
    while (sem_wait(&app->queue.free_slots) == -1) {
        if (errno != EINTR) return -1;
        if (app->shutting_down) return -1;
    }

    pthread_mutex_lock(&app->queue.mutex);
    if (app->shutting_down) {
        pthread_mutex_unlock(&app->queue.mutex);
        sem_post(&app->queue.free_slots);
        return -1;
    }
    app->queue.items[app->queue.count++] = alarm;
    pthread_cond_signal(&app->queue.not_empty);
    pthread_mutex_unlock(&app->queue.mutex);
    return 0;
}

static Alarm *queue_pop(App *app) {
    Alarm *alarm;
    size_t best = 0;
    size_t index;

    pthread_mutex_lock(&app->queue.mutex);
    while (app->queue.count == 0 && !app->shutting_down)
        pthread_cond_wait(&app->queue.not_empty, &app->queue.mutex);

    if (app->queue.count == 0 && app->shutting_down) {
        pthread_mutex_unlock(&app->queue.mutex);
        return NULL;
    }

    for (index = 1; index < app->queue.count; index++) {
        if (app->queue.items[index]->priority > app->queue.items[best]->priority)
            best = index;
    }
    alarm = app->queue.items[best];
    for (index = best; index + 1 < app->queue.count; index++)
        app->queue.items[index] = app->queue.items[index + 1];
    app->queue.count--;
    pthread_mutex_unlock(&app->queue.mutex);
    sem_post(&app->queue.free_slots);
    return alarm;
}

static void journal_alarm(App *app, const Alarm *alarm, const char *state) {
    char timestamp[32];
    struct tm local_time;
    FILE *file;

    localtime_r(&alarm->received_at, &local_time);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &local_time);
    pthread_mutex_lock(&app->journal_mutex);
    file = fopen(JOURNAL_FILE, "a");
    if (file != NULL) {
        fprintf(file, "%s | #%lu | %-8s | %-15s | %-9s | %s\n",
                timestamp, alarm->id, priority_name(alarm->priority),
                alarm->resident, state, alarm->message);
        fflush(file);
        fsync(fileno(file));
        fclose(file);
    }
    pthread_mutex_unlock(&app->journal_mutex);
}

static void update_received_stats(App *app, Priority priority) {
    pthread_mutex_lock(&app->stats_mutex);
    app->stats->received++;
    if (priority == PRIORITY_FALL) app->stats->fall_alarms++;
    else if (priority == PRIORITY_MEDICINE) app->stats->medicine_alarms++;
    else app->stats->service_alarms++;
    msync(app->stats, app->stats_size, MS_ASYNC);
    pthread_mutex_unlock(&app->stats_mutex);
}

static void update_completed_stats(App *app) {
    pthread_mutex_lock(&app->stats_mutex);
    app->stats->completed++;
    msync(app->stats, app->stats_size, MS_ASYNC);
    pthread_mutex_unlock(&app->stats_mutex);
}

static void *worker_main(void *argument) {
    App *app = argument;
    Alarm *alarm;

    while ((alarm = queue_pop(app)) != NULL) {
        printf("Worker %lu handles alarm #%lu (%s) for %s\n",
               (unsigned long)pthread_self(), alarm->id,
               priority_name(alarm->priority), alarm->resident);
        journal_alarm(app, alarm, "STARTED");
        struct timespec work = {.tv_sec = 0, .tv_nsec = 150000000L};
        nanosleep(&work, NULL);
        journal_alarm(app, alarm, "COMPLETED");
        update_completed_stats(app);
        free(alarm);
    }
    return NULL;
}

static int parse_alarm(char *input, Alarm *alarm) {
    char *save = NULL;
    char *priority = strtok_r(input, "|\r\n", &save);
    char *resident = strtok_r(NULL, "|\r\n", &save);
    char *message = strtok_r(NULL, "\r\n", &save);

    if (priority == NULL || resident == NULL || message == NULL) return -1;
    alarm->priority = parse_priority(priority);
    snprintf(alarm->resident, sizeof(alarm->resident), "%s", resident);
    snprintf(alarm->message, sizeof(alarm->message), "%s", message);
    alarm->received_at = time(NULL);
    return 0;
}

static void *client_main(void *argument) {
    ClientJob *job = argument;
    App *app = job->app;
    int client_fd = job->client_fd;
    char input[256];
    ssize_t bytes;
    Alarm *alarm = malloc(sizeof(*alarm));
    free(job);

    if (alarm == NULL) {
        close(client_fd);
        return NULL;
    }
    bytes = read(client_fd, input, sizeof(input) - 1);
    if (bytes <= 0) {
        free(alarm);
        close(client_fd);
        return NULL;
    }
    input[bytes] = '\0';

    if (parse_alarm(input, alarm) == -1) {
        (void)write_all(client_fd, "ERROR: use TYPE|NAME|MESSAGE\n", 29);
        free(alarm);
    } else {
        pthread_mutex_lock(&app->stats_mutex);
        alarm->id = app->next_id++;
        pthread_mutex_unlock(&app->stats_mutex);
        update_received_stats(app, alarm->priority);
        journal_alarm(app, alarm, "RECEIVED");
        if (queue_push(app, alarm) == 0)
            (void)write_all(client_fd, "OK: alarm queued\n", 17);
        else
            free(alarm);
    }
    close(client_fd);
    return NULL;
}

static void map_stats(App *app) {
    app->stats_size = sizeof(PersistentStats);
    app->stats_fd = open(STATS_FILE, O_RDWR | O_CREAT, 0644);
    if (app->stats_fd == -1) fatal("open stats");
    if (ftruncate(app->stats_fd, (off_t)app->stats_size) == -1)
        fatal("ftruncate stats");
    app->stats = mmap(NULL, app->stats_size, PROT_READ | PROT_WRITE,
                      MAP_SHARED, app->stats_fd, 0);
    if (app->stats == MAP_FAILED) fatal("mmap stats");
    app->next_id = (unsigned long)app->stats->received + 1;
}

static int create_server_socket(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int reuse = 1;
    struct sockaddr_in address;

    if (fd == -1) fatal("socket");
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(PORT);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) == -1)
        fatal("bind");
    if (listen(fd, 16) == -1) fatal("listen");
    if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) == -1)
        fatal("fcntl");
    return fd;
}

static void print_report_with_child(App *app) {
    int channel[2];
    pid_t child;
    char report[512];
    ssize_t bytes;
    PersistentStats snapshot;

    /* A child must not lock a mutex inherited from a multithreaded parent. */
    pthread_mutex_lock(&app->stats_mutex);
    snapshot = *app->stats;
    pthread_mutex_unlock(&app->stats_mutex);

    if (pipe(channel) == -1) return;
    child = fork();
    if (child == -1) {
        close(channel[0]);
        close(channel[1]);
        return;
    }
    if (child == 0) {
        close(channel[0]);
        int length = snprintf(report, sizeof(report),
            "\n=== CareDispatch report ===\nReceived: %lu\nCompleted: %lu\n"
            "Falls: %lu | Medicine: %lu | Service: %lu\n",
            (unsigned long)snapshot.received,
            (unsigned long)snapshot.completed,
            (unsigned long)snapshot.fall_alarms,
            (unsigned long)snapshot.medicine_alarms,
            (unsigned long)snapshot.service_alarms);
        (void)write_all(channel[1], report, (size_t)length);
        close(channel[1]);
        _exit(EXIT_SUCCESS);
    }
    close(channel[1]);
    while ((bytes = read(channel[0], report, sizeof(report))) > 0)
        (void)write_all(STDOUT_FILENO, report, (size_t)bytes);
    close(channel[0]);
    waitpid(child, NULL, 0);
}

static void shutdown_app(App *app, pthread_t workers[]) {
    size_t index;

    pthread_mutex_lock(&app->queue.mutex);
    app->shutting_down = 1;
    pthread_cond_broadcast(&app->queue.not_empty);
    pthread_mutex_unlock(&app->queue.mutex);
    for (index = 0; index < WORKER_COUNT; index++) pthread_join(workers[index], NULL);
    print_report_with_child(app);
    msync(app->stats, app->stats_size, MS_SYNC);
    munmap(app->stats, app->stats_size);
    close(app->stats_fd);
    close(app->listen_fd);
    pthread_mutex_destroy(&app->journal_mutex);
    pthread_mutex_destroy(&app->stats_mutex);
    queue_destroy(&app->queue);
}

int main(void) {
    App app;
    pthread_t workers[WORKER_COUNT];
    sigset_t signals;
    size_t index;

    memset(&app, 0, sizeof(app));
    queue_init(&app.queue);
    pthread_mutex_init(&app.journal_mutex, NULL);
    pthread_mutex_init(&app.stats_mutex, NULL);
    map_stats(&app);
    app.listen_fd = create_server_socket();

    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &signals, NULL);
    for (index = 0; index < WORKER_COUNT; index++)
        pthread_create(&workers[index], NULL, worker_main, &app);

    printf("CareDispatch listens on 127.0.0.1:%d\n", PORT);
    printf("Send: FALL|Anna|Resident fell in bathroom\n");

    while (!app.shutting_down) {
        struct timespec timeout = {.tv_sec = 0, .tv_nsec = 200000000L};
        siginfo_t signal_info;
        int client_fd;

        if (sigtimedwait(&signals, &signal_info, &timeout) >= 0) break;
        client_fd = accept(app.listen_fd, NULL, NULL);
        if (client_fd == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
            break;
        }
        ClientJob *job = malloc(sizeof(*job));
        pthread_t client_thread;
        if (job == NULL) {
            close(client_fd);
            continue;
        }
        job->app = &app;
        job->client_fd = client_fd;
        if (pthread_create(&client_thread, NULL, client_main, job) == 0)
            pthread_detach(client_thread);
        else {
            free(job);
            close(client_fd);
        }
    }

    puts("\nStopping safely...");
    shutdown_app(&app, workers);
    return EXIT_SUCCESS;
}
