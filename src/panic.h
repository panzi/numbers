#ifndef PANIC_H
#define PANIC_H
#pragma once

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#define panicf(...) \
	{ \
		fprintf(stderr, "%s:%u:%s: ", __FILE__, __LINE__, __func__); \
		fprintf(stderr, __VA_ARGS__); \
		fputc('\n', stderr); \
		exit(1); \
	}

#define panice(...) \
	{ \
		fprintf(stderr, "%s:%u:%s: %s: ", __FILE__, __LINE__, __func__, strerror(errno)); \
		fprintf(stderr, __VA_ARGS__); \
		fputc('\n', stderr); \
		exit(1); \
	}

#endif
