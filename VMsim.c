#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <semaphore.h>
#include <unistd.h>
#include <sched.h>
#include <sys/types.h>
#include <string.h>
#include <sys/time.h>

/* Virtual Pager */
typedef struct vpager_t {
	int p_size;
	int p_num;
	int id;
	FILE* source;
	void* stack;
	int active;
} vpager_t;

/* Memory Manager */
typedef struct mmanager_t {
	int f_size;
	int f_num;
	int* frames;
	int* counters;
	int* pid;
	int pagers_num;
	vpager_t** pagers;
} mmanager_t;

/* Page Request */
typedef struct prequest_t {
	int pid;
	int address;
	int page;
	int offset;
} prequest_t;

/* Memory Manager */
mmanager_t* init_mmanager(int S, int F, int n);
void manage(mmanager_t* m);
int find_frame(mmanager_t* m, int pid, int page);
int find_free(mmanager_t* m);
int find_lru(mmanager_t* m);
void set_frame(mmanager_t* m, int frame, int pid, int address);
int pagers_finished(mmanager_t* m);
void release_mmanager(mmanager_t* m);
int handle_fault(void* o);
void handle_request(mmanager_t* m);

/* Virtual Pager */
vpager_t** start_vpagers(int n, int S, int P);
vpager_t* init_vpager(int S, int P, int file_num);
int read_line(vpager_t* p);
void release_vpager(vpager_t* p);
int read_all(void* p);

/* Page request Handler */
void init_request();
int submit_request(vpager_t* p, int value);
void set_request(vpager_t* p, prequest_t* r, int value);
void clear_request();
void release_request();

/* shared variables */
/* storing the global request */
sem_t* mutex;
prequest_t* request;
/* storing the accessed frame */
sem_t* m_response;
int PHYSICAL_FRAME = -1;

int main(int argc, char** argv) {
	if (argc < 5) {
		printf("Vmsim requires 4 arguments, %d provided.\n", argc);
		return 0;
	}

	/* get argument values */
	int S = atoi(argv[1]),
		P = atoi(argv[2]),
		F = atoi(argv[3]),
		n = atoi(argv[4]);

    mutex = malloc(sizeof(sem_t));
    sem_init(mutex, 1, 1);

    m_response = malloc(sizeof(sem_t));
    sem_init(m_response, 1, 0);

    init_request();
	mmanager_t* m = init_mmanager(S, F, n);
	m->pagers = start_vpagers(n, S, P);

	/* blocking call to manage */
	manage(m);

	/* process cleanup */
	release_mmanager(m);
	release_request();
	free(mutex);
	free(m_response);

	return 0;
}

void init_request() {
	request = (prequest_t*)malloc(sizeof(prequest_t));
    request->pid = -1;
}

/* only sets the request if it's not set */
int submit_request(vpager_t* p, int value) {
	if (request->pid != -1) {
		return -1;
	}

	set_request(p, request, value);

	return 0;
}

/* sets the values of the provided request */
void set_request(vpager_t* p, prequest_t* r, int value) {
	r->pid = p->id;
	r->address = value;
	r->offset = value % p->p_size;
	r->page = value / p->p_size;
}

void clear_request() {
	request->pid = -1;
}

void release_request() {
	free(request);
}

mmanager_t* init_mmanager(int S, int F, int n) {
	mmanager_t* m = malloc(sizeof(mmanager_t));
	m->f_size = S;
	m->f_num = F;

	m->frames = malloc(m->f_num * sizeof(int));
	m->counters = malloc(m->f_num * sizeof(int));
	m->pid = malloc(m->f_num * sizeof(int));
	for (int i=0; i<m->f_num; i++) {
		m->frames[i] = -1;
		m->counters[i] = -1;
		m->pid[i] = -1;
	}
	m->pagers_num = n;

	return m;
}

void release_mmanager(mmanager_t* m) {
	for (int i = 0; i < m->pagers_num; i++) {
		release_vpager(m->pagers[i]);
	}
	free(m->pagers);

	free(m->frames);
	free(m->counters);
	free(m);
}

/* blocking loop to manage requests from pagers */
void manage(mmanager_t* m) {
	/* loop keeps mem manager thread alive while pagers are running */
	while (1) {
		sem_wait(mutex);
		int pid = request->pid;
		sem_post(mutex);

		if (pid != -1) {
			handle_request(m);
		}

		// if all pagers are finished, exit */
		if (pagers_finished(m) != -1) {
			break;
		}
	}
}

/* checks if all pagers have finished */
int pagers_finished(mmanager_t* m) {
	for (int i=0; i<m->pagers_num; i++) {
		if (m->pagers[i]->active == 0) {
			return -1;
		}
	}
	return 0;
}


void handle_request(mmanager_t* m) {
	sem_wait(mutex);
	// find an available frame in the memory manager
	int frame = find_frame(m, request->pid, request->page);

	if (frame == -1) {
		/* no frame triggers a page fault */
		void *stack = malloc(1024);
		stack += 1024;
		clone(handle_fault, stack, CLONE_VM, (void *)m);
	} else {
		set_frame(m, frame, request->pid, request->page);
		clear_request();
		sem_post(mutex);
	}
}

int handle_fault(void* o) {
	mmanager_t* m = (mmanager_t *)o;
	printf("[Process %d] accesses address %d (page number = %d, page offset = %d) not in main memory.\n", request->pid, request->address, request->page, request->offset);

	// find and set the first free frame
	int free_frame = find_free(m);
	if (free_frame == -1) {
		// replace the lru frame if there's no free frame
		free_frame = find_lru(m);
		printf("[Process %d] replaces a frame (frame number = %d) from the main memory.\n", request->pid, free_frame);
	} else {
		printf("[Process %d] finds a free frame in main memory (frame number = %d).\n", request->pid, free_frame);

	}

	printf("[Process %d] issues an I/O operation to swap in demanded page (page number = %d).\n", request->pid, request->page);
	usleep(1000);
	set_frame(m, free_frame, request->pid, request->page);
	printf("[Process %d] demanded page (page number = %d) has been swapped in main memory (frame number = %d).\n", request->pid, request->page, free_frame);
	clear_request();
	sem_post(mutex);
	return 0;
}

/* returns the index of the frame with the given page and pid */
int find_frame(mmanager_t* m, int pid, int page) {
	for (int i = 0; i < m->f_num; i++) {
		if (m->frames[i] == page && m->pid[i] == pid) {
			return i;
		}
	}

	return -1;
}

/* using the counter method of lru */
int find_lru(mmanager_t* m) {
	int min_i = 0;

	for (int i=0; i < m->f_num; i++) {
		if (m->counters[i] < m->counters[min_i]) {
			min_i = i;
		}
	}

	return min_i;
}

/* gets the index of the first unset frame */
int find_free(mmanager_t* m) {
	for (int i=0; i<m->f_num; i++) {
		if (m->frames[i] == -1) {
			return i;
		}
	}

	return -1;
}

/* sets the page, counter, and pid values of the frame index */
struct timeval cur_time;
void set_frame(mmanager_t* m, int frame, int pid, int page) {
	m->frames[frame] = page;
	gettimeofday(&cur_time, NULL);
	m->counters[frame] = cur_time.tv_usec;
	m->pid[frame] = pid;
	PHYSICAL_FRAME = frame;
	sem_post(m_response);
}

/* spins off a new vpager thread */
vpager_t** start_vpagers(int n, int S, int P) {
	vpager_t** pagers = malloc(n * sizeof(vpager_t*));
	for (int i = 0; i < n; i++) {
		pagers[i] = init_vpager(S, P, i+1);
		clone(read_all, pagers[i]->stack, CLONE_VM, (void*)pagers[i]);
	}
	return pagers;
}

vpager_t* init_vpager(int S, int P, int file_num) {
	vpager_t* p = malloc(sizeof(vpager_t));
	p->p_size = S;
	p->p_num = P;
	p->stack=(void *)malloc(4096);
	p->stack+=4096;
	p->id = file_num;
	p->active = 0;

	char f_name[16];
	sprintf(f_name,"trace_%d.txt", p->id);
	p->source = fopen(f_name, "r");

	return p;
}

void release_vpager(vpager_t* p) {
	free(p);
}

/* accesses all the addresses in the pager's trace file */
int read_all(void* o) {
    size_t len = 0;
	char* line;
	vpager_t* p = (vpager_t*)o;
	ssize_t read = getline(&line, &len, p->source);

	int success = 0;

	while (read != -1) {
		sem_wait(mutex);
		success = submit_request(p, atoi(line));
		sem_post(mutex);
		if (success != -1) {
			// wait for response
			sem_wait(m_response);
			prequest_t local;
			set_request(p, &local, atoi(line));
			printf("[Process %d] accesses address %d (page number = %d, page offset = %d) in main memory (frame number = %d).\n", p->id, local.address, local.page, local.offset, PHYSICAL_FRAME);
			read = getline(&line, &len, p->source);
		}
	}
	// process ends after all lines have been read
	printf("[Process %d] ends.\n", p->id);
	p->active = -1;

	return 0;
}

/* returns value on line if not EOF, -1 otherwise */
int read_line(vpager_t* p) {
	int value = -1;
	fscanf(p->source, "%d", &value);
	return value;
}
