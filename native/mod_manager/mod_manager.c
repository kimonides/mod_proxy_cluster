/*
 *  mod_cluster
 *
 *  Copyright(c) 2008 Red Hat Middleware, LLC,
 *  and individual contributors as indicated by the @authors tag.
 *  See the copyright.txt in the distribution for a
 *  full listing of individual contributors. 
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library in the file COPYING.LIB;
 *  if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place - Suite 330, Boston, MA 02111-1307, USA
 *
 * @author Jean-Frederic Clere
 * @version $Revision$
 */

#include "apr_strings.h"
#include "apr_lib.h"
#include "apr_uuid.h"

#define CORE_PRIVATE
#include "httpd.h"
#include "http_config.h"
#include "http_log.h"
#include "http_main.h"
#include "http_request.h"
#include "http_protocol.h"
#include "http_core.h"
#include "scoreboard.h"
#include "mod_proxy.h"
#include "ap_mpm.h"

#include "mod_proxy_cluster.h"

#include "slotmem.h"

#include "node.h"
#include "host.h"
#include "context.h"
#include "balancer.h"
#include "sessionid.h"
#include "domain.h"

#define DEFMAXCONTEXT   100
#define DEFMAXNODE      20
#define DEFMAXHOST      20
#define DEFMAXSESSIONID 0 /* it has performance/security impact */
#define MAXMESSSIZE     1024

/* Warning messages */
#define SBALBAD "Balancer name contained an upper case character. We will use \"%s\" instead."

/* Error messages */
#define TYPESYNTAX 1
#define SMESPAR "SYNTAX: Can't parse MCMP message. It might have contained illegal symbols or unknown elements."
#define SBALBIG "SYNTAX: Balancer field too big"
#define SBAFBIG "SYNTAX: A field is too big"
#define SROUBIG "SYNTAX: JVMRoute field too big"
#define SROUBAD "SYNTAX: JVMRoute can't be empty"
#define SDOMBIG "SYNTAX: LBGroup field too big"
#define SHOSBIG "SYNTAX: Host field too big"
#define SPORBIG "SYNTAX: Port field too big"
#define STYPBIG "SYNTAX: Type field too big"
#define SALIBAD "SYNTAX: Alias without Context"
#define SCONBAD "SYNTAX: Context without Alias"
#define SBADFLD "SYNTAX: Invalid field \"%s\" in message"
#define SMISFLD "SYNTAX: Mandatory field(s) missing in message"
#define SCMDUNS "SYNTAX: Command is not supported"
#define SMULALB "SYNTAX: Only one Alias in APP command"
#define SMULCTB "SYNTAX: Only one Context in APP command"
#define SREADER "SYNTAX: %s can't read POST data"

#define SJIDBIG "SYNTAX: JGroupUuid field too big"
#define SJDDBIG "SYNTAX: JGroupData field too big"
#define SJIDBAD "SYNTAX: JGroupUuid can't be empty"

#define TYPEMEM 2
#define MNODEUI "MEM: Can't update or insert node with \"%s\" JVMRoute"
#define MNODERM "MEM: Old node with \"%s\" JVMRoute still exists"
#define MBALAUI "MEM: Can't update or insert balancer for node with \"%s\" JVMRoute"
#define MNODERD "MEM: Can't read node with \"%s\" JVMRoute"
#define MHOSTRD "MEM: Can't read host alias for node with \"%s\" JVMRoute"
#define MHOSTUI "MEM: Can't update or insert host alias for node with \"%s\" JVMRoute"
#define MCONTUI "MEM: Can't update or insert context for node with \"%s\" JVMRoute"
#define MJBIDRD "MEM: Can't read JGroupId"
#define MJBIDUI "MEM: Can't update or insert JGroupId"

/* Protocol version supported */
#define VERSION_PROTOCOL "0.2.1"

/* Internal substitution for node commands */
#define NODE_COMMAND "/NODE_COMMAND"

/* range of the commands */
#define RANGECONTEXT 0
#define RANGENODE    1
#define RANGEDOMAIN  2

/* define HAVE_CLUSTER_EX_DEBUG to have extented debug in mod_cluster */
#define HAVE_CLUSTER_EX_DEBUG 0

/* define content-type */
#define TEXT_PLAIN 1
#define TEXT_XML 2

/* Data structure for shared memory block */
typedef struct version_data {
    apr_uint64_t counter;
} version_data;

/* mutex and lock for tables/slotmen */
static apr_thread_mutex_t *nodes_global_mutex = NULL;
static apr_file_t *nodes_global_lock = NULL;
static apr_thread_mutex_t *contexts_global_mutex = NULL;
static apr_file_t *contexts_global_lock = NULL;

/* counter for the version (nodes) */
static apr_shm_t *versionipc_shm = NULL;

/* shared memory */
static mem_t *contextstatsmem = NULL;
static mem_t *nodestatsmem = NULL;
static mem_t *hoststatsmem = NULL;
static mem_t *balancerstatsmem = NULL;
static mem_t *sessionidstatsmem = NULL;
static mem_t *domainstatsmem = NULL;

static slotmem_storage_method *storage = NULL;
static balancer_method *balancerhandler = NULL;
static void (*advertise_info)(request_rec *) = NULL;

module AP_MODULE_DECLARE_DATA manager_module;

static char balancer_nonce[APR_UUID_FORMATTED_LENGTH + 1];

typedef struct mod_manager_config
{
    /* base name for the shared memory */
    char *basefilename;
    /* max number of context supported */
    int maxcontext;
    /* max number of node supported */
    int maxnode;
    /* max number of host supported */
    int maxhost;
    /* max number of session supported */
    int maxsessionid;

    /* version, the version is increased each time the node update logic is called */
    unsigned int tableversion;

    /* Should be the slotmem persisted (1) or not (0) */
    int persistent;

    /* check for nonce in the command logic */
    int nonce;

    /* default name for balancer */
    char *balancername;

    /* allow aditional display */
    int allow_display;
    /* allow command logic */
    int allow_cmd;
    /* don't context in first status page */
    int reduce_display;  
    /* maximum message size */
    int maxmesssize;
    /* Enable MCPM receiver */
    int enable_mcpm_receive;
    /* Enable WebSocket Proxy */
    int enable_ws_tunnel;

} mod_manager_config;

/*
 * routines for the node_storage_method
 */
static apr_status_t loc_read_node(int ids, nodeinfo_t **node)
{
    return (get_node(nodestatsmem, node, ids));
}
static int loc_get_ids_used_node(int *ids)
{
    return(get_ids_used_node(nodestatsmem, ids)); 
}
static int loc_get_max_size_node(void)
{
    if (nodestatsmem)
        return(get_max_size_node(nodestatsmem));
    else
        return 0;
}
static apr_status_t loc_remove_node(nodeinfo_t *node)
{
    return (remove_node(nodestatsmem, node));
}
static apr_status_t loc_find_node(nodeinfo_t **node, const char *route)
{
    return (find_node(nodestatsmem, node, route));
}

/*
 * Increase the version of the nodes table
 */
static void inc_version_node(void)
{
    version_data *base;
    base = (version_data *)apr_shm_baseaddr_get(versionipc_shm);
    base->counter++;
}

/* Check is the nodes (in shared memory) were modified since last
 * call to worker_nodes_are_updated().
 * return codes:
 *   0 : No update of the nodes since last time.
 *   x: The version has changed the local table need to be updated.
 */
static unsigned int loc_worker_nodes_need_update(void *data, apr_pool_t *pool)
{
    int size;
    server_rec *s = (server_rec *) data;
    unsigned int last = 0;
    version_data *base;
    mod_manager_config *mconf = ap_get_module_config(s->module_config, &manager_module);

    size = loc_get_max_size_node();
    if (size == 0)
        return 0; /* broken */

    base = (version_data *)apr_shm_baseaddr_get(versionipc_shm);
    last = base->counter;

    if (last != mconf->tableversion)
        return last;
    return (0);
}
/* Store the last version update in the proccess config */
static int loc_worker_nodes_are_updated(void *data, unsigned int last)
{
    server_rec *s = (server_rec *) data;
    mod_manager_config *mconf = ap_get_module_config(s->module_config, &manager_module);
    mconf->tableversion = last;
    return (0);
}
static apr_status_t lock_memory(apr_file_t *file, apr_thread_mutex_t *mutex)
{
    apr_status_t rv;
    rv = apr_file_lock(file, APR_FLOCK_EXCLUSIVE);
    if (rv != APR_SUCCESS)
        return rv;
    rv = apr_thread_mutex_lock(mutex);
    if (rv != APR_SUCCESS)
        apr_file_unlock(file);
    return rv;
}
static apr_status_t unlock_memory(apr_file_t *file, apr_thread_mutex_t *mutex)
{
    apr_thread_mutex_unlock(mutex);
    return(apr_file_unlock(file));
}
static apr_status_t loc_lock_nodes(void)
{
    return(lock_memory(nodes_global_lock, nodes_global_mutex));
}
static apr_status_t loc_unlock_nodes(void)
{
    return(unlock_memory(nodes_global_lock, nodes_global_mutex));
}
static int loc_get_max_size_context(void)
{
    if (contextstatsmem)
        return(get_max_size_context(contextstatsmem));
    else
        return 0;
}
static int loc_get_max_size_host(void)
{
    if (hoststatsmem)
        return(get_max_size_host(hoststatsmem));
    else
        return 0;
}
/* Remove the virtual hosts and contexts corresponding the node */
static void loc_remove_host_context(int node, apr_pool_t *pool)
{
    /* for read the hosts */
    int i;
    int size = loc_get_max_size_host();
    int *id;
    int sizecontext = loc_get_max_size_context();
    int *idcontext;

    if (size == 0)
        return;
    id = apr_palloc(pool, sizeof(int) * size);
    idcontext = apr_palloc(pool, sizeof(int) * sizecontext);
    size = get_ids_used_host(hoststatsmem, id);
    for (i=0; i<size; i++) {
        hostinfo_t *ou;

        if (get_host(hoststatsmem, &ou, id[i]) != APR_SUCCESS)
            continue;
        if (ou->node == node)
            remove_host(hoststatsmem, ou);
    }

    sizecontext = get_ids_used_context(contextstatsmem, idcontext);
    for (i=0; i<sizecontext; i++) {
        contextinfo_t *context;
        if (get_context(contextstatsmem, &context, idcontext[i]) != APR_SUCCESS)
            continue;
        if (context->node == node)
            remove_context(contextstatsmem, context);
    }
}
static const struct node_storage_method node_storage =
{
    loc_read_node,
    loc_get_ids_used_node,
    loc_get_max_size_node,
    loc_worker_nodes_need_update,
    loc_worker_nodes_are_updated,
    loc_remove_node,
    loc_find_node,
    loc_remove_host_context,
    loc_lock_nodes,
    loc_unlock_nodes
};

/*
 * routines for the context_storage_method
 */
static apr_status_t loc_read_context(int ids, contextinfo_t **context)
{
    return (get_context(contextstatsmem, context, ids));
}
static int loc_get_ids_used_context(int *ids)
{
    return(get_ids_used_context(contextstatsmem, ids)); 
}
static apr_status_t loc_lock_contexts(void)
{
    return(lock_memory(contexts_global_lock, contexts_global_mutex));
}
static apr_status_t loc_unlock_contexts(void)
{
    return(unlock_memory(contexts_global_lock, contexts_global_mutex));
}
static const struct context_storage_method context_storage =
{
    loc_read_context,
    loc_get_ids_used_context,
    loc_get_max_size_context,
    loc_lock_contexts,
    loc_unlock_contexts
};

/*
 * routines for the host_storage_method
 */
static apr_status_t loc_read_host(int ids, hostinfo_t **host)
{
    return (get_host(hoststatsmem, host, ids));
}
static int loc_get_ids_used_host(int *ids)
{
    return(get_ids_used_host(hoststatsmem, ids)); 
}
static const struct host_storage_method host_storage =
{
    loc_read_host,
    loc_get_ids_used_host,
    loc_get_max_size_host
};

/*
 * routines for the balancer_storage_method
 */
static apr_status_t loc_read_balancer(int ids, balancerinfo_t **balancer)
{
    return (get_balancer(balancerstatsmem, balancer, ids));
}
static int loc_get_ids_used_balancer(int *ids)
{
    return(get_ids_used_balancer(balancerstatsmem, ids)); 
}
static int loc_get_max_size_balancer(void)
{
    if (balancerstatsmem)
        return(get_max_size_balancer(balancerstatsmem));
    else
        return 0;
}
static const struct balancer_storage_method balancer_storage =
{
    loc_read_balancer,
    loc_get_ids_used_balancer,
    loc_get_max_size_balancer
};
/*
 * routines for the sessionid_storage_method
 */
static apr_status_t loc_read_sessionid(int ids, sessionidinfo_t **sessionid)
{
    return (get_sessionid(sessionidstatsmem, sessionid, ids));
}
static int loc_get_ids_used_sessionid(int *ids)
{
    return(get_ids_used_sessionid(sessionidstatsmem, ids)); 
}
static int loc_get_max_size_sessionid(void)
{
    if (sessionidstatsmem)
        return(get_max_size_sessionid(sessionidstatsmem));
    else
        return 0;
}
static apr_status_t loc_remove_sessionid(sessionidinfo_t *sessionid)
{
    return (remove_sessionid(sessionidstatsmem, sessionid));
}
static apr_status_t loc_insert_update_sessionid(sessionidinfo_t *sessionid)
{
    return (insert_update_sessionid(sessionidstatsmem, sessionid));
}
static const struct  sessionid_storage_method sessionid_storage =
{
    loc_read_sessionid,
    loc_get_ids_used_sessionid,
    loc_get_max_size_sessionid,
    loc_remove_sessionid,
    loc_insert_update_sessionid
};

/*
 * routines for the domain_storage_method
 */
static apr_status_t loc_read_domain(int ids, domaininfo_t **domain)
{
    return (get_domain(domainstatsmem, domain, ids));
}
static int loc_get_ids_used_domain(int *ids)
{
    return(get_ids_used_domain(domainstatsmem, ids)); 
}
static int loc_get_max_size_domain(void)
{
    if (domainstatsmem)
        return(get_max_size_domain(domainstatsmem));
    else
        return 0;
}
static apr_status_t loc_remove_domain(domaininfo_t *domain)
{
    return (remove_domain(domainstatsmem, domain));
}
static apr_status_t loc_insert_update_domain(domaininfo_t *domain)
{
    return (insert_update_domain(domainstatsmem, domain));
}
static apr_status_t loc_find_domain(domaininfo_t **domain, const char *route, const char *balancer)
{
    return (find_domain(domainstatsmem, domain, route, balancer));
}
static const struct  domain_storage_method domain_storage =
{
    loc_read_domain,
    loc_get_ids_used_domain,
    loc_get_max_size_domain,
    loc_remove_domain,
    loc_insert_update_domain,
    loc_find_domain
};

/* helper for the handling of the Alias: host1,... Context: context1,... */
struct cluster_host {
    char *host;
    char *context;
    struct cluster_host *next;
};

/*
 * cleanup logic
 */
static apr_status_t cleanup_manager(void *param)
{
    /* shared memory */
    contextstatsmem = NULL;
    nodestatsmem = NULL;
    hoststatsmem = NULL;
    balancerstatsmem = NULL;
    sessionidstatsmem = NULL;
    domainstatsmem = NULL;
    if (nodes_global_lock) {
        apr_file_close(nodes_global_lock);
        nodes_global_lock = NULL;
    }
    if (contexts_global_lock) {
        apr_file_close(contexts_global_lock);
        contexts_global_lock = NULL;
    }
    if (versionipc_shm) {
        apr_shm_destroy(versionipc_shm);
        versionipc_shm = NULL;
    }
    return APR_SUCCESS;
}
static void mc_initialize_cleanup(apr_pool_t *p)
{
    apr_pool_cleanup_register(p, NULL, cleanup_manager, apr_pool_cleanup_null);
}

static void normalize_balancer_name(char* balancer_name, server_rec *s)
{
    int upper_case_char_found = 0;
    char* balancer_name_start = balancer_name;
    for (;*balancer_name; ++balancer_name) {
        if(!upper_case_char_found) {
            upper_case_char_found = apr_isupper(*balancer_name);
        }
        *balancer_name = apr_tolower(*balancer_name);
    }
    balancer_name = balancer_name_start;
    if(upper_case_char_found) {
        ap_log_error(APLOG_MARK, APLOG_NOERRNO|APLOG_WARNING, 0, s, SBALBAD, balancer_name);
    }
}

/*
 * Whether the module is called from a MPM that re-enter main() and
 * pre/post_config phases.
 */
static APR_INLINE int is_child_process(void)
{
#ifdef WIN32
    return getenv("AP_PARENT_PID") != NULL;
#else
    return 0;
#endif
}

/*
 * call after parser the configuration.
 * create the shared memory.
 */
static int manager_init(apr_pool_t *p, apr_pool_t *plog,
                          apr_pool_t *ptemp, server_rec *s)
{
    char *node;
    char *context;
    char *host;
    char *balancer;
    char *sessionid;
    char *domain;
    char *version;
    version_data *base;
    void *data;
    const char *userdata_key = "mod_manager_init";
    apr_uuid_t uuid;
    mod_manager_config *mconf = ap_get_module_config(s->module_config, &manager_module);
    apr_status_t rv;
    apr_pool_userdata_get(&data, userdata_key, s->process->pool);
    if (!data) {
        /* first call do nothing */
        apr_pool_userdata_set((const void *)1, userdata_key, apr_pool_cleanup_null, s->process->pool);
        return OK;
    }

    if (mconf->basefilename) {
        node = apr_pstrcat(ptemp, mconf->basefilename, "/manager.node", NULL);
        context = apr_pstrcat(ptemp, mconf->basefilename, "/manager.context", NULL);
        host = apr_pstrcat(ptemp, mconf->basefilename, "/manager.host", NULL);
        balancer = apr_pstrcat(ptemp, mconf->basefilename, "/manager.balancer", NULL);
        sessionid = apr_pstrcat(ptemp, mconf->basefilename, "/manager.sessionid", NULL);
        domain = apr_pstrcat(ptemp, mconf->basefilename, "/manager.domain", NULL);
        version = apr_pstrcat(ptemp, mconf->basefilename, "/manager.version", NULL);
    } else {
        node = ap_server_root_relative(ptemp, "logs/manager.node");
        context = ap_server_root_relative(ptemp, "logs/manager.context");
        host = ap_server_root_relative(ptemp, "logs/manager.host");
        balancer = ap_server_root_relative(ptemp, "logs/manager.balancer");
        sessionid = ap_server_root_relative(ptemp, "logs/manager.sessionid");
        domain = ap_server_root_relative(ptemp, "logs/manager.domain");
        version = ap_server_root_relative(ptemp, "logs/manager.version");
    }

    /* Do some sanity checks */
    if (mconf->maxhost < mconf->maxnode)
        mconf->maxhost = mconf->maxnode;
    if (mconf->maxcontext < mconf->maxhost)
        mconf->maxcontext = mconf->maxhost;

    /* Get a provider to handle the shared memory */
    storage = ap_lookup_provider(SLOTMEM_STORAGE, "shared", "0");
    if (storage == NULL) {
        ap_log_error(APLOG_MARK, APLOG_NOERRNO|APLOG_EMERG, 0, s, "ap_lookup_provider %s failed", SLOTMEM_STORAGE);
        return  !OK;
    }
    nodestatsmem = create_mem_node(node, &mconf->maxnode, mconf->persistent, p, storage);
    if (nodestatsmem == NULL) {
        ap_log_error(APLOG_MARK, APLOG_NOERRNO|APLOG_EMERG, 0, s, "create_mem_node %s failed", node);
        return  !OK;
    }
    if (get_last_mem_error(nodestatsmem) != APR_SUCCESS) {
        char buf[120];
        ap_log_error(APLOG_MARK, APLOG_NOERRNO|APLOG_EMERG, 0, s, "create_mem_node %s failed: %s",
                     node, apr_strerror(get_last_mem_error(nodestatsmem), buf, sizeof(buf)));
        return  !OK;
    }

    contextstatsmem = create_mem_context(context, &mconf->maxcontext, mconf->persistent, p, storage);
    if (contextstatsmem == NULL) {
        ap_log_error(APLOG_MARK, APLOG_NOERRNO|APLOG_EMERG, 0, s, "create_mem_context failed");
        return  !OK;
    }

    hoststatsmem = create_mem_host(host, &mconf->maxhost, mconf->persistent, p, storage);
    if (hoststatsmem == NULL) {
        ap_log_error(APLOG_MARK, APLOG_NOERRNO|APLOG_EMERG, 0, s, "create_mem_host failed");
        return  !OK;
    }

    balancerstatsmem = create_mem_balancer(balancer, &mconf->maxhost, mconf->persistent, p, storage);
    if (balancerstatsmem == NULL) {
        ap_log_error(APLOG_MARK, APLOG_NOERRNO|APLOG_EMERG, 0, s, "create_mem_balancer failed");
        return  !OK;
    }

    if (mconf->maxsessionid) {
        /* Only create sessionid stuff if required */
        sessionidstatsmem = create_mem_sessionid(sessionid, &mconf->maxsessionid, mconf->persistent, p, storage);
        if (sessionidstatsmem == NULL) {
            ap_log_error(APLOG_MARK, APLOG_NOERRNO|APLOG_EMERG, 0, s, "create_mem_sessionid failed");
            return  !OK;
        }
    }

    domainstatsmem = create_mem_domain(domain, &mconf->maxnode, mconf->persistent, p, storage);
    if (domainstatsmem == NULL) {
        ap_log_error(APLOG_MARK, APLOG_NOERRNO|APLOG_EMERG, 0, s, "create_mem_domain failed");
        return  !OK;
    }

    if (is_child_process()) {
        rv = apr_shm_attach(&versionipc_shm, (const char *) version, p);
    } else {
        rv = apr_shm_create(&versionipc_shm, sizeof(version_data), (const char *) version, p);
    }
    if (rv != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_EMERG, rv, s, "create_share_version failed");
        return  !OK;
    }
    base = (version_data *)apr_shm_baseaddr_get(versionipc_shm);
    base->counter = 0;

    /* Get a provider to ping/pong logics */

    balancerhandler = ap_lookup_provider("proxy_cluster", "balancer", "0");
    if (balancerhandler == NULL) {
        ap_log_error(APLOG_MARK, APLOG_NOERRNO|APLOG_WARNING, 0, s, "can't find a ping/pong logic");
    }

    advertise_info = ap_lookup_provider("advertise", "info", "0");

    /*
     * Retrieve a UUID and store the nonce.
     */
    apr_uuid_get(&uuid);
    apr_uuid_format(balancer_nonce, &uuid);

    /*
     * clean up to prevent backgroup thread (proxy_cluster_watchdog_func) to crash
     */
    mc_initialize_cleanup(p);

    return OK;
}
static apr_status_t decodeenc(char **ptr);
static char **process_buff(request_rec *r, char *buff)
{
    int i = 0;
    char *s = buff;
    char **ptr = NULL;
    for (; *s != '\0'; s++) {
        if (*s == '&' || *s == '=') {
            i++;
        }
    }
    ptr = apr_palloc(r->pool, sizeof(char *) * (i + 2));
    if (ptr == NULL)
        return NULL;

    s = buff;
    ptr[0] = s;
    ptr[i+1] = NULL;
    i = 1;
    for (; *s != '\0'; s++) {
        /* our separators */
        if (*s == '&' || *s == '=') {
            *s = '\0';
            ptr[i] = s + 1;
            i++;
        }
    }

    if (decodeenc(ptr) != APR_SUCCESS) {
        return NULL;
    }

    return ptr;
}
/*
 * Insert the hosts from Alias information
 */
static apr_status_t  insert_update_hosts(mem_t *mem, char *str, int node, int vhost)
{
    char *ptr = str;
    char *previous = str;
    hostinfo_t info;
    char empty[1] = {'\0'};
    apr_status_t status;

    info.node = node;
    info.vhost = vhost;
    if (ptr == NULL) {
        ptr = empty;
        previous = ptr;
    }
    while (*ptr) {
        if (*ptr == ',') {
            *ptr = '\0';
            strncpy(info.host, previous, HOSTALIASZ);
            status = insert_update_host(mem, &info); 
            if (status != APR_SUCCESS)
                return status;
            previous = ptr + 1;
        }
        ptr ++;
    }
    strncpy(info.host, previous, sizeof(info.host));
    return insert_update_host(mem, &info); 
}
/*
 * Insert the context from Context information
 * Note:
 * 1 - if status is REMOVE remove_context will be called.
 * 2 - return codes of REMOVE are ignored (always success).
 *
 */
static apr_status_t  insert_update_contexts(mem_t *mem, char *str, int node, int vhost, int status)
{
    char *ptr = str;
    char *previous = str;
    apr_status_t ret = APR_SUCCESS;
    contextinfo_t info;
    char empty[2] = {'/','\0'};

    info.node = node;
    info.vhost = vhost;
    info.status = status;
    if (ptr == NULL) {
        ptr = empty;
        previous = ptr;
    }
    while (*ptr) {
        if (*ptr == ',') {
            *ptr = '\0';
            info.id = 0;
            strncpy(info.context, previous, sizeof(info.context));
            if (status != REMOVE) {
                ret = insert_update_context(mem, &info);
                if (ret != APR_SUCCESS)
                    return ret;
            } else
                remove_context(mem, &info);

            previous = ptr + 1;
        }
        ptr ++;
    }
    info.id = 0;
    strncpy(info.context, previous, sizeof(info.context));
    if (status != REMOVE)
        ret = insert_update_context(mem, &info); 
    else
        remove_context(mem, &info);
    return ret;
}
/*
 * Check that the node could be handle as is there were the same.
 */
static int  is_same_node(nodeinfo_t *nodeinfo, nodeinfo_t *node) {
    if (strcmp(nodeinfo->mess.balancer,node->mess.balancer))
        return 0;
    if (strcmp(nodeinfo->mess.Host, node->mess.Host))
        return 0;
    if (strcmp(nodeinfo->mess.Port,node->mess.Port))
        return 0;
    if (strcmp(nodeinfo->mess.Type, node->mess.Type))
        return 0;
    if (nodeinfo->mess.reversed != node->mess.reversed)
        return 0;

    /* Those means the reslist has to be changed */
    if (nodeinfo->mess.smax !=  node->mess.smax)
        return 0;
    if (nodeinfo->mess.ttl != node->mess.ttl)
        return 0;

    /* All other fields can be modified without causing problems */
    return -1;
}

/*
 * Process a CONFIG message
 * Balancer: <Balancer name>
 * <balancer configuration>
 * StickySession	StickySessionCookie	StickySessionPath	StickySessionRemove
 * StickySessionForce	Timeout	Maxattempts
 * JvmRoute?: <JvmRoute>
 * Domain: <Domain>
 * <Host: <Node IP>
 * Port: <Connector Port>
 * Type: <Type of the connector>
 * Reserved: <Use connection pool initiated by Tomcat *.>
 * <node conf>
 * flushpackets	flushwait	ping	smax	ttl
 * Virtual hosts in JBossAS
 * Alias: <vhost list>
 * Context corresponding to the applications.
 * Context: <context list>
 */
static char * process_config(request_rec *r, char **ptr, int *errtype)
{
    /* Process the node/balancer description */
    nodeinfo_t nodeinfo;
    nodeinfo_t *node;
    balancerinfo_t balancerinfo;
    int mpm_threads;
    
    struct cluster_host *vhost; 
    struct cluster_host *phost; 

    int i = 0;
    int id;
    int vid = 1; /* zero and "" is empty */
    void *sconf = r->server->module_config;
    mod_manager_config *mconf = ap_get_module_config(sconf, &manager_module);

    vhost = apr_palloc(r->pool, sizeof(struct cluster_host));

    /* Map nothing by default */
    vhost->host = NULL;
    vhost->context = NULL;
    vhost->next = NULL;
    phost = vhost;

    /* Fill default nodes values */
    memset(&nodeinfo.mess, '\0', sizeof(nodeinfo.mess));
    if (mconf->balancername != NULL) {
        normalize_balancer_name(mconf->balancername, r->server);
        strncpy(nodeinfo.mess.balancer, mconf->balancername, sizeof(nodeinfo.mess.balancer));
        nodeinfo.mess.balancer[sizeof(nodeinfo.mess.balancer) -1] = '\0';
    } else {
        strcpy(nodeinfo.mess.balancer, "mycluster");
    }
    strcpy(nodeinfo.mess.Host, "localhost");
    strcpy(nodeinfo.mess.Port, "8009");
    strcpy(nodeinfo.mess.Type, "ajp");
    nodeinfo.mess.reversed = 0;
    nodeinfo.mess.remove = 0; /* not marked as removed */
    nodeinfo.mess.flushpackets = flush_off; /* FLUSH_OFF; See enum flush_packets in proxy.h flush_off */
    nodeinfo.mess.flushwait = PROXY_FLUSH_WAIT;
    nodeinfo.mess.ping = apr_time_from_sec(10);
    ap_mpm_query(AP_MPMQ_MAX_THREADS, &mpm_threads);
    nodeinfo.mess.smax = mpm_threads + 1;
    nodeinfo.mess.ttl = apr_time_from_sec(60);
    nodeinfo.mess.timeout = 0;
    nodeinfo.mess.id = 0;
    nodeinfo.mess.lastcleantry = 0;

    /* Fill default balancer values */
    memset(&balancerinfo, '\0', sizeof(balancerinfo));
    if (mconf->balancername != NULL) {
        normalize_balancer_name(mconf->balancername, r->server);
        strncpy(balancerinfo.balancer, mconf->balancername, sizeof(balancerinfo.balancer));
        balancerinfo.balancer[sizeof(balancerinfo.balancer) - 1] = '\0';
    } else {
        strcpy(balancerinfo.balancer, "mycluster");
    }
    balancerinfo.StickySession = 1;
    balancerinfo.StickySessionForce = 1;
    strcpy(balancerinfo.StickySessionCookie, "JSESSIONID");
    strcpy(balancerinfo.StickySessionPath, "jsessionid");
    balancerinfo.Maxattempts = 1;
    balancerinfo.Timeout = 0;

    while (ptr[i]) {
        /* XXX: balancer part */
        if (strcasecmp(ptr[i], "Balancer") == 0) {
            if (strlen(ptr[i+1])>=sizeof(nodeinfo.mess.balancer)) {
                *errtype = TYPESYNTAX;
                return SBALBIG;
            }
            normalize_balancer_name(ptr[i+1], r->server);
            strncpy(nodeinfo.mess.balancer, ptr[i+1], sizeof(nodeinfo.mess.balancer));
            nodeinfo.mess.balancer[sizeof(nodeinfo.mess.balancer) - 1] = '\0';
            strncpy(balancerinfo.balancer, ptr[i+1], sizeof(balancerinfo.balancer));
            balancerinfo.balancer[sizeof(balancerinfo.balancer) - 1] = '\0';
        }
        if (strcasecmp(ptr[i], "StickySession") == 0) {
            if (strcasecmp(ptr[i+1], "no") == 0)
                balancerinfo.StickySession = 0;
        }
        if (strcasecmp(ptr[i], "StickySessionCookie") == 0) {
            if (strlen(ptr[i+1])>=sizeof(balancerinfo.StickySessionCookie)) {
                *errtype = TYPESYNTAX;
                return SBAFBIG;
            }
            strcpy(balancerinfo.StickySessionCookie, ptr[i+1]);
        }
        if (strcasecmp(ptr[i], "StickySessionPath") == 0) {
            if (strlen(ptr[i+1])>=sizeof(balancerinfo.StickySessionPath)) {
                *errtype = TYPESYNTAX;
                return SBAFBIG;
            }
            strcpy(balancerinfo.StickySessionPath, ptr[i+1]);
        }
        if (strcasecmp(ptr[i], "StickySessionRemove") == 0) {
            if (strcasecmp(ptr[i+1], "yes") == 0)
                balancerinfo.StickySessionRemove = 1;
        }
        if (strcasecmp(ptr[i], "StickySessionForce") == 0) {
            if (strcasecmp(ptr[i+1], "no") == 0)
                balancerinfo.StickySessionForce = 0;
        }
        /* Note that it is workerTimeout (set/getWorkerTimeout in java code) */ 
        if (strcasecmp(ptr[i], "WaitWorker") == 0) {
            balancerinfo.Timeout = apr_time_from_sec(atoi(ptr[i+1]));
        }
        if (strcasecmp(ptr[i], "Maxattempts") == 0) {
            balancerinfo.Maxattempts = atoi(ptr[i+1]);
        }

        /* XXX: Node part */
        if (strcasecmp(ptr[i], "JVMRoute") == 0) {
            if (strlen(ptr[i+1])>=sizeof(nodeinfo.mess.JVMRoute)) {
                *errtype = TYPESYNTAX;
                return SROUBIG;
            }
            strcpy(nodeinfo.mess.JVMRoute, ptr[i+1]);
        }
        /* We renamed it LBGroup */
        if (strcasecmp(ptr[i], "Domain") == 0) {
            if (strlen(ptr[i+1])>=sizeof(nodeinfo.mess.Domain)) {
                *errtype = TYPESYNTAX;
                return SDOMBIG;
            }
            strcpy(nodeinfo.mess.Domain, ptr[i+1]);
        }
        if (strcasecmp(ptr[i], "Host") == 0) {
            char *p_read = ptr[i+1], *p_write = ptr[i+1];
            int flag = 0;
            if (strlen(ptr[i+1])>=sizeof(nodeinfo.mess.Host)) {
                *errtype = TYPESYNTAX;
                return SHOSBIG;
            }

            /* Removes %zone from an address */
            if (*p_read == '[') {
                while (*p_read) {
                    *p_write = *p_read++;
                    if ((*p_write == '%' || flag) && *p_write != ']') {
                        flag = 1;
                    } else {
                        p_write++;
                    }
                }
                *p_write = '\0';
            }

            strcpy(nodeinfo.mess.Host, ptr[i+1]);
        }
        if (strcasecmp(ptr[i], "Port") == 0) {
            if (strlen(ptr[i+1])>=sizeof(nodeinfo.mess.Port)) {
                *errtype = TYPESYNTAX;
                return SPORBIG;
            }
            strcpy(nodeinfo.mess.Port, ptr[i+1]);
        }
        if (strcasecmp(ptr[i], "Type") == 0) {
            if (strlen(ptr[i+1])>=sizeof(nodeinfo.mess.Type)) {
                *errtype = TYPESYNTAX;
                return STYPBIG;
            }
            strcpy(nodeinfo.mess.Type, ptr[i+1]);
        }
        if (strcasecmp(ptr[i], "Reversed") == 0) {
            if (strcasecmp(ptr[i+1], "yes") == 0) {
            nodeinfo.mess.reversed = 1;
            }
        }
        if (strcasecmp(ptr[i], "flushpackets") == 0) {
            if (strcasecmp(ptr[i+1], "on") == 0) {
                nodeinfo.mess.flushpackets = flush_on;
            }
            else if (strcasecmp(ptr[i+1], "auto") == 0) {
                nodeinfo.mess.flushpackets = flush_auto;
            }
        }
        if (strcasecmp(ptr[i], "flushwait") == 0) {
            nodeinfo.mess.flushwait = atoi(ptr[i+1]) * 1000;
        }
        if (strcasecmp(ptr[i], "ping") == 0) {
            nodeinfo.mess.ping = apr_time_from_sec(atoi(ptr[i+1]));
        }
        if (strcasecmp(ptr[i], "smax") == 0) {
            nodeinfo.mess.smax = atoi(ptr[i+1]);
        }
        if (strcasecmp(ptr[i], "ttl") == 0) {
            nodeinfo.mess.ttl = apr_time_from_sec(atoi(ptr[i+1]));
        }
        if (strcasecmp(ptr[i], "Timeout") == 0) {
            nodeinfo.mess.timeout = apr_time_from_sec(atoi(ptr[i+1]));
        }

        /* Hosts and contexts (optional paramters) */
        if (strcasecmp(ptr[i], "Alias") == 0) {
            if (phost->host && !phost->context) {
                *errtype = TYPESYNTAX;
                return SALIBAD;
            }
            if (phost->host) {
               phost->next = apr_palloc(r->pool, sizeof(struct cluster_host));
               phost = phost->next;
               phost->next = NULL;
               phost->host = ptr[i+1];
               phost->context = NULL;
            } else {
               phost->host = ptr[i+1];
            }
        }
        if (strcasecmp(ptr[i], "Context") == 0) {
            if (phost->context) {
                *errtype = TYPESYNTAX;
                return SCONBAD;
            }
            phost->context = ptr[i+1];
        }
        i++;
        i++;
    }

    /* Check for JVMRoute */
    if (nodeinfo.mess.JVMRoute[0] == '\0') {
        *errtype = TYPESYNTAX;
        return SROUBAD;
    }

    if ( mconf->enable_ws_tunnel && strcmp(nodeinfo.mess.Type, "ajp")) {
        if (!strcmp(nodeinfo.mess.Type, "http"))
            strcpy(nodeinfo.mess.Type, "ws");
        if (!strcmp(nodeinfo.mess.Type, "https"))
            strcpy(nodeinfo.mess.Type, "wss");
    }
    /* Insert or update balancer description */
    if (insert_update_balancer(balancerstatsmem, &balancerinfo) != APR_SUCCESS) {
        *errtype = TYPEMEM;
        return apr_psprintf(r->pool, MBALAUI, nodeinfo.mess.JVMRoute);
    }

    /* check for removed node */
    loc_lock_nodes();
    node = read_node(nodestatsmem, &nodeinfo);
    if (node != NULL) {
        /* If the node is removed (or kill and restarted) and recreated unchanged that is ok: network problems */
        if (! is_same_node(node, &nodeinfo)) {
            /* Here we can't update it because the old one is still in */
            ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                         "process_config: node %s already exist", node->mess.JVMRoute);
            strcpy(node->mess.JVMRoute, "REMOVED");
            node->mess.remove = 1;
            insert_update_node(nodestatsmem, node, &id);
            loc_remove_host_context(node->mess.id, r->pool);
            inc_version_node();
            loc_unlock_nodes();
            *errtype = TYPEMEM;
            return apr_psprintf(r->pool, MNODERM, node->mess.JVMRoute);
        }
    }

    /* Insert or update node description */
    if (insert_update_node(nodestatsmem, &nodeinfo, &id) != APR_SUCCESS) {
        loc_unlock_nodes();
        *errtype = TYPEMEM;
        return apr_psprintf(r->pool, MNODEUI, nodeinfo.mess.JVMRoute);
    }
    inc_version_node();

    /* Insert the Alias and corresponding Context */
    phost = vhost;
    if (phost->host == NULL && phost->context == NULL) {
        loc_unlock_nodes();
        return NULL; /* Alias and Context missing */
    }
    while (phost) {
        if (insert_update_hosts(hoststatsmem, phost->host, id, vid) != APR_SUCCESS) {
            loc_unlock_nodes();
            return apr_psprintf(r->pool, MHOSTUI, nodeinfo.mess.JVMRoute);
        }
        if (insert_update_contexts(contextstatsmem, phost->context, id, vid, STOPPED) != APR_SUCCESS) {
            loc_unlock_nodes();
            return apr_psprintf(r->pool, MCONTUI, nodeinfo.mess.JVMRoute);
        }
        phost = phost->next;
        vid++;
    }
    loc_unlock_nodes();
    return NULL;
}
/*
 * Process a DUMP command.
 */
static char * process_dump(request_rec *r, int *errtype)
{
    int size, i;
    int *id;

    unsigned char type;
    const char *accept_header = apr_table_get(r->headers_in, "Accept");

    if (accept_header && strstr((char *)accept_header, "text/xml") != NULL )  {
        ap_set_content_type(r, "text/xml");
        type = TEXT_XML;
        ap_rprintf(r, "<?xml version=\"1.0\" standalone=\"yes\" ?>\n");
    } else {
        ap_set_content_type(r, "text/plain");
        type = TEXT_PLAIN;
    }

    size = loc_get_max_size_balancer();
    if (size == 0)
       return NULL;

    if ( type == TEXT_XML ) {
       ap_rprintf(r, "<Dump><Balancers>");
    }

    id = apr_palloc(r->pool, sizeof(int) * size);
    size = get_ids_used_balancer(balancerstatsmem, id);
    for (i=0; i<size; i++) {
        balancerinfo_t *ou;
        if (get_balancer(balancerstatsmem, &ou, id[i]) != APR_SUCCESS)
            continue;

        switch (type) {
            case TEXT_XML:
            {
                ap_rprintf(r, "<Balancer id=\"%d\" name=\"%.*s\">\
                                <StickySession>\
                                    <Enabled>%d</Enabled>\
                                    <Cookie>%.*s</Cookie>\
                                    <Path>%.*s</Path>\
                                    <Remove>%d</Remove>\
                                    <Force>%d</Force>\
                                </StickySession>\
                                <Timeout>%d</Timeout>\
                                <MaxAttempts>%d</MaxAttempts>\
                                </Balancer>",
                           id[i], (int) sizeof(ou->balancer), ou->balancer, ou->StickySession,
                           (int) sizeof(ou->StickySessionCookie), ou->StickySessionCookie, (int) sizeof(ou->StickySessionPath), ou->StickySessionPath,
                           ou->StickySessionRemove, ou->StickySessionForce,
                           (int) apr_time_sec(ou->Timeout),
                           ou->Maxattempts);
                           break;
            }
            case TEXT_PLAIN:
            default: {

                ap_rprintf(r, "balancer: [%d] Name: %.*s Sticky: %d [%.*s]/[%.*s] remove: %d force: %d Timeout: %d maxAttempts: %d\n",
                           id[i], (int) sizeof(ou->balancer), ou->balancer, ou->StickySession,
                           (int) sizeof(ou->StickySessionCookie), ou->StickySessionCookie, (int) sizeof(ou->StickySessionPath), ou->StickySessionPath,
                           ou->StickySessionRemove, ou->StickySessionForce,
                           (int) apr_time_sec(ou->Timeout),
                           ou->Maxattempts);
                break;
            }

        }
    }
    if ( type == TEXT_XML ) {
       ap_rprintf(r, "</Balancers>");
    }

    size = loc_get_max_size_node();
    id = apr_palloc(r->pool, sizeof(int) * size);
    size = get_ids_used_node(nodestatsmem, id);

    if ( type == TEXT_XML ) {
       ap_rprintf(r, "<Nodes>");
    }
    for (i=0; i<size; i++) {
        nodeinfo_t *ou;
        if (get_node(nodestatsmem, &ou, id[i]) != APR_SUCCESS)
            continue;

        switch(type) {
            case TEXT_XML:
            {
                ap_rprintf(r, "<Node id=\"%d\">\
                                    <Balancer>%.*s</Balancer>\
                                    <JVMRoute>%.*s</JVMRoute>\
                                    <LBGroup>%.*s</LBGroup>\
                                    <Host>%.*s</Host>\
                                    <Port>%.*s</Port>\
                                    <Type>%.*s</Type>\
                                    <FlushPackets>%d</FlushPackets>\
                                    <FlushWait>%d</FlushWait>\
                                    <Ping>%d</Ping>\
                                    <Smax>%d</Smax>\
                                    <Ttl>%d</Ttl>\
                                    <Timeout>%d</Timeout>\
                                </Node>",
                            ou->mess.id,
                           (int) sizeof(ou->mess.balancer), ou->mess.balancer,
                           (int) sizeof(ou->mess.JVMRoute), ou->mess.JVMRoute,
                           (int) sizeof(ou->mess.Domain), ou->mess.Domain,
                           (int) sizeof(ou->mess.Host), ou->mess.Host,
                           (int) sizeof(ou->mess.Port), ou->mess.Port,
                           (int) sizeof(ou->mess.Type), ou->mess.Type,
                           ou->mess.flushpackets, ou->mess.flushwait/1000, (int) apr_time_sec(ou->mess.ping), ou->mess.smax,
                           (int) apr_time_sec(ou->mess.ttl), (int) apr_time_sec(ou->mess.timeout));
                break;
            }
            case TEXT_PLAIN:
            default:
            {
                ap_rprintf(r, "node: [%d:%d],Balancer: %.*s,JVMRoute: %.*s,LBGroup: [%.*s],Host: %.*s,Port: %.*s,Type: %.*s,flushpackets: %d,flushwait: %d,ping: %d,smax: %d,ttl: %d,timeout: %d\n",
                           id[i], ou->mess.id,
                           (int) sizeof(ou->mess.balancer), ou->mess.balancer,
                           (int) sizeof(ou->mess.JVMRoute), ou->mess.JVMRoute,
                           (int) sizeof(ou->mess.Domain), ou->mess.Domain,
                           (int) sizeof(ou->mess.Host), ou->mess.Host,
                           (int) sizeof(ou->mess.Port), ou->mess.Port,
                           (int) sizeof(ou->mess.Type), ou->mess.Type,
                           ou->mess.flushpackets, ou->mess.flushwait/1000, (int) apr_time_sec(ou->mess.ping), ou->mess.smax,
                           (int) apr_time_sec(ou->mess.ttl), (int) apr_time_sec(ou->mess.timeout));

                break;
            }
        }
    }

    if ( type == TEXT_XML ) {
       ap_rprintf(r, "</Nodes><Hosts>");
    }

    size = loc_get_max_size_host();
    id = apr_palloc(r->pool, sizeof(int) * size);
    size = get_ids_used_host(hoststatsmem, id);
    for (i=0; i<size; i++) {
        hostinfo_t *ou;
        if (get_host(hoststatsmem, &ou, id[i]) != APR_SUCCESS)
            continue;

        switch (type) {
            case TEXT_XML:
            {
                ap_rprintf(r, "<Host id=\"%d\" alias=\"%.*s\">\
                                    <Vhost>%d</Vhost>\
                                    <Node>%d</Node>\
                                </Host>",
                 id[i], (int) sizeof(ou->host), ou->host, ou->vhost,ou->node);
                 break;
            }
            case TEXT_PLAIN:
            default:
            {
                ap_rprintf(r, "host: %d [%.*s] vhost: %d node: %d\n", id[i], (int) sizeof(ou->host), ou->host, ou->vhost,
                          ou->node);
                break;

            }
        }
    }
    if ( type == TEXT_XML ) {
       ap_rprintf(r, "</Hosts><Contexts>");
    }

    size = loc_get_max_size_context();
    id = apr_palloc(r->pool, sizeof(int) * size);
    size = get_ids_used_context(contextstatsmem, id);
    for (i=0; i<size; i++) {
        contextinfo_t *ou;
        if (get_context(contextstatsmem, &ou, id[i]) != APR_SUCCESS)
            continue;

        switch ( type ) {
            case TEXT_XML:
            {
                char *status;
                status = "REMOVED";
                switch (ou->status) {
                    case ENABLED:
                        status = "ENABLED";
                        break;
                    case DISABLED:
                        status = "DISABLED";
                        break;
                    case STOPPED:
                        status = "STOPPED";
                        break;
                }
                ap_rprintf(r, "<Context id=\"%d\" path=\"%.*s\">\
                                <Vhost>%d</Vhost>\
                                <Node>%d</Node>\
                                <Status id=\"%d\">%s</Status>\
                               </Context>",
                    id[i], (int) sizeof(ou->context), ou->context, ou->vhost, ou->node,ou->status, status);        
                    break;
                }
            case TEXT_PLAIN:
            default:
            {
                ap_rprintf(r, "context: %d [%.*s] vhost: %d node: %d status: %d\n", id[i],
                           (int) sizeof(ou->context), ou->context,
                           ou->vhost, ou->node,
                           ou->status);
                break;
            }
        }
    }

    if ( type == TEXT_XML ) {
       ap_rprintf(r, "</Contexts></Dump>");
    }
    return NULL;
}
/*
 * Process a INFO command.
 * Statics informations ;-)
 */
static char * process_info(request_rec *r, int *errtype)
{
    int size, i;
    int *id;

    unsigned char type;
    const char *accept_header = apr_table_get(r->headers_in, "Accept");

    if (accept_header && strstr((char *)accept_header, "text/xml") != NULL )  {
        ap_set_content_type(r, "text/xml");
        type = TEXT_XML;
        ap_rprintf(r, "<?xml version=\"1.0\" standalone=\"yes\" ?>\n");
    } else {
        ap_set_content_type(r, "text/plain");
        type = TEXT_PLAIN;
    }

    size = loc_get_max_size_node();
    if (size == 0)
        return NULL;
    id = apr_palloc(r->pool, sizeof(int) * size);
    size = get_ids_used_node(nodestatsmem, id);

    if ( type == TEXT_XML ) {
       ap_rprintf(r, "<Info><Nodes>");
    }

    for (i=0; i<size; i++) {
        nodeinfo_t *ou;
        proxy_worker_shared *proxystat;
        char *flushpackets;
        char *pptr;
        if (get_node(nodestatsmem, &ou, id[i]) != APR_SUCCESS)
            continue;

        switch ( type ) {
            case TEXT_XML:
            {
                ap_rprintf(r, "<Node id=\"%d\" name=\"%.*s\">\
                    <Balancer>%.*s</Balancer>\
                    <LBGroup>%.*s</LBGroup>\
                    <Host>%.*s</Host>\
                    <Port>%.*s</Port>\
                    <Type>%.*s</Type>", 
                       id[i],
                       (int) sizeof(ou->mess.JVMRoute), ou->mess.JVMRoute,
                       (int) sizeof(ou->mess.balancer), ou->mess.balancer,
                       (int) sizeof(ou->mess.Domain), ou->mess.Domain,
                       (int) sizeof(ou->mess.Host), ou->mess.Host,
                       (int) sizeof(ou->mess.Port), ou->mess.Port,
                       (int) sizeof(ou->mess.Type), ou->mess.Type);
                break;
            }
            case TEXT_PLAIN:
            default:
            {
                ap_rprintf(r, "Node: [%d],Name: %.*s,Balancer: %.*s,LBGroup: %.*s,Host: %.*s,Port: %.*s,Type: %.*s",
                           id[i],
                           (int) sizeof(ou->mess.JVMRoute), ou->mess.JVMRoute,
                           (int) sizeof(ou->mess.balancer), ou->mess.balancer,
                           (int) sizeof(ou->mess.Domain), ou->mess.Domain,
                           (int) sizeof(ou->mess.Host), ou->mess.Host,
                           (int) sizeof(ou->mess.Port), ou->mess.Port,
                           (int) sizeof(ou->mess.Type), ou->mess.Type);
                break;
            }
        }

        flushpackets = "Off";
        switch (ou->mess.flushpackets) {
            case flush_on:
                flushpackets = "On";
                break;
            case flush_auto:
                flushpackets = "Auto";
        }

        switch ( type ) {
            case TEXT_XML:
            {
                ap_rprintf(r, "<Flushpackets>%s</Flushpackets>\
                              <Flushwait>%d</Flushwait>\
                              <Ping>%d</Ping>\
                              <Smax>%d</Smax>\
                              <Ttl>%d</Ttl>",
                           flushpackets, ou->mess.flushwait/1000,
                           (int) apr_time_sec(ou->mess.ping),
                           ou->mess.smax,
                           (int) apr_time_sec(ou->mess.ttl));
                break;
            }
            case TEXT_PLAIN:
            default:
            {
                ap_rprintf(r, ",Flushpackets: %s,Flushwait: %d,Ping: %d,Smax: %d,Ttl: %d",
                           flushpackets, ou->mess.flushwait/1000,
                           (int) apr_time_sec(ou->mess.ping),
                           ou->mess.smax,
                           (int) apr_time_sec(ou->mess.ttl));
                break;
            }
        }

        pptr = (char *) ou;
        pptr = pptr + ou->offset;
        proxystat  = (proxy_worker_shared *) pptr;

        switch ( type ) {
            case TEXT_XML:  
            {
                ap_rprintf(r, "<Elected>%d</Elected>\
                                <Read>%d</Read>\
                                <Transfered>%d</Transfered>\
                                <Connected>%d</Connected>\
                                <Load>%d</Load>\
                                </Node>",
                           (int) proxystat->elected, (int) proxystat->read, (int) proxystat->transferred,
                           (int) proxystat->busy, proxystat->lbfactor);
                break;
            }
            case TEXT_PLAIN:
            default:
            {
                ap_rprintf(r, ",Elected: %d,Read: %d,Transfered: %d,Connected: %d,Load: %d\n",
                           (int) proxystat->elected, (int) proxystat->read, (int) proxystat->transferred,
                           (int) proxystat->busy, proxystat->lbfactor);
                break;
            }
        }
        
    }

    if ( type == TEXT_XML ) {
        ap_rprintf(r, "</Nodes>");
    }

    /* Process the Vhosts */
    size = loc_get_max_size_host();
    id = apr_palloc(r->pool, sizeof(int) * size);
    size = get_ids_used_host(hoststatsmem, id);
    if ( type == TEXT_XML ) {
        ap_rprintf(r, "<Vhosts>");
    }
    for (i=0; i<size; i++) {
        hostinfo_t *ou;
        if (get_host(hoststatsmem, &ou, id[i]) != APR_SUCCESS)
            continue;

        switch ( type ) {
            case TEXT_XML:
            {
                ap_rprintf(r, "<Vhost id=\"%d\" alias=\"%.*s\">\
                                <Node id=\"%d\"/>\
                                </Vhost>\
                ",
                    ou->vhost, (int ) sizeof(ou->host), ou->host, ou->node);
                break;
            }
            case TEXT_PLAIN:
            default:
            {
                ap_rprintf(r, "Vhost: [%d:%d:%d], Alias: %.*s\n",
                           ou->node, ou->vhost, id[i], (int ) sizeof(ou->host), ou->host);
                break;
            }
        }
    }

    if ( type == TEXT_XML ) {
        ap_rprintf(r, "</Vhosts>");
    }

    /* Process the Contexts */
    size = loc_get_max_size_context();
    id = apr_palloc(r->pool, sizeof(int) * size);
    size = get_ids_used_context(contextstatsmem, id);

    if ( type == TEXT_XML ) {
        ap_rprintf(r, "<Contexts>");
    }

    for (i=0; i<size; i++) {
        contextinfo_t *ou;
        char *status;
        if (get_context(contextstatsmem, &ou, id[i]) != APR_SUCCESS)
            continue;
        status = "REMOVED";
        switch (ou->status) {
            case ENABLED:
                status = "ENABLED";
                break;
            case DISABLED:
                status = "DISABLED";
                break;
            case STOPPED:
                status = "STOPPED";
                break;
        }

        switch ( type ) {
            case TEXT_XML:
            {
                ap_rprintf(r, "<Context id=\"%d\">\
                                 <Status id=\"%d\">%s</Status>\
                                 <Context>%.*s</Context>\
                                 <Node id=\"%d\"/>\
                                 <Vhost id=\"%d\"/>\
                                </Context>",
                                id[i], ou->status, status, (int) sizeof(ou->context), ou->context, ou->node, ou->vhost);
                break;
            }
            case TEXT_PLAIN:
            default:
            {
                ap_rprintf(r, "Context: [%d:%d:%d], Context: %.*s, Status: %s\n",
                           ou->node, ou->vhost, id[i],
                           (int) sizeof(ou->context), ou->context,
                           status);
                break;
            }
        }
    }

    if ( type == TEXT_XML ) {
        ap_rprintf(r, "</Contexts></Info>");
    }
    return NULL;
}

/* Process a *-APP command that applies to the node NOTE: the node is locked */
static char * process_node_cmd(request_rec *r, int status, int *errtype, nodeinfo_t *node)
{
    /* for read the hosts */
    int i,j;
    int size = loc_get_max_size_host();
    int *id;

    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                "process_node_cmd %d processing node: %d", status, node->mess.id);
    if (size == 0)
        return NULL;
    id = apr_palloc(r->pool, sizeof(int) * size);
    size = get_ids_used_host(hoststatsmem, id);
    for (i=0; i<size; i++) {
        hostinfo_t *ou;
        int sizecontext;
        int *idcontext;

        if (get_host(hoststatsmem, &ou, id[i]) != APR_SUCCESS)
            continue;
        if (ou->node != node->mess.id)
            continue;
        /* If the host corresponds to a node process all contextes */
        sizecontext = get_max_size_context(contextstatsmem);
        idcontext = apr_palloc(r->pool, sizeof(int) * sizecontext);
        sizecontext = get_ids_used_context(contextstatsmem, idcontext);
        for (j=0; j<sizecontext; j++) {
            contextinfo_t *context;
            if (get_context(contextstatsmem, &context, idcontext[j]) != APR_SUCCESS)
                continue;
            if (context->vhost == ou->vhost &&
                context->node == ou->node) {
                /* Process the context */
                if (status != REMOVE) {
                    context->status = status;
                    insert_update_context(contextstatsmem, context);
                } else
                    remove_context(contextstatsmem, context);

            }
        }
        if (status == REMOVE) {
            remove_host(hoststatsmem, ou);
        }
    }

    /* The REMOVE-APP * removes the node (well mark it removed) */
    if (status == REMOVE) {
        int id;
        node->mess.remove = 1;
        insert_update_node(nodestatsmem, node, &id);
    }
    return NULL;

}

/* Process an enable/disable/stop/remove application message */
static char * process_appl_cmd(request_rec *r, char **ptr, int status, int *errtype, int global, int fromnode)
{
    nodeinfo_t nodeinfo;
    nodeinfo_t *node;
    struct cluster_host *vhost;

    int i = 0;
    hostinfo_t hostinfo;
    hostinfo_t *host;
    char *p_tmp;

    memset(&nodeinfo.mess, '\0', sizeof(nodeinfo.mess));
    /* Map nothing by default */
    vhost = apr_palloc(r->pool, sizeof(struct cluster_host));
    vhost->host = NULL;
    vhost->context = NULL;
    vhost->next = NULL;

    while (ptr[i]) {
        if (strcasecmp(ptr[i], "JVMRoute") == 0) {
            if (strlen(ptr[i+1])>=sizeof(nodeinfo.mess.JVMRoute)) {
                *errtype = TYPESYNTAX;
                return SROUBIG;
            }
            strcpy(nodeinfo.mess.JVMRoute, ptr[i+1]);
            nodeinfo.mess.id = 0;
        }
        if (strcasecmp(ptr[i], "Alias") == 0) {
            if (vhost->host) {
                *errtype = TYPESYNTAX;
                return SMULALB;
            }
            p_tmp = ptr[i+1];
            /* Aliases to lower case for further case-insensitive treatment, IETF RFC 1035 Section 2.3.3. */
            while (*p_tmp) {
                *p_tmp = apr_tolower(*p_tmp);
                ++p_tmp;
            }
            vhost->host = ptr[i+1];
        }
        if (strcasecmp(ptr[i], "Context") == 0) {
            if (vhost->context) {
                *errtype = TYPESYNTAX;
                return SMULCTB;
            }
            vhost->context = ptr[i+1];
        }
        i++;
        i++;
    }

    /* Check for JVMRoute, Alias and Context */
    if (nodeinfo.mess.JVMRoute[0] == '\0') {
        *errtype = TYPESYNTAX;
        return SROUBAD;
    }
    if (vhost->context == NULL && vhost->host != NULL) {
        *errtype = TYPESYNTAX;
        return SALIBAD;
    }
    if (vhost->host == NULL && vhost->context != NULL) {
        *errtype = TYPESYNTAX;
        return SCONBAD;
    }

    /* Read the node */
    loc_lock_nodes();
    node = read_node(nodestatsmem, &nodeinfo);
    if (node == NULL) {
        loc_unlock_nodes();
        if (status == REMOVE)
            return NULL; /* Already done */
        *errtype = TYPEMEM;
        return apr_psprintf(r->pool, MNODERD, nodeinfo.mess.JVMRoute);
    }

    /* If the node is marked removed check what to do */
    if (node->mess.remove) {
        loc_unlock_nodes();
        if (status == REMOVE)
            return NULL; /* Already done */
        else {
            /* Act has if the node wasn't found */
            *errtype = TYPEMEM;
            return apr_psprintf(r->pool, MNODERD, node->mess.JVMRoute);
        }
    }
    inc_version_node();

    /* Process the * APP commands */
    if (global) {
        char *ret;
        ret = process_node_cmd(r, status, errtype, node);
        loc_unlock_nodes();
        return ret;
    }

    /* Read the ID of the virtual host corresponding to the first Alias */
    hostinfo.node = node->mess.id;
    if (vhost->host != NULL) {
        char *s = hostinfo.host;
        int j = 1;
        strncpy(hostinfo.host, vhost->host, HOSTALIASZ);
        while (*s != ',' && j<sizeof(hostinfo.host)) {
           j++;
           s++;
        }
        *s = '\0';
    } else
        hostinfo.host[0] = '\0';

    hostinfo.id = 0;
    host = read_host(hoststatsmem, &hostinfo);
    if (host == NULL) {
        /* If REMOVE ignores it */
        if (status == REMOVE) {
            loc_unlock_nodes();
            return NULL;
        } else {
            int vid, size, *id;
            /* Find the first available vhost id */
            vid = 0;
            size = loc_get_max_size_host();
            id = apr_palloc(r->pool, sizeof(int) * size);
            size = get_ids_used_host(hoststatsmem, id);
            for (i=0; i<size; i++) {
                hostinfo_t *ou;
                if (get_host(hoststatsmem, &ou, id[i]) != APR_SUCCESS)
                    continue;

            if(ou->node == node->mess.id && ou->vhost > vid)
                vid = ou->vhost;
            }
            vid++; /* Use next one. */
            ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server, "process_appl_cmd: adding vhost: %d node: %d",
                         vid, node->mess.id);

            /* If the Host doesn't exist yet create it */
            if (insert_update_hosts(hoststatsmem, vhost->host, node->mess.id, vid) != APR_SUCCESS) {
                loc_unlock_nodes();
                *errtype = TYPEMEM;
                return apr_psprintf(r->pool, MHOSTUI, nodeinfo.mess.JVMRoute);
            }
            hostinfo.id = 0;
            hostinfo.node = node->mess.id;
            if (vhost->host != NULL) {
                strncpy(hostinfo.host, vhost->host, sizeof(hostinfo.host));
                hostinfo.host[sizeof(hostinfo.host) - 1] = '\0';
            } else {
                hostinfo.host[0] = '\0';
            }
            host = read_host(hoststatsmem, &hostinfo);
            if (host == NULL) {
                loc_unlock_nodes();
                *errtype = TYPEMEM;
                return apr_psprintf(r->pool, MHOSTRD, node->mess.JVMRoute);
            }
        }
    }

    if (status == ENABLED) {
        /* There is no load balancing between balancers */
        int size = loc_get_max_size_context();
        int *id = apr_palloc(r->pool, sizeof(int) * size);
        size = get_ids_used_context(contextstatsmem, id);
        for (i=0; i<size; i++) {
            contextinfo_t *ou;
            if (get_context(contextstatsmem, &ou, id[i]) != APR_SUCCESS)
                continue;
            if (strcmp(ou->context, vhost->context) ==0) {
                /* There is the same context somewhere else */
                nodeinfo_t *hisnode;
                if (get_node(nodestatsmem, &hisnode, ou->node) != APR_SUCCESS)
                    continue;
                if (strcmp(hisnode->mess.balancer, node->mess.balancer)) {
                    /* the same context would be on 2 different balancer */
                    ap_log_error(APLOG_MARK, APLOG_NOERRNO|APLOG_WARNING, 0, r->server,
                                 "ENABLE: context %s is in balancer %s and %s", vhost->context,
                                  node->mess.balancer, hisnode->mess.balancer);
                }
            }
        }
    }

    /* Now update each context from Context: part */
    if (insert_update_contexts(contextstatsmem, vhost->context, node->mess.id, host->vhost, status) != APR_SUCCESS) {
        loc_unlock_nodes();
        *errtype = TYPEMEM;
        return apr_psprintf(r->pool, MCONTUI, node->mess.JVMRoute);
    }

    /* Remove the host if all the contextes have been removed */
    if (status == REMOVE) {
        int size = loc_get_max_size_context();
        int *id = apr_palloc(r->pool, sizeof(int) * size);
        size = get_ids_used_context(contextstatsmem, id);
        for (i=0; i<size; i++) {
            contextinfo_t *ou;
            if (get_context(contextstatsmem, &ou, id[i]) != APR_SUCCESS)
                continue;
            if (ou->vhost == host->vhost &&
                ou->node == node->mess.id)
                break;
        }
        if (i==size) {
            int size = loc_get_max_size_host();
            int *id = apr_palloc(r->pool, sizeof(int) * size);
            size = get_ids_used_host(hoststatsmem, id);
            for (i=0; i<size; i++) {
                 hostinfo_t *ou;

                 if (get_host(hoststatsmem, &ou, id[i]) != APR_SUCCESS)
                     continue;
                 if(ou->vhost == host->vhost && ou->node == node->mess.id)
                     remove_host(hoststatsmem, ou);
            }
        }
    } else if (status == STOPPED) {
        /* insert_update_contexts in fact makes that vhost->context corresponds only to the first context... */
        contextinfo_t in;
        contextinfo_t *ou;
        in.id = 0;
        strncpy(in.context, vhost->context, CONTEXTSZ);
        in.vhost = host->vhost;
        in.node = node->mess.id;
        ou = read_context(contextstatsmem, &in);
        if (ou != NULL) {
            ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server, "process_appl_cmd: STOP-APP nbrequests %d", ou->nbrequests);
            if (fromnode) {
                ap_set_content_type(r, "text/plain");
                ap_rprintf(r, "Type=STOP-APP-RSP&JvmRoute=%.*s&Alias=%.*s&Context=%.*s&Requests=%d",
                           (int) sizeof(nodeinfo.mess.JVMRoute), nodeinfo.mess.JVMRoute,
                           (int) sizeof(vhost->host), vhost->host,
                           (int) sizeof(vhost->context), vhost->context,
                           ou->nbrequests);
                ap_rprintf(r, "\n");
            }
        } else {
            ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server, "process_appl_cmd: STOP-APP can't read_context");
        }
    } 
    loc_unlock_nodes();
    return NULL;
}
static char * process_enable(request_rec *r, char **ptr, int *errtype, int global)
{
    return process_appl_cmd(r, ptr, ENABLED, errtype, global, 0);
}
static char * process_disable(request_rec *r, char **ptr, int *errtype, int global)
{
    return process_appl_cmd(r, ptr, DISABLED, errtype, global, 0);
}
static char * process_stop(request_rec *r, char **ptr, int *errtype, int global, int fromnode)
{
    return process_appl_cmd(r, ptr, STOPPED, errtype, global, fromnode);
}
static char * process_remove(request_rec *r, char **ptr, int *errtype, int global)
{
    return process_appl_cmd(r, ptr, REMOVE, errtype, global, 0);
}

/*
 * Call the ping/pong logic
 * Do a ping/png request to the node and set the load factor.
 */
static int isnode_up(request_rec *r, int id, int Load)
{
    if (balancerhandler != NULL) {
        return (balancerhandler->proxy_node_isup(r, id, Load));
    }
    return OK;
}
/*
 * Call the ping/pong logic using scheme://host:port
 * Do a ping/png request to the node and set the load factor.
 */
static int ishost_up(request_rec *r, char *scheme, char *host, char *port)
{
    if (balancerhandler != NULL) {
        return (balancerhandler->proxy_host_isup(r, scheme, host, port));
    }
    return OK;
}
/*
 * Process the STATUS command
 * Load -1 : Broken
 * Load 0  : Standby.
 * Load 1-100 : Load factor.
 */
static char * process_status(request_rec *r, char **ptr, int *errtype)
{
    int Load = -1;
    nodeinfo_t nodeinfo;
    nodeinfo_t *node;

    int i = 0;

    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server, "Processing STATUS");
    while (ptr[i]) {
        if (strcasecmp(ptr[i], "JVMRoute") == 0) {
            if (strlen(ptr[i+1])>=sizeof(nodeinfo.mess.JVMRoute)) {
                *errtype = TYPESYNTAX;
                return SROUBIG;
            }
            strcpy(nodeinfo.mess.JVMRoute, ptr[i+1]);
            nodeinfo.mess.id = 0;
        }
        else if (strcasecmp(ptr[i], "Load") == 0) {
            Load = atoi(ptr[i+1]);
        }
        else {
            *errtype = TYPESYNTAX;
            return apr_psprintf(r->pool, SBADFLD, ptr[i]);
        }
        i++;
        i++;
    }

    /* Read the node */
    node = read_node(nodestatsmem, &nodeinfo);
    if (node == NULL) {
        *errtype = TYPEMEM;
        return apr_psprintf(r->pool, MNODERD, nodeinfo.mess.JVMRoute);
    }

    /*
     * If the node is usualable do a ping/pong to prevent Split-Brain Syndrome
     * and update the worker status and load factor acccording to the test result.
     */
    ap_set_content_type(r, "text/plain");
    ap_rprintf(r, "Type=STATUS-RSP&JVMRoute=%.*s", (int) sizeof(nodeinfo.mess.JVMRoute), nodeinfo.mess.JVMRoute);

    if (isnode_up(r, node->mess.id, Load) != OK)
        ap_rprintf(r, "&State=NOTOK");
    else
        ap_rprintf(r, "&State=OK");
    ap_rprintf(r, "&id=%d", (int) ap_scoreboard_image->global->restart_time);

    ap_rprintf(r, "\n");
    return NULL;
}

/*
 * Process the VERSION command
 */
static char * process_version(request_rec *r, char **ptr, int *errtype)
{
    const char *accept_header = apr_table_get(r->headers_in, "Accept");

    if (accept_header && strstr((char *)accept_header, "text/xml") != NULL )  {
        ap_set_content_type(r, "text/xml");
        ap_rprintf(r, "<?xml version=\"1.0\" standalone=\"yes\" ?>\n");
        ap_rprintf(r, "<version><release>%s</release><protocol>%s</protocol></version>", MOD_CLUSTER_EXPOSED_VERSION, VERSION_PROTOCOL);
    } else {
        ap_set_content_type(r, "text/plain");
        ap_rprintf(r, "release: %s, protocol: %s", MOD_CLUSTER_EXPOSED_VERSION, VERSION_PROTOCOL);
    }
    ap_rprintf(r, "\n");
    return NULL;
}
/*
 * Process the PING command
 * With a JVMRoute does a cping/cpong in the node.
 * Without just answers ok.
 * NOTE: It is hard to cping/cpong a host + port but CONFIG + PING + REMOVE_APP *
 *       would do the same.
 */
static char * process_ping(request_rec *r, char **ptr, int *errtype)
{
    nodeinfo_t nodeinfo;
    nodeinfo_t *node;
    char *scheme = NULL;
    char *host = NULL;
    char *port = NULL;

    int i = 0;

    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server, "Processing PING");
    nodeinfo.mess.id = -1;
    while (ptr[i] && ptr[i][0] != '\0') {
        if (strcasecmp(ptr[i], "JVMRoute") == 0) {
            if (strlen(ptr[i+1])>=sizeof(nodeinfo.mess.JVMRoute)) {
                *errtype = TYPESYNTAX;
                return SROUBIG;
            }
            strcpy(nodeinfo.mess.JVMRoute, ptr[i+1]);
            nodeinfo.mess.id = 0;
        }
        else if (strcasecmp(ptr[i], "Scheme") == 0)
            scheme = apr_pstrdup(r->pool, ptr[i+1]);
        else if (strcasecmp(ptr[i], "Host") == 0)
            host = apr_pstrdup(r->pool, ptr[i+1]);
        else if (strcasecmp(ptr[i], "Port") == 0)
            port = apr_pstrdup(r->pool, ptr[i+1]);
        else {
            *errtype = TYPESYNTAX;
            return apr_psprintf(r->pool, SBADFLD, ptr[i]);
        }
        i++;
        i++;
    }
    if (nodeinfo.mess.id == -1) {
        /* PING scheme, host, port or just httpd */
        if (scheme == NULL && host == NULL && port == NULL) {
            ap_set_content_type(r, "text/plain");
            ap_rprintf(r, "Type=PING-RSP&State=OK");
        }  else {
            if (scheme == NULL || host == NULL || port == NULL) {
                *errtype = TYPESYNTAX;
                return apr_psprintf(r->pool, SMISFLD);
            }
            ap_set_content_type(r, "text/plain");
            ap_rprintf(r, "Type=PING-RSP");

            if (ishost_up(r, scheme, host, port) != OK)
                ap_rprintf(r, "&State=NOTOK");
            else
                ap_rprintf(r, "&State=OK");
        }
    } else {

        /* Read the node */
        node = read_node(nodestatsmem, &nodeinfo);
        if (node == NULL) {
            *errtype = TYPEMEM;
            return apr_psprintf(r->pool, MNODERD, nodeinfo.mess.JVMRoute);
        }

        /*
         * If the node is usualable do a ping/pong to prevent Split-Brain Syndrome
         * and update the worker status and load factor acccording to the test result.
         */
        ap_set_content_type(r, "text/plain");
        ap_rprintf(r, "Type=PING-RSP&JVMRoute=%.*s", (int) sizeof(nodeinfo.mess.JVMRoute), nodeinfo.mess.JVMRoute);

        if (isnode_up(r, node->mess.id, -2) != OK)
            ap_rprintf(r, "&State=NOTOK");
        else
            ap_rprintf(r, "&State=OK");
    }
    ap_rprintf(r, "&id=%d", (int) ap_scoreboard_image->global->restart_time);

    ap_rprintf(r, "\n");
    return NULL;
}

/*
 * Decodes a '%' escaped string, and returns the number of characters
 * (From mod_proxy_ftp.c).
 */

/* already called in the knowledge that the characters are hex digits */
/* Copied from modules/proxy/proxy_util.c */
static int mod_manager_hex2c(const char *x)
{
    int i, ch;

#if !APR_CHARSET_EBCDIC
    ch = x[0];
    if (apr_isdigit(ch)) {
        i = ch - '0';
    }
    else if (apr_isupper(ch)) {
        i = ch - ('A' - 10);
    }
    else {
        i = ch - ('a' - 10);
    }
    i <<= 4;

    ch = x[1];
    if (apr_isdigit(ch)) {
        i += ch - '0';
    }
    else if (apr_isupper(ch)) {
        i += ch - ('A' - 10);
    }
    else {
        i += ch - ('a' - 10);
    }
    return i;
#else /*APR_CHARSET_EBCDIC*/
    /*
     * we assume that the hex value refers to an ASCII character
     * so convert to EBCDIC so that it makes sense locally;
     *
     * example:
     *
     * client specifies %20 in URL to refer to a space char;
     * at this point we're called with EBCDIC "20"; after turning
     * EBCDIC "20" into binary 0x20, we then need to assume that 0x20
     * represents an ASCII char and convert 0x20 to EBCDIC, yielding
     * 0x40
     */
    char buf[1];

    if (1 == sscanf(x, "%2x", &i)) {
        buf[0] = i & 0xFF;
        ap_xlate_proto_from_ascii(buf, 1);
        return buf[0];
    }
    else {
        return 0;
    }
#endif /*APR_CHARSET_EBCDIC*/
}

/* Processing of decoded characters */
static apr_status_t decodeenc(char **ptr)
{
    int val, i, j;
    char ch;
    val = 0;
    while (NULL != ptr[val]) {
        if (ptr[val][0] == '\0') {
            return APR_SUCCESS;   /* special case for no characters */
        }
        for (i = 0, j = 0; ptr[val][i] != '\0'; i++, j++) {
            /* decode it if not already done */
            ch = ptr[val][i];
            if (ch == '%' && apr_isxdigit(ptr[val][i + 1]) && apr_isxdigit(ptr[val][i + 2])) {
                ch = (char) mod_manager_hex2c(&(ptr[val][i + 1]));
                i += 2;
            }

            /* process decoded, = and & are legit characters */
            /* from apr_escape_entity() */
            if (ch == '<' || ch == '>' || ch == '\"' || ch == '\'') {
                return TYPESYNTAX;
            }
            /* from apr_escape_shell() */
            if (ch == '\r' || ch == '\n') {
                return TYPESYNTAX;
            }

            ptr[val][j] = ch;
        }
        ptr[val][j] = '\0';
        val++;
    }
    return APR_SUCCESS;
}

/* Check that the method is one of ours */
static int check_method(request_rec *r)
{
    int ours = 0;
    if (strcasecmp(r->method, "CONFIG") == 0)
        ours = 1;
    else if (strcasecmp(r->method, "ENABLE-APP") == 0)
        ours = 1;
    else if (strcasecmp(r->method, "DISABLE-APP") == 0)
        ours = 1;
    else if (strcasecmp(r->method, "STOP-APP") == 0)
        ours = 1;
    else if (strcasecmp(r->method, "REMOVE-APP") == 0)
        ours = 1;
    else if (strcasecmp(r->method, "STATUS") == 0)
        ours = 1;
    else if (strcasecmp(r->method, "DUMP") == 0)
        ours = 1;
    else if (strcasecmp(r->method, "ERROR") == 0)
        ours = 1;
    else if (strcasecmp(r->method, "INFO") == 0)
        ours = 1;
    else if (strcasecmp(r->method, "PING") == 0)
        ours = 1;
    else if (strcasecmp(r->method, "ADDID") == 0)
        ours = 1;
    else if (strcasecmp(r->method, "REMOVEID") == 0)
        ours = 1;
    else if (strcasecmp(r->method, "QUERY") == 0)
        ours = 1;
    else if (strcasecmp(r->method, "VERSION") == 0)
        ours = 1;
    return ours;
}
/*
 * This routine is called before mod_proxy translate name.
 * This allows us to make decisions before mod_proxy
 * to be able to fill tables even with ProxyPass / balancer...
 */
static int manager_trans(request_rec *r)
{
    int ours = 0;
    core_dir_config *conf =
        (core_dir_config *)ap_get_module_config(r->per_dir_config,
                                                &core_module);
    mod_manager_config *mconf = ap_get_module_config(r->server->module_config,
                                                     &manager_module);
 
    if (conf && conf->handler && r->method_number == M_GET &&
        strcmp(conf->handler, "mod_cluster-manager") == 0) {
        r->handler = "mod_cluster-manager";
        r->filename = apr_pstrdup(r->pool, r->uri);
        return OK;
    }
    if (r->method_number != M_INVALID)
        return DECLINED;
    if (!mconf->enable_mcpm_receive)
        return DECLINED; /* Not allowed to receive MCMP */

    ours = check_method(r); 
    if (ours) {
        int i;
        /* The method one of ours */
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                    "manager_trans %s (%s)", r->method, r->uri);
        r->handler = "mod-cluster"; /* that hack doesn't work on httpd-2.4.x */
        i = strlen(r->uri);
        if (strcmp(r->uri, "*") == 0 || (i>=2 && r->uri[i-1] == '*' && r->uri[i-2] == '/')) {
            r->filename = apr_pstrdup(r->pool, NODE_COMMAND);
        } else {
            r->filename = apr_pstrdup(r->pool, r->uri);
        }
        return OK;
    }
    
    return DECLINED;
}

/* Create the commands that are possible on the context */
static char*context_string(request_rec *r, contextinfo_t *ou, char *Alias, char *JVMRoute)
{
    char context[CONTEXTSZ+1];
    char *raw;
    strncpy(context, ou->context, CONTEXTSZ+1);
    raw = apr_pstrcat(r->pool, "JVMRoute=", JVMRoute, "&Alias=", Alias, "&Context=", context, NULL);
    return raw;
}
static char *balancer_nonce_string(request_rec *r)
{
    char *ret = "";
    void *sconf = r->server->module_config;
    mod_manager_config *mconf = ap_get_module_config(sconf, &manager_module);
    if (mconf->nonce)
        ret = apr_psprintf(r->pool, "nonce=%s&", balancer_nonce);
    return ret;
}
static void context_command_string(request_rec *r, contextinfo_t *ou, char *Alias, char *JVMRoute)
{
    if (ou->status == DISABLED) {
        ap_rprintf(r, "<a href=\"%s?%sCmd=ENABLE-APP&Range=CONTEXT&%s\">Enable</a> ",
                   r->uri, balancer_nonce_string(r), context_string(r, ou, Alias, JVMRoute));
        ap_rprintf(r, " <a href=\"%s?%sCmd=STOP-APP&Range=CONTEXT&%s\">Stop</a>",
                   r->uri, balancer_nonce_string(r), context_string(r, ou, Alias, JVMRoute));
    }
    if (ou->status == ENABLED) {
        ap_rprintf(r, "<a href=\"%s?%sCmd=DISABLE-APP&Range=CONTEXT&%s\">Disable</a>",
                   r->uri, balancer_nonce_string(r), context_string(r, ou, Alias, JVMRoute));
        ap_rprintf(r, " <a href=\"%s?%sCmd=STOP-APP&Range=CONTEXT&%s\">Stop</a>",
                   r->uri, balancer_nonce_string(r), context_string(r, ou, Alias, JVMRoute));
    }
    if (ou->status == STOPPED) {
        ap_rprintf(r, "<a href=\"%s?%sCmd=ENABLE-APP&Range=CONTEXT&%s\">Enable</a> ",
                   r->uri, balancer_nonce_string(r), context_string(r, ou, Alias, JVMRoute));
        ap_rprintf(r, "<a href=\"%s?%sCmd=DISABLE-APP&Range=CONTEXT&%s\">Disable</a>",
                   r->uri, balancer_nonce_string(r), context_string(r, ou, Alias, JVMRoute));
    }
}
/* Create the commands that are possible on the node */
static char*node_string(request_rec *r, char *JVMRoute)
{
    char *raw = apr_pstrcat(r->pool, "JVMRoute=", JVMRoute, NULL);
    return raw;
}
static void node_command_string(request_rec *r, char *JVMRoute)
{
    ap_rprintf(r, "<a href=\"%s?%sCmd=ENABLE-APP&Range=NODE&%s\">Enable Contexts</a> ",
               r->uri, balancer_nonce_string(r), node_string(r, JVMRoute));
    ap_rprintf(r, "<a href=\"%s?%sCmd=DISABLE-APP&Range=NODE&%s\">Disable Contexts</a> ",
               r->uri, balancer_nonce_string(r), node_string(r, JVMRoute));
    ap_rprintf(r, "<a href=\"%s?%sCmd=STOP-APP&Range=NODE&%s\">Stop Contexts</a>",
               r->uri, balancer_nonce_string(r), node_string(r, JVMRoute));
}
static void domain_command_string(request_rec *r, char *Domain)
{
    ap_rprintf(r, "<a href=\"%s?%sCmd=ENABLE-APP&Range=DOMAIN&Domain=%s\">Enable Nodes</a> ",
               r->uri, balancer_nonce_string(r), Domain);
    ap_rprintf(r, "<a href=\"%s?%sCmd=DISABLE-APP&Range=DOMAIN&Domain=%s\">Disable Nodes</a> ",
               r->uri, balancer_nonce_string(r), Domain);
    ap_rprintf(r, "<a href=\"%s?%sCmd=STOP-APP&Range=DOMAIN&Domain=%s\">Stop Nodes</a>",
               r->uri, balancer_nonce_string(r), Domain);
}

/*
 * Process the parameters and display corresponding informations.
 */
static void manager_info_contexts(request_rec *r, int reduce_display, int allow_cmd, int node, int host, char *Alias, char *JVMRoute)
{
    int size, i;
    int *id;
    /* Process the Contexts */
    if (!reduce_display)
        ap_rprintf(r, "<h3>Contexts:</h3>");
    ap_rprintf(r, "<pre>");
    size = loc_get_max_size_context();
    if (size == 0)
        return;
    id = apr_palloc(r->pool, sizeof(int) * size);
    size = get_ids_used_context(contextstatsmem, id);
    for (i=0; i<size; i++) {
        contextinfo_t *ou;
        char *status;
        if (get_context(contextstatsmem, &ou, id[i]) != APR_SUCCESS)
            continue;
        if (ou->node != node || ou->vhost != host)
            continue;
        status = "REMOVED";
        switch (ou->status) {
            case ENABLED:
                status = "ENABLED";
                break;
            case DISABLED:
                status = "DISABLED";
                break;
            case STOPPED:
                status = "STOPPED";
                break;
        }
        ap_rprintf(r, "%.*s, Status: %s Request: %d ", (int) sizeof(ou->context), ou->context, status, ou->nbrequests);
        if (allow_cmd)
            context_command_string(r, ou, Alias, JVMRoute);
        ap_rprintf(r, "\n");
    }
    ap_rprintf(r, "</pre>");
}
static void manager_info_hosts(request_rec *r, int reduce_display, int allow_cmd, int node, char *JVMRoute)
{
    int size, i, j;
    int *id, *idChecker;
    int vhost = 0;

    /* Process the Vhosts */
    size = loc_get_max_size_host();
    if (size == 0)
        return;
    id = apr_palloc(r->pool, sizeof(int) * size);
    size = get_ids_used_host(hoststatsmem, id);
    idChecker = apr_pcalloc(r->pool, sizeof(int) * size);
    for (i=0; i<size; i++) {
        hostinfo_t *ou;
        if (get_host(hoststatsmem, &ou, id[i]) != APR_SUCCESS)
            continue;
        if (ou->node != node)
            continue;
        if (ou->vhost != vhost) {
            /* if we've logged this already, contine */
            if (idChecker[i] == 1)
                continue;
            if (vhost && !reduce_display)
                ap_rprintf(r, "</pre>");
            if (!reduce_display)
                ap_rprintf(r, "<h2> Virtual Host %d:</h2>", ou->vhost);
            manager_info_contexts(r, reduce_display, allow_cmd, ou->node, ou->vhost, ou->host, JVMRoute);
            if (reduce_display)
                ap_rprintf(r, "Aliases: ");
            else {
                ap_rprintf(r, "<h3>Aliases:</h3>");
                ap_rprintf(r, "<pre>");
            }
            vhost = ou->vhost;
        
            if (reduce_display)
                ap_rprintf(r, "%.*s ", (int) sizeof(ou->host), ou->host);
            else
                ap_rprintf(r, "%.*s\n", (int) sizeof(ou->host), ou->host);
            
            /* Go ahead and check for any other later alias entries for this vhost and print them now */
            for (j=i+1; j<size; j++) {
                hostinfo_t *pv;
                if (get_host(hoststatsmem, &pv, id[j]) != APR_SUCCESS)
                    continue;
                if (pv->node != node)
                    continue;
                if (pv->vhost != vhost)
                    continue;

                /* mark this entry as logged */
                idChecker[j]=1;
                /* step the outer loop forward if we can */
                if (i == j-1)
                    i++;
                if (reduce_display)
                    ap_rprintf(r, "%.*s ", (int) sizeof(pv->host), pv->host);
                else
                    ap_rprintf(r, "%.*s\n", (int) sizeof(pv->host), pv->host);
            }
        }
    }
    if (size && !reduce_display)
        ap_rprintf(r, "</pre>");

}
static void manager_sessionid(request_rec *r)
{
    int size, i;
    int *id;

    /* Process the Sessionids */
    size = loc_get_max_size_sessionid();
    if (size == 0)
        return;
    id = apr_palloc(r->pool, sizeof(int) * size);
    size = get_ids_used_sessionid(sessionidstatsmem, id);
    if (!size)
        return;
    ap_rprintf(r, "<h1>SessionIDs:</h1>");
    ap_rprintf(r, "<pre>");
    for (i=0; i<size; i++) {
        sessionidinfo_t *ou;
        if (get_sessionid(sessionidstatsmem, &ou, id[i]) != APR_SUCCESS)
            continue;
        ap_rprintf(r, "id: %.*s route: %.*s\n", (int) sizeof(ou->sessionid), ou->sessionid, (int) sizeof(ou->JVMRoute), ou->JVMRoute);
    }
    ap_rprintf(r, "</pre>");

}

#if HAVE_CLUSTER_EX_DEBUG
static void manager_domain(request_rec *r, int reduce_display)
{
    int size, i;
    int *id;

    /* Process the domain information: the removed node belonging to a domain are stored there */
    if (reduce_display)
        ap_rprintf(r, "<br/>LBGroup:");
    else
        ap_rprintf(r, "<h1>LBGroup:</h1>");
    ap_rprintf(r, "<pre>");
    size = loc_get_max_size_domain();
    if (size == 0)
        return;
    id = apr_palloc(r->pool, sizeof(int) * size);
    size = get_ids_used_domain(domainstatsmem, id);
    for (i=0; i<size; i++) {
        domaininfo_t *ou;
        if (get_domain(domainstatsmem, &ou, id[i]) != APR_SUCCESS)
            continue;
        ap_rprintf(r, "dom: %.*s route: %.*s balancer: %.*s\n",
                   sizeof(ou->domain), ou->domain,
                   sizeof(ou->JVMRoute), ou->JVMRoute,
                   sizeof(ou->balancer), ou->balancer);
    }
    ap_rprintf(r, "</pre>");

}
#endif

static int count_sessionid(request_rec *r, char *route)
{
    int size, i;
    int *id;
    int count = 0;

    /* Count the sessionid corresponding to the route */
    size = loc_get_max_size_sessionid();
    if (size == 0)
        return 0;
    id = apr_palloc(r->pool, sizeof(int) * size);
    size = get_ids_used_sessionid(sessionidstatsmem, id);
    for (i=0; i<size; i++) {
        sessionidinfo_t *ou;
        if (get_sessionid(sessionidstatsmem, &ou, id[i]) != APR_SUCCESS)
            continue;
        if (strcmp(route, ou->JVMRoute) == 0)
            count++; 
    }
    return count;
}
static void process_error(request_rec *r, char *errstring, int errtype)
{
    r->status_line = apr_psprintf(r->pool, "ERROR");
    apr_table_setn(r->err_headers_out, "Version", VERSION_PROTOCOL);
    switch (errtype) {
      case TYPESYNTAX:
         apr_table_setn(r->err_headers_out, "Type", "SYNTAX");
         break;
      case TYPEMEM:
         apr_table_setn(r->err_headers_out, "Type", "MEM");
         break;
      default:
         apr_table_setn(r->err_headers_out, "Type", "GENERAL");
         break;
    }
    apr_table_setn(r->err_headers_out, "Mess", errstring);
    ap_log_error(APLOG_MARK, APLOG_NOERRNO|APLOG_WARNING, 0, r->server,
            "manager_handler %s error: %s", r->method, errstring);
}
static void sort_nodes(nodeinfo_t *nodes, int nbnodes)
{
    int i;
    int changed = -1;
    if (nbnodes <=1)
        return;
    while(changed) {
        changed = 0;
        for (i=0; i<nbnodes-1; i++) {
            if (strcmp(nodes[i].mess.Domain, nodes[i+1].mess.Domain)> 0) {
                nodeinfo_t node;
                node = nodes[i+1];
                nodes[i+1] = nodes[i];
                nodes[i] = node;
                changed = -1;
            }
        }
    }
}
static char *process_domain(request_rec *r, char **ptr, int *errtype, const char *cmd, const char *domain)
{
    int size, i;
    int *id;
    int pos;
    char *errstring = NULL;
    size = loc_get_max_size_node();
    if (size == 0)
        return NULL;
    id = apr_palloc(r->pool, sizeof(int) * size);
    size = get_ids_used_node(nodestatsmem, id);

    for (pos=0;ptr[pos]!=NULL && ptr[pos+1]!=NULL; pos=pos+2) ;

    ptr[pos] = apr_pstrdup(r->pool, "JVMRoute");
    ptr[pos+2] = NULL;
    ptr[pos+3] = NULL;
    ap_log_error(APLOG_MARK, APLOG_NOERRNO|APLOG_EMERG, 0, r->server, "process_domain");
    for (i=0; i<size; i++) {
        nodeinfo_t *ou;
        if (get_node(nodestatsmem, &ou, id[i]) != APR_SUCCESS)
            continue;
        if (strcmp(ou->mess.Domain, domain) != 0)
            continue;
        /* add the JVMRoute */
        ptr[pos+1] = apr_pstrdup(r->pool, ou->mess.JVMRoute);
        if (strcasecmp(cmd, "ENABLE-APP") == 0)
            errstring = process_enable(r, ptr, errtype, RANGENODE);
        else if (strcasecmp(cmd, "DISABLE-APP") == 0)
            errstring = process_disable(r, ptr, errtype, RANGENODE);
        else if (strcasecmp(cmd, "STOP-APP") == 0)
            errstring = process_stop(r, ptr, errtype, RANGENODE, 0);
        else if (strcasecmp(cmd, "REMOVE-APP") == 0)
            errstring = process_remove(r, ptr, errtype, RANGENODE);
    }
    return errstring;
}
/* XXX: move to mod_proxy_cluster as a provider ? */
static void printproxy_stat(request_rec *r, int reduce_display, proxy_worker_shared *proxystat)
{
    char *status = NULL;
    if (proxystat->status & PROXY_WORKER_NOT_USABLE_BITMAP)
        status = "NOTOK";
    else
        status = "OK";
    if (reduce_display)
        ap_rprintf(r, " %s ", status);
    else
        ap_rprintf(r, ",Status: %s,Elected: %d,Read: %d,Transferred: %d,Connected: %d,Load: %d",
               status,
               (int) proxystat->elected, (int) proxystat->read, (int) proxystat->transferred,
               (int) proxystat->busy, proxystat->lbfactor);
}
/* Display module information */
static void modules_info(request_rec *r)
{
    if (ap_find_linked_module("mod_proxy_cluster.c") != NULL)
        ap_rputs("mod_proxy_cluster.c: OK<br/>", r);
    else
        ap_rputs("mod_proxy_cluster.c: missing<br/>", r);

    if (ap_find_linked_module("mod_sharedmem.c") != NULL)
        ap_rputs("mod_sharedmem.c: OK<br/>", r);
    else
        ap_rputs("mod_sharedmem.c: missing<br/>", r);

    ap_rputs("Protocol supported: ", r);
    if (ap_find_linked_module("mod_proxy_http.c") != NULL)
        ap_rputs("http ", r);
    if (ap_find_linked_module("mod_proxy_ajp.c") != NULL) 
        ap_rputs("AJP ", r);
    if (ap_find_linked_module("mod_ssl.c") != NULL) 
        ap_rputs("https", r);
    ap_rputs("<br/>", r);

    if (ap_find_linked_module("mod_advertise.c") != NULL)
        ap_rputs("mod_advertise.c: OK<br/>", r);
    else
        ap_rputs("mod_advertise.c: not loaded<br/>", r);

}
/* Process INFO message and mod_cluster_manager pages generation */
static int manager_info(request_rec *r)
{
    int size, i, sizesessionid;
    int *id;
    apr_table_t *params = apr_table_make(r->pool, 10);
    int access_status;
    const char *name;
    nodeinfo_t *nodes;
    int nbnodes = 0;
    char *domain = "";
    char *errstring = NULL;
    void *sconf = r->server->module_config;
    mod_manager_config *mconf = ap_get_module_config(sconf, &manager_module);

    if (r->args) {
        char *args = apr_pstrdup(r->pool, r->args);
        char *tok, *val;
        while (args && *args) {
            if ((val = ap_strchr(args, '='))) {
                *val++ = '\0';
                if ((tok = ap_strchr(val, '&')))
                    *tok++ = '\0';
                /*
                 * Special case: contexts contain path information
                 */
                if ((access_status = ap_unescape_url(val)) != OK)
                    if (strcmp(args, "Context") || (access_status !=  HTTP_NOT_FOUND))
                        return access_status;
                apr_table_setn(params, args, val);
                args = tok;
            }
            else
                return HTTP_BAD_REQUEST;
        }
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                "manager_info request:%s", r->args);
    }

    /*
     * Check that the supplied nonce matches this server's nonce;
     * otherwise ignore all parameters, to prevent a CSRF attack.
     */
    if (mconf->nonce && ((name = apr_table_get(params, "nonce")) == NULL
        || strcmp(balancer_nonce, name) != 0)) {
        apr_table_clear(params);
    }

    /* process the parameters */
    if (r->args) {
        const char *val = apr_table_get(params, "Refresh");
        const char *cmd = apr_table_get(params, "Cmd");
        const char *typ = apr_table_get(params, "Range");
        const char *domain = apr_table_get(params, "Domain");
        /* Process the Refresh parameter */
        if (val) {
            long t = atol(val);
            apr_table_set(r->headers_out, "Refresh", apr_ltoa(r->pool,t < 1 ? 10 : t));
        }
        /* Process INFO and DUMP */
        if (cmd != NULL) {
            int errtype = 0;
            if (strcasecmp(cmd, "DUMP") == 0) {
                errstring = process_dump(r, &errtype);
                if (!errstring)
                    return OK;
            } else if (strcasecmp(cmd, "INFO") == 0) {
                errstring = process_info(r, &errtype);
                if (!errstring)
                    return OK;
            }
            if (errstring) {
                process_error(r, errstring, errtype);
            }
        }
        /* Process other command if any */
        if (cmd != NULL && typ !=NULL && mconf->allow_cmd && errstring == NULL) {
            int global = RANGECONTEXT;
            int errtype = 0;
            int i;
            char **ptr;
            const apr_array_header_t *arr = apr_table_elts(params);
            const apr_table_entry_t *elts = (const apr_table_entry_t *)arr->elts;

            if (strcasecmp(typ,"NODE")==0)
                global = RANGENODE;
            else if (strcasecmp(typ,"DOMAIN")==0)
                global = RANGEDOMAIN;

            if (global == RANGEDOMAIN)
                ptr = apr_palloc(r->pool, sizeof(char *) * (arr->nelts + 2) * 2);
            else
                ptr = apr_palloc(r->pool, sizeof(char *) * (arr->nelts + 1) * 2);
            for (i = 0; i < arr->nelts; i++) {
                ptr[i*2] = elts[i].key;
                ptr[i*2+1] = elts[i].val;
            }
            ptr[arr->nelts*2] = NULL;
            ptr[arr->nelts*2+1] = NULL;
             
            if (global == RANGEDOMAIN)
                errstring = process_domain(r, ptr, &errtype, cmd, domain);
            else if (strcasecmp(cmd, "ENABLE-APP") == 0)
                errstring = process_enable(r, ptr, &errtype, global);
            else if (strcasecmp(cmd, "DISABLE-APP") == 0)
                errstring = process_disable(r, ptr, &errtype, global);
            else if (strcasecmp(cmd, "STOP-APP") == 0)
                errstring = process_stop(r, ptr, &errtype, global, 0);
            else if (strcasecmp(cmd, "REMOVE-APP") == 0)
                errstring = process_remove(r, ptr, &errtype, global);
            else {
                errstring = SCMDUNS;
                errtype = TYPESYNTAX;
            }
            if (errstring) {
                process_error(r, errstring, errtype);
            }
        }
    }
    
    ap_set_content_type(r, "text/html; charset=ISO-8859-1");
    ap_rputs(DOCTYPE_HTML_3_2
             "<html><head>\n<title>Mod_cluster Status</title>\n</head><body>\n",
             r);
    ap_rvputs(r, "<h1>", MOD_CLUSTER_EXPOSED_VERSION, "</h1>", NULL);

    if (errstring) {
        ap_rvputs(r, "<h1> Command failed: ", errstring , "</h1>\n", NULL);
        ap_rvputs(r, " <a href=\"", r->uri, "\">Continue</a>\n", NULL);
        ap_rputs("</body></html>\n", r);
        return OK;
    }

    /* Advertise information */
    if (mconf->allow_display) {
        ap_rputs("start of \"httpd.conf\" configuration<br/>", r); 
        modules_info(r);
        if (advertise_info != NULL)
            advertise_info(r);
        ap_rputs("end of \"httpd.conf\" configuration<br/><br/>", r);
    }

    ap_rvputs(r, "<a href=\"", r->uri, "?", balancer_nonce_string(r),
                 "refresh=10",
                 "\">Auto Refresh</a>", NULL);

    ap_rvputs(r, " <a href=\"", r->uri, "?", balancer_nonce_string(r),
                 "Cmd=DUMP&Range=ALL",
                 "\">show DUMP output</a>", NULL);

    ap_rvputs(r, " <a href=\"", r->uri, "?", balancer_nonce_string(r),
                 "Cmd=INFO&Range=ALL",
                 "\">show INFO output</a>", NULL);

    ap_rputs("\n", r);

    sizesessionid = loc_get_max_size_sessionid();

    size = loc_get_max_size_node();
    if (size == 0)
        return OK;
    id = apr_palloc(r->pool, sizeof(int) * size);
    size = get_ids_used_node(nodestatsmem, id);


    /* read the node to sort them by domain */
    nodes = apr_palloc(r->pool, sizeof(nodeinfo_t) * size);
    for (i=0; i<size; i++) {
        nodeinfo_t *ou;
        if (get_node(nodestatsmem, &ou, id[i]) != APR_SUCCESS)
            continue;
        memcpy(&nodes[nbnodes],ou, sizeof(nodeinfo_t));
        nbnodes++;
    }
    sort_nodes(nodes, nbnodes);

    /* display the ordered nodes */
    for (i=0; i<size; i++) {
        char *flushpackets;
        nodeinfo_t *ou = &nodes[i];
        char *pptr = (char *) ou;

        if (strcmp(domain, ou->mess.Domain) != 0) {
            if (mconf->reduce_display)
                ap_rprintf(r, "<br/><br/>LBGroup %.*s: ", (int) sizeof(ou->mess.Domain), ou->mess.Domain);
            else
                ap_rprintf(r, "<h1> LBGroup %.*s: ", (int) sizeof(ou->mess.Domain), ou->mess.Domain);
            domain = ou->mess.Domain;
            if (mconf->allow_cmd)
                domain_command_string(r, domain);
            if (!mconf->reduce_display)
                ap_rprintf(r, "</h1>\n");
        }
        if (mconf->reduce_display)
            ap_rprintf(r, "<br/><br/>Node %.*s ",
                   (int) sizeof(ou->mess.JVMRoute), ou->mess.JVMRoute);
        else 
            ap_rprintf(r, "<h1> Node %.*s (%.*s://%.*s:%.*s): </h1>\n",
                   (int) sizeof(ou->mess.JVMRoute), ou->mess.JVMRoute,
                   (int) sizeof(ou->mess.Type), ou->mess.Type,
                   (int) sizeof(ou->mess.Host), ou->mess.Host,
                   (int) sizeof(ou->mess.Port), ou->mess.Port);
        pptr = pptr + ou->offset;
        if (mconf->reduce_display) {
            printproxy_stat(r, mconf->reduce_display, (proxy_worker_shared *) pptr);
        }

        if (mconf->allow_cmd)
            node_command_string(r, ou->mess.JVMRoute);

        if (!mconf->reduce_display) {
            ap_rprintf(r, "<br/>\n");
            ap_rprintf(r, "Balancer: %.*s,LBGroup: %.*s", (int) sizeof(ou->mess.balancer), ou->mess.balancer,
                   (int) sizeof(ou->mess.Domain), ou->mess.Domain);

            flushpackets = "Off";
            switch (ou->mess.flushpackets) {
                case flush_on:
                    flushpackets = "On";
                    break;
                case flush_auto:
                    flushpackets = "Auto";
            }
            ap_rprintf(r, ",Flushpackets: %s,Flushwait: %d,Ping: %d,Smax: %d,Ttl: %d",
                   flushpackets, ou->mess.flushwait,
                   (int) ou->mess.ping, ou->mess.smax, (int) ou->mess.ttl);
        }

        if (mconf->reduce_display)
            ap_rprintf(r, "<br/>\n");
        else {
            printproxy_stat(r, mconf->reduce_display, (proxy_worker_shared *) pptr);
        }

        if (sizesessionid) {
            ap_rprintf(r, ",Num sessions: %d",  count_sessionid(r, ou->mess.JVMRoute));
        }
        ap_rprintf(r, "\n");

        /* Process the Vhosts */
        manager_info_hosts(r, mconf->reduce_display, mconf->allow_cmd, ou->mess.id, ou->mess.JVMRoute); 
    }
    /* Display the sessions */
    if (sizesessionid)
        manager_sessionid(r);
#if HAVE_CLUSTER_EX_DEBUG
    manager_domain(r, mconf->reduce_display);
#endif


    ap_rputs("</body></html>\n", r);
    return OK;
}

/* Process the requests from the ModClusterService */
static int manager_handler(request_rec *r)
{
    apr_bucket_brigade *input_brigade;
    char *errstring = NULL;
    int errtype = 0;
    char *buff;
    apr_size_t bufsiz=0, maxbufsiz, len;
    apr_status_t status;
    int global = 0;
    int ours = 0;
    char **ptr;
    void *sconf = r->server->module_config;
    mod_manager_config *mconf;
  
    if (strcmp(r->handler, "mod_cluster-manager") == 0) {
        /* Display the nodes information */
        if (r->method_number != M_GET)
            return DECLINED;
        return(manager_info(r));
    }

    mconf = ap_get_module_config(sconf, &manager_module);
    if (!mconf->enable_mcpm_receive)
        return DECLINED; /* Not allowed to receive MCMP */

    ours = check_method(r);
    if (!ours)
        return DECLINED;

    /* Use a buffer to read the message */
    if (mconf->maxmesssize)
       maxbufsiz = mconf->maxmesssize;
    else {
       /* we calculate it */
       maxbufsiz = 9 + JVMROUTESZ;
       maxbufsiz = bufsiz + (mconf->maxhost * HOSTALIASZ) + 7;
       maxbufsiz = bufsiz + (mconf->maxcontext * CONTEXTSZ) + 8;
    }
    if (maxbufsiz< MAXMESSSIZE)
       maxbufsiz = MAXMESSSIZE;
    buff = apr_pcalloc(r->pool, maxbufsiz);
    input_brigade = apr_brigade_create(r->pool, r->connection->bucket_alloc);
    len = maxbufsiz;
    while ((status = ap_get_brigade(r->input_filters, input_brigade, AP_MODE_READBYTES, APR_BLOCK_READ, len)) == APR_SUCCESS) {
        apr_brigade_flatten(input_brigade, buff + bufsiz, &len);
        apr_brigade_cleanup(input_brigade);
        bufsiz += len;
        if (bufsiz >= maxbufsiz || len == 0) break;
        len = maxbufsiz - bufsiz;
    }

    if (status != APR_SUCCESS) {
        errstring = apr_psprintf(r->pool, SREADER, r->method);
        r->status_line = apr_psprintf(r->pool, "ERROR");
        apr_table_setn(r->err_headers_out, "Version", VERSION_PROTOCOL);
        apr_table_setn(r->err_headers_out, "Type", "SYNTAX");
        apr_table_setn(r->err_headers_out, "Mess", errstring);
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                "manager_handler %s error: %s", r->method, errstring);
        return 500;
    }
    buff[bufsiz] = '\0';

    /* XXX: Size limit it? */
    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                "manager_handler %s (%s) processing: \"%s\"", r->method, r->filename, buff);

    ptr = process_buff(r, buff);
    if (ptr == NULL) {
        process_error(r, SMESPAR, TYPESYNTAX);
        return 500;
    }
    if (strstr(r->filename, NODE_COMMAND))
        global = 1;

    if (strcasecmp(r->method, "CONFIG") == 0)
        errstring = process_config(r, ptr, &errtype);
    /* Application handling */
    else if (strcasecmp(r->method, "ENABLE-APP") == 0)
        errstring = process_enable(r, ptr, &errtype, global);
    else if (strcasecmp(r->method, "DISABLE-APP") == 0)
        errstring = process_disable(r, ptr, &errtype, global);
    else if (strcasecmp(r->method, "STOP-APP") == 0)
        errstring = process_stop(r, ptr, &errtype, global, 1);
    else if (strcasecmp(r->method, "REMOVE-APP") == 0)
        errstring = process_remove(r, ptr, &errtype, global);
    /* Status handling */
    else if (strcasecmp(r->method, "STATUS") == 0)
        errstring = process_status(r, ptr, &errtype);
    else if (strcasecmp(r->method, "DUMP") == 0)
        errstring = process_dump(r, &errtype);
    else if (strcasecmp(r->method, "INFO") == 0)
        errstring = process_info(r, &errtype);
    else if (strcasecmp(r->method, "PING") == 0)
        errstring = process_ping(r, ptr, &errtype);
    else if (strcasecmp(r->method, "VERSION") == 0)
        errstring = process_version(r, ptr, &errtype);
    else {
        errstring = SCMDUNS;
        errtype = TYPESYNTAX;
    }

    /* Check error string and build the error message */
    if (errstring) {
        process_error(r, errstring, errtype);
        return 500;
    }

    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                "manager_handler %s  OK", r->method);

    ap_rflush(r);
    return (OK);
}

/*
 *  Attach to the shared memory when the child is created.
 */
static void  manager_child_init(apr_pool_t *p, server_rec *s)
{
    char *node;
    char *context;
    char *host;
    char *balancer;
    char *sessionid;
    char *filename;
    mod_manager_config *mconf = ap_get_module_config(s->module_config, &manager_module);

    if (storage == NULL) {
        /* that happens when doing a gracefull restart for example after additing/changing the storage provider */
        ap_log_error(APLOG_MARK, APLOG_NOERRNO|APLOG_EMERG, 0, s, "Fatal storage provider not initialized");
        return;
    }
    if (apr_thread_mutex_create(&nodes_global_mutex, APR_THREAD_MUTEX_DEFAULT, p) != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_ERR|APLOG_NOERRNO, 0, s,
                    "manager_child_init: apr_thread_mutex_create failed");
        return;
    }
    if (apr_thread_mutex_create(&contexts_global_mutex, APR_THREAD_MUTEX_DEFAULT, p) != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_ERR|APLOG_NOERRNO, 0, s,
                    "manager_child_init: apr_thread_mutex_create failed");
        return;
    }

    mconf->tableversion = 0;

    if (mconf->basefilename) {
        node = apr_pstrcat(p, mconf->basefilename, "/manager.node", NULL);
        context = apr_pstrcat(p, mconf->basefilename, "/manager.context", NULL);
        host = apr_pstrcat(p, mconf->basefilename, "/manager.host", NULL);
        balancer = apr_pstrcat(p, mconf->basefilename, "/manager.balancer", NULL);
        sessionid = apr_pstrcat(p, mconf->basefilename, "/manager.sessionid", NULL);
    } else {
        node = ap_server_root_relative(p, "logs/manager.node");
        context = ap_server_root_relative(p, "logs/manager.context");
        host = ap_server_root_relative(p, "logs/manager.host");
        balancer = ap_server_root_relative(p, "logs/manager.balancer");
        sessionid = ap_server_root_relative(p, "logs/manager.sessionid");
    }

    /* create the global node file look */
    filename = apr_pstrcat(p, node , ".lock", NULL);
    if (apr_file_open(&nodes_global_lock, filename, APR_WRITE|APR_CREATE, APR_OS_DEFAULT, p) != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_ERR|APLOG_NOERRNO, 0, s,
                    "manager_child_init: apr_file_open for lock failed");
        return;
    }
    filename = apr_pstrcat(p, context , ".lock", NULL);
    if (apr_file_open(&contexts_global_lock, filename, APR_WRITE|APR_CREATE, APR_OS_DEFAULT, p) != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_ERR|APLOG_NOERRNO, 0, s,
                    "manager_child_init: apr_file_open for lock failed");
        return;
    }


    nodestatsmem = get_mem_node(node, &mconf->maxnode, p, storage);
    if (nodestatsmem == NULL) {
        ap_log_error(APLOG_MARK, APLOG_NOERRNO|APLOG_EMERG, 0, s, "get_mem_node %s failed", node);
        return;
    }
    if (get_last_mem_error(nodestatsmem) != APR_SUCCESS) {
        char buf[120];
        ap_log_error(APLOG_MARK, APLOG_NOERRNO|APLOG_EMERG, 0, s, "get_mem_node %s failed: %s",
                     node, apr_strerror(get_last_mem_error(nodestatsmem), buf, sizeof(buf)));
        return;
    }

    contextstatsmem = get_mem_context(context, &mconf->maxcontext, p, storage);
    if (contextstatsmem == NULL) {
        ap_log_error(APLOG_MARK, APLOG_NOERRNO|APLOG_EMERG, 0, s, "get_mem_context failed");
        return;
    }

    hoststatsmem = get_mem_host(host, &mconf->maxhost, p, storage);
    if (hoststatsmem == NULL) {
        ap_log_error(APLOG_MARK, APLOG_NOERRNO|APLOG_EMERG, 0, s, "get_mem_host failed");
        return;
    }

    balancerstatsmem = get_mem_balancer(balancer, &mconf->maxhost, p, storage);
    if (balancerstatsmem == NULL) {
        ap_log_error(APLOG_MARK, APLOG_NOERRNO|APLOG_EMERG, 0, s, "get_mem_balancer failed");
        return;
    }

    if (mconf->maxsessionid) {
        /*  Try to get sessionid stuff only if required */
        sessionidstatsmem = get_mem_sessionid(sessionid, &mconf->maxsessionid, p, storage);
        if (sessionidstatsmem == NULL) {
            ap_log_error(APLOG_MARK, APLOG_NOERRNO|APLOG_EMERG, 0, s, "get_mem_sessionid failed");
            return;
        }
    }
}

/*
 * Supported directives.
 */
static const char *cmd_manager_maxcontext(cmd_parms *cmd, void *mconfig, const char *word)
{
    mod_manager_config *mconf = ap_get_module_config(cmd->server->module_config, &manager_module);
    const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);
    if (err != NULL) {
        return err;
    }
    mconf->maxcontext = atoi(word);
    return NULL;
}
static const char *cmd_manager_maxnode(cmd_parms *cmd, void *mconfig, const char *word)
{
    mod_manager_config *mconf = ap_get_module_config(cmd->server->module_config, &manager_module);
    const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);
    if (err != NULL) {
        return err;
    }
    mconf->maxnode = atoi(word);
    return NULL;
}
static const char *cmd_manager_maxhost(cmd_parms *cmd, void *mconfig, const char *word)
{
    mod_manager_config *mconf = ap_get_module_config(cmd->server->module_config, &manager_module);
    const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);
    if (err != NULL) {
        return err;
    }
    mconf->maxhost = atoi(word);
    return NULL;
}
static const char *cmd_manager_maxsessionid(cmd_parms *cmd, void *mconfig, const char *word)
{
    mod_manager_config *mconf = ap_get_module_config(cmd->server->module_config, &manager_module);
    const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);
    if (err != NULL) {
        return err;
    }
    mconf->maxsessionid = atoi(word);
    return NULL;
}
static const char *cmd_manager_memmanagerfile(cmd_parms *cmd, void *mconfig, const char *word)
{
    mod_manager_config *mconf = ap_get_module_config(cmd->server->module_config, &manager_module);
    const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);
    if (err != NULL) {
        return err;
    }
    mconf->basefilename = apr_pstrdup(cmd->pool, word);
    if (apr_dir_make_recursive(mconf->basefilename, APR_UREAD | APR_UWRITE | APR_UEXECUTE, cmd->pool) != APR_SUCCESS)
        return  "Can't create directory corresponding to MemManagerFile";
    return NULL;
}
static const char *cmd_manager_balancername(cmd_parms *cmd, void *mconfig, const char *word)
{
    mod_manager_config *mconf = ap_get_module_config(cmd->server->module_config, &manager_module);
    mconf->balancername = apr_pstrdup(cmd->pool, word);
    normalize_balancer_name(mconf->balancername, cmd->server);
    return NULL;
}
static const char*cmd_manager_pers(cmd_parms *cmd, void *dummy, const char *arg)
{
    mod_manager_config *mconf = ap_get_module_config(cmd->server->module_config, &manager_module);
    const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);
    if (err != NULL) {
        return err;
    }
    if (strcasecmp(arg, "Off") == 0)
       mconf->persistent = 0;
    else if (strcasecmp(arg, "On") == 0)
       mconf->persistent = CREPER_SLOTMEM;
    else {
       return "PersistSlots must be one of: "
              "off | on";
    }
    return NULL;
}

static const char*cmd_manager_nonce(cmd_parms *cmd, void *dummy, const char *arg)
{
    mod_manager_config *mconf = ap_get_module_config(cmd->server->module_config, &manager_module);
    if (strcasecmp(arg, "Off") == 0)
       mconf->nonce = 0;
    else if (strcasecmp(arg, "On") == 0)
       mconf->nonce = -1;
    else {
       return "CheckNonce must be one of: "
              "off | on";
    }
    return NULL;
}
static const char*cmd_manager_allow_display(cmd_parms *cmd, void *dummy, const char *arg)
{
    mod_manager_config *mconf = ap_get_module_config(cmd->server->module_config, &manager_module);
    if (strcasecmp(arg, "Off") == 0)
       mconf->allow_display = 0;
    else if (strcasecmp(arg, "On") == 0)
       mconf->allow_display = -1;
    else {
       return "AllowDisplay must be one of: "
              "off | on";
    }
    return NULL;
}
static const char*cmd_manager_allow_cmd(cmd_parms *cmd, void *dummy, const char *arg)
{
    mod_manager_config *mconf = ap_get_module_config(cmd->server->module_config, &manager_module);
    if (strcasecmp(arg, "Off") == 0)
       mconf->allow_cmd = 0;
    else if (strcasecmp(arg, "On") == 0)
       mconf->allow_cmd = -1;
    else {
       return "AllowCmd must be one of: "
              "off | on";
    }
    return NULL;
}
static const char*cmd_manager_reduce_display(cmd_parms *cmd, void *dummy, const char *arg)
{
    mod_manager_config *mconf = ap_get_module_config(cmd->server->module_config, &manager_module);
    if (strcasecmp(arg, "Off") == 0)
       mconf->reduce_display = 0;
    else if (strcasecmp(arg, "On") == 0)
       mconf->reduce_display = -1;
    else {
       return "ReduceDisplay must be one of: "
              "off | on";
    }
    return NULL;
}
static const char*cmd_manager_maxmesssize(cmd_parms *cmd, void *mconfig, const char *word)
{
    mod_manager_config *mconf = ap_get_module_config(cmd->server->module_config, &manager_module);
    const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);
    if (err != NULL) {
        return err;
    }
    mconf->maxmesssize = atoi(word);
    if (mconf->maxmesssize < MAXMESSSIZE)
       return "MaxMCMPMessSize must bigger than 1024";
    return NULL;
}
static const char*cmd_manager_enable_mcpm_receive(cmd_parms *cmd, void *dummy)
{
    mod_manager_config *mconf = ap_get_module_config(cmd->server->module_config, &manager_module);
    if (!cmd->server->is_virtual)
        return "EnableMCPMReceive must be in a VirtualHost";
    mconf->enable_mcpm_receive = -1;
    return NULL;
}
static const char*cmd_manager_enable_ws_tunnel(cmd_parms *cmd, void *dummy)
{
    mod_manager_config *mconf = ap_get_module_config(cmd->server->module_config, &manager_module);
    const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);
    if (err != NULL) {
        return err;
    }
    if (ap_find_linked_module("mod_proxy_wstunnel.c") != NULL) {
        mconf->enable_ws_tunnel = -1;
        return NULL;
    } else {
        return "EnableWsTunnel requires mod_proxy_wstunnel.c";
    }
}


static const command_rec  manager_cmds[] =
{
    AP_INIT_TAKE1(
        "Maxcontext",
        cmd_manager_maxcontext,
        NULL,
        OR_ALL,
        "Maxcontext - number max context supported by mod_cluster"
    ),
    AP_INIT_TAKE1(
        "Maxnode",
        cmd_manager_maxnode,
        NULL,
        OR_ALL,
        "Maxnode - number max node supported by mod_cluster"
    ),
    AP_INIT_TAKE1(
        "Maxhost",
        cmd_manager_maxhost,
        NULL,
        OR_ALL,
        "Maxhost - number max host (Alias in virtual hosts) supported by mod_cluster"
    ),
    AP_INIT_TAKE1(
        "Maxsessionid",
        cmd_manager_maxsessionid,
        NULL,
        OR_ALL,
        "Maxsessionid - number session (Used to track number of sessions per nodes) supported by mod_cluster"
    ),
    AP_INIT_TAKE1(
        "MemManagerFile",
        cmd_manager_memmanagerfile,
        NULL,
        OR_ALL,
        "MemManagerFile - base name of the files used to create/attach to shared memory"
    ),
    AP_INIT_TAKE1(
        "ManagerBalancerName",
        cmd_manager_balancername,
        NULL,
        OR_ALL,
        "ManagerBalancerName - name of a balancer corresponding to the manager"
    ),
    AP_INIT_TAKE1(
        "PersistSlots",
        cmd_manager_pers,
        NULL,
        OR_ALL,
        "PersistSlots - Persist the slot mem elements on | off (Default: off No persistence)"
    ),
    AP_INIT_TAKE1(
        "CheckNonce",
        cmd_manager_nonce,
        NULL,
        OR_ALL,
        "CheckNonce - Switch check of nonce when using mod_cluster-manager handler on | off (Default: on Nonce checked)"
    ),
    AP_INIT_TAKE1(
        "AllowDisplay",
        cmd_manager_allow_display,
        NULL,
        OR_ALL,
        "AllowDisplay - Display additional information in the mod_cluster-manager page on | off (Default: off Only version displayed)"
    ),
    AP_INIT_TAKE1(
        "AllowCmd",
        cmd_manager_allow_cmd,
        NULL,
        OR_ALL,
        "AllowCmd - Allow commands using mod_cluster-manager URL on | off (Default: on Commmands allowed)"
    ),
    AP_INIT_TAKE1(
        "ReduceDisplay",
        cmd_manager_reduce_display,
        NULL,
        OR_ALL,
        "ReduceDisplay - Don't contexts in the main mod_cluster-manager page. on | off (Default: off Context displayed)"
    ),
   AP_INIT_TAKE1(
        "MaxMCMPMessSize",
        cmd_manager_maxmesssize,
        NULL,
        OR_ALL,
        "MaxMCMPMaxMessSize - Maximum size of MCMP messages. (Default: calculated min value: 1024)"
    ),
    AP_INIT_NO_ARGS(
        "EnableMCPMReceive",
         cmd_manager_enable_mcpm_receive,
         NULL,
         OR_ALL,
         "EnableMCPMReceive - Allow the VirtualHost to receive MCPM."
    ),
    AP_INIT_NO_ARGS(
        "EnableWsTunnel",
         cmd_manager_enable_ws_tunnel,
         NULL,
         OR_ALL,
         "EnableWsTunnel - Use ws or wss instead http or https when creating nodes (Allow Websockets proxing)."
    ),
    {NULL}
};

/* hooks declaration */

static void manager_hooks(apr_pool_t *p)
{
    static const char * const aszSucc[]={ "mod_proxy.c", NULL };

    /* Create the shared tables for mod_proxy_cluster */
    ap_hook_post_config(manager_init, NULL, NULL, APR_HOOK_MIDDLE);

    /* Attach to the shared tables with create the child */
    ap_hook_child_init(manager_child_init, NULL, NULL, APR_HOOK_MIDDLE);

    /* post read_request handling: to be handle to use ProxyPass / */
    ap_hook_translate_name(manager_trans, NULL, aszSucc,
                              APR_HOOK_FIRST);

    /* Process the request from the ModClusterService */
    ap_hook_handler(manager_handler, NULL, NULL, APR_HOOK_REALLY_FIRST);

    /* Register nodes/hosts/contexts table provider */
    ap_register_provider(p, "manager" , "shared", "0", &node_storage);
    ap_register_provider(p, "manager" , "shared", "1", &host_storage);
    ap_register_provider(p, "manager" , "shared", "2", &context_storage);
    ap_register_provider(p, "manager" , "shared", "3", &balancer_storage);
    ap_register_provider(p, "manager" , "shared", "4", &sessionid_storage);
    ap_register_provider(p, "manager" , "shared", "5", &domain_storage);
}

/*
 * Config creation stuff
 */
static void *create_manager_config(apr_pool_t *p)
{
    mod_manager_config *mconf = apr_pcalloc(p, sizeof(*mconf));

    mconf->basefilename = NULL;
    mconf->maxcontext = DEFMAXCONTEXT;
    mconf->maxnode = DEFMAXNODE;
    mconf->maxhost = DEFMAXHOST;
    mconf->maxsessionid = DEFMAXSESSIONID;
    mconf->tableversion = 0;
    mconf->persistent = 0;
    mconf->nonce = -1;
    mconf->balancername = NULL;
    mconf->allow_display = 0;
    mconf->allow_cmd = -1;
    mconf->reduce_display = 0;
    mconf->enable_mcpm_receive = 0;
    mconf->enable_ws_tunnel = 0;
    return mconf;
}

static void *create_manager_server_config(apr_pool_t *p, server_rec *s)
{
    return(create_manager_config(p));
}
static void *merge_manager_server_config(apr_pool_t *p, void *server1_conf,
                                         void *server2_conf)
{
    mod_manager_config *mconf1 = (mod_manager_config *) server1_conf;
    mod_manager_config *mconf2 = (mod_manager_config *) server2_conf;
    mod_manager_config *mconf = apr_pcalloc(p, sizeof(*mconf));

    mconf->basefilename = NULL;
    mconf->maxcontext = DEFMAXCONTEXT;
    mconf->maxnode = DEFMAXNODE;
    mconf->tableversion = 0;
    mconf->persistent = 0;
    mconf->nonce = -1;
    mconf->balancername = NULL;
    mconf->allow_display = 0;
    mconf->allow_cmd = -1;
    mconf->reduce_display = 0;

    if (mconf2->basefilename)
        mconf->basefilename = apr_pstrdup(p, mconf2->basefilename);
    else if (mconf1->basefilename)
        mconf->basefilename = apr_pstrdup(p, mconf1->basefilename);

    if (mconf2->maxcontext != DEFMAXCONTEXT)
        mconf->maxcontext = mconf2->maxcontext;
    else if (mconf1->maxcontext != DEFMAXCONTEXT)
        mconf->maxcontext = mconf1->maxcontext;

    if (mconf2->maxnode != DEFMAXNODE)
        mconf->maxnode = mconf2->maxnode;
    else if (mconf1->maxnode != DEFMAXNODE)
        mconf->maxnode = mconf1->maxnode;

    if (mconf2->maxhost != DEFMAXHOST)
        mconf->maxhost = mconf2->maxhost;
    else if (mconf1->maxhost != DEFMAXHOST)
        mconf->maxhost = mconf1->maxhost;

    if (mconf2->maxsessionid != DEFMAXSESSIONID)
        mconf->maxsessionid = mconf2->maxsessionid;
    else if (mconf1->maxsessionid != DEFMAXSESSIONID)
        mconf->maxsessionid = mconf1->maxsessionid;

    if (mconf2->persistent != 0)
        mconf->persistent = mconf2->persistent;
    else if (mconf1->persistent != 0)
        mconf->persistent = mconf1->persistent;

    if (mconf2->nonce != -1)
        mconf->nonce = mconf2->nonce;
    else if (mconf1->nonce != -1)
        mconf->nonce = mconf1->nonce;

    if (mconf2->balancername)
        mconf->balancername = apr_pstrdup(p, mconf2->balancername);
    else if (mconf1->balancername)
        mconf->balancername = apr_pstrdup(p, mconf1->balancername);

    if (mconf2->allow_display != 0)
        mconf->allow_display = mconf2->allow_display;
    else if (mconf1->allow_display != 0)
        mconf->allow_display = mconf1->allow_display;

    if (mconf2->allow_cmd != -1)
        mconf->allow_cmd = mconf2->allow_cmd;
    else if (mconf1->allow_cmd != -1)
        mconf->allow_cmd = mconf1->allow_cmd;

    if (mconf2->reduce_display != 0)
        mconf->reduce_display = mconf2->reduce_display;
    else if (mconf1->reduce_display != 0)
        mconf->reduce_display = mconf1->reduce_display;

    if (mconf2->enable_mcpm_receive != 0)
        mconf->enable_mcpm_receive = mconf2->enable_mcpm_receive;
    else if (mconf1->enable_mcpm_receive != 0)
        mconf->enable_mcpm_receive = mconf1->enable_mcpm_receive;

    if (mconf2->enable_ws_tunnel != 0)
        mconf->enable_ws_tunnel = mconf2->enable_ws_tunnel;
    else if (mconf1->enable_ws_tunnel != 0)
        mconf->enable_ws_tunnel = mconf1->enable_ws_tunnel;

    return mconf;
}

/* Module declaration */

module AP_MODULE_DECLARE_DATA manager_module = {
    STANDARD20_MODULE_STUFF,
    NULL,
    NULL,
    create_manager_server_config,
    merge_manager_server_config,
    manager_cmds,      /* command table */
    manager_hooks      /* register hooks */
};
