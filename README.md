SCS
====

[![Build Status](https://travis-ci.org/cvxgrp/scs.svg?branch=master)](https://travis-ci.org/cvxgrp/scs)

<!--- remove appveyor since I don't know how to make it compile with lapack yet
[![Build status](https://ci.appveyor.com/api/projects/status/4542u6kom5293qpm)](https://ci.appveyor.com/project/bodono/scs)
-->

SCS (`splitting conic solver`) is a numerical optimization package for solving
large-scale convex cone problems, based on our paper [Conic Optimization via
Operator Splitting and Homogeneous Self-Dual
Embedding](http://www.stanford.edu/~boyd/papers/scs.html). It is written in C
and can be used in other C, C++,
[Python](https://github.com/bodono/scs-python),
[Matlab](https://github.com/bodono/scs-matlab),
[R](https://github.com/bodono/scs-r),
[Julia](https://github.com/JuliaOpt/SCS.jl), programs via the linked
interfaces. It can also be called as a solver from convex optimization
toolboxes [CVX](http://cvxr.com/cvx/) (3.0 or later),
[CVXPY](https://github.com/cvxgrp/cvxpy),
[Convex.jl](https://github.com/JuliaOpt/Convex.jl), and
[Yalmip](https://github.com/johanlofberg/YALMIP).

The current version is `2.0.0`. If you wish to cite SCS, please use the
following:
```
@article{ocpb:16,
    author       = {B. O'Donoghue and E. Chu and N. Parikh and S. Boyd},
    title        = {Conic Optimization via Operator Splitting and Homogeneous Self-Dual Embedding},
    journal      = {Journal of Optimization Theory and Applications},
    month        = {June},
    year         = {2016},
    volume       = {169},
    number       = {3},
    pages        = {1042-1068},
    url          = {http://stanford.edu/~boyd/papers/scs.html},
}
@misc{scs,
    author       = {B. O'Donoghue and E. Chu and N. Parikh and S. Boyd},
    title        = {{SCS}: Splitting Conic Solver, version 2.0.0},
    howpublished = {\url{https://github.com/cvxgrp/scs}},
    month        = oct,
    year         = 2017
}
```

----
SCS numerically solves convex cone programs using the alternating direction
method of multipliers
([ADMM](http://web.stanford.edu/~boyd/papers/admm_distr_stats.html)).  It
returns solutions to both the primal and dual problems if the problem is
feasible, or a certificate of infeasibility otherwise. It solves the following
primal cone problem:

```
minimize        c'x
subject to      Ax + s = b
                s in K
```
over variables `x` and `s`, where `A`, `b` and `c` are user-supplied data and
`K` is a user-defined convex cone.  The dual problem is given by
```
maximize        -b'y
subject to      -A'y == c
                y in K^*
```
over variable `y`, where `K^*` denotes the dual cone to `K`.

The cone `K` can be any Cartesian product of the following primitive cones:
+ zero cone `{x | x = 0 }` (dual to the free cone `{x | x in R}`)
+ positive orthant `{x | x >= 0}`
+ second-order cone `{(t,x) | ||x||_2 <= t}`
+ positive semidefinite cone `{ X | min(eig(X)) >= 0, X = X^T }`
+ exponential cone `{(x,y,z) | y e^(x/y) <= z, y>0 }`
+ dual exponential cone `{(u,v,w) | −u e^(v/u) <= e w, u<0}`
+ power cone `{(x,y,z) | x^a * y^(1-a) >= |z|, x>=0, y>=0}`
+ dual power cone `{(u,v,w) | (u/a)^a * (v/(1-a))^(1-a) >= |w|, u>=0, v>=0}`

The rows of the data matrix `A` correspond to the cones in `K`.  **The rows of
`A` must be in the order of the cones given above, i.e., first come the rows
that correspond to the zero/free cones, then those that correspond to the
positive orthants, then SOCs, etc.** For a `k` dimensional semidefinite cone
when interpreting the rows of the data matrix `A` SCS assumes that the `k x k`
matrix variable has been vectorized by scaling the off-diagonal entries by
`sqrt(2)` and stacking the **lower triangular elements column-wise** to create a
vector of length `k(k+1)/2`. See the section on semidefinite programming below.

At termination SCS returns solution `(x*, s*, y*)` if the problem is feasible,
or a certificate of infeasibility otherwise. See
[here](http://web.stanford.edu/~boyd/cvxbook/) for more details about
cone programming and certificates of infeasibility.

**Semidefinite Programming**

SCS assumes that the matrix variables and the input data corresponding to
semidefinite cones have been vectorized by **scaling the off-diagonal entries by
`sqrt(2)`** and stacking the lower triangular elements **column-wise**. For a `k
x k` matrix variable (or data matrix) this operation would create a vector of
length `k(k+1)/2`. ScsScaling by `sqrt(2)` is required to preserve the
inner-product.

**To recover the matrix solution this operation must be inverted on the
components of the vector returned by SCS corresponding to semidefinite cones**.
That is, the off-diagonal entries must be scaled by `1/sqrt(2)` and the upper
triangular entries are filled in by copying the values of lower triangular
entries.

More explicitly, we want to express
`Tr(C X)` as `vec(C)'*vec(X)`, where the `vec` operation takes the `k x k` matrix
```
X = [ X11 X12 ... X1k
      X21 X22 ... X2k
      ...
      Xk1 Xk2 ... Xkk ]
```
and produces a vector consisting of
```
vec(X) = (X11, sqrt(2)*X21, ..., sqrt(2)*Xk1, X22, sqrt(2)*X32, ..., Xkk).
```


**Linear equation solvers**

Each iteration of SCS requires the solution of a set of linear equations.  This
package includes two implementations for solving linear equations: a direct
solver which uses a cached LDL factorization and an indirect solver based on
conjugate gradients. The indirect solver can be run on either the cpu or
gpu.

The direct solver uses the LDL and AMD packages numerical linear
algebra packages by Timothy Davis and others, the necessary files are included.
See [here](http://www.cise.ufl.edu/research/sparse/) for more information about
these packages.

### Using SCS in C
Typing `make` at the command line will compile the code and create
SCS libraries in the `out` folder. It will also produce six demo binaries in the
`out` folder named `demo_direct`, `demo_indirect`, `demo_gpu`, `demo_SOCP_direct`,
`demo_SOCP_indirect`, and `demo_SOCP_gpu`.

If `make` completes successfully, it will produce three static library files,
`libscsdir.a`, `libscsindir.a`, and `libscsgpu.a` under the `out` folder and
three dynamic library files `libscsdir.ext`, `libscsindir.ext`, and
`libscsgpu.ext` (where `.ext` extension is platform dependent) in the same
folder. To include the libraries in your own source code, compile with the
linker option with `-L(PATH_TO_SCS)\out` and `-lscsdir` or `-lscsindir` (as
needed).

The API and required data structures are defined in the file `include/scs.h`.
The four main API functions are:

* `ScsWork * scs_init(const ScsData * d, const ScsCone * k, ScsInfo * info);`

    This initializes the ScsWork struct containing the workspace that scs will
    use, and performs the necessary preprocessing (e.g. matrix factorization).
    All inputs `d`, `k`, and `info` must be memory allocated before calling.

* `scs_int scs_solve(ScsWork * w, const ScsData * d, const ScsCone * k, ScsSolution * sol, ScsInfo * info);`

    This solves the problem as defined by ScsData `d` and ScsCone `k` using the
    workspace in `w`. The solution is returned in `sol` and information about
    the solve is returned in `info` (outputs must have memory allocated before
    calling).  None of the inputs can be NULL. You can call `scs_solve` many
    times for one call to `scs_init`, so long as the matrix `A` does not change
    (vectors `b` and `c` can change).

* `void scs_finish(ScsWork * w);`

    Called after all solves completed to free allocated memory and other
    cleanup.

* `scs_int scs(const ScsData * d, const ScsCone * k, ScsSolution * sol, ScsInfo * info);`

    Convenience method that simply calls all the above routines in order, for
    cases where the workspace does not need to be reused. All inputs must have
    memory allocated before this call.

The data matrix `A` is specified in column-compressed format and the vectors
`b` and `c` are specified as dense arrays. The solutions `x` (primal), `s`
(slack), and `y` (dual) are returned as dense arrays. Cones are specified as
the struct above, the rows of `A` must correspond to the cones in the
exact order as specified by the cone struct (i.e. put linear cones before
second-order cones etc.).

**Warm-start**

You can warm-start SCS (supply a guess of the solution) by setting warm_start in
ScsData to `1` and supplying the warm-starts in the ScsSolution struct (`x`,`y`,
and `s`). All inputs must be warm-started if any one is. These are used to
initialize the iterates in `scs_solve`.

**Re-using matrix factorization**

If using the direct version you can factorize the matrix once and solve many
times. Simply call scs_init once, and use `scs_solve` many times with the same
workspace, changing the input data `b` and `c` (and optionally warm-starts) for
each iteration. See run_scs.c for an example.

**Using your own linear system solver**

To use your own linear system solver simply implement all the methods and the
two structs in `include/linSys.h` and plug it in.

**Solving SDPs**

In order to solve SDPs you must have BLAS and LAPACK installed.
Edit `scs.mk` to set `USE_LAPACK = 1` and point to the location of these
libraries. Without these you can still solve problems using the other cones.

