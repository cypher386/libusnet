#include <arpa/inet.h>

#include "usnet_socket.h"
#include "usnet_udp.h"
#include "usnet_in.h"
#include "usnet_eth.h"
#include "usnet_ip_out.h"
#include "usnet_in_pcb.h"
#include "usnet_common.h"

u_long g_udp_sendspace = 9216;      /* really max datagram size */
u_long g_udp_recvspace = 40 * (1024 + sizeof(struct usn_sockaddr_in));
u_long g_sb_max = SB_MAX;     /* patchable */
int    g_fds_idx;
struct usn_socket* g_fds[MAX_SOCKETS];

void usnet_socket_init()
{
   int i;
   g_fds_idx = 0;
   for (i=0; i<MAX_SOCKETS; i++)
      g_fds[i] = 0;
}

/*
 * Allot mbufs to a sockbuf.
 * Attempt to scale mbmax so that mbcnt doesn't become limiting
 * if buffering efficiency is near the normal case.
 */
int
sbreserve(sb, cc)
   struct sockbuf *sb;
   u_long cc;
{

   if (cc > g_sb_max * MCLBYTES / (MSIZE + MCLBYTES))
      return (0);
   sb->sb_hiwat = cc;
   sb->sb_mbmax = min(cc * 2, g_sb_max);
   if (sb->sb_lowat > sb->sb_hiwat)
      sb->sb_lowat = sb->sb_hiwat;
   return (1);
}

/*
 * Free mbufs held by a socket, and reserved mbuf space.
 */
void
sbrelease(sb)
   struct sockbuf *sb;
{
   // FIXME: 
   //sbflush(sb);
   sb->sb_hiwat = sb->sb_mbmax = 0;
}
/*
 * Socket buffer (struct sockbuf) utility routines.
 *
 * Each socket contains two socket buffers: one for sending data and
 * one for receiving data.  Each buffer contains a queue of mbufs,
 * information about the number of mbufs and amount of data in the
 * queue, and other fields allowing select() statements and notification
 * on data availability to be implemented.
 *
 * Data stored in a socket buffer is maintained as a list of records.
 * Each record is a list of mbufs chained together with the m_next
 * field.  Records are chained together with the m_nextpkt field. The upper
 * level routine soreceive() expects the following conventions to be
 * observed when placing information in the receive buffer:
 *
 * 1. If the protocol requires each message be preceded by the sender's
 *    name, then a record containing that name must be present before
 *    any associated data (mbuf's must be of type MT_SONAME).
 * 2. If the protocol supports the exchange of ``access rights'' (really
 *    just additional data associated with the message), and there are
 *    ``rights'' to be received, then a record containing this data
 *    should be present (mbuf's must be of type MT_RIGHTS).
 * 3. If a name or rights record exists, then it must be followed by
 *    a data record, perhaps of zero length.
 *
 * Before using a new socket structure it is first necessary to reserve
 * buffer space to the socket, by calling sbreserve().  This should commit
 * some of the available buffer space in the system buffer pool for the
 * socket (currently, it does nothing but enforce limits).  The space
 * should be released by calling sbrelease() when the socket is destroyed.
 */
int
soreserve(struct usn_socket *so, u_long sndcc, u_long rcvcc)
{

   if (sbreserve(&so->so_snd, sndcc) == 0)
      goto bad;
   if (sbreserve(&so->so_rcv, rcvcc) == 0)
      goto bad2;
   if (so->so_rcv.sb_lowat == 0)
      so->so_rcv.sb_lowat = 1;
   if (so->so_snd.sb_lowat == 0)
      so->so_snd.sb_lowat = MCLBYTES;
   if (so->so_snd.sb_lowat > so->so_snd.sb_hiwat)
      so->so_snd.sb_lowat = so->so_snd.sb_hiwat;
   return (0);
bad2:
   sbrelease(&so->so_snd);
bad:
   return -1;// no buffer available: (ENOBUFS);
}

// functions on tcp-related socket.
static void
udp_detach(struct inpcb *inp)
{
   if (inp == g_udp_last_inpcb)
      g_udp_last_inpcb = &g_udb;
   in_pcbdetach(inp);
}

int
udp_usrreq(struct usn_socket *so, int req,
           usn_mbuf_t *m, usn_mbuf_t *addr, usn_mbuf_t *control)
{
   struct inpcb *inp = sotoinpcb(so);
   int error = 0;
   int s;

   (void)s;

   // FIXME: do we need?
   //if (req == PRU_CONTROL)
   //   return (in_control(so, (u_long)m, (caddr_t)addr,
   //      (struct ifnet *)control));

   if (inp == NULL && req != PRU_ATTACH) {
      error = EINVAL;
      goto release;
   }   
   /*  
    * Note: need to block udp_input while changing
    * the udp pcb queue and/or pcb addresses.
    */
   switch (req) {

   case PRU_ATTACH:
      if (inp != NULL) {
         error = EINVAL;
         break;
      }   
      error = in_pcballoc(so, &g_udb);
      if (error)
         break;
      error = soreserve(so, g_udp_sendspace, g_udp_recvspace);
      if (error)
         break;
      ((struct inpcb *) so->so_pcb)->inp_ip.ip_ttl = 64;// default ip ttl: ip_defttl;
      break;

   case PRU_DETACH:
      udp_detach(inp);
      break;

   case PRU_BIND:
      error = in_pcbbind(inp, addr);
      break;

   case PRU_LISTEN:
      //error = EOPNOTSUPP;
      error = in_pcblisten(inp, addr);
      break;

/*
   case PRU_CONNECT:
      if (inp->inp_faddr.s_addr != INADDR_ANY) {
         error = EISCONN;
         break;
      }
      error = in_pcbconnect(inp, addr);
      splx(s);
      if (error == 0)
         soisconnected(so);
      break;

   case PRU_CONNECT2:
      error = EOPNOTSUPP;
      break;

   case PRU_ACCEPT:
      error = EOPNOTSUPP;
      break;

   case PRU_DISCONNECT:
      if (inp->inp_faddr.s_addr == INADDR_ANY) {
         error = ENOTCONN;
         break;
      }
      s = splnet();
      in_pcbdisconnect(inp);
      inp->inp_laddr.s_addr = INADDR_ANY;
      splx(s);
      so->so_state &= ~SS_ISCONNECTED;    // XXX 
      break;

   case PRU_SHUTDOWN:
      socantsendmore(so);
      break;

   case PRU_SEND:
      return (udp_output(inp, m, addr, control));

   case PRU_ABORT:
      soisdisconnected(so);
      udp_detach(inp);
      break;

   case PRU_SOCKADDR:
      in_setsockaddr(inp, addr);
      break;

   case PRU_PEERADDR:
      in_setpeeraddr(inp, addr);
      break;

   case PRU_SENSE:
       // stat: don't bother with a blocksize.
      return (0);
   case PRU_SENDOOB:
   case PRU_FASTTIMO:
   case PRU_SLOWTIMO:
   case PRU_PROTORCV:
   case PRU_PROTOSEND:
      error =  EOPNOTSUPP;
      break;

   case PRU_RCVD:
   case PRU_RCVOOB:
      return (EOPNOTSUPP); // do not free mbuf's
*/
   default:
      DEBUG("udp_usrreq");
   }

release:
   if (control) {
      DEBUG("udp control data unexpectedly retained\n");
      usn_free_mbuf(control);
   }
   if (m)
      usn_free_mbuf(m);
   return (error);
}


// functions on tcp-related socket.
int
tcp_usrreq(struct usn_socket *so, int req,
           usn_mbuf_t *m, usn_mbuf_t *addr, usn_mbuf_t *control)
{
   return 0;
}

struct usn_socket*
usnet_get_socket(u_int32 fd)
{
   if ( fd == 0 || fd >= MAX_SOCKETS )
      return NULL;
   return g_fds[fd];
}

int32
usnet_create_socket(u_int32 dom, struct usn_socket **aso, u_int32 type, u_int32 proto)
{
   int                i, error;
   struct usn_socket *so;
   long               lproto = proto;

   for ( i=1; i<MAX_SOCKETS; i++) {
      if ( g_fds[i] == 0 ) break;
   }
   if ( i >= MAX_SOCKETS )
      return -1;

   // check proto and dom.
   if ( dom != USN_AF_INET     // tcp/ip protocol
        || ( type != SOCK_STREAM && type != SOCK_DGRAM ) ) { // tcp,udp
      return 0;
   }

   // TODO: using multi-hash, and find empty socket slot.
   so = (struct usn_socket*) usn_get_buf(0,sizeof(*so));
   bzero((caddr_t)so, sizeof(*so));
   so->so_type = type;
   so->so_family = dom;
   so->so_pcb = 0;
   if ( type == SOCK_DGRAM )
      so->so_usrreq = udp_usrreq;
   else if ( type == SOCK_STREAM )
      so->so_usrreq = tcp_usrreq;

   error = so->so_usrreq(so, PRU_ATTACH, (usn_mbuf_t *)0, 
                          (usn_mbuf_t *)lproto, (usn_mbuf_t *)0);

   if ( error < 0 )
      return -1;

   g_fds[i] = so;
   *aso = so;
   so->so_fd = i;

   return i;
}

int32
usnet_bind_socket(u_int32 fd, u_int32 addr, u_short port)
{
   struct usn_socket      *so = usnet_get_socket(fd);
   struct usn_sockaddr_in *saddr;
   usn_mbuf_t             *nam;
   int                     ret = 0;
  
   if ( so == NULL )
      return -1;

   nam = usn_get_mbuf(0, sizeof(struct usn_sockaddr_in), 0);
   if ( nam == NULL )
      return -2;

   saddr = mtod(nam, struct usn_sockaddr_in*);
   saddr->sin_len = 8; // FIXME: sizeof(struct usn_sockaddr_in);
   saddr->sin_family = so->so_family;
   saddr->sin_port = htons(port);
   saddr->sin_addr.s_addr = addr;

   ret = so->so_usrreq(so, PRU_BIND, 0, nam, 0);

   if ( ret < 0 )
      return ret;

   if (nam)
      usn_free_mbuf(nam);
   return 0;
}

int32
usnet_listen_socket(u_int32 fd, int32 flags, accept_handler_cb accept_cb, error_handler_cb error_cb, void* arg)
{
   struct usn_socket      *so = usnet_get_socket(fd);
   usn_mbuf_t             *nam;
   struct usn_appcb       *cb;
   int    ret = 0;

   if ( so == NULL )
      return -1;

   nam = usn_get_mbuf(0, sizeof(struct usn_appcb), 0);
   if ( nam == NULL )
      return -2;
   
   cb = mtod(nam, struct usn_appcb*); 
   memset(cb,0, sizeof(*cb));
   cb->fd = fd;
   cb->arg = arg;
   cb->accept_cb = accept_cb;
   cb->error_cb = error_cb;

   ret = so->so_usrreq(so, PRU_LISTEN, 0, nam, 0); 
   if ( ret != 0 )
      return ret;

   // XXX: why to check?
   //if ( so->so_q == 0 )
      so->so_options |= SO_ACCEPTCONN;

   so->so_qlimit = USN_DEF_BACKLOG;

   if (nam)
      usn_free_mbuf(nam);

   return 0;
}

/*
 * Append address and data, and optionally, control (ancillary) data
 * to the receive queue of a socket.  If present,
 * m0 must include a packet header with total length.
 * Returns 0 if no space in sockbuf or insufficient mbufs.
 */
int32
sbappendaddr( struct sockbuf *sb, struct usn_sockaddr *asa, 
              usn_mbuf_t *m0, usn_mbuf_t *control)
{
   usn_mbuf_t *m, *n; 
   int space = asa->sa_len;

   n = m = NULL;

   if (m0)
      space += m0->mlen;

   for (n = control; n; n = n->next) {
      space += n->mlen;
      if (n->next == 0)  /* keep pointer to last control buf */
         break;
   }   

   if (space > sbspace(sb))
      return (0);

   if (asa->sa_len > MSIZE)
      return (0);

   m = usn_get_mbuf(0, MSIZE, 0);
   if (m == 0)
      return (0);
   m->mlen = asa->sa_len;
   m->flags = BUF_ADDR;
   bcopy((caddr_t)asa, mtod(m, caddr_t), asa->sa_len);

   if (n) 
      n->next = m0;      /* concatenate data to control */
   else
      control = m0; 

   m->next = control;
   for (n = m; n; n = n->next)
      sballoc(sb, n); 

   // insert new buffer at the end of the queue.
   n = sb->sb_mb;
   if (n) {
      while (n->queue)
         n = n->queue;
      n->queue = m;
   } else
      sb->sb_mb = m;
   return (1);
}

int32
usnet_wakeup_socket(struct inpcb* inp)
{
   struct usn_socket *so;
   struct sockbuf* sb;
   struct usn_sockaddr addr;
   usn_mbuf_t  *m, *n, *maddr, *opts;

   DEBUG("process a packet");
   m = n = maddr = opts = NULL;
   if ( inp == NULL )
      return 0;

   so = inp->inp_socket;
   if ( so == NULL )
      return 0;

   sb = &so->so_rcv;
   if ( sb == NULL )
      return 0;

   if ( sb->sb_mb == NULL)
      return 0;

   // FIXME: a loop of handling queue.
   m = sb->sb_mb;
   n = m->queue;
   if ( m->flags & BUF_ADDR ) {
      maddr = m;
      m = m->next;
      memcpy(&addr, maddr->head, mtod(maddr, struct usn_sockaddr*)->sa_len);
      usn_free_mbuf(maddr);
   }
   if (m == NULL) {
      sb->sb_mb = n;
      return 0;
   }
   m->queue = n;
   sb->sb_mb = m;

#ifdef notyet
   // get options
   if ( m->flags & BUF_CTLMSG ) {
      opts = m;
      n = m;
      while ( n->next && n->next->flags & BUF_CTLMSG)
         n = n->next;
      m = n->next;
      n->next = NULL;
      m->prev = NULL;
   }
#endif

   if ( (m->flags & BUF_DATA) == 0) {
      DEBUG("no data found");
      return 0;
   }
        
   if ( so->so_options & SO_ACCEPTCONN ) {
      DEBUG("calling accept callback");
      inp->inp_appcb.accept_cb(so->so_fd, &addr, 8/*len of sockadrr_in*/, inp->inp_appcb.arg);
   }

   if (opts)
      usn_free_mbuf(opts);

   if (m)
      usn_free_mbuf(m);

   return 0;
}

// @return: 
//   >= 0: length of available buffer.
//    < 0: error code.
int32
usnet_read_socket(u_int fd, u_char *buf, u_int len)
{
   struct usn_socket      *so = usnet_get_socket(fd);
   int32 ret = 0;
   
   if ( so == NULL || so->so_rcv.sb_mb == NULL ) {
      buf = NULL; 
      return 0;
   }

   // FIXME: get buffer length and copy the buffer

   return ret;
}

usn_buf_t*
usnet_get_sobuffer(u_int32 fd)
{
   struct usn_socket *so = usnet_get_socket(fd);
   usn_buf_t *buf = 0;

   if ( so == NULL || so->so_rcv.sb_mb == NULL )
      return buf;
   
   buf = (usn_buf_t*)so->so_rcv.sb_mb;

   // XXX
   so->so_rcv.sb_mb = NULL;

   return buf;
}


int32
usnet_write_sobuffer(u_int fd, usn_buf_t *buf)
{
   struct usn_socket *so = usnet_get_socket(fd);
   struct inpcb *pcb = 0;
   usn_mbuf_t   *m = 0;
   usn_ip_t     *ip;
   usn_udphdr_t *uh;

   if ( buf == NULL ) 
      return 0;

   if ( so == NULL )
      return 0;

   pcb = (struct inpcb*)so->so_pcb;

   if ( pcb == NULL )
      return 0;

   m = (usn_mbuf_t*) buf;
   if ( m->head - m->start < sizeof(*ip) + sizeof(*uh) ) {
      // FIXME: reallocate mbuf.
      DEBUG("reallacate mbuf: notyet");
      return 0;
   }
   m->head -= sizeof(*uh);
   m->mlen += sizeof(*uh);
   uh = mtod(m, usn_udphdr_t*);

   m->head -= sizeof(*ip);
   m->mlen += sizeof(*ip);
   ip = mtod(m, usn_ip_t*);

   uh->uh_sport = pcb->inp_lport;
   uh->uh_dport = g_udp_in.sin_port;//pcb->inp_fport;

   ip->ip_src.s_addr = pcb->inp_laddr.s_addr;
   ip->ip_dst.s_addr = inet_addr("10.10.10.1");//pcb->inp_faddr.s_addr;
   ip->ip_len = ntohs(m->mlen);

   // FIXME: enqueue msg
   (void)so;
   // FIXME: wakeup process

   ipv4_output(m, 0, 0, IP_ROUTETOIF);

   return 0;
}

int32
usnet_writeto_sobuffer(u_int fd, usn_buf_t *buf, struct usn_sockaddr_in *addr)
{
   struct usn_socket *so = usnet_get_socket(fd);
   struct inpcb *pcb = 0;
   usn_mbuf_t   *m = 0;
   usn_ip_t     *ip;
   usn_udphdr_t *uh;

   if ( buf == NULL ) 
      return 0;

   if ( so == NULL )
      return 0;

   pcb = (struct inpcb*)so->so_pcb;

   if ( pcb == NULL )
      return 0;

   m = (usn_mbuf_t*) buf;
   if ( m->head - m->start < sizeof(*ip) + sizeof(*uh) ) {
      // FIXME: reallocate mbuf.
      DEBUG("reallacate mbuf: notyet");
      return 0;
   }
   m->head -= sizeof(*uh);
   m->mlen += sizeof(*uh);
   uh = mtod(m, usn_udphdr_t*);

   m->head -= sizeof(*ip);
   m->mlen += sizeof(*ip);
   ip = mtod(m, usn_ip_t*);

   uh->uh_sport = pcb->inp_lport;
   uh->uh_dport = addr->sin_port;//pcb->inp_fport;

   ip->ip_src.s_addr = pcb->inp_laddr.s_addr;
   ip->ip_dst.s_addr = addr->sin_addr.s_addr;
   ip->ip_len = ntohs(m->mlen);

   // FIXME: enqueue msg
   (void)so;
   // FIXME: wakeup process

   ipv4_output(m, 0, 0, IP_ROUTETOIF);

   return 0;
}
