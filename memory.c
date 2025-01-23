//
// Created by fuqiuluo on 25-1-22.
//
#include "memory.h"

#include <linux/tty.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/memory.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <asm/cpu.h>
#include <asm/page.h>
#include <linux/pgtable.h>
#include <linux/vmalloc.h>

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 8, 0))
#include <linux/mmap_lock.h>
#define MM_READ_LOCK(mm) mmap_read_lock(mm);
#define MM_READ_UNLOCK(mm) mmap_read_unlock(mm);
#else
#include <linux/rwsem.h>
#define MM_READ_LOCK(mm) down_read(&(mm)->mmap_sem);
#define MM_READ_UNLOCK(mm) up_read(&(mm)->mmap_sem);
#endif

#include "mmuhack.h"

#ifdef CONFIG_CMA
//#warning CMA is enabled!
#endif

#if !defined(ARCH_HAS_VALID_PHYS_ADDR_RANGE) || defined(MODULE)
static inline int memk_valid_phys_addr_range(phys_addr_t addr, size_t size)
{
	return addr + size <= __pa(high_memory);
}
#define IS_VALID_PHYS_ADDR_RANGE(x,y) memk_valid_phys_addr_range(x,y)
#else
#define IS_VALID_PHYS_ADDR_RANGE(x,y) valid_phys_addr_range(x,y)
#endif

uintptr_t get_module_base(pid_t pid, char *name) {
    struct pid *pid_struct;
    struct task_struct *task;
    struct mm_struct *mm;
    struct vm_area_struct *vma;
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0))
    struct vma_iterator vmi;
#endif
    char *buf = NULL;
    uintptr_t result;

    result = 0;

    pid_struct = find_get_pid(pid);
    if (!pid_struct) {
        return 0;
    }

    task = get_pid_task(pid_struct, PIDTYPE_PID);
    put_pid(pid_struct);
    if (!task) {
        return 0;
    }

    mm = get_task_mm(task);
    put_task_struct(task);
    if (!mm) {
        return 0;
    }

    buf = kmalloc(PATH_MAX, GFP_KERNEL);
    if (!buf) {
        mmput(mm);
        return 0;
    }

    MM_READ_LOCK(mm)

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0))
    vma_iter_init(&vmi, mm, 0);
    for_each_vma(vmi, vma)
#else
        for (vma = mm->mmap; vma; vma = vma->vm_next)
#endif
    {
        char *path_nm;
        if (vma->vm_file) {
            path_nm = file_path(vma->vm_file, buf, PATH_MAX - 1);
            if (IS_ERR(path_nm)) {
                continue;
            }

            if (!strcmp(kbasename(path_nm), name)) {
                result = vma->vm_start;
                goto ret;
            }

        }
    }

    ret:
    MM_READ_UNLOCK(mm)

    mmput(mm);
    kfree(buf);
    return result;
}

phys_addr_t vaddr_to_phy_addr(struct mm_struct *mm, uintptr_t va) {
    pte_t *ptep;
    phys_addr_t page_addr;
    uintptr_t page_offset;

    ptep = page_from_virt_user(mm, va);

    if (!pte_present(*ptep)) {
        return 0;
    }

    // #define __pte_to_phys(pte)	(pte_val(pte) & PTE_ADDR_MASK)
    page_offset = va & (PAGE_SIZE - 1);
#if defined(__pte_to_phys)
    page_addr = (phys_addr_t) __pte_to_phys(*ptep);
#elif defined(pte_pfn)
    page_addr = (phys_addr_t) (pte_pfn(*pte) << PAGE_SHIFT);
#else
#error unsupported kernel version：__pte_to_phys or pte_pfn
#endif
    if (page_addr == 0) { // why?
        return 0;
    }

    return page_addr + page_offset;
}

int read_process_memory(pid_t pid, void __user*addr, void __user*dest, size_t size) {
    struct task_struct *task;
    struct mm_struct *mm;
    struct pid *pid_struct;
    phys_addr_t pa;
    int ret;
    void* mapped;

    pid_struct = find_get_pid(pid);
    if (!pid_struct) {
        return -1;
    }

    task = get_pid_task(pid_struct, PIDTYPE_PID);

    //rcu_read_lock();
    //task = pid_task(find_vpid(pid), PIDTYPE_PID);
    //rcu_read_unlock();
    // 为什么不用上面的方式获取task_struct?
    // 上面的方式获取不会让记数+1！要改吗？随便你咯

    put_pid(pid_struct);
    if(!task) {
        return -2;
    }

    mm = get_task_mm(task);
    put_task_struct(task);
    if (!mm) {
        return -3;
    }

    ret = -4;
    MM_READ_LOCK(mm)
    pa = vaddr_to_phy_addr(mm, (uintptr_t)addr);
    MM_READ_UNLOCK(mm)
    mmput(mm);

    if (pa && pfn_valid(__phys_to_pfn(pa)) && IS_VALID_PHYS_ADDR_RANGE(pa, size)){
        // why not use kmap_atomic?
        // '/proc/vmstat' -> nr_isolated_anon & nr_isolated_file
        // There is a quantity limit, it will panic when used up!
        mapped = ioremap_cache(pa, size);
        if (mapped && !copy_to_user(dest, mapped, size)) {
            ret = 0;
        }
        if (mapped) {
            iounmap(mapped);
        }
    }

    return ret;
}

int write_process_memory(pid_t pid, void __user*addr, void __user*src, size_t size) {
    struct task_struct *task;
    struct mm_struct *mm;
    struct pid *pid_struct;
    phys_addr_t pa;
    int ret;
    void* mapped;

    pid_struct = find_get_pid(pid);
    if (!pid_struct) {
        return -1;
    }

    task = get_pid_task(pid_struct, PIDTYPE_PID);
    put_pid(pid_struct);
    if(!task) {
        return -2;
    }

    mm = get_task_mm(task);
    put_task_struct(task);
    if (!mm) {
        return -3;
    }

    ret = -4;
    MM_READ_LOCK(mm)
    pa = vaddr_to_phy_addr(mm, (uintptr_t)addr);
    MM_READ_UNLOCK(mm)
    mmput(mm);

    if (pa && pfn_valid(__phys_to_pfn(pa)) && IS_VALID_PHYS_ADDR_RANGE(pa, size)){
        // why not use kmap_atomic?
        // '/proc/vmstat' -> nr_isolated_anon & nr_isolated_file
        // There is a quantity limit, it will panic when used up!
        mapped = ioremap_cache(pa, size);
        if (mapped && !copy_from_user(mapped, src, size)) {
            ret = 0;
        }
        if (mapped) {
            iounmap(mapped);
        }
    }
    return ret;
}

int access_process_vm_by_pid(pid_t from, void __user*from_addr, pid_t to, void __user*to_addr, size_t size) {
    struct task_struct *task;
    char __kernel *buf;
    int ret;

    rcu_read_lock();
    // find_vpid() does not take a reference to the pid, so we must hold RCU
    task = pid_task(find_vpid(from), PIDTYPE_PID);
    rcu_read_unlock();

    if (!task || !task->mm) return -ESRCH;

    buf = vmalloc(size);
    if (!buf) return -ENOMEM;

    ret = access_process_vm(task, (unsigned long) from_addr, buf, (int) size, 0);
    if (ret != size) {
        vfree(buf);
        return -EIO;
    }

    rcu_read_lock();
    task = pid_task(find_vpid(to), PIDTYPE_PID);
    rcu_read_unlock();

    if (!task || !task->mm) {
        vfree(buf);
        return -ESRCH;
    }

    ret = access_process_vm(task, (unsigned long) to_addr, buf, (int) size, FOLL_WRITE);
    if (ret != size) {
        vfree(buf);
        return -EIO;
    }

    vfree(buf);
    return 0;
}
