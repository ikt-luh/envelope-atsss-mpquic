// queue.h
// very basic queue for Femto-QUIC - thanks ChatGPT

#ifndef QUEUE_H
#define QUEUE_H

#include <stdbool.h>
#include <semaphore.h>

typedef struct {
    char data_buffer[262144];  // data to be sent
    size_t data_len;  // length of data in data_buffer
    int path_id;  // multipath path ID
    void *additional_data;  // additional data to be sent (e.g., *picoquic_cnx_t)
    int send_mode;
} QueueElement;

// Define a node in the queue
typedef struct Node {
    QueueElement element;
    struct Node *next;
} Node;

// Define the queue itself
typedef struct {
    Node *front;
    Node *rear;
    int size;
    sem_t mutex;
} Queue;

// Function prototypes

// Initialize a queue
void initQueue(Queue *queue);

// Check if the queue is empty
bool isQueueEmpty(Queue *queue);

// Enqueue (add) an element to the queue
bool enqueue(Queue *queue, QueueElement element);

// Dequeue (remove) an element from the queue
bool dequeue(Queue *queue, QueueElement *element);

// Peek at the front element of the queue without removing it
bool peekQueue(Queue *queue, QueueElement *element);

// Get the size of the queue
int getQueueSize(Queue *queue);

// Clear the queue and free allocated memory
void clearQueue(Queue *queue);

#endif // QUEUE_H
