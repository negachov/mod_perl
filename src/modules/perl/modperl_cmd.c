#include "mod_perl.h"

static char *modperl_cmd_unclosed_directive(cmd_parms *parms)
{
    return apr_pstrcat(parms->pool, parms->cmd->name,
                       "> directive missing closing '>'", NULL);
}

static char *modperl_cmd_too_late(cmd_parms *parms)
{
    return apr_pstrcat(parms->pool, "mod_perl already running, "
                       "too late for ", parms->cmd->name, NULL);
}

char *modperl_cmd_push_handlers(MpAV **handlers, const char *name,
                                apr_pool_t *p)
{
    modperl_handler_t *h = modperl_handler_new(p, name);

    if (!*handlers) {
        *handlers = modperl_handler_array_new(p);
        MP_TRACE_d(MP_FUNC, "created handler stack\n");
    }

    /* XXX parse_handler if Perl is running */

    modperl_handler_array_push(*handlers, h);
    MP_TRACE_d(MP_FUNC, "pushed handler: %s\n", h->name);

    return NULL;
}


#define MP_CMD_SRV_TRACE \
    MP_TRACE_d(MP_FUNC, "%s %s\n", parms->cmd->name, arg)

#define MP_CMD_SRV_CHECK \
MP_CMD_SRV_TRACE; \
{ \
    const char *err = ap_check_cmd_context(parms, GLOBAL_ONLY); \
    if (err) return err; \
}

MP_CMD_SRV_DECLARE(trace)
{
    MP_CMD_SRV_CHECK;
    modperl_trace_level_set(parms->server, arg);
    return NULL;
}

static int modperl_vhost_is_running(server_rec *s)
{
#ifdef USE_ITHREADS
    MP_dSCFG(s);
    int is_vhost = (s != modperl_global_get_server_rec());

    if (is_vhost && scfg->mip) {
        return TRUE;
    }
    else {
        return FALSE;
    }
#else
    return TRUE;
#endif
}

MP_CMD_SRV_DECLARE(switches)
{
    server_rec *s = parms->server;
    MP_dSCFG(s);
    if (modperl_is_running() && modperl_vhost_is_running(s)) {
        return modperl_cmd_too_late(parms);
    }
    MP_TRACE_d(MP_FUNC, "arg = %s\n", arg);
    modperl_config_srv_argv_push(arg);
    return NULL;
}

MP_CMD_SRV_DECLARE(modules)
{
    MP_dSCFG(parms->server);

    if (modperl_is_running() &&
        modperl_init_vhost(parms->server, parms->pool, NULL) != OK)
    {
        return "init mod_perl vhost failed";
    }

    if (modperl_is_running()) {
#ifdef USE_ITHREADS
        /* XXX: .htaccess support cannot use this perl with threaded MPMs */
        dTHXa(scfg->mip->parent->perl);
#endif
        MP_TRACE_d(MP_FUNC, "load PerlModule %s\n", arg);

        if (!modperl_require_module(aTHX_ arg, FALSE)) {
            return SvPVX(ERRSV);
        }
    }
    else {
        MP_TRACE_d(MP_FUNC, "push PerlModule %s\n", arg);
        *(const char **)apr_array_push(scfg->PerlModule) = arg;
    }

    return NULL;
}

MP_CMD_SRV_DECLARE(requires)
{
    MP_dSCFG(parms->server);

    if (modperl_is_running() &&
        modperl_init_vhost(parms->server, parms->pool, NULL) != OK)
    {
        return "init mod_perl vhost failed";
    }

    if (modperl_is_running()) {
#ifdef USE_ITHREADS
        /* XXX: .htaccess support cannot use this perl with threaded MPMs */
        dTHXa(scfg->mip->parent->perl);
#endif

        MP_TRACE_d(MP_FUNC, "load PerlRequire %s\n", arg);

        if (!modperl_require_file(aTHX_ arg, FALSE)) {
            return SvPVX(ERRSV);
        }
    }
    else {
        MP_TRACE_d(MP_FUNC, "push PerlRequire %s\n", arg);
        *(const char **)apr_array_push(scfg->PerlRequire) = arg;
    }

    return NULL;
}

static MP_CMD_SRV_DECLARE2(handle_vars)
{
    MP_dSCFG(parms->server);
    modperl_config_dir_t *dcfg = (modperl_config_dir_t *)mconfig;
    const char *name = parms->cmd->name;

    modperl_table_modify_t func =
        strEQ(name, "PerlSetVar") ? apr_table_setn : apr_table_addn;

    func(dcfg->vars, arg1, arg2);

    MP_TRACE_d(MP_FUNC, "%s DIR: arg1 = %s, arg2 = %s\n",
               name, arg1, arg2);

    /* make available via Apache->server->dir_config */
    if (!parms->path) {
        func(scfg->vars, arg1, arg2);

        MP_TRACE_d(MP_FUNC, "%s SRV: arg1 = %s, arg2 = %s\n",
                   name, arg1, arg2);
    }

    return NULL;
}

MP_CMD_SRV_DECLARE2(set_var)
{
    return modperl_cmd_handle_vars(parms, mconfig, arg1, arg2);
}

MP_CMD_SRV_DECLARE2(add_var)
{
    return modperl_cmd_handle_vars(parms, mconfig, arg1, arg2);
}

MP_CMD_SRV_DECLARE2(set_env)
{
    MP_dSCFG(parms->server);
    modperl_config_dir_t *dcfg = (modperl_config_dir_t *)mconfig;
 
#ifdef ENV_IS_CASELESS /* i.e. WIN32 */
    /* we turn off env magic during hv_store later, so do this now,
     * else lookups on keys with lowercase characters will fails
     * because Perl will uppercase them prior to lookup.
     */
    modperl_str_toupper((char *)arg1);
#endif

    MP_TRACE_d(MP_FUNC, "arg1 = %s, arg2 = %s\n", arg1, arg2);

    if (!parms->path) {
        /* will be propagated to environ */
        apr_table_setn(scfg->SetEnv, arg1, arg2);
    }

    apr_table_setn(dcfg->SetEnv, arg1, arg2);

    return NULL;
}

MP_CMD_SRV_DECLARE(pass_env)
{
    MP_dSCFG(parms->server);
    char *val = getenv(arg);
 
    if (val) {
        apr_table_setn(scfg->PassEnv, arg, apr_pstrdup(parms->pool, val));
        MP_TRACE_d(MP_FUNC, "arg = %s, val = %s\n", arg, val);
    }
    else {
        MP_TRACE_d(MP_FUNC, "arg = %s: not found via getenv()\n", arg);
    }

    return NULL;
}

MP_CMD_SRV_DECLARE(options)
{
    MP_dSCFG(parms->server);
    modperl_config_dir_t *dcfg = (modperl_config_dir_t *)mconfig;
    int is_per_dir = parms->path ? 1 : 0;
    modperl_options_t *opts = is_per_dir ? dcfg->flags : scfg->flags;
    apr_pool_t *p = parms->pool;
    const char *error;

    MP_TRACE_d(MP_FUNC, "arg = %s\n", arg);
    if ((error = modperl_options_set(p, opts, arg)) && !is_per_dir) {
        /* maybe a per-directory option outside of a container */
        if (modperl_options_set(p, dcfg->flags, arg) == NULL) {
            error = NULL;
        }
    }

    if (error) {
        return error;
    }

    return NULL;
}

MP_CMD_SRV_DECLARE(init_handlers)
{
    if (parms->path) {
        return modperl_cmd_header_parser_handlers(parms, mconfig, arg);
    }

    return modperl_cmd_post_read_request_handlers(parms, mconfig, arg);
}

static const char *modperl_cmd_parse_args(pTHX_ apr_pool_t *p,
                                          const char *args,
                                          HV **hv)
{
    const char *orig_args = args;
    char *pair, *key, *val;
    *hv = newHV();

    while (*(pair = ap_getword(p, &args, ',')) != '\0') {
        key = ap_getword_nc(p, &pair, '=');
        val = pair;

        if (!(*key && *val)) {
            SvREFCNT_dec(*hv);
            *hv = Nullhv;
            return apr_pstrcat(p, "invalid args spec: ",
                               orig_args, NULL);
        }

        hv_store(*hv, key, strlen(key), newSVpv(val,0), 0);
    }

    return NULL;
}

MP_CMD_SRV_DECLARE(perl)
{
    apr_pool_t *p = parms->pool;
    server_rec *s = parms->server;
    const char *endp = ap_strrchr_c(arg, '>');
    const char *errmsg;
    modperl_handler_t *handler;
    AV *args = Nullav;
    HV *hv = Nullhv;
    SV **handler_name;
    int status = OK;
#ifdef USE_ITHREADS
    MP_dSCFG(s);
    pTHX;
#endif

    if (endp == NULL) {
        return modperl_cmd_unclosed_directive(parms);
    }

    /* we must init earlier than normal */
    modperl_run(p, s);

    if (modperl_init_vhost(s, p, NULL) != OK) {
        return "init mod_perl vhost failed";
    }

#ifdef USE_ITHREADS
    /* XXX: .htaccess support cannot use this perl with threaded MPMs */
    aTHX = scfg->mip->parent->perl;
#endif

    arg = apr_pstrndup(p, arg, endp - arg);

    if ((errmsg = modperl_cmd_parse_args(aTHX_ p, arg, &hv))) {
        return errmsg;
    }

    if (!(handler_name = hv_fetch(hv, "handler", strlen("handler"), 0))) {
        /* XXX: we will have a default handler in the future */
        return "no <Perl> handler specified";
    }

    handler = modperl_handler_new(p, SvPVX(*handler_name));

    modperl_handler_make_args(aTHX_ &args,
                              "Apache::CmdParms", parms,
                              "HV", hv,
                              NULL);

    status = modperl_callback(aTHX_ handler, p, NULL, s, args);

    SvREFCNT_dec((SV*)args);

    if (status != OK) {
        return SvTRUE(ERRSV) ? SvPVX(ERRSV) :
            apr_psprintf(p, "<Perl> handler %s failed with status=%d",
                         handler->name, status);
    }

    return NULL;
}

#define MP_POD_FORMAT(s) \
   (ap_strstr_c(s, "httpd") || ap_strstr_c(s, "apache"))

MP_CMD_SRV_DECLARE(pod)
{
    char line[MAX_STRING_LEN];

    if (arg && *arg && !(MP_POD_FORMAT(arg) || strstr("pod", arg))) {  
        return "Unknown =back format";
    }

    while (!ap_cfg_getline(line, sizeof(line), parms->config_file)) {
        if (strEQ(line, "=cut")) {
            break;
        }
        if (strnEQ(line, "=over", 5) && MP_POD_FORMAT(line)) {
            break;
        }
    }

    return NULL;
}

MP_CMD_SRV_DECLARE(pod_cut)
{
    return "=cut without =pod";
}

MP_CMD_SRV_DECLARE(END)
{
    char line[MAX_STRING_LEN];

    while (!ap_cfg_getline(line, sizeof(line), parms->config_file)) {
	/* soak up rest of the file */
    }

    return NULL;
}

/*
 * XXX: the name of this directive may or may not stay.
 * need a way to note that a module has config directives.
 * don't want to start mod_perl when we see a non-special PerlModule.
 */
MP_CMD_SRV_DECLARE(load_module)
{
    apr_pool_t *p = parms->pool;
    server_rec *s = parms->server;
    const char *errmsg;

    MP_TRACE_d(MP_FUNC, "LoadPerlModule %s\n", arg);

    /* we must init earlier than normal */
    modperl_run(p, s);

    if ((errmsg = modperl_cmd_modules(parms, mconfig, arg))) {
        return errmsg;
    }

    return modperl_module_add(p, s, arg);
}

#ifdef MP_COMPAT_1X

MP_CMD_SRV_DECLARE_FLAG(taint_check)
{
    if (flag_on) {
        return modperl_cmd_switches(parms, mconfig, "-T");
    }

    return NULL;
}

MP_CMD_SRV_DECLARE_FLAG(warn)
{
    if (flag_on) {
        return modperl_cmd_switches(parms, mconfig, "-w");
    }

    return NULL;
}

MP_CMD_SRV_DECLARE_FLAG(send_header)
{
    char *arg = flag_on ? "+ParseHeaders" : "-ParseHeaders";
    return modperl_cmd_options(parms, mconfig, arg);
}

MP_CMD_SRV_DECLARE_FLAG(setup_env)
{
    char *arg = flag_on ? "+SetupEnv" : "-SetupEnv";
    return modperl_cmd_options(parms, mconfig, arg);
}

#endif /* MP_COMPAT_1X */

#ifdef USE_ITHREADS

#define MP_INTERP_SCOPE_USAGE "PerlInterpScope must be one of "

#define MP_INTERP_SCOPE_DIR_OPTS \
"handler, subrequest or request"

#define MP_INTERP_SCOPE_DIR_USAGE \
MP_INTERP_SCOPE_USAGE MP_INTERP_SCOPE_DIR_OPTS
 
#define MP_INTERP_SCOPE_SRV_OPTS \
"connection, " MP_INTERP_SCOPE_DIR_OPTS

#define MP_INTERP_SCOPE_SRV_USAGE \
MP_INTERP_SCOPE_USAGE MP_INTERP_SCOPE_SRV_OPTS

MP_CMD_SRV_DECLARE(interp_scope)
{
    modperl_interp_scope_e *scope;
    modperl_config_dir_t *dcfg = (modperl_config_dir_t *)mconfig;
    MP_dSCFG(parms->server);
    int is_per_dir = parms->path ? 1 : 0;

    scope = is_per_dir ? &dcfg->interp_scope : &scfg->interp_scope;

    switch (toLOWER(*arg)) {
      case 'h':
        if (strcaseEQ(arg, "handler")) {
            *scope = MP_INTERP_SCOPE_HANDLER;
            break;
        }
      case 's':
        if (strcaseEQ(arg, "subrequest")) {
            *scope = MP_INTERP_SCOPE_SUBREQUEST;
            break;
        }
      case 'r':
        if (strcaseEQ(arg, "request")) {
            *scope = MP_INTERP_SCOPE_REQUEST;
            break;
        }
      case 'c':
        if (!is_per_dir && strcaseEQ(arg, "connection")) {
            *scope = MP_INTERP_SCOPE_CONNECTION;
            break;
        }
      default:
        return is_per_dir ?
             MP_INTERP_SCOPE_DIR_USAGE : MP_INTERP_SCOPE_SRV_USAGE;
    };

    return NULL;
}

#define MP_CMD_INTERP_POOL_IMP(xitem) \
const char *modperl_cmd_interp_##xitem(cmd_parms *parms, \
                                      void *mconfig, const char *arg) \
{ \
    MP_dSCFG(parms->server); \
    int item = atoi(arg); \
    scfg->interp_pool_cfg->xitem = item; \
    MP_TRACE_d(MP_FUNC, "%s %d\n", parms->cmd->name, item); \
    return NULL; \
}

MP_CMD_INTERP_POOL_IMP(start);
MP_CMD_INTERP_POOL_IMP(max);
MP_CMD_INTERP_POOL_IMP(max_spare);
MP_CMD_INTERP_POOL_IMP(min_spare);
MP_CMD_INTERP_POOL_IMP(max_requests);

#endif /* USE_ITHREADS */
