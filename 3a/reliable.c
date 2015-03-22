
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
  - start time = at rel_init()  <-- use clock_gettime()
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

struct reliable_state {
  rel_t *next;			/* Linked list for traversing all connections */
  rel_t **prev;

  conn_t *c;			/* This is the connection object */

  /* Add your own data fields below this */
  sliding_window_sender_buffer *sendingWindow;
  sliding_window_receiver_buffer *receivingWindow;

};
rel_t *rel_list;

//TODO: finish wrapper struct
struct packet_wrapper{
  packet_t *packet;
  int timeLastTransmitted;
}


struct sliding_window_sender_buffer{
  packet_t *lastAcknowledged;
  packet_t *lastSent;
  packet_t *mostRecentAdd;
  int bufferSize;
};

struct sliding_window_receiver_buffer{
  packet_t *lastFrameReceived;
  int largestAcceptableFrame;
  int bufferSize;
};



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

  //CONSIDER: conn_free() in rlib.c
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
  if(pkt->len == 12){
    //ACK
    int recAckNo = pkt->ackno;
    packet_t *lastAcknowledged = r->sendingWindow->lastAcknowledged;
    if(lastAcknowledged == NULL){
      lastAcknowledged = pkt;
    }
    packet_t *tempLastAck = lastAcknowledged;
    while(recAckNo > tempLastAck->ackno){
      tempLastAck = tempLastAck->next;
      tempLastAck->prev = NULL;
      free(lastAcknowledged);
      lastAcknowledged = tempLastAck;
    }
  }
  else{
    //DATA
    //TODO: CALL rel_out
    packet_t lastReceivedPacket = r->receivingWindow->lastFrameReceived;
    int largestAcceptableSeqNo = r->receivingWindow->largestAcceptableFrame;
    if(lastReceivedPacket == NULL){
      //First packet in the link list (haven't seen a packet before this)
      lastReceivedPacket = pkt;
      //TODO: Calculate Window Size
       r->receivingWindow->largestAcceptableFrame = pkt->seqno + WINDOWSIZE;
    }
    else{

      //Receiving Window Exists
      
      if(pkt->seqno == lastReceivedPacket->seqno +1 && pkt->seqno <= r->receivingWindow->largestAcceptableFrame){
        pkt->next = lastReceivedPacket->next;
        pkt->prev = NULL;
        free(lastReceivedPacket);
        lastReceivedPacket = pkt;
        r->receivingWindow->largestAcceptableFrame +=1;
        


        packet_t *temp;
        while(lastReceivedPacket->next != NULL){
          temp = lastReceivedPacket->next;
          if(lastReceivedPacket->seqno == temp->seqno - 1){
            temp->prev = NULL;
            free(lastReceivedPacket);
            lastReceivedPacket = temp;
            r->receivingWindow->largestAcceptableFrame +=1;
          }
          else{
            break;
          }
        }
        //Send Ack for updates
        ack_packet *ack = (ack_packet*) malloc(sizeof(ack_packet));
        ack->cksum = 0; //TODO: Implement Checksum
        ack->len = 12;
        ack->ackno = lastReceivedPacket->seqno+1;

        conn_sendpkt(r->c, ack, ack->len);
      }
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
void
rel_read (rel_t *s)
{
  char buffer[1000];
  while(1){
    int conn_stdin_value = conn_input(s->c, buffer, 1000);

    if(conn_stdin_value <= 0){
      break;
    }
    else{
      packet_t *newPacket = (packet_t*) malloc(sizeof(packet_t));
      

      if(s->sendingWindow->mostRecentAdd == NULL){
        newPacket->seqno = 0;
        s->sendingWindow->mostRecentAdd = newPacket;
      }
      else{
        newPacket->seqno = s->mostRecentAdd->seqno + 1;
      }
      
      newPacket->len = strln(buffer) + sizeof(packet_t); //TODO: maybe subtract 500 for data built in
      strcpy(newPacket->data, buffer);
      
      //TODO: make this a method
      s->sendingWindow->mostRecentAdd->next = newPacket;
      newPacket->prev = s->sendingWindow->mostRecentAdd;
      s->sendingWindow->mostRecentAdd = newPacket;
      s->sendingWindow->lastSent = newPacket;

      conn_sendpkt(s->c, newPacket, newPacket->len);
    }
  }

  return;
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
 size_t availableSpace = conn_bufspace(r->c);
 size_t bytesWritten = 0;
 rel_t data = r->c->rel;

 while(__Bytes still to write ____ && (bytesWritten < availableSpace)){
    //int conn_output (conn_t *c, const void *_buf, size_t _n)
    conn_output(connection)

    bytesWritten += _____Bytes in packet____;
}
 

}


/* This function is called periodically (default is 1/5 retransmission interval)
 * This function should inspect packets and retransmit packets that have NOT been ACKed
 * 
 * DO NOT retransmit every packet, every time the time is fired!
 * ^^Keep track of which packets need to be retransmitted and when
 *
 * can use clock_gettime() in rlib.c
*/
void
rel_timer ()
{
  /* Retransmit any packets that need to be retransmitted */

}
