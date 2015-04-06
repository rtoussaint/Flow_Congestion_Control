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


#define MAX_PAYLOAD_SIZE 500
#define EOF_PACKET_SIZE 12
#define DATA_PACKET_SIZE 512
#define ACK_PACKET_SIZE 8
#define DATA_PACKET_HEADER_SIZE 12

enum senderState {
	SENDING, WAITING_FOR_EOF_ACK, SENDER_DONE
};
enum receiverState {
	RECEIVING, RECEIVER_DONE
};

struct reliable_state {
	rel_t *next;
	rel_t **prev;
	conn_t *c;

	int receiver_window_size;
	int timeout;

	/*
	 * Sender State
	 * While there is a buffered sender packet, nextPacketToSend represents the seqno of that packet.
	 * When that packet is acked and removed, nextPacketToSend represents the seqno of the next packet
	 * to read and send.
	 */
	packet_t *lastSentPacket;
	int nextPacketToSend;
	struct timespec timeLastSentPacketTransmitted;
	enum senderState sState;

	//Receiver state
	packet_t *lastReceivedPacket;
	int nextPacketToReceive;
	enum receiverState rState;
};


void changePacketToHostByteOrder (packet_t *pkt) {
	if(pkt->seqno)
		pkt->seqno = ntohl (pkt->seqno);
	pkt->len = ntohs (pkt->len);
	pkt->ackno = ntohl (pkt->ackno);
}

void changePacketToNetworkByteOrder (packet_t *pkt) {
	if(pkt->seqno)
		pkt->seqno = htonl (pkt->seqno);
	pkt->len = htons (pkt->len);
	pkt->ackno = htonl (pkt->ackno);
}

void sendDataAcknowledgement(rel_t *r, uint32_t ackno) {
	struct ack_packet *ackPacket;
	ackPacket = xmalloc (ACK_PACKET_SIZE);
	memset((void*) ackPacket, 0, ACK_PACKET_SIZE);
	ackPacket->len = (uint16_t) ACK_PACKET_SIZE;
	ackPacket->ackno = (uint32_t) ackno;

	changePacketToNetworkByteOrder((packet_t*) ackPacket);
	ackPacket->cksum = cksum(ackPacket, ACK_PACKET_SIZE);
	conn_sendpkt(r->c, (packet_t*) ackPacket, ACK_PACKET_SIZE);
	free(ackPacket);
}


bool isReceivedPacketBuffered(rel_t *r) {
	return r->lastReceivedPacket != NULL;
}


bool isSentPacketBuffered(rel_t* r) {
	return r->lastSentPacket != NULL;
}


//Global list of reliable states.
rel_t *rel_list;


rel_t * rel_create (conn_t *c, const struct sockaddr_storage *ss,
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


	//Initialize session state.
	r->timeout = cc->timeout;
	r->receiver_window_size = cc->window;
	r->nextPacketToReceive = 1;
	r->nextPacketToSend = 1;
	r->sState = SENDING;
	r->rState = RECEIVING;

	return r;
}


void
rel_destroy (rel_t *r)
{
	if (r->next)
		r->next->prev = r->prev;
	*r->prev = r->next;
	conn_destroy (r->c);

	free(r);
}


void
rel_demux (const struct config_common *cc,
		const struct sockaddr_storage *ss, packet_t *pkt, size_t len){ }


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


bool isValidAckToBeHandled(rel_t* r, packet_t* pkt) {
	return pkt->len == ACK_PACKET_SIZE &&			//The packet is actually an ack
			isSentPacketBuffered(r) && 				//A packet exists to be acked.
			r->sState != SENDER_DONE &&				//The sender has not received an ack for the EOF
			pkt->ackno == r->nextPacketToSend + 1;	//The ack acks the current packet in the sending window.
}

void handleAck(rel_t* r) {
	free(r->lastSentPacket);
	r->lastSentPacket = NULL;
	r->nextPacketToSend += 1;
	if(r->sState == WAITING_FOR_EOF_ACK) { //Done sending once all packets have been acked.
		r->sState = SENDER_DONE;
		destroyConnectionIfAppropriate(r);
	} else {
		rel_read(r);
	}
}

bool isValidDataPacket(rel_t *r, packet_t* pkt) {
	return r->rState == RECEIVING && 				//Have not received the EOF
			!isReceivedPacketBuffered(r) && 		//There is not already an unacked packet in the sending window.
			pkt->seqno == r->nextPacketToReceive; 	//The data packet is the one we were expecting
}

void clearReceivingWindowAndSendAck(rel_t* r) {
	r->lastReceivedPacket = NULL;
	r->nextPacketToReceive += 1;
	sendDataAcknowledgement(r, r->nextPacketToReceive);
}


void handleEOFDataPacket(rel_t *r) {
	conn_output (r->c, NULL, 0);
	r->rState = RECEIVER_DONE;
	clearReceivingWindowAndSendAck(r);
	destroyConnectionIfAppropriate(r);
}


void
rel_recvpkt (rel_t *r, packet_t *pkt, size_t n)
{
	if((size_t) ntohs(pkt->len) != n || isPacketChecksumInvalid(pkt)) {
		return;
	}

	changePacketToHostByteOrder(pkt);

	if(isValidAckToBeHandled(r, pkt)) {
		handleAck(r);
	}
	else if(isValidDataPacket(r, pkt)) {
		if(pkt->len == EOF_PACKET_SIZE) {
			handleEOFDataPacket(r);
		}
		else {
			r->lastReceivedPacket = pkt;
			rel_output(r);
		}
	}
	else if(pkt->len > ACK_PACKET_SIZE && pkt->seqno < r->nextPacketToReceive) {
		sendDataAcknowledgement(r, r->nextPacketToReceive);
	}
}


void updateTimeLastTransmittedAndSendPacket(rel_t* r) {
	clock_gettime(CLOCK_MONOTONIC, &r->timeLastSentPacketTransmitted);
	conn_sendpkt(r->c, r->lastSentPacket, ntohs (r->lastSentPacket->len));
}


void sendDataPacket(int bytes, rel_t *s) {
	s->lastSentPacket->seqno = (uint32_t) s->nextPacketToSend;
	s->lastSentPacket->len = (uint16_t) ((bytes == -1) ? EOF_PACKET_SIZE : DATA_PACKET_HEADER_SIZE + bytes);
	s->lastSentPacket->ackno = 0;
	changePacketToNetworkByteOrder(s->lastSentPacket);

	s->lastSentPacket->cksum = cksum(s->lastSentPacket, ntohs(s->lastSentPacket->len));

	if(bytes == -1) {
		s->sState = WAITING_FOR_EOF_ACK;
	}
	updateTimeLastTransmittedAndSendPacket(s);
}


void
rel_read (rel_t *s) {
	int conn_stdin_value;
	if(!isSentPacketBuffered(s)) {
		packet_t* packetToSend;
		packetToSend = (packet_t*) xmalloc(sizeof(packet_t));
		memset(packetToSend, 0, sizeof(*packetToSend));
		conn_stdin_value = conn_input(s->c, packetToSend->data, MAX_PAYLOAD_SIZE);

		if(conn_stdin_value != 0) {
			s->lastSentPacket = packetToSend;
			sendDataPacket(conn_stdin_value, s);
		}
		else {
			free(packetToSend);
		}
	}
}


void
rel_output (rel_t *r) {
	int bytesToWrite =  r->lastReceivedPacket->len - DATA_PACKET_HEADER_SIZE;
	if(conn_bufspace(r->c) > bytesToWrite) {
		conn_output(r->c, r->lastReceivedPacket->data, bytesToWrite);
		clearReceivingWindowAndSendAck(r);
	}
}


bool
packetHasTimedOut(struct timespec timeLastTransmitted, int timeout) {
	struct timespec currentTime;
	clock_gettime(CLOCK_MONOTONIC, &currentTime);
	return 1000*(currentTime.tv_sec - timeLastTransmitted.tv_sec) > timeout;
}

bool retransmissionNecessary(rel_t* r) {
	return r->sState != SENDER_DONE && isSentPacketBuffered(r) &&
			packetHasTimedOut(r->timeLastSentPacketTransmitted, r->timeout);
}


void
rel_timer () {
	rel_t *sessionTemp = rel_list;
	while(sessionTemp != NULL){
		if(retransmissionNecessary(sessionTemp)) {
			updateTimeLastTransmittedAndSendPacket(sessionTemp);
		}
		sessionTemp = sessionTemp->next;
	}
}
