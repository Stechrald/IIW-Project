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
#include <libxml/parser.h>
#include <libxml/tree.h>

#define MAX_THREAD 2
#define PORT 80
#define MAX_BUF 1024
#define SERVER_STRING "Server: webServer/1.0.0\r\n"
#define LOG_INFO 44
#define SYSTEM_ERR 42
#define NOT_FOUND 404
#define INTERNAL_ERROR 500
#define NOT_IMPLEMENTED 501

const char *not_found = "The requested URL was not found on this server";
const char *not_implemented = "This method is not supported by the server";
const char *internal_error = "An internal server error occurred";
const char *xml_path = "/home/christian/wurfl2.xml";

void logger(int type_info, char *str1, char *str2, int i);
void show_error(int connfd,char *strTitle, const char *strBody, char *strError);

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

	// threads array initializzation
	threads = malloc(sizeof(struct thread_struct)*MAX_THREAD);
	if (threads == NULL){
		logger(SYSTEM_ERR, SERVER_STRING, "Error in allocation memory for thread structure", (int)getpid());
		exit(EXIT_FAILURE);
	}
	// end threads array initializzation


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


/* This function release a mutex */
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
	
	// finds the request method
	while(req[i] != ' '){
		type[j] = req[i];
		++i;
		++j;
	}
	
	j = 0;
	i = i+1;
	// finds the path of file requested
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


/* This function sends the rigth error code to the client,
 * because requested file is failed */
void show_error(int connfd, char *strTitle, const char *strBody, char *strError){
	char buf[MAX_BUF];
	strcpy(buf, "HTTP/1.1 ");
	strcat(buf, strError);
	strcat(buf, " \r\n");
	send(connfd, buf, strlen(buf), 0);
	strcpy(buf, SERVER_STRING);
	send(connfd, buf, strlen(buf), 0);
	strcpy(buf, "Content-Type: text/html\r\n");
	send(connfd, buf, strlen(buf), 0);
	strcpy(buf, "\r\n");
	send(connfd, buf, strlen(buf), 0);
	
	strcpy(buf, "<HTML><TITLE>");
	strcat(buf, strTitle);
	strcat(buf, "</TITLE>\r\n");
	send(connfd, buf, strlen(buf), 0);
	strcpy(buf, "<BODY><P>");
	strcat(buf, strBody);
	send(connfd, buf, strlen(buf), 0);
	strcpy(buf, "</P></BODY></HTML>\r\n");
	send(connfd, buf, strlen(buf), 0);
}


/* This function sends the requested file to the client using "sendfile" procedure */
void send_request_file(int connfd, int fd, char *path){
	struct stat *buf;
	int rc;	
	
	// alloc memory for stat structure
	buf = (struct stat *)malloc(sizeof(struct stat));
	if (buf == NULL){
		logger(INTERNAL_ERROR, "Server", "Error in allocation memory for file statistic structure", (int)getpid());
		show_error(connfd, "Internal Server Error", internal_error,"500");
		return;
	}
			
	// takes the information of the file
	if (fstat(fd, buf) == -1){
		logger(INTERNAL_ERROR, "Server", "Error in fstat function for obtain file statistic", (int)getpid());
		show_error(connfd, "Internal Server Error", internal_error,"500");
		return;
	}
		
	int i;
	if (strlen(path) > 1){
		i = 1;
	}
	
	else{
		i = 0;
	}
	// sends header HTTP to client
	header_successful(connfd, i, buf->st_size);		
	// write the file in the socket
	rc = sendfile(connfd, fd, NULL, buf->st_size);

	// if errors occured during write procedure
	if (rc == -1){
		logger(INTERNAL_ERROR, "Server", "Error in write file to socket", (int)getpid());
		show_error(connfd, "Internal Server Error", internal_error,"500");
		return;		
	}

	// incomplete transfer
	if (rc != buf->st_size){
		perror("Incomplete transfer");
		return;
	}
	
	logger(LOG_INFO, "Server", "Successful send file", (int)getpid());	
}


/* This function parses the information in HTTP request; in particular it founds accept line and user-agent line */
void parse_info(char *req, char **info, const char *object)
{
	int i;
	char *ptr;
	ptr = malloc(MAX_BUF);
	strcpy(ptr, req); // necessario altrimenti strstr mi taglia la request
		
	*info = strstr(ptr, object);

	for (i = 0; i < strlen(*info); ++i){
		if ((*info)[i] == '\n'){
			(*info)[i] = '\0';
			break;
		}
	}
	
	for (i = 0; i < strlen(*info); ++i){
		if ((*info)[i] == ' '){
			*info = *info + i + 1;
			break;
		}
	}
	return;
}


void parse_xmlfile(char *user_agent, int *h, int *w, xmlNodePtr root){
	xmlNodePtr cur, cur2;
	xmlChar *height;
	xmlChar *width;
		
	for (cur = root; cur; cur = cur->next){		
		if (cur->type == XML_ELEMENT_NODE && !xmlStrcmp(cur->name, (const xmlChar *)"group") && !xmlStrcmp(xmlGetProp(cur,(const xmlChar*) "id"), (const xmlChar *)"display")){	
			char *agent = (char *)xmlGetProp(cur->parent,(const xmlChar*) "user_agent");
			if (strcmp(user_agent, agent) == 0){				
				
				for (cur2 = cur->children; cur2; cur2 = cur2->next) {
							
					if(!xmlStrcmp(xmlGetProp(cur2,(const xmlChar*) "name"), (const xmlChar *)"resolution_height"))	
						height = xmlGetProp(cur2,(const xmlChar*) "value"); 				
					if(!xmlStrcmp(xmlGetProp(cur2,(const xmlChar*) "name"), (const xmlChar *)"resolution_width"))  
           	 			width = xmlGetProp(cur2,(const xmlChar*) "value"); 	
				}
				*w = atoi((char *)width);
				*h = atoi((char *)height);
				
				return;
			}		
		}
		parse_xmlfile(user_agent, h, w, cur->children);
	}
	
}


/* This function parses and processes the request send by the client to check the right method to call */
void web_request(int connfd){
	int fd;
	ssize_t nread;
	char req[MAX_BUF];
	char path[MAX_BUF];
	char *user_agent;
	char *accept;
	int parse_value;
	char type[4];
	int height, width;
	
	user_agent = malloc(MAX_BUF);
	if (user_agent == NULL){
		perror("error in malloc");
		return;
	}
	
	accept = malloc(MAX_BUF);
	if (accept == NULL){
		perror("error in malloc");
		return;
	}
	
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
		// parse informations in HTTP request (accept, user-agent)
		parse_info(req, &accept, "Accept");
		parse_info(req, &user_agent, "User-Agent");
		
		// parse client request
		parse_value = parse_request(req, path, type);
		
		//printf("HTTP Method: %s\n", type);
		//printf("File requested: %s\n", path);
		
		xmlDocPtr doc;
		xmlNodePtr cur;

		doc = xmlParseFile(xml_path);
		if (doc == NULL ) {
			fprintf(stderr,"Document not parsed successfully. \n");
			return;
		}
		
		cur = xmlDocGetRootElement(doc);
		
		if (cur == NULL) {
			fprintf(stderr,"empty document\n");
			xmlFreeDoc(doc);
			return;
		}
		user_agent[strlen(user_agent)-1] = '\0';
		printf("%s\n", user_agent);
		
		parse_xmlfile(user_agent, &height, &width, cur);
		printf("%d %d\n", height, width);
		
	
		
		// verifies that the request is a GET request
		if (strcmp(type, "GET") == 0){
			if (parse_value == 1){
				// the first page showed to user
				fd = open("/home/christian/index.html", O_RDONLY);
				if (fd == -1){
					logger(NOT_FOUND, "Server", "404 error code, file not found", (int)getpid());
					show_error(connfd,"Not Found", not_found, "404");
					return;
				}
			}	
			else{
				fd = open(path, O_RDONLY);
				if (fd == -1){
					logger(NOT_FOUND, "Server", "404 error code, file not found", (int)getpid());
					show_error(connfd,"Not Found", not_found, "404");
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
			// sends a server error to client
			logger(NOT_IMPLEMENTED, "Server", "501 error code, method not implemented", (int)getpid());
			show_error(connfd,"Not Implemented", not_implemented,"501");
			return;
		}
		
		// sends file to client
		send_request_file(connfd, fd, path);
		
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
			// empty queue, wait condition  
			if (pthread_cond_wait(&cond_queue, &queue_mtx) != 0){
				logger(SYSTEM_ERR, "Server", "Error in wait condition for empty queue", (int)getpid());
				pthread_exit(NULL);
			}
		}
		printf("Wake up thread\n");
		thread->fd = queue[get]; // take the first connection socket on the list
		if (++get == MAX_THREAD){
			get = 0;
		}
		++i; // busy thread	
		mutex_unlock(&queue_mtx);

		// TODO work thread
		fprintf(stderr, "Start work\n");
		web_request(thread->fd);
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

/* This function creates a pool of thread (MAX_THREAD) */
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

	// clear sockaddr_in's memory
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

	// initialize all resources
	initialize_resource();
	// create pool threads
	make_threads();

	// manage request
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
