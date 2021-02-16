/*
 * ----------------------------------------------------------------------------
 * "THE BEER-WARE LICENSE" (Revision 42):
 * <quackmore-ff@yahoo.com> wrote this file.  As long as you retain this notice
 * you can do whatever you want with this stuff. If we meet some day, and you 
 * think this stuff is worth it, you can buy me a beer in return. Quackmore
 * ----------------------------------------------------------------------------
 */

// SDK includes
extern "C"
{
#include "ip_addr.h"
#include "mem.h"
#include "osapi.h"
#include "sntp.h"
#include "user_interface.h"
#include "espbot_hal.h"
}

#include "app_http_routes.hpp"
#include "espbot.hpp"
#include "espbot_cron.hpp"
#include "espbot_diagnostic.hpp"
#include "espbot_event_codes.h"
#include "espbot_global.hpp"
#include "espbot_http.hpp"
#include "espbot_http_routes.hpp"
#include "espbot_json.hpp"
#include "espbot_mem_mon.hpp"
#include "espbot_spiffs.hpp"
#include "espbot_timedate.hpp"
#include "espbot_utils.hpp"
#include "espbot_webserver.hpp"

static os_timer_t delay_timer;

void init_controllers(void)
{
    os_timer_disarm(&delay_timer);
}

static void getAPlist(void *param)
{
    struct espconn *ptr_espconn = (struct espconn *)param;
    ALL("getAPlist");
    char *scan_list = new char[40 + ((32 + 6) * Wifi::get_ap_count())];
    if (scan_list)
    {
        char *tmp_ptr;
        espmem.stack_mon();
        fs_sprintf(scan_list, "{\"AP_count\": %d,\"AP_SSIDs\":[", Wifi::get_ap_count());
        for (int idx = 0; idx < Wifi::get_ap_count(); idx++)
        {
            tmp_ptr = scan_list + os_strlen(scan_list);
            if (idx > 0)
                *(tmp_ptr++) = ',';
            os_sprintf(tmp_ptr, "\"%s\"", Wifi::get_ap_name(idx));
        }
        tmp_ptr = scan_list + os_strlen(scan_list);
        fs_sprintf(tmp_ptr, "]}");
        http_response(ptr_espconn, HTTP_OK, HTTP_CONTENT_JSON, scan_list, true);
        Wifi::free_ap_list();
    }
    else
    {
        esp_diag.error(ROUTES_GETAPLIST_HEAP_EXHAUSTED, (40 + ((32 + 6) * Wifi::get_ap_count())));
        ERROR("getAPlist heap exhausted %d", (32 + (40 + ((32 + 6) * Wifi::get_ap_count()))));
        // may be the list was too big but there is enough heap memory for a response
        http_response(ptr_espconn, HTTP_SERVER_ERROR, HTTP_CONTENT_JSON, f_str("Heap exhausted"), false);
        Wifi::free_ap_list();
    }
}

static const char *get_file_mime_type(char *filename)
{
    ALL("get_file_mime_type");
    char *ptr;
    ptr = (char *)os_strstr(filename, f_str("."));
    if (ptr == NULL)
        return f_str("application/octet-stream");
    else
    {
        if (os_strcmp(ptr, f_str(".css")) == 0)
            return f_str("text/css");
        else if (os_strcmp(ptr, f_str(".html")) == 0)
            return f_str("text/html");
        else if (os_strcmp(ptr, f_str(".js")) == 0)
            return f_str("text/javascript");
        else if (os_strcmp(ptr, f_str(".cfg")) == 0)
            return f_str("text/plain");
        else if (os_strcmp(ptr, f_str(".log")) == 0)
            return f_str("text/plain");
        else if (os_strcmp(ptr, f_str(".prg")) == 0)
            return f_str("text/plain");
        else if (os_strcmp(ptr, f_str(".txt")) == 0)
            return f_str("text/plain");
        else if (os_strcmp(ptr, f_str(".jpeg")) == 0)
            return f_str("image/jpeg");
        else if (os_strcmp(ptr, f_str(".jpg")) == 0)
            return f_str("image/jpeg");
        else if (os_strcmp(ptr, f_str(".png")) == 0)
            return f_str("image/png");
        else
            return f_str("application/octet-stream");
    }
}

static void send_remaining_file(struct http_split_send *p_sr)
{
    ALL("send_remaining_file");
    if (!http_espconn_in_use(p_sr->p_espconn))
    {
        TRACE("send_remaining_file espconn %X state %d, abort", p_sr->p_espconn, p_sr->p_espconn->state);
        delete[] p_sr->content;
        // there will be no send, so trigger a check of pending send
        system_os_post(USER_TASK_PRIO_0, SIG_HTTP_CHECK_PENDING_RESPONSE, '0');
        return;
    }

    int remaining_size = p_sr->content_size - p_sr->content_transferred;
    if (remaining_size > get_http_msg_max_size())
    {
        // the remaining file size is bigger than response_max_size
        // will split the remaining file over multiple messages
        int buffer_size = get_http_msg_max_size();
        Heap_chunk buffer(buffer_size, dont_free);
        struct http_split_send *p_pending_response = new struct http_split_send;
        if ((buffer.ref == NULL) || (p_pending_response == NULL))
        {
            delete[] p_sr->content;
            int mem_size = sizeof(struct http_split_send);
            if (buffer.ref == NULL)
            {
                mem_size = buffer_size;
                delete[] buffer.ref;
            }
            esp_diag.error(ROUTES_SEND_REMAINING_MSG_HEAP_EXHAUSTED, mem_size);
            ERROR("send_remaining_file heap exhausted %d", mem_size);
            http_response(p_sr->p_espconn, HTTP_SERVER_ERROR, HTTP_CONTENT_JSON, f_str("Heap exhausted"), false);
            return;
        }
        Espfile sel_file(p_sr->content);
        int res = sel_file.n_read(buffer.ref, p_sr->content_transferred, buffer_size);
        if (res < SPIFFS_OK)
        {
            delete[] p_sr->content;
            delete[] buffer.ref;
            delete p_pending_response;
            http_response(p_sr->p_espconn, HTTP_SERVER_ERROR, HTTP_CONTENT_JSON, f_str("Error reading file"), false);
            return;
        }

        // setup the remaining message
        p_pending_response->p_espconn = p_sr->p_espconn;
        p_pending_response->order = p_sr->order + 1;
        p_pending_response->content = p_sr->content;
        p_pending_response->content_size = p_sr->content_size;
        p_pending_response->content_transferred = p_sr->content_transferred + buffer_size;
        p_pending_response->action_function = send_remaining_file;
        Queue_err result = pending_split_send->push(p_pending_response);
        if (result == Queue_full)
        {
            delete[] p_sr->content;
            delete[] buffer.ref;
            delete p_pending_response;
            esp_diag.error(ROUTES_SEND_REMAINING_MSG_PENDING_RES_QUEUE_FULL);
            ERROR("send_remaining_file full pending res queue");
            return;
        }
        TRACE("send_remaining_file: *p_espconn: %X, msg (splitted) len: %d",
              p_sr->p_espconn, buffer_size);
        http_send_buffer(p_sr->p_espconn, p_sr->order, buffer.ref, buffer_size);
    }
    else
    {
        // this is the last piece of the message
        Heap_chunk buffer(remaining_size, dont_free);
        if (buffer.ref == NULL)
        {
            esp_diag.error(ROUTES_SEND_REMAINING_MSG_HEAP_EXHAUSTED, remaining_size);
            ERROR("send_remaining_file heap exhausted %d", remaining_size);
            http_response(p_sr->p_espconn, HTTP_SERVER_ERROR, HTTP_CONTENT_JSON, f_str("Heap exhausted"), false);
            delete[] p_sr->content;
            return;
        }
        Espfile sel_file(p_sr->content);
        int res = sel_file.n_read(buffer.ref, p_sr->content_transferred, remaining_size);
        if (res < SPIFFS_OK)
        {
            delete[] p_sr->content;
            http_response(p_sr->p_espconn, HTTP_SERVER_ERROR, HTTP_CONTENT_JSON, f_str("Error reading file"), false);
            return;
        }
        TRACE("send_remaining_file: *p_espconn: %X, msg (splitted) len: %d",
              p_sr->p_espconn, remaining_size);
        http_send_buffer(p_sr->p_espconn, p_sr->order, buffer.ref, remaining_size);
        delete[] p_sr->content;
    }
}

void return_file(struct espconn *p_espconn, Http_parsed_req *parsed_req, char *filename)
{
    ALL("return_file");
    int file_size = Espfile::size(filename);
    // let's start with the header
    Http_header header;
    header.m_code = HTTP_OK;
    header.m_content_type = (char *)get_file_mime_type(filename);
    header.m_content_length = file_size;
    header.m_content_range_start = 0;
    header.m_content_range_end = 0;
    header.m_content_range_total = 0;
    if (parsed_req->origin)
    {
        header.m_origin = new char[(os_strlen(parsed_req->origin) + 1)];
        if (header.m_origin == NULL)
        {
            esp_diag.error(ROUTES_RETURN_FILE_HEAP_EXHAUSTED, (os_strlen(parsed_req->origin) + 1));
            ERROR("return_file heap exhausted %d", (os_strlen(parsed_req->origin) + 1));
            http_response(p_espconn, HTTP_SERVER_ERROR, HTTP_CONTENT_JSON, f_str("Heap exhausted"), false);
            return;
        }
        os_strcpy(header.m_origin, parsed_req->origin);
    }
    char *header_str = http_format_header(&header);
    if (header_str == NULL)
    {
        esp_diag.error(ROUTES_RETURN_FILE_HEAP_EXHAUSTED, (os_strlen(header_str)));
        ERROR("return_file heap exhausted %d", (os_strlen(header_str)));
        http_response(p_espconn, HTTP_SERVER_ERROR, HTTP_CONTENT_JSON, f_str("Heap exhausted"), false);
        return;
    }
    // ok send the header
    http_send_buffer(p_espconn, 0, header_str, os_strlen(header_str));
    // and now the file
    if (file_size == 0)
        // the file in empty => nothing to do
        return;
    if (file_size > get_http_msg_max_size())
    {
        // will split the file over multiple messages
        // each the size of http_msg_max_size
        int buffer_size = get_http_msg_max_size();
        Heap_chunk buffer(buffer_size, dont_free);
        Heap_chunk filename_copy(os_strlen(filename), dont_free);
        struct http_split_send *p_pending_response = new struct http_split_send;
        if ((buffer.ref == NULL) || (filename_copy.ref == NULL) || (p_pending_response == NULL))
        {
            int mem_size;
            if (buffer.ref == NULL)
            {
                mem_size = get_http_msg_max_size();
            }
            else if (filename_copy.ref == NULL)
            {
                delete[] buffer.ref;
                mem_size = os_strlen(filename);
            }
            else
            {
                delete[] buffer.ref;
                delete[] filename_copy.ref;
                mem_size = sizeof(struct http_split_send);
            }
            esp_diag.error(ROUTES_RETURN_FILE_HEAP_EXHAUSTED, mem_size);
            ERROR("return_file heap exhausted %d", mem_size);
            http_response(p_espconn, HTTP_SERVER_ERROR, HTTP_CONTENT_JSON, f_str("Heap exhausted"), false);
            return;
        }
        os_strncpy(filename_copy.ref, filename, os_strlen(filename));
        Espfile sel_file(filename);
        int res = sel_file.n_read(buffer.ref, buffer_size);
        if (res < SPIFFS_OK)
        {
            delete[] buffer.ref;
            delete[] filename_copy.ref;
            delete p_pending_response;
            http_response(p_espconn, HTTP_SERVER_ERROR, HTTP_CONTENT_JSON, f_str("Error reading file"), false);
            return;
        }
        // setup the remaining message
        p_pending_response->p_espconn = p_espconn;
        p_pending_response->order = 2;
        p_pending_response->content = filename_copy.ref;
        p_pending_response->content_size = file_size;
        p_pending_response->content_transferred = buffer_size;
        p_pending_response->action_function = send_remaining_file;
        Queue_err result = pending_split_send->push(p_pending_response);
        if (result == Queue_full)
        {
            delete[] buffer.ref;
            delete[] filename_copy.ref;
            delete p_pending_response;
            esp_diag.error(ROUTES_RETURN_FILE_PENDING_RES_QUEUE_FULL);
            ERROR("return_file full pending response queue");
            return;
        }
        // send the file piece
        TRACE("return_file *p_espconn: %X, msg (splitted) len: %d", p_espconn, buffer_size);
        http_send_buffer(p_espconn, 1, buffer.ref, buffer_size);
        espmem.stack_mon();
    }
    else
    {
        // no need to split the file over multiple messages
        Heap_chunk buffer(file_size, dont_free);
        if (buffer.ref == NULL)
        {
            esp_diag.error(ROUTES_RETURN_FILE_HEAP_EXHAUSTED, file_size);
            ERROR("return_file heap exhausted %d", file_size);
            http_response(p_espconn, HTTP_SERVER_ERROR, HTTP_CONTENT_JSON, f_str("Heap exhausted"), false);
            return;
        }
        Espfile sel_file(filename);
        int res = sel_file.n_read(buffer.ref, file_size);
        if (res < SPIFFS_OK)
        {
            http_response(p_espconn, HTTP_SERVER_ERROR, HTTP_CONTENT_JSON, f_str("Error reading file"), false);
            return;
        }
        TRACE("return_file *p_espconn: %X, msg (full) len: %d", p_espconn, file_size);
        http_send_buffer(p_espconn, 1, buffer.ref, file_size);
    }
}

void preflight_response(struct espconn *p_espconn, Http_parsed_req *parsed_req)
{
    ALL("preflight_response");
    Http_header header;
    header.m_code = HTTP_OK;
    header.m_content_type = (char *)get_file_mime_type(HTTP_CONTENT_JSON);
    if (parsed_req->origin)
    {
        header.m_origin = new char[os_strlen(parsed_req->origin) + 1];
        if (header.m_origin == NULL)
        {
            esp_diag.error(ROUTES_PREFLIGHT_RESPONSE_HEAP_EXHAUSTED, (os_strlen(parsed_req->origin) + 1));
            http_response(p_espconn, HTTP_SERVER_ERROR, HTTP_CONTENT_JSON, f_str("Heap exhausted"), false);
            return;
        }
        os_strcpy(header.m_origin, parsed_req->origin);
    }
    else
    {
        header.m_origin = NULL;
    }
    if (parsed_req->acrh)
    {
        header.m_acrh = new char[os_strlen(parsed_req->acrh) + 1];
        if (header.m_acrh == NULL)
        {
            esp_diag.error(ROUTES_PREFLIGHT_RESPONSE_HEAP_EXHAUSTED, (os_strlen(parsed_req->acrh) + 1));
            http_response(p_espconn, HTTP_SERVER_ERROR, HTTP_CONTENT_JSON, f_str("Heap exhausted"), false);
            return;
        }
        os_strcpy(header.m_acrh, parsed_req->acrh);
    }
    else
    {
        header.m_acrh = NULL;
    }
    header.m_content_length = 0;
    header.m_content_range_start = 0;
    header.m_content_range_end = 0;
    header.m_content_range_total = 0;
    char *header_str = http_format_header(&header);
    if (header_str == NULL)
    {
        esp_diag.error(ROUTES_PREFLIGHT_RESPONSE_HEAP_EXHAUSTED);
        http_response(p_espconn, HTTP_SERVER_ERROR, HTTP_CONTENT_JSON, f_str("Heap exhausted"), false);
        return;
    }
    // ok send the header
    http_send_buffer(p_espconn, 0, header_str, os_strlen(header_str));
}

static void getCron(struct espconn *ptr_espconn, Http_parsed_req *parsed_req)
{
    ALL("getCron");
    char *msg = cron_state_json_stringify();
    if (msg)
        http_response(ptr_espconn, HTTP_OK, HTTP_CONTENT_JSON, msg, true);
    else
        http_response(ptr_espconn, HTTP_SERVER_ERROR, HTTP_CONTENT_JSON, f_str("Heap exhausted"), false);
}

static void setCron(struct espconn *ptr_espconn, Http_parsed_req *parsed_req)
{
    ALL("setCron");
    JSONP req_croncfg(parsed_req->req_content, parsed_req->content_len);
    int enabled = req_croncfg.getInt("cron_enabled");
    if (req_croncfg.getErr() != JSON_noerr)
    {
        http_response(ptr_espconn, HTTP_BAD_REQUEST, HTTP_CONTENT_JSON, f_str("Json bad syntax"), false);
        return;
    }
    if (enabled)
        enable_cron();
    else
        disable_cron();
    cron_state_save();

    char *msg = cron_state_json_stringify();
    if (msg)
        http_response(ptr_espconn, HTTP_OK, HTTP_CONTENT_JSON, msg, true);
    else
        http_response(ptr_espconn, HTTP_SERVER_ERROR, HTTP_CONTENT_JSON, f_str("Heap exhausted"), false);
    espmem.stack_mon();
}

static void getHexMemDump(struct espconn *ptr_espconn, Http_parsed_req *parsed_req)
{
    ALL("getHexMemDump");
    JSONP mem_param(parsed_req->req_content, parsed_req->content_len);
    char address_str[16];
    mem_param.getStr("address", address_str, 16);
    int length = mem_param.getInt("length");
    if (mem_param.getErr() != JSON_noerr)
    {
        http_response(ptr_espconn, HTTP_BAD_REQUEST, HTTP_CONTENT_JSON, f_str("Json bad syntax"), false);
        return;
    }
    char *address = (char *)atoh(address_str);
    int msg_len = 48 + length * 3;
    Heap_chunk msg(msg_len, dont_free);
    if (msg.ref == NULL)
    {
        esp_diag.error(ROUTES_GETHEXMEMDUMP_HEAP_EXHAUSTED, msg_len);
        ERROR("getHexMemDump heap exhausted %d", msg_len);
        http_response(ptr_espconn, HTTP_SERVER_ERROR, HTTP_CONTENT_JSON, f_str("Heap exhausted"), false);
        return;
    }
    espmem.stack_mon();
    fs_sprintf(msg.ref,
               "{\"address\":\"%X\",\"length\": %d,\"content\":\"",
               address,
               length);
    int cnt;
    char *ptr = msg.ref + os_strlen(msg.ref);
    for (cnt = 0; cnt < length; cnt++)
    {
        os_sprintf(ptr, " %X", *(address + cnt));
        ptr = msg.ref + os_strlen(msg.ref);
    }
    fs_sprintf(msg.ref + os_strlen(msg.ref), "\"}");
    http_response(ptr_espconn, HTTP_OK, HTTP_CONTENT_JSON, msg.ref, true);
}

static void getLastReset(struct espconn *ptr_espconn, Http_parsed_req *parsed_req)
{
    ALL("getLastReset");
    // enum rst_reason
    // {
    //     REASON_DEFAULT_RST = 0,
    //     REASON_WDT_RST = 1,
    //     REASON_EXCEPTION_RST = 2,
    //     REASON_SOFT_WDT_RST = 3,
    //     REASON_SOFT_RESTART = 4,
    //     REASON_DEEP_SLEEP_AWAKE = 5,
    //     REASON_EXT_SYS_RST = 6
    // };
    // struct rst_info
    // {
    //     uint32 reason;
    //     uint32 exccause;
    //     uint32 epc1;
    //     uint32 epc2;
    //     uint32 epc3;
    //     uint32 excvaddr;
    //     uint32 depc;
    // };
    // {"date":"","reason":"","exccause":"","epc1":"","epc2":"","epc3":"","evcvaddr":"","depc":"","sp":"","spDump":[]}
    // array item "01234567",
    int str_len = 111 + 24 + 7 * 8 + 11 * 19 + 1;
    Heap_chunk msg(str_len, dont_free);
    if (msg.ref == NULL)
    {
        esp_diag.error(ROUTES_GETLASTRESET_HEAP_EXHAUSTED, str_len);
        ERROR("getlastreset heap exhausted %d", str_len);
        http_response(ptr_espconn, HTTP_SERVER_ERROR, HTTP_CONTENT_JSON, f_str("Heap exhausted"), false);
        return;
    }
    struct rst_info *last_rst = system_get_rst_info();
    fs_sprintf(msg.ref,
               "{\"date\":\"%s\","
               "\"reason\":\"%X\","
               "\"exccause\":\"%X\","
               "\"epc1\":\"%X\",",
               esp_time.get_timestr(espbot.get_last_reboot_time()),
               last_rst->reason,
               last_rst->exccause,
               last_rst->epc1);
    fs_sprintf(msg.ref + os_strlen(msg.ref),
               "\"epc2\":\"%X\","
               "\"epc3\":\"%X\","
               "\"evcvaddr\":\"%X\","
               "\"depc\":\"%X\",",
               last_rst->epc2,
               last_rst->epc3,
               last_rst->excvaddr,
               last_rst->depc);
    fs_sprintf(msg.ref + os_strlen(msg.ref),
               "\"sp\":\"%X\","
               "\"spDump\":[",
               get_last_crash_SP());
    uint32 address;
    int res = get_last_crash_stack_dump(0, &address);
    while (res == 0)
    {
        fs_sprintf(msg.ref + os_strlen(msg.ref), "\"%X\",", address);
        res = get_last_crash_stack_dump(1, &address);
    }
    fs_sprintf(msg.ref + os_strlen(msg.ref) - 1, "]}");
    http_response(ptr_espconn, HTTP_OK, HTTP_CONTENT_JSON, msg.ref, true);
}

static void getMemDump(struct espconn *ptr_espconn, Http_parsed_req *parsed_req)
{
    ALL("getMemDump");
    JSONP mem_param(parsed_req->req_content, parsed_req->content_len);
    char address_str[16];
    mem_param.getStr("address", address_str, 16);
    int length = mem_param.getInt("length");
    if (mem_param.getErr() != JSON_noerr)
    {
        http_response(ptr_espconn, HTTP_BAD_REQUEST, HTTP_CONTENT_JSON, f_str("Json bad syntax"), false);
        return;
    }
    char *address = (char *)atoh(address_str);
    Heap_chunk msg(48 + length, dont_free);
    espmem.stack_mon();
    if (msg.ref == NULL)
    {
        esp_diag.error(ROUTES_GETMEMDUMP_HEAP_EXHAUSTED, 48 + length);
        ERROR("getMemDump heap exhausted %d", 48 + length);
        http_response(ptr_espconn, HTTP_SERVER_ERROR, HTTP_CONTENT_JSON, f_str("Heap exhausted"), false);
        return;
    }
    fs_sprintf(msg.ref,
               "{\"address\":\"%X\",\"length\": %d,\"content\":\"",
               address,
               length);
    int cnt;
    char *ptr = msg.ref + os_strlen(msg.ref);
    for (cnt = 0; cnt < length; cnt++)
        os_sprintf(ptr++, "%c", *(address + cnt));
    fs_sprintf(msg.ref + os_strlen(msg.ref), "\"}");
    http_response(ptr_espconn, HTTP_OK, HTTP_CONTENT_JSON, msg.ref, true);
}

static void getMemInfo(struct espconn *ptr_espconn, Http_parsed_req *parsed_req)
{
    ALL("getMemInfo");
    Heap_chunk msg(166 + 54, dont_free); // formatting string and values
    if (msg.ref == NULL)
    {
        esp_diag.error(ROUTES_GETMEMINFO_HEAP_EXHAUSTED, (166 + 54));
        ERROR("getMemInfo heap exhausted %d", (166 + 54));
        http_response(ptr_espconn, HTTP_SERVER_ERROR, HTTP_CONTENT_JSON, f_str("Heap exhausted"), false);
        return;
    }
    fs_sprintf(msg.ref,
               "{\"stack_max_addr\":\"%X\",\"stack_min_addr\":\"%X\",",
               espmem.get_max_stack_addr(),  //  8
               espmem.get_min_stack_addr()); //  8
    fs_sprintf((msg.ref + os_strlen(msg.ref)),
               "\"heap_start_addr\":\"%X\",\"heap_free_size\": %d,",
               espmem.get_start_heap_addr(), //  8
               system_get_free_heap_size()); //  6
    fs_sprintf((msg.ref + os_strlen(msg.ref)),
               "\"heap_max_size\": %d,\"heap_min_size\": %d,",
               espmem.get_max_heap_size(),  //  6
               espmem.get_mim_heap_size()); //  6
    fs_sprintf((msg.ref + os_strlen(msg.ref)),
               "\"heap_objs\": %d,\"heap_max_objs\": %d}",
               espmem.get_heap_objs(),      //  6
               espmem.get_max_heap_objs()); //  6
    espmem.stack_mon();
    http_response(ptr_espconn, HTTP_OK, HTTP_CONTENT_JSON, msg.ref, true);
}

static void ackDiagnosticEvents(struct espconn *ptr_espconn, Http_parsed_req *parsed_req)
{
    ALL("ackDiagnosticEvents");
    esp_diag.ack_events();
    http_response(ptr_espconn, HTTP_OK, HTTP_CONTENT_JSON, f_str("{\"msg\":\"Events acknoledged\"}"), false);
}

static void getDiagnosticCfg(struct espconn *ptr_espconn, Http_parsed_req *parsed_req)
{
    ALL("getDiagnosticCfg");
    // "{"diag_led_mask":256,"serial_log_mask":256,"uart_0_bitrate":3686400,"sdk_print_enabled":1}"
    int msg_len = 91;
    Heap_chunk msg(msg_len, dont_free);
    if (msg.ref)
    {
        fs_sprintf(msg.ref,
                   "{\"diag_led_mask\":%d,\"serial_log_mask\":%d,",
                   esp_diag.get_led_mask(),
                   esp_diag.get_serial_log_mask());
        fs_sprintf(msg.ref + os_strlen(msg.ref),
                   "\"uart_0_bitrate\":%d,\"sdk_print_enabled\":%d}",
                   esp_diag.get_uart_0_bitrate(),
                   esp_diag.get_sdk_print_enabled());
        http_response(ptr_espconn, HTTP_OK, HTTP_CONTENT_JSON, msg.ref, true);
    }
    else
    {
        esp_diag.error(ROUTES_GETDIAGNOSTICCFG_HEAP_EXHAUSTED, msg_len);
        ERROR("getDiagnosticCfg heap exhausted %d", msg_len);
        http_response(ptr_espconn, HTTP_SERVER_ERROR, HTTP_CONTENT_JSON, f_str("Heap exhausted"), false);
    }
}

static void setDiagnosticCfg(struct espconn *ptr_espconn, Http_parsed_req *parsed_req)
{
    ALL("setDiagnosticCfg");
    JSONP diag_cfg(parsed_req->req_content, parsed_req->content_len);
    int diag_led_mask = diag_cfg.getInt("diag_led_mask");
    int serial_log_mask = diag_cfg.getInt("serial_log_mask");
    int sdk_print_enabled = diag_cfg.getInt("sdk_print_enabled");
    int uart_0_bitrate = diag_cfg.getInt("uart_0_bitrate");
    if (diag_cfg.getErr() != JSON_noerr)
    {
        http_response(ptr_espconn, HTTP_BAD_REQUEST, HTTP_CONTENT_JSON, f_str("Json bad syntax"), false);
        return;
    }
    esp_diag.set_led_mask(diag_led_mask);
    esp_diag.set_serial_log_mask(serial_log_mask);
    esp_diag.set_sdk_print_enabled(sdk_print_enabled);
    esp_diag.set_uart_0_bitrate((uint32)uart_0_bitrate);
    esp_diag.save_cfg();

    // "{"diag_led_mask":256,"serial_log_mask":256,"uart_0_bitrate":3686400,"sdk_print_enabled":1}"
    int msg_len = 91;
    Heap_chunk msg(msg_len, dont_free);
    if (msg.ref == NULL)
    {
        esp_diag.error(ROUTES_SETDIAGNOSTICCFG_HEAP_EXHAUSTED, msg_len);
        ERROR("setDiagnosticCfg heap exhausted %d", msg_len);
        http_response(ptr_espconn, HTTP_SERVER_ERROR, HTTP_CONTENT_JSON, f_str("Heap exhausted"), false);
        return;
    }
    fs_sprintf(msg.ref,
               "{\"diag_led_mask\":%d,\"serial_log_mask\":%d,",
               esp_diag.get_led_mask(),
               esp_diag.get_serial_log_mask());
    fs_sprintf(msg.ref + os_strlen(msg.ref),
               "\"uart_0_bitrate\":%d,\"sdk_print_enabled\":%d}",
               esp_diag.get_uart_0_bitrate(),
               esp_diag.get_sdk_print_enabled());
    http_response(ptr_espconn, HTTP_OK, HTTP_CONTENT_JSON, msg.ref, true);
    espmem.stack_mon();
}

static void getDiagnosticEvents_next(struct http_split_send *p_sr)
{
    ALL("getdiagnosticevents_next");
    if (!http_espconn_in_use(p_sr->p_espconn))
    {
        TRACE("getdiagnosticevents_next espconn %X state %d, abort", p_sr->p_espconn, p_sr->p_espconn->state);
        // there will be no send, so trigger a check of pending send
        system_os_post(USER_TASK_PRIO_0, SIG_HTTP_CHECK_PENDING_RESPONSE, '0');
        return;
    }
    int remaining_size = (p_sr->content_size - p_sr->content_transferred) * (42 + 12 + 1 + 2 + 4 + 12) + 2;
    if (remaining_size > get_http_msg_max_size())
    {
        // the remaining content size is bigger than response_max_size
        // will split the remaining content over multiple messages
        int buffer_size = get_http_msg_max_size();
        Heap_chunk buffer(buffer_size, dont_free);
        if (buffer.ref == NULL)
        {
            esp_diag.error(ROUTES_GETDIAGNOSTICEVENTS_NEXT_HEAP_EXHAUSTED, buffer_size);
            ERROR("getdiagnosticevents_next heap exhausted %d", buffer_size);
            http_response(p_sr->p_espconn, HTTP_SERVER_ERROR, HTTP_CONTENT_JSON, f_str("Heap exhausted"), false);
            return;
        }
        struct http_split_send *p_pending_response = new struct http_split_send;
        if (p_pending_response == NULL)
        {
            esp_diag.error(ROUTES_GETDIAGNOSTICEVENTS_NEXT_HEAP_EXHAUSTED, sizeof(struct http_split_send));
            ERROR("getdiagnosticevents_next not heap exhausted %dn", sizeof(struct http_split_send));
            http_response(p_sr->p_espconn, HTTP_SERVER_ERROR, HTTP_CONTENT_JSON, f_str("Heap exhausted"), false);
            delete[] buffer.ref;
            return;
        }
        struct dia_event *event_ptr;
        int ev_count = (buffer_size - 18) / (42 + 12 + 1 + 2 + 4 + 12) + p_sr->content_transferred;
        int idx;
        for (idx = p_sr->content_transferred; idx < ev_count; idx++)
        {
            fs_sprintf(buffer.ref + os_strlen(buffer.ref), ",");
            event_ptr = esp_diag.get_event(idx);
            if (event_ptr)
                fs_sprintf(buffer.ref + os_strlen(buffer.ref), "{\"ts\":%d,\"ack\":%d,\"type\":\"%X\",\"code\":\"%X\",\"val\":%d}",
                           event_ptr->timestamp,
                           event_ptr->ack,
                           event_ptr->type,
                           event_ptr->code,
                           event_ptr->value);
        }
        // setup the remaining message
        p_pending_response->p_espconn = p_sr->p_espconn;
        p_pending_response->order = p_sr->order + 1;
        p_pending_response->content = p_sr->content;
        p_pending_response->content_size = p_sr->content_size;
        p_pending_response->content_transferred = ev_count;
        p_pending_response->action_function = getDiagnosticEvents_next;
        Queue_err result = pending_split_send->push(p_pending_response);
        if (result == Queue_full)
        {
            delete[] buffer.ref;
            delete p_pending_response;
            esp_diag.error(ROUTES_GETDIAGNOSTICEVENTS_NEXT_PENDING_RES_QUEUE_FULL);
            ERROR("getdiagnosticevents_next full pending res queue");
            return;
        }
        TRACE("getdiagnosticevents_next: *p_espconn: %X, msg (splitted) len: %d",
              p_sr->p_espconn, buffer_size);
        http_send_buffer(p_sr->p_espconn, p_sr->order, buffer.ref, os_strlen(buffer.ref));
    }
    else
    {
        // this is the last piece of the message
        Heap_chunk buffer(remaining_size, dont_free);
        if (buffer.ref == NULL)
        {
            esp_diag.error(ROUTES_GETDIAGNOSTICEVENTS_NEXT_HEAP_EXHAUSTED, remaining_size);
            ERROR("getdiagnosticevents_next heap exhausted %d", remaining_size);
            http_response(p_sr->p_espconn, HTTP_SERVER_ERROR, HTTP_CONTENT_JSON, f_str("Heap exhausted"), false);
            return;
        }
        struct dia_event *event_ptr;
        int idx;
        for (idx = p_sr->content_transferred; idx < p_sr->content_size; idx++)
        {
            fs_sprintf(buffer.ref + os_strlen(buffer.ref), ",");
            event_ptr = esp_diag.get_event(idx);
            if (event_ptr)
                fs_sprintf(buffer.ref + os_strlen(buffer.ref), "{\"ts\":%d,\"ack\":%d,\"type\":\"%X\",\"code\":\"%X\",\"val\":%d}",
                           event_ptr->timestamp,
                           event_ptr->ack,
                           event_ptr->type,
                           event_ptr->code,
                           event_ptr->value);
        }
        fs_sprintf(buffer.ref + os_strlen(buffer.ref), "]}");
        TRACE("getdiagnosticevents_next: *p_espconn: %X, msg (splitted) len: %d",
              p_sr->p_espconn, remaining_size);
        http_send_buffer(p_sr->p_espconn, p_sr->order, buffer.ref, os_strlen(buffer.ref));
    }
}

static void getDiagnosticEvents(struct espconn *ptr_espconn, Http_parsed_req *parsed_req)
{
    ALL("getDiagnosticEvents");
    // count the diagnostic events and calculate the message content length
    // {"diag_events":[
    // {"ts":,"ack":,"type":"","code":"","val":},
    // ]}
    int evnt_count = 0;
    int content_len = 16 + 2 - 1;
    {
        char tmp_msg[42 + 12 + 1 + 2 + 4 + 12 + 1];
        struct dia_event *event_ptr = esp_diag.get_event(evnt_count);
        while (event_ptr)
        {
            evnt_count++;
            fs_sprintf(tmp_msg, "{\"ts\":%d,\"ack\":%d,\"type\":\"%X\",\"code\":\"%X\",\"val\":%d},",
                       event_ptr->timestamp,
                       event_ptr->ack,
                       event_ptr->type,
                       event_ptr->code,
                       event_ptr->value);
            content_len += os_strlen(tmp_msg);
            if (evnt_count >= esp_diag.get_max_events_count())
                break;
            event_ptr = esp_diag.get_event(evnt_count);
        }
    }
    // let's start with the header
    Http_header header;
    header.m_code = HTTP_OK;
    header.m_content_type = HTTP_CONTENT_JSON;
    header.m_content_length = content_len;
    header.m_content_range_start = 0;
    header.m_content_range_end = 0;
    header.m_content_range_total = 0;
    bool heap_exhausted = false;
    if (parsed_req->origin)
    {
        header.m_origin = new char[(os_strlen(parsed_req->origin) + 1)];
        if (header.m_origin == NULL)
        {
            esp_diag.error(ROUTES_GETDIAGNOSTICEVENTS_HEAP_EXHAUSTED, (os_strlen(parsed_req->origin) + 1));
            ERROR("getDiagnosticEvents heap exhausted %d", (os_strlen(parsed_req->origin) + 1));
            heap_exhausted = true;
        }
        else
            os_strcpy(header.m_origin, parsed_req->origin);
    }
    char *header_str = http_format_header(&header);
    if (header_str == NULL)
    {
        esp_diag.error(ROUTES_GETDIAGNOSTICEVENTS_HEAP_EXHAUSTED, (os_strlen(header_str)));
        ERROR("getDiagnosticEvents heap exhausted %d", (os_strlen(header_str)));
        heap_exhausted = true;
    }
    if (heap_exhausted)
        http_response(ptr_espconn, HTTP_SERVER_ERROR, HTTP_CONTENT_JSON, f_str("Heap exhausted"), false);
    else
        // ok send the header
        http_send_buffer(ptr_espconn, 0, header_str, os_strlen(header_str));

    // and now the content
    if (content_len > get_http_msg_max_size())
    {
        // will split the content over multiple messages
        // each the size of http_msg_max_size
        int buffer_size = get_http_msg_max_size();
        Heap_chunk buffer(buffer_size, dont_free);
        if (buffer.ref == NULL)
        {
            esp_diag.error(ROUTES_GETDIAGNOSTICEVENTS_HEAP_EXHAUSTED, buffer_size);
            ERROR("getDiagnosticEvents heap exhausted %d", buffer_size);
            return;
        }
        struct http_split_send *p_pending_response = new struct http_split_send;
        if (p_pending_response == NULL)
        {
            esp_diag.error(ROUTES_GETDIAGNOSTICEVENTS_HEAP_EXHAUSTED, sizeof(struct http_split_send));
            ERROR("getDiagnosticEvents heap exhausted %d", sizeof(struct http_split_send));
            delete[] buffer.ref;
            return;
        }
        fs_sprintf(buffer.ref, "{\"diag_events\":[");
        bool first_time = true;
        struct dia_event *event_ptr;
        int ev_count = (buffer_size - 18) / (42 + 12 + 1 + 2 + 4 + 12);
        int idx;
        for (idx = 0; idx < ev_count; idx++)
        {
            if (first_time)
                first_time = false;
            else
                fs_sprintf(buffer.ref + os_strlen(buffer.ref), ",");
            event_ptr = esp_diag.get_event(idx);
            if (event_ptr)
                fs_sprintf(buffer.ref + os_strlen(buffer.ref), "{\"ts\":%d,\"ack\":%d,\"type\":\"%X\",\"code\":\"%X\",\"val\":%d}",
                           event_ptr->timestamp,
                           event_ptr->ack,
                           event_ptr->type,
                           event_ptr->code,
                           event_ptr->value);
        }
        // setup the next message
        p_pending_response->p_espconn = ptr_espconn;
        p_pending_response->order = 2;
        p_pending_response->content = "";
        p_pending_response->content_size = evnt_count;
        p_pending_response->content_transferred = ev_count;
        p_pending_response->action_function = getDiagnosticEvents_next;
        Queue_err result = pending_split_send->push(p_pending_response);
        if (result == Queue_full)
        {
            delete[] buffer.ref;
            delete p_pending_response;
            esp_diag.error(ROUTES_GETDIAGEVENTS_PENDING_RES_QUEUE_FULL);
            ERROR("getDiagnosticEvents full pending response queue");
            return;
        }
        // send the content fragment
        TRACE("getDiagnosticEvents *p_espconn: %X, msg (splitted) len: %d", ptr_espconn, buffer_size);
        http_send_buffer(ptr_espconn, 1, buffer.ref, os_strlen(buffer.ref));
        espmem.stack_mon();
    }
    else
    {
        // no need to split the content over multiple messages
        Heap_chunk buffer(content_len, dont_free);
        if (buffer.ref == NULL)
        {
            esp_diag.error(ROUTES_GETDIAGNOSTICEVENTS_HEAP_EXHAUSTED, content_len);
            ERROR("getDiagnosticEvents heap exhausted %d", content_len);
            http_response(ptr_espconn, HTTP_SERVER_ERROR, HTTP_CONTENT_JSON, f_str("Heap exhausted"), false);
            return;
        }
        fs_sprintf(buffer.ref, "{\"diag_events\":[");
        bool first_time = true;
        struct dia_event *event_ptr;
        int idx;
        for (idx = 0; idx < evnt_count; idx++)
        {
            if (first_time)
                first_time = false;
            else
                fs_sprintf(buffer.ref + os_strlen(buffer.ref), ",");
            event_ptr = esp_diag.get_event(idx);
            if (event_ptr)
                fs_sprintf(buffer.ref + os_strlen(buffer.ref), "{\"ts\":%d,\"ack\":%d,\"type\":\"%X\",\"code\":\"%X\",\"val\":%d}",
                           event_ptr->timestamp,
                           event_ptr->ack,
                           event_ptr->type,
                           event_ptr->code,
                           event_ptr->value);
        }
        fs_sprintf(buffer.ref + os_strlen(buffer.ref), "]}");
        TRACE("getDiagnosticEvents *p_espconn: %X, msg (full) len: %d", ptr_espconn, content_len);
        http_send_buffer(ptr_espconn, 1, buffer.ref, os_strlen(buffer.ref));
    }
}

static void getDeviceName(struct espconn *ptr_espconn, Http_parsed_req *parsed_req)
{
    ALL("getDeviceName");
    Heap_chunk msg(64, dont_free);
    if (msg.ref)
    {
        fs_sprintf(msg.ref, "{\"device_name\":\"%s\"}", espbot.get_name());
        http_response(ptr_espconn, HTTP_OK, HTTP_CONTENT_JSON, msg.ref, true);
    }
    else
    {
        esp_diag.error(ROUTES_GETDEVICENAME_HEAP_EXHAUSTED, 64);
        ERROR("getDeviceName heap exhausted %d", 64);
        http_response(ptr_espconn, HTTP_SERVER_ERROR, HTTP_CONTENT_JSON, f_str("Heap exhausted"), false);
    }
}

static void setDeviceName(struct espconn *ptr_espconn, Http_parsed_req *parsed_req)
{
    ALL("setDeviceName");
    JSONP device_cfg(parsed_req->req_content, parsed_req->content_len);
    char device_name[32];
    device_cfg.getStr("device_name", device_name, 32);
    if (device_cfg.getErr() != JSON_noerr)
    {
        http_response(ptr_espconn, HTTP_BAD_REQUEST, HTTP_CONTENT_JSON, f_str("Json bad syntax"), false);
        return;
    }
    espbot.set_name(device_name);
    Heap_chunk msg(64, dont_free);
    espmem.stack_mon();
    if (msg.ref == NULL)
    {
        esp_diag.error(ROUTES_SETDEVICENAME_HEAP_EXHAUSTED, 64);
        ERROR("setDeviceName heap exhausted %d", 64);
        http_response(ptr_espconn, HTTP_SERVER_ERROR, HTTP_CONTENT_JSON, f_str("Heap exhausted"), false);
        return;
    }
    fs_sprintf(msg.ref, "{\"device_name\":\"%s\"}", espbot.get_name());
    http_response(ptr_espconn, HTTP_OK, HTTP_CONTENT_JSON, msg.ref, true);
}

static void getFs(struct espconn *ptr_espconn, Http_parsed_req *parsed_req)
{
    ALL("getFs");
    int total_size = esp_spiffs_total_size();
    int used_size = esp_spiffs_used_size();

    Heap_chunk msg(128, dont_free);
    if (msg.ref == NULL)
    {
        esp_diag.error(ROUTES_GETFS_HEAP_EXHAUSTED, 128);
        ERROR("getFs heap exhausted %d", 128);
        http_response(ptr_espconn, HTTP_SERVER_ERROR, HTTP_CONTENT_JSON, f_str("Heap exhausted"), false);
        return;
    }
    fs_sprintf(msg.ref,
               "{\"file_system_size\": %d,"
               "\"file_system_used_size\": %d}",
               total_size,
               used_size);
    http_response(ptr_espconn, HTTP_OK, HTTP_CONTENT_JSON, msg.ref, true);
}

static void checkFS(struct espconn *ptr_espconn, Http_parsed_req *parsed_req)
{
    ALL("checkFS");
    http_response(ptr_espconn, HTTP_ACCEPTED, HTTP_CONTENT_JSON, f_str("{\"msg\":\"FS check started...\"}"), false);

    // give some time to the http reply before starting the FS check
    os_timer_disarm(&delay_timer);
    os_timer_setfn(&delay_timer, (os_timer_func_t *)esp_spiffs_check, NULL);
    os_timer_arm(&delay_timer, 1000, 0);
}

static void getFileList(struct espconn *ptr_espconn, Http_parsed_req *parsed_req)
{
    ALL("getFileList");
    int file_cnt = 0;
    struct spiffs_dirent *file_ptr = esp_spiffs_list(0);
    // count files first
    while (file_ptr)
    {
        file_cnt++;
        file_ptr = esp_spiffs_list(1);
    }
    // now prepare the list
    // {
    //    "files":[
    //      {"name":"file_1","size":1024},
    //      {"name":"file_2","size":2048}
    //    ]
    // }
    // {"files":[]}
    // {"name":"","size":},
    int file_list_len = 12 + (file_cnt * (20 + 31 + 7)) + 1;
    Heap_chunk file_list(file_list_len, dont_free);
    if (file_list.ref == NULL)
    {
        esp_diag.error(ROUTES_GETFILELIST_HEAP_EXHAUSTED, file_list_len);
        ERROR("getFileList heap exhausted %d", file_list_len);
        http_response(ptr_espconn, HTTP_SERVER_ERROR, HTTP_CONTENT_JSON, f_str("Heap exhausted"), false);
        return;
    }
    char *tmp_ptr;
    fs_sprintf(file_list.ref, "{\"files\":[");
    file_ptr = esp_spiffs_list(0);
    while (file_ptr)
    {
        tmp_ptr = file_list.ref + os_strlen(file_list.ref);
        if (tmp_ptr != (file_list.ref + os_strlen(f_str("{\"files\":["))))
            *(tmp_ptr++) = ',';
        fs_sprintf(tmp_ptr, "{\"name\":\"%s\",\"size\":%d}", (char *)file_ptr->name, file_ptr->size);
        file_ptr = esp_spiffs_list(1);
    }
    tmp_ptr = file_list.ref + os_strlen(file_list.ref);
    fs_sprintf(tmp_ptr, "]}");
    http_response(ptr_espconn, HTTP_OK, HTTP_CONTENT_JSON, file_list.ref, true);
    espmem.stack_mon();
}

static void getFile(struct espconn *ptr_espconn, Http_parsed_req *parsed_req)
{
    ALL("getFile");
    char *file_name = parsed_req->url + os_strlen(f_str("/api/file/"));
    if (os_strlen(file_name) == 0)
    {
        http_response(ptr_espconn, HTTP_BAD_REQUEST, HTTP_CONTENT_JSON, f_str("No file name provided"), false);
        return;
    }
    if (!Espfile::exists(file_name))
    {
        http_response(ptr_espconn, HTTP_BAD_REQUEST, HTTP_CONTENT_JSON, f_str("File not found"), false);
        return;
    }
    return_file(ptr_espconn, parsed_req, file_name);
}

static void deleteFile(struct espconn *ptr_espconn, Http_parsed_req *parsed_req)
{
    ALL("deleteFile");
    char *file_name = parsed_req->url + os_strlen(f_str("/api/file/"));
    if (os_strlen(file_name) == 0)
    {
        http_response(ptr_espconn, HTTP_BAD_REQUEST, HTTP_CONTENT_JSON, f_str("No file name provided"), false);
        return;
    }
    if (!Espfile::exists(file_name))
    {
        http_response(ptr_espconn, HTTP_NOT_FOUND, HTTP_CONTENT_JSON, f_str("File not found"), false);
        return;
    }
    Espfile sel_file(file_name);
    int res = sel_file.remove();
    espmem.stack_mon();
    // check if the file was deleted
    if (res != SPIFFS_OK)
    {
        http_response(ptr_espconn, HTTP_SERVER_ERROR, HTTP_CONTENT_JSON, f_str("Error deleting file"), false);
    }
    else
    {
        http_response(ptr_espconn, HTTP_OK, HTTP_CONTENT_JSON, f_str("{\"msg\":\"File deleted\"}"), false);
    }
}

static void createFile(struct espconn *ptr_espconn, Http_parsed_req *parsed_req)
{
    ALL("createFile");
    char *file_name = parsed_req->url + os_strlen(f_str("/api/file/"));
    if (os_strlen(file_name) == 0)
    {
        http_response(ptr_espconn, HTTP_BAD_REQUEST, HTTP_CONTENT_JSON, f_str("No file name provided"), false);
        return;
    }
    if (Espfile::exists(file_name))
    {
        http_response(ptr_espconn, HTTP_BAD_REQUEST, HTTP_CONTENT_JSON, f_str("File already exists"), false);
        return;
    }
    Espfile sel_file(file_name);
    int res = sel_file.n_append(parsed_req->req_content, parsed_req->content_len);
    espmem.stack_mon();
    if (res < SPIFFS_OK)
    {
        http_response(ptr_espconn, HTTP_SERVER_ERROR, HTTP_CONTENT_JSON, f_str("Error creating file"), false);
    }
    else
    {
        http_response(ptr_espconn, HTTP_CREATED, HTTP_CONTENT_JSON, f_str("{\"msg\":\"File created\"}"), false);
    }
}

static void appendToFile(struct espconn *ptr_espconn, Http_parsed_req *parsed_req)
{
    ALL("appendToFile");
    char *file_name = parsed_req->url + os_strlen(f_str("/api/file/"));
    if (os_strlen(file_name) == 0)
    {
        http_response(ptr_espconn, HTTP_BAD_REQUEST, HTTP_CONTENT_JSON, f_str("No file name provided"), false);
        return;
    }
    if (!Espfile::exists(file_name))
    {
        http_response(ptr_espconn, HTTP_NOT_FOUND, HTTP_CONTENT_JSON, f_str("File not found"), false);
        return;
    }
    Espfile sel_file(file_name);
    espmem.stack_mon();
    int res = sel_file.n_append(parsed_req->req_content, parsed_req->content_len);
    if (res < SPIFFS_OK)
    {
        http_response(ptr_espconn, HTTP_SERVER_ERROR, HTTP_CONTENT_JSON, f_str("Error modifying file"), false);
    }
    else
    {
        http_response(ptr_espconn, HTTP_OK, HTTP_CONTENT_JSON, f_str("{\"msg\":\"File modified\"}"), false);
    }
}

static void getGpioCfg(struct espconn *ptr_espconn, Http_parsed_req *parsed_req)
{
    ALL("getGpioCfg");
    int gpio_id;
    char *gpio_name = parsed_req->url + os_strlen(f_str("/api/gpio/cfg/"));
    if (os_strlen(gpio_name) == 0)
    {
        http_response(ptr_espconn, HTTP_BAD_REQUEST, HTTP_CONTENT_JSON, f_str("No GPIO ID provided"), false);
        return;
    }
    gpio_id = atoi(gpio_name);
    // {"gpio_id":,"gpio_type":"unprovisioned"}
    Heap_chunk msg(48, dont_free);
    if (msg.ref)
    {
        int result = esp_gpio.get_config(gpio_id);
        switch (result)
        {
        case ESPBOT_GPIO_WRONG_IDX:
            http_response(ptr_espconn, HTTP_BAD_REQUEST, HTTP_CONTENT_JSON, f_str("Wrong GPIO ID"), false);
            return;
        case ESPBOT_GPIO_UNPROVISIONED:
            fs_sprintf(msg.ref, "{\"gpio_id\": %d,\"gpio_type\":\"unprovisioned\"}", gpio_id);
            http_response(ptr_espconn, HTTP_OK, HTTP_CONTENT_JSON, msg.ref, true);
            return;
        case ESPBOT_GPIO_INPUT:
            fs_sprintf(msg.ref, "{\"gpio_id\": %d,\"gpio_type\":\"input\"}", gpio_id);
            http_response(ptr_espconn, HTTP_OK, HTTP_CONTENT_JSON, msg.ref, true);
            return;
        case ESPBOT_GPIO_OUTPUT:
            fs_sprintf(msg.ref, "{\"gpio_id\": %d,\"gpio_type\":\"output\"}", gpio_id);
            http_response(ptr_espconn, HTTP_OK, HTTP_CONTENT_JSON, msg.ref, true);
            return;
        default:
            http_response(ptr_espconn, HTTP_SERVER_ERROR, HTTP_CONTENT_JSON, f_str("Gpio.get_config error"), false);
            return;
        }
    }
    else
    {
        esp_diag.error(ROUTES_GETGPIOCFG_HEAP_EXHAUSTED, 48);
        ERROR("getGpioCfg heap exhausted %d", 48);
        http_response(ptr_espconn, HTTP_SERVER_ERROR, HTTP_CONTENT_JSON, f_str("Heap exhausted"), false);
    }
    espmem.stack_mon();
}

static void setGpioCfg(struct espconn *ptr_espconn, Http_parsed_req *parsed_req)
{
    ALL("setGpioCfg");
    int gpio_id;
    char *gpio_name = parsed_req->url + os_strlen(f_str("/api/gpio/cfg/"));
    if (os_strlen(gpio_name) == 0)
    {
        http_response(ptr_espconn, HTTP_BAD_REQUEST, HTTP_CONTENT_JSON, f_str("No GPIO ID provided"), false);
        return;
    }
    gpio_id = atoi(gpio_name);

    JSONP req_gpio(parsed_req->req_content, parsed_req->content_len);
    char gpio_type_str[16];
    req_gpio.getStr("gpio_type", gpio_type_str, 16);
    if (req_gpio.getErr() != JSON_noerr)
    {
        http_response(ptr_espconn, HTTP_BAD_REQUEST, HTTP_CONTENT_JSON, f_str("Json bad syntax"), false);
        return;
    }

    if ((os_strcmp(gpio_type_str, f_str("UNPROVISIONED")) == 0) ||
        (os_strcmp(gpio_type_str, f_str("unprovisioned")) == 0))
    {
        if (esp_gpio.unconfig(gpio_id) == ESPBOT_GPIO_WRONG_IDX)
        {
            http_response(ptr_espconn, HTTP_BAD_REQUEST, HTTP_CONTENT_JSON, f_str("Wrong GPIO ID"), false);
            return;
        }
    }
    else if ((os_strcmp(gpio_type_str, f_str("INPUT")) == 0) ||
             (os_strcmp(gpio_type_str, f_str("input")) == 0))

    {
        if (esp_gpio.config(gpio_id, ESPBOT_GPIO_INPUT) == ESPBOT_GPIO_WRONG_IDX)
        {
            http_response(ptr_espconn, HTTP_BAD_REQUEST, HTTP_CONTENT_JSON, f_str("Wrong GPIO ID"), false);
            return;
        }
    }
    else if ((os_strcmp(gpio_type_str, f_str("OUTPUT")) == 0) ||
             (os_strcmp(gpio_type_str, f_str("output")) == 0))
    {
        if (esp_gpio.config(gpio_id, ESPBOT_GPIO_OUTPUT) == ESPBOT_GPIO_WRONG_IDX)
        {
            http_response(ptr_espconn, HTTP_BAD_REQUEST, HTTP_CONTENT_JSON, f_str("Wrong GPIO ID"), false);
            return;
        }
    }
    else
    {
        http_response(ptr_espconn, HTTP_BAD_REQUEST, HTTP_CONTENT_JSON, f_str("Wrong GPIO type"), false);
        return;
    }

    // {"gpio_id": 1,"gpio_type":"unprovisioned"}
    Heap_chunk msg(48, dont_free);
    if (msg.ref == NULL)
    {
        esp_diag.error(ROUTES_SETGPIOCFG_HEAP_EXHAUSTED, parsed_req->content_len);
        ERROR("setGpioCfg heap exhausted %d", parsed_req->content_len);
        http_response(ptr_espconn, HTTP_SERVER_ERROR, HTTP_CONTENT_JSON, f_str("Heap exhausted"), false);
        return;
    }

    int result = esp_gpio.get_config(gpio_id);
    switch (result)
    {
    case ESPBOT_GPIO_WRONG_IDX:
        http_response(ptr_espconn, HTTP_BAD_REQUEST, HTTP_CONTENT_JSON, f_str("Wrong GPIO ID"), false);
        return;
    case ESPBOT_GPIO_UNPROVISIONED:
        fs_sprintf(msg.ref, "{\"gpio_id\": %d,\"gpio_type\":\"unprovisioned\"}", gpio_id);
        http_response(ptr_espconn, HTTP_OK, HTTP_CONTENT_JSON, msg.ref, true);
        return;
    case ESPBOT_GPIO_INPUT:
        fs_sprintf(msg.ref, "{\"gpio_id\": %d,\"gpio_type\":\"input\"}", gpio_id);
        http_response(ptr_espconn, HTTP_OK, HTTP_CONTENT_JSON, msg.ref, true);
        return;
    case ESPBOT_GPIO_OUTPUT:
        fs_sprintf(msg.ref, "{\"gpio_id\": %d,\"gpio_type\":\"output\"}", gpio_id);
        http_response(ptr_espconn, HTTP_OK, HTTP_CONTENT_JSON, msg.ref, true);
        return;
    default:
        http_response(ptr_espconn, HTTP_SERVER_ERROR, HTTP_CONTENT_JSON, f_str("Gpio.get_config error"), false);
        return;
    }
    espmem.stack_mon();
}

static void getGpioLevel(struct espconn *ptr_espconn, Http_parsed_req *parsed_req)
{
    ALL("getGpioLevel");
    int gpio_id;
    char *gpio_name = parsed_req->url + os_strlen(f_str("/api/gpio/"));
    if (os_strlen(gpio_name) == 0)
    {
        http_response(ptr_espconn, HTTP_BAD_REQUEST, HTTP_CONTENT_JSON, f_str("No GPIO ID provided"), false);
        return;
    }
    gpio_id = atoi(gpio_name);

    // {"gpio_id":,"gpio_level":"unprovisioned"}
    Heap_chunk msg(48, dont_free);
    if (msg.ref == NULL)
    {
        esp_diag.error(ROUTES_GETGPIOLEVEL_HEAP_EXHAUSTED, 48);
        ERROR("getGpioLevel heap exhausted %d", 64);
        http_response(ptr_espconn, HTTP_SERVER_ERROR, HTTP_CONTENT_JSON, f_str("Heap exhausted"), false);
        return;
    }

    int result = esp_gpio.read(gpio_id);
    switch (result)
    {
    case ESPBOT_GPIO_WRONG_IDX:
        http_response(ptr_espconn, HTTP_BAD_REQUEST, HTTP_CONTENT_JSON, f_str("Wrong GPIO ID"), false);
        return;
    case ESPBOT_GPIO_UNPROVISIONED:
        fs_sprintf(msg.ref, "{\"gpio_id\":%d,\"gpio_level\":\"unprovisioned\"}", gpio_id);
        break;
    case ESPBOT_LOW:
        fs_sprintf(msg.ref, "{\"gpio_id\":%d,\"gpio_level\":\"low\"}", gpio_id);
        break;
    case ESPBOT_HIGH:
        fs_sprintf(msg.ref, "{\"gpio_id\":%d,\"gpio_level\":\"high\"}", gpio_id);
        break;
    default:
        break;
    }
    http_response(ptr_espconn, HTTP_OK, HTTP_CONTENT_JSON, msg.ref, true);
    espmem.stack_mon();
}

static void setGpioLevel(struct espconn *ptr_espconn, Http_parsed_req *parsed_req)
{
    ALL("setGpioLevel");
    int gpio_id;
    char *gpio_name = parsed_req->url + os_strlen(f_str("/api/gpio/"));
    if (os_strlen(gpio_name) == 0)
    {
        http_response(ptr_espconn, HTTP_BAD_REQUEST, HTTP_CONTENT_JSON, f_str("No GPIO ID provided"), false);
        return;
    }
    gpio_id = atoi(gpio_name);

    int output_level;
    JSONP req_gpio(parsed_req->req_content, parsed_req->content_len);
    char output_level_str[16];
    req_gpio.getStr("gpio_level", output_level_str, 16);
    if (req_gpio.getErr() != JSON_noerr)
    {
        http_response(ptr_espconn, HTTP_BAD_REQUEST, HTTP_CONTENT_JSON, f_str("Json bad syntax"), false);
        return;
    }

    if ((os_strcmp(output_level_str, f_str("LOW")) == 0) ||
        (os_strcmp(output_level_str, f_str("low")) == 0))
        output_level = ESPBOT_LOW;
    else if ((os_strcmp(output_level_str, f_str("HIGH")) == 0) ||
             (os_strcmp(output_level_str, f_str("high")) == 0))
        output_level = ESPBOT_HIGH;
    else
    {
        http_response(ptr_espconn, HTTP_BAD_REQUEST, HTTP_CONTENT_JSON, f_str("Wrong GPIO level"), false);
        return;
    }
    int result = esp_gpio.set(gpio_id, output_level);

    // {"gpio_id":,"gpio_level":"unprovisioned"}
    Heap_chunk msg(48, dont_free);
    if (msg.ref == NULL)
    {
        esp_diag.error(ROUTES_SETGPIOLEVEL_HEAP_EXHAUSTED, 48);
        ERROR("setGpioLevel heap exhausted %d", 48);
        http_response(ptr_espconn, HTTP_SERVER_ERROR, HTTP_CONTENT_JSON, f_str("Heap exhausted"), false);
        return;
    }
    switch (result)
    {
    case ESPBOT_GPIO_WRONG_IDX:
        http_response(ptr_espconn, HTTP_BAD_REQUEST, HTTP_CONTENT_JSON, f_str("Wrong GPIO ID"), false);
        return;
    case ESPBOT_GPIO_UNPROVISIONED:
        http_response(ptr_espconn, HTTP_BAD_REQUEST, HTTP_CONTENT_JSON, f_str("GPIO is unprovisioned"), false);
        return;
    case ESPBOT_GPIO_WRONG_LVL:
        http_response(ptr_espconn, HTTP_BAD_REQUEST, HTTP_CONTENT_JSON, f_str("Wrong output level"), false);
        return;
    case ESPBOT_GPIO_CANNOT_CHANGE_INPUT:
        http_response(ptr_espconn, HTTP_BAD_REQUEST, HTTP_CONTENT_JSON, f_str("Cannot change input"), false);
        return;
    case ESPBOT_GPIO_OK:
    {
        int result = esp_gpio.read(gpio_id);
        switch (result)
        {
        case ESPBOT_GPIO_WRONG_IDX:
            http_response(ptr_espconn, HTTP_BAD_REQUEST, HTTP_CONTENT_JSON, f_str("Wrong GPIO ID"), false);
            return;
        case ESPBOT_GPIO_UNPROVISIONED:
            fs_sprintf(msg.ref, "{\"gpio_id\":%d,\"gpio_level\":\"unprovisioned\"}", gpio_id);
            break;
        case ESPBOT_LOW:
            fs_sprintf(msg.ref, "{\"gpio_id\":%d,\"gpio_level\":\"low\"}", gpio_id);
            break;
        case ESPBOT_HIGH:
            fs_sprintf(msg.ref, "{\"gpio_id\":%d,\"gpio_level\":\"high\"}", gpio_id);
            break;
        default:
            break;
        }
        http_response(ptr_espconn, HTTP_OK, HTTP_CONTENT_JSON, msg.ref, true);
        return;
    }
    default:
        break;
    }
    espmem.stack_mon();
}

static void getMdns(struct espconn *ptr_espconn, Http_parsed_req *parsed_req)
{
    ALL("getMdns");
    // "{"mdns_enabled": 0}" 20 chars
    int msg_len = 20;
    Heap_chunk msg(msg_len, dont_free);
    if (msg.ref)
    {
        fs_sprintf(msg.ref,
                   "{\"mdns_enabled\": %d}",
                   esp_mDns.is_enabled());
        http_response(ptr_espconn, HTTP_OK, HTTP_CONTENT_JSON, msg.ref, true);
    }
    else
    {
        esp_diag.error(ROUTES_GETMDNS_HEAP_EXHAUSTED, msg_len);
        ERROR("getMdns heap exhausted %d", msg_len);
        http_response(ptr_espconn, HTTP_SERVER_ERROR, HTTP_CONTENT_JSON, f_str("Heap exhausted"), false);
    }
}

static void setMdns(struct espconn *ptr_espconn, Http_parsed_req *parsed_req)
{
    ALL("setMdns");
    JSONP mdns_cfg(parsed_req->req_content, parsed_req->content_len);
    int mdns_enabled = mdns_cfg.getInt("mdns_enabled");
    if (mdns_cfg.getErr() != JSON_noerr)
    {
        http_response(ptr_espconn, HTTP_BAD_REQUEST, HTTP_CONTENT_JSON, f_str("Json bad syntax"), false);
        return;
    }
    if (mdns_enabled)
        esp_mDns.enable();
    else
        esp_mDns.disable();
    esp_mDns.save_cfg();

    int msg_len = 36;
    Heap_chunk msg(msg_len, dont_free);
    if (msg.ref == NULL)
    {
        esp_diag.error(ROUTES_SETMDNS_HEAP_EXHAUSTED, msg_len);
        ERROR("setMdns heap exhausted %d", msg_len);
        http_response(ptr_espconn, HTTP_SERVER_ERROR, HTTP_CONTENT_JSON, f_str("Heap exhausted"), false);
        return;
    }
    fs_sprintf(msg.ref,
               "{\"mdns_enabled\": %d}",
               esp_mDns.is_enabled());
    http_response(ptr_espconn, HTTP_OK, HTTP_CONTENT_JSON, msg.ref, true);
    espmem.stack_mon();
}

static void getTimedateCfg(struct espconn *ptr_espconn, Http_parsed_req *parsed_req)
{
    ALL("getTimedateCfg");
    // "{"sntp_enabled": 0,"timezone": -12}" 36 chars
    int msg_len = 36;
    Heap_chunk msg(msg_len, dont_free);
    if (msg.ref)
    {
        fs_sprintf(msg.ref,
                   "{\"sntp_enabled\": %d,\"timezone\": %d}",
                   esp_time.sntp_enabled(),
                   esp_time.get_timezone());
        http_response(ptr_espconn, HTTP_OK, HTTP_CONTENT_JSON, msg.ref, true);
    }
    else
    {
        esp_diag.error(ROUTES_GETTIMEDATECFG_HEAP_EXHAUSTED, msg_len);
        ERROR("getTimedateCfg heap exhausted %d", msg_len);
        http_response(ptr_espconn, HTTP_SERVER_ERROR, HTTP_CONTENT_JSON, f_str("Heap exhausted"), false);
    }
}

static void setTimedateCfg(struct espconn *ptr_espconn, Http_parsed_req *parsed_req)
{
    ALL("setTimedateCfg");
    JSONP req_timedate(parsed_req->req_content, parsed_req->content_len);
    int sntp_enabled = req_timedate.getInt("sntp_enabled");
    int timezone = req_timedate.getInt("timezone");
    if (req_timedate.getErr() != JSON_noerr)
    {
        http_response(ptr_espconn, HTTP_BAD_REQUEST, HTTP_CONTENT_JSON, f_str("Json bad syntax"), false);
        return;
    }

    if (sntp_enabled)
        esp_time.enable_sntp();
    else
        esp_time.disable_sntp();
    esp_time.set_timezone(timezone);
    esp_time.save_cfg();

    int msg_len = 36;
    Heap_chunk msg(msg_len, dont_free);
    if (msg.ref == NULL)
    {
        esp_diag.error(ROUTES_SETTIMEDATECFG_HEAP_EXHAUSTED, msg_len);
        ERROR("setTimedateCfg heap exhausted %d", msg_len);
        http_response(ptr_espconn, HTTP_SERVER_ERROR, HTTP_CONTENT_JSON, f_str("Heap exhausted"), false);
        return;
    }
    fs_sprintf(msg.ref,
               "{\"sntp_enabled\": %d,\"timezone\": %d}",
               esp_time.sntp_enabled(),
               esp_time.get_timezone());
    http_response(ptr_espconn, HTTP_OK, HTTP_CONTENT_JSON, msg.ref, true);
    espmem.stack_mon();
}

static void getTimedate(struct espconn *ptr_espconn, Http_parsed_req *parsed_req)
{
    ALL("getTimedate");
    // "{"timestamp":1589272200,"timedate":"Tue May 12 09:30:00 2020","sntp_enabled":0,"timezone": -12}" 95 chars
    int msg_len = 95 + 1;
    Heap_chunk msg(msg_len, dont_free);
    if (msg.ref)
    {
        uint32 current_timestamp = esp_time.get_timestamp();
        fs_sprintf(msg.ref,
                   "{\"timestamp\":%d,\"date\":\"%s\",",
                   current_timestamp,
                   esp_time.get_timestr(current_timestamp));
        fs_sprintf(msg.ref + os_strlen(msg.ref),
                   "\"sntp_enabled\":%d,\"timezone\":%d}",
                   esp_time.sntp_enabled(),
                   esp_time.get_timezone());
        http_response(ptr_espconn, HTTP_OK, HTTP_CONTENT_JSON, msg.ref, true);
    }
    else
    {
        esp_diag.error(ROUTES_GETTIMEDATE_HEAP_EXHAUSTED, msg_len);
        ERROR("getTimedate heap exhausted %d", msg_len);
        http_response(ptr_espconn, HTTP_SERVER_ERROR, HTTP_CONTENT_JSON, f_str("Heap exhausted"), false);
    }
}

static void setTimedate(struct espconn *ptr_espconn, Http_parsed_req *parsed_req)
{
    ALL("setTimedate");
    JSONP req_timedate(parsed_req->req_content, parsed_req->content_len);
    int32 timestamp = (int32)req_timedate.getInt("timestamp");
    if (req_timedate.getErr() != JSON_noerr)
    {
        http_response(ptr_espconn, HTTP_BAD_REQUEST, HTTP_CONTENT_JSON, f_str("Json bad syntax"), false);
        return;
    }

    esp_time.set_time_manually((uint32)timestamp);

    // "{"timestamp": 01234567891}"
    int msg_len = 27;
    Heap_chunk msg(msg_len, dont_free);
    if (msg.ref == NULL)
    {
        esp_diag.error(ROUTES_SETTIMEDATE_HEAP_EXHAUSTED, msg_len);
        ERROR("setTimedate heap exhausted %d", msg_len);
        http_response(ptr_espconn, HTTP_SERVER_ERROR, HTTP_CONTENT_JSON, f_str("Heap exhausted"), false);
        return;
    }
    fs_sprintf(msg.ref,
               "{\"timestamp\": %d}",
               timestamp);
    http_response(ptr_espconn, HTTP_OK, HTTP_CONTENT_JSON, msg.ref, true);
    espmem.stack_mon();
}

static void ota_answer_on_completion(void *param)
{
    struct espconn *ptr_espconn = (struct espconn *)param;
    switch (esp_ota.get_last_result())
    {
    case OTA_SUCCESS:
        http_response(ptr_espconn, HTTP_OK, HTTP_CONTENT_JSON, f_str("{\"msg\":\"OTA completed. Rebooting...\"}"), false);
        break;
    case OTA_FAILED:
        http_response(ptr_espconn, HTTP_SERVER_ERROR, HTTP_CONTENT_JSON, f_str("OTA failed"), false);
        break;
    case OTA_ALREADY_TO_THE_LATEST:
        http_response(ptr_espconn, HTTP_OK, HTTP_CONTENT_JSON, f_str("{\"msg\":\"Binary version already to the latest\"}"), false);
        break;
    default:
        break;
    }
}

static void startOTA(struct espconn *ptr_espconn, Http_parsed_req *parsed_req)
{
    ALL("startOTA");
    esp_ota.set_cb_on_completion(ota_answer_on_completion);
    esp_ota.set_cb_param((void *)ptr_espconn);
    esp_ota.start_upgrade();
}

static void getOtaCfg(struct espconn *ptr_espconn, Http_parsed_req *parsed_req)
{
    ALL("getOtaCfg");
    int msg_len = 90 +
                  16 +
                  6 +
                  os_strlen(esp_ota.get_path()) +
                  10;
    Heap_chunk msg(msg_len, dont_free);
    if (msg.ref)
    {
        fs_sprintf(msg.ref,
                   "{\"host\":\"%s\",\"port\":%d,\"path\":\"%s\",",
                   esp_ota.get_host(),
                   esp_ota.get_port(),
                   esp_ota.get_path());
        fs_sprintf((msg.ref + os_strlen(msg.ref)),
                   "\"check_version\":%d,\"reboot_on_completion\":%d}",
                   esp_ota.get_check_version(),
                   esp_ota.get_reboot_on_completion());
        http_response(ptr_espconn, HTTP_OK, HTTP_CONTENT_JSON, msg.ref, true);
    }
    else
    {
        esp_diag.error(ROUTES_GETOTACFG_HEAP_EXHAUSTED, msg_len);
        ERROR("getOtaCfg heap exhausted %d", msg_len);
        http_response(ptr_espconn, HTTP_SERVER_ERROR, HTTP_CONTENT_JSON, f_str("Heap exhausted"), false);
    }
}

static void setOtaCfg(struct espconn *ptr_espconn, Http_parsed_req *parsed_req)
{
    ALL("setOtaCfg");
    JSONP req_otacfg(parsed_req->req_content, parsed_req->content_len);
    char host[16];
    req_otacfg.getStr("host", host, 16);
    int port = req_otacfg.getInt("port");
    char path[128];
    req_otacfg.getStr("path", path, 128);
    bool check_version = (bool)req_otacfg.getInt("check_version");
    bool reboot_on_completion = req_otacfg.getInt("reboot_on_completion");
    if (req_otacfg.getErr() != JSON_noerr)
    {
        http_response(ptr_espconn, HTTP_BAD_REQUEST, HTTP_CONTENT_JSON, f_str("Json bad syntax"), false);
        return;
    }

    esp_ota.set_host(host);
    esp_ota.set_port(port);
    esp_ota.set_path(path);
    esp_ota.set_check_version(check_version);
    esp_ota.set_reboot_on_completion(reboot_on_completion);
    esp_ota.save_cfg();

    // {"host":"","port":,"path":"","check_version":,"reboot_on_completion":}

    int msg_len = 70 + 15 + 5 + os_strlen(esp_ota.get_path()) + 1 + 1 + 1;
    Heap_chunk msg(msg_len, dont_free);
    if (msg.ref == NULL)
    {
        esp_diag.error(ROUTES_SETOTACFG_HEAP_EXHAUSTED, msg_len);
        ERROR("setOtaCfg heap exhausted %d", msg_len);
        http_response(ptr_espconn, HTTP_SERVER_ERROR, HTTP_CONTENT_JSON, f_str("Heap exhausted"), false);
        return;
    }
    fs_sprintf(msg.ref,
               "{\"host\":\"%s\",\"port\":%d,\"path\":\"%s\",",
               esp_ota.get_host(),
               esp_ota.get_port(),
               esp_ota.get_path());
    fs_sprintf((msg.ref + os_strlen(msg.ref)),
               "\"check_version\":%d,\"reboot_on_completion\":%d}",
               esp_ota.get_check_version(),
               esp_ota.get_reboot_on_completion());
    http_response(ptr_espconn, HTTP_OK, HTTP_CONTENT_JSON, msg.ref, true);
    espmem.stack_mon();
}

static void getWifi(struct espconn *ptr_espconn, Http_parsed_req *parsed_req)
{
    ALL("getWifi");
    // {"op_mode":"STATION","SSID":"","ip_address":"123.123.123.123"}
    Heap_chunk msg(62 + 32 + 1, dont_free);
    if (msg.ref)
    {
        switch (Wifi::get_op_mode())
        {
        case STATION_MODE:
            fs_sprintf(msg.ref, "{\"op_mode\":\"STATION\",\"SSID\":\"%s\",", Wifi::station_get_ssid());
            break;
        case SOFTAP_MODE:
            fs_sprintf(msg.ref, "{\"op_mode\":\"AP\",");
            break;
        case STATIONAP_MODE:
            fs_sprintf(msg.ref, "{\"op_mode\":\"AP\",");
            break;
        default:
            break;
        }
        char *ptr = msg.ref + os_strlen(msg.ref);
        struct ip_info tmp_ip;
        Wifi::get_ip_address(&tmp_ip);
        char *ip_ptr = (char *)&tmp_ip.ip.addr;
        fs_sprintf(ptr, "\"ip_address\":\"%d.%d.%d.%d\"}", ip_ptr[0], ip_ptr[1], ip_ptr[2], ip_ptr[3]);
        http_response(ptr_espconn, HTTP_OK, HTTP_CONTENT_JSON, msg.ref, true);
    }
    else
    {
        esp_diag.error(ROUTES_GETWIFI_HEAP_EXHAUSTED, 44 + 32 + 42);
        ERROR("getWifi heap exhausted %d", 44 + 32 + 42);
        http_response(ptr_espconn, HTTP_SERVER_ERROR, HTTP_CONTENT_JSON, f_str("Heap exhausted"), false);
    }
}

static void getWifiApCfg(struct espconn *ptr_espconn, Http_parsed_req *parsed_req)
{
    ALL("getWifiApCfg");
    int msg_len = 29 + 64 + 1;
    Heap_chunk msg(msg_len, dont_free);
    if (msg.ref)
    {
        // {"ap_channel":11,"ap_pwd":""}
        fs_sprintf(msg.ref,
                   "{\"ap_channel\":%d,\"ap_pwd\":\"%s\"}",
                   Wifi::ap_get_ch(),
                   Wifi::ap_get_password());
        http_response(ptr_espconn, HTTP_OK, HTTP_CONTENT_JSON, msg.ref, true);
    }
    else
    {
        esp_diag.error(ROUTES_GETWIFIAPCFG_HEAP_EXHAUSTED, msg_len);
        ERROR("getWifiApCfg heap exhausted %d", msg_len);
        http_response(ptr_espconn, HTTP_SERVER_ERROR, HTTP_CONTENT_JSON, f_str("Heap exhausted"), false);
    }
}

static void setWifiApCfg(struct espconn *ptr_espconn, Http_parsed_req *parsed_req)
{
    ALL("setWifiApCfg");
    JSONP req_apcfg(parsed_req->req_content, parsed_req->content_len);
    int ap_channel = req_apcfg.getInt("ap_channel");
    char ap_pwd[64];
    req_apcfg.getStr("ap_pwd", ap_pwd, 64);
    if (req_apcfg.getErr() != JSON_noerr)
    {
        http_response(ptr_espconn, HTTP_BAD_REQUEST, HTTP_CONTENT_JSON, f_str("Json bad syntax"), false);
        return;
    }

    Wifi::ap_set_ch(ap_channel);
    Wifi::ap_set_pwd(ap_pwd, os_strlen(ap_pwd));
    Wifi::cfg_save();
    espmem.stack_mon();

    int msg_len = 29 + 64 + 1;
    Heap_chunk msg(msg_len, dont_free);
    if (msg.ref == NULL)
    {
        esp_diag.error(ROUTES_SETWIFIAPCFG_HEAP_EXHAUSTED, msg_len);
        ERROR("setWifiApCfg heap exhausted %d", msg_len);
        http_response(ptr_espconn, HTTP_SERVER_ERROR, HTTP_CONTENT_JSON, f_str("Heap exhausted"), false);
        return;
    }
    // {"ap_channel":11,"ap_pwd":""}
    fs_sprintf(msg.ref,
               "{\"ap_channel\":%d,\"ap_pwd\":\"%s\"}",
               Wifi::ap_get_ch(),
               Wifi::ap_get_password());
    http_response(ptr_espconn, HTTP_OK, HTTP_CONTENT_JSON, msg.ref, true);
}

static void getWifiStationCfg(struct espconn *ptr_espconn, Http_parsed_req *parsed_req)
{
    ALL("getWifiStationCfg");
    int msg_len = 36 + 32 + 64 + 1;
    Heap_chunk msg(msg_len, dont_free);
    if (msg.ref)
    {
        // {"station_ssid":"","station_pwd":""}
        fs_sprintf(msg.ref,
                   "{\"station_ssid\":\"%s\",\"station_pwd\":\"%s\"}",
                   Wifi::station_get_ssid(),
                   Wifi::station_get_password());
        http_response(ptr_espconn, HTTP_OK, HTTP_CONTENT_JSON, msg.ref, true);
    }
    else
    {
        esp_diag.error(ROUTES_GETWIFISTATIONCFG_HEAP_EXHAUSTED, msg_len);
        ERROR("getWifiStationCfg heap exhausted %d", msg_len);
        http_response(ptr_espconn, HTTP_SERVER_ERROR, HTTP_CONTENT_JSON, f_str("Heap exhausted"), false);
    }
}

static void setWifiStationCfg(struct espconn *ptr_espconn, Http_parsed_req *parsed_req)
{
    ALL("setWifiStationCfg");
    JSONP req_stationcfg(parsed_req->req_content, parsed_req->content_len);
    char ssid[32];
    req_stationcfg.getStr("station_ssid", ssid, 32);
    char pwd[64];
    req_stationcfg.getStr("station_pwd", pwd, 64);
    if (req_stationcfg.getErr() != JSON_noerr)
    {
        http_response(ptr_espconn, HTTP_BAD_REQUEST, HTTP_CONTENT_JSON, f_str("Json bad syntax"), false);
        return;
    }

    Wifi::station_set_ssid(ssid, os_strlen(ssid));
    Wifi::station_set_pwd(pwd, os_strlen(pwd));

    // wait before connecting to the AP so that the client could get the response
    os_timer_disarm(&delay_timer);
    os_timer_setfn(&delay_timer, (os_timer_func_t *)Wifi::connect, NULL);
    os_timer_arm(&delay_timer, 1000, 0);
    // Wifi::connect();
    espmem.stack_mon();

    Heap_chunk msg(140, dont_free);
    if (msg.ref == NULL)
    {
        esp_diag.error(ROUTES_SETWIFISTATIONCFG_HEAP_EXHAUSTED, 140);
        ERROR("setWifiStationCfg heap exhausted %d", 140);
        http_response(ptr_espconn, HTTP_SERVER_ERROR, HTTP_CONTENT_JSON, f_str("Heap exhausted"), false);
        return;
    }
    fs_sprintf(msg.ref,
               "{\"station_ssid\":\"%s\",\"station_pwd\":\"%s\"}",
               Wifi::station_get_ssid(),
               Wifi::station_get_password());
    http_response(ptr_espconn, HTTP_OK, HTTP_CONTENT_JSON, msg.ref, true);
}

static void connectWifi(struct espconn *ptr_espconn, Http_parsed_req *parsed_req)
{
    ALL("connectWifi");
    // {"msg":"Connecting to SSID ..."}
    Heap_chunk msg(32 + 32 + 1, dont_free);
    if (msg.ref)
    {
        fs_sprintf(msg.ref, "{\"msg\":\"Connecting to SSID %s...\",", Wifi::station_get_ssid());
        http_response(ptr_espconn, HTTP_ACCEPTED, HTTP_CONTENT_JSON, msg.ref, true);
        os_timer_disarm(&delay_timer);
        os_timer_setfn(&delay_timer, (os_timer_func_t *)Wifi::connect, NULL);
        os_timer_arm(&delay_timer, 500, 0);
    }
    else
    {
        esp_diag.error(ROUTES_CONNECTWIFI_HEAP_EXHAUSTED, 44 + 32 + 42);
        ERROR("connectWifi heap exhausted %d", 44 + 32 + 42);
        http_response(ptr_espconn, HTTP_SERVER_ERROR, HTTP_CONTENT_JSON, f_str("Heap exhausted"), false);
    }
}

static void disconnectWifi(struct espconn *ptr_espconn, Http_parsed_req *parsed_req)
{
    ALL("disconnectWifi");
    // {"msg":"Disconnecting from SSID ..."}
    Heap_chunk msg(37 + 32 + 1, dont_free);
    if (msg.ref)
    {
        fs_sprintf(msg.ref, "{\"msg\":\"Disconnecting from SSID %s...\",", Wifi::station_get_ssid());
        http_response(ptr_espconn, HTTP_ACCEPTED, HTTP_CONTENT_JSON, msg.ref, true);
        os_timer_disarm(&delay_timer);
        os_timer_setfn(&delay_timer, (os_timer_func_t *)Wifi::set_stationap, NULL);
        os_timer_arm(&delay_timer, 500, 0);
    }
    else
    {
        esp_diag.error(ROUTES_DISCONNECTWIFI_HEAP_EXHAUSTED, 44 + 32 + 42);
        ERROR("disconnectWifi heap exhausted %d", 44 + 32 + 42);
        http_response(ptr_espconn, HTTP_SERVER_ERROR, HTTP_CONTENT_JSON, f_str("Heap exhausted"), false);
    }
}

void espbot_http_routes(struct espconn *ptr_espconn, Http_parsed_req *parsed_req)
{
    ALL("espbot_http_routes");

    if (parsed_req->req_method == HTTP_OPTIONS) // HTTP CORS
    {
        preflight_response(ptr_espconn, parsed_req);
        return;
    }
    if ((0 == os_strcmp(parsed_req->url, f_str("/"))) && (parsed_req->req_method == HTTP_GET))
    {
        return_file(ptr_espconn, parsed_req, (char *)f_str("index.html"));
        return;
    }
    if ((os_strncmp(parsed_req->url, f_str("/api/"), 5)) && (parsed_req->req_method == HTTP_GET))
    {
        // not an api: look for specified file
        char *file_name = parsed_req->url + os_strlen("/");
        return_file(ptr_espconn, parsed_req, file_name);
        return;
    }
    if ((0 == os_strcmp(parsed_req->url, f_str("/api/cron"))) && (parsed_req->req_method == HTTP_GET))
    {
        getCron(ptr_espconn, parsed_req);
        return;
    }
    if ((0 == os_strcmp(parsed_req->url, f_str("/api/cron"))) && (parsed_req->req_method == HTTP_POST))
    {
        setCron(ptr_espconn, parsed_req);
        return;
    }
    if ((0 == os_strcmp(parsed_req->url, f_str("/api/debug/lastReset"))) && (parsed_req->req_method == HTTP_GET))
    {
        getLastReset(ptr_espconn, parsed_req);
        return;
    }
    if ((0 == os_strcmp(parsed_req->url, f_str("/api/debug/hexMemDump"))) && (parsed_req->req_method == HTTP_POST))
    {
        getHexMemDump(ptr_espconn, parsed_req);
        return;
    }
    if ((0 == os_strcmp(parsed_req->url, f_str("/api/debug/memDump"))) && (parsed_req->req_method == HTTP_POST))
    {
        getMemDump(ptr_espconn, parsed_req);
        return;
    }
    if ((0 == os_strcmp(parsed_req->url, f_str("/api/debug/memInfo"))) && (parsed_req->req_method == HTTP_GET))
    {
        getMemInfo(ptr_espconn, parsed_req);
        return;
    }
    if ((0 == os_strcmp(parsed_req->url, f_str("/api/deviceName"))) && (parsed_req->req_method == HTTP_GET))
    {
        getDeviceName(ptr_espconn, parsed_req);
        return;
    }
    if ((0 == os_strcmp(parsed_req->url, f_str("/api/deviceName"))) && (parsed_req->req_method == HTTP_POST))
    {
        setDeviceName(ptr_espconn, parsed_req);
        return;
    }
    if ((0 == os_strcmp(parsed_req->url, f_str("/api/diagnostic"))) && (parsed_req->req_method == HTTP_GET))
    {
        getDiagnosticEvents(ptr_espconn, parsed_req);
        return;
    }
    if ((0 == os_strcmp(parsed_req->url, f_str("/api/diagnostic"))) && (parsed_req->req_method == HTTP_POST))
    {
        ackDiagnosticEvents(ptr_espconn, parsed_req);
        return;
    }
    if ((0 == os_strcmp(parsed_req->url, f_str("/api/diagnostic/cfg"))) && (parsed_req->req_method == HTTP_GET))
    {
        getDiagnosticCfg(ptr_espconn, parsed_req);
        return;
    }
    if ((0 == os_strcmp(parsed_req->url, f_str("/api/diagnostic/cfg"))) && (parsed_req->req_method == HTTP_POST))
    {
        setDiagnosticCfg(ptr_espconn, parsed_req);
        return;
    }
    if ((0 == os_strcmp(parsed_req->url, f_str("/api/file"))) && (parsed_req->req_method == HTTP_GET))
    {
        getFileList(ptr_espconn, parsed_req);
        return;
    }
    if ((0 == os_strncmp(parsed_req->url, f_str("/api/file/"), os_strlen(f_str("/api/file/")))) && (parsed_req->req_method == HTTP_GET))
    {
        getFile(ptr_espconn, parsed_req);
        return;
    }
    if ((0 == os_strncmp(parsed_req->url, f_str("/api/file/"), os_strlen(f_str("/api/file/")))) && (parsed_req->req_method == HTTP_POST))
    {
        createFile(ptr_espconn, parsed_req);
        return;
    }
    if ((0 == os_strncmp(parsed_req->url, f_str("/api/file/"), os_strlen(f_str("/api/file/")))) && (parsed_req->req_method == HTTP_PUT))
    {
        appendToFile(ptr_espconn, parsed_req);
        return;
    }
    if ((0 == os_strncmp(parsed_req->url, f_str("/api/file/"), os_strlen(f_str("/api/file/")))) && (parsed_req->req_method == HTTP_DELETE))
    {
        deleteFile(ptr_espconn, parsed_req);
        return;
    }
    if ((0 == os_strcmp(parsed_req->url, f_str("/api/fs"))) && (parsed_req->req_method == HTTP_GET))
    {
        getFs(ptr_espconn, parsed_req);
        return;
    }
    if ((0 == os_strcmp(parsed_req->url, f_str("/api/fs/check"))) && (parsed_req->req_method == HTTP_POST))
    {
        checkFS(ptr_espconn, parsed_req);
        return;
    }
    if ((0 == os_strncmp(parsed_req->url, f_str("/api/gpio/cfg/"), os_strlen(f_str("/api/gpio/cfg/")))) && (parsed_req->req_method == HTTP_GET))
    {
        getGpioCfg(ptr_espconn, parsed_req);
        return;
    }
    if ((0 == os_strncmp(parsed_req->url, f_str("/api/gpio/cfg/"), os_strlen(f_str("/api/gpio/cfg/")))) && (parsed_req->req_method == HTTP_POST))
    {
        setGpioCfg(ptr_espconn, parsed_req);
        return;
    }
    if ((0 == os_strncmp(parsed_req->url, f_str("/api/gpio/"), os_strlen(f_str("/api/gpio/")))) && (parsed_req->req_method == HTTP_GET))
    {
        getGpioLevel(ptr_espconn, parsed_req);
        return;
    }
    if (0 == (os_strncmp(parsed_req->url, f_str("/api/gpio/"), os_strlen(f_str("/api/gpio/")))) && (parsed_req->req_method == HTTP_POST))
    {
        setGpioLevel(ptr_espconn, parsed_req);
        return;
    }
    if ((0 == os_strcmp(parsed_req->url, f_str("/api/mdns"))) && (parsed_req->req_method == HTTP_GET))
    {
        getMdns(ptr_espconn, parsed_req);
        return;
    }
    if ((0 == os_strcmp(parsed_req->url, f_str("/api/mdns"))) && (parsed_req->req_method == HTTP_POST))
    {
        setMdns(ptr_espconn, parsed_req);
        return;
    }
    if ((0 == os_strcmp(parsed_req->url, f_str("/api/reboot"))) && (parsed_req->req_method == HTTP_POST))
    {
        http_response(ptr_espconn, HTTP_ACCEPTED, HTTP_CONTENT_JSON, f_str("{\"msg\":\"Device rebooting...\"}"), false);
        espbot.reset(ESP_REBOOT);
        return;
    }
    if ((0 == os_strcmp(parsed_req->url, f_str("/api/timedate"))) && (parsed_req->req_method == HTTP_GET))
    {
        getTimedate(ptr_espconn, parsed_req);
        return;
    }
    if ((0 == os_strcmp(parsed_req->url, f_str("/api/timedate"))) && (parsed_req->req_method == HTTP_POST))
    {
        setTimedate(ptr_espconn, parsed_req);
        return;
    }
    if ((0 == os_strcmp(parsed_req->url, f_str("/api/timedate/cfg"))) && (parsed_req->req_method == HTTP_GET))
    {
        getTimedateCfg(ptr_espconn, parsed_req);
        return;
    }
    if ((0 == os_strcmp(parsed_req->url, f_str("/api/timedate/cfg"))) && (parsed_req->req_method == HTTP_POST))
    {
        setTimedateCfg(ptr_espconn, parsed_req);
        return;
    }
    if ((0 == os_strcmp(parsed_req->url, f_str("/api/ota"))) && (parsed_req->req_method == HTTP_POST))
    {
        startOTA(ptr_espconn, parsed_req);
        return;
    }
    if ((0 == os_strcmp(parsed_req->url, f_str("/api/ota/cfg"))) && (parsed_req->req_method == HTTP_GET))
    {
        getOtaCfg(ptr_espconn, parsed_req);
        return;
    }
    if ((os_strcmp(parsed_req->url, f_str("/api/ota/cfg")) == 0) && (parsed_req->req_method == HTTP_POST))
    {
        setOtaCfg(ptr_espconn, parsed_req);
        return;
    }
    if ((0 == os_strcmp(parsed_req->url, f_str("/api/ota/reboot"))) && (parsed_req->req_method == HTTP_POST))
    {
        http_response(ptr_espconn, HTTP_ACCEPTED, HTTP_CONTENT_JSON, f_str("{\"msg\":\"Rebooting after OTA...\"}"), false);
        espbot.reset(ESP_OTA_REBOOT);
        return;
    }
    if ((0 == os_strcmp(parsed_req->url, f_str("/api/wifi"))) && (parsed_req->req_method == HTTP_GET))
    {
        getWifi(ptr_espconn, parsed_req);
        return;
    }
    if ((0 == os_strcmp(parsed_req->url, f_str("/api/wifi/scan"))) && (parsed_req->req_method == HTTP_GET))
    {
        Wifi::scan_for_ap(NULL, getAPlist, (void *)ptr_espconn);
        return;
    }
    if ((0 == os_strcmp(parsed_req->url, f_str("/api/wifi/ap/cfg"))) && (parsed_req->req_method == HTTP_GET))
    {
        getWifiApCfg(ptr_espconn, parsed_req);
        return;
    }
    if ((0 == os_strcmp(parsed_req->url, f_str("/api/wifi/ap/cfg"))) && (parsed_req->req_method == HTTP_POST))
    {
        setWifiApCfg(ptr_espconn, parsed_req);
        return;
    }
    if ((0 == os_strcmp(parsed_req->url, f_str("/api/wifi/station/cfg"))) && (parsed_req->req_method == HTTP_GET))
    {
        getWifiStationCfg(ptr_espconn, parsed_req);
        return;
    }
    if ((0 == os_strcmp(parsed_req->url, f_str("/api/wifi/station/cfg"))) && (parsed_req->req_method == HTTP_POST))
    {
        setWifiStationCfg(ptr_espconn, parsed_req);
        return;
    }
    if ((0 == os_strcmp(parsed_req->url, f_str("/api/wifi/connect"))) && (parsed_req->req_method == HTTP_POST))
    {
        connectWifi(ptr_espconn, parsed_req);
        return;
    }
    if ((0 == os_strcmp(parsed_req->url, f_str("/api/wifi/disconnect"))) && (parsed_req->req_method == HTTP_POST))
    {
        disconnectWifi(ptr_espconn, parsed_req);
        return;
    }
    //
    // now the custom app routes
    //
    if (app_http_routes(ptr_espconn, parsed_req))
        return;
    else
        http_response(ptr_espconn, HTTP_BAD_REQUEST, HTTP_CONTENT_JSON, f_str("I'm sorry, my responses are limited. You must ask the right question."), false);
}
