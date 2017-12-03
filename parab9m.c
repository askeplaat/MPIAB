#include <stdio.h>         // for print
#include <stdlib.h>        // for rand
#include <assert.h>        // for print
//#include <pthread.h>
#include <mpi.h>
#include "parabm.h"         // for prototypes and data structures



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
#ifndef MSGPASS
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
#endif
int total_jobs = 0;
/*
pthread_mutex_t jobmutex[N_MACHINES];
pthread_mutex_t global_jobmutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t job_available[N_MACHINES];
//pthread_cond_t global_job_available = PTHREAD_COND_INITIALIZER;
pthread_mutex_t treemutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t donemutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t global_queues_mutex = PTHREAD_MUTEX_INITIALIZER;
*/
int n_par = 1;

hash_entry_type hash_table[HASH_TABLE_SIZE];

// all these will no longer work...
int sum_global_selects = 0;
int sum_global_leaf_eval = 0;
int sum_global_updates = 0;
int sum_global_downward_aborts = 0;

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
  //  printf("%d FLC: N: %d ", my_process_id, node->n_children); print_path(node->path, node->depth);
  for (n = 0; n < node->n_children; n++) {
    if (node->live_children[n]) {
      //      printf("%d LIVE child %d ", my_process_id, n); print_path(node->path, node->depth);
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
void do_select(node_type *node) {
  if (!node) {
    printf("ERROR: child not found in select\n");
    exit(0);
  }
  printf("IN SELECT. Node: %p. Live: %d\n", node, live_node(node));
  if (node) {

#define PRINT_SELECT
#ifdef PRINT_SELECT
    printf("M%d  %s SELECT d:%d  ---   <%d:%d>  \n", 
	   node->board,  node->maxormin==MAXNODE?"+":"-",
	   node->depth,
	   node->a, node->b);
#endif
    if (leaf_node(node)) { // depth == 0; frontier, do playout/eval
      do_playout(node);
    } else if (live_node(node)) {
      int flc = first_live_child(node); // index of first_live_child
      printf("FLC: %d\n", flc);
      if (flc == NO_LIVE_CHILD) {
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
	printf("Highest child in par is %d\n", highest_child_in_par);

	int children_created = 0;
	for (int p = 0; p < TREE_WIDTH; p++) {
	  if (node->live_children[p] && !seq(node)) {
	    node->n_children ++;
	    printf("%d CALLING CREATE CHILD (%d) AT FROM DO_SELECT node/active: %d root/active: %d   ", 
		   my_process_id, p, node->n_active_kids, root->n_active_kids); print_path(node->path, node->depth);
	    /*
	    this creates a child, I really need to create a brother (same level, not a level deeper to the leaves). no, I do need to create a child, but for some reason it creates the wrong one. I am at 0.0, need to create 0.0.1, and do create 0.1.0
 the p in the argument is not passed to the new_leaf, but taken as the id of the parent
	    */
	    create_child_at(node, p, opposite(node));
	    children_created++;
	    // do not create more children in par than 
	    if (children_created > highest_child_in_par) {
	      break;
	    }
	  } // if
	} // for
      } else { // flc is existing child
	//	printf("%d Doing select child at of child %d  ", 
	//     my_process_id, flc); print_path(node->path, node->depth);
	select_child_at(node, flc, opposite(node));
      }
    } else if (dead_node(node) && !root_node(node)) { // cutoff: alpha==beta
      // record cutoff and do this in statistics which child number caused it
      global_unorderedness_seq_x[node->depth] += (double)node->my_child_number; //child_number(node->path);
      global_unorderedness_seq_n[node->depth]++;
      /*
      printf("guos(%d): %lf/%d\n", node->depth,
	     global_unorderedness_seq_x[node->depth],
	     global_unorderedness_seq_n[node->depth]);
      */
#define UPDATE_AFTER_CUTOFF
#ifdef UPDATE_AFTER_CUTOFF
//      if (node->parent) {
	//do we need BESTCHILD anymore? update now has a way of updating values without needing the bestchild pointer
	//	schedule(my_id, node->parent, BESTCHILD, node);
	// update really schdules the parent, but I do not have the parent node here, so UPDATE has to fudge something. 
      printf("CUTOFF\n");
      update_parent_at(node, my_process_id);
	//	schedule(my_id, node, UPDATE, node->my_child_number, lb, ub);
	//    }
#endif
    } else {
      printf("M%d ERROR: not leaf, not dead, not live: %d\n", 
	     node->board, node->path);
      //      print_tree(root, 2);
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
void do_expand(child_msg_type *msg) {
  if (msg->depth <0 || msg->depth > TREE_DEPTH) {
    printf("ERROR in Do Expand depth %d\n", msg->depth);
  }

  printf("%d EXPAND: depth: %d  ", my_process_id, msg->depth); print_path(msg->path, msg->depth);

  node_type *node = lookup(msg->path, msg->depth);

  // receiver must store parent-at, and copy wa wb

  printf("EXPAND. childnumber is: %d while path is ", msg->child_number); print_path(msg->path, msg->depth);
  if (!node) {
    // existing nodes are not created
    // exisitng nodes are traveersed, and then SELECT?

    node = new_leaf(msg, msg->child_number, msg->mm); 
    if (!node) {
      printf("NEW LEAF returned null\n");
    }
    node->parent_at = msg->parent_at;
    node->path = msg->path;
    //    printf("Storing node "); print_path(node->path, node->depth);
    store(node);
  }  else {
    printf("Node existed "); print_path(node->path, node->depth);
  }
  // make sure existing children get the new wa and wb bounds of their parent
  node->wa = msg->wa;
  node->wb = msg->wb;

  if (live_node(node)) {
    //    schedule(my_id, node, SELECT, NO_ARG);
    do_select(node); // at same machine
  }
}


/******************************
 *** PLAYOUT                ***
 ******************************/

// just std ab evaluation. no mcts playout
void do_playout(node_type *node) {
  node->a = node->b = node->lb = node->ub = evaluate(node);
  
  printf("M%d PLAYOUT d:%d    A:%d    ", 
   	 node->board,  node->depth, node->a);
  print_path(node->path, node->depth);
  
  // can we do this? access a pointer of a node located at another machine?
  //  schedule(node->parent, UPDATE, node->lb, node->ub);
  //  if (node->parent) {
    //    set_best_child(node);
    //    node->parent->best_child = node;
    //    schedule(my_id, node->parent, UPDATE, {int from_me = my_id}, node->lb, node->ub);
  update_parent_at(node, my_process_id);
    //  }
}

int evaluate(node_type *node) {
  //  return node->path;
  global_leaf_eval[node->board]++;
  srand(hash(node->path, node->depth)); // create deterministic leaf values
  return rand() % (INFTY/8) - (INFTY/16);
}

/*****************************
 *** UPDATE                ***
 *****************************/

// backup through the tree
// best_lb and best_ub are the highest lb of all closed kids and the lowest ub of all closed kids... or is it the highest lb and ub in max nodes, and the lowest lb and ub in min nodes???????? or is it just the latest lb and ub and are the best values computed here, in this code, by node?
// possibly different children
void do_update(parent_msg_type *msg) {
  if (msg->depth <0 || msg->depth > TREE_DEPTH) {
    printf("ERROR in Do Update depth %d\n", msg->depth);
  }
  node_type *node = lookup(msg->path, msg->depth);
  if (!node) {
    printf("ERROR: parent not found in update --  "); print_path(msg->path, msg->depth);
    //    exit(0);
    return;
  }
  node->depth = msg->depth;
  int from_child_number = msg->from_child;
  int lb_of_child = msg->lb;
  int ub_of_child = msg->ub;
  node->maxormin = msg->mm;
  node->n_active_kids--;
  if (node == root) {
    printf("***************************************************************************************UPDATING ROOT\n");
  }

  printf("%d UPDATE from:%d, lb:%d, ub:%d mm:%d, d:%d, active:%d root/active:%d\n", 
	 node->path, from_child_number, lb_of_child, ub_of_child, node->maxormin, 
	 node->depth, node->n_active_kids, root->n_active_kids);

  if (node) {
    int continue_updating = 0;
    int old_lb = node->lb;
    int old_ub = node->ub;
    
    //    global_updates[node->board]++;

    if (node->maxormin == MAXNODE) {
      //      I think that we should only update lb/ub. a/b should not be updated bottom up, only be passed in top down
	//      int old_a = node->a;
	//      node->a = max(node->a, node->best_child->a);
	//      node->b = max_of_beta_kids(node); //  infty if unexpanded kids
      // if we have expanded a full max node, then a beta has been found, which should be propagated upwards to my min parenr
      //      node->lb = max(node->lb, node->best_child->lb);
      //      node->ub = max_of_ub_kids(node);
      // keep track of open children
      if (node->open_child[from_child_number]) {
	node->n_open_kids --;
	node->open_child[from_child_number] = FALSE;
      }
      node->lb = max(node->lb, lb_of_child);
      node->max_of_closed_kids_ub = max(node->max_of_closed_kids_ub, ub_of_child);
      node->ub = node->n_open_kids > 0 ? INFTY : node->max_of_closed_kids_ub;
      continue_updating = (node->ub != INFTY || node->lb != old_lb);
    }
    if (node->maxormin == MINNODE) {
      //      node->a = min_of_alpha_kids(node);
      //      int old_b = node->b;
      //      node->b = min(node->b, node->best_child->b);
      node->ub = min(node->ub, ub_of_child);
      node->min_of_closed_kids_lb = min(node->min_of_closed_kids_lb, lb_of_child);
      node->lb = node->n_open_kids > 0 ? -INFTY : node->min_of_closed_kids_lb;
      continue_updating = (node->lb != -INFTY || node->ub != old_ub); // if a full min node has been expanded, then an alpha has been bound, and we should propagate it to the max parent
    }
    
    node->live_children[from_child_number] = msg->child_live;
    //node->lb < node->ub; dit klopt niet. dit is of de huidige node live is. niet het kind.

    printf("Continue update is %d. lb: %d, ub: %d, oldlb:%d, oldub:%d live: %d\n", 
	   continue_updating, node->lb, node->ub, old_lb, old_ub, msg->child_live);

    //    if (node == root) {
    //      printf("Updating root <%d:%d>\n", node->a, node->b);
    //    }
    
    if (continue_updating) {
      // schedule downward update to parallel searches
      // downward_update_children(node);
      //      so downard update is scheduled for all children, irregardless of theire existence. also for non-expanded children
      for (int ch = 0; ch < node->n_children; ch++) {
	if (node->live_children[ch]) {
	  printf("Updating live child %d for node  ", ch); print_path(node->path, node->depth);
	  update_child_at(node, ch);
	}
      }
      // schedule upward update
      //      if (node->parent) {
	//	if (node->parent->best_child) {
	//	set_best_child(node);
	//	} 
	//      	printf("%d schedule update %d\n", node->path, node->parent->path);
	//	schedule(my_id, node->parent, UPDATE, from_me, lb, ub);
      update_parent_at(node, my_process_id);
	//      }
    } else {
      decr_active_root();
      // keep going, no longer autmatic select of root. select of this node
      //      schedule(node, SELECT);
    }
  }
}

// in a parallel search when a bound is updated it must be propagated to other
// subtrees that may be searched in parallel
void downward_update_children(update_msg_type *msg) {
  //  return;
  if (msg->depth <0 || msg->depth > TREE_DEPTH) {
    printf("ERROR in Do downward depth %d\n", msg->depth);
  }
  node_type *node = lookup(msg->path, msg->depth);
  if (!node) {
    printf("ERROR: child not found in update\n"); print_path(msg->path, msg->depth);
    exit(0);
  }

  int a_of_parent = msg->a;
  int b_of_parent = msg->b;

  if (node) {
    int continue_update = 0;
    //   int was_live = live_node(node);
    continue_update |= a_of_parent < node->a;
    node->a = max(a_of_parent, node->a);
    continue_update |= b_of_parent > node->b;
    node->b = min(b_of_parent, node->b);
    
    if (continue_update) {
      for (int ch = 0; ch < node->n_children; ch++) {
	update_child_at(node, ch);
      }
    }
  }
}

/*

// process new bound, update my job queue for selects with this node and then propagate down
void do_bound_down(int my_id, node_type *node) {
  if (node) {
    if (update_selects_with_bound(node)) {
      downward_update_children(my_id, node);
    }
  }
}


*/
// end

//(rot13 "zvpxrl@znfgrevatrznpf.bet")
