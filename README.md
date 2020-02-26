numbers
=======

Countdown numbers game solver.

* [Build](#build)
* [Usage](#usage)
* [Numbers Game Rules](#numbers-game-rules)
* [Algorithm](#algorithm)
* [Optimizations](#optimizations)
* [Other Resources](#other-resources)
* [Old Solvers](#old-solvers)

Build
-----

    git clone https://github.com/panzi/numbers.git
    cd numbers
    make

Usage
-----

    Usage: ./build/numbers [OPTIONS] TARGET NUMBER...

    OPTIONS:

        -h, --help             Print this help message.
        -t, --threads=COUNT    Spawn COUNT threads. (default: cpus)

                               Special COUNT values:
                                  cpus ...... use number of CPUs (CPU cores)
                                  numbers ... use number count

        -r, --rpn              Print solutions in reverse Polish notation.
        -e, --expr             Print solutions in usual notation (default).
        -p, --paren            Like --expr but never skip parenthesis.

Getting the number of CPU cores is supported on systems that support
`sysconf(_SC_NPROCESSORS_ONLN)`. On other systems it will take the number
count as threads. The way threading is implemented this is the
maximum number of possible threads anyway.

### Numbers Game Rules

In this "given number" doesn't refer to a certain value of a number, but to
a number as given in the problem set. The same value may occur more than once
in the problem set.

* all given numbers are positive integers
* each given number may be used at most once
* allowed operations are addition, subtraction, multiplication, and division
* at no intermediate step in the process can the current running total become
  negative or involve a fraction
* use all of this to produce a given target number

In the original game from the TV show only certain given numbers are allowed,
there are always 6 of them, and the target number is in the range 101 to 999.
But this program can take any positive 64bit integer for any number and the
target. Given enough RAM and CPU time it supports up to 64 given numbers
on a 64bit machine (32 on a 32bit machine). On my machine (16 GB RAM, 64bit
Linux) up to 9 numbers work fine (of course depending a lot on the given
numbers).

Memory complexity of the used algorithm is O(N), or O(2n-1) to be overly
precise. All of the memory is allocated at the start of the program and freed
at the end. During runtime two array indices are manipulated instead of
allocating and freeing memory. This is what sets it apart to my [old numbers
game solver](https://github.com/panzi/numbers-c) that runs out of memory on my
16 GB RAM machine when trying to solve a game with 9 numbers.

### Algorithm

This uses a calculation strategy that is called reverse Polish notation (RPN).
Doing math that way doesn't need parenthesis. You build up a stack of values
and operations like this:

    5 4 + 2 *

The `+` adds the `5` and `4` and leaves a `9` on the stack:

    9 2 *

So now the `9` and `2` are multiplied which leaves:

    18

This is useful because it is an easy way to express these mathematical
operations just in an array of values and operations. If the number of possible
values are limited you know the maximum size of the array in advance:

    number_of_values * 2 - 1

However, since we don't just want to calculate an expression using RPN, but
want to actually remember what the expression was so we can print it if it
matches the target we don't pop off anything from the operation stack when
pushing on operations. Instead we track the calculated intermediate values
on a separate stack. We need a separate stack for that since we don't know
where on the operation stack the operations before the current one end without
searching through it in reverse.

So now the search for a solution just works as follows:

* call [solve numbers](#solve-numbers)

#### Solve Numbers

* for all unused numbers
  * pop the number on the operation and value stacks
  * call [check solution](#check-solution)
  * call [solve operations](#solve-operations)
  * call [solve numbers](#solve-numbers)
  * pop the number from the operations and value stack

#### Check Solution

* if only one value is on the value stack and it is the target
  * print the operations stack

#### Solve Operations

* if more than one values are on the value stack:
  * for all operations
    * push the operation on the operation stack
    * push the result of the operation on the value stack
    * call [check solution](#check-solution)
    * call [solve operations](#solve-operations)
    * call [solve numbers](#solve-numbers)
    * pop the operation from operation stack
    * pop the value from the value stack

### Optimizations

The first optimization is just to adhere to the rules of the numbers game:
Don't push operations that would generate forbidden intermediate results.
See: [Numbers Game Rules](#numbers-game-rules)

#### Further discard operations that:

* result in `0`
  * A - A = 0
* result in on of its own operands
  * A - B = B
  * A / B = B
  * A * 1 = A
  * A / 1 = A

#### Commutative rules:

    1 2 +

Is the same as:

    2 1 +

Since of the no negative or fractional intermediate results rule we simply
discard any operations where the left hand side operand is smaller then the
right hand side operand and make this the first check before everything.

#### Apply associative rules:

    1 2 3 + +

Is the same as:

    1 2 + 3 +

So we need only try one of those. I see the second result as redundant.
Since it is easy to check if the previous operation is the same as the
current one we drop those and take the second version. In particular we
drop the operation if:

* if top of the stack is `+`
  * and new operation would be `+` or `-`
* if top of the stack is `-`
  * and new operation would be `+`
  * and new operation would be `-`, except if the other way of writing
    this calculation would have a negative intermediate result
* if top of the stack is `*` or `/`
  * and new operation would be `*` or `/`

However, we can do more when combined with commutativity. We want to
sort our expressions so that in a chain of `+` and `-` operations the
operand size decreases and the `-` operations are grouped to the end.
(Same for chains `*` and `/` operations.)

Drop operations if:

* operation is `+` and
  * either left hand operand is also `+` and
    * right hand operand of that nested `+` has a smaller value than the
      right hand operand of the new operation
  * or left hand operand is `-`
* operation is `*` and
  * either left hand operand is also `*` and
    * right hand operand of that nested `*` has a smaller value than the
      right hand operand of the new operation
  * or left hand operand is `/`

**Note:** All of these rules will still give redundant results if a number
occurs more than once in the game. I don't think it would be woth it to
optimize for that case.

#### Multithreading

These days computers have many cores, it would be a waste to not use them
all. So instead of running just one solver you can split up the first level
(the initial numbers) and instantiate a solver for each split and run them
concurrently. This is simple and easy to do, but not optimal. It means you
can only have at most one thread for each number and in praxis these
different sub problems have a vastly different runtime. This means several
threads will finish way before the last one. The thread with the lowest
number will even finish immediately.

This changes the initial step of the algorithm to:

* split set of numbers into `thread count` buckets
* for each bucket
  * create a new solver
  * spawn thread
  * call [solve numbers](#solve-numbers) only for the numbers of the bucket

**Note:** The nested `solve numbers` calls still have to operate on all
numbers.

**Note:** Printing the operant stack need to be protected from concurrency.
And without buffering the results and then merging them the results will
appear in basically random order using multithreading.

I'm still working on coming up with a better multithreading strategy. Instead
of the above one could maybe change the `solve numbers` function to something
like this:

* for all unused numbers
  * pop the number on the operation and value stacks
  * if less than `maximum thread count` threads are running
    * copy the state of the current solver
    * spawn a new thread
    * call [check solution](#check-solution)
    * call [solve operations](#solve-operations)
    * call [solve numbers](#solve-numbers)
    * pop the number from the operations and value stack
  * else on same thread
    * call [check solution](#check-solution)
    * call [solve operations](#solve-operations)
    * call [solve numbers](#solve-numbers)
    * pop the number from the operations and value stack

However, that check would happen extremely often and would need to guard
against concurrency, which could bring the performance down a lot again.

### Other Resources

* http://datagenetics.com/blog/august32014/index.html Strategies to solve the
  Countdown Numbers Game using RPN
* https://github.com/rvedotrc/numbers Another C implementation of a solver
  using RPN

### Old Solvers

These are old solvers that I've written that use a worse strategy. See the
other C implementation for a description.

* https://github.com/panzi/numbers-c
* https://github.com/panzi/numbers-python
* https://github.com/panzi/numbers-js
* https://github.com/panzi/numbers-rust
