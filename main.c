#include "hadoop_group_info.h"
#include "hadoop_user_info.h"

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct options {
  int use_reentrant;
  int verbose;
  int num_threads;
};

static struct options g_options;

static gid_t *g_group_ids;

static int g_num_group_ids;

static pthread_mutex_t g_strerror_lock = PTHREAD_MUTEX_INITIALIZER;

static void usage(void)
{
  fprintf(stderr, "unix-groups-reentrancy-test\n\n");
  fprintf(stderr, "options:\n");
  fprintf(stderr, 
    "-h:             this help message.\n");
  fprintf(stderr, 
    "-n:             use the non-reentrant versions of the functions\n");
  fprintf(stderr, 
    "-v:             be verbose\n");
}

static int find_all_gids(void)
{
  uint32_t g;
  gid_t *n_group_ids;
  struct hadoop_group_info *ginfo = NULL;
  int ret;

  ginfo = hadoop_group_info_alloc(g_options.use_reentrant);
  if (!ginfo) {
    fprintf(stderr, "hadoop_group_info_alloc failed\n");
    ret = ENOMEM;
    goto done;
  }
  for (g = 0; g <= 65535; g++) {
    ret = hadoop_group_info_fetch(ginfo, g);
    if (ret != 0)
      continue;
    n_group_ids = realloc(g_group_ids,
        sizeof(gid_t) * (g_num_group_ids + 1));
    if (!n_group_ids) {
      fprintf(stderr, "failed to reallocate an array of size %d\n",
              g_num_group_ids + 1);
      ret = ENOMEM;
      goto done;
    }
    g_group_ids = n_group_ids;
    g_group_ids[g_num_group_ids++] = g;
  }
  ret = 0;
done:
  if (ginfo) {
    hadoop_group_info_free(ginfo);
  }
  return ret;
}

static void *test_gids(void *v)
{
  int i, iter = 0;
  struct hadoop_group_info *ginfo = NULL;
  int ret = 0;

  ginfo = hadoop_group_info_alloc(g_options.use_reentrant);
  if (!ginfo) {
    fprintf(stderr, "hadoop_group_info_alloc failed.  v=%p\n", v);
    ret = ENOMEM;
    goto done;
  }
  while (1) {
    for (i = 0; i <= g_num_group_ids; i++) {
      ret = hadoop_group_info_fetch(ginfo, g_group_ids[i]);
      if (ret != 0) {
        pthread_mutex_lock(&g_strerror_lock);
        fprintf(stderr, "hadoop_group_info_fetch: error %d (%s)\n",
                ret, strerror(ret));
        pthread_mutex_unlock(&g_strerror_lock);
        break;
      }
      iter++;
      if (iter == 1000) {
        fprintf(stderr, ".");
        iter = 0;
      }
    }
  }
done:
  if (ginfo) {
    hadoop_group_info_free(ginfo);
  }
  return (void*)(uintptr_t)ret;
}

static void get_options(struct options *opts, int argc, char **argv)
{
  int c;

  memset(opts, sizeof(*opts), 0);
  opts->use_reentrant = 1;
  opts->num_threads = 10;

  while ((c = getopt (argc, argv, "hnt:v")) != -1) {
    switch (c) {
    case 'h':
      usage();
      exit(0);
      break;
    case 'n':
      opts->use_reentrant = 0;
      break;
    case 't':
      opts->num_threads = atoi(optarg);
      if (opts->num_threads <= 0) {
        fprintf(stderr, "error: invalid number of threads: %d\n\n", 
                opts->num_threads);
        usage();
        exit(EXIT_FAILURE);
      }
    case 'v':
      opts->verbose = 1;
      break;
    case '?':
      usage();
      exit(1);
      break;
    }
  }
}

typedef void* (*test_func_t)(void *);

static void run_test(pthread_t **threads, int nthreads, test_func_t fun)
{
  int ret, i;
  pthread_t *m_threads;

  m_threads = calloc(nthreads, sizeof(pthread_t));
  if (!m_threads) {
    fprintf(stderr, "failed to allocate an array for threads\n");
    exit(EXIT_FAILURE);
  }
  *threads = m_threads;

  for (i = 0; i < nthreads; i++) {
    ret = pthread_create(&m_threads[i], NULL, fun, NULL);
    if (ret) {
      pthread_mutex_lock(&g_strerror_lock);
      fprintf(stderr, "pthread_create error: %d (%s)\n",
              ret, strerror(ret));
      pthread_mutex_unlock(&g_strerror_lock);
      exit(EXIT_FAILURE);
    }
  }
}

static void join_threads(pthread_t *threads, int nthreads)
{
  void *retval;
  int ret, i;

  for (i = 0; i < nthreads; i++) {
    retval = 0;
    ret = pthread_join(threads[i], &retval);
    if (ret) {
      pthread_mutex_lock(&g_strerror_lock);
      fprintf(stderr, "pthread_join error on thread %d: %d (%s)\n",
              i, ret, strerror(ret));
      pthread_mutex_unlock(&g_strerror_lock);
      exit(EXIT_FAILURE);
    }
    if (retval != NULL) {
      pthread_mutex_lock(&g_strerror_lock);
      ret = (int)(uintptr_t)retval;
      fprintf(stderr, "pthread %d returned error: %d (%s)\n",
              i, ret, strerror(ret));
      pthread_mutex_unlock(&g_strerror_lock);
      exit(EXIT_FAILURE);
    }
  }
  free(threads);
}

int main(int argc, char **argv)
{
  int i, ret;
  pthread_t *threads;

  get_options(&g_options, argc, argv);
  fprintf(stderr, "locating all groups on system...\n");
  ret = find_all_gids();
  if (ret) {
    fprintf(stderr, "failed to find all gids: error %d (%s)\n",
            ret, strerror(ret));
    exit(EXIT_FAILURE);
  }
  if (g_options.verbose) {
    const char *prefix = "";
    fprintf(stderr, "found group IDs: ");
    for (i = 0; i < g_num_group_ids; i++) {
      fprintf(stderr, "%s%" PRId64, prefix, (uint64_t)g_group_ids[i]);
      prefix = ", ";
    }
    fprintf(stderr, "\n");
  }
  fprintf(stderr, "starting test threads...\n");
  run_test(&threads, g_options.num_threads, test_gids);

  join_threads(threads, g_options.num_threads);

  return 0;
}
