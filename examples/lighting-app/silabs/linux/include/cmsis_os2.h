/*
 *
 *    Copyright (c) 2026 Project CHIP Authors
 *    All rights reserved.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// SIMULATION-DISCLOSURE: POSIX adapter for CMSIS-RTOS2 primitives used by
// examples/lighting-app/silabs/src/AppTask.cpp when hosted on Linux/Cirque.

typedef int32_t osStatus_t;

enum
{
    osOK             = 0,
    osError          = -1,
    osErrorTimeout   = -2,
    osErrorResource  = -3,
    osErrorParameter = -4,
};

#define osWaitForever 0xFFFFFFFFU
#define pdMS_TO_TICKS(ms) ((uint32_t) (ms))

typedef void * osTimerId_t;
typedef void * osMessageQueueId_t;
typedef void * osThreadId_t;
typedef void * osSemaphoreId_t;

typedef void (*osTimerFunc_t)(void * argument);
typedef void (*osThreadFunc_t)(void * argument);

typedef enum
{
    osTimerOnce     = 0,
    osTimerPeriodic = 1,
} osTimerType_t;

typedef struct
{
    const char * name;
    uint32_t attr_bits;
    void * cb_mem;
    uint32_t cb_size;
} osTimerAttr_t;

typedef struct
{
    const char * name;
    uint32_t attr_bits;
    void * cb_mem;
    uint32_t cb_size;
    void * mq_mem;
    uint32_t mq_size;
} osMessageQueueAttr_t;

typedef struct
{
    const char * name;
    uint32_t attr_bits;
    void * cb_mem;
    uint32_t cb_size;
    void * stack_mem;
    uint32_t stack_size;
    int32_t priority;
} osThreadAttr_t;

osTimerId_t osTimerNew(osTimerFunc_t func, osTimerType_t type, void * argument, const osTimerAttr_t * attr);
osStatus_t osTimerStart(osTimerId_t timer_id, uint32_t ticks);
osStatus_t osTimerStop(osTimerId_t timer_id);
uint32_t osTimerIsRunning(osTimerId_t timer_id);
osStatus_t osTimerDelete(osTimerId_t timer_id);

osMessageQueueId_t osMessageQueueNew(uint32_t msg_count, uint32_t msg_size, const osMessageQueueAttr_t * attr);
osStatus_t osMessageQueuePut(osMessageQueueId_t mq_id, const void * msg_ptr, uint8_t msg_prio, uint32_t timeout);
osStatus_t osMessageQueueGet(osMessageQueueId_t mq_id, void * msg_ptr, uint8_t * msg_prio, uint32_t timeout);
osStatus_t osMessageQueueDelete(osMessageQueueId_t mq_id);

osThreadId_t osThreadNew(osThreadFunc_t func, void * argument, const osThreadAttr_t * attr);

#ifdef __cplusplus
}
#endif
