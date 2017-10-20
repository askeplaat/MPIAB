#include <stdio.h>         // for print
#include <stdlib.h>        // for rand
#include <cilk/cilk.h>     // for spawn and sync
#include <cilk/cilk_api.h> // for cilk workers report
#include <assert.h>        // for print
#include <pthread.h>       // for mutex locks
#include <string.h>        // for strcmp in main()
#include "parab.h"         // for prototypes and data structures


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
  return local_top[machine] * 4 > N_JOBS || depth > SEQ_DEPTH;
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
  return child_number(node->path) > suo(node->depth);
}


int empty_buffers(int home) {
  for (int i = 0; i < N_MACHINES; i++) {
    if (buffer_top[i][home] > 0) {
      return FALSE;
    }
  }
  return TRUE;
}

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

  for (i = 0; i<N_MACHINES; i++) {
    cilk_spawn do_work_queue(i);
  }
  //  printf("M:%d. Before Cilk sync\n", i);
  cilk_sync;
  //  printf("Root is solved. jobs: %d. root: <%d:%d> [%d:%d]\n", total_jobs, root->a, root->b, root->lb, root->ub);
}

void do_work_queue(int i) {
  int workerNum = __cilkrts_get_worker_number();
  //  printf("My CILK worker number is %d. global_empty: %d\n", workerNum, global_empty_machines);
  //  printf("M:%d. global_empty: %d, total_jobs: %d\n", i, global_empty_machines, total_jobs);

  //  lock(&treemutex);

  //  printf("M:%d. global_empty: %d, total_jobs: %d\n", i, global_empty_machines, total_jobs);

  //  unlock(&treemutex);

  //    printf("enter workqueue lbub<%d:%d>  ab<%d:%d> wab<%d:%d> LIVENODE: %d global_done: %d ", 
  //	   root->lb, root->ub, root->a, root->b, root->wa, root->wb, live_node(root), global_done);


  int safety_counter = SAFETY_COUNTER_INIT;

  while (safety_counter-- > 0 && !global_done && live_node(root)) { 
    job_type *job = NULL;

    //    printf("globalempty machines: %d\n", global_empty_machines);
    if (i == root->board && global_empty_machines >= N_MACHINES) {
      //     printf("* globalempty machines: %d\n", global_empty_machines);
      add_to_queue(i, new_job(root, SELECT)); 
    }

    job = pull_job(i);
     
    if (job) {
      //     lock_node(job->node);
      //        lock(&treemutex);
      process_job(i, job);
      //          unlock(&treemutex); 
      //      unlock_node(job->node);
    } else {
      // pull returned null, queue must be empty. ask a random machine to flush their buffer
      int steal_target = rand() % N_MACHINES;
      //      printf("STEAL target: %d\n", steal_target);
      flush_buffer(steal_target, i);
    } 
    /*
    lock(&global_jobmutex); // check
    check_consistency_empty();
    unlock(&global_jobmutex); // check
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



// schedule node at the remote job_queue that is its home machine
// with job action t, and optional argument a
// pass the continuation
void schedule(int my_id, node_type *node, int type, int from, int lb, int ub) {
  if (node) {
    // send to remote machine
    int home_machine = node->board;

    int was_empty = empty_jobs(home_machine);  
    add_to_queue(my_id, new_job(node, type, from, lb, ub));
    /*
    if (was_empty) {
      //      printf("Signalling machine %d for path %d type:[%d]. in wait: %d\n", home_machine, node->path, t, global_in_wait);
      pthread_cond_signal(&job_available[home_machine]);
      //   pthread_cond_signal(&global_job_available[home_machine]); //how do we know that we signal the correct machine?
    }
    */
  } 
}

// which q? one per processor
void add_to_queue(int my_id, job_type *job) {
  int home_machine = job->node->board;
  int jobt = job->type_of_job;
  if (home_machine >= N_MACHINES) {
    printf("ERROR: home_machine %d too big\n", home_machine);
    exit(1);
  }
  /*
  if (top[home_machine][jobt] >= N_JOBS) {
    printf("M%d Top:%d ERROR: queue [%d] full\n", home_machine, top[home_machine][jobt], jobt);
    exit(1);
  }
  */
  push_job(my_id, home_machine, job);
}


/* 
** PUSH JOB
*/

// home_machine is the machine at which the node should be stored
void push_job(int my_id, int home_machine, job_type *job) {
  //  printf("M:%d PUSH   ", home_machine);
#ifdef GLOBAL_QUEUE
#ifdef LOCAL_LOCK
  lock(&jobmutex[home_machine]); // push
#else
  lock(&global_jobmutex); // push
#endif
#endif
  total_jobs++;
  if (empty_jobs(home_machine) && global_empty_machines > 0) {
    // I was empty, not anymore
    //    printf("M:%d is empty. decr global-empty %d\n", home_machine, global_empty_machines);
    global_empty_machines--;
    /*
hmm. this is invoked when local_top is empty. but push now only works with buffer. so localtop sometimes says empty when there are nonempty buffers.  causing too many selects to be scheduled.
so empty should first do a global flush before more jobs are scheduled. or take the remote buffer counts into account
dit zou dus voor een te lage waarde van globalemptymachines moeten leiden. of andersom?????
emptyjobs zegt te vaak ja. 
dus globalemptymachines is te laag
			   dus te weinig selects gescheduled. dus soms deadlock, omdat er jobs in buffers zitten
    */
  }
  int jobt = job->type_of_job;
  //int jobt = SELECT;
#ifdef MSGPASS
  MPI_Send(data, count, type, dest, tag, comm);
#else
#ifdef GLOBAL_QUEUE
  queue[home_machine][++(top[home_machine][jobt])][jobt] = job;
  max_q_length[home_machine][jobt] = 
    max(max_q_length[home_machine][jobt], top[home_machine][jobt]);
#else
  // check if local buffer is full, then need to flush to main work queue, which is remote on another machine
  if (buffer_top[my_id][home_machine] >= BUFFER_SIZE-1) {
    flush_buffer(my_id, home_machine);
  }
  // fill local buffer with one new job
  local_buffer[my_id][home_machine][++buffer_top[my_id][home_machine]] = job;
#endif
#endif
  
#ifdef LOCAL_LOCK
  unlock(&jobmutex[home_machine]); // push
#else
  unlock(&global_jobmutex); // push
#endif

#define PRINT_PUSHES
#ifdef PRINT_PUSHES
  if (seq(job->node)) {
    //        printf("ERROR: pushing job while in seq mode ");
  }
  assert(home_machine == job->node->board);

  printf("    M:%d P:%d %s TOP[%d] PUSH  [%d] <%d:%d> total_jobs: %d\n", 
	 job->node->board, job->node->path, 
 	 job->node->maxormin==MAXNODE?"+":"-", 
	 job->node->board, job->type_of_job,
	 job->node->a, job->node->b, total_jobs);
#endif
  //  sort_queue(queue[home_machine], top[home_machine]);
  //  print_queue(queue[home_machine], top[home_machine]);
}


void flush_buffer(int my_id, int home_machine) {
  if (buffer_top[my_id][home_machine] == 0) {
    return;
  }
    // local buffer is full. flush to the remote machine and insert in the queue. lock the remote machine queue
    lock(&jobmutex[home_machine]);
    int item = 0;
    //    printf("Flushing [%d] buffer to M:%d size %d\n", my_id, home_machine, buffer_top[my_id][home_machine]);
    for (item = 1; item <= buffer_top[my_id][home_machine]; item++) {
      // insert the items on top of the current top, append at the end
      job_type *job = local_buffer[my_id][home_machine][item];
      local_queue[home_machine][++local_top[home_machine]] = job;
      if (local_top[home_machine] >= N_JOBS) {
	printf("Local_top[%d]: %d\n", home_machine, local_top[home_machine]);
	printf("Root: <%d:%d>\n", root->a, root->b);
	//kunnen we queues printen waaruit blijkt waar de deadlock zit?
	for (int m=0; m < N_MACHINES; m++) {
	  for (int j=1; j < local_top[m]-1; j++) {
	    if (j%1000==0) {
	      printf("M:%d Q[%d]=%d\n", m, j, local_queue[m][j]->type_of_job);
	    }
	  }
	}
	print_q_stats();
	exit(0);
      }
      //      first buffer-top index value is 1 (0 is not used) but for loop with item is 0..buffer_top non-inclusive. going one too low
#undef PRINT_FLUSH
#ifdef PRINT_FLUSH
      if (job) {
	printf("    M:%d P:%d %s TOP[%d]FLUSHING  [%d] <%d:%d> total_jobs: %d\n", 
	     job->node->board, job->node->path, 
	     job->node->maxormin==MAXNODE?"+":"-", 
	     job->node->board, job->type_of_job,
	     job->node->a, job->node->b, total_jobs);
      } else {
	printf("Job is null in FLUSH\n");
      }
#endif
    }
    //    printf("flushed %d jobs. local_top: %d\n", item-1, local_top[home_machine]);
    buffer_top[my_id][home_machine] = 0;
    unlock(&jobmutex[home_machine]);
    max_q_length[home_machine] = 
      max(max_q_length[home_machine], local_top[home_machine]);
}

void check_consistency_empty() {
  int e = 0;
  for (int i = 0; i < N_MACHINES; i++) {
    e += empty_jobs(i);
  }
  if (e != global_empty_machines) {
    printf("ERROR: inconsistency empty jobs %d %d\n", global_empty_machines, e);
    exit(0);
  }
}

void check_job_consistency() {
  /*
  int j = 0;
  for (int i = 0; i < N_MACHINES; i++) {
    j += top[i][SELECT] + top[i][UPDATE] + top[i][BOUND_DOWN] + top[i][PLAYOUT];
  }
  if (total_jobs != j) {
    printf("ERROR Inconsistency total_jobs =/= j %d %d\n", total_jobs, j);
    exit(0);
  }
  */
}

/*
** PULL JOB
*/

// home_machine is the id of the home_machine of the node
job_type *pull_job(int home_machine) {
  //  printf("M:%d Pull   ", home_machine);

  //#ifdef LOCAL_LOCK
  lock(&jobmutex[home_machine]);  // pull
  //#else
  //  lock(&global_jobmutex);  // pull
  //#endif
  //#ifdef GLOBAL_QUEUE
  //#else
  // no locks, not ever. this cannot be right. it must be protected from a push flush
  if (local_top[home_machine] > 0) {
    //    how can pull know if there are jobs remotely that can be gotten througha forceed remote flush, and versus just waiting for the jobs to accumulate and be flushed to us automatically?
    // hwo can we solve the startup problem? the machine must be allowed to run as a small machine in order to grow bigger
    job_type *job = local_queue[home_machine][local_top[home_machine]--];
    if (empty_jobs(home_machine)) {
      /*
      emotyjobs is too hi there may be jobs in a buffer, where empty says top is 0, so epmty is too high, and global emptymachines is too high, so too many selects are scheduled
local-empty should take local-buffertop into account
      */
	//	printf("M:%d will be empty. incr global_empty %d\n", home_machine, global_empty_machines);
	global_empty_machines++;
    }
#undef PRINT_PULLS
#ifdef PRINT_PULLS
    if (job) {
      printf("    M:%d P:%d %s TOP[%d]PULL  [%d] <%d:%d> jobs: %d\n", 
	     job->node->board, job->node->path, 
	     job->node->maxormin==MAXNODE?"+":"-", 
	     job->node->board, job->type_of_job,
	     job->node->a, job->node->b, total_jobs);
    } else {
      printf("  M:%d  PULL: job is null. local_top: %d\n", home_machine, local_top[home_machine]);
    }
#endif
    unlock(&jobmutex[home_machine]);  // pull
    return job;
  } else {
    // local queue is empty. wait for the buffer to give me a refill 
    unlock(&jobmutex[home_machine]);  // pull
    return NULL;
  }
  exit(99); // should never reach
  //#endif
  int jobt = BOUND_DOWN;
  // first try bound_down, then try update, then try select
  while (jobt > 0) {
#ifdef GLOBAL_QUEUE
    if (top[home_machine][jobt] > 0) {
#else
    if (local_top[home_machine] > 0) {
#endif
      total_jobs--;
      /*
      if (no_more_jobs_in_system(home_machine)) {

	pthread_cond_signal(&job_available[root->board]);
	//pthread_cond_signal(&global_job_available);
	// send signal naar root home machnien;
      }
      */
      //      assert(total_jobs >= 0);

#ifdef GLOBAL_QUEUE
      job_type *job = queue[home_machine][top[home_machine][jobt]--][jobt];
#else
      job_type *job = local_queue[home_machine][local_top[home_machine]--];
#endif
      if (empty_jobs(home_machine)) {
	//	printf("M:%d will be empty. incr global_empty %d\n", home_machine, global_empty_machines);
	global_empty_machines++;
      }
      //      check_job_consistency();
#ifdef LOCAL_LOCK
      unlock(&jobmutex[home_machine]);  // pull
#else
      unlock(&global_jobmutex);  // pull
#endif
      return job;
    }
    jobt --;
  }
#ifdef LOCAL_LOCK
  unlock(&jobmutex[home_machine]); // pull
#else
  unlock(&global_jobmutex); // pull
#endif
  global_no_jobs[home_machine]++;
  return NULL;
}





/*
// swap the pointers to jobs in the job array
void swap_jobs(job_type *q[], int t1, int t2) {
  job_type *tmp = q[t1];
  q[t1] = q[t2];
  q[t2] = tmp;
}

// this is not a full sort
// this is a single pass that, performed on a sorted list, will 
// keep it sorted.
void sort_queue(job_type *q[], int t) {
  return;
  // last inserted job is at the top
  // percolate update to the top. that is, percolate SELECTS down
  if (!q[t]) {
    return;
  }
  int top = t;
  if (q[t]->type_of_job == SELECT) {  
    //  keep going down until the other node is a SELECT
    while (top-- > 0 && q[top] && q[top]->type_of_job != SELECT) {
      swap_jobs(q, top+1, top);
    }
  }
  // now top is either an UPDATE or an EXPAND or a SELECT next to other SELECTS
  // now sort on depth. nodes with a low value for depth are closest 
  // to the leaves, so I want the lowest depth values to be nearest the top
  while (top-- > 0 && q[top] && q[top]->node->depth < q[top+1]->node->depth) {
    swap_jobs(q, top+1, top);
  }
}
*/

// there is a new bound. update the selects in the job queue 
int update_selects_with_bound(node_type *node) {
  return TRUE; // since in the shared mem version all updates to bounds
  // are already done in downwardupdartechildren: as soon as you update
  // the bounds in the child nodes, since the job queue 
  // has pointers to the nodes, all entries in the 
  // job queue are updated automatically

  int home_machine = node->board;
  int continue_update = 0;
  // find all the entries for this node in the job queue
#ifdef GLOBAL_QUEUE
  for (int i = 0; i < top[home_machine][SELECT]; i++) {
    job_type *job = queue[home_machine][i][SELECT];
#else
  for (int i = 0; i < local_top[home_machine]; i++) {
    job_type *job = local_queue[home_machine][i];
#endif
    if (job->node == node && job->type_of_job == SELECT) {
      //      since node == node I do not really have to update the bounds, they are already updated....
      continue_update |= job->node->a < node->a;
      job->node->a = max(job->node->a, node->a);
      continue_update |= job->node->b > node->b;
      job->node->b = min(job->node->b, node->b);
    }
  }
  return (continue_update);
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

void process_job(int my_id, job_type *job) {
  //  printf("Process job\n");
  if (job && job->node && live_node(job->node)) {
    switch (job->type_of_job) {
    case SELECT:      do_select(my_id, job->node);  break;
    case PLAYOUT:     do_playout(my_id, job->node); break;
    case UPDATE:      do_update(my_id, job->node);  break;
    case BOUND_DOWN:  do_bound_down(my_id, job->node);  break;
    otherwise: printf("ERROR: invalid job  type in q\n"); exit(0); break;
    }
  }
}

