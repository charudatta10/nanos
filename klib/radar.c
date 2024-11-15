#include <kernel.h>
#include <http.h>
#include <lwip.h>
#include <pagecache.h>
#include <storage.h>
#include <fs.h>
#include <tls.h>

#define RADAR_HOSTNAME  "radar.relayered.net"
#define RADAR_PORT      443

#define RADAR_CA_CERT   "-----BEGIN CERTIFICATE-----\n\
MIIE0DCCA7igAwIBAgIBBzANBgkqhkiG9w0BAQsFADCBgzELMAkGA1UEBhMCVVMx\
EDAOBgNVBAgTB0FyaXpvbmExEzARBgNVBAcTClNjb3R0c2RhbGUxGjAYBgNVBAoT\
EUdvRGFkZHkuY29tLCBJbmMuMTEwLwYDVQQDEyhHbyBEYWRkeSBSb290IENlcnRp\
ZmljYXRlIEF1dGhvcml0eSAtIEcyMB4XDTExMDUwMzA3MDAwMFoXDTMxMDUwMzA3\
MDAwMFowgbQxCzAJBgNVBAYTAlVTMRAwDgYDVQQIEwdBcml6b25hMRMwEQYDVQQH\
EwpTY290dHNkYWxlMRowGAYDVQQKExFHb0RhZGR5LmNvbSwgSW5jLjEtMCsGA1UE\
CxMkaHR0cDovL2NlcnRzLmdvZGFkZHkuY29tL3JlcG9zaXRvcnkvMTMwMQYDVQQD\
EypHbyBEYWRkeSBTZWN1cmUgQ2VydGlmaWNhdGUgQXV0aG9yaXR5IC0gRzIwggEi\
MA0GCSqGSIb3DQEBAQUAA4IBDwAwggEKAoIBAQC54MsQ1K92vdSTYuswZLiBCGzD\
BNliF44v/z5lz4/OYuY8UhzaFkVLVat4a2ODYpDOD2lsmcgaFItMzEUz6ojcnqOv\
K/6AYZ15V8TPLvQ/MDxdR/yaFrzDN5ZBUY4RS1T4KL7QjL7wMDge87Am+GZHY23e\
cSZHjzhHU9FGHbTj3ADqRay9vHHZqm8A29vNMDp5T19MR/gd71vCxJ1gO7GyQ5HY\
pDNO6rPWJ0+tJYqlxvTV0KaudAVkV4i1RFXULSo6Pvi4vekyCgKUZMQWOlDxSq7n\
eTOvDCAHf+jfBDnCaQJsY1L6d8EbyHSHyLmTGFBUNUtpTrw700kuH9zB0lL7AgMB\
AAGjggEaMIIBFjAPBgNVHRMBAf8EBTADAQH/MA4GA1UdDwEB/wQEAwIBBjAdBgNV\
HQ4EFgQUQMK9J47MNIMwojPX+2yz8LQsgM4wHwYDVR0jBBgwFoAUOpqFBxBnKLbv\
9r0FQW4gwZTaD94wNAYIKwYBBQUHAQEEKDAmMCQGCCsGAQUFBzABhhhodHRwOi8v\
b2NzcC5nb2RhZGR5LmNvbS8wNQYDVR0fBC4wLDAqoCigJoYkaHR0cDovL2NybC5n\
b2RhZGR5LmNvbS9nZHJvb3QtZzIuY3JsMEYGA1UdIAQ/MD0wOwYEVR0gADAzMDEG\
CCsGAQUFBwIBFiVodHRwczovL2NlcnRzLmdvZGFkZHkuY29tL3JlcG9zaXRvcnkv\
MA0GCSqGSIb3DQEBCwUAA4IBAQAIfmyTEMg4uJapkEv/oV9PBO9sPpyIBslQj6Zz\
91cxG7685C/b+LrTW+C05+Z5Yg4MotdqY3MxtfWoSKQ7CC2iXZDXtHwlTxFWMMS2\
RJ17LJ3lXubvDGGqv+QqG+6EnriDfcFDzkSnE3ANkR/0yBOtg2DZ2HKocyQetawi\
DsoXiWJYRBuriSUBAA/NxBti21G00w9RKpv0vHP8ds42pM3Z2Czqrpv1KrKQ0U11\
GIo/ikGQI31bS/6kA1ibRrLDYGCD+H1QQc7CoZDDu+8CL9IVVO5EFdkKrqeKM+2x\
LXY2JtwE65/3YR8V3Idv7kaWKK2hJn0KCacuBKONvPi8BDAB\
-----END CERTIFICATE-----"

#define RADAR_STATS_INTERVAL    seconds(60)
#define RADAR_STATS_BATCH_SIZE  5

static struct telemetry {
    heap h;
    heap phys;
    tuple env;
    buffer auth_header;
    klog_dump dump;
    boolean dump_done;
    u64 boot_id;
    boolean running;
    timestamp retry_backoff;
    struct timer retry_timer;
    closure_struct(timer_handler, retry_func);
    struct timer stats_timer;
    closure_struct(timer_handler, stats_func);
    u64 stats_mem_used[RADAR_STATS_BATCH_SIZE];
    int stats_count;
} telemetry;

static void telemetry_crash_report(void);
static void telemetry_boot(void);
static void telemetry_stats_send(void);

closure_function(2, 0, void, telemetry_connect,
                 ip_addr_t, addr, connection_handler, ch)
{
    connection_handler ch = bound(ch);
    if (tls_connect(&bound(addr), RADAR_PORT, ch) < 0)
        apply(ch, 0);
    closure_finish();
}

static void telemetry_dns_cb(sstring name, const ip_addr_t *ipaddr, void *callback_arg)
{
    connection_handler ch = callback_arg;
    if (!ipaddr) {
        msg_err("Radar: failed to look up server hostname");
        goto error;
    }
    thunk t = closure(telemetry.h, telemetry_connect, *ipaddr, ch);
    if (t != INVALID_ADDRESS) {
        async_apply(t);
        return;
    }
error:
    apply(ch, 0);
}

closure_func_basic(timer_handler, void, retry_timer_func,
                   u64 expiry, u64 overruns)
{
    if (overruns == timer_disabled)
        return;
    if (telemetry.dump)
        telemetry_crash_report();
    else
        telemetry_boot();
}

static void telemetry_retry(void)
{
    register_timer(kernel_timers, &telemetry.retry_timer, CLOCK_ID_MONOTONIC, telemetry.retry_backoff, false, 0,
            init_closure_func(&telemetry.retry_func, timer_handler, retry_timer_func));
    if (telemetry.retry_backoff < seconds(600))
        telemetry.retry_backoff <<= 1;
}

static boolean telemetry_req(sstring url, buffer data, buffer_handler bh)
{
    tuple req = allocate_tuple();
    if (req == INVALID_ADDRESS)
        return false;
    set(req, sym(url), alloca_wrap_sstring(url));
    set(req, sym(Host), alloca_wrap_cstring(RADAR_HOSTNAME));
    set(req, sym(RADAR-KEY), telemetry.auth_header);
    set(req, sym(Content-Type), alloca_wrap_cstring("application/json"));
    status s = http_request(telemetry.h, bh, HTTP_REQUEST_METHOD_POST, req, data);
    deallocate_value(req);
    if (is_ok(s)) {
        return true;
    } else {
        timm_dealloc(s);
        return false;
    }
}

closure_function(2, 1, boolean, telemetry_recv,
                 value_handler, vh, buffer_handler, out,
                 buffer data)
{
    if (data) {
        value_handler vh = bound(vh);
        if (vh) {
            buffer_handler parser = allocate_http_parser(telemetry.h, vh);
            if (parser != INVALID_ADDRESS) {
                status s = apply(parser, data);
                if (!is_ok(s)) {
                    msg_err("Radar: failed to parse HTTP response: %v", s);
                    timm_dealloc(s);
                }
            } else {
                msg_err("Radar: failed to allocate HTTP parser");
                apply(vh, 0);
            }
        }
        apply(bound(out), 0);   /* close connection */
        return true;
    } else {  /* connection closed */
        closure_finish();
        if (telemetry.dump) {
            if (telemetry.dump_done) {
                /* We just sent a crash report: clear the log dump (so that it's not
                 * sent again at the next boot), then send a boot event. */
                klog_dump_clear();
                deallocate(telemetry.h, telemetry.dump, sizeof(*telemetry.dump));
                telemetry.dump = 0;
                telemetry_boot();
            } else {
                telemetry_retry();
            }
        } else if (!telemetry.running) {
            if (telemetry.boot_id) {
                /* The boot event has been sent: start collecting usage metrics. */
                for (int count = 0; count < RADAR_STATS_BATCH_SIZE; count++)
                    telemetry.stats_mem_used[count] = heap_allocated(telemetry.phys);
                telemetry_stats_send();
                telemetry.stats_count = 0;
                register_timer(kernel_timers, &telemetry.stats_timer, CLOCK_ID_MONOTONIC, RADAR_STATS_INTERVAL, false,
                        RADAR_STATS_INTERVAL, (timer_handler)&telemetry.stats_func);
                telemetry.running = true;
            } else {
                telemetry_retry();
            }
        }
    }
    return false;
}

closure_function(3, 1, input_buffer_handler, telemetry_ch,
                 sstring, url, buffer, data, value_handler, vh,
                 buffer_handler out)
{
    buffer data = bound(data);
    input_buffer_handler in = INVALID_ADDRESS;
    if (out) {
        boolean success = telemetry_req(bound(url), data, out);
        if (success)
            in = closure(telemetry.h, telemetry_recv, bound(vh), out);
        else
            deallocate_buffer(data);
    } else {    /* connection failed */
        deallocate_buffer(data);
        if (!telemetry.running)
            telemetry_retry();
    }
    closure_finish();
    return in;
}

boolean telemetry_send(sstring url, buffer data, value_handler vh)
{
    connection_handler ch;
    ch = closure(telemetry.h, telemetry_ch, url, data, vh);
    if (ch == INVALID_ADDRESS)
        return false;
    ip_addr_t radar_addr;
    err_t err = dns_gethostbyname(ss(RADAR_HOSTNAME), &radar_addr, telemetry_dns_cb, ch);
    switch (err) {
    case ERR_OK:
        if (tls_connect(&radar_addr, RADAR_PORT, ch) == 0)
            return true;
        break;
    case ERR_INPROGRESS:
        return true;
    }
    deallocate_closure(ch);
    return false;
}

static void telemetry_print_env(buffer b)
{
    /* Assumes that the buffer already contains at least one JSON attribute
     * (hence the initial comma in the strings below). */
    buffer nanos_ver = get(telemetry.env, sym(NANOS_VERSION));
    if (nanos_ver)
        bprintf(b, ",\"nanosVersion\":\"%b\"", nanos_ver);
    buffer ops_ver = get(telemetry.env, sym(OPS_VERSION));
    if (ops_ver)
        bprintf(b, ",\"opsVersion\":\"%b\"", ops_ver);
    buffer image_name = get(telemetry.env, sym(RADAR_IMAGE_NAME));
    if (image_name)
        bprintf(b, ",\"imageName\":\"%b\"", image_name);
}

closure_func_basic(value_handler, void, telemetry_crash_recv,
                   value v)
{
    if (v) {
        value resp = get(v, sym(start_line));
        if (resp) {
            buffer word;
            for (u64 i = 0; (word = get(resp, integer_key(i))); i++)
                if (buffer_strstr(word, ss("OK")) == 0) {
                    telemetry.dump_done = true;
                    break;
                }
        }
    }
    closure_finish();
}

static void telemetry_crash_report(void)
{
    buffer b = allocate_buffer(telemetry.h, PAGESIZE);
    if (b == INVALID_ADDRESS)
        goto error;
    value_handler vh = closure_func(telemetry.h, value_handler, telemetry_crash_recv);
    if (vh == INVALID_ADDRESS) {
        goto err_free_buf;
    }
    bprintf(b, "{\"bootID\":%ld", telemetry.dump->boot_id);
    telemetry_print_env(b);
    buffer_write_cstring(b, ",\"dump\":\"");
    for (int i = 0; (i < sizeof(telemetry.dump->msgs)) && telemetry.dump->msgs[i]; i++) {
        /* Escape JSON special characters. */
        char c = telemetry.dump->msgs[i];
        switch (c) {
        case '\n':
            buffer_write_cstring(b, "\\n");
            break;
        case '"':
            buffer_write_cstring(b, "\\\"");
            break;
        case '/':
            buffer_write_cstring(b, "\\/");
            break;
        case '\\':
            buffer_write_cstring(b, "\\\\");
            break;
        case '\t':
            buffer_write_cstring(b, "\\t");
            break;
        case '\r':
            buffer_write_cstring(b, "\\r");
            break;
        case '\b':
            buffer_write_cstring(b, "\\b");
            break;
        case '\f':
            buffer_write_cstring(b, "\\f");
            break;
        default:
            buffer_write(b, &c, 1);
        }
    }
    buffer_write_cstring(b, "\"}\r\n");
    if (!telemetry_send(ss("/api/v1/crashes"), b, vh)) {
        goto err_free_buf;
    }
    return;
  err_free_buf:
    deallocate_buffer(b);
  error:
    telemetry_retry();
}

closure_func_basic(value_handler, void, telemetry_boot_recv,
                   value v)
{
    telemetry.boot_id = 0;
    if (!v) /* couldn't allocate HTTP parser */
        return;
    buffer content = get(v, sym(content));
    if (content) {
        int index = buffer_strstr(content, ss("\"id\""));
        if (index < 0)
            goto exit;
        buffer_consume(content, index);
        buffer_consume(content, buffer_strchr(content, ':') + 1);
        index = buffer_strchr(content, ',');
        if (index < 0) {
            index = buffer_strchr(content, '}');
            if (index < 0)
                goto exit;
        }
        parse_int(alloca_wrap_buffer(buffer_ref(content, 0), index), 10, &telemetry.boot_id);
        klog_set_boot_id(telemetry.boot_id);
    }
  exit:
    closure_finish();
}

static void telemetry_boot(void)
{
    struct netif *netif = netif_get_default();
    if (!netif)
        goto error;
    buffer b = allocate_buffer(telemetry.h, 64);
    if (b == INVALID_ADDRESS)
        goto error;
    value_handler vh = closure_func(telemetry.h, value_handler, telemetry_boot_recv);
    if (vh == INVALID_ADDRESS) {
        goto err_free_buf;
    }
    char addr[IPADDR_STRLEN_MAX];
    bprintf(b, "{\"privateIP\":\"%s\"",
            isstring(addr, ipaddr_ntoa_r(&netif->ip_addr, addr, sizeof(addr))));
    telemetry_print_env(b);
    buffer_write_cstring(b, "}\r\n");
    if (!telemetry_send(ss("/api/v1/boots"), b, vh)) {
        deallocate_closure(vh);
        goto err_free_buf;
    }
    netif_unref(netif);
    return;
  err_free_buf:
    deallocate_buffer(b);
  error:
    if (netif)
        netif_unref(netif);
    telemetry_retry();
}

closure_function(2, 4, void, telemetry_vh,
                 buffer, b, int, count,
                 u8 *uuid, sstring label, filesystem fs, inode mount_point)
{
    u64 block_size = fs_blocksize(fs);
    buffer b = bound(b);
    bprintf(b, "%s{\"volume\":\"", (bound(count) == 0) ? sstring_empty() : ss(","));
    if (!sstring_is_empty(label))
        bprintf(b, "%s", label);
    else
        print_uuid(b, uuid);
    bprintf(b, "\",\"used\":%ld,\"total\":%ld}", fs_usedblocks(fs) * block_size,
            fs_totalblocks(fs) * block_size);
    bound(count)++;
}

static void telemetry_stats_send(void)
{
    buffer b = allocate_buffer(telemetry.h, 128);
    if (b == INVALID_ADDRESS) {
        msg_err("%s: failed to allocate buffer", func_ss);
        return;
    }
    bprintf(b, "{\"bootID\":%ld,\"memUsed\":[", telemetry.boot_id);
    for (int i = 0; i < RADAR_STATS_BATCH_SIZE; i++)
        bprintf(b, "%ld%s", telemetry.stats_mem_used[i],
                (i < RADAR_STATS_BATCH_SIZE - 1) ? ss(",") : sstring_empty());
    buffer_write_cstring(b, "],\"diskUsage\":[");
    storage_iterate(stack_closure(telemetry_vh, b, 0));
    buffer_write_cstring(b, "]}\r\n");
    if (!telemetry_send(ss("/api/v1/machine-stats"), b, 0)) {
        msg_err("Radar: failed to send stats");
        deallocate_buffer(b);
    }
}

closure_func_basic(timer_handler, void, telemetry_stats,
                   u64 expiry, u64 overruns)
{
    if (overruns == timer_disabled)
        return;
    telemetry.stats_mem_used[telemetry.stats_count++] = heap_allocated(telemetry.phys);
    if (telemetry.stats_count == RADAR_STATS_BATCH_SIZE) {
        telemetry.stats_count = 0;
        telemetry_stats_send();
    }
}

closure_func_basic(status_handler, void, klog_dump_loaded,
                   status s)
{
    if (is_ok(s)) {
        if (telemetry.dump->exit_code != 0) {
            telemetry_crash_report();
        } else {
            deallocate(telemetry.h, telemetry.dump, sizeof(*telemetry.dump));
            telemetry.dump = 0;
            telemetry_boot();
        }
    } else
        timm_dealloc(s);
    closure_finish();
}

int init(status_handler complete)
{
    kernel_heaps kh = get_kernel_heaps();
    telemetry.h = heap_locked(kh);
    telemetry.phys = (heap)heap_physical(kh);
    if (tls_set_cacert(RADAR_CA_CERT, sizeof(RADAR_CA_CERT)) != 0) {
        msg_err("Radar: failed to set CA certificate");
        return KLIB_INIT_FAILED;
    }
    telemetry.env = get_environment();
    telemetry.auth_header = get(telemetry.env, sym(RADAR_KEY));
    telemetry.retry_backoff = seconds(1);
    init_timer(&telemetry.retry_timer);
    telemetry.running = false;
    init_timer(&telemetry.stats_timer);
    init_closure_func(&telemetry.stats_func, timer_handler, telemetry_stats);
    telemetry.dump = allocate(telemetry.h, sizeof(*telemetry.dump));
    if (telemetry.dump == INVALID_ADDRESS) {
        msg_err("Radar: failed to allocate log dump");
        return KLIB_INIT_FAILED;
    }
    status_handler sh = closure_func(telemetry.h, status_handler, klog_dump_loaded);
    if (sh == INVALID_ADDRESS) {
        msg_err("Radar: failed to allocate log dump load handler");
        deallocate(telemetry.h, telemetry.dump, sizeof(*telemetry.dump));
        return KLIB_INIT_FAILED;
    }
    klog_load(telemetry.dump, sh);
    return KLIB_INIT_OK;
}
