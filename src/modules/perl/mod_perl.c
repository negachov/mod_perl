#include "mod_perl.h"

#ifndef USE_ITHREADS
static apr_status_t modperl_shutdown(void *data)
{
    modperl_cleanup_data_t *cdata = (modperl_cleanup_data_t *)data;
    PerlInterpreter *perl = (PerlInterpreter *)cdata->data;
    apr_array_header_t *handles;

    handles = modperl_xs_dl_handles_get(aTHX_ cdata->pool);

    PL_perl_destruct_level = 2;
    MP_TRACE_i(MP_FUNC, "destroying interpreter=0x%lx\n",
               (unsigned long)perl);

    perl_destruct(perl);
    perl_free(perl);

    if (handles) {
        modperl_xs_dl_handles_close(cdata->pool, handles);
    }

    return APR_SUCCESS;
}
#endif

static void my_xs_init(pTHX)
{
    xs_init(aTHX); /* see modperl_xsinit.c */

    newCONSTSUB(PL_defstash,
                "ModPerl::XSLoader::BOOTSTRAP",
                newSViv(1));
}

PerlInterpreter *modperl_startup(server_rec *s, apr_pool_t *p)
{
    MP_dSCFG(s);
    PerlInterpreter *perl;
    int status;
    char **argv;
    int argc;
#ifndef USE_ITHREADS
    modperl_cleanup_data_t *cdata;
#endif

#ifdef MP_USE_GTOP
    MP_TRACE_m_do(
        scfg->gtop = modperl_gtop_new(p);
        modperl_gtop_do_proc_mem_before(MP_FUNC ": perl_parse");
    );
#endif

    argv = modperl_config_srv_argv_init(scfg, &argc);

    if (!(perl = perl_alloc())) {
        perror("perl_alloc");
        exit(1);
    }

    perl_construct(perl);
#ifdef MP_DEBUG
    {
        dTHXa(perl);
        PL_perl_destruct_level = 2;
    }
#endif
    status = perl_parse(perl, my_xs_init, argc, argv, NULL);

    if (status) {
        perror("perl_parse");
        exit(1);
    }

    perl_run(perl);

    MP_TRACE_i(MP_FUNC, "constructed interpreter=0x%lx\n",
               (unsigned long)perl);

#ifdef MP_USE_GTOP
    MP_TRACE_m_do(
        modperl_gtop_do_proc_mem_after(MP_FUNC ": perl_parse");
    );
#endif

    if (!modperl_config_apply_PerlModule(s, scfg, perl, p)) {
        exit(1);
    }
    if (!modperl_config_apply_PerlRequire(s, scfg, perl, p)) {
        exit(1);
    }

#ifndef USE_ITHREADS
    cdata = modperl_cleanup_data_new(p, (void*)perl);
    apr_pool_cleanup_register(p, cdata,
                              modperl_shutdown, apr_pool_cleanup_null);
#endif
    
    return perl;
}

void modperl_init(server_rec *base_server, apr_pool_t *p)
{
    server_rec *s;
    modperl_config_srv_t *base_scfg = modperl_config_srv_get(base_server);
    PerlInterpreter *base_perl;

    MP_TRACE_d_do(MpSrv_dump_flags(base_scfg,
                                   base_server->server_hostname));

#ifndef USE_ITHREADS
    if (base_scfg->threaded_mpm) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, base_server,
                     "cannot use threaded MPM without ithreads enabled Perl");
        exit(1);
    }
#endif

    if (!MpSrvENABLED(base_scfg)) {
        /* how silly */
        return;
    }

    base_perl = modperl_startup(base_server, p);

#ifdef USE_ITHREADS
    modperl_interp_init(base_server, p, base_perl);
    MpInterpBASE_On(base_scfg->mip->parent);
#endif

    for (s=base_server->next; s; s=s->next) {
        MP_dSCFG(s);
        PerlInterpreter *perl = base_perl;

        MP_TRACE_d_do(MpSrv_dump_flags(scfg, s->server_hostname));

        /* if alloc flags is On, virtual host gets its own parent perl */
        if (MpSrvPARENT(scfg)) {
            perl = modperl_startup(s, p);
            MP_TRACE_i(MP_FUNC,
                       "created parent interpreter for VirtualHost %s\n",
                       modperl_server_desc(s, p));
        }

        if (!modperl_config_apply_PerlModule(s, scfg, perl, p)) {
            exit(1);
        }
        if (!modperl_config_apply_PerlRequire(s, scfg, perl, p)) {
            exit(1);
        }

#ifdef USE_ITHREADS

        if (!MpSrvENABLED(scfg)) {
            scfg->mip = NULL;
            continue;
        }

        /* if alloc flags is On or clone flag is On,
         *  virtual host gets its own mip
         */
        if (MpSrvPARENT(scfg) || MpSrvCLONE(scfg)) {
            MP_TRACE_i(MP_FUNC, "modperl_interp_init() server=%s\n",
                       modperl_server_desc(s, p));
            modperl_interp_init(s, p, perl);
        }

        /* if we allocated a parent perl, mark it to be destroyed */
        if (MpSrvPARENT(scfg)) {
            MpInterpBASE_On(scfg->mip->parent);
        }

        if (!scfg->mip) {
            /* since mips are created after merge_server_configs()
             * need to point to the base mip here if this vhost
             * doesn't have its own
             */
            scfg->mip = base_scfg->mip;
        }

#endif /* USE_ITHREADS */

    }
}

#ifdef USE_ITHREADS
static void modperl_init_clones(server_rec *s, apr_pool_t *p)
{
    modperl_config_srv_t *base_scfg = modperl_config_srv_get(s);
#ifdef MP_TRACE
    char *base_name = modperl_server_desc(s, p);
#endif /* MP_TRACE */

    if (!base_scfg->threaded_mpm) {
        MP_TRACE_i(MP_FUNC, "no clones created for non-threaded mpm\n");
        return;
    }

    for (; s; s=s->next) {
        MP_dSCFG(s);
#ifdef MP_TRACE
        char *name = modperl_server_desc(s, p);

        MP_TRACE_i(MP_FUNC, "PerlInterpScope set to %s for %s\n",
                   modperl_interp_scope_desc(scfg->interp_scope), name);
#else
        char *name = NULL;
#endif /* MP_TRACE */

        if (scfg->mip->tipool->idle) {
#ifdef MP_TRACE
            if (scfg->mip == base_scfg->mip) {
                MP_TRACE_i(MP_FUNC,
                           "%s interp pool inherited from %s\n",
                           name, base_name);
            }
            else {
                MP_TRACE_i(MP_FUNC,
                           "%s interp pool already initialized\n",
                           name);
            }
#endif /* MP_TRACE */
        }
        else {
            MP_TRACE_i(MP_FUNC, "initializing interp pool for %s\n",
                       name);
            modperl_tipool_init(scfg->mip->tipool);
        }
    }
}
#endif /* USE_ITHREADS */

static void modperl_init_globals(server_rec *s, apr_pool_t *pconf)
{
    int threaded_mpm;
    ap_mpm_query(AP_MPMQ_IS_THREADED, &threaded_mpm);

    modperl_global_init_pconf(pconf, pconf);
    modperl_global_init_threaded_mpm(pconf, threaded_mpm);
    modperl_global_init_server_rec(pconf, s);

    modperl_tls_create_request_rec(pconf);
}

static apr_status_t modperl_sys_init(void)
{
#if 0 /*XXX*/
    PERL_SYS_INIT(0, NULL);

#ifdef PTHREAD_ATFORK
    if (!ap_exists_config_define("PERL_PTHREAD_ATFORK_DONE")) {
        PTHREAD_ATFORK(Perl_atfork_lock,
                       Perl_atfork_unlock,
                       Perl_atfork_unlock);
        *(char **)apr_array_push(ap_server_config_defines) =
            "PERL_PTHREAD_ATFORK_DONE";
    }
#endif
#endif
    return APR_SUCCESS;
}

static apr_status_t modperl_sys_term(void *data)
{
#if 0 /*XXX*/
    PERL_SYS_TERM();
#endif
    return APR_SUCCESS;
}

void modperl_hook_init(apr_pool_t *pconf, apr_pool_t *plog, 
                       apr_pool_t *ptemp, server_rec *s)
{
    modperl_sys_init();
    apr_pool_cleanup_register(pconf, NULL, modperl_sys_term, NULL);
    modperl_init_globals(s, pconf);
    modperl_init(s, pconf);
}

void modperl_pre_config_handler(apr_pool_t *p, apr_pool_t *plog,
                                apr_pool_t *ptemp)
{
    /* XXX: htf can we have PerlPreConfigHandler
     * without first configuring mod_perl ?
     */
}

static int modperl_hook_pre_connection(conn_rec *c)
{
    modperl_input_filter_register_connection(c);
    modperl_output_filter_register_connection(c);
    return OK;
}

static void modperl_hook_post_config(apr_pool_t *pconf, apr_pool_t *plog,
                                     apr_pool_t *ptemp, server_rec *s)
{
#ifdef USE_ITHREADS
    MP_dSCFG(s);
    dTHXa(scfg->mip->parent->perl);
#endif
    ap_add_version_component(pconf, MP_VERSION_STRING);
    ap_add_version_component(pconf,
                             Perl_form(aTHX_ "Perl/v%vd", PL_patchlevel));
    modperl_mgv_hash_handlers(pconf, s);
#ifdef USE_ITHREADS
    modperl_init_clones(s, pconf);
#endif
}

static int modperl_hook_create_request(request_rec *r)
{
    MP_dRCFG;

    modperl_config_req_init(r, rcfg);

    return OK;
}

static int modperl_hook_post_read_request(request_rec *r)
{
    /* if 'PerlOptions +GlobalRequest' is outside a container */
    modperl_global_request_cfg_set(r);

    return OK;
}

static int modperl_hook_header_parser(request_rec *r)
{
    /* if 'PerlOptions +GlobalRequest' is inside a container */
    modperl_global_request_cfg_set(r);

    return OK;
}

void modperl_register_hooks(apr_pool_t *p)
{
    ap_hook_open_logs(modperl_hook_init,
                      NULL, NULL, APR_HOOK_MIDDLE);

    ap_hook_post_config(modperl_hook_post_config,
                        NULL, NULL, APR_HOOK_MIDDLE);

    ap_hook_handler(modperl_response_handler,
                    NULL, NULL, APR_HOOK_MIDDLE);

    ap_hook_handler(modperl_response_handler_cgi,
                    NULL, NULL, APR_HOOK_MIDDLE);

    ap_hook_insert_filter(modperl_output_filter_register_request,
                          NULL, NULL, APR_HOOK_LAST);

    ap_hook_insert_filter(modperl_input_filter_register_request,
                          NULL, NULL, APR_HOOK_LAST);

    ap_register_output_filter(MODPERL_OUTPUT_FILTER_NAME,
                              modperl_output_filter_handler,
                              AP_FTYPE_CONTENT);

    ap_register_input_filter(MODPERL_INPUT_FILTER_NAME,
                             modperl_input_filter_handler,
                             AP_FTYPE_CONTENT);

    ap_hook_pre_connection(modperl_hook_pre_connection,
                           NULL, NULL, APR_HOOK_FIRST);

    ap_hook_create_request(modperl_hook_create_request,
                           NULL, NULL, APR_HOOK_MIDDLE);

    ap_hook_post_read_request(modperl_hook_post_read_request,
                              NULL, NULL, APR_HOOK_FIRST);

    ap_hook_header_parser(modperl_hook_header_parser,
                          NULL, NULL, APR_HOOK_FIRST);

    modperl_register_handler_hooks();
}

static const command_rec modperl_cmds[] = {  
    MP_CMD_SRV_ITERATE("PerlSwitches", switches, "Perl Switches"),
    MP_CMD_SRV_ITERATE("PerlModule", modules, "PerlModule"),
    MP_CMD_SRV_ITERATE("PerlRequire", requires, "PerlRequire"),
    MP_CMD_DIR_ITERATE("PerlOptions", options, "Perl Options"),
#ifdef MP_TRACE
    MP_CMD_SRV_TAKE1("PerlTrace", trace, "Trace level"),
#endif
#ifdef USE_ITHREADS
    MP_CMD_SRV_TAKE1("PerlInterpStart", interp_start,
                     "Number of Perl interpreters to start"),
    MP_CMD_SRV_TAKE1("PerlInterpMax", interp_max,
                     "Max number of running Perl interpreters"),
    MP_CMD_SRV_TAKE1("PerlInterpMaxSpare", interp_max_spare,
                     "Max number of spare Perl interpreters"),
    MP_CMD_SRV_TAKE1("PerlInterpMinSpare", interp_min_spare,
                     "Min number of spare Perl interpreters"),
    MP_CMD_SRV_TAKE1("PerlInterpMaxRequests", interp_max_requests,
                     "Max number of requests per Perl interpreters"),
    MP_CMD_DIR_TAKE1("PerlInterpScope", interp_scope,
                     "Scope of a Perl interpreter"),
#endif
    MP_CMD_ENTRIES,
    { NULL }, 
}; 

void modperl_response_init(request_rec *r)
{
    MP_dRCFG;

    /* setup buffer for output */
    rcfg->wbucket.pool = r->pool;
    rcfg->wbucket.filters = &r->output_filters;
    rcfg->wbucket.outcnt = 0;
}

void modperl_response_finish(request_rec *r)
{
    MP_dRCFG;

    /* flush output buffer */
    modperl_wbucket_flush(&rcfg->wbucket);
}

static int modperl_response_handler_run(request_rec *r, int finish)
{
    int retval;

    modperl_response_init(r);

    retval = modperl_callback_per_dir(MP_RESPONSE_HANDLER, r);

    if ((retval == DECLINED) && r->content_type) {
        r->handler = r->content_type; /* let http_core or whatever try */
    }

    if (finish) {
        modperl_response_finish(r);
    }

    return retval;
}

int modperl_response_handler(request_rec *r)
{
    if (!strEQ(r->handler, "modperl")) {
        return DECLINED;
    }

    return modperl_response_handler_run(r, TRUE);
}

int modperl_response_handler_cgi(request_rec *r)
{
    MP_dDCFG;
    MP_dRCFG;
    GV *h_stdin, *h_stdout;
    int retval;
#ifdef USE_ITHREADS
    pTHX;
    modperl_interp_t *interp;
#endif

    if (!strEQ(r->handler, "perl-script")) {
        return DECLINED;
    }

#ifdef USE_ITHREADS
    interp = modperl_interp_select(r, r->connection, r->server);
    aTHX = interp->perl;
    if (MpInterpPUTBACK(interp)) {
        rcfg->interp = interp;
    }
#endif

    if (MpDirPARSE_HEADERS(dcfg)) {
        rcfg->wbucket.header_parse = 1;
    }

    h_stdout = modperl_io_tie_stdout(aTHX_ r);
    h_stdin  = modperl_io_tie_stdin(aTHX_ r);

    modperl_env_request_tie(aTHX_ r);
    retval = modperl_response_handler_run(r, FALSE);

    modperl_io_handle_untie(aTHX_ h_stdout);
    modperl_io_handle_untie(aTHX_ h_stdin);

    modperl_env_request_untie(aTHX_ r);

#ifdef USE_ITHREADS
    if (MpInterpPUTBACK(interp)) {
        /* PerlInterpScope handler */
        modperl_interp_unselect(interp);
        rcfg->interp = NULL;
    }
#endif

    /* flush output buffer after interpreter is putback */
    modperl_response_finish(r);

    return retval;
}

module AP_MODULE_DECLARE_DATA perl_module = {
    STANDARD20_MODULE_STUFF, 
    modperl_config_dir_create, /* dir config creater */
    modperl_config_dir_merge,  /* dir merger --- default is to override */
    modperl_config_srv_create, /* server config */
    modperl_config_srv_merge,  /* merge server config */
    modperl_cmds,              /* table of config file commands       */
    modperl_register_hooks,    /* register hooks */
};
