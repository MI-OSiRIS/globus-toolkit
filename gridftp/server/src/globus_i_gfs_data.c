
#include "globus_i_gridftp_server.h"
/* provides local_extensions */
#include "extensions.h"
#include <unistd.h>
#include <openssl/des.h>

#define USER_NAME_MAX   64
#define FTP_SERVICE_NAME "file"

struct passwd *                         globus_l_gfs_data_pwent = NULL;
static globus_gfs_storage_iface_t *     globus_l_gfs_dsi = NULL;
globus_extension_registry_t             globus_i_gfs_dsi_registry;
globus_extension_handle_t               globus_i_gfs_active_dsi_handle;
static globus_bool_t                    globus_l_gfs_data_is_remote_node = GLOBUS_FALSE;

typedef enum
{
    GLOBUS_L_GFS_DATA_REQUESTING = 1,
    GLOBUS_L_GFS_DATA_CONNECTING,
    GLOBUS_L_GFS_DATA_CONNECTED,
    GLOBUS_L_GFS_DATA_ABORTING,
    GLOBUS_L_GFS_DATA_ABORT_CLOSING,
    GLOBUS_L_GFS_DATA_FINISH,
    GLOBUS_L_GFS_DATA_COMPLETING,
    GLOBUS_L_GFS_DATA_COMPLETE
} globus_l_gfs_data_state_t;

typedef enum
{
    GLOBUS_L_GFS_DATA_HANDLE_VALID = 1,
    GLOBUS_L_GFS_DATA_HANDLE_INUSE,
    GLOBUS_L_GFS_DATA_HANDLE_CLOSING,
    GLOBUS_L_GFS_DATA_HANDLE_CLOSING_AND_DESTROYED,
    GLOBUS_L_GFS_DATA_HANDLE_CLOSED
} globus_l_gfs_data_handle_state_t;

typedef enum
{
    GLOBUS_L_GFS_DATA_INFO_TYPE_COMMAND = 1,
    GLOBUS_L_GFS_DATA_INFO_TYPE_TRANSFER,
    GLOBUS_L_GFS_DATA_INFO_TYPE_PASSIVE,
    GLOBUS_L_GFS_DATA_INFO_TYPE_ACTIVE,
    GLOBUS_L_GFS_DATA_INFO_TYPE_STAT,
    GLOBUS_L_GFS_DATA_INFO_TYPE_SEND,
    GLOBUS_L_GFS_DATA_INFO_TYPE_RECV,
    GLOBUS_L_GFS_DATA_INFO_TYPE_LIST
} globus_l_gfs_data_info_type_t;

typedef struct
{
    globus_gfs_operation_t   op;
    
    union
    {
        globus_gridftp_server_write_cb_t write;
        globus_gridftp_server_read_cb_t  read;
    } callback;
    void *                              user_arg;
    globus_gfs_finished_info_t *        finished_info;
} globus_l_gfs_data_bounce_t;

typedef struct
{
    globus_i_gfs_acl_handle_t           acl_handle;
    gss_cred_id_t                       del_cred;   
    void *                              session_arg;
    void *                              data_handle;
    globus_mutex_t                      mutex;
    int                                 ref;
    globus_gfs_storage_iface_t *        dsi;
    globus_extension_handle_t           dsi_handle;

    char *                              mod_dsi_name;
    globus_gfs_storage_iface_t *        mod_dsi;
    globus_extension_handle_t           mod_dsi_handle;

    globus_handle_table_t               data_table;
    int                                 node_ndx;
} globus_l_gfs_data_session_t;

typedef struct
{
    globus_l_gfs_data_session_t *       session_handle;
    struct globus_l_gfs_data_operation_s * op;
    globus_l_gfs_data_handle_state_t    state;
    globus_gfs_data_info_t              info;
    globus_ftp_control_handle_t         data_channel;
    int                                 remote_handle_id;
    globus_bool_t                       is_mine;
} globus_l_gfs_data_handle_t;  

typedef struct globus_l_gfs_data_operation_s
{
    globus_l_gfs_data_state_t           state;
    globus_bool_t                       writing;
    globus_l_gfs_data_handle_t *        data_handle;
    int                                 data_handle_id;
    
    globus_l_gfs_data_session_t *       session_handle;
    void *                              info_struct;
    globus_l_gfs_data_info_type_t       type;

    int                                 id;
    globus_gfs_ipc_handle_t             ipc_handle;
    
    uid_t                               uid;
    /* transfer stuff */
    globus_range_list_t                 range_list;
    globus_off_t                        partial_offset;
    globus_off_t                        partial_length;
    const char *                        list_type;

    globus_off_t                        max_offset;
    globus_off_t                        recvd_bytes;
    globus_range_list_t                 recvd_ranges;
    
    int                                 nstreams;
    int                                 stripe_count;
    int *                               eof_count;
    globus_bool_t                       eof_ready;
    int                                 node_count;
    int                                 node_ndx;
    int                                 write_stripe;

    int                                 stripe_connections_pending;
       
    int                                 write_delta;
    int                                 stripe_chunk;
    globus_range_list_t                 stripe_range_list;
    
    /* command stuff */
    globus_gfs_command_type_t           command;
    char *                              pathname;
    globus_off_t                        cksm_offset;
    globus_off_t                        cksm_length;
    char *                              cksm_alg;
    char *                              cksm_response;
    mode_t                              chmod_mode;
    char *                              rnfr_pathname;    
    /**/
    
    int                                 transfer_id; 
    int                                 event_mask;
    
    globus_i_gfs_data_callback_t        callback;
    globus_i_gfs_data_event_callback_t  event_callback;
    void *                              user_arg;

    int                                 ref;
    globus_result_t                     cached_res;
    void *                              info;

    globus_gfs_storage_iface_t *        dsi;
    int                                 sent_partial_eof;
} globus_l_gfs_data_operation_t;

typedef struct
{
    globus_l_gfs_data_operation_t *     op;
    int                                 event_type;
} globus_l_gfs_data_trev_bounce_t;

typedef struct
{
    globus_result_t                     result;
    globus_gfs_ipc_handle_t             ipc_handle;
    int                                 id;
    globus_l_gfs_data_handle_t *        handle;
    globus_bool_t                       bi_directional;
    globus_i_gfs_data_callback_t        callback;
    void *                              user_arg;
} globus_l_gfs_data_active_bounce_t;

typedef struct
{
    globus_gfs_ipc_handle_t             ipc_handle;
    int                                 id;
    globus_l_gfs_data_handle_t *        handle;
    globus_bool_t                       bi_directional;
    char *                              contact_string;
    globus_i_gfs_data_callback_t        callback;
    void *                              user_arg;
    globus_result_t                     result;
} globus_l_gfs_data_passive_bounce_t;

typedef struct
{
    globus_l_gfs_data_operation_t *     op;
    globus_object_t *                   error;
    int                                 stat_count;
    globus_gfs_stat_t *                 stat_array;
} globus_l_gfs_data_stat_bounce_t;

static
void
globus_l_gfs_data_end_transfer_kickout(
    void *                              user_arg);

static
void
globus_l_gfs_data_start_abort(
    globus_l_gfs_data_operation_t *     op);

static
void
globus_l_gfs_data_write_eof_cb(
    void *                              user_arg,
    globus_ftp_control_handle_t *       handle,
    globus_object_t *                   error,
    globus_byte_t *                     buffer,
    globus_size_t                       length,
    globus_off_t                        offset,
    globus_bool_t                       eof);

static
void
globus_l_gfs_data_end_read_kickout(
    void *                              user_arg);
    
static
void
globus_l_gfs_blocking_dispatch_kickout(
    void *                              user_arg)
{
    globus_l_gfs_data_operation_t *     op;
    globus_gfs_transfer_info_t *        transfer_info;
    globus_gfs_command_info_t *         cmd_info;
    globus_gfs_data_info_t *            data_info;
    globus_gfs_stat_info_t *            stat_info;

    op = (globus_l_gfs_data_operation_t *) user_arg;

    globus_thread_blocking_will_block();

    switch(op->type)
    {
        case GLOBUS_L_GFS_DATA_INFO_TYPE_COMMAND:
            cmd_info = (globus_gfs_command_info_t *) op->info_struct;
            op->session_handle->dsi->command_func(
                op, cmd_info, op->session_handle->session_arg);
            break;

        case GLOBUS_L_GFS_DATA_INFO_TYPE_STAT:
            stat_info = op->info_struct;
            op->session_handle->dsi->stat_func(
                op, stat_info, op->session_handle->session_arg);
            break;

        case GLOBUS_L_GFS_DATA_INFO_TYPE_PASSIVE:
            data_info = (globus_gfs_data_info_t *) op->info_struct;
            op->session_handle->dsi->passive_func(
                op, data_info, op->session_handle->session_arg);
            break;

        case GLOBUS_L_GFS_DATA_INFO_TYPE_ACTIVE:
            data_info = (globus_gfs_data_info_t *) op->info_struct;
            op->session_handle->dsi->active_func(
                op, data_info, op->session_handle->session_arg);
            break;

        case GLOBUS_L_GFS_DATA_INFO_TYPE_SEND:
            transfer_info = (globus_gfs_transfer_info_t *) op->info_struct;
            op->dsi->send_func(
                op, transfer_info, op->session_handle->session_arg);
            break;

        case GLOBUS_L_GFS_DATA_INFO_TYPE_RECV:
            transfer_info = (globus_gfs_transfer_info_t *) op->info_struct;
            op->dsi->recv_func(
                op, transfer_info, op->session_handle->session_arg);
            break;

        case GLOBUS_L_GFS_DATA_INFO_TYPE_LIST:
            transfer_info = (globus_gfs_transfer_info_t *) op->info_struct;
            op->session_handle->dsi->list_func(
                op, transfer_info, op->session_handle->session_arg);
            break;

        default:
            globus_assert(0 && "possible memory corruption");
            break;
    }
}

void
globus_i_gfs_monitor_init(
    globus_i_gfs_monitor_t *            monitor)
{
    globus_mutex_init(&monitor->mutex, NULL);
    globus_cond_init(&monitor->cond, NULL);
    monitor->done = GLOBUS_FALSE;
}

void
globus_i_gfs_monitor_wait(
    globus_i_gfs_monitor_t *            monitor)
{
    globus_mutex_lock(&monitor->mutex);
    {
        while(!monitor->done)
        {
            globus_cond_wait(&monitor->cond, &monitor->mutex);
        }
    }
    globus_mutex_unlock(&monitor->mutex);
}

void
globus_i_gfs_monitor_destroy(
    globus_i_gfs_monitor_t *            monitor)
{
    globus_mutex_destroy(&monitor->mutex);
    globus_cond_destroy(&monitor->cond);
}

void
globus_i_gfs_monitor_signal(
    globus_i_gfs_monitor_t *            monitor)
{
    globus_mutex_lock(&monitor->mutex);
    {
        monitor->done = GLOBUS_TRUE;
        globus_cond_signal(&monitor->cond);
    }
    globus_mutex_unlock(&monitor->mutex);
}

static
globus_gfs_storage_iface_t *
globus_i_gfs_data_new_dsi(
    globus_extension_handle_t *         dsi_handle,
    const char *                        dsi_name)
{
    globus_gfs_storage_iface_t *        new_dsi;

    /* see if we already have this name loaded, if so use it */
    new_dsi = (globus_gfs_storage_iface_t *) globus_extension_lookup(
        dsi_handle, GLOBUS_GFS_DSI_REGISTRY, dsi_name);
    if(new_dsi == NULL)
    {
        /* if not already load it, activate it */
        char                            buf[256];

        snprintf(buf, 256, "globus_gridftp_server_%s", dsi_name);
        buf[255] = 0;

        if(globus_extension_activate(buf) != GLOBUS_SUCCESS)
        {
            globus_i_gfs_log_message(
                GLOBUS_I_GFS_LOG_ERR, "Unable to activate %s\n", buf);
        }
        else
        {
            new_dsi = (globus_gfs_storage_iface_t *) 
                globus_extension_lookup(
                    dsi_handle,
                    GLOBUS_GFS_DSI_REGISTRY,
                    dsi_name);
        }
    }
    return new_dsi;
}

static
globus_gfs_storage_iface_t *
globus_l_gfs_data_new_dsi(
    globus_l_gfs_data_session_t *       session_handle,
    const char *                        in_module_name)
{
    const char *                        module_name;

    if(in_module_name == NULL || *in_module_name == '\0')
    {
        return session_handle->dsi;
    }
    if(!(session_handle->dsi->descriptor & GLOBUS_GFS_DSI_DESCRIPTOR_SENDER))
    {
        goto type_error;
    }

    module_name = globus_i_gfs_config_get_module_name(in_module_name);
    if(module_name == NULL)
    {
        goto type_error;
    }
    /* if there was a prevous loaded module */
    if(session_handle->mod_dsi_name != NULL)
    {
        /* if there was a last module and it is this one use it */
        if(strcmp(module_name, session_handle->mod_dsi_name) != 0)
        {
            globus_free(session_handle->mod_dsi_name);
            globus_extension_release(session_handle->mod_dsi_handle);

            session_handle->mod_dsi_name = globus_libc_strdup(module_name);
            session_handle->mod_dsi = globus_i_gfs_data_new_dsi(
                &session_handle->mod_dsi_handle,
                session_handle->mod_dsi_name);
            if(session_handle->mod_dsi == NULL)
            {
                goto error;
            }
        }
    }
    else
    {
        session_handle->mod_dsi_name =  globus_libc_strdup(module_name);
        session_handle->mod_dsi = globus_i_gfs_data_new_dsi(
            &session_handle->mod_dsi_handle,
            session_handle->mod_dsi_name);
        if(session_handle->mod_dsi == NULL)
        {
            goto error;
        }
    }

    return session_handle->mod_dsi;

error:
    globus_free(session_handle->mod_dsi_name);
type_error:
    return NULL;
}


static
void
globus_l_gfs_data_auth_init_cb(
    const char *                        resource_id,
    void *                              user_arg,
    globus_result_t                     result)
{
    gss_buffer_desc                     buffer;
    int                                 maj_stat;
    int                                 min_stat;    
    globus_l_gfs_data_operation_t *     op;
    globus_gfs_session_info_t *         session_info;
    GlobusGFSName(globus_l_gfs_data_auth_init_cb);

    op = (globus_l_gfs_data_operation_t *) user_arg;
    session_info = (globus_gfs_session_info_t *) op->info_struct;

    if(result != GLOBUS_SUCCESS)
    {
        goto error;
    }

    if(session_info->del_cred != NULL)
    {    
        maj_stat = gss_export_cred(
            &min_stat, session_info->del_cred, NULL, 0, &buffer);
        if(maj_stat != GSS_S_COMPLETE)
        {
            result = GlobusGFSErrorWrapFailed("gss_export_cred", min_stat);
            goto error;
        }
        maj_stat = gss_import_cred(
            &min_stat, 
            &session_info->del_cred, 
            GSS_C_NO_OID, 
            0, 
            &buffer, 
            0, 
            NULL);
        if(maj_stat != GSS_S_COMPLETE)
        {
            result = GlobusGFSErrorWrapFailed("gss_import_cred", min_stat);
            goto error_import;
        }
        maj_stat = gss_release_buffer(&min_stat, &buffer);
        if(maj_stat != GSS_S_COMPLETE)
        {
            result = GlobusGFSErrorWrapFailed("gss_release_buffer", min_stat);
            goto error_import;
        }
    }

    if(op->session_handle->dsi->init_func != NULL)
    {
        op->session_handle->dsi->init_func(op, session_info);
    }
    else
    {
        if(op->callback == NULL)
        {
            globus_gfs_ipc_reply_session(
                op->ipc_handle, GLOBUS_SUCCESS, op->session_handle);
        }
        else
        {
            globus_gfs_finished_info_t      finished_info;
            finished_info.result = GLOBUS_SUCCESS;
            finished_info.session_arg = op->session_handle;
            
            op->callback(
                &finished_info,
                op->user_arg);
        }
    }    

    return;

error_import:
    gss_release_buffer(&min_stat, &buffer);

error:
    if(op->callback == NULL)
    {
        globus_gfs_ipc_reply_session(op->ipc_handle, result, NULL);
    }
    else
    {
        globus_gfs_finished_info_t      finished_info;
        finished_info.result = result;
        finished_info.session_arg = NULL;
        
        op->callback(
            &finished_info,
            op->user_arg);
    }
}


static
void
globus_l_gfs_data_authorize(
    globus_l_gfs_data_operation_t *     op,
    const gss_ctx_id_t                  context,
    globus_gfs_session_info_t *         session_info)
{
    int                                 rc;
    globus_result_t                     res;
    int                                 gid;
    char *                              pw_file;
    char *                              usr;
    char *                              grp;
    char *                              pw_hash;
    char                                authz_usr[USER_NAME_MAX];
    struct passwd *                     pwent;
    struct group *                      grent = NULL;
    GlobusGFSName(globus_l_gfs_data_authorize);

    pw_file = (char *)globus_i_gfs_config_string("pw_file");
    /* if there is a del cred we are using gsi, look it up in the gridmap */
    if(session_info->del_cred != NULL)
    {
        if(context != NULL)
        {
            if(strcmp(":globus-mapping:", session_info->username) == 0)
            {
                usr = NULL;
            }
            else
            {
                usr = session_info->username;
            }
            res = globus_gss_assist_map_and_authorize(
                context,
                FTP_SERVICE_NAME,
                usr,
                authz_usr,
                USER_NAME_MAX);
            if(res != GLOBUS_SUCCESS)
            {
                goto pwent_error;
            }
            usr = authz_usr;
        }
        else
        {
            if(strcmp(":globus-mapping:", session_info->username) == 0)
            {
                rc = globus_gss_assist_gridmap(
                    (char *) session_info->subject, &usr);
            }
            else
            {
                rc = globus_gss_assist_userok(
                    session_info->subject, session_info->username);
                usr = session_info->username;
            }
            if(rc != 0)
            {
                res = GlobusGFSErrorParameter("gridmap");
                goto pwent_error;
            }
        }
        pwent = getpwnam(usr);
        if(pwent == NULL)
        {
            res = GlobusGFSErrorParameter("pwent id");
            goto pwent_error;
        }
        gid = pwent->pw_gid;
        grent = getgrgid(pwent->pw_gid);
        if(grent == NULL)
        {
            res = GlobusGFSErrorParameter("group id");
            goto pwent_error;
        }
    }
    /* if anonymous use and we are allowing it */
    else if(globus_i_gfs_config_bool("allow_anonymous") &&
        globus_i_gfs_config_is_anonymous(session_info->username))
    {
        /* if we are root, set to anon user */
        if(getuid() == 0)
        {
            usr = globus_i_gfs_config_string("anonymous_user");
            if(usr == NULL)
            {
                res = GlobusGFSErrorParameter("anon");
                goto pwent_error;
            }
            pwent = getpwnam(usr);
            if(pwent == NULL)
            {
                res = GlobusGFSErrorParameter("pwent");
                goto pwent_error;
            }
            grp = globus_i_gfs_config_string("anonymous_group");
            if(grp == NULL)
            {
                grent = getgrgid(pwent->pw_gid);
            }
            else
            {
                grent = getgrnam(grp);
            }
            if(grent == NULL)
            {
                res = GlobusGFSErrorParameter("group ent");
                goto pwent_error;
            }
        }
        /* if not root, jsut run as is */
        else
        {
            pwent = getpwuid(getuid());
            if(pwent == NULL)
            {
                res = GlobusGFSErrorParameter("pwent");
                goto pwent_error;
            }
            gid = pwent->pw_gid;
            grent = getgrgid(pwent->pw_gid);
            if(grent == NULL)
            {
                res = GlobusGFSErrorParameter("grent");
                goto pwent_error;
            }
        }
        /* since this is anonymous change the user name */
        pwent->pw_name = "anonymous";
        grent->gr_name = "anonymous";
    }
    else if(pw_file != NULL)
    {
        /* if we have not yet looked it up for this user */
        if(globus_l_gfs_data_pwent == NULL)
        {
            FILE * pw = fopen(pw_file, "r");
            if(pw == NULL)
            {
                res = GlobusGFSErrorParameter("pw file");
                goto pwent_error;
            }

            do
            {
                pwent = fgetpwent(pw);
            }
            while(pwent != NULL && 
                strcmp(pwent->pw_name, session_info->username) != 0);

            if(pwent == NULL)
            {
                res = GlobusGFSErrorParameter("pwent");
                goto pwent_error;
            }
            fclose(pw);
        }
        else
        {
            /* if already looked up (and setuid()) use global value */
            pwent = globus_l_gfs_data_pwent;
            if(strcmp(pwent->pw_name, session_info->username) != 0)
            {
                res = GlobusGFSErrorParameter("username");
                goto pwent_error;
            }
        }
        grent = getgrgid(pwent->pw_gid);
        if(grent == NULL)
        {
            res = GlobusGFSErrorParameter("grent");
            goto pwent_error;
        }
        pw_hash = DES_crypt(session_info->password, pwent->pw_passwd);
        if(strcmp(pw_hash, pwent->pw_passwd) != 0)
        {
            res = GlobusGFSErrorParameter("passwords");
            goto pwent_error;
        }
        gid = pwent->pw_gid;
    }
    else
    {
        res = GlobusGFSErrorParameter("auth");
        goto pwent_error;
    }

    globus_l_gfs_data_pwent = pwent;
    /* change process ids */
    rc = setgid(gid);
    if(rc != 0)
    {
        res = GlobusGFSErrorParameter("setgid");
        goto uid_error;
    }
    rc = setuid(pwent->pw_uid);
    if(rc != 0)
    {
        res = GlobusGFSErrorParameter("setuid");
        goto uid_error;
    }
    rc = globus_i_gfs_acl_init(
        &op->session_handle->acl_handle,
        pwent,
        grent,
        session_info->password,
        session_info->host_id,
        FTP_SERVICE_NAME,
        &res,
        globus_l_gfs_data_auth_init_cb,
        op);
    if(rc < 0)
    {
        goto acl_error;
    }
    else if(rc == GLOBUS_GFS_ACL_COMPLETE)
    {
        globus_l_gfs_data_auth_init_cb(NULL, op, res);
    }

    return;

acl_error:
uid_error:
pwent_error:
    if(op->callback == NULL)
    {
        globus_gfs_ipc_reply_session(op->ipc_handle, res, NULL);
    }
    else
    {
        globus_gfs_finished_info_t      finished_info;
        finished_info.result = res;
        finished_info.session_arg = NULL;
        
        op->callback(
            &finished_info,
            op->user_arg);
    }
}

void
globus_i_gfs_data_init()
{
    char *                              dsi_name;
    
    dsi_name = globus_i_gfs_config_string("dsi");
    
    globus_extension_register_builtins(local_extensions);

    globus_l_gfs_dsi = (globus_gfs_storage_iface_t *) globus_extension_lookup(
        &globus_i_gfs_active_dsi_handle, GLOBUS_GFS_DSI_REGISTRY, dsi_name);
    if(!globus_l_gfs_dsi)
    {
        char                            buf[256];
        
        snprintf(buf, 256, "globus_gridftp_server_%s", dsi_name);
        buf[255] = 0;
    
        if(globus_extension_activate(buf) != GLOBUS_SUCCESS)
        {
            globus_i_gfs_log_message(
                GLOBUS_I_GFS_LOG_ERR, "Unable to activate %s\n", buf);
            exit(1);
        }
        
        globus_l_gfs_dsi = (globus_gfs_storage_iface_t *) globus_extension_lookup(
            &globus_i_gfs_active_dsi_handle, GLOBUS_GFS_DSI_REGISTRY, dsi_name);
    }
    
    if(!globus_l_gfs_dsi)
    {
        globus_i_gfs_log_message(
           GLOBUS_I_GFS_LOG_ERR, "Couldn't find the %s extension\n", dsi_name);
        exit(1);
    }
}

static
globus_result_t
globus_l_gfs_data_operation_init(
    globus_l_gfs_data_operation_t **    u_op)
{
    globus_l_gfs_data_operation_t *     op;
    globus_result_t                     result;
    GlobusGFSName(globus_l_gfs_operation_init);
    
    op = (globus_l_gfs_data_operation_t *) 
        globus_calloc(1, sizeof(globus_l_gfs_data_operation_t));
    if(!op)
    {
        result = GlobusGFSErrorMemory("op");
        goto error_alloc;
    }
    
    globus_range_list_init(&op->recvd_ranges);
    globus_range_list_init(&op->stripe_range_list);
    op->recvd_bytes = 0;
    op->max_offset = -1;
    
    *u_op = op;
    return GLOBUS_SUCCESS;
    
error_alloc:
    return result;
}

static
void
globus_l_gfs_data_operation_destroy(
    globus_l_gfs_data_operation_t *     op)
{
    globus_range_list_destroy(op->recvd_ranges);
    globus_range_list_destroy(op->stripe_range_list);
    if(op->pathname)
    {
        globus_free(op->pathname);
    }
    if(op->list_type)
    {
        globus_free((char *) op->list_type);
    }
    if(op->eof_count != NULL)
    {
        globus_free(op->eof_count);
    }
    globus_free(op);
}

    
void
globus_i_gfs_data_request_stat(
    globus_gfs_ipc_handle_t             ipc_handle,
    void *                              session_arg,
    int                                 id,
    globus_gfs_stat_info_t *            stat_info,
    globus_i_gfs_data_callback_t        cb,
    void *                              user_arg)
{
    globus_l_gfs_data_operation_t *     op;
    globus_result_t                     result;
    globus_l_gfs_data_session_t *       session_handle;    
    GlobusGFSName(globus_i_gfs_data_stat_request);

    session_handle = (globus_l_gfs_data_session_t *) session_arg;

    result = globus_l_gfs_data_operation_init(&op);
    if(result != GLOBUS_SUCCESS)
    {
        result = GlobusGFSErrorWrapFailed(
            "globus_l_gfs_data_operation_init", result);
        goto error_op;
    }
    
    op->ipc_handle = ipc_handle;
    op->id = id;
    op->uid = getuid();
    
    op->state = GLOBUS_L_GFS_DATA_REQUESTING;
    op->callback = cb;
    op->user_arg = user_arg;
    op->session_handle = session_handle;
    op->info_struct = stat_info;
    op->type = GLOBUS_L_GFS_DATA_INFO_TYPE_STAT;

    if(session_handle->dsi->descriptor & GLOBUS_GFS_DSI_DESCRIPTOR_BLOCKING)
    { 
        globus_callback_register_oneshot(
            NULL,
            NULL,
            globus_l_gfs_blocking_dispatch_kickout,
            op);
    }
    else
    {
        session_handle->dsi->stat_func(
            op, stat_info, session_handle->session_arg);
    }
    return;

error_op:
    return;
}

static
void
globus_l_gfs_data_stat_kickout(
    void *                              user_arg)
{
    globus_l_gfs_data_stat_bounce_t *   bounce_info;
    globus_gfs_ipc_reply_t              reply;   

    bounce_info = (globus_l_gfs_data_stat_bounce_t *) user_arg;

    memset(&reply, '\0', sizeof(globus_gfs_ipc_reply_t));
 
    reply.type = GLOBUS_GFS_OP_STAT;
    reply.id = bounce_info->op->id;
    reply.result = bounce_info->error ? 
        globus_error_put(bounce_info->error) : GLOBUS_SUCCESS;
    reply.info.stat.stat_array =  bounce_info->stat_array;
    reply.info.stat.stat_count =  bounce_info->stat_count;
    reply.info.stat.uid =  bounce_info->op->uid;

    if(bounce_info->op->callback != NULL)
    {
        bounce_info->op->callback(
            &reply,
            bounce_info->op->user_arg);
    }
    else
    {    
        globus_gfs_ipc_reply_finished(
            bounce_info->op->ipc_handle,
            &reply);
    }
                
    globus_l_gfs_data_operation_destroy(bounce_info->op);
    if(bounce_info->stat_array)
    {
        globus_free(bounce_info->stat_array);
    }
    globus_free(bounce_info);
}

void
globus_i_gfs_data_request_command(
    globus_gfs_ipc_handle_t             ipc_handle,
    void *                              session_arg,
    int                                 id,
    globus_gfs_command_info_t *         cmd_info,
    globus_i_gfs_data_callback_t        cb,
    void *                              user_arg)
{
    globus_l_gfs_data_operation_t *     op;
    globus_result_t                     result;
    globus_extension_handle_t           new_dsi_handle;
    globus_gfs_storage_iface_t *        new_dsi;
    globus_l_gfs_data_session_t *       session_handle;
    char *                              dsi_name;
    GlobusGFSName(globus_i_gfs_data_command_request);

    session_handle = (globus_l_gfs_data_session_t *) session_arg;

    result = globus_l_gfs_data_operation_init(&op);
    if(result != GLOBUS_SUCCESS)
    {
        result = GlobusGFSErrorWrapFailed(
            "globus_l_gfs_data_operation_init", result);
        goto error_op;
    }
    op->ipc_handle = ipc_handle;
    op->id = id;
    op->state = GLOBUS_L_GFS_DATA_REQUESTING;
    op->command = cmd_info->command;
    op->pathname = globus_libc_strdup(cmd_info->pathname);
    op->callback = cb;
    op->user_arg = user_arg;
    op->session_handle = session_handle;
    op->info_struct = cmd_info;
    op->type = GLOBUS_L_GFS_DATA_INFO_TYPE_COMMAND;
    dsi_name = cmd_info->pathname;

    if(cmd_info->command == GLOBUS_GFS_CMD_SITE_DSI &&
        session_handle->dsi->descriptor & GLOBUS_GFS_DSI_DESCRIPTOR_SENDER)
    {
        new_dsi = globus_i_gfs_data_new_dsi(&new_dsi_handle, dsi_name);

        /* if we couldn't load it, error */
        if(new_dsi == NULL)
        {
            globus_gridftp_server_finished_command(op, result, NULL);
        }
        /* if it is the wrong type release and error */
        else if(!(new_dsi->descriptor & GLOBUS_GFS_DSI_DESCRIPTOR_SENDER))
        {
            globus_gridftp_server_finished_command(op, result, NULL);
        }
        /* if all is well */
        else
        {
            /* if not the global default release the reference */
            if(session_handle->dsi != globus_l_gfs_dsi)
            {
                globus_extension_release(session_handle->dsi_handle);
            }
            /* set to new dsi */
            session_handle->dsi_handle = new_dsi_handle;
            op->session_handle->dsi = new_dsi;
        }
    }
    else if(
        session_handle->dsi->descriptor & GLOBUS_GFS_DSI_DESCRIPTOR_BLOCKING)
    {
        globus_callback_register_oneshot(
            NULL,
            NULL,
            globus_l_gfs_blocking_dispatch_kickout,
            op);
    }
    else
    { 
        session_handle->dsi->command_func(
            op, cmd_info, session_handle->session_arg);
    }

    return;

error_op:
    return;
}

static
globus_result_t
globus_l_gfs_data_handle_init(
    globus_l_gfs_data_handle_t **       u_handle,
    globus_gfs_data_info_t *            data_info)
{
    globus_l_gfs_data_handle_t *        handle;
    globus_result_t                     result;
    globus_ftp_control_dcau_t           dcau;
    GlobusGFSName(globus_l_gfs_data_handle_init);
    
    handle = (globus_l_gfs_data_handle_t *) 
        globus_malloc(sizeof(globus_l_gfs_data_handle_t));
    if(!handle)
    {
        result = GlobusGFSErrorMemory("handle");
        goto error_alloc;
    }
    
    memcpy(&handle->info, data_info, sizeof(globus_gfs_data_info_t));
    
    result = globus_ftp_control_handle_init(&handle->data_channel);
    if(result != GLOBUS_SUCCESS)
    {
        result = GlobusGFSErrorWrapFailed(
            "globus_ftp_control_handle_init", result);
        goto error_data;
    }

    handle->state = GLOBUS_L_GFS_DATA_HANDLE_VALID;
    handle->op = NULL;

    if(0 && !globus_l_gfs_data_is_remote_node)
    {
        /* this is too restrictive... if they connect to the server via ipv6
         * doesnt mean they know about ipv6 servers... this ends up requiring
         * that they use ipv6 commands
         */
        result = globus_ftp_control_data_set_interface(
            &handle->data_channel, handle->info.interface);
        if(result != GLOBUS_SUCCESS)
        {
            result = GlobusGFSErrorWrapFailed(
                "globus_ftp_control_data_set_interface", result);
            goto error_control;
        }
    }

    result = globus_ftp_control_local_mode(
        &handle->data_channel, handle->info.mode);
    if(result != GLOBUS_SUCCESS)
    {
        result = GlobusGFSErrorWrapFailed(
            "globus_ftp_control_local_mode", result);
        goto error_control;
    }
    
    result = globus_ftp_control_local_type(
        &handle->data_channel, handle->info.type, 0);
    if(result != GLOBUS_SUCCESS)
    {
        result = GlobusGFSErrorWrapFailed(
            "globus_ftp_control_local_type", result);
        goto error_control;
    }
    
    if(handle->info.tcp_bufsize > 0)
    {
        globus_ftp_control_tcpbuffer_t  tcpbuffer;
        
        tcpbuffer.mode = GLOBUS_FTP_CONTROL_TCPBUFFER_FIXED;
        tcpbuffer.fixed.size = handle->info.tcp_bufsize;
        
        result = globus_ftp_control_local_tcp_buffer(
            &handle->data_channel, &tcpbuffer);
        if(result != GLOBUS_SUCCESS)
        {
            result = GlobusGFSErrorWrapFailed(
                "globus_ftp_control_local_tcp_buffer", result);
            goto error_control;
        }
    }
    
    if(handle->info.mode == 'S')
    {
        handle->info.nstreams = 1;
    }
    else
    {
        globus_ftp_control_parallelism_t  parallelism;
        
        globus_assert(handle->info.mode == 'E');
        
        parallelism.mode = GLOBUS_FTP_CONTROL_PARALLELISM_FIXED;
        parallelism.fixed.size = handle->info.nstreams;
        
        result = globus_ftp_control_local_parallelism(
            &handle->data_channel, &parallelism);
        if(result != GLOBUS_SUCCESS)
        {
            result = GlobusGFSErrorWrapFailed(
                "globus_ftp_control_local_parallelism", result);
            goto error_control;
        }

        result = globus_ftp_control_local_send_eof(
            &handle->data_channel, GLOBUS_FALSE);
        if(result != GLOBUS_SUCCESS)
        {
            result = GlobusGFSErrorWrapFailed(
                "globus_ftp_control_local_send_eof", result);
            goto error_control;
        }
    }
    dcau.mode = handle->info.dcau;
    dcau.subject.mode = handle->info.dcau;
    dcau.subject.subject = handle->info.subject;
    result = globus_ftp_control_local_dcau(
        &handle->data_channel, &dcau, handle->info.del_cred);
    if(result != GLOBUS_SUCCESS)
    {
        result = GlobusGFSErrorWrapFailed(
            "globus_ftp_control_local_dcau", result);
        goto error_control;
    }
    if(handle->info.dcau != 'N')
    {
        result = globus_ftp_control_local_prot(
            &handle->data_channel, handle->info.prot);
        if(result != GLOBUS_SUCCESS)
        {
            result = GlobusGFSErrorWrapFailed(
                "globus_ftp_control_local_prot", result);
            goto error_control;
        }
    }
    if(handle->info.ipv6)
    {
        result = globus_ftp_control_ipv6_allow(
            &handle->data_channel, GLOBUS_TRUE);
        if(result != GLOBUS_SUCCESS)
        {
            result = GlobusGFSErrorWrapFailed(
                "globus_ftp_control_ipv6_allow", result);
            goto error_control;
        }
    }

    *u_handle = handle;
    return GLOBUS_SUCCESS;

error_control:
    globus_ftp_control_handle_destroy(&handle->data_channel);
    
error_data:
    globus_free(handle);
    
error_alloc:
    return result;
}

static
void
globus_l_gfs_data_abort_kickout(
    void *                              user_arg)
{
    globus_bool_t                       start_finish = GLOBUS_FALSE;
    globus_l_gfs_data_operation_t *     op;

    op = (globus_l_gfs_data_operation_t *) user_arg;

    if(op->session_handle->dsi->trev_func != NULL &&
        op->event_mask & GLOBUS_GFS_EVENT_TRANSFER_ABORT
        /* && op->data_handle->is_mine XXX don't think this matters here */
        )
    {
        op->session_handle->dsi->trev_func(
            op->transfer_id,
            GLOBUS_GFS_EVENT_TRANSFER_ABORT,
            op->session_handle->session_arg);
    }

    globus_mutex_lock(&op->session_handle->mutex);
    {
        switch(op->state)
        {
            /* if finished was called while waiting for this */
            case GLOBUS_L_GFS_DATA_FINISH:
                start_finish = GLOBUS_TRUE;
                break;

            case GLOBUS_L_GFS_DATA_ABORT_CLOSING:
                op->state = GLOBUS_L_GFS_DATA_ABORTING;
                break;

            case GLOBUS_L_GFS_DATA_CONNECTING:
            case GLOBUS_L_GFS_DATA_CONNECTED:
            case GLOBUS_L_GFS_DATA_REQUESTING:
            case GLOBUS_L_GFS_DATA_ABORTING:
            case GLOBUS_L_GFS_DATA_COMPLETING:
            case GLOBUS_L_GFS_DATA_COMPLETE:
            default:
                globus_assert(0 && "bad state, possible memory corruption");
                break;
        }

        op->ref--;
        globus_assert(op->ref > 0);
    }
    globus_mutex_unlock(&op->session_handle->mutex);

    if(start_finish)
    {
        globus_l_gfs_data_end_transfer_kickout(op);
    }
}

static
void
globus_l_gfs_data_finish_fc_cb(
    void *                              callback_arg,
    globus_ftp_control_handle_t *       ftp_handle,
    globus_object_t *                   error)
{
    globus_result_t                     result;
    globus_l_gfs_data_operation_t *     op;

    op = (globus_l_gfs_data_operation_t *) callback_arg;

    globus_mutex_lock(&op->session_handle->mutex);
    {
        switch(op->data_handle->state)
        {
            case GLOBUS_L_GFS_DATA_HANDLE_CLOSING:
                op->data_handle->state = GLOBUS_L_GFS_DATA_HANDLE_CLOSED;
                break;

            case GLOBUS_L_GFS_DATA_HANDLE_CLOSING_AND_DESTROYED:
                result = globus_ftp_control_handle_destroy(
                    &op->data_handle->data_channel);
                globus_assert(result == GLOBUS_SUCCESS);
                globus_free(op->data_handle);
                op->data_handle = NULL;
                break;

            case GLOBUS_L_GFS_DATA_HANDLE_VALID:
            case GLOBUS_L_GFS_DATA_HANDLE_INUSE:
            case GLOBUS_L_GFS_DATA_HANDLE_CLOSED:
            default:
                globus_assert(0 && "possible memory corruption");
                break;
        }
    }
    globus_mutex_unlock(&op->session_handle->mutex);

    globus_l_gfs_data_end_transfer_kickout(callback_arg);
}

static
void
globus_l_gfs_data_abort_fc_cb(
    void *                              callback_arg,
    globus_ftp_control_handle_t *       ftp_handle,
    globus_object_t *                   error)
{
    globus_result_t                     result;
    globus_l_gfs_data_operation_t *     op;

    op = (globus_l_gfs_data_operation_t *) callback_arg;

    globus_mutex_lock(&op->session_handle->mutex);
    {
        switch(op->data_handle->state)
        {
            case GLOBUS_L_GFS_DATA_HANDLE_CLOSING:
                op->data_handle->state = GLOBUS_L_GFS_DATA_HANDLE_CLOSED;
                break;

            case GLOBUS_L_GFS_DATA_HANDLE_CLOSING_AND_DESTROYED:
                result = globus_ftp_control_handle_destroy(
                    &op->data_handle->data_channel);
                globus_assert(result == GLOBUS_SUCCESS);
                globus_free(op->data_handle);
                op->data_handle = NULL;
                break;

            case GLOBUS_L_GFS_DATA_HANDLE_VALID:
            case GLOBUS_L_GFS_DATA_HANDLE_INUSE:
            case GLOBUS_L_GFS_DATA_HANDLE_CLOSED:
            default:
                globus_assert(0 && "possible memory corruption");
                break;
        }
    }
    globus_mutex_unlock(&op->session_handle->mutex);

    globus_l_gfs_data_abort_kickout(callback_arg);
}

static
void
globus_l_gfs_data_destroy_cb(
    void *                              callback_arg,
    globus_ftp_control_handle_t *       ftp_handle,
    globus_object_t *                   error)
{
    globus_result_t                     result;
    globus_bool_t                       free_session = GLOBUS_FALSE;
    globus_l_gfs_data_session_t *       session_handle;
    globus_l_gfs_data_handle_t *        data_handle;
    GlobusGFSName(globus_i_gfs_data_handle_destroy);

    data_handle = (globus_l_gfs_data_handle_t *) callback_arg;
    session_handle = data_handle->session_handle;

    globus_mutex_lock(&session_handle->mutex);
    {
        session_handle->ref--;
        if(session_handle->ref == 0)
        {
            free_session = GLOBUS_TRUE;
        }
        switch(data_handle->state)
        {
            /* destroy did come from server-lib so clean it up */
            case GLOBUS_L_GFS_DATA_HANDLE_CLOSING_AND_DESTROYED:
                result = globus_ftp_control_handle_destroy(
                    &data_handle->data_channel);
                globus_assert(result == GLOBUS_SUCCESS);
                globus_free(data_handle);
                break;

            /* none of these are possible */
            case GLOBUS_L_GFS_DATA_HANDLE_CLOSING:
            case GLOBUS_L_GFS_DATA_HANDLE_CLOSED:
            case GLOBUS_L_GFS_DATA_HANDLE_INUSE:
            case GLOBUS_L_GFS_DATA_HANDLE_VALID:
            default:
                globus_assert(0 && "possible memory corruption");
                break;
        }

    }
    globus_mutex_unlock(&session_handle->mutex);
}

/* 
 */
void
globus_i_gfs_data_request_handle_destroy(
    globus_gfs_ipc_handle_t             ipc_handle,
    void *                              in_session_arg,
    int                                 data_connection_id)
{
    globus_bool_t                       rc;
    void *                              session_arg;
    globus_bool_t                       pass = GLOBUS_FALSE;
    globus_result_t                     result;
    globus_l_gfs_data_session_t *       session_handle;    
    globus_l_gfs_data_handle_t *        data_handle;
    GlobusGFSName(globus_i_gfs_data_handle_destroy);

    session_handle = (globus_l_gfs_data_session_t *) in_session_arg;

    session_handle->data_handle = NULL;

    globus_mutex_lock(&session_handle->mutex);
    {
        data_handle = (globus_l_gfs_data_handle_t *) globus_handle_table_lookup(
            &session_handle->data_table, data_connection_id);
        if(data_handle == NULL)
        {
        }
        rc = globus_handle_table_decrement_reference(
            &session_handle->data_table, data_connection_id);
        globus_assert(!rc);

        session_arg = session_handle->session_arg;
        switch(data_handle->state)
        {
            case GLOBUS_L_GFS_DATA_HANDLE_VALID:
                session_handle->ref++;
                data_handle->state = 
                    GLOBUS_L_GFS_DATA_HANDLE_CLOSING_AND_DESTROYED;
                if(!data_handle->is_mine)
                {
                    pass = GLOBUS_TRUE;
                }
                else
                {
                    /* set directly to closed to that when callback
                        returns we clean it up */
                    result = globus_ftp_control_data_force_close(
                        &data_handle->data_channel,
                        globus_l_gfs_data_destroy_cb,
                        data_handle);
                    if(result != GLOBUS_SUCCESS)
                    {
                        session_handle->ref--;
                        /* this is strange, there is little we can do here */
                    }
                }
                break;

            case GLOBUS_L_GFS_DATA_HANDLE_INUSE:
                globus_assert(data_handle->op != NULL);
                globus_l_gfs_data_start_abort(data_handle->op);
                break;

            case GLOBUS_L_GFS_DATA_HANDLE_CLOSING:
                data_handle->state = 
                    GLOBUS_L_GFS_DATA_HANDLE_CLOSING_AND_DESTROYED;
                break;

            case GLOBUS_L_GFS_DATA_HANDLE_CLOSED:
                if(!data_handle->is_mine)
                {
                    pass = GLOBUS_TRUE;
                }
                else
                {
                    result = globus_ftp_control_handle_destroy(
                        &data_handle->data_channel);
                    if(result == GLOBUS_SUCCESS)
                    {
                        /* XXX this is strange, shouldn't be happening, make it 
                            an assert  */
                        globus_free(data_handle);
                    }
                }
                break;

            /* we shouldn't get this callback twice */
            case GLOBUS_L_GFS_DATA_HANDLE_CLOSING_AND_DESTROYED:
                globus_assert(0 && "got destroyed callback twice");
                break;
            default:
                globus_assert(0 && "likey memory corruption");
                break;
        }
    }
    globus_mutex_unlock(&session_handle->mutex);
    if(pass)
    {
        if(session_handle->dsi->data_destroy_func != NULL)
        {                
            session_handle->dsi->data_destroy_func(
                data_handle->remote_handle_id, session_arg);
        }
        else
        {
            /* XXX dsi impl error, what to do? */
        }
        globus_free(data_handle);
    }
}

static
void
globus_l_gfs_data_passive_kickout(
    void *                              user_arg)
{
    globus_l_gfs_data_passive_bounce_t * bounce_info;
    globus_gfs_ipc_reply_t              reply;   
    
    bounce_info = (globus_l_gfs_data_passive_bounce_t *) user_arg;

    memset(&reply, '\0', sizeof(globus_gfs_ipc_reply_t));
    reply.info.data.contact_strings = (const char **) 
        globus_calloc(1, sizeof(char *));
 
    reply.type = GLOBUS_GFS_OP_PASSIVE;
    reply.id = bounce_info->id;
    reply.result = bounce_info->result;

    reply.info.data.data_handle_id = globus_handle_table_insert(
        &bounce_info->handle->session_handle->data_table,
        bounce_info->handle,
        1);

    reply.info.data.bi_directional = bounce_info->bi_directional;
    reply.info.data.cs_count = 1;
    reply.info.data.contact_strings[0] = bounce_info->contact_string;

    /* as soon as we finish the data handle can be in play, set its
        state appropriately.  if not success then we never created a
        handle */
    if(bounce_info->result == GLOBUS_SUCCESS)
    {
        bounce_info->handle->is_mine = GLOBUS_TRUE;
        bounce_info->handle->state = GLOBUS_L_GFS_DATA_HANDLE_VALID;
    }
    else
    {
        globus_assert(bounce_info->handle == NULL);
    }

    if(bounce_info->callback != NULL)
    {
        bounce_info->callback(
            &reply,
            bounce_info->user_arg);        
    }
    else
    {
        globus_gfs_ipc_reply_finished(
            bounce_info->ipc_handle,
            &reply);
    }
 
    globus_free(reply.info.data.contact_strings);
    globus_free(bounce_info->contact_string);
    globus_free(bounce_info);
}

/*
 *
 *  NOTE: if bounce struct can't be allocated we fail. This should be 
 *        corrected at some point, possibly by preaalocating a bunch in
 *        in a globus_memory_t.
 */
void
globus_i_gfs_data_request_passive(
    globus_gfs_ipc_handle_t             ipc_handle,
    void *                              session_arg,
    int                                 id,
    globus_gfs_data_info_t *            data_info,
    globus_i_gfs_data_callback_t        cb,
    void *                              user_arg)
{
    globus_l_gfs_data_handle_t *        handle = NULL;
    globus_result_t                     result;
    globus_ftp_control_host_port_t      address;
    globus_sockaddr_t                   addr;
    char *                              cs;
    globus_l_gfs_data_passive_bounce_t * bounce_info;
    globus_l_gfs_data_operation_t *     op;
    globus_l_gfs_data_session_t *       session_handle;
    globus_bool_t                       ipv6_addr = GLOBUS_FALSE;
    GlobusGFSName(globus_i_gfs_data_request_passive);

    session_handle = (globus_l_gfs_data_session_t *) session_arg;

    if(session_handle->dsi->passive_func != NULL)
    {
        result = globus_l_gfs_data_operation_init(&op);
        if(result != GLOBUS_SUCCESS)
        {
            result = GlobusGFSErrorWrapFailed(
                "globus_l_gfs_data_operation_init", result);
            goto error_op;
        }
 
        op->ipc_handle = ipc_handle;
        op->id = id;
        op->state = GLOBUS_L_GFS_DATA_REQUESTING;
        op->callback = cb;
        op->user_arg = user_arg;
        op->session_handle = session_handle;
        op->info_struct = data_info;
        op->type = GLOBUS_L_GFS_DATA_INFO_TYPE_PASSIVE;
        if(session_handle->dsi->descriptor & GLOBUS_GFS_DSI_DESCRIPTOR_BLOCKING)
        {
            globus_callback_register_oneshot(
                NULL,
                NULL,
                globus_l_gfs_blocking_dispatch_kickout,
                op);
        }
        else
        {
            session_handle->dsi->passive_func(
                op, data_info, session_handle->session_arg);
        }
    }
    else
    {
        if(data_info->del_cred == NULL)
        {
            data_info->del_cred = session_handle->del_cred;
        }
        result = globus_l_gfs_data_handle_init(&handle, data_info);
        if(result != GLOBUS_SUCCESS)
        {
            result = GlobusGFSErrorWrapFailed(
                "globus_l_gfs_data_handle_init", result);
            goto error_handle;
        }
        handle->session_handle = session_handle;

        address.host[0] = 1; /* prevent address lookup */
        address.port = 0;
        result = globus_ftp_control_local_pasv(&handle->data_channel, &address);
        if(result != GLOBUS_SUCCESS)
        {
            result = GlobusGFSErrorWrapFailed(
                "globus_ftp_control_local_pasv", result);
            goto error_control;
        }

        /* XXX This needs to be smarter.  The address should be the same one
         * the user is connected to on the control channel (at least when
         * operating as a normal standalone server)
         */
        /* its ok to use AF_INET here since we are requesting the LOCAL
         * address.  we just use AF_INET to store the port
         */
        if(!globus_l_gfs_data_is_remote_node)
        {
            ipv6_addr = (strchr(handle->info.interface, ':') != NULL);
        }
        
        if(globus_l_gfs_data_is_remote_node ||
            (ipv6_addr && !handle->info.ipv6))
        {
            GlobusLibcSockaddrSetFamily(addr, AF_INET);
            GlobusLibcSockaddrSetPort(addr, address.port);
            result = globus_libc_addr_to_contact_string(
                &addr,
                GLOBUS_LIBC_ADDR_LOCAL | GLOBUS_LIBC_ADDR_NUMERIC |
                    (handle->info.ipv6 ? 0 : GLOBUS_LIBC_ADDR_IPV4),
                &cs);
            if(result != GLOBUS_SUCCESS)
            {
                result = GlobusGFSErrorWrapFailed(
                    "globus_libc_addr_to_contact_string", result);
                goto error_control;
            }
        }
        else
        {
            if(ipv6_addr)
            {
                cs = globus_common_create_string(
                    "[%s]:%d", handle->info.interface, (int) address.port);
            }
            else
            {
                cs = globus_common_create_string(
                    "%s:%d", handle->info.interface, (int) address.port);
            }
        }
        
        bounce_info = (globus_l_gfs_data_passive_bounce_t *)
            globus_malloc(sizeof(globus_l_gfs_data_passive_bounce_t));
        if(!bounce_info)
        {
            result = GlobusGFSErrorMemory("bounce_info");
            globus_panic(NULL, result, "small malloc failure, no recovery");
        }
        
        bounce_info->result = GLOBUS_SUCCESS;
        bounce_info->ipc_handle = ipc_handle;
        bounce_info->id = id;
        bounce_info->handle = handle;
        bounce_info->bi_directional = GLOBUS_TRUE; /* XXX MODE S only */
        bounce_info->contact_string = cs;
        bounce_info->callback = cb;
        bounce_info->user_arg = user_arg;

        session_handle->data_handle = handle;
        
        result = globus_callback_register_oneshot(
            NULL,
            NULL,
            globus_l_gfs_data_passive_kickout,
            bounce_info);
        if(result != GLOBUS_SUCCESS)
        {
            result = GlobusGFSErrorWrapFailed(
                "globus_callback_register_oneshot", result);
            globus_panic(NULL, result, "small malloc failure, no recovery");
        }
    }
    return;

error_control:
    globus_ftp_control_handle_destroy(&handle->data_channel);
    globus_free(handle);
    handle = NULL;
    
error_handle:
error_op:

    bounce_info = (globus_l_gfs_data_passive_bounce_t *)
        globus_malloc(sizeof(globus_l_gfs_data_passive_bounce_t));
    if(!bounce_info)
    {
        result = GlobusGFSErrorMemory("bounce_info");
        globus_panic(NULL, result, "small malloc failure, no recovery");
    }
    bounce_info->ipc_handle = ipc_handle;
    bounce_info->id = id;
    bounce_info->handle = handle;
    bounce_info->bi_directional = GLOBUS_TRUE; /* XXX MODE S only */
    bounce_info->contact_string = cs;
    bounce_info->callback = cb;
    bounce_info->user_arg = user_arg;
    bounce_info->result = result;
    bounce_info->handle = NULL;
    result = globus_callback_register_oneshot(
        NULL,
        NULL,
        globus_l_gfs_data_passive_kickout,
        bounce_info);
    if(result != GLOBUS_SUCCESS)
    {
        result = GlobusGFSErrorWrapFailed(
            "globus_callback_register_oneshot", result);
        globus_panic(NULL, result, "small malloc failure, no recovery");
    }
    return;
}

static
void
globus_l_gfs_data_active_kickout(
    void *                              user_arg)
{
    globus_l_gfs_data_active_bounce_t * bounce_info;
    globus_gfs_ipc_reply_t              reply;   

    bounce_info = (globus_l_gfs_data_active_bounce_t *) user_arg;

    bounce_info->handle->is_mine = GLOBUS_TRUE;

    memset(&reply, '\0', sizeof(globus_gfs_ipc_reply_t));

    reply.type = GLOBUS_GFS_OP_ACTIVE;
    reply.id = bounce_info->id;
    reply.result = GLOBUS_SUCCESS;
    reply.info.data.data_handle_id = globus_handle_table_insert(
        &bounce_info->handle->session_handle->data_table,
        bounce_info->handle,
        1);
    reply.info.data.bi_directional = bounce_info->bi_directional;

    /* as soon as we finish the data handle can be in play, set its
        state appropriately.  if not success then we never created a
        handle */
    if(bounce_info->result == GLOBUS_SUCCESS)
    {
        bounce_info->handle->state = GLOBUS_L_GFS_DATA_HANDLE_VALID;
    }
    else
    {
        globus_assert(bounce_info->handle == NULL);
    }

    if(bounce_info->callback != NULL)
    {
        bounce_info->callback(
            &reply,
            bounce_info->user_arg);        
    }
    else
    {
        globus_gfs_ipc_reply_finished(
            bounce_info->ipc_handle,
            &reply);
    }
  
    globus_free(bounce_info);
}

void
globus_i_gfs_data_request_active(
    globus_gfs_ipc_handle_t             ipc_handle,
    void *                              session_arg,
    int                                 id,
    globus_gfs_data_info_t *            data_info,
    globus_i_gfs_data_callback_t        cb,
    void *                              user_arg)
{
    globus_l_gfs_data_handle_t *        handle;
    globus_result_t                     result;
    globus_ftp_control_host_port_t *    addresses;
    int                                 i;
    globus_l_gfs_data_active_bounce_t * bounce_info;
    globus_l_gfs_data_operation_t *     op;
    globus_l_gfs_data_session_t *       session_handle;    
    GlobusGFSName(globus_i_gfs_data_request_active);

    session_handle = (globus_l_gfs_data_session_t *) session_arg;

    if(session_handle->dsi->active_func != NULL)
    {
        result = globus_l_gfs_data_operation_init(&op);
        if(result != GLOBUS_SUCCESS)
        {
            result = GlobusGFSErrorWrapFailed(
                "globus_l_gfs_data_operation_init", result);
            goto error_op;
        }
        
        op->ipc_handle = ipc_handle;
        op->id = id;
        op->state = GLOBUS_L_GFS_DATA_REQUESTING;
        op->callback = cb;
        op->user_arg = user_arg;
        op->session_handle = session_handle;
        op->info_struct = data_info;
        op->type = GLOBUS_L_GFS_DATA_INFO_TYPE_ACTIVE;
        if(session_handle->dsi->descriptor & GLOBUS_GFS_DSI_DESCRIPTOR_BLOCKING)
        {
            globus_callback_register_oneshot(
                NULL,
                NULL,
                globus_l_gfs_blocking_dispatch_kickout,
                op);
        }
        else
        {
            session_handle->dsi->active_func(
                op, data_info, session_handle->session_arg);
        }
    }
    else
    {
        if(data_info->del_cred == NULL)
        {
            data_info->del_cred = session_handle->del_cred;
        }
        result = globus_l_gfs_data_handle_init(&handle, data_info);
        if(result != GLOBUS_SUCCESS)
        {
            result = GlobusGFSErrorWrapFailed(
                "globus_l_gfs_data_handle_init", result);
            goto error_handle;
        }
        handle->session_handle = session_handle;
        addresses = (globus_ftp_control_host_port_t *)
            globus_malloc(sizeof(globus_ftp_control_host_port_t) * 
                data_info->cs_count);
        if(!addresses)
        {
            result = GlobusGFSErrorMemory("addresses");
            goto error_addresses;
        }
        
        for(i = 0; i < data_info->cs_count; i++)
        {
            result = globus_libc_contact_string_to_ints(
                data_info->contact_strings[i],
                addresses[i].host,  &addresses[i].hostlen, &addresses[i].port);
            if(result != GLOBUS_SUCCESS)
            {
                result = GlobusGFSErrorWrapFailed(
                    "globus_libc_contact_string_to_ints", result);
                goto error_format;
            }
        }
        
        if(data_info->cs_count == 1)
        {
            result = globus_ftp_control_local_port(
                &handle->data_channel, addresses);
        }
        else
        {
            result = globus_ftp_control_local_spor(
                &handle->data_channel, addresses, data_info->cs_count);
        }
        if(result != GLOBUS_SUCCESS)
        {
            result = GlobusGFSErrorWrapFailed(
                "globus_ftp_control_local_port/spor", result);
            goto error_control;
        }
        
        bounce_info = (globus_l_gfs_data_active_bounce_t *)
            globus_malloc(sizeof(globus_l_gfs_data_active_bounce_t));
        if(!bounce_info)
        {
            result = GlobusGFSErrorMemory("bounce_info");
            globus_panic(NULL, result, "small malloc failure, no recovery");
        }
        
        bounce_info->ipc_handle = ipc_handle;
        bounce_info->id = id;
        bounce_info->handle = handle;
        bounce_info->bi_directional = GLOBUS_TRUE; /* XXX MODE S only */
        bounce_info->callback = cb;
        bounce_info->user_arg = user_arg;
        bounce_info->result = GLOBUS_SUCCESS;
        
        session_handle->data_handle = handle;
        
        result = globus_callback_register_oneshot(
            NULL,
            NULL,
            globus_l_gfs_data_active_kickout,
            bounce_info);
        if(result != GLOBUS_SUCCESS)
        {
            result = GlobusGFSErrorWrapFailed(
                "globus_callback_register_oneshot", result);
            globus_panic(NULL, result, "small malloc failure, no recovery");
        }
        
        globus_free(addresses);
    }
    return;

error_control:
error_format:
    globus_free(addresses);
error_addresses:
    globus_ftp_control_handle_destroy(&handle->data_channel);
    globus_free(handle);
error_handle:
error_op:
    bounce_info = (globus_l_gfs_data_active_bounce_t *)
        globus_malloc(sizeof(globus_l_gfs_data_active_bounce_t));
    if(!bounce_info)
    {
        result = GlobusGFSErrorMemory("bounce_info");
        globus_panic(NULL, result, "small malloc failure, no recovery");
    }
    bounce_info->ipc_handle = ipc_handle;
    bounce_info->id = id;
    bounce_info->handle = handle;
    bounce_info->bi_directional = GLOBUS_TRUE; /* XXX MODE S only */
    bounce_info->callback = cb;
    bounce_info->user_arg = user_arg;
    bounce_info->result = result;
    bounce_info->handle = NULL;
    result = globus_callback_register_oneshot(
        NULL,
        NULL,
        globus_l_gfs_data_active_kickout,
        bounce_info);
    if(result != GLOBUS_SUCCESS)
    {
        result = GlobusGFSErrorWrapFailed(
            "globus_callback_register_oneshot", result);
        globus_panic(NULL, result, "small malloc failure, no recovery");
    }
    return;
}

    
void
globus_i_gfs_data_request_recv(
    globus_gfs_ipc_handle_t             ipc_handle,
    void *                              session_arg,
    int                                 id,
    globus_gfs_transfer_info_t *        recv_info,
    globus_i_gfs_data_callback_t        cb,
    globus_i_gfs_data_event_callback_t  event_cb,
    void *                              user_arg)
{
    globus_l_gfs_data_operation_t *     op;
    globus_result_t                     result;
    globus_l_gfs_data_handle_t *        data_handle;
    globus_l_gfs_data_session_t *       session_handle;    
    GlobusGFSName(globus_i_gfs_data_recv_request);

    session_handle = (globus_l_gfs_data_session_t *) session_arg;

    data_handle = (globus_l_gfs_data_handle_t *) globus_handle_table_lookup(
        &session_handle->data_table, recv_info->data_handle_id);
    if(data_handle == NULL)
    {
        result = GlobusGFSErrorData("Data handle not found");
        goto error_handle;
    }

    if(!data_handle->is_mine)
    {
        recv_info->data_handle_id = data_handle->remote_handle_id;
    }
    
    result = globus_l_gfs_data_operation_init(&op);
    if(result != GLOBUS_SUCCESS)
    {
        result = GlobusGFSErrorWrapFailed(
            "globus_l_gfs_data_operation_init", result);
        goto error_op;
    }

    op->ipc_handle = ipc_handle;    
    op->session_handle = session_handle;
    op->type = GLOBUS_L_GFS_DATA_INFO_TYPE_RECV;
    op->info_struct = recv_info;
    op->ref = 1;
    op->id = id;
    op->state = GLOBUS_L_GFS_DATA_REQUESTING;
    op->writing = GLOBUS_FALSE;
    op->data_handle = data_handle;
    op->data_handle_id = recv_info->data_handle_id;
    data_handle->op = op;
    op->range_list = recv_info->range_list;
    op->partial_offset = recv_info->partial_offset;
    op->callback = cb;
    op->event_callback = event_cb;
    op->user_arg = user_arg;
    op->node_ndx = recv_info->node_ndx;
    session_handle->node_ndx = recv_info->node_ndx;
    op->node_count = recv_info->node_count;    
    op->stripe_count = recv_info->stripe_count;
    /* events and disconnects cannot happen while i am in this
        function */
    globus_assert(data_handle->state == GLOBUS_L_GFS_DATA_HANDLE_VALID);
    data_handle->state = GLOBUS_L_GFS_DATA_HANDLE_INUSE;

    op->dsi = globus_l_gfs_data_new_dsi(session_handle, recv_info->module_name);
    if(op->dsi == NULL)
    {
        globus_gridftp_server_finished_transfer(
            op, GlobusGFSErrorGeneric("bad module"));
        goto error_module;
    }

    if(session_handle->dsi->descriptor & GLOBUS_GFS_DSI_DESCRIPTOR_BLOCKING)
    {
        globus_callback_register_oneshot(
            NULL,
            NULL,
            globus_l_gfs_blocking_dispatch_kickout,
            op);
    }
    else
    {
        op->dsi->recv_func(
            op, recv_info, session_handle->session_arg);
    }

    return;

error_module:
error_op:
error_handle:
    return;
}

    
void
globus_i_gfs_data_request_send(
    globus_gfs_ipc_handle_t             ipc_handle,
    void *                              session_arg,
    int                                 id,
    globus_gfs_transfer_info_t *        send_info,
    globus_i_gfs_data_callback_t        cb,
    globus_i_gfs_data_event_callback_t  event_cb,
    void *                              user_arg)   
{
    globus_l_gfs_data_operation_t *     op;
    globus_result_t                     result;
    globus_l_gfs_data_handle_t *        data_handle;
    globus_l_gfs_data_session_t *       session_handle;    
    GlobusGFSName(globus_i_gfs_data_send_request);

    session_handle = (globus_l_gfs_data_session_t *) session_arg;

    data_handle = (globus_l_gfs_data_handle_t *) globus_handle_table_lookup(
        &session_handle->data_table, send_info->data_handle_id);
    if(data_handle == NULL)
    {
        result = GlobusGFSErrorData("Data handle not found");
        goto error_handle;
    }
    if(!data_handle->is_mine)
    {
        send_info->data_handle_id = data_handle->remote_handle_id;
    }

    result = globus_l_gfs_data_operation_init(&op);
    if(result != GLOBUS_SUCCESS)
    {
        result = GlobusGFSErrorWrapFailed(
            "globus_l_gfs_data_operation_init", result);
        goto error_op;
    }
    op->ipc_handle = ipc_handle;
    op->session_handle = session_handle;
    op->type = GLOBUS_L_GFS_DATA_INFO_TYPE_SEND;
    op->info_struct = send_info;
    op->ref = 1;
    op->id = id;
    op->state = GLOBUS_L_GFS_DATA_REQUESTING;
    op->writing = GLOBUS_TRUE;
    op->data_handle = data_handle;
    op->data_handle_id = send_info->data_handle_id;
    data_handle->op = op;
    op->range_list = send_info->range_list;
    op->partial_length = send_info->partial_length;
    op->partial_offset = send_info->partial_offset;
    op->callback = cb;
    op->event_callback = event_cb;
    op->user_arg = user_arg;
    op->node_ndx = send_info->node_ndx;
    session_handle->node_ndx = send_info->node_ndx;
    op->write_stripe = 0;
    op->stripe_chunk = send_info->node_ndx;
    op->node_count = send_info->node_count;
    op->stripe_count = send_info->stripe_count;
    op->nstreams = send_info->nstreams;
    op->eof_count = (int *) globus_calloc(1, op->stripe_count * sizeof(int));

    /* events and disconnects cannot happen while i am in this
        function */
    globus_assert(data_handle->state == GLOBUS_L_GFS_DATA_HANDLE_VALID);
    data_handle->state = GLOBUS_L_GFS_DATA_HANDLE_INUSE;

    op->dsi = globus_l_gfs_data_new_dsi(session_handle, send_info->module_name);
    if(op->dsi == NULL)
    {
        globus_gridftp_server_finished_transfer(
            op, GlobusGFSErrorGeneric("bad module"));
        goto error_module;
    }

    if(session_handle->dsi->descriptor & GLOBUS_GFS_DSI_DESCRIPTOR_BLOCKING)
    {
        globus_callback_register_oneshot(
            NULL,
            NULL,
            globus_l_gfs_blocking_dispatch_kickout,
            op);
    }
    else
    {
        op->dsi->send_func(
            op, send_info, session_handle->session_arg);
    }

    return;
    
error_module:
error_op:
error_handle:
    return;
}

static
void
globus_l_gfs_data_list_write_cb(
    globus_gfs_operation_t   op,
    globus_result_t                     result,
    globus_byte_t *                     buffer,
    globus_size_t                       nbytes,
    void *                              user_arg)
{
    globus_gridftp_server_control_list_buffer_free(buffer);
    
    globus_gridftp_server_finished_transfer(op, result); 
}


static
void
globus_l_gfs_data_list_stat_cb(
    globus_gfs_data_reply_t *           reply,
    void *                              user_arg)
{
    GlobusGFSName(globus_l_gfs_data_list_stat_cb);
    globus_gfs_operation_t   op;
    globus_byte_t *                     list_buffer;
    globus_size_t                       buffer_len;
    globus_l_gfs_data_bounce_t *        bounce_info;
    globus_result_t                     result;
 
    op = (globus_gfs_operation_t) user_arg;
    bounce_info = (globus_l_gfs_data_bounce_t *) op->user_arg;

    if(reply->result != GLOBUS_SUCCESS)
    {
        result = reply->result;
        goto error;
    }
    
    result = globus_gridftp_server_control_list_buffer_alloc(
            op->list_type,
            op->uid,
            reply->info.stat.stat_array, 
            reply->info.stat.stat_count,
            &list_buffer,
            &buffer_len);    
    if(result != GLOBUS_SUCCESS)
    {
        result = GlobusGFSErrorWrapFailed(
           "globus_gridftp_server_control_list_buffer_alloc", result);
        goto error;
    }
    
    globus_gridftp_server_begin_transfer(op, 0, NULL);
    
    result = globus_gridftp_server_register_write(
        op,
        list_buffer,  
        buffer_len,
        0,
        -1,
        globus_l_gfs_data_list_write_cb,
        bounce_info);
    if(result != GLOBUS_SUCCESS)
    {
        result = GlobusGFSErrorWrapFailed(
            "globus_gridftp_server_register_write", result);
        goto error;
    }

    return;
    
error:
    globus_gridftp_server_finished_transfer(op, result); 
    return;
}

void
globus_i_gfs_data_request_list(
    globus_gfs_ipc_handle_t             ipc_handle,
    void *                              session_arg,
    int                                 id,
    globus_gfs_transfer_info_t *        list_info,
    globus_i_gfs_data_callback_t        cb,
    globus_i_gfs_data_event_callback_t  event_cb,
    void *                              user_arg)
{
    globus_l_gfs_data_operation_t *     stat_op;
    globus_l_gfs_data_operation_t *     data_op;
    globus_result_t                     result;
    globus_l_gfs_data_handle_t *        data_handle;
    globus_gfs_stat_info_t *            stat_info;
    globus_l_gfs_data_session_t *       session_handle;    
    GlobusGFSName(globus_i_gfs_data_list_request);

    session_handle = (globus_l_gfs_data_session_t *) session_arg;

    data_handle = (globus_l_gfs_data_handle_t *) globus_handle_table_lookup(
        &session_handle->data_table, list_info->data_handle_id);
    if(data_handle == NULL)
    {
        result = GlobusGFSErrorData("Data handle not found");
        goto error_handle;
    }
    if(!data_handle->is_mine)
    {
        list_info->data_handle_id = data_handle->remote_handle_id;
    }

    result = globus_l_gfs_data_operation_init(&data_op);
    if(result != GLOBUS_SUCCESS)
    {
        result = GlobusGFSErrorWrapFailed(
            "globus_l_gfs_data_operation_init", result);
        goto error_op;
    }

    data_op->ipc_handle = ipc_handle;    
    data_op->session_handle = session_handle;
    data_op->type = GLOBUS_L_GFS_DATA_INFO_TYPE_TRANSFER;
    data_op->info_struct = list_info;
    data_op->ref = 1;
    data_op->id = id;
    data_op->state = GLOBUS_L_GFS_DATA_REQUESTING;
    data_op->writing = GLOBUS_TRUE;
    data_op->data_handle = data_handle;
    data_op->data_handle_id = list_info->data_handle_id;
    data_handle->op = data_op;
    data_op->list_type = strdup(list_info->list_type);
    data_op->uid = getuid();
    /* XXX */
    data_op->callback = cb;
    data_op->event_callback = event_cb;
    data_op->user_arg = user_arg;
    data_op->node_ndx = list_info->node_ndx;
    data_op->write_stripe = 0;
    data_op->stripe_chunk = list_info->node_ndx;
    data_op->node_count = list_info->node_count;
    data_op->stripe_count = list_info->stripe_count;
    data_op->nstreams = list_info->nstreams;
    data_op->eof_count = (int *) 
        globus_calloc(1, data_op->stripe_count * sizeof(int));
    
    /* events and disconnects cannot happen while i am in this
        function */
    globus_assert(data_handle->state == GLOBUS_L_GFS_DATA_HANDLE_VALID);
    data_handle->state = GLOBUS_L_GFS_DATA_HANDLE_INUSE;

    if(session_handle->dsi->list_func != NULL)
    {
        if(session_handle->dsi->descriptor & GLOBUS_GFS_DSI_DESCRIPTOR_BLOCKING)
        { 
            globus_callback_register_oneshot(
                NULL,
                NULL,
                globus_l_gfs_blocking_dispatch_kickout,
                data_op);
        }
        else
        {
            session_handle->dsi->list_func(
                data_op, list_info, session_handle->session_arg);
        }
    }
    else
    {    
        result = globus_l_gfs_data_operation_init(&stat_op);
        if(result != GLOBUS_SUCCESS)
        {
            result = GlobusGFSErrorWrapFailed(
                "globus_l_gfs_data_operation_init", result);
            goto error_op;
        }
        stat_op->state = GLOBUS_L_GFS_DATA_REQUESTING;
        stat_op->callback = globus_l_gfs_data_list_stat_cb;
        stat_op->user_arg = data_op;
        stat_op->session_handle = session_handle;
        stat_op->type = GLOBUS_L_GFS_DATA_INFO_TYPE_STAT;

        stat_info = (globus_gfs_stat_info_t *) 
            globus_calloc(1, sizeof(globus_gfs_stat_info_t));
        
        stat_info->pathname = list_info->pathname;
        stat_info->file_only = GLOBUS_FALSE;

        stat_op->info = stat_info;
        stat_op->info_struct = stat_info;
     
        if(session_handle->dsi->descriptor & GLOBUS_GFS_DSI_DESCRIPTOR_BLOCKING)
        { 
            globus_callback_register_oneshot(
                NULL,
                NULL,
                globus_l_gfs_blocking_dispatch_kickout,
                stat_op);
        }
        else
        {
            session_handle->dsi->stat_func(
                stat_op, stat_info, session_handle->session_arg);
        }
    }
    
    return;

error_handle:
error_op:
    return;
}

/***********************************************************************
 *  finished transfer callbacks
 *  ---------------------------
 **********************************************************************/
static
void
globus_l_gfs_data_finish_connected(
    globus_l_gfs_data_operation_t *     op)
{
    globus_result_t                     result = GLOBUS_SUCCESS;
    if(op->data_handle->is_mine)
    {
        if(op->writing)
        {
            if(op->node_ndx != 0 || 
                op->stripe_count == 1 || 
                op->eof_ready)
            {
                result = globus_ftp_control_data_write(
                    &op->data_handle->data_channel,
                    "",
                    0,
                    0,
                    GLOBUS_TRUE,
                    globus_l_gfs_data_write_eof_cb,
                    op);
                if(result != GLOBUS_SUCCESS)
                {
                    globus_i_gfs_log_result(
                        "write_eof error", result);
                    op->cached_res = result;
                    globus_callback_register_oneshot(
                        NULL,
                        NULL,
                        globus_l_gfs_data_end_transfer_kickout,
                        op);
                }
            }
        }
        else
        {
            globus_callback_register_oneshot(
                NULL,
                NULL,
                globus_l_gfs_data_end_read_kickout,
                op);
        }
    }
    else
    {
        globus_callback_register_oneshot(
            NULL,
            NULL,
            globus_l_gfs_data_end_transfer_kickout,
            op);
    }
}                        


static
void
globus_l_gfs_data_begin_cb(
    void *                              callback_arg,
    struct globus_ftp_control_handle_s * handle,
    unsigned int                        stripe_ndx,
    globus_bool_t                       reused,
    globus_object_t *                   error)
{
    globus_bool_t                       destroy_op = GLOBUS_FALSE;
    globus_bool_t                       connect_event = GLOBUS_FALSE;
    globus_bool_t                       finish = GLOBUS_FALSE;
    globus_gfs_ipc_event_reply_t *      event_reply;
    globus_l_gfs_data_operation_t *     op;

    op = (globus_l_gfs_data_operation_t *) callback_arg;

    globus_mutex_lock(&op->session_handle->mutex);
    {
        switch(op->state)
        {
            case GLOBUS_L_GFS_DATA_CONNECTING:
                op->stripe_connections_pending--;
                globus_assert(op->ref > 0);

                if(error != NULL)
                {
                    /* something wrong, start the abort process */
                    op->cached_res = 
                        globus_error_put(globus_object_copy(error));
                    goto err_lock;
                }
                if(!op->stripe_connections_pending)
                {
                    op->ref--;
                    /* everything is well, send the begin event */
                    op->state = GLOBUS_L_GFS_DATA_CONNECTED;
                    connect_event = GLOBUS_TRUE;
                }
                break;
            /* this happens when a finished comes right after a begin,
                usually because 0 bytes were written.  we need to send
                the transfer_connected event and then finish. */
            case GLOBUS_L_GFS_DATA_FINISH:
                op->stripe_connections_pending--;
                if(!op->stripe_connections_pending)
                {
                    op->ref--;
                    /* everything is well, send the begin event */
                    connect_event = GLOBUS_TRUE;
                    finish = GLOBUS_TRUE;
                }

                globus_assert(op->ref > 0);
                    
                break;

            /* this happens when a transfer is aborted before a connection
                is esstablished.  it could be in this state
                depending on how quickly the abort process happens.  */
            case GLOBUS_L_GFS_DATA_ABORTING:
            case GLOBUS_L_GFS_DATA_ABORT_CLOSING:
                op->ref--;
                globus_assert(op->ref > 0);
                break;
                /* we need to dec the reference count and clean up if needed.
                also we ignore the error value here, it is likely canceled */
            case GLOBUS_L_GFS_DATA_COMPLETING:
                op->ref--;
                if(op->ref == 0)
                {
                    destroy_op = GLOBUS_TRUE;
                    op->state = GLOBUS_L_GFS_DATA_COMPLETE;
                }
                break;

            case GLOBUS_L_GFS_DATA_COMPLETE:
            case GLOBUS_L_GFS_DATA_CONNECTED:
            case GLOBUS_L_GFS_DATA_REQUESTING:
            default:
                globus_assert(0 && "not possible state.  memory corruption");
                break;
        }
    }
    globus_mutex_unlock(&op->session_handle->mutex);

    if(connect_event && op->data_handle->is_mine)
    {
        event_reply = (globus_gfs_ipc_event_reply_t *) 
            globus_calloc(1, sizeof(globus_gfs_ipc_event_reply_t));
        if(event_reply == NULL)
        {
            goto err;
        }
        event_reply->type = GLOBUS_GFS_EVENT_TRANSFER_CONNECTED;
        event_reply->id = op->id;
        event_reply->transfer_id = (int) op;
        if(op->event_callback != NULL)
        {
            op->event_callback(event_reply, op->user_arg);
        }
        else
        {
            globus_gfs_ipc_reply_event(op->ipc_handle, event_reply);
        }
        globus_free(event_reply);
    }
    if(finish)
    {
        globus_l_gfs_data_finish_connected(op);
    }        
    
    if(destroy_op)
    {
        /* pass the complete event */
        if(op->session_handle->dsi->trev_func &&
            op->event_mask & GLOBUS_GFS_EVENT_TRANSFER_COMPLETE)
        {
            op->session_handle->dsi->trev_func(
                op->transfer_id,
                GLOBUS_GFS_EVENT_TRANSFER_COMPLETE,
                op->session_handle->session_arg);
        }
        /* destroy the op */
        globus_l_gfs_data_operation_destroy(op);
    }
    return;

  err:
    globus_mutex_lock(&op->session_handle->mutex);

  err_lock:
    /* start abort process */
    globus_l_gfs_data_start_abort(op);
    globus_mutex_unlock(&op->session_handle->mutex);
}

static
void
globus_l_gfs_data_begin_kickout(
    void *                              callback_arg)
{
    globus_l_gfs_data_operation_t *     op;

    op = (globus_l_gfs_data_operation_t *) callback_arg;
    globus_l_gfs_data_begin_cb(
        callback_arg,
        &op->data_handle->data_channel,
        0,
        GLOBUS_TRUE,
        NULL);
}


static
void
globus_l_gfs_data_end_transfer_kickout(
    void *                              user_arg)
{
    globus_l_gfs_data_operation_t *     op;
    globus_gfs_ipc_event_reply_t        event_reply;
    globus_gfs_ipc_reply_t              reply;
    globus_bool_t                       destroy_op = GLOBUS_FALSE;
    globus_bool_t                       disconnect = GLOBUS_FALSE;

    op = (globus_l_gfs_data_operation_t *) user_arg;

    memset(&reply, '\0', sizeof(globus_gfs_ipc_reply_t));

    /* deal with the data handle */
    globus_mutex_lock(&op->session_handle->mutex);
    {
        globus_assert(op->data_handle != NULL);
        switch(op->data_handle->state)
        {
            case GLOBUS_L_GFS_DATA_HANDLE_INUSE:
                op->data_handle->state = GLOBUS_L_GFS_DATA_HANDLE_VALID;
                break;

            case GLOBUS_L_GFS_DATA_HANDLE_CLOSING:
            case GLOBUS_L_GFS_DATA_HANDLE_CLOSED:
                disconnect = GLOBUS_TRUE;
                break;

            case GLOBUS_L_GFS_DATA_HANDLE_CLOSING_AND_DESTROYED:
            case GLOBUS_L_GFS_DATA_HANDLE_VALID:
            default:
                globus_assert(0 && "possible memory corruption");
                break;
        }
    }
    globus_mutex_unlock(&op->session_handle->mutex);

    if(disconnect && op->data_handle->is_mine)
    {
        memset(&event_reply, '\0', sizeof(globus_gfs_ipc_event_reply_t));
        event_reply.id = op->id;
        event_reply.data_handle_id = op->data_handle_id;

        event_reply.type = GLOBUS_GFS_EVENT_DISCONNECTED;
        if(op->event_callback != NULL)
        {
            op->event_callback(
                &event_reply,
                op->user_arg);
        }
        else
        {
            globus_gfs_ipc_reply_event(
                op->ipc_handle,
                &event_reply);
        }
    }

    reply.type = GLOBUS_GFS_OP_TRANSFER;
    reply.id = op->id;
    reply.result = op->cached_res;
    
    globus_assert(!op->writing || 
        (op->sent_partial_eof == 1 || op->stripe_count == 1 ||
        (op->node_ndx == 0 && op->eof_ready))); 
    /* tell the control side the finished was called */
    if(op->callback != NULL)
    {
        op->callback(&reply, op->user_arg);
    }
    else
    {
        globus_gfs_ipc_reply_finished(
            op->ipc_handle,
            &reply);
    }

    /* remove the refrence for this callback.  It is posible the before
        aquireing this lock the completing state occured and we are
        ready to finish */
    globus_mutex_lock(&op->session_handle->mutex);
    {
        op->ref--;
        if(op->ref == 0)
        {
            destroy_op = GLOBUS_TRUE;
        }
    }
    globus_mutex_unlock(&op->session_handle->mutex);

    if(destroy_op)
    {
        /* pass the complete event */
        if(op->session_handle->dsi->trev_func &&
            op->event_mask & GLOBUS_GFS_EVENT_TRANSFER_COMPLETE)
        {
            op->session_handle->dsi->trev_func(
                op->transfer_id,
                GLOBUS_GFS_EVENT_TRANSFER_COMPLETE,
                op->session_handle->session_arg);
        }
        /* destroy the op */
        globus_l_gfs_data_operation_destroy(op);
    }
}

static
void
globus_l_gfs_data_end_read_kickout(
    void *                              user_arg)
{
    globus_result_t                     result;
    globus_bool_t                       end = GLOBUS_FALSE;
    globus_l_gfs_data_operation_t *     op;
    globus_gfs_ipc_event_reply_t        event_reply;

    op = (globus_l_gfs_data_operation_t *) user_arg;

    memset(&event_reply, '\0', sizeof(globus_gfs_ipc_event_reply_t));
    event_reply.id = op->id;
    event_reply.recvd_bytes = op->recvd_bytes;
    event_reply.recvd_ranges = op->recvd_ranges;
    event_reply.node_ndx = op->node_ndx;
    
    event_reply.type = GLOBUS_GFS_EVENT_BYTES_RECVD;
    if(op->event_callback != NULL)
    {
        op->event_callback(
            &event_reply,
            op->user_arg);
    }
    else
    {
        globus_gfs_ipc_reply_event(
            op->ipc_handle,
            &event_reply);
    }

    event_reply.type = GLOBUS_GFS_EVENT_RANGES_RECVD;
    if(op->event_callback != NULL)
    {
        op->event_callback(
            &event_reply,
            op->user_arg);
    }
    else
    {
        globus_gfs_ipc_reply_event(
            op->ipc_handle,
            &event_reply);
    }
    
    globus_mutex_lock(&op->session_handle->mutex);
    {
        switch(op->data_handle->state)
        {
            case GLOBUS_L_GFS_DATA_HANDLE_INUSE:
                /* if not mode e we need to close it */
                if(op->data_handle->info.mode != 'E')
                {
                    op->data_handle->state = GLOBUS_L_GFS_DATA_HANDLE_CLOSING;
                    result = globus_ftp_control_data_force_close(
                        &op->data_handle->data_channel,
                        globus_l_gfs_data_finish_fc_cb,
                        op);
                    if(result != GLOBUS_SUCCESS)
                    {
                        op->data_handle->state = GLOBUS_L_GFS_DATA_HANDLE_CLOSED;
                        end = GLOBUS_TRUE;
                    }
                }
                else
                {
                    end = GLOBUS_TRUE;
                }
                break;

            default:
                break;
        }
    }
    globus_mutex_unlock(&op->session_handle->mutex);
    if(end)
    {    
        globus_l_gfs_data_end_transfer_kickout(op);
    }
}

static
void
globus_l_gfs_data_cb_error(
    globus_l_gfs_data_handle_t *        data_handle)
{
    globus_l_gfs_data_operation_t *     op;

    op = data_handle->op;

    globus_mutex_lock(&op->session_handle->mutex);
    {
        switch(data_handle->state)
        {
            case GLOBUS_L_GFS_DATA_HANDLE_INUSE:
                data_handle->state = GLOBUS_L_GFS_DATA_HANDLE_CLOSED;
                /* XXX free it here ??? */
                break;

            case GLOBUS_L_GFS_DATA_HANDLE_CLOSING:
            case GLOBUS_L_GFS_DATA_HANDLE_CLOSING_AND_DESTROYED:
            case GLOBUS_L_GFS_DATA_HANDLE_CLOSED:
                break;

            case GLOBUS_L_GFS_DATA_HANDLE_VALID:
            default:
                globus_assert(0 && "possible memory corruption");
                break;
        }
    }
    globus_mutex_unlock(&op->session_handle->mutex);

}

static
void
globus_l_gfs_data_send_eof_cb(
    void *                              callback_arg,
    struct globus_ftp_control_handle_s * handle,
    globus_object_t *			error)
{
    globus_l_gfs_data_operation_t *     op;

    op = (globus_l_gfs_data_operation_t *) callback_arg;
    if(error != NULL)
    {
        /* XXX this should be thread safe see not in write_eof cb */
        globus_l_gfs_data_cb_error(op->data_handle);
        op->cached_res = globus_error_put(globus_object_copy(error));
    }
    globus_l_gfs_data_end_transfer_kickout(op);
}

static
void
globus_l_gfs_data_write_eof_cb(
    void *                              user_arg,
    globus_ftp_control_handle_t *       handle,
    globus_object_t *                   error,
    globus_byte_t *                     buffer,
    globus_size_t                       length,
    globus_off_t                        offset,
    globus_bool_t                       eof)
{    
    /* XXX mode s only */
    globus_bool_t                       end = GLOBUS_FALSE;
    globus_result_t                     result;  
    globus_l_gfs_data_operation_t *     op;
    GlobusGFSName(globus_l_gfs_data_write_eof_cb);
    
    op = (globus_l_gfs_data_operation_t *) user_arg;

    if(error != NULL)
    {
        /* XXX this should be thread safe since we only get this
            callback after a finsihed_transfer() from the user.  we 
            could still get events or disconnects, but the abort process
            does not touch the data_handle->state */    
        globus_i_gfs_log_result(
            "write_eof_cb error", result);
        globus_l_gfs_data_cb_error(op->data_handle);
        op->cached_res = globus_error_put(globus_object_copy(error));
        end = GLOBUS_TRUE;
    }
    else
    {
        globus_mutex_lock(&op->session_handle->mutex);
        {
            switch(op->data_handle->state)
            {
                case GLOBUS_L_GFS_DATA_HANDLE_INUSE:
                /* if not mode e we need to close it */
                if(op->data_handle->info.mode == 'E')
                {        
                    result = globus_ftp_control_data_send_eof(
                        &op->data_handle->data_channel,
                        op->eof_count,
                        op->stripe_count,
                        (op->node_ndx == 0 || op->stripe_count == 1) ? 
                            GLOBUS_TRUE : GLOBUS_FALSE,
                        globus_l_gfs_data_send_eof_cb,
                        op);
                    if(op->node_ndx != 0 && op->stripe_count > 1)
                    {
                       /* I think we want the eof event to kick off even
                        though we may have an error here since someone is 
                        expecting it.  The transfer should still error out
                        normally */    
                        globus_gfs_ipc_event_reply_t        event_reply;
                        memset(&event_reply, '\0', 
                            sizeof(globus_gfs_ipc_event_reply_t));
                        event_reply.id = op->id;
                        event_reply.eof_count = op->eof_count;
                        event_reply.type = GLOBUS_GFS_EVENT_PARTIAL_EOF_COUNT;
                        event_reply.node_count = op->node_count;
                        if(op->event_callback != NULL)
                        {
                                op->event_callback(
                                &event_reply,
                                op->user_arg);
                        }
                        else
                        {
                            globus_gfs_ipc_reply_event(
                                op->ipc_handle,
                                &event_reply);
                        }   
                        op->sent_partial_eof++;                     
                    }
                    if(result != GLOBUS_SUCCESS)
                    {
                        globus_i_gfs_log_result(
                            "ERROR", result);
                        op->cached_res = result;
                        globus_l_gfs_data_end_transfer_kickout(op);
                    }
                }
                else
                {
                    op->data_handle->state = GLOBUS_L_GFS_DATA_HANDLE_CLOSING;
                    result = globus_ftp_control_data_force_close(
                        &op->data_handle->data_channel,
                        globus_l_gfs_data_finish_fc_cb,
                        op);
                    if(result != GLOBUS_SUCCESS)
                    {
                        op->data_handle->state = GLOBUS_L_GFS_DATA_HANDLE_CLOSED;
                        end = GLOBUS_TRUE;
                    }
                }

                default:
                    break;
            }
        }
        globus_mutex_unlock(&op->session_handle->mutex);
    }

    if(end)
    {
        globus_l_gfs_data_end_transfer_kickout(op);
    }
}

static
void
globus_l_gfs_data_send_eof(
    globus_l_gfs_data_operation_t *     op)
{
    globus_result_t                     result;  
    GlobusGFSName(globus_l_gfs_data_send_eof);
    
    globus_mutex_lock(&op->session_handle->mutex);
    {
        switch(op->state)
        {
            case GLOBUS_L_GFS_DATA_FINISH:
                op->eof_ready = GLOBUS_TRUE;
                result = globus_ftp_control_data_write(
                    &op->data_handle->data_channel,
                    "",
                    0,
                    0,
                    GLOBUS_TRUE,
                    globus_l_gfs_data_write_eof_cb,
                    op);
                if(result != GLOBUS_SUCCESS)
                {
                    globus_i_gfs_log_result(
                        "send_eof error", result);
                    op->cached_res = result;
                    globus_callback_register_oneshot(
                        NULL,
                        NULL,
                        globus_l_gfs_data_end_transfer_kickout,
                        op);
                }
                break;
            case GLOBUS_L_GFS_DATA_CONNECTED:
                op->eof_ready = GLOBUS_TRUE;
                break;
            default:
                /* figure out what needs to happen in other states */
                break;
        }
    }
    globus_mutex_unlock(&op->session_handle->mutex);
}    

static
void
globus_l_gfs_data_write_cb(
    void *                              user_arg,
    globus_ftp_control_handle_t *       handle,
    globus_object_t *                   error,
    globus_byte_t *                     buffer,
    globus_size_t                       length,
    globus_off_t                        offset,
    globus_bool_t                       eof)
{
    globus_l_gfs_data_bounce_t *        bounce_info;
    GlobusGFSName(globus_l_gfs_data_write_cb);
    
    bounce_info = (globus_l_gfs_data_bounce_t *) user_arg;
    
    bounce_info->callback.write(
        bounce_info->op,
        error ? globus_error_put(globus_object_copy(error)) : GLOBUS_SUCCESS,
        buffer,
        length,
        bounce_info->user_arg);
        
    globus_free(bounce_info);
}

static
void
globus_l_gfs_data_read_cb(
    void *                              user_arg,
    globus_ftp_control_handle_t *       handle,
    globus_object_t *                   error,
    globus_byte_t *                     buffer,
    globus_size_t                       length,
    globus_off_t                        offset,
    globus_bool_t                       eof)
{
    globus_l_gfs_data_bounce_t *        bounce_info;
    GlobusGFSName(globus_l_gfs_data_read_cb);
    
    bounce_info = (globus_l_gfs_data_bounce_t *) user_arg;
    
    bounce_info->callback.read(
        bounce_info->op,
        error ? globus_error_put(globus_object_copy(error)) : GLOBUS_SUCCESS,
        buffer,
        length,
        offset,
        eof,
        bounce_info->user_arg);
    
    globus_free(bounce_info);
}

static
void
globus_l_gfs_data_trev_kickout(
    void *                              user_arg)
{
    globus_l_gfs_data_trev_bounce_t *   bounce_info;
    globus_gfs_ipc_event_reply_t *      event_reply;
    globus_bool_t                       pass;
    globus_bool_t                       destroy_op = GLOBUS_FALSE;
    GlobusGFSName(globus_l_gfs_data_trev_kickout);

    bounce_info = (globus_l_gfs_data_trev_bounce_t *) user_arg;
    event_reply = (globus_gfs_ipc_event_reply_t *) 
        globus_calloc(1, sizeof(globus_gfs_ipc_event_reply_t));
         
    event_reply->id = bounce_info->op->id;
    event_reply->node_ndx = bounce_info->op->node_ndx;
    globus_mutex_lock(&bounce_info->op->session_handle->mutex);
    {
        switch(bounce_info->op->state)
        {
            case GLOBUS_L_GFS_DATA_CONNECTING:
            case GLOBUS_L_GFS_DATA_CONNECTED:
            case GLOBUS_L_GFS_DATA_ABORTING:
            case GLOBUS_L_GFS_DATA_ABORT_CLOSING:
                pass = GLOBUS_TRUE;
                break;

            case GLOBUS_L_GFS_DATA_FINISH:
                pass = GLOBUS_FALSE;
                break;

            case GLOBUS_L_GFS_DATA_COMPLETING:
            case GLOBUS_L_GFS_DATA_COMPLETE:
            case GLOBUS_L_GFS_DATA_REQUESTING:
            default:
                globus_assert(0 && "possibly memory corruption");
                break;
        }    
        switch(bounce_info->event_type)
        {
            case GLOBUS_GFS_EVENT_BYTES_RECVD:
                event_reply->recvd_bytes = bounce_info->op->recvd_bytes;
                bounce_info->op->recvd_bytes = 0;
                event_reply->type = GLOBUS_GFS_EVENT_BYTES_RECVD;
                break;
            
            case GLOBUS_GFS_EVENT_RANGES_RECVD:
                event_reply->type = GLOBUS_GFS_EVENT_RANGES_RECVD;
                globus_range_list_init(&event_reply->recvd_ranges);
                globus_range_list_merge(
                    &event_reply->recvd_ranges,
                    bounce_info->op->recvd_ranges,
                    NULL);
                globus_range_list_remove(
                    bounce_info->op->recvd_ranges, 0, GLOBUS_RANGE_LIST_MAX);                
                break;
            
            default:
                globus_assert(0 && "invalid state, not possible");
                break;
        } 
    }
    globus_mutex_unlock(&bounce_info->op->session_handle->mutex);

    if(globus_i_gfs_config_bool("sync"))
    {
        sync();
    }
    
    if(pass)
    {
        if(bounce_info->op->event_callback != NULL)
        {
            bounce_info->op->event_callback(
                event_reply,
                bounce_info->op->user_arg);        
        }
        else
        {
            globus_gfs_ipc_reply_event(
                bounce_info->op->ipc_handle,
                event_reply);
        }
    }
    
    globus_mutex_lock(&bounce_info->op->session_handle->mutex);
    {    
        bounce_info->op->ref--;
        if(bounce_info->op->ref == 0)
        {
            destroy_op = GLOBUS_TRUE;
            globus_assert(
                bounce_info->op->state == GLOBUS_L_GFS_DATA_COMPLETING);
        }
    }
    globus_mutex_unlock(&bounce_info->op->session_handle->mutex);

    if(destroy_op)
    {
        /* pass the complete event */
        if(bounce_info->op->session_handle->dsi->trev_func &&
            bounce_info->op->event_mask & GLOBUS_GFS_EVENT_TRANSFER_COMPLETE)
        {
            bounce_info->op->session_handle->dsi->trev_func(
                bounce_info->op->transfer_id,
                GLOBUS_GFS_EVENT_TRANSFER_COMPLETE,
                bounce_info->op->session_handle->session_arg);
        }
        globus_l_gfs_data_operation_destroy(bounce_info->op);
    }

    if(event_reply->recvd_ranges)
    {
        globus_range_list_destroy(event_reply->recvd_ranges);     
    }
    globus_free(bounce_info);
    globus_free(event_reply);       
}

static
void
globus_l_gfs_data_force_close(
    globus_l_gfs_data_operation_t *     op)
{
    globus_result_t                     result;

    /* handle the data_handle state machine */
    if(op->data_handle->is_mine)
    {
        switch(op->data_handle->state)
        {
            case GLOBUS_L_GFS_DATA_HANDLE_INUSE:
                op->data_handle->state = GLOBUS_L_GFS_DATA_HANDLE_CLOSING;
                result = globus_ftp_control_data_force_close(
                    &op->data_handle->data_channel,
                    globus_l_gfs_data_finish_fc_cb,
                    op);
                if(result != GLOBUS_SUCCESS)
                {
                    op->data_handle->state = GLOBUS_L_GFS_DATA_HANDLE_CLOSED;
                    globus_i_gfs_log_result(
                        "force_close", result);
                    globus_callback_register_oneshot(
                        NULL,
                        NULL,
                        globus_l_gfs_data_end_transfer_kickout,
                        op);
                }
                break;

            /* already started closing the handle */
            case GLOBUS_L_GFS_DATA_HANDLE_CLOSED:
            case GLOBUS_L_GFS_DATA_HANDLE_CLOSING:
                break;

            case GLOBUS_L_GFS_DATA_HANDLE_CLOSING_AND_DESTROYED:
            case GLOBUS_L_GFS_DATA_HANDLE_VALID:
            default:
                globus_assert(0 && "only should be called when inuse");
                break;
        }
    }
    else
    {
        switch(op->data_handle->state)
        {
            case GLOBUS_L_GFS_DATA_HANDLE_INUSE:
                op->data_handle->state = GLOBUS_L_GFS_DATA_HANDLE_CLOSED;
                globus_callback_register_oneshot(
                    NULL,
                    NULL,
                    globus_l_gfs_data_end_transfer_kickout,
                    op);
                break;

            /* already started closing the handle */
            case GLOBUS_L_GFS_DATA_HANDLE_CLOSED:
            case GLOBUS_L_GFS_DATA_HANDLE_CLOSING:
                break;

            case GLOBUS_L_GFS_DATA_HANDLE_CLOSING_AND_DESTROYED:
            case GLOBUS_L_GFS_DATA_HANDLE_VALID:
            default:
                globus_assert(0 && "only should be called when inuse");
                break;
        }
    }        
}

/* must be called locked */
static
void
globus_l_gfs_data_start_abort(
    globus_l_gfs_data_operation_t *     op)
{
    globus_result_t                     result;

    switch(op->state)
    {
        case GLOBUS_L_GFS_DATA_REQUESTING:
            op->state = GLOBUS_L_GFS_DATA_ABORTING;
            break;

        case GLOBUS_L_GFS_DATA_CONNECTING:
        case GLOBUS_L_GFS_DATA_CONNECTED:
            if(op->data_handle->is_mine)
            {
                globus_assert(op->data_handle->state == 
                    GLOBUS_L_GFS_DATA_HANDLE_INUSE);
                op->data_handle->state = GLOBUS_L_GFS_DATA_HANDLE_CLOSING;
                result = globus_ftp_control_data_force_close(
                    &op->data_handle->data_channel,
                    globus_l_gfs_data_abort_fc_cb,
                    op);
                if(result != GLOBUS_SUCCESS)
                {
                    globus_callback_register_oneshot(
                        NULL,
                        NULL,
                        globus_l_gfs_data_abort_kickout,
                        op);
                    op->data_handle->state = GLOBUS_L_GFS_DATA_HANDLE_CLOSED;
                }
            }
            else
            {
                globus_callback_register_oneshot(
                    NULL,
                    NULL,
                    globus_l_gfs_data_abort_kickout,
                    op);
            }
            op->state = GLOBUS_L_GFS_DATA_ABORT_CLOSING;
            op->ref++;
            break;

        /* everything post finished can ignore abort, because dsi is already
            done and connections should be torn down, or in the process
            of tearing down */
        case GLOBUS_L_GFS_DATA_FINISH:
        case GLOBUS_L_GFS_DATA_COMPLETING:
        case GLOBUS_L_GFS_DATA_COMPLETE:
            break;

        case GLOBUS_L_GFS_DATA_ABORT_CLOSING:
        case GLOBUS_L_GFS_DATA_ABORTING:
            /* do nothing cause it has already been done */
            break;

        default:
            break;
    }

}

void
globus_i_gfs_data_request_transfer_event(
    globus_gfs_ipc_handle_t             ipc_handle,
    void *                              session_arg,
    int                                 transfer_id,
    globus_gfs_event_info_t *           event_info)
{
    globus_result_t                     result;
    globus_l_gfs_data_trev_bounce_t *   bounce_info;
    globus_l_gfs_data_session_t *       session_handle;
    globus_l_gfs_data_operation_t *     op;
    globus_bool_t                       destroy_op = GLOBUS_FALSE;
    globus_bool_t                       pass = GLOBUS_FALSE;
    GlobusGFSName(globus_i_gfs_data_kickoff_event);

    session_handle = (globus_l_gfs_data_session_t *) session_arg;
    op = (globus_l_gfs_data_operation_t *) transfer_id;

    if(op == NULL)
    {
        globus_assert(0 && "i wanna know when this happens");
    }
    globus_mutex_lock(&op->session_handle->mutex);
    {
        globus_assert(op->data_handle != NULL);

        /* this is the final event.  dec reference */
        switch(event_info->type)
        {
            /* if this event has been received we SHOULD be in complete state
                if we are not it is a bad message and we ignore it */
            case GLOBUS_GFS_EVENT_TRANSFER_COMPLETE:
                if(op->state == GLOBUS_L_GFS_DATA_FINISH)
                {
                    /* even tho we are passing do not up the ref because this
                        is the barrier message */
                    op->state = GLOBUS_L_GFS_DATA_COMPLETING;
                    pass = GLOBUS_TRUE;
                }
                else
                {
                    /* XXX log a bad message */
                    globus_assert(0 && "for now we assert");
                    pass = GLOBUS_FALSE;
                }
                break;

            case GLOBUS_GFS_EVENT_FINAL_EOF_COUNT:
                /* XXX check state here, move send_eof call out of lock */
                op->eof_count = event_info->eof_count;
                globus_l_gfs_data_send_eof(op);
                break;

            case GLOBUS_GFS_EVENT_BYTES_RECVD:
            case GLOBUS_GFS_EVENT_RANGES_RECVD:
                /* we ignore these 2 events for everything except the
                    connected state */
                /* if finished already happened ignore these completely */
                if(op->state != GLOBUS_L_GFS_DATA_CONNECTED)
                {
                    pass = GLOBUS_FALSE;
                }
                else
                {
                    /* if the DSI is handling these events */
                    if(session_handle->dsi->trev_func != NULL &&
                        event_info->type & op->event_mask)
                    {
                        op->ref++;
                        pass = GLOBUS_TRUE;
                    }
                    /* if DSI not handling, take care of for them */
                    else
                    {
                        pass = GLOBUS_FALSE;
                        /* since this will be put in a callback we must up 
                            ref */
                        op->ref++;

                        bounce_info = (globus_l_gfs_data_trev_bounce_t *)
                            globus_malloc(
                                sizeof(globus_l_gfs_data_trev_bounce_t));
                        if(!bounce_info)
                        {
                            result = GlobusGFSErrorMemory("bounce_info");
                        }
                
                        bounce_info->event_type = event_info->type;
                        bounce_info->op = 
                            (globus_l_gfs_data_operation_t *) transfer_id;
        
                        globus_callback_register_oneshot(
                            NULL,
                            NULL,
                            globus_l_gfs_data_trev_kickout,
                            bounce_info);
                    }
                }
                break;

            case GLOBUS_GFS_EVENT_TRANSFER_ABORT:
                /* start the abort process */
                globus_l_gfs_data_start_abort(op);
                break;
                
            /* only pass though if in connected state and the dsi wants
                the event */
            default:
                if(op->state != GLOBUS_L_GFS_DATA_CONNECTED ||
                    session_handle->dsi->trev_func == NULL ||
                    !(event_info->type & op->event_mask))
                {
                    pass = GLOBUS_FALSE;
                }
                else
                {
                    op->ref++;
                    pass = GLOBUS_TRUE;
                }
                break;
        }
    }
    globus_mutex_unlock(&op->session_handle->mutex);

    /* if is possible that events slip through here after setting to 
        GLOBUS_L_GFS_DATA_COMPLETE.  This is ok because the only
        gauretee made is that none will come after 
        GLOBUS_GFS_EVENT_TRANSFER_COMPLETE.  This is gaurenteed with
        the reference count. */
    if(pass)
    {
        /* if a TRANSFER_COMPLETE event we must respect the barrier */
        if(event_info->type != GLOBUS_GFS_EVENT_TRANSFER_COMPLETE)
        {
            session_handle->dsi->trev_func(
                op->transfer_id, 
                event_info->type, 
                session_handle->session_arg);
        }
        globus_mutex_lock(&op->session_handle->mutex);
        {
            op->ref--;
            if(op->ref == 0)
            {
                globus_assert(op->state == GLOBUS_L_GFS_DATA_COMPLETING);
                destroy_op = GLOBUS_TRUE;
            }
        }
        globus_mutex_unlock(&op->session_handle->mutex);
        if(destroy_op)
        {
            if(session_handle->dsi->trev_func &&
                op->event_mask & GLOBUS_GFS_EVENT_TRANSFER_COMPLETE)
            {
                session_handle->dsi->trev_func(
                    op->transfer_id,
                    GLOBUS_GFS_EVENT_TRANSFER_COMPLETE,
                    op->session_handle->session_arg);
            }
            /* destroy the op */
            globus_l_gfs_data_operation_destroy(op);
        }
    }

    return;
}

static
void
globus_l_gfs_data_ipc_error_cb(
    globus_gfs_ipc_handle_t             ipc_handle,
    globus_result_t                     result,
    void *                              user_arg)
{
    globus_i_gfs_log_result(
        "IPC ERROR", result);
    
    return;
}

static
void
globus_l_gfs_data_ipc_open_cb(
    globus_gfs_ipc_handle_t             ipc_handle,
    globus_result_t                     result,
    void *                              user_arg)
{
    globus_i_gfs_monitor_t *            monitor;

    monitor = (globus_i_gfs_monitor_t *) user_arg;

    globus_i_gfs_monitor_signal(monitor);
}


globus_result_t
globus_i_gfs_data_node_start(
    globus_xio_handle_t                 handle,
    globus_xio_system_handle_t          system_handle,
    const char *                        remote_contact,
    const char *                        local_contact)
{
    globus_result_t                     res;
    globus_i_gfs_monitor_t              monitor;

    globus_i_gfs_monitor_init(&monitor);
    
    res = globus_gfs_ipc_handle_create(
        &globus_gfs_ipc_default_iface,
        system_handle,
        globus_l_gfs_data_ipc_open_cb,
        &monitor,
        globus_l_gfs_data_ipc_error_cb,
        NULL);
    
    //globus_i_gfs_monitor_wait(&monitor);
    
    globus_l_gfs_data_is_remote_node = GLOBUS_TRUE;
    
    globus_i_gfs_monitor_destroy(&monitor);

    return res;
}

void
globus_i_gfs_data_session_start(
    globus_gfs_ipc_handle_t             ipc_handle,
    const gss_ctx_id_t                  context,
    globus_gfs_session_info_t *         session_info,
    globus_i_gfs_data_callback_t        cb,
    void *                              user_arg)
{
    globus_l_gfs_data_operation_t *     op;
    globus_result_t                     result;
    globus_l_gfs_data_session_t *       session_handle;    
    GlobusGFSName(globus_i_gfs_data_session_start);

    result = globus_l_gfs_data_operation_init(&op);
    if(result != GLOBUS_SUCCESS)
    {
        result = GlobusGFSErrorWrapFailed(
            "globus_i_gfs_data_session_start", result);
        globus_assert(0);
    }
    session_handle = (globus_l_gfs_data_session_t *)
        globus_calloc(1, sizeof(globus_l_gfs_data_session_t));
    if(session_handle == NULL)
    {
        /* XXX deal with this */
    }
    session_handle->dsi = globus_l_gfs_dsi;
    globus_handle_table_init(&session_handle->data_table, NULL);

    globus_mutex_init(&session_handle->mutex, NULL);
    session_handle->ref = 1;
    op->session_handle = session_handle;
    op->ipc_handle = ipc_handle;
    op->uid = getuid();
    op->state = GLOBUS_L_GFS_DATA_REQUESTING;
    op->callback = cb;
    op->user_arg = user_arg;
    op->info_struct = session_info;

    if(globus_i_gfs_config_bool("node_authorizes"))
    {
        globus_l_gfs_data_authorize(op, context, session_info);
    }
    else
    {
        globus_l_gfs_data_auth_init_cb(NULL, op, GLOBUS_SUCCESS);
    }
}

void
globus_i_gfs_data_session_stop(
    globus_gfs_ipc_handle_t             ipc_handle,
    void *                              session_arg)
{
    globus_bool_t                       free_session = GLOBUS_FALSE;
    globus_l_gfs_data_session_t *       session_handle;    
    GlobusGFSName(globus_i_gfs_data_session_stop);

    session_handle = (globus_l_gfs_data_session_t *) session_arg;
    if(session_handle != NULL)
    {
        if(session_handle->dsi->destroy_func != NULL)
        {
            session_handle->dsi->destroy_func(session_handle->session_arg);
        }
        globus_mutex_lock(&session_handle->mutex);
        {
            session_handle->ref--;
            /* can't jsut free bcause we may be waiting on a force close */
            if(session_handle->ref == 0)
            {
                free_session = GLOBUS_TRUE;
            }
        }
        globus_mutex_unlock(&session_handle->mutex);

        if(free_session)
        {
            if(session_handle->dsi != globus_l_gfs_dsi)
            {
                globus_extension_release(session_handle->dsi_handle);
            }
            globus_free(session_handle);
        }
    }
}



/************************************************************************
 * 
 * Public functions
 * 
 ***********************************************************************/

void
globus_gridftp_server_finished_command(
    globus_gfs_operation_t              op,
    globus_result_t                     result,
    char *                              command_data)
{
    globus_gfs_ipc_reply_t              reply;   
    GlobusGFSName(globus_gridftp_server_finished_command);

    /* XXX gotta do a oneshot */
    switch(op->command)
    {
      case GLOBUS_GFS_CMD_CKSM:
        op->cksm_response = globus_libc_strdup(command_data);
        break;      
      case GLOBUS_GFS_CMD_MKD:
      case GLOBUS_GFS_CMD_RMD:
      case GLOBUS_GFS_CMD_DELE:
      case GLOBUS_GFS_CMD_RNTO:
      case GLOBUS_GFS_CMD_SITE_CHMOD:
      default:
        break;      
    }

    memset(&reply, '\0', sizeof(globus_gfs_ipc_reply_t));
    reply.type = GLOBUS_GFS_OP_COMMAND;
    reply.id = op->id;
    reply.result = result;
    reply.info.command.command = op->command;
    reply.info.command.checksum = op->cksm_response;
    reply.info.command.created_dir = op->pathname;

    if(op->callback != NULL)
    {
        op->callback(
            &reply,
            op->user_arg);        
    }
    else
    {
        globus_gfs_ipc_reply_finished(
            op->ipc_handle,
            &reply);
    }

    globus_l_gfs_data_operation_destroy(op);
    
    return;
}

void
globus_gridftp_server_finished_stat(
    globus_gfs_operation_t              op,
    globus_result_t                     result,
    globus_gfs_stat_t *                 stat_array,
    int                                 stat_count)
{
    globus_l_gfs_data_stat_bounce_t *   bounce_info;
    globus_gfs_stat_t *                 stat_copy;
    GlobusGFSName(globus_gridftp_server_finished_stat);

    globus_free(op->info);
    if(result == GLOBUS_SUCCESS)
    {
        stat_copy = (globus_gfs_stat_t *)
            globus_malloc(sizeof(globus_gfs_stat_t) * stat_count);
        if(stat_copy == NULL)
        {
            result = GlobusGFSErrorMemory("stat_copy");
            goto error_alloc;
        }
        memcpy(
            stat_copy,
            stat_array,
            sizeof(globus_gfs_stat_t) * stat_count);
    }
    else
    {
        stat_copy = NULL;
        stat_count = 0;
    }

    bounce_info = (globus_l_gfs_data_stat_bounce_t *)
        globus_malloc(sizeof(globus_l_gfs_data_stat_bounce_t));
    if(bounce_info == NULL)
    {
        result = GlobusGFSErrorMemory("bounce_info");
        goto error_alloc;
    }
        
    bounce_info->op = op;
    bounce_info->error = result == GLOBUS_SUCCESS 
        ? GLOBUS_NULL : globus_error_get(result);
    bounce_info->stat_count = stat_count;
    bounce_info->stat_array = stat_copy;
    result = globus_callback_register_oneshot(
        GLOBUS_NULL,
        GLOBUS_NULL,
        globus_l_gfs_data_stat_kickout,
        bounce_info);
    if(result != GLOBUS_SUCCESS)
    {
        result = GlobusGFSErrorWrapFailed(
            "globus_callback_register_oneshot", result);
        goto error_oneshot;
    }

    return;

error_oneshot:
error_alloc:
    globus_panic(
        GLOBUS_NULL,
        result,
        "[%s:%d] Unrecoverable error",
        _gfs_name,
        __LINE__);
    return;
}

globus_result_t
globus_gridftp_server_begin_transfer(
    globus_gfs_operation_t              op,
    int                                 event_mask,
    void *                              event_arg)
{
    globus_bool_t                       pass_abort = GLOBUS_FALSE;
    globus_bool_t                       destroy_op = GLOBUS_FALSE;
    globus_result_t                     res;
    globus_result_t                     result;
    globus_gfs_ipc_event_reply_t *      event_reply;
    GlobusGFSName(globus_gridftp_server_begin_transfer);
    
    op->event_mask = event_mask;
    op->transfer_id = (int) event_arg;

    event_reply = (globus_gfs_ipc_event_reply_t *)
        globus_calloc(1, sizeof(globus_gfs_ipc_event_reply_t));
    if(event_reply == NULL)
    {
        res = GlobusGFSErrorMemory("event_reply");
        goto err;
    }

    /* increase refrence count for the events.  This gets decreased when
        the COMPLETE event occurs.  it is safe to increment outside of a
        lock because until we enable events there should be no
        contention
       increase the reference count a second time for this function.
       It is possible that after enabling events but before getting the lock
        that we: 1) get an abort, 2) get a finished() from dsi, 
        3) get a complete, 4) free the op.  if this happens there will
        be no memory at op->mutex. we get around this with an extra
        reference count */
    op->ref += 2; 
    event_reply->type = GLOBUS_GFS_EVENT_TRANSFER_BEGIN;
    event_reply->id = op->id;
    event_reply->transfer_id = (int) op;
    if(op->event_callback != NULL)
    {
        op->event_callback(event_reply, op->user_arg);
    }
    else
    {
        globus_gfs_ipc_reply_event(op->ipc_handle, event_reply);
    }
    globus_free(event_reply);
    /* at this point events can happen that change the state before
        the lock is aquired */

    globus_mutex_lock(&op->session_handle->mutex);
    {
        switch(op->state)
        {
            /* if going according to plan */
            case GLOBUS_L_GFS_DATA_REQUESTING:
                op->state = GLOBUS_L_GFS_DATA_CONNECTING;
                if(op->data_handle->is_mine)
                {
                    if(op->writing)
                    {
                        result = globus_ftp_control_data_connect_write(
                            &op->data_handle->data_channel,
                            globus_l_gfs_data_begin_cb,
                            op);
                    }
                    else
                    {
                        result = globus_ftp_control_data_connect_read(
                            &op->data_handle->data_channel,
                            globus_l_gfs_data_begin_cb,
                            op);
                    }
                }
                else
                {
                    /* rarre case where we are willing to check return code
                        from oneshot, however, if it ever fail many many 
                        other things are likely to break frist */
                    result = globus_callback_register_oneshot(
                        NULL,
                        NULL,
                        globus_l_gfs_data_begin_kickout,
                        op);
                }
                if(result != GLOBUS_SUCCESS)
                {
                    op->state = GLOBUS_L_GFS_DATA_ABORTING;
                    /* if the connects fail tell the dsi to abort */
                    op->cached_res = result;
                    if(op->session_handle->dsi->trev_func != NULL &&
                        op->event_mask & GLOBUS_GFS_EVENT_TRANSFER_ABORT &&
                        !op->data_handle->is_mine)
                    {
                        pass_abort = GLOBUS_TRUE;
                        op->ref++;
                    }
                }
                else
                {
                    /* for the begin callback on success */
                    if(op->writing && op->data_handle->is_mine)
                    {
                        op->ref++;
                        op->stripe_connections_pending = 
                            op->stripe_count;
                    }
                    else
                    {
                        op->ref++;
                        op->stripe_connections_pending = 1;                        
                    }
                }
                break;

            /* if in this state we have delayed the pass to the dsi until
                after we know they have requested events */
            case GLOBUS_L_GFS_DATA_ABORTING:
                if(op->session_handle->dsi->trev_func != NULL &&
                    op->event_mask & GLOBUS_GFS_EVENT_TRANSFER_ABORT &&
                    !op->data_handle->is_mine)
                {
                    pass_abort = GLOBUS_TRUE;
                    op->ref++;
                }
                break;

            /* we are waiting for the force close callback to return */
            case GLOBUS_L_GFS_DATA_ABORT_CLOSING:
                break;

            /* nothing to do here, finishing is in the works */
            case GLOBUS_L_GFS_DATA_FINISH:
                break;

            /* if this happens we went through all the step in the above 
                doc box. */
            case GLOBUS_L_GFS_DATA_COMPLETING:
                break;

            /* the reference counting should make htis not possible */
            case GLOBUS_L_GFS_DATA_COMPLETE:
                globus_assert(0 && 
                    "reference counts are likely messed up");
                break;

            /* this could only happen if the dsi did something bad, like
                maybe call this function twice? */
            case GLOBUS_L_GFS_DATA_CONNECTING:
            case GLOBUS_L_GFS_DATA_CONNECTED:
                globus_assert(0 && 
                    "In connecting state before it should be possible");
                break;
            default:
                globus_assert(0 && "this should not be possible");
                break;
        }

        op->ref--;
        if(op->ref == 0)
        {
            globus_assert(op->state == GLOBUS_L_GFS_DATA_COMPLETING);
            destroy_op = GLOBUS_TRUE;
        }
    }
    globus_mutex_unlock(&op->session_handle->mutex);

    if(pass_abort)
    {
        op->session_handle->dsi->trev_func(
            op->transfer_id,
            GLOBUS_GFS_EVENT_TRANSFER_ABORT,
            op->session_handle->session_arg);
        globus_mutex_lock(&op->session_handle->mutex);
        {
            op->ref--;
            if(op->ref == 0)
            {
                globus_assert(op->state == GLOBUS_L_GFS_DATA_COMPLETING);
                destroy_op = GLOBUS_TRUE;
            }
        }
        globus_mutex_unlock(&op->session_handle->mutex);
    }

    if(destroy_op)
    {
        if(op->session_handle->dsi->trev_func &&
            op->event_mask & GLOBUS_GFS_EVENT_TRANSFER_COMPLETE)
        {
            /* XXX does this call need to be in a oneshot? */
            op->session_handle->dsi->trev_func(
                op->transfer_id,
                GLOBUS_GFS_EVENT_TRANSFER_COMPLETE,
                op->session_handle->session_arg);
        }
        /* destroy the op */
        globus_l_gfs_data_operation_destroy(op);
    }

    return GLOBUS_SUCCESS;

err:
    return result;
}

void
globus_gridftp_server_finished_transfer(
    globus_gfs_operation_t              op,
    globus_result_t                     result)
{
    GlobusGFSName(globus_gridftp_server_finished_transfer);

    globus_mutex_lock(&op->session_handle->mutex);
    {
        /* move the data_handle state to VALID.  at first error if will
            be moved to invalid */
        switch(op->state)
        {
            /* this is the normal case */
            case GLOBUS_L_GFS_DATA_CONNECTED:
                if(result != GLOBUS_SUCCESS)
                {
                    op->cached_res = result;
                    globus_l_gfs_data_force_close(op);
                    goto err_close;
                }
                globus_l_gfs_data_finish_connected(op);                  
                break;

            /* finishing in connecting state with no error likely means 
                a zero byte transfer.  the finish will be kicked off in
                the connect_cb when it comes, here we just let it fall
                through and change state to finished
                XXX think we need another state here, what if we get
                an abort while waiting for connect_cb but we are finished */
            case GLOBUS_L_GFS_DATA_CONNECTING:
                if(result != GLOBUS_SUCCESS)
                {
                    op->cached_res = result;
                    globus_l_gfs_data_force_close(op);
                    goto err_close;
                }
                break;
            case GLOBUS_L_GFS_DATA_REQUESTING:
            case GLOBUS_L_GFS_DATA_ABORTING:
                if(result != GLOBUS_SUCCESS)
                {
                    op->cached_res = result;
                    globus_l_gfs_data_force_close(op);
                    goto err_close;
                }
                globus_callback_register_oneshot(
                    NULL,
                    NULL,
                    globus_l_gfs_data_end_transfer_kickout,
                    op);
                break;

            /* waiting for a force close callback to return.  will switch
                to the finished state, when the force close callback comes
                back it will continue the finish process */
            case GLOBUS_L_GFS_DATA_ABORT_CLOSING:
                break;

            case GLOBUS_L_GFS_DATA_COMPLETING:
            case GLOBUS_L_GFS_DATA_COMPLETE:
            case GLOBUS_L_GFS_DATA_FINISH:
            default:
                globus_assert(0 && "Invalid state");
                break;
        }
        op->state = GLOBUS_L_GFS_DATA_FINISH;
    }
    globus_mutex_unlock(&op->session_handle->mutex);

    return;

err_close:
    op->state = GLOBUS_L_GFS_DATA_FINISH;
    globus_mutex_unlock(&op->session_handle->mutex);
}

static
void
globus_l_gfs_operation_finished_kickout(
    void *                              user_arg)
{
    globus_l_gfs_data_bounce_t *        bounce;
    globus_l_gfs_data_operation_t *     op;

    bounce = (globus_l_gfs_data_bounce_t *) user_arg;
    op = bounce->op;

    if(op->callback != NULL)
    {
        op->callback(
            bounce->finished_info,
            op->user_arg);        
    }
    else
    {
        if(bounce->finished_info->type == GLOBUS_GFS_OP_SESSION_START)
        {
            globus_gfs_ipc_reply_session(
                op->ipc_handle,
                bounce->finished_info->result,
                bounce->finished_info->session_arg);
        }
        else
        {
            globus_gfs_ipc_reply_finished(
                op->ipc_handle,
                bounce->finished_info);
        }
    }
    globus_l_gfs_data_operation_destroy(op);
   
    globus_free(bounce);
}

void
globus_gridftp_server_operation_finished(
    globus_gfs_operation_t              op,
    globus_result_t                     result,
    globus_gfs_finished_info_t *        finished_info)
{
    globus_l_gfs_data_bounce_t *        bounce;
    globus_l_gfs_data_handle_t *        data_handle;
    globus_bool_t                       kickout = GLOBUS_TRUE;
    GlobusGFSName(globus_gridftp_server_operation_finished);

    bounce = (globus_l_gfs_data_bounce_t *)
        globus_malloc(sizeof(globus_l_gfs_data_bounce_t));
    if(bounce == NULL)
    {
        globus_panic(NULL, result, "small malloc failure, no recovery");
    }
    bounce->op = op;
    bounce->finished_info = finished_info;

    finished_info->id = op->id;
    finished_info->result = result;

    switch(finished_info->type)
    {
        case GLOBUS_GFS_OP_TRANSFER:
            globus_gridftp_server_finished_transfer(
                op, finished_info->result);
            kickout = GLOBUS_FALSE;
            break;

        case GLOBUS_GFS_OP_SESSION_START:
            if(op->session_handle)
            {
                op->session_handle->session_arg = 
                    (void *) finished_info->session_arg;
            }
            finished_info->session_arg = op->session_handle;
            break;

        case GLOBUS_GFS_OP_PASSIVE:
        case GLOBUS_GFS_OP_ACTIVE:
            data_handle = (globus_l_gfs_data_handle_t *) 
                globus_calloc(1, sizeof(globus_l_gfs_data_handle_t));
            if(data_handle == NULL)
            {
                globus_panic(NULL, result, "small malloc failure, no recovery");
            }
            data_handle->session_handle = op->session_handle;
            data_handle->remote_handle_id = 
                (int) finished_info->info.data.data_handle_id;
            data_handle->is_mine = GLOBUS_FALSE;
            data_handle->state = GLOBUS_L_GFS_DATA_HANDLE_VALID;
            finished_info->info.data.data_handle_id = 
                globus_handle_table_insert(
                    &data_handle->session_handle->data_table,
                    data_handle,
                    1);
            break;

        default:
            break;
    }
    if(kickout)
    {
        globus_l_gfs_operation_finished_kickout(bounce);
    }
    else
    {
        globus_free(bounce);
    }
/*
    result = globus_callback_register_oneshot(
        NULL,
        NULL,
        globus_l_gfs_operation_finished_kickout,
        bounce);
    if(result != GLOBUS_SUCCESS)
    {
        result = GlobusGFSErrorWrapFailed(
            "globus_callback_register_oneshot", result);
        globus_panic(NULL, result, "small malloc failure, no recovery");
    }
*/
    return;  
}
    
void
globus_gridftp_server_operation_event(
    globus_gfs_operation_t              op,
    globus_result_t                     result,
    globus_gfs_event_info_t *           event_info)
{
    event_info->id = op->id;

    /* XXX gotta do a onesot here ?? */
    switch(event_info->type)
    {
        case GLOBUS_GFS_EVENT_TRANSFER_BEGIN:
            globus_gridftp_server_begin_transfer(
                op, event_info->event_mask, (void *)event_info->transfer_id);
            break;
        default:
            if(op->event_callback != NULL)
            {
                op->event_callback(
                    event_info,
                    op->user_arg);        
            }
            else
            {
                globus_gfs_ipc_reply_event(
                    op->ipc_handle,
                    event_info);
            }
            break;
    }        

    return;
}

void
globus_gridftp_server_update_bytes_written(
    globus_gfs_operation_t              op,
    globus_off_t                        offset,
    globus_off_t                        length)
{
    GlobusGFSName(globus_gridftp_server_update_bytes_written);

    globus_mutex_lock(&op->session_handle->mutex);
    {
        op->recvd_bytes += length;
        globus_range_list_insert(op->recvd_ranges, offset, length);
    }
    globus_mutex_unlock(&op->session_handle->mutex);

    return;
}

void
globus_gridftp_server_get_optimal_concurrency(
    globus_gfs_operation_t              op,
    int *                               count)
{
    GlobusGFSName(globus_gridftp_server_get_optimal_concurrency);

    if(!op->writing)
    {
        /* query number of currently connected streams and update
            data_info (only if recieving) */
        globus_ftp_control_data_query_channels(
            &op->data_handle->data_channel,
            &op->data_handle->info.nstreams,
            0);
            
        if(op->data_handle->info.nstreams == 0)
        {
            op->data_handle->info.nstreams = 1;
        }
    }
    
    *count = op->data_handle->info.nstreams * op->stripe_count * 2;
}

void
globus_gridftp_server_get_block_size(
    globus_gfs_operation_t              op,
    globus_size_t *                     block_size)
{
    GlobusGFSName(globus_gridftp_server_get_block_size);
    
    *block_size = op->data_handle->info.blocksize;
}


/* this is used to translate the restart and partial offset/lengths into
    a sets of ranges to transfer... storage interface shouldn't know about
    partial or restart semantics, it only needs to know which offsets to 
    read from the data source, and what offset to write to data sink
    (dest offset only matters for mode e, but again, storage interface 
    doesn't know about modes)
*/
void
globus_gridftp_server_get_write_range(
    globus_gfs_operation_t   op,
    globus_off_t *                      offset,
    globus_off_t *                      length,
    globus_off_t *                      write_delta,
    globus_off_t *                      transfer_delta)
{
    GlobusGFSName(globus_gridftp_server_get_write_range);
    globus_off_t                        tmp_off = 0;
    globus_off_t                        tmp_len = -1;
    globus_off_t                        tmp_write = 0;
    globus_off_t                        tmp_transfer = 0;
    int                                 rc;

    if(globus_range_list_size(op->range_list))
    {
        rc = globus_range_list_remove_at(
            op->range_list,
            0,
            &tmp_off,
            &tmp_len);
    }
    if(op->data_handle->info.mode == 'S')
    {
        tmp_write = tmp_off;
    }
    if(op->partial_offset > 0)
    {
        tmp_off += op->partial_offset;
        tmp_write += op->partial_offset;
        tmp_transfer = 0 - op->partial_offset;
    }
    if(offset)
    {
        *offset = tmp_off;
    }
    if(length)
    {
        *length = tmp_len;
    }
    if(write_delta)
    {
        *write_delta = tmp_write;
    }
    if(transfer_delta)
    {
        *transfer_delta = tmp_transfer;
    }
    return;
}

void
globus_gridftp_server_get_read_range(
    globus_gfs_operation_t              op,
    globus_off_t *                      offset,
    globus_off_t *                      length,
    globus_off_t *                      write_delta)
{
    globus_off_t                        tmp_off = 0;
    globus_off_t                        tmp_len = -1;
    globus_off_t                        tmp_write = 0;
    int                                 rc;
    globus_off_t                        start_offset;
    globus_off_t                        end_offset;
    globus_off_t                        stripe_block_size;
    int                                 size;
    int                                 i;
    GlobusGFSName(globus_gridftp_server_get_read_range);
    /* this whole function is crazy ugly and inneffiecient...
       needs rethinking */
    globus_mutex_lock(&op->session_handle->mutex);
    {
        if(op->node_count > 1)
        {
            stripe_block_size = op->data_handle->info.stripe_blocksize;
            start_offset = op->stripe_chunk * stripe_block_size;
            end_offset = start_offset + stripe_block_size;
                
            if(globus_range_list_size(op->stripe_range_list))
            {
                rc = globus_range_list_remove_at(
                    op->stripe_range_list,
                    0,
                    &tmp_off,
                    &tmp_len);
            
                tmp_write = op->write_delta;
            }
            else if((size = globus_range_list_size(op->range_list)) != 0)
            {
                for(i = 0; i < size; i++)
                {
                    rc = globus_range_list_at(
                        op->range_list,
                        i,
                        &tmp_off,
                        &tmp_len);
            
                    if(op->partial_length != -1)
                    {
                        if(tmp_len == -1)
                        {
                            tmp_len = op->partial_length;
                        }
                        if(tmp_off + tmp_len > op->partial_length)
                        {
                            tmp_len = op->partial_length - tmp_off;
                            if(tmp_len < 0)
                            {
                                tmp_len = 0;
                            }
                        }
                    }
                    
                    if(op->partial_offset > 0)
                    {
                        tmp_off += op->partial_offset;
                        tmp_write = 0 - op->partial_offset;
                    }
                    
                    globus_range_list_insert(
                        op->stripe_range_list, tmp_off, tmp_len);
                    op->write_delta = tmp_write;
                }
                globus_range_list_remove(
                    op->stripe_range_list, 0, start_offset);
                globus_range_list_remove(
                    op->stripe_range_list, end_offset, GLOBUS_RANGE_LIST_MAX);
                op->stripe_chunk += op->node_count;
                
                if(globus_range_list_size(op->stripe_range_list))
                {
                    rc = globus_range_list_remove_at(
                        op->stripe_range_list,
                        0,
                        &tmp_off,
                        &tmp_len);
                
                    tmp_write = op->write_delta;
                }
                else
                {
                    tmp_len = 0;
                    tmp_off = 0;
                    tmp_write = 0;
                }
            }
            else
            {
                tmp_len = 0;
            }
        }
        else if(globus_range_list_size(op->range_list))
        {
            rc = globus_range_list_remove_at(
                op->range_list,
                0,
                &tmp_off,
                &tmp_len);
            
            if(op->partial_length != -1)
            {
                if(tmp_len == -1)
                {
                    tmp_len = op->partial_length;
                }
                if(tmp_off + tmp_len > op->partial_length)
                {
                    tmp_len = op->partial_length - tmp_off;
                    if(tmp_len < 0)
                    {
                        tmp_len = 0;
                    }
                }
            }
            
            if(op->partial_offset > 0)
            {
                tmp_off += op->partial_offset;
                tmp_write = 0 - op->partial_offset;
            }
        }
        else
        {
            tmp_len = 0;
        }
            
    }
    globus_mutex_unlock(&op->session_handle->mutex);
    if(offset)
    {
        *offset = tmp_off;
    }
    if(length)
    {
        *length = tmp_len;
    }
    if(write_delta)
    {
        *write_delta = tmp_write;
    }
    
    return; 
}

globus_result_t
globus_gridftp_server_register_read(
    globus_gfs_operation_t   op,
    globus_byte_t *                     buffer,
    globus_size_t                       length,
    globus_gridftp_server_read_cb_t     callback,  
    void *                              user_arg)
{
    globus_result_t                     result;
    globus_l_gfs_data_bounce_t *        bounce_info;
    GlobusGFSName(globus_gridftp_server_register_read);
    
    bounce_info = (globus_l_gfs_data_bounce_t *)
        globus_malloc(sizeof(globus_l_gfs_data_bounce_t));
    if(!bounce_info)
    {
        result = GlobusGFSErrorMemory("bounce_info");
        goto error_alloc;
    }
    
    bounce_info->op = op;
    bounce_info->callback.read = callback;
    bounce_info->user_arg = user_arg;
    
    result = globus_ftp_control_data_read(
        &op->data_handle->data_channel,
        buffer,
        length,
        globus_l_gfs_data_read_cb,
        bounce_info);
    if(result != GLOBUS_SUCCESS)
    {
        result = GlobusGFSErrorWrapFailed(
            "globus_ftp_control_data_read", result);
        goto error_register;
    }
    
    return GLOBUS_SUCCESS;

error_register:
    globus_free(bounce_info);
    
error_alloc:
    return result;
}


globus_result_t
globus_gridftp_server_register_write(
    globus_gfs_operation_t   op,
    globus_byte_t *                     buffer,  
    globus_size_t                       length,  
    globus_off_t                        offset,  
    int                                 stripe_ndx,  
    globus_gridftp_server_write_cb_t    callback,  
    void *                              user_arg)
{
    globus_result_t                     result;
    globus_l_gfs_data_bounce_t *        bounce_info;
    GlobusGFSName(globus_gridftp_server_register_write);
    
    bounce_info = (globus_l_gfs_data_bounce_t *)
        globus_malloc(sizeof(globus_l_gfs_data_bounce_t));
    if(!bounce_info)
    {
        result = GlobusGFSErrorMemory("bounce_info");
        goto error_alloc;
    }
    
    bounce_info->op = op;
    bounce_info->callback.write = callback;
    bounce_info->user_arg = user_arg;

    if(op->data_handle->info.mode == 'E' && op->stripe_count > 1)
    {
        /* XXX not sure what this is all about */
        globus_mutex_lock(&op->session_handle->mutex);
        {
            if(stripe_ndx != -1)
            {
                op->write_stripe = stripe_ndx;
            }
            if(op->write_stripe >= op->stripe_count)
            {
                op->write_stripe %= op->stripe_count;
            }    
            result = globus_ftp_control_data_write_stripe(
                &op->data_handle->data_channel,
                buffer,
                length,
                offset,
                GLOBUS_FALSE,
                op->write_stripe,
                globus_l_gfs_data_write_cb,
                bounce_info);
                
            op->write_stripe++;
        }    
        globus_mutex_unlock(&op->session_handle->mutex);
    }
    else
    {
        result = globus_ftp_control_data_write(
            &op->data_handle->data_channel,
            buffer,
            length,
            offset,
            GLOBUS_FALSE,
            globus_l_gfs_data_write_cb,
            bounce_info);
    }
    if(result != GLOBUS_SUCCESS)
    {
        result = GlobusGFSErrorWrapFailed(
            "globus_ftp_control_data_write", result);
        goto error_register;
    }
    
    return GLOBUS_SUCCESS;

error_register:
    globus_free(bounce_info);
    
error_alloc:
    return result;
}


void
globus_i_gfs_data_request_set_cred(
    globus_gfs_ipc_handle_t             ipc_handle,
    void *                              session_arg,
    gss_cred_id_t                       del_cred)
{
    globus_l_gfs_data_session_t *       session_handle;  
    globus_result_t                     result;  
    gss_buffer_desc                     buffer;
    int                                 maj_stat;
    int                                 min_stat;    
    GlobusGFSName(globus_i_gfs_data_request_set_cred);

    session_handle = (globus_l_gfs_data_session_t *) session_arg;

    if(session_handle->dsi->set_cred_func != NULL)
    {
        session_handle->dsi->set_cred_func(
            del_cred, session_handle->session_arg);
    }
    /* XXX how to free old cred? */
    if(del_cred != NULL)
    {    
        maj_stat = gss_export_cred(
            &min_stat, del_cred, NULL, 0, &buffer);
        if(maj_stat != GSS_S_COMPLETE)
        {
            result = GlobusGFSErrorWrapFailed("gss_export_cred", min_stat);
            goto error;
        }
        maj_stat = gss_import_cred(
            &min_stat, 
            &session_handle->del_cred, 
            GSS_C_NO_OID, 
            0, 
            &buffer, 
            0, 
            NULL);
        if(maj_stat != GSS_S_COMPLETE)
        {
            result = GlobusGFSErrorWrapFailed("gss_import_cred", min_stat);
            goto error_import;
        }
        maj_stat = gss_release_buffer(&min_stat, &buffer);
        if(maj_stat != GSS_S_COMPLETE)
        {
            result = GlobusGFSErrorWrapFailed("gss_release_buffer", min_stat);
            goto error_import;
        }
    }
    return;

error_import:
    gss_release_buffer(&min_stat, &buffer);
error:
    return;
}    


/* this end receives the buffer */
void
globus_i_gfs_data_request_buffer_send(
    globus_gfs_ipc_handle_t             ipc_handle,
    void *                              session_arg,
    globus_byte_t *                     buffer,
    int                                 buffer_type,
    globus_size_t                       buffer_len)
{
    globus_l_gfs_data_session_t *       session_handle;    
    GlobusGFSName(globus_i_gfs_data_request_buffer_send);

    session_handle = (globus_l_gfs_data_session_t *) session_arg;

    if(buffer_type & GLOBUS_GFS_BUFFER_SERVER_DEFINED)
    {
        switch(buffer_type)
        {
            case GLOBUS_GFS_BUFFER_EOF_INFO:
                break;
            default:
                break;
        }
    }

    if(session_handle->dsi->buffer_send_func != NULL)
    {
        session_handle->dsi->buffer_send_func(
            buffer_type, buffer, buffer_len, session_handle->session_arg);
    }
    return;
}
