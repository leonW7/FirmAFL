/*
   american fuzzy lop - high-performance binary-only instrumentation
   -----------------------------------------------------------------

   Written by Andrew Griffiths <agriffiths@google.com> and
              Michal Zalewski <lcamtuf@google.com>

   Idea & design very much by Andrew Griffiths.

   Copyright 2015, 2016, 2017 Google Inc. All rights reserved.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at:

     http://www.apache.org/licenses/LICENSE-2.0

   This code is a shim patched into the separately-distributed source
   code of QEMU 2.10.0. It leverages the built-in QEMU tracing functionality
   to implement AFL-style instrumentation and to take care of the remaining
   parts of the AFL fork server logic.

   The resulting QEMU binary is essentially a standalone instrumentation
   tool; for an example of how to leverage it for other purposes, you can
   have a look at afl-showmap.c.

 */
#include "zyw_config.h"
int print_debug = 0;
int libuclibc_addr;

#ifdef MEM_MAPPING
void * snapshot_shmem_start;
void * snapshot_shmem_pt;
int snapshot_shmem_id;

target_ulong pre_map_page[2048];//jjhttpd 0x31000 //0x1000// 0x56000 //0x51000
int pre_map_index = 0;
void add_premap_page(target_ulong pc)
{
  assert(pc!=0);
  assert(pre_map_index < (2048 - 1));
  pre_map_page[pre_map_index++] = (pc & 0xfffff000);
}
int if_premap_page(target_ulong pc)
{
  assert(pc!=0);
  for(int i=0; i< 2048; i++)
  {
    if(pre_map_page[i] == (pc & 0xfffff000))
      return 1;
  }
  return 0;
}

int if_page_pc(target_ulong pc)
{
  assert(pc!=0);
  if((pc & 0xfff) == 0)
  {
    return 1;
  }
  else
  {
    return 0;
  }
}

#ifdef SNAPSHOT_SYNC

char *phys_addr_stored_bitmap;
int syn_shmem_id = 0; 

#endif

#endif

#include <sys/shm.h>
#include "../../config.h"

/***************************
 * VARIOUS AUXILIARY STUFF *
 ***************************/

/* A snippet patched into tb_find_slow to inform the parent process that
   we have hit a new block that hasn't been translated yet, and to tell
   it to translate within its own context, too (this avoids translation
   overhead in the next forked-off copy). */

#define AFL_QEMU_CPU_SNIPPET1 do { \
    afl_request_tsl(pc, cs_base, flags); \
  } while (0)

/* This snippet kicks in when the instruction pointer is positioned at
   _start and does the usual forkserver stuff, not very different from
   regular instrumentation injected via afl-as.h. */

extern void feed_input(CPUState *env);
int fork_times = 0;
#define AFL_QEMU_CPU_SNIPPET2 do { \
    if(env->active_tc.PC == afl_entry_point && fork_times==0) { \
      fork_times=1; \
      afl_setup(); \
      afl_forkserver(cpu); \
      feed_input(cpu); \
    } \
    afl_maybe_log(env->active_tc.PC); \
  } while (0)


#define AFL_QEMU_CPU_SNIPPET3 do { \
    if(env->active_tc.PC == afl_entry_point && fork_times==0) { \
      fork_times=1; \
      start_run(); \
    } \
    afl_maybe_log(env->active_tc.PC); \
  } while (0)

/* We use one additional file descriptor to relay "needs translation"
   messages between the child and the fork server. */


#define TSL_FD (FORKSRV_FD - 1)

/* This is equivalent to afl-as.h: */

static unsigned char *afl_area_ptr;

/* Exported variables populated by the code patched into elfload.c: */
abi_ulong afl_entry_point, /* ELF entry point (_start) */
          afl_start_code,  /* .text start pointer      */
          afl_end_code;    /* .text end pointer        */
/* Set in the child process in forkserver mode: */

unsigned char afl_fork_child;
unsigned int afl_forksrv_pid;

/* Instrumentation ratio: */

static unsigned int afl_inst_rms = MAP_SIZE;

/* Function declarations. */

static void afl_setup(void);
static void afl_forkserver(CPUState*);
static inline void afl_maybe_log(abi_ulong);

static void afl_wait_tsl(CPUState*, int);
static void afl_request_tsl(target_ulong, target_ulong, uint64_t);

/* Data structure passed around by the translate handlers: */

struct afl_tsl {
  target_ulong pc;
  target_ulong cs_base;
  uint64_t flags;
};

/* Some forward decls: */

//TranslationBlock *tb_htable_lookup(CPUState*, target_ulong, target_ulong, uint32_t);
static inline TranslationBlock *tb_find(CPUState*, TranslationBlock*, int);

/*************************
 * ACTUAL IMPLEMENTATION *
 *************************/

/* Set up SHM region and initialize other stuff. */

static void afl_setup(void) {

  char *id_str = getenv(SHM_ENV_VAR),
       *inst_r = getenv("AFL_INST_RATIO");

  int shm_id;

  if (inst_r) {

    unsigned int r;

    r = atoi(inst_r);

    if (r > 100) r = 100;
    if (!r) r = 1;

    afl_inst_rms = MAP_SIZE * r / 100;

  }

  if (id_str) {

    shm_id = atoi(id_str);
    afl_area_ptr = shmat(shm_id, NULL, 0);

    if (afl_area_ptr == (void*)-1) exit(1);

    /* With AFL_INST_RATIO set to a low value, we want to touch the bitmap
       so that the parent doesn't give up on us. */

    if (inst_r) afl_area_ptr[0] = 1;


  }

  if (getenv("AFL_INST_LIBS")) {

    afl_start_code = 0;
    afl_end_code   = (abi_ulong)-1;

  }

  /* pthread_atfork() seems somewhat broken in util/rcu.c, and I'm
     not entirely sure what is the cause. This disables that
     behaviour, and seems to work alright? */

  rcu_disable_atfork();

}

int run_time = 0;

static void start_run() {
  if(run_time == 0)
  {
    run_time = 1;
    int cmd = 0x10;// start mem write callback
    USER_MODE_TIME user_mode_time;
    write_aflcmd(cmd,  &user_mode_time);
  }
 
}


/* Fork server logic, invoked once we hit _start. */

static void afl_forkserver(CPUState *cpu) {

  static unsigned char tmp[4];

  if (!afl_area_ptr) return;

  /* Tell the parent that we're alive. If the parent doesn't want
     to talk, assume that we're not running in forkserver mode. */

  if (write(FORKSRV_FD + 1, tmp, 4) != 4) return;

  afl_forksrv_pid = getpid();

  /* All right, let's await orders... */

  while (1) {

    pid_t child_pid;
    int status, t_fd[2];

    /* Whoops, parent dead? */

    if (read(FORKSRV_FD, tmp, 4) != 4) exit(2);

    /* Establish a channel with child to grab translation commands. We'll
       read from t_fd[0], child will write to TSL_FD. */

    if (pipe(t_fd) || dup2(t_fd[1], TSL_FD) < 0) exit(3);
    close(t_fd[1]);

    child_pid = fork();
    if (child_pid < 0) exit(4);

    if (!child_pid) {

      /* Child process. Close descriptors and run free. */

#ifdef MEM_MAPPING
      int cmd = 0x10;// start mem write callback
      USER_MODE_TIME user_mode_time;
      write_aflcmd(cmd,  &user_mode_time);
//SHMAT
      //snapshot_shmem_start = shmat(snapshot_shmem_id, guest_base + 0x182000000 ,  1); //zyw 
      //snapshot_shmem_start = shmat(snapshot_shmem_id, 0x80200000 ,  0); //zyw 
      //snapshot_shmem_start = shmat(snapshot_shmem_id, NULL,  1); //zyw 

      //memset(snapshot_shmem_start, 0, 1024*1024*16); // oh, it takes lots of time here !!!!!!!!!!!!, the most time cost in user mode; memset 16M

      //snapshot_shmem_pt = snapshot_shmem_start + 8 * 0x1000;
      afl_fork_child = 1;
#endif

      close(FORKSRV_FD);
      close(FORKSRV_FD + 1);
      close(t_fd[0]);
      printf("*********t_fd:%d,%d,%d,%d\n", t_fd[0], t_fd[1], TSL_FD, FORKSRV_FD);
      return;

    }

    /* Parent. */

    close(TSL_FD);

    if (write(FORKSRV_FD + 1, &child_pid, 4) != 4) exit(5);

    /* Collect translation requests until child dies and closes the pipe. */

    afl_wait_tsl(cpu, t_fd[0]);

    /* Get and relay exit status to parent. */
    if (waitpid(child_pid, &status, 0) < 0) exit(6);
    status = WEXITSTATUS(status);//zyw
    //printf("status:%d\n", status); 
    if (write(FORKSRV_FD + 1, &status, 4) != 4) exit(7);

  }

}


/* The equivalent of the tuple logging routine from afl-as.h. */

static inline void afl_maybe_log(abi_ulong cur_loc) {

  static __thread abi_ulong prev_loc;

  /* Optimize for cur_loc > afl_end_code, which is the most likely case on
     Linux systems. */

  if (cur_loc > afl_end_code || cur_loc < afl_start_code || !afl_area_ptr)
    return;

  /* Looks like QEMU always maps to fixed locations, so ASAN is not a
     concern. Phew. But instruction addresses may be aligned. Let's mangle
     the value to get something quasi-uniform. */

  cur_loc  = (cur_loc >> 4) ^ (cur_loc << 8);
  cur_loc &= MAP_SIZE - 1;

  /* Implement probabilistic instrumentation by looking at scrambled block
     address. This keeps the instrumented locations stable across runs. */

  if (cur_loc >= afl_inst_rms) return;
  //printf("%lx, %lx. %lx\n",afl_area_ptr, cur_loc ^ prev_loc, MAP_SIZE);
  //printf("%x", *(afl_area_ptr + (cur_loc ^ prev_loc)));
  afl_area_ptr[cur_loc ^ prev_loc]++;
  prev_loc = cur_loc >> 1;

}


/* This code is invoked whenever QEMU decides that it doesn't have a
   translation of a particular block and needs to compute it. When this happens,
   we tell the parent to mirror the operation, so that the next fork() has a
   cached copy. */

static void afl_request_tsl(target_ulong pc, target_ulong cb, uint64_t flags) {

  struct afl_tsl t;

  if (!afl_fork_child) return;

  t.pc      = pc;
  t.cs_base = cb;
  t.flags   = flags;

  if (write(TSL_FD, &t, sizeof(struct afl_tsl)) != sizeof(struct afl_tsl))
    return;

}

pthread_mutex_t *p_mutex_shared = NULL;
int shmid = -1;

void cross_process_mutex_first_init()
{
    key_t key_id = ftok(".", 1);
    //printf("???????????? key_id:%d\n", key_id);
    //shmid = shmget(key_id, sizeof(pthread_mutex_t), IPC_CREAT | IPC_EXCL | 0644);
    shmid = shmget(key_id, sizeof(pthread_mutex_t), IPC_CREAT );
    if (shmid < 0)
    {
        perror("shmget() create failed");
        return -1;
    }
    printf("shmget() create success, shmid is %d.\n", shmid);
 
    p_mutex_shared = shmat(shmid, NULL, 0);
    if (p_mutex_shared == (void *)-1)
    {
        perror("shmat() failed");
        // 删除共享内存，这里实际只是标记为删除，真正的删除动作在所有挂接的进程都脱接的状态下进行。
        // 同时不允许有新进程挂接到该共享内存上。
        shmctl(shmid, IPC_RMID, 0);
        return -2;
    }
    printf("shmat() success.\n");
 
    // 初始化共享内存段，存放互斥锁，该锁用于不同进程之间的线程互斥。
    pthread_mutexattr_t mutextattr;
    pthread_mutexattr_init(&mutextattr);
    // 设置互斥锁在进程之间共享
    pthread_mutexattr_setpshared(&mutextattr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(p_mutex_shared, &mutextattr);
}

void cross_process_mutex_init()
{
    key_t key_id = ftok(".", 1);
    shmid = shmget(key_id, 0, 0);
    if (shmid < 0)
    {
        perror("shmget() failed");
        return -1;
    }
    p_mutex_shared = shmat(shmid, NULL, 0);
    if (p_mutex_shared == NULL)
    {
        perror("shmat() failed");
        return -2;
    }
}


void cross_shamem_disconn()
{
    if (shmdt(p_mutex_shared) == -1)
    {
      printf("share mem disconnect");
    }
    p_mutex_shared = NULL;

}


/* This is the other side of the same channel. Since timeouts are handled by
   afl-fuzz simply killing the child, we can just wait until the pipe breaks. */

static void afl_wait_tsl(CPUState *cpu, int fd) {

  struct afl_tsl t;
  TranslationBlock *tb;

  while (1) {

    /* Broken pipe means it's time to return to the fork server routine. */

    if (read(fd, &t, sizeof(struct afl_tsl)) != sizeof(struct afl_tsl))
      break;

    tb = tb_htable_lookup(cpu, t.pc, t.cs_base, t.flags);
    if(!tb) {
      mmap_lock();
      tb_lock();
      if(!if_page_pc(t.pc) && if_premap_page(t.pc))
      {
        pthread_mutex_lock(p_mutex_shared);
        //printf("afl_wait_tsl lock:%x\n",t.pc);
        tb_gen_code(cpu, t.pc, t.cs_base, t.flags, 0);
        //printf("afl_wait_tsl unlock\n");
        pthread_mutex_unlock(p_mutex_shared);
      }
      mmap_unlock();
      tb_unlock();
       
    }

  }

  close(fd);

}


#ifdef MEM_MAPPING

int write_aflcmd(int cmd, USER_MODE_TIME *user_mode_time)  
{  
    const char *fifo_name_user = "./user_cpu_state";  
    int pipe_fd = -1;  
    int res = 0;  
    const int open_mode_user = O_WRONLY;  
  
    if(access(fifo_name_user, F_OK) == -1)  
    {  
        res = mkfifo(fifo_name_user, 0777);  
        if(res != 0)  
        { 
            fprintf(stderr, "Could not create fifo %s\n", fifo_name_user);  
            exit(EXIT_FAILURE);  
        }  
    } 

    pipe_fd = open(fifo_name_user, open_mode_user);    
    if(pipe_fd != -1)  
    { 
      int type = 2; 
      res = write(pipe_fd, &type, sizeof(int));  
      if(res == -1)  
      {  
        fprintf(stderr, "Write type on pipe\n");  
        exit(EXIT_FAILURE);  
      }
      res = write(pipe_fd, &cmd, sizeof(int));  
      if(res == -1)  
      {  
        fprintf(stderr, "Write error on pipe\n");  
        exit(EXIT_FAILURE);  
      }
      res = write(pipe_fd, user_mode_time, sizeof(USER_MODE_TIME));  
      if(res == -1)  
      {  
        fprintf(stderr, "Write error on pipe\n");  
        exit(EXIT_FAILURE);  
      }
      if(print_debug)
      {
        printf("write cmd ok:%x\n", cmd);  
      }
      close(pipe_fd);   
    }  
    else  
        exit(EXIT_FAILURE);  
  
    return 1;  
}  

target_ulong startTrace(target_ulong start, target_ulong end)
{
    afl_start_code = start;
    afl_end_code   = end;
    return 0;
}


#endif