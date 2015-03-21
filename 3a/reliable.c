
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


/*
TODO:
- read through requirements section

*/



struct reliable_state {
  rel_t *next;			/* Linked list for traversing all connections */
  rel_t **prev;

  conn_t *c;			/* This is the connection object */

  /* Add your own data fields below this */

};
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
  r->next = rel_list;
  r->prev = &rel_list;
  if (rel_list)
    rel_list->prev = &r->next;
  rel_list = r;

  /* Do any other initialization you need here */


  return r;
}

/* The library will call rel_destroy when it receives an ICMP port unreachable
 * Also called when ALL of the following hold:
 * 1. Read an EOF from the other side (a data packet with 0 bytes payload field)
 * 2. Read an EOF or error from the input (conn_input returned -1) 
 * 3. All packets you send have been acknowledged
 * 4. You have written all output data with conn_output
 *
 * One side must wait 2*MSS before destroying the connection (incase the last ACK gets lost)
*/
void
rel_destroy (rel_t *r)
{
  if (r->next)
    r->next->prev = r->prev;
  *r->prev = r->next;
  conn_destroy (r->c);

  /* Free any other allocated memory here */
}


/* This function only gets called when the process is running as a
 * server and must handle connections from multiple clients.  You have
 * to look up the rel_t structure based on the address in the
 * sockaddr_storage passed in.  If this is a new connection (sequence
 * number 1), you will need to allocate a new conn_t using rel_create
 * ().  (Pass rel_create NULL for the conn_t, so it will know to
 * allocate a new connection.)
 */
void
rel_demux (const struct config_common *cc, const struct sockaddr_storage *ss, packet_t *pkt, size_t len)
{



  //New Connection
  

}

/* This function is called by the library when a packet is received 
 * and supplied you with the packet
 *
*/
void
rel_recvpkt (rel_t *r, packet_t *pkt, size_t n)
{


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
void
rel_read (rel_t *s)
{

}



/* This function is called by the library when output has drained 
 * Call conn_output it writes to STDOUT
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

}


/* This function is called periodically (default is 1/5 retransmission interval)
 * This function should inspect packets and retransmit packets that have NOT been ACKed
 * 
 * DO NOT retransmit every packet, every time the time is fired!
 * ^^Keep track of which packets need to be retransmitted and when
*/
void
rel_timer ()
{
  /* Retransmit any packets that need to be retransmitted */

}
