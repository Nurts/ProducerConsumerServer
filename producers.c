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
#include <math.h>
#include <prodcon.h>

#define INPUT_MAX_CLIENTS 2000
/*
**	PRODUCER
*/
char* random_string;

void* producer_service(void* val);

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

//Generate Random String
    random_string = malloc(sizeof(char) * MAX_LETTERS);
    for(int i = 0; i < MAX_LETTERS; i++){
        random_string[i] = (random() % 26) + 97;
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
        
        int status = pthread_create(&threads[i], NULL, producer_service, &clients[i]);

        printf("Producer is created\n");
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



int get_response(int csock, char* buf, int bufsize){
    char go_buf[16];
    if ( read( csock, go_buf, bufsize) <= 0 ){
        fprintf(stderr, "The server has gone.\n");
        return -1;
    }
    go_buf[bufsize] = '\0';

    if(strcmp(go_buf, buf) != 0){
        fprintf(stderr, "Didn't get response to GO\n");
        return -1;
    }
    return 0;
}


void* producer_service(void* val){
    struct client_struct *client = (struct client_struct *)val;
    int csock = client -> sock;

    //if bad sleep
    if(client -> is_bad){
        sleep(SLOW_CLIENT);
    }

    //Generate the items
    int item_size = (random() % MAX_LETTERS) + 1;
    
    do{
        //Producer asks for permission  
        if ( write( csock, "PRODUCE\r\n", 9) < 0 ){
            fprintf( stderr, "client write: %s\n", strerror(errno) );
            break;
        }

        //Get reply to GO
        if(get_response(csock, "GO\r\n", 4) < 0){
            break;
        }

        //Send letter count
        int conv = htonl(item_size);

        if( write(csock, (char*) &conv, 4) < 0){
            fprintf( stderr, "client write: %s\n", strerror(errno) );
            break;
        }

        if(get_response(csock, "GO\r\n", 4) < 0){
            break;
        }
        //Stream letters
        int left_bytes = item_size;
        char letters[BUFSIZE];
        while(left_bytes > 0){
            //Bytes to write at this iternation
            int bytes_to_write = ( (BUFSIZE < left_bytes) ? BUFSIZE : left_bytes );

            int written_bytes = write(csock, random_string + (item_size - left_bytes), bytes_to_write);
            if( written_bytes < 0){
                fprintf( stderr, "client write: %s\n", strerror(errno) );
                close(csock);
                pthread_exit(0);
            }

            left_bytes -= written_bytes;
        }
        printf("%d bytes were sent\n", item_size - left_bytes);

        //DONE
        if(get_response(csock, "DONE\r\n", 6) < 0){
            break;
        }

    }while(0);
    
    close(csock);
    pthread_exit(0);
}

