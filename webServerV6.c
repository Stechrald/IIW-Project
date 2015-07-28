#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <string.h>
#include <pthread.h>
#include <sys/sendfile.h>
#include <sys/time.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <wand/MagickWand.h>
#include <mysql/mysql.h>

#define MAX_THREAD 128 // power of 2
#define MIN_THREAD 4
#define PORT 8080
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
const char *cache_path = "/home/christian/cache";
const char *priority_file = "/home/christian/cache/priority.txt";

void logger(int type_info, char *str1, char *str2, int i);
void show_error(int connfd,char *strTitle, const char *strBody, char *strError);

// structure containing thread informations
struct thread_struct{
	pthread_t tid; // thread id
	int fd; // managed connection file descriptor
	int pos; // position of thread in array
	int canc; // flag for thread cancellation
};

pthread_cond_t cond_queue = PTHREAD_COND_INITIALIZER; // conditon for queue, static initialization
pthread_mutex_t queue_mtx = PTHREAD_MUTEX_INITIALIZER; // queue mutex, static initialization
pthread_mutex_t image_mtx = PTHREAD_MUTEX_INITIALIZER;

struct thread_struct *threads; // threads array

int *queue; // queue of socket fd
int i = 0; // number of working threads
int free_t = 0; // index of first free thread
int curr = MIN_THREAD; // current number of max threads
int put, get; // head and tail of the queue


/* This function calculates the correct time, formatted for logging routine */
char *now(void)
{
	char *buff;
	time_t ticks;
	
	buff = malloc(MAX_BUF*2);
	if (buff == NULL){
		logger(SYSTEM_ERR, "Server", "Error in allocation time buffer", (int)getpid());
		free(threads);
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
	threads = (struct thread_struct *)malloc(sizeof(struct thread_struct)*MIN_THREAD);
	if (threads == NULL){
		logger(SYSTEM_ERR, SERVER_STRING, "Error in allocation memory for thread structure", (int)getpid());
		exit(EXIT_FAILURE);
	}
	// end threads array initializzation


	// queue initalization
	queue = malloc(sizeof(int)*MAX_THREAD);
	if (queue == NULL){
		logger(SYSTEM_ERR, "Server", "Error in allocation memory for queue", (int)getpid());
		free(threads);
		exit(EXIT_FAILURE);
	}
	
	put = get = 0;
	// end queue initialization
}


/* This function locks a mutex to protect an area of memory shared between threads */
void mutex_lock(pthread_mutex_t *mutex){
	if (pthread_mutex_lock(mutex) != 0){
		logger(SYSTEM_ERR, "Server", "Error in lock mutex semaphore", (int)getpid());
		free(threads);
		exit(EXIT_FAILURE);
	}
}


/* This function release a mutex */
void mutex_unlock(pthread_mutex_t *mutex){
	if (pthread_mutex_unlock(mutex) != 0){
		logger(SYSTEM_ERR, "Server", "Error in unlock mutex semaphore", (int)getpid());
		free(threads);
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
	send(connfd, buf, strlen(buf), MSG_NOSIGNAL);  // use MSG_NOSIGNAL to escape SIGPIPE signal  
	memset(buf, 0, sizeof(buf));
	strcpy(buf, SERVER_STRING);
	// TODO causa crash
	send(connfd, buf, strlen(buf), MSG_NOSIGNAL);
	// Content_Lenght FUNDAMENTAL FOR PERSISTENCY IMPLEMENTATION AND CLIENT LOCK AVOIDANCE DURING READING ROUTINE
	sprintf(buf, "Content-Length: %d\r\n", (int)size); // uses sprintf to insert size param
	send(connfd, buf, strlen(buf), MSG_NOSIGNAL);
	if (i == 1){
		strcpy(buf, "Content-Type: image/jpg\r\n");
		send(connfd, buf, strlen(buf), MSG_NOSIGNAL);
	}
	else{
		strcpy(buf, "Content-Type: text/html\r\n");
		send(connfd, buf, strlen(buf), MSG_NOSIGNAL);
	}
	strcpy(buf, "Connection: keep-alive\r\n");
	send(connfd, buf, strlen(buf), MSG_NOSIGNAL);
	strcpy(buf, "\r\n");
	send(connfd, buf, strlen(buf), MSG_NOSIGNAL);
}


/* This function sends only the header to the client,
 * in response to a HEAD request */
void head_request(int connfd){
	char buf[MAX_BUF];

	strcpy(buf, "HTTP/1.1 200 OK\r\n");
	send(connfd, buf, strlen(buf), MSG_NOSIGNAL);
	strcpy(buf, SERVER_STRING);
	send(connfd, buf, strlen(buf), MSG_NOSIGNAL);
	strcpy(buf, "Connection: keep-alive\r\n");
	send(connfd, buf, strlen(buf), MSG_NOSIGNAL);
	strcpy(buf, "\r\n");
	send(connfd, buf, strlen(buf), MSG_NOSIGNAL);
}


/* This function sends the rigth error code to the client,
 * because requested file is failed */
void show_error(int connfd, char *strTitle, const char *strBody, char *strError){
	char buf[MAX_BUF];
	strcpy(buf, "HTTP/1.1 ");
	strcat(buf, strError);
	strcat(buf, " \r\n");
	send(connfd, buf, strlen(buf), MSG_NOSIGNAL);
	strcpy(buf, SERVER_STRING);
	send(connfd, buf, strlen(buf), MSG_NOSIGNAL);
	strcpy(buf, "Content-Type: text/html\r\n");
	send(connfd, buf, strlen(buf), MSG_NOSIGNAL);
	strcpy(buf, "\r\n");
	send(connfd, buf, strlen(buf), MSG_NOSIGNAL);
	
	strcpy(buf, "<HTML><TITLE>");
	strcat(buf, strTitle);
	strcat(buf, "</TITLE>\r\n");
	send(connfd, buf, strlen(buf), MSG_NOSIGNAL);
	strcpy(buf, "<BODY><P>");
	strcat(buf, strBody);
	send(connfd, buf, strlen(buf), MSG_NOSIGNAL);
	strcpy(buf, "</P></BODY></HTML>\r\n");
	send(connfd, buf, strlen(buf), MSG_NOSIGNAL);
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
		free(threads);
		exit(EXIT_FAILURE);
	}
			
	// takes the information of the file
	if (fstat(fd, buf) == -1){
		logger(INTERNAL_ERROR, "Server", "Error in fstat function for obtain file statistic", (int)getpid());
		show_error(connfd, "Internal Server Error", internal_error,"500");
		free(threads);
		exit(EXIT_FAILURE);
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
	free(ptr);
	return;
}


char *access_cache(char *path, int h, int w){
	//char *image_name = path + 32;
	//int fd;
	
	// ACCESSO ESCLUSIVO
	/*fd = open(priority_file, O_RDONLY);
	if (fd == -1){
		perror("Error in open");
		exit(EXIT_FAILURE);
	}*/
	
	return NULL;
}

/* This function read an image from a path, resize it with width and height (passed as arguments), 
 * and save resized image in cache folder */
// TODO genera crash
void insert_resized(char *path, int q, int wid, int hei, char *cache) {
	
	// START MagickWand initialization
	MagickWand *wand = NULL;
	MagickWandGenesis();
	
	wand = NewMagickWand();
	// END MagickWand initialization
	
	// open image in read mode
	if (MagickReadImage(wand, path) == MagickFalse) {
		perror("error in read");
		// LOGGER
		DestroyMagickWand(wand);
		MagickWandTerminus();
		free(threads);
		exit(EXIT_FAILURE);
	}
	
	// resize selected image
	if (MagickResizeImage(wand, wid, hei, LanczosFilter, 1) == 0) {
		perror("error in resize");
		// LOGGER
		DestroyMagickWand(wand);
		MagickWandTerminus();
		free(threads);
		exit(EXIT_FAILURE);
	}
	
	// set quality to image
	if (MagickSetImageCompressionQuality(wand, q) == 0) {
		perror("error in compression");
		// LOGGER
		DestroyMagickWand(wand);
		MagickWandTerminus();
		free(threads);
		exit(EXIT_FAILURE);
	}
	
	// open new file in write mode and create the modified image
	if (MagickWriteImage(wand, cache) == 0) {
		perror("error in write");
		// LOGGER
		DestroyMagickWand(wand);
		MagickWandTerminus();
		free(threads);
		exit(EXIT_FAILURE);
	}
	
	// release MagickWand
	DestroyMagickWand(wand);
	MagickWandTerminus();
}

/* This function insert a modified image into cache and update the priority file */
char *insert_cache(char *path, int height, int width){
	int fd_priority;
	char new_path[MAX_BUF];
	char *image_name;
	
	// START create new path (cache path)
	memset(new_path, 0, strlen(new_path));
	strcat(new_path, cache_path);
	strcat(new_path, "/");
	strcat(new_path, path + 32);
	// END create new path (cache path)
	
	//int q = 95; // TODO take quality from HTTP request
	//insert_resized(path, q, width, height, new_path); // resize image and insert it into cache
	
	// open priority file in write mode or create if not exists
	fd_priority = open(priority_file, O_CREAT|O_WRONLY|O_APPEND, 0777); 
	if (fd_priority == -1){
		perror("Error in open");
		free(threads);
		exit(EXIT_FAILURE);
	}
	
	
	// START write procedure in priority file
	if (write(fd_priority, path+32, strlen(path+32)) != (int) strlen(path+32)){
		perror("Error in write");
		free(threads);
		exit(EXIT_FAILURE);
	}
	//write file name
	char c = ';';
	if (write(fd_priority, &c , 1) != 1){
		perror("Error in write");
		free(threads);
		exit(EXIT_FAILURE);
	}
	//write divider char
	char w[10]; 
	char h[10];
	sprintf(h, "%d" ,height);
	if (write(fd_priority, h, strlen(h)) != (int) strlen(h)){
		perror("Error in write");
		free(threads);
		exit(EXIT_FAILURE);
	}
	//write height
	if (write(fd_priority, &c, 1) != 1){
		perror("Error in write");
		free(threads);
		exit(EXIT_FAILURE);
	}
	//write divider char
	
	sprintf(w, "%d" ,width);
	if (write(fd_priority, w, strlen(w)) != (int) strlen(w)){
		perror("Error in write");
		free(threads);
		exit(EXIT_FAILURE);
	}
	//write width
	
	c = '\n';
	if (write(fd_priority, &c, 1) != 1){
		perror("Error in write");
		free(threads);
		exit(EXIT_FAILURE);
	}
	//write newline
	// END write procedure in priority file
	
	if (close(fd_priority) == -1){
		perror("Error in close");
		free(threads);
		exit(EXIT_FAILURE);
	}
	//close file
	image_name = new_path;
	return image_name;
}

/* This function tries to connect her to MYSQL Database */
MYSQL *db_connect(void){

 	// credentials for db connection
 	const char* host = "localhost"; 
	const char* database = "wurfl"; 
	const char* db_user = "root"; 
	const char* db_pass = "asroma93"; 
    
    MYSQL *mysql = mysql_init(NULL);

    if (mysql == NULL) {
            perror("Error in memory allocation");
            return NULL;
    }

    if (mysql_real_connect (mysql, host, db_user, db_pass, "", 0, NULL, 0) == NULL) {
			perror("Error in connection");
			return NULL;
    }
    
    if (mysql_select_db (mysql, database)) {
            perror("Error in database selection");
			return NULL;
    }

	return mysql;

}

/* This function performs a query to local database obtaining information of device */
int db_query(char *user, int *h, int *w){
	MYSQL *mysql;
	MYSQL_RES *res;
	MYSQL_ROW row;
	char query[MAX_BUF];

	mysql = db_connect();
	if (mysql == NULL){
		return 0;
	}
	sprintf(query, "SELECT height, width FROM devices WHERE user_agent LIKE '%s'", user);
	
	// performs query
	if (mysql_query(mysql, query) != 0){
		perror("Error in mysql_query");
		return 0;
	}
	
	// take results
	res = mysql_use_result(mysql);
	if (res == NULL){
		perror("Error in mysql_use_result");
		return 0;
	}
	
	// take columns from result array
	row = mysql_fetch_row(res);
	if (row == NULL){
		*h = *w = 0;
		mysql_free_result(res); // release memory
		mysql_close(mysql); // close connection
		return 1;
	}
	*h = atoi(row[0]);
	*w = atoi(row[1]);
	
	mysql_free_result(res); // realease memory 
	mysql_close(mysql); // close connection
	return 1;
}


/* This function parses and processes the request send by the client to check the right method to call */
void web_request(int connfd){
	int fd;
	ssize_t nread;
	char req[MAX_BUF];
	char path[MAX_BUF];
	char *user_agent;
	//char *accept;
	int parse_value;
	char type[4];
	int height, width;
	
	height = width = 0;
	
		
	user_agent = malloc(MAX_BUF);
	if (user_agent == NULL){
		perror("error in malloc");
		return;
	}
	
	/*accept = malloc(MAX_BUF);
	if (accept == NULL){
		perror("error in malloc");
		return;
	}*/
	
	while(1){
		//for(;;){;}
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
		//parse_info(req, &accept, "Accept");
		parse_info(req, &user_agent, "User-Agent");
		// parse client request
		parse_value = parse_request(req, path, type);
		
		user_agent[strlen(user_agent)-1] = '\0';
		//accept[strlen(accept)-1] = '\0';

		if(db_query(user_agent, &height, &width) == 0){
			logger(INTERNAL_ERROR, "Server", "Error in database connection", (int)getpid());
			show_error(connfd, "Internal Server Error", internal_error,"500");
			return;
		}
		if (height == 0 && width == 0){
			height = 600;
			width = 800;
		}

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
				char *image_name;
				if (strcmp(path, "/favicon.ico") == 0 || strcmp(path, "/home/christian/favicon.ico") == 0){
					return;
				}
				
				/*image_name = access_cache(path, height, width);
				if (image_name == NULL){
					mutex_lock(&image_mtx);
					image_name = insert_cache(path, height, width);
					mutex_unlock(&image_mtx);
				}*/
				image_name = path;
				fd = open(image_name, O_RDONLY);
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
		close(fd);
	}
}

/* This function swap two thread structure in pool maintaining partition */
void swap(int pos){
	struct thread_struct temp;
	
	if (threads[free_t].pos == free_t){
		printf("SI SONO UGUALI\n");
	}
	else{
		printf("NO NON SONO UGUALI\n");
		printf("POS: %d --- FREE: %d\n", threads[free_t].pos, free_t);
	}
	//printf("CIAO\n");
	//printf("PROVA: %d\n", pos);
	temp.tid = threads[pos].tid;
	temp.fd = threads[pos].fd;
	temp.pos = threads[pos].pos;
	temp.canc = threads[pos].canc;
	
	threads[pos].tid = threads[free_t].tid;
	threads[pos].fd = threads[free_t].fd;
	threads[pos].pos = threads[free_t].pos;
	threads[pos].canc = threads[free_t].canc;
	
	threads[free_t].tid = temp.tid;
	threads[free_t].fd = temp.fd;
	threads[free_t].pos = temp.pos;
	threads[free_t].canc = temp.canc;
}

void swap_struct(struct thread_struct **a, struct thread_struct **b){
	struct thread_struct *t;
	
	t = *b;
	*b = *a;
	*a = t;
}

/* This function is the starting point for newly generated threads (created by the main thread)
 * here a thread takes its first connection socket if queue is not empty; 
 * otherwise it waits for a condition update (queue not empty) from the main thread) */
void *handle_conn(void *p){
	// argument passed by pthread_create
	struct thread_struct *thread = (struct thread_struct *)p;

	if(pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL) != 0){
		perror("Error in setcancelstate");
		free(threads);
		exit(EXIT_FAILURE);
	}

	while(1){

		// mutual exclusion in queue (protected queue)
		mutex_lock(&queue_mtx);
		while (get == put){
			//fprintf(stderr, "Sleep\n");
			// empty queue, wait condition 
			if (pthread_cond_wait(&cond_queue, &queue_mtx) != 0){
				perror("error in cond_wait");
				free(threads);
				exit(EXIT_FAILURE);
			}
			
			if (thread->canc == 1){
				printf("THREAD CANCELLATO\n");
				mutex_unlock(&queue_mtx);
				printf("STO USCENDO\n");
				pthread_exit(NULL);
			}
		}

		//swap(thread->pos);
		struct thread_struct *temp;
		temp = &threads[free_t];
		swap_struct(&thread, &temp);
		//thread->pos = free_t;
		free_t++;
		
		thread->fd = queue[get]; // take the first connection socket on the list
		if (++get == MAX_THREAD){
			get = 0;
		}
		++i; // busy thread	
		printf("SONO OCCUPATO: %d\n", i);
		printf("SONO IL THREAD: %d\n", (int)pthread_self());
		mutex_unlock(&queue_mtx);
		web_request(thread->fd);
		close(thread->fd);
		
		// protected thread counter
		mutex_lock(&queue_mtx);
		--i; // thread is now allowed to work
		printf("SONO LIBERO: %d\n", i);
		printf("SONO IL THREAD: %d\n", (int)pthread_self());
		free_t--;
		memset(temp, 0, sizeof(struct thread_struct));
		temp = &threads[free_t];
		swap_struct(&thread, &temp);
		//thread->pos = free_t;
		mutex_unlock(&queue_mtx);
		
		// signal sent to main thread: thread aviable for new connection
		if (pthread_cond_signal(&cond_queue) != 0){
			logger(SYSTEM_ERR, "Server", "Error in signal condition to main thread", (int)getpid());
            free(threads);
            exit(EXIT_FAILURE);
        }
	}
}

/* This function creates a pool of thread (MIN_THREAD) */
void make_threads(void){
	int j;

	for (j=0; j<MIN_THREAD; ++j){
		// creates a new thread
		threads[j].pos = j;
		threads[j].canc = 0;
		if (pthread_create(&(threads[j].tid), NULL, handle_conn, &(threads[j])) != 0){
			logger(SYSTEM_ERR, "Server", "Error while create threads", (int)getpid());
			free(threads);
			exit(EXIT_FAILURE);
		}
	}
}

/* This function starts the server:
 * 	- create socket
 * 	- set address structure
 * 	- bind
 * 	- listen */
int start_server(void){
	int sockfd;
	struct sockaddr_in addr;

	// creates a TCP socket
	sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd == -1){
		logger(SYSTEM_ERR, "Server", "Error in open socket", (int)getpid());
		free(threads);
		return EXIT_FAILURE;
	}

	// clear sockaddr_in's memory
	memset((void *)&addr, 0, sizeof(addr));

	addr.sin_family = AF_INET;
	addr.sin_port = htons(PORT);
	addr.sin_addr.s_addr = htonl(INADDR_ANY); // accepts connection from any IP address

	if (bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)) == -1){
		logger(SYSTEM_ERR, "Server", "Error in bind socket", (int)getpid());
		free(threads);
		return EXIT_FAILURE;
	}

	// listen procedure in main socket (listen socket)
	if (listen(sockfd, 0) == -1){
		logger(SYSTEM_ERR, "Server", "Error in listen socket", (int)getpid());
		free(threads);
		return EXIT_FAILURE;
	}
	
	return sockfd;
}

/* This function increment the pool of threads if is necessary.
 * It produce a percent of working threads, and if it is > 0.5 then increase(double) the dimension of pool */
void pool_increment(void){
	float perc;
	int j;
	
	perc = free_t/(float)curr;
	
	printf("CURRENT PIPPO THREADS: %d\n", curr);
	if (perc > 0.5){
		// max threads
		if (curr == MAX_THREAD){
			printf("CURRENT PIPPO BAUDO THREADS: %d\n", curr);
			return;
		}
		threads = realloc(threads, sizeof(struct thread_struct)*(curr*2));
		if (threads == NULL){
			perror("error in realloc");
			logger(SYSTEM_ERR, "Server", "Error in reallocation memory", (int)getpid());
			free(threads);
			exit(EXIT_FAILURE);
		}
		for (j=curr; j < curr*2; ++j){
			threads[j].pos = j;
			threads[j].canc = 0;
			if (pthread_create(&(threads[j].tid), NULL, handle_conn, &(threads[j])) != 0){
				logger(SYSTEM_ERR, "Server", "Error while create threads", (int)getpid());
				free(threads);
				exit(EXIT_FAILURE);
			}
		}
		curr = curr*2;
		printf("CURRENT PIPPO BAUDO GAY THREADS: %d\n", curr);
	}
}

/* This function controls, in periodic way, the number of threads in the system, and decrements its if necessary.
 * It produce a percent of working threads in the system and if it is <= 0.25 then the control thread decrements the pool */
void *pool_decrement(void *p){
	p = p;
	float perc;
	int j;
	struct thread_struct *temp;
	
	for(;;){
		usleep(1000000);
		mutex_lock(&queue_mtx);
		perc = free_t/(float)curr; // calculate the percent of working thread
		if (perc <= 0.25){
			// not necessary a decrement
			if (curr == MIN_THREAD){  
				mutex_unlock(&queue_mtx);
				continue;
			}
			for (j=curr/2; j<curr; ++j){
				
				threads[j].canc = 1; // set a thread as cancellable
			}
			
			// send condition signal to all threads
			if (pthread_cond_broadcast(&cond_queue) != 0){
				perror("Error in cond_broadcast");
				free(threads);
				exit(EXIT_FAILURE);
			}
			printf("BROADCAST\n");
		
			// temorary structure
			temp = (struct thread_struct *)malloc(sizeof(struct thread_struct)*(curr/2));
			if (temp == NULL){
				perror("error in malloc");
				logger(SYSTEM_ERR, "Server", "Error in allocation memory", (int)getpid());
				free(threads);
				exit(EXIT_FAILURE);
			}
			
			// copy all thread structure survived
			for (j=0; j<curr/2; j++){
					temp[j].tid = threads[j].tid;
					temp[j].fd = threads[j].fd;
					temp[j].pos = threads[j].pos;
					temp[j].canc = threads[j].canc;
			}
		
			free(threads); // release memory
			threads = temp;
		
			printf("REALLOCATED\n");

			curr = curr/2;
			printf("CURRENT PIPPO BAUDO FROCIO THREADS: %d\n", curr);
		}
		mutex_unlock(&queue_mtx);
	}
}

/* This function create a control thread that controls, in periodic way, the number of threads in the system
 * and decrements them if necessary */
void make_timer_thread(void){
	pthread_t tid;
	
	if (pthread_create(&tid, NULL, pool_decrement, NULL) != 0){
		logger(SYSTEM_ERR, "Server", "Error while create control thread", (int)getpid());
		free(threads);
		exit(EXIT_FAILURE);
	}
}

int main(void)
{
	int sockfd, connfd;
	
	signal(SIGPIPE, SIG_IGN); // ignore SIGPIPE signal to safe send
	sockfd = start_server();
	logger(LOG_INFO, "Start Server", "Work", (int)getpid());

	// initialize all resources
	initialize_resource();
	// create pool threads
	make_threads();
	make_timer_thread(); // create a control thread

	// manage request
	for (;;){
		
		if ((connfd = accept(sockfd, (struct sockaddr *)NULL, NULL)) == -1){
			logger(SYSTEM_ERR, "Server", "Error in accept socket", (int)getpid());
			return EXIT_FAILURE;
		}

		mutex_lock(&queue_mtx);
		pool_increment(); // increments pool of threads
		mutex_unlock(&queue_mtx);
		// sets a timeval structure to manage socket block
		struct timeval tv;
		tv.tv_sec = 10; 
		tv.tv_usec = 0;
		// sets socket option into read procedure
		if (setsockopt(connfd, SOL_SOCKET, SO_RCVTIMEO|SO_KEEPALIVE,(char *)&tv,sizeof(struct timeval)) < 0){
			logger(SYSTEM_ERR, "Server", "Error in set timeout for socket", (int)getpid());
			free(threads);
			return EXIT_FAILURE;
		}
		mutex_lock(&queue_mtx); // protected queue
		
		while (i == MAX_THREAD){
			fprintf(stderr, "Busy server...\n");
			if (pthread_cond_wait(&cond_queue, &queue_mtx)){
				logger(SYSTEM_ERR, "Server", "Error in wait condition for busy queue", (int)getpid());
				free(threads);
				return EXIT_FAILURE;
			}
		}
		queue[put] = connfd;
		if (++put == MAX_THREAD){
			put = 0;
		}
		if(pthread_cond_signal(&cond_queue) != 0){
			logger(SYSTEM_ERR, "Server", "Error in signal condition", (int)getpid());
			free(threads);
			return EXIT_FAILURE;
		}
		mutex_unlock(&queue_mtx);
	}
}
