#ifndef MODPERL_GLOBAL_H
#define MODPERL_GLOBAL_H

typedef struct {
#if MP_THREADED
    perl_mutex glock;
#endif
    int flags;
    void *data;
    const char *name;
} modperl_global_t;

#if MP_THREADED
typedef apr_threadkey_t modperl_tls_t;
#else
typedef modperl_global_t modperl_tls_t;
#endif

void modperl_global_request_cfg_set(request_rec *r);

void modperl_global_request_set(request_rec *r);

void modperl_global_request_obj_set(pTHX_ SV *svr);

void modperl_global_init(modperl_global_t *global, apr_pool_t *p,
                         void *data, const char *name);

void modperl_global_lock(modperl_global_t *global);

void modperl_global_unlock(modperl_global_t *global);

void *modperl_global_get(modperl_global_t *global);

void modperl_global_set(modperl_global_t *global, void *data);

#define MP_GLOBAL_DECL(gname, type) \
void modperl_global_init_##gname(apr_pool_t *p, type gname); \
void modperl_global_lock_##gname(void); \
void modperl_global_unlock_##gname(void); \
type modperl_global_get_##gname(void); \
void modperl_global_set_##gname(void *)

MP_GLOBAL_DECL(pconf, apr_pool_t *);
MP_GLOBAL_DECL(server_rec, server_rec *);
MP_GLOBAL_DECL(threaded_mpm, int);

apr_status_t modperl_tls_create(apr_pool_t *p, modperl_tls_t **key);
apr_status_t modperl_tls_get(modperl_tls_t *key, void **data);
apr_status_t modperl_tls_set(modperl_tls_t *key, void *data);
void modperl_tls_reset_cleanup(apr_pool_t *p, modperl_tls_t *key, void *data);

#define MP_TLS_DECL(gname, type) \
apr_status_t modperl_tls_create_##gname(apr_pool_t *p); \
apr_status_t modperl_tls_get_##gname(type *data); \
apr_status_t modperl_tls_set_##gname(void *data); \
void modperl_tls_reset_cleanup_##gname(apr_pool_t *p, type data)

MP_TLS_DECL(request_rec, request_rec *);

#endif /* MODPERL_GLOBAL_H */