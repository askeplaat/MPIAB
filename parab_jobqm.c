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

void start_processes(int n_proc) {
  global_done = FALSE;
  MPI_Comm_rank(MPI_COMM_WORLD, &my_process_id);
  do_work_queue(my_process_id);
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
       	printf("%d Selecting child %d depth: %d ", my_process_id, msg->child_number, msg->depth); 
	print_path(msg->path, msg->depth); // path is alwyas [0,0,0,0] so lways the samen node is selected. the leftfirst leaf....
	if (msg->depth <0 || msg->depth > TREE_DEPTH) {
	  printf("ERROR in Dispatch depth %d\n", msg->depth);
	}
	node_type *node = lookup(msg->path, msg->depth);
	if (node) {
	  do_select2(node);
	} else { 
	  printf("LOOKUP returned NULL, could not find node in message in TT path: "); print_path(msg->path, msg->depth);
	  decr_active_root();
	  /*
maybe this can happen correctly.
  maybe this is an update child of a child that does not exist. then do not create, I would say. But just continue with select
or whatever.
	  */
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
	root->n_active_kids--;
	printf("Decrementing Active count in root\n", root->n_active_kids);
      } else {
	printf("ERROR: Wrong MPI message type\n");
        exit(0);
      }
}



void do_work_queue(int i) {
  //  int workerNum = __cilkrts_get_worker_number();
  int safety_counter = SAFETY_COUNTER_INIT;
  global_empty_machines = world_size;

  printf("WORK QUEUE START: Root depth is %d\n", root->depth);

  while (safety_counter-- > 0 && !global_done && live_node(root)) { 
    job_type *job = NULL;

    //    printf("%d active kids: %d\n", my_process_id, root->n_active_kids);
    print_path(root->path, TREE_DEPTH);
    printf("Root (depth: %d) values: expanded_children: %d live_children: %d lu: <%d,%d>\n", 
	   root->depth, root->n_expanded_children, root->n_live_children, root->lb, root->ub);
    //    root->n_children++;
    //    printf("Root children: %d\n", root->n_children);
    //    printf("PRINT_TREE: ");
    //    print_tree(root->path, TREE_DEPTH);
  

    if (i == root->board && live_node(root) && root->n_active_kids == 0) {
      //     printf("* globalempty machines: %d\n", global_empty_machines);

      printf("%d CALLING SELECT_CHILD_AT FROM EMPTY WORKQUEUE at root. n_active: %d. n_expanded: %d. n_live: %d  ", 
	     my_process_id, root->n_active_kids, root->n_expanded_children, root->n_live_children);
      int ch = 0;
      for (ch = 0; ch < TREE_WIDTH; ch++) {
	if (!root->expanded_children[ch]) {
	  printf("Depth: %d CREATE CHILD %d\n", root->depth, ch);
	  create_child_at(root, ch, MINNODE); 
	  break;
	} else if (root->live_children[ch]) {
	  printf("Depth: %d SELECT CHILD %d\n", root->depth, ch);
	  select_child_at(root, ch, MINNODE); 
	  break;
	}
      }
      if (ch > TREE_WIDTH) {
	printf("ERROR: NO LIVE KIDS IN ROOT. ch: %d. expanded_children: %d\n", ch, root->n_expanded_children);
	exit(0);
      }
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
	printf("MPISend Idle msg\n");
	MPI_Send(&idle_msg, sizeof(idle_msg), MPI_BYTE, 0, 0, MPI_COMM_WORLD);
      }
      printf("RECEIVING...");fflush(stdout);
      MPI_Recv(msg, sizeof(child_msg_type), MPI_BYTE, MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);
      printf("RECEIVED %d\n", msg->msg_type);fflush(stdout);
      //      if (i != root->board) {
      if (msg->msg_type != IDLE_MSG && msg->msg_type != NONIDLE_MSG) {
	if (i != root->board) {
	  printf("MPISend nonidle msg\n");
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
  node->expanded_children[child_number] = TRUE;
  node->n_expanded_children++;
  //  printf("create child: Root n_children: %d\n", root->n_children);
  //  node->n_children++;
  //  printf("create child: Root n_children: %d\n", root->n_children);

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

  printf("MPISEND %d create sending CHILD %d to %d\n", my_process_id, child_number, home_machine);

  // the idea to only send the pointer, not the full node.
  MPI_Send(&child_msg, sizeof(child_msg_type), MPI_BYTE, home_machine, 0, MPI_COMM_WORLD);
  // receiver must store parent-at, and copy wa wb
}

void select_child_at(node_type *node, int child_number, int mm) {
  int i = 0;
  child_msg_type child_msg;
  child_msg.msg_type = SELECT_CHILD_AT;
  int home_machine = node->children_at[child_number];
  node->n_active_kids++;
  //  node->n_children++;

  //  node->children_at[child_number] = home_machine;
  child_msg.child_number = child_number;
  //  child_msg.depth = node->depth;
  child_msg.mm = mm;
  for (i=0; i < TREE_DEPTH; i++) {
    child_msg.path[i] = node->path[i]; 
  }
  child_msg.path[node->depth - 1] = child_number;

  child_msg.wa = node->wa;
  child_msg.wb = node->wb;
  child_msg.depth = node->depth - 1;
  child_msg.parent_at = my_process_id;

  //  do_nonidle_machine(home_machine);
  printf("MPISEND %d select sending CHILD %d to %d  ", my_process_id, child_number, home_machine);
  print_path(child_msg.path, child_msg.depth);

  // the idea to only send the pointer, not the full node.
  MPI_Send(&child_msg, sizeof(child_msg_type), MPI_BYTE, home_machine, 0, MPI_COMM_WORLD);
  // receiver must store parent-at, and copy wa wb
}

 

void decr_active_root() {
  update_msg_type m;
  m.msg_type = DECR_ACTIVE_ROOT;
  printf("MPISend Decr Active\n");
  MPI_Send(&m, sizeof(update_msg_type), MPI_BYTE, 0, 0, MPI_COMM_WORLD);
}

void update_parent_at(node_type *node, int from_child_id) {
  if (node->depth >= TREE_DEPTH) { 
    return;
  }

  parent_msg_type parent_msg;
  parent_msg.msg_type = UPDATE_PARENT_AT;
  int home_machine = node->parent_at;
  //but this is not passed to msg
  //should be parent's active kid number that is decremented.' 
  parent_msg.from_child = from_child_id;
  parent_msg.depth = node->depth +1;
  int old_live = node->live_children[from_child_id];
  int new_live = parent_msg.child_live = node->lb < node->ub;
  if (old_live && !new_live) {
    node->n_live_children--;
  }
  parent_msg.lb = node->lb;
  parent_msg.ub = node->ub;
  parent_msg.mm = opposite(node);
  for (int i=0; i < TREE_DEPTH; i++) {
    parent_msg.path[i] = node->path[i];
  }

  printf("MPISEND %d SEND update to parent %d\n", my_process_id, home_machine);
  fflush(stdout);
  MPI_Send(&parent_msg, sizeof(parent_msg_type), MPI_BYTE, home_machine, 0, MPI_COMM_WORLD);
}

void update_child_at(node_type *node, int child_number) {
  int i = 0;
  if (node->n_expanded_children > TREE_WIDTH
      || node->children_at[child_number] < 0 
      || node->children_at[child_number] > world_size) {
    return;
  }
  update_msg_type update_msg;
  update_msg.msg_type = UPDATE_CHILD_AT;
  int home_machine = node->children_at[child_number];
  //  update_msg->from_child = from_child_id;
  update_msg.depth = node->depth - 1; // -1 since child: closer to leaves which are dpeth==0
  update_msg.a = node->a;
  update_msg.b = node->b;
  for (i= TREE_DEPTH - 1; i >= node->depth; i--) {
    update_msg.path[i] = node->path[i];
  }
  update_msg.path[node->depth-1] = child_number;

  //  printf("Updating child "); print_path(update_msg.path, update_msg.depth);
  printf("MPISEND %d SEND update to child %d\n", my_process_id, home_machine);
  fflush(stdout);
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
