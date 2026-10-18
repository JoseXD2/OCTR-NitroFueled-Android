#pragma once
#include <fmt/format.h>
#define LOG_ERROR(c, ...) fmt::print(stderr, __VA_ARGS__), fputc('\n', stderr)
