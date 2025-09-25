#pragma once

typedef struct {
	void **list;
	int64_t size;

	uint32_t head;
	uint32_t tail;

	pthread_mutex_t lock;
} Queue;

Queue queue_init(void) {
	int64_t initial_size = 64;
	Queue q = (Queue){
		.list = calloc(sizeof(void *), initial_size),
		.size = initial_size,
		.head = 0,
		.tail = 0
	};

	pthread_mutex_init(&q.lock, NULL);
	return q;
}

int64_t queue_size_unsafe(Queue *q) {
	return (int64_t)q->tail - (int64_t)q->head;
}

bool queue_push(Queue *q, void *data) {
	pthread_mutex_lock(&q->lock);

	int64_t cur_size = queue_size_unsafe(q);
	if (cur_size >= q->size) {
		pthread_mutex_unlock(&q->lock);
		return false;
	}

	uint32_t wrapped_tail = q->tail % q->size; 
	q->list[wrapped_tail] = data;
	q->tail += 1;

	pthread_mutex_unlock(&q->lock);
	return true;
}

void *queue_pop_unsafe(Queue *q) {
	int64_t cur_size = queue_size_unsafe(q);
	if (cur_size <= 0) {
		return NULL;
	}

	uint32_t wrapped_head = q->head % q->size; 
	void *data = q->list[wrapped_head];
	q->head += 1;
	return data;
}

void *queue_peek_unsafe(Queue *q) {
	int64_t cur_size = queue_size_unsafe(q);
	if (cur_size <= 0) {
		return NULL;
	}

	uint32_t wrapped_head = q->head % q->size; 
	void *data = q->list[wrapped_head];
	return data;
}

void *queue_pop(Queue *q) {
	pthread_mutex_lock(&q->lock);
	void *data = queue_pop_unsafe(q);
	pthread_mutex_unlock(&q->lock);
	return data;
}

void *queue_peek(Queue *q) {
	pthread_mutex_lock(&q->lock);
	void *data = queue_peek_unsafe(q);
	pthread_mutex_unlock(&q->lock);
	return data;
}

int64_t queue_size(Queue *q) {
	pthread_mutex_lock(&q->lock);
	int64_t cur_size = queue_size_unsafe(q);
	pthread_mutex_unlock(&q->lock);
	return cur_size;
}
