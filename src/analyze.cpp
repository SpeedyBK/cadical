#include "internal.hpp"

#include "clause.hpp"
#include "iterator.hpp"
#include "proof.hpp"
#include "macros.hpp"
#include "message.hpp"

#include <algorithm>

namespace CaDiCaL {

/*------------------------------------------------------------------------*/

void Internal::learn_empty_clause () {
  assert (!unsat);
  LOG ("learned empty clause");
  if (proof) proof->trace_empty_clause ();
  unsat = true;
}

void Internal::learn_unit_clause (int lit) {
  LOG ("learned unit clause %d", lit);
  if (proof) proof->trace_unit_clause (lit);
  iterating = true;
  stats.fixed++;
}

/*------------------------------------------------------------------------*/

void Internal::rescore () {
  stats.rescored++;
  VRB ("rescore %ld", stats.rescored);
  for (Var * v = vtab + 1; v <= vtab + max_var; v++)
    v->score /= scinc;
  scinc = 1;
}

// Important variables recently used in conflict analysis are 'bumped',
// which means to move them to the front of the VMTF decision queue.  The
// 'bumped' time stamp is updated accordingly.  It is used to determine
// whether the 'queue.assigned' pointer has to be moved in 'unassign'.

void Internal::bump_variable (Var * v) {
  if (!v->next) return;
  if (queue.assigned == v) queue.assigned = v->prev ? v->prev : v->next;
  queue.dequeue (v), queue.enqueue (v);
  v->bumped = ++stats.bumped;
  v->score += scinc;
  if (v->score > 1e100) rescore ();
  int idx = var2idx (v);
  if (!vals[idx]) queue.assigned = v;
  LOG ("VMTF bumped and moved to front %d", idx);
}

// Initially we proposed to bump the variable in the current 'bumped' stamp
// order only.  This maintains the current order between bumped variables.
// On few benchmarks this however lead to a large number of propagations per
// seconds, which can be reduced by an order of magnitude by focusing
// somewhat on recently assigned variables more, particularly in this
// situation.  This can easily be achieved by using the sum of the 'bumped'
// time stamp and trail height 'trail' for comparison.  Note that 'bumped'
// is always increasing and gets really large, while 'trail' can never be
// larger than the number of variables, so there is likely a potential for
// further optimization.

struct bumped_plus_trail_smaller {
  Internal * internal;
  bumped_plus_trail_smaller (Internal * s) : internal (s) { }
  bool operator () (int a, int b) {
    Var & u = internal->var (a), & v = internal->var (b);
    return u.bumped + u.trail < v.bumped + v.trail;
  }
};

struct bumped_earlier {
  Internal * internal;
  bumped_earlier (Internal * s) : internal (s) { }
  bool operator () (int a, int b) {
    return internal->var (a).bumped < internal->var (b).bumped;
  }
};

struct trail_smaller {
  Internal * internal;
  trail_smaller (Internal * s) : internal (s) { }
  bool operator () (int a, int b) {
    return internal->var (a).trail < internal->var (b).trail;
  }
};

struct score_smaller {
  Internal * internal;
  score_smaller (Internal * s) : internal (s) { }
  bool operator () (int a, int b) {
    return internal->var (a).score < internal->var (b).score;
  }
};

void Internal::sort_seen () {
  switch (opts.bumpsort) {
    default: case 0: break;
    case 1:
      sort (seen.begin (), seen.end (), bumped_earlier (this));
      break;
    case 2:
      sort (seen.begin (), seen.end (), trail_smaller (this));
      break;
    case 3;
      sort (seen.begin (), seen.end (), bumped_plus_trail_smaller (this));
      break;
    case 4;
      sort (seen.begin (), seen.end (), score_smaller (this));
      break;
    case 5;
      reverse (seen.begin (), seen.end ());
      break;
  }
}

void Internal::bump_and_clear_seen_variables () {
  START (bump);
  sort_seen ();
  for (const_int_iterator i = seen.begin (); i != seen.end (); i++) {
    Var * v = &var (*i);
    assert (v->seen);
    v->seen = false;
    bump_variable (v);
  }
  seen.clear ();
  scinc /= opts.decay;
  if (scinc > 1e100) rescore ();
  STOP (bump);
}

/*------------------------------------------------------------------------*/

// Clause activity is replaced by a move to front scheme as well with
// 'resolved' as time stamp.  Only long and high glue clauses are stamped
// since small or low glue clauses are kept anyhow (and do not actually have
// a 'resolved' field).  We keep the relative order of bumped clauses by
// sorting them first.

void Internal::bump_resolved_clauses () {
  START (bump);
  sort (resolved.begin (), resolved.end (), resolved_earlier ());
  for (const_clause_iterator i = resolved.begin (); i != resolved.end (); i++)
    (*i)->resolved () = ++stats.resolved;
  STOP (bump);
  resolved.clear ();
}

void Internal::resolve_clause (Clause * c) {
  if (!c->redundant) return;
  if (c->size <= opts.keepsize) return;
  if (c->glue <= opts.keepglue) return;
  assert (c->extended);
  resolved.push_back (c);
}

/*------------------------------------------------------------------------*/

// During conflict analysis literals not seen yet either become part of the
// first-uip clauses (if on lower decision level), are dropped (if fixed),
// or are resolved away (if on the current decision level and different from
// the first UIP).  At the same time we update the number of seen literals on
// a decision level and the smallest trail position of a seen literal for
// each decision level.  This both helps conflict clause minimization.  The
// number of seen levels is the glucose level (also called glue, or LBD).

inline bool Internal::analyze_literal (int lit) {
  Var & v = var (lit);
  if (v.seen) return false;
  if (!v.level) return false;
  assert (val (lit) < 0);
  if (v.level < level) clause.push_back (lit);
  Level & l = control[v.level];
  if (!l.seen++) {
    LOG ("found new level %d contributing to conflict", v.level);
    levels.push_back (v.level);
  }
  if (v.trail < l.trail) l.trail = v.trail;
  v.seen = true;
  seen.push_back (lit);
  LOG ("analyzed literal %d assigned at level %d", lit, v.level);
  return v.level == level;
}

void Internal::clear_levels () {
  for (const_int_iterator i = levels.begin (); i != levels.end (); i++)
    control[*i].reset ();
  levels.clear ();
}

// By sorting the first UIP clause literals before minimization, we
// establish the invariant that the two watched literals are on the largest
// decision highest level.

struct trail_greater_than {
  Internal * internal;
  trail_greater_than (Internal * s) : internal (s) { }
  bool operator () (int a, int b) {
    return internal->var (a).trail > internal->var (b).trail;
  }
};

void Internal::analyze () {
  assert (conflict);
  if (!level) { learn_empty_clause (); conflict = 0; return; }

  START (analyze);

  // First derive the first UIP clause.
  //
  Clause * reason = conflict;
  LOG (reason, "analyzing conflict");
  resolve_clause (reason);
  int open = 0, uip = 0;
  const_int_iterator i = trail.end ();
  for (;;) {
    const const_literal_iterator end = reason->end ();
    const_literal_iterator j = reason->begin ();
    while (j != end)
      if (analyze_literal (*j++))
	open++;
    while (!var (uip = *--i).seen)
      ;
    if (!--open) break;
    reason = var (uip).reason;
    LOG (reason, "analyzing %d reason", uip);
  }
  LOG ("first UIP %d", uip);
  clause.push_back (-uip);
  check_clause ();

  // Update glue statistics.
  //
  bump_resolved_clauses ();
  const int glue = (int) levels.size ();
  LOG ("1st UIP clause of size %d and glue %d", (int) clause.size (), glue);
  UPDATE_AVG (fast_glue_avg, glue);
  UPDATE_AVG (slow_glue_avg, glue);

  if (opts.minimize) minimize_clause ();	// minimize clause

  stats.units += (clause.size () == 1);
  stats.binaries += (clause.size () == 2);

  // Determine back jump level, backtrack and assign flipped literal.
  //
  Clause * driving_clause = 0;
  int jump = 0;
  if (clause.size () > 1) {
    sort (clause.begin (), clause.end (), trail_greater_than (this));
    driving_clause = new_learned_clause (glue);
    jump = var (clause[1]).level;
  }
  UPDATE_AVG (jump_avg, jump);
  backtrack (jump);
  assign (-uip, driving_clause);

  // Update decision heuristics and clean up.
  //
  bump_and_clear_seen_variables ();
  clause.clear (), clear_levels ();
  conflict = 0;

  STOP (analyze);
}

// We wait reporting a learned unit until propagation of that unit is
// completed.  Otherwise the 'i' report line might prematurely give the
// number of remaining variables.

void Internal::iterate () { iterating = false; report ('i'); }

};
