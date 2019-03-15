/*
 * ----------------------------------------------------------------------------
 * "THE BEER-WARE LICENSE" (Revision 42):
 * <quackmore-ff@yahoo.com> wrote this file.  As long as you retain this notice
 * you can do whatever you want with this stuff. If we meet some day, and you 
 * think this stuff is worth it, you can buy me a beer in return. Quackmore
 * ----------------------------------------------------------------------------
 */

extern "C"
{
#include "mem.h"
#include "user_interface.h"
}

#include "espbot_debug.hpp"
#include "espbot_global.hpp"

/*
 * DEBUGGER
 */

void ICACHE_FLASH_ATTR Esp_mem::init(void)
{
    uint32 first_stack_var;
    void *heap_var = os_zalloc(1);

    // stack vars
    m_stack_min_addr = (uint32)&first_stack_var;
    m_stack_max_addr = (uint32)&first_stack_var;

    // init heap infos and data structures
    m_heap_start_addr = (uint32)heap_var;
    if (heap_var)
        os_free(heap_var);
    m_max_heap_size = system_get_free_heap_size();
    m_min_heap_size = m_max_heap_size;
    m_heap_objs = 0;
    m_max_heap_objs = 0;

    int ii;
    for (ii = 0; ii < HEAP_ARRAY_SIZE; ii++)
    {
        m_heap_array[ii].size = -1;
        m_heap_array[ii].addr = NULL;
    }
    stack_mon();
}

void ICACHE_FLASH_ATTR Esp_mem::heap_mon(void)
{
    uint32 currentHeap = system_get_free_heap_size();
    //esplog.all("Esp_mem::heap_mon: %d\n", currentHeap);
    if (m_min_heap_size > currentHeap)
        m_min_heap_size = currentHeap;
}

void ICACHE_FLASH_ATTR Esp_mem::stack_mon(void)
{
    uint32 stack_var_addr = (uint32)&stack_var_addr;
    if (stack_var_addr > m_stack_max_addr)
        m_stack_max_addr = stack_var_addr;
    if (stack_var_addr < m_stack_min_addr)
        m_stack_min_addr = stack_var_addr;
}

void *Esp_mem::espbot_zalloc(size_t size)
{
    void *addr = os_zalloc(size);
    if (addr)
    {
        espmem.m_heap_objs++;
        if (espmem.m_heap_objs > espmem.m_max_heap_objs)
            espmem.m_max_heap_objs = espmem.m_heap_objs;
        int idx;
        for (idx = 0; idx < HEAP_ARRAY_SIZE; idx++)
        {
            if (espmem.m_heap_array[idx].size == -1)
            {
                espmem.m_heap_array[idx].size = size;
                espmem.m_heap_array[idx].addr = addr;
                break;
            }
        }
        espmem.stack_mon();
    }
    else
    {
        esplog.fatal("Esp_mem::espbot_zalloc heap exhausted\n");
    }
    espmem.heap_mon();
    return addr;
}

void Esp_mem::espbot_free(void *addr)
{
    espmem.m_heap_objs--;
    os_free(addr);
    int idx;
    for (idx = 0; idx < HEAP_ARRAY_SIZE; idx++)
    {
        if (espmem.m_heap_array[idx].addr == addr)
        {
            // eliminate current item from heap allocated items list
            espmem.m_heap_array[idx].size = -1;
            break;
        }
    }
    espmem.stack_mon();
}

uint32 ICACHE_FLASH_ATTR Esp_mem::get_min_stack_addr(void)
{
    esplog.all("Esp_mem::get_min_stack_addr\n");
    return m_stack_min_addr;
}

uint32 ICACHE_FLASH_ATTR Esp_mem::get_max_stack_addr(void)
{
    esplog.all("Esp_mem::get_max_stack_addr\n");
    return m_stack_max_addr;
}

uint32 ICACHE_FLASH_ATTR Esp_mem::get_start_heap_addr(void)
{
    esplog.all("Esp_mem::get_start_heap_addr\n");
    return m_heap_start_addr;
}

uint32 ICACHE_FLASH_ATTR Esp_mem::get_max_heap_size(void)
{
    esplog.all("Esp_mem::get_max_heap_size\n");
    return m_max_heap_size;
}

uint32 ICACHE_FLASH_ATTR Esp_mem::get_mim_heap_size(void)
{
    esplog.all("Esp_mem::get_mim_heap_size\n");
    return m_min_heap_size;
}

uint32 ICACHE_FLASH_ATTR Esp_mem::get_used_heap_size(void)
{
    esplog.all("Esp_mem::get_used_heap_size\n");
    int idx;
    int used_heap = 0;
    stack_mon();
    for (idx = 0; idx < HEAP_ARRAY_SIZE; idx++)
    {
        if (m_heap_array[idx].size > 0)
            used_heap += m_heap_array[idx].size;
    }
    return used_heap;
}

uint32 ICACHE_FLASH_ATTR Esp_mem::get_heap_objs(void)
{
    esplog.all("Esp_mem::get_heap_objs\n");
    return m_heap_objs;
}

uint32 ICACHE_FLASH_ATTR Esp_mem::get_max_heap_objs(void)
{
    esplog.all("Esp_mem::get_max_heap_objs\n");
    return m_max_heap_objs;
}

struct heap_item *Esp_mem::get_heap_item(List_item item)
{
    esplog.all("get_heap_item\n");
    static int idx = 0;
    stack_mon();
    if (item == first)
    {
        for (idx = 0; idx < HEAP_ARRAY_SIZE; idx++)
            if (m_heap_array[idx].size != -1)
                break;
    }
    else
    {
        for (idx = (idx + 1); idx < HEAP_ARRAY_SIZE; idx++)
            if (m_heap_array[idx].size != -1)
                break;
    }
    if (idx == HEAP_ARRAY_SIZE)
        return NULL;
    else
        return &(m_heap_array[idx]);
}

#ifdef ESPBOT
// C++ wrapper

extern "C" void *call_espbot_zalloc(size_t size) // wrapper function
{
    return espmem.espbot_zalloc(size);
}

extern "C" void call_espbot_free(void *addr) // wrapper function
{
    espmem.espbot_free(addr);
}

#endif