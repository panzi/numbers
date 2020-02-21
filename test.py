#!/usr/bin/env python3

import sys
from subprocess import Popen, PIPE
from os.path import abspath, join as joinpath, dirname
from random import randint

def eval(code):
	ops = code.strip().split()
	stack = []
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
	succes_count = 0
	for testnr in range(256):
		target = randint(1, 999)
		numbers = [randint(1, 200) for n in range(6)]
		pipe = Popen([binary_path, '--rpn', str(target), *[str(num) for num in numbers]], stdout=PIPE)

		ok = True
		def write_fail(msg):
			nonlocal ok
			if ok:
				sys.stdout.write(' [ FAIL ]\n')
				ok = False
			sys.stdout.write(f'     {msg}\n')

		sys.stdout.write(f'TEST: target={target}, numbers={repr(numbers)}'.ljust(73))
		for line in pipe.stdout:
			line = line.decode().strip()
			if line:
				try:
					output_target = eval(line)
				except (ValueError, IndexError):
					write_fail(f'{line}: error evaluating code')
				else:
					if output_target != target:
						write_fail(f'{line}: {output_target} != {target}')
		code = pipe.wait()
		if code != 0:
			write_fail(f'exit code: {code}')

		if ok:
			print(' [  OK  ]')
			succes_count += 1
		else:
			status = 1
			fail_count += 1

	print(f'failed: {fail_count}, succeeded: {succes_count}')

	return status

if __name__ == '__main__':
	sys.exit(test())
