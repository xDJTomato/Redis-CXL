/* zmalloc - total amount of allocated memory aware version of malloc()
 * 使用libnuma优化的NUMA感知内存分配器
 *
 * Copyright (c) 2009-2010, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEPTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

// ============================================================================
// 头文件包含
// ============================================================================
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <numa.h>
#include <numaif.h>
#include <sched.h>
#include <limits.h>
#include "config.h"

// ============================================================================
// 宏定义和常量
// ============================================================================
#if defined(__sun)
    #define PREFIX_SIZE sizeof(long long)
#else
    #define PREFIX_SIZE sizeof(size_t)
#endif

// ============================================================================
// 全局变量声明
// ============================================================================

// 内存分配统计
static size_t used_memory = 0;
static int zmalloc_thread_safe = 0;
pthread_mutex_t used_memory_mutex = PTHREAD_MUTEX_INITIALIZER;

// NUMA相关配置
static int numa_support_available = 0;   // libnuma是否可用
static int default_numa_node = -1;       // 默认NUMA节点(-1表示自动选择)
static int numa_initialized = 0;         // NUMA库是否已初始化

// NUMA策略配置
static int *sorted_nodes_by_distance = NULL;  // 按距离排序的节点数组
static int num_available_nodes = 0;           // 可用节点数量

// ============================================================================
// 宏定义
// ============================================================================

// 内存操作宏定义（线程安全）
#define increment_used_memory(_n) do { \
    if (zmalloc_thread_safe) { \
        pthread_mutex_lock(&used_memory_mutex); \
        used_memory += _n; \
        pthread_mutex_unlock(&used_memory_mutex); \
    } else { \
        used_memory += _n; \
    } \
} while(0)

#define decrement_used_memory(_n) do { \
    if (zmalloc_thread_safe) { \
        pthread_mutex_lock(&used_memory_mutex); \
        used_memory -= _n; \
        pthread_mutex_unlock(&used_memory_mutex); \
    } else { \
        used_memory -= _n; \
    } \
} while(0)

// ============================================================================
// 枚举定义
// ============================================================================

// NUMA分配策略枚举
typedef enum {
    NUMA_POLICY_DEFAULT = 0,      // 默认策略（当前逻辑）
    NUMA_POLICY_DISTANCE_FIRST,   // 距离优先策略
    NUMA_POLICY_ROUND_ROBIN,      // 轮询策略（预留）
    NUMA_POLICY_BALANCED          // 负载均衡策略（预留）
} numa_policy_t;

static numa_policy_t current_numa_policy = NUMA_POLICY_DISTANCE_FIRST;

// ============================================================================
// 函数声明
// ============================================================================
static void *numa_alloc_distance_first(size_t total_size);
static void init_sorted_nodes(void);
static int compare_nodes_by_distance(const void *a, const void *b);

// ============================================================================
// NUMA节点管理函数
// ============================================================================

/**
 * 寻找距离当前线程最近的NUMA节点
 */
static int find_nearest_numa_node(void) {
    int max_node = numa_max_node();
    if (max_node < 0) return -1;
    
    // 获取当前CPU编号
    int current_cpu = sched_getcpu();
    if (current_cpu < 0) {
        // 如果无法获取当前CPU，返回第一个可用节点
        for (int i = 0; i <= max_node; i++) {
            if (numa_bitmask_isbitset(numa_all_nodes_ptr, i)) {
                return i;
            }
        }
        return -1;
    }
    
    // 获取当前CPU所属的节点
    int current_node = numa_node_of_cpu(current_cpu);
    if (current_node >= 0) {
        return current_node;
    }
    
    // 如果无法确定当前节点，使用距离计算
    int nearest_node = -1;
    int min_distance = INT_MAX;
    
    for (int i = 0; i <= max_node; i++) {
        if (numa_bitmask_isbitset(numa_all_nodes_ptr, i)) {
            // 计算到节点0的距离（作为参考）
            int distance = numa_distance(0, i);
            if (distance < min_distance) {
                min_distance = distance;
                nearest_node = i;
            }
        }
    }
    
    return nearest_node;
}

/**
 * 比较函数：用于节点距离排序
 */
static int compare_nodes_by_distance(const void *a, const void *b) {
    int node_a = *(int*)a;
    int node_b = *(int*)b;
    
    // 获取当前CPU编号
    int current_cpu = sched_getcpu();
    if (current_cpu < 0) {
        // 如果无法获取当前CPU，使用节点0作为参考
        return numa_distance(0, node_a) - numa_distance(0, node_b);
    }
    
    // 获取当前CPU所属的节点
    int current_node = numa_node_of_cpu(current_cpu);
    if (current_node >= 0) {
        return numa_distance(current_node, node_a) - numa_distance(current_node, node_b);
    }
    
    // 回退到使用节点0作为参考
    return numa_distance(0, node_a) - numa_distance(0, node_b);
}

/**
 * 初始化NUMA节点排序（按距离）
 */
static void init_sorted_nodes(void) {
    if (sorted_nodes_by_distance != NULL) {
        free(sorted_nodes_by_distance);
    }
    
    int max_node = numa_max_node();
    if (max_node < 0) {
        num_available_nodes = 0;
        sorted_nodes_by_distance = NULL;
        return;
    }
    
    // 收集所有可用节点
    int *available_nodes = malloc((max_node + 1) * sizeof(int));
    num_available_nodes = 0;
    
    for (int i = 0; i <= max_node; i++) {
        if (numa_bitmask_isbitset(numa_all_nodes_ptr, i)) {
            available_nodes[num_available_nodes++] = i;
        }
    }
    
    if (num_available_nodes == 0) {
        free(available_nodes);
        sorted_nodes_by_distance = NULL;
        return;
    }
    
    // 按距离排序
    sorted_nodes_by_distance = available_nodes;
    qsort(sorted_nodes_by_distance, num_available_nodes, sizeof(int), compare_nodes_by_distance);
    
    printf("zmalloc: NUMA nodes sorted by distance: ");
    for (int i = 0; i < num_available_nodes; i++) {
        printf("%d ", sorted_nodes_by_distance[i]);
    }
    printf("\n");
}

/**
 * 距离优先分配策略：按距离顺序尝试分配
 */
static void *numa_alloc_distance_first(size_t total_size) {
    if (sorted_nodes_by_distance == NULL || num_available_nodes == 0) {
        return NULL;
    }
    
    // 按距离顺序尝试每个节点
    for (int i = 0; i < num_available_nodes; i++) {
        int node = sorted_nodes_by_distance[i];
        void *ptr = numa_alloc_onnode(total_size, node);
        if (ptr != NULL) {
            printf("zmalloc: Allocated %zu bytes on node %d (distance priority: %d)\n", 
                   total_size, node, i);
            return ptr;
        } else {
            printf("zmalloc: Failed to allocate %zu bytes on node %d, trying next node\n", 
                   total_size, node);
        }
    }
    
    return NULL;  // 所有节点都分配失败
}

// ============================================================================
// NUMA初始化函数
// ============================================================================

/**
 * 初始化NUMA支持
 */
static void zmalloc_numa_init(void) {
    if (numa_initialized) return;
    
    // 检查NUMA是否可用
    numa_support_available = numa_available();
    if (numa_support_available == 0) {
        // NUMA库可用，检查系统是否有可用的NUMA节点
        int max_node = numa_max_node();
        if (max_node >= 0) {
            // 初始化节点排序
            init_sorted_nodes();
            
            if (num_available_nodes > 0) {
                // 设置默认节点为最近节点
                default_numa_node = sorted_nodes_by_distance[0];
                printf("zmalloc: NUMA support enabled, %d nodes available, nearest node: %d\n", 
                       num_available_nodes, default_numa_node);
            } else {
                printf("zmalloc: NUMA available but no available nodes found\n");
                numa_support_available = -1;
            }
        } else {
            printf("zmalloc: NUMA library available but no NUMA nodes detected\n");
            numa_support_available = -1;
        }
    } else {
        printf("zmalloc: NUMA not available, using standard allocation\n");
    }
    
    numa_initialized = 1;
}

// ============================================================================
// 内存分配核心函数
// ============================================================================

/**
 * 内存不足处理函数
 */
static void zmalloc_oom(size_t size, size_t total_size) {
    fprintf(stderr, "zmalloc: Out of memory trying to allocate %zu bytes (requested: %zu bytes)\n", 
            total_size, size);
    fflush(stderr);
    abort();
}

/**
 * 在指定NUMA节点分配内存（内部函数）
 */
static void *zmalloc_internal(size_t size, int node) {
    void *ptr = NULL;
    size_t total_size = size + PREFIX_SIZE;
    
    // 确保NUMA已初始化
    if (!numa_initialized) zmalloc_numa_init();
    
    // 根据NUMA可用性和节点选择分配策略
    if (numa_support_available == 0 && node >= 0) {
        // 在指定NUMA节点分配内存
        ptr = numa_alloc_onnode(total_size, node);
    } else if (numa_support_available == 0) {
        // 使用智能分配策略
        switch (current_numa_policy) {
            case NUMA_POLICY_DISTANCE_FIRST:
                ptr = numa_alloc_distance_first(total_size);
                break;
            case NUMA_POLICY_DEFAULT:
            default:
                // 使用默认节点
                if (default_numa_node >= 0) {
                    ptr = numa_alloc_onnode(total_size, default_numa_node);
                }
                break;
        }
        
        // 如果智能分配失败，回退到标准分配
        if (ptr == NULL) {
            printf("zmalloc: Smart allocation failed, falling back to standard allocation\n");
            ptr = malloc(total_size);
        }
    } else {
        // 标准分配（NUMA不可用或没有可用节点）
        ptr = malloc(total_size);
    }
    
    if (!ptr) zmalloc_oom(size, total_size);
    
#ifdef HAVE_MALLOC_SIZE
    increment_used_memory(redis_malloc_size(ptr));
    return ptr;
#else
    // 存储分配大小在内存块开头
    *((size_t*)ptr) = size;
    increment_used_memory(total_size);
    return (char*)ptr + PREFIX_SIZE;
#endif
}

/**
 * 在指定NUMA节点释放内存（内部函数）
 */
static void zfree_internal(void *ptr) {
#ifndef HAVE_MALLOC_SIZE
    void *realptr;
    size_t oldsize;
#endif

    if (ptr == NULL) return;
    
#ifdef HAVE_MALLOC_SIZE
    decrement_used_memory(redis_malloc_size(ptr));
    
    // 根据NUMA可用性选择释放方式
    if (numa_support_available == 0) {
        numa_free(ptr, redis_malloc_size(ptr));
    } else {
        free(ptr);
    }
#else
    realptr = (char*)ptr - PREFIX_SIZE;
    oldsize = *((size_t*)realptr);
    decrement_used_memory(oldsize + PREFIX_SIZE);
    
    // 根据NUMA可用性选择释放方式
    if (numa_support_available == 0) {
        numa_free(realptr, oldsize + PREFIX_SIZE);
    } else {
        free(realptr);
    }
#endif
}

// ============================================================================
// 公共API函数
// ============================================================================

/**
 * 标准内存分配函数
 */
void *zmalloc(size_t size) {
    return zmalloc_internal(size, -1);  // 使用自动节点选择
}

/**
 * 在指定NUMA节点分配内存
 */
void *zmalloc_on_node(size_t size, int node) {
    return zmalloc_internal(size, node);
}

/**
 * 内存重新分配函数
 */
void *zrealloc(void *ptr, size_t size) {
#ifndef HAVE_MALLOC_SIZE
    void *realptr;
#endif
    size_t oldsize;
    void *newptr;
    size_t total_size = size + PREFIX_SIZE;

    if (ptr == NULL) return zmalloc(size);
    
#ifdef HAVE_MALLOC_SIZE
    oldsize = redis_malloc_size(ptr);
    
    // 重新分配内存
    if (numa_support_available == 0) {
        newptr = numa_realloc(ptr, oldsize, total_size);
    } else {
        newptr = realloc(ptr, total_size);
    }
    
    if (!newptr) zmalloc_oom(size, total_size);

    decrement_used_memory(oldsize);
    increment_used_memory(redis_malloc_size(newptr));
    return newptr;
#else
    realptr = (char*)ptr - PREFIX_SIZE;
    oldsize = *((size_t*)realptr);
    
    // 重新分配内存
    if (numa_support_available == 0) {
        newptr = numa_realloc(realptr, oldsize + PREFIX_SIZE, total_size);
    } else {
        newptr = realloc(realptr, total_size);
    }
    
    if (!newptr) zmalloc_oom(size, total_size);

    *((size_t*)newptr) = size;
    decrement_used_memory(oldsize + PREFIX_SIZE);
    increment_used_memory(total_size);
    return (char*)newptr + PREFIX_SIZE;
#endif
}

/**
 * 在指定NUMA节点重新分配内存
 */
void *zrealloc_on_node(void *ptr, size_t size, int node) {
    // 对于realloc，我们无法直接指定目标节点
    // 这里先分配新内存，复制数据，然后释放旧内存
    void *newptr = zmalloc_on_node(size, node);
    if (ptr && newptr) {
        // 直接计算要复制的数据大小，避免函数调用
        size_t copy_size;
        
#ifdef HAVE_MALLOC_SIZE
        // 如果系统提供malloc_size函数，使用它
        copy_size = redis_malloc_size(ptr);
#else
        // 否则从内存块头部读取大小信息
        void *realptr = (char*)ptr - PREFIX_SIZE;
        copy_size = *((size_t*)realptr);
#endif
        
        // 确保不会复制超过新大小的数据
        if (copy_size > size) 
            copy_size = size;
            
        // 复制数据
        memcpy(newptr, ptr, copy_size);
    }
    
    // 释放旧内存
    if (ptr) 
        zfree(ptr);
        
    return newptr;
}

/**
 * 内存释放函数
 */
void zfree(void *ptr) {
    zfree_internal(ptr);
}

/**
 * 字符串复制函数
 */
char *zstrdup(const char *s) {
    size_t l = strlen(s) + 1;
    char *p = zmalloc(l);
    memcpy(p, s, l);
    return p;
}

// ============================================================================
// 内存管理辅助函数
// ============================================================================

/**
 * 获取已使用内存总量
 */
size_t zmalloc_used_memory(void) {
    size_t um;
    if (zmalloc_thread_safe) pthread_mutex_lock(&used_memory_mutex);
    um = used_memory;
    if (zmalloc_thread_safe) pthread_mutex_unlock(&used_memory_mutex);
    return um;
}

/**
 * 启用线程安全
 */
void zmalloc_enable_thread_safeness(void) {
    zmalloc_thread_safe = 1;
}

/**
 * 获取当前线程的NUMA节点
 */
int zmalloc_get_current_numa_node(void) {
    if (!numa_initialized) zmalloc_numa_init();
    
    if (numa_support_available == 0) {
        // 返回找到的最近节点，而不是numa_preferred()
        return default_numa_node;
    }
    return -1;  // NUMA不可用
}

/**
 * 设置默认NUMA节点
 */
void zmalloc_set_numa_node(int node) {
    if (!numa_initialized) zmalloc_numa_init();
    
    if (node >= 0 && node < numa_max_node() + 1) {
        default_numa_node = node;
        printf("zmalloc: Default NUMA node set to %d\n", node);
    } else {
        fprintf(stderr, "zmalloc: Invalid NUMA node %d\n", node);
    }
}

/**
 * 设置NUMA分配策略
 */
void zmalloc_set_numa_policy(numa_policy_t policy) {
    current_numa_policy = policy;
    printf("zmalloc: NUMA policy set to %d\n", policy);
}

/**
 * 清理NUMA资源
 */
void zmalloc_cleanup_numa(void) {
    if (sorted_nodes_by_distance != NULL) {
        free(sorted_nodes_by_distance);
        sorted_nodes_by_distance = NULL;
    }
    num_available_nodes = 0;
    numa_initialized = 0;
}