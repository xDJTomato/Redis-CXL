/* SDSLib, A C dynamic strings library
 *
 * Copyright (c) 2006-2010, Salvatore Sanfilippo <antirez at gmail dot com>
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
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#define SDS_ABORT_ON_OOM

#include "sds.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include "zmalloc.h"

// 内存不足时的处理函数
static void sdsOomAbort(void) {
    fprintf(stderr,"SDS: Out Of Memory (SDS_ABORT_ON_OOM defined)\n");
    abort();
}

/**
 * 创建指定长度的SDS字符串
 * @param init 初始化数据指针，可以为NULL
 * @param initlen 初始化数据的长度
 * @return 返回指向字符串内容的指针（即buf字段）
 */
sds sdsnewlen(const void *init, size_t initlen) {
    struct sdshdr *sh;

    // 分配内存：头部结构体 + 字符串长度 + 1（用于'\0'）
    sh = zmalloc(sizeof(struct sdshdr)+initlen+1);
#ifdef SDS_ABORT_ON_OOM
    if (sh == NULL) sdsOomAbort();  // 内存不足时中止程序
#else
    if (sh == NULL) return NULL;    // 内存不足时返回NULL
#endif
    
    // 初始化SDS头部信息
    sh->len = initlen;   // 设置字符串长度
    sh->free = 0;        // 初始时没有预分配空间
    
    // 复制初始化数据
    if (initlen) {
        if (init) 
            memcpy(sh->buf, init, initlen);  // 复制提供的初始化数据
        else 
            memset(sh->buf,0,initlen);       // 如果没有数据，填充0
    }
    
    // 添加字符串结束符
    sh->buf[initlen] = '\0';
    
    // 返回指向字符串内容的指针（隐藏头部信息）
    return (char*)sh->buf;
}

/**
 * 创建空SDS字符串
 * @return 空字符串的SDS指针
 */
sds sdsempty(void) {
    return sdsnewlen("",0);
}

/**
 * 从C字符串创建SDS
 * @param init C字符串指针，可以为NULL
 * @return 新创建的SDS指针
 */
sds sdsnew(const char *init) {
    size_t initlen = (init == NULL) ? 0 : strlen(init);
    return sdsnewlen(init, initlen);
}

/**
 * 获取SDS字符串长度
 * @param s SDS字符串指针
 * @return 字符串长度
 */
size_t sdslen(const sds s) {
    // 通过指针运算获取头部结构体指针
    // s指向buf字段，减去头部大小得到sdshdr指针
    struct sdshdr *sh = (void*) (s-(sizeof(struct sdshdr)));
    return sh->len;
}

/**
 * 复制SDS字符串
 * @param s 源SDS字符串
 * @return 新创建的SDS副本
 */
sds sdsdup(const sds s) {
    return sdsnewlen(s, sdslen(s));
}

/**
 * 释放SDS字符串内存
 * @param s 要释放的SDS字符串
 */
void sdsfree(sds s) {
    if (s == NULL) return;
    // 释放整个SDS结构（头部+内容）
    zfree(s-sizeof(struct sdshdr));
}

/**
 * 获取SDS可用空间大小
 * @param s SDS字符串
 * @return 可用字节数
 */
size_t sdsavail(sds s) {
    struct sdshdr *sh = (void*) (s-(sizeof(struct sdshdr)));
    return sh->free;
}

/**
 * 更新SDS长度信息（基于实际字符串长度）
 * @param s SDS字符串
 */
void sdsupdatelen(sds s) {
    struct sdshdr *sh = (void*) (s-(sizeof(struct sdshdr)));
    int reallen = strlen(s);  // 获取实际C字符串长度
    
    // 调整free字段，保持总空间不变
    sh->free += (sh->len-reallen);
    sh->len = reallen;        // 更新为实际长度
}

/**
 * 为SDS字符串预分配更多空间（空间不足时自动扩容）
 * @param s SDS字符串
 * @param addlen 需要额外增加的长度
 * @return 可能重新分配后的SDS指针
 */
static sds sdsMakeRoomFor(sds s, size_t addlen) {
    struct sdshdr *sh, *newsh;
    size_t free = sdsavail(s);
    size_t len, newlen;

    // 如果当前可用空间足够，直接返回
    if (free >= addlen) return s;
    
    // 计算需要的新长度（采用2倍扩容策略）
    len = sdslen(s);
    sh = (void*) (s-(sizeof(struct sdshdr)));
    newlen = (len+addlen)*2;  // 2倍扩容，减少频繁重新分配
    
    // 重新分配内存
    newsh = zrealloc(sh, sizeof(struct sdshdr)+newlen+1);
#ifdef SDS_ABORT_ON_OOM
    if (newsh == NULL) sdsOomAbort();
#else
    if (newsh == NULL) return NULL;
#endif

    // 更新可用空间信息
    newsh->free = newlen - len;
    return newsh->buf;
}

/**
 * 向SDS字符串追加指定长度的数据
 * @param s 目标SDS字符串
 * @param t 要追加的数据指针
 * @param len 要追加的数据长度
 * @return 追加后的SDS指针
 */
sds sdscatlen(sds s, void *t, size_t len) {
    struct sdshdr *sh;
    size_t curlen = sdslen(s);

    // 确保有足够空间
    s = sdsMakeRoomFor(s,len);
    if (s == NULL) return NULL;
    
    sh = (void*) (s-(sizeof(struct sdshdr)));
    
    // 复制数据到字符串末尾
    memcpy(s+curlen, t, len);
    
    // 更新长度和可用空间
    sh->len = curlen+len;
    sh->free = sh->free-len;
    
    // 添加字符串结束符
    s[curlen+len] = '\0';
    return s;
}

/**
 * 向SDS字符串追加C字符串
 * @param s 目标SDS字符串
 * @param t 要追加的C字符串
 * @return 追加后的SDS指针
 */
sds sdscat(sds s, char *t) {
    return sdscatlen(s, t, strlen(t));
}

/**
 * 将指定长度的数据复制到SDS字符串（覆盖原有内容）
 * @param s 目标SDS字符串
 * @param t 源数据指针
 * @param len 要复制的数据长度
 * @return 复制后的SDS指针
 */
sds sdscpylen(sds s, char *t, size_t len) {
    struct sdshdr *sh = (void*) (s-(sizeof(struct sdshdr)));
    size_t totlen = sh->free+sh->len;  // 当前总空间

    // 如果空间不足，重新分配
    if (totlen < len) {
        s = sdsMakeRoomFor(s,len-sh->len);
        if (s == NULL) return NULL;
        sh = (void*) (s-(sizeof(struct sdshdr)));
        totlen = sh->free+sh->len;
    }
    
    // 复制数据
    memcpy(s, t, len);
    s[len] = '\0';
    
    // 更新长度和可用空间
    sh->len = len;
    sh->free = totlen-len;
    return s;
}

/**
 * 将C字符串复制到SDS字符串（覆盖原有内容）
 * @param s 目标SDS字符串
 * @param t 源C字符串
 * @return 复制后的SDS指针
 */
sds sdscpy(sds s, char *t) {
    return sdscpylen(s, t, strlen(t));
}

/**
 * 格式化字符串并追加到SDS
 * @param s 目标SDS字符串
 * @param fmt 格式化字符串
 * @param ... 可变参数
 * @return 追加后的SDS指针
 */
sds sdscatprintf(sds s, const char *fmt, ...) {
    va_list ap;
    char *buf, *t;
    size_t buflen = 16;  // 初始缓冲区大小

    // 动态调整缓冲区大小，直到能够容纳格式化结果
    while(1) {
        buf = zmalloc(buflen);
#ifdef SDS_ABORT_ON_OOM
        if (buf == NULL) sdsOomAbort();
#else
        if (buf == NULL) return NULL;
#endif
        
        // 检查格式化结果是否被截断
        buf[buflen-2] = '\0';
        va_start(ap, fmt);
        vsnprintf(buf, buflen, fmt, ap);
        va_end(ap);
        
        // 如果缓冲区不够大，重新分配更大的缓冲区
        if (buf[buflen-2] != '\0') {
            zfree(buf);
            buflen *= 2;  // 双倍扩容
            continue;
        }
        break;
    }
    
    // 将格式化结果追加到SDS
    t = sdscat(s, buf);
    zfree(buf);
    return t;
}

/**
 * 去除SDS字符串两端的指定字符
 * @param s 目标SDS字符串
 * @param cset 要去除的字符集合
 * @return 处理后的SDS指针
 */
sds sdstrim(sds s, const char *cset) {
    struct sdshdr *sh = (void*) (s-(sizeof(struct sdshdr)));
    char *start, *end, *sp, *ep;
    size_t len;

    // 找到字符串开头和结尾的非指定字符位置
    sp = start = s;
    ep = end = s+sdslen(s)-1;
    
    // 跳过开头的指定字符
    while(sp <= end && strchr(cset, *sp)) sp++;
    
    // 跳过结尾的指定字符
    while(ep > start && strchr(cset, *ep)) ep--;
    
    // 计算新长度
    len = (sp > ep) ? 0 : ((ep-sp)+1);
    
    // 如果需要移动数据
    if (sh->buf != sp) memmove(sh->buf, sp, len);
    
    // 更新字符串信息
    sh->buf[len] = '\0';
    sh->free = sh->free+(sh->len-len);
    sh->len = len;
    return s;
}

sds sdsrange(sds s, long start, long end) {
    struct sdshdr *sh = (void*) (s-(sizeof(struct sdshdr)));
    size_t newlen, len = sdslen(s);

    if (len == 0) return s;
    if (start < 0) {
        start = len+start;
        if (start < 0) start = 0;
    }
    if (end < 0) {
        end = len+end;
        if (end < 0) end = 0;
    }
    newlen = (start > end) ? 0 : (end-start)+1;
    if (newlen != 0) {
        if (start >= (signed)len) start = len-1;
        if (end >= (signed)len) end = len-1;
        newlen = (start > end) ? 0 : (end-start)+1;
    } else {
        start = 0;
    }
    if (start != 0) memmove(sh->buf, sh->buf+start, newlen);
    sh->buf[newlen] = 0;
    sh->free = sh->free+(sh->len-newlen);
    sh->len = newlen;
    return s;
}

void sdstolower(sds s) {
    int len = sdslen(s), j;

    for (j = 0; j < len; j++) s[j] = tolower(s[j]);
}

void sdstoupper(sds s) {
    int len = sdslen(s), j;

    for (j = 0; j < len; j++) s[j] = toupper(s[j]);
}

int sdscmp(sds s1, sds s2) {
    size_t l1, l2, minlen;
    int cmp;

    l1 = sdslen(s1);
    l2 = sdslen(s2);
    minlen = (l1 < l2) ? l1 : l2;
    cmp = memcmp(s1,s2,minlen);
    if (cmp == 0) return l1-l2;
    return cmp;
}

/* Split 's' with separator in 'sep'. An array
 * of sds strings is returned. *count will be set
 * by reference to the number of tokens returned.
 *
 * On out of memory, zero length string, zero length
 * separator, NULL is returned.
 *
 * Note that 'sep' is able to split a string using
 * a multi-character separator. For example
 * sdssplit("foo_-_bar","_-_"); will return two
 * elements "foo" and "bar".
 *
 * This version of the function is binary-safe but
 * requires length arguments. sdssplit() is just the
 * same function but for zero-terminated strings.
 */
sds *sdssplitlen(char *s, int len, char *sep, int seplen, int *count) {
    int elements = 0, slots = 5, start = 0, j;

    sds *tokens = zmalloc(sizeof(sds)*slots);
#ifdef SDS_ABORT_ON_OOM
    if (tokens == NULL) sdsOomAbort();
#endif
    if (seplen < 1 || len < 0 || tokens == NULL) return NULL;
    if (len == 0) {
        *count = 0;
        return tokens;
    }
    for (j = 0; j < (len-(seplen-1)); j++) {
        /* make sure there is room for the next element and the final one */
        if (slots < elements+2) {
            sds *newtokens;

            slots *= 2;
            newtokens = zrealloc(tokens,sizeof(sds)*slots);
            if (newtokens == NULL) {
#ifdef SDS_ABORT_ON_OOM
                sdsOomAbort();
#else
                goto cleanup;
#endif
            }
            tokens = newtokens;
        }
        /* search the separator */
        if ((seplen == 1 && *(s+j) == sep[0]) || (memcmp(s+j,sep,seplen) == 0)) {
            tokens[elements] = sdsnewlen(s+start,j-start);
            if (tokens[elements] == NULL) {
#ifdef SDS_ABORT_ON_OOM
                sdsOomAbort();
#else
                goto cleanup;
#endif
            }
            elements++;
            start = j+seplen;
            j = j+seplen-1; /* skip the separator */
        }
    }
    /* Add the final element. We are sure there is room in the tokens array. */
    tokens[elements] = sdsnewlen(s+start,len-start);
    if (tokens[elements] == NULL) {
#ifdef SDS_ABORT_ON_OOM
                sdsOomAbort();
#else
                goto cleanup;
#endif
    }
    elements++;
    *count = elements;
    return tokens;

#ifndef SDS_ABORT_ON_OOM
cleanup:
    {
        int i;
        for (i = 0; i < elements; i++) sdsfree(tokens[i]);
        zfree(tokens);
        return NULL;
    }
#endif
}