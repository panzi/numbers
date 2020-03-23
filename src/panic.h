/**
 *    numbers - a countdown numbers game solver
 *    Copyright (C) 2020  Mathias Panzenböck
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
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
