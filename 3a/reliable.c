#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stddef.h>
#include <assert.h>
#include <poll.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include "rlib.h"


/*
SENDER:
- Read in a file generated, break it into 1000 byte packets, add a control hearder to packet
- Send file to the receiver through congested link
- Print out the amount of time to transfer the file
- start time = at rel_init() <-- use clock_gettime()
- finish time = at rel_destroy()
RECEIVER:
- Read the packets being send
- Write data to output file
 **Make sure that the file received is identical to the file sent**
^^use diff to check this
- Send ACKs back
Relayer = bottleneck of the network (multiple hosts trying to send across)
- set parameters in config.xml
Congestion Control Algorithm = TCP-Reno (slow-start and congestion avoidance)
PACKET INFORMATION:
You can tell ACK from Data packet by the size
ACK size = 12 bytes
Data size = 16 - 1016 bytes
len, seqno, ackno use BIG ENDIAN
^^Use htonl/htons for WRITES
^^Use ntohl/ntohs for READS
 */
/* AT THE BEGINNING OF THE PROGRAM
//TODO:
1. receiver should send the sender an EOF to tell the sender that it does not have any data to send
- consider using conn_sendpkt() in rlib.c
 */
#define MAX_BUFFER_SIZE 2000 //TODO: change this value

static const int MAX_PACKET_SIZE = 500;


/**
 * Packet wrapper structure that contains the packet, pointers to the previous
 * and next packet, and the time the packet was last transmitted.
 */
typedef struct packet_wrapper {
	packet_t *packet;
	struct packet_wrapper *next;
	struct packet_wrapper *prev;
	struct timespec *timeLastSent;
} packet_wrapper;


/**
 * The sender sliding window starts with the last acknowledged packet.
 * It also features the last sent packet and the packet that was most recently
 * added from the buffer (using conn_input).
 */
typedef struct sliding_window_sender_buffer {
	packet_wrapper *firstUnackedPacket; //head of the list, has not been acked yet...once acked, it is freed.
	packet_wrapper *mostRecentAdd;
	int nextSeqno;
	int eofReached; //initialized to 0 and then set to 1 when EOF reached.
} sliding_window_sender_buffer;


/**
 * The receiver sliding window begins with the last frame received. This
 * packet has not been written yet. The largest Acceptable frame is the
 * maximum packet seqno for which the packet can be added to the receiver
 * sliding window.
 */
typedef struct sliding_window_receiver_buffer{
	packet_wrapper *firstUnwrittenPacket;
	int nextSeqnoToWrite;
	int largestAcceptableFrame;
	int eofReceived; //starts at 0 and becomes 1 when an EOF is received from the other node.
} sliding_window_receiver_buffer;


/**
 * rel_t is a wrapper around a connection. We added a receiver_window_size which
 * represents the maximum number of packets that can be present in the receiving
 * window. The structure also contains a sending window and receiving window as
 * well as a pointer cc to the config_common structure which has general information
 * about the program and connections.
 */
struct reliable_state {
	rel_t *next; /* Linked list for traversing all connections */
	rel_t **prev;
	conn_t *c; /* This is the connection object */
	int receiver_window_size;
	int timeout;
	sliding_window_sender_buffer *sendingWindow;
	sliding_window_receiver_buffer *receivingWindow;
};


//Linked list of reliable state sessions.
rel_t *rel_list;


/* Creates a new reliable protocol session, returns NULL on failure.
 * Exactly one of c and ss should be NULL. (ss is NULL when called
 * from rlib.c, while c is NULL when this function is called from
 * rel_demux.) */
rel_t *
rel_create (conn_t *c, const struct sockaddr_storage *ss,
		const struct config_common *cc)
{
	rel_t *r;
	r = xmalloc (sizeof (*r));
	memset (r, 0, sizeof (*r));
	if (!c) {
		c = conn_create (r, ss);
		if (!c) {
			free (r);
			return NULL;
		}
	}
	r->c = c;
	r->next = rel_list;
	r->prev = &rel_list;
	if (rel_list)
		rel_list->prev = &r->next;
	rel_list = r;

	r->receivingWindow = (sliding_window_receiver_buffer*) malloc(sizeof(sliding_window_receiver_buffer));
	r->sendingWindow = (sliding_window_sender_buffer*) malloc(sizeof(sliding_window_sender_buffer));
	memset(r->receivingWindow, 0, sizeof(sliding_window_receiver_buffer));
	memset(r->sendingWindow, 0, sizeof(sliding_window_sender_buffer));


	r->receivingWindow->largestAcceptableFrame = cc->window;
	r->receivingWindow->nextSeqnoToWrite = 1;
	r->receivingWindow->eofReceived = 0;
	r->sendingWindow->nextSeqno = 1;
	r->sendingWindow->eofReached = 0;
	r->timeout = cc->timeout;
	r->receiver_window_size = cc->window;
	return r;
}


/**
 * Free all the packets in a window. This function is called
 * in the rel_destroy function.
 */
void free_packets_in_window(packet_wrapper *head) {
	packet_wrapper *temp = head;
	packet_wrapper *tempToFree;
	while(temp != NULL) {
		tempToFree = temp;
		temp = temp->next;
		free(tempToFree);
	}
}


/* The library will call rel_destroy when it receives an ICMP port unreachable
 * Also called when ALL of the following hold:
 * 1. Read an EOF from the other side (a data packet with 0 bytes payload field)
 * 2. Read an EOF or error from the input (conn_input returned -1)
 * 3. All packets you send have been acknowledged
 * 4. You have written all output data with conn_output
 *
 * One side must wait 2*MSS before destroying the connection (incase the last ACK gets lost)
 * both the sender and the receiver should be able to close the connection
 */
void
rel_destroy (rel_t *r)
{
	if (r->next)
		r->next->prev = r->prev;
	*r->prev = r->next;
	conn_destroy (r->c);
	/* Free any other allocated memory here */
	free_packets_in_window(r->sendingWindow->firstUnackedPacket); //free sending window packets.
	free_packets_in_window(r->receivingWindow->firstUnwrittenPacket); //free receiving window packets.
	free(r->receivingWindow);
	free(r->sendingWindow);
	//CONSIDER: conn_free() in rlib.c
}


/* This function only gets called when the process is running as a
 * server and must handle connections from multiple clients. You have
 * to look up the rel_t structure based on the address in the
 * sockaddr_storage passed in. If this is a new connection (sequence
 * number 1), you will need to allocate a new conn_t using rel_create
 * (). (Pass rel_create NULL for the conn_t, so it will know to
 * allocate a new connection.)
 *
 * Possibly not necessary for 3a or 3b!!!
 *
 */
void
rel_demux (const struct config_common *cc, const struct sockaddr_storage *ss, packet_t *pkt, size_t len)
{
	// rel_t *state = rel_list;
	// while(state != NULL) {
	// if(addreq(ss, &(state->c->peer)) == 1) {
	// rel_recvpkt(state, pkt, len);
	// return;
	// }
	// state = state->next;
	// }
	// rel_create(NULL, ss, cc);
}


/**
 * Sends an acknowledgment packet on the given reliable state session's connection.
 */
void sendDataAcknowledgement(rel_t *r) {
	packet_wrapper *temp = r->receivingWindow->firstUnwrittenPacket;
	while(temp != NULL) {
		packet_wrapper *nextPacket = temp->next;
		if(nextPacket != NULL && temp->packet->seqno == nextPacket->packet->seqno - 1) {
			temp = nextPacket;
		} else {
			packet_t *ackPacket = (packet_t*) malloc(sizeof(packet_t));
			memset(ackPacket, 0, sizeof(packet_t));
			ackPacket->len = 12;
			ackPacket->cksum = cksum(ackPacket, ackPacket->len);
			ackPacket->ackno = temp->packet->seqno + 1;
			conn_sendpkt(r->c, ackPacket, ackPacket->len);
			//			printf("Sending ack for packet %d\n", ackPacket->ackno - 1);
			break;
		}
	}
}

void destroyOnConditionsSatisfied(rel_t *r) {
	if(r->sendingWindow->mostRecentAdd == NULL && r->sendingWindow->eofReached == 1 && r->receivingWindow->eofReceived) {
		rel_destroy(r);
	}
}


/* This function is called by the library when a packet is received
 * and supplied you with the packet
 */
void
rel_recvpkt (rel_t *r, packet_t *pkt, size_t n)
{
	if(pkt->len == 12 && pkt->ackno != 0){ //ACK
		packet_wrapper *temp = r->sendingWindow->firstUnackedPacket; //head packet cannot be NULL - otherwise there would be no ACK
		packet_wrapper *nextPacket;
		while(temp != NULL && temp->packet->seqno <= pkt->ackno - 1) {
			nextPacket = temp->next;
			free(temp);
			temp = nextPacket;
		}
		r->sendingWindow->firstUnackedPacket = temp;

		destroyOnConditionsSatisfied(r); //its possible that all packets have now been acked.
	}
	else if(pkt->len == 12) { //Received EOF from the other node.
		r->receivingWindow->eofReceived = 1;
		destroyOnConditionsSatisfied(r);
	}
	else {

		packet_wrapper *firstUnwrittenPacket = r->receivingWindow->firstUnwrittenPacket;
		int nextSeqno = r->receivingWindow->nextSeqnoToWrite;
		int largestAcceptableSeqNo = r->receivingWindow->largestAcceptableFrame;

		if(pkt->seqno >= nextSeqno && pkt->seqno <= largestAcceptableSeqNo) {
			if(firstUnwrittenPacket == NULL){
				r->receivingWindow->firstUnwrittenPacket = (packet_wrapper*) malloc(sizeof(packet_wrapper));
				memset(r->receivingWindow->firstUnwrittenPacket, 0, sizeof(packet_wrapper));
				r->receivingWindow->firstUnwrittenPacket->packet = pkt;
			}
			else {
				packet_wrapper *temp = firstUnwrittenPacket;
				while(temp != NULL) {
					packet_wrapper *previousPacket = temp->prev;
					if(pkt->seqno == temp->packet->seqno) { //packet already exists.
						break;
					}
					else if(pkt->seqno < temp->packet->seqno) {
						packet_wrapper *newPacket = (packet_wrapper*) malloc(sizeof(packet_wrapper));
						memset(newPacket, 0, sizeof(packet_wrapper));
						newPacket->packet = pkt;
						newPacket->prev = temp->prev;
						newPacket->next = temp;
						temp->prev = newPacket;
						if(temp == firstUnwrittenPacket) {
							r->receivingWindow->firstUnwrittenPacket = newPacket;
						} else if(previousPacket) {
							previousPacket->next = temp;
						}
						break;
					}
					else if(temp->next == NULL) {
						packet_wrapper *newPacket = (packet_wrapper*) malloc(sizeof(packet_wrapper));
						memset(newPacket, 0, sizeof(packet_wrapper));
						newPacket->packet = pkt;
						temp->next = newPacket;
						newPacket->prev = temp;
						break;
					}
					else {
						temp = temp->next;
					}
				}
			}
			sendDataAcknowledgement(r);
			rel_output(r); //write packets to standard output.
		}
	}
}


/* To get the data that you need inorder to send, keep calling conn_input
 * --> int conn_input (conn_t *c, void *buf, size_t n)
 *
 * If conn_input returns 0, return from rel_read (do not loop)
 * When data becomes available at a later moment, the libary will call rel_read ONCE
 *
 * When SENDER buffer becomes full, break from the loop (even if more data is available from conn_input)
 *
 * When ACKS are received later and space becomes available in window, call rel_read again
 */
char buffer[500];

void
rel_read (rel_t *s)
{
	int conn_stdin_value;
	packet_wrapper *newPacketWrapper;
	packet_t *newPacket;
	int count = 0;


	while(1){
		memset(buffer, 0, MAX_PACKET_SIZE);
		conn_stdin_value = conn_input(s->c, buffer, MAX_PACKET_SIZE);

		//		memset(buffer, 0, MAX_PACKET_SIZE);
		//		strcpy(buffer, "hello world");
		//		conn_stdin_value = 11;
		//		if(count == 1) {
		//			break;
		//		}
		//		count++;

		if(conn_stdin_value == 0){ //no data read so break out of loop.
			break;
		}
		else if(conn_stdin_value == -1) { //EOF read so tear down connection.
			newPacketWrapper = (packet_wrapper*) malloc(sizeof(packet_wrapper));
			memset(newPacketWrapper, 0, sizeof(packet_wrapper));
			newPacket = (packet_t*) malloc(sizeof(packet_t));
			newPacket->len = 12;
			conn_sendpkt(s->c, newPacket, newPacket->len);

			s->sendingWindow->eofReached = 1;
			destroyOnConditionsSatisfied(s);
			break;
		}
		else {
			//TODO impose sending window size
			newPacketWrapper = (packet_wrapper*) malloc(sizeof(packet_wrapper));
			memset(newPacketWrapper, 0, sizeof(packet_wrapper));
			newPacket = (packet_t*) malloc(sizeof(packet_t));
			memset(newPacket, 0, sizeof(packet_t));
			newPacketWrapper->packet = newPacket;
			newPacketWrapper->timeLastSent = (struct timespec *) malloc(sizeof(struct timespec));
			clock_gettime(CLOCK_MONOTONIC, newPacketWrapper->timeLastSent);
			newPacket->len = 516;
			strncpy(newPacket->data, buffer, 30);
			newPacket->seqno = s->sendingWindow->nextSeqno;
			newPacket->cksum = cksum(newPacket, newPacket->len);

			s->sendingWindow->nextSeqno += 1;
			if(s->sendingWindow->mostRecentAdd == NULL){
				s->sendingWindow->mostRecentAdd = s->sendingWindow->firstUnackedPacket = newPacketWrapper;
			}
			else{
				s->sendingWindow->mostRecentAdd->next = newPacketWrapper;
				newPacketWrapper->prev = s->sendingWindow->mostRecentAdd;
				s->sendingWindow->mostRecentAdd = newPacketWrapper;
			}
			//			printf("Sending data packet %d\n", newPacket->seqno);
			conn_sendpkt(s->c, newPacket, newPacket->len);
		}
	}
}


/* This function is called by the library when output has drained
 * Call conn_output, it writes to STDOUT
 * conn_bufspace is useful --> tells how much space is availale for use by conn_output
 * DO NOT WRITE MORE THAN conn_bufspace Bytes
 *
 * If you are unable to output certain bytes to STDOUT, do NOT ACK them back to the sender
 * If you have more bytes to output, wait until the library calls rel_out again and then
 * call conn_bufspace to see how much additional space is available
 *
 * Send more ACKS for the bytes that have been written to STDOUT
 * ^^ Sender will also send more data once ACKs are received
 */
void
rel_output (rel_t *r)
{
	packet_wrapper *temp = r->receivingWindow->firstUnwrittenPacket;
	while(temp != NULL && temp->packet->seqno == r->receivingWindow->nextSeqnoToWrite && conn_bufspace(r->c) > 1000) {
		conn_output(r->c, temp->packet->data, strlen(temp->packet->data));
		r->receivingWindow->nextSeqnoToWrite += 1;
		r->receivingWindow->largestAcceptableFrame += 1;
		r->receivingWindow->firstUnwrittenPacket = temp->next;
		free(temp);
		temp = r->receivingWindow->firstUnwrittenPacket;
	}
}


/*
 * Retransmit packets that timeout. Does not send ACKS!!!
 */
void
rel_timer ()
{
	/* Retransmit any packets that need to be retransmitted */
	rel_t *sessionTemp = rel_list;
	while(sessionTemp != NULL){
		packet_wrapper *packetWrapperTemp = sessionTemp->sendingWindow->firstUnackedPacket;
		struct timespec currentTime;
		clock_gettime(CLOCK_MONOTONIC, &currentTime);
		while(packetWrapperTemp != NULL){
			if(1000*(currentTime.tv_sec - packetWrapperTemp->timeLastSent->tv_sec) > sessionTemp->timeout){ //timeout
				conn_sendpkt(sessionTemp->c, packetWrapperTemp->packet, packetWrapperTemp->packet->len);
				clock_gettime(CLOCK_MONOTONIC, packetWrapperTemp->timeLastSent);
			}
			packetWrapperTemp = packetWrapperTemp->next;
		}
		sessionTemp = sessionTemp->next;
	}
}

