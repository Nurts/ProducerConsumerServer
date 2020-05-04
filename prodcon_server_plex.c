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
#include <sys/select.h>

#define QLEN 5
#define BUFSIZE 1024
#define MAX_CLIENTS 512
#define MAX_PROD 480
#define MAX_CON 480

// Each item has a random-sized letters buffer between 1 and 1 million.
#define MAX_LETTERS 1000000

int ITEM_BUFSIZE = 0;
int prod_amt = 0, con_amt = 0;
pthread_mutex_t mutex, prod_mutex, con_mutex;
sem_t full, empty;

typedef struct item_t
{
    int size;
    char *letters;
} ITEM;

ITEM **buffer;
int buffer_count;

int passivesock( char *service, char *protocol, int qlen, int *rport );
void* producer_handler(void* ptr);
void* consumer_handler(void* ptr);

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
    int rport = 0;
    //producer and consumer amount
    
    
    switch (argc){
		case 2:
			// No args? let the OS choose a port and tell the user
			rport = 1;
			ITEM_BUFSIZE = atoi(argv[1]);
                break;
		case 3:
			// User provides a port? then use it
			service = argv[1];
            ITEM_BUFSIZE = atoi(argv[2]);
			break;
		default:
			fprintf( stderr, "usage: [port] bufsize\n" );
			exit(-1);printf( "server: port %d\n", rport );	
		fflush( stdout );

	}

    //Mutex, semaphore init
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


    // Multiplexing vars
    fd_set read_fds, dup_fds;
    int nfds = msock + 1;

    FD_ZERO(&dup_fds);
    FD_SET(msock, &dup_fds);

    printf("Server is running on localhost! \n");
    fflush(stdout);

    for(;;){
        
        //Reset file descriptor
        memcpy((char*) &read_fds, (char*) &dup_fds, sizeof(read_fds));


        if (select(nfds, &read_fds, (fd_set*) 0, (fd_set*) 0, (struct timeval*) 0) < 0){
			fprintf( stderr, "server select: %s\n", strerror(errno) );
            free(buffer);
			exit(-1);
		}

        if(FD_ISSET(msock, &read_fds)){
            alen = sizeof(fsin);
            int ssock = accept( msock, (struct sockaddr *)&fsin, &alen );
            if (ssock < 0){
                fprintf( stderr, "accept: %s\n", strerror(errno) );
                exit(-1);
            }
            
            //Insert new socket
            FD_SET(ssock, &dup_fds);

            if(ssock + 1 > nfds){
                nfds = ssock + 1;
            }
        }

        for(int fd = 0; fd < nfds; fd++){

            if(fd != msock && FD_ISSET(fd, &read_fds)){
                
                char should_close = 1;
                
                do{

                    if(prod_amt + con_amt >= MAX_CLIENTS){
                        printf("Too many clients in the server. Client arrived and got kicked out !\n");
                        break;
                    }

                    char buf[BUFSIZE];
                    int cc  = 0;
                    
                    if ( (cc = read(fd, buf, BUFSIZE )) <= 0 ){
                        printf( "The client has arrived and gone !\n" );
                        break;
                    }
                    
                    buf[cc] = '\0';
                        
                    if(strcmp(buf, "PRODUCE\r\n") == 0){
                        printf("PRODUCER has arrived !\n");
                        fflush(stdout);
                        
                        if(prod_amt >= MAX_PROD){
                            printf("Too many producers in the server. PRODUCER got kicked out !\n");
                            break;
                        }
                        mutex_increment(&prod_mutex, &prod_amt, 1);
                        
                        //CREATE THREAD FOR PRODUCER
                        pthread_t thread;
                        int* ssock = (int*) malloc(sizeof(int));
                        *ssock = fd;
                        if(pthread_create(&thread, NULL, producer_handler, (void *)ssock) != 0){
                            printf("Error creating thread ! PRODUCER got kicket out !\n");
                            break;
                        }

                    }else if(strcmp(buf, "CONSUME\r\n") == 0){
                        printf("CONSUMER has arrived !\n");
                        fflush(stdout);

                        if(con_amt >= MAX_CON){
                            printf("Too many consumers in the server. CONSUMSER got kicked out !\n");
                            break;
                        }
                        mutex_increment(&con_mutex, &con_amt, 1);
                        //CREATE THREAD FOR CONSUMER
                        pthread_t thread;
                        int* ssock = (int*) malloc(sizeof(int));
                        *ssock = fd;
                        if(pthread_create(&thread, NULL, consumer_handler, (void *)ssock) != 0){
                            printf("Error creating thread ! CONSUMER got kicket out !\n");
                            break;
                        }

                    }
                    //Socket was successfully passed to seperate thread. We won't close this one. 
                    should_close = 0;
                }while(0);

                //If something went wrong, close the socket
                if(should_close){
                    fflush(stdout);
                    close(fd);
                }

                //Delete fd from dup_fds, we don't need to monitor this socket anymore (It got deleted or will be handled by its own thread);
                FD_CLR(fd, &dup_fds);

                //Update nfds
                if(nfds == fd + 1)
                    nfds --;
            }   

        }

    }    
    free(buffer);
    pthread_exit(0);
}

void* producer_handler(void* ptr){
    int ssock = *((int *) ptr);

    sem_wait(&empty);
    //Respond with GO
    write(ssock, "GO\r\n", 4);

    //Read item size
    ITEM* new_item = (ITEM* )malloc(sizeof(ITEM));

    int ret;
    
    if(read(ssock, (char *) &ret, 4) <= 0 ){
        free(new_item);
    }else{

        int item_size = ntohl(ret);
        new_item -> letters = (char* ) calloc(item_size, sizeof(char));
        new_item -> size = item_size;
        
        //Read item letters
        int bytes_read = 0, cc = 0 ;
        while(bytes_read < item_size && (cc = read(ssock, new_item -> letters + bytes_read, BUFSIZE)) > 0){
            bytes_read += cc;
        }
        
        //Put the item into buffer 
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
    
    close(ssock);
    free( (int*) ptr);
    
    mutex_increment(&prod_mutex, &prod_amt, -1);
}

void* consumer_handler(void* ptr){
    int ssock = *((int *) ptr);

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

    close(ssock);
    free( (int*) ptr);

    mutex_increment(&con_mutex, &con_amt, -1);
}