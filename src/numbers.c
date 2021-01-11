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
#define PRIN "lu"
#define MAX_NUMBERS (sizeof(size_t) * 8)

// for generation:
const Number NUMBERS[] = {
	1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10,
	25, 50, 75, 100,
};
#define DEFAULT_NUMBER_COUNT 6

typedef struct TargetRangeS {
	Number start;
	Number end;
} TargetRange;

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
	TargetRange            target;
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
	Index            number_count;
	size_t           thread_count;
	volatile size_t  available_count;
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
			case OpVal: printf("%" PRIN, ctx->ops[index].value); break;
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
		printf("%" PRIN, ctx->ops[index].value);
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
	if (ctx->vals_index == 1) {
		const Number result = ctx->vals[0].value;
		if (ctx->target.start <= result && ctx->target.end >= result) {
			int errnum = pthread_mutex_lock(&ctx->mngr->iolock);
			if (errnum != 0) {
				panicf("locking io mutex: %s", strerror(errnum));
			}

			if (ctx->target.start != ctx->target.end) {
				printf("%" PRIN " = ", result);
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

					// Note: Any good compiler should only generate one div instruction for
					//       the next two lines, since the reminder is just a byproduct of
					//       the division.
					value = lhs / rhs;
					if (lhs % rhs == 0) {
						// only whole numbers as intermediate results allowed
						if (!(lhs_op->op == OpDiv && ctx->ops[lhs_ops_index - 1].value < rhs)) {
							// chains of multiplications/divisions need to be in descending order
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
	// I thought I could use a max_used_mask instead of tracking used_count,
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
				// + 3 proved to be a good balance to reduce thread communication overhead at recursion leafs
				if (ctx->used_count + 3 < ctx->count && mngr->available_count > 0) { // fast test
					int errnum = pthread_mutex_lock(&mngr->worker_lock);
					if (errnum != 0) {
						panicf("locking worker synchronization mutex: %s", strerror(errnum));
					}

					// safe test
					const bool fork_solver = mngr->available_count > 0;
					if (fork_solver) {
						size_t thread_index = 0;
						for (; thread_index < mngr->thread_count; ++ thread_index) {
							if (!mngr->solvers[thread_index].active) {
								break;
							}
						}

						assert(thread_index < mngr->thread_count);

						NumbersCtx *other = &mngr->solvers[thread_index];

						other->active = true;
						mngr->available_count --;

						errnum = pthread_mutex_unlock(&mngr->worker_lock);

						other->used_mask  = ctx->used_mask;
						other->used_count = ctx->used_count;
						other->ops_index  = ctx->ops_index;
						other->vals_index = ctx->vals_index;

						memcpy(other->ops,  ctx->ops,  sizeof(Element)    * ctx->ops_index);
						memcpy(other->vals, ctx->vals, sizeof(ValElement) * ctx->vals_index);

						if (sem_post(&other->semaphore) != 0) {
							panice("posting to semaphore of worker thread %zu", thread_index);
						}
					} else {
						errnum = pthread_mutex_unlock(&mngr->worker_lock);
					}

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

static void* worker_proc_solve(void *ptr) {
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
		assert(ctx->mngr->available_count < ctx->mngr->thread_count);
		const size_t available_count = ++ ctx->mngr->available_count;

		errnum = pthread_mutex_unlock(&ctx->mngr->worker_lock);
		if (errnum != 0) {
			panicf("unlocking worker synchronization mutex: %s", strerror(errnum));
		}

		if (available_count == ctx->mngr->thread_count) {
			if (sem_post(&ctx->mngr->semaphore) != 0) {
				panice("posting to thread manager semaphore");
			}
		}
	}

	return NULL;
}

static void* worker_proc_generate(void *ptr) {
	NumbersCtx *ctx = (NumbersCtx*)ptr;
	for (;;) {
		if (sem_wait(&ctx->semaphore) != 0) {
			panice("worker waiting for work");
		}

		if (!ctx->alive) {
			break;
		}

		solve_vals(ctx);

		ctx->active = false;

		if (sem_post(&ctx->mngr->semaphore) != 0) {
			panice("posting to thread manager semaphore");
		}
	}

	return NULL;
}

static void thread_manager_create(ThreadManager *mngr, const Index count, const size_t threads, const PrintStyle print_style, bool generate);
static void thread_manager_destroy(ThreadManager *mngr);

void solve(ThreadManager *mngr, const TargetRange target, const Number numbers[]) {
	assert(mngr->available_count == mngr->thread_count);

	for (size_t thread_index = 0; thread_index < mngr->thread_count; ++ thread_index) {
		NumbersCtx *solver = &mngr->solvers[thread_index];
		assert(!solver->active);

		solver->target     = target;
		solver->numbers    = numbers;
		solver->used_mask  = 0,
		solver->used_count = 0,
		solver->ops_index  = 0;
		solver->vals_index = 0;
	}

	mngr->solvers[0].active = true;
	mngr->available_count --;

	if (sem_post(&mngr->solvers[0].semaphore) != 0) {
		panice("posting to semaphore of worker thread 0");
	}

	if (sem_wait(&mngr->semaphore) != 0) {
		panice("waiting on thread manager semaphore");
	}
}

void generate(ThreadManager *mngr, const TargetRange target, const Number numbers[]) {
	assert(mngr->available_count == 0);

	size_t thread_index = 0;

	for (;;) {
		for (thread_index = 0; thread_index < mngr->thread_count; ++ thread_index) {
			NumbersCtx *solver = &mngr->solvers[thread_index];

			if (!solver->active) {
				break;
			}
		}

		if (thread_index < mngr->thread_count) {
			NumbersCtx *solver = &mngr->solvers[thread_index];
			solver->target     = target;
			solver->numbers    = numbers;
			solver->used_mask  = 0,
			solver->used_count = 0,
			solver->ops_index  = 0;
			solver->vals_index = 0;
			solver->active     = true;
			break;
		} else {
			if (sem_wait(&mngr->semaphore) != 0) {
				panice("waiting on thread manager semaphore");
			}
		}
	}

	if (sem_post(&mngr->solvers[thread_index].semaphore) != 0) {
		panice("posting to semaphore of worker thread 0");
	}
}

void thread_manager_create(ThreadManager *mngr, const Index count, const size_t threads, const PrintStyle print_style, bool generate) {
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

	*mngr = (ThreadManager) {
		.number_count    = count,
		.thread_count    = threads,
		.available_count = generate ? 0 : threads,
		.solvers         = solvers,
		.print_style     = print_style,
		.iolock          = PTHREAD_MUTEX_INITIALIZER,
		.worker_lock     = PTHREAD_MUTEX_INITIALIZER,
	};

	if (sem_init(&mngr->semaphore, 0, 0) != 0) {
		panice("initializing semaphore of thread manager");
	}

	void* (*worker_proc)(void *) = generate ? &worker_proc_generate : &worker_proc_solve;

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
			.target      = { .start = 0, .end = 0 },
			.numbers     = NULL,
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
			.mngr        = mngr,
		};

		if (sem_init(&solver->semaphore, 0, 0) != 0) {
			panice("initializing semaphore of worker thread %zu", thread_index);
		}

		int errnum = pthread_create(&solver->thread, NULL, worker_proc, solver);
		if (errnum != 0) {
			panicf("starting worker thread %zu: %s", thread_index, strerror(errnum));
		}
	}
}

void thread_manager_destroy(ThreadManager *mngr) {
	for (size_t thread_index = 0; thread_index < mngr->thread_count; ++ thread_index) {
		NumbersCtx *solver = &mngr->solvers[thread_index];

		if (solver->alive) {
			solver->alive = false;

			if (sem_post(&solver->semaphore) != 0) {
				panice("posting to semaphore of worker thread %zu", thread_index);
			}
		}
	}

	int errnum = 0;
	for (size_t thread_index = 0; thread_index < mngr->thread_count; ++ thread_index) {
		NumbersCtx *solver = &mngr->solvers[thread_index];

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

	free(mngr->solvers);

	if (sem_destroy(&mngr->semaphore) != 0) {
		panice("freeing semaphore of thread manager");
	}

	errnum = pthread_mutex_destroy(&mngr->worker_lock);
	if (errnum != 0) {
		panicf("destroying io mutex: %s", strerror(errnum));
	}

	errnum = pthread_mutex_destroy(&mngr->iolock);
	if (errnum != 0) {
		panicf("destroying io mutex: %s", strerror(errnum));
	}
}

static void usage(int argc, char *const argv[]) {
	const char *bin = argc > 0 ? argv[0] : "numbers";
	printf("Usage: %s [OPTIONS] TARGET NUMBER...\n", bin);
	printf("       %s --generate [TARGET]\n", bin);
	printf(
		"\n"
		"TARGET may be a single number or an inclusive range in the form START..END.\n"
		"\n"
		"EXAMPLE:\n"
		"\n"
		"\t%s 100..200 1 2 3 25 50 75\n"
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
		"\t-g, --generate         Generate standard numbers games with %u numbers and\n"
		"\t                       their solutions. If no target is given all targets\n"
		"\t                       from 100 to 999 are iterated over.\n"
		"\n"
		"numbers  Copyright (C) 2020  Mathias Panzenböck\n"
		"This program comes with ABSOLUTELY NO WARRANTY.\n"
		"This is free software, and you are welcome to redistribute it.\n"
		"For more details see: https://github.com/panzi/numbers\n",
		bin, DEFAULT_NUMBER_COUNT
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
		|| value > (long long)ULONG_MAX
#endif
	) {
		panicf("%s: %s", error_message, str);
	}
	return (unsigned long) value;
}

TargetRange parse_target_range(const char *target) {
	TargetRange range = { .start = 100, .end = 999 };

	if (!*target) {
		panicf("target range must not be empty string");
	}

	const char *target_end = NULL;
	if (target[0] == '.' && target[1] == '.') {
		target_end = target + 2;
		if (*target_end) {
			range.end = parse_number(target_end, "target range end is not a valid numbers game number");
		}
	} else {
		errno = 0;
		long long value = strtoll(target, (char**)&target_end, 10);

		if (errno != 0) {
			panice("target range start is not a valid numbers game number: %s", target);
		} else if (value <= 0
#if ULONG_MAX < LLONG_MAX
			|| value > (long long)ULONG_MAX
#endif
		) {
			panicf("target range start is not a valid numbers game number: %s", target);
		}

		range.start = value;

		if (target_end[0] == '.' && target_end[1] == '.') {
			target_end += 2;
			range.end = parse_number(target_end, "target range end is not a valid numbers game number");
		} else if (*target_end) {
			panicf("target range start is not a valid numbers game number: %s", target);
		} else {
			range.end = range.start;
		}
	}

	return range;
}

void select_and_solve(ThreadManager *mngr, Number numbers[], size_t number_index, size_t selection_index_start, TargetRange target) {
	if (number_index == mngr->number_count) {
		if (target.start == target.end) {
			printf("TARGET=%" PRIN " ", target.start);
		} else {
			printf("TARGET=%" PRIN "..%" PRIN " ", target.start, target.end);
		}
		printf("NUMBERS=[%" PRIN ", %" PRIN ", %" PRIN ", %" PRIN ", %" PRIN " %" PRIN "]\n",
			numbers[0], numbers[1], numbers[2], numbers[3], numbers[4], numbers[5]);

		// TODO: Each thread only has < 50% CPU usage. Maybe because solve()
		//       actually only takes a tiny amount of time and most of the time is
		//       spent creating and joining threads?
		generate(mngr, target, numbers);
	} else {
		for (size_t selection_index = selection_index_start; selection_index < (sizeof(NUMBERS) / sizeof(Number));) {
			numbers[number_index] = NUMBERS[selection_index];
			select_and_solve(mngr, numbers, number_index + 1, ++ selection_index, target);
		}
	}
}

int main(int argc, char *argv[]) {
	struct option long_options[] = {
		{"help",     no_argument,       0, 'h'},
		{"threads",  required_argument, 0, 't'},
		{"rpn",      no_argument,       0, 'r'},
		{"expr",     no_argument,       0, 'e'},
		{"paren",    no_argument,       0, 'p'},
		{"generate", no_argument,       0, 'g'},
		{0,          0,                 0,  0 },
	};

	PrintStyle print_style = PrintExpr;
	size_t threads = 0;
	bool generate = false;

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

			case 'g':
				generate = true;
				break;

			case '?':
				usage(argc, argv);
				return 1;
		}
	}

	size_t count = argc - optind;

	if (generate) {
		if (count > 1) {
			panicf("too many arguments");
		}
		count = DEFAULT_NUMBER_COUNT;
	} else {
		if (count == 0) {
			panicf("argument TARGET is missing");
		}

		-- count;
		if (count == 0) {
			panicf("need at least one NUMBER argument");
		}

		if (count > MAX_NUMBERS) {
			panicf("too many numbers: %zu > %zu", count, MAX_NUMBERS);
		}
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

	ThreadManager mngr;
	thread_manager_create(&mngr, count, threads, print_style, generate);

	if (generate) {
		TargetRange target = { .start = 100, .end = 999 };

		if (optind < argc) {
			target = parse_target_range(argv[optind]);
		}

		select_and_solve(&mngr, numbers, 0, 0, target);

		for (;;) {
			size_t thread_index = 0;
			for (thread_index = 0; thread_index < mngr.thread_count; ++ thread_index) {
				NumbersCtx *solver = &mngr.solvers[thread_index];

				if (solver->active) {
					break;
				}
			}

			if (thread_index < mngr.thread_count) {
				if (sem_wait(&mngr.semaphore) != 0) {
					panice("waiting on thread manager semaphore");
				}
			} else {
				break;
			}
		}

	} else {
		TargetRange target = parse_target_range(argv[optind]);
		++ optind;

		for (int index = optind; index < argc; ++ index) {
			const Number number = parse_number(argv[index], "number is not a valid numbers game number");
			numbers[index - optind] = number;
		}

		solve(&mngr, target, numbers);
	}
	thread_manager_destroy(&mngr);
	free(numbers);

	return 0;
}
