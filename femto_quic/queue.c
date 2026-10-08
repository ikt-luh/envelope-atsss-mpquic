#include <stdio.h>
#include <stdlib.h>
#include "queue.h"

// Initialize the queue
void initQueue(Queue *queue) {
    queue->front = NULL;
    queue->rear = NULL;
    queue->size = 0;
    sem_init(&queue->mutex, 0, 1);
}

// Check if the queue is empty
bool isQueueEmpty(Queue *queue) {
    return queue->size == 0;
}

// Enqueue (add) an element to the queue
bool enqueue(Queue *queue, QueueElement element) {
    // Allocate memory for a new node
    Node *newNode = (Node *)malloc(sizeof(Node));
    if (!newNode) {
        return false; // Memory allocation failed
    }

    newNode->element = element;
    newNode->next = NULL;

    // If the queue is empty, both front and rear will point to the new node
    sem_wait(&queue->mutex);
    if (isQueueEmpty(queue)) {
        queue->front = newNode;
        queue->rear = newNode;
    } else {
        // Add the new node at the end of the queue and update the rear pointer
        queue->rear->next = newNode;
        queue->rear = newNode;
    }

    queue->size++;
    sem_post(&queue->mutex);
    return true;
}

// Dequeue (remove) an element from the queue
bool dequeue(Queue *queue, QueueElement *element) {
    sem_wait(&queue->mutex);
    if (isQueueEmpty(queue)) {
	sem_post(&queue->mutex);
        return false; // Queue is empty
    }

    // Store the front node
    Node *temp = queue->front;

    // Copy the data of the front element to the output parameter
    *element = temp->element;

    // Move the front pointer to the next node
    queue->front = queue->front->next;

    // If the queue becomes empty, update the rear pointer as well
    if (queue->front == NULL) {
        queue->rear = NULL;
    }

    // Free the old front node
    free(temp);

    queue->size--;
    sem_post(&queue->mutex);
    return true;
}

// Peek at the front element of the queue without removing it
bool peekQueue(Queue *queue, QueueElement *element) {
    if (isQueueEmpty(queue)) {
        return false; // Queue is empty
    }

    // Copy the front element's data to the output parameter
    *element = queue->front->element;
    return true;
}

// Get the size of the queue
int getQueueSize(Queue *queue) {
    return queue->size;
}

// Clear the queue and free all allocated memory
void clearQueue(Queue *queue) {
    sem_wait(&queue->mutex);
    QueueElement temp;
    while (!isQueueEmpty(queue)) {
        dequeue(queue, &temp);
    }
    sem_post(&queue->mutex);
}
