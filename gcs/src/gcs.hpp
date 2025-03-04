/*
 * Copyright (C) 2008-2020 Codership Oy <info@codership.com>
 *
 * $Id$
 */

/*!
 * @file gcs.c Public GCS API
 */

#ifndef _gcs_h_
#define _gcs_h_

#include "gcs_gcache.hpp"

#include <gu_config.h>
#include <gu_buf.h>
#include <gu_errno.h>
#include <gu_uuid.h>
#include <gu_status.hpp>

#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <stdio.h>

#include <wsrep_api.h>


/*! @typedef @brief Sequence number type. */
typedef int64_t gcs_seqno_t;

/*! @def @brief Illegal sequence number. Action not serialized. */
static const gcs_seqno_t GCS_SEQNO_ILL   = -1;
/*! @def @brief Empty state. No actions applied. */
static const gcs_seqno_t GCS_SEQNO_NIL   =  0;
/*! @def @brief Start of the sequence */
static const gcs_seqno_t GCS_SEQNO_FIRST =  1;
/*! @def @brief history UUID length */
#define GCS_UUID_LEN 16
/*! @def @brief maximum supported size of an action (2GB - 1) */
#define GCS_MAX_ACT_SIZE 0x7FFFFFFF

/*! Connection handle type */
typedef struct gcs_conn gcs_conn_t;

/*! @brief Creates GCS connection handle.
 *
 * @param conf      gu_config_t* configuration object, can be null.
 * @param cache     pointer to the gcache object.
 * @param node_name human readable name of the node, can be null.
 * @param inc_addr  address at which application accepts incoming requests.
 *                  Used for load balancing, can be null.
 * @param repl_proto_ver max replicator protocol version.
 * @param appl_proto_ver max application ptotocol version.
 * @return pointer to GCS connection handle, NULL in case of failure.
 */
extern gcs_conn_t*
gcs_create  (gu_config_t* conf, gcache_t* cache,
             const char* node_name, const char* inc_addr,
             int repl_proto_ver, int appl_proto_ver);

/*! @brief Initialize group history values (optional).
 * Serves to provide group history persistence after process restart (in case
 * these data were saved somewhere on persistent storage or the like). If these
 * values are provided, it is only a hint for the group, as they might be
 * outdated. Actual seqno and UUID are returned in GCS_ACT_CONF action (see
 * below) and are determined by quorum.
 *
 * This function must be called before gcs_open() or after gcs_close().
 *
 * @param seqno Sequence number of the application state (last action applied).
 *              Should be negative for undefined state.
 * @param uuid  UUID of the sequence (group ID).
 *              Should be all zeroes for undefined state.
 *
 * @return 0 in case of success, -EBUSY if conneciton is already opened,
 *         -EBADFD if connection object is being destroyed.
 */
extern long gcs_init (gcs_conn_t   *conn,
                      gcs_seqno_t   seqno,
                      const uint8_t uuid[GCS_UUID_LEN]);

/*! @brief Opens connection to group (joins channel).
 *
 * @param conn connection object
 * @param channel a name of the channel to join. It must uniquely identify
 *                the channel. If the channel with such name does not exist,
 *                it is created. Processes that joined the same channel
 *                receive the same actions.
 * @param url     an URL-like string that specifies backend communication
 *                driver in the form "TYPE://ADDRESS?options". For gcomm
 *                backend it can be "gcomm://localhost:4567", for dummy backend
 *                ADDRESS field is ignored.
 *                Currently supported backend types: "dummy", "vsbes", "gcomm"
 * @param bootstrap bootstrap a new group
 *
 * @return negative error code, 0 in case of success.
 */
extern long gcs_open  (gcs_conn_t *conn,
                       const char *channel,
                       const char *url,
                       bool        bootstrap);

/*! @brief Closes connection to group.
 *
 * @param  conn connection handle
 * @return negative error code or 0 in case of success.
 */
#ifdef GCS_FOR_GARB
extern long gcs_close (gcs_conn_t *conn, bool explicit_close = false);
#else
extern long gcs_close (gcs_conn_t *conn);
#endif

/*! @brief Frees resources associuated with connection handle.
 *
 * @param  conn connection handle
 * @return negative error code or 0 in case of success.
 */
extern long gcs_destroy (gcs_conn_t *conn);

/*! @brief Deprecated. Waits until the group catches up.
 * This call checks if any member of the group (including this one) has a
 * long slave queue. Should be called before gcs_repl(), gcs_send().
 *
 * @return negative error code, 1 if wait is required, 0 otherwise
 */
extern long gcs_wait (gcs_conn_t *conn);

/*! @typedef @brief Action types.
 * There is a conceptual difference between "messages"
 * and "actions". Messages are ELEMENTARY pieces of information
 * atomically delivered by group communication. They are typically
 * limited in size to a single IP packet. Events generated by group
 * communication layer must be delivered as a single message.
 *
 * For the purpose of this work "action" is a higher level concept
 * introduced to overcome the message size limitation. Application
 * replicates information in actions of ARBITRARY size that are
 * fragmented into as many messages as needed. As such actions
 * can be delivered only in primary configuration, when total order
 * of underlying messages is established.
 * The best analogy for action/message concept would be word/letter.
 *
 * The purpose of GCS library is to hide message handling from application.
 * Therefore application deals only with "actions".
 * Application can only send actions of types GCS_ACT_TORDERED,
 * GCS_ACT_COMMIT_CUT and GCS_ACT_STATE_REQ.
 * Actions of type GCS_ACT_SYNC, GCS_ACT_CONF are generated by the library.
 */
typedef enum gcs_act_type
{
/* ordered actions */
    GCS_ACT_TORDERED,   //! action representing state change, will be assigned
                        //  global seqno
    GCS_ACT_COMMIT_CUT, //! group-wide action commit cut
    GCS_ACT_STATE_REQ,  //! request for state transfer
    GCS_ACT_CONF,       //! new configuration
    GCS_ACT_JOIN,       //! joined group (received all state data)
    GCS_ACT_SYNC,       //! synchronized with group
    GCS_ACT_FLOW,       //! flow control
    GCS_ACT_SERVICE,    //! service action, sent by GCS
    GCS_ACT_ERROR,      //! error happened while receiving the action
    GCS_ACT_INCONSISTENCY,//! inconsistency event
    GCS_ACT_UNKNOWN     //! undefined/unknown action type
}
gcs_act_type_t;

/*! String representations of action types */
extern const char* gcs_act_type_to_str(gcs_act_type_t);

/*! @brief Sends a vector of buffers as a single action to group and returns.
 * A copy of action will be returned through gcs_recv() call, or discarded
 * in case it is not delivered by group.
 * For a better means to replicate an action see gcs_repl(). @see gcs_repl()
 *
 * @param conn group connection handle
 * @param act_bufs   action buffer vector
 * @param act_size   total action size (the sum of buffer sizes)
 * @param act_type   action type
 * @param scheduled  whether the call was scheduled by gcs_schedule()
 * @return           negative error code, action size in case of success
 * @retval -EINTR    thread was interrupted while waiting to enter the monitor
 */
extern long gcs_sendv (gcs_conn_t*          conn,
                       const struct gu_buf* act_bufs,
                       size_t               act_size,
                       gcs_act_type_t       act_type,
                       bool                 scheduled);

/*! A wrapper for single buffer communication */
static inline long gcs_send (gcs_conn_t*    const conn,
                             const void*    const act,
                             size_t         const act_size,
                             gcs_act_type_t const act_type,
                             bool           const scheduled)
{
    struct gu_buf const buf = { act, static_cast<ssize_t>(act_size) };
    return gcs_sendv (conn, &buf, act_size, act_type, scheduled);
}

/*!*/
struct gcs_action {
    const void*    buf; /*! unlike input, output goes as a single buffer */
    ssize_t        size;
    gcs_seqno_t    seqno_g;
    gcs_seqno_t    seqno_l;
    gcs_act_type_t type;
    char           sender_id[GU_UUID_STR_LEN+1];
};

/*! @brief Replicates a vector of buffers as a single action.
 * Sends action to group and blocks until it is received. Upon return global
 * and local IDs are set. Arguments are the same as in gcs_recv().
 * @see gcs_recv()
 *
 * @param conn      group connection handle
 * @param act_in    action buffer vector (total size is passed in action)
 * @param action    action struct
 * @param scheduled whether the call was preceded by gcs_schedule()
 * @return          negative error code, action size in case of success
 * @retval -EINTR:  thread was interrupted while waiting to enter the monitor
 */
extern long gcs_replv (gcs_conn_t*          conn,
                       const struct gu_buf* act_in,
                       struct gcs_action*   action,
                       bool                 scheduled);

/*! A wrapper for single buffer communication */
static inline long gcs_repl (gcs_conn_t*        const conn,
                             struct gcs_action* const action,
                             bool               const scheduled)
{
    struct gu_buf const buf = { action->buf, action->size };
    return gcs_replv (conn, &buf, action, scheduled);
}

/*! @brief Receives an action from group.
 * Blocks if no actions are available. Action buffer is allocated by GCS
 * and must be freed by application when action is no longer needed.
 * Also sets global and local action IDs. Global action ID uniquely identifies
 * action in the history of the group and can be used to identify the state
 * of the application for state snapshot purposes. Local action ID is a
 * monotonic gapless number sequence starting with 1 which can be used
 * to serialize access to critical sections.
 *
 * @param conn   group connection handle
 * @param action action object
 * @return       negative error code, action size in case of success,
 * @retval 0     on connection close
 */
extern long gcs_recv (gcs_conn_t*        conn,
                      struct gcs_action* action);

/*!
 * @brief Schedules entry to CGS send monitor.
 * Locks send monitor and should be quickly followed by gcs_repl()/gcs_send()
 *
 * @retval 0       - won't queue
 * @retval >0      - queue handle
 * @retval -EAGAIN - too many queued threads
 * @retval -EBADFD - connection is closed
 */
extern long gcs_schedule (gcs_conn_t* conn);

/*!
 * @brief Interrupt a thread waiting to enter send monitor.
 *
 * @param  conn    GCS connection
 * @param  handle  queue handle returned by @func gcs_schedule(). Must be > 0
 *
 * @retval 0       success
 * @retval -ESRCH  no such thread/already interrupted
 */
extern long gcs_interrupt (gcs_conn_t* conn, long handle);

/*!
 * Resume receivng from group.
 *
 * @param conn     GCS connection
 *
 * @retval 0       success
 * @retval -EBADFD connection is in closed state
 */
extern long gcs_resume_recv (gcs_conn_t* conn);

/*!
 * After action with this seqno is applied, this thread is guaranteed to see
 * all the changes made by the client, even on other nodes.
 *
 * @retval 0       success
 * @retval -EPERM  operation not permitted (in NON_PRIMARY state)
 * @retval -EAGAIN operation may be retried later (in transient state)
 */
extern long gcs_caused (gcs_conn_t* conn, gcs_seqno_t& seqno);

/*! @brief Sends state transfer request
 * Broadcasts state transfer request which will be passed to one of the
 * suitable group members.
 *
 * @param conn  connection to group
 * @param ver   STR version.
 * @param req   opaque byte array that contains data required for
 *              the state transfer (application dependent)
 * @param size  request size
 * @param donor desired state transfer donor name. Supply empty string to
 *              choose automatically.
 * @param seqno response to request was ordered with this seqno.
 *              Must be skipped in local queues.
 * @return negative error code, index of state transfer donor in case of success
 *         (notably, -EAGAIN means try later, -EHOSTUNREACH means desired donor
 *         is unavailable)
 */
extern long gcs_request_state_transfer (gcs_conn_t  *conn,
                                        int          ver,
                                        const void  *req,
                                        size_t       size,
                                        const char  *donor,
                                        const gu_uuid_t* ist_uuid,
                                        gcs_seqno_t ist_seqno,
                                        gcs_seqno_t *seqno);

/*! @brief Turns off flow control on the node.
 * Effectively desynchronizes the node from the cluster (while the node keeps on
 * receiving all the actions). Requires gcs_join() to return to normal.
 *
 * @param conn  connection to group
 * @param seqno response to request was ordered with this seqno.
 *              Must be skipped in local queues.
 * @return negative error code, 0 in case of success.
 */
extern long gcs_desync (gcs_conn_t* conn, gcs_seqno_t* seqno);

/*! @brief Informs group on behalf of donor that state stransfer is over.
 * If status is non-negative, joiner will be considered fully joined to group.
 *
 * @param conn opened connection to group
 * @param status negative error code in case of state transfer failure,
 *               0 or (optional) seqno corresponding to transferred state.
 * @return negative error code, 0 in case of success
 */
extern long gcs_join (gcs_conn_t *conn, gcs_seqno_t status);

/*! @brief Allocate local seqno for accessing local resources.
 *
 *
 * @param conn connection to group
 * @return local seqno, negative error code in case of error
 */
extern gcs_seqno_t gcs_local_sequence(gcs_conn_t* conn);


///////////////////////////////////////////////////////////////////////////////

/* Service functions */

/*! Informs group about the last applied action on this node */
extern long gcs_set_last_applied (gcs_conn_t* conn, gcs_seqno_t seqno);

/* GCS Configuration */

/*! Registers configurable parameters with conf object
 * @return false if success, true if error happened */
extern bool
gcs_register_params (gu_config_t* conf);

/*! sets the key to a given value
 *
 * @return 0 in case of success, 1 if key not found or negative error code */
extern long
gcs_param_set (gcs_conn_t* conn, const char* key, const char *value);

/*! returns the value of the key
 *
 * @return NULL if key not found */
extern const char*
gcs_param_get (gcs_conn_t* conn, const char* key);

/* Logging options */
extern long gcs_conf_set_log_file     (FILE *file);
extern long gcs_conf_set_log_callback (void (*logger) (int, const char*));
extern long gcs_conf_self_tstamp_on   ();
extern long gcs_conf_self_tstamp_off  ();
extern long gcs_conf_debug_on         ();
extern long gcs_conf_debug_off        ();

/* Sending options (deprecated, use gcs_param_set instead) */
/* Sets maximum DESIRED network packet size.
 * For best results should be multiple of MTU */
extern long
gcs_conf_set_pkt_size (gcs_conn_t *conn, long pkt_size);

#define GCS_DEFAULT_PKT_SIZE 64500 /* 43 Eth. frames to carry max IP packet */

/*
 * Configuration action
 */

/*! Possible node states */
typedef enum gcs_node_state
{
    GCS_NODE_STATE_NON_PRIM, /// in non-primary configuration, outdated state
    GCS_NODE_STATE_PRIM,     /// in primary conf, needs state transfer
    GCS_NODE_STATE_JOINER,   /// in primary conf, receiving state transfer
    GCS_NODE_STATE_DONOR,    /// joined, donating state transfer
    GCS_NODE_STATE_JOINED,   /// contains full state
    GCS_NODE_STATE_SYNCED,   /// syncronized with group
    GCS_NODE_STATE_MAX
}
gcs_node_state_t;

/*! Convert state code to null-terminates string */
extern const char*
gcs_node_state_to_str (gcs_node_state_t state);

/*! New configuration action */
typedef struct gcs_act_conf {
    gcs_seqno_t      seqno;    //! last global seqno applied by this group
    gcs_seqno_t      conf_id;  //! configuration ID (-1 if non-primary)
    uint8_t          uuid[GCS_UUID_LEN];/// group UUID
    long             memb_num; //! number of members in configuration
    long             my_idx;   //! index of this node in the configuration
    gcs_node_state_t my_state; //! current node state
    int              repl_proto_ver; //! replicator  protocol version to use
    int              appl_proto_ver; //! application protocol version to use
    char             data[1];  /*! member array (null-terminated ID, name,
                                *  incoming address, 8-byte cached seqno) */
} gcs_act_conf_t;

struct gcs_stats
{
    double    send_q_len_avg; //! average send queue length per send call
    double    recv_q_len_avg; //! average recv queue length per queued action
    long long fc_paused_ns;   //! total nanoseconds spent in paused state
    double    fc_paused_avg;  //! faction of time paused due to flow control
    long long fc_ssent;       //! flow control stops sent
    long long fc_csent;       //! flow control conts sent
    long long fc_received;    //! flow control stops received
    size_t    recv_q_size;    //! current recv queue size
    int       recv_q_len;     //! current recv queue length
    int       recv_q_len_max; //! maximum recv queue length
    int       recv_q_len_min; //! minimum recv queue length
    int       send_q_len;     //! current send queue length
    int       send_q_len_max; //! maximum send queue length
    int       send_q_len_min; //! minimum send queue length
    long      fc_lower_limit; //! Flow-control interval lower limit
    long      fc_upper_limit; //! Flow-control interval upper limit
    int       fc_status;      //! Flow-control status (ON=1/OFF=0)
    bool      fc_active;      //! flow control is currently active
    bool      fc_requested;   //! flow control is requested by this node
};

/*! Fills stats struct */
extern void gcs_get_stats (gcs_conn_t *conn, struct gcs_stats* stats);
/*! flushes stats counters */
extern void gcs_flush_stats(gcs_conn_t *conn);

void gcs_get_status(gcs_conn_t* conn, gu::Status& status);

extern void gcs_join_notification(gcs_conn_t *conn);

extern void
gcs_fetch_pfs_info (gcs_conn_t* conn, wsrep_node_info_t* entries, uint32_t size);

gcs_node_state_t gcs_get_state_for_uuid(gcs_conn_t* conn, gu_uuid_t uuid);

/*! A node with this name will be treated as a stateless arbitrator */
#define GCS_ARBITRATOR_NAME "garb"

#endif // _gcs_h_
