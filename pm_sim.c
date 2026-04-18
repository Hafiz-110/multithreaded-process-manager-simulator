#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>

// define constants
#define MAX_PROCESSES 64
#define MAX_CHILDREN  64

// define process_state structure
typedef enum {
    RUNNING,
    WAITING,
    ZOMBIE,
    TERMINATED
} process_state;

// define pcb structure
typedef struct {
    int pid;
    int ppid;
    process_state state;
    int exit_status;

    int children[MAX_CHILDREN];
    int child_count;

    int active;
} pcb;

// argument structure
typedef struct {
    int  thread_id;
    char filename[256];
} thread_arg;

// global process table
pcb process_table[MAX_PROCESSES];
int process_count = 0;
int next_pid      = 1;

// Synchronization
pthread_mutex_t table_lock;
pthread_cond_t  table_changed;
int change_count = 0;
char last_action[256];

// Helper: notify monitor (call while holding table_lock)
static void notify_monitor(const char *action) {
    strncpy(last_action, action, sizeof(last_action) - 1);
    last_action[sizeof(last_action) - 1] = '\0';
    change_count++;
    pthread_cond_broadcast(&table_changed);
}

// pm_init
void pm_init(void) {
    pthread_mutex_init(&table_lock, NULL);
    pthread_cond_init(&table_changed, NULL);

    memset(process_table, 0, sizeof(process_table));

    // initial process 
    process_table[0].pid         = 1;
    process_table[0].ppid        = 0;
    process_table[0].state       = RUNNING;
    process_table[0].exit_status = -1;
    process_table[0].child_count = 0;
    process_table[0].active      = 1;

    process_count = 1;
    next_pid      = 2;

    snprintf(last_action, sizeof(last_action), "Initial Process Table");
}

// Internal helpers (call with table_lock held)
static pcb *get_process(int pid) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (process_table[i].active && process_table[i].pid == pid)
            return &process_table[i];
    }
    return NULL;
}

static void add_child(pcb *parent, int child_pid) {
    if (parent->child_count < MAX_CHILDREN)
        parent->children[parent->child_count++] = child_pid;
}

static void remove_child(pcb *parent, int child_pid) {
    int idx = -1;
    for (int i = 0; i < parent->child_count; i++) {
        if (parent->children[i] == child_pid) { idx = i; break; }
    }
    if (idx == -1) return;
    for (int i = idx; i < parent->child_count - 1; i++)
        parent->children[i] = parent->children[i + 1];
    parent->child_count--;
}

// Called under table_lock.
static void kill_descendants(int pid) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (process_table[i].active && process_table[i].ppid == pid) {
            kill_descendants(process_table[i].pid); // depth-first
            process_table[i].state  = TERMINATED;
            process_table[i].active = 0;
        }
    }
}

// pm_fork
int pm_fork(int parent_pid, int thread_id) {
    pthread_mutex_lock(&table_lock);

    pcb *parent = get_process(parent_pid);
    if (parent == NULL) {
        pthread_mutex_unlock(&table_lock);
        return -1;
    }

    // find a free slot
    int index = -1;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (!process_table[i].active) { index = i; break; }
    }
    if (index == -1) {                       // table full
        pthread_mutex_unlock(&table_lock);
        return -1;
    }

    int child_pid = next_pid++;

    process_table[index].pid         = child_pid;
    process_table[index].ppid        = parent_pid;
    process_table[index].state       = RUNNING;
    process_table[index].exit_status = -1;
    process_table[index].child_count = 0;
    memset(process_table[index].children, 0, sizeof(process_table[index].children));
    process_table[index].active      = 1;

    if (index >= process_count) process_count = index + 1;

    add_child(parent, child_pid);

    char action[256];
    snprintf(action, sizeof(action), "Thread %d calls pm_fork %d", thread_id, parent_pid);
    notify_monitor(action);

    pthread_mutex_unlock(&table_lock);
    return child_pid;
}

// pm_exit
int pm_exit(int pid, int status, int thread_id) {
    pthread_mutex_lock(&table_lock);

    pcb *p = get_process(pid);
    if (p == NULL) {
        pthread_mutex_unlock(&table_lock);
        return -1;
    }

    p->exit_status = status;
    p->state       = ZOMBIE;
    p->active      = 1;          // stay active until parent reaps

    char action[256];
    snprintf(action, sizeof(action), "Thread %d calls pm_exit %d %d", thread_id, pid, status);
    notify_monitor(action);

    pthread_mutex_unlock(&table_lock);
    return 0;
}

// pm_wait
int pm_wait(int parent_pid, int child_pid, int thread_id) {
    pthread_mutex_lock(&table_lock);

    pcb *parent = get_process(parent_pid);
    if (parent == NULL) {
        pthread_mutex_unlock(&table_lock);
        return -1;
    }

    // when parent has no children
    if (parent->child_count == 0) {
        pthread_mutex_unlock(&table_lock);
        return -1;
    }

    if (child_pid != -1) {
        int found = 0;
        for (int i = 0; i < parent->child_count; i++) {
            if (parent->children[i] == child_pid) { found = 1; break; }
        }
        if (!found) {
            pthread_mutex_unlock(&table_lock);
            return -1;
        }
    }

    pcb *target_child = NULL;

    while (1) {
        target_child = NULL;

        if (child_pid == -1) {
            // wait for any child to become zombie
            for (int i = 0; i < parent->child_count; i++) {
                pcb *c = get_process(parent->children[i]);
                if (c && c->state == ZOMBIE) { target_child = c; break; }
            }
        } else {
            pcb *c = get_process(child_pid);
            if (c && c->state == ZOMBIE) target_child = c;
        }

        if (target_child != NULL) break;

        // block parent until something changes
        parent->state = WAITING;
        pthread_cond_wait(&table_changed, &table_lock);
        // re-read parent pointer — table might have changed
        parent = get_process(parent_pid);
        if (parent == NULL) {            // parent was killed while waiting
            pthread_mutex_unlock(&table_lock);
            return -1;
        }
    }

    parent->state = RUNNING;

    int status     = target_child->exit_status;
    int target_pid = target_child->pid;

    remove_child(parent, target_pid);

    target_child->state  = TERMINATED;
    target_child->active = 0;

    char action[256];
    snprintf(action, sizeof(action), "Thread %d calls pm_wait %d %d", thread_id, parent_pid, child_pid);
    notify_monitor(action);

    pthread_mutex_unlock(&table_lock);
    return status;
}

// pm_kill
int pm_kill(int pid, int thread_id) {
    pthread_mutex_lock(&table_lock);

    pcb *p = get_process(pid);
    if (p == NULL) {
        pthread_mutex_unlock(&table_lock);
        return -1;
    }

    // remove from parent's child list
    pcb *parent = get_process(p->ppid);
    if (parent != NULL)
        remove_child(parent, pid);

    kill_descendants(pid);

    p->state       = TERMINATED;
    p->active      = 0;
    p->exit_status = -1;

    char action[256];
    snprintf(action, sizeof(action), "Thread %d calls pm_kill %d", thread_id, pid);
    notify_monitor(action);

    pthread_mutex_unlock(&table_lock);
    return 0;
}

// pm_ps  (writes one snapshot to fp from a local copy of the table)
// convert the enum value to str
static const char *state_to_string(process_state s) {
    switch (s) {
        case RUNNING:    return "RUNNING";
        case WAITING:    return "WAITING";
        case ZOMBIE:     return "ZOMBIE";
        case TERMINATED: return "TERMINATED";
        default:         return "UNKNOWN";
    }
}

void pm_ps(FILE *fp, pcb *table) {
    fprintf(fp, "PID\t\tPPID\t\tSTATE\t\tEXIT_STATUS\n");
    fprintf(fp, "----------------------------------------------\n");
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (!table[i].active) continue;
        pcb *p = &table[i];
        fprintf(fp, "%d\t\t%d\t\t%s\t\t",
                p->pid, p->ppid, state_to_string(p->state));
        if (p->state == ZOMBIE)
            fprintf(fp, "%d\n", p->exit_status);
        else
            fprintf(fp, "-\n");
    }
}

// Monitor thread
void *monitor_thread(void *arg) {
    (void)arg;

    FILE *fp = fopen("snapshots.txt", "w");
    if (!fp) { perror("fopen snapshots.txt"); exit(1); }

    int last_seen = -1;   // force first print immediately

    while (1) {
        pthread_mutex_lock(&table_lock);

        // wait only when up-to-date
        while (last_seen == change_count)
            pthread_cond_wait(&table_changed, &table_lock);

        // take a consistent snapshot 
        char   action_copy[256];
        pcb    snapshot[MAX_PROCESSES];
        strncpy(action_copy, last_action, sizeof(action_copy) - 1);
        action_copy[sizeof(action_copy) - 1] = '\0';
        memcpy(snapshot, process_table, sizeof(snapshot));

        last_seen = change_count;

        pthread_mutex_unlock(&table_lock);

        // write outside the lock
        fprintf(fp, "%s\n", action_copy);
        pm_ps(fp, snapshot);
        fprintf(fp, "\n");
        fflush(fp);
    }

    fclose(fp);
    return NULL;
}

// Worker thread
void *worker_thread(void *arg) {
    thread_arg *t = (thread_arg *)arg;

    FILE *fp = fopen(t->filename, "r");
    if (!fp) { perror("fopen script"); return NULL; }

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        // strip trailing newline
        line[strcspn(line, "\n")] = '\0';
        if (line[0] == '\0') continue;        // skip blank lines

        if (strncmp(line, "fork", 4) == 0) {
            int p;
            sscanf(line, "fork %d", &p);
            pm_fork(p, t->thread_id);

        } else if (strncmp(line, "exit", 4) == 0) {
            int pid, status;
            sscanf(line, "exit %d %d", &pid, &status);
            pm_exit(pid, status, t->thread_id);

        } else if (strncmp(line, "wait", 4) == 0) {
            int p, c;
            sscanf(line, "wait %d %d", &p, &c);
            pm_wait(p, c, t->thread_id);

        } else if (strncmp(line, "kill", 4) == 0) {
            int pid;
            sscanf(line, "kill %d", &pid);
            pm_kill(pid, t->thread_id);

        } else if (strncmp(line, "sleep", 5) == 0) {
            int ms;
            sscanf(line, "sleep %d", &ms);
            usleep((unsigned int)(ms * 1000));
        }
    }

    fclose(fp);
    return NULL;
}

// main
int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <thread_file1> [thread_file2 ...]\n", argv[0]);
        return 1;
    }

    pm_init();

    int num_threads = argc - 1;
    pthread_t  *workers = malloc((size_t)num_threads * sizeof(pthread_t));
    thread_arg *args    = malloc((size_t)num_threads * sizeof(thread_arg));
    if (!workers || !args) { perror("malloc"); return 1; }

    // start monitor first then signal the initial state
    pthread_t monitor;
    pthread_create(&monitor, NULL, monitor_thread, NULL);

    // notify monitor about the initial process table
    pthread_mutex_lock(&table_lock);
    notify_monitor("Initial Process Table");
    pthread_mutex_unlock(&table_lock);

    // start worker threads
    for (int i = 0; i < num_threads; i++) {
        args[i].thread_id = i;
        strncpy(args[i].filename, argv[i + 1], sizeof(args[i].filename) - 1);
        args[i].filename[sizeof(args[i].filename) - 1] = '\0';
        pthread_create(&workers[i], NULL, worker_thread, &args[i]);
    }

    // wait for all threads to finish
    for (int i = 0; i < num_threads; i++)
        pthread_join(workers[i], NULL);

    // give the monitor a moment to flush the last snapshot
    usleep(50000);

    pthread_cancel(monitor);
    pthread_join(monitor, NULL);

    free(workers);
    free(args);
    return 0;
}
