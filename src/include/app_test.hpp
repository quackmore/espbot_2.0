/*
 * ----------------------------------------------------------------------------
 * "THE BEER-WARE LICENSE" (Revision 42):
 * <quackmore-ff@yahoo.com> wrote this file.  As long as you retain this notice
 * you can do whatever you want with this stuff. If we meet some day, and you 
 * think this stuff is worth it, you can buy me a beer in return. Quackmore
 * ----------------------------------------------------------------------------
 */
#ifndef __ESPBOT_TEST_HPP__
#define __ESPBOT_TEST_HPP__

extern "C"
{
#include "ip_addr.h"
}

// #define TEST_FUNCTIONS 1

void init_test(struct ip_addr, uint32, char *);
void test_webclient(void);
void run_test(int32, int32);

#endif