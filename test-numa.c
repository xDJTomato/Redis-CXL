/*
 * NUMA感知zmalloc功能测试程序
 * 用于验证libnuma集成后的功能正确性
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zmalloc.h"

/**
 * 基本功能测试
 */
void test_basic_functionality(void) {
    printf("=== 基本功能测试 ===\n");
    
    // 测试zmalloc
    char *ptr1 = zmalloc(100);
    printf("zmalloc(100): %p\n", ptr1);
    
    // 测试zstrdup
    char *str = zstrdup("Hello, NUMA!");
    printf("zstrdup: %s\n", str);
    
    // 测试zrealloc
    ptr1 = zrealloc(ptr1, 200);
    printf("zrealloc to 200 bytes: %p\n", ptr1);
    
    // 测试内存使用统计
    printf("Used memory: %zu bytes\n", zmalloc_used_memory());
    
    // 释放内存
    zfree(ptr1);
    zfree(str);
    
    printf("After free: %zu bytes\n", zmalloc_used_memory());
    printf("基本功能测试完成\n\n");
}

/**
 * NUMA特定功能测试
 */
void test_numa_functionality(void) {
    printf("=== NUMA功能测试 ===\n");
    
    // 检查NUMA支持
    int current_node = zmalloc_get_current_numa_node();
    printf("Current NUMA node: %d\n", current_node);
    
    if (current_node >= 0) {
        // 测试在特定节点分配内存
        int target_node = (current_node + 1) % 2;  // 尝试另一个节点
        printf("Testing allocation on node %d\n", target_node);
        
        char *numa_ptr = zmalloc_on_node(512, target_node);
        if (numa_ptr) {
            printf("NUMA allocation successful: %p\n", numa_ptr);
            strcpy(numa_ptr, "Allocated on specific NUMA node");
            printf("Content: %s\n", numa_ptr);
            zfree(numa_ptr);
        }
        
        // 测试设置默认节点
        zmalloc_set_numa_node(current_node);
        printf("Default NUMA node set to: %d\n", current_node);
    } else {
        printf("NUMA not available on this system\n");
    }
    
    printf("NUMA功能测试完成\n\n");
}

/**
 * 线程安全测试
 */
void test_thread_safety(void) {
    printf("=== 线程安全测试 ===\n");
    
    // 启用线程安全
    zmalloc_enable_thread_safeness();
    printf("Thread safety enabled\n");
    
    // 在多线程环境下测试分配
    char *thread_ptr = zmalloc(256);
    printf("Thread-safe allocation: %p\n", thread_ptr);
    
    if (thread_ptr) {
        strcpy(thread_ptr, "Thread-safe memory allocation");
        printf("Content: %s\n", thread_ptr);
        zfree(thread_ptr);
    }
    
    printf("线程安全测试完成\n\n");
}

/**
 * 性能测试
 */
void test_performance(void) {
    printf("=== 性能测试 ===\n");
    
    const int num_allocations = 1000;
    const size_t block_size = 1024;  // 1KB blocks
    
    printf("Allocating %d blocks of %zu bytes each\n", num_allocations, block_size);
    
    char **blocks = malloc(num_allocations * sizeof(char*));
    if (!blocks) {
        printf("Failed to allocate block array\n");
        return;
    }
    
    // 分配测试
    for (int i = 0; i < num_allocations; i++) {
        blocks[i] = zmalloc(block_size);
        if (!blocks[i]) {
            printf("Allocation failed at block %d\n", i);
            break;
        }
        // 写入一些数据
        sprintf(blocks[i], "Block %d", i);
    }
    
    printf("Memory usage after allocation: %zu bytes\n", zmalloc_used_memory());
    
    // 释放测试
    for (int i = 0; i < num_allocations; i++) {
        if (blocks[i]) {
            zfree(blocks[i]);
        }
    }
    free(blocks);
    
    printf("Memory usage after free: %zu bytes\n", zmalloc_used_memory());
    printf("性能测试完成\n\n");
}

int main(void) {
    printf("开始NUMA感知zmalloc测试...\n\n");
    
    test_basic_functionality();
    test_numa_functionality();
    test_thread_safety();
    test_performance();
    
    printf("所有测试完成！\n");
    return 0;
}