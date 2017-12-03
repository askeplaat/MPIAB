#include <stdio.h>         // for print
#include <stdlib.h>        // for rand
//#include <cilk/cilk.h>     // for spawn and sync
//#include <cilk/cilk_api.h> // for cilk workers report
#include <mpi.h>
#include <assert.h>        // for print
//#include <pthread.h>       // for mutex locks
#include <string.h>        // for strcmp in main()
#include "parabm.h"         // for prototypes and data structures


int global_empty_machines = N_MACHINES;

/*******************************
 **** JOB Q                  ***
 ******************************/

// throttling parallelism
// how are the job queues doing? if empty, then allow parallelism, 
// if getting full, then turn sequential
// other policies are possible: left first, deepest first (now it is
// in effect a breadth first policy. not very efficient)
// par should be near leaves. not near root
int seq(node_type *node) {
  return FALSE;
#ifdef PUO
  return puo(node);
#endif
  // this assumes jobs are distributed evenly over queues
  int machine = node->board;
  int depth = node->depth;  // if too far away from leaves then seq
  // depth == 0 at leaves
  // low SEQ_DEPTH number means it is easy for nodes to have a depth that exceeds thresholds and therefore return TRUE for remainging sequential
  // big SEQ_DEPTH number means little parallelism
#ifdef GLOBAL_QUEUE
  return top[machine][SELECT] * 4 > N_JOBS || depth > SEQ_DEPTH;
#else
  //  return local_top[machine] * 4 > N_JOBS || depth > SEQ_DEPTH;
#endif
}


double suo(int d) {
  if (global_unorderedness_seq_n[d]) {
    return global_unorderedness_seq_x[d]/global_unorderedness_seq_n[d];
  } else {
    return FALSE;
  }
}

// true if current child number is larger than sequential unorderedness.
// this prevents parallelism beyond where the sequential algorithm would venture
int puo(node_type *node) {
  return child_number(node) > suo(node->depth);
}

#ifndef MSGPASS
int empty_buffers(int home) {
  for (int i = 0; i < N_MACHINES; i++) {
    if (buffer_top[i][home] > 0) {
      return FALSE;
    }
  }
  return TRUE;
}
#endif

/*
int not_empty(int top, int home) {
  lock(&jobmutex[home]);  // empty
  int t = top > 0;
  unlock(&jobmutex[home]); // // empty
  return t;
}



int empty(int top, int home) {
  return !not_empty(top, home);
}
*/

/*
void copy_job_to_node(job_type *job, node_type *node) {
  node->job_type_of_job = job->type_of_job;
  node->job_from_machine = job->from;
  node->job_lb = job->lb;
  node->job_ub = job->ub;
}

void copy_node_to_job(job_type *job, node_type *node) {
  job->node = node;
  job->type_of_job = node->job_type_of_job;
  job->from = node->job_from_machine;
  job->lb = node->job_lb;
  job->ub = node->job_ub;
}
*/
#ifndef MSGPASS
int empty_jobs(int home) {
#ifdef GLOBAL_QUEUE
  int x = top[home][SELECT] < 1 && 
    top[home][UPDATE] < 1 &&
    top[home][BOUND_DOWN] < 1 &&
    top[home][PLAYOUT] < 1;
#else
  int x = local_top[home] < 1 && 
    local_top[home] < 1 &&
    local_top[home] < 1 &&
    local_top[home] < 1 && empty_buffers(home);
#endif
  return x;
}
#endif
// this should be atomic over all job queues. needs global lock, over all queues
/*
int no_more_jobs_in_system(int home) {
  lock(&global_queues_mutex);
  if (!empty_jobs(home)) {
    unlock(&global_queues_mutex);
      return FALSE;
  } else {
    for (int i = 0; i < N_MACHINES; i++) {
      if (!empty_jobs(i)) {
	unlock(&global_queues_mutex);
	return FALSE;
      }
    }
  }
  unlock(&global_queues_mutex);
  return TRUE;
}
*/

/*
int not_empty_and_live_root() {
  int home = root->board;
  lock(&jobmutex[home]);  // empty
  int e =  total_jobs > 0 && live_node(root);
  unlock(&jobmutex[home]);  // empty
  return e;
}

int all_empty_and_live_root() {
  int home = root->board;
  lock(&jobmutex[home]);  // empty
  int e =  total_jobs <= 0 && live_node(root);
  (&jobmutex[home]);  // empty
  return e;
}
*/

void start_processes(int n_proc) {
  int i;
  //  schedule(root, SELECT, -INFTY, INFTY);
  //  for (i = 0; i<n_proc; i++) {

  global_done = FALSE;
#ifdef MSGPASS
  MPI_Comm_rank(MPI_COMM_WORLD, &my_process_id);
  do_work_queue(my_process_id);
#else
  for (i = 0; i<N_MACHINES; i++) {
    cilk_spawn do_work_queue(i);
  }
  //  printf("M:%d. Before Cilk sync\n", i);
  cilk_sync;
  //  printf("Root is solved. jobs: %d. root: <%d:%d> [%d:%d]\n", total_jobs, root->a, root->b, root->lb, root->ub);
#endif
}

void do_idle_machine(int from) {
  if (from < 0 || from >= world_size) {
    printf("ERROR: bad machine number in do idle machine: %d\n", from);
  }
  printf("IDLE %d\n", from);
  int old = global_no_jobs[from] == FALSE;
  int new = global_no_jobs[from] = TRUE;
  if (old == FALSE && new == TRUE) {
    global_empty_machines ++;
    if (global_empty_machines >= world_size) {
      printf("ERROR: global_empty_machines too large: %d\n", global_empty_machines);
    }
  }
}

void do_nonidle_machine(int from) {
  if (from < 0 || from >= world_size) {
    printf("ERROR: bad machine number in do nonidle machine: %d\n", from);
  }
  //  printf("NONIDLE %d\n", from);
  int old = global_no_jobs[from] == TRUE;
  int new = global_no_jobs[from] = FALSE;
  if (old == TRUE && new == FALSE) {
    global_empty_machines --;
    if (global_empty_machines < 0) {
      printf("ERROR: global_empty_machines too small: %d\n", global_empty_machines);
    }
  }
}
    
void print_path(int path[], int depth) {
  //  for (int i = depth; i >= 0; i--) {
  for (int i = TREE_DEPTH-1; i >= depth; i--) {
    printf(" [%d]%d", i, path[i]);
  } 
  printf("\n");
}

void msg_dispatch(child_msg_type *msg, int from) {
      // dispatch
  //  printf("DISPATCHING\n");
      if (msg->msg_type == CREATE_CHILD_AT) {
	//	printf("%d Expanding ", my_process_id); print_path(msg->path, msg->depth);
	do_expand((child_msg_type *)msg);
      } else if (msg->msg_type == SELECT_CHILD_AT) {
	printf("%d Selecting ", my_process_id); print_path(msg->path, msg->depth); // path is alwyas [0,0,0,0] so lways the samen node is selected. the leftfirst leaf....
	if (msg->depth <0 || msg->depth > TREE_DEPTH) {
	  printf("ERROR in Dispatch depth %d\n", msg->depth);
	}
	node_type *node = lookup(msg->path, msg->depth);
	if (node) {
	  do_select(node);
	} else {
	  printf("LOOKUP returned NULL\n");
	}
      } else if (msg->msg_type == UPDATE_PARENT_AT) {
	printf("%d Update parent ", my_process_id); print_path(msg->path, msg->depth);
        do_update((parent_msg_type *)msg);
      } else if (msg->msg_type == UPDATE_CHILD_AT) {
	printf("%d Update child ", my_process_id); print_path(msg->path, msg->depth);
        downward_update_children((update_msg_type *)msg);
      } else if (msg->msg_type == IDLE_MSG) {
	do_idle_machine(from);
      } else if (msg->msg_type == NONIDLE_MSG) {
	do_nonidle_machine(from);
      } else if (msg->msg_type == DECR_ACTIVE_ROOT) {
	printf("Decrementing Active count in root\n", root->n_active_kids);
	root->n_active_kids--;
      } else {
	printf("ERROR: Wrong MPI message type\n");
        exit(0);
      }
}



void do_work_queue(int i) {
  //  int workerNum = __cilkrts_get_worker_number();
  int safety_counter = SAFETY_COUNTER_INIT;
  global_empty_machines = world_size;

  while (safety_counter-- > 0 && !global_done && live_node(root)) { 
    job_type *job = NULL;

    //    printf("%d active kids: %d\n", my_process_id, root->n_active_kids);

    if (i == root->board && live_node(root) && root->n_active_kids == 0) {
      //     printf("* globalempty machines: %d\n", global_empty_machines);

      printf("%d CALLING SELECT_CHILD_AT FROM EMPTY WORKQUEUE at root. n_active: %d\n", my_process_id, root->n_active_kids);
      select_child_at(root, 0, MAXNODE);
    }

    if (my_process_id == i) {
      child_msg_type idle_msg, nonidle_msg;
      idle_msg.msg_type = IDLE_MSG;
      nonidle_msg.msg_type = NONIDLE_MSG;
      MPI_Status status;
      child_msg_type *msg = malloc(max(sizeof(child_msg_type), sizeof(parent_msg_type)));
      for (int i=0; i<TREE_DEPTH; i++) {
         ((child_msg_type*)msg)->path[i] = 0;
      }

      if (i != root->board) {
	MPI_Send(&idle_msg, sizeof(idle_msg), MPI_BYTE, 0, 0, MPI_COMM_WORLD);
      }
      //      printf("RECEIVING\n");
      MPI_Recv(msg, sizeof(child_msg_type), MPI_BYTE, MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);
      //      if (i != root->board) {
      if (msg->msg_type != IDLE_MSG && msg->msg_type != NONIDLE_MSG) {
	if (i != root->board) {
	  MPI_Send(&nonidle_msg,  sizeof(idle_msg), MPI_BYTE, 0, 0, MPI_COMM_WORLD);
	} else {
	  do_nonidle_machine(i);
	}
      } 

      msg_dispatch(msg, status.MPI_SOURCE);
      //      printf("QQQ\n");
    }

    //    printf("globalempty machines: %d\n", global_empty_machines);
    //    if (i == root->board && global_empty_machines >= world_size) {
    // if root still is live, and thus has live kids, and none is being searched, can that happen????
    /*
    in roll-out, can there be live kids that are not searched?
      it can in mcts. it happens all the time, after every child. at the end of a search/playout
      it can in ab, it is the root next mvoe sequence. does orll out ab have a next move loop?
no it has not. it goes all the way up to the root once it has expanded a singel leaf. it does not 
automatically continue to the next brother.
does it really not?????
how dreadfully inefficient!
      so, in rollout, we need to restart a roll out form the root as long as the root is live and
there are no active searhces. 
There can be no active searches wile the root is live. so liveness is no indicator for activity. 
      we need to track activity separately, 

      updated, but live, and then? automatic 
    */
    
    global_done |= !live_node(root);
    if (global_done) {
      break;
    }
  } // while 

  if (safety_counter <= 0) {
    printf("M:%d ERROR: safety triggered\n", i);
  } else {
    //    printf("M:%d safety counter: %d. root is at machine: %d\n", i, safety_counter, root->board);
  }

  //  global_done = 1;

  //  printf("M:%d. Finished. Queue is empty or root is solved. jobs: %d. root: <%d:%d> \n", i, total_jobs, root->a, root->b);

}


void create_child_at(node_type *node, int child_number, int mm) {
  int i = 0;
  child_msg_type child_msg;
  child_msg.msg_type = CREATE_CHILD_AT;
  int home_machine = where(node);
  node->n_active_kids++;
  /*
klopt dit zo?
elke select down doet een incr
zijn er genoeg updates om hem naar 0 te krijgen?
  */
  node->children_at[child_number] = home_machine;
  //  child_msg.depth = node->depth;
  child_msg.child_number = child_number;
  child_msg.mm = mm;
  for (i=0; i < TREE_DEPTH; i++) { 
    child_msg.path[i] = node->path[i]; 
  }
  child_msg.path[node->depth-1] = child_number;

  //  here child_nuimber should be given to node->dpeth-1 (or +1)?

  child_msg.wa = node->wa;
  child_msg.wb = node->wb;
  child_msg.depth = node->depth - 1;
  child_msg.parent_at = my_process_id;

  //  do_nonidle_machine(home_machine);

  //  printf("%d create sending CHILD %d to %d\n", my_process_id, child_number, home_machine);

  // the idea to only send the pointer, not the full node.
  MPI_Send(&child_msg, sizeof(child_msg_type), MPI_BYTE, home_machine, 0, MPI_COMM_WORLD);
  // receiver must store parent-at, and copy wa wb
}

void select_child_at(node_type *node, int child_number, int mm) {
  int i = 0;
  child_msg_type child_msg;
  child_msg.msg_type = SELECT_CHILD_AT;
  int home_machine = where(node);
  node->n_active_kids++;

  node->children_at[child_number] = home_machine;
  child_msg.child_number = child_number;
  //  child_msg.depth = node->depth;
  child_msg.mm = mm;
  for (i=0; i < TREE_DEPTH; i++) {
    child_msg.path[i] = node->path[i]; 
  }
  child_msg.path[node->depth] = child_number;

  child_msg.wa = node->wa;
  child_msg.wb = node->wb;
  child_msg.depth = node->depth - 1;
  child_msg.parent_at = my_process_id;

  //  do_nonidle_machine(home_machine);

  //  printf("%d select sending CHILD %d to %d\n", my_process_id, child_number, home_machine);

  // the idea to only send the pointer, not the full node.
  MPI_Send(&child_msg, sizeof(child_msg_type), MPI_BYTE, home_machine, 0, MPI_COMM_WORLD);
  // receiver must store parent-at, and copy wa wb
}

void decr_active_root() {
  update_msg_type m;
  m.msg_type = DECR_ACTIVE_ROOT;
  MPI_Send(&m, sizeof(update_msg_type), MPI_BYTE, 0, 0, MPI_COMM_WORLD);
}

void update_parent_at(node_type *node, int from_child_id) {
  parent_msg_type parent_msg;
  parent_msg.msg_type = UPDATE_PARENT_AT;
  int home_machine = node->parent_at;
  //but this is not passed to msg
  //should be parent's active kid number that is decremented.' 
  /*
klopt dit? zo gauw er een update at een node is is die node inactive?
kan er ook nooit meer iets gebeuren dat hij weer active wordt?
alle kinderen inactief?
  of kan re nog een update komen, van een parallel kind?
ja tohc?

what we really want is to know if there is a possibility for an update that might come.
only if there really cannot anymore come an update then a new root select should be scheduled

so we should track the number of outstanding jobs

or we can just flag when all machines are idle *and* no messages are in transit.
How do we know they are idle? if they are blocked in a receive? so we should keep track of the machines that are in receive

					of tellertje bijhouden in elke node. increment voor elk kind dat select of expand down gaat, en decrement voor elke update die omhoog voorbij komt.
als de root dan op nul staat is er geen uitstaand werk meer en is de machine idle.
kan niet anders. toch?
					Roll out kent maar twee dingen: naar beneden en naar boven gaan. ook paralllel roll out, die gaat naar beneden en naar boven. vandaar het tellertje.

en het tellertje telt hoeveel actieve kinderen er zijn. dus elke expand van een kind en elke select van een kind zorgt voor een ophoging. 
en elke update van een kind naar een parent verlaaggt de teller van die parent
dus een 2-par expand hoogt de teller met 2 op.

  */

  parent_msg.from_child = from_child_id;
  parent_msg.depth = node->depth +1;
  parent_msg.child_live = node->lb < node->ub;
  parent_msg.lb = node->lb;
  parent_msg.ub = node->ub;
  parent_msg.mm = opposite(node);
  for (int i=0; i < TREE_DEPTH; i++) {
    parent_msg.path[i] = node->path[i];
  }
  //  printf("%d SEND update to parent\n", my_process_id);
  MPI_Send(&parent_msg, sizeof(parent_msg_type), MPI_BYTE, home_machine, 0, MPI_COMM_WORLD);
}

void update_child_at(node_type *node, int child_number) {
  int i = 0;
  update_msg_type update_msg;
  update_msg.msg_type = UPDATE_CHILD_AT;
  int home_machine = node->children_at[child_number];
  //  update_msg->from_child = from_child_id;
  update_msg.depth = node->depth - 1; // -1 since child: closer to leaves which are dpeth==0
  update_msg.a = node->a;
  update_msg.b = node->b;
  for (i=0; i < TREE_DEPTH; i++) {
    update_msg.path[i] = node->path[i];
  }
  update_msg.path[node->depth] = child_number;

  //  printf("%d SEND update to child\n", my_process_id);
  MPI_Send(&update_msg, sizeof(update_msg_type), MPI_BYTE, home_machine, 0, MPI_COMM_WORLD);

}




void print_queue(job_type *q[], int t){
  for (int i = 0; i <= t; i++) {
    if (q[i] && q[i]->node) {
      printf("Q[%d]: T:[%d] P:%d <%d:%d> depth:%d\n", 
	     i, q[i]->type_of_job, q[i]->node->path, 
	     q[i]->node->a, q[i]->node->b, q[i]->node->depth);
    }
  }
}
