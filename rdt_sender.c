#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <sys/time.h>
#include <time.h>
#include <assert.h>
#include <math.h>

#include"packet.h"
#include"common.h"

#define STDIN_FD    0
#define RETRY  120 //milli second 

//*sudo sysctl -w net.ipv4.ip_forward=1* can be entered into terminal if mahimahi doesn't work


int next_seqno=0;      //pointer to last sent packet 
int send_base=0;		//pointer to last Acked packet

int sockfd, serverlen;
struct sockaddr_in serveraddr;
struct itimerval timer; 
tcp_packet *sndpkt;
tcp_packet *recvpkt;
sigset_t sigmask;     
FILE *fp;

float cwnd = 1.0;
int ssthresh = 64;
int dupacks = 0;
float rtt = 0.0;
int timer_flag = 0;

int max(int a, int b)
{   if(a > b)
        return a;  
    else 
        return b; 
}

void start_timer()
{
    sigprocmask(SIG_UNBLOCK, &sigmask, NULL);
    setitimer(ITIMER_REAL, &timer, NULL);
}

void stop_timer()
{
    sigprocmask(SIG_BLOCK, &sigmask, NULL);
}

void resend_packets(int sig)
{
    if (sig == SIGALRM)
    {
        VLOG(INFO, "Timout happend for pkt %d", send_base);
    }
    if(timer_flag == 1)
    {
        stop_timer();
        timer_flag=0;
    }
    int curr = send_base; //move pointer to lask Acked packet
    fseek(fp, curr, SEEK_SET);
    int len2 = 0;
    char buffer[DATA_SIZE];
    len2 = fread(buffer, 1, DATA_SIZE, fp);

    // handle when EOF is reached while resending packets 
    if (len2 <= 0)
    {
        sndpkt = make_packet(0);
        sndpkt->hdr.seqno = next_seqno;
        sndpkt->hdr.ctr_flags = 2;
        if(sendto(sockfd, sndpkt, TCP_HDR_SIZE,  0,
                (const struct sockaddr *)&serveraddr, serverlen) < 0)
        {
            error("send to");
        }
        free(sndpkt);
    }else{
        // resend the lowest unAcked packet 
        sndpkt = make_packet(len2);
        memcpy(sndpkt->data, buffer, len2);
        sndpkt->hdr.seqno = curr;
        if(sendto(sockfd, sndpkt, TCP_HDR_SIZE + get_data_size(sndpkt), 0, ( const struct sockaddr *)&serveraddr, serverlen) < 0)
            {
                error("sendto");
            }
        free(sndpkt);
        fseek(fp, next_seqno, SEEK_SET);
        ssthresh = max(cwnd/2,2);
        cwnd=1.0;
        start_timer();
        timer_flag=1;
    }
}


/*
 * init_timer: Initialize timeer
 * delay: delay in milli seconds
 * sig_handler: signal handler function for resending unACKed packets
 */
void init_timer(int delay, void (*sig_handler)(int)) 
{
    signal(SIGALRM, resend_packets);
    timer.it_interval.tv_sec = delay / 1000;    // sets an interval of the timer
    timer.it_interval.tv_usec = (delay % 1000) * 1000; 
    timer.it_value.tv_sec = delay / 1000;       // sets an initial value
    timer.it_value.tv_usec = (delay % 1000) * 1000;

    sigemptyset(&sigmask);
    sigaddset(&sigmask, SIGALRM);
}


int main (int argc, char **argv)
{
    struct timeval time_begin, time_stamp;
    long micro_s;
    gettimeofday (&time_begin, NULL);

    int portno, len;
    char *hostname;
    char buffer[DATA_SIZE];
    FILE *cwnd_file;
  	cwnd_file = fopen("cwnd.csv", "w"); 
    /* check command line arguments */
    if (argc != 4) {
        fprintf(stderr,"usage: %s <hostname> <port> <FILE>\n", argv[0]);
        exit(0);
    }
    hostname = argv[1];
    portno = atoi(argv[2]);
    fp = fopen(argv[3], "r");
    if (fp == NULL) {
        error(argv[3]);
    }

    /* socket: create the socket */
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) 
        error("ERROR opening socket");

    /* initialize server server details */
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serverlen = sizeof(serveraddr);

    /* covert host into network byte order */
    if (inet_aton(hostname, &serveraddr.sin_addr) == 0) {
        fprintf(stderr,"ERROR, invalid host %s\n", hostname);
        exit(0);
    }

    /* build the server's Internet address */
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_port = htons(portno);

    assert(MSS_SIZE - TCP_HDR_SIZE > 0);

    //Stop and wait protocol
    init_timer(RETRY, resend_packets);
    int eof_flag = 0;
    int pckts_inflight;
    while (1)
    {
        VLOG (DEBUG, "CWND:  %f ssthresh: %d dupacks: %d", cwnd, ssthresh, dupacks);
        // calculate time_stamp and record the cwnd to cwnd_csv file 
        gettimeofday (&time_stamp, NULL);
        micro_s = (time_stamp.tv_sec - time_begin.tv_sec)*1000000L+time_stamp.tv_usec - time_begin.tv_usec;
        fprintf(cwnd_file, "%ld : %d\n", micro_s / 1000, (int) cwnd);

        // readjust the pointer to the last packet sent after change in cwnd, if necessary 
        if(next_seqno >= send_base + (cwnd*DATA_SIZE) || send_base > next_seqno)
        {
            next_seqno = send_base;
            VLOG(DEBUG, "CHAGED %d %d", next_seqno, send_base);
        }

        // calculate packets inflight (easier for debugging)
        pckts_inflight = (int)((next_seqno - send_base)/DATA_SIZE);
        while(pckts_inflight < cwnd)
        {
            fseek(fp, next_seqno, SEEK_SET);
            bzero(&buffer, sizeof(buffer));
            len = fread(buffer, 1, DATA_SIZE, fp);

            // handle EOF 
            if (len <= 0)
            {
                // send a FIN packet to receiver, indicated by ctr_flags=2
                VLOG(INFO, "End Of File has been reached");
                sndpkt = make_packet(0);
                sndpkt->hdr.seqno = next_seqno;
                sndpkt->hdr.ctr_flags = 2;
                sendto(sockfd, sndpkt, TCP_HDR_SIZE,  0,
                        (const struct sockaddr *)&serveraddr, serverlen);
                free(sndpkt);
                if(timer_flag==0)
                {
                    start_timer();
                    timer_flag=1;
                }
                break;
            }
           
            // read from file and send data packet 
            sndpkt = make_packet(len);
            memcpy(sndpkt->data, buffer, len);
            sndpkt->hdr.seqno = next_seqno;
            VLOG(DEBUG, "Sending packet %d to %s", 
                    next_seqno, inet_ntoa(serveraddr.sin_addr));
            if(sendto(sockfd, sndpkt, TCP_HDR_SIZE + get_data_size(sndpkt), 0, 
                        ( const struct sockaddr *)&serveraddr, serverlen) < 0)
            {
                error("sendto");
            }
            if(timer_flag == 0)
            {
                start_timer();
                timer_flag = 1;
            }
            next_seqno += len;
            pckts_inflight+=1;
            free(sndpkt);
        }

        do{
            if(recvfrom(sockfd, buffer, MSS_SIZE, 0, (struct sockaddr *) &serveraddr, (socklen_t *)&serverlen) < 0)
            {
                error("recvfrom");
            }
            recvpkt = (tcp_packet *)buffer;
            assert(get_data_size(recvpkt) <= DATA_SIZE);
            VLOG(DEBUG, "Received PKT %d", recvpkt->hdr.ackno);
            // check FIN 
            if(recvpkt->hdr.ctr_flags==2)
            {
                eof_flag=1;
                if(timer_flag == 1)
                {
                    stop_timer();
                    timer_flag = 0;
                }
                break;
            }

            // if packet is duplicate ACK 
            if(recvpkt->hdr.ackno == send_base)
            {
                dupacks+=1;
                VLOG(INFO, "dupacks at %d: %d", send_base, dupacks);

                //Fast Retransmit
                if(dupacks == 3)
                {
                    if (timer_flag == 1)
                    {
                        stop_timer();
                        timer_flag = 0;
                    }
                    resend_packets(1);
                    break;
                }
            }else if(recvpkt->hdr.ackno > send_base) // if packet is in-order ACK
            {
                if(send_base == 0)
                {
                    ssthresh = 64; // handle edge case when ssthresh is 1 before receiver is connected to the sender 
                }

                dupacks = 0;
                if(timer_flag == 1)
                {
                    stop_timer();
                    timer_flag=0;
                }

                // increment the cwnd by status(SlowStart or Congestion Control), and by how many packets the latest ACK pckt acknowledged (cumulative ACK)
                int pckts_acked = (int)(recvpkt->hdr.ackno - send_base) / recvpkt->hdr.data_size;
                // Slow Start
                if(cwnd < ssthresh)
                {
                    cwnd +=pckts_acked; 
                }
                else{ // Congestion Control
                    cwnd += pckts_acked/cwnd;
                }
                send_base = recvpkt->hdr.ackno;
                break;
            }
        }while(recvpkt->hdr.ackno >= send_base);
        bzero(recvpkt, sizeof(*recvpkt));

        if(eof_flag == 1)
        {
            VLOG(INFO, "File Transfer Completed");
            break;
        }
    }
    close(sockfd);
    fclose(fp);
    fclose(cwnd_file);
    return 0;
}
