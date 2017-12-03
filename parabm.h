
/*
 * parab.h
 *
 * Aske Plaat 2017
*/



/*
 * defines
 */

#define N_MACHINES 1
#define TREE_WIDTH 5
#define TREE_DEPTH 3

#define SEQ_DEPTH 1  // low is low par threshold. low is much par

#define N_JOBS 500000 
#define BUFFER_SIZE 100

#define HASH_TABLE_SIZE 1024*1024

#define INFTY  99999

#define SELECT 1
#define PLAYOUT 2
#define UPDATE 3
#define BOUND_DOWN 4
#define JOB_TYPES (BOUND_DOWN+1)

#define MAXNODE 1
#define MINNODE 2

#define TRUE 1
#define FALSE 0

#define SAFETY_COUNTER_INIT 100000000

#define NO_LIVE_CHILD -1
#define NO_ARG 0

#define AB_OPEN_INFTY
#define UL_OPEN_INFTY
#define NWS_ON

#define INVALID 0xabcdefL

#define RANDOM_TABLE_SIZE 100



// MSG types
#define CREATE_CHILD_AT 0
#define UPDATE_PARENT_AT 1
#define UPDATE_CHILD_AT 2
#define SELECT_CHILD_AT 3
#define IDLE_MSG 4
#define NONIDLE_MSG 5
#define DECR_ACTIVE_ROOT 6

// use Parallel Unorderedness to determine how much parallelism there should be scheduled
#undef PUO
#define LOCKS
#undef LOCAL_Q 

#define LOCAL_LOCK
#undef GLOBAL_QUEUE


#define MSGPASS


/*
 * structs
 */

typedef struct node node_type;
typedef struct node { 
  int board;
  int wa; // window-alpha, passed in from caller.
  int wb;  // wa and wb are only used in liveness calulation. not stored in nodes lb, ub, a, b
  int a; // true alpha computed as max lower bound
  int b; // true beta, computerd as lowest upper bound
  int lb;
  int ub;
  //  node_type **children;
  int from_child; // I am the parent of a child, and that child scheduled me. which child was that?
  int max_of_closed_kids_ub;
  int min_of_closed_kids_lb;
  int n_open_kids;
  int n_active_kids;
  int my_child_number;
  int open_child[TREE_WIDTH];
  int children_at[TREE_WIDTH]; // array of number/id of machine that holds the child
  int live_children[TREE_WIDTH]; // array of liveness boolean of my children
  int n_children;
  //  node_type *parent;
  int parent_at; // machine number that holds the parent;
  //  node_type *best_child;
  //  int best_child; // machine number that holds the best child;
  int maxormin;
  int depth; // leaves: 0. root: depth
  int *path;//[TREE_DEPTH];
  //  pthread_mutex_t nodelock;
} nt;

struct job {
  node_type *node;
  int type_of_job;
  int from;
  int lb;
  int ub;
};
typedef struct job job_type;


typedef struct child_msg_struct {
  int msg_type;
  int path[TREE_DEPTH+1];
  int depth;
  int child_number;
  int mm;
  int wa;
  int wb;
  int parent_at;
} ch1;
typedef struct child_msg_struct child_msg_type;

typedef struct parent_msg_struct {
  int msg_type;
  int path[TREE_DEPTH+1];
  int depth;
  int from_child;
  int child_live;
  int lb;
  int ub;
  int mm;
} ch2;
typedef struct parent_msg_struct parent_msg_type;


typedef struct update_msg_struct {
  int msg_type;
  int path[TREE_DEPTH+1];
  int depth;
  int a;
  int b;
} ch3;
typedef struct update_msg_struct update_msg_type;


typedef struct hash_entry_struct {
  int key;
  node_type *node;
} h;
typedef struct hash_entry_struct hash_entry_type;


/*
 * variables
 */

extern node_type *root;
extern hash_entry_type hash_table[HASH_TABLE_SIZE];
extern int global_empty_machines;
extern int my_process_id; // MPI machine rank
#ifndef MSGPASS
#ifdef GLOBAL_QUEUE
extern job_type *queue[N_MACHINES][N_JOBS][JOB_TYPES];
extern int top[N_MACHINES][JOB_TYPES];
extern int max_q_length[N_MACHINES][JOB_TYPES];
#else
extern job_type *local_queue[N_MACHINES][N_JOBS];
extern int local_top[N_MACHINES];
extern job_type *local_buffer[N_MACHINES][N_MACHINES][BUFFER_SIZE];
extern int buffer_top[N_MACHINES][N_MACHINES];
extern int max_q_length[N_MACHINES];
#endif
#endif
extern int total_jobs;
extern pthread_mutex_t jobmutex[N_MACHINES];
extern pthread_mutex_t global_jobmutex;
extern pthread_cond_t job_available[N_MACHINES];
//extern pthread_cond_t global_job_available;
extern pthread_mutex_t treemutex;
extern pthread_mutex_t donemutex;
extern pthread_mutex_t global_queues_mutex;
extern int n_par;
extern int global_selects[N_MACHINES];
extern int global_updates[N_MACHINES];
extern int global_leaf_eval[N_MACHINES];
extern int global_downward_aborts[N_MACHINES];
extern int sum_global_selects;
extern int sum_global_updates;
extern int sum_global_leaf_eval;
extern int sum_global_downward_aborts;
extern double global_unorderedness_seq_x[TREE_DEPTH];
extern int global_unorderedness_seq_n[TREE_DEPTH];
extern int global_no_jobs[N_MACHINES];
extern int global_done;
extern int global_in_wait;
extern int my_process_id;  // MPI
extern int world_size;
extern int random_table[RANDOM_TABLE_SIZE];


/*
 * prototypes
 */

void print_q_stats();
void print_queues();
node_type *lookup(int path[], int depth);
void store(node_type *node);
int lock(pthread_mutex_t *mutex);
void print_path(int path[], int depth);
void select_child_at(node_type *node, int child_number, int mm);
int unlock(pthread_mutex_t *mutex);
int lock_node(node_type *n);
int unlock_node(node_type *n);
int where();
void decr_active_root();
int start_mtdf();
int start_alphabeta(int a, int b);
int child_number(node_type *node);
int puo(node_type *node);
void set_best_child(node_type *node);
//job_type *new_job(node_type *n, int t);
node_type *new_leaf(child_msg_type *msg, int ch, int mm);
int hash(int path[], int depth);
int opposite(node_type *node);
node_type *first_child(node_type *node);
int max_of_beta_kids(node_type *node);
int min_of_alpha_kids(node_type *node);
int max_of_ub_kids(node_type *node);
int min_of_lb_kids(node_type *node);
void compute_bounds(node_type *node);
int leaf_node(node_type *node);
int seq(node_type *node);
int min(int a, int b);
int max(int a, int b);
int no_more_jobs_in_system(int home);
int update_selects_with_bound(node_type *node);
void sort_queue(job_type *q[], int t);
void print_queue(job_type *q[], int t);
node_type *next_brother(node_type *node);
int main(int argc, char *argv[]);
void print_tree(node_type *node, int d);
void create_tree(int d);
void push_job(int my_id, int home_machine, job_type *job);
job_type *pull_job(int home_machine);
void mk_children(node_type *n, int d);
int unexpanded_node(node_type *node);
int expanded_node(node_type *node);
int live_node(node_type *node);
int dead_node(node_type *node);
int live_node_lbub(node_type *node);
int dead_node_lbub(node_type *node);
void start_processes(int n_proc);
void do_work_queue(int i);
int not_empty_and_live_root();
int empty(int top, int home);
void do_expand(child_msg_type *msg);
void create_child_at(node_type *node, int child_number, int mm);
void update_parent_at(node_type *node, int from_child_id);
void update_child_at(node_type *node, int child_number);
int not_empty(int top, int home);
//void add_to_queue(int my_id, job_type *job);
void process_job(int my_id, job_type *job);
//void schedule(int my_id, node_type *node, int t);
void do_select(node_type *node);
int first_live_child(node_type *node);
void do_playout(node_type *node);
int evaluate(node_type *node);
int root_node(node_type *node);
void do_update(parent_msg_type *msg);
void do_bound_down(int my_id, node_type *node);
void downward_update_children(update_msg_type *msg);
void store_node(node_type *node); 
void update_bounds_down(node_type *node, int a, int b);
void check_consistency_empty();
//void flush_buffer(int my_id, int home_machine);
