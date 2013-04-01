/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "hadoop_group_info.h"

#include <errno.h>
#include <grp.h>
#include <pthread.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct hadoop_group_info *hadoop_group_info_alloc(int use_reentrant)
{
  struct hadoop_group_info *ginfo;
  size_t buf_sz;
  char *buf;

  ginfo = calloc(1, sizeof(struct hadoop_group_info));
  if (!use_reentrant) {
    ginfo->buf_sz = 0;
    ginfo->buf = NULL;
  } else {
    buf_sz = sysconf(_SC_GETGR_R_SIZE_MAX);
    if (buf_sz < 1024) {
      buf_sz = 1024;
    }
    buf = malloc(buf_sz);
    if (!buf) {
      free(ginfo);
      return NULL;
    }
    ginfo->buf_sz = buf_sz;
    ginfo->buf = buf;
  }
  return ginfo;
}

void hadoop_group_info_clear(struct hadoop_group_info *ginfo)
{
  struct group *group = &ginfo->group;
  char **g;

  if (!ginfo->buf_sz) {
    free(group->gr_name);
    free(group->gr_passwd);
    if (group->gr_mem) {
      for (g = group->gr_mem; *g; g++) {
        free(*g);
      }
      free(group->gr_mem);
    }
  }
  group->gr_name = NULL;
  group->gr_passwd = NULL;
  group->gr_gid = 0;
  group->gr_mem = NULL;
}

void hadoop_group_info_free(struct hadoop_group_info *ginfo)
{
  if (!ginfo->buf_sz) {
    hadoop_group_info_clear(ginfo);
  } else {
    free(ginfo->buf);
  }
  free(ginfo);
}

/**
 * Different platforms use different error codes to represent "group not found."
 * So whitelist the errors which do _not_ mean "group not found."
 *
 * @param err           The errno
 *
 * @return              The error code to use
 */
static int getgrgid_error_translate(int err)
{
  if ((err == EIO) || (err == EMFILE) || (err == ENFILE) ||
      (err == ENOMEM) || (err == ERANGE)) {
    return err;
  }
  return ENOENT;
}

static int hadoop_group_info_fetch_nonreentrant(
      struct hadoop_group_info *ginfo, gid_t gid)
{
  struct group *group;
  int i, num_users = 0;
  char **g;

  do {
    errno = 0;
    group = getgrgid(gid);
  } while ((!group) && (errno == EINTR));
  if (!group) {
    return getgrgid_error_translate(errno);
  }
  ginfo->group.gr_name = strdup(group->gr_name);
  if (!ginfo->group.gr_name) {
    hadoop_group_info_clear(ginfo);
    return ENOMEM;
  }
  ginfo->group.gr_passwd = strdup(group->gr_passwd);
  if (!ginfo->group.gr_passwd) {
    hadoop_group_info_clear(ginfo);
    return ENOMEM;
  }
  ginfo->group.gr_gid = group->gr_gid;
  if (group->gr_mem) {
    for (g = group->gr_mem, num_users = 0;
         *g; g++) {
      num_users++;
    }
  }
  ginfo->group.gr_mem = calloc(num_users + 1, sizeof(char *));
  if (!ginfo->group.gr_mem) {
    hadoop_group_info_clear(ginfo);
    return ENOMEM;
  }
  for (i = 0; i < num_users; i++) {
    ginfo->group.gr_mem[i] = strdup(group->gr_mem[i]);
    if (!ginfo->group.gr_mem[i]) {
      hadoop_group_info_clear(ginfo);
      return ENOMEM;
    }
  }
  return 0;
}

static int hadoop_group_info_fetch_reentrant(struct hadoop_group_info *ginfo,
                                             gid_t gid)
{
  struct group *group;
  int err;
  size_t buf_sz;
  char *nbuf;

  for (;;) {
    do {
      group = NULL;
      err = getgrgid_r(gid, &ginfo->group, ginfo->buf,
                         ginfo->buf_sz, &group);
    } while ((!group) && (err == EINTR));
    if (group) {
      return 0;
    }
    if (err != ERANGE) {
      return getgrgid_error_translate(errno);
    }
    buf_sz = ginfo->buf_sz * 2;
    nbuf = realloc(ginfo->buf, buf_sz);
    if (!nbuf) {
      return ENOMEM;
    }
    ginfo->buf = nbuf;
    ginfo->buf_sz = buf_sz;
  }
}

int hadoop_group_info_fetch(struct hadoop_group_info *ginfo, gid_t gid)
{
  int ret;
  hadoop_group_info_clear(ginfo);

  if (!ginfo->buf_sz) {
    static pthread_mutex_t g_getgrnam_lock = PTHREAD_MUTEX_INITIALIZER;

    pthread_mutex_lock(&g_getgrnam_lock);
    ret = hadoop_group_info_fetch_nonreentrant(ginfo, gid);
    pthread_mutex_unlock(&g_getgrnam_lock);
    return ret;
  } else {
    return hadoop_group_info_fetch_reentrant(ginfo, gid);
  }
}

#undef TESTING

#ifdef TESTING
/**
 * A main() is provided so that quick testing of this
 * library can be done. 
 */
int main(int argc, char **argv) {
  char **groupname;
  struct hadoop_group_info *ginfo;
  int ret, reentrant  = !getenv("NONREENTRANT");
  
  if (!reentrant) {
    fprintf(stderr, "testing non-reentrant...\n");
  }
  ginfo = hadoop_group_info_alloc(reentrant);
  if (!ginfo) {
    fprintf(stderr, "hadoop_group_info_alloc returned NULL.\n");
    return EXIT_FAILURE;
  }
  for (groupname = argv + 1; *groupname; groupname++) {
    gid_t gid = atoi(*groupname);
    if (gid == 0) {
      fprintf(stderr, "won't accept non-parseable group-name or gid 0: %s\n",
              *groupname);
      return EXIT_FAILURE;
    }
    ret = hadoop_group_info_fetch(ginfo, gid);
    if (!ret) {
      fprintf(stderr, "gid[%lld] : gr_name = %s\n",
              (long long)gid, ginfo->group.gr_name);
    } else {
      fprintf(stderr, "group[%lld] : error %d (%s)\n",
              (long long)gid, ret, strerror(ret));
    }
  }
  hadoop_group_info_free(ginfo);
  return EXIT_SUCCESS;
}
#endif
