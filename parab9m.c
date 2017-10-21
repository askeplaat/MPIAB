#include <stdio.h>         // for print
#include <stdlib.h>        // for rand
#include <assert.h>        // for print
#include <pthread.h>
#include "parab.h"         // for prototypes and data structures


/* 
 * parab.c
 *
 * parallel alpha beta based on TDS and MCTS ideas, 
 * roll out alphabeta, see Bojun Huang AAAI 2015
 * Pruning Game Tree by Rollouts
 * http://www.aaai.org/ocs/index.php/AAAI/AAAI15/paper/view/9828/9354
 *
 * Aske Plaat 2017
 *
 * parab6.c 27 april 2017 werkt op 1 thread corrected alphabeta
 * parab7.c 27 april 2017 parallelle versie, cilk threads
 * 
 * parab4.c
 * introduces next_brother pointer in node
 * needed since we generate one at a time, then process (search), and 
 * then generate the next brother, which thjen is searched with the new bound.
 * note that this is a very sequential way of looking at alphabeta
 * 
 * parab5.c going back to separate lb and ub
 * 
 * parab6.c using alpha and beta as upward lb and ub and as downward alpha and beta
 * the logic is there, and it is much cleaner this way. Code is about 50% shorter* although I am now cheating on node accesses to parent and child,
 * that in a distributed memory setting need to be fixed, they are remote references
 *
 * SELECT: node.a = parent.a; node.b = parent.b; update a=max(a, lb); b=min(b, ub)
 * UPDATE: MAX: node.lb=max(node.lb, child.lb); MAX && CLOSED: node.ub=max(all-children.ub); MIN: node.ub=min(node.ub, child.ub); MIN && CLOSED: node.lb=min(all-children.lb); if some values changed, UPDATE node.parent. 
 * LIVE: a<b
 * DEAD: not LIVE (alphabeta cutoff)
 * TOUCHED: some children of node are expanded AND (lb > -INF || ub < INF)
 * CLOSED: all children of node are expanded AND (lb > -INF || ub < INF)
 * OPEN: zero children are expanded and have meaningful bounds
 * node == not LEAF && OPEN: expand one child, mark it OPEN
 * node == not LEAF && TOUCHED: expand one child, mark it OPEN
 * node == not LEAF && CLOSED && LIVE: SELECT left-most child 
 * node == LEAF && LIVE && OPEN: evaluate, making this node CLOSED
 * node == DEAD: select first LIVE && OPEN/TOUCHED/CLOSED brother of this node
 * node == CLOSED: UPDATE node.parent
 * klopt dit? alle gevallen gehad? gaat de select na de update goed, neemt die de
 * ub/lb en a/b currect over?
 * 
 * Doet OPEN/TOUCHED/CLOSED er toe? Only LEAF/INNER en LIVE/DEAD?
 * SELECT: compute ab/b and select left-most live child. if not exist then EXPAND. if leaf then evalute and UPDATE
 * UPDATE: update parents lb/ub until no change, then SELECT (root/or node does not matter)
 * if node==LIVE/OPEN   then SELECT(node) -> push leftmost LIVE/OPEN child
 * if node==DEAD/CLOSED then UPDATE(node) -> push parent
 * EVALUATE transforms OPEN to CLOSED
 * UPDATE transforms CLOSED to OPEN (if no changes to bounds)
 * is CLOSED: DEAD en OPEN: LIVE?
 */

/*

rule: processing/accessing of node can only be done locally
parent or child cannot be be processed/accessed, but can be scheduled (for processing/accessing on other machine)
why? since in this way no synchronous remote lookups happen.
all remote access is asynchronous & thus zero synchronization overhead

SELECT:
if node.live then
  if node.leaf then 
    schedule node PLAYOUT
  else 
    for all c=kids do
    if c.live && puo not exhausted then
       schedule c EXPAND
else  (dead node, abcutoff)
    cutoff record global-unorderedness
    schedule parent BESTCHILD node (cutting child is best child)
    schedule node UPDATE 

EXPAND:
node.create (before node creation liveness can not be determined)
if node.live then
  schedule node SELECT
  else 
    do nothing (let node die)

PLAYOUT:
node.evaluate
schedule parent as node BESTCHILD node as child
schedule parent as node UPDATE

BESTCHILD:
if child.ab > node.ab then node.bestchild = child

UPDATE: 
node.ab = max/min node.ab and node.bestchild.ab or max/min of b/a
node.live = updated (parent?)
if values changed then
  schedule parent as node BESTCHILD node as child
  schedule parent as node UPDATE
  for all c=kids do
    schedule c DOWNWARD with node.ab as parent.ab

DOWNWARD:
  node.ab = max/min node.ab and parent.ab
  if values changed then
    for all c=kids do
      schedule c DOWNWARD with node.ab as parent.ab

*/

/***************************
 ** GLOBAL VARIABLES      **
 **************************/

 
node_type *root = NULL;
int my_process_id; // MPI rank
#ifdef GLOBAL_QUEUE
job_type *queue[N_MACHINES][N_JOBS][JOB_TYPES];
int top[N_MACHINES][JOB_TYPES];
int max_q_length[N_MACHINES][JOB_TYPES];
#else
job_type *local_queue[N_MACHINES][N_JOBS];
int local_top[N_MACHINES];
job_type *local_buffer[N_MACHINES][N_MACHINES][BUFFER_SIZE];
int buffer_top[N_MACHINES][N_MACHINES];
int max_q_length[N_MACHINES];
#endif
int total_jobs = 0;
pthread_mutex_t jobmutex[N_MACHINES];
pthread_mutex_t global_jobmutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t job_available[N_MACHINES];
//pthread_cond_t global_job_available = PTHREAD_COND_INITIALIZER;
pthread_mutex_t treemutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t donemutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t global_queues_mutex = PTHREAD_MUTEX_INITIALIZER;
int n_par = 1;

// all these will no longer work...
int sum_global_selects = 0;
int sum_global_leaf_eval = 0;
int sum_global_updates = 0;
int sum_global_downward_aborts = 0;
int global_empty_machines = N_MACHINES;

int global_selects[N_MACHINES];
int global_leaf_eval[N_MACHINES];
int global_updates[N_MACHINES];
int global_downward_aborts[N_MACHINES];

int global_no_jobs[N_MACHINES];
int global_done = FALSE;
int global_in_wait = 0;

/* statistics to track which child caused cutoffs at CUT nodes, measure for the orderedness of the tree */
double global_unorderedness_seq_x[TREE_DEPTH];
int global_unorderedness_seq_n[TREE_DEPTH];

// return the index of the first live child
int first_live_child(node_type *node) {
  int n = 0;
  for (n = 0; n < node->n_children; n++) {
    if (node->live_children[n]) {
      return n;
    }
  }
  return NO_LIVE_CHILD;
}

/******************************
 *** SELECT                 ***
 ******************************/

// traverse to the left most deepest open node or leaf node
// but only one node at a time, since each node has its own home machine
// precondition: my bounds 
void do_select(int my_id, node_type *node) {
  global_selects[node->board]++; // does not work with distributed address spaces
  if (node && live_node(node)) {

    /* 
     * alpha beta update, top down 
     */
    
    // get bounds from parent. this requires a synchronous remote MPI lookup to parent bounds.
    // this might be optimized in the call to (scheduling of) the select. but then they are old bounds,
    // at schedule time. not at execute time. or bounds should be propagated by hte parents to us, when
    // a change happens. so hash entries of a node must have ab bounds, which are asynchronously updated, not on request, but when in changes, change propagation
    //    compute_bounds(node); // will be done by explicit job to updates boundsexplicitly

#undef PRINT_SELECT
#ifdef PRINT_SELECT
    printf("M%d P%d: %s SELECT d:%d  ---   <%d:%d>  \n", 
	   node->board, node->path, node->maxormin==MAXNODE?"+":"-",
	   node->depth,
	   node->a, node->b);
#endif
    if (leaf_node(node)) { // depth == 0; frontier, do playout/eval
      //      printf("M:%d PLAYOUT\n", node->board);
      schedule(my_id, node, PLAYOUT, NO_ARG);
    } else if (live_node(node)) {
      int flc = first_live_child(node); // index of first_live_child
      if (flc != NO_LIVE_CHILD) {
#ifdef PUO 
	int highest_child_in_par = 0;
	if (global_unorderedness_seq_n[node->depth]) {
	  highest_child_in_par = global_unorderedness_seq_x[node->depth]/global_unorderedness_seq_n[node->depth];
	} else {
	  highest_child_in_par = flc;
	}
	if (highest_child_in_par < 1 || highest_child_in_par > TREE_WIDTH) {
	  highest_child_in_par = 1;
	}
#else
	int highest_child_in_par = n_par;
#endif
	//	betekent dit dat we in subsequent passes wel of niet voorbij n_par kunnen komen? pakken we in de tweede pass de draad op waar de in de eerste gebleven waren? starten we bij kind n of bij kind 0 (overnieuw)?

	for (int p = flc; p < higest_child_in_par; p++) {
	  if (node->live_children[p] && !seq(node)) {
	    int child_path = 10 * node->path + p + 1;
moet hier node->children_at[p] gezet worden?
	    schedule(my_id, of hier child_path... node->children_at[p] nee moet een node pointer zijn, EXPAND, 
		     child_path, node->wa, node->wb); 
 toch liever niet? node wordt remote pas gecreeerd, hier kunnen we hooguit de naam van de node geven, het pad, en dan wordt hij remote gemaakt
	  } 
	}
      }
    } else if (dead_node(node) && !root_node(node)) { // cutoff: alpha==beta
      // record cutoff and do this in statistics which child number caused it
      global_unorderedness_seq_x[node->depth] += (double)child_number(node->path);
      global_unorderedness_seq_n[node->depth]++;
      /*
      printf("guos(%d): %lf/%d\n", node->depth,
	     global_unorderedness_seq_x[node->depth],
	     global_unorderedness_seq_n[node->depth]);
      */
#define UPDATE_AFTER_CUTOFF
#ifdef UPDATE_AFTER_CUTOFF
      if (node->parent) {
do we need BESTCHILD anymore?
	schedule(my_id, node->parent, BESTCHILD, node);
	schedule(my_id, node->parent, UPDATE, {int from_me = node->board;}, lb, ub);
      }
#endif
    } else {
      printf("M%d ERROR: not leaf, not dead, not live: %d\n", 
	     node->board, node->path);
      print_tree(root, 2);
      exit(0);
    }
  }
}


/********************************
 *** EXPAND                   ***
 ********************************/

    /*
Hmm. Dit is apart. Nieuwe nodes worden op hun home machine gemaakt.
In de TT; en ook als job in de job queue.
dus new_leaf is een RPC?
En wat is de betekenis van de pointer die new_leaf opleverd als de nieuwe leaf
	    op een andere machine zit? In SHM is dat ok, maar later in Distr Mem
	    Is de pointer betekenisloos of misleidend.

	   OK. Laten we voor SHM en threads het maar even zo laten dan.
    */
// Add new children, schedule them for selection
// Can this work? it references nodes (children) at other home machines
// must find out if remote pointers is doen by new_leaf or by schedule
node_type *do_expand(int parent_id, node_type *node, parent_wa, parent_wb) {
  node_type *node = lookup(node_path);
  if (!node) {
    // existing nodes are not created
    // exisitng nodes are traveersed, and then SELECT?
    node = new_leaf(parent_id);  XXXXXXXX
    dit kan niet zo. new_leaf bevat de board berekening, daar pas wordt bepaald op welke machine de node 
      terechtkomt. Dus de new_leaf moet al voor verzending plaatsvinden, anders weet je niet aan wie je het moet verzenden. ok. simpel. dus new_leaf in de select voor de schedule zetten.

      copieer je een node of alleen het pad (een pointer). aleen een pointer kan niet, want voor het reconstrueren van alle waarden heb je de node nodig, en die zit in een gegeugen elders.

      dus toch de hele node oversturen (deep copy)
    //copy in wa wb bounds
    
    node->path = node_path;
    store(node);
  } 
  // make sure existing children get the new wa and wb bounds of their parent
  node->wa = parent_wa;
  node->wb = parent_wb;

  if (node_live(node)) {
    schedule(my_id, node, SELECT, NO_ARG);
  }
}


/******************************
 *** PLAYOUT                ***
 ******************************/

// just std ab evaluation. no mcts playout
void do_playout(int my_id, node_type *node) {
  node->a = node->b = node->lb = node->ub = evaluate(node);
  
  printf("M%d P%d: PLAYOUT d:%d    A:%d\n", 
   	 node->board, node->path, node->depth, node->a);
  
  // can we do this? access a pointer of a node located at another machine?
  //  schedule(node->parent, UPDATE, node->lb, node->ub);
  if (node->parent) {
    //    set_best_child(node);
    //    node->parent->best_child = node;
    schedule(my_id, node->parent, UPDATE, {int from_me = my_id}, node->lb, node->ub);
  }
}

int evaluate(node_type *node) {
  //  return node->path;
  global_leaf_eval[node->board]++;
  srand(node->path); // create deterministic leaf values
  return rand() % (INFTY/8) - (INFTY/16);
}

/*****************************
 *** UPDATE                ***
 *****************************/

// backup through the tree
// best_lb and best_ub are the highest lb of all closed kids and the lowest ub of all closed kids... or is it the highest lb and ub in max nodes, and the lowest lb and ub in min nodes???????? or is it just the latest lb and ub and are the best values computed here, in this code, by node?
// possibly different children
void do_update(int my_id, node_type *node, int from_child, int lb, int ub) {
  //  printf("%d UPDATE\n", node->path);

  if (node && node->best_child) {
    int continue_updating = 0;
    
    global_updates[node->board]++;

    if (node->maxormin == MAXNODE) {
      int old_a = node->a;
      node->a = max(node->a, node->best_child->a);
      node->b = max_of_beta_kids(node); //  infty if unexpanded kids
      // if we have expanded a full max node, then a beta has been found, which should be propagated upwards to my min parenr
      continue_updating = (node->b != INFTY || node->a != old_a);
      //      node->lb = max(node->lb, node->best_child->lb);
      //      node->ub = max_of_ub_kids(node);
      // keep track of open children
      if (node->open_child[from_child]) {
	node->n_open_kids --;
	node->open_child[from_child] = FALSE;
      }
      node->lb = max(node->lb, best_lb);
      node->ub = node->n_open_kids < 1 ? INFTY : node->max_of_closed_kids_ub;
      node->max_of_closed_kids_ub = max(node->max_of_closed_kids_ub, ub);
    }
    if (node->maxormin == MINNODE) {
      node->a = min_of_alpha_kids(node);
      int old_b = node->b;
      node->b = min(node->b, node->best_child->b);
      continue_updating = (node->a != -INFTY || node->b != old_b); // if a full min node has been expanded, then an alpha has been bound, and we should propagate it to the max parent
      node->lb = min_of_lb_kids(node);
      node->ub = min(node->ub, node->best_child->ub);
    }
    /*
    check_abort(node); or better: propagate updates downward, not necessarily aborting, but must at least update bounds.
    Should not we here check if any of the children die, so that they can be removed from the job queues and the search of them is stopped?

				      par search is more than ensemble. in ensemble search all searches are independent. in par they are dependent, the influence each other, one result may stop 
				      bound propagtion, is that asynch to job queue, just scan all jobs for subchildren, and update bound, or can we send an update/select job to the queues?
    */
#undef PRINT_UPDATE
#ifdef PRINT_UPDATE
    if (node && node->parent) {
      printf("M%d P%d %s UPDATE d:%d  --  %d:<%d:%d> n_ch:%d\n", 
	     node->board, node->path, node->maxormin==MAXNODE?"+":"-", 
	     node->depth, node->parent->path,
	     node->a, node->b, node->n_children);
    }
#endif

    //    if (node == root) {
    //      printf("Updating root <%d:%d>\n", node->a, node->b);
    //    }
    
    if (continue_updating) {
      // schedule downward update to parallel searches
      downward_update_children(my_id, node);
      // schedule upward update
      if (node->parent) {
	//	if (node->parent->best_child) {
	set_best_child(node);
	//	} 
	//      	printf("%d schedule update %d\n", node->path, node->parent->path);
	schedule(my_id, node->parent, UPDATE, from_me, lb, ub);
      }
    } else {
      // keep going, no longer autmatic select of root. select of this node
      //      schedule(node, SELECT);
    }
  }
}

// in a parallel search when a bound is updated it must be propagated to other
// subtrees that may be searched in parallel
void downward_update_children(int my_id, node_type *node) {
  //  return;
  if (node) {
    for (int ch = 0; ch < node->n_children && node->children[ch]; ch++) {
      node_type *child = node->children[ch]; 
      int continue_update = 0;
      // this direct updating of children
      // onlworks in shared memeory.
      // in distributed memory the bounds have to be 
      // sent as messages to the remote machine
      int was_live = live_node(child);
      continue_update |= child->a < node->a;
      child->a = max(child->a, node->a);
      continue_update |= child->b > node->b;
      child->b = min(child->b, node->b);
      if (continue_update) {
	schedule(my_id, child, BOUND_DOWN);
	if (was_live && dead_node(child)) {
	  //	  printf("DOWN: %d <%d:%d>\n", child->path, child->a, child->b);
      	  global_downward_aborts[node->board]++;
	  //
	}
      }
    }
  }
}

// process new bound, update my job queue for selects with this node and then propagate down
void do_bound_down(int my_id, node_type *node) {
  if (node) {
    if (update_selects_with_bound(node)) {
      downward_update_children(my_id, node);
    }
  }
}

// end
