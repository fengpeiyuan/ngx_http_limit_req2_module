#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>



typedef struct {
    ngx_int_t processnumber;
    ngx_int_t pid;
} ngx_http_limit_req2_shctx_buffer;

typedef struct {
    ngx_int_t current_slots;
    ngx_int_t last_used_pos;
    ngx_http_limit_req2_shctx_buffer *buffer;
} ngx_http_limit_req2_shctx_t;

typedef struct {
	ngx_http_limit_req2_shctx_t	*shm;
    ngx_slab_pool_t             *shpool;
} ngx_http_limit_req2_ctx_t;

typedef struct {
    ngx_slab_pool_t             *shpool;
} ngx_http_limit_req2_lock_t;

typedef struct {
    ngx_int_t limit_number;
    ngx_shm_zone_t *ctx_shm_zone;
    ngx_int_t buffer_pos;
    ngx_int_t processinit;
    ngx_shm_zone_t *lock_shm_zone;
} ngx_http_limit_req2_loc_conf_t;

static ngx_int_t worker_processes;
static ngx_int_t number;

static void *ngx_http_limit_req2_create_loc_conf(ngx_conf_t *cf);
static char *ngx_http_limit_req2_merge_loc_conf(ngx_conf_t*, void*, void*);
static char *ngx_http_limit_req2_number(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static ngx_int_t ngx_http_limit_req2_init_shm_zone(ngx_shm_zone_t *shm_zone, void *data);
static ngx_int_t ngx_http_limit_req2_init_lock_shm_zone (ngx_shm_zone_t *shm_zone, void *data);
static ngx_int_t ngx_http_limit_req2_init(ngx_conf_t *cf);
static ngx_int_t ngx_http_limit_req2_access_handler(ngx_http_request_t *r);
static ngx_int_t ngx_http_limit_req2_log_handler(ngx_http_request_t *r);

static ngx_command_t ngx_http_limit_req2_commands[] = {
    { ngx_string("limit_number"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
	  ngx_http_limit_req2_number,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },
      ngx_null_command
};

static ngx_http_module_t  ngx_http_limit_req2_ctx = {
    NULL,                                  /* preconfiguration */
	ngx_http_limit_req2_init,
    NULL,                                  /* create main configuration */
    NULL,                                  /* init main configuration */
    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */
	ngx_http_limit_req2_create_loc_conf, /* create location configuration */
	ngx_http_limit_req2_merge_loc_conf   /* merge location configuration */
};

ngx_module_t  ngx_http_limit_req2_module = {
    NGX_MODULE_V1,
    &ngx_http_limit_req2_ctx, /* module context */
    ngx_http_limit_req2_commands,    /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    NULL,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};

static void *
ngx_http_limit_req2_create_loc_conf(ngx_conf_t *cf){
    ngx_http_limit_req2_loc_conf_t  *conf;
    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_limit_req2_loc_conf_t));
    if (conf == NULL) {
        return NGX_CONF_ERROR;
    }
    conf->limit_number = NGX_CONF_UNSET;
    conf->buffer_pos = NGX_CONF_UNSET;
    return conf;
}

static char *
ngx_http_limit_req2_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child){
	ngx_http_limit_req2_loc_conf_t *prev = parent;
	ngx_http_limit_req2_loc_conf_t *conf = child;
    if (conf->ctx_shm_zone == NULL) {
        conf->ctx_shm_zone = prev->ctx_shm_zone;
    }
    if(conf->limit_number<=0){
    	conf->limit_number = prev->limit_number;
    }
    if(conf->buffer_pos<=0){
    	conf->buffer_pos = prev->buffer_pos;
    }

    printf("##merge limit_number:%ld,buffer_pos:%ld,pref limit_number:%ld,buffer_pos:%ld",conf->limit_number,conf->buffer_pos,prev->limit_number,prev->buffer_pos);

    return NGX_CONF_OK;
}

static ngx_int_t
ngx_http_limit_req2_access_handler(ngx_http_request_t *r){
	ngx_http_limit_req2_loc_conf_t *lrlc;
	ngx_http_limit_req2_ctx_t  *ctx;
	ngx_http_limit_req2_lock_t  *lock;
	lrlc = ngx_http_get_module_loc_conf(r, ngx_http_limit_req2_module);
	ctx = lrlc->ctx_shm_zone->data;
	lock = lrlc->lock_shm_zone->data;

	ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,  "##0.limit_number:%ld,buffer_pos:%d,init:%d\n",lrlc->limit_number,lrlc->buffer_pos,lrlc->processinit);


    if (lrlc->processinit != 1) {
        ngx_shmtx_lock(&((ngx_slab_pool_t*)(lrlc->lock_shm_zone->shm.addr))->mutex);
        if (lrlc->processinit != 1) {
        	lrlc->processinit = 1;
            if (ctx->shm->last_used_pos >= worker_processes) {
                char buf[16] = {0};
                ngx_int_t *pid;
                ngx_pool_t *pool = NULL;
                pool = r->pool;
                ngx_array_t* arr = ngx_array_create(pool, worker_processes, sizeof(ngx_int_t));
                FILE *fp = popen("ps -ef | grep 'nginx' | grep -i worker | awk \'{print $2}\'", "r");
                if (fp == NULL) {
                    return NGX_ERROR;
                }
                while (NULL != fgets(buf, sizeof(buf), fp)) {
                    pid = (ngx_int_t*)ngx_array_push(arr);
                    *pid = atoi(buf);
                }
                ngx_uint_t i;
                ngx_int_t j;
                for (j = 0; j < worker_processes; j++) {
                    ngx_http_limit_req2_shctx_buffer *sb = ctx->shm->buffer + j;
                    for (i = 0; i < arr->nelts; ++i) {
                        pid = (ngx_int_t*)((ngx_int_t*)arr->elts + i);
                        if (*pid == sb->pid) {
                            break;
                        }
                    }
                    if (i >= arr->nelts) {
                    	lrlc->buffer_pos = j;
                        sb->pid = getpid();
                        break;
                    }
                }
                pclose(fp);
                ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,  "##1.here \n");

            } else {
            	lrlc->buffer_pos = ctx->shm->last_used_pos;
                ngx_http_limit_req2_shctx_buffer *sb = ctx->shm->buffer + lrlc->buffer_pos;
                sb->pid = getpid();
                sb->processnumber = lrlc->limit_number/worker_processes;
                ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,  "##2.limit_number:%d,worker_processes:%d,processnumber:%d,pos:%d\n",lrlc->limit_number,worker_processes,
                		sb->processnumber,lrlc->buffer_pos);

            }
            ctx->shm->last_used_pos++;

        }
        ngx_shmtx_unlock(&((ngx_slab_pool_t*)(lrlc->lock_shm_zone->shm.addr))->mutex);
    }

    ngx_http_limit_req2_shctx_buffer *curr_sb = ctx->shm->buffer + lrlc->buffer_pos;
    curr_sb->processnumber--;//ctx sign here

    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,  "##3.processnumber:%d,pos:%d\n",curr_sb->processnumber,lrlc->buffer_pos);

    if(curr_sb->processnumber<=0){
    	return NGX_HTTP_FORBIDDEN;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_limit_req2_log_handler(ngx_http_request_t *r){
	ngx_http_limit_req2_loc_conf_t *lrlc;
	ngx_http_limit_req2_ctx_t  *ctx;
	ngx_http_limit_req2_lock_t  *lock;
	lrlc = ngx_http_get_module_loc_conf(r, ngx_http_limit_req2_module);
	ctx = lrlc->ctx_shm_zone->data;
	lock = lrlc->lock_shm_zone->data;
    ngx_http_limit_req2_shctx_buffer *curr_sb = ctx->shm->buffer + lrlc->buffer_pos;
    curr_sb->processnumber++;
    return NGX_OK;
}


static char *
ngx_http_limit_req2_number(ngx_conf_t *cf, ngx_command_t *cmd, void *conf){
    ngx_http_limit_req2_ctx_t *ctx;
    ngx_http_limit_req2_lock_t *lock;
	ngx_str_t                 *value;
    ngx_int_t 				  *limit_number;
    ngx_shm_zone_t            *shm_zone;
    ngx_shm_zone_t            *lock_shm_zone;
    ngx_core_conf_t *ccf = (ngx_core_conf_t *) ngx_get_conf(cf->cycle->conf_ctx, ngx_core_module);
    ngx_http_limit_req2_loc_conf_t *lrlc = conf;
    limit_number = &(lrlc->limit_number);
    worker_processes = ccf->worker_processes;
    if(worker_processes<=0)
    	worker_processes = 1;
    value = cf->args->elts;
    *limit_number = ngx_atoi(value[1].data, value[1].len);
    number = *limit_number;
    if (*limit_number == NGX_ERROR) {
    	return "invalid number";
    }
    if(*limit_number <=0 || *limit_number <= worker_processes){
    	return "number is too small, should be more than worker processes.";
    }
    //ctx
    ctx = ngx_pcalloc(cf->pool, sizeof(ngx_http_limit_req2_ctx_t));
    if (ctx == NULL) {
        	return NGX_CONF_ERROR;
    }
    ngx_str_t ctx_name = ngx_string("ctx");
    shm_zone = ngx_shared_memory_add(cf, &ctx_name, 160 * ngx_pagesize,&ngx_http_limit_req2_module);
    if (shm_zone == NULL) {
        return NGX_CONF_ERROR;
    }
    if (shm_zone->data) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,"shm_zone has already has value.");
        return NGX_CONF_ERROR;
    }
    shm_zone->init = ngx_http_limit_req2_init_shm_zone;
    shm_zone->data = ctx;
    lrlc->ctx_shm_zone = shm_zone;


    //lock
    lock = ngx_pcalloc(cf->pool, sizeof(ngx_http_limit_req2_lock_t));
    if (lock == NULL) {
       	return NGX_CONF_ERROR;
    }
    ngx_str_t lock_name = ngx_string("lock");
    lock_shm_zone = ngx_shared_memory_add(cf, &lock_name, 250 * ngx_pagesize,&ngx_http_limit_req2_module);
    if (lock_shm_zone == NULL) {
        return NGX_CONF_ERROR;
    }
    if (lock_shm_zone->data) {
         ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,"shm_zone lock has already has value.");
         return NGX_CONF_ERROR;
    }
    lock_shm_zone->init = ngx_http_limit_req2_init_lock_shm_zone;
    lock_shm_zone->data = lock;
    lrlc->lock_shm_zone = lock_shm_zone;

    return NGX_CONF_OK;

}

static ngx_int_t
ngx_http_limit_req2_init_shm_zone (ngx_shm_zone_t *shm_zone, void *data){
	ngx_int_t len;
	ngx_http_limit_req2_ctx_t  *octx = data;
    ngx_http_limit_req2_ctx_t  *ctx;
    ctx = shm_zone->data;

    if (octx) {
        ctx->shm = octx->shm;
        ctx->shpool = octx->shpool;
        return NGX_OK;
    }

    ctx->shpool = (ngx_slab_pool_t *) shm_zone->shm.addr;
    if (shm_zone->shm.exists) {
        ctx->shm = ctx->shpool->data;
        return NGX_OK;
    }

    ctx->shm = ngx_slab_alloc(ctx->shpool, worker_processes * sizeof(ngx_http_limit_req2_shctx_t));
    if (ctx->shm == NULL) {
        return NGX_ERROR;
    }
    
    ctx->shm->buffer = ngx_slab_alloc(ctx->shpool, worker_processes * sizeof(ngx_http_limit_req2_shctx_buffer));
    if (ctx->shm->buffer == NULL) {
        return NGX_ERROR;
    }
    ctx->shm->last_used_pos = 0;
//    ctx->shm->buffer->processnumber = number / worker_processes;
//    printf("processnumber:%ld\n",ctx->shm->buffer->processnumber);

    ctx->shpool->data = ctx->shm;
    len = sizeof("in limit req2") + shm_zone->shm.name.len;
    ctx->shpool->log_ctx = ngx_slab_alloc(ctx->shpool, len);
    if (ctx->shpool->log_ctx == NULL) {
        return NGX_ERROR;
    }
    ngx_sprintf(ctx->shpool->log_ctx, "in limit req2");
    ctx->shpool->log_nomem = 0;

    return NGX_OK;
}

static ngx_int_t
ngx_http_limit_req2_init_lock_shm_zone (ngx_shm_zone_t *shm_zone, void *data){
    ngx_http_limit_req2_lock_t  *olock = data;
    ngx_http_limit_req2_lock_t  *lock;
    lock = shm_zone->data;
    if (olock) {
        lock->shpool = olock->shpool;
        return NGX_OK;
    }
    lock->shpool = (ngx_slab_pool_t *) shm_zone->shm.addr;
    if (shm_zone->shm.exists) {
        lock->shpool = lock->shpool->data;
        return NGX_OK;
    }
    return NGX_OK;
}


static ngx_int_t
ngx_http_limit_req2_init(ngx_conf_t *cf){
    ngx_http_handler_pt        *h;
    ngx_http_handler_pt        *h2;
    ngx_http_core_main_conf_t  *cmcf;
    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);
    h = ngx_array_push(&cmcf->phases[NGX_HTTP_ACCESS_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }
    *h = ngx_http_limit_req2_access_handler;

    h2= ngx_array_push(&cmcf->phases[NGX_HTTP_LOG_PHASE].handlers);
    if (h2 == NULL) {
        return NGX_ERROR;
    }
    *h2 = ngx_http_limit_req2_log_handler;
    return NGX_OK;
}




