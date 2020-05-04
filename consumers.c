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
#include <fcntl.h>
#include <math.h>
#include <prodcon.h>
/*
**	CONSUMER
*/
#define INPUT_MAX_CLIENTS 2000
#define DEV_FILE "/dev/null" 
void* consumer_service(void* val);

/*
**      Poisson interarrival times. Adapted from various sources
**      r = desired arrival rate
*/
double poissonRandomInterarrivalDelay( double r )
{
    return (log((double) 1.0 - 
			((double) rand())/((double) RAND_MAX)))/-r;
}

// Struct needed for passing arguments through pthread_create
struct client_struct{
    int sock;
    _Bool is_bad;
};

int main( int argc, char *argv[] ){
	
    char *service;		
    char *host = "localhost";
    int client_number, bad;
    double rate;
    
// input processing
    if(argc == 5){
        service = argv[1];
        client_number = atoi(argv[2]);
        rate = atof(argv[3]);
        bad = atoi(argv[4]);
    }else if(argc == 6){
        host = argv[1];
        service = argv[2];
        client_number = atoi(argv[3]);
        rate = atof(argv[4]);
        bad = atoi(argv[5]);
    }else{
        fprintf( stderr, "usage: [host] port num rate bad\n" );
		exit(-1);
    }
    
    if(rate <= 0){
        fprintf( stderr, "rate should be floating point number greater than 0\n");
        exit(-1);
    }
    if(bad < 0 || bad > 100){
        fprintf( stderr, "bad should be interger between 0 and 100\n");
        exit(-1);
    }
    if(client_number > INPUT_MAX_CLIENTS){
        fprintf( stderr, "Max number of clients should be %d\n", MAX_CLIENTS);
        exit(-1);
    }


    struct client_struct clients[client_number];
//-----------------------------------------------------------------------------------
// BAD client assignment

    for(int i = 0; i < client_number; i++){
        clients[i].is_bad = 0;
    }

    int bad_client_num = client_number * bad / 100, rand_size = client_number;
    printf("Number of slow clients: %d\n", bad_client_num);

    while(bad_client_num > 0){
        int rand_pos = random() % rand_size;
        // printf("pos: %d \n", rand_pos);

        for(int i = 0; i < client_number; i++){
            if(clients[i].is_bad){
                continue;
            }
            if(rand_pos == 0){
                clients[i].is_bad = 1;
                break;
            }
            rand_pos --;
        }

        rand_size --;
        bad_client_num --;
    }

//------------------------------------------------------------------------------------
    
    pthread_t threads[client_number];


    for(int i = 0; i < client_number; i++){

        double time_to_wait = poissonRandomInterarrivalDelay(rate);

        printf("Time to wait next consumer: %f seconds\n", time_to_wait);

        usleep( (int)(time_to_wait * 1000000) );
        
        if ( ( clients[i].sock = connectsock( host, service, "tcp" )) == 0 ){
            fprintf( stderr, "Cannot connect to server.\n" );
            exit( -1 );
        }
        fflush( stdout );
        
        int status = pthread_create(&threads[i], NULL, consumer_service, &clients[i]);

        printf("Consumer is created\n");
        fflush( stdout );

        if(status != 0){
            printf("Error creating thread");
            exit(-1);
        }
    }

    for(int i = 0; i < client_number; i++){
        pthread_join(threads[i], NULL);
    }
    pthread_exit(0);
}


void* consumer_service(void* val){
    struct client_struct *client = (struct client_struct *)val;
    int csock = client -> sock;
    int item_size = 0;
    char message[BUFSIZE] = REJECT;

    //if bad sleep
    if(client -> is_bad){
        sleep(SLOW_CLIENT);
    }

    //Generate the items
    
    do{
        //Consumer asks for permission
        if ( write( csock, "CONSUME\r\n", 9) < 0 ){
            fprintf( stderr, "client write: %s\n", strerror(errno) );
            break;
        }

        //Read letter count
        int ret = 0;
        if ( read( csock, (char*) &ret, 4) <= 0 ){
            fprintf(stderr, "The server has gone.\n");
            break;
        }
        item_size = ntohl(ret);

        //Read the letters
        int fd_null = open(DEV_FILE, O_WRONLY);
        char temp_buffer[BUFSIZE];
        int cc = 0, bytes_read = 0;
        while(bytes_read < item_size  && (cc = read(csock, temp_buffer, BUFSIZE)) > 0){
            bytes_read += cc;
            write(fd_null, temp_buffer, cc);
        }
        printf("Consumed item size: %d\n", bytes_read);
        close(fd_null);

        if(bytes_read == item_size){
            sprintf(message, "%s %d", SUCCESS, bytes_read);
        }else if(bytes_read != item_size){
            sprintf(message, "%s %d", BYTE_ERROR, bytes_read);
        }
    }while(0);

    char filename[128];
    sprintf(filename, "%ld.txt", pthread_self());
    int fd = open(filename, O_WRONLY | O_CREAT, 0777);
    if(write(fd, message, strlen(message)) < 0){
        fprintf(stderr, "Couldn't write to file");
    }

    close(fd);
    close(csock);
    pthread_exit(0);
}
