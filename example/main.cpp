#include "thrdpool.h"
#include <cstdio>
#include <iostream>
#include <stdlib.h>
#include <thread>

void my_routine(void* context) // 我们要执行的函数
{
    auto index = reinterpret_cast<unsigned long long>(context);
    auto thr_id = std::this_thread::get_id();
    std::cout << "task-" << index << " start in " << thr_id << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(10));
}

void my_pending(
    const struct thrdpool_task* task) // 线程池销毁后，没执行的任务会到这里
{
    auto index = reinterpret_cast<unsigned long long>(task->context);
    printf("pending task-%llu.\n", index);
}

int main()
{
    thrdpool_t* thrd_pool = thrdpool_create(3, 1024); // 创建
    struct thrdpool_task task;
    unsigned long long i;

    for (i = 0; i < 10; i++)
    {
        task.routine = &my_routine;
        task.context = reinterpret_cast<void*>(i);
        task.group = i;
        thrdpool_schedule(&task, thrd_pool); // 调用
    }
    getchar(); // 卡住主线程，按回车继续
    thrdpool_destroy(&my_pending, thrd_pool); // 结束
    return 0;
}