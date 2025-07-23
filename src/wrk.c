// Copyright (C) 2012 - Will Glozer.  All rights reserved.

#include "wrk.h"
#include "unistd.h"
#include "script.h"
#include "main.h"
#include "hdr_histogram.h"
#include "stats.h"
#include "assert.h"

// Max recordable latency of 1 day
#define MAX_LATENCY 24L * 60 * 60 * 1000000
uint64_t raw_latency[MAXTHREADS][MAXL];

#define DEFAULT_RQ_SENT_BEFORE_CX_RESET 150

static struct config {
    uint64_t num_urls;
    uint64_t threads_count;
	thread *threads;
    uint64_t connections;
    int dist; //0: fixed; 1: exp; 2: normal; 3: zipf
    uint64_t duration;
    uint64_t timeout;
    uint64_t pipeline;
    double rate;
    uint64_t delay_ms;
    bool     print_separate_histograms;
    bool     print_sent_requests;
    bool     latency;
    bool     dynamic;
    bool     record_all_responses;
    bool     print_all_responses;
    bool     print_realtime_latency;
    char    *script;
    SSL_CTX *ctx; //https://www.openssl.org/docs/man3.0/man3/SSL_CTX_new.html
    /* added by TR */
    char    *rate_file;
	bool blocking;
	uint64_t rq_sent_before_cx_reset;
} cfg;

static struct {
    stats **requests;
    pthread_mutex_t mutex;
} statistics;

static struct {
	uint64_t stats[5];
	uint64_t responses;
	pthread_mutex_t mutex;
	FILE * file;
	char *file_path;
} requests_stats;

static struct sock sock = {
    .connect  = sock_connect,
    .close    = sock_close,
    .read     = sock_read,
    .write    = sock_write,
    .readable = sock_readable
};

static struct http_parser_settings parser_settings = {
    .on_message_complete = response_complete
};


/* added by TR */
static int *target_rate;
static uint64_t start_time;

static uint64_t* total_requests_sent_target; /* added by RS */

static volatile sig_atomic_t stop = 0;

static void handler(int sig) {
    stop = 1;
}

int min(int a, int b){
    return (a<b) ? a : b;
}

/* added by TR */
void parse_rate(int *rate)
{
    FILE * fp;

    if(cfg.rate_file == NULL){
        printf("ERROR: no rate file provided\n");
        exit(-1);
    }
    
    printf("loading rate from file %s\n", cfg.rate_file);
    
    if ((fp = fopen(cfg.rate_file, "r")) == NULL){
        perror("rate_file");
        exit(-1);
    }

    float next_timestamp;
    int next_rate, current_rate=1;
    int next_index=0;

    int i=0;

    float current_timestamp;
    fscanf(fp,"%f,%d\n",&current_timestamp, &current_rate);
    cfg.rate = current_rate;
    total_requests_sent_target[0] = current_rate;
    i = (int) current_timestamp; // should still be zero

    while( i < cfg.duration ){
        int r=fscanf(fp,"%f,%d\n",&next_timestamp, &next_rate);
        if(r == EOF){
            break;
        }
        // printf("%f: %d\n", next_timestamp, next_rate);

        next_index=(int) next_timestamp;

        // printf("next index: %d, current_rate: %d\n", next_index, current_rate);
        
        assert(next_index >= i);
        
        int j=i;
        for(; j<min(next_index, cfg.duration); j++){
            float rate_per_connection= (float) current_rate / (float) cfg.connections;
            rate[j]= (int) ((float) 1000000 / rate_per_connection);
            total_requests_sent_target[j+1] = total_requests_sent_target[j] + next_rate;
        }

        i=j;
        current_rate=next_rate;
    }

    rate[i++]=(int) ((float) 1000000 / ((float) current_rate / (float) cfg.connections));
    
    int nb_filed_values=i;
    for(; i < cfg.duration; i++){
        rate[i] = rate[i % nb_filed_values];
        total_requests_sent_target[i] = total_requests_sent_target[i-1] + (total_requests_sent_target[(i % nb_filed_values)+1] - total_requests_sent_target[(i % nb_filed_values)-0]);
    }

    /* display for debugging */
    // for(i=0; i<cfg.duration; i++){
    //     printf("timestamp: %d \t delay:%d\n", i, rate[i]);
    // }
    // for(i=0; i<cfg.duration; i++){
    //     printf("timestamp: %d \t total_requests_sent_target:%ld\n", i, total_requests_sent_target[i]);
    // }

}


static void usage() {
    printf("Usage: wrk <options> <url>                                       \n"
           "  Options:                                                       \n"
           "    -c, --connections <N>  Connections to keep open              \n"
           "    -D, --dist        <S>  fixed, exp, norm, zipf                \n"
           "    -P                     Print each request's latency          \n"
           "    -p                     Print 99th latency every 0.2s to file \n"
           "                           under the current working directory   \n"
           "    -d, --duration    <T>  Duration of test                      \n"
           "    -t, --threads     <N>  Number of threads to use              \n"
           "                                                                 \n"
           "    -s, --script      <S>  Load Lua script file                  \n"
           "    -H, --header      <H>  Add header to request                 \n"
           "    -L, --latency          Print latency statistics              \n"
           "    -S, --separate         Print statistics on different url     \n" 
           "    -T, --timeout     <T>  Socket/request timeout [unit:s]       \n"
           "    -B, --batch_latency    Measure latency of whole              \n"
           "                           batches of pipelined ops              \n"
           "                           (as opposed to each op)               \n"
           "    -r, --requests         Show the number of sent requests      \n"
           "    -v, --version          Print version details                 \n"
           "    -f, --file        <S>  Load rate file                        \n"
           "    -o, --stats-file <file> Requests stats output file           \n"
           "    -b, --blocking         Enable blocking mode, connexions wait for response\n"
           "    -R --rq_cx_reset <N>   Requests sent per connexion before it reset\n"
           "                                                                 \n"
           "  Numeric arguments may include a SI unit (1k, 1M, 1G)           \n"
           "  Time arguments may include a time unit (2s, 2m, 2h)            \n");
}

int main(int argc, char **argv) {
    char *url, **headers = zmalloc(argc * sizeof(char *));
    struct http_parser_url parts = {};

    char **urls = zmalloc(argc * sizeof(url));
    struct http_parser_url *mul_parts = zmalloc(argc * sizeof(struct http_parser_url));

    if (parse_args(&cfg, &urls, &mul_parts, headers, argc, argv)) {
        usage();
        exit(1);
    }

    cfg.threads = zcalloc(cfg.num_urls * cfg.threads_count * sizeof(thread));
    uint64_t connections = cfg.connections / cfg.threads_count;
    
    char *time = format_time_s(cfg.duration);

    lua_State **L = zmalloc(cfg.num_urls * sizeof(lua_State *));
    pthread_mutex_init(&statistics.mutex, NULL);
    statistics.requests = zmalloc(cfg.num_urls * sizeof(stats *));
	
	// RS : requests_stats init
    pthread_mutex_init(&requests_stats.mutex, NULL);

	if(requests_stats.file_path != NULL){
		if ((requests_stats.file = fopen(requests_stats.file_path, "w")) == NULL){
			perror("ERROR opening requests_stats file");
			exit(-1);
		}
	}

    /*statitical variables*/
    struct hdr_histogram* total_latency_histogram;
    struct hdr_histogram* total_real_latency_histogram;
    struct hdr_histogram* total_request_histogram;

    hdr_init(1, MAX_LATENCY, 3, &total_latency_histogram);
    hdr_init(1, MAX_LATENCY, 3, &total_real_latency_histogram);
    hdr_init(1, MAX_LATENCY, 3, &total_request_histogram);
    
    uint64_t *runtime_us = zcalloc(cfg.num_urls * sizeof(uint64_t));
    uint64_t total_complete  = 0;
    uint64_t total_bytes     = 0;
    uint64_t sent_requests[cfg.num_urls];
    uint64_t total_sent_requests  = 0;
    uint64_t start_urls[cfg.num_urls];
    /*statitical variables*/

    /* added by RS */
    total_requests_sent_target = (uint64_t*) malloc(cfg.duration * sizeof(uint64_t));
    /* added by TR */
    target_rate = (int*) malloc(cfg.duration * sizeof(int));
    parse_rate(target_rate);


    double throughput = cfg.rate / cfg.threads_count;
    
    char **purls = urls;

    for(int id_url=0; id_url< cfg.num_urls; id_url++ ){
        url = *purls;
        purls++;
        parts = *mul_parts;
        mul_parts++;
        
        char *schema  = copy_url_part(url, &parts, UF_SCHEMA);
        char *host    = copy_url_part(url, &parts, UF_HOST);
        char *port    = copy_url_part(url, &parts, UF_PORT);

        char *service = port ? port : schema;

        if (!strncmp("https", schema, 5)) {
            if ((cfg.ctx = ssl_init()) == NULL) {
                fprintf(stderr, "unable to initialize SSL\n");
                ERR_print_errors_fp(stderr);
                exit(1);
            }
            sock.connect  = ssl_connect;
            sock.close    = ssl_close;
            sock.read     = ssl_read;
            sock.write    = ssl_write;
            sock.readable = ssl_readable;
        }

        signal(SIGPIPE, SIG_IGN);
        signal(SIGINT,  SIG_IGN);

        statistics.requests[id_url] = stats_alloc(10);
        hdr_init(1, MAX_LATENCY, 3, &((statistics.requests[id_url])->histogram));
        // create lua_create
        L[id_url] = script_create(cfg.script, url, headers);

        if (!script_resolve(L[id_url], host, service)) {
            char *msg = strerror(errno);
            fprintf(stderr, "unable to connect to %s:%s %s\n", host, service, msg);
            exit(1);
        }

		printf("Running %s test @ %s\n", time, url);
        printf("  %"PRIu64" threads and %"PRIu64" connections\n\n",
                cfg.threads_count, cfg.connections);

        uint64_t stop_at = time_us() + (cfg.duration * 1000000); // check timeout
        /* added by TR */
        start_time = time_us();

        for (uint64_t id_thread = 0; id_thread < cfg.threads_count; id_thread++) {
            uint64_t i = id_url * cfg.threads_count + id_thread;
            thread *t = &cfg.threads[i];
            t->tid           =  i;
            t->loop          = aeCreateEventLoop(10 + cfg.connections * cfg.num_urls * 3);
            t->connections   = connections;
            t->stop_at       = stop_at;
            t->complete      = 0;
            t->sent          = 0;
            t->monitored     = 0;
            t->target        = throughput/10; //Shuang
            t->accum_latency = 0;
            t->L = script_create(cfg.script, url, headers);
            script_init(L[id_url], t, argc - optind, &argv[optind]);

            if (i == 0) {
                cfg.pipeline = script_verify_request(t->L);
                cfg.dynamic = !script_is_static(t->L);
                if (script_want_response(t->L)) {
                    // printf("script_want_response\n");
                    parser_settings.on_header_field = header_field;
                    parser_settings.on_header_value = header_value;
                    parser_settings.on_body         = response_body;
                }
            }

            if (!t->loop || pthread_create(&t->thread, NULL, &thread_main, t)) {
                char *msg = strerror(errno);
                fprintf(stderr, "unable to create thread %"PRIu64" for %s: %s\n", id_thread, url, msg);
                exit(2);
            }
        }
        start_urls[id_url] = time_us();
    }
    
    struct sigaction sa = {
        .sa_handler = handler,
        .sa_flags   = 0,
    };

    sigfillset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);

    purls = urls;
    for(uint64_t id_url=0; id_url< cfg.num_urls; id_url++ ){        
        url = *purls;
        purls++;

        for (uint64_t id_thread = 0; id_thread < cfg.threads_count; id_thread++) {
            thread *t = &cfg.threads[id_url*cfg.threads_count+id_thread];
            pthread_join(t->thread, NULL); 
        }
        // timer
        runtime_us[id_url] = time_us() -start_urls[id_url];
    }
    
    uint64_t end = time_us();
    uint64_t total_runtime_us = end - start_urls[0];

	
	if(requests_stats.file_path != NULL && (fflush(requests_stats.file) !=0 || fclose(requests_stats.file) != 0)) {
		perror("ERROR closing requests_stats file");
		exit(-1);
	}

    purls = urls;

    for(uint64_t id_url=0; id_url< cfg.num_urls; id_url++ ){
        sent_requests[id_url] = 0;

        url = *purls;
        purls++;

        uint64_t complete = 0;
        uint64_t bytes    = 0;
        errors errors     = { 0 };
        // init 2 histograms with MAX_LATENCY
        struct hdr_histogram* latency_histogram;
        struct hdr_histogram* real_latency_histogram;
        hdr_init(1, MAX_LATENCY, 3, &latency_histogram);
        hdr_init(1, MAX_LATENCY, 3, &real_latency_histogram);
        for (uint64_t id_thread = 0; id_thread < cfg.threads_count; id_thread++) {
            uint64_t i = id_url*cfg.threads_count+id_thread;
            thread *t = &cfg.threads[i];
            complete += t->complete;
            bytes    += t->bytes;

            errors.connect += t->errors.connect;
            errors.read    += t->errors.read;
            errors.write   += t->errors.write;
            errors.timeout += t->errors.timeout;
            errors.status  += t->errors.status;

            hdr_add(latency_histogram, t->latency_histogram);
            hdr_add(real_latency_histogram, t->real_latency_histogram);
            if (cfg.print_all_responses) {
                char filename[10] = {0};
                sprintf(filename, "%" PRIu64 ".txt", i);
                FILE* ff = fopen(filename, "w");
                uint64_t nnum = MAXL;
                if ((t->complete) < nnum) nnum = t->complete;
                for (uint64_t j=1; j <= nnum; ++j)
                    fprintf(ff, "%" PRIu64 "\n", raw_latency[i][j]);
                fclose(ff);
            }

            if(cfg.print_sent_requests){
                sent_requests[id_url] += t->sent;
            }
            
        }

        long double runtime_s   = runtime_us[id_url] / 1000000.0;
        long double req_per_s   = complete   / runtime_s;
        long double bytes_per_s = bytes      / runtime_s;

        stats *latency_stats = stats_alloc(10);
        latency_stats->min = hdr_min(latency_histogram);
        latency_stats->max = hdr_max(latency_histogram);
        latency_stats->histogram = latency_histogram;

        if(cfg.print_separate_histograms){
            printf("\n-----------------------------------------------------------------------\n");
            printf("Test Results @ %s \n", url);
            if(cfg.print_sent_requests){
                printf("Sent %ld requests\n\n", sent_requests[id_url]);
            }

            print_stats_header();
            print_stats("Latency", latency_stats, format_time_us);
            print_stats("Req/Sec", statistics.requests[id_url], format_metric);

            if (cfg.latency) {
                print_hdr_latency(latency_histogram,
                        "Recorded Latency");
                printf("-----------------------------------------------------------------------\n");
            }

            char *runtime_msg = format_time_us(runtime_us[id_url]);

            printf("  %"PRIu64" requests in %s, %sB read\n",
                complete, runtime_msg, format_binary(bytes));
            if (errors.connect || errors.read || errors.write || errors.timeout) {
                printf("  Socket errors: connect %d, read %d, write %d, timeout %d\n",
                    errors.connect, errors.read, errors.write, errors.timeout);
            }

            if (errors.status) {
                printf("  Non-2xx or 3xx responses: %d\n", errors.status);
            }

            printf("Requests/sec: %9.2Lf  \n", req_per_s);
            printf("Transfer/sec: %10sB\n", format_binary(bytes_per_s));

            if (script_has_done(L[id_url])) {
                script_summary(L[id_url], runtime_us[id_url], complete, bytes);
                script_errors(L[id_url], &errors);
                script_done(L[id_url], latency_stats, statistics.requests[id_url]);
            }
        }

        if(cfg.print_sent_requests){
            total_sent_requests += sent_requests[id_url];
        }
        total_complete += complete;
        total_bytes += bytes;
        hdr_add(total_latency_histogram, latency_histogram);
        hdr_add(total_real_latency_histogram, real_latency_histogram);
        hdr_add(total_request_histogram, (statistics.requests[id_url])->histogram);
                
    }

    if(cfg.num_urls>1){    
        printf("\n-----------------------------------------------------------------------\n");
        printf("---------------------------Overall Statistics--------------------------\n");
        printf("-----------------------------------------------------------------------\n");
        stats *latency_stats = stats_alloc(10);
        latency_stats->min = hdr_min(total_latency_histogram);
        latency_stats->max = hdr_max(total_latency_histogram);
        latency_stats->histogram = total_latency_histogram;
        stats *request_stats = stats_alloc(10);
        request_stats->min = hdr_min(total_request_histogram);
        request_stats->max = hdr_max(total_request_histogram);
        request_stats->histogram = total_request_histogram;

        if(cfg.print_sent_requests){
            printf("Sent %ld requests\n\n", total_sent_requests);
        }
        print_stats_header();
        print_stats("Latency", latency_stats, format_time_us);
        print_stats("Req/Sec", request_stats, format_metric);

        if (cfg.latency) {
            print_hdr_latency(total_latency_histogram,
                    "Recorded Overall Latency");
            printf("-----------------------------------------------------------------------\n");
        }
        long double total_runtime_s   = total_runtime_us/ 1000000.0;
        long double total_req_per_s   = total_complete   / total_runtime_s;
        long double total_bytes_per_s = total_bytes      / total_runtime_s;
        char *total_runtime_msg = format_time_us(total_runtime_us);

        printf("  Overall %"PRIu64" requests in %s, %sB read\n", total_complete, total_runtime_msg, format_binary(total_bytes));

        printf("Requests/sec: %9.2Lf\n", total_req_per_s);
        printf("Transfer/sec: %10sB\n", format_binary(total_bytes_per_s));
    }
    return 0;
}

void *thread_main(void *arg) {
    thread *thread = arg;
    /* State of an event based program */
    aeEventLoop *loop = thread->loop;
    // connection cs
    thread->cs = zcalloc(thread->connections * sizeof(connection));
    tinymt64_init(&thread->rand, time_us());
    hdr_init(1, MAX_LATENCY, 3, &thread->latency_histogram);
    hdr_init(1, MAX_LATENCY, 3, &thread->real_latency_histogram);

    char *request = NULL;
    size_t length = 0;

    if (!cfg.dynamic) {
        script_request(thread->L, &request, &length);
    }
    thread->ff = NULL;

    if ((cfg.print_realtime_latency) && ((thread->tid%cfg.threads_count) == 0)) {
        char filename[50];
        snprintf(filename, 50, "%s/url%" PRIu64 "thread%" PRIu64 ".txt", getcwd(NULL,0), (thread->tid/cfg.threads_count), (thread->tid%cfg.threads_count));
        printf("filename %s\n",filename);
        thread->ff = fopen(filename, "w");
    }

    // double throughput = (thread->throughput / 1000000.0) / thread->connections;

    connection *c = thread->cs;

    
    /* RS : spread the connections start over the first second */
    uint64_t step = 1000 / thread->connections;

    for (uint64_t i = 0; i < thread->connections; i++, c++) {
        c->thread     = thread;
        c->ssl        = cfg.ctx ? SSL_new(cfg.ctx) : NULL;
        c->request    = request;
        c->length     = length;
        c->complete   = 0;
        c->done   = 0;
        c->sent       = 0;
		c->last_response_timestamp = time_us() + (i * step * 1000);
		aeCreateTimeEvent(loop, i*step, delayed_initial_connect, c, NULL); // delay initial connection
    }

    uint64_t calibrate_delay = CALIBRATE_DELAY_MS + (thread->connections * step);
    uint64_t timeout_delay = TIMEOUT_INTERVAL_MS + (thread->connections * step);

    aeCreateTimeEvent(loop, calibrate_delay, calibrate, thread, NULL);
    aeCreateTimeEvent(loop, timeout_delay, check_timeouts, thread, NULL);
    
	if (thread->tid == 0 && requests_stats.file_path != NULL) {
		fprintf(requests_stats.file, "timestamp,requests_sent,responses,1xx,2xx,3xx,4xx,5xx,timeout\n");
		aeCreateTimeEvent(loop, 1, requestsStats, thread, NULL);
	}

    thread->start = time_us();
    /*aeMain does the job of processing the event loop that is initialized in the previous phase.*/
    aeMain(loop);

    aeDeleteEventLoop(loop);
 
    zfree(thread->cs);

    if (cfg.print_realtime_latency && (thread->tid % cfg.threads_count) == 0) fclose(thread->ff);

    return NULL;
}

static int requestsStats(aeEventLoop *loop, long long id, void *data) {
	
	uint64_t rqsts_sent = 0;
	uint64_t responses_count = 0;
	uint64_t timeout_count = 0;
	for (uint64_t i = 0; i < cfg.threads_count; i++) {
		thread *t = &cfg.threads[i];
		rqsts_sent += t->sent;
		responses_count += t->complete;
		timeout_count += t->errors.timeout;
	}
	fprintf(requests_stats.file, "%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld\n",
		time_us() / 1000000,
		rqsts_sent,
		responses_count,
		requests_stats.stats[0],
		requests_stats.stats[1],
		requests_stats.stats[2],
		requests_stats.stats[3],
		requests_stats.stats[4],
		timeout_count
	);
	return 1000;
}

static int connect_socket(thread *thread, connection *c) {
    struct addrinfo *addr = thread->addr;
    struct aeEventLoop *loop = thread->loop;
    int fd, flags;

    fd = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);

    flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    if (connect(fd, addr->ai_addr, addr->ai_addrlen) == -1) {
        if (errno != EINPROGRESS) goto error;
    }

    flags = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flags, sizeof(flags));

    flags = AE_READABLE | AE_WRITABLE;

    if (aeCreateFileEvent(loop, fd, flags, socket_connected, c) == AE_OK) {
        c->parser.data = c;
        c->fd = fd;
        return fd;
    }
            
  error:
    thread->errors.connect++;
    close(fd);
    return -1;
}

static int reconnect_socket(thread *thread, connection *c) {
    aeDeleteFileEvent(thread->loop, c->fd, AE_WRITABLE | AE_READABLE);
    sock.close(c);
    close(c->fd);
    // printf("reconnect_socket\n");
    return connect_socket(thread, c);
}

static int delayed_initial_connect(aeEventLoop *loop, long long id, void *data) {
    connection* c = data;
    connect_socket(c->thread, c);
    return AE_NOMORE;
}

static int calibrate(aeEventLoop *loop, long long id, void *data) {
    thread *thread = data;

    long double mean = hdr_mean(thread->latency_histogram);
    long double latency = hdr_value_at_percentile(
            thread->latency_histogram, 90.0) / 1000.0L;
    long double interval = MAX(latency * 2, 10);

    if (mean == 0) return CALIBRATE_DELAY_MS;

    thread->mean     = (uint64_t) mean;
    hdr_reset(thread->latency_histogram);

    thread->start    = time_us();
    thread->interval = interval;
    thread->requests = 0;
    
    printf("  Thread calibration: mean lat.: %.3fms, rate sampling interval: %dms\n",
            (thread->mean)/1000.0,
            thread->interval);

    aeCreateTimeEvent(loop, thread->interval, sample_rate, thread, NULL);

    return AE_NOMORE;
}

static int check_timeouts(aeEventLoop *loop, long long id, void *data) {
    thread *thread = data;
    connection *c  = thread->cs;
    uint64_t now   = time_us();

    for (uint64_t i = 0; i < thread->connections; i++, c++) {
        if (
			c->sent > c->done // if there is request in the air
			&& (now - c->last_response_timestamp) > (cfg.timeout * 1000) // if the last response was received more than timeout earlier
		) {
			// printf("timeout : sent: %ld, done: %ld, lastresp: %ld, now: %ld, diff: %ld, timeout: %ld\n", c->sent, c->done, c->last_response_timestamp, now, now - c->last_response_timestamp, cfg.timeout * 1000);
            thread->errors.timeout += c->sent - c->done;
			c->done = c->sent;
			reconnect_socket(thread, c);
        }
    }

    if (stop || now >= thread->stop_at) {
        aeStop(loop);
    }

    return TIMEOUT_INTERVAL_MS; // call check_timeouts after 2s
}

static int sample_rate(aeEventLoop *loop, long long id, void *data) {
    thread *thread = data;

    uint64_t elapsed_ms = (time_us() - thread->start) / 1000;
    // requests here indicates: real-time throughput of thread. (req/sec/thread)
    uint64_t requests = (thread->requests / (double) elapsed_ms) * 1000;

    uint64_t id_url = thread->tid / cfg.threads_count; 
    pthread_mutex_lock(&statistics.mutex);
    stats_record(statistics.requests[id_url], requests);
    pthread_mutex_unlock(&statistics.mutex);

    thread->requests = 0;
    thread->start    = time_us();
    return thread->interval; // call sample_rate again after thread->interval
}

static int header_field(http_parser *parser, const char *at, size_t len) {
    connection *c = parser->data;
    if (c->state == VALUE) {
        *c->headers.cursor++ = '\0';
        c->state = FIELD;
    }
    buffer_append(&c->headers, at, len);
    return 0;
}

static int header_value(http_parser *parser, const char *at, size_t len) {
    connection *c = parser->data;
    if (c->state == FIELD) {
        *c->headers.cursor++ = '\0';
        c->state = VALUE;
    }
    buffer_append(&c->headers, at, len);
    return 0;
}

static int response_body(http_parser *parser, const char *at, size_t len) {
    connection *c = parser->data;
    buffer_append(&c->body, at, len);
    return 0;
}

/* added by TR */
static uint64_t usec_to_next_send(connection *c) {
    uint64_t now = time_us();
    int index = (now-start_time)/1000000;
    uint64_t delay = target_rate[index%cfg.duration];
    /* printf("index for next rate: %d\n", index); */

    // max sleep duration of 2s
    return min(delay, 2 * 1000000);
}



static int delay_request_direct(aeEventLoop *loop, long long id, void *data) {
    connection* c = data;
    aeCreateFileEvent(c->thread->loop, c->fd, AE_WRITABLE, socket_writeable, c);
    return AE_NOMORE;
}

static int response_complete(http_parser *parser) {
    connection *c = parser->data;
    thread *thread = c->thread;
    uint64_t now = time_us();
    int status = parser->status_code;

    thread->complete++;
    thread->requests++;

    if (status > 399) {
        thread->errors.status++;
    }

	pthread_mutex_lock(&requests_stats.mutex);
    requests_stats.stats[(status / 100) - 1]++;
    pthread_mutex_unlock(&requests_stats.mutex);

    if (c->headers.buffer) {
        *c->headers.cursor++ = '\0';
        script_response(thread->L, status, &c->request, c->length, &c->headers, &c->body);
        c->state = FIELD;
    }

    // Record if needed, either last in batch or all, depending in cfg:
    if (cfg.record_all_responses) {
        assert(now > c->actual_latency_start[c->complete & MAXO] );
        uint64_t actual_latency_timing = now - c->actual_latency_start[c->complete & MAXO];
        hdr_record_value(thread->latency_histogram, actual_latency_timing);
        hdr_record_value(thread->real_latency_histogram, actual_latency_timing);

        thread->monitored++;
        thread->accum_latency += actual_latency_timing;
        // thread->target  = throughput/10; response side twice the interval.
        if (thread->monitored == thread->target) {
            if (cfg.print_realtime_latency && (thread->tid%cfg.threads_count) == 0) {
                fprintf(thread->ff, "%" PRId64 "\n", hdr_value_at_percentile(thread->real_latency_histogram, 99));
                fflush(thread->ff);
            }
            thread->monitored = 0;
            thread->accum_latency = 0;
            hdr_reset(thread->real_latency_histogram);
        }
        if (cfg.print_all_responses && ((thread->complete) < MAXL))
            raw_latency[thread->tid][thread->complete] = actual_latency_timing;
    }

    // Count all responses (including pipelined ones:)
    c->complete++;
	c->done++;
	c->last_response_timestamp = now;

    if (now >= thread->stop_at) {
        aeStop(thread->loop);
        goto done;
    }

    if (!http_should_keep_alive(parser)) {
        reconnect_socket(thread, c);
        goto done;
    }

    http_parser_init(parser, HTTP_RESPONSE);

  done:
    return 0;
}

static void socket_connected(aeEventLoop *loop, int fd, void *data, int mask) {
    connection *c = data;

    switch (sock.connect(c)) {
        case OK:    break;
        case ERROR: goto error;
        case RETRY: return;
    }

    http_parser_init(&c->parser, HTTP_RESPONSE);
    c->written = 0;

    aeCreateFileEvent(c->thread->loop, fd, AE_READABLE, socket_readable, c);

    aeCreateFileEvent(c->thread->loop, fd, AE_WRITABLE, socket_writeable, c);

    return;

  error:
    c->thread->errors.connect++;
    reconnect_socket(c->thread, c);

}

static void socket_writeable(aeEventLoop *loop, int fd, void *data, int mask) {
    connection *c = data;
    thread *thread = c->thread;

    uint64_t index = (time_us()-start_time)/1000000;
    if (index >= cfg.duration) {
        aeStop(thread->loop);
        return;
    }
    // delay that connection if enough requests have already been sent this second or if it is blocked
    if (
		!c->written // no request is currently being sent through the connexion
		&& (
			(cfg.blocking && c->sent != c->done) // if blocking mode is enable and no response have yet been received for the last request sent
			|| thread->sent >= (total_requests_sent_target[index] / cfg.threads_count) // enough request have been send for the time
		)
	) {
        // Not yet time to send. Delay:
        aeDeleteFileEvent(loop, fd, AE_WRITABLE);
        aeCreateTimeEvent(thread->loop, 1000, delay_request_direct, c, NULL);
        // printf("delayed, sent: %ld, target: %ld\n", thread->sent, (total_requests_sent_target[index] / cfg.threads_count));
        return;
    }

    if (!c->written && cfg.dynamic) {
        script_request(thread->L, &c->request, &c->length);
    }
    // buf: indicates the pos. where the request remaining to send is.
    char  *buf = c->request + c->written;
    // len: the length of request that haven't been sent.
    size_t len = c->length  - c->written;
    size_t n;
    switch (sock.write(c, buf, len, &n)) {
        case OK:    break;
        case ERROR: goto error;
        case RETRY: return;
    }
    if (!c->written) {
		uint64_t now = time_us();
        c->start = now;
        c->actual_latency_start[c->sent & MAXO] = c->start;

		if (c->sent == c->done) {
			c->last_response_timestamp = now;
		}

        c->sent++;
    }
    c->written += n;
    if (c->written == c->length) {
        c->written = 0;
        thread->sent++;

        aeDeleteFileEvent(loop, fd, AE_WRITABLE);

		if (c->sent % cfg.rq_sent_before_cx_reset == 0) {
			reconnect_socket(thread, c);
			return;
		}

        
        uint64_t time_usec_to_wait = usec_to_next_send(c);
        if (time_usec_to_wait) {
            int msec_to_wait = round((time_usec_to_wait / 1000.0L) + 0.5);

            // Not yet time to send. Delay:
            aeDeleteFileEvent(loop, fd, AE_WRITABLE);
            aeCreateTimeEvent(thread->loop, msec_to_wait, delay_request_direct, c, NULL);
            return;
        }
        

        aeCreateFileEvent(thread->loop, c->fd, AE_WRITABLE, socket_writeable, c);
    }
    return;

  error:
    thread->errors.write++;
    reconnect_socket(thread, c);
}


static void socket_readable(aeEventLoop *loop, int fd, void *data, int mask) {
    connection *c = data;
    size_t n;

    do {
        switch (sock.read(c, &n)) {
            case OK:    break;
            case ERROR: goto error;
            case RETRY: return;
        }
		size_t ret = http_parser_execute(&c->parser, &parser_settings, c->buf, n);
        if (ret != n) {
			goto error;
		}
        c->thread->bytes += n;
    } while (n == RECVBUF && sock.readable(c) > 0);

    return;

  error:
    c->thread->errors.read++;
    reconnect_socket(c->thread, c);
}

static uint64_t time_us() {
    struct timeval t;
    gettimeofday(&t, NULL);
    return (t.tv_sec * 1000000) + t.tv_usec;
}

static char *copy_url_part(char *url, struct http_parser_url *parts, enum http_parser_url_fields field) {
    char *part = NULL;

    if (parts->field_set & (1 << field)) {
        uint16_t off = parts->field_data[field].off;
        uint16_t len = parts->field_data[field].len;
        part = zcalloc(len + 1 * sizeof(char));
        memcpy(part, &url[off], len);
    }

    return part;
}

static struct option longopts[] = {
    { "connections",    required_argument, NULL, 'c' },
    { "duration",       required_argument, NULL, 'd' },
    { "threads",        required_argument, NULL, 't' },
    { "script",         required_argument, NULL, 's' },
    { "header",         required_argument, NULL, 'H' },
    { "separate",       no_argument,       NULL, 'S' },
    { "latency",        no_argument,       NULL, 'L' },
    { "requests",       no_argument,       NULL, 'r' },
    { "batch_latency",  no_argument,       NULL, 'B' },
    { "timeout",        required_argument, NULL, 'T' },
    { "help",           no_argument,       NULL, 'h' },
    { "version",        no_argument,       NULL, 'v' },
    { "dist",           required_argument, NULL, 'D' },
    { "stats-file",     required_argument, NULL, 'o' },
    { "blocking",       no_argument,       NULL, 'b' },
    { "rq_cx_reset",    required_argument, NULL, 'R' },
    { NULL,             0,                 NULL,  0  }
};

static int parse_args(struct config *cfg, char ***urls, struct http_parser_url **mul_parts, char **headers, int argc, char **argv) {
    int c;
    char **header = headers;
    char ***url =   zmalloc(sizeof(urls));
    *url = *urls;
    struct http_parser_url **mul_part = zmalloc(sizeof(mul_parts));
    *mul_part = *mul_parts;

    memset(cfg, 0, sizeof(struct config));
    cfg->num_urls = 0;
    cfg->threads_count     = 2;
    cfg->connections = 10;
    cfg->duration    = 10;
    cfg->timeout     = SOCKET_TIMEOUT_MS;
    cfg->rate        = 0;
    cfg->record_all_responses = true;
    cfg->print_all_responses = false;
    cfg->print_realtime_latency = false;
    cfg->print_separate_histograms = false;
    cfg->print_sent_requests = false;
    cfg->dist = 0;
	cfg->blocking = false;
	cfg->rq_sent_before_cx_reset = DEFAULT_RQ_SENT_BEFORE_CX_RESET;

    while ((c = getopt_long(argc, argv, "t:c:s:d:D:H:T:f:o:R:LPrSpbBv?", longopts, NULL)) != -1) {
        switch (c) {
            case 't':
                if (scan_metric(optarg, &cfg->threads_count)) return -1;
                break;
            case 'c':
                if (scan_metric(optarg, &cfg->connections)) return -1;
                break;
            case 'D':
                if (!strcmp(optarg, "fixed"))
                    cfg->dist = 0;
                if (!strcmp(optarg, "exp"))
                    cfg->dist = 1;
                if (!strcmp(optarg, "norm"))
                    cfg->dist = 2;
                if (!strcmp(optarg, "zipf"))
                    cfg->dist = 3;
                break;
            case 'd':
                if (scan_time(optarg, &cfg->duration)) return -1;
                break;
            case 's':
                cfg->script = optarg;
                break;
            case 'H':
                *header++ = optarg;
                break;
            case 'P': /* Shuang: print each requests's latency */
                cfg->print_all_responses = true;
                break;
            case 'p': /* Shuang: print avg latency every 0.2s */
                cfg->print_realtime_latency = true;
                break;
            case 'L':
                cfg->latency = true;
                break;
            case 'r':
                cfg->print_sent_requests = true;
                break;
            case 'S':
                cfg->print_separate_histograms = true;
                break;
            case 'B':
                cfg->record_all_responses = false;
                break;
            case 'T':
                if (scan_time(optarg, &cfg->timeout)) return -1;
                cfg->timeout *= 1000;
                break;
            case 'v':
                printf("wrk %s [%s] ", VERSION, aeGetApiName());
                printf("Copyright (C) 2012 Will Glozer\n");
                break;
            case 'f':
                /* added by TR */
                cfg->rate_file = optarg;
                break;
			case 'o':
				requests_stats.file_path = optarg;
				break;
			case 'b':
				cfg->blocking = true;
				break;
			case 'R':
				if (scan_metric(optarg, &cfg->rq_sent_before_cx_reset)) return -1;
				break;
            case 'h': 
            case '?': 
            case ':': 
            default:
                return -1;
        }
    }

    if (optind == argc || !cfg->threads_count || !cfg->duration) return -1;

    if (!cfg->connections || cfg->connections < cfg->threads_count) {
        fprintf(stderr, "number of connections must be >= threads\n");
        return -1;
    }

    for(int i = optind; i<argc; i++)
    {   
        if (!script_parse_url(argv[i], *mul_part)) {
            // RS : further args must be args for the init lua function
            break;
            // fprintf(stderr, "invalid URL: %s\n", argv[i]);
            // return -1;
        }
        **url = argv[i];
        (*mul_part)++;
        (*url)++;
        cfg->num_urls += 1;
    }
    cfg->print_separate_histograms = (cfg->print_separate_histograms || (cfg->num_urls==1));
    *header = NULL;
    *mul_part = NULL;
    *url = NULL;
    return 0;
}

#include <sys/stat.h>
int file_size(char* filename)
{
    struct stat statbuf;
    stat(filename,&statbuf);
    int size=statbuf.st_size;
 
    return size;
}

static void print_stats_header() {
    printf("  Thread Stats%6s%11s%8s%12s\n", "Avg", "Stdev", "99%", "+/- Stdev");
}

static void print_units(long double n, char *(*fmt)(long double), int width) {
    char *msg = fmt(n);
    int len = strlen(msg), pad = 2;

    if (isalpha(msg[len-1])) pad--;
    if (isalpha(msg[len-2])) pad--;
    width -= pad;

    printf("%*.*s%.*s", width, width, msg, pad, "  ");

    free(msg);
}

static void print_stats(char *name, stats *stats, char *(*fmt)(long double)) {
    long double mean  = stats_summarize(stats);
    long double stdev = stats_stdev(stats, mean);

    printf("    %-10s", name);
    print_units(mean,  fmt, 8);
    print_units(stdev, fmt, 10);
    print_units(stats_percentile(stats, 99.0), fmt, 9);
    printf("%8.2Lf%%\n", stats_within_stdev(stats, mean, stdev, 1));
}

static void print_hdr_latency(struct hdr_histogram* histogram, const char* description) {
    long double percentiles[] = { 50.0, 75.0, 90.0, 99.0, 99.9, 99.99, 99.999, 100.0};
    printf("  Latency Distribution (HdrHistogram - %s)\n", description);
    for (size_t i = 0; i < sizeof(percentiles) / sizeof(long double); i++) {
        long double p = percentiles[i];
        int64_t n = hdr_value_at_percentile(histogram, p);
        printf("%7.3Lf%%", p);
        print_units(n, format_time_us, 10);
        printf("\n");
    }
    printf("\n%s\n", "  Detailed Percentile spectrum:");
    hdr_percentiles_print(histogram, stdout, 5, 1000.0, CLASSIC);
}