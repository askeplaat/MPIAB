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
int first_expanded_live_child(node_type *node) {
  int n = 0;
  //  printf("%d FLC: N: %d ", my_process_id, node->n_children); print_path(node->path, node->depth);
  // TREE_WIDTH >= n_expanded_children >= n_live_children >= 0
  for (n = 0; n < node->n_expanded_children; n++) {
    if (node->expanded_children[n] && node->live_children[n]) {
      return n;
    }
  }
  return NO_LIVE_CHILD;
}


int first_unexpanded_child(node_type *node) {
  int n = 0;

  for (n = 0; n < node->n_expanded_children; n++) {
    if (!node->expanded_children[n]) {
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
  printf("SELECT SHOULD NOT BE INVOKED\n");
  exit(0);
  if (!node) {
    printf("ERROR: child not found in select\n");
    exit(0);
  } else /* if (live_node(node)) */ {
    printf("IN SELECT. Node: %p. Live: %d. n_exp_children: %d;  n_live_children: %d\n", 
	   node, live_node(node), node->n_expanded_children, node->n_live_children);
  
    if (node->depth == TREE_DEPTH && root != node) {
      printf("ERROR: root != node while depth is: %d\n", node->depth);
      exit(0);
    }
    if (node->depth == TREE_DEPTH) {
      printf("SELECT ROOT root children: %d/%d\n", node->n_expanded_children, node->n_live_children);
    }

#define PRINT_SELECT 
#ifdef PRINT_SELECT
    printf("M%d  %s SELECT d:%d  ---  ab <%d:%d>  lu <%d,%d>    \n", 
	   node->board,  node->maxormin==MAXNODE?"+":"-",
	   node->depth,
	   node->a, node->b, node->lb, node->ub);
#endif
    if (leaf_node(node)) { // depth == 0; frontier, do playout/eval
      do_playout(node);
    } else if (live_node(node)) {
      printf("LIVE NODE live: %d, ab: <%d,%d>, lu: <%d,%d>   ", 
	     live_node(node), node->a, node->b, node->lb, node->ub); print_path(node->path, node->depth);
      int flc = first_expanded_live_child(node); // index of first_live_child
      printf("FLC: %d\n", flc);
      if (flc == NO_LIVE_CHILD) {
	/*
no live child is dubbelzinnig. het lan betekenen no children expanded
het kan ook betekeknen wel kinderen maar allemaal dood
  first live child moet het eerste live child teruggeven, en als er geen zijn dan dus een expand, en als er geen geexpandeerd meer kunnen worden dan dus broer.
dan moet ik een msg sturen dat parent naar een broer gaat
	*/
	// create new children; expand
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
      	printf("Highest child in par is %d.\n", highest_child_in_par);

	int children_created = 0;
	int p ;
	for (p = 0; p < TREE_WIDTH; p++) {
	  if (node->live_children[p] && !seq(node)) {
	    printf("%d CALLING CREATE CHILD (%d) AT FROM DO_SELECT node/active: %d root/active: %d   ", 
		   my_process_id, p, node->n_active_kids, 
		   root->n_active_kids); print_path(node->path, node->depth);
	    create_child_at(node, p, opposite(node));
	    children_created++;
	    if (children_created > highest_child_in_par) {
	      break;
	    } // if break
	  } // if seq
	} // for p
	printf("out of for. p is %d\n", p);
	print_tree(root->path, TREE_DEPTH);
	if (!children_created) {
	  printf("ERROR, node is selected in AB but no live or unexpanded children. n_expanded_kids: %d   ", 
		 node->n_expanded_children); print_path(node->path, node->depth);
	  print_path(node->path, node->depth);
	  print_tree(root->path, TREE_DEPTH);
	  fflush(stdout);
	  /*	  select_brother_at(node);
hoe kan het dat ik hier uit kom? Hoe kan het dat een dode node geselecteerd wordt?
  als alle kids geexpandeerd zijn, dan mmoet er toch een updaet langsgekomen zijn die de node ook 
dood gemaakt heeft?
  well, that is not the case for MCTS. MCTS re-visits dead nodes, when they are the best.

What about AB?
	  */
	}
      } else { // flc is existing child
	//	printf("FLC IS VALID -- ");
	//      	printf("%d Doing select child at of child %d  ", 
	//	     my_process_id, flc); print_path(node->path, node->depth);
	if (node->children_at[flc] != INVALID) {
	  select_child_at(node, flc, opposite(node));
	} else {
	  create_child_at(node, flc, opposite(node));
	}
      }
    } else if (dead_node(node) && !root_node(node)) { // cutoff: alpha==beta
      printf("DEAD NODE\n");
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
      printf("CUTOFF update lu: <%d,%d>\n", node->lb, node->ub);
      update_parent_at(node, node->my_child_number);
	//	schedule(my_id, node, UPDATE, node->my_child_number, lb, ub);
	//    }
#endif
    } else {
      printf("M%d ERROR: not leaf, not dead, not live: %d\n", 
	     node->board, node->path);
      //      print_tree(root, 2);
      exit(0);
    } // live or dead
  } // !node or node or dead
}

void expand_new_children(node_type *node, int funexc) {
  // create new children; expand
#ifdef PUO 
  int highest_child_in_par = 0;
  if (global_unorderedness_seq_n[node->depth]) {
    highest_child_in_par = global_unorderedness_seq_x[node->depth]/global_unorderedness_seq_n[node->depth];
  } else {
    highest_child_in_par = funexc;
  }
  if (highest_child_in_par < 1 || highest_child_in_par > TREE_WIDTH) {
    highest_child_in_par = 1;
  }
#else
  int highest_child_in_par = n_par;
#endif   
  printf("Highest child in par is %d.\n", highest_child_in_par);

  int children_created = 0;
  int p ;
  for (p = 0; p < TREE_WIDTH; p++) {
    if (!node->expanded_children[p] && !seq(node)) {
      printf("%d CALLING CREATE CHILD (%d) AT FROM DO_SELECT node/active: %d root/active: %d   ", 
		   my_process_id, p, node->n_active_kids, 
		   root->n_active_kids); print_path(node->path, node->depth);
      create_child_at(node, p, opposite(node));
      children_created++;
      if (children_created > highest_child_in_par) {
	break;
      } // if break
    } // if seq
  } // for p
  printf("out of for. p is %d\n", p);
  if (!children_created) {
    printf("ERROR, node is selected in AB but no live or unexpanded children. n_expanded_kids: %d, rootdepth: %d, nodedepth: %d  ", 
	   node->n_expanded_children, root->depth, node->depth); print_path(node->path, node->depth);
    print_path(node->path, node->depth);
    print_tree(root->path, TREE_DEPTH);
  } 
}


/******************************
 *** SELECT2                ***
 *** 1. if expanded live children, select first
 *** 2. if unexpanded children, expand first
 *** 3. otherwise (only dead children), update (lb/ub) of dead node
 ******************************/

// traverse to the left most deepest open node or leaf node
// but only one node at a time, since each node has its own home machine
// precondition: my bounds 
void do_select2(node_type *node) {
  if (!node) {
    printf("ERROR: child not found in select\n");
    exit(0);
  }
  
  printf("M%d  %s SELECT2 d:%d  ---  ab <%d:%d>  lu <%d,%d>    \n", 
	 node->board,  node->maxormin==MAXNODE?"+":"-",
	 node->depth,
	 node->a, node->b, node->lb, node->ub);
  
  // LEAF

  if (leaf_node(node)) { // depth == 0; frontier, do playout/eval
    do_playout(node);
    return;
  } 
    
  if (live_node(node)) {
    printf("LIVE NODE live: %d, ab: <%d,%d>, lu: <%d,%d>   ", 
	   live_node(node), node->a, node->b, node->lb, node->ub); print_path(node->path, node->depth);

    // LIVE EXPANDED 

    int felc = first_expanded_live_child(node); // index of first_live_child
    printf("FELC: %d depth: %d   ", felc, node->depth); print_path(node->path, node->depth);
    if (felc != NO_LIVE_CHILD) {
      if (node->children_at[felc] == INVALID) {
	printf("ERROR: invalid machine at child\n");
      }
      select_child_at(node, felc, opposite(node));
      return;
    }

    // UNEXPANDED
    // no expanded live children, try unexpanded

    int funexc = first_unexpanded_child(node);
    if (funexc == NO_LIVE_CHILD) {
      expand_new_children(node, funexc);
      return;
    } 
  } else if (dead_node(node) && !root_node(node)) { // cutoff: alpha==beta
    // no live or unexpanded children, we only have dead, return an update
    printf("DEAD NODE\n");
    // record cutoff and do this in statistics which child number caused it
    global_unorderedness_seq_x[node->depth] += (double)node->my_child_number; //child_number(node->path);
    global_unorderedness_seq_n[node->depth]++;

    printf("CUTOFF update lu: <%d,%d>\n", node->lb, node->ub);
    update_parent_at(node, node->my_child_number);
    return;

  } // live or dead

} // do_select2




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

  //  printf("%d EXPAND: depth: %d  ", my_process_id, msg->depth); print_path(msg->path, msg->depth);

  node_type *node = lookup(msg->path, msg->depth);

  // receiver must store parent-at, and copy wa wb

  if (!node) {
    // existing nodes are not created
    // exisitng nodes are traveersed, and then SELECT?

    node = new_leaf(msg, msg->child_number, msg->mm); 
    if (!node) {
      printf("NEW LEAF returned null\n");
    }
    printf("EXPAND depth: %d:%d. childnumber is: %d while path is ", msg->depth, node->depth, msg->child_number); print_path(msg->path, msg->depth);

    node->parent_at = msg->parent_at;
    node->path = msg->path;
    printf("Storing node "); print_path(node->path, node->depth);
    /*
    if (node->depth == TREE_DEPTH) {
      printf("Setting as Root %d   ", node->depth); print_path(node->path, node->depth);
      root = node;
    }
    */
    store(node);
  }  else {
    printf("Node existed "); print_path(node->path, node->depth);
  }
  // make sure existing children get the new wa and wb bounds of their parent
  node->wa = msg->wa;
  node->wb = msg->wb;

  if (live_node(node)) {
    //    schedule(my_id, node, SELECT, NO_ARG);
    do_select2(node); // at same machine
  }
}


/******************************
 *** PLAYOUT                ***
 ******************************/

// just std ab evaluation. no mcts playout
void do_playout(node_type *node) {
  if (!node || !live_node(node)) {
      return;
  }
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
  update_parent_at(node, node->my_child_number);
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
  if (!node || !live_node(node) ) {
    printf("ERROR: parent not found in update or not live --  "); print_path(msg->path, msg->depth);
    //    exit(0);
    return;
  }
  node->depth = msg->depth;
  int from_child_number = msg->from_child;
  int old_lb = node->lb_of_child[from_child_number];
  int old_ub = node->ub_of_child[from_child_number];
  int lb_of_child =  node->lb_of_child[from_child_number] = msg->lb;
  int ub_of_child =  node->ub_of_child[from_child_number] = msg->ub;
  node->maxormin = msg->mm;
  node->n_active_kids--;
  if (node == root) {
    printf("***************************************************************************************UPDATING ROOT\n");
  }

  printf("%d UPDATE from:%d, lb:%d, ub:%d mm:%d, d:%d, active:%d root/active:%d   ", 
	 node->path, from_child_number, lb_of_child, ub_of_child, node->maxormin, 
	 node->depth, node->n_active_kids, root->n_active_kids);
  print_path(node->path, node->depth);

  if (node) {
    int continue_updating = 0;
    
    //    global_updates[node->board]++;

    printf("N_EXPANDED_KIDS is %d.  msg-child_live: %d. from_child: %d. live[from_child]: %d  lu: <%d,%d>", 
	   node->n_expanded_children, msg->child_live, from_child_number, node->live_children[from_child_number], lb_of_child, ub_of_child); 
    print_path(node->path, node->depth);
    /*
management of open kids/expanded children
is now done in create_child_at

    // if was live and now dead, so decr n_open_kids
    if (node->n_expanded_children < TREE_WIDTH &&
	node->live_children[from_child_number] && !msg->child_live) {
	node->n_expanded_children ++;
	printf("N_EXPANDED_KIDS updated to %d  ", node->n_expanded_children); print_path(node->path, node->depth);
    }
    */

    if (node->maxormin == MAXNODE) {
      // if we have expanded a full max node, then a beta has been found, which should be propagated upwards to my min parenr
      node->lb = max(node->lb, lb_of_child);
      /*
      //      node->max_of_closed_kids_lb = max(node->max_of_closed_kids_lb, lb_of_child);
      node->max_of_closed_kids_ub = max(node->max_of_closed_kids_ub, ub_of_child);
      node->lb = node->n_expanded_children < TREE_WIDTH ? INFTY : node->lb;
      node->ub = node->n_expanded_children < TREE_WIDTH ? INFTY : node->max_of_closed_kids_ub;
      */
      node->ub = max_of_kids(node->ub_of_child);

      printf("%d continue update %d  child %d lu: <%d,%d> m-m+: <%d,%d>\n", 
	     node->path, continue_updating, node->my_child_number, node->lb, node->ub, node->min_of_closed_kids_lb, node->max_of_closed_kids_ub);
    }
    if (node->maxormin == MINNODE) {
      node->ub = min(node->ub, ub_of_child);
      /*
      node->min_of_closed_kids_lb = min(node->min_of_closed_kids_lb, lb_of_child);
      //      node->min_of_closed_kids_ub = min(node->min_of_closed_kids_ub, ub_of_child);
      node->lb = node->n_expanded_children < TREE_WIDTH ? -INFTY : node->min_of_closed_kids_lb;
      node->ub = node->n_expanded_children < TREE_WIDTH ? -INFTY : node->ub;
      */
      node->lb = min_of_kids(node->lb_of_child);

      printf("%d continue update %d  child %d lu: <%d,%d> m-m+: <%d,%d>\n", 
	     node->path, continue_updating, node->my_child_number, node->lb, node->ub, node->min_of_closed_kids_lb, node->max_of_closed_kids_ub);
    }
    
    node->live_children[from_child_number] = msg->child_live;
    printf("Setting child %d node to live: %d  ", from_child_number, msg->child_live); print_path(node->path, node->depth);
    //node->lb < node->ub; dit klopt niet. dit is of de huidige node live is. niet het kind.

    //    //    printf("Continue update is %d. lb: %d, ub: %d, oldlb:%d, oldub:%d live: %d\n", 
    //	   continue_updating, node->lb, node->ub, old_lb, old_ub, msg->child_live);

    //    if (node == root) {
    //      printf("Updating root <%d:%d>\n", node->a, node->b);
    //    }
    
    continue_updating = (node->ub != old_ub || node->lb != old_lb);
    if (continue_updating) {
      // schedule downward update to parallel searches
      // downward_update_children(node);
      //      so downard update is scheduled for all children, irregardless of theire existence. also for non-expanded children
      for (int ch = 0; ch < node->n_expanded_children; ch++) {
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
      printf("%d schedule further parent update %d <%d,%d>\n", node->path, node->my_child_number, node->lb, node->ub);
      update_parent_at(node, node->my_child_number);
	//      }
    } else {
      decr_active_root();
      // keep going, no longer autmatic select of root. select of this node
      //      schedule(node, SELECT);
    }
    print_tree(root->path, TREE_DEPTH);
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
  if (!node || !live_node(node)) {
    printf("ERROR: child not found in update or not live\n"); print_path(msg->path, msg->depth);
    return; 
    //    exit(0);
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
      for (int ch = 0; ch < node->n_expanded_children; ch++) {
	if (node->live_children[ch]) {
	  update_child_at(node, ch);
	}
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
