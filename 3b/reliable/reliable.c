#include <stdio.h>
#include <stdbool.h>
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

#define MAX_PAYLOAD_SIZE 1000
#define EOF_PACKET_SIZE 16
#define DATA_PACKET_SIZE 1016
#define ACK_PACKET_SIZE 12
#define DATA_PACKET_HEADER_SIZE 16

enum senderState {
	SENDING, WAITING_FOR_EOF_ACK, SENDER_DONE
};
enum receiverState {
	RECEIVING, RECEIVER_DONE
};

typedef struct packet_wrapper {
	struct timespec timeSent;
	struct packet_wrapper *next;
	packet_t *pkt;
} packet_wrapper;

typedef struct send_buffer {
	uint32_t max_size;
	uint32_t next_seqno;
	packet_wrapper *head;
	packet_wrapper *tail;
} send_buffer;

typedef struct recv_buffer {
	uint32_t max_size;
	uint32_t next_expected;
	uint32_t adv_window;
	packet_wrapper *head;
	packet_wrapper *tail;
} recv_buffer;

struct reliable_state {

	conn_t *c;			/* This is the connection object */

	/* Add your own data fields below this */
	int receiver_window_size;
	int timeout;

	send_buffer *sWindow;
	enum senderState sState;


	recv_buffer *rWindow;
	enum receiverState rState;
};

void changePacketToHostByteOrder (packet_t *pkt) {
	if(pkt->seqno)
		pkt->seqno = ntohl (pkt->seqno);
	pkt->len = ntohs (pkt->len);
	pkt->ackno = ntohl (pkt->ackno);
	pkt->rwnd = ntohl (pkt->rwnd);
}

void changePacketToNetworkByteOrder (packet_t *pkt) {
	if(pkt->seqno)
		pkt->seqno = htonl (pkt->seqno);
	pkt->len = htons (pkt->len);
	pkt->ackno = htonl (pkt->ackno);
	pkt->rwnd = htonl (pkt->rwnd);
}

void sendDataAcknowledgement(rel_t *r, uint32_t ackno) {
	struct ack_packet *ackPacket;
	ackPacket = xmalloc (ACK_PACKET_SIZE);
	memset((void*) ackPacket, 0, ACK_PACKET_SIZE);
	ackPacket->len = (uint16_t) ACK_PACKET_SIZE;
	ackPacket->ackno = (uint32_t) ackno;
	ackPacket->rwnd = 0; //TODO

	changePacketToNetworkByteOrder((packet_t*) ackPacket);
	ackPacket->cksum = cksum(ackPacket, ACK_PACKET_SIZE);
	conn_sendpkt(r->c, (packet_t*) ackPacket, ACK_PACKET_SIZE);
	free(ackPacket);
}

bool isSendingWindowFull(rel_t *r) {
	return r->sWindow->head != NULL && (ntohl(r->sWindow->tail->pkt->seqno) - ntohl(r->sWindow->head->pkt->seqno)) >= r->sWindow->max_size;
}

rel_t *rel_list;





/* Creates a new reliable protocol session, returns NULL on failure.
 * Exactly one of c and ss should be NULL.  (ss is NULL when called
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
	rel_list = r;

	/* Do any other initialization you need here */
	r->timeout = cc->timeout;
	r->receiver_window_size = cc->window;

	r->sWindow = (send_buffer *) xmalloc(sizeof(send_buffer));
	memset(r->sWindow, 0, sizeof(send_buffer));
	r->sWindow->max_size = cc->window;
	r->sWindow->next_seqno = 1;

	r->rWindow = (recv_buffer *) xmalloc(sizeof(recv_buffer));
	memset(r->rWindow, 0, sizeof(recv_buffer));
	r->rWindow->next_expected = 1;
	r->rWindow->max_size = cc->window;
	r->rWindow->adv_window = cc->window;

	r->sState = SENDING;
	r->rState = RECEIVING;

	return r;
}

void
rel_destroy (rel_t *r)
{
	conn_destroy (r->c);

	/* Free any other allocated memory here */
	free(r->sWindow);
	free(r->rWindow);
	free(r);
}


void
rel_demux (const struct config_common *cc,
		const struct sockaddr_storage *ss,
		packet_t *pkt, size_t len)
{
	//leave it blank here!!!
}

bool
isPacketChecksumInvalid(packet_t* pkt) {
	int checksum = pkt->cksum;
	memset (&(pkt->cksum), 0, sizeof (pkt->cksum));
	return cksum(pkt, ntohs(pkt->len)) != checksum;
}

void destroyConnectionIfAppropriate(rel_t *r) {
	if(r->sState == SENDER_DONE && r->rState == RECEIVER_DONE) {
		rel_destroy(r);
	}
}

bool isSendingWindowEmpty(rel_t *r) {
	return r->sWindow->head == NULL;
}

bool isValidAckToBeHandled(rel_t* r, packet_t* pkt) {
	return pkt->len == ACK_PACKET_SIZE &&				//The packet is actually an ack
			!isSendingWindowEmpty(r) && 					//A packet exists to be acked.
			r->sState != SENDER_DONE &&					//The sender has not received an ack for the EOF
			ntohl(r->sWindow->head->pkt->seqno) < pkt->ackno;	//The ack acks the current packet in the sending window.
}

void handleAck(rel_t* r, packet_t *pkt) {
	packet_wrapper *temp = r->sWindow->head;
	while(temp != NULL && ntohl(temp->pkt->seqno) < pkt->ackno) {
		r->sWindow->head = temp->next;
		free(temp->pkt);
		free(temp);
		temp = r->sWindow->head;
		if(temp == NULL) {
			r->sWindow->tail = NULL;
		}
	}

	if(r->sState == WAITING_FOR_EOF_ACK && isSendingWindowEmpty(r)) { //Done sending once all packets have been acked.
		r->sState = SENDER_DONE;
		destroyConnectionIfAppropriate(r);
	} else {
		rel_read(r);
	}
}

bool isValidDataPacket(rel_t *r, packet_t* pkt) {
	return r->rState == RECEIVING && 				//Have not received the EOF
			pkt->seqno >= r->rWindow->next_expected &&		//There is not already an unacked packet in the sending window.
			pkt->seqno <=  r->rWindow->next_expected + r->rWindow->max_size; 	//The data packet is the one we were expecting
}

void addinorder_recv(rel_t *r, packet_t *pkt)
{
	packet_wrapper *newWrapper;
	newWrapper = (packet_wrapper*) xmalloc(sizeof(packet_wrapper));
	memset(newWrapper, 0, sizeof(packet_wrapper));

	packet_t *newPacket;
	newPacket = (packet_t*) xmalloc(sizeof(packet_t));
	memset(newPacket, 0, sizeof(packet_t));

	memcpy(newPacket, pkt, sizeof(packet_t)); //SOURCE of possible error

	newWrapper->pkt = newPacket;

	packet_wrapper *temp = r->rWindow->head;

	r->rWindow->adv_window = r->rWindow->adv_window - 1;

	if(temp == NULL) {
		r->rWindow->head = r->rWindow->tail = newWrapper;
		return;
	}
	if(pkt->seqno < temp->pkt->seqno) {
		newWrapper->next = temp;
		r->rWindow->head = newWrapper;
		return;
	}

	while(temp != NULL) {
		packet_wrapper *next = temp->next;
		if(temp->pkt->seqno == pkt->seqno) {
			r->rWindow->adv_window = r->rWindow->adv_window + 1;
			free(newWrapper);
			break;
		}
		if(next == NULL) {
			temp->next = newWrapper;
			r->rWindow->tail = newWrapper;
			break;
		}
		if(pkt->seqno < next->pkt->seqno) {
			temp->next = newWrapper;
			newWrapper->next = next;
			break;
		}
		temp = temp->next;
	}
}

void
rel_recvpkt (rel_t *r, packet_t *pkt, size_t n)
{
	if((size_t) ntohs(pkt->len) != n || isPacketChecksumInvalid(pkt)) {
		return;
	}

	changePacketToHostByteOrder(pkt);

	if(isValidAckToBeHandled(r, pkt)) {
		handleAck(r, pkt);
	}
	else if(isValidDataPacket(r, pkt)) {
		addinorder_recv(r, pkt);
		rel_output(r);
	}
}

void sendDataPacket(rel_t* r, packet_wrapper* wrapper) {
	clock_gettime(CLOCK_MONOTONIC, &wrapper->timeSent);
	conn_sendpkt(r->c, wrapper->pkt, ntohs (wrapper->pkt->len));
}

void buildPacket(rel_t *r, int bytes, packet_t *pkt) {
	packet_wrapper *tail = r->sWindow->tail;
	pkt->seqno = (tail) ? (uint32_t) ntohl(tail->pkt->seqno) + 1 : (uint32_t) r->sWindow->next_seqno;
	pkt->len = (uint16_t) ((bytes == -1) ? EOF_PACKET_SIZE : DATA_PACKET_HEADER_SIZE + bytes);
	pkt->ackno = 0;
	pkt->rwnd = 0; //TODO
	changePacketToNetworkByteOrder(pkt);

	pkt->cksum = cksum(pkt, ntohs(pkt->len));
}

packet_wrapper* add_end_to_s_window(rel_t *r, packet_t *pkt) {
	packet_wrapper *newWrapper;
	newWrapper = (packet_wrapper*) xmalloc(sizeof(packet_wrapper));
	memset(newWrapper, 0, sizeof(*newWrapper));
	newWrapper->pkt = pkt;
	if(r->sWindow->head == NULL) {
		r->sWindow->head = r->sWindow->tail = newWrapper;
	} else {
		r->sWindow->tail->next = newWrapper;
		r->sWindow->tail = newWrapper;
	}
	return newWrapper;
}

void
rel_read (rel_t *s)
{
	if(s->c->sender_receiver == RECEIVER)
	{
		//if already sent EOF to the sender
		//  return;
		//else
		//  send EOF to the sender
	}
	else //run in the sender mode
	{
		int conn_stdin_value;
		packet_wrapper* newWrapper;
		packet_t* packetToSend;

		while(!isSendingWindowFull(s) && s->sState == SENDING) {
			packetToSend = (packet_t*) xmalloc(sizeof(packet_t));
			memset(packetToSend, 0, sizeof(packet_t));
			conn_stdin_value = conn_input(s->c, packetToSend->data, MAX_PAYLOAD_SIZE);

			if(conn_stdin_value != 0) {
				buildPacket(s, conn_stdin_value, packetToSend);
				newWrapper = add_end_to_s_window(s, packetToSend);
				sendDataPacket(s, newWrapper);
				if(conn_stdin_value == -1) {
					s->sState = WAITING_FOR_EOF_ACK;
				}
				s->sWindow->next_seqno += 1;
			}
			else {
				free(packetToSend);
				break;
			}
		}
	}
}

void
rel_output (rel_t *r)
{
	packet_wrapper *temp = r->rWindow->head;
	while(temp != NULL && temp->pkt->seqno == r->rWindow->next_expected) {
		int bytesToWrite =  temp->pkt->len - DATA_PACKET_HEADER_SIZE;
		if(conn_bufspace(r->c) > bytesToWrite) {
			conn_output(r->c, temp->pkt->data, bytesToWrite);
			r->rWindow->head = temp->next;

			if(temp->pkt->len == EOF_PACKET_SIZE) {
				r->rState = RECEIVER_DONE;
				destroyConnectionIfAppropriate(r);
			}

			free(temp->pkt);
			free(temp);
			r->rWindow->next_expected += 1;
		}
		else {
			break;
		}
		temp = r->rWindow->head;
	}
	sendDataAcknowledgement(r, r->rWindow->next_expected);
}

bool
packetHasTimedOut(struct timespec timeLastTransmitted, int timeout) {
	struct timespec currentTime;
	clock_gettime(CLOCK_MONOTONIC, &currentTime);
	return 1000*(currentTime.tv_sec - timeLastTransmitted.tv_sec) > timeout;
}

void
rel_timer ()
{
	/* Retransmit any packets that need to be retransmitted */
	rel_t *sessionTemp = rel_list;
	if(sessionTemp->sState != SENDER_DONE){
		packet_wrapper* wrapper = sessionTemp->sWindow->head;
		while(wrapper != NULL) {
			if(packetHasTimedOut(wrapper->timeSent, sessionTemp->timeout)) {
				sendDataPacket(sessionTemp, wrapper);
			}
			wrapper = wrapper->next;
		}
	}
}
