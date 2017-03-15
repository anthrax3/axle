#include "pipe.h"
#include <kernel/util/multitasking/tasks/task.h>
#include <std/memory.h>
#include <std/std.h>
#include <kernel/util/vfs/fs.h>

static void pipe_create(pipe_t** read, pipe_t** write) {
	task_t* current = task_with_pid(getpid());

	pipe_t* r = (pipe_t*)kmalloc(sizeof(pipe_t));
	memset(r, 0, sizeof(pipe_t));
	r->dir = READ;
	r->fd = current->fd_max++;
	r->pids = array_m_create(32);
	array_m_insert(r->pids, getpid());

	pipe_t* w = (pipe_t*)kmalloc(sizeof(pipe_t));
	memset(w, 0, sizeof(pipe_t));
	w->dir = WRITE;
	w->fd = current->fd_max++;
	w->pids = array_m_create(32);
	array_m_insert(w->pids, getpid());

	//read and write pipe share the same buffer
	w->cb = kmalloc(sizeof(circular_buffer));
	cb_init(w->cb, 64, 1);
	r->cb = w->cb;

	*read = r;
	*write = w;
}

static void pipe_destroy(pipe_t* pipe) {
	kfree(pipe);
}

int pipe(int pipefd[2]) {
	task_t* current = task_with_pid(getpid());
	
	//create read and write pipes
	pipe_t* read = NULL;
	pipe_t* write = NULL;
	pipe_create(&read, &write);

	//then, assign file descriptors of new pipes to input array
	pipefd[0] = read->fd;
	pipefd[1] = write->fd;

	if (current->pipes->size + 1 >= current->pipes->max_size) {
		ASSERT(0, "%s[%d] ran out of pipes!", current->name, current->id);
	}

	array_m_insert(current->pipes, read);
	array_m_insert(current->pipes, write);
}

static pipe_t* find_pipe(int fd) {
	task_t* current = task_with_pid(getpid());
	pipe_t* pipe = NULL;
	for (int i = 0; i < current->pipes->size; i++) {
		pipe_t* tmp = array_m_lookup(current->pipes, i);
		if (tmp->fd == fd) {
			//found the pipe we're looking for!
			pipe = tmp;
			break;
		}
	}
	return pipe;
}

int pipe_read(int fd, char* buf, int count) {

	task_t* current = task_with_pid(getpid());

	//ensure fd is a valid pipe
	pipe_t* pipe = find_pipe(fd);
	//did we find it?
	if (!pipe) {
		printf_err("pipe_read: fd %d was not valid", fd);
		return -1;
	}

	if (pipe->dir != READ) {
		printf_err("pipe_read: fd %d was not a read pipe", fd);
		return -1;
	}

	int i = 0;
	for (; i < count; i++) {
		//check if we're out of items to read 
		if (pipe->cb->count == 0) {
			break;
		}

		cb_pop_front(pipe->cb, &(buf[i]));

		//check if we hit EOF
		if (buf[i] == EOF) {
			i--;
			break;
		}
	}
	buf[i] = '\0';
	return i;
}

int pipe_write(int fd, char* buf, int count) {
	task_t* current = task_with_pid(getpid());

	//ensure fd is a valid pipe
	pipe_t* pipe = find_pipe(fd);
	//did we find it?
	if (!pipe) {
		printf_err("pipe_write: fd %d was not valid", fd);
		return -1;
	}

	//correct direction?
	if (pipe->dir != WRITE) {
		printf_err("pipe_write: fd %d was not a write pipe", fd);
		return -1;
	}

	int i = 0;
	for (; i < count; i++) {
		if (pipe->cb->count == pipe->cb->capacity) {
			printf_err("pipe_write: pipe was full");
			break;
		}
		cb_push_back(pipe->cb, &(buf[i]));
	}

	return i;
}

int pipe_close(int fd) {
	pipe_t* pipe = find_pipe(fd);
	if (!pipe) {
		printf_err("pipe_close() fd %d was not valid", fd);
		return -1;
	}

	//if the number of tasks referencing this pipe end is exactly 1,
	//then after removing this pipe from that tasks the pipe will be 
	//ready to be destroyed.
	//if this is a write pipe and this is the only task referencing it,
	//then we should write EOF
	//we need to write EOF *before* removing the pipe from the tasks's
	//list of pipes because pipe_write calls pipe_find to see if the pipe is valid,
	//which check's the process's list of pipes
	task_t* current = task_with_pid(getpid());
	if (pipe->dir == WRITE && current->pipes->size == 1) {
		//when closing write end,
		//write EOF to buffer
		char eof = EOF;
		pipe_write(fd, &eof, 1);
	}

	//remove current PID from list of PIDs referencing this pipe end
	int idx = array_m_index(pipe->pids, getpid());
	if (idx == ARR_NOT_FOUND) {
		printf_err("pipe_close() on pipe not owned in proc");
		return;
	}
	array_m_remove(pipe->pids, idx);

	//remove this pipe from process's list of pipes
	idx = array_m_index(current->pipes, pipe);
	if (idx == ARR_NOT_FOUND) {
		printf_err("pipe_close() on pipe not present in proc list");
		return;
	}
	array_m_remove(current->pipes, idx);
	
	//if there are more processes referencing this pipe, 
	//quit early
	if (pipe->pids->size) {
		return;
	}

	//if no PIDs are referencing this pipe end, 
	//tear it down
	
	if (pipe->dir == READ) {
		//when closing a read end where there are no other processes
		//with a reference to the pipe, it's safe to destroy backing pipe resources
		cb_free(pipe->cb);
		kfree(pipe->cb);
	}
	pipe_destroy(pipe);
}

