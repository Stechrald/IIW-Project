#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <string.h>
#include <pthread.h>
#include <sys/sendfile.h>
#include <sys/time.h>

#define MAX_THREAD 2
#define PORT 80
#define MAX_BUF 1024
#define SERVER_STRING "Server: webServer/1.0.0\r\n"
#define LOG_INFO 44
#define SYSTEM_ERR 42
#define NOT_FOUND 404
#define INTERNAL_ERROR 500
#define NOT_IMPLEMENTED 501

void logger(int type_info, char *str1, char *str2, int i);

// structure containing thread informations
struct thread_struct{
	pthread_t tid; // thread id
	int fd; // managed connection file descriptor
};

pthread_cond_t cond_queue = PTHREAD_COND_INITIALIZER; // conditon for queue, static initialization
pthread_mutex_t queue_mtx = PTHREAD_MUTEX_INITIALIZER; // queue mutex, static initialization

struct thread_struct *threads; // threads array

int *queue;
int i = 0; // number of working threads 
int put, get; // head and tail of the queue


/* This function calculates the correct time, formatted for logging routine */

char *now(void)
{
	char *buff;
	time_t ticks;
	
	buff = malloc(MAX_BUF*2);
	if (buff == NULL){
		logger(SYSTEM_ERR, "Server", "Error in allocation time buffer", (int)getpid());
		exit(EXIT_FAILURE);
	}
	
	// read time using system call "time"
	ticks = time(NULL); 
    // write time in buffer
    sprintf(buff, "%.24s", ctime(&ticks));

	return buff;
}

/* This function implements the logging service and writes into a log file the most important operations:
 * - LOG_INFO: all correct operations
 * - NOT_FOUND: an operation that has requested a not present file
 * - NOT_IMPLEMENTED: an operation that has requested a non supported method
 * - INTERNAL_ERROR: an operation that has generated an internal error 
 * - OTHER: other internal errors that not concern client requests */

void logger(int type_info, char *str1, char *str2, int i){
	char log_buf[MAX_BUF*2];
	int fd;
	
	fd = open("/home/christian/log.txt", O_WRONLY|O_CREAT|O_APPEND, 0664);
	if (fd == -1){
		perror("Error in open");
		return;
	}
	
	if (type_info == LOG_INFO){
		sprintf(log_buf, "INFO: %s - %s[PID: %d] - %s\n", now(), str1, i, str2);
	}
	else if (type_info == NOT_FOUND){
		sprintf(log_buf, "NOT FOUND: %s - %s[PID: %d] - %s\n", now(), str1, i, str2);
	}
	else if (type_info == NOT_IMPLEMENTED){
		sprintf(log_buf, "NOT IMPLEMENTED: %s - %s[PID: %d] - %s\n", now(), str1, i, str2);
	}
	else if (type_info == INTERNAL_ERROR){
		sprintf(log_buf, "NOT FOUND: %s - %s[PID: %d] - %s\n", now(), str1, i, str2);
	}
	else{
		sprintf(log_buf, "SYSTEM ERROR: %s - %s[PID: %d] - %s\n", now(), str1, i, str2);
	}
	
	// write into log file
	if (write(fd, log_buf, strlen(log_buf)) != strlen(log_buf)){
		perror("Error in write");
		return;
	}
}


// TODO read function in another file
ssize_t readn(int fd, void *buf, size_t n)
{
  size_t  nleft;
  ssize_t nread;
  char *ptr;

  ptr = buf;
  nleft = n;
  while (*(ptr-1) != '\n') { 
    if ((nread = read(fd, ptr, nleft)) < 0) {
      if (errno == EINTR)
        nread = 0;
      // if timeout happens while socket is reading (socket is blocking)  
      if (errno == EWOULDBLOCK){
		return -1; 
	  }
      else
        return(-1);
    }
    // if client closes its socket 
    else if (nread == 0)
      break;	/* EOF */

    nleft -= nread;
    ptr += nread;
  }
  return(n-nleft);	/* returns a positive value (>= 0) */
}
// end read function


/* This function initializes the most important resources of server,
 * like thread structure or queue for connection management. 
 * Note that synchronization resources as mutex and condition are initialized in a static way */

void initialize_resource(void){

	// threads array initialization
	threads = malloc(sizeof(struct thread_struct)*MAX_THREAD);
	if (threads == NULL){
		logger(SYSTEM_ERR, SERVER_STRING, "Error in allocation memory for thread structure", (int)getpid());
		exit(EXIT_FAILURE);
	}
	// end threads array initialization


	// queue initalization
	queue = malloc(sizeof(int)*MAX_THREAD);
	if (queue == NULL){
		logger(SYSTEM_ERR, "Server", "Error in allocation memory for queue", (int)getpid());
		exit(EXIT_FAILURE);
	}
	
	put = get = 0;
	// end queue initialization
}

/* This function locks a mutex to protect an area of memory shared between threads */
void mutex_lock(pthread_mutex_t *mutex){
	if (pthread_mutex_lock(mutex) != 0){
		logger(SYSTEM_ERR, "Server", "Error in lock mutex semaphore", (int)getpid());
		exit(EXIT_FAILURE);
	}
}

/* This function releases a mutex */
void mutex_unlock(pthread_mutex_t *mutex){
	if (pthread_mutex_unlock(mutex) != 0){
		logger(SYSTEM_ERR, "Server", "Error in unlock mutex semaphore", (int)getpid());
		exit(EXIT_FAILURE);
	}
}

/* This function parses the client request; 
 * it finds requested method and file path*/

int parse_request(char *req, char *path, char *type){
	int i = 0;
	int j = 0;
	
	// finds requested method
	while(req[i] != ' '){
		type[j] = req[i];
		++i;
		++j;
	}
	
	j = 0;
	i = i+1;
	// finds requested file path 
	while(req[i] != ' '){
		path[j] = req[i];
		++i;
		++j;
	}
	path[j] = '\0';
	return j;
}

/* This function sends the 200 success code to client,
 * because its request was successfully fulfilled. Sends header to client */

void header_successful(int connfd, int i, ssize_t size){
	char buf[MAX_BUF];

	strcpy(buf, "HTTP/1.1 200 OK\r\n");
	send(connfd, buf, strlen(buf), 0);
	strcpy(buf, SERVER_STRING);
	send(connfd, buf, strlen(buf), 0);
	// Content_Lenght FUNDAMENTAL FOR PERSISTENCY IMPLEMENTATION AND CLIENT LOCK AVOIDANCE DURING READING ROUTINE
	sprintf(buf, "Content-Length: %d\r\n", (int)size); // uses sprintf to insert size param
	send(connfd, buf, strlen(buf), 0);
	if (i == 1){
		strcpy(buf, "Content-Type: image/jpg\r\n");
		send(connfd, buf, strlen(buf), 0);
	}
	else{
		strcpy(buf, "Content-Type: text/html\r\n");
		send(connfd, buf, strlen(buf), 0);
	}
	strcpy(buf, "Connection: keep-alive\r\n");
	send(connfd, buf, strlen(buf), 0);
	strcpy(buf, "\r\n");
	send(connfd, buf, strlen(buf), 0);
}

/* This function sends only the header to the client,
 * in response to a HEAD request */

void head_request(int connfd){
	char buf[MAX_BUF];

	strcpy(buf, "HTTP/1.1 200 OK\r\n");
	send(connfd, buf, strlen(buf), 0);
	strcpy(buf, SERVER_STRING);
	send(connfd, buf, strlen(buf), 0);
	strcpy(buf, "Connection: keep-alive\r\n");
	send(connfd, buf, strlen(buf), 0);
	strcpy(buf, "\r\n");
	send(connfd, buf, strlen(buf), 0);
}

/* This function sends the 404 error code to the client,
 * because requested file was not found */

void not_found(int connfd){
	char buf[MAX_BUF];

	strcpy(buf, "HTTP/1.1 404 \r\n");
	send(connfd, buf, strlen(buf), 0);
	strcpy(buf, SERVER_STRING);
	send(connfd, buf, strlen(buf), 0);
	strcpy(buf, "Content-Type: text/html\r\n");
	send(connfd, buf, strlen(buf), 0);
	strcpy(buf, "\r\n");
	send(connfd, buf, strlen(buf), 0);

	strcpy(buf, "<HTML><TITLE>Not Found</TITLE>\r\n");
	send(connfd, buf, strlen(buf), 0);
	strcpy(buf, "<BODY><P>The server could not fulfill\r\n");
	send(connfd, buf, strlen(buf), 0);
	strcpy(buf, "your request because the resource specified\r\n");
	send(connfd, buf, strlen(buf), 0);
	strcpy(buf, "is unavailable or nonexistent.\r\n");
	send(connfd, buf, strlen(buf), 0);
	strcpy(buf, "</BODY></HTML>\r\n");
	send(connfd, buf, strlen(buf), 0);
}

/* This function sends the 500 error code to the client, 
 * because an internal error occurred */

void internal_error(int connfd){
	char buf[MAX_BUF];

	strcpy(buf, "HTTP/1.1 500 \r\n");
	send(connfd, buf, strlen(buf), 0);
	strcpy(buf, SERVER_STRING);
	send(connfd, buf, strlen(buf), 0);
	strcpy(buf, "Content-Type: text/html\r\n");
	send(connfd, buf, strlen(buf), 0);
	strcpy(buf, "\r\n");
	send(connfd, buf, strlen(buf), 0);

	strcpy(buf, "<HTML><TITLE>Internal Server Error</TITLE>\r\n");
	send(connfd, buf, strlen(buf), 0);
	strcpy(buf, "<BODY><P>An internal server error occurred!</P>\r\n");
	send(connfd, buf, strlen(buf), 0);
	strcpy(buf, "</BODY></HTML>\r\n");
	send(connfd, buf, strlen(buf), 0);
}

/* This function sends the 501 error code to the client, 
 * because it has requested a method that is not implemented in the server */

void not_implemented(int connfd){
	char buf[MAX_BUF];

	strcpy(buf, "HTTP/1.1 501 \r\n");
	send(connfd, buf, strlen(buf), 0);
	strcpy(buf, SERVER_STRING);
	send(connfd, buf, strlen(buf), 0);
	strcpy(buf, "Content-Type: text/html\r\n");
	send(connfd, buf, strlen(buf), 0);
	strcpy(buf, "\r\n");
	send(connfd, buf, strlen(buf), 0);

	strcpy(buf, "<HTML><TITLE>Not Implemented</TITLE>\r\n");
	send(connfd, buf, strlen(buf), 0);
	strcpy(buf, "<BODY><P>This method is not supported by the server</P>\r\n");
	send(connfd, buf, strlen(buf), 0);
	strcpy(buf, "</BODY></HTML>\r\n");
	send(connfd, buf, strlen(buf), 0);
}

/* This function sends the requested file to the client using "sendfile" procedure */

void send_request_file(int connfd, int fd, char *path, pthread_t tid){
	struct stat *buf;
	int rc;	
	
	// memory allocation for stat structure
	buf = (struct stat *)malloc(sizeof(struct stat));
	if (buf == NULL){
		logger(INTERNAL_ERROR, "Server", "Error in allocation memory for file statistic structure", (int)getpid());
		internal_error(connfd);
		return;
	}
			
	// takes informations from the file
	if (fstat(fd, buf) == -1){
		logger(INTERNAL_ERROR, "Server", "Error in fstat function for obtain file statistic", (int)getpid());
		internal_error(connfd);
		return;
	}
		
	int i;
	if (strlen(path) > 1){
		i = 1;
	}
	
	else{
		i = 0;
	}
	// sends HTTP header to the client
	header_successful(connfd, i, buf->st_size);		
	// write the file in the socket
	rc = sendfile(connfd, fd, NULL, buf->st_size);
	printf("Served by %u: ", (unsigned int)tid);

	// if errors occured during write procedure
	if (rc == -1){
		logger(INTERNAL_ERROR, "Server", "Error in write file to socket", (int)getpid());
		internal_error(connfd);
		return;		
	}

	// incomplete transfer
	if (rc != buf->st_size){
		perror("Incomplete transfer");
		return;
	}
	
	logger(LOG_INFO, "Server", "Successful send file", (int)getpid());	
}

/* This function parses and processes the request send by the client to check the right method to call */
void web_request(int connfd, pthread_t tid){
	int fd;
	ssize_t nread;
	char req[MAX_BUF];
	char path[MAX_BUF];
	int parse_value;
	char type[4];
	
	// TODO Modify the image with image magick and control cache
	while(1){
		
		// clean path buffer
		memset((void *)&path, 0, sizeof(path));
		nread = readn(connfd, req, MAX_BUF);
		
		if (nread == -1){
			perror("Error in read\n");
			return;
		}
		
		if (nread == 0){
			printf("Client close its connection\n");
			return;
		}
		
		// parses client request
		parse_value = parse_request(req, path, type);
		printf("HTTP Method: %s\n", type);
		printf("File requested: %s\n", path);
		
		// verifies that the request is a GET request
		if (strcmp(type, "GET") == 0){
			if (parse_value == 1){
				// the first page showed to user
				fd = open("/home/christian/index.html", O_RDONLY);
				if (fd == -1){
					logger(NOT_FOUND, "Server", "404 error code, file not found", (int)getpid());
					not_found(connfd);
					return;
				}
			}	
			else{
				fd = open(path, O_RDONLY);
				if (fd == -1){
					logger(NOT_FOUND, "Server", "404 error code, file not found", (int)getpid());
					not_found(connfd);
					return;
				}
			}
		}
		
		// verifies that the request is a HEAD request
		else if (strcmp(type, "HEAD") == 0){
			// send only the header to client
			head_request(connfd);
			logger(LOG_INFO, "Server", "Header send for HEAD method", (int)getpid());
			continue;
		}
		
		// otherwise sends an error to the client; other methods are not implemented
		else{
			// sends a server error to the client
			logger(NOT_IMPLEMENTED, "Server", "501 error code, method not implemented", (int)getpid());
			not_implemented(connfd);
			return;
		}
		
		// sends file to the client
		send_request_file(connfd, fd, path, tid);
		
		if (close(fd) == -1){
			logger(SYSTEM_ERR, "Server", "Close file failed", (int)getpid());
			pthread_exit(NULL);
		}
	}
}

/* This function is the starting point for newly generated threads (created by the main thread)
 * here a thread takes its first connection socket if queue is not empty; 
 * otherwise it waits for a condition update (queue not empty) from the main thread) */
void *handle_conn(void *p){
	// argument passed by pthread_create
	struct thread_struct *thread = (struct thread_struct *)p;

	while(1){
		// mutual exclusion in queue (protected queue)
		mutex_lock(&queue_mtx);
		while (get == put){
			fprintf(stderr, "Sleep\n");
			// empty queue, waits for condition update
			if (pthread_cond_wait(&cond_queue, &queue_mtx) != 0){
				logger(SYSTEM_ERR, "Server", "Error in wait condition for empty queue", (int)getpid());
				pthread_exit(NULL);
			}
		}
		printf("Wake up thread %u\n", (unsigned int)pthread_self());
		thread->fd = queue[get]; // takes the first connection socket on the list
		if (++get == MAX_THREAD){
			get = 0;
		}
		++i; // busy thread 	
		mutex_unlock(&queue_mtx);

		// TODO work thread
		fprintf(stderr, "Start work\n");
		web_request(thread->fd, thread->tid);
		fprintf(stderr, "End work\n");		
		
		printf("Close\n");
		if(close(thread->fd) == -1){
			logger(SYSTEM_ERR, "Server", "Close socket failed", (int)getpid());
			pthread_exit(NULL);
		}
		
		// protected thread counter
		mutex_lock(&queue_mtx);
		--i; // thread is now allowed to work
		mutex_unlock(&queue_mtx);

		// signal sent to main thread: thread aviable for new connection
		if (pthread_cond_signal(&cond_queue) != 0){
			logger(SYSTEM_ERR, "Server", "Error in signal condition to main thread", (int)getpid());
            pthread_exit(NULL);
        }
	}
}

/* This function creates a pool of MAX_THREAD threads */
void make_threads(void){
	int j;

	for (j=0; j<MAX_THREAD; ++j){
		// creates a new thread
		if (pthread_create(&(threads[j].tid), NULL, handle_conn, &(threads[j])) != 0){
			logger(SYSTEM_ERR, "Server", "Error while create threads", (int)getpid());
			exit(EXIT_FAILURE);
		}
	}
}

int start_server(void){
	int sockfd;
	struct sockaddr_in addr;

	// creates a TCP socket
	sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd == -1){
		logger(SYSTEM_ERR, "Server", "Error in open socket", (int)getpid());
		return EXIT_FAILURE;
	}

	// clears sockaddr_in memory
	memset((void *)&addr, 0, sizeof(addr));

	addr.sin_family = AF_INET;
	addr.sin_port = htons(PORT);
	addr.sin_addr.s_addr = htonl(INADDR_ANY); // accepts connection from any IP address

	if (bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)) == -1){
		logger(SYSTEM_ERR, "Server", "Error in bind socket", (int)getpid());
		return EXIT_FAILURE;
	}

	// listen procedure in main socket (listen socket)
	if (listen(sockfd, 0) == -1){
		logger(SYSTEM_ERR, "Server", "Error in listen socket", (int)getpid());
		return EXIT_FAILURE;
	}
	
	return sockfd;
}

int main(void)
{
	int sockfd, connfd;
	
	sockfd = start_server();
	logger(LOG_INFO, "Start Server", "Work", (int)getpid());

	// initializes all resources
	initialize_resource();
	// creates pool of threads
	make_threads();

	// manages request
	for (;;){
		if ((connfd = accept(sockfd, (struct sockaddr *)NULL, NULL)) == -1){
			logger(SYSTEM_ERR, "Server", "Error in accept socket", (int)getpid());
			return EXIT_FAILURE;
		}
		
		
		// sets a timeval structure to manage socket block
		struct timeval tv;
		tv.tv_sec = 20; 
		tv.tv_usec = 0;
		// sets socket option into read procedure
		if (setsockopt(connfd, SOL_SOCKET, SO_RCVTIMEO,(char *)&tv,sizeof(struct timeval)) < 0){
			logger(SYSTEM_ERR, "Server", "Error in set timeout for socket", (int)getpid());
			return EXIT_FAILURE;
		}

		mutex_lock(&queue_mtx); // protected queue
		
		while (i == MAX_THREAD){
			fprintf(stderr, "Busy server...\n");
			if (pthread_cond_wait(&cond_queue, &queue_mtx)){
				logger(SYSTEM_ERR, "Server", "Error in wait condition for busy queue", (int)getpid());
				return EXIT_FAILURE;
			}
		}
		queue[put] = connfd;
		if (++put == MAX_THREAD){
			put = 0;
		}
		if(pthread_cond_signal(&cond_queue) != 0){
			logger(SYSTEM_ERR, "Server", "Error in signal condition", (int)getpid());
			return EXIT_FAILURE;
		}
		mutex_unlock(&queue_mtx);
	}
}
