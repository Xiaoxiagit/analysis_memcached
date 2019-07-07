
#include "memcached.h"

/* exported globals */
struct settings settings;
conn **conns;

/* file scope variables */
static conn *listen_conn = NULL;
static int max_fds;
static struct event_base *main_base;

static void settings_init(void) {
    settings.use_cas = true;
    settings.access = 0700;
    settings.port = 11211;
    settings.udpport = 0;
    /* By default this string should be NULL for getaddrinfo() */
    settings.inter = NULL;
    settings.maxbytes = 64 * 1024 * 1024;
    settings.maxconns = 1024; // to limit connections-related memory to about 5MB
    settings.verbose = 0;
    settings.oldest_live = 0;
    settings.oldest_cas = 0;    // supplements accuracy of oldest_live
    settings.evict_to_free = 1; // push old items out of cache when memory runs out
    settings.socketpath = NULL; // by default, not using a unix socket
    settings.factor = 1.25;
    settings.chunk_size = 48;   // space for a modest key and value
    settings.num_threads = 4;   // N workers
    settings.num_threads_per_udp = 0;
    settings.prefix_delimiter = ':';
    settings.detail_enabled = 0;
    settings.reqs_per_event = 20;
    settings.backlog = 1024;
    settings.binding_protocol = negotiating_prot;
    settings.item_size_max = 1024 * 1024;   // The famous 1MB upper limit
    settings.slab_page_size = 1024 * 1024;  // chunks are split from 1MB pages
    settings.slab_chunk_size_max = settings.slab_page_size / 2;
    settings.sasl = false;
    settings.maxconns_fast = true;
    settings.lru_crawler = false;
    settings.lru_carwler_sleep = 100;
    settings.lru_crawler_tocrawl = 0;
    settings.lru_maintainer_thread = false;
    settings.lru_segmented = true;
    settings.hot_lru_pct = 20;
    settings.warm_lru_pct = 40;
    settings.hot_max_factor = 0.2;
    settings.warm_max_factor = 2.0;
    settings.inline_ascii_response = false;
    settings.temp_lru = false;
    settings.temporary_ttl = 61;
    settings.idle_timeout = 0;  // disabled
    settings.hashpower_init = 0;
    settings.slab_reassign = true;
    settings.slab_automove = 1;
    settings.slab_automove_ratio = 0.8;
    settings.slab_automove_window = 30;
    settings.shutdown_command = false;
    settings.tail_repair_time = TAIL_REPAIR_TIME_DEFAULT;
    settings.flush_enabled = true;
    settings.dump_enabled = true;
    settings.crawls_persleep = 1000;
    settings.logger_watcher_buf_size = LOGGER_WATCHER_BUF_SIZE;
    settings.logger_buf_size = LOGGER_BUF_SIZE;
    settings.drop_privileges = true;
#ifdef MEMCACHED_DEBUG
    settings.relaxed_privileges = false;
#endif
}

// Connection timeout thread bits
static pthread_t conn_timeout_tid;

// 处理链接空闲的connection
#define CONNS_PER_SLICE 100
#define TIMEOUT_MSG_SIZE (1 + sizeof(int))
static void *conn_timeout_thread(void *arg) {
    int i;
    conn *c;
    char buf[TIMEOUT_MSG_SIZE];
    rel_time_t oldest_last_cmd;
    int sleep_time;
    useconds_t timeslice = 1000000 / (max_fds / CONNS_PER_SLICE);

    while(1) {
        if (settings.verbose > 2)
            fprintf(stderr, "idle timeout thread at top of connection list\n");

        oldest_last_cmd = current_time;

        // 依次循环处理每一个connection
        for (i = 0; i < max_fds; i++) {
            // 每处理一定量的connection后休息一段时间
            if ((i % CONNS_PER_SLICE) == 0) {
                if (settings.verbose > 2)
                    fprintf(stderr, "idle timeout thread sleeping for %ulus\n",
                            (unsigned int)timeslice);
                usleep(timeslice);
            }
            
            // 判断此connection是否被使用
            if (!conns[i])
                continue;

            c = conns[i];   // 获取connection
            
            // 判断是否是TCP网络协议
            if (!IS_TCP(c->transport))
                continue;
            // 判断connection的状态
            if (c->state != conn_new_cmd && c->state != conn_read)
                continue;
            // 判断是否超时
            if ((current_time - c->last_cmd_time) > settings.idle_timeout) {
                buf[0] = 't';
                memcpy(&buf[1], &i, sizeof(int));
                // 向该线程发送connection超时的信息
                if (write(c->thread->notify_send_fd, buf, TIMEOUT_MSG_SIZE) != 
                        TIMEOUT_MSG_SIZE)
                    perror("Failed to write timeout to notify pipe");
            } else {
                if (c->last_cmd_time < oldest_last_cmd)
                    oldest_last_cmd = c->last_cmd_time;
            }
        }

        // This is soonest we could have another connection time out
        // 判断最近的过期时间
        sleep_time = settings.idle_timeout - (current_time - oldest_last_cmd) + 1;
        if (sleep_time <= 0)
            sleep_time = 1;

        if (settings.verbose > 2)
            fprintf(stderr, 
                    "idle timeout thread finished pass, sleeping for %ds\n",
                    sleep_time);
        usleep((useconds_t) sleep_time * 1000000);
    }

    return NULL;
}

conn *conn_new(const int sfd, enum conn_states init_state,
                const int event_flags,
                const int read_buffer_size, enum network_transport transport,
                struct event_base *base) {
    conn *c;

    assert(sfd >= 0 && sfd < max_fds);
    c = conns[sfd];
    
    // 判断此connection结构体是否存在
    if (NULL == c) {
        // 分配一个新的connection结构体，并初始化
        if (!(c = (conn *)calloc(1, sizeof(conn)))) {
            STATS_LOCK();
            stats.malloc_fails++;
            STATS_UNLOCK();
            fprintf(stderr, "Failed to allocate connection object\n");
            return NULL;
        }
        MEMCACHED_CONN_CREATE(c);

        c->rbuf = c->wbuf = 0;
        c->ilist = 0;
        c->suffixlist = 0;
        c->iov = 0;
        c->msglist = 0;
        c->hdrbuf = 0;

        c->rsize = read_buffer_size;
        c->wsize = DATA_BUFFER_SIZE;
        c->isize = ITEM_LIST_INITIAL;
        c->suffixsize = SUFFIX_LIST_INITIAL;
        c->iovsize = IOV_LIST_INITIAL;
        c->msgsize = MSG_LIST_INITIAL;
        c->hdrsize = 0;

        c->rbuf = (char *)malloc((size_t)c->rsize);
        c->wbuf = (char *)malloc((size_t)c->wsize);
        c->ilist = (item **)malloc(sizeof(item *) * c->isize);
        c->suffixlist = (char **)malloc(sizeof(char *) * c->suffixsize);
        c->iov = (struct iovec *)malloc(sizeof(struct iovec) * c->iovsize);
        c->msglist = (struct msghdr *)malloc(sizeof(struct msghdr) * c->msgsize);

        if (c->rbuf == 0 || c->wbuf == 0 || c->ilist == 0 || c->iov == 0 ||
                c->msglist == 0 || c->suffixlist == 0) {
            conn_free(c);
            STATS_LOCK();
            stats.malloc_fails++;
            STATS_UNLOCK();
            fprintf(stderr, "Failed to allocate buffers for connection\n");
            return NULL;
        }

        STATS_LOCK();
        stats_state.conn_structs++;
        STATS_UNLOCK();

        c->sfd = sfd;
        conns[sfd] = c;     // 加入conneciton数组中
    }

    // 设定网络协议
    c->transport = transport;
    c->protocol = settings.binding_protocol;

    /* unix socket mode doesn't need this, so zeroed out. but why
     * is this done for every command? presumably for UDP
     * mode
     */
    if (!settings.socketpath) {
        c->request_addr_size = sizeof(c->request_addr);
    } else {
        c->request_addr_size = 0;
    }

    if (transport == tcp_transport && init_state == conn_new_cmd) {
        if (getpeername(sfd, (struct sockaddr *) &c->request_addr,
                            &c->request_addr_size)) {
            perror("getpeername");
            memset(&c->request_addr, 0, sizeof(c->request_addr));
        }
    }

    if (settings.verbose > 1) {
        if (init_state == conn_listening) {
            fprintf(stderr, "<%d server listening (%s)\n", sfd,
                prot_text(c->protocol));
        } else if (IS_UDP(transport)) {
            fprintf(stderr, "<%d server listening (udp)\n", sfd);
        } else if (c->protocol == negotiating_prot) {
            fprintf(stderr, "<%d new auto-negotiating client connection\n",
                    sfd);
        } else if (c->protocol == ascii_prot) {
            fprintf(stderr, "<%d new ascii client connection.\n", sfd);
        } else if (c->protocol == binary_prot) {
            fprintf(stderr, "<%d new binary client connection.\n", sfd);
        } else {
            fprintf(stderr, "<%d new unknown (%d) client connection\n",
                    sfd, c->protocol);
            assert(false);
        }
    }
    
    // 初始化新的connection结构体
    c->state = init_state;
    c->rlbytes = 0;
    c->cmd = -1;
    c->rbytes = c->wbytes = 0;
    c->wcurr = c->wbuf;
    c->rcurr = c->rbuf;
    c->ritem = 0;
    c->icurr = c->ilist;
    c->suffixcurr = c->suffixlist;
    c->ileft = 0;
    c->suffixleft = 0;
    c->iovused = 0;
    c->msgcurr = 0;
    c->msgused = 0;
    c->authenticated = false;
    c->last_cmd_time = current_time;    // initialize for idle kicker
#ifdef EXTSTORE
    c->io_wraplist = NULL;
    c->io_wraplist = 0;
#endif

    c->write_and_go = init_state;
    c->write_and_free = 0;
    c->item = 0;

    c->noreply = false;
    
    // 设置connection的处理函数
    event_set(&c->event, sfd, event_flags, event_handler, (void *)c);
    event_base_set(base, &c->event);
    c->ev_falgs = event_flags;

    if (event_add(&c->event, 0) == -1) {
        perror("event_add");
        return NULL;
    }

    STATS_LOCK();
    stats_state.curr_conns++;
    stats.total_conns++;
    STATS_UNLOCK();

    MEMCACHED_CONN_ALLOCATE(c->sfd);

    return c;
}

static int start_conn_timeout_thread() {
    int ret;

    if (settings.idle_timeout == 0)
        return -1;

    if ((ret = pthread_create(&conn_timeout_tid, NULL, 
                    conn_timeout_thread, NULL)) != 0) {
        fprintf(stderr, "Can't create idle connection timeout thread: %s\n",
            strerror(ret));
        return -1;
    }

    return 0;
}

/**
 * Initialize the connections array. We don't actually allocate connection
 * structures until they're needed, so as to avoid wasting memory when the 
 * maximum connection count is much higher than the actual number of
 * connections.
 *
 * This does end up wasting a few pointers' worth of memory for FDs that are
 * used for things other than connections, but that's worth it in exchange for
 * being able to directly index the conns array by FD.
 */
static void conn_init(void) {
    /* We're ulinkely to see an FD much higher than maxconns. */
    int next_fd = dup(1);   // 获取第一个未被使用的文件描述符
    int headroom = 10;   /* account for extra unexpected open FDs */
    struct rlimit rl;       // 资源限制

    max_fds = settings.maxconns + headroom + next_fd; // 获取需要connection结构个数

    /* But if possible, get the actual highest FD we can possibly ever see. */
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
        max_fds = rl.rlim_max;
    } else {
        fprintf(stderr, "Failed to query maximum file descriptor; "
                        "falling back to maxconns\n");
    }

    // 关闭文件描述符
    close(next_fd);

    // 申请connect数组空间
    if ((conns = calloc(max_fds, sizeof(conn *))) == NULL) {
        fprintf(stderr, "Failed to allocate connection structures\n");
        // This is unrecoverable so bail out early.
        exit(1);
    }
}

/**
 * We keep the current time of day in a global variable that's updated by a
 * timer event. This saves us a bunch of time() system calls (we really only
 * need to get the time once a second, whereas there can be tens of thousands
 * of requests a second) and allows us to use server-start-relative timestamps
 * rather than absolute UNIX timestamps, a space savings on systems where
 * sizeof(time_t) > sizeof(unsigned int).
 */
volatile rel_time_t current_time;
static struct event clockevent;

/* libevent uses a montonic clock when available for event scheduling. Aside
 * from jitter, simply ticking our internal timer here is accurate enough.
 * Note that users who are setting explicit dates for expiration times *must*
 * ensure their clocks are corrct before starting memcached. */
// current_time的时间处理函数
static void clock_handler(const int fd, const short which, void *arg) {
    struct timeval t = {.tv_sec = 1, .tv_usec = 0};
    static bool initialized = false;
#if defined(HAVE_CLOCK_GETTIME) && defined(CLOCK_MONOTONIC)
    static bool monotonic = false;
    static time_t monotonic_start;
#endif

    if (initialized) {
        // only delete the event if it's actually there
        evtimer_del(&clockevent);
    } else {
        initialized = true;
        /* process_started is initialized to time() - 2. We initialize to 1 so
         * flush_all won't underflow during tests. */
#if defined(HAVE_CLOCK_GETTIME) && defined(CLOCK_MONOTONIC)
        struct timespec ts;
        if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
            monotonic = true;
            monotonic_start = ts.tv_sec - ITEM_UPDATE_INTERVAL - 2;
        }
#endif
    }

    // While we're here, check for hash table expansion.
    // This function should be quick to avoid delaying the timer.
    // 判断是否需要扩展hash表
    assoc_start_expand(stats_state.curr_items);
    
    // 继续添加定时器
    evtimer_set(&clockevent, clock_handler, 0);
    event_base_set(main_base, &clockevent);
    evtimer_add(&clockevent, &t);

    // 设置current_time的值
#if defined(HAVE_CLOCK_GETTIME) && defined(CLOCK_MONOTONIC)
    if (monotonic) {
        struct timespec ts;
        if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1)
            return;
        current_time = (rel_time_t) (ts.tv_sec - monotonic_start);
        return;
    }
#endif
    {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        current_time = (rel_time_t) (tv.tv_sec - process_started);
    }
}

static int server_sockets(int port, enum network_transport transport,
                            FILE *portnumber_file) {
    if (settings.inter == NULL) {
        return server_socket(settings.inter, port, transport, portnumber_file);
    }
}

static int new_socket_unix(void) {
    int sfd;
    int flags;

    if ((sfd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
        perror("socket()");
        return -1;
    }

    if ((flags = fcntl(sfd, F_GETFL, 0)) < 0 ||
            fcntl(sfd, F_SETFL, flags | O_NOBLOCK) < 0) {
        perror("setting O_NONBLOCK");
        close(sfd);
        return -1;
    }
    return sfd;
}

// 创建Unix文件描述符
static int server_socket_unix(const char *path, int access_mask) {
    int sfd;
    struct linger ling = {0, 0};
    struct sockaddr_un addr;
    struct stat tstat;
    int flags = 1;
    int old_umask;

    if (!path) {
        return 1;
    }

    if ((sfd = new_socket_unix()) == -1) {
        return 1;
    }

    /**
     * Clean up a previous socket file if we left it around
     */
    if (lstat(path, &tstat) == 0) {
        if (S_ISSOCK(tstat.st_mode))
            unlink(path);
    }

    // 设置文件描述符的属性
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, (void *)&flags, sizeof(flags));
    setsockopt(sfd, SOL_SOCKET, SO_KEEPALIVE, (void *)&flags, sizeof(flags));
    setsockopt(sfd, SOL_SOCKET, SO_LINGER, (void *)&ling, sizeof(ling));

    /**
     * the memset call clears nonstandard fields in some implementations
     * that otherwise mess things up.
     */
    memset(&addr, 0, sizeof(addr));
    // 设置绑定协议
    addr.sum_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    assert(strcmp(addr.sun_path, path) == 0);
    old_umask = umask( ~(access_mask&0777));
    if (bind(sfd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("bind()");
        close(sfd);
        umask(old_umask);
        return 1;
    }
    umask(old_umask);
    if (listen(sfd, settings.backlog) == -1) {
        perror("listen()");
        close(sfd);
        return 1;
    }
    if (!(listen_conn = conn_new(sfd, conn_listening,
                                    EV_READ | EV_PERSIST, 1,
                                    local_transport, main_base))) {
        fprintf(stderr, "failed to create listening connection\n");
        exit(EXIT_FAILURE);
    }

    return 0;
}

int main (int argc, char **argv) {
    
    // 与网络无关部分
    // init settings
    settings_init();
#ifdef EXTSTORE
    settings.ext_item_size = 512;
    setitngs.ext_item_age = UINT_MAX;
    settings.ext_low_ttl = 0;
    settings.ext_recache_rate = 2000;
    settings.ext_max_frag = 0.8;
    settings.ext_drop_unread = false;
    settings.ext_wbuf_size = 1024 * 1024 * 4;
    settings.ext_compact_under = 0;
    settings.ext_drop_under = 0;
    settings.slab_automove_freeratio = 0.01;
    ext_cf.page_size = 1024 * 1024 * 64;
    ext_cf.page_count = 64;
    ext_cf.wbuf_size = settings.ext_wbuf_size;
    ext_cf.io_threadcount = 1;
    ext_cf.io_depth = 1;
    ext_cf.page_buckets = 4;
    ext_cf.wbuf_count = ext_cf.page_buckets;
#endif
    
    // initialize main thread libevent instance
#if defined(LIBEVENT_VERSION_NUMBER) && LIBEVENT_VERSION_NUMBER >= 0x02000101
    /* If libevent version is larger/equal to 2.0.2-alpha, use newer version */
    struct event_config *ev_config;
    ev_config = event_config_new();
    event_config_set_flag(ev_config, EVENT_BASE_FLAG_NOLOCK);
    main_base = event_base_new_with_config(ev_config);
    event_config_free(ev_config);
#else
    /* Otherwise, use older API */
    main_base = event_init();
#endif

    /* initialize other stuff */
    conn_init();

 #ifdef EXTSTORE
    // 其他处理
    memcached_thread_init(settings.num_threads, storage);
#else
    // 其他处理
    memcached_thread_init(settings.num_threads, NULL);
#endif

    if (settings.idle_timeout && start_conn_timeout_thread() == -1) {
        exit(EXIT_FAILURE);
    }

    /* initialise clock event */
    clock_handler(0, 0, 0);

    /* create unix mode sockets after dropping privileges */
    if (settings.socketpath != NULL) {
        errno = 0;
        if (server_socket_unix(settings.socketpath, settings.access)) {
            vperror("failed to listen on UNIX socket: %s", settings.socketpath);
            exit(EX_OSERR);
        }
    }

    /* create the listening socket, bind it, end init */
    if (settings.socketpath == NULL) {
        const char *portnumber_filename = getenv("MEMCACHED_PORT_FILENAME");
        char *temp_portnumber_filename = NULL;
        size_t len;
        FILE *portnumber_file = NULL;

        if (portnumber_filename != NULL) {
            len = strlen(portnumber_filename)+4+1;
            temp_portnumber_filename = malloc(len);
            snprintf(temp_portnumber_filename,
                     len,
                     "%s.lck", portnumber_filename);

            portnumber_file = fopen(temp_portnumber_filename, "a");
            if (portnumber_file == NULL) {
                fprintf(stderr, "Failed to open \"%s\": %s\n",
                        temp_portnumber_filename, strerror(errno));
            }
        }

        // 绑定监听tcp端口
        errno = 0;
        if (settings.port && server_sockets(settings.port, tcp_transport,
                                            portnumber_file)) {
            vperror("failed to listen on TCP port %d", settings.port);
            exit(EX_OSERR);
        }

        // 绑定监听udp接口
        /**
         * initialization order: first create the listening sockets
         * (may need root on low ports), then drop root if needed,
         * then daemonize if needed, then init libevent (in some cases
         * descriptors created by libevent wouldn't survive forking)
         */
        
        // create the UDP listening socket and bind it
        errno = 0;
        if (settings.udpport && server_sockets(settings.udpport, udp_transport,
                                                portnumber_file)) {
            vperror("failed to listen on UDP port %d", settings.udpport);
            exit(EX_OSERR);
        }

        if (portnumber_file) {
            fclose(portnumber_file);
            rename(temp_portnumber_filename, portnumber_filename);
        }
        if (temp_portnumber_filename)
            free(temp_portnumber_filename);
    }

    /* Give the sockets a moment to open. I know this is dumb, but the error
     * is only an advisory.
     */
    usleep(1000);
    if (stats_state.curr_conns + stats_state.reserved_fds >= settings.maxconns - 1) {
        fprintf(stderr, "Maxconns setting is too low, use -c to increase.\n");
        exit(EXIT_FAILURE);
    }

    if (pid_file != NULL) {
        save_pid(pid_file);
    }

    /* Drop privileges to no longer needed */
    if (settings.drop_privileges) {
        drop_privileges();
    }

    // 其他代码
    
    // 主循环
    // enter the event loop
    if (event_base_loop(main_base, 0) != 0) {
        retval = EXIT_FAILURE;
    }

    // 其他代码
    
    // remove the PID file if we're a daemon
    if (do_daemonize)
        remove_pidfile(pid_file);
    /* Clean up strdup() call for bind() address */
    if (settings.inter)
        free(settings.inter);
    if (l_socket)
        free(l_socket);
    if (u_socket)
        free(u_socket);

    /* cleanup base */
    event_base_free(main_base);
}
