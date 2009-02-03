/*
 *  Copyright (C) 2004 Trog <trog@clamav.net>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 *  MA 02110-1301, USA.
 */

#if HAVE_CONFIG_H
#include "clamav-config.h"
#endif

#include <stdio.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>

#include "shared/output.h"

#include "thrmgr.h"
#include "others.h"
#include "mpool.h"

#if defined(C_LINUX)
#include <malloc.h>
#endif

#define FALSE (0)
#define TRUE (1)

/* BSD and HP-UX need a bigger stacksize than the system default */
#if defined (C_BSD) || defined (C_HPUX)
#define C_BIGSTACK 1
#endif

static work_queue_t *work_queue_new(void)
{
	work_queue_t *work_q;

	work_q = (work_queue_t *) malloc(sizeof(work_queue_t));
	if (!work_q) {
		return NULL;
	}

	work_q->head = work_q->tail = NULL;
	work_q->item_count = 0;
	return work_q;
}

static int work_queue_add(work_queue_t *work_q, void *data)
{
	work_item_t *work_item;

	if (!work_q) {
		return FALSE;
	}
	work_item = (work_item_t *) malloc(sizeof(work_item_t));
	if (!work_item) {
		return FALSE;
	}

	work_item->next = NULL;
	work_item->data = data;
	gettimeofday(&(work_item->time_queued), NULL);

	if (work_q->head == NULL) {
		work_q->head = work_q->tail = work_item;
		work_q->item_count = 1;
	} else {
		work_q->tail->next = work_item;
		work_q->tail = work_item;
		work_q->item_count++;
	}
	return TRUE;
}

static void *work_queue_pop(work_queue_t *work_q)
{
	work_item_t *work_item;
	void *data;

	if (!work_q || !work_q->head) {
		return NULL;
	}
	work_item = work_q->head;
	data = work_item->data;
	work_q->head = work_item->next;
	if (work_q->head == NULL) {
		work_q->tail = NULL;
	}
	free(work_item);
	work_q->item_count--;
	return data;
}

static struct threadpool_list {
	threadpool_t *pool;
	struct threadpool_list *nxt;
} *pools = NULL;
static pthread_mutex_t pools_lock = PTHREAD_MUTEX_INITIALIZER;

static void add_topools(threadpool_t *t)
{
	struct threadpool_list *new = malloc(sizeof(*new));
	if(!new) {
		logg("!Unable to add threadpool to list\n");
		return;
	}
	new->pool = t;
	pthread_mutex_lock(&pools_lock);
	new->nxt = pools;
	pools = new;
	pthread_mutex_unlock(&pools_lock);
}

static void remove_frompools(threadpool_t *t)
{
	struct threadpool_list *l, *prev;
	struct task_desc *desc;
	pthread_mutex_lock(&pools_lock);
	prev = NULL;
	l = pools;
	while(l && l->pool != t) {
		prev = l;
		l = l->nxt;
	}
	if(!l)
		return;
	if(prev)
		prev->nxt = l->nxt;
	if(l == pools)
		pools = l->nxt;
	free(l);
	desc = t->tasks;
	while(desc) {
		struct task_desc *q = desc;
		desc = desc->nxt;
		free(q);
	}
	t->tasks = NULL;
	pthread_mutex_unlock(&pools_lock);
}

int thrmgr_printstats(int f)
{
	struct threadpool_list *l;
	unsigned cnt, pool_cnt = 0;
	size_t pool_used = 0, pool_total = 0, seen_cnt = 0, error_flag = 0;
	float mem_heap = 0, mem_mmap = 0, mem_used = 0, mem_free = 0, mem_releasable = 0;
	const struct cl_engine **seen = NULL;

	pthread_mutex_lock(&pools_lock);
	for(cnt=0,l=pools;l;l=l->nxt) cnt++;
	mdprintf(f,"POOLS: %u\n\n", cnt);
	for(l= pools;l && !error_flag;l = l->nxt) {
		threadpool_t *pool = l->pool;
		const char *state;
		work_item_t *q;
		struct timeval tv_now;
		long umin=~0UL, umax=0, usum=0;
		int invalids=0;
		struct task_desc *task;
		cnt = 0;

		if(!pool) {
			mdprintf(f,"NULL\n\n");
			continue;
		}
		pthread_mutex_lock(&pool->pool_mutex);
		/* now we can access desc->, knowing that they won't get freed
		 * because the other tasks can't quit while pool_mutex is taken
		 */
		switch(pool->state) {
			case POOL_INVALID:
				state = "INVALID";
				break;
			case POOL_VALID:
				state = "VALID";
				break;
			case POOL_EXIT:
				state = "EXIT";
				break;
			default:
				state = "??";
				break;
		}
		mdprintf(f, "STATE: %s %s\n", state, l->nxt ? "" : "PRIMARY");
		mdprintf(f, "THREADS: live %u  idle %u max %u idle-timeout %u\n"
				,pool->thr_alive, pool->thr_idle, pool->thr_max,
				pool->idle_timeout);
		mdprintf(f,"QUEUE: %u items", pool->queue->item_count);
		gettimeofday(&tv_now, NULL);
		if(pool->queue->head) {
			for(q=pool->queue->head;q;q=q->next) {
				long delta;
				delta = tv_now.tv_usec - q->time_queued.tv_usec;
				delta += (tv_now.tv_sec - q->time_queued.tv_sec)*1000000;
				if(delta < 0) {
					invalids++;
					continue;
				}
				if(delta > umax)
					umax = delta;
				if(delta < umin)
					umin = delta;
				usum += delta;
				++cnt;
			}
			mdprintf(f," min_wait: %.6f max_wait: %.6f avg_wait: %.6f",
					umin/1e6, umax/1e6, usum /(1e6*cnt));
			if(invalids)
				mdprintf(f," (INVALID timestamps: %u)", invalids);
		}
		if(cnt + invalids != (unsigned)pool->queue->item_count)
			mdprintf(f," (ERROR: %u != %u)", cnt + invalids,
					(unsigned)pool->queue->item_count);
		mdprintf(f, "\n");
		for(task = pool->tasks; task; task = task->nxt) {
			long delta;
			size_t used, total;

			delta = tv_now.tv_usec - task->tv.tv_usec;
			delta += (tv_now.tv_sec - task->tv.tv_sec)*1000000;
			mdprintf(f,"\t%s %f %s\n",
					task->command ? task->command : "N/A",
					delta/1e6,
					task->filename ? task->filename:"");
			if (task->engine) {
				/* we usually have at most 2 engines so a linear
				 * search is good enough */
				size_t i;
				for (i=0;i<seen_cnt;i++) {
					if (seen[i] == task->engine)
						break;
				}
				/* we need to count the memusage from the same
				 * engine only once */
				if (i == seen_cnt) {
					const struct cl_engine **s;
					/* new engine */
					++seen_cnt;
					s = realloc(seen, seen_cnt * sizeof(*seen));
					if (!s) {
						error_flag = 1;
						break;
					}
					seen = s;
					seen[seen_cnt - 1] = task->engine;

					if (mp_getstats(task->engine, &used, &total) != -1) {
						pool_used += used;
						pool_total += total;
						pool_cnt++;
					}
				}
			}
		}
		mdprintf(f,"\n");
		pthread_mutex_unlock(&pool->pool_mutex);
	}
	free(seen);
#if defined(C_LINUX)
	{
		struct mallinfo inf = mallinfo();
		mem_heap = inf.arena/(1024*1024.0);
		mem_mmap = inf.hblkhd/(1024*1024.0);
		mem_used = inf.uordblks/(1024*1024.0);
		mem_free = inf.fordblks/(1024*1024.0);
		mem_releasable = inf.keepcost/(1024*1024.0);
		/* TODO: figure out how to print these statistics on other OSes */
	}
#endif
	if (error_flag) {
		mdprintf(f, "ERROR: error encountered while formatting statistics\n");
	} else {
		mdprintf(f,"MEMSTATS: heap %.3fM mmap %.3fM used %.3fM free %.3fM releasable %.3fM pools %u pools_used %.3fM pools_total %.3fM\n",
			mem_heap, mem_mmap, mem_used, mem_free, mem_releasable, pool_cnt,
			pool_used/(1024*1024.0), pool_total/(1024*1024.0));
	}
	mdprintf(f,"END\n");
	pthread_mutex_unlock(&pools_lock);
	return 0;
}

void thrmgr_destroy(threadpool_t *threadpool)
{
	if (!threadpool) {
		return;
	}
	if (pthread_mutex_lock(&threadpool->pool_mutex) != 0) {
		logg("!Mutex lock failed\n");
		exit(-1);
	}
	if(threadpool->state != POOL_VALID) {
		if (pthread_mutex_unlock(&threadpool->pool_mutex) != 0) {
			logg("!Mutex unlock failed\n");
			exit(-1);
		}
		return;
	}
	threadpool->state = POOL_EXIT;

	/* wait for threads to exit */
	if (threadpool->thr_alive > 0) {
		if (pthread_cond_broadcast(&(threadpool->pool_cond)) != 0) {
			pthread_mutex_unlock(&threadpool->pool_mutex);
			return;
		}
	}
	while (threadpool->thr_alive > 0) {
		if (pthread_cond_wait (&threadpool->pool_cond, &threadpool->pool_mutex) != 0) {
			pthread_mutex_unlock(&threadpool->pool_mutex);
			return;
		}
	}
	remove_frompools(threadpool);
	if (pthread_mutex_unlock(&threadpool->pool_mutex) != 0) {
		logg("!Mutex unlock failed\n");
		exit(-1);
	}

	pthread_mutex_destroy(&(threadpool->pool_mutex));
	pthread_cond_destroy(&(threadpool)->idle_cond);
	pthread_cond_destroy(&(threadpool->pool_cond));
	pthread_attr_destroy(&(threadpool->pool_attr));
	free(threadpool->queue);
	free(threadpool);
	return;
}

threadpool_t *thrmgr_new(int max_threads, int idle_timeout, void (*handler)(void *))
{
	threadpool_t *threadpool;
#if defined(C_BIGSTACK)
	size_t stacksize;
#endif

	if (max_threads <= 0) {
		return NULL;
	}

	threadpool = (threadpool_t *) malloc(sizeof(threadpool_t));
	if (!threadpool) {
		return NULL;
	}

	threadpool->queue = work_queue_new();
	if (!threadpool->queue) {
		free(threadpool);
		return NULL;
	}
	threadpool->thr_max = max_threads;
	threadpool->thr_alive = 0;
	threadpool->thr_idle = 0;
	threadpool->idle_timeout = idle_timeout;
	threadpool->handler = handler;
	threadpool->tasks = NULL;

	if(pthread_mutex_init(&(threadpool->pool_mutex), NULL)) {
		free(threadpool->queue);
		free(threadpool);
		return NULL;
	}

	if (pthread_cond_init(&(threadpool->pool_cond), NULL) != 0) {
		pthread_mutex_destroy(&(threadpool->pool_mutex));
		free(threadpool->queue);
		free(threadpool);
		return NULL;
	}

	if (pthread_cond_init(&(threadpool->idle_cond),NULL) != 0)  {
		pthread_cond_destroy(&(threadpool->pool_cond));
		pthread_mutex_destroy(&(threadpool->pool_mutex));
		free(threadpool->queue);
		free(threadpool);
		return NULL;
	}

	if (pthread_attr_init(&(threadpool->pool_attr)) != 0) {
		pthread_cond_destroy(&(threadpool->idle_cond));
		pthread_cond_destroy(&(threadpool->pool_cond));
		pthread_mutex_destroy(&(threadpool->pool_mutex));
		free(threadpool->queue);
		free(threadpool);
		return NULL;
	}

	if (pthread_attr_setdetachstate(&(threadpool->pool_attr), PTHREAD_CREATE_DETACHED) != 0) {
		pthread_attr_destroy(&(threadpool->pool_attr));
		pthread_cond_destroy(&(threadpool->idle_cond));
		pthread_cond_destroy(&(threadpool->pool_cond));
		pthread_mutex_destroy(&(threadpool->pool_mutex));
		free(threadpool->queue);
		free(threadpool);
		return NULL;
	}

#if defined(C_BIGSTACK)
	pthread_attr_getstacksize(&(threadpool->pool_attr), &stacksize);
	stacksize = stacksize + 64 * 1024;
	if (stacksize < 1048576) stacksize = 1048576; /* at least 1MB please */
	logg("Set stacksize to %u\n", stacksize);
	pthread_attr_setstacksize(&(threadpool->pool_attr), stacksize);
#endif
	threadpool->state = POOL_VALID;

	add_topools(threadpool);
	return threadpool;
}

static pthread_key_t stats_tls_key;
static pthread_once_t stats_tls_key_once = PTHREAD_ONCE_INIT;

static void stats_tls_key_alloc(void)
{
	pthread_key_create(&stats_tls_key, NULL);
}

static const char *IDLE_TASK = "IDLE";

/* no mutex is needed, we are using  thread local variable */
void thrmgr_setactivetask(const char *filename, const char* command)
{
	struct task_desc *desc;
	pthread_once(&stats_tls_key_once, stats_tls_key_alloc);
	desc = pthread_getspecific(stats_tls_key);
	if(!desc)
		return;
	desc->filename = filename;
	if(command) {
		if(command == IDLE_TASK && desc->command == command)
			return;
		desc->command = command;
		gettimeofday(&desc->tv, NULL);
	}
}

void thrmgr_setactiveengine(const struct cl_engine *engine)
{
	struct task_desc *desc;
	pthread_once(&stats_tls_key_once, stats_tls_key_alloc);
	desc = pthread_getspecific(stats_tls_key);
	if(!desc)
		return;
	desc->engine = engine;
}

/* thread pool mutex must be held on entry */
static void stats_init(threadpool_t *pool)
{
	struct task_desc *desc = calloc(1, sizeof(*desc));
	if(!desc)
		return;
	pthread_once(&stats_tls_key_once, stats_tls_key_alloc);
	pthread_setspecific(stats_tls_key, desc);
	if(!pool->tasks)
		pool->tasks = desc;
	else {
		desc->nxt = pool->tasks;
		pool->tasks->prv = desc;
		pool->tasks = desc;
	}
}

/* thread pool mutex must be held on entry */
static void stats_destroy(threadpool_t *pool)
{
	struct task_desc *desc = pthread_getspecific(stats_tls_key);
	if(!desc)
		return;
	if(desc->prv)
		desc->prv->nxt = desc->nxt;
	if(desc->nxt)
		desc->nxt->prv = desc->prv;
	if(pool->tasks == desc)
		pool->tasks = desc->nxt;
	free(desc);
	pthread_setspecific(stats_tls_key, NULL);
}

static void *thrmgr_worker(void *arg)
{
	threadpool_t *threadpool = (threadpool_t *) arg;
	void *job_data;
	int retval, must_exit = FALSE, stats_inited = FALSE;
	struct timespec timeout;

	/* loop looking for work */
	for (;;) {
		if (pthread_mutex_lock(&(threadpool->pool_mutex)) != 0) {
			logg("!Fatal: mutex lock failed\n");
			exit(-2);
		}
		if(!stats_inited) {
			stats_init(threadpool);
			stats_inited = TRUE;
		}
		thrmgr_setactiveengine(NULL);
		thrmgr_setactivetask(NULL, IDLE_TASK);
		timeout.tv_sec = time(NULL) + threadpool->idle_timeout;
		timeout.tv_nsec = 0;
		threadpool->thr_idle++;
		while (((job_data=work_queue_pop(threadpool->queue)) == NULL)
				&& (threadpool->state != POOL_EXIT)) {
			/* Sleep, awaiting wakeup */
			pthread_cond_signal(&threadpool->idle_cond);
			retval = pthread_cond_timedwait(&(threadpool->pool_cond),
				&(threadpool->pool_mutex), &timeout);
			if (retval == ETIMEDOUT) {
				must_exit = TRUE;
				break;
			}
		}
		threadpool->thr_idle--;
		if (threadpool->state == POOL_EXIT) {
			must_exit = TRUE;
		}

		if (pthread_mutex_unlock(&(threadpool->pool_mutex)) != 0) {
			logg("!Fatal: mutex unlock failed\n");
			exit(-2);
		}
		if (job_data) {
			threadpool->handler(job_data);
		} else if (must_exit) {
			break;
		}
	}
	if (pthread_mutex_lock(&(threadpool->pool_mutex)) != 0) {
		/* Fatal error */
		logg("!Fatal: mutex lock failed\n");
		exit(-2);
	}
	threadpool->thr_alive--;
	if (threadpool->thr_alive == 0) {
		/* signal that all threads are finished */
		pthread_cond_broadcast(&threadpool->pool_cond);
	}
	stats_destroy(threadpool);
	if (pthread_mutex_unlock(&(threadpool->pool_mutex)) != 0) {
		/* Fatal error */
		logg("!Fatal: mutex unlock failed\n");
		exit(-2);
	}
	return NULL;
}

int thrmgr_dispatch(threadpool_t *threadpool, void *user_data)
{
	pthread_t thr_id;

	if (!threadpool) {
		return FALSE;
	}

	/* Lock the threadpool */
	if (pthread_mutex_lock(&(threadpool->pool_mutex)) != 0) {
		logg("!Mutex lock failed\n");
		return FALSE;
	}

	if (threadpool->state != POOL_VALID) {
		if (pthread_mutex_unlock(&(threadpool->pool_mutex)) != 0) {
			logg("!Mutex unlock failed\n");
		}
		return FALSE;
	}
	if (!work_queue_add(threadpool->queue, user_data)) {
		if (pthread_mutex_unlock(&(threadpool->pool_mutex)) != 0) {
			logg("!Mutex unlock failed\n");
			return FALSE;
		}
		return FALSE;
	}

	if ((threadpool->thr_idle < threadpool->queue->item_count) &&
			(threadpool->thr_alive < threadpool->thr_max)) {
		/* Start a new thread */
		if (pthread_create(&thr_id, &(threadpool->pool_attr),
				thrmgr_worker, threadpool) != 0) {
			logg("!pthread_create failed\n");
		} else {
			threadpool->thr_alive++;
		}
	}
	pthread_cond_signal(&(threadpool->pool_cond));

	if (pthread_mutex_unlock(&(threadpool->pool_mutex)) != 0) {
		logg("!Mutex unlock failed\n");
		return FALSE;
	}
	return TRUE;
}

int thrmgr_group_dispatch(threadpool_t *threadpool, jobgroup_t *group, void *user_data)
{
    int ret;
    pthread_mutex_lock(&group->mutex);
    group->jobs++;
    if (!(ret = thrmgr_dispatch(threadpool, user_data))) {
	group->jobs--;
    }
    pthread_mutex_unlock(&group->mutex);
    return ret;
}

void thrmgr_group_finished(jobgroup_t *group, enum thrmgr_exit exitc)
{
    pthread_mutex_lock(&group->mutex);
    group->exit_total++;
    switch (exitc) {
	case EXIT_OK:
	    group->exit_ok++;
	    break;
	case EXIT_ERROR:
	    group->exit_error++;
	    break;
    }
    if (group->jobs) {
	if (!--group->jobs)
	    pthread_cond_signal(&group->empty);
    }
    pthread_mutex_unlock(&group->mutex);
}

void thrmgr_group_waitforall(jobgroup_t *group, unsigned *ok, unsigned *error, unsigned *total)
{
    pthread_mutex_lock(&group->mutex);
    /* TODO: should check progexit here */
    while (group->jobs) {
	pthread_cond_wait(&group->empty, &group->mutex);
    }
    *ok = group->exit_ok;
    *error = group->exit_error;
    *total = group->exit_total;
    pthread_mutex_unlock(&group->mutex);
}

jobgroup_t *thrmgr_group_new(void)
{
    jobgroup_t dummy = JOBGROUP_INITIALIZER;
    jobgroup_t *group = malloc(sizeof(*group));
    if (!group)
	return NULL;
    memcpy(group, &dummy, sizeof(dummy));
    return group;
}
