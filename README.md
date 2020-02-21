numbers
=======

Countdown numbers game solver.

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
        -t, --threads=COUNT    Spawn COUNT threads. Uses number of CPUs per default.
        -r, --rpn              Print solutions in reverse Polish notation.
        -e, --expr             Print solutions in usual notation (default).
        -p, --paren            Like --expr but never skip parenthesis.
