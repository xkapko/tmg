#define _POSIX_C_SOURCE 200809L

#include "color.h"

#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define TMG_MAJOR 1
#define TMG_MINOR 0
#define TMG_PATCH 1

#define DEFAULT_SOCKET_PATH "tmg.socket"
#define DEFAULT_SOCKET_BACKLOG 32
#define DEFAULT_PROGRAM "notify-send"
#define DEFAULT_PQ_CAPACITY 32
#define DEFAULT_ENV_VAR "TMG_SOCKET"

#define MAXSIZE 1024

#define OP_CREATE 0
#define OP_CHANGE 1
#define OP_DELETE 2
#define OP_LIST 3
#define OP_QUIT 4

#define N_OPS 5

/// Log a message to standard error.
#define LOG(...) dprintf(2, __VA_ARGS__);
/// Send a reply to a client on connection `conn`.
#define REPLY(conn, ...) dprintf(conn, __VA_ARGS__);
/// Macro for logging exiting when encountering an unrecoverable error.
#define UNRECOVERABLE(res, msg, action) \
    res = action;                       \
    if (res) {                          \
        perror(msg);                    \
        exit(1);                        \
    }

#ifdef DEBUG
#define DEBUGMSG(m) printf("{\n\tsecs: %d\n" \
                           "\tmins: %d\n"    \
                           "\thours: %d\n"   \
                           "\tid: %d\n"      \
                           "\top: %d\n"      \
                           "\tmsg: %s\n}\n", \
        m.secs,                              \
        m.minutes,                           \
        m.hours,                             \
        m.id,                                \
        m.op,                                \
        m.arg)

#define DEBUGARGS()                                                                                                                                           \
    printf("daemon = %d; list = %d\n", arg_daemon, arg_list);                                                                                                 \
    printf("secs = %d, mins = %d, hours = %d, change = %d, delete = %d, backlog = %d\n", arg_secs, arg_mins, arg_hours, arg_change, arg_delete, arg_backlog); \
    printf("socket_path = '%s', calling = '%s'\n", arg_sock_path, arg_run);

#define DEBUGTIMER(t) printf("{\n\tid: %d\n"  \
                             "\tbegin: %ld\n" \
                             "\tend: %ld\n"   \
                             "\tmsg: %s\n"    \
                             "}\n",           \
        t.id,                                 \
        t.begin,                              \
        t.end,                                \
        t.arg);

#define DEBUGPRINT(...) dprintf(2, __VA_ARGS__);
#else
#define DEBUGMSG(m) (void) 0;
#define DEBUGARGS() (void) 0;
#define DEBUGTIMER(t) (void) 0;
#define DEBUGPRINT(...) (void) 0;
#endif

// command line options
bool arg_daemon = false, arg_quiet = false, arg_list = false, arg_quit = false;
int arg_change = 0, arg_delete = 0, arg_secs = 0, arg_hours = 0, arg_mins = 0, arg_backlog = 0;
char *arg_run = NULL;
char *arg_sock_path = NULL;

/// Print help message to standard output.
void help()
{
    dprintf(2, "usage: tmg [-D] [-H HOURS] [-m MINUTES] [-s SECONDS] [-R COMMAND]\n");
    dprintf(2, "           [-l] [-d ID] [-c ID] [-b BACKLOG] [-S SOCKET_PATH]\n");
    dprintf(2, "           [-h] [-v]\n\n");
    dprintf(2, "timer manager: create & manage timed commands\n\n");
    dprintf(2, "options:\n");
    dprintf(2, "  -h\t\tdisplay this help message\n");
    dprintf(2, "  -v\t\tprint the version information\n");
    dprintf(2, "  -l\t\tlist active timers\n");
    dprintf(2, "  -H <num>\tset number of hours when creating/changing a timer\n");
    dprintf(2, "  -m <num>\tset number of minutes when creating/changing a timer\n");
    dprintf(2, "  -s <num>\tset number of seconds when creating/changing a timer\n");
    dprintf(2, "  -R <str>\tcommand to run once timer finishes (<shell> -c <cmd>)\n");
    dprintf(2, "  -S <str>\tpath to daemon socket\n");
    dprintf(2, "  -d <num>\tdelete timer by id\n");
    dprintf(2, "  -c <num>\tchange timer by id\n");
    dprintf(2, "  -b <num>\tbacklog of socket connections\n");
}

void version()
{
    printf("tmg %d.%d.%d\n", TMG_MAJOR, TMG_MINOR, TMG_PATCH);
}

/// A single timer.
typedef struct
{
    // Unique ID of a timer.
    int id;
    // Argument for the timer operation.
    char arg[MAXSIZE];
    // Beginning of the timer.
    time_t begin;
    // End of the timer.
    time_t end;
    // Internal status of the timer.
    // tmg_timer_status_t status;
} tmg_timer_t;

/// Timer manager.
typedef struct
{
    // current id
    int current;
    // socket file descriptor
    int sockfd;
    // socket path
    char *sock_path;
    // timer thread id
    pthread_t timer_thread;
    // manager mutex
    pthread_mutex_t mutex;
    // priority queue of timers
    struct q
    {
        tmg_timer_t *timers;
        size_t len;
        size_t cap;
    } q;
} tmg_manager_t;

/// Messages sent from the client.
typedef struct
{
    // timer id (where sensible), operation
    int id, op;
    // changed/created times
    int secs, minutes, hours;
    // changed/created argument
    char arg[MAXSIZE];
} tmg_client_message_t;

/// Buffer for replies received from the daemon.
typedef struct
{
    char body[MAXSIZE * 2];
} tmg_reply_t;

int create_thread(tmg_manager_t *mgr);

/// Timer comparison function.
int timer_cmp(const void *a, const void *b)
{
    tmg_timer_t *ta = (tmg_timer_t *) a;
    tmg_timer_t *tb = (tmg_timer_t *) b;

    return -(ta->end - tb->end);
}

/// Resolve the socket path.
///
/// The order in which the values are used:
/// 1. command line option
/// 2. value of the environmental variable `TMG_SOCKET`
/// 3. the default path is `$XDG_RUNTIME_DIR/tmg.socket`
char *socket_path()
{
    char *ptr = NULL;
    char *env_val = NULL;

    if (arg_sock_path != NULL) {
        ptr = strdup(arg_sock_path);
    } else if ((env_val = getenv(DEFAULT_ENV_VAR)) != NULL) {
        ptr = strdup(env_val);
    } else {
        env_val = getenv("XDG_RUNTIME_DIR");
        if (env_val != NULL) {
            ptr = malloc(strlen(env_val) + strlen(DEFAULT_SOCKET_PATH) + 2); // |env_val| + |DEFAULT_SOCKET_PATH| + '/' + '\0'
            if (ptr != NULL) {
                sprintf(ptr, "%s/%s", env_val, DEFAULT_SOCKET_PATH);
            }
        }
    }

    return ptr;
}

/// Enqueue a timer onto the priority queue.
///
/// After adding the timer to the queue, the queue is sorted in a descending order,
/// so that the last timer in the list is the timer which ends the soonest. A thread is
/// spawned for the timer.
int enq(tmg_manager_t *mgr, tmg_timer_t *timer)
{
    int last_id, new_last_id;
    tmg_timer_t *new;
    int res;
    bool restart_thread = false;

    UNRECOVERABLE(res, "unrecoverable: could not acquire mutex lock", pthread_mutex_lock(&mgr->mutex));
    last_id = (mgr->q.len != 0) ? mgr->q.timers[mgr->q.len - 1].id : -1;
    if (mgr->q.cap == mgr->q.len) {
        new = realloc(mgr->q.timers, sizeof(tmg_timer_t) * mgr->q.cap * 2);
        if (new == NULL) {
            pthread_mutex_unlock(&mgr->mutex);
            perror("enqueue realloc");
            return -1;
        }
        mgr->q.timers = new;
        mgr->q.cap *= 2;
    }

    memcpy(&mgr->q.timers[mgr->q.len++], timer, sizeof(tmg_timer_t));
    qsort(mgr->q.timers, mgr->q.len, sizeof(tmg_timer_t), timer_cmp);
    new_last_id = mgr->q.timers[mgr->q.len - 1].id;
    restart_thread = new_last_id != last_id;
    if (restart_thread && last_id != -1) {
        pthread_cancel(mgr->timer_thread); // SAFETY: fails only if the thread is dead already, but no harm either way.
    }
    UNRECOVERABLE(res, "unrecoverable: someone stole my mutex!", pthread_mutex_unlock(&mgr->mutex));
    if (restart_thread)
        return create_thread(mgr);

    return 0;
}

/// Remove the last timer from the queue.
///
/// This function is not responsible for closing/restarting the timer thread.
int deq(tmg_manager_t *mgr)
{
    int res;
    UNRECOVERABLE(res, "unrecoverable: could not acquire mutex lock", pthread_mutex_lock(&mgr->mutex));
    if (mgr->q.len <= 0) {
        pthread_mutex_unlock(&mgr->mutex);
        return 0;
    }

    --(mgr->q.len);
    UNRECOVERABLE(res, "unrecoverable: someone stole my mutex!", pthread_mutex_unlock(&mgr->mutex));

    return 0;
}

/// This function runs in the timer thread.
///
/// After the timer ends, this function is responsible for scheduling the next timer thread.
void *thread_routine(void *args)
{
    tmg_manager_t *mgr = (tmg_manager_t *) args;
    tmg_timer_t timer = { 0 };
    int res;
    time_t secs, curr_time;
    char *prog;

    // Should be able to be canceled at any time (when the cancellation is *ENABLED*).
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
    // Disable cancellation, take the manager mutex and copy necessary stuff.
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
    pthread_mutex_lock(&mgr->mutex); // SAFETY: it is okay to wait for mutex lock, because the thread should always be canceled before creating a new one.
    curr_time = time(NULL);
    memcpy(&timer, &mgr->q.timers[mgr->q.len - 1], sizeof(tmg_timer_t));
    LOG("%s: new thread for timer with id '%d' created\n", TRACE, timer.id);
    secs = timer.end - curr_time;
    DEBUGPRINT("new timer thread for timer: ");
    DEBUGTIMER(timer);
    // register cleanup handler
    pthread_cleanup_push((void (*)(void *)) free, prog);
    UNRECOVERABLE(res, "unrecoverable: someone stole my mutex!", pthread_mutex_unlock(&mgr->mutex));
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);

    if (secs > 0) {
        // The thread sleeps for the number of seconds needed to finish the timer.
        sleep(secs);
    } else {
        // If the timer is finished, we skip the sleeping, log the discrepancy and schedule the next timer.
        LOG("%s: timer id: %d is delayed by %ld seconds\n", WARN, timer.id, -secs);
    }

    res = system(timer.arg);
    if (res != 0) {
        LOG("%s could not run command '%s': %s", ERR, timer.arg, strerror(errno));
    }

    pthread_cleanup_pop(true);

    // Disable cancellation and try to schedule the next thread.
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
    pthread_mutex_lock(&mgr->mutex);
    // Dequeue the finished timer.
    deq(mgr);
    // If there are no more timers, cancel the thread and wait.
    if (mgr->q.len <= 0) {
        pthread_mutex_unlock(&mgr->mutex);
        pthread_exit(NULL);
    }
    // Because we use a **RECURSIVE** mutex, we can keep the mutex locked even though `create_thread()` also locks the mutex.
    create_thread(mgr); // SAFETY: return value not checked, because if this fails, the entire process exits.
    // Unlock and exit the thread.
    UNRECOVERABLE(res, "unrecoverable: someone stole my mutex!", pthread_mutex_unlock(&mgr->mutex));
    pthread_exit(NULL);
}

/// Function responsible for creating the detached timer thread.
int create_thread(tmg_manager_t *mgr)
{
    pthread_t thread;
    int res;

    UNRECOVERABLE(res, "unrecoverable: could not acquire mutex lock", pthread_mutex_lock(&mgr->mutex));
    UNRECOVERABLE(res, "unrecoverable: could not create timer thread", pthread_create(&thread, NULL, thread_routine, mgr));
    pthread_detach(thread);
    mgr->timer_thread = thread;
    UNRECOVERABLE(res, "unrecoverable: someome stole my mutex!", pthread_mutex_unlock(&mgr->mutex));

    return 0;
}

/// Initialize the timer manager.
int init_manager(tmg_manager_t *mgr)
{
    int res;
    struct sockaddr_un addr;
    pthread_mutexattr_t attrs;

    mgr->current = 0;
    mgr->sock_path = socket_path();
    mgr->sockfd = socket(AF_UNIX, SOCK_STREAM, 0);

    mgr->q.len = 0;
    mgr->q.cap = DEFAULT_PQ_CAPACITY;
    mgr->q.timers = calloc(mgr->q.cap, sizeof(tmg_timer_t));

    pthread_mutexattr_init(&attrs);
    pthread_mutexattr_settype(&attrs, PTHREAD_MUTEX_RECURSIVE);
    res = pthread_mutex_init(&mgr->mutex, &attrs);
    if (mgr->sock_path == NULL || mgr->sockfd == -1 || mgr->q.timers == NULL) {
        perror("resources");
        goto init_manager_err;
    }

    memset(&addr, 0, sizeof(struct sockaddr_un));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, mgr->sock_path, sizeof(addr.sun_path) - 1);

    (void) unlink(mgr->sock_path);
    res = bind(mgr->sockfd, (struct sockaddr *) &addr, sizeof(struct sockaddr_un));
    if (res == -1) {
        perror("bind()");
        goto init_manager_err;
    }

    return 0;

init_manager_err:
    free(mgr->sock_path);
    free(mgr->q.timers);
    close(mgr->sockfd);
    pthread_mutex_destroy(&mgr->mutex);
    return -1;
}

/// Free the timer manager.
void free_manager(tmg_manager_t *mgr)
{
    close(mgr->sockfd);
    (void) unlink(mgr->sock_path);
    free(mgr->sock_path);
    free(mgr->q.timers);
    pthread_mutex_destroy(&mgr->mutex);
}

/// Daemon's handler for creating a new timer.
int create_timer(tmg_manager_t *mgr, const tmg_client_message_t *msg, int conn)
{
    tmg_timer_t timer = { 0 };
    int res;

    LOG("%s: creating timer\n", INFO)
    timer.id = ++(mgr->current);
    timer.begin = time(NULL);
    timer.end = timer.begin + msg->secs + msg->minutes * 60 + msg->hours * 60 * 60;
    memcpy(timer.arg, msg->arg, MAXSIZE);

    res = enq(mgr, &timer);
    if (res) {
        REPLY(conn, "%s timer creation failed\n", NOK);
        return -1;
    }
    REPLY(conn, "%s timer created successfully with id '%d'\n", OK, mgr->current);
    return 0;
}

/// Daemon's handler for changing timer operation.
int change_timer(tmg_manager_t *mgr, const tmg_client_message_t *msg, int conn)
{
    tmg_timer_t *timer;
    bool restart_thread, found = false;
    int last_id;
    int res;
    LOG("%s: changing timer with id '%d'\n", INFO, msg->id);

    UNRECOVERABLE(res, "unrecoverable: could not acquire mutex", pthread_mutex_lock(&mgr->mutex));
    if (mgr->q.len <= 0) {
        REPLY(conn, "%s no timers found\n", OK);
        UNRECOVERABLE(res, "unrecoverable: someome stole my mutex!", pthread_mutex_unlock(&mgr->mutex));
        return 0;
    }
    last_id = mgr->q.timers[mgr->q.len - 1].id;
    for (size_t i = 0; i < mgr->q.len; i++) {
        if (mgr->q.timers[i].id != msg->id) {
            continue;
        }
        found = true;
        // SAFETY: no real errors are returned on thread cancellation.
        pthread_cancel(mgr->timer_thread);
        timer = &mgr->q.timers[i];
        // SEMANTICS: changing a timer modifies its end time, by add (subtracting in the case of negative values)
        // the time values to the end time of the timer.
        timer->end = time(NULL) + msg->secs + msg->minutes * 60 + msg->hours * 60 * 60;
        if (strlen(msg->arg) > 0) {
            strncpy(timer->arg, msg->arg, MAXSIZE);
        }
        qsort(mgr->q.timers, mgr->q.len, sizeof(tmg_timer_t), timer_cmp);
        if ((restart_thread = last_id != mgr->q.timers[mgr->q.len - 1].id || last_id == msg->id)) {
            pthread_cancel(mgr->timer_thread);
        }
        break;
    }
    if (!found) {
        REPLY(conn, "%s no timers matching id '%d' found\n", OK, msg->id);
        UNRECOVERABLE(res, "unrecoverable: someome stole my mutex!", pthread_mutex_unlock(&mgr->mutex));
        return 0;
    }
    if (restart_thread)
        create_thread(mgr); // SAFETY: this fails, process exits.
    UNRECOVERABLE(res, "unrecoverable: someome stole my mutex!", pthread_mutex_unlock(&mgr->mutex));

    REPLY(conn, "%s timer with id '%d' changed successfully\n", OK, msg->id);
    return 0;
}

/// Daemon's handler for delete timer operation.
int delete_timer(tmg_manager_t *mgr, const tmg_client_message_t *msg, int conn)
{
    tmg_timer_t a, tmp = { 0 };
    int res;
    bool restart_thread, found = false;
    LOG("%s: deleting timer with id '%d'\n", INFO, msg->id);

    UNRECOVERABLE(res, "unrecoverable: could not acquire mutex", pthread_mutex_lock(&mgr->mutex));
    if (mgr->q.len <= 0) {
        REPLY(conn, "%s no timers found\n", NOK);
        return 0;
    }
    if ((restart_thread = msg->id == mgr->q.timers[mgr->q.len - 1].id)) {
        deq(mgr);
        pthread_cancel(mgr->timer_thread);
        if (restart_thread && mgr->q.len > 0)
            create_thread(mgr); // SAFETY: this fails, process exits.
        UNRECOVERABLE(res, "unrecoverable: someome stole my mutex!", pthread_mutex_unlock(&mgr->mutex));
        REPLY(conn, "%s deleted timer with id '%d'\n", OK, msg->id);
        return 0;
    }
    for (size_t i = 0; i < mgr->q.len; i++) {
        if (mgr->q.timers[i].id == msg->id) {
            memcpy(&a, &mgr->q.timers[i], sizeof(tmg_timer_t));
            memcpy(&tmp, &mgr->q.timers[mgr->q.len - 1], sizeof(tmg_timer_t));
            memcpy(&mgr->q.timers[mgr->q.len - 1], &a, sizeof(tmg_timer_t));
            memcpy(&mgr->q.timers[i], &tmp, sizeof(tmg_timer_t));
            deq(mgr);
            found = true;
            break;
        }
    }
    if (!found) {
        UNRECOVERABLE(res, "unrecoverable: someome stole my mutex!", pthread_mutex_unlock(&mgr->mutex));
        REPLY(conn, "%s no timers matching id '%d' found\n", OK, msg->id);
        return 0;
    }
    qsort(mgr->q.timers, mgr->q.len, sizeof(tmg_timer_t), timer_cmp);
    if (restart_thread && mgr->q.len > 0)
        create_thread(mgr); // SAFETY: this fails, process exits.
    UNRECOVERABLE(res, "unrecoverable: someome stole my mutex!", pthread_mutex_unlock(&mgr->mutex));
    REPLY(conn, "%s deleted timer with id '%d'\n", OK, msg->id);

    return 0;
}

/// Daemon's handler for list timer operation.
int list_timers(tmg_manager_t *mgr, const tmg_client_message_t *msg, int conn)
{
    (void) msg;
    int res;
    tmg_timer_t timer;
    struct tm *lt;
    char *table_buf;
    char begin_buf[64];
    char end_buf[64];
    size_t maxsize, currsize, maxarg, currarg;
    LOG("%s: listing timers\n", INFO);
    maxsize = 105;
    maxarg = 31;

    UNRECOVERABLE(res, "unrecoverable: could not acquire mutex", pthread_mutex_lock(&mgr->mutex));
    for (size_t i = 0; i < mgr->q.len; i++) {
        currsize = 0;
        timer = mgr->q.timers[(mgr->q.len - 1) - i];
        lt = localtime(&timer.begin);
        strftime(begin_buf, sizeof(begin_buf), "%a %b %e %H:%M:%S %Y", lt);
        lt = localtime(&timer.end);
        strftime(end_buf, sizeof(end_buf), "%a %b %e %H:%M:%S %Y", lt);
        currarg = strlen(timer.arg);
        currsize = snprintf(NULL, 0, "| %-10d | %-25s | %-25s | %-*s |\n", timer.id, begin_buf, end_buf, (int) currarg, timer.arg);
        maxsize = (currsize > maxsize ? currsize : maxsize);
        maxarg = (currarg > maxarg ? currarg : maxarg);
    }

    table_buf = calloc(maxsize, 1);
    if (table_buf == NULL) {
        UNRECOVERABLE(res, "unrecoverable: someome stole my mutex!", pthread_mutex_unlock(&mgr->mutex));
        return 1;
    }
    table_buf[0] = '+';
    for (size_t i = 1; i < maxsize - 2; i++) {
        table_buf[i] = '-';
    }
    table_buf[maxsize - 2] = '+';
    table_buf[maxsize - 1] = '\n';

    REPLY(conn, "%s listing timers:\n", OK);
    REPLY(conn, "%s", table_buf);
    REPLY(conn, "| %-10s | %-25s | %-25s | %-*s |\n", "ID", "BEGIN TIME", "END TIME", (int) maxarg, "ARGUMENT");
    for (size_t i = 0; i < mgr->q.len; i++) {
        timer = mgr->q.timers[(mgr->q.len - 1) - i];
        lt = localtime(&timer.begin);
        strftime(begin_buf, sizeof(begin_buf), "%a %b %e %H:%M:%S %Y", lt);
        lt = localtime(&timer.end);
        strftime(end_buf, sizeof(end_buf), "%a %b %e %H:%M:%S %Y", lt);
        REPLY(conn, "%s", table_buf);
        REPLY(conn, "| %-10d | %-25s | %-25s | %-*s |\n", timer.id, begin_buf, end_buf, (int) maxarg, timer.arg);
    }
    REPLY(conn, "%s", table_buf);
    UNRECOVERABLE(res, "unrecoverable: someome stole my mutex!", pthread_mutex_unlock(&mgr->mutex));
    return 0;
}

/// Daemon's handler for quit operation.
int quit_daemon(tmg_manager_t *mgr, const tmg_client_message_t *msg, int conn)
{
    (void) msg;
    int res;
    LOG("%s: quit daemon request\n", INFO);
    UNRECOVERABLE(res, "unrecoverable: could not acquire mutex", pthread_mutex_lock(&mgr->mutex));
    // Check if the thread is running and cancel it
    if (pthread_kill(mgr->timer_thread, 0) == ESRCH) {
        pthread_cancel(mgr->timer_thread);
    }

    REPLY(conn, "%s quit request recieved\n", OK);
    UNRECOVERABLE(res, "unrecoverable: someome stole my mutex!", pthread_mutex_unlock(&mgr->mutex));
    return 0;
}

/// Map from operations to handlers.
static int (*handlers[N_OPS])(tmg_manager_t *, const tmg_client_message_t *, int) = {
    create_timer,
    change_timer,
    delete_timer,
    list_timers,
    quit_daemon,
};

/// Main function of the client.
int client_main()
{
    int res, sock;
    ssize_t wb, rb;
    tmg_client_message_t msg = { 0 };
    tmg_reply_t reply = { 0 };
    struct sockaddr_un addr;
    char *sock_path;

    sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock == -1) {
        perror("socket()");
        return -1;
    }

    sock_path = socket_path();
    if (sock_path == NULL) {
        perror("sock_path");
        return -1;
    }

    memset(&addr, 0, sizeof(struct sockaddr_un));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    res = connect(sock, (struct sockaddr *) &addr, arg_backlog ? arg_backlog : DEFAULT_SOCKET_BACKLOG);
    if (res == -1) {
        LOG("%s could not connect to socket '%s', is the daemon running?\n", ERR, sock_path);
        free(sock_path);
        return -1;
    }

    msg.hours = arg_hours;
    msg.minutes = arg_mins;
    msg.secs = arg_secs;
    // So that we do not overflow the buffer
    strncpy(msg.arg, arg_run != NULL ? arg_run : "", MAXSIZE - 1);

    if (arg_change) {
        msg.op = OP_CHANGE;
        msg.id = arg_change;
    } else if (arg_delete) {
        msg.op = OP_DELETE;
        msg.id = arg_delete;
    } else if (arg_list) {
        msg.op = OP_LIST;
    } else if (arg_quit) {
        msg.op = OP_QUIT;
    }

    wb = write(sock, &msg, sizeof(tmg_client_message_t));
    if (wb == -1) {
        perror("write(message)");
        free(sock_path);
        close(sock);
        return -1;
    }

    while ((rb = read(sock, &reply, sizeof(tmg_reply_t))) > 0) {
        write(0, reply.body, sizeof(reply.body));
        memset(&reply, 0, sizeof(reply));
    }

    free(sock_path);
    close(sock);

    return 0;
}

/// Main function of the daemon.
int daemon_main()
{
    int res, conn, backlog;
    ssize_t rb;
    // Buffer for client messages
    // Replies are handled in
    tmg_client_message_t msg = { 0 };
    tmg_manager_t mgr = { 0 };

    res = init_manager(&mgr);
    if (res) {
        return res;
    }

    backlog = (arg_backlog > 0) ? arg_backlog : DEFAULT_SOCKET_BACKLOG;
    res = listen(mgr.sockfd, backlog);
    if (res == -1) {
        perror("listen()");
        free_manager(&mgr);
        return -1;
    }

    LOG("%s: listening for connections on socket '%s' with backlog '%d'\n", INFO, mgr.sock_path, backlog);

    while ((conn = accept(mgr.sockfd, NULL, NULL)) != -1) {
        LOG("%s: client connecected\n", INFO);
        rb = read(conn, &msg, sizeof(msg));
        if (rb != sizeof(msg)) {
            LOG("%s: malformed message recieved, closing connection\n", ERR);
            close(conn);
            memset(&msg, 0, sizeof(tmg_client_message_t));
            continue;
        }

        DEBUGMSG(msg);

        if (msg.op < N_OPS && msg.op >= 0) {
            handlers[msg.op](&mgr, &msg, conn);
            if (msg.op == OP_QUIT) {
                close(conn);
                LOG("%s: client disconnected\n", INFO);
                break;
            }
        }

        memset(&msg, 0, sizeof(tmg_client_message_t));
        close(conn);
        LOG("%s: client disconnected\n", INFO);
    }

    free_manager(&mgr);
    LOG("%s: daemon exitting\n", TRACE);

    return 0;
}

int main(int argc, char *argv[])
{
    int o, res;

    while ((o = getopt(argc, argv, "hvDm:R:s:H:S:ld:c:b:q")) != -1) {
        switch (o) {
        case 'D':
            arg_daemon = true;
            break;
        case 'l':
            arg_list = true;
            break;
        case 'm':
            arg_mins = atoi(optarg);
            break;
        case 'H':
            arg_hours = atoi(optarg);
            break;
        case 's':
            arg_secs = atoi(optarg);
            break;
        case 'c':
            arg_change = atoi(optarg);
            break;
        case 'd':
            arg_delete = atoi(optarg);
            break;
        case 'b':
            arg_backlog = atoi(optarg);
            break;
        case 'R':
            arg_run = strdup(optarg);
            break;
        case 'S':
            arg_sock_path = strdup(optarg);
            break;
        case 'q':
            arg_quit = true;
            break;
        case 'v':
            version();
            res = 0;
            goto cleanup;
        default:
            help();
            res = 1;
            goto cleanup;
        }
    }

    if (arg_daemon) {
        res = daemon_main();
    } else {
        res = client_main();
    }

cleanup:
    free(arg_sock_path);
    free(arg_run);

    return res;
}
