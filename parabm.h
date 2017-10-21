
/*
 * parab.h
 *
 * Aske Plaat 2017
*/



/*
 * defines
 */

#define N_MACHINES 4
#define TREE_WIDTH 4
#define TREE_DEPTH 4

#define SEQ_DEPTH 1  // low is low par threshold. low is much par

#define N_JOBS 500000 
#define BUFFER_SIZE 100

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

// use Parallel Unorderedness to determine how much parallelism there should be scheduled
#undef PUO
#define LOCKS
#undef LOCAL_Q 

#define LOCAL_LOCK
#undef GLOBAL_QUEUE


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
  int children_at[TREE_WIDTH]; // array of number/id of machine that holds the child
  int live_children[TREE_WIDTH]; // array of liveness boolean of my children
  int n_children;
  //  node_type *parent;
  int parent; // machine number that holds the parent;
  //  node_type *best_child;
  int best_child; // machine number that holds the best child;
  int maxormin;
  int depth;
  int path;
  //  pthread_mutex_t nodelock;
  int job_type_of_job; // room for MPI msgs for data from the job
  int job_from_machine;
  int job_lb;
  int job_ub;
} nt;

struct job {
  node_type *node;
  int type_of_job;
  int from;
  int lb;
  int ub;
};
typedef struct job job_type;



/*
 * variables
 */

extern node_type *root;
extern int global_empty_machines;
extern int my_process_id; // MPI machine rank
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

/*
 * prototypes
 */

void print_q_stats();
void print_queues();
int lock(pthread_mutex_t *mutex);
int unlock(pthread_mutex_t *mutex);
int lock_node(node_type *n);
int unlock_node(node_type *n);
int start_mtdf();
int start_alphabeta(int a, int b);
int child_number(int p);
int puo(node_type *node);
void set_best_child(node_type *node);
job_type *new_job(node_type *n, int t);
node_type *new_leaf(node_type *p);
int opposite(int m);
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
int not_empty(int top, int home);
void add_to_queue(int my_id, job_type *job);
void process_job(int my_id, job_type *job);
void schedule(int my_id, node_type *node, int t);
void do_select(int my_id, node_type *node);
node_type * first_live_child(node_type *node, int p);
void do_playout(int my_id, node_type *node);
int evaluate(node_type *node);
void do_update(int my_id, node_type *node);
void do_bound_down(int my_id, node_type *node);
void downward_update_children(int my_id, node_type *node);
void store_node(node_type *node); 
void update_bounds_down(node_type *node, int a, int b);
void check_consistency_empty();
void flush_buffer(int my_id, int home_machine);
