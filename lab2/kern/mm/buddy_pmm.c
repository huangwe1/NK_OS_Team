#include <pmm.h>
#include <list.h>
#include <string.h>
#include <buddy_pmm.h>
#include <stdio.h>

#define PAGE_SIZE 4096
#define p_num(order) (1 << (order))  // 块大小(页) = 2^order
#define MAX_POSSIBLE_ORDER 20  

// 定义伙伴系统结构体
typedef struct {
    list_entry_t free_array[MAX_POSSIBLE_ORDER + 1];  // 各阶空闲链表
    int max_order;                                   // 系统支持的最大阶数
    size_t nr_free;                                  // 空闲页总数
} buddy_system_t;

buddy_system_t buddy_system;
#define free_list (buddy_system.free_array)  // 空闲链表数组
#define max_order (buddy_system.max_order)   // 最大阶数
#define nr_free (buddy_system.nr_free)       // 空闲页总数

// 初始化伙伴系统
static void
buddy_system_init(void) {
    for (int i = 0; i < MAX_POSSIBLE_ORDER + 1; i++) {
        list_init(&free_list[i]);  // 初始化各阶空闲链表
    }
    max_order = 0;
    nr_free = 0;
}

// 计算n对应的阶数（2^order >= n）
static int get_order(size_t n) {
    if (n == 0) return 0;
    int order = 0;
    size_t size = 1;
    while (size < n) {
        size <<= 1;
        order++;
    }
    return order; }

// 计算小于等于n的最大2的幂对应的阶数
static int get_max_order_for_size(size_t n) {
    if (n == 0) return 0;
    int order = 0;
    size_t size = 1;
    while (size * 2 <= n) {
        size <<= 1;
        order++;
    }
    return order;
}

// 初始化内存映射
static void buddy_init_memmap(struct Page *base, size_t n) {
    assert(n > 0);
    struct Page *p = base;
    for (; p != base + n; p++) {
        assert(PageReserved(p));
        p->flags = p->property = 0;
        set_page_ref(p, 0);
    }

    // 计算初始内存可支持的最大阶数
    max_order = get_max_order_for_size(n);

    // 将初始内存块划分为最大可能的2的幂次方块
    size_t remaining = n;
    p = base;
    while (remaining > 0) {
        int order = 0;
        size_t max_block = 1;
        // 找到最大的2的幂次方块（不超过remaining和当前max_order）
        while (max_block * 2 <= remaining && order + 1 <= max_order) {
            max_block <<= 1;
            order++;
        }
        
        // 标记块属性
        p->property = order;
        SetPageProperty(p);
        // 添加到对应阶的空闲链表（修正：数组访问用[]）
        list_add(&free_list[order], &(p->page_link));
        // 更新计数
        nr_free += max_block;
        remaining -= max_block;
        p += max_block;
    }
}
// 分配n页内存
static struct Page *buddy_alloc_pages(size_t n) {
    assert(n > 0);
    if (n > nr_free) {
        return NULL;  // 内存不足
    }
    
    int order = get_order(n);
    if (order > max_order) {
        return NULL;  // 超过当前系统最大阶数
    }
    
    // 从order开始查找可用块
    int i;
    for (i = order; i <= max_order; i++) {
        if (!list_empty(&free_list[i])) {  
            break;
        }
    }
    
    if (i > max_order) {
        return NULL;  // 未找到可用块
    }
    
    // 从i阶链表中取出一个块
    list_entry_t *le = list_next(&free_list[i]);  
    struct Page *p = le2page(le, page_link);
    list_del(le);
    nr_free -= p_num(i);
    ClearPageProperty(p);
    
    // 分裂块直到达到所需阶数
    while (i > order) {
        i--;
        struct Page *buddy = p + p_num(i);
        buddy->property = i;
        SetPageProperty(buddy);
        list_add(&free_list[i], &(buddy->page_link));  
        nr_free += p_num(i);
    }
    
    p->property = order;
    return p;
}

// 计算伙伴块地址
static struct Page *get_buddy(struct Page *p, int order) {
    size_t block_pages = p_num(order);       // 块包含的页数（2^order）
    size_t block_bytes = block_pages * PAGE_SIZE;  // 块的总字节数
    uintptr_t pa = page2pa(p);               // 当前块的物理地址（字节）
    uintptr_t buddy_pa = pa - block_bytes;   // 伙伴块物理地址 = 当前地址 ^ 块字节数
    return pa2page(buddy_pa);                // 转换为Page指针
}

// 释放n页内存
static void buddy_free_pages(struct Page *base, size_t n) {
    assert(n > 0 && base != NULL);
    
    struct Page *p = base;
    // 验证释放的内存块
    for (; p != base + n; p++) {
        assert(!PageReserved(p) && !PageProperty(p));
        p->flags = 0;
        set_page_ref(p, 0);
    }
    
    int order = get_order(n);
    assert(p_num(order) == n);
    
    base->property = order;
    SetPageProperty(base);
    list_add(&free_list[order], &(base->page_link));  
    nr_free += n;
    
    // 尝试合并伙伴块（上限为max_order）
    struct Page *current = base;
    while (order < max_order) {
        struct Page *buddy = get_buddy(current, order);
        
        // 检查伙伴是否可合并
        if (!PageProperty(buddy) || buddy->property != order) {
           break;
        }
        
        // 从链表中移除当前块和伙伴块
        list_del(&(current->page_link));
        list_del(&(buddy->page_link));
        nr_free -= p_num(order) * 2;
        
        // 确定合并后的块起始地址
        struct Page *merged = (current < buddy) ? current : buddy;
        merged->property = order + 1;
        SetPageProperty(merged);
        list_add(&free_list[order + 1], &(merged->page_link));  
        nr_free += p_num(order + 1);
        
        current = merged;
        order++;
    }
}

// 获取空闲页数量
static size_t buddy_nr_free_pages(void) {
    return nr_free;
}
static void print_free_blocks(void) {
    // 从最高阶到最低阶遍历，确保大的块先打印
     cprintf("\n");
    for (int order = max_order; order >= 0; order--) {
        list_entry_t *le = &free_list[order];
        // 遍历当前阶的所有空闲块
        for (le = list_next(le); le != &free_list[order]; le = list_next(le)) {
            size_t block_size = p_num(order) ;
            cprintf("%llu|", block_size);
        }
    }
}
// 测试最小内存单位（1页）的分配与释放
static void test_min_unit_operation(void) {
    cprintf("=== 测试最小内存单位（1页）操作 ===\n");
    size_t initial_free = buddy_nr_free_pages();
    print_free_blocks();
    struct Page *p = buddy_alloc_pages(1);
    assert(p != NULL);
    assert(p->property == 0);  // 验证阶数为0
    assert(buddy_nr_free_pages() == initial_free - 1);
    assert(!PageProperty(p));  // 已分配的页不应标记为空闲
    print_free_blocks();
    // 释放1页
    buddy_free_pages(p, 1);
    assert(buddy_nr_free_pages() == initial_free);
    assert(PageProperty(p));   // 释放后应标记为空闲
    assert(p->property == 0);  // 释放后阶数保持0
    print_free_blocks();

    cprintf("=== 最小内存单位测试通过 ===\n\n");
}

// 测试最大内存单位的分配与释放
static void test_max_unit_operation(void) {
    cprintf("=== 测试最大内存单位操作 ===\n");
    size_t initial_free = buddy_nr_free_pages();
    size_t max_block_size = p_num(max_order);  // 最大块大小：2^max_order页
    
    // 分配最大块
    struct Page *p = buddy_alloc_pages(max_block_size);
    assert(p != NULL);
    assert(p->property == max_order);  // 验证阶数为max_order
    assert(buddy_nr_free_pages() == initial_free - max_block_size);
    print_free_blocks();

    // 尝试分配比最大块大1页的内存（应失败）
    struct Page *p_over = buddy_alloc_pages(max_block_size + 1);
    assert(p_over == NULL);

    // 释放最大块
    buddy_free_pages(p, max_block_size);
    assert(buddy_nr_free_pages() == initial_free);
    assert(PageProperty(p));
    assert(p->property == max_order);
    print_free_blocks();

    // 验证释放后是否正确加入max_order链表
    list_entry_t *le = &free_list[max_order];
    assert(!list_empty(le));  // max_order链表不应为空
    struct Page *check_p = le2page(list_next(le), page_link);
    assert(check_p == p);     // 链表中应包含刚释放的页

    cprintf("=== 最大内存单位测试通过 ===\n\n");
}

// 测试伙伴块合并逻辑（从低阶到高阶逐步合并）
static void test_block_merging(void) {
    cprintf("=== 测试伙伴块合并逻辑 ===\n");
    size_t initial_free = buddy_nr_free_pages();
    
    int start_order = 3  ;//
    size_t block_pages = p_num(start_order);
    
    // 首先确保有足够的高阶块可以分裂
    if (initial_free < block_pages * 2) {
        cprintf("内存不足，跳过伙伴合并测试\n");
        return;
    }
    // 现在分配两个相同阶数的块，它们应该是伙伴
    struct Page *p1 = buddy_alloc_pages(block_pages);
    print_free_blocks();

    struct Page *p2 = buddy_alloc_pages(block_pages);
    print_free_blocks();
    assert(p1 != NULL && p2 != NULL);
    
    
    // 计算p1的伙伴并验证
    struct Page *buddy_of_p2 = get_buddy(p2, start_order);
    uintptr_t pa1 = page2pa(p1);
    uintptr_t pa2 = page2pa(p2);
    uintptr_t buddy_pa = page2pa(buddy_of_p2);
    
    
    // 释放p1和p2，测试合并逻辑
    buddy_free_pages(p2, block_pages);
    buddy_free_pages(p1, block_pages);
    print_free_blocks();
    
    cprintf("=== 伙伴块合并逻辑测试通过 ===\n\n");
}
// 综合测试入口
static void buddy_system_test(void) {
    cprintf("===== 开始伙伴系统综合测试 =====\n");
     cprintf("当前空闲页总数: %lu\n", nr_free);

    test_min_unit_operation();
    test_max_unit_operation();
    test_block_merging();
    cprintf("===== 所有伙伴系统测试通过 =====\n");
}

const struct pmm_manager buddy_pmm_manager = {
    .name = "buddy_pmm_manager",
    .init = buddy_system_init,
    .init_memmap = buddy_init_memmap,
    .alloc_pages = buddy_alloc_pages,
    .free_pages = buddy_free_pages,
    .nr_free_pages = buddy_nr_free_pages,
    .check = buddy_system_test,  
};