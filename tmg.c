#define _POSIX_C_SOURCE 200809L

#include <getopt.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_SOCKET_PATH "tmg.socket"
#define DEFAULT_SOCKET_BACKLOG 32
#define DEFAULT_PROGRAM "notify-send"
#define DEFAULT_PQ_CAPACITY 32

#define MAXSIZE 1024

#define OP_CREATE 0
#define OP_CHANGE 1
#define OP_DELETE 2
#define OP_LIST 3
#define OP_QUIT 4

#define N_OPS 5

#ifdef DEBUG
#define DEBUGMSG(m) printf("{\n\tsecs: %d\n"     \
                           "\tmins: %d\n"        \
                           "\thours: %d\n"       \
                           "\tid: %d\n"          \
                           "\top: %d\n"          \
                           "\tmsg: %s\n"         \
                           "\tmsg_len: %d\n}\n", \
        m.secs,                                  \
        m.minutes,                               \
        m.hours,                                 \
        m.id,                                    \
        m.op,                                    \
        m.arg)

#define DEBUGARGS()                                                                                                                                           \
    printf("daemon = %d; list = %d\n", arg_daemon, arg_list);                                                                                                 \
    printf("secs = %d, mins = %d, hours = %d, change = %d, delete = %d, backlog = %d\n", arg_secs, arg_mins, arg_hours, arg_change, arg_delete, arg_backlog); \
    printf("socket_path = '%s', prog = '%s', message = '%s'\n", arg_sock_path, arg_prog, arg_msg);

#define DEBUGTIMER(t) printf("{\n\tid: %d\n"  \
                             "\tbegin: %ld\n" \
                             "\tend: %ld\n"   \
                             "\tmsg: %s\n"    \
                             "}\n",           \
        t.id,                                 \
        t.begin,                              \
        t.end,                                \
        t.arg);

#define DEBUGPRINT(args...) dprintf(2, args);
#else
#define DEBUGMSG(m) (void) 0;
#define DEBUGARGS() (void) 0;
#define DEBUGTIMER(t) (void) 0;
#define DEBUGPRINT(args...) (void) 0;
#endif

// command line options
bool arg_daemon, arg_list, arg_quit = false;
int arg_change, arg_delete, arg_secs, arg_hours, arg_mins, arg_backlog = 0;
char *arg_msg = NULL;
char *arg_prog = NULL;
char *arg_sock_path = NULL;

void help()
{
    printf("usage: tmg [-D] [-H HOURS] [-m MINUTES] [-s SECONDS] [-M message] [-S SOCKET_PATH] [-p PROGRAM]\n");
    printf("           [-l] [-d ID] [-c ID] [-b BACKLOG]\n\n");
    printf("timer manager: create & manage timers\n\n");
    printf("options:\n");
    printf("  -h\t\tdisplay this help message\n");
    printf("  -l\t\tlist active timers\n");
    printf("  -H <num>\tset number of hours when creating/changing a timer\n");
    printf("  -m <num>\tset number of minutes when creating/changing a timer\n");
    printf("  -s <num>\tset number of seconds when creating/changing a timer\n");
    printf("  -M <str>\tset timer argument\n");
    printf("  -S <str>\tpath to daemon socket\n");
    printf("  -p <str>\tprogram to use when running timer actions\n");
    printf("  -d <num>\tdelete timer by id\n");
    printf("  -c <num>\tchange timer by id\n");
    printf("  -b <num>\tbacklog of socket connections\n");
}

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

typedef struct
{
    // current id
    int current;
    // socket file descriptor
    int sockfd;
    // socket path
    char *sock_path;
    // Program to be run after the timer finishes
    char *program;
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

typedef struct
{
    // timer id (where sensible), operation
    int id, op;
    // changed/created times
    int secs, minutes, hours;
    // changed/created argument
    char arg[MAXSIZE];
} tmg_client_message_t;

typedef struct
{
    char body[MAXSIZE * 2];
} tmg_reply_t;

int make_timer_thread(tmg_manager_t *mgr);

int timer_cmp(const void *a, const void *b)
{
    tmg_timer_t *ta = (tmg_timer_t *) a;
    tmg_timer_t *tb = (tmg_timer_t *) b;

    return -(ta->end - tb->end);
}

int enq(tmg_manager_t *mgr, tmg_timer_t *timer)
{
    pthread_mutex_lock(&mgr->mutex);
    int last_id, new_last_id;
    tmg_timer_t *new;
    last_id = (mgr->q.len != 0) ? mgr->q.timers[mgr->q.len - 1].id : -1;
    if (mgr->q.cap == mgr->q.len) {
        new = realloc(mgr->q.timers, sizeof(tmg_timer_t) * mgr->q.cap * 2);
        if (new == NULL) {
            perror("tmg: enqueue timer");
            return -1;
        }
        mgr->q.timers = new;
        mgr->q.cap *= 2;
    }

    memcpy(&mgr->q.timers[mgr->q.len++], timer, sizeof(tmg_timer_t));
    qsort(mgr->q.timers, mgr->q.len, sizeof(tmg_timer_t), timer_cmp);
    new_last_id = mgr->q.timers[mgr->q.len - 1].id;
    if (new_last_id != last_id && last_id != -1) {
        pthread_cancel(mgr->timer_thread);
        pthread_mutex_unlock(&mgr->mutex);
        make_timer_thread(mgr);
    } else if (new_last_id != last_id) {
        pthread_mutex_unlock(&mgr->mutex);
        make_timer_thread(mgr);
    } else {
        pthread_mutex_unlock(&mgr->mutex);
    }

    return 0;
}

int deq(tmg_manager_t *mgr)
{
    pthread_mutex_lock(&mgr->mutex);
    if (mgr->q.len <= 0) {
        return 0;
    }

    --(mgr->q.len);
    pthread_mutex_unlock(&mgr->mutex);
    return 0;
}

/// This function runs in the timer thread.
void *run_timer_thread(void *args)
{
    tmg_manager_t *mgr = (tmg_manager_t *) args;
    tmg_timer_t timer = { 0 };
    int res;
    size_t msg_len;
    size_t prog_len;
    time_t secs, curr_time;
    char *cmdbuf;
    char *prog;

    // Disable cancellation, take the manager mutex and copy necessary stuff
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
    pthread_mutex_lock(&mgr->mutex);
    curr_time = time(NULL);
    prog = strdup(mgr->program);
    memcpy(&timer, &mgr->q.timers[mgr->q.len - 1], sizeof(tmg_timer_t));
    secs = timer.end - curr_time;
    DEBUGPRINT("new timer thread for timer: ");
    DEBUGTIMER(timer);
    // register handler
    pthread_cleanup_push((void (*)(void *)) free, prog);
    pthread_mutex_unlock(&mgr->mutex);
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

    if (secs < 0) {
        dprintf(2, "timer is past it's due, cancelling\n");
        pthread_exit(NULL);
    }

    sleep(secs);

    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
    msg_len = strlen(timer.arg);
    prog_len = strlen(prog);
    // register another handler
    cmdbuf = malloc(msg_len + prog_len + 2);
    pthread_cleanup_push((void (*)(void *)) free, cmdbuf);
    sprintf(cmdbuf, "%s %s", prog, timer.arg);

    res = system(cmdbuf);
    if (res != 0) {
        perror("tmg");
    }
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);

    // Cleanup
    pthread_cleanup_pop(true);
    pthread_cleanup_pop(true);

    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
    res = pthread_mutex_trylock(&mgr->mutex);
    if (res) {
        pthread_exit(NULL);
    }
    deq(mgr);
    if (mgr->q.len <= 0) {
        pthread_mutex_unlock(&mgr->mutex);
        pthread_exit(NULL);
    }
    make_timer_thread(mgr);
    pthread_mutex_unlock(&mgr->mutex);
    pthread_exit(NULL);
}

int make_timer_thread(tmg_manager_t *mgr)
{
    pthread_mutex_lock(&mgr->mutex);
    pthread_t thread;
    int res;

    res = pthread_create(&thread, NULL, run_timer_thread, mgr);
    if (res == -1) {
        perror("tmg");
        pthread_mutex_unlock(&mgr->mutex);
        return -1;
    }
    pthread_detach(thread);
    mgr->timer_thread = thread;
    pthread_mutex_unlock(&mgr->mutex);

    return 0;
}

int init_manager(tmg_manager_t *mgr)
{
    int res, len;
    struct sockaddr_un addr;
    pthread_mutexattr_t attrs;

    mgr->current = 0;
    mgr->program = arg_prog != NULL ? strdup(arg_prog) : strdup(DEFAULT_PROGRAM);
    mgr->sock_path = arg_sock_path != NULL ? strdup(arg_sock_path) : strdup(DEFAULT_SOCKET_PATH);
    mgr->sockfd = socket(AF_UNIX, SOCK_STREAM, 0);

    mgr->q.len = 0;
    mgr->q.cap = DEFAULT_PQ_CAPACITY;
    mgr->q.timers = calloc(sizeof(tmg_timer_t), mgr->q.cap);

    pthread_mutexattr_init(&attrs);
    pthread_mutexattr_settype(&attrs, PTHREAD_MUTEX_RECURSIVE);
    res = pthread_mutex_init(&mgr->mutex, &attrs);
    if (mgr->program == NULL || mgr->sock_path == NULL || mgr->sockfd == -1 || mgr->q.timers == NULL) {
        perror("tmg");
        goto init_manager_err;
    }

    memset(&addr, 0, sizeof(struct sockaddr_un));
    addr.sun_family = AF_UNIX;
    len = strlen(mgr->sock_path);
    strncpy(addr.sun_path, mgr->sock_path, len);

    (void) unlink(mgr->sock_path);
    res = bind(mgr->sockfd, (struct sockaddr *) &addr, sizeof(struct sockaddr_un));
    if (res == -1) {
        perror("tgm");
        goto init_manager_err;
    }

    return 0;

init_manager_err:
    free(mgr->program);
    free(mgr->sock_path);
    free(mgr->q.timers);
    close(mgr->sockfd);
    pthread_mutex_destroy(&mgr->mutex);
    return -1;
}

void free_manager(tmg_manager_t *mgr)
{
    close(mgr->sockfd);
    free(mgr->sock_path);
    free(mgr->program);
    free(mgr->q.timers);
    pthread_mutex_destroy(&mgr->mutex);
}

int create_timer(tmg_manager_t *mgr, const tmg_client_message_t *msg, int conn)
{
    tmg_timer_t timer = { 0 };
    int res;
    timer.id = ++(mgr->current);
    timer.begin = time(NULL);
    timer.end = timer.begin + msg->secs + msg->minutes * 60 + msg->hours * 60 * 60;
    memcpy(timer.arg, msg->arg, MAXSIZE);

    res = enq(mgr, &timer);
    if (res) {
        dprintf(conn, "timer creation failed.\n");
        return -1;
    }
    dprintf(conn, "timer created successfully.\n");
    return 0;
}

int change_timer(tmg_manager_t *mgr, const tmg_client_message_t *msg, int conn)
{
    tmg_timer_t *timer;
    bool restart_thread;
    int res, last_id;

    pthread_mutex_lock(&mgr->mutex);
    if (mgr->q.len <= 0) {
        dprintf(conn, "no timers found.\n");
        return 0;
    }
    last_id = mgr->q.timers[mgr->q.len - 1].id;
    for (int i = 0; i < mgr->q.len; i++) {
        if (mgr->q.timers[i].id != msg->id) {
            continue;
        }

        pthread_cancel(mgr->timer_thread);
        timer = &mgr->q.timers[i];
        timer->end = timer->begin + msg->secs + msg->minutes * 60 + msg->hours * 60 * 60;
        strncpy(timer->arg, msg->arg, MAXSIZE - 1);
        qsort(mgr->q.timers, mgr->q.len, sizeof(tmg_timer_t), timer_cmp);
        if ((restart_thread = last_id != mgr->q.timers[mgr->q.len - 1].id || last_id == msg->id)) {
            pthread_cancel(mgr->timer_thread);
        }
        break;
    }
    pthread_mutex_unlock(&mgr->mutex);
    if (restart_thread)
        make_timer_thread(mgr);

    return 0;
}

int delete_timer(tmg_manager_t *mgr, const tmg_client_message_t *msg, int conn)
{
    tmg_timer_t a, tmp = { 0 };
    bool restart_thread = false;

    pthread_mutex_lock(&mgr->mutex);
    if (mgr->q.len <= 0) {
        return 0;
    }
    if ((restart_thread = msg->id == mgr->q.timers[mgr->q.len - 1].id)) {
        deq(mgr);
        pthread_cancel(mgr->timer_thread);
    }
    for (int i = 0; i < mgr->q.len; i++) {
        if (mgr->q.timers[i].id == msg->id) {
            memcpy(&a, &mgr->q.timers[i], sizeof(tmg_timer_t));
            memcpy(&tmp, &mgr->q.timers[mgr->q.len - 1], sizeof(tmg_timer_t));
            memcpy(&mgr->q.timers[mgr->q.len - 1], &a, sizeof(tmg_timer_t));
            deq(mgr);
            break;
        }
    }
    qsort(mgr->q.timers, mgr->q.len, sizeof(tmg_timer_t), timer_cmp);
    pthread_mutex_unlock(&mgr->mutex);

    if (restart_thread)
        make_timer_thread(mgr);

    return 0;
}
int list_timers(tmg_manager_t *mgr, const tmg_client_message_t *msg, int conn)
{
    tmg_timer_t timer;
    struct tm *lt;
    char begin_buf[64];
    char end_buf[64];
    tmg_reply_t reply = { 0 };

    pthread_mutex_lock(&mgr->mutex);
    for (int i = 0; i < mgr->q.len; i++) {
        timer = mgr->q.timers[(mgr->q.len - 1) - i];
        lt = localtime(&timer.begin);
        strftime(begin_buf, sizeof(begin_buf), "%a %b %e %H:%M:%S %Y", lt);
        lt = localtime(&timer.end);
        strftime(end_buf, sizeof(end_buf), "%a %b %e %H:%M:%S %Y", lt);
        sprintf(reply.body, "[id: %d; begin: %s; end: %s; argument: {%s}]\n", timer.id, begin_buf, end_buf, timer.arg);
        write(conn, &reply, sizeof(reply));
    }
    pthread_mutex_unlock(&mgr->mutex);
    return 0;
}

int quit_handler(tmg_manager_t *mgr, const tmg_client_message_t *msg, int conn)
{
    pthread_mutex_lock(&mgr->mutex);
    pthread_cancel(mgr->timer_thread);
    tmg_reply_t reply = { 0 };

    sprintf(reply.body, "quit request recieved\n");
    write(conn, &reply, sizeof(reply));
    pthread_mutex_unlock(&mgr->mutex);
    return 0;
}

static int (*handlers[N_OPS])(tmg_manager_t *, const tmg_client_message_t *, int) = {
    create_timer,
    change_timer,
    delete_timer,
    list_timers,
    quit_handler,
};

int client_main()
{
    int res, sock;
    size_t len;
    ssize_t wb, rb;
    tmg_client_message_t msg = { 0 };
    tmg_reply_t reply = { 0 };
    struct sockaddr_un addr;
    char *sock_path;

    sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock == -1) {
        perror("tgm");
        return -1;
    }

    sock_path = arg_sock_path != NULL ? strdup(arg_sock_path) : strdup(DEFAULT_SOCKET_PATH);
    memset(&addr, 0, sizeof(struct sockaddr_un));
    addr.sun_family = AF_UNIX;
    len = strlen(sock_path);
    strncpy(addr.sun_path, sock_path, len);

    res = connect(sock, (struct sockaddr *) &addr, arg_backlog ? arg_backlog : DEFAULT_SOCKET_BACKLOG);
    if (res == -1) {
        perror("tmg");
        free(sock_path);
        return -1;
    }

    msg.hours = arg_hours;
    msg.minutes = arg_mins;
    msg.secs = arg_secs;
    // So that we do not overflow the buffer
    strncpy(msg.arg, arg_msg != NULL ? arg_msg : "", MAXSIZE - 1);

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
        perror("tmg");
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

int daemon_main()
{
    int res;
    int conn;
    ssize_t rb;
    // Buffer for client messages
    // Replies are handled in
    tmg_client_message_t msg = { 0 };
    tmg_manager_t mgr = { 0 };

    res = init_manager(&mgr);
    if (res) {
        return res;
    }

    res = listen(mgr.sockfd, arg_backlog ? arg_backlog : DEFAULT_SOCKET_BACKLOG);
    if (res == -1) {
        perror("tgm");
        free_manager(&mgr);
        return -1;
    }

    dprintf(2, "listening for connections\n");

    while ((conn = accept(mgr.sockfd, NULL, NULL)) != -1) {
        dprintf(2, "conn: established\n");
        rb = read(conn, &msg, sizeof(msg));
        if (rb != sizeof(msg)) {
            dprintf(2, "malformed message recieved, closing connection\n");
            close(conn);
            memset(&msg, 0, sizeof(tmg_client_message_t));
            continue;
        }

        DEBUGMSG(msg);

        if (msg.op < N_OPS && msg.op >= 0) {
            handlers[msg.op](&mgr, &msg, conn);
            if (msg.op == OP_QUIT) {
                break;
            }
        }

        memset(&msg, 0, sizeof(tmg_client_message_t));
        close(conn);
        dprintf(2, "conn: closed\n");
    }

    free_manager(&mgr);

    return 0;
}

int main(int argc, char *argv[])
{
    int o, res;

    while ((o = getopt(argc, argv, "hDm:M:s:H:S:p:ld:c:b:q")) != -1) {
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
        case 'M':
            arg_msg = strdup(optarg);
            break;
        case 'S':
            arg_sock_path = strdup(optarg);
            break;
        case 'p':
            arg_prog = strdup(optarg);
            break;
        case 'q':
            arg_quit = true;
            break;
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
    free(arg_prog);
    free(arg_sock_path);
    free(arg_msg);

    return res;
}
