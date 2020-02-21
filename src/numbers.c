#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <getopt.h>

#include "panic.h"

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
} PrintStyle;

typedef struct ElementS {
	Op op;
	Number value;
} Element;

typedef struct NumbersCtxS {
	Number        target;
	const Number *numbers;
	bool         *used;
	size_t        count;
	Element      *ops;
	size_t        ops_size;
	size_t        ops_index;
	Number       *vals;
	size_t        vals_size;
	size_t        vals_index;
	PrintStyle    print_style;
} NumbersCtx;

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

		if (lhs_precedence < this_precedence) {
			putchar('(');
		}
		print_expr(ctx, lhs_index - 1);
		if (lhs_precedence < this_precedence) {
			putchar(')');
		}

		switch (op) {
			case OpAdd: printf(" + "); break;
			case OpSub: printf(" - "); break;
			case OpMul: printf(" * "); break;
			case OpDiv: printf(" / "); break;
			default: assert(false);
		}

		if (rhs_precedence < this_precedence) {
			putchar('(');
		}
		print_expr(ctx, index - 1);
		if (rhs_precedence < this_precedence) {
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

static void solve_next(NumbersCtx *ctx) {
	if (ctx->vals_index == 1 && ctx->target == ctx->vals[0]) {
		switch (ctx->print_style) {
			case PrintRpn:  print_solution_rpn(ctx);  break;
			case PrintExpr: print_solution_expr(ctx); break;
			default: assert(false);
		}
	}

	for (size_t index = 0; index < ctx->count; ++ index) {
		if (!ctx->used[index]) {
			ctx->used[index] = true;
			const Number number = ctx->numbers[index];
			push_op(ctx, OpVal, number);
			assert(ctx->vals_index < ctx->vals_size);
			ctx->vals[ctx->vals_index] = number;
			++ ctx->vals_index;

			if (ctx->vals_index > 1) {
				const Number lhs = ctx->vals[ctx->vals_index - 2];
				Number value = 0;

				-- ctx->vals_index;
				if (number <= lhs) {
					value = ctx->vals[ctx->vals_index - 1] = lhs + number;
					push_op(ctx, OpAdd, value);
					solve_next(ctx);
					pop_op(ctx);

					if (lhs != number) {
						value = ctx->vals[ctx->vals_index - 1] = lhs - number;
						push_op(ctx, OpSub, value);
						solve_next(ctx);
						pop_op(ctx);
					}

					value = ctx->vals[ctx->vals_index - 1] = lhs * number;
					push_op(ctx, OpMul, value);
					solve_next(ctx);
					pop_op(ctx);

					if (lhs % number == 0) {
						value = ctx->vals[ctx->vals_index - 1] = lhs / number;
						push_op(ctx, OpDiv, value);
						solve_next(ctx);
						pop_op(ctx);
					}
				}
				++ ctx->vals_index;
				ctx->vals[ctx->vals_index - 2] = lhs;
			}
			solve_next(ctx);

			-- ctx->vals_index;
			pop_op(ctx);
			ctx->used[index] = false;
		}
	}
}

void solve(const Number target, const Number numbers[], const size_t count, PrintStyle print_style) {
	if (count == 0) {
		panicf("need at least one number");
	}
	const size_t ops_size = count + count - 1;
	Element *ops = calloc(ops_size, sizeof(Element));
	if (!ops) {
		panice("allocating operand stack of size %zu", ops_size);
	}

	const size_t vals_size = count;
	Number *vals = calloc(vals_size, sizeof(Number));
	if (!vals) {
		panice("allocating value stack of size %zu", vals_size);
	}

	bool *used = calloc(count, sizeof(Number));
	if (!used) {
		panice("allocating used array of size %zu", count);
	}

	NumbersCtx ctx = {
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
	};
	
	solve_next(&ctx);

	free(ops);
	free(vals);
	free(used);
}

static void usage(int argc, char *const argv[]) {
	printf("Usage: %s [OPTIONS] TARGET NUMBER...\n", argc > 0 ? argv[0] : "numbers");
	printf(
		"\n"
		"OPTIONS:\n"
		"\n"
		"\t-h, --help    Print this help message.\n"
		"\t-r, --rpn     Print solutions in reverse Polish notation.\n"
		"\t-e, --expr    Print solutions in usual notation (default).\n"
		"\n"
	);
}

int main(int argc, char *argv[]) {
	struct option long_options[] = {
		{"help",  no_argument, 0, 'h'},
		{"rpn",   no_argument, 0, 'r'},
		{"expr",  no_argument, 0, 'e'},
		{0,                 0, 0,  0 },
	};

	PrintStyle print_style = PrintExpr;

	for(;;) {
		int c = getopt_long(argc, argv, "hue", long_options, NULL);
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

			case '?':
				usage(argc, argv);
				return 1;
		}
	}

	if (argc - optind < 2) {
		usage(argc, argv);
		return 1;
	}

	char *endptr = NULL;
	const Number target = strtoul(argv[optind], &endptr, 10);
	if (!*argv[optind] || *endptr || target == 0) {
		panice("target is not a valid numbers game number: %s", argv[optind]);
	}
	++ optind;

	const size_t count = argc - optind;
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

	solve(target, numbers, count, print_style);
	free(numbers);

	return 0;
}
