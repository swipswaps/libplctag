/***************************************************************************
 *   Copyright (C) 2015 by OmanTek                                         *
 *   Author Kyle Hayes  kylehayes@omantek.com                              *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU Library General Public License as       *
 *   published by the Free Software Foundation; either version 2 of the    *
 *   License, or (at your option) any later version.                       *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this program; if not, write to the                 *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

/**************************************************************************
 * CHANGE LOG                                                             *
 *                                                                        *
 * 2015-09-12  KRH - Created file.                                        *
 *                                                                        *
 **************************************************************************/


#include <platform.h>
#include <ab/ab_common.h>
#include <ab/cip.h>
#include <ab/defs.h>
#include <ab/eip.h>
#include <ab/error_codes.h>
#include <ab/session.h>
//#include <ab/connection.h>
#include <ab/request.h>
#include <util/debug.h>
#include <stdlib.h>
#include <time.h>


static ab_session_p session_create_unsafe(const char* host, int gw_port, const char *path, int plc_type, int use_connected_msg);
static int session_init(ab_session_p session);
static int get_plc_type(attr attribs);
static int add_session_unsafe(ab_session_p n);
static int remove_session_unsafe(ab_session_p n);
static ab_session_p find_session_by_host_unsafe(const char *gateway, const char *path);
static int session_match_valid(const char *host, const char *path, ab_session_p session);
static int session_connect(ab_session_p session);
static void session_destroy(void *session);
static int session_register(ab_session_p session);
static int session_unregister_unsafe(ab_session_p session);
static THREAD_FUNC(session_handler);
//static int check_incoming_data_unsafe(ab_session_p session);
//static int check_outgoing_data_unsafe(ab_session_p session);
//static int send_current_request(ab_session_p session);
//static int process_response_packet_unsafe(ab_session_p session);
//static int match_request_and_response(ab_request_p request, eip_cip_co_resp *response);
//static void receive_response_unsafe(ab_session_p session, ab_request_p request);
static int prepare_request(ab_session_p session, ab_request_p request);
static int send_eip_request(ab_session_p session);
static int recv_eip_response(ab_session_p session);


/*
 * session_get_new_seq_id_unsafe
 *
 * A wrapper to get a new session sequence ID.  Not thread safe.
 *
 * Note that this is dangerous to use in threaded applications
 * because 32-bit processors will not implement a 64-bit
 * integer as an atomic entity.
 */

uint64_t session_get_new_seq_id_unsafe(ab_session_p sess)
{
    return sess->session_seq_id++;
}

/*
 * session_get_new_seq_id
 *
 * A thread-safe function to get a new session sequence ID.
 */

uint64_t session_get_new_seq_id(ab_session_p sess)
{
    uint16_t res = 0;

    //pdebug(DEBUG_DETAIL, "entering critical block %p",global_session_mut);
    critical_block(sess->session_mutex) {
        res = (uint16_t)session_get_new_seq_id_unsafe(sess);
    }
    //pdebug(DEBUG_DETAIL, "leaving critical block %p", global_session_mut);

    return res;
}


static int connection_is_usable(ab_connection_p connection)
{
    if(!connection) {
        return 0;
    }

    if(connection->exclusive) {
        return 0;
    }

    if(connection->disconnect_in_progress) {
        return 0;
    }

    return 1;
}



ab_connection_p session_find_connection_by_path_unsafe(ab_session_p session,const char *path)
{
    ab_connection_p connection;

    connection = session->connections;

    /*
     * there are a lot of conditions.
     * We do not want to use connections that are in the process of shutting down.
     * We do not want to use connections that are used exclusively by one tag.
     * We want to use connections that have the same path as the tag.
     */

    while(connection) {
        /* scan past any that are in the process of being deleted. */
        while(connection && !rc_inc(connection)) {
            connection = connection->next;
        }

        /* did we walk off the end? */
        if(!connection) {
            break;
        }

        if(connection_is_usable(connection) && str_cmp_i(connection->path, path)==0) {
            /* this is the one! */
            break;
        }

        /* we are not going to use this connection, release it. */
        rc_dec(connection);

        /* try the next one. */
        connection = connection->next;
    }

    /* add to the ref count.  This is safe if connection is null. */
    return connection;
}



int session_add_connection_unsafe(ab_session_p session, ab_connection_p connection)
{
    pdebug(DEBUG_DETAIL, "Starting");

    /* add the connection to the list in the session */
    connection->next = session->connections;
    session->connections = connection;

    pdebug(DEBUG_DETAIL, "Done");

    return PLCTAG_STATUS_OK;
}

int session_add_connection(ab_session_p session, ab_connection_p connection)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_DETAIL, "Starting");

    if(session) {
        critical_block(session->session_mutex) {
            rc = session_add_connection_unsafe(session, connection);
        }
    } else {
        pdebug(DEBUG_WARN, "Session ptr is null!");
        rc = PLCTAG_ERR_NULL_PTR;
    }

    pdebug(DEBUG_DETAIL, "Done");

    return rc;
}

/* must have the session mutex held here. */
int session_remove_connection_unsafe(ab_session_p session, ab_connection_p connection)
{
    ab_connection_p cur;
    ab_connection_p prev;
    /* int debug = session->debug; */
    int rc;

    pdebug(DEBUG_DETAIL, "Starting");

    cur = session->connections;
    prev = NULL;

    while (cur && cur != connection) {
        prev = cur;
        cur = cur->next;
    }

    if (cur && cur == connection) {
        if (prev) {
            prev->next = cur->next;
        } else {
            session->connections = cur->next;
        }

        rc = PLCTAG_STATUS_OK;
    } else {
        rc = PLCTAG_ERR_NOT_FOUND;
    }

    pdebug(DEBUG_DETAIL, "Done");

    return rc;
}

int session_remove_connection(ab_session_p session, ab_connection_p connection)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_DETAIL, "Starting");

    if(session) {
        critical_block(session->session_mutex) {
            rc = session_remove_connection_unsafe(session, connection);
        }
    } else {
        rc = PLCTAG_ERR_NULL_PTR;
    }



//ab_connection_p session_find_connection_by_path_unsafe(ab_session_p session,const char *path)
//{
//    ab_connection_p connection;
//
//    connection = session->connections;
//
//    /*
//     * there are a lot of conditions.
//     * We do not want to use connections that are in the process of shutting down.
//     * We do not want to use connections that are used exclusively by one tag.
//     * We want to use connections that have the same path as the tag.
//     */
//
//    while(connection) {
//        /* scan past any that are in the process of being deleted. */
//        while(connection && !rc_inc(connection)) {
//            connection = connection->next;
//        }
//
//        /* did we walk off the end? */
//        if(!connection) {
//            break;
//        }
//
//        if(connection_is_usable(connection) && str_cmp_i(connection->path, path)==0) {
//            /* this is a possible connection, is it in the process of being destroyed? */
//            connection = rc_inc(connection);
//
//            if(connection) {
//                break;
//            }
//        }
//
//        /* we are not going to use this connection, release it. */
//        rc_dec(connection);
//
//        /* try the next one. */
//        connection = connection->next;
//    }
//
//    /* add to the ref count.  This is safe if connection is null. */
//    return connection;
//}
//
//
//
//int session_add_connection_unsafe(ab_session_p session, ab_connection_p connection)
//{
//    pdebug(DEBUG_DETAIL, "Starting");
//
//    /* add the connection to the list in the session */
//    connection->next = session->connections;
//    session->connections = connection;
//
//    pdebug(DEBUG_DETAIL, "Done");
//
//    return PLCTAG_STATUS_OK;
//}
//
//int session_add_connection(ab_session_p session, ab_connection_p connection)
//{
//    int rc = PLCTAG_STATUS_OK;
//
//    pdebug(DEBUG_DETAIL, "Starting");
//
//    if(session) {
//        critical_block(session->session_mutex) {
//            rc = session_add_connection_unsafe(session, connection);
//        }
//    } else {
//        pdebug(DEBUG_WARN, "Session ptr is null!");
//        rc = PLCTAG_ERR_NULL_PTR;
//    }
//
//    pdebug(DEBUG_DETAIL, "Done");
//
//    return rc;
//}
//
///* must have the session mutex held here. */
//int session_remove_connection_unsafe(ab_session_p session, ab_connection_p connection)
//{
//    ab_connection_p cur;
//    ab_connection_p prev;
//    /* int debug = session->debug; */
//    int rc;
//
//    pdebug(DEBUG_DETAIL, "Starting");
//
//    cur = session->connections;
//    prev = NULL;
//
//    while (cur && cur != connection) {
//        prev = cur;
//        cur = cur->next;
//    }
//
//    if (cur && cur == connection) {
//        if (prev) {
//            prev->next = cur->next;
//        } else {
//            session->connections = cur->next;
//        }
//
//        rc = PLCTAG_STATUS_OK;
//    } else {
//        rc = PLCTAG_ERR_NOT_FOUND;
//    }
//
//    pdebug(DEBUG_DETAIL, "Done");
//
//    return rc;
//}
//
//int session_remove_connection(ab_session_p session, ab_connection_p connection)
//{
//    int rc = PLCTAG_STATUS_OK;
//
//    pdebug(DEBUG_DETAIL, "Starting");
//
//    if(session) {
//        critical_block(session->session_mutex) {
//            rc = session_remove_connection_unsafe(session, connection);
//        }
//    } else {
//        rc = PLCTAG_ERR_NULL_PTR;
//    }
//
//    pdebug(DEBUG_DETAIL, "Done");
//
//    return rc;
//}
//

int session_find_or_create(ab_session_p *tag_session, attr attribs)
{
    /*int debug = attr_get_int(attribs,"debug",0);*/
    const char *session_gw = attr_get_str(attribs, "gateway", "");
    const char *session_path = attr_get_str(attribs, "path", "");
    int use_connected_msg = attr_get_int(attribs, "use_connected_msg", 0);
    int session_gw_port = attr_get_int(attribs, "gateway_port", AB_EIP_DEFAULT_PORT);
    int plc_type = get_plc_type(attribs);
    ab_session_p session = AB_SESSION_NULL;
    int new_session = 0;
    int shared_session = attr_get_int(attribs, "share_session", 1); /* share the session by default. */
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_DETAIL, "Starting");

    if(plc_type == AB_PROTOCOL_PLC && str_length(session_path) > 0) {
        /* this means it is DH+ */
        use_connected_msg = 1;
        attr_set_int(attribs, "use_connected_msg", 1);
    }

    critical_block(global_session_mut) {
        /* if we are to share sessions, then look for an existing one. */
        if (shared_session) {
            session = find_session_by_host_unsafe(session_gw, session_path);
        } else {
            /* no sharing, create a new one */
            session = AB_SESSION_NULL;
        }

        if (session == AB_SESSION_NULL) {
            pdebug(DEBUG_DETAIL,"Creating new session.");
            session = session_create_unsafe(session_gw, session_gw_port, session_path, plc_type, use_connected_msg);

            if (session == AB_SESSION_NULL) {
                pdebug(DEBUG_WARN, "unable to create or find a session!");
                rc = PLCTAG_ERR_BAD_GATEWAY;
            } else {
                new_session = 1;
            }
        } else {
            pdebug(DEBUG_DETAIL,"Reusing existing session.");
        }
    }

    /*
     * do this OUTSIDE the mutex in order to let other threads not block if
     * the session creation process blocks.
     */

    if(new_session) {
        rc = session_init(session);
        if(rc != PLCTAG_STATUS_OK) {
            rc_dec(session);
            session = AB_SESSION_NULL;
        } else {
            /* save the status */
            //session->status = rc;
        }
    }

    /* store it into the tag */
    *tag_session = session;

    pdebug(DEBUG_DETAIL, "Done");

    return rc;
}




int get_plc_type(attr attribs)
{
    const char* cpu_type = attr_get_str(attribs, "plc", attr_get_str(attribs, "cpu", "NONE"));

    if (!str_cmp_i(cpu_type, "plc") || !str_cmp_i(cpu_type, "plc5") || !str_cmp_i(cpu_type, "slc") ||
        !str_cmp_i(cpu_type, "slc500")) {
        return AB_PROTOCOL_PLC;
    } else if (!str_cmp_i(cpu_type, "micrologix800") || !str_cmp_i(cpu_type, "mlgx800") || !str_cmp_i(cpu_type, "micro800")) {
        return AB_PROTOCOL_MLGX800;
    } else if (!str_cmp_i(cpu_type, "micrologix") || !str_cmp_i(cpu_type, "mlgx")) {
        return AB_PROTOCOL_MLGX;
    } else if (!str_cmp_i(cpu_type, "compactlogix") || !str_cmp_i(cpu_type, "clgx") || !str_cmp_i(cpu_type, "lgx") ||
               !str_cmp_i(cpu_type, "controllogix") || !str_cmp_i(cpu_type, "contrologix") ||
               !str_cmp_i(cpu_type, "flexlogix") || !str_cmp_i(cpu_type, "flgx")) {
        return AB_PROTOCOL_LGX;
    } else {
        pdebug(DEBUG_WARN, "Unsupported device type: %s", cpu_type);

        return PLCTAG_ERR_BAD_DEVICE;
    }

    return PLCTAG_STATUS_OK;
}


int add_session_unsafe(ab_session_p n)
{
    pdebug(DEBUG_DETAIL, "Starting");

    if (!n) {
        return PLCTAG_ERR_NULL_PTR;
    }

    n->prev = NULL;
    n->next = sessions;

    if (sessions) {
        sessions->prev = n;
    }

    sessions = n;

    pdebug(DEBUG_DETAIL, "Done");

    return PLCTAG_STATUS_OK;
}

int add_session(ab_session_p s)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_DETAIL, "Starting.");

    critical_block(global_session_mut) {
        rc = add_session_unsafe(s);
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}

int remove_session_unsafe(ab_session_p n)
{
    ab_session_p tmp;

    pdebug(DEBUG_DETAIL, "Starting");

    if (!n || !sessions) {
        return 0;
    }

    tmp = sessions;

    while (tmp && tmp != n)
        tmp = tmp->next;

    if (!tmp || tmp != n) {
        pdebug(DEBUG_WARN, "Session not found!");
        return PLCTAG_ERR_NOT_FOUND;
    }

    if (n->next) {
        n->next->prev = n->prev;
    }

    if (n->prev) {
        n->prev->next = n->next;
    } else {
        sessions = n->next;
    }

    n->next = NULL;
    n->prev = NULL;

    pdebug(DEBUG_DETAIL, "Done");

    return PLCTAG_STATUS_OK;
}

int remove_session(ab_session_p s)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_DETAIL, "Starting.");

    critical_block(global_session_mut) {
        rc = remove_session_unsafe(s);
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}


int session_match_valid(const char *host, const char *path, ab_session_p session)
{
    if(!session) {
        return 0;
    }

    if(session->status !=  PLCTAG_STATUS_OK && session->status != PLCTAG_STATUS_PENDING) {
        return 0;
    }

    if(str_cmp_i(host,session->host)) {
        return 0;
    }

    if(str_cmp_i(path,session->path)) {
        return 0;
    }

    return 1;
}


ab_session_p find_session_by_host_unsafe(const char* host, const char *path)
{
    ab_session_p session;

    session = sessions;

    while(session) {
        /* scan past any that are in the process of being deleted. */
        while(session && !rc_inc(session)) {
            session = session->next;
        }

        if(!session) {
            break;
        }

        if(session_match_valid(host, path, session)) {
            /* this is the one! */
            break;
        }

        /* we are not going to use this session, release it. */
        rc_dec(session);

        /* try the next one. */
        session = session->next;
    }

    return session;
}



ab_session_p session_create_unsafe(const char* host, int gw_port, const char *path, int plc_type, int use_connected_msg)
{
    static volatile uint32_t srand_setup = 0;
    static volatile uint32_t connection_id = 0;

    int rc = PLCTAG_STATUS_OK;
    ab_session_p session = AB_SESSION_NULL;

    pdebug(DEBUG_INFO, "Starting");

    pdebug(DEBUG_DETAIL, "Warning: not using passed port %d", gw_port);

    session = (ab_session_p)rc_alloc(sizeof(struct ab_session_t), session_destroy);
    if (!session) {
        pdebug(DEBUG_WARN, "Error allocating new session.");
        return AB_SESSION_NULL;
    }

    session->data_capacity = EIP_CIP_PREFIX_SIZE + MAX_CIP_MSG_SIZE_EX;

    session->path = str_dup(path);
    if(path && str_length(path) && !session->path) {
        pdebug(DEBUG_WARN, "Unable to duplicate path string!");
        rc_dec(session);
        return NULL;
    }

    rc = cip_encode_path(path, use_connected_msg, plc_type, &session->conn_path, &session->conn_path_size, &session->dhp_dest);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_INFO,"Unable to convert path links strings to binary path!");
        rc_dec(session);
        return NULL;
    }

    session->plc_type = plc_type;
    session->data_capacity = EIP_CIP_PREFIX_SIZE + MAX_CIP_MSG_SIZE_EX;
    session->use_connected_msg = use_connected_msg;
    session->status = PLCTAG_STATUS_PENDING;
    session->conn_serial_number = (uint16_t)(intptr_t)(session);

    /* check for ID set up. This does not need to be thread safe since we just need a random value. */
    if(srand_setup == 0) {
        srand((unsigned int)time_ms());
        srand_setup = 1;
    }

    if(connection_id == 0) {
        connection_id = rand();
    }

    session->session_seq_id =  rand();

    /* guess the max CIP payload size. */
    switch(plc_type) {
    case AB_PROTOCOL_PLC:
    case AB_PROTOCOL_MLGX:
        session->max_payload_size = MAX_CIP_PCCC_MSG_SIZE;
        break;

    case AB_PROTOCOL_LGX:
        session->max_payload_size = MAX_CIP_MSG_SIZE;
        break;

    case AB_PROTOCOL_MLGX800:
        session->max_payload_size = MAX_CIP_MSG_SIZE;
        break;

    default:
        pdebug(DEBUG_WARN,"Unknown protocol/cpu type!");
        rc_dec(session);
        return NULL;
        break;
    }


    /*
     * Why is connection_id global?  Because it looks like the PLC might
     * be treating it globally.  I am seeing ForwardOpen errors that seem
     * to be because of duplicate connection IDs even though the session
     * was closed.
     *
     * So, this is more or less unique across all invocations of the library.
     * FIXME - this could collide.  The probability is low, but it could happen
     * as there are only 32 bits.
     */
    session->conn_serial_number = ++connection_id;

    /* set up the packet interval to a reasonable default */
    //session->next_packet_interval_us = SESSION_DEFAULT_PACKET_INTERVAL;

    /* set up packet round trip information */
    for(int index=0; index < SESSION_NUM_ROUND_TRIP_SAMPLES; index++) {
        session->round_trip_samples[index] = SESSION_DEFAULT_RESEND_INTERVAL_MS;
    }

    /* add the new session to the list. */
    add_session_unsafe(session);

    pdebug(DEBUG_INFO, "Done");

    return session;
}


/*
 * session_init
 *
 * This calls several blocking methods and so must not keep the main mutex
 * locked during them.
 */
int session_init(ab_session_p session)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting.");

    if((rc = mutex_create(&(session->session_mutex))) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to create session mutex!");
        session->status = rc;
        return rc;
    }

    if((rc = thread_create((thread_p*)&(session->handler_thread), session_handler, 32*1024, session)) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to create session thread!");
        session->status = rc;
        return rc;
    }

//    /* we must connect to the gateway and register */
//    if ((rc = session_connect(session)) != PLCTAG_STATUS_OK) {
//        pdebug(DEBUG_WARN, "session connect failed!");
//        session->status = rc;
//        return rc;
//    }
//
//    if ((rc = session_register(session)) != PLCTAG_STATUS_OK) {
//        pdebug(DEBUG_WARN, "session registration failed!");
//        session->status = rc;
//        return rc;
//    }

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}

/*
 * session_connect()
 *
 * Connect to the host/port passed via TCP.
 */

int session_connect(ab_session_p session)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting.");

    /* Open a socket for communication with the gateway. */
    rc = socket_create(&(session->sock));

    if (rc) {
        pdebug(DEBUG_WARN, "Unable to create socket for session!");
        return 0;
    }

    rc = socket_connect_tcp(session->sock, session->host, AB_EIP_DEFAULT_PORT);

    if (rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to connect socket for session!");
        return rc;
    }

    /* everything is OK.  We have a TCP stream open to a gateway. */
    session->is_connected = 1;

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}

/* must have the session mutex held here */
void session_destroy(void *session_arg)
{
    ab_session_p session = session_arg;
    ab_request_p req = NULL;

    pdebug(DEBUG_INFO, "Starting.");

    if (!session) {
        pdebug(DEBUG_WARN, "Session ptr is null!");

        return;
    }


    /* terminate the thread first. */
    session->terminating = 1;

    /* so remove the session from the list so no one else can reference it. */
    remove_session(session);

    /* unregister and close the socket. */
    session_unregister_unsafe(session);

    if(session->handler_thread) {
        thread_join(session->handler_thread);
        thread_destroy(&(session->handler_thread));
        session->handler_thread = NULL;
    }

    /* remove any remaining requests, they are dead */
    req = session->requests;
    while(req) {
        session_remove_request(session, req);
        req = session->requests;
    }

    /* we are done with the mutex, finally destroy it. */
    if(session->session_mutex) {
        mutex_destroy(&(session->session_mutex));
        session->session_mutex = NULL;
    }

    pdebug(DEBUG_INFO, "Done.");

    return;
}


int session_register(ab_session_p session)
{
    eip_session_reg_req* req;
    eip_encap_t* resp;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting.");

    /*
     * clear the session data.
     *
     * We use the receiving buffer because we do not have a request and nothing can
     * be coming in (we hope) on the socket yet.
     */
    mem_set(session->data, 0, sizeof(eip_session_reg_req));

    req = (eip_session_reg_req*)(session->data);

    /* fill in the fields of the request */
    req->encap_command = h2le16(AB_EIP_REGISTER_SESSION);
    req->encap_length = h2le16(sizeof(eip_session_reg_req) - sizeof(eip_encap_t));
    req->encap_session_handle = h2le32(session->session_handle);
    req->encap_status = h2le32(0);
    req->encap_sender_context = h2le64((uint64_t)0);
    req->encap_options = h2le32(0);

    req->eip_version = h2le16(AB_EIP_VERSION);
    req->option_flags = h2le16(0);

    /*
     * socket ops here are _ASYNCHRONOUS_!
     *
     * This is done this way because we do not have everything
     * set up for a request to be handled by the thread.  I think.
     */

    /* send registration to the gateway */
    session->data_size = sizeof(eip_session_reg_req);
    session->data_offset = 0;

    rc = send_eip_request(session);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN,"Error sending session registration request! rc=%d", rc);
        return rc;
    }

    /* get the response from the gateway */
    rc = recv_eip_response(session);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Error receiving session registration response!");
        return rc;
    }

//    /* set the offset back to zero for the next packet */
//    session->data_offset = 0;
//
//    pdebug(DEBUG_INFO, "received response:");
//    pdebug_dump_bytes(DEBUG_INFO, session->data, data_size);

    /* encap header is at the start of the buffer */
    resp = (eip_encap_t*)(session->data);

    /* check the response status */
    if (le2h16(resp->encap_command) != AB_EIP_REGISTER_SESSION) {
        pdebug(DEBUG_WARN, "EIP unexpected response packet type: %d!", resp->encap_command);
        return PLCTAG_ERR_BAD_DATA;
    }

    if (le2h32(resp->encap_status) != AB_EIP_OK) {
        pdebug(DEBUG_WARN, "EIP command failed, response code: %d", le2h32(resp->encap_status));
        return PLCTAG_ERR_REMOTE_ERR;
    }

    /*
     * after all that, save the session handle, we will
     * use it in future packets.
     */
    session->session_handle = le2h32(resp->encap_session_handle);
    session->registered = 1;

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_OK;
}

int session_unregister_unsafe(ab_session_p session)
{
    if (session->sock) {
        session->is_connected = 0;
        session->registered = 0;
        socket_close(session->sock);
        socket_destroy(&(session->sock));
        session->sock = NULL;
    }

    return PLCTAG_STATUS_OK;
}



void session_destroy(void *session_arg)
{
    ab_session_p session = session_arg;
    ab_request_p req = NULL;

    pdebug(DEBUG_INFO, "Starting.");

    if (!session) {
        pdebug(DEBUG_WARN, "Session ptr is null!");

        return;
    }


    /* terminate the thread first. */
    session->terminating = 1;

    /* so remove the session from the list so no one else can reference it. */
    remove_session(session);

    /* get rid of the handler thread. */
    if(session->handler_thread) {
        thread_join(session->handler_thread);
        thread_destroy(&(session->handler_thread));
        session->handler_thread = NULL;
    }

    if(session->targ_connection_id) {
        /* need to mark the session as not terminating to get the sends to complete. */
        session->terminating = 0;
        perform_forward_close(session);
        session->terminating = 1;
    }

    /* unregister and close the socket. */
    session_unregister_unsafe(session);

    /* remove any remaining requests, they are dead */
    req = session->requests;
    while(req) {
        session_remove_request(session, req);
        req = session->requests;
    }

    /* we are done with the mutex, finally destroy it. */
    if(session->session_mutex) {
        mutex_destroy(&(session->session_mutex));
        session->session_mutex = NULL;
    }

    if(session->conn_path) {
        mem_free(session->conn_path);
        session->conn_path = NULL;
    }

    if(session->path) {
        mem_free(session->path);
        session->path = NULL;
    }

    if(session->host) {
        mem_free(session->host);
        session->host = NULL;
    }

    pdebug(DEBUG_INFO, "Done.");

    return;
}




/*
 * session_add_request_unsafe
 *
 * You must hold the mutex before calling this!
 */
int session_add_request_unsafe(ab_session_p sess, ab_request_p req)
{
    int rc = PLCTAG_STATUS_OK;
    ab_request_p cur, prev;
    int total_requests = 0;

    pdebug(DEBUG_INFO, "Starting.");

    if(!sess) {
        pdebug(DEBUG_WARN, "Session is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    req = rc_inc(req);

    if(!req) {
        pdebug(DEBUG_WARN, "Request is either null or in the process of being deleted.");
        return PLCTAG_ERR_NULL_PTR;
    }

    /* make sure the request points to the session */
    req->session = sess;

    /* we add the request to the end of the list. */
    cur = sess->requests;
    prev = NULL;

    while (cur) {
        prev = cur;
        cur = cur->next;
        total_requests++;
    }

    if (!prev) {
        sess->requests = req;
    } else {
        prev->next = req;
    }

    /* update the request's refcount as we point to it. */

    pdebug(DEBUG_INFO,"Total requests in the queue: %d",total_requests);

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}

/*
 * session_add_request
 *
 * This is a thread-safe version of the above routine.
 */
int session_add_request(ab_session_p sess, ab_request_p req)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_DETAIL, "Starting. sess=%p, req=%p", sess, req);

    critical_block(sess->session_mutex) {
        rc = session_add_request_unsafe(sess, req);
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}


/*
 * session_remove_request_unsafe
 *
 * You must hold the mutex before calling this!
 */
int session_remove_request_unsafe(ab_session_p sess, ab_request_p req)
{
    int rc = PLCTAG_STATUS_OK;
    ab_request_p cur, prev;

    pdebug(DEBUG_DETAIL, "Starting.");

    if(sess == NULL || req == NULL) {
        return rc;
    }

    /* find the request and remove it from the list. */
    cur = sess->requests;
    prev = NULL;

    while (cur && cur != req) {
        prev = cur;
        cur = cur->next;
    }

    if (cur == req) {
        if (!prev) {
            sess->requests = cur->next;
        } else {
            prev->next = cur->next;
        }
    } /* else not found */

    req->next = NULL;
    req->session = NULL;

    /* release the request refcount */
    rc_dec(req);

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}



/*
 * session_remove_request
 *
 * This is a thread-safe version of the above routine.
 */
int session_remove_request(ab_session_p sess, ab_request_p req)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_DETAIL, "Starting.  session=%p, req=%p", sess, req);

    if(sess == NULL || req == NULL) {
        return rc;
    }

    critical_block(sess->session_mutex) {
        rc = session_remove_request_unsafe(sess, req);
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}



/*****************************************************************
 **************** Session handling functions *********************
 ****************************************************************/


THREAD_FUNC(session_handler)
{
    ab_session_p session = arg;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting thread for session %p", session);

    while(!session->terminating) {
        ab_request_p request = NULL;

        do {
            /* is the session ready? */
            if(!session->registered) {
                /* we must connect to the gateway and register */
                if ((rc = session_connect(session)) != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_WARN, "session connect failed!");
                    session->status = rc;
                    break;
                }

                if ((rc = session_register(session)) != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_WARN, "session registration failed!");
                    session->status = rc;
                    break;
                }
            }

            /*
             * session is ready.
             *
             * Take a request from the queue, send it and wait for the response.
             */

            critical_block(session->session_mutex) {
                /* grab a request off the front of the list. */
                request = session->requests;
                if(request) {
                    session->requests = request->next;

                    /* get, or try to get, a reference to the request. */
                    //request = rc_inc(request);
                }
            }

            if(!request) {
                /* nothing to do. */
                pdebug(DEBUG_DETAIL, "No requests to process.");

                break;
            }

            if(request_check_abort(request)) {
                pdebug(DEBUG_DETAIL, "Request is aborted.");
                rc = PLCTAG_ERR_ABORT;

                request->status = rc;
                request->resp_received = 1;
                request->request_size = 0;

                break;
            }

            /* request is good, set it up and process it. */
            rc = prepare_request(session, request);
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "Unable to prepare request, rc=%d!", rc);

                request->status = rc;
                request->resp_received = 1;
                request->request_size = 0;

                break;
            }

            /* copy the data from the request into the session's buffer. */
            mem_copy(session->data, request->data, request->request_size);
            session->data_size = request->request_size;

            rc = send_eip_request(session);
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "Error sending packet! rc=%d", rc);

                request->status = rc;
                request->resp_received = 1;
                request->request_size = 0;

                break;
            }

            rc = recv_eip_response(session);
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "Error receiving packet response! rc=%d", rc);

                request->status = rc;
                request->resp_received = 1;
                request->request_size = 0;

                break;
            }

            /* copy the data back into the request buffer. */
            mem_copy(request->data, session->data, session->data_size);

            rc = PLCTAG_STATUS_OK;

            request->status = rc;
            request->resp_received = 1;
            request->request_size = session->data_size;
        } while(0);

        if(request) {
            rc_dec(request);
        }

        /* give up the CPU a bit. */
        if(!session->terminating) {
            sleep_ms(1);
        }
    }

    THREAD_RETURN(0);
}



int prepare_request(ab_session_p session, ab_request_p request)
{
    eip_encap_t* encap = (eip_encap_t*)(request->data);
    int payload_size = request->request_size - sizeof(eip_encap_t);

    pdebug(DEBUG_DETAIL, "Starting.");

    if(!session) {
        pdebug(DEBUG_WARN,"Called with null session!");
        return PLCTAG_ERR_NULL_PTR;
    }

    /* fill in the fields of the request. */

    encap->encap_length = h2le16(payload_size);
    encap->encap_session_handle = h2le32(session->session_handle);
    encap->encap_status = h2le32(0);
    encap->encap_options = h2le32(0);

    /* set up the session sequence ID for this transaction */
    if(le2h16(encap->encap_command) == AB_EIP_READ_RR_DATA) {
        /* get new ID */
        session->session_seq_id++;

        request->session_seq_id = session->session_seq_id;
        encap->encap_sender_context = h2le64(session->session_seq_id); /* link up the request seq ID and the packet seq ID */

        pdebug(DEBUG_INFO,"Preparing unconnected packet with session sequence ID %llx",session->session_seq_id);
    } else if(le2h16(encap->encap_command) == AB_EIP_CONNECTED_SEND) {
        eip_cip_co_req *conn_req = (eip_cip_co_req*)(request->data);

        pdebug(DEBUG_DETAIL, "cpf_targ_conn_id=%x", request->connection->targ_connection_id);

        /* set up the connection information */
        conn_req->cpf_targ_conn_id = h2le32(request->connection->targ_connection_id);
        request->conn_id = request->connection->orig_connection_id;

        request->connection->conn_seq_num++;
        conn_req->cpf_conn_seq_num = h2le16(request->connection->conn_seq_num);
        request->conn_seq = request->connection->conn_seq_num;

        pdebug(DEBUG_INFO,"Preparing connected packet with connection ID %x and sequence ID %u(%x)",request->conn_id, request->conn_seq, request->conn_seq);
    } else {
        pdebug(DEBUG_WARN, "Unsupported packet type %x!", le2h16(encap->encap_command));
        return PLCTAG_ERR_UNSUPPORTED;
    }

    /* display the data */
    pdebug(DEBUG_INFO,"Prepared packet of size %d",request->request_size);
    pdebug_dump_bytes(DEBUG_INFO, request->data, request->request_size);

    pdebug(DEBUG_INFO,"Done.");

    return PLCTAG_STATUS_OK;
}




int send_eip_request(ab_session_p session)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_DETAIL, "Starting.");

    if(!session) {
        pdebug(DEBUG_WARN, "Session pointer is null.");
        return PLCTAG_ERR_NULL_PTR;
    }

    pdebug(DEBUG_DETAIL,"Sending packet of size %d",session->data_size);
    pdebug_dump_bytes(DEBUG_DETAIL, session->data, session->data_size);

    session->data_offset = 0;

    /* send the packet */
    do {
        rc = socket_write(session->sock, session->data + session->data_offset, session->data_size - session->data_offset);

        if(rc >= 0) {
            session->data_offset += rc;
        }

        /* give up the CPU if we still are looping */
        if(!session->terminating && rc >= 0 && session->data_offset < session->data_size) {
            sleep_ms(1);
        }
    } while(!session->terminating && rc >= 0 && session->data_offset < session->data_size);

    if(session->terminating) {
        pdebug(DEBUG_WARN, "Session is terminating.");
        return PLCTAG_ERR_ABORT;
    }

    if(rc < 0) {
        pdebug(DEBUG_WARN,"Error, %d, writing socket!", rc);
        return rc;
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_OK;
}



/*
 * recv_eip_response
 *
 * Look at the passed session and read any data we can
 * to fill in a packet.  If we already have a full packet,
 * punt.
 */
int recv_eip_response(ab_session_p session)
{
    uint32_t data_needed = 0;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_DETAIL,"Starting.");

    if(!session) {
        pdebug(DEBUG_WARN,"Called with null session!");
        return PLCTAG_ERR_NULL_PTR;
    }


    session->data_offset = 0;
    session->data_size = 0;
    data_needed = sizeof(eip_encap_t);

    do {
        rc = socket_read(session->sock, session->data + session->data_offset,
                         data_needed - session->data_offset);

        /*pdebug(DEBUG_DETAIL,"socket_read rc=%d",rc);*/

        if (rc < 0) {
            /* error! */
            pdebug(DEBUG_WARN,"Error reading socket! rc=%d",rc);
            return rc;
        } else {
            session->data_offset += rc;

            /*pdebug_dump_bytes(session->debug, session->data, session->data_offset);*/

            /* recalculate the amount of data needed if we have just completed the read of an encap header */
            if(session->data_offset >= sizeof(eip_encap_t)) {
                data_needed = sizeof(eip_encap_t) + le2h16(((eip_encap_t*)(session->data))->encap_length);

                if(data_needed > session->data_capacity) {
                    pdebug(DEBUG_WARN,"Packet response (%d) is larger than possible buffer size (%d)!", data_needed, session->data_capacity);
                    return PLCTAG_ERR_TOO_LARGE;
                }
            }
        }

        /* did we get all the data? */
        if(!session->terminating && session->data_offset < data_needed) {
            /* do not hog the CPU */
            sleep_ms(1);
        }
    } while(!session->terminating && session->data_offset < data_needed);

    if(session->terminating) {
        pdebug(DEBUG_INFO,"Session is terminating, returning...");
        rc = PLCTAG_ERR_ABORT;
        return rc;
    } else {
        session->resp_seq_id = le2h64(((eip_encap_t*)(session->data))->encap_sender_context);
        session->has_response = 1;
        session->data_size = data_needed;

        rc = PLCTAG_STATUS_OK;

        pdebug(DEBUG_DETAIL, "request received all needed data (%d bytes of %d).", session->data_offset, data_needed);

        pdebug_dump_bytes(DEBUG_DETAIL, session->data, session->data_offset);

        if(le2h16(((eip_encap_t*)(session->data))->encap_command) == AB_EIP_READ_RR_DATA) {
            eip_encap_t *encap = (eip_encap_t*)(session->data);
            pdebug(DEBUG_INFO,"Received unconnected packet with session sequence ID %llx",encap->encap_sender_context);
        } else if(le2h16(((eip_encap_t*)(session->data))->encap_command) == AB_EIP_CONNECTED_SEND) {
            eip_cip_co_resp *resp = (eip_cip_co_resp*)(session->data);
            pdebug(DEBUG_INFO,"Received connected packet with connection ID %x and sequence ID %u(%x)",le2h32(resp->cpf_orig_conn_id), le2h16(resp->cpf_conn_seq_num), le2h16(resp->cpf_conn_seq_num));
        }
    }

    return rc;
}
