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
#define MAX_BUF 4096

// struct that contains many information of thread
struct thread_struct{
	pthread_t tid; // thread id
	int fd; // connfd
};


pthread_cond_t cond_queue = PTHREAD_COND_INITIALIZER; // conditon for queue
pthread_mutex_t queue_mtx = PTHREAD_MUTEX_INITIALIZER; // queue mutex

struct thread_struct *threads; // threads array

int *queue;
int i = 0; // number of thread at work
int put, get;

// TODO read function in another file
ssize_t readn(int fd, void *buf, size_t n)
{
  size_t  nleft;
  ssize_t nread;
  char *ptr;

  ptr = buf;
  nleft = n;
  while (nleft > 0) {
    if ((nread = read(fd, ptr, nleft)) < 0) {
      if (errno == EINTR)
        nread = 0;
      // if timeout for socket in read (socket is blocking)  
      if (errno == EWOULDBLOCK){
		break; 
	  }
      else
        return(-1);
    }
    // if client close its socket 
    else if (nread == 0)
      break;	/* EOF */

    nleft -= nread;
    ptr += nread;
  }
  return(n-nleft);	/* restituisce >= 0 */
}
// end read function

void initialize_resource(void){

	// threads array initializzation
	threads = malloc(sizeof(struct thread_struct)*MAX_THREAD);
	if (threads == NULL){
		perror("Error in malloc");
		exit(EXIT_FAILURE);
	}
	// end threads array initializzation

	// queue initalization
	queue = malloc(sizeof(int)*MAX_THREAD);
	if (queue == NULL){
		perror("Error in malloc");
		exit(EXIT_FAILURE);
	}
	
	put = get = 0;
	// end queue initialization
}

/* This function locks a mutex for protect an area of memory shared between threads */
void mutex_lock(pthread_mutex_t *mutex){
	if (pthread_mutex_lock(mutex) != 0){
		perror("Error in lock mutex");
		exit(EXIT_FAILURE);
	}
}

/* This function release a mutex */
void mutex_unlock(pthread_mutex_t *mutex){
	if (pthread_mutex_unlock(mutex) != 0){
		perror("Error in unlock mutex");
		exit(EXIT_FAILURE);
	}
}

void web_request(int connfd){
	int rc, fd;
	ssize_t nread;
	struct stat *buf;
	char req[MAX_BUF];
	
	// TODO Parsing of client's request and send the correct file
	// TODO Modify the image with image magick and control cache
	// TODO persistance of HTTP 1.1 (insert cycle for(;;))
	nread = readn(connfd, req, MAX_BUF);
	if (nread == -1){
		perror("Error in read\n");
		return;
	}
	
	printf("ciao\n");		
	if (req[3] == ' '){
		// first file html to open that contains a list of image
		fd = open("/home/christian/index.html", O_RDONLY);
		if (fd == -1){
			perror("Error in open");
			return;
		}
	
		// alloc memory for stat structure
		buf = (struct stat *)malloc(sizeof(struct stat));
		if (buf == NULL){
			perror("Error in malloc");
			return;
		}
	
		// take the information of the file
		if (fstat(fd, buf) == -1){
			perror("Error in stat");
			return;
		}
	}
	
	// write the file in the socket
	rc = sendfile(connfd, fd, NULL, buf->st_size);

	// error in write
	if (rc == -1){
		perror("Error in sendfile");
		return;
	}

	// incomplete transfer
	if (rc != buf->st_size){
		perror("Incomplete transfer");
		return;
	}	
	
	if (close(fd) == -1){
		perror("Error in close");
		pthread_exit(NULL);
	}
}

/* This function is the starting point of the new threads when they are created from main thread 
 * here the thread take the first connection socket if queue is not empty; 
 * else it wait a condition from main thread (queue not empty) */
void *handle_conn(void *p){
	// argument passed from pthread_create
	struct thread_struct *thread = (struct thread_struct *)p;

	while(1){
		// mutual exclusion in queue (queue protected)
		mutex_lock(&queue_mtx);
		while (get == put){
			fprintf(stderr, "Sleep\n");
			// empty queue, wait condition  
			if (pthread_cond_wait(&cond_queue, &queue_mtx) != 0){
				perror("Error in wait condition");
				pthread_exit(NULL);
			}
		}
		fprintf(stderr, "Wake up\n");
		thread->fd = queue[get]; // take the first connection socket on list
		if (++get == MAX_THREAD){
			get = 0;
		}
		++i; // thread busy	
		mutex_unlock(&queue_mtx);

		// TODO work thread
		fprintf(stderr, "Start work\n");
		web_request(thread->fd);
		fprintf(stderr, "End work\n");
		
		// close connection socket
		if (close(thread->fd) == -1){
			perror("Error in close");
			pthread_exit(NULL);
		}
		
		// protect thread counter
		mutex_lock(&queue_mtx);
		--i; // thread is now free to work
		mutex_unlock(&queue_mtx);

		// signal to main thread: thread aviable for new connection
		if (pthread_cond_signal(&cond_queue) != 0){
            perror("Error in signal condition");
            pthread_exit(NULL);
        }
	}
}

/* This function create a pool of thread (MAX_THREAD) */
void make_threads(void){
	int j;

	for (j=0; j<MAX_THREAD; ++j){
		// create a new thread
		if (pthread_create(&(threads[j].tid), NULL, handle_conn, &(threads[j])) != 0){
			perror("Error in thread spawn");
			exit(EXIT_FAILURE);
		}
	}
}

int main(void)
{
	int sockfd, connfd;
	struct sockaddr_in addr;

	// create a TCP socket
	sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd == -1){
		perror("Error in socket");
		return EXIT_FAILURE;
	}

	// clear sockaddr_in's memory
	memset((void *)&addr, 0, sizeof(addr));

	addr.sin_family = AF_INET;
	addr.sin_port = htons(PORT);
	addr.sin_addr.s_addr = htonl(INADDR_ANY); // accept connection from any IP address

	if (bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)) == -1){
		perror("Error in bind");
		return EXIT_FAILURE;
	}

	// listen in main socket (listen socket)
	if (listen(sockfd, 0) == -1){
		perror("Error in listen");
		return EXIT_FAILURE;
	}

	// initialize mutex and condition
	initialize_resource();
	// create pool threads
	make_threads();

	// manage request
	for (;;){
		if ((connfd = accept(sockfd, (struct sockaddr *)NULL, NULL)) == -1){
			perror("Error in accept");
			return EXIT_FAILURE;
		}

		// set a structure timeval for manage socket blocking
		struct timeval tv;
		tv.tv_sec = 5;  /* 5 Secs Timeout */
		tv.tv_usec = 0;
		// set socket option in read
		if (setsockopt(connfd, SOL_SOCKET, SO_RCVTIMEO,(char *)&tv,sizeof(struct timeval)) < 0){
			perror("Error in set socket options\n");
			return EXIT_FAILURE;
		}

		mutex_lock(&queue_mtx); // protect queue
		
		while (i == MAX_THREAD){
			fprintf(stderr, "Busy server...\n");
			if (pthread_cond_wait(&cond_queue, &queue_mtx)){
				perror("Error in wait condition");
				return EXIT_FAILURE;
			}
		}
		queue[put] = connfd;
		if (++put == MAX_THREAD){
			put = 0;
		}
		pthread_cond_signal(&cond_queue);
		mutex_unlock(&queue_mtx);
	}
}
