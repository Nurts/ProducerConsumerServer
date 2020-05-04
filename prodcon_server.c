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
#include <prodcon.h>

int ITEM_BUFSIZE = 0;
int prod_amt = 0, con_amt = 0, client_amt = 0;
int total_produced = 0, total_consumed = 0, total_max_rejected = 0, total_slow = 0, total_prod_rejected = 0, total_con_rejected = 0;
pthread_mutex_t mutex, prod_mutex, con_mutex, total_mutex, total_prod_mutex, total_con_mutex;
struct timeval select_timeout;
sem_t full, empty;

ITEM **buffer;
int buffer_count;

typedef struct lru_node_t{
    struct timeval arrival_time;
    int socket;
    struct lru_node_t* prev;
    struct lru_node_t* next;
} LRU_NODE;

//add lru node to end of the queue
void add_lru_node(LRU_NODE* node, LRU_NODE** head, LRU_NODE** tail);
//remove any lru node
void delete_lru_node(LRU_NODE* node, LRU_NODE** head, LRU_NODE** tail);
//Status Handler Function
void status_handler(int ssock, char* buf);


void* producer_handler(void* ptr);
void* consumer_handler(void* ptr);

void mutex_increment(pthread_mutex_t* mutex_ptr, int* value_ptr, int inc_value){
    pthread_mutex_lock(mutex_ptr);
    (*value_ptr) += inc_value;
    pthread_mutex_unlock(mutex_ptr);
}
void reject_slow_clients(LRU_NODE** head, LRU_NODE** tail, fd_set* fdset, int* nfds);




int main( int argc, char *argv[] ){
        
    char *service;
    struct sockaddr_in fsin;
    socklen_t alen;
    int msock;
    int rport = 0;
    //head and tail of lru queue
    LRU_NODE* head = NULL;
    LRU_NODE* tail = NULL;
    
    LRU_NODE* sock_to_node[4096];
    
    
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
    pthread_mutex_init( &total_mutex, NULL);
    pthread_mutex_init( &total_con_mutex, NULL);
    pthread_mutex_init( &total_prod_mutex, NULL);
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

        select_timeout.tv_sec = 1;
        select_timeout.tv_usec = 0;

        int select_res = select(nfds, &read_fds, (fd_set*) 0, (fd_set*) 0, &select_timeout);
        if (select_res < 0){
			fprintf( stderr, "server select: %s\n", strerror(errno) );
            break;
		}else if(select_res == 0){
            reject_slow_clients(&head, &tail, &dup_fds, &nfds);
            continue;
        }

        if(FD_ISSET(msock, &read_fds)){
            alen = sizeof(fsin);
            int ssock = accept( msock, (struct sockaddr *)&fsin, &alen );
            if (ssock < 0){
                fprintf( stderr, "accept: %s\n", strerror(errno) );
                exit(-1);
            }
            
            if(client_amt + 1 > MAX_CLIENTS){
                printf("Too many clients in the server. Client arrived and got kicked out!\n");
                total_max_rejected++;
                close(ssock);
            }
            else{
                mutex_increment(&total_mutex, &client_amt, 1);
                //Insert new socket
                FD_SET(ssock, &dup_fds);

                if(ssock + 1 > nfds){
                    nfds = ssock + 1;
                }

                LRU_NODE* node = malloc(sizeof(LRU_NODE));
                node -> next = NULL;
                node -> prev = NULL;
                node -> socket = ssock;
                sock_to_node[ssock] = node;
                gettimeofday(&(node -> arrival_time), NULL);
                add_lru_node(node, &head, &tail);
                
            }
        }

        for(int fd = 0; fd < nfds; fd++){

            if(fd != msock && FD_ISSET(fd, &read_fds)){
                
                delete_lru_node(sock_to_node[fd], &head, &tail);

                char should_close = 1;
                
                do{

                    // if(prod_amt + con_amt >= MAX_CLIENTS){
                    //     printf("Too many clients in the server. Client arrived and got kicked out !\n");
                    //     break;
                    // }

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
                            total_prod_rejected++;
                            break;
                        }
                        //CREATE THREAD FOR PRODUCER
                        pthread_t thread;
                        int* ssock = (int*) malloc(sizeof(int));
                        *ssock = fd;
                        if(pthread_create(&thread, NULL, producer_handler, (void *)ssock) != 0){
                            printf("Error creating thread ! PRODUCER got kicket out !\n");
                            break;
                        }
                        mutex_increment(&prod_mutex, &prod_amt, 1);

                    }else if(strcmp(buf, "CONSUME\r\n") == 0){
                        printf("CONSUMER has arrived !\n");
                        fflush(stdout);

                        if(con_amt >= MAX_CON){
                            printf("Too many consumers in the server. CONSUMSER got kicked out !\n");
                            total_con_rejected++;
                            break;
                        }
                        //CREATE THREAD FOR CONSUMER
                        pthread_t thread;
                        int* ssock = (int*) malloc(sizeof(int));
                        *ssock = fd;
                        if(pthread_create(&thread, NULL, consumer_handler, (void *)ssock) != 0){
                            printf("Error creating thread ! CONSUMER got kicket out !\n");
                            break;
                        }
                        mutex_increment(&con_mutex, &con_amt, 1);

                    }
                    else if(strncmp("STATUS", buf, 6) == 0){
                        status_handler(fd, buf);
                        break;
                    }
                    else{
                        printf("Client's identification message is not correct!");
                        break;
                    }

                    //Socket was successfully passed to seperate thread. We won't close this one. 
                    should_close = 0;
                }while(0);

                //If something went wrong, close the socket
                if(should_close){
                    fflush(stdout);
                    mutex_increment(&total_mutex, &client_amt, -1);
                    close(fd);
                }

                //Delete fd from dup_fds, we don't need to monitor this socket anymore (It got deleted or will be handled by its own thread);
                FD_CLR(fd, &dup_fds);

                //Update nfds
                if(nfds == fd + 1)
                    nfds --;
            }   

        }
        reject_slow_clients(&head, &tail, &dup_fds, &nfds);
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
        new_item -> psd = ssock;
        new_item -> size = item_size;
        
        //Put the item into buffer 
        pthread_mutex_lock(&mutex);
        //---------------------------------------------
        buffer[buffer_count] = new_item;
        buffer_count++;
        printf("Item was added; Item count in buffer is %d\n", buffer_count);
        //----------------------------------------------
        pthread_mutex_unlock(&mutex);
        sem_post(&full);
        

        printf("Produced item size: %d\n",item_size);
        fflush(stdout);
    }
    
    free( (int*) ptr);
    
    mutex_increment(&prod_mutex, &prod_amt, -1);
    mutex_increment(&total_mutex, &client_amt, -1);
    mutex_increment(&total_prod_mutex, &total_produced, 1);
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

    //Tell producer consumer is ready for streaming
    write(item -> psd, "GO\r\n", 4);
    
    //Stream item letters
    int bytes_read = 0, cc = 0 ;
    char* temp_buffer[BUFSIZE];
    while(bytes_read < item->size && (cc = read(item -> psd, temp_buffer, BUFSIZE)) > 0){
        write(ssock, temp_buffer, cc);
        bytes_read += cc;
    }

    //End producer
    write(item -> psd, "DONE\r\n", 6);

    printf("Consumed item size: %d\n", item -> size);
    fflush(stdout);

    close(item -> psd);
    free(item);

    close(ssock);
    free( (int*) ptr);

    mutex_increment(&con_mutex, &con_amt, -1);
    mutex_increment(&total_mutex, &client_amt, -1);
    mutex_increment(&total_con_mutex, &total_consumed, 1);
}

void delete_lru_node(LRU_NODE* node, LRU_NODE** head, LRU_NODE** tail){
    if(node -> next != NULL && node -> prev != NULL ){
        node -> next -> prev = node -> prev;
        node -> prev -> next = node -> next;
    }
    else if(node -> next != NULL){
        *head = node -> next; 
        node -> next -> prev = NULL;
    }else if(node -> prev != NULL){
        *tail = node -> prev;
        node -> prev -> next = NULL;
    }else{
        *head = NULL;
        *tail = NULL;
    }
    free(node);
}

void add_lru_node(LRU_NODE* node, LRU_NODE** head, LRU_NODE** tail){
    if(*tail == NULL){
        *head = node;
        *tail = node;
    }else{
        (*tail) -> next = node;
        node -> prev = *tail;
        *tail = node;
    }
}

void reject_slow_clients(LRU_NODE** head, LRU_NODE** tail, fd_set* fdset, int* nfds){
    struct timeval current_time;
    gettimeofday(&current_time, NULL);
    //Reject slow clients
    while(*head != NULL && current_time.tv_sec - ((*head) -> arrival_time).tv_sec >= REJECT_TIME){
        close((*head) -> socket);
        FD_CLR((*head) -> socket, fdset);
        if(*nfds == ((*head) -> socket) + 1)
            *nfds --;
        
        delete_lru_node(*head, head, tail);

        mutex_increment(&total_mutex, &client_amt, -1);
        printf("Client has arrived and didn't identify iteslf, thus got rejected!\n");
        total_slow ++;
    }
}

//PART 5
void status_handler(int ssock, char* buf){
    int res = -1;
    if(strncmp(buf, "STATUS/CURRCLI", 14) == 0){
        res = client_amt; 
    }else if(strncmp(buf, "STATUS/CURRPROD", 15) == 0){
        res = prod_amt;
    }else if(strncmp(buf, "STATUS/CURRCONS", 15) == 0){
        res = con_amt;
    }else if(strncmp(buf, "STATUS/TOTPROD", 14) == 0){
        res = total_produced;
    }else if(strncmp(buf, "STATUS/TOTCONS", 14) == 0){
        res = total_consumed;
    }else if(strncmp(buf, "STATUS/REJMAX", 13) == 0){
        res = total_max_rejected;
    }else if(strncmp(buf, "STATUS/REJSLOW", 14) == 0){
        res = total_slow;
    }else if(strncmp(buf, "STATUS/REJPROD", 14) == 0){
        res = total_prod_rejected;
    }else if(strncmp(buf, "STATUS/REJCONS", 14) == 0){
        res = total_con_rejected;
    }
    char temp[64] = "Unidentified command!";
    if(res != -1){
        sprintf(temp, "%d\r\n", res);
    }
    write(ssock, temp, strlen(temp));
}