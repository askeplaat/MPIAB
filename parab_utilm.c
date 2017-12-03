#include <stdio.h>         // for print
#include <stdlib.h>        // for rand
//#include <cilk/cilk.h>     // for spawn and sync
//#include <cilk/cilk_api.h> // for cilk workers report
#include <assert.h>        // for print
#include <mpi.h>
//#include <pthread.h>       // for mutex locks
#include <string.h>        // for strcmp in main()
#include "parabm.h"         // for prototypes and data structures




/************************
 ** JOB              ***
 ***********************/

/*
job_type *new_job(node_type *n, int t, int a) {
  job_type *job = malloc(sizeof(job_type));
  job->node = n;
  job->type_of_job = t;
  job->argument = a;
  return job;
}
*/

/*
int lock(pthread_mutex_t *mutex) {
#ifdef LOCKS
  //  printf("LOCK   %p\n", mutex);
  pthread_mutex_lock(mutex);
#endif
}
int unlock(pthread_mutex_t *mutex) {
#ifdef LOCKS
  //  printf("UNLOCK %p\n", mutex);
  pthread_mutex_unlock(mutex);
#endif
}
*/
/*
int lock_node(node_type *node) {
  if (node) {
    lock(&node->nodelock);  // node
    if (node->parent) {
      lock(&node->parent->nodelock);  // node
    }

    if (0&&node->children) {
      for (int ch = 0; ch < node->n_children && node->children[ch]; ch++) {
	node_type *child = node->children[ch]; 
	printf("l %d ", node->path);
	lock(&child->nodelock);  // node
	printf(" +\n", node->path);
      }
    }
  }
}

int unlock_node(node_type *node) {
  if (node) {
    unlock(&node->nodelock);  // node
    if (node->parent) {
      unlock(&node->parent->nodelock);  // node
    }

    if (0&&node->children) {
      for (int ch = 0; ch < node->n_children && node->children[ch]; ch++) {
	node_type *child = node->children[ch]; 
	unlock(&child->nodelock);  // node
      }
    }
  }
}
*/




int random_table[RANDOM_TABLE_SIZE];


void check_paths(int p1[], int p2[], int d) {
  for (int i = 0; i < TREE_DEPTH; i++) {
    if (p1[i] != p2[i]) {
      printf("ERROR path mismatch:\n");
      print_path(p1, d);
      print_path(p2, d);
    }
  }
}




// depth = 0 for leaves
// depth = big at root
int hash(int path[], int depth) {
  print_path(path, depth);
  //  int h = 0;
  int h = depth * 1234567L;
  for (int i = TREE_DEPTH-1; i >= depth; i--) {
    //  for (int i = depth; i >= 0; i--) {
    printf("HASH(%d): %d\n", i, path[i]);
    h ^= random_table[path[i]];
    h %= 748291034753L;
  }
  /*
  hmm. with this hash function, [0,0,0] hashes to the same value as [0,0,0,0]. this is not good.
different depth should hash to different values.
    But still. a modulo of 0 remains a 0. and if they are all 0, then nothing changes.
need different hash functino that takes depht into accoutn.
just add depth to the input?
   add it as a 0th array value, as a value before the childnumbers start?
  */
  //  printf("HASH: %0x\n", h);
  return h % HASH_TABLE_SIZE;;
}

node_type *lookup(int path[], int depth) {
  if (depth < 0 || depth > TREE_DEPTH) {
    printf("ERROR. Depth out of bounds %d    ", depth); print_path(path, depth);
    exit(0);
  }
  printf("\nLOOKUP(%d)   ", depth); print_path(path, depth);
  int entry = hash(path, depth);
  if (hash_table[entry].key == entry) {
    node_type *node = hash_table[entry].node;
    printf("Matching hashkey; Returning Node 0x%x key: 0x%x  depth: %d. node-depth: %d     ", 
	   node, entry, depth, node->depth); print_path(path, depth);
    check_paths(path, node->path, depth);
    return node;
  } else {
    printf("NULL: entry: 0x%x  key: 0x%x\n", entry, hash_table[entry].key);
    return NULL;
  }
}

void store(node_type *node) {
  int entry = hash(node->path, node->depth);
  hash_table[entry].key  = entry;
  hash_table[entry].node = node;
}


/************************
 *** NODE             ***
 ************************/

int *null_path() {
  return calloc(TREE_DEPTH+1, sizeof(int)); // zero fill
}

/*
 * allocate in memory space of home machine
 */

#define MAXMININIT

node_type *new_leaf(child_msg_type *msg, int ch, int mm) {
  if (msg && msg->depth < 0) {
    return NULL;
  }
  node_type *node = malloc(sizeof(node_type));
  node->board = my_process_id; //rand() % N_MACHINES;  // my_machine
  node->maxormin = mm;
  node->wa = -INFTY;
  node->wb = INFTY;
  if (msg) {
    node->wa = msg->wa;
    node->wb = msg->wb;
    node->my_child_number = msg->child_number;
    node->parent_at = msg->parent_at;
    node->path = msg->path; // hmm, ergens mot toch de alloc van dit array plaatsvinden. ofwel in de msg crete ofwel in de new-leaf. ik zou zeggen in new-leaf. Maar msgs hebben allemaal een mooie nette malloc, dus dat maar aanhouden?
  } else {
    node->my_child_number = 0;
    node->parent_at = 0;
    node->path = null_path(); // or an array filled with zero's
    node->depth = 999999;
  }
  node->a = -INFTY;
  node->b = INFTY;
  node->lb = -INFTY;
  node->ub = INFTY;
  node->from_child = -1; // I was scheduled by a child, whose parent I am, and that child was number xx, this is used in UPDATE, to record which children are open
  node->my_child_number = ch; // child number of my parent that I am
  //  node->children =  NULL;
  node->n_children = 0;
  node->n_active_kids = 0;
  node->n_open_kids = TREE_WIDTH;
  for (int i= 0; i < TREE_WIDTH; i++) {
    node->open_child[i] = TRUE;
    node->live_children[i] = TRUE;
  }
  node->max_of_closed_kids_ub = -INFTY;
  node->min_of_closed_kids_lb = INFTY;
  //  node->parent_path = p->path;
  //  node->best_child = NULL;
  //  node->path = 10 * parent->path + ch + 1;;
  if (msg) {
    node->depth = msg->depth;// - 1;
  }
  //pthread_mutex_init(&node->nodelock, NULL);  
  return node; // return to where? return a pointer to our address space to a different machine's address space?
}

int max(int a, int b) {
  return a>b?a:b;
}
int min(int a, int b) {
  return a<b?a:b;
}

/*
int max_of_beta_kids(node_type *node) {
  int b = -INFTY;
  int ch = 0;
  for (ch = 0; ch < node->n_children && node->children[ch]; ch++) {
    node_type *child = node->children[ch];
    if (child) {
      b = max(b, child->b);
    }
  }
#ifdef AB_OPEN_INFTY
  if (ch < node->n_children) {
    b = -INFTY;
  }
#endif
  // if there are unexpanded kids in a max node, then beta is infty
  // the upper bound of a max node with open children is infinity, it can
  // still be any value
  return (b == -INFTY)?INFTY:b;
}

int min_of_alpha_kids(node_type *node) {
  int a = INFTY;
  int ch = 0;
  for (ch = 0; ch < node->n_children && node->children[ch]; ch++) {
    node_type *child = node->children[ch];
    if (child) {
      a = min(a, child->a);
    } 
  }
#ifdef AB_OPEN_INFTY
  if (ch < node->n_children) {
    a = INFTY;
  }
#endif
  // if there are unexpanded kids in a min node, then alpha is -infty
  return (a == INFTY)?-INFTY:a;
}

int max_of_ub_kids(node_type *node) {
  int ub = -INFTY;
  int ch = 0;
  for (ch = 0; ch < node->n_children && node->children[ch]; ch++) {
    node_type *child = node->children[ch];
    if (child) {
      ub = max(ub, child->ub);
    }
  }
#ifdef UL_OPEN_INFTY
  if (ch < node->n_children) {
    ub = -INFTY;
  }
#endif
  // if there are unexpanded kids in a max node, then beta is infty
  // the upper bound of a max node with open children is infinity, it can
  // still be any value
  return (ub == -INFTY)?INFTY:ub;
}

int min_of_lb_kids(node_type *node) {
  int lb = INFTY;
  int ch = 0;
  for (ch = 0; ch < node->n_children && node->children[ch]; ch++) {
    node_type *child = node->children[ch];
    if (child) {
      lb = min(lb, child->lb);
    } 
  }
#ifdef UL_OPEN_INFTY
  if (ch < node->n_children) {
    lb = INFTY;
  }
#endif
  // if there are unexpanded kids in a min node, then alpha is -infty
  return (lb == INFTY)?-INFTY:lb;
}

*/

#ifndef MSGPASS
void set_best_child(node_type *node) {
  if (node && node->parent) {
    if (node->parent->best_child) {
      //      printf("SET BEST CHILD from %d ", node->parent->best_child->path);    
    } else {
      //      printf("SET BEST CHILD from -- ");    
    }
    if (node->parent->maxormin == MAXNODE && 
	(!node->parent->best_child || 
	 node->a > node->parent->best_child->a)) {
      /*if my value is better than your current best child then update best child
	update beste child shpuld be coniditiaonal only if it is better. 
	this updates it to the last best child. In parallel timing may be off, and this may be wrong
      */
      node->parent->best_child = node;	  
    }
    if (node->parent->maxormin == MINNODE && 
	(!node->parent->best_child || 
	 node->b < node->parent->best_child->b)) {
      node->parent->best_child = node;	  
    }
    if (node->parent->best_child) {
      //      printf("to %d\n", node->parent->best_child->path);
    } else {
      //      printf("to --\n");
    }
  }
}
#endif

/*
int child_number(int p[]) {
  return p - (10*(p/10));
}
*/

int child_number(node_type *node) {
  if (node) {
    return node->my_child_number;
  }
}

int where() {
  return rand() % world_size;  // my_machine
}

void print_unorderedness() {
  for (int i = 0; i < TREE_DEPTH; i++) {
    if (global_unorderedness_seq_n[i]) {
      printf("seq u.o. (%d): %3.2lf\n", i, 
	     global_unorderedness_seq_x[i]/global_unorderedness_seq_n[i]);
    } else {
      printf("seq u.o. (%d) zero\n", i);
    }
  }
}

int opposite(node_type *node) {
  return (node->maxormin==MAXNODE) ? MINNODE : MAXNODE;
}

/*
void print_tree(node_type *node, int d) {
  if (node && d >= 0) {
    printf("%d: %d %s <%d,%d>\n",
	   node->depth, node->path, ((node->maxormin==MAXNODE)?"+":"-"), node->a, node->b);
    for (int ch = 0; ch < node->n_children; ch++) {
      print_tree(node->children[ch], d-1);
    }
  }
}
*/

int my_process_id = -1;
int world_size = 0;


/***************************
 *** MAIN                 **
 ***************************/

int main(int argc, char *argv[]) { 
  int g = -INFTY;
  if (argc != 3) {
    printf("Usage: %s {w,n,m} n-par\n", argv[0]);
    exit(1);
  }
  if (my_process_id == 0) {
    printf("\n\n\n\n\n");
  }

  char *alg_choice = argv[1];
  n_par = atoi(argv[2]);
  if (n_par > TREE_WIDTH) {
    printf("It does not make sense to ask to schedule %d children at once in the job queue when nodes have only %d children to begin with\n", n_par, TREE_WIDTH);
    exit(0);
  }
  if (my_process_id == 0) {
    printf("Hello from ParAB with %d machine%s and %d children in par in queue. (w,d)=(%d,%d)\n", world_size, world_size==1?"":"s", n_par, TREE_WIDTH, TREE_DEPTH);
  }


  for (int i = 0; i < RANDOM_TABLE_SIZE; i++) {
    random_table[i] = rand();
  }

  for (int i = 0; i < HASH_TABLE_SIZE; i++) {
    hash_table[i].key = INVALID;
    hash_table[i].node = INVALID;
  }

#ifndef MSGPASS

  for (int i = 0; i < world_size; i++) {
    global_selects[i] = 0;
    global_leaf_eval[i] = 0;
    global_updates[i] = 0;
    global_downward_aborts[i] = 0;
#ifdef GLOBAL_QUEUE
#else
      local_top[i] = 0;
#endif
    for (int j = 1; j < JOB_TYPES; j++) {
#ifdef GLOBAL_QUEUE
      top[i][j] = 0;
      max_q_length[i][j] = 0;
#else
      buffer_top[i][j] = 0;
      max_q_length[i] = 0;
#endif
    }
    //   pthread_mutex_init(&jobmutex[i], NULL);
    //    pthread_cond_init(&job_available[i], NULL);
    global_no_jobs[i] = 0;
    //    jobmutex[i] = PTHREAD_MUTEX_INITIALIZER;
  }
#endif
  total_jobs = 0;

  for (int i = 0; i < TREE_DEPTH; i++) {
    global_unorderedness_seq_x[i] = TREE_WIDTH/2;
    global_unorderedness_seq_n[i] = 1;
  }

#ifdef MSGPASS
  MPI_Init(NULL, NULL);
  MPI_Comm_rank(MPI_COMM_WORLD, &my_process_id);
  MPI_Comm_size(MPI_COMM_WORLD, &world_size);
  global_empty_machines = world_size;


  // attach gdb
  /*  
  {  
  int i = 0;
    char hostname[256];
    gethostname(hostname, sizeof(hostname));
    printf("PID %d on %s ready for attach\n", getpid(), hostname);
    fflush(stdout);
    while (0 == i){
      sleep(5);
    }
  }
  */
  
#else
  int numWorkers = __cilkrts_get_nworkers();
  printf("CILK has %d worker threads\n", numWorkers);
#endif

  /* 
   * process algorithms
   */
  if (my_process_id == 0) {
    if (!root) {
      create_tree(TREE_DEPTH);// aha! each alphabeta always creates new tree!
    }
    
    if (strcmp(alg_choice, "w") == 0) {
      printf("Wide-window Alphabeta\n");
      g = start_alphabeta(-INFTY, INFTY);
    } else if (strcmp(alg_choice, "n") == 0) {
      printf("Null-window Alphabeta\n");
      int b = 77;
      g = start_alphabeta(b-1, b);
      if (g < b) { printf("NWS fail low (ub)\n"); } else { printf("NWS fail high (lb)\n"); }
    } else if (strcmp(alg_choice, "m") == 0) {
      printf("MTD(f)\n");
      g = start_mtdf();
    } else {
      printf("ERROR: invalid algorithm choice %s\n", alg_choice);
    }

    printf("Done. value: %d\n", g);
  }
#ifndef MSGPASS
  //  print_tree(root, min(3, TREE_DEPTH));
  print_q_stats();
  printf("SumSelects: %d\n", sum_global_selects);
  printf("SumLeaf Evals: %d\n", sum_global_leaf_eval);
  printf("SumUpdates: %d\n", sum_global_updates);
  printf("SumDownward parallel aborted searches: %d\n", sum_global_downward_aborts);
#else
  MPI_Finalize();
#endif
  //  print_unorderedness();
  /*
  for (int i = 0; i < N_MACHINES; i++) {
    printf("M%d: no jobs: %d\n", i, global_no_jobs[i]);
  }
  */
  return 0;
}
  
void create_tree(int d) {
  root = new_leaf(NULL, 0, MAXNODE);
  root->depth = d;
  //  mk_children(root, d-1);
}
/*  
void print_q_stats() {
  for (int i=0; i < N_MACHINES; i++) {
    sum_global_selects += global_selects[i];
    sum_global_leaf_eval += global_leaf_eval[i];
    sum_global_updates += global_updates[i];
    sum_global_downward_aborts += global_downward_aborts[i];
    //    printf("[%d,%d,%d,%d]\n", global_selects[i], global_leaf_eval[i], global_updates[i], global_downward_aborts[i]);
    //  for (int j=1; j < JOB_TYPES; j++) {
    //      printf("Max Q length %d [%d]\n", max_q_length[i], i);
      //    }
  }
}
*/
/*
void print_queues() {
  printf("********* %d ********\n", total_jobs);
  for (int i=0; i < N_MACHINES; i++) {
    for (int j=1; j < JOB_TYPES; j++) {
      printf("\ntop[%d][%d]: %d      ", i,j, top[i][j]);
      for (int k=1; queue[i][k][j] && queue[i][k][j]->node && k <= top[i][j] ; k++) {
	printf("%d [%d] <%d,%d>, ", 
	       queue[i][k][j]->node->path, 
	       queue[i][k][j]->type_of_job,
	       queue[i][k][j]->node->a, 
	       queue[i][k][j]->node->b);
      }
    }
    printf("\n");
  }
  printf("%%%%%%%%%%%%%%%%\n", total_jobs);
}
*/

// simple, new leaf is initialized with a wide window
int start_alphabeta(int a, int b) {
  root->wa = a; // store bounds in passed-down window alpha/beta
  root->wb = b;
#ifndef MSGPASS
  schedule(root->board, root, SELECT);
  flush_buffer(root->board, root->board);
#else
  printf("CALLING CREATE_CHILD_AT FROM START_ALPHABETA\n");;
  create_child_at(root, 0, MAXNODE);
#endif
  start_processes(N_MACHINES);
  return root->ub >= b ? root->lb : root->ub;
  // dit moet een return value zijn buiten het window. fail soft ab
}

int start_mtdf() {
  int lb = -INFTY-1;
  int ub = INFTY+1;
  int g = 0;
  int b = INFTY;

  do {
    if (g == lb) { b = g+1; } else { b = g; }
    //    printf("enter MTD(%d,%d) lbub<%d:%d> lbub<%d:%d>  ab<%d:%d> wab<%d:%d>  ", 
    //	   b-1, b, lb, ub, root->lb, root->ub, root->a, root->b, root->wa, root->wb);
    g = start_alphabeta(b-1, b);
    if (g < b)   { ub = g;  } else { lb = g; }
    //    printf("--> exit MTD(%d) lbub<%d:%d>\n", g, lb, ub);
  } while (lb < ub);

  printf("finished MTD(%d): lb/ub:<%d:%d> node-lbub:<%d:%d> node-ab:<%d,%d>\n", g, lb, ub, root->lb, root->ub, root->a, root->b);

  return g;
}





/***************************
 *** OPEN/LIVE           ***
 ***************************/

int leaf_node(node_type *node) {
  return node && node->depth <= 0;
}
int live_node(node_type *node) {
  //  return node && node->lb < node->ub; // alpha beta???? window is live. may be open or closed
  return node && max(node->wa, node->a) < min(node->wb, node->b); // alpha beta???? window is live. may be open or closed
}

int live_node_lbub(node_type *node) {
  return node && max(node->a, node->wa) < min(node->b, node->wb); 
// alpha beta???? window is live. may be open or closed
  //  printf("ERROR: using lb/ub\n");

  //misschien  moet lb ub met ab en wa wb vergeleken worden
  //  return node && node->lb < node->ub; // alpha beta???? window is live. may be open or closed
}
int dead_node(node_type *node) {  
  return node && !live_node(node);    // ab cutoff. is closed
}
int dead_node_lbub(node_type *node) {  
  //  printf("ERROR: using lb/ub\n");
  return node && !live_node_lbub(node);    // ab cutoff. is closed
}
/*
void compute_bounds(node_type *node) {
  if (node && node->parent) {
    int old_a = node->a;
    int old_b = node->b;
    node->a = max(node->a, node->parent->a);
    node->b = min(node->b, node->parent->b);
    //    printf("%d COMPUTEBOUNDS <%d:%d> -> <%d:%d>\n", 
    //	   node->path, old_a, old_b, node->a, node->b);
  }
}
*/
int root_node(node_type *node) {
  return node == root;
}
