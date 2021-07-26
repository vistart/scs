#include "scs.h"
#include "aa.h"
#include "ctrlc.h"
#include "glbopts.h"
#include "linalg.h"
#include "linsys.h"
#include "normalize.h"
#include "rw.h"
#include "util.h"

SCS(timer) global_timer;

/* printing header */
static const char *HEADER[] = {
    " iter ",    " pri res ", " dua res ", "   gap   ",
    "   obj   ", "  scale  ", " time (s)",
};
static const scs_int HSPACE = 9;
static const scs_int HEADER_LEN = 7;
static const scs_int LINE_LEN = 66;

static void free_residuals(ScsResiduals * r) {
  if (r) {
    scs_free(r->ax);
    scs_free(r->ax_s);
    scs_free(r->px);
    scs_free(r->aty);
    scs_free(r->ax_s_btau);
    scs_free(r->px_aty_ctau);
    scs_free(r);
  }
}

static void free_work(ScsWork *w) {
  if (w) {
    scs_free(w->u);
    scs_free(w->u_t);
    scs_free(w->v);
    scs_free(w->v_prev);
    scs_free(w->rsk);
    scs_free(w->h);
    scs_free(w->g);
    scs_free(w->b_normalized);
    scs_free(w->c_normalized);
    scs_free(w->rho_y_vec);
    scs_free(w->lin_sys_warm_start);
    if (w->cone_boundaries) {
      scs_free(w->cone_boundaries);
    }
    if (w->scal) {
      scs_free(w->scal->D);
      scs_free(w->scal->E);
      scs_free(w->scal);
    }
    SCS(free_sol)(w->xys_orig);
    free_residuals(w->r_orig);
    if (w->stgs->normalize) {
      SCS(free_sol)(w->xys_normalized);
      free_residuals(w->r_normalized);
    }
    scs_free(w);
  }
}

static void print_init_header(const ScsData *d, const ScsCone *k) {
  scs_int i;
  ScsSettings *stgs = d->stgs;
  char *cone_str = SCS(get_cone_header)(k);
  char *lin_sys_method = SCS(get_lin_sys_method)(d->A, d->P);
#ifdef USE_LAPACK
  scs_int acceleration_lookback = stgs->acceleration_lookback;
  scs_int acceleration_interval = stgs->acceleration_interval;
#else
  scs_int acceleration_lookback = 0;
  scs_int acceleration_interval = 0;
#endif
  for (i = 0; i < LINE_LEN; ++i) {
    scs_printf("-");
  }
  scs_printf(
      "\n\t       SCS v%s - Splitting Conic Solver\n\t(c) Brendan "
      "O'Donoghue, Stanford University, 2012\n",
      SCS(version)());
  for (i = 0; i < LINE_LEN; ++i) {
    scs_printf("-");
  }
  scs_printf("\n");
  scs_printf("problem:  variables n: %i, constraints m: %i\n", (int)d->n,
             (int)d->m);
  scs_printf("%s", cone_str);
  scs_free(cone_str);
  scs_printf(
      "settings: eps_abs: %.1e, eps_rel: %.1e, eps_infeas: %.1e\n"
      "\t  alpha: %.2f, scale: %.2e, adaptive_scaling: %i\n"
      "\t  max_iters: %i, normalize: %i, warm_start: %i\n",
      /*, rho_x: %.2e\n", */
      stgs->eps_abs, stgs->eps_rel, stgs->eps_infeas,
      stgs->alpha, stgs->scale, (int)stgs->adaptive_scaling,
      (int)stgs->max_iters, (int)stgs->normalize, (int)stgs->warm_start);
      /* , stgs->rho_x); */
  if (stgs->acceleration_lookback != 0) {
    scs_printf("\t  acceleration_lookback: %i, acceleration_interval: %i\n",
      (int)acceleration_lookback, (int)acceleration_interval);
  }
  if (stgs->time_limit_secs) {
    scs_printf("\t  time_limit_secs: %.2e,\n", stgs->time_limit_secs);
  }
  if (lin_sys_method) {
    scs_printf("%s", lin_sys_method);
    scs_free(lin_sys_method);
  }

#ifdef MATLAB_MEX_FILE
  mexEvalString("drawnow;");
#endif
}

static void populate_on_failure(scs_int m, scs_int n, ScsSolution *sol,
                                ScsInfo *info, scs_int status_val,
                                const char *msg) {
  if (info) {
    info->gap = NAN;
    info->res_pri = NAN;
    info->res_dual = NAN;
    info->pobj = NAN;
    info->dobj = NAN;
    info->iter = -1;
    info->status_val = status_val;
    info->solve_time = NAN;
    strcpy(info->status, msg);
  }
  if (sol) {
    if (n > 0) {
      if (!sol->x) {
        sol->x = (scs_float *)scs_calloc(n, sizeof(scs_float));
      }
      SCS(scale_array)(sol->x, NAN, n);
    }
    if (m > 0) {
      if (!sol->y) {
        sol->y = (scs_float *)scs_calloc(m, sizeof(scs_float));
      }
      SCS(scale_array)(sol->y, NAN, m);
      if (!sol->s) {
        sol->s = (scs_float *)scs_calloc(m, sizeof(scs_float));
      }
      SCS(scale_array)(sol->s, NAN, m);
    }
  }
}

static scs_int failure(ScsWork *w, scs_int m, scs_int n, ScsSolution *sol,
                       ScsInfo *info, scs_int stint, const char *msg,
                       const char *ststr) {
  scs_int status = stint;
  populate_on_failure(m, n, sol, info, status, ststr);
  scs_printf("Failure:%s\n", msg);
  scs_end_interrupt_listener();
  return status;
}

/* given x,y,s warm start, set v = [x; s / R + y; 1]
 * where R = diag(w->rho_y_vec).
 */
static void warm_start_vars(ScsWork *w, ScsSolution *sol) {
  scs_int n = w->n, m = w->m, i;
  scs_float * v = w->v;
  /* normalize the warm-start */
  if (w->stgs->normalize) {
    SCS(normalize_sol)(w, sol);
  }
  memcpy(v, sol->x, n * sizeof(scs_float));
  for (i = 0; i < m; ++i) {
    v[i + n] = sol->y[i] + sol->s[i] / w->rho_y_vec[i];
  }
  v[n + m] = 1.0;
  /* un-normalize so sol unchanged */
  if (w->stgs->normalize) {
    SCS(un_normalize_sol)(w, sol);
  }
}

static void compute_residuals(ScsResiduals *r, scs_int m, scs_int n) {
  r->res_pri = SAFEDIV_POS(NORM(r->ax_s_btau, m), r->tau);
  r->res_dual = SAFEDIV_POS(NORM(r->px_aty_ctau, n), r->tau);
  r->res_unbdd_a = NAN;
  r->res_unbdd_p = NAN;
  r->res_infeas = NAN;
  if (r->ctx_tau < 0) {
    r->res_unbdd_a = SAFEDIV_POS(NORM(r->ax_s, n), -r->ctx_tau);
    r->res_unbdd_p = SAFEDIV_POS(NORM(r->px, n), -r->ctx_tau);
  }
  if (r->bty_tau < 0) {
    r->res_infeas = SAFEDIV_POS(NORM(r->aty, n), -r->bty_tau);
  }
}

static void unnormalize_residuals(ScsWork *w) {
  ScsResiduals *r_n = w->r_normalized; /* normalized residuals */
  ScsResiduals *r = w->r_orig; /* original problem residuals */
  scs_float pd = w->scal->primal_scale * w->scal->dual_scale;

  /* copy vars */
  r->last_iter = r_n->last_iter;
  r->tau = r_n->tau;

  /* mem copy arrays */
  memcpy(r->ax, r_n->ax, w->m * sizeof(scs_float));
  memcpy(r->ax_s, r_n->ax_s, w->m * sizeof(scs_float));
  memcpy(r->ax_s_btau, r_n->ax_s_btau, w->m * sizeof(scs_float));
  memcpy(r->aty, r_n->aty, w->n * sizeof(scs_float));
  memcpy(r->px, r_n->px, w->n * sizeof(scs_float));
  memcpy(r->px_aty_ctau, r_n->px_aty_ctau, w->n * sizeof(scs_float));

  /* unnormalize */
  r->kap = r_n->kap / pd;
  r->bty_tau = r_n->bty_tau / pd;
  r->ctx_tau = r_n->ctx_tau / pd;
  r->xt_p_x_tau = r_n->xt_p_x_tau / pd;
  r->xt_p_x = r_n->xt_p_x / pd;
  r->ctx = r_n->ctx / pd;
  r->bty = r_n->bty / pd;
  r->pobj = r_n->pobj / pd;
  r->dobj = r_n->dobj / pd;
  r->gap = r_n->gap / pd;

  SCS(un_normalize_primal)(w, r->ax);
  SCS(un_normalize_primal)(w, r->ax_s);
  SCS(un_normalize_primal)(w, r->ax_s_btau);
  SCS(un_normalize_dual)(w, r->aty);
  SCS(un_normalize_dual)(w, r->px);
  SCS(un_normalize_dual)(w, r->px_aty_ctau);

  compute_residuals(r, w->m, w->n);
}

/* calculates un-normalized residual quantities */
/* this is somewhat slow but not a bottleneck */
static void populate_residual_struct(ScsWork *w, scs_int iter) {
  scs_int n = w->n, m = w->m;
  /* normalized x,y,s terms */
  scs_float *x = w->xys_normalized->x;
  scs_float *y = w->xys_normalized->y;
  scs_float *s = w->xys_normalized->s;
  ScsResiduals *r = w->r_normalized; /* normalized residuals */

  /* checks if the residuals are unchanged by checking iteration */
  if (r->last_iter == iter) {
    return;
  }
  r->last_iter = iter;

  memcpy(x, w->u, n * sizeof(scs_float));
  memcpy(y, &(w->u[n]), m * sizeof(scs_float));
  memcpy(s, &(w->rsk[n]), m * sizeof(scs_float));

  r->tau = ABS(w->u[n + m]);
  r->kap = ABS(w->rsk[n + m]);

  /**************** PRIMAL *********************/
  memset(r->ax, 0, m * sizeof(scs_float));
  /* ax = Ax */
  SCS(accum_by_a)(w->A, w->p, x, r->ax);

  memcpy(r->ax_s, r->ax, m * sizeof(scs_float));
  /* ax_s = Ax + s */
  SCS(add_scaled_array)(r->ax_s, s, m, 1.);

  memcpy(r->ax_s_btau, r->ax_s, m * sizeof(scs_float));
  /* ax_s_btau = Ax + s - b * tau */
  SCS(add_scaled_array)(r->ax_s_btau, w->b_normalized, m, -r->tau);

  /**************** DUAL *********************/
  memset(r->px, 0, n * sizeof(scs_float));
  if (w->P) {
    /* px = Px */
    SCS(accum_by_p)(w->P, w->p, x, r->px);
    r->xt_p_x_tau = SCS(dot)(r->px, x, n);
  } else {
    r->xt_p_x_tau = 0.;
  }

  memset(r->aty, 0, n * sizeof(scs_float));
  /* aty = A'y */
  SCS(accum_by_atrans)(w->A, w->p, y, r->aty);

  /* r->px_aty_ctau = Px */
  memcpy(r->px_aty_ctau, r->px, n * sizeof(scs_float));
  /* r->px_aty_ctau = Px + A'y */
  SCS(add_scaled_array)(r->px_aty_ctau, r->aty, n, 1.);
  /* r->px_aty_ctau = Px + A'y + c * tau */
  SCS(add_scaled_array)(r->px_aty_ctau, w->c_normalized, n, r->tau);

  /**************** OTHERS *****************/
  r->bty_tau = SCS(dot)(y, w->b_normalized, m);
  r->ctx_tau = SCS(dot)(x, w->c_normalized, n);


  r->bty = SAFEDIV_POS(r->bty_tau, r->tau);
  r->ctx = SAFEDIV_POS(r->ctx_tau, r->tau);
  r->xt_p_x = SAFEDIV_POS(r->xt_p_x_tau, r->tau * r->tau);

  r->gap = ABS(r->xt_p_x + r->ctx + r->bty);
  r->pobj = r->xt_p_x / 2. + r->ctx;
  r->dobj = -r->xt_p_x / 2. - r->bty;

  compute_residuals(r, m, n);

  if (w->stgs->normalize) {
    memcpy(w->xys_orig->x, w->xys_normalized->x, n * sizeof(scs_float));
    memcpy(w->xys_orig->y, w->xys_normalized->y, m * sizeof(scs_float));
    memcpy(w->xys_orig->s, w->xys_normalized->s, m * sizeof(scs_float));
    SCS(un_normalize_sol)(w, w->xys_orig);
    unnormalize_residuals(w);
  }
}

static void cold_start_vars(ScsWork *w) {
  scs_int l = w->n + w->m + 1;
  memset(w->v, 0, l * sizeof(scs_float));
  w->v[l - 1] = 1.;
}

/* utility function that scales first n entries in inner prod by rho_x   */
/* and last m entries by 1 / rho_y_vec, assumes length of array is n + m */
/* See .note_on_scale in repo for explanation */
static scs_float dot_with_diag_scaling(ScsWork *w, const scs_float *x,
                                       const scs_float *y) {
  scs_int i, n = w->n, len = w->n + w->m;
  scs_float ip = 0.0;
  for (i = 0; i < n; ++i) {
    ip += w->stgs->rho_x * x[i] * y[i];
  }
  for (i = n; i < len; ++i) {
    ip += x[i] * y[i] * w->rho_y_vec[i - n];
  }
  return ip;
}

static inline scs_float get_tau_scale(ScsWork *w) {
  return TAU_FACTOR; /* TAU_FACTOR * w->stgs->scale; */
}

static scs_float root_plus(ScsWork *w, scs_float *p, scs_float *mu, scs_float eta) {
  scs_float b, c, tau, a, tau_scale;
  tau_scale = get_tau_scale(w);
  a = tau_scale + dot_with_diag_scaling(w, w->g, w->g);
  b = (dot_with_diag_scaling(w, mu, w->g) -
       2 * dot_with_diag_scaling(w, p, w->g) - eta * tau_scale);
  c = dot_with_diag_scaling(w, p, p) - dot_with_diag_scaling(w, p, mu);
  tau = (-b + SQRTF(MAX(b * b - 4 * a * c, 0.))) / (2 * a);
  return tau;
}

/* status < 0 indicates failure */
static scs_int project_lin_sys(ScsWork *w, scs_int iter) {
  scs_int n = w->n, m = w->m, l = n + m + 1, status, i;
  scs_float * warm_start = SCS_NULL, tol = 0.;
  memcpy(w->u_t, w->v, l * sizeof(scs_float));
  SCS(scale_array)(w->u_t, w->stgs->rho_x, n);
  for (i = n; i < l - 1 ; ++i) {
    w->u_t[i] *= -w->rho_y_vec[i - n];
  }
  #if INDIRECT > 0
  /* compute warm start using the cone projection output */
  warm_start = w->lin_sys_warm_start;
  memcpy(warm_start, w->u, (l - 1) * sizeof(scs_float));
  /* warm_start = u[:n] + tau * g[:n] */
  SCS(add_scaled_array)(warm_start, w->g, l - 1, w->u[l - 1]);
  /* use normalized residuals to compute tolerance */
  tol = MIN(CG_NORM(w->r_normalized->ax_s_btau, w->m),
            CG_NORM(w->r_normalized->px_aty_ctau, w->n));
  /* tol ~ O(1/k^(1+eps)) guarantees convergence */
  /* use warm-start to calculate tolerance rather than w->u_t, since warm_start
   * should be approximately equal to the true solution */
  tol = CG_TOL_FACTOR * MIN(tol,
        CG_NORM(warm_start, w->n) / POWF((scs_float)iter + 1, CG_RATE));
  tol = MAX(CG_BEST_TOL, tol);
  #endif
  status = SCS(solve_lin_sys)(w->A, w->P, w->p, w->u_t, warm_start, tol);
  if (iter < FEASIBLE_ITERS) {
    w->u_t[l - 1] = 1.;
  } else {
    w->u_t[l - 1] = root_plus(w, w->u_t, w->v, w->v[l - 1]);
  }
  SCS(add_scaled_array)(w->u_t, w->g, l - 1, -w->u_t[l - 1]);
  return status;
}

/* compute the [r;s;kappa] iterate
   rsk^{k+1} = R ( u^{k+1} + v^k - 2 * u_t^{k+1} )
   uses Moreau decomposition to get projection onto dual cone
   since it depends on v^k MUST be called before update_dual_vars is done
   effect of w->stgs->alpha is cancelled out
   see .note_on_scale.
*/
static void compute_rsk(ScsWork *w) {
  scs_int i, l = w->m + w->n + 1;
  /* r, should = 0 so skip */
  /*
  for (i = 0; i < w->n; ++i) {
    w->rsk[i] = w->stgs->rho_x * (w->v[i] + w->u[i] - 2 * w->u_t[i]);
  }
  */
  /* s */
  for (i = w->n; i < l - 1; ++i) {
    w->rsk[i] = (w->v[i] + w->u[i] - 2 * w->u_t[i]) * w->rho_y_vec[i - w->n];
  }
  /* kappa, incorporates tau scaling parameter */
  w->rsk[l - 1] = get_tau_scale(w) * (w->v[l - 1] + w->u[l - 1] - 2 * w->u_t[l - 1]);
}

static void update_dual_vars(ScsWork *w) {
  scs_int i, l = w->n + w->m + 1;
  scs_float a = w->stgs->alpha;
  /* compute and store [r;s;kappa] */
  compute_rsk(w);
  for (i = 0; i < l; ++i) {
    w->v[i] += a * (w->u[i] - w->u_t[i]);
  }
}

/* status < 0 indicates failure */
static scs_int project_cones(ScsWork *w, const ScsCone *k, scs_int iter) {
  scs_int i, n = w->n, l = w->n + w->m + 1, status;
  for (i = 0; i < l; ++i) {
    w->u[i] = 2 * w->u_t[i] - w->v[i];
  }
  /* u = [x;y;tau] */
  status = SCS(proj_dual_cone)(&(w->u[n]), k, w->cone_work, w->stgs->normalize);
  if (iter < FEASIBLE_ITERS) {
    w->u[l - 1] = 1.0;
  } else {
    w->u[l - 1] = MAX(w->u[l - 1], 0.);
  }
  return status;
}

static void sety(ScsWork *w, ScsSolution *sol) {
  if (!sol->y) {
    sol->y = (scs_float *)scs_calloc(w->m, sizeof(scs_float));
  }
  memcpy(sol->y, &(w->u[w->n]), w->m * sizeof(scs_float));
}

/* s is contained in rsk */
static void sets(ScsWork *w, ScsSolution *sol) {
  if (!sol->s) {
    sol->s = (scs_float *)scs_calloc(w->m, sizeof(scs_float));
  }
  memcpy(sol->s, &(w->rsk[w->n]), w->m * sizeof(scs_float));
}

static void setx(ScsWork *w, ScsSolution *sol) {
  if (!sol->x) {
    sol->x = (scs_float *)scs_calloc(w->n, sizeof(scs_float));
  }
  memcpy(sol->x, w->u, w->n * sizeof(scs_float));
}

static scs_int solved(ScsWork *w, ScsSolution *sol, ScsInfo *info,
                      scs_int iter) {
  SCS(scale_array)(sol->x, SAFEDIV_POS(1.0, w->r_orig->tau), w->n);
  SCS(scale_array)(sol->y, SAFEDIV_POS(1.0, w->r_orig->tau), w->m);
  SCS(scale_array)(sol->s, SAFEDIV_POS(1.0, w->r_orig->tau), w->m);
  if (info->status_val == SCS_UNFINISHED) {
    strcpy(info->status, "solved (inaccurate)");
    return SCS_SOLVED_INACCURATE;
  }
  strcpy(info->status, "solved");
  return SCS_SOLVED;
}

static scs_int infeasible(ScsWork *w, ScsSolution *sol, ScsInfo *info) {
  SCS(scale_array)(sol->y, -1 / w->r_orig->bty_tau, w->m);
  SCS(scale_array)(sol->x, NAN, w->n);
  SCS(scale_array)(sol->s, NAN, w->m);
  if (info->status_val == SCS_UNFINISHED) {
    strcpy(info->status, "infeasible (inaccurate)");
    return SCS_INFEASIBLE_INACCURATE;
  }
  strcpy(info->status, "infeasible");
  return SCS_INFEASIBLE;
}

static scs_int unbounded(ScsWork *w, ScsSolution *sol, ScsInfo *info) {
  SCS(scale_array)(sol->x, -1 / w->r_orig->ctx_tau, w->n);
  SCS(scale_array)(sol->s, -1 / w->r_orig->ctx_tau, w->m);
  SCS(scale_array)(sol->y, NAN, w->m);
  if (info->status_val == SCS_UNFINISHED) {
    strcpy(info->status, "unbounded (inaccurate)");
    return SCS_UNBOUNDED_INACCURATE;
  }
  strcpy(info->status, "unbounded");
  return SCS_UNBOUNDED;
}

static scs_int is_solved_status(scs_int status) {
  return status == SCS_SOLVED || status == SCS_SOLVED_INACCURATE;
}

static scs_int is_infeasible_status(scs_int status) {
  return status == SCS_INFEASIBLE || status == SCS_INFEASIBLE_INACCURATE;
}

static scs_int is_unbounded_status(scs_int status) {
  return status == SCS_UNBOUNDED || status == SCS_UNBOUNDED_INACCURATE;
}

static void get_info(ScsWork *w, ScsSolution *sol, ScsInfo *info, scs_int iter) {
  ScsResiduals *r = w->r_orig;
  info->iter = iter;
  info->res_infeas = r->res_infeas;
  info->res_unbdd_a = r->res_unbdd_a;
  info->res_unbdd_p = r->res_unbdd_p;
  info->scale = w->stgs->scale;
  info->scale_updates = w->scale_updates;
  if (is_solved_status(info->status_val)) {
    info->gap = r->gap;
    info->res_pri = r->res_pri;
    info->res_dual = r->res_dual;
    info->pobj = r->xt_p_x / 2. + r->ctx;
    info->dobj = -r->xt_p_x / 2. - r->bty;
  } else if (is_unbounded_status(info->status_val)) {
    info->gap = NAN;
    info->res_pri = NAN;
    info->res_dual = NAN;
    info->pobj = -INFINITY;
    info->dobj = -INFINITY;
  } else if (is_infeasible_status(info->status_val)) {
    info->gap = NAN;
    info->res_pri = NAN;
    info->res_dual = NAN;
    info->pobj = INFINITY;
    info->dobj = INFINITY;
  }
}

/* sets solutions, re-scales by inner prods if infeasible or unbounded */
static void get_solution(ScsWork *w, ScsSolution *sol, ScsInfo *info,
                         scs_int iter) {
  if (info->status_val == SCS_UNFINISHED) {
    /* failed to converge within limits */
    if (iter == w->stgs->max_iters) {
      populate_on_failure(w->m, w->n, sol, info, SCS_MAX_ITERS, "hit max_iters");
    }
    else if (w->time_limit_reached) {
      populate_on_failure(w->m, w->n, sol, info, SCS_TIME_LIMIT, "hit time_limit_secs");
    }
    else {
      scs_printf("Error: should not be in this state (1).\n");
    }
    return;
  }
  populate_residual_struct(w, iter);
  setx(w, sol);
  sety(w, sol);
  sets(w, sol);
  if (w->stgs->normalize) {
    SCS(un_normalize_sol)(w, sol);
  }
  if (is_solved_status(info->status_val)) {
    info->status_val = solved(w, sol, info, iter);
  }
  else if (is_infeasible_status(info->status_val)) {
    info->status_val = infeasible(w, sol, info);
  }
  else if (is_unbounded_status(info->status_val)) {
    info->status_val = unbounded(w, sol, info);
  }
  else {
    /* If failed we have already left this function */
    scs_printf("Error: should not be in this state (2).\n");
  }
  get_info(w, sol, info, iter);
}

static void print_summary(ScsWork *w, scs_int i, SCS(timer) * solve_timer) {
  ScsResiduals *r = w->r_orig;
  scs_printf("%*i|", (int)strlen(HEADER[0]), (int)i);
  scs_printf("%*.2e ", (int)HSPACE, r->res_pri);
  scs_printf("%*.2e ", (int)HSPACE, r->res_dual);
  scs_printf("%*.2e ", (int)HSPACE, r->gap);
  scs_printf("%*.2e ", (int)HSPACE, r->pobj);
  scs_printf("%*.2e ", (int)HSPACE, w->stgs->scale);
  scs_printf("%*.2e ", (int)HSPACE, SCS(tocq)(solve_timer) / 1e3);
  scs_printf("\n");

#if VERBOSITY > 0
  scs_printf("Norm u = %4f, ", SCS(norm)(w->u, w->n + w->m + 1));
  scs_printf("Norm u_t = %4f, ", SCS(norm)(w->u_t, w->n + w->m + 1));
  scs_printf("Norm v = %4f, ", SCS(norm)(w->v, w->n + w->m + 1));
  scs_printf("Norm x = %4f, ", SCS(norm)(w->xys_orig->x, w->n));
  scs_printf("Norm y = %4f, ", SCS(norm)(w->xys_orig->y,  w->m));
  scs_printf("Norm s = %4f, ", SCS(norm)(w->xys_orig->s, w->m));
  scs_printf("Norm |Ax + s| = %1.2e, ", SCS(norm)(r->ax_s, w->m));
  scs_printf("tau = %4f, ", w->u[w->n + w->m]);
  scs_printf("kappa = %4f, ", w->rsk[w->n + w->m]);
  scs_printf("|u - u_t| = %1.2e, ",
             SCS(norm_diff)(w->u, w->u_t, w->n + w->m + 1));
  scs_printf("res_infeas = %1.2e, ", r->res_infeas);
  scs_printf("res_unbdd_a = %1.2e, ", r->res_unbdd_a);
  scs_printf("res_unbdd_p = %1.2e, ", r->res_unbdd_p);
  scs_printf("ctx_tau = %1.2e, ", r->ctx_tau);
  scs_printf("bty_tau = %1.2e\n", r->bty_tau);
#endif

#ifdef MATLAB_MEX_FILE
  mexEvalString("drawnow;");
#endif
}

static void print_header(ScsWork *w, const ScsCone *k) {
  scs_int i;
  for (i = 0; i < LINE_LEN; ++i) {
    scs_printf("-");
  }
  scs_printf("\n");
  for (i = 0; i < HEADER_LEN - 1; ++i) {
    scs_printf("%s|", HEADER[i]);
  }
  scs_printf("%s\n", HEADER[HEADER_LEN - 1]);
  for (i = 0; i < LINE_LEN; ++i) {
    scs_printf("-");
  }
  scs_printf("\n");
#ifdef MATLAB_MEX_FILE
  mexEvalString("drawnow;");
#endif
}

static scs_float get_dual_cone_dist(const scs_float *y, const ScsCone *k,
                                    ScsConeWork *c, scs_int m) {
  scs_float dist;
  scs_float *t = (scs_float *)scs_calloc(m, sizeof(scs_float));
  memcpy(t, y, m * sizeof(scs_float));
  SCS(proj_dual_cone)(t, k, c, 0);
  dist = SCS(norm_inf_diff)(t, y, m);
#if VERBOSITY > 0
  SCS(print_array)(y, m, "y");
  SCS(print_array)(t, m, "proj_y");
  scs_printf("dist = %4f\n", dist);
#endif
  scs_free(t);
  return dist;
}

/* via moreau */
static scs_float get_pri_cone_dist(const scs_float *s, const ScsCone *k,
                                   ScsConeWork *c, scs_int m) {
  scs_float dist;
  scs_float *t = (scs_float *)scs_calloc(m, sizeof(scs_float));
  memcpy(t, s, m * sizeof(scs_float));
  SCS(scale_array)(t, -1.0, m);
  SCS(proj_dual_cone)(t, k, c, 0);
  dist = SCS(norm_inf)(t, m); /* ||s - Pi_c(s)|| = ||Pi_c*(-s)|| */
#if VERBOSITY > 0
  SCS(print_array)(s, m, "s");
  SCS(print_array)(t, m, "(s - proj_s)");
  scs_printf("dist = %4f\n", dist);
#endif
  scs_free(t);
  return dist;
}

static void print_footer(const ScsData *d, const ScsCone *k, ScsSolution *sol,
                         ScsWork *w, ScsInfo *info,
                         scs_float total_lin_sys_time,
                         scs_float total_cone_time,
                         scs_float total_accel_time) {
  scs_int i;
  char *lin_sys_str = SCS(get_lin_sys_summary)(w->p, info);

  for (i = 0; i < LINE_LEN; ++i) {
    scs_printf("-");
  }
  scs_printf("\n");
  scs_printf("status:  %s\n", info->status);
  scs_printf("timings: total: %1.2es = setup: %1.2es + solve: %1.2es\n",
             (info->setup_time + info->solve_time) / 1e3,
             info->setup_time / 1e3, info->solve_time / 1e3);
  scs_printf("\t lin-sys: %1.2es, cones: %1.2es, accel: %1.2es\n",
             total_lin_sys_time / 1e3, total_cone_time / 1e3,
             total_accel_time / 1e3);
  scs_printf("%s", lin_sys_str);
  scs_free(lin_sys_str);

  for (i = 0; i < LINE_LEN; ++i) {
    scs_printf("-");
  }
  scs_printf("\n");

  if (is_infeasible_status(info->status_val)) {
    scs_printf("cone: dist(y, K*) = %.2e\n",
               get_dual_cone_dist(sol->y, k, w->cone_work, d->m));
    scs_printf("cert: |A'y| = %.2e\n", info->res_infeas);
    scs_printf("      b'y = %.2f\n", SCS(dot)(d->b, sol->y, d->m));
  } else if (is_unbounded_status(info->status_val)) {
    scs_printf("cone: dist(s, K) = %.2e\n",
               get_pri_cone_dist(sol->s, k, w->cone_work, d->m));
    scs_printf("cert: |Ax+s| = %.2e\n", info->res_unbdd_a);
    scs_printf("      |Px| = %.2e\n", info->res_unbdd_p);
    scs_printf("      c'x = %.2f\n", SCS(dot)(d->c, sol->x, d->n));
  } else if (is_solved_status(info->status_val)) {
    scs_printf("cones: dist(s, K) = %.2e, dist(y, K*) = %.2e\n",
               get_pri_cone_dist(sol->s, k, w->cone_work, d->m),
               get_dual_cone_dist(sol->y, k, w->cone_work, d->m));
    scs_printf("comp slack: s'y/|s||y| = %.2e, ",
               SCS(dot)(sol->s, sol->y, d->m) /
               MAX(1e-9, SCS(norm)(sol->s, d->m)) /
               MAX(1e-9, SCS(norm)(sol->y, d->m)));
    scs_printf("gap: |x'Px+c'x+b'y| = %.2e\n",
               info->gap);
    scs_printf("pri res: |Ax+s-b| = %.2e, ", info->res_pri);
    scs_printf("dua res: |Px+A'y+c| = %.2e\n", info->res_dual);
  } else { /* A failure mode (eg hit max_iters) */ }
  for (i = 0; i < LINE_LEN; ++i) {
    scs_printf("-");
  }
  scs_printf("\n");
  scs_printf("objective = %.6f\n", info->pobj);
  for (i = 0; i < LINE_LEN; ++i) {
    scs_printf("-");
  }
  scs_printf("\n");
#ifdef MATLAB_MEX_FILE
  mexEvalString("drawnow;");
#endif
}

static scs_int has_converged(ScsWork *w, scs_int iter) {
  scs_float eps_abs = w->stgs->eps_abs;
  scs_float eps_rel = w->stgs->eps_rel;
  scs_float eps_infeas = w->stgs->eps_infeas;
  scs_float grl, prl, drl;

  ScsResiduals *r = w->r_orig;
  scs_float *b = w->b_orig;
  scs_float *c = w->c_orig;
  scs_float *s = w->xys_orig->s;

  if (r->tau > 0.) {
    /* xt_p_x, ctx, bty already have tau divided out */
    grl = MAX(MAX(ABS(r->xt_p_x), ABS(r->ctx)), ABS(r->bty));
    /* s, ax, px, aty do *not* have tau divided out, so need to divide */
    prl = MAX(MAX(NORM(b, w->m) * r->tau,
              NORM(s, w->m)), NORM(r->ax, w->m)) / r->tau;
    drl = MAX(MAX(NORM(c, w->n) * r->tau,
              NORM(r->px, w->n)), NORM(r->aty, w->n)) / r->tau;
    if (isless(r->res_pri, eps_abs + eps_rel * prl) &&
        isless(r->res_dual, eps_abs + eps_rel * drl) &&
        isless(r->gap, eps_abs + eps_rel * grl)) {
      return SCS_SOLVED;
    }
  }
  if (isless(r->res_unbdd_a, eps_infeas) &&
      isless(r->res_unbdd_p, eps_infeas)) {
    return SCS_UNBOUNDED;
  }
  if (isless(r->res_infeas, eps_infeas)) {
    return SCS_INFEASIBLE;
  }
  return 0;
}

static scs_int validate(const ScsData *d, const ScsCone *k) {
  ScsSettings *stgs = d->stgs;
  if (d->m <= 0 || d->n <= 0) {
    scs_printf("m and n must both be greater than 0; m = %li, n = %li\n",
               (long)d->m, (long)d->n);
    return -1;
  }
  if (d->m < d->n) {
    /* scs_printf("WARN: m less than n, problem likely degenerate\n"); */
    /* return -1; */
  }
  if (SCS(validate_lin_sys)(d->A, d->P) < 0) {
    scs_printf("invalid linear system input data\n");
    return -1;
  }
  if (SCS(validate_cones)(d, k) < 0) {
    scs_printf("cone validation error\n");
    return -1;
  }
  if (stgs->max_iters <= 0) {
    scs_printf("max_iters must be positive\n");
    return -1;
  }
  if (stgs->eps_abs < 0) {
    scs_printf("eps_abs tolerance must be positive\n");
    return -1;
  }
  if (stgs->eps_rel < 0) {
    scs_printf("eps_rel tolerance must be positive\n");
    return -1;
  }
  if (stgs->eps_infeas < 0) {
    scs_printf("eps_infeas tolerance must be positive\n");
    return -1;
  }
  if (stgs->alpha <= 0 || stgs->alpha >= 2) {
    scs_printf("alpha must be in (0,2)\n");
    return -1;
  }
  if (stgs->rho_x <= 0) {
    scs_printf("rho_x must be positive (1e-3 works well).\n");
    return -1;
  }
  if (stgs->scale <= 0) {
    scs_printf("scale must be positive (1 works well).\n");
    return -1;
  }
  return 0;
}

static ScsResiduals *init_residuals(const ScsData *d) {
  ScsResiduals * r = (ScsResiduals *)scs_calloc(1, sizeof(ScsResiduals));
  r->last_iter = -1;
  r->ax = (scs_float *)scs_calloc(d->m, sizeof(scs_float));
  r->ax_s = (scs_float *)scs_calloc(d->m, sizeof(scs_float));
  r->ax_s_btau = (scs_float *)scs_calloc(d->m, sizeof(scs_float));
  r->px = (scs_float *)scs_calloc(d->n, sizeof(scs_float));
  r->aty = (scs_float *)scs_calloc(d->n, sizeof(scs_float));
  r->px_aty_ctau = (scs_float *)scs_calloc(d->n, sizeof(scs_float));
  return r;
}

static ScsWork *init_work(const ScsData *d, const ScsCone *k) {
  ScsWork *w = (ScsWork *)scs_calloc(1, sizeof(ScsWork));
  scs_int l = d->n + d->m + 1;
  if (d->stgs->verbose) {
    print_init_header(d, k);
  }
  if (!w) {
    scs_printf("ERROR: allocating work failure\n");
    return SCS_NULL;
  }
  /* get settings and dims from data struct */
  w->d = d;
  w->k = k;
  w->stgs = d->stgs;
  w->m = d->m;
  w->n = d->n;
  w->last_scale_update_iter = 0;
  w->sum_log_scale_factor = 0.;
  w->n_log_scale_factor = 0;
  w->scale_updates = 0;
  w->time_limit_reached = 0;
  /* allocate workspace: */
  w->u = (scs_float *)scs_calloc(l, sizeof(scs_float));
  w->u_t = (scs_float *)scs_calloc(l, sizeof(scs_float));
  w->v = (scs_float *)scs_calloc(l, sizeof(scs_float));
  w->v_prev = (scs_float *)scs_calloc(l, sizeof(scs_float));
  w->rsk = (scs_float *)scs_calloc(l, sizeof(scs_float));
  w->h = (scs_float *)scs_calloc((l - 1), sizeof(scs_float));
  w->g = (scs_float *)scs_calloc((l - 1), sizeof(scs_float));
  w->lin_sys_warm_start = (scs_float *)scs_calloc((l - 1), sizeof(scs_float));
  w->rho_y_vec = (scs_float *)scs_calloc(d->m, sizeof(scs_float));
  /* x,y,s struct */
  w->xys_orig = (ScsSolution *)scs_calloc(1, sizeof(ScsSolution));
  w->xys_orig->x = (scs_float *)scs_calloc(d->n, sizeof(scs_float));
  w->xys_orig->s = (scs_float *)scs_calloc(d->m, sizeof(scs_float));
  w->xys_orig->y = (scs_float *)scs_calloc(d->m, sizeof(scs_float));
  w->r_orig = init_residuals(d);

  w->A = d->A;
  w->P = d->P;

  w->b_orig = d->b;
  w->c_orig = d->c;
  w->b_normalized = (scs_float *)scs_calloc(d->m, sizeof(scs_float));
  w->c_normalized = (scs_float *)scs_calloc(d->n, sizeof(scs_float));
  memcpy(w->b_normalized, w->b_orig, w->m * sizeof(scs_float));
  memcpy(w->c_normalized, w->c_orig, w->n * sizeof(scs_float));
  SCS(set_rho_y_vec)(k, w->stgs->scale, w->rho_y_vec, w->m);

  if (!w->c_normalized) {
    scs_printf("ERROR: work memory allocation failure\n");
    return SCS_NULL;
  }

  if (w->stgs->normalize) {
    w->xys_normalized = (ScsSolution *)scs_calloc(1, sizeof(ScsSolution));
    w->xys_normalized->x = (scs_float *)scs_calloc(d->n, sizeof(scs_float));
    w->xys_normalized->s = (scs_float *)scs_calloc(d->m, sizeof(scs_float));
    w->xys_normalized->y = (scs_float *)scs_calloc(d->m, sizeof(scs_float));
    w->r_normalized = init_residuals(d);

#ifdef COPYAMATRIX
    if (!SCS(copy_matrix)(&(w->A), d->A)) {
      scs_printf("ERROR: copy A matrix failed\n");
      return SCS_NULL;
    }
    if (w->P && !SCS(copy_matrix)(&(w->P), d->P)) {
      scs_printf("ERROR: copy P matrix failed\n");
      return SCS_NULL;
    }
#endif
    /* this allocates memory that must be freed */
    w->cone_boundaries_len = SCS(set_cone_boundaries)(k, &w->cone_boundaries);
    w->scal = (ScsScaling *)scs_calloc(1, sizeof(ScsScaling));
    SCS(normalize)(w->P, w->A, w->b_normalized, w->c_normalized, w->scal,
                   w->cone_boundaries, w->cone_boundaries_len);
#if VERBOSITY > 0
    SCS(print_array)(w->scal->D, d->m, "D");
    scs_printf("norm(D) = %4f\n", SCS(norm)(w->scal->D, d->m));
    SCS(print_array)(w->scal->E, d->n, "E");
    scs_printf("norm(E) = %4f\n", SCS(norm)(w->scal->E, d->n));
#endif
  } else {
    w->xys_normalized = w->xys_orig;
    w->r_normalized = w->r_orig;
    w->cone_boundaries_len = 0;
    w->cone_boundaries = SCS_NULL;
    w->scal = SCS_NULL;
  }
  if (!(w->cone_work = SCS(init_cone)(k, w->scal, w->m))) {
    scs_printf("ERROR: init_cone failure\n");
    return SCS_NULL;
  }
  if (!(w->p = SCS(init_lin_sys_work)(w->A, w->P, w->rho_y_vec, w->stgs->rho_x))) {
    scs_printf("ERROR: init_lin_sys_work failure\n");
    return SCS_NULL;
  }
  /* hack: negative acceleration_lookback interpreted as type-I */
  if (!(w->accel = aa_init(l, ABS(w->stgs->acceleration_lookback),
                           w->stgs->acceleration_lookback < 0,
                           AA_REGULARIZATION, AA_RELAXATION, VERBOSITY))) {
    if (w->stgs->verbose) {
      scs_printf("WARN: aa_init returned NULL, no acceleration applied.\n");
    }
  }
  return w;
}

static void update_work_cache(ScsWork * w) {
  /* g = (I + M)^{-1} h */
  memcpy(w->g, w->h, (w->n + w->m) * sizeof(scs_float));
  SCS(scale_array)(&(w->g[w->n]), -1., w->m);
  SCS(solve_lin_sys)(w->A, w->P, w->p, w->g, SCS_NULL, CG_BEST_TOL);
  return;
}

static scs_int update_work(const ScsData *d, ScsWork *w, ScsSolution *sol) {
  /* before normalization */
  scs_int n = d->n;
  scs_int m = d->m;
  if (w->stgs->warm_start) {
    warm_start_vars(w, sol);
  } else {
    cold_start_vars(w);
  }

  /* h = [c;b] */
  memcpy(w->h, w->c_normalized, n * sizeof(scs_float));
  memcpy(&(w->h[n]), w->b_normalized, m * sizeof(scs_float));
  update_work_cache(w);
  return 0;
}


static void maybe_update_scale(ScsWork *w, const ScsCone *k, scs_int iter) {
  scs_int i;
  scs_float factor, new_scale;

  ScsResiduals *r = w->r_orig;
  ScsSolution *xys= w->xys_orig;
  scs_float *b = w->b_orig;
  scs_float *c = w->c_orig;

  scs_int iters_since_last_update = iter - w->last_scale_update_iter;
  /* ||Ax + s - b * tau|| */
  scs_float relative_res_pri = SAFEDIV_POS(SCALE_NORM(r->ax_s_btau, w->m),
    MAX(MAX(SCALE_NORM(r->ax, w->m), SCALE_NORM(xys->s, w->m)), SCALE_NORM(b, w->m) * r->tau));
  /* ||Px + A'y + c * tau|| */
  scs_float relative_res_dual = SAFEDIV_POS(SCALE_NORM(r->px_aty_ctau, w->n),
    MAX(MAX(SCALE_NORM(r->px, w->n), SCALE_NORM(r->aty, w->n)),  SCALE_NORM(c, w->n) * r->tau));

  /* higher scale makes res_pri go down faster, so increase if res_pri larger */
  w->sum_log_scale_factor += log(relative_res_pri) - log(relative_res_dual);
  w->n_log_scale_factor++;

  /* geometric mean */
  factor = SQRTF(exp(w->sum_log_scale_factor /
                (scs_float)(w->n_log_scale_factor)));

  /* need at least RESCALING_MIN_ITERS since last update */
  if (iters_since_last_update < RESCALING_MIN_ITERS) {
    return;
  }
  new_scale = MIN(MAX(w->stgs->scale * factor, MIN_SCALE_VALUE), MAX_SCALE_VALUE);
  if (new_scale == w->stgs->scale) {
    return;
  }
  if (SCS(should_update_rho_y_vec(factor, iters_since_last_update))) {
    w->scale_updates++;
    w->sum_log_scale_factor = 0;
    w->n_log_scale_factor = 0;
    w->last_scale_update_iter = iter;
    w->stgs->scale = new_scale;
    SCS(set_rho_y_vec)(k, w->stgs->scale, w->rho_y_vec, w->m);
    SCS(update_linsys_rho_y_vec)(w->A, w->P, w->p, w->rho_y_vec);

    /* update pre-solved quantities */
    update_work_cache(w);

    /* reset acceleration so that old iterates aren't affecting new values */
    aa_reset(w->accel);

    /* update v, using fact that rsk, u, u_t vectors should be the same */
    /* solve: R (v^+ + u - 2u_t) = rsk => v^+ = R^-1 rsk + 2u_t - u  */
    /* only elements with scale on diag */
    for (i = w->n; i < w->n + w->m; i++) {
      w->v[i] = w->rsk[i] / w->rho_y_vec[i - w->n] + 2 * w->u_t[i] - w->u[i];
    }
  }
}

scs_int SCS(solve)(ScsWork *w, ScsSolution *sol, ScsInfo * info) {
  scs_int i;
  scs_float v_norm;
  SCS(timer) solve_timer, lin_sys_timer, cone_timer, accel_timer;
  scs_float total_accel_time = 0.0, total_cone_time = 0.0,
            total_lin_sys_time = 0.0;
  if (!sol || !w || !info) {
    scs_printf("ERROR: missing ScsWork, ScsSolution or ScsInfo input\n");
    return SCS_FAILED;
  }
  scs_int l = w->m + w->n + 1;
  const ScsData * d = w->d;
  const ScsCone * k = w->k;
  /* TODO delete me */
  scs_int* XXX = scs_calloc(10, sizeof(scs_int));
  XXX[0] = 1;
  info->setup_time = w->setup_time;
  /* initialize ctrl-c support */
  scs_start_interrupt_listener();
  SCS(tic)(&solve_timer);
  info->status_val = SCS_UNFINISHED; /* not yet converged */
  update_work(d, w, sol);

  if (w->stgs->verbose) {
    print_header(w, k);
  }

  /* SCS */
  for (i = 0; i < w->stgs->max_iters; ++i) {
    /* scs is homogeneous so scale the iterate to keep norm reasonable */
    /* this should this be before applying any acceleration */
    if (i >= FEASIBLE_ITERS) {
      v_norm = SCS(norm)(w->v, l); /* always l2 norm */
      SCS(scale_array)(w->v, SQRTF((scs_float)l) * ITERATE_NORM / v_norm, l);
    }
    /* accelerate here so that last step always projection onto cone */
    /* this ensures the returned iterates always satisfy conic constraints */
    if (i > 0 && i % w->stgs->acceleration_interval == 0) {
      SCS(tic)(&accel_timer);
      w->aa_norm = aa_apply(w->v, w->v_prev, w->accel);
      /*
      if (r->aa_norm < 0) {
        return failure(w, w->m, w->n, sol, info, SCS_FAILED,
            "error in accelerate", "failure");
      }
      */
      total_accel_time += SCS(tocq)(&accel_timer);
    }
    memcpy(w->v_prev, w->v, l * sizeof(scs_float));

    SCS(tic)(&lin_sys_timer);
    if (project_lin_sys(w, i) < 0) {
      return failure(w, w->m, w->n, sol, info, SCS_FAILED,
                     "error in project_lin_sys", "failure");
    }
    total_lin_sys_time += SCS(tocq)(&lin_sys_timer);

    SCS(tic)(&cone_timer);
    if (project_cones(w, k, i) < 0) {
      return failure(w, w->m, w->n, sol, info, SCS_FAILED,
                     "error in project_cones", "failure");
    }
    total_cone_time += SCS(tocq)(&cone_timer);

    update_dual_vars(w);

    if (i % CONVERGED_INTERVAL == 0) {
      if (scs_is_interrupted()) {
        return failure(w, w->m, w->n, sol, info, SCS_SIGINT, "interrupted",
                     "interrupted");
      }
      populate_residual_struct(w, i);
      if ((info->status_val = has_converged(w, i)) != 0) {
        break;
      }
      if (w->stgs->time_limit_secs) {
        if (SCS(tocq)(&solve_timer) > 1000. * w->stgs->time_limit_secs) {
          w->time_limit_reached = 1;
          break;
        }
      }
    }

    if (w->stgs->verbose && i % PRINT_INTERVAL == 0) {
      populate_residual_struct(w, i);
      print_summary(w, i, &solve_timer);
    }

    /* if residuals are fresh then maybe compute new scale */
    if (w->stgs->adaptive_scaling && i == w->r_orig->last_iter) {
      maybe_update_scale(w, k, i);
    }

    /* Log *after* updating scale so residual recalc does not affect alg */
    if (w->stgs->log_csv_filename) {
      /* calc residuals every iter if logging to csv */
      populate_residual_struct(w, i);
      SCS(log_data_to_csv)(d, k, w, i, &solve_timer);
    }
  }

  if (w->stgs->log_csv_filename) {
    /* calc residuals every iter if logging to csv */
    populate_residual_struct(w, i);
    SCS(log_data_to_csv)(d, k, w, i, &solve_timer);
  }

  if (w->stgs->verbose) {
    populate_residual_struct(w, i);
    print_summary(w, i, &solve_timer);
  }

  /* populate solution vectors (unnormalized) and info */
  get_solution(w, sol, info, i);
  info->solve_time = SCS(tocq)(&solve_timer);

  if (w->stgs->verbose) {
    print_footer(d, k, sol, w, info, total_lin_sys_time, total_cone_time,
                 total_accel_time);
  }

  scs_end_interrupt_listener();
  return info->status_val;
}

void SCS(finish)(ScsWork *w) {
  if (w) {
    SCS(finish_cone)(w->cone_work);
    if (w->stgs && w->stgs->normalize) {
#ifndef COPYAMATRIX
      SCS(un_normalize)(w->A, w->P, w->scal);
#else
      SCS(free_scs_matrix)(w->A);
      SCS(free_scs_matrix)(w->P);
#endif
    }
    if (w->p) {
      SCS(free_lin_sys_work)(w->p);
    }
    if (w->accel) {
      aa_finish(w->accel);
    }
    free_work(w);
  }
}

ScsWork *SCS(init)(const ScsData *d, const ScsCone *k) {
#if VERBOSITY > 1
  SCS(tic)(&global_timer);
#endif
  ScsWork *w;
  SCS(timer) init_timer;
  scs_start_interrupt_listener();
  if (!d || !k) {
    scs_printf("ERROR: Missing ScsData or ScsCone input\n");
    return SCS_NULL;
  }
#if VERBOSITY > 0
  SCS(print_data)(d);
  SCS(print_cone_data)(k);
#endif
  if (validate(d, k) < 0) {
    scs_printf("ERROR: Validation returned failure\n");
    return SCS_NULL;
  }
  SCS(tic)(&init_timer);
  if (d->stgs->write_data_filename) {
    SCS(write_data)(d, k);
  }
  w = init_work(d, k);
  w->setup_time = SCS(tocq)(&init_timer);
  scs_end_interrupt_listener();
  return w;
}

/* this just calls SCS(init), SCS(solve), and SCS(finish) */
scs_int scs(const ScsData *d, const ScsCone *k, ScsSolution *sol,
            ScsInfo *info) {
  scs_int status;
  ScsWork *w = SCS(init)(d, k);
#if VERBOSITY > 0
  scs_printf("size of scs_int = %lu, size of scs_float = %lu\n",
             sizeof(scs_int), sizeof(scs_float));
#endif
  if (w) {
    SCS(solve)(w, sol, info);
    status = info->status_val;
  } else {
    status = failure(SCS_NULL, d ? d->m : -1, d ? d->n : -1, sol, info,
                     SCS_FAILED, "could not initialize work", "failure");
  }
  SCS(finish)(w);
  return status;
}
