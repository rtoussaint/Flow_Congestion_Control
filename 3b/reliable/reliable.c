
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

uint32_t min(int a, int b);

struct reliable_state {
	rel_t *next;			/* Linked list for traversing all connections */
	rel_t **prev;

	conn_t *c;			/* This is the connection object */

	/* Add your own data fields below this */

	struct config_common *cc;
	uint32_t CongestionWindow;
	uint32_t MaxWindow;
	uint32_t EffectiveWindow;
	uint32_t ssthresh;
};

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
} sliding_window_sender_buffer;

/* Creates a new reliable protocol session, returns NULL on failure.
 * Exactly one of c and ss should be NULL.  (ss is NULL when called
 * from rlib.c, while c is NULL when this function is called from
 * rel_demux.) */

rel_t *rel_list;

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
	r->cc = cc;
	r->CongestionWindow = 1;

	return r;
}

void
rel_destroy (rel_t *r)
{
	conn_destroy (r->c);

	/* Free any other allocated memory here */
}


void
rel_demux (const struct config_common *cc,
		const struct sockaddr_storage *ss,
		packet_t *pkt, size_t len)
{
	//leave it blank here!!!
}

void
rel_recvpkt (rel_t *r, packet_t *pkt, size_t n)
{
	uint32_t AdvertisedWindow = pkt->rwnd;
	r->MaxWindow = min(r->CongestionWindow, AdvertisedWindow);

}


void
rel_read (rel_t *s)
{
	if (!eof) {
		eof = (packet_t *) xmalloc(sizeof(packet_t));
		eof->rwnd = 0;
		eof->seqno = 0;
		eof->len = 12;
	}

	//if already sent EOF to the sender
	//  return;
	//else
	//  send EOF to the sender
	if(s->c->sender_receiver == RECEIVER)
	{
		if (s->eof_sent)
			return;
		else
		{
			conn_sendpkt(s->c, eof, 12);
			s->eof_sent = 1;
		}
	}
	else //run in the sender mode
	{
		//same logic as lab 1
	}
}

void
rel_output (rel_t *r)
{
}

void
rel_timer ()
{
	  /* Retransmit any packets that need to be retransmitted */


}

uint32_t
min(int a, int b) {
	return (a < b) ? a : b;
}
