#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <threads.h>
#include <linux/limits.h>
#include <dirent.h>
#include <unistd.h>
#include <stddef.h>

#define SUCCESS 0

typedef struct directories_queue {
    struct directory* first;
    struct directory* last;
    struct directory* held_by_thread; /* Pointer to the last directory associated with a thread */
} directories_queue;

typedef struct directory {
    char path[PATH_MAX];
    long owner_thread_id;
    struct directory* next;
} directory;

typedef struct threads_queue {
    struct thread* first;
    struct thread* last;
    struct thread* holding_dir; /* Pointer to the last thread associated with a directory */
} threads_queue;

typedef struct thread {
    long id;
    struct thread* next;
} thread;



/****** GLOBAL VARIABLES ******/
char* search_term;
int num_of_threads;
int was_error = 0;
int num_of_ready_threads = 0;
directories_queue* queue;
threads_queue* waiting_queue;
thrd_t* threads;
mtx_t create_threads_lock;
mtx_t queue_lock;
mtx_t matching_files_lock;
mtx_t dec_num_of_threads_lock;
cnd_t all_threads_are_ready;
cnd_t start_searching;
cnd_t queue_not_empty;
int matching_files = 0;
int waiting_threads_num = 0;
int existing_threads;



/***** functions *****/
void thread_initialize();
void destroy_thread();
void add_thread_to_queue();
void add_directory_to_queue(directory* directory);
void directory_search(char* path);
void thread_search(long i);
int dequeue_directory(long id, char* path);
void* thread_func(void* i);
void create_threads();



void thread_initialize() {
    mtx_init(&create_threads_lock,mtx_plain);
    mtx_init(&queue_lock,mtx_plain);
    mtx_init(&matching_files_lock,mtx_plain);
    mtx_init(&dec_num_of_threads_lock,mtx_plain);
    cnd_init(&all_threads_are_ready);
    cnd_init(&start_searching);
    cnd_init(&queue_not_empty);
}

void destroy_thread() {
    mtx_destroy(&create_threads_lock);
    mtx_destroy(&queue_lock);
    mtx_destroy(&matching_files_lock);
    mtx_destroy(&dec_num_of_threads_lock);
    cnd_destroy(&all_threads_are_ready);
    cnd_destroy(&start_searching);
    cnd_destroy(&queue_not_empty);
}

void add_thread_to_queue(long id) {
    /* no directory available for searching => this thread needs to wait */
    thread* new_thread = malloc(sizeof(thread));
    if (new_thread == NULL) {
        perror("ERROR! malloc failed\n");
        was_error = 1;
        return;
    }
    new_thread->id = id;
    if (waiting_queue->first == NULL) {
        /* There is no other waiting threads */
        waiting_queue->first = new_thread;
        waiting_queue->holding_dir = NULL;
    }
    else 
        waiting_queue->last->next = new_thread;
    waiting_queue->last = new_thread;
}

void add_directory_to_queue(directory* directory) {
    if (queue->first == NULL)
        queue->first = directory;
    else 
        queue->last->next = directory;
    queue->last = directory;
    /* Assign a thread to the added directory */
    if (waiting_queue->holding_dir != waiting_queue->last && waiting_queue->holding_dir != NULL) {
        waiting_queue->holding_dir = waiting_queue->holding_dir->next;
        directory->owner_thread_id = waiting_queue->holding_dir->id;
    }
    else if (waiting_queue->holding_dir == NULL && waiting_queue->first != NULL) {
        waiting_queue->holding_dir = waiting_queue->first;
        directory->owner_thread_id = waiting_queue->holding_dir->id;
    }
}

void directory_search(char* path) {
    DIR* dir = opendir(path);
    struct dirent* directory_entry;
    char total_path[PATH_MAX];
    struct stat entry_stats;
    directory* new_directory;
    /* error while searching thread => print an error message to stderr and exit that thread */
    if (dir == NULL) {
        printf("Directory %s: Permission denied.\n", path);
        return;
    }
    while((directory_entry = readdir(dir)) != NULL) {
        strcpy(total_path, path);
        strcat(total_path, "/");
        strcat(total_path, directory_entry->d_name);
        if (stat(total_path, &entry_stats) != SUCCESS){ 
            /* file isn't a directory and the file name contains the search term */
            if (strstr(directory_entry->d_name, search_term) != NULL){
            	mtx_lock(&matching_files_lock);
            	matching_files++;
            	mtx_unlock(&matching_files_lock);
            	printf("%s\n", total_path);
            }
        }
        /* If the name in the dirent is "." OR ".." ignore it */
        else if ((strcmp(directory_entry->d_name, ".") == 0) || (strcmp(directory_entry->d_name, "..") == 0)) {
            continue;
        }
        else if (S_ISDIR(entry_stats.st_mode)) { 
            	new_directory = malloc(sizeof(directory));
            	if (new_directory == NULL) {
                	perror("ERROR! malloc failed\n");
                	was_error = 1;
                	return;
            	}
            	strcpy(new_directory->path, total_path);
            	new_directory->owner_thread_id = -1;
            	new_directory->next = NULL;
           		mtx_lock(&queue_lock);
            	add_directory_to_queue(new_directory);
            	cnd_broadcast(&queue_not_empty);
            	mtx_unlock(&queue_lock);
        }
        else if (strstr(directory_entry->d_name, search_term) != NULL) {
            mtx_lock(&matching_files_lock);
            matching_files++;
            mtx_unlock(&matching_files_lock);
            printf("%s\n", total_path);
        }
    }
    closedir(dir);
}

void thread_search(long i) {
    char path[PATH_MAX];
    int wait_flag;
    while(1) {
        wait_flag = 0;
        mtx_lock(&queue_lock);
        while(queue->first == NULL) {
            /* Wait for the queue becomes non-empty */
            if (!wait_flag) {
                wait_flag = 1;
                waiting_threads_num++;
            }
            /* all other searching threads are already waiting */
            if (waiting_threads_num == existing_threads) { 
                cnd_broadcast(&queue_not_empty);
                mtx_unlock(&queue_lock);
                thrd_exit(0);
            }
            cnd_wait(&queue_not_empty, &queue_lock);
        }
        /* the queue is not empty */
        if (wait_flag) { 
            wait_flag = 0;
            waiting_threads_num--;
        }
        /* the queue is not empty => first in the queue != NULL */
        if (queue->held_by_thread == NULL) {            /* there is no thread that holds any directory */
            queue->held_by_thread = queue->first;       /* The first directory to be taken */
            queue->held_by_thread->owner_thread_id = i; 
        }
        else if (queue->held_by_thread != queue->last ) { /* directory without owners */
            queue->held_by_thread->next->owner_thread_id = i;
            queue->held_by_thread = queue->held_by_thread->next;
        }
        if (dequeue_directory(i, path) != SUCCESS) {
            /* There is no a directory for this thread at the moment */
            add_thread_to_queue(i);
            wait_flag = 1;
            cnd_wait(&queue_not_empty, &queue_lock);
        }
        /* There is a directory for this thread */
        else { 
            mtx_unlock(&queue_lock);
            directory_search(path);
        }
    }
}

int dequeue_directory(long id, char* path) {
    /* queue is not empty */
    directory* prev = queue->first;
    directory* tmp = queue->first;
    thread* thread_prev;
    thread* thread_tmp;
    while (tmp != NULL) {
        /* searching directory who associated with this thread */
        if (tmp->owner_thread_id == id) {
            strcpy(path, tmp->path);
            prev->next = tmp->next;
            if (tmp == queue->first) {
                queue->first = tmp->next;
                queue->held_by_thread = NULL;
            }
            else if (queue->last->path == tmp->path || queue->held_by_thread->path == tmp->path)
                queue->held_by_thread = prev;
            break;
        }
        prev = tmp;
        tmp = tmp->next;
    }
    /* no associated directory for this thread */
    if (tmp == NULL) { 
        return -1;
    }
    /* dequeue this thread from waiting_queue */
    else {
        thread_prev = waiting_queue->first;
        thread_tmp = waiting_queue->first;
        while (thread_tmp != NULL) {
            if (thread_tmp->id == id) {
                if (thread_tmp == waiting_queue->holding_dir)
                    waiting_queue->holding_dir = waiting_queue->holding_dir->next;
                thread_prev->next = thread_tmp->next;
                if (thread_tmp == waiting_queue->first)
                    waiting_queue->first = thread_tmp->next;
                free(thread_tmp);
                break;
            }
            thread_prev = thread_tmp;
            thread_tmp = thread_tmp->next;
        }
        free(tmp);
        return SUCCESS;
    }
}

void* thread_func(void* i) {
    int id;
    mtx_lock(&create_threads_lock);
    num_of_ready_threads++;
    if(num_of_ready_threads == num_of_threads){
        cnd_broadcast(&all_threads_are_ready);
    }
    cnd_wait(&start_searching, &create_threads_lock);
    mtx_unlock(&create_threads_lock);
    id = (long) i;
    thread_search(id);
    return i;
}

void create_threads() {
    mtx_lock(&create_threads_lock);
    long i;
    int ret_val;
    for (i=0; i<num_of_threads; i++) {
        ret_val = thrd_create(&threads[i], (void *)thread_func, (void *)i);
        if (ret_val != SUCCESS) {
            perror("ERROR! pthread_create failed\n");
            exit(1);
        }
    }
    /* The main thread waiting for all searching threads to be ready */
    cnd_wait(&all_threads_are_ready, &create_threads_lock);
    /* The main thread signals searching threads to start searching */
    cnd_broadcast(&start_searching); 
    mtx_unlock(&create_threads_lock);  
}

/***** main ******/
int main(int argc, char** argv) {
    directory* root;
    int i;
    if (argc != 4) {
        perror("ERROR! Incorrect number of command line arguments\n");
		exit(1);
    }
    if (opendir(argv[1]) == NULL) {
        perror("ERROR! The directory can't be searched\n");
		exit(1);
    }
    root = malloc(sizeof(directory));
    queue = malloc(sizeof(directories_queue));
    waiting_queue = malloc(sizeof(threads_queue));
    if (root == NULL || queue == NULL || waiting_queue == NULL) {
        perror("ERROR! malloc failed\n");
		exit(1);
    }
    thread_initialize();    
    search_term = argv[2];
    num_of_threads = atoi(argv[3]); /* valid integer = greater than 0 */
    existing_threads = num_of_threads;
    strcpy(root->path, argv[1]);
    root->next = NULL;
    root->owner_thread_id = -1;
    queue->first = root;
    queue->last = root;
    queue->held_by_thread = NULL;
    threads = malloc(sizeof(pthread_t)*num_of_threads);
    if (threads == NULL) {
        perror("ERROR! malloc failed\n");
		exit(1);
    }
    create_threads();
    for (i=0; i<num_of_threads; i++) {
        thrd_join(threads[i], NULL);
    }
    printf("Done searching, found %d files\n", matching_files);
    destroy_thread();
    free(queue);
    free(waiting_queue);
    if (!was_error)
        exit(SUCCESS);
    exit(1);
}
