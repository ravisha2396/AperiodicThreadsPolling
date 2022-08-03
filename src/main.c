
#include <zephyr.h>
#include <kernel.h>
#include <sys/printk.h>
#include <sys/util.h>
#include <stdbool.h>
#include <stdlib.h>
#include "task_model_p4.h"
#include <timing/timing.h>
#include <shell/shell_uart.h>
#include <logging/log.h>
#include <sys/time_units.h>
LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);


// Global flag threads use to determine if to stop running
static bool running = true;

struct k_timer budget_timer;

// k_thread structures / data
static struct k_thread thread_structs[NUM_THREADS+1];
static k_tid_t thread_tids[NUM_THREADS+1];

static int thread_index[NUM_THREADS+1];

static int done[NUM_THREADS+1];
static struct k_sem wait_sem[NUM_THREADS+1];

// Support up to sixteen threads' stacks
K_THREAD_STACK_DEFINE(thread_stack_area, STACK_SIZE * NUM_THREADS+1);

K_THREAD_STACK_DEFINE(my_stack_area, 128);

struct k_work priority_low_work, priority_high_work;

uint64_t t_resp=0;

static struct req_type data;

uint32_t ctr=0;

static struct k_sem cmd_sem;

void aperiodic_switched_in(){
    if(k_current_get()==thread_tids[NUM_THREADS] && poll_info.left_budget > 0){
        // start budget timer with left budget
        k_timer_start(&budget_timer,K_NSEC(poll_info.left_budget), K_NSEC(poll_info.left_budget));
    }
}

void aperiodic_switched_out(){
    if(k_current_get()==thread_tids[NUM_THREADS] && poll_info.left_budget > 0){
        // calculate left budget and stop the budget timer
        poll_info.left_budget = 1000000*k_timer_remaining_get(&budget_timer);
        k_timer_stop(&budget_timer);
    }
}

static void timer_expiry_function(struct k_timer *timer_exp)
{
    int id = *(int *)timer_exp->user_data;
    int ret;
    if(id==NUM_THREADS){
        poll_info.left_budget = 1000000*BUDGET;
        k_work_submit(&priority_high_work);
    }
    ret = done[id];  
    if (ret==1) {
        k_sem_give(&wait_sem[id]);
    }
    else {
        //LOG_DBG("task %d misses its deadline \n", id);
        k_sem_give(&wait_sem[id]);
    }
}

void priority_low_function(){
    // Decrease priority
    k_thread_priority_set(thread_tids[NUM_THREADS], 14);
} 

void priority_high_function(){
    // Increase priority
    k_thread_priority_set(thread_tids[NUM_THREADS], poll_info.priority);
} 

static void budget_expiry_function(struct k_timer *timer_exp)
{
    // make left budget zero and reduce priority 
    poll_info.left_budget = 0;
    k_timer_stop(&budget_timer);
    k_work_submit(&priority_low_work);
}

// Function that is executed for each thread in the task set
static void thread_execute_periodic(void *v_task_info, void *v_thread_id, void *unused)
{
    //wait for activation input from shell command
    k_sem_take(&cmd_sem, K_FOREVER);

    struct task_s *task_info = (struct task_s *)v_task_info;
    int thread_id = *(int *)v_thread_id; 
 
	uint32_t period;

    struct k_timer task_timer;

    k_timer_init(&task_timer, timer_expiry_function, NULL);
    task_timer.user_data = v_thread_id;

    
    LOG_DBG("task %d gets started \n", thread_id);
	period = 1000000*task_info->period; 

    k_timer_start(&task_timer,K_NSEC(period), K_NSEC(period));

    while (running) {

        looping(task_info->loop_iter);

        done[thread_id]=1;
        
        k_sem_take(&wait_sem[thread_id], K_FOREVER);
    }
    k_timer_stop(&task_timer);
}

// polling server

static void thread_execute_aperiodic(void *a_task_info, void *v_thread_id, void *unused)
{   

    //wait for activation input from shell command
    k_sem_take(&cmd_sem, K_FOREVER);

    struct task_aps *task_info = (struct task_aps *)a_task_info;
    int thread_id = *(int *)v_thread_id; 

    LOG_DBG("Thread id: %d\n", thread_id);
	uint32_t period;

    struct k_timer task_timer;
    
    k_timer_init(&task_timer, timer_expiry_function, NULL);
    task_timer.user_data = v_thread_id;
    uint32_t end = 0;
    LOG_DBG("task %d gets started \n", thread_id);
	period = 1000000*task_info->period; 
    
    k_timer_start(&task_timer,K_NSEC(period), K_NSEC(period));
    
    memset(&data, 0, sizeof(struct req_type));
    
    while(running) {
        while(1){
            if(!running){
                break;
            }
            if(!k_msgq_get(&req_msgq, &data, K_NO_WAIT)){
                // if active and budget remaining
                if(poll_info.left_budget>0){
                    ctr++;
                    k_timer_start(&budget_timer, K_NSEC(poll_info.left_budget),K_NO_WAIT);
                    looping(data.iterations);
                    end = k_cycle_get_32();
                    t_resp+=sub32(data.arr_time, end);
                    poll_info.left_budget = 1000000*k_timer_remaining_get(&budget_timer);
                }
                // if in background
                else{
                    ctr++;
                    looping(data.iterations);
                    end = k_cycle_get_32();
                    t_resp+=sub32(data.arr_time, end);
                }

            }
           // make bg when queue is empty
           else{
               k_timer_stop(&budget_timer);
               poll_info.left_budget = 0;
               k_work_submit(&priority_low_work);
           } 
                
        }
    }
    t_resp = k_cyc_to_ms_near64(t_resp);

    k_timer_stop(&budget_timer);
    k_timer_stop(&task_timer);
}

// Start all threads defined in the task set
static void start_threads(void)
{
    k_sem_init(&cmd_sem,0,NUM_THREADS+1);
	    
    for (int i = 0; i < NUM_THREADS+1; i++) {
        k_sem_init(&wait_sem[i], 0, 1);
        done[i]=0;
    }

    // Small delay to make sure we set each thread's name before it runs

    // Start each thread
    for (int i = 0; i < NUM_THREADS; i++) {
		thread_index[i]=i;
        thread_tids[i] = k_thread_create(&thread_structs[i],
                                         &thread_stack_area[STACK_SIZE * i],
                                         STACK_SIZE * sizeof(k_thread_stack_t),
                                         thread_execute_periodic, (void *)&threads[i],
                                         (void *)&thread_index[i], NULL, threads[i].priority,
                                         0, K_MSEC(10));

        k_thread_name_set(thread_tids[i], threads[i].t_name);

    }

    thread_index[NUM_THREADS]=NUM_THREADS;
    thread_tids[NUM_THREADS] = k_thread_create(&thread_structs[NUM_THREADS],
                                         &thread_stack_area[STACK_SIZE * NUM_THREADS],
                                         STACK_SIZE * sizeof(k_thread_stack_t),
                                         thread_execute_aperiodic, (void *)&poll_info,
                                         (void *)&thread_index[NUM_THREADS], NULL, poll_info.priority,
                                         0, K_MSEC(10));

    poll_info.poll_tid = thread_tids[NUM_THREADS];

    k_thread_name_set(thread_tids[NUM_THREADS], poll_info.t_name);


    //printk("Threads Initialized!\n");
    LOG_DBG("Threads Initialized!\n");
}

// "Entry point" of our code
void main(void)
{   
    k_timer_init(&budget_timer, budget_expiry_function, NULL);
    k_work_init(&priority_low_work, priority_low_function);
    k_work_init(&priority_high_work, priority_high_function);
    // Spawn the threads
    start_threads();

    // start request timer
    k_timer_start(&req_timer, K_USEC(ARR_TIME), K_NO_WAIT); 

    for(int i=0;i<NUM_THREADS+1;i++){
        k_sem_give(&cmd_sem);
    }

    k_sleep(K_MSEC(TOTAL_TIME));

    k_timer_stop(&req_timer);
    // Stop the threads!
    running = false;

    //terminate waiting threads waiting on semaphore
    for (int i = 0; i < NUM_THREADS+1; ++i) {
        k_sem_give(&wait_sem[i]);
    }

    //wait for threads to exit
    for (int i = 0; i < NUM_THREADS+1; ++i) {
        k_thread_join(&thread_structs[i],K_FOREVER);
    }

    printk("Average response time: %lld\n", (t_resp/ctr));
    printk("no.of requests processed: %d\n", ctr);

    LOG_DBG("Stopped threads\n");
}
