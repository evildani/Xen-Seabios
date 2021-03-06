/*
 * xenbus.c: static, synchronous, read-only xenbus client for hvmloader.
 *
 * Copyright (c) 2009 Tim Deegan, Citrix Systems (R&D) Ltd.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "xen.h" // hypercalls
#include "config.h" // CONFIG_*
#include "util.h"
#include "bitops.h" //test_and_clear_bit


static struct xenstore_domain_interface *rings; /* Shared ring with dom0 */
static evtchn_port_t event;                     /* Event-channel to dom0 */
static char payload[XENSTORE_PAYLOAD_MAX + 1];  /* Unmarshalling area */


/*
 * Connect our xenbus client to the backend.
 * Call once, before any other xenbus actions.
 */
void xenbus_setup(void)
{
    if (!CONFIG_XEN)
        return;
    struct xen_hvm_param param;

    /* Ask Xen where the xenbus shared page is. */
    param.domid = DOMID_SELF;
    param.index = HVM_PARAM_STORE_PFN;
    if ( hypercall_hvm_op(HVMOP_get_param, &param) ){
        panic("Error on setup");
    }
    rings = (void *) (unsigned long) (param.value << 12);

    /* Ask Xen where the xenbus event channel is. */
    param.domid = DOMID_SELF;
    param.index = HVM_PARAM_STORE_EVTCHN;
    if ( hypercall_hvm_op(HVMOP_get_param, &param) ){
        panic("error on hypercall to define rings and channel");
    }
    event = param.value;
    dprintf(1,"Xenbus rings @0x%lx, event channel %lu\n",
           (unsigned long) rings, (unsigned long) event);
}

/*
 * Reset the xenbus connection
*/
void xenbus_shutdown(void)
{
    if(rings == NULL){
    	panic("rings not defined\n");
    }
    memset(rings, 0, sizeof *rings);
    memset(get_shared_info(), 0, 1024);
    rings = NULL;
}

/*
 * 1. Get xen shared info
 * 2. get the guest event handle
 * 3. while no events pending
 * 4 .issue a yield to the CPU until event arrives
 */
static void ring_wait(void)
{
	dprintf(6,"RING WAIT ISSUED...\n");
    struct shared_info *shinfo = get_shared_info();
    struct sched_poll poll;

    memset(&poll, 0, sizeof(poll));
    set_xen_guest_handle(poll.ports, &event);
    poll.nr_ports = 1;

    while ( !test_and_clear_bit(event, shinfo->evtchn_pending) )
        hypercall_sched_op(SCHEDOP_poll, &poll);
}

/*
 * Writes data to xenstore ring
 */
static void ring_write(char *data, u32 len)
{
    u32 part;

    if(len >= XENSTORE_PAYLOAD_MAX){
    	panic("Write Error on RINGS, more data than available buffer");
    }

    while ( len )
    {
        while ( (part = (XENSTORE_RING_SIZE - 1) -
                 MASK_XENSTORE_IDX(rings->req_prod - rings->req_cons)) == 0 ){
        	ring_wait();
        	//The ring is not empty or not ready
        }
        if ( part > (XENSTORE_RING_SIZE - MASK_XENSTORE_IDX(rings->req_prod)) )
            part = XENSTORE_RING_SIZE - MASK_XENSTORE_IDX(rings->req_prod);

        if ( part > len ){ /* Don't write more than we were asked for */
            part = len;
        }
        memcpy(rings->req + MASK_XENSTORE_IDX(rings->req_prod), data, part);
        barrier();
        rings->req_prod += part;
        len -= part;
    }
}

/*
 * reads response from xenstore ring
 */
static void ring_read(char *data, u32 len)
{
    u32 part;

    if(len >= XENSTORE_PAYLOAD_MAX){
    	panic("RING READ ERROR, more data that buffer space on rings");
    }

    while ( len )
    {
        while ( (part = MASK_XENSTORE_IDX(rings->rsp_prod -rings->rsp_cons)) == 0 ){
            ring_wait();
            //The ring is not ready or not empty
        }
        /* Don't overrun the end of the ring */
        if ( part > (XENSTORE_RING_SIZE - MASK_XENSTORE_IDX(rings->rsp_cons)) )
            part = XENSTORE_RING_SIZE - MASK_XENSTORE_IDX(rings->rsp_cons);

        if ( part > len ){ /* Don't read more than we were asked for */
            part = len;
        }
        memcpy(data, rings->rsp + MASK_XENSTORE_IDX(rings->rsp_cons), part);
        barrier();
        rings->rsp_cons += part;
        len -= part;
    }
}


/*
 * Send a request and wait for the answer.
 * Returns 0 for success, or an errno for error.
*/
int xenbus_send(u32 type, u32 len, char *data,
                       u32 *reply_len, char **reply_data)
{
    struct xsd_sockmsg hdr;
    evtchn_send_t send;
    int i,ret = 0;

    /* Not acceptable to use xenbus before setting it up */
    if(rings == NULL){
    	dprintf(1,"ERROR: Rings are not defined\n");
    	panic("XENBUS rings not defined\n");
    }

    /* Put the request on the ring */
    hdr.type = type;
    hdr.req_id = 222;  /* We only ever issue one request at a time */
    hdr.tx_id = 0;   /* We never use transactions */
    hdr.len = len;
    dprintf(4,"Before ring write header\n");
    ring_write((char *) &hdr, sizeof hdr);
    dprintf(4,"after ring write header\n");
    ring_write(data, len);
    dprintf(4,"after ring write\n");
    /* Tell the other end about the request */
    send.port = event;
    dprintf(4,"Before hypercall send\n");
    ret = hypercall_event_channel_op(EVTCHNOP_send, &send);
    dprintf(4,"Hypercall ret is: %d\n",ret);
    /* Properly we should poll the event channel now but that involves
     * mapping the shared-info page and handling the bitmaps. */
    /* Pull the reply off the ring */
    dprintf(4,"Before ring read header\n");
    ring_read((char *) &hdr, sizeof(hdr));
    dprintf(4,"after read header before ring read payload\n");
    ring_read(payload, hdr.len);
    dprintf(4,"after ring read payload\n");
    /* For sanity's sake, nul-terminate the answer */
    payload[hdr.len] = '\0';
    /* Handle errors */
    if ( hdr.type == XS_ERROR )
    {
        *reply_len = 0;
        for ( i = 0; i < ((sizeof xsd_errors) / (sizeof xsd_errors[0])); i++ ){
            if ( !strcmp(xsd_errors[i].errstring, payload) ){
                return xsd_errors[i].errnum;
            }
        }
        return EIO;
    }
    *reply_data = payload;
    *reply_len = hdr.len;
    return hdr.type;
}


/*
 * Read a xenstore key.  Returns a nul-terminated string (even if the XS
 * data wasn't nul-terminated) or NULL.  The returned string is in a
 * static buffer, so only valid until the next xenstore/xenbus operation.
 */
 char * xenstore_read(char *path){
    if(rings == NULL){
         panic("rings not defined");
    }
    u32 len = 0;
    char *answer = NULL;

    /* Include the nul in the request */
    if ( xenbus_send(XS_READ, strlen(path)+1, path, &len, &answer) ){
        return NULL;
    }
    /* We know xenbus_send() nul-terminates its answer, so just pass it on. */
    return answer;
}

 /*
  * This function reads domU related entries from the xenstore
  * For now we print them on console using dprintf 1
  */
 int test_xenstore(void){
 	int ret;
 	char test[11] = {'d','e','v','i','c','e','/','v','b','d','\0'};
 	char * replay_data = malloc_high(1024);
 	memset(replay_data,0,1024);
 	replay_data[1023] = '\0';
 	u32 replay_len = 0;
 	ret = xenbus_send(XS_DIRECTORY	,strlen(test)+1,test,&replay_len,&replay_data);
 	dprintf(1,"Xenbus return is: %d\n Response is device vbd-id: %s \n",ret,replay_data);
 	return 0;
 }
