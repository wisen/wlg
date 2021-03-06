/*
This file is part of wlg - A pretty simple workload mix generator
Copyright (C) 2014 Patrick Bellasi <derkling@gmail.com>

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE.  See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 51 Franklin
Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/types.h>

#ifdef DEBUG
# define DB(x) x
#else
# define DB(x)
#endif


////////////////////////////////////////////////////////////////////////////////
// Log formatting
////////////////////////////////////////////////////////////////////////////////

#define COLOR_WHITE  "\033[1;37m"
#define COLOR_LGRAY  "\033[37m"
#define COLOR_GRAY   "\033[1;30m"
#define COLOR_BLACK  "\033[30m"
#define COLOR_RED    "\033[31m"
#define COLOR_LRED   "\033[1;31m"
#define COLOR_GREEN  "\033[32m"
#define COLOR_LGREEN "\033[1;32m"
#define COLOR_BROWN  "\033[33m"
#define COLOR_YELLOW "\033[1;33m"
#define COLOR_BLUE   "\033[34m"
#define COLOR_LBLUE  "\033[1;34m"
#define COLOR_PURPLE "\033[35m"
#define COLOR_PINK   "\033[1;35m"
#define COLOR_CYAN   "\033[36m"
#define COLOR_LCYAN  "\033[1;36m"

# define LOG_FMT(color, level, pid, worker, fmt) \
		"\033[0m%011.3f %c %6u:%-8.8s: " color fmt "\033[0m", \
		timespec_elapsed_us(NULL), \
		level, pid, worker

# define WD(fmt) LOG_FMT(COLOR_LGRAY, 'D', wdata->pid, wdata->name, fmt)
# define FD(fmt) LOG_FMT(COLOR_LGRAY, 'D', pid, "wlg", fmt)
# define FI(fmt) LOG_FMT(COLOR_GREEN, 'I', pid, "wlg", fmt)
# define FE(fmt) LOG_FMT(COLOR_RED,   'E', pid, "wlg", fmt)

////////////////////////////////////////////////////////////////////////////////
// Globals
////////////////////////////////////////////////////////////////////////////////

static int conf_vr = 0;     // Verbose output
static uint8_t conf_bw = 0; // BATCH workers count
static uint8_t conf_iw = 0; // INTERACTIVE workers count
static uint8_t conf_pw = 0; // PERIODIC workers count
static uint8_t conf_yw = 0; // YIELD workers count
static uint8_t conf_tm = 0;
static uint8_t conf_td = 5; // Test duration
static struct timespec start_ts;
static char *conf_iparams;
static char *conf_pparams;
static char *conf_yparams;
static float start_us = 0;
static uint32_t pid = 0;

/* Workers synchronized start support */
pthread_mutex_t start_mtx = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  start_cv = PTHREAD_COND_INITIALIZER;

struct wdata {

	uint8_t id;
	uint32_t pid;

	// Name format "K_000"
	char name[9];

	/* Worker kind */
#define WORKER_BATCH       0
#define WORKER_INTERACTIVE 1
#define WORKER_PERIODC     2
#define WORKER_YIELD       3
	uint8_t kind;

	/* Worker params */
	union {
		struct {
			uint32_t interval_max;
			uint32_t duration_max;
		} interrupt;
		struct {
			uint32_t duration;
			uint32_t duty_cycle;
		} period;
		struct {
			uint32_t period;
			uint32_t interval;
		} yield;
	} params;

};

static char *worker_kind[] = {
	"Batch", "Interactive", "Periodic", "Yield" };


static void
barf(const char *msg)
{
        fprintf(stderr, "%s (error: %s)\n", msg, strerror(errno));
        exit(1);
}

/** Return the PID of the calling process/thread */
inline pid_t gettid() {
	return syscall(SYS_gettid);
}

////////////////////////////////////////////////////////////////////////////////
// Time management utilities
////////////////////////////////////////////////////////////////////////////////

#define US_TO_NS 1000
#define  S_TO_MS 1000
#define MS_TO_NS 1000000
#define  S_TO_US 1000000
#define  S_TO_NS 1000000000

void timespec_now(struct timespec *ts)
{
	clock_gettime(CLOCK_MONOTONIC_RAW, ts);
}

float
timespec_elapsed_us(struct timespec *ref_ts)
{
	struct timespec now_ts;
	float ref_us = start_us;
	float now_us;

	clock_gettime(CLOCK_MONOTONIC_RAW, &now_ts);

	if (ref_ts != NULL)
		ref_us = ((float)ref_ts->tv_sec * S_TO_US
			+ (float)ref_ts->tv_nsec / US_TO_NS);

	now_us  = ((float)now_ts.tv_sec * S_TO_US)
		+ ((float)now_ts.tv_nsec / US_TO_NS);

	return now_us - ref_us;

}

void timespec_add_ms(struct timespec *ts, uint32_t ms)
{
	uint32_t sec = ms / S_TO_MS;
	ms = ms - sec * S_TO_MS;

	// perform the addition
	ts->tv_nsec += ms * MS_TO_NS;

	// adjust the time
	ts->tv_sec += ts->tv_nsec / S_TO_NS + sec;
	ts->tv_nsec = ts->tv_nsec % S_TO_NS;
}

void timespec_add_us(struct timespec *ts, uint32_t us)
{
	uint32_t sec = us / S_TO_US;
	us = us - sec * S_TO_US;

	// perform the addition
	ts->tv_nsec += us * US_TO_NS;

	// adjust the time
	ts->tv_sec += ts->tv_nsec / S_TO_NS + sec;
	ts->tv_nsec = ts->tv_nsec % S_TO_NS;
}

void timespec_add_ns(struct timespec *ts, uint32_t ns)
{
	uint32_t sec = ns / S_TO_NS;
	ns = ns - sec * S_TO_NS;

	// perform the addition
	ts->tv_nsec += ns;

	// adjust the time
	ts->tv_sec += ts->tv_nsec / S_TO_NS + sec;
	ts->tv_nsec = ts->tv_nsec % S_TO_NS;
}

int timespec_compare(struct timespec *a, struct timespec *b)
{
	if (a->tv_sec != b->tv_sec)
		return a->tv_sec - b->tv_sec;

	return a->tv_nsec - b->tv_nsec;
}

// not null if a is older than b, i.e. a > b
int timespec_older(struct timespec *a, struct timespec *b)
{
	if (a->tv_sec > b->tv_sec)
		return 1;

	if (a->tv_sec < b->tv_sec)
		return 0;

	if (a->tv_nsec >= b->tv_nsec)
		return 1;

	return 0;
}


// computes a = a - b
void timespec_subtract(struct timespec *a, struct timespec *b)
{
	a->tv_nsec = a->tv_nsec - b->tv_nsec;
	if (a->tv_nsec < 0) {
		// borrow.
		a->tv_nsec += S_TO_NS;
		a->tv_sec -= 1;
	}

	a->tv_sec = a->tv_sec - b->tv_sec;
}

// convert the timespec into milliseconds (may overflow)
int timespec_milliseconds(struct timespec *a) 
{
	return a->tv_sec * S_TO_MS + a->tv_nsec / MS_TO_NS;
}

void timespec_print(struct timespec *a)
{
	printf("%li.%09li\n", a->tv_sec, a->tv_nsec);
}


////////////////////////////////////////////////////////////////////////////////
// Workers definition
////////////////////////////////////////////////////////////////////////////////

/* Block until we're ready to go */
static void
sync_start(struct wdata *wdata)
{
	/* Wait start conditon */
	pthread_mutex_lock(&start_mtx);
	pthread_cond_wait(&start_cv, &start_mtx);
	pthread_mutex_unlock(&start_mtx);

	DB(printf(WD("started\n")));
}


static void
busy_loop(void)
{
	volatile uint16_t i = 1;
	for ( ; i ; ++i);
}

static inline uint32_t
normal_random(uint32_t max_value)
{
	double value = max_value;
	value *= random();
	value /= RAND_MAX;
	return value;
}

static void
worker_batch(struct wdata *wdata)
{
	/* Dummy busy loop */
	//DB(printf("%s loop\n", wdata->name));
	busy_loop();
}

static void
worker_interactive(struct wdata *wdata)
{
	uint32_t delay, process;
	struct timespec end_ts, now_ts;

	/* Here we just need fast even if not reporducible and/or "safe"
	 * random numbers. We just need to introduce some variation on
	 * timings */

	/* Setup next interrupt (uniform distribution) */
	delay = normal_random(wdata->params.interrupt.interval_max);
	DB(printf(WD("sleeping for %9d [us]\n"), delay));
	usleep(delay);

	/* Setup processing time (unifor distribution) */
	process = normal_random(wdata->params.interrupt.duration_max);
	DB(printf(WD("process  for %9d [us]\n"), process));

	/* Configure processing end */
	clock_gettime(CLOCK_MONOTONIC_RAW, &end_ts);
	timespec_add_us(&end_ts, process);

	//printf("End processing @ ");
	//timespec_print(&end_ts);

	while (1) {
		clock_gettime(CLOCK_MONOTONIC_RAW, &now_ts);
		//printf("Now processing @ ");
		//timespec_print(&now_ts);
		if (timespec_older(&now_ts , &end_ts))
			break;
		busy_loop();
	}

}

static void
worker_periodic(struct wdata *wdata)
{
	uint32_t sleep, process;
	struct timespec end_ts, now_ts;

	/* Setup next interrupt (uniform distribution) */
	process = ( (float) wdata->params.period.duration *
		  ( (float) wdata->params.period.duty_cycle / 100.0) );
	sleep   = wdata->params.period.duration - process;

	DB(printf(WD("sleeping for %9d [us]\n"), sleep));
	usleep(sleep);

	DB(printf(WD("process  for %9d [us]\n"), process));

	/* Configure processing end */
	clock_gettime(CLOCK_MONOTONIC_RAW, &end_ts);
	timespec_add_us(&end_ts, process);

	//printf("End processing @ ");
	//timespec_print(&end_ts);

	while (1) {
		clock_gettime(CLOCK_MONOTONIC_RAW, &now_ts);
		//printf("Now processing @ ");
		//timespec_print(&now_ts);
		if (timespec_older(&now_ts , &end_ts))
			break;
		busy_loop();
	}
}

static void
worker_yield(struct wdata *wdata)
{
	uint32_t period   = wdata->params.yield.period;
	uint32_t interval = wdata->params.yield.interval;
	struct timespec now_ts, end_ts, yield_ts;

	/* Configure processing end */
	clock_gettime(CLOCK_MONOTONIC_RAW, &end_ts);
	timespec_add_us(&end_ts, period);

	// Burst period
	DB(printf(WD("burst  for %9d [us]\n"), period));
	while (1) {
		clock_gettime(CLOCK_MONOTONIC_RAW, &now_ts);
		//printf("Now processing @ ");
		//timespec_print(&now_ts);
		if (timespec_older(&now_ts , &end_ts))
			break;
		busy_loop();
	}

	/* Configure yield end */
	clock_gettime(CLOCK_MONOTONIC_RAW, &end_ts);
	timespec_add_us(&end_ts, period);

	/* Configure next yield time */
	clock_gettime(CLOCK_MONOTONIC_RAW, &yield_ts);
	DB(printf(WD("Sum first yield to @ ")));
	DB(timespec_print(&yield_ts));
	timespec_add_us(&yield_ts, interval);

	// Yield period
	DB(printf(WD("yield  for %9d [us]\n"), period));
	DB(printf(WD("next yield @ ")));
	DB(timespec_print(&yield_ts));
	while (1) {
		clock_gettime(CLOCK_MONOTONIC_RAW, &now_ts);
		DB(printf(WD("Now processing @ ")));
		DB(timespec_print(&now_ts));
		if (timespec_older(&now_ts , &end_ts))
			break;
		// Yield if an interval has passed
		if (timespec_older(&now_ts , &yield_ts)) {
			timespec_add_us(&yield_ts, interval);
			DB(printf(WD("YIELD, next yield @")));
			DB(timespec_print(&yield_ts));
			pthread_yield();
		}
	}
}

static void *
worker(void *conf)
{
	struct wdata *wdata = (struct wdata*) conf;
	struct timespec now_ts;
	struct timespec end_ts;

	/* Setup random number generator */
	wdata->pid = gettid();
	srandom(wdata->pid);

	/* Setup worker name */
	snprintf(wdata->name, sizeof(wdata->name), "wlg_%c%03d",
		worker_kind[wdata->kind][0], wdata->id);
	prctl(PR_SET_NAME, wdata->name, NULL, NULL, NULL);
	DB(printf(WD("worker created\n")));

	sync_start(wdata);

	/* Setup worker termination time */
	clock_gettime(CLOCK_MONOTONIC_RAW, &end_ts);
	end_ts.tv_sec += conf_td;

	while (1) {

		/* Check end of test */
		clock_gettime(CLOCK_MONOTONIC_RAW, &now_ts);
		if (timespec_older(&now_ts, &end_ts))
			break;

		/* Do workload */
		switch (wdata->kind) {
		case WORKER_BATCH:
			worker_batch(wdata);
			break;
		case WORKER_INTERACTIVE:
			worker_interactive(wdata);
			break;
		case WORKER_PERIODC:
			worker_periodic(wdata);
			break;
		case WORKER_YIELD:
			worker_yield(wdata);
			break;
		}

	}

	DB(printf(WD("terminated\n")));

	return NULL;
}

////////////////////////////////////////////////////////////////////////////////
// Setup workload
////////////////////////////////////////////////////////////////////////////////

static char *opts = "b:d:hi:p:y:";
static struct option long_options[] =
{
	{"batch",    required_argument, 0, 'b'},
	{"duration", required_argument, 0, 'd'},
	{"help",     no_argument,       0, 'h'},
	{"intrrupt", required_argument, 0, 'i'},
	{"process",  required_argument, 0, 'p'},
	{"verbose",  no_argument,       &conf_vr, 1},
	{"yield",    required_argument, 0, 'y'},
	{0, 0, 0, 0}
};

static void
print_usage(char *prog)
{
	fprintf(stderr, " \n");
	fprintf(stderr, "Usage: %s <options> <workload>\n", prog);
	fprintf(stderr, " \n");
	fprintf(stderr, " <options>:\n");
	fprintf(stderr, "   -d, --duration - test duration in [s] (default: 5)\n");
	fprintf(stderr, "   --verbose      - enable verbose output\n");
	fprintf(stderr, " \n");
	fprintf(stderr, " <workload>:\n");
	fprintf(stderr, "   -b N - spawn N BATCH threads\n");
	fprintf(stderr, "   -i N,[<I,D>] - spawn N INTERACTIVE tasks with the specified execution model:\n");
	fprintf(stderr, "            start (at least) once every I [us]\n");
	fprintf(stderr, "            run for up to D [us]\n");
	fprintf(stderr, "     I and D are upper bounds for normally distributed actual values\n");
	fprintf(stderr, "   -p N,[<P,D>] - spawn N PERIODC tasks with the specified execution model:\n");
	fprintf(stderr, "            period duration of P [us]\n");
	fprintf(stderr, "            running duty-cycle of D [us]\n");
	fprintf(stderr, "   -y N,[<P,I>] - spawn N YIELD tasks with the specified execution model:\n");
	fprintf(stderr, "            burst/yield period duration of P [us]\n");
	fprintf(stderr, "            yielding interval of I [us] (during the yield period)\n");
	fprintf(stderr, " \n");
}

static void
parse_cmdline(int argc, char *argv[])
{
	int option_index = 0;
	int c;

	while (1) {

		c = getopt_long (argc, argv, opts,
			long_options, &option_index);

		if (c == -1)
			break;

		switch(c) {
		case 'b':
			/* DB(printf(FD("B [%s]\n"), optarg)); */
			if (sscanf(optarg, "%hhu", &conf_bw) < 1) {
				fprintf(stderr, FE("Wrong BATCH workload specification\n"));
				goto exit_error;
			}
			break;
		case 'd':
			/* DB(printf(FD("D [%s]\n"), optarg)); */
			if (sscanf(optarg, "%hhu", &conf_td) < 1) {
				fprintf(stderr, FE("Wrong workload duration specification\n"));
			}
			break;
		case 'h':
			print_usage(argv[0]);
			exit (0);
			break;
		case 'i':
			/* DB(printf(FD("I [%s]\n"), optarg)); */
			if (sscanf(optarg, "%hhu", &conf_iw) < 1) {
				fprintf(stderr, FE("Wrong INTERACTIVE workload specification\n"));
				goto exit_error;
			}
			conf_iparams = optarg;
			break;
		case 'p':
			/* DB(printf(FD("P [%s]\n"), optarg)); */
			if (sscanf(optarg, "%hhu", &conf_pw) < 1) {
				fprintf(stderr, FE("Wrong PERIOD workload specification\n"));
				goto exit_error;
			}
			conf_pparams = optarg;
			break;
		case 'y':
			/* DB(printf(FD("Y [%s]\n"), optarg)); */
			if (sscanf(optarg, "%hhu", &conf_yw) < 1) {
				fprintf(stderr, FE("Wrong YIELD workload specification\n"));
				goto exit_error;
			}
			conf_yparams = optarg;
			break;
		default:
			print_usage(argv[0]);
			abort();
		}

	}

	return;

exit_error:

	print_usage(argv[0]);
	exit(-1);

}



////////////////////////////////////////////////////////////////////////////////
// Main
////////////////////////////////////////////////////////////////////////////////

static pthread_t
create_worker(struct wdata *wdata)
{
	pthread_attr_t attr;
	pthread_t childid;
	int err;

	/* thread mode */
	if (pthread_attr_init(&attr) != 0)
		barf("pthread_attr_init:");

	err = pthread_create(&childid, &attr, worker, (void*)wdata);
	if (err != 0) {
		fprintf(stderr, "pthread_create failed: %s (%d)\n",
				strerror(err), err);
		exit(-1);
	}
	return childid;


}

static uint32_t workers_count = 0;
static struct wdata *workers_data;
static pthread_t *workers;

int
main(int argc, char *argv[])
{
	struct timespec end_ts;
	char *param = NULL;
	uint8_t i, w = 0;
	uint32_t p1, p2;

	pid = gettid();

	/* Compute end test time */
	clock_gettime(CLOCK_MONOTONIC_RAW, &start_ts);
	start_us = ((float)start_ts.tv_sec * S_TO_US
		+ (float)start_ts.tv_nsec / US_TO_NS);

	parse_cmdline(argc, argv);
	printf(FI("Running for %d [s] with (B,I,P) workers: (%d,%d,%d)\n"),
			conf_td, conf_bw, conf_iw, conf_pw);

	printf(FI("Setup workers..\n"));

	/* Allocate handlers for workers */
	workers_count = conf_bw + conf_iw + conf_pw + conf_yw;
	workers = malloc(workers_count + sizeof(pthread_t));
	workers_data = malloc(workers_count * sizeof(struct wdata));

	/* Lock threads initialization */
	pthread_mutex_lock(&start_mtx);

	/* Allocate BATCH workers */
	for (i = 0; i < conf_bw; ++i) {
		workers_data[w+i].id = i+1;
		workers_data[w+i].pid = 0;
		workers_data[w+i].kind = WORKER_BATCH;

		printf(FI("wlg_B%03d: batch\n"), i+1);

		workers[w+i] = create_worker(workers_data+w+i);
	}
	w += i;

	/* Allocate INTERACTIVE workers */
	strsep(&conf_iparams, ",");
	for (i = 0; i < conf_iw; ++i) {
		workers_data[w+i].id = i+1;
		workers_data[w+i].pid = 0;
		workers_data[w+i].kind = WORKER_INTERACTIVE;

		param = strsep(&conf_iparams, ",");
		sscanf(param, "%d", &p1);
		param = strsep(&conf_iparams, ",");
		sscanf(param, "%d", &p2);

		printf(FI("wlg_I%03d: max_interval %6d [us], max_duration %6d [us]\n"),
			i+1, p1, p2);
		workers_data[w+i].params.interrupt.interval_max = p1;
		workers_data[w+i].params.interrupt.duration_max = p2;

		workers[w+i] = create_worker(workers_data+w+i);
	}
	w += i;

	/* Allocate PERIODIC workers */
	strsep(&conf_pparams, ",");
	for (i = 0; i < conf_pw; ++i) {
		workers_data[w+i].id = i+1;
		workers_data[w+i].pid = 0;
		workers_data[w+i].kind = WORKER_PERIODC;

		param = strsep(&conf_pparams, ",");
		sscanf(param, "%d", &p1);
		param = strsep(&conf_pparams, ",");
		sscanf(param, "%d", &p2);
		if (p2 > 100) {
			fprintf(stderr, FE("Wrong PERIOD workload specification (duty-cycle > 100)\n"));
			exit(-1);
		}

		printf(FI("wlg_P%03d:     interval %6d [us], duty-cycle   %6d [%%]\n"),
			i+1, p1, p2);
		workers_data[w+i].params.period.duration =   p1;
		workers_data[w+i].params.period.duty_cycle = p2;

		/* workers_data[w+i].params.period.duration = 500e3; // 500 ms */
		/* workers_data[w+i].params.period.duty_cycle = 10;  //  10 % */

		workers[w+i] = create_worker(workers_data+w+i);
	}
	w += i;

	/* Allocate YIELD workers */
	strsep(&conf_yparams, ",");
	for (i = 0; i < conf_yw; ++i) {
		workers_data[w+i].id = i+1;
		workers_data[w+i].pid = 0;
		workers_data[w+i].kind = WORKER_YIELD;

		param = strsep(&conf_yparams, ",");
		sscanf(param, "%d", &p1);
		param = strsep(&conf_yparams, ",");
		sscanf(param, "%d", &p2);
		if (p2 > p1) {
			fprintf(stderr, FE("Wrong YIELD workload specification (period > yield_interval)\n"));
			exit(-1);
		}

		printf(FI("wlg_Y%03d:     period %6d [us], yield_interval %6d [us]\n"), i+1, p1, p2);
		workers_data[w+i].params.yield.period =   p1;
		workers_data[w+i].params.yield.interval = p2;

		workers[w+i] = create_worker(workers_data+w+i);
	}
	w += i;

	/* Unlock threads initializartion */
	pthread_mutex_unlock(&start_mtx);
	usleep(1000 * w);
	DB(printf(FD("Wait for workers being ready...\n")));
	for (i = 0; i < w; ++i) {
		while (!workers_data[i].pid)
			usleep(1000);
		DB(printf(FD("%s ready!\n"), workers_data[i].name));
	}

	pthread_mutex_lock(&start_mtx);
	DB(printf(FI("Start workers...\n")));
	pthread_cond_broadcast(&start_cv);
	clock_gettime(CLOCK_MONOTONIC_RAW, &start_ts);
	pthread_mutex_unlock(&start_mtx);

	printf(FI("Wait for workers termination...\n"));
	for (i = 0; i < w; ++i) {
		pthread_join(workers[i], NULL);
		DB(printf(FD("%s joined!\n"), workers_data[i].name));
	}

	/* Compute end test time */
	clock_gettime(CLOCK_MONOTONIC_RAW, &end_ts);
	timespec_subtract(&end_ts, &start_ts);
	printf(FI("Time: %lu.%lu\n"), end_ts.tv_sec, end_ts.tv_nsec / MS_TO_NS);

	return 0;

}

