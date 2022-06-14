#include <fcntl.h>

#include <msgpack.h>
#include <chunkio/chunkio.h>

#include <fluent-bit/flb_input_chunk.h>
#include <fluent-bit/flb_input.h>
#include <fluent-bit/flb_output.h>
#include <fluent-bit/flb_trace_chunk.h>
#include <fluent-bit/flb_pack.h>
#include <fluent-bit/flb_base64.h>
#include <fluent-bit/flb_storage.h>
#include <fluent-bit/flb_router.h>
#include <fluent-bit/flb_kv.h>


/* Register external function to emit records, check 'plugins/in_emitter' */
int in_emitter_add_record(const char *tag, int tag_len,
                          const char *buf_data, size_t buf_size,
                          struct flb_input_instance *in);

struct flb_trace_chunk *flb_trace_chunk_new(struct flb_input_chunk *chunk)
{
    struct flb_trace_chunk *trace;
    struct flb_input_instance *f_ins = (struct flb_input_instance *)chunk->in;

    trace = flb_calloc(1, sizeof(struct flb_trace_chunk));
    if (trace == NULL) {
        return NULL;
    }

    trace->ic = chunk;
    trace->trace_id = flb_sds_create("");
    flb_sds_printf(&trace->trace_id, "%s%d", f_ins->trace_ctxt->trace_prefix,
                   f_ins->trace_ctxt->trace_count++);
    return trace;
}

void flb_trace_chunk_destroy(struct flb_trace_chunk *trace)
{
    flb_free(trace);
}

struct flb_trace_chunk_context *flb_trace_chunk_context_new(struct flb_config *config, const char *output_name, const char *trace_prefix, struct mk_list *props)
{
    struct flb_input_instance *input;
    struct flb_output_instance *output;
    struct flb_trace_chunk_context *ctx;
    struct mk_list *head;
    struct flb_kv *prop;
    int ret;


    input = (void *)flb_input_new(config, "emitter", NULL, FLB_FALSE);
    if (input == NULL) {
        flb_error("could not load trace emitter");
        return NULL;
    }
    input->event_type = FLB_EVENT_TYPE_LOG | FLB_EVENT_TYPE_HAS_TRACE;
    ret = flb_input_set_property(input, "alias", "trace-emitter");
    if (ret != 0) {
        flb_error("unable to set alias for trace emitter");
        flb_input_instance_destroy(input);
        return NULL;
    }
    ret = flb_input_instance_init(input, config);
    if (ret == -1) {
        flb_error("cannot initialize trace emitter");
        flb_input_instance_destroy(input);
        return NULL;
    }
    /* Storage context */
    ret = flb_storage_input_create(config->cio, input);
    if (ret == -1) {
        return NULL;
    }
    output = flb_output_new(config, output_name, NULL, 0);
    if (output == NULL) {
        flb_error("could not create trace output");
        //flb_free(input);
        return NULL;
    }
    // this might be unnecessary given the direct routing
    //flb_output_set_property(output, "match", "*");

    if (props != NULL) {
        mk_list_foreach(head, props) {
            prop = mk_list_entry(head, struct flb_kv, _head);
            flb_output_set_property(output, prop->key, prop->val);
        }
    }

    ret = flb_output_instance_init(output, config);
    if (ret == -1) {
        flb_error("cannot initialize trace emitter output");
        flb_output_instance_destroy(output);
        return NULL;
    }

    ret = flb_router_connect_direct(input, output);
    if (ret != 0) {
        flb_error("unable to route traces");
        return NULL;
    }

    flb_router_connect(input, output);

    ctx = flb_calloc(1, sizeof(struct flb_trace_chunk_context));
    ctx->output = (void *)output;
    ctx->input = (void *)input;
    ctx->trace_prefix = flb_sds_create(trace_prefix);

    return ctx;
}

void flb_trace_chunk_context_destroy(struct flb_trace_chunk_context *ctxt)
{
    struct flb_output_instance *output = (struct flb_output_instance *)ctxt->output;
    struct flb_input_instance *input = (struct flb_input_instance *)ctxt->input;
    
    /*
    flb_input_instance_exit(input, input->config);
    flb_input_instance_destroy(input);

    // Stop any worker thread
    if (flb_output_is_threaded(output) == FLB_TRUE) {
        flb_output_thread_pool_destroy(output);
    }
    if (output->p->cb_exit != NULL)
    {
        output->p->cb_exit(output->context, output->config);
    }
    flb_output_instance_destroy(ctxt->output);
    */

    flb_free(ctxt);
}

int flb_trace_chunk_input(struct flb_trace_chunk *trace, char *buf, int buf_size)
{
    msgpack_packer mp_pck;
    msgpack_sbuffer mp_sbuf;
    struct flb_input_instance *input = (struct flb_input_instance *)trace->ic->in;
    int rc = -1;
    int slen;
    flb_sds_t tag = flb_sds_create("trace");
    unsigned char b64enc[102400];
    size_t bc64enclen;


    msgpack_sbuffer_init(&mp_sbuf);
    msgpack_packer_init(&mp_pck, &mp_sbuf, msgpack_sbuffer_write);

    msgpack_pack_array(&mp_pck, 2);
    flb_pack_time_now(&mp_pck);
    msgpack_pack_map(&mp_pck, 4);

    msgpack_pack_str_with_body(&mp_pck, "type", 4);
    msgpack_pack_int(&mp_pck, FLB_TRACE_CHUNK_TYPE_INPUT);

    msgpack_pack_str_with_body(&mp_pck, "trace_id", strlen("trace_id"));
    msgpack_pack_str_with_body(&mp_pck, trace->trace_id, strlen(trace->trace_id));

    msgpack_pack_str_with_body(&mp_pck, "input_instance", strlen("input_instance"));
    msgpack_pack_str_with_body(&mp_pck, input->name, strlen(input->name));

    msgpack_pack_str_with_body(&mp_pck, "content", strlen("content"));
    flb_base64_encode(b64enc, sizeof(b64enc)-1, &bc64enclen,
                    (unsigned char *)buf, buf_size);
    msgpack_pack_str_with_body(&mp_pck, b64enc, bc64enclen);

    in_emitter_add_record(tag, flb_sds_len(tag), mp_sbuf.data, mp_sbuf.size,
                          trace->ic->in->trace_ctxt->input);
sbuffer_error:
    flb_sds_destroy(tag);
    msgpack_sbuffer_destroy(&mp_sbuf);
    return rc;
}

int flb_trace_chunk_filter(struct flb_trace_chunk *tracer, void *pfilter, char *buf, int buf_size)
{
    msgpack_packer mp_pck;
    msgpack_sbuffer mp_sbuf;
    int rc = -1;
    struct flb_input_instance *input = (struct flb_input_instance *)tracer->ic->in;
    struct flb_filter_instance *filter = (struct flb_filter_instance *)pfilter;
    flb_sds_t tag = flb_sds_create("trace");
    struct flb_time tm;
    unsigned char b64enc[102400];
    size_t bc64enclen;


    if (tracer == NULL) {
        goto tracer_error;
    }

    msgpack_sbuffer_init(&mp_sbuf);
    msgpack_packer_init(&mp_pck, &mp_sbuf, msgpack_sbuffer_write);

    msgpack_pack_array(&mp_pck, 2);
    flb_pack_time_now(&mp_pck);
    msgpack_pack_map(&mp_pck, 6);

    msgpack_pack_str_with_body(&mp_pck, "type", strlen("type"));
    rc = msgpack_pack_int(&mp_pck, FLB_TRACE_CHUNK_TYPE_FILTER);
    if (rc == -1) {
        goto sbuffer_error;
    }

    flb_time_get(&tm);
    msgpack_pack_str_with_body(&mp_pck, "time", strlen("time"));
    msgpack_pack_double(&mp_pck, flb_time_to_double(&tm));

    msgpack_pack_str_with_body(&mp_pck, "trace_id", strlen("trace_id"));
    msgpack_pack_str_with_body(&mp_pck, tracer->trace_id, strlen(tracer->trace_id));

    
    msgpack_pack_str_with_body(&mp_pck, "filter_instance", strlen("filter_instance"));
    rc = msgpack_pack_str_with_body(&mp_pck, filter->name, strlen(filter->name));
    if (rc == -1) {
        goto sbuffer_error;
    }
    
    msgpack_pack_str_with_body(&mp_pck, "record_version", strlen("record_version"));
    rc = msgpack_pack_int(&mp_pck, tracer->tracer_versions++);
    if (rc == -1) {
        goto sbuffer_error;
    }

    flb_base64_encode(b64enc, sizeof(b64enc)-1, &bc64enclen, (unsigned char *)buf, buf_size);
    msgpack_pack_str_with_body(&mp_pck, "record", strlen("record"));
    msgpack_pack_str_with_body(&mp_pck, b64enc, bc64enclen);

    in_emitter_add_record(tag, flb_sds_len(tag), mp_sbuf.data, mp_sbuf.size,
                          tracer->ic->in->trace_ctxt->input);
    
    rc = 0;

sbuffer_error:
    msgpack_sbuffer_destroy(&mp_sbuf);
tracer_error:
    flb_sds_destroy(tag);
    return rc;
}