/* zmalloc - total amount of allocated memory aware version of malloc()
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

#ifndef _ZMALLOC_H
#define _ZMALLOC_H

#include <stddef.h>

void *zmalloc(size_t size);
void *zrealloc(void *ptr, size_t size);
void zfree(void *ptr);
char *zstrdup(const char *s);
size_t zmalloc_used_memory(void);
void zmalloc_enable_thread_safeness(void);

// libnuma相关的扩展函数
void zmalloc_set_numa_node(int node);  // 设置默认的NUMA节点
int zmalloc_get_current_numa_node(void);  // 获取当前线程的NUMA节点
void *zmalloc_on_node(size_t size, int node);  // 在指定NUMA节点分配内存
void *zrealloc_on_node(void *ptr, size_t size, int node);  // 在指定NUMA节点重新分配

// NUMA策略相关函数
typedef enum {
    NUMA_POLICY_DEFAULT = 0,      // 默认策略
    NUMA_POLICY_DISTANCE_FIRST,   // 距离优先策略
    NUMA_POLICY_ROUND_ROBIN,      // 轮询策略
    NUMA_POLICY_BALANCED          // 负载均衡策略
} numa_policy_t;

void zmalloc_set_numa_policy(numa_policy_t policy);  // 设置NUMA分配策略
void zmalloc_cleanup_numa(void);  // 清理NUMA资源

#endif /* _ZMALLOC_H */