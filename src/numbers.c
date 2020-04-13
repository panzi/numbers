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
#include <semaphore.h>
#include <limits.h>

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

struct ThreadManagerS;

typedef struct NumbersCtxS {
	Number                 target;
	const Number          *numbers;
	Index                  count;
	size_t                 used_mask;
	Index                  used_count;
	Element               *ops;
	Index                  ops_size;
	Index                  ops_index;
	ValElement            *vals;
	Index                  vals_size;
	Index                  vals_index;
	volatile bool          active;
	volatile bool          alive;
	struct ThreadManagerS *mngr;
	pthread_t              thread;
	sem_t                  semaphore;
} NumbersCtx;

typedef struct ThreadManagerS {
	size_t           thread_count;
	volatile size_t  active_count;
	NumbersCtx      *solvers;
	PrintStyle       print_style;
	pthread_mutex_t  iolock;
	pthread_mutex_t  worker_lock;
	sem_t            semaphore;
} ThreadManager;

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
			(ctx->mngr->print_style == PrintParen && ctx->ops[lhs_index - 1].op != OpVal);
		const bool right_paren = rhs_precedence < this_precedence ||
			(ctx->mngr->print_style == PrintParen && ctx->ops[index - 1].op != OpVal);

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
		int errnum = pthread_mutex_lock(&ctx->mngr->iolock);
		if (errnum != 0) {
			panicf("locking io mutex: %s", strerror(errnum));
		}

		switch (ctx->mngr->print_style) {
			case PrintRpn:   print_solution_rpn(ctx);  break;
			case PrintExpr:  print_solution_expr(ctx); break;
			case PrintParen: print_solution_expr(ctx); break;
			default: assert(false);
		}

		errnum = pthread_mutex_unlock(&ctx->mngr->iolock);
		if (errnum != 0) {
			panicf("unlocking io mutex: %s", strerror(errnum));
		}
	}
}

static void solve_vals_internal(NumbersCtx *ctx);

static inline void solve_vals(NumbersCtx *ctx) {
	if (ctx->used_count < ctx->count) {
		solve_vals_internal(ctx);
	}
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

void solve_vals_internal(NumbersCtx *ctx) {
	// I thought I could use a max_used_mask instead of tracking used_cout,
	// but it somehow made it slower!?
	ThreadManager *mngr = ctx->mngr;
	const size_t used = ctx->used_mask;
	const Index count = ctx->count;
	size_t mask = 1;
	// I thought I can move ++/-- ctx->vals_index and ++/-- ctx->used_count
	// out of the loop, but it made it somehow slower!?
	for (Index index = 0; index < count; ++ index) {
		if ((used & mask) == 0) {
			ctx->used_mask = used | mask;
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

			if (ctx->used_count < ctx->count) {
				// + 3 prooved to be a good balance to reduce thread communication overhead at recursion leaves
				if (ctx->used_count + 3 < ctx->count && mngr->active_count < mngr->thread_count) { // fast test
					int errnum = pthread_mutex_lock(&ctx->mngr->worker_lock);
					if (errnum != 0) {
						panicf("locking worker synchronization mutex: %s", strerror(errnum));
					}

					// safe test
					const bool fork_solver = mngr->active_count < mngr->thread_count;
					if (fork_solver) {
						size_t thread_index = 0;
						for (; thread_index < mngr->thread_count; ++ thread_index) {
							if (!mngr->solvers[thread_index].active) {
								break;
							}
						}

						assert(thread_index < mngr->thread_count);

						NumbersCtx *other = &mngr->solvers[thread_index];
						other->used_mask  = ctx->used_mask;
						other->used_count = ctx->used_count;
						other->ops_index  = ctx->ops_index;
						other->vals_index = ctx->vals_index;

						memcpy(other->ops,  ctx->ops,  sizeof(Element)    * ctx->ops_index);
						memcpy(other->vals, ctx->vals, sizeof(ValElement) * ctx->vals_index);

						other->active = true;
						other->mngr->active_count ++;

						if (sem_post(&other->semaphore) != 0) {
							panice("posting to semaphore of worker thread %zu", thread_index);
						}
					}

					errnum = pthread_mutex_unlock(&ctx->mngr->worker_lock);
					if (errnum != 0) {
						panicf("unlocking worker synchronization mutex: %s", strerror(errnum));
					}

					if (!fork_solver) {
						solve_vals_internal(ctx);
					}
				} else {
					solve_vals_internal(ctx);
				}
			}

			-- ctx->vals_index;
			pop_op(ctx);
			ctx->used_mask = used;
			-- ctx->used_count;
		}
		mask <<= 1;
	}
}

static void* worker_proc(void *ptr) {
	NumbersCtx *ctx = (NumbersCtx*)ptr;
	for (;;) {
		if (sem_wait(&ctx->semaphore) != 0) {
			panice("worker waiting for work");
		}

		if (!ctx->alive) {
			break;
		}

		solve_vals(ctx);

		int errnum = pthread_mutex_lock(&ctx->mngr->worker_lock);
		if (errnum != 0) {
			panicf("locking worker synchronization mutex: %s", strerror(errnum));
		}

		ctx->active = false;
		ctx->mngr->active_count --;

		if (ctx->mngr->active_count == 0) {
			if (sem_post(&ctx->mngr->semaphore) != 0) {
				panice("posting to thread manager semaphore");
			}
		}

		errnum = pthread_mutex_unlock(&ctx->mngr->worker_lock);
		if (errnum != 0) {
			panicf("unlocking worker synchronization mutex: %s", strerror(errnum));
		}
	}

	return NULL;
}

void solve(const Number target, const Number numbers[], const Index count, size_t threads, PrintStyle print_style) {
	if (count == 0) {
		panicf("need at least one number");
	}

	if (threads == 0) {
		panicf("need at least one thread");
	}

	const Index ops_size = count + count - 1;
	const Index vals_size = count;

	NumbersCtx *solvers = calloc(threads, sizeof(NumbersCtx));
	if (!solvers) {
		panice("allocating contexts %zu", threads);
	}

	ThreadManager mngr = {
		.thread_count = threads,
		.active_count = 0,
		.solvers      = solvers,
		.print_style  = print_style,
	};

	int errnum = pthread_mutex_init(&mngr.iolock, NULL);
	if (errnum != 0) {
		panicf("initializing io mutex: %s", strerror(errnum));
	}

	errnum = pthread_mutex_init(&mngr.worker_lock, NULL);
	if (errnum != 0) {
		panicf("initializing worker synchronization mutex: %s", strerror(errnum));
	}

	if (sem_init(&mngr.semaphore, 0, 0) != 0) {
		panice("initializing semaphore of thread manager");
	}

	for (size_t thread_index = 0; thread_index < threads; ++ thread_index) {
		Element *ops = calloc(ops_size, sizeof(Element));
		if (!ops) {
			panice("allocating operand stack of size %u", ops_size);
		}

		ValElement *vals = calloc(vals_size, sizeof(ValElement));
		if (!vals) {
			panice("allocating value stack of size %u", vals_size);
		}

		NumbersCtx *solver = &solvers[thread_index];

		*solver = (NumbersCtx){
			.target      = target,
			.numbers     = numbers,
			.count       = count,
			.used_mask   = 0,
			.used_count  = 0,
			.ops         = ops,
			.ops_size    = ops_size,
			.ops_index   = 0,
			.vals        = vals,
			.vals_size   = vals_size,
			.vals_index  = 0,
			.active      = false,
			.alive       = true,
			.mngr        = &mngr,
		};

		if (sem_init(&solver->semaphore, 0, 0) != 0) {
			panice("initializing semaphore of worker thread %zu", thread_index);
		}

		errnum = pthread_create(&solver->thread, NULL, &worker_proc, solver);
		if (errnum != 0) {
			panicf("starting worker thread %zu: %s", thread_index, strerror(errnum));
		}
	}

	solvers[0].active = true;
	solvers[0].mngr->active_count ++;

	if (sem_post(&solvers[0].semaphore) != 0) {
		panice("posting to semaphore of worker thread 0");
	}

	if (sem_wait(&mngr.semaphore) != 0) {
		panice("waiting on thread manager semaphore");
	}

	for (size_t thread_index = 0; thread_index < threads; ++ thread_index) {
		NumbersCtx *solver = &solvers[thread_index];

		if (solver->alive) {
			solver->alive = false;

			if (sem_post(&solver->semaphore) != 0) {
				panice("posting to semaphore of worker thread %zu", thread_index);
			}
		}
	}

	for (size_t thread_index = 0; thread_index < threads; ++ thread_index) {
		NumbersCtx *solver = &solvers[thread_index];

		errnum = pthread_join(solver->thread, NULL);
		if (errnum != 0) {
			fprintf(stderr, "wating for worker thread %zu to end: %s\n", thread_index, strerror(errnum));
		}

		if (sem_destroy(&solver->semaphore) != 0) {
			fprintf(stderr, "freeing semaphore of worker thread %zu: %s\n", thread_index, strerror(errno));
		}

		free(solver->ops);
		free(solver->vals);
	}

	free(solvers);

	if (sem_destroy(&mngr.semaphore) != 0) {
		panice("freeing semaphore of thread manager");
	}

	errnum = pthread_mutex_destroy(&mngr.worker_lock);
	if (errnum != 0) {
		panicf("destroying io mutex: %s", strerror(errnum));
	}

	errnum = pthread_mutex_destroy(&mngr.iolock);
	if (errnum != 0) {
		panicf("destroying io mutex: %s", strerror(errnum));
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

unsigned long parse_number(const char *str, const char *error_message) {
	errno = 0;
	char *endptr = NULL;
	long long value = strtoll(str, &endptr, 10);
	if (errno != 0) {
		panice("%s: %s", error_message, str);
	} else if (!*str || *endptr || value <= 0
#if ULONG_MAX < LLONG_MAX
		|| value > (long long)ULONG_MAX)
#endif
	) {
		panicf("%s: %s", error_message, str);
	}
	return (unsigned long) value;
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
	bool threads_from_numbers = false;
#else
	bool threads_from_numbers = true;
#endif

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
				if (strcasecmp(optarg, "numbers") == 0) {
					threads_from_numbers = true;
					threads = 0;
				} else if (strcasecmp(optarg, "cpus") == 0) {
					threads_from_numbers = false;
					threads = 0;
				} else {
					threads = parse_number(optarg, "illegal thread count");
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

	const Number target = parse_number(argv[optind], "target is not a valid numbers game number");
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
		const Number number = parse_number(argv[index], "number is not a valid numbers game number");
		numbers[index - optind] = number;
	}

	solve(target, numbers, count, threads, print_style);
	free(numbers);

	return 0;
}
