#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <pthread.h>
#include <semaphore.h>

#define QLEN 5
#define BUFSIZE 1024
#define MAX_CLIENTS 512
#define MAX_PROD 480
#define MAX_CON 480

// Each item has a random-sized letters buffer between 1 and 1 million.
#define MAX_LETTERS 1000000

int ITEM_BUFSIZE = 0;
int prod_count = 0, con_count = 0;
pthread_mutex_t mutex, prod_mutex, con_mutex;
sem_t full, empty;

int passivesock( char *service, char *protocol, int qlen, int *rport );
void* client_service(void* val);

typedef struct item_t
{
        int size;
        char *letters;
} ITEM;

ITEM **buffer;
int buffer_count;
void mutex_increment(pthread_mutex_t* mutex_ptr, int* value_ptr, int inc_value){
    pthread_mutex_lock(mutex_ptr);
    (*value_ptr) += inc_value;
    pthread_mutex_unlock(mutex_ptr);
}


int main( int argc, char *argv[] ){
        char *service;
        struct sockaddr_in fsin;
        socklen_t alen;
        int msock;
        int ssock;
        int rport = 0;

        switch (argc) 
	{
		case	2:
			// No args? let the OS choose a port and tell the user
			rport = 1;
			ITEM_BUFSIZE = atoi(argv[1]);
                        break;
		case	3:
			// User provides a port? then use it
			service = argv[1];
                        ITEM_BUFSIZE = atoi(argv[2]);
			break;
		default:
			fprintf( stderr, "usage: server [port]\n" );
			exit(-1);
	}

        pthread_mutex_init( &mutex, NULL );
        pthread_mutex_init( &prod_mutex, NULL);
        pthread_mutex_init( &con_mutex, NULL);
        sem_init( &full, 0, 0 );
	sem_init( &empty, 0, ITEM_BUFSIZE);

        //Don't know when to free this buffer, since server might close at any time
        buffer = (ITEM**) malloc(sizeof(ITEM*) * ITEM_BUFSIZE);
        buffer_count = 0;

        msock = passivesock( service, "tcp", QLEN, &rport );
	if (rport)
	{
		//	Tell the user the selected port
		printf( "server: port %d\n", rport );	
		fflush( stdout );
	}

        for(;;){
                int* ssock = (int * )malloc(sizeof(int));

		alen = sizeof(fsin);
		*ssock = accept( msock, (struct sockaddr *)&fsin, &alen );
		if (*ssock < 0)
		{
			fprintf( stderr, "accept: %s\n", strerror(errno) );
			exit(-1);
		}
                // if()


		printf( "A client has arrived!\n" );
                fflush(stdout);
                //Check if already 512 clients are connected to server
                if(prod_count + con_count >= MAX_CLIENTS){        
                        close(*ssock);
                        free(ssock);
                        printf("Too many clients on the server. The client got kicked out!\n");
                        fflush(stdout);
                        continue;
                }
		// fflush( stdout );
                
                pthread_t thread;
                int status = pthread_create(&thread, NULL, client_service, (void *)ssock);
                if(status != 0){
                        printf("Error creating thread");
                        free(buffer);
                        free(ssock);
                        exit(-1);
                }
        }
        
        free(buffer);
        pthread_exit(0);
}

void producer_handler(int ssock);
void consumer_handler(int ssock);

void* client_service(void* val){
        int *ssock_ptr = (int *)val;
        int ssock = *ssock_ptr;
        char buf[BUFSIZE];
        int cc  = 0;
        
        if ( (cc = read( ssock, buf, BUFSIZE )) <= 0 ){
                printf( "The client has gone.\n" );
        }else{
                buf[cc] = '\0';
                if(strcmp(buf, "PRODUCE\r\n") == 0){
                        
                        if(prod_count < MAX_PROD){
                                mutex_increment(&prod_mutex, &prod_count, 1);
                                
                                producer_handler(ssock);

                                mutex_increment(&prod_mutex, &prod_count, -1);

                        }
                        
                }else if(strcmp(buf, "CONSUME\r\n") == 0){

                        if(con_count < MAX_CON){
                                mutex_increment(&con_mutex, &con_count, 1);
                                
                                consumer_handler(ssock);
                                
                                mutex_increment(&con_mutex, &con_count, -1);
                        }
                }
        }

        close(ssock);
        free(ssock_ptr);
        pthread_exit(0);
}


void producer_handler(int ssock){

        sem_wait(&empty);
        //Respond with GO
        if(write(ssock, "GO\r\n", 4) < 0){
                return;
        }

        //Read item size
        ITEM* new_item = (ITEM* )malloc(sizeof(ITEM));

        int ret;
        
        if(read(ssock, (char *) &ret, 4) <=0 ){
                free(new_item);
                return;
        }
        int item_size = ntohl(ret);
        


        new_item -> letters = (char* ) calloc(item_size, sizeof(char));
        new_item -> size = item_size;
        
        //Read item letters
        int bytes_read = 0, cc = 0 ;
        while(bytes_read < item_size && (cc = read(ssock, new_item -> letters + bytes_read, BUFSIZE)) > 0){
                bytes_read += cc;
        }
        pthread_mutex_lock(&mutex);
        //---------------------------------------------
        buffer[buffer_count] = new_item;
        buffer_count++;
        printf("Item was added; Item count in buffer is %d\n", buffer_count);
        //----------------------------------------------
        pthread_mutex_unlock(&mutex);
        sem_post(&full);
        write(ssock, "DONE\r\n", 6);
        printf("Produced item size: %d\n",item_size);
        fflush(stdout);
}

void consumer_handler(int ssock){
        //Consume item if available
        sem_wait(&full);
        pthread_mutex_lock(&mutex);
        //-----------------------------------------------
        ITEM* item = buffer[buffer_count - 1];
        buffer_count --;
        printf("Item was consumed; Item count int buffer is %d\n", buffer_count);
        //-----------------------------------------------
        pthread_mutex_unlock(&mutex);
        sem_post(&empty);
        
        //Send item size
        int conv = htonl(item -> size);
        write(ssock, (char*) &conv, 4);

        //Send item letters
        write(ssock, item -> letters, item -> size);
        printf("Consumed item size: %d\n", item -> size);
        fflush(stdout);
        free(item -> letters);
        free(item);
}
