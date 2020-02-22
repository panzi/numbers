#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <getopt.h>
#include <pthread.h>

#include "panic.h"

#if defined(_WIN16) || defined(_WIN32) || defined(_WIN64)
#define __WINDOWS__
#endif

#if !defined(__WINDOWS__)
#include <unistd.h>
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

typedef struct NumbersCtxS {
	Number           target;
	const Number    *numbers;
	bool            *used;
	size_t           count;
	Element         *ops;
	size_t           ops_size;
	size_t           ops_index;
	Number          *vals;
	size_t           vals_size;
	size_t           vals_index;
	PrintStyle       print_style;
	pthread_mutex_t *iolock;
} NumbersCtx;

static void print_solution_rpn(const NumbersCtx *ctx) {
	for (size_t index = 0; index < ctx->ops_index; ++ index) {
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

static void push_op(NumbersCtx *ctx, Op op, Number value) {
	assert(ctx->ops_index < ctx->ops_size);

	ctx->ops[ctx->ops_index].op    = op;
	ctx->ops[ctx->ops_index].value = value;
	++ ctx->ops_index;
}

static void pop_op(NumbersCtx *ctx) {
	assert(ctx->ops_index > 0);
	-- ctx->ops_index;
}

static size_t get_expr_end(const NumbersCtx *ctx, size_t index) {
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

static void print_expr(const NumbersCtx *ctx, size_t index) {
	const Op op = ctx->ops[index].op;

	if (op == OpVal) {
		printf("%lu", ctx->ops[index].value);
	} else {
		assert(index > 0);
		const size_t lhs_index = get_expr_end(ctx, index - 1);
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
	size_t index = ctx->ops_index;
	assert(index > 0);
	print_expr(ctx, index - 1);
	putchar('\n');
}

static void test_solution(NumbersCtx *ctx) {
	if (ctx->vals_index == 1 && ctx->target == ctx->vals[0]) {
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

static void solve_vals_range(NumbersCtx *ctx, size_t start_index, size_t end_index);

static inline void solve_vals(NumbersCtx *ctx) {
	solve_vals_range(ctx, 0, ctx->count);
}

static void solve_ops(NumbersCtx *ctx) {
	if (ctx->vals_index > 1) {
		const Number lhs = ctx->vals[ctx->vals_index - 2];
		const Number rhs = ctx->vals[ctx->vals_index - 1];
		Number value = 0;

		-- ctx->vals_index;
		if (lhs >= rhs) {
			// intermediate reuslts need to be in descending order
			const Element *top_op = &ctx->ops[ctx->ops_index - 1];
			//   discard  ==    use
			// X Y Z + +  ==  X Y + Z +
			// X Y Z - +  ==  Y Z - X +
			// X Y Z + -  ==  X Y - Z -
			// X Y Z - -  ==  X Y - Z +  EXCEPT FOR WHEN X - Y WOULD BE NEGATIVE!!
			//                           Negative intermediate results are forbidden.
			if (top_op->op != OpAdd) {
				if (top_op->op != OpSub && !(top_op->op == OpVal &&
				      ctx->ops[ctx->ops_index - 2].op == OpAdd &&
				      ctx->ops[ctx->ops_index - 3].op == OpVal &&
				      ctx->ops[ctx->ops_index - 3].value < top_op->value)) {
					// chains of additions need to be in descending order
					value = ctx->vals[ctx->vals_index - 1] = lhs + rhs;
					push_op(ctx, OpAdd, value);
					test_solution(ctx);
					solve_ops(ctx);
					solve_vals(ctx);
					pop_op(ctx);
				}

				// V = top_op->value
				// Z = ctx->ops[ctx->ops_index - 2].value
				// Y - Z = V
				// Y = V + Z
				if ((top_op->op != OpSub || lhs < (top_op->value + ctx->ops[ctx->ops_index - 2].value)) && lhs != rhs) {
					// a intermediate result of 0 is useless
					if (!(top_op->op == OpVal &&
					      (ctx->ops[ctx->ops_index - 2].op == OpAdd || ctx->ops[ctx->ops_index - 2].op == OpSub) &&
					      ctx->ops[ctx->ops_index - 3].op == OpVal &&
					      ctx->ops[ctx->ops_index - 3].value < top_op->value)) {
						// chains of subdivisions/additions need to be in descending order
						value = ctx->vals[ctx->vals_index - 1] = lhs - rhs;
						push_op(ctx, OpSub, value);
						test_solution(ctx);
						solve_ops(ctx);
						solve_vals(ctx);
						pop_op(ctx);
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
				if (top_op->op != OpMul && top_op->op != OpDiv) {
					if (!(top_op->op == OpVal &&
					      ctx->ops[ctx->ops_index - 2].op == OpMul &&
					      ctx->ops[ctx->ops_index - 3].op == OpVal &&
					      ctx->ops[ctx->ops_index - 3].value < top_op->value)) {
						// chains of multiplications need to be in descending order
						value = ctx->vals[ctx->vals_index - 1] = lhs * rhs;
						push_op(ctx, OpMul, value);
						test_solution(ctx);
						solve_ops(ctx);
						solve_vals(ctx);
						pop_op(ctx);
					}

					if (lhs % rhs == 0) {
						// only whole numbers as intermediate results allowed
						if (!(top_op->op == OpVal &&
						      (ctx->ops[ctx->ops_index - 2].op == OpMul || ctx->ops[ctx->ops_index - 2].op == OpDiv) &&
						      ctx->ops[ctx->ops_index - 3].op == OpVal &&
						      ctx->ops[ctx->ops_index - 3].value < top_op->value)) {
							// chains of multiplications/divisions need to be in descending order
							value = ctx->vals[ctx->vals_index - 1] = lhs / rhs;
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
		ctx->vals[ctx->vals_index - 1] = rhs;
		ctx->vals[ctx->vals_index - 2] = lhs;
	}
}

static void solve_vals_range(NumbersCtx *ctx, size_t start_index, size_t end_index) {
	for (size_t index = start_index; index < end_index; ++ index) {
		if (!ctx->used[index]) {
			ctx->used[index] = true;
			const Number number = ctx->numbers[index];
			push_op(ctx, OpVal, number);
			assert(ctx->vals_index < ctx->vals_size);
			ctx->vals[ctx->vals_index] = number;
			++ ctx->vals_index;

			test_solution(ctx);
			solve_ops(ctx);
			solve_vals(ctx);

			-- ctx->vals_index;
			pop_op(ctx);
			ctx->used[index] = false;
		}
	}
}

typedef struct ThreadCtxS {
	NumbersCtx ctx;
	size_t start_index;
	size_t end_index;
	pthread_t thread;
} ThreadCtx;

static void* worker_proc(void *ptr) {
	ThreadCtx *worker = (ThreadCtx*)ptr;
	solve_vals_range(&worker->ctx, worker->start_index, worker->end_index);
	return NULL;
}

void solve(const Number target, const Number numbers[], const size_t count, size_t threads, PrintStyle print_style) {
	if (count == 0) {
		panicf("need at least one number");
	}

	if (threads == 0) {
		panicf("need at least one thread");
	}

	if (threads > count) {
		threads = count;
	}

	const size_t ops_size = count + count - 1;
	const size_t vals_size = count;

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
			panice("allocating operand stack of size %zu", ops_size);
		}

		Number *vals = calloc(vals_size, sizeof(Number));
		if (!vals) {
			panice("allocating value stack of size %zu", vals_size);
		}

		bool *used = calloc(count, sizeof(bool));
		if (!used) {
			panice("allocating used array of size %zu", count);
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
			.used        = used,
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
		free(worker->ctx.used);
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
		"\t-t, --threads=COUNT    Spawn COUNT threads. Uses number of CPUs per default.\n"
#else
		"\t-t, --threads=COUNT    Spawn COUNT threads. (default is number count)\n"
#endif
		"\t-r, --rpn              Print solutions in reverse Polish notation.\n"
		"\t-e, --expr             Print solutions in usual notation (default).\n"
		"\t-p, --paren            Like --expr but never skip parenthesis.\n"
		"\n"
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

#ifdef HAS_GET_CPU_COUNT
	size_t threads = get_cpu_count();
#else
	size_t threads = 0;
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
				threads = strtoul(optarg, &endptr, 10);
				if (!*optarg || *endptr || threads == 0) {
					panice("illegal thread count: %s", optarg);
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

#ifndef HAS_GET_CPU_COUNT
	if (threads == 0) {
		threads = count;
	}
#endif

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
