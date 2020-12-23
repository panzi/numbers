#!/usr/bin/env python3

import sys
from subprocess import Popen, PIPE
from os.path import abspath, join as joinpath, dirname
from random import randint, choice
from time import monotonic
from typing import List, Union

UINT64_MAX = 0xffff_ffff_ffff_ffff
TIMEOUT    = 1

def generate_game(min_size:int=1, max_size:int=7, max_number:int=UINT64_MAX, max_target:int=UINT64_MAX):
	if min_size < 1:
		raise ValueError('min_size has to be >= 1: %r' % min_size)

	if max_size < min_size:
		raise ValueError('max_size (%r) < min_size (%r)' % (max_size, min_size))

	if max_size > 64:
		raise ValueError('max_size too big: %r' % max_size)

	if max_target > UINT64_MAX or max_target < 1:
		raise ValueError('invalid max_target: %r' % max_target)

	if max_number > UINT64_MAX or max_number < 1:
		raise ValueError('invalid max_number: %r' % max_number)
	
	size = randint(min_size, max_size)
	numbers: List[int] = []
	vals: List[int] = []
	code: List[Union[str, int]] = []

	def finished():
		return (
			len(numbers) == size and
			len(vals) == 1 and
			vals[0] > 0 and
			vals[0] <= max_target
		)

	def generate_vals():
		if monotonic() - start_ts > TIMEOUT:
			raise TimeoutError

		while len(numbers) < size:
			if max_number <= 10 or randint(0, 1) == 0:
				number = randint(1, 10)
			else:
				number = randint(11, max_number)

			numbers.append(number)
			vals.append(number)
			code.append(number)

			if finished(): return
			generate_ops()
			if finished(): return
			generate_vals()
			if finished(): return

			code.pop()
			vals.pop()
			numbers.pop()

	def generate_ops():
		if monotonic() - start_ts > TIMEOUT:
			raise TimeoutError

		if len(vals) >= 2:
			lhs = vals[-2]
			rhs = vals.pop()

			value = vals[-1] = lhs + rhs
			code.append('+')
			if finished(): return
			generate_ops()
			if finished(): return
			generate_vals()
			if finished(): return
			code.pop()

			if lhs >= rhs:
				value = vals[-1] = lhs - rhs
				code.append('-')
				if finished(): return
				generate_ops()
				if finished(): return
				generate_vals()
				if finished(): return
				code.pop()

			value = vals[-1] = lhs * rhs
			code.append('*')
			if finished(): return
			generate_ops()
			if finished(): return
			generate_vals()
			if finished(): return
			code.pop()

			if lhs >= rhs and rhs != 0 and lhs % rhs == 0:
				value = vals[-1] = lhs // rhs
				code.append('/')
				if finished(): return
				generate_ops()
				if finished(): return
				generate_vals()
				if finished(): return
				code.pop()

			vals.append(rhs)
			vals[-2] = lhs

	for _ in range(64):
		try:
			numbers.clear()
			vals.clear()
			code.clear()
			start_ts = monotonic()
			generate_vals()
		except TimeoutError:
			pass
		else:
			break

	if not finished():
		raise RuntimeError(f'failed to generate game for parameters min_size={min_size}, max_size={max_size} (selected size={size}), max_number={max_number} max_target={max_target}')

	return {
		'target': vals[0],
		'numbers': numbers,
		'code': code,
	}

def eval(code:list) -> int:
	ops = code
	stack: List[int] = []
	for op in ops:
		if op == '+':
			val = stack.pop()
			stack[-1] = stack[-1] + val
		elif op == '-':
			val = stack.pop()
			stack[-1] = stack[-1] - val
		elif op == '*':
			val = stack.pop()
			stack[-1] = stack[-1] * val
		elif op == '/':
			val = stack.pop()
			stack[-1] = stack[-1] // val
		else:
			stack.append(int(op))
	if len(stack) != 1:
		raise ValueError("too many values left on stack")
	return stack[0]

binary_path = joinpath(dirname(abspath(__file__)), 'build', 'numbers')

def test():
	status = 0
	fail_count = 0
	success_count = 0
	failed_games = []
	for testnr in range(1, 1001):
		game = generate_game(max_number=500, max_target=999)
		target = game['target']
		numbers = game['numbers']
		pipe = Popen([binary_path, '--rpn', str(target), *[str(num) for num in numbers]], stdout=PIPE)
		if pipe.stdout is None:
			# impossible, but for typing
			raise TypeError("pipe.stdout is None")

		ok = True
		def write_fail(msg):
			nonlocal ok
			if ok:
				sys.stdout.write(' [ FAIL ]\n')
				ok = False
			sys.stdout.write(f'    {msg}\n')

		code: Union[str, int] = ' '.join(str(op) for op in game['code'])
		sys.stdout.write(f'{testnr}: target={target}, numbers={repr(numbers)}, code={repr(code)}'.ljust(150))
		sys.stdout.flush()
		found_solution = False
		for line_bytes in pipe.stdout:
			line = line_bytes.decode().strip()
			if line:
				try:
					output_target = eval(line.strip().split())
				except (ValueError, IndexError):
					write_fail(f'{line}: error evaluating code')
				else:
					if output_target == target:
						found_solution = True
					else:
						write_fail(f'{line}: {output_target} != {target}')
		code = pipe.wait()
		if code != 0:
			write_fail(f'exit code: {code}')

		if not found_solution:
			write_fail("No solution found!")
			failed_games.append(game)

		if ok:
			print(' [  OK  ]')
			success_count += 1
		else:
			status = 1
			fail_count += 1

	if failed_games:
		print()
		print(f'Failed games:')
		for game in failed_games:
			target = game['target']
			numbers = game['numbers']
			code = ' '.join(str(op) for op in game['code'])
			print(f'    target={target}, numbers={repr(numbers)}, code={repr(code)}')

	print()
	print(f'failed: {fail_count}, succeeded: {success_count}')

	return status

if __name__ == '__main__':
	sys.exit(test())
