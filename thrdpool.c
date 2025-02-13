/*
  Copyright (c) 2019 Sogou, Inc.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

  Author: Xie Han (xiehan@sogou-inc.com)
*/

#include "thrdpool.h"
#include "msgqueue.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

struct __thrdpool_thrd_routine_param
{
    thrdpool_t* pool;
    size_t index;
};

typedef struct __thrdpool_thrd_routine_param routine_param;

struct __thrdpool
{
    size_t nthreads;
    size_t stacksize;
    pthread_t tid;
    pthread_mutex_t mutex;
    pthread_key_t key;
    pthread_cond_t* terminate;

    size_t nmsgques;
    msgqueue_t** msgqueues;
    routine_param* params;
};

struct __thrdpool_task_entry
{
    void* link;
    struct thrdpool_task task;
};

static pthread_t __zero_tid;

static void* __thrdpool_routine(void* arg)
{
    routine_param* param = (routine_param*)arg;
    // thrdpool_t* pool = (thrdpool_t*)arg;
    thrdpool_t* pool = param->pool;
    struct __thrdpool_task_entry* entry;
    void (*task_routine)(void*);
    void* task_context;
    pthread_t tid;
    pthread_t myid = pthread_self();

    pthread_setspecific(pool->key, pool);
    while (!pool->terminate)
    {
        entry = (struct __thrdpool_task_entry*)msgqueue_get(
            pool->msgqueues[param->index % pool->nmsgques]);
        if (!entry)
            break;

        task_routine = entry->task.routine;
        task_context = entry->task.context;
        free(entry);
        task_routine(task_context);

        if (pool->nthreads == 0)
        {
            /* Thread pool was destroyed by the task. */
            free(pool);
            return NULL;
        }
    }

    /* One thread joins another. Don't need to keep all thread IDs. */
    pthread_mutex_lock(&pool->mutex);
    tid = pool->tid;
    pool->tid = pthread_self();
    if (--pool->nthreads == 0)
        pthread_cond_signal(pool->terminate);

    pthread_mutex_unlock(&pool->mutex);
    if (memcmp(&tid, &__zero_tid, sizeof(pthread_t)) != 0)
    {
        pthread_join(tid, NULL);
    }

    return NULL;
}

static void __thrdpool_terminate(int in_pool, thrdpool_t* pool)
{
    pthread_cond_t term = PTHREAD_COND_INITIALIZER;

    pthread_mutex_lock(&pool->mutex);
    for (size_t i = 0; i < pool->nmsgques; i++)
    {
        msgqueue_set_nonblock(pool->msgqueues[i]);
    }
    pool->terminate = &term;

    if (in_pool)
    {
        /* Thread pool destroyed in a pool thread is legal. */
        pthread_detach(pthread_self());
        pool->nthreads--;
    }

    while (pool->nthreads > 0)
        pthread_cond_wait(&term, &pool->mutex);

    pthread_mutex_unlock(&pool->mutex);
    if (memcmp(&pool->tid, &__zero_tid, sizeof(pthread_t)) != 0)
        pthread_join(pool->tid, NULL);
}

static int __thrdpool_create_threads(size_t nthreads, thrdpool_t* pool)
{
    pthread_attr_t attr;
    pthread_t tid;
    int ret;

    ret = pthread_attr_init(&attr);
    if (ret == 0)
    {
        if (pool->stacksize)
            pthread_attr_setstacksize(&attr, pool->stacksize);

        pool->params = (routine_param*)malloc(sizeof(routine_param) * nthreads);
        for (size_t i = 0; i < nthreads; i++)
        {
            pool->params[i].pool = pool;
            pool->params[i].index = i;
        }

        while (pool->nthreads < nthreads)
        {
            ret = pthread_create(&tid, &attr, __thrdpool_routine,
                                 &(pool->params[pool->nthreads]));
            if (ret == 0)
                pool->nthreads++;
            else
                break;
        }

        pthread_attr_destroy(&attr);
        if (pool->nthreads == nthreads)
            return 0;

        __thrdpool_terminate(0, pool);
    }

    errno = ret;
    return -1;
}

thrdpool_t* thrdpool_create(size_t nthreads, size_t stacksize)
{
    thrdpool_t* pool;
    int ret;

    pool = (thrdpool_t*)malloc(sizeof(thrdpool_t));
    if (!pool)
        return NULL;

    pool->msgqueues = msgqueues_create(nthreads, 0, 0);
    pool->nmsgques = nthreads;

    if (pool->msgqueues)
    {
        ret = pthread_mutex_init(&pool->mutex, NULL);
        if (ret == 0)
        {
            ret = pthread_key_create(&pool->key, NULL);
            if (ret == 0)
            {
                pool->stacksize = stacksize;
                pool->nthreads = 0;
                memset(&pool->tid, 0, sizeof(pthread_t));
                pool->terminate = NULL;
                if (__thrdpool_create_threads(nthreads, pool) >= 0)
                    return pool;

                pthread_key_delete(pool->key);
            }

            pthread_mutex_destroy(&pool->mutex);
        }

        errno = ret;
        for (size_t i = 0; i < pool->nmsgques; i++)
            msgqueue_destroy(pool->msgqueues[i]);
        free(pool->msgqueues);
        pool->nmsgques = 0;
        if (pool->params)
            free(pool->params);
    }

    free(pool);
    return NULL;
}

inline void __thrdpool_schedule(const struct thrdpool_task* task, void* buf,
                                thrdpool_t* pool);

void __thrdpool_schedule(const struct thrdpool_task* task, void* buf,
                         thrdpool_t* pool)
{
    ((struct __thrdpool_task_entry*)buf)->task = *task;
    if (task->group != 0)
        msgqueue_put(buf, pool->msgqueues[task->group % pool->nmsgques]);
    else
        msgqueue_put(buf, pool->msgqueues[rand() % pool->nmsgques]);
}

int thrdpool_schedule(const struct thrdpool_task* task, thrdpool_t* pool)
{
    void* buf = malloc(sizeof(struct __thrdpool_task_entry));

    if (buf)
    {
        __thrdpool_schedule(task, buf, pool);
        return 0;
    }

    return -1;
}

/*
int thrdpool_increase(thrdpool_t* pool)
{
    pthread_attr_t attr;
    pthread_t tid;
    int ret;

    ret = pthread_attr_init(&attr);
    if (ret == 0)
    {
        if (pool->stacksize)
            pthread_attr_setstacksize(&attr, pool->stacksize);

        pthread_mutex_lock(&pool->mutex);
        ret = pthread_create(&tid, &attr, __thrdpool_routine, pool);
        if (ret == 0)
            pool->nthreads++;

        pthread_mutex_unlock(&pool->mutex);
        pthread_attr_destroy(&attr);
        if (ret == 0)
            return 0;
    }

    errno = ret;
    return -1;
}

*/
inline int thrdpool_in_pool(thrdpool_t* pool);

int thrdpool_in_pool(thrdpool_t* pool)
{
    return pthread_getspecific(pool->key) == pool;
}

void thrdpool_destroy(void (*pending)(const struct thrdpool_task*),
                      thrdpool_t* pool)
{
    int in_pool = thrdpool_in_pool(pool);
    struct __thrdpool_task_entry* entry;

    __thrdpool_terminate(in_pool, pool);
    for (size_t i = 0; i < pool->nmsgques; i++)
    {
        while (1)
        {
            entry =
                (struct __thrdpool_task_entry*)msgqueue_get(pool->msgqueues[i]);
            if (!entry)
                break;
            if (pending)
                pending(&entry->task);

            free(entry);
        }
    }

    pthread_key_delete(pool->key);
    pthread_mutex_destroy(&pool->mutex);
    for (size_t i = 0; i < pool->nmsgques; i++)
        msgqueue_destroy(pool->msgqueues[i]);
    free(pool->msgqueues);
    pool->nmsgques = 0;
    if (pool->params)
        free(pool->params);
    if (!in_pool)
        free(pool);
}
