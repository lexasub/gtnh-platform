#pragma once

#include <cstdio>

extern int g_tests, g_passed, g_failed;

void test_check(bool cond, const char* file, int line, const char* expr, const char* msg = nullptr);

#define CHECK(cond, ...) \
    test_check(!!(cond), __FILE__, __LINE__, #cond, ##__VA_ARGS__)

#define CHECK_EQ(a, b, ...) \
    test_check((a) == (b), __FILE__, __LINE__, #a " == " #b, ##__VA_ARGS__)

#define CHECK_NE(a, b, ...) \
    test_check((a) != (b), __FILE__, __LINE__, #a " != " #b, ##__VA_ARGS__)

#define CHECK_GE(a, b, ...) \
    test_check((a) >= (b), __FILE__, __LINE__, #a " >= " #b, ##__VA_ARGS__)
