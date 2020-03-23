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
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <getopt.h>
#include <pthread.h>

#include "panic.h"

#if defined(_WIN16) || defined(_WIN32) || defined(_WIN64)
#	define __WINDOWS__
#endif

#if !defined(__WINDOWS__)
#	include <unistd.h>
#endif

#ifdef _SC_NPROCESSORS_ONLN
#	define HAS_GET_CPU_COUNT
static size_t get_cpu_count() {
	const long nprocs = sysconf(_SC_NPROCESSORS_ONLN);
	if (nprocs < 0) {
		panice("getting number of CPUs/cores");
	}
	return (size_t)nprocs;
}
#endif

typedef uint64_t Number;
typedef uint16_t Index;
#define MAX_NUMBERS (sizeof(size_t) * 8)

typedef enum OpE {
	OpVal = '0',
	OpAdd = '+',
	OpSub = '-',
	OpMul = '*',
	OpDiv = '/',
} Op;

typedef enum PrintStyleE {
	PrintRpn,
	PrintExpr,
	PrintParen,
} PrintStyle;

typedef struct ElementS {
	Op op;
	Number value;
} Element;

typedef struct ValElementS {
	Number value;
	size_t ops_index;
} ValElement;

typedef struct NumbersCtxS {
	Number           target;
	const Number    *numbers;
	Index            count;
	size_t           used;
	Index            used_count;
	Element         *ops;
	Index            ops_size;
	Index            ops_index;
	ValElement      *vals;
	Index            vals_size;
	Index            vals_index;
	PrintStyle       print_style;
	pthread_mutex_t *iolock;
} NumbersCtx;

static void print_solution_rpn(const NumbersCtx *ctx) {
	for (Index index = 0; index < ctx->ops_index; ++ index) {
		if (index > 0) {
			putchar(' ');
		}
		switch (ctx->ops[index].op) {
			case OpVal: printf("%lu", ctx->ops[index].value); break;
			case OpAdd: putchar('+'); break;
			case OpSub: putchar('-'); break;
			case OpMul: putchar('*'); break;
			case OpDiv: putchar('/'); break;
			default: assert(false);
		}
	}
	putchar('\n');
}

static inline void push_op(NumbersCtx *ctx, Op op, Number value) {
	assert(ctx->ops_index < ctx->ops_size);

	ctx->ops[ctx->ops_index] = (Element){
		.op    = op,
		.value = value,
	};
	++ ctx->ops_index;
}

static inline void pop_op(NumbersCtx *ctx) {
	assert(ctx->ops_index > 0);
	-- ctx->ops_index;
}

static Index get_expr_end(const NumbersCtx *ctx, Index index) {
	const Op op = ctx->ops[index].op;
	if (op == OpVal) {
		return index;
	} else {
		assert(index > 0);
		index = get_expr_end(ctx, index - 1);
		assert(index > 0);
		return get_expr_end(ctx, index - 1);
	}
}

static int get_precedence(Op op) {
	switch (op) {
		case OpVal: return 1;
		case OpAdd: return 0;
		case OpSub: return 0;
		case OpMul: return 1;
		case OpDiv: return 1;
		default:
			assert(false);
			return 2;
	}
}

static void print_expr(const NumbersCtx *ctx, Index index) {
	const Op op = ctx->ops[index].op;

	if (op == OpVal) {
		printf("%lu", ctx->ops[index].value);
	} else {
		assert(index > 0);
		const Index lhs_index = get_expr_end(ctx, index - 1);
		assert(lhs_index > 0);
		const int this_precedence = get_precedence(ctx->ops[index].op);
		const int lhs_precedence  = get_precedence(ctx->ops[lhs_index - 1].op);
		const int rhs_precedence  = get_precedence(ctx->ops[index - 1].op);

		const bool left_paren = lhs_precedence < this_precedence ||
			(ctx->print_style == PrintParen && ctx->ops[lhs_index - 1].op != OpVal);
		const bool right_paren = rhs_precedence < this_precedence ||
			(ctx->print_style == PrintParen && ctx->ops[index - 1].op != OpVal);

		if (left_paren) {
			putchar('(');
		}
		print_expr(ctx, lhs_index - 1);
		if (left_paren) {
			putchar(')');
		}

		switch (op) {
			case OpAdd: printf(" + "); break;
			case OpSub: printf(" - "); break;
			case OpMul: printf(" * "); break;
			case OpDiv: printf(" / "); break;
			default: assert(false);
		}

		if (right_paren) {
			putchar('(');
		}
		print_expr(ctx, index - 1);
		if (right_paren) {
			putchar(')');
		}
	}
}

static void print_solution_expr(const NumbersCtx *ctx) {
	Index index = ctx->ops_index;
	assert(index > 0);
	print_expr(ctx, index - 1);
	putchar('\n');
}

static void test_solution(NumbersCtx *ctx) {
	if (ctx->vals_index == 1 && ctx->target == ctx->vals[0].value) {
		int errnum = 0;
		if (ctx->iolock) {
			errnum = pthread_mutex_lock(ctx->iolock);
			if (errnum != 0) {
				panicf("%s: locking io mutex", strerror(errnum));
			}
		}

		switch (ctx->print_style) {
			case PrintRpn:   print_solution_rpn(ctx);  break;
			case PrintExpr:  print_solution_expr(ctx); break;
			case PrintParen: print_solution_expr(ctx); break;
			default: assert(false);
		}

		if (ctx->iolock) {
			errnum = pthread_mutex_unlock(ctx->iolock);
			if (errnum != 0) {
				panicf("%s: unlocking io mutex", strerror(errnum));
			}
		}
	}
}

static void solve_vals_range(NumbersCtx *ctx, Index start_index, Index end_index);

static inline void solve_vals(NumbersCtx *ctx) {
	solve_vals_range(ctx, 0, ctx->count);
}

static void solve_ops(NumbersCtx *ctx) {
	if (ctx->vals_index > 1) {
		const ValElement *lhs_val = &ctx->vals[ctx->vals_index - 2];
		const ValElement *rhs_val = &ctx->vals[ctx->vals_index - 1];
		const Number lhs = lhs_val->value;
		const Number rhs = rhs_val->value;

		if (lhs >= rhs) {
			const Index lhs_ops_index = lhs_val->ops_index;
			const Index rhs_ops_index = rhs_val->ops_index;
			const Element *lhs_op = &ctx->ops[lhs_ops_index];
			const Element *rhs_op = &ctx->ops[rhs_ops_index];
			Number value = 0;

			-- ctx->vals_index;
			// intermediate results need to be in descending order
			//   discard  ==    use
			// X Y Z + +  ==  X Y + Z +
			// X Y Z - +  ==  Y Z - X +
			// X Y Z + -  ==  X Y - Z -
			// X Y Z - -  ==  X Y - Z +  EXCEPT FOR WHEN X - Y WOULD BE NEGATIVE!!
			//                           Negative intermediate results are forbidden.
			if (rhs_op->op != OpAdd) {
				if (rhs_op->op != OpSub && !(
					(lhs_op->op == OpAdd && ctx->ops[lhs_ops_index - 1].value < rhs) ||
					(lhs_op->op == OpSub))) {
					// chains of additions need to be in descending order
					value = lhs + rhs;
					ctx->vals[ctx->vals_index - 1] = (ValElement){
						.value = value,
						.ops_index = ctx->ops_index,
					};
					push_op(ctx, OpAdd, value);
					test_solution(ctx);
					solve_ops(ctx);
					solve_vals(ctx);
					pop_op(ctx);
				}

				// V = top_op->value = rhs
				// Z = ctx->ops[ctx->ops_index - 2].value
				// Y - Z = V
				// Y = V + Z
				if ((rhs_op->op != OpSub || lhs < (rhs + ctx->ops[rhs_ops_index - 1].value)) &&
				    lhs != rhs) {
					// a intermediate result of 0 is useless
					if (!(lhs_op->op == OpSub && ctx->ops[lhs_ops_index - 1].value < rhs)) {
						// chains of subdivisions/additions need to be in descending order
						value = lhs - rhs;
						if (value != rhs) {
							// X - Y = Y is just a roundabout way to write Y
							ctx->vals[ctx->vals_index - 1] = (ValElement){
								.value = value,
								.ops_index = ctx->ops_index,
							};
							push_op(ctx, OpSub, value);
							test_solution(ctx);
							solve_ops(ctx);
							solve_vals(ctx);
							pop_op(ctx);
						}
					}
				}
			}

			if (rhs != 1) {
				// X * 1 and X / 1 are useless

				//   discard  ==    use
				// X Y Z * *  ==  X Y * Z *
				// X Y Z / *  ==  Y Z / X *
				// X Y Z * /  ==  X Y / Z /
				// X Y Z / /  ==  X Y / Z *
				if (rhs_op->op != OpMul && rhs_op->op != OpDiv) {
					if (!((lhs_op->op == OpMul && ctx->ops[lhs_ops_index - 1].value < rhs) ||
					      (lhs_op->op == OpDiv))) {
						// chains of multiplications need to be in descending order
						value = lhs * rhs;
						ctx->vals[ctx->vals_index - 1] = (ValElement){
							.value = value,
							.ops_index = ctx->ops_index,
						};
						push_op(ctx, OpMul, value);
						test_solution(ctx);
						solve_ops(ctx);
						solve_vals(ctx);
						pop_op(ctx);
					}

					if (lhs % rhs == 0) {
						// only whole numbers as intermediate results allowed
						if (!(lhs_op->op == OpDiv && ctx->ops[lhs_ops_index - 1].value < rhs)) {
							// chains of multiplications/divisions need to be in descending order
							value = lhs / rhs;
							if (value != rhs) {
								// X / Y = Y is just a roundabout way to write Y
								ctx->vals[ctx->vals_index - 1] = (ValElement){
									.value = value,
									.ops_index = ctx->ops_index,
								};
								push_op(ctx, OpDiv, value);
								test_solution(ctx);
								solve_ops(ctx);
								solve_vals(ctx);
								pop_op(ctx);
							}
						}
					}
				}
			}
			++ ctx->vals_index;
			ctx->vals[ctx->vals_index - 1] = (ValElement){ .value = rhs, .ops_index = rhs_ops_index };
			ctx->vals[ctx->vals_index - 2] = (ValElement){ .value = lhs, .ops_index = lhs_ops_index };
		}
	}
}

static void solve_vals_range(NumbersCtx *ctx, Index start_index, Index end_index) {
	// I thought I could use a max_used_mask instead of tracking used_cout,
	// but it somehow made it slower!?
	if (ctx->used_count < ctx->count) {
		const size_t used = ctx->used;
		size_t mask = 1 << start_index;
		// I thought I can move ++/-- ctx->vals_index and ++/-- ctx->used_count
		// out of the loop, but it made it somehow slower!?
		for (Index index = start_index; index < end_index; ++ index) {
			if ((used & mask) == 0) {
				ctx->used = used | mask;
				++ ctx->used_count;
				const Number number = ctx->numbers[index];
				assert(ctx->vals_index < ctx->vals_size);
				ctx->vals[ctx->vals_index] = (ValElement){
					.value = number,
					.ops_index = ctx->ops_index,
				};
				push_op(ctx, OpVal, number);
				++ ctx->vals_index;

				test_solution(ctx);
				solve_ops(ctx);
				solve_vals(ctx);

				-- ctx->vals_index;
				pop_op(ctx);
				ctx->used = used;
				-- ctx->used_count;
			}
			mask <<= 1;
		}
	}
}

typedef struct ThreadCtxS {
	NumbersCtx ctx;
	Index start_index;
	Index end_index;
	pthread_t thread;
} ThreadCtx;

static void* worker_proc(void *ptr) {
	ThreadCtx *worker = (ThreadCtx*)ptr;
	solve_vals_range(&worker->ctx, worker->start_index, worker->end_index);
	return NULL;
}

void solve(const Number target, const Number numbers[], const Index count, size_t threads, PrintStyle print_style) {
	if (count == 0) {
		panicf("need at least one number");
	}

	if (threads == 0) {
		panicf("need at least one thread");
	}

	if (threads > count) {
		threads = count;
	}

	const Index ops_size = count + count - 1;
	const Index vals_size = count;

	ThreadCtx *workers = calloc(threads, sizeof(ThreadCtx));
	if (!workers) {
		panice("allocating contexts %zu", threads);
	}

	pthread_mutex_t iolock;
	int errnum = pthread_mutex_init(&iolock, NULL);
	if (errnum != 0) {
		panicf("%s: initializing io mutex", strerror(errnum));
	}

	const size_t stride = 1 + ((count - 1) / threads);
	size_t index = 0;
	for (size_t thread_index = 0; thread_index < threads; ++ thread_index) {
		Element *ops = calloc(ops_size, sizeof(Element));
		if (!ops) {
			panice("allocating operand stack of size %u", ops_size);
		}

		ValElement *vals = calloc(vals_size, sizeof(ValElement));
		if (!vals) {
			panice("allocating value stack of size %u", vals_size);
		}

		ThreadCtx *worker = &workers[thread_index];
		worker->start_index = index;
		index += stride;
		worker->end_index = index;
		if (worker->end_index > count) {
			worker->end_index = count;
		}

		worker->ctx = (NumbersCtx){
			.target      = target,
			.numbers     = numbers,
			.used        = 0,
			.used_count  = 0,
			.count       = count,
			.ops         = ops,
			.ops_size    = ops_size,
			.ops_index   = 0,
			.vals        = vals,
			.vals_size   = vals_size,
			.vals_index  = 0,
			.print_style = print_style,
			.iolock      = &iolock,
		};

		errnum = pthread_create(&worker->thread, NULL, &worker_proc, worker);
		if (errnum != 0) {
			panicf("%s: starting worker therad %zu", strerror(errnum), thread_index);
		}
	}

	for (size_t thread_index = 0; thread_index < threads; ++ thread_index) {
		ThreadCtx *worker = &workers[thread_index];
		const int errnum = pthread_join(worker->thread, NULL);
		if (errnum != 0) {
			fprintf(stderr, "wating for worker thread to end: %s\n", strerror(errnum));
		}

		free(worker->ctx.ops);
		free(worker->ctx.vals);
	}

	free(workers);

	errnum = pthread_mutex_destroy(&iolock);
	if (errnum != 0) {
		panicf("%s: destroying io mutex", strerror(errnum));
	}
}

static void usage(int argc, char *const argv[]) {
	printf("Usage: %s [OPTIONS] TARGET NUMBER...\n", argc > 0 ? argv[0] : "numbers");
	printf(
		"\n"
		"OPTIONS:\n"
		"\n"
		"\t-h, --help             Print this help message.\n"
#ifdef HAS_GET_CPU_COUNT
		"\t-t, --threads=COUNT    Spawn COUNT threads. (default: cpus)\n"
#else
		"\t-t, --threads=COUNT    Spawn COUNT threads. (default: numbers)\n"
#endif
		"\n"
		"\t                       Special COUNT values:\n"
#ifdef HAS_GET_CPU_COUNT
		"\t                          cpus ...... use number of CPUs (CPU cores)\n"
#endif
		"\t                          numbers ... use number count\n"
		"\n"
		"\t                       Note: Thread count is limited to the number count.\n"
		"\t                       Meaning in a normal numbers game with 6 numbers only\n"
		"\t                       6 threads can be utilized.\n"
		"\n"
		"\t                       Note: If more than 1 thread is used the order of the\n"
		"\t                       results is random.\n"
		"\n"
		"\t-r, --rpn              Print solutions in reverse Polish notation.\n"
		"\t-e, --expr             Print solutions in usual notation (default).\n"
		"\t-p, --paren            Like --expr but never skip parenthesis.\n"
		"\n"
		"numbers  Copyright (C) 2020  Mathias Panzenböck\n"
		"This program comes with ABSOLUTELY NO WARRANTY.\n"
		"This is free software, and you are welcome to redistribute it.\n"
		"For more details see: https://github.com/panzi/numbers\n"
	);
}

int main(int argc, char *argv[]) {
	struct option long_options[] = {
		{"help",    no_argument,       0, 'h'},
		{"threads", required_argument, 0, 't'},
		{"rpn",     no_argument,       0, 'r'},
		{"expr",    no_argument,       0, 'e'},
		{"paren",   no_argument,       0, 'p'},
		{0,         0,                 0,  0 },
	};

	PrintStyle print_style = PrintExpr;
	size_t threads = 0;

#ifdef HAS_GET_CPU_COUNT
	bool threads_from_numbers = true;
#else
	bool threads_from_numbers = false;
#endif

	char *endptr = NULL;
	for(;;) {
		int c = getopt_long(argc, argv, "ht:rep", long_options, NULL);
		if (c == -1)
			break;

		switch (c) {
			case 'h':
				usage(argc, argv);
				return 0;

			case 'r':
				print_style = PrintRpn;
				break;

			case 'e':
				print_style = PrintExpr;
				break;

			case 'p':
				print_style = PrintParen;
				break;

			case 't':
				endptr = NULL;
				if (strcasecmp(optarg, "numbers") == 0) {
					threads_from_numbers = true;
					threads = 0;
				} else if (strcasecmp(optarg, "cpus") == 0) {
					threads_from_numbers = false;
					threads = 0;
				} else {
					threads = strtoul(optarg, &endptr, 10);
					if (!*optarg || *endptr || threads == 0) {
						panice("illegal thread count: %s", optarg);
					}
				}
				break;

			case '?':
				usage(argc, argv);
				return 1;
		}
	}

	if (argc - optind < 2) {
		usage(argc, argv);
		return 1;
	}

	endptr = NULL;
	const Number target = strtoul(argv[optind], &endptr, 10);
	if (!*argv[optind] || *endptr || target == 0) {
		panice("target is not a valid numbers game number: %s", argv[optind]);
	}
	++ optind;

	const size_t count = argc - optind;

	if (count > MAX_NUMBERS) {
		panicf("too many numbers: %zu > %zu", count, MAX_NUMBERS);
	}

	if (threads == 0) {
		if (threads_from_numbers) {
			threads = count;
		} else {
#ifdef HAS_GET_CPU_COUNT
			threads = get_cpu_count();
#else
			panicf("--threads cpus is not supported on this platform");
#endif
		}
	}

	Number *numbers = calloc(count, sizeof(Number));
	if (!numbers) {
		panice("allocating numbers array of size %zu", count);
	}

	for (int index = optind; index < argc; ++ index) {
		endptr = NULL;
		const Number number = strtoul(argv[index], &endptr, 10);
		if (!*argv[index] || *endptr || number == 0) {
			panice("number %d is not a valid numbers game number: %s", index, argv[index]);
		}

		numbers[index - optind] = number;
	}

	solve(target, numbers, count, threads, print_style);
	free(numbers);

	return 0;
}
