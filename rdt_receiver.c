#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <assert.h>
#include <math.h>

#include "common.h"
#include "packet.h"

tcp_packet *recvpkt;
tcp_packet *sndpkt;

typedef struct {
    int buffer_size; // number of packets the buffer can store
    int max_seqno; // highest seqno received 
    int bf_pointer; // pointer to the gap in buffer
    char *buffered_pkts; // dynamic array of data from out-of-order packets 
}pktBuffer;

pktBuffer* makePcktBuffer(int l)
{
    pktBuffer *rpb;
    rpb = malloc(sizeof(pktBuffer));
    rpb->max_seqno=0;
    rpb->bf_pointer=0;
    rpb->buffer_size = l;
    rpb->buffered_pkts = malloc(rpb->buffer_size*DATA_SIZE);
    bzero(rpb->buffered_pkts, l*DATA_SIZE);
    return rpb;
}

pktBuffer *rcvpktBuffer; 

int max(int a, int b)
{   if(a > b)
        return a;  
    else 
        return b; 
}

int main(int argc, char **argv) {
    int sockfd; /* socket */
    int portno; /* port to listen on */
    int clientlen; /* byte size of client's address */
    struct sockaddr_in serveraddr; /* server's addr */
    struct sockaddr_in clientaddr; /* client addr */
    int optval; /* flag value for setsockopt */
    FILE *fp;
    char buffer[MSS_SIZE];
    char d_buffer[DATA_SIZE];
    bzero(d_buffer, DATA_SIZE);
    struct timeval tp;
    int recv_next = 0;   // pointer to first byte of available receive window

    /* 
     * check command line arguments 
     */
    if (argc != 3) {
        fprintf(stderr, "usage: %s <port> FILE_RECVD\n", argv[0]);
        exit(1);
    }
    portno = atoi(argv[1]);

    fp  = fopen(argv[2], "w");
    if (fp == NULL) {
        error(argv[2]);
    }

    /* 
     * socket: create the parent socket 
     */
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) 
        error("ERROR opening socket");

    optval = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, 
            (const void *)&optval , sizeof(int));

    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serveraddr.sin_port = htons((unsigned short)portno);

    if (bind(sockfd, (struct sockaddr *) &serveraddr, 
                sizeof(serveraddr)) < 0) 
        error("ERROR on binding");

    VLOG(DEBUG, "epoch time, bytes received, sequence number");
    int packetBufferSize = 64;
    rcvpktBuffer = makePcktBuffer(packetBufferSize); //initialize buffer for at most 64 out-of-order packets
    int i;
    int eof_flag = 0;
    clientlen = sizeof(clientaddr);
    while (1) {
        if (recvfrom(sockfd, buffer, MSS_SIZE, 0,
            (struct sockaddr *) &clientaddr, (socklen_t *)&clientlen) < 0) {
                error("ERROR in recvfrom");
        }
        recvpkt = (tcp_packet *) buffer;
        assert(get_data_size(recvpkt) <= DATA_SIZE);
        gettimeofday(&tp, NULL);
        VLOG(DEBUG, "%lu, %d, %d", tp.tv_sec, recvpkt->hdr.data_size, recvpkt->hdr.seqno);
        if(recv_next == recvpkt->hdr.seqno) // only ACK in-order packets
        {
            // terminate receiver after sending ack for last packet 
            if(recvpkt->hdr.ctr_flags == 2) // ctr_flag = 2 is used as FIN 
            {
                VLOG(INFO, "End Of File has been reached");
                sndpkt = make_packet(0);
                sndpkt->hdr.ctr_flags = 2;            
                sndpkt->hdr.ackno = recv_next;
                VLOG(INFO, "Send ACK %d", recv_next);
                if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, 
                    (struct sockaddr *) &clientaddr, clientlen) < 0) {
                    error("ERROR in sendto");
                }
                free(sndpkt);
                break;
            }

            // write data from in-order packwt 
            fseek(fp, recvpkt->hdr.seqno, SEEK_SET);
            fwrite(recvpkt->data, 1, get_data_size(recvpkt), fp);
            recv_next = recvpkt->hdr.seqno+get_data_size(recvpkt);
            rcvpktBuffer->max_seqno = max(rcvpktBuffer->max_seqno, recvpkt->hdr.seqno);

            // write data in the buffer if exists
            i=max(0,rcvpktBuffer->bf_pointer);
            while(rcvpktBuffer->max_seqno > recv_next) // check if there are buffered packets
            {
                if(memcmp(rcvpktBuffer->buffered_pkts+i, d_buffer, DATA_SIZE)==0) // break when there is another gap (=the received packet only partially fills the gap)
                {
                    break;
                }
                fseek(fp, recv_next+i, SEEK_SET);
                if(eof_flag == 1 && recv_next+i >= rcvpktBuffer->max_seqno) // if buffer is at the EOF, break
                {
                    fwrite(rcvpktBuffer->buffered_pkts+i, 1, rcvpktBuffer->max_seqno - recv_next+i, fp);
                    break;
                }else{
                    fwrite(rcvpktBuffer->buffered_pkts+i, 1, DATA_SIZE, fp);
                    recv_next +=DATA_SIZE;
                    i+=DATA_SIZE;
                }
            }
            if(rcvpktBuffer->max_seqno > recv_next) // if gap was only partially filled
            {
                rcvpktBuffer->bf_pointer = i;
            }else{
                bzero(rcvpktBuffer->buffered_pkts, sizeof(char)*rcvpktBuffer->buffer_size*DATA_SIZE);
                rcvpktBuffer->bf_pointer=0;
            }

            // send ACK back to sender accordingly
            sndpkt = make_packet(0);
            if(eof_flag == 1 && rcvpktBuffer->max_seqno == recv_next)
            {
                sndpkt->hdr.ctr_flags = 2;
            }else{
                sndpkt->hdr.ctr_flags = ACK;            
            }
            sndpkt->hdr.data_size = recvpkt->hdr.data_size;
            sndpkt->hdr.ackno = recv_next;
            VLOG(INFO, "Send ACK %d", recv_next);
            if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, 
                (struct sockaddr *) &clientaddr, clientlen) < 0) {
                error("ERROR in sendto");
            }
            free(sndpkt);
            if(eof_flag ==1 && rcvpktBuffer->max_seqno == recv_next) // exit condition 
            {
                break;
            }
        }else if(recv_next < recvpkt->hdr.seqno) // handle buffering of out of order packets
        {
            int byte_offset = recvpkt->hdr.seqno - recv_next;
            int pckt_offset = (int)byte_offset/DATA_SIZE;
            int buffer_pos = (pckt_offset-1)*DATA_SIZE;
            // update the maximum sequence number received if necessary
            rcvpktBuffer->max_seqno=max(recvpkt->hdr.seqno, rcvpktBuffer->max_seqno);

            if(recvpkt->hdr.ctr_flags ==2)
            {
                eof_flag=1;
            }
            // memcmp function is used check whether the out-of-order packet had been received and buffered before
            else if(memcmp(rcvpktBuffer->buffered_pkts+buffer_pos, d_buffer, get_data_size(recvpkt))==0)
            {
                // buffer the data recevied from out-of-order packet 
                memcpy(rcvpktBuffer->buffered_pkts+buffer_pos, recvpkt->data, get_data_size(recvpkt)); 

                // readjust size of dynamic array of packets if neeccessary
                if(pckt_offset > packetBufferSize)  
                {
                    packetBufferSize*=2;
                    rcvpktBuffer->buffer_size = packetBufferSize;
                    rcvpktBuffer->buffered_pkts = realloc(rcvpktBuffer->buffered_pkts, sizeof(char)*packetBufferSize*DATA_SIZE);
                }
            }

            // Resend duplicate ack 
            sndpkt = make_packet(0);
            sndpkt->hdr.ctr_flags = ACK;            
            sndpkt->hdr.data_size = recvpkt->hdr.data_size;
            sndpkt->hdr.ackno = recv_next;
            VLOG(INFO, "SEND DUP %d", recv_next);
            if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, 
                (struct sockaddr *) &clientaddr, clientlen) < 0) {
                error("ERROR in sendto");
            }
            free(sndpkt);   
        }

        bzero(recvpkt, sizeof(*recvpkt));
    }
    close(sockfd);
    free(rcvpktBuffer);
    fclose(fp);
    return 0;
}
