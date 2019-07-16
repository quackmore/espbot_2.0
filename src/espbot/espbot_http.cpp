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
#include "osapi.h"
#include "user_interface.h"
#include "mem.h"
#include "ip_addr.h"
}

#include "espbot_queue.hpp"
#include "espbot_list.hpp"
#include "espbot_http.hpp"
#include "espbot_http_routes.hpp"
#include "espbot.hpp"
#include "espbot_global.hpp"
#include "espbot_logger.hpp"
#include "espbot_json.hpp"
#include "espbot_utils.hpp"
#include "espbot_debug.hpp"

static int http_msg_max_size;

void set_http_msg_max_size(int size)
{
    http_msg_max_size = size;
}

int get_http_msg_max_size(void)
{
    return http_msg_max_size;
}

char *code_msg(int code)
{
    esplog.all("code_msg\n");
    switch (code)
    {
    case HTTP_OK:
        return "OK";
    case HTTP_BAD_REQUEST:
        return "Bad Request";
    case HTTP_UNAUTHORIZED:
        return "Unauthorized";
    case HTTP_FORBIDDEN:
        return "Forbidden";
    case HTTP_NOT_FOUND:
        return "Not Found";
    case HTTP_SERVER_ERROR:
        return "Internal Server Error";
    default:
        return "";
    }
}

char *json_error_msg(int code, char *msg)
{
    esplog.all("json_error_msg\n");
    Heap_chunk err_msg(54 + 3 + 22 + os_strlen(msg), dont_free);
    if (err_msg.ref)
    {
        os_sprintf(err_msg.ref,
                   "{\"error\":{\"code\": %d,\"message\": \"%s\",\"reason\": \"%s\"}}",
                   code, code_msg(code), msg);
        return err_msg.ref;
    }
    else
    {
        esplog.error("json_error_msg - not enough heap memory %d\n", (56 + os_strlen(msg)));
        return NULL;
    }
}

Http_header::Http_header()
{
    esplog.all("Http_header::Http_header\n");
    m_content_type = NULL;
    m_acrh = NULL;
    m_origin = NULL;
}

Http_header::~Http_header()
{
    esplog.all("Http_header::~Http_header\n");
    if (m_acrh)
        delete[] m_acrh;
    if (m_origin)
        delete[] m_origin;
}

//
// HTTP responding:
// ----------------
// to make sure espconn_send is called after espconn_sent_callback of the previous packet
// a flag is set before calling espconn_send (will be reset by sendcb)
//
// befor sending a response the flag will be checked
// when the flag is found set (espconn_send not done yet)
// the response is queued
//

static Queue<struct http_send> *pending_send;
static char *send_buffer;
static bool esp_busy_sending_data = false;

static os_timer_t clear_busy_sending_data_timer;

static void clear_busy_sending_data(void *arg)
{
    esplog.trace("clear_busy_sending_data\n");
    // something went wrong an this timeout was triggered
    // clear the flag, the buffer and trigger a check of the pending responses queue
    os_timer_disarm(&clear_busy_sending_data_timer);
    if (send_buffer)
    {
        delete[] send_buffer;
        send_buffer = NULL;
    }
    esp_busy_sending_data = false;
    system_os_post(USER_TASK_PRIO_0, SIG_HTTP_CHECK_PENDING_RESPONSE, '0');
}

void http_check_pending_send(void)
{
    esplog.all("http_check_pending_send\n");
    if (espwebsvr.get_status() == down)
    {
        // meanwhile the server went down
        esplog.trace("http_check_pending_send - clearing pending send and response queues\n");
        // clear the pending send queue
        struct http_send *p_pending_send = pending_send->front();
        while (p_pending_send)
        {
            pending_send->pop();
            delete[] p_pending_send->msg;
            delete p_pending_send;
            p_pending_send = pending_send->front();
        }
        // clear the split send queue
        struct http_split_send *p_pending_response = pending_split_send->front();
        while (p_pending_response)
        {
            pending_split_send->pop();
            delete[] p_pending_response->content;
            delete p_pending_response;
            p_pending_response = pending_split_send->front();
        }
        return;
    }
    // the server is up!
    // check pending send queue
    struct http_send *p_pending_send = pending_send->front();
    if (p_pending_send)
    {
        esplog.trace("pending send found: *p_espconn: %X, msg len: %d\n",
                     p_pending_send->p_espconn, os_strlen(p_pending_send->msg));
        http_send_buffer(p_pending_send->p_espconn, p_pending_send->msg);
        // the send procedure will clear the buffer so just delete the http_send
        // esplog.trace("http_check_pending_send: deleting p_pending_send\n");
        delete p_pending_send;
        pending_send->pop();
        // a pending response was found
        // wait for next pending response check so skip any other code
        return;
    }
    // no pending send was found
    // check other pending actions (such as long messages that required to be split)
    struct http_split_send *p_pending_response = pending_split_send->front();
    if (p_pending_response)
    {
        esplog.trace("pending split response found: *p_espconn: %X\n"
                     "                            content_size: %d\n"
                     "                     content_transferred: %d\n"
                     "                         action_function: %X\n",
                     p_pending_response->p_espconn,
                     p_pending_response->content_size,
                     p_pending_response->content_transferred,
                     p_pending_response->action_function);
        p_pending_response->action_function(p_pending_response);
        // don't free the content yet
        // esplog.trace("http_check_pending_send: deleting p_pending_response\n");
        delete p_pending_response;
        pending_split_send->pop();
        // serving just one pending_split_send, so that just one espconn_send is engaged
        // next one will be triggered by a espconn_send completion
    }
}

void http_sentcb(void *arg)
{
    esplog.all("http_sentcb\n");
    struct espconn *ptr_espconn = (struct espconn *)arg;
    // clear the flag and the timeout timer
    os_timer_disarm(&clear_busy_sending_data_timer);
    // clear the message_buffer
    if (send_buffer)
    {
        // esplog.trace("http_sentcb: deleting send_buffer %X\n", send_buffer);
        delete[] send_buffer;
        send_buffer = NULL;
    }
    esp_busy_sending_data = false;
    system_os_post(USER_TASK_PRIO_0, SIG_HTTP_CHECK_PENDING_RESPONSE, '0');
}

//
// won't check the length of the sent message
//
void http_send_buffer(struct espconn *p_espconn, char *msg)
{
    // Profiler ret_file("http_send_buffer");
    ETS_INTR_LOCK();
    if (esp_busy_sending_data) // previous espconn_send not completed yet
    {
        ETS_INTR_UNLOCK();
        esplog.trace("http_send_buffer - previous espconn_send not completed yet\n");
        struct http_send *response_data = new struct http_send;
        espmem.stack_mon();
        if (response_data)
        {
            response_data->p_espconn = p_espconn;
            response_data->msg = msg;
            Queue_err result = pending_send->push(response_data);
            if (result == Queue_full)
                esplog.error("http_send_buffer: pending send queue is full\n");
        }
        else
        {
            esplog.error("http_send_buffer: not enough heap memory (%d)\n", sizeof(struct http_send));
        }
    }
    else // previous espconn_send completed
    {
        esp_busy_sending_data = true;
        ETS_INTR_UNLOCK();
        esplog.trace("espconn_send: *p_espconn: %X\n"
                     "                     msg: %s\n",
                     p_espconn,
                     msg);
        // set a timeout timer for clearing the esp_busy_sending_data in case something goes wrong
        os_timer_disarm(&clear_busy_sending_data_timer);
        os_timer_setfn(&clear_busy_sending_data_timer, (os_timer_func_t *)clear_busy_sending_data, NULL);
        os_timer_arm(&clear_busy_sending_data_timer, 2000, 0);

        send_buffer = msg;
        sint8 res = espconn_send(p_espconn, (uint8 *)send_buffer, os_strlen(send_buffer));
        espmem.stack_mon();
        if (res)
        {
            esplog.error("http_send_buffer: error sending response, error code %d\n", res);
            // nevermind about sentcb, there is a timeout now
            // esp_busy_sending_data = false;
            // delete[] send_buffer;
            // send_buffer = NULL;
        }
        // esp_free(send_buffer); // http_sentcb will free it
    }
    system_soft_wdt_feed();
}

void http_response(struct espconn *p_espconn, int code, char *content_type, char *msg, bool free_msg)
{
    esplog.all("http_response\n");
    esplog.trace("response: *p_espconn: %X\n"
                 "                code: %d\n"
                 "        content-type: %s\n"
                 "          msg length: %d\n"
                 "            free_msg: %d\n",
                 p_espconn, code, content_type, os_strlen(msg), free_msg);
    // when code is not 200 format the error msg as json
    if (code >= HTTP_BAD_REQUEST)
    {
        char *err_msg = json_error_msg(code, msg);

        // free original message
        if (free_msg)
            delete[] msg;
        if (err_msg)
        {
            // replace original msg with the formatted one
            msg = err_msg;
            free_msg = true;
        }
        else
        {
            return;
        }
    }
    // Now format the message header
    int header_len = 110 +
                     3 +
                     os_strlen(code_msg(code)) +
                     os_strlen(content_type) +
                     os_strlen(msg);
    Heap_chunk msg_header(header_len, dont_free);
    if (msg_header.ref == NULL)
    {
        esplog.error("http_response: not enough heap memory (%d)\n", header_len);
        return;
    }
    os_sprintf(msg_header.ref, "HTTP/1.0 %d %s\r\nServer: espbot/2.0\r\n"
                               "Content-Type: %s\r\n"
                               "Content-Length: %d\r\n"
                               "Access-Control-Allow-Origin: *\r\n\r\n",
               code, code_msg(code), content_type, os_strlen(msg));
    // send separately the header from the content
    // to avoid allocating twice the memory for the message
    // especially very large ones
    http_send_buffer(p_espconn, msg_header.ref);
    // when there is no message that's all
    if (os_strlen(msg) == 0)
        return;
    if (free_msg)
    {
        http_send(p_espconn, msg);
    }
    else
    {
        // response message is not allocated on heap
        // copy it to a buffer
        Heap_chunk msg_short(os_strlen(msg), dont_free);
        if (msg_header.ref)
        {
            os_strcpy(msg_short.ref, msg);
            http_send(p_espconn, msg_short.ref);
        }
        else
        {
            esplog.error("http_response: not enough heap memory (%d)\n", os_strlen(msg));
        }
    }
    espmem.stack_mon();
}

Queue<struct http_split_send> *pending_split_send;

static void send_remaining_msg(struct http_split_send *p_sr)
{
    esplog.all("http_send_remaining_msg\n");

    if ((p_sr->content_size - p_sr->content_transferred) > http_msg_max_size)
    {
        // the message is bigger than response_max_size
        // will split the message
        int buffer_size = http_msg_max_size;
        Heap_chunk buffer(buffer_size + 1, dont_free);
        if (buffer.ref)
        {
            struct http_split_send *p_pending_response = new struct http_split_send;
            if (p_pending_response)
            {
                os_strncpy(buffer.ref, p_sr->content + p_sr->content_transferred, buffer_size);
                // setup the remaining message
                p_pending_response->p_espconn = p_sr->p_espconn;
                p_pending_response->content = p_sr->content;
                p_pending_response->content_size = p_sr->content_size;
                p_pending_response->content_transferred = p_sr->content_transferred + buffer_size;
                p_pending_response->action_function = send_remaining_msg;
                Queue_err result = pending_split_send->push(p_pending_response);
                if (result == Queue_full)
                    esplog.error("http_send_buffer: pending response queue is full\n");

                esplog.trace("send_remaining_msg: *p_espconn: %X\n"
                             "msg (splitted) len: %d\n",
                             p_sr->p_espconn, os_strlen(buffer.ref));
                http_send_buffer(p_sr->p_espconn, buffer.ref);
            }
            else
            {
                esplog.error("send_remaining_msg: not enough heap memory (%d)\n", sizeof(struct http_split_send));
                delete[] buffer.ref;
                delete[] p_sr->content;
            }
        }
        else
        {
            esplog.error("send_remaining_msg: not enough heap memory (%d)\n", buffer_size);
            delete[] p_sr->content;
        }
    }
    else
    {
        // this is the last piece of the message
        int buffer_size = http_msg_max_size;
        Heap_chunk buffer(buffer_size + 1, dont_free);
        if (buffer.ref)
        {
            os_strncpy(buffer.ref, p_sr->content + p_sr->content_transferred, buffer_size);
            esplog.trace("send_remaining_msg: *p_espconn: %X\n"
                         "          msg (last piece) len: %d\n",
                         p_sr->p_espconn, os_strlen(buffer.ref));
            http_send_buffer(p_sr->p_espconn, buffer.ref);
        }
        else
        {
            esplog.error("send_remaining_msg: not enough heap memory (%d)\n", buffer_size);
        }
        // esplog.trace("send_remaining_msg: deleting p_sr->content\n");
        delete[] p_sr->content;
    }
}

//
// will split the message when the length is greater than http response_max_size
//
void http_send(struct espconn *p_espconn, char *msg)
{
    // Profiler ret_file("http_send");
    esplog.all("http_send\n");
    if (os_strlen(msg) > http_msg_max_size)
    {
        // the message is bigger than response_max_size
        // will split the message
        int buffer_size = http_msg_max_size;
        Heap_chunk buffer(buffer_size + 1, dont_free);
        if (buffer.ref)
        {
            struct http_split_send *p_pending_response = new struct http_split_send;
            if (p_pending_response)
            {
                os_strncpy(buffer.ref, msg, buffer_size);
                // setup the remaining message
                p_pending_response->p_espconn = p_espconn;
                p_pending_response->content = msg;
                p_pending_response->content_size = os_strlen(msg);
                p_pending_response->content_transferred = buffer_size;
                p_pending_response->action_function = send_remaining_msg;
                Queue_err result = pending_split_send->push(p_pending_response);
                if (result == Queue_full)
                    esplog.error("http_send_buffer: pending response queue is full\n");

                esplog.trace("http_send: *p_espconn: %X\n"
                             "       msg (splitted) len: %d\n",
                             p_espconn, os_strlen(buffer.ref));
                http_send_buffer(p_espconn, buffer.ref);
            }
            else
            {
                esplog.error("http_send: not enough heap memory (%d)\n", sizeof(struct http_split_send));
                delete[] buffer.ref;
                delete[] msg;
            }
        }
        else
        {
            esplog.error("http_send: not enough heap memory (%d)\n", buffer_size);
            delete[] msg;
        }
    }
    else
    {
        // no need to split the message, just send it
        esplog.trace("http_send: *p_espconn: %X\n"
                     "           msg (full) len: %d\n",
                     p_espconn, os_strlen(msg));
        http_send_buffer(p_espconn, msg);
    }
}

char *http_format_header(class Http_header *p_header)
{
    esplog.all("http_format_header\n");
    // allocate a buffer
    // HTTP...        ->  37 + 3 + 22 =  62
    // Content-Type   ->  19 + 17     =  36
    // Content-Length ->  22 + 5      =  27
    // Content-Range  ->  32 + 15     =  47
    // Pragma         ->  24          =  24
    //                                = 196
    int header_length = 196;
    if (p_header->m_acrh)
    {
        header_length += 37; // Access-Control-Request-Headers string format
        header_length += os_strlen(p_header->m_acrh);
        header_length += os_strlen("Access-Control-Allow-Methods: GET,POST,PUT,DELETE,OPTIONS\r\n");
    }

    if (p_header->m_origin)
    {
        header_length += 34; // Origin string format
        header_length += os_strlen(p_header->m_origin);
    }

    Heap_chunk header_msg(header_length, dont_free);
    if (header_msg.ref)
    {
        // setup the header
        char *ptr = header_msg.ref;
        os_sprintf(ptr, "HTTP/1.0 %d %s\r\nServer: espbot/2.0\r\n",
                   p_header->m_code, code_msg(p_header->m_code));
        ptr = ptr + os_strlen(ptr);
        os_sprintf(ptr, "Content-Type: %s\r\n", p_header->m_content_type);
        ptr = ptr + os_strlen(ptr);
        if (p_header->m_content_range_total > 0)
        {
            os_sprintf(ptr, "Content-Range: bytes %d-%d/%d\r\n", p_header->m_content_range_total, p_header->m_content_range_total, p_header->m_content_range_total);
            ptr = ptr + os_strlen(ptr);
        }
        os_sprintf(ptr, "Content-Length: %d\r\n", p_header->m_content_length);
        ptr = ptr + os_strlen(ptr);
        // os_sprintf(ptr, "Date: Wed, 28 Nov 2018 12:00:00 GMT\r\n");
        // os_printf("---->msg: %s\n", msg.ref);
        if (p_header->m_origin)
        {
            os_sprintf(ptr, "Access-Control-Allow-Origin: %s\r\n", p_header->m_origin);
            ptr = ptr + os_strlen(ptr);
        }
        if (p_header->m_acrh)
        {
            os_sprintf(ptr, "Access-Control-Allow-Methods: GET,POST,PUT,DELETE,OPTIONS\r\n");
            ptr = ptr + os_strlen(ptr);
            os_sprintf(ptr, "Access-Control-Allow-Headers: Content-Type,%s\r\n", p_header->m_acrh);
            ptr = ptr + os_strlen(ptr);
        }
        os_sprintf(ptr, "Pragma: no-cache\r\n\r\n");
        return header_msg.ref;
    }
    else
    {
        esplog.error("http_format_header: not enough heap memory (%d)\n", header_length);
        return NULL;
    }
}

// end of HTTP responding

//
// HTTP Receiving:
//

Http_parsed_req::Http_parsed_req()
{
    esplog.all("Http_parsed_req::Http_parsed_req\n");
    no_header_message = false;
    req_method = HTTP_UNDEFINED;
    acrh = NULL;
    origin = NULL;
    url = NULL;
    content_len = 0;
    req_content = NULL;
}

Http_parsed_req::~Http_parsed_req()
{
    esplog.all("Http_parsed_req::~Http_parsed_req\n");
    if (acrh)
        delete[] acrh;
    if (origin)
        delete[] origin;
    if (url)
        delete[] url;
    if (req_content)
        delete[] req_content;
}

void http_parse_request(char *req, Http_parsed_req *parsed_req)
{
    esplog.all("http_parse_request\n");
    char *tmp_ptr = req;
    char *end_ptr = NULL;
    espmem.stack_mon();
    int len = 0;

    if (tmp_ptr == NULL)
    {
        esplog.error("http_parse_request - cannot parse empty message\n");
        return;
    }

    if (os_strncmp(tmp_ptr, "GET ", 4) == 0)
    {
        parsed_req->req_method = HTTP_GET;
        tmp_ptr += 4;
    }
    else if (os_strncmp(tmp_ptr, "POST ", 5) == 0)
    {
        parsed_req->req_method = HTTP_POST;
        tmp_ptr += 5;
    }
    else if (os_strncmp(tmp_ptr, "PUT ", 4) == 0)
    {
        parsed_req->req_method = HTTP_PUT;
        tmp_ptr += 4;
    }
    else if (os_strncmp(tmp_ptr, "PATCH ", 6) == 0)
    {
        parsed_req->req_method = HTTP_PATCH;
        tmp_ptr += 6;
    }
    else if (os_strncmp(tmp_ptr, "DELETE ", 7) == 0)
    {
        parsed_req->req_method = HTTP_DELETE;
        tmp_ptr += 7;
    }
    else if (os_strncmp(tmp_ptr, "OPTIONS ", 8) == 0)
    {
        parsed_req->req_method = HTTP_OPTIONS;
        tmp_ptr += 8;
    }
    else
    {
        parsed_req->no_header_message = true;
    }

    if (parsed_req->no_header_message)
    {
        parsed_req->content_len = os_strlen(tmp_ptr);
        parsed_req->req_content = new char[parsed_req->content_len + 1];
        if (parsed_req->req_content == NULL)
        {
            esplog.error("http_parse_request - not enough heap memory\n");
            return;
        }
        os_memcpy(parsed_req->req_content, tmp_ptr, parsed_req->content_len);
        return;
    }

    // this is a standard request with header

    // checkout url
    end_ptr = (char *)os_strstr(tmp_ptr, " HTTP");
    if (end_ptr == NULL)
    {
        esplog.error("http_parse_request - cannot find HTTP token\n");
        return;
    }
    len = end_ptr - tmp_ptr;
    parsed_req->url = new char[len + 1];
    if (parsed_req->url == NULL)
    {
        esplog.error("http_parse_request - not enough heap memory\n");
        return;
    }
    os_memcpy(parsed_req->url, tmp_ptr, len);

    // checkout Access-Control-Request-Headers
    tmp_ptr = req;
    tmp_ptr = (char *)os_strstr(tmp_ptr, "Access-Control-Request-Headers: ");
    if (tmp_ptr == NULL)
    {
        tmp_ptr = req;
        tmp_ptr = (char *)os_strstr(tmp_ptr, "access-control-request-headers: ");
    }
    if (tmp_ptr != NULL)
    {
        tmp_ptr += 32;
        end_ptr = (char *)os_strstr(tmp_ptr, "\r\n");
        if (end_ptr == NULL)
        {
            esplog.error("http_parse_request - cannot find Access-Control-Request-Headers\n");
            return;
        }
        len = end_ptr - tmp_ptr;
        parsed_req->acrh = new char[len + 1];
        if (parsed_req->acrh == NULL)
        {
            esplog.error("http_parse_request - not enough heap memory\n");
            return;
        }
        os_strncpy(parsed_req->acrh, tmp_ptr, len);
    }

    // checkout Origin
    tmp_ptr = req;
    tmp_ptr = (char *)os_strstr(tmp_ptr, "Origin: ");
    if (tmp_ptr == NULL)
    {
        tmp_ptr = req;
        tmp_ptr = (char *)os_strstr(tmp_ptr, "origin: ");
    }
    if (tmp_ptr != NULL)
    {
        tmp_ptr += 8;
        end_ptr = (char *)os_strstr(tmp_ptr, "\r\n");
        if (end_ptr == NULL)
        {
            esplog.error("http_parse_request - cannot find origin\n");
            return;
        }
        len = end_ptr - tmp_ptr;
        parsed_req->origin = new char[len + 1];
        if (parsed_req->origin == NULL)
        {
            esplog.error("http_parse_request - not enough heap memory\n");
            return;
        }
        os_strncpy(parsed_req->origin, tmp_ptr, len);
    }

    // checkout for request content
    // and calculate the effective content length
    tmp_ptr = req;
    tmp_ptr = (char *)os_strstr(tmp_ptr, "\r\n\r\n");
    if (tmp_ptr == NULL)
    {
        esplog.error("http_parse_request - cannot find Content start\n");
        return;
    }
    tmp_ptr += 4;
    parsed_req->content_len = os_strlen(tmp_ptr);
    parsed_req->req_content = new char[parsed_req->content_len + 1];
    if (parsed_req->req_content == NULL)
    {
        esplog.error("http_parse_request - not enough heap memory\n");
        return;
    }
    os_memcpy(parsed_req->req_content, tmp_ptr, parsed_req->content_len);

    // checkout Content-Length
    parsed_req->h_content_len = parsed_req->content_len;
    tmp_ptr = req;
    tmp_ptr = (char *)os_strstr(tmp_ptr, "Content-Length: ");
    if (tmp_ptr == NULL)
    {
        tmp_ptr = req;
        tmp_ptr = (char *)os_strstr(tmp_ptr, "content-length: ");
        if (tmp_ptr == NULL)
        {
            esplog.trace("http_parse_request - didn't find any Content-Length\n");
        }
    }
    if (tmp_ptr != NULL)
    {
        tmp_ptr += 16;
        end_ptr = (char *)os_strstr(tmp_ptr, "\r\n");
        if (end_ptr == NULL)
        {
            esplog.error("http_parse_request - cannot find Content-Length value\n");
            return;
        }
        len = end_ptr - tmp_ptr;
        Heap_chunk tmp_str(len + 1);
        if (tmp_str.ref == NULL)
        {
            esplog.error("http_parse_request - not enough heap memory\n");
            return;
        }
        os_strncpy(tmp_str.ref, tmp_ptr, len);
        parsed_req->h_content_len = atoi(tmp_str.ref);
    }
}

class Http_pending_req
{
public:
    Http_pending_req();
    ~Http_pending_req();
    struct espconn *p_espconn;
    char *request;
    int content_len;
    int content_received;
};

Http_pending_req::Http_pending_req()
{
    esplog.all("Http_pending_req::Http_pending_req\n");
    p_espconn = NULL;
    request = NULL;
    content_len = 0;
    content_received = 0;
}

Http_pending_req::~Http_pending_req()
{
    esplog.all("Http_pending_req::~Http_pending_req\n");
    if (request)
        delete[] request;
}

static List<Http_pending_req> *pending_requests;

void http_save_pending_request(void *arg, char *precdata, unsigned short length, Http_parsed_req *parsed_req)
{
    esplog.all("http_save_pending_request\n");
    Http_pending_req *pending_req = new Http_pending_req;
    if (pending_req == NULL)
    {
        esplog.error("http_save_pending_request - not enough heap memory [%d]\n", sizeof(Http_pending_req));
        return;
    }
    // total expected message length
    int msg_len = length + (parsed_req->h_content_len - parsed_req->content_len);
    pending_req->request = new char[msg_len + 1];
    if (pending_req->request == NULL)
    {
        esplog.error("http_save_pending_request - not enough heap memory [%d]\n", msg_len);
        delete pending_req;
        return;
    }
    pending_req->p_espconn = (struct espconn *)arg;
    os_strncpy(pending_req->request, precdata, length);
    pending_req->content_len = parsed_req->h_content_len;
    pending_req->content_received = parsed_req->content_len;
    List_err err = pending_requests->push_back(pending_req);
    if (err != list_ok)
    {
        esplog.error("http_save_pending_request - cannot save pending request\n");
        delete pending_req;
        return;
    }
}

void http_check_pending_requests(struct espconn *p_espconn, char *new_msg, void (*msg_complete)(void *, char *, unsigned short))
{
    esplog.all("http_check_pending_requests\n");
    // look for a pending request on p_espconn
    Http_pending_req *p_p_req = pending_requests->front();
    while (p_p_req)
    {
        if (p_p_req->p_espconn == p_espconn)
            break;
        p_p_req = pending_requests->next();
    }
    if (p_p_req == NULL)
    {
        esplog.error("http_check_pending_requests - cannot find pending request on espconn %X\n", p_espconn);
        return;
    }
    // add the received message part
    char *str_ptr = p_p_req->request + os_strlen(p_p_req->request);
    os_strncpy(str_ptr, new_msg, os_strlen(new_msg));
    p_p_req->content_received += os_strlen(new_msg);
    // check if the message is completed
    if (p_p_req->content_len == p_p_req->content_received)
    {
        msg_complete((void *)p_espconn, p_p_req->request, os_strlen(p_p_req->request));
        pending_requests->remove();
    }
}

class Http_pending_res
{
public:
    Http_pending_res();
    ~Http_pending_res();
    struct espconn *p_espconn;
    char *response;
    int content_len;
    int content_received;
};

Http_pending_res::Http_pending_res()
{
    esplog.all("Http_pending_res::Http_pending_res\n");
    p_espconn = NULL;
    response = NULL;
    content_len = 0;
    content_received = 0;
}

Http_pending_res::~Http_pending_res()
{
    esplog.all("Http_pending_res::~Http_pending_res\n");
    if (response)
        delete[] response;
}

static List<Http_pending_res> *pending_responses;

// void print_pending_response(void)
// {
//     os_printf("---------> pending responses\n");
//     Http_pending_res *p_p_res = pending_responses->front();
//     int ii=0;
//     while (p_p_res)
//     {
//         os_printf("---------> response\n");
//         os_printf("                   p_espconn: %X\n", p_p_res->p_espconn);
//         os_printf("                    response: %s\n", p_p_res->response);
//         os_printf("                 content_len: %d\n", p_p_res->content_len);
//         os_printf("            content_received: %d\n", p_p_res->content_received);
//         p_p_res = pending_responses->next();
//         ii++;
//         if (ii>4)
//             break;
//     }
//     os_printf("---------> end\n");
// }

void http_save_pending_response(struct espconn *p_espconn, char *precdata, unsigned short length, Http_parsed_response *parsed_res)
{
    esplog.all("http_save_pending_response\n");
    Http_pending_res *pending_res = new Http_pending_res;
    if (pending_res == NULL)
    {
        esplog.error("http_save_pending_response - not enough heap memory [%d]\n", sizeof(Http_pending_res));
        return;
    }
    // total expected message length
    int msg_len = length + (parsed_res->h_content_len - parsed_res->content_len);
    pending_res->response = new char[msg_len + 1];
    if (pending_res->response == NULL)
    {
        esplog.error("http_save_pending_response - not enough heap memory [%d]\n", msg_len);
        delete pending_res;
        return;
    }
    pending_res->p_espconn = p_espconn;
    os_strncpy(pending_res->response, precdata, length);
    pending_res->content_len = parsed_res->h_content_len;
    pending_res->content_received = parsed_res->content_len;
    List_err err = pending_responses->push_back(pending_res);
    if (err != list_ok)
    {
        esplog.error("http_save_pending_response - cannot save pending response\n");
        delete pending_res;
        return;
    }
}

void http_check_pending_responses(struct espconn *p_espconn, char *new_msg, void (*msg_complete)(void *, char *, unsigned short))
{
    esplog.all("http_check_pending_responses\n");
    // look for a pending request on p_espconn
    Http_pending_res *p_p_res = pending_responses->front();
    while (p_p_res)
    {
        if (p_p_res->p_espconn == p_espconn)
            break;
        p_p_res = pending_responses->next();
    }
    if (p_p_res == NULL)
    {
        esplog.error("http_check_pending_responses - cannot find pending response on espconn %X\n", p_espconn);
        return;
    }
    // add the received message part
    char *str_ptr = p_p_res->response + os_strlen(p_p_res->response);
    os_strncpy(str_ptr, new_msg, os_strlen(new_msg));
    p_p_res->content_received += os_strlen(new_msg);
    // check if the message is completed
    if (p_p_res->content_len == p_p_res->content_received)
    {
        msg_complete((void *)p_espconn, p_p_res->response, os_strlen(p_p_res->response));
        pending_responses->remove();
    }
}

void http_init(void)
{
    esplog.all("http_init\n");

    http_msg_max_size = 256;

    pending_send = new Queue<struct http_send>(8);
    pending_split_send = new Queue<struct http_split_send>(4);
    pending_requests = new List<Http_pending_req>(4, delete_content);
    pending_responses = new List<Http_pending_res>(4, delete_content);
}

void http_queues_clear(void)
{
    esplog.all("http_queues_clear\n");
    struct http_send *p_send = pending_send->front();
    while (p_send)
    {
        delete[] p_send->msg;
        delete p_send;
        pending_send->pop();
        p_send = pending_send->front();
    }
    struct http_split_send *p_split = pending_split_send->front();
    while (p_split)
    {
        delete[] p_split->content;
        delete p_split;
        pending_split_send->pop();
        p_split = pending_split_send->front();
    }
}

//
// HTTP parsing response
//

Http_parsed_response::Http_parsed_response()
{
    esplog.all("Http_parsed_response::Http_parsed_response\n");

    no_header_message = true;
    content_range_start = 0;
    content_range_end = 0;
    content_range_size = 0;
    h_content_len = 0;
    content_len = 0;
    body = NULL;
}

Http_parsed_response::~Http_parsed_response()
{
    esplog.all("Http_parsed_response::~Http_parsed_response\n");
    if (body)
        delete[] body;
}

void http_parse_response(char *response, Http_parsed_response *parsed_response)
{
    esplog.all("http_parse_response\n");
    char *tmp_ptr = response;
    char *end_ptr = NULL;
    char *tmp_str = NULL;
    int len = 0;
    espmem.stack_mon();

    if (tmp_ptr == NULL)
    {
        esplog.error("http_parse_response - cannot parse empty message\n");
        return;
    }

    // looking for HTTP CODE
    tmp_ptr = response;
    tmp_ptr = (char *)os_strstr(tmp_ptr, "HTTP");
    if (tmp_ptr == NULL)
    {
        tmp_ptr = response;
        parsed_response->no_header_message = true;
    }
    else
    {
        parsed_response->no_header_message = false;
        tmp_ptr = (char *)os_strstr(tmp_ptr, " ");
        do
        {
            tmp_ptr++;
        } while (*tmp_ptr == ' ');
    }
    if (parsed_response->no_header_message)
    {
        parsed_response->content_len = os_strlen(tmp_ptr);
        parsed_response->body = new char[parsed_response->content_len + 1];
        if (parsed_response->body == NULL)
        {
            esplog.error("http_parse_response - not enough heap memory\n");
            return;
        }
        os_strncpy(parsed_response->body, tmp_ptr, parsed_response->content_len);
        return;
    }

    end_ptr = (char *)os_strstr(tmp_ptr, " ");
    if (end_ptr == NULL)
    {
        esplog.error("http_parse_response - cannot find HTTP code\n");
        return;
    }
    len = end_ptr - tmp_ptr;
    {
        Heap_chunk tmp_str(len + 1);
        if (tmp_str.ref == NULL)
        {
            esplog.error("http_parse_response - not enough heap memory\n");
            return;
        }
        os_strncpy(tmp_str.ref, tmp_ptr, len);
        parsed_response->http_code = atoi(tmp_str.ref);
    }

    // now the content-length
    tmp_ptr = response;
    tmp_ptr = (char *)os_strstr(tmp_ptr, "Content-Length: ");
    if (tmp_ptr == NULL)
    {
        esplog.trace("http_parse_response - didn't find any Content-Length\n");
    }
    else
    {
        tmp_ptr += 16;
        end_ptr = (char *)os_strstr(tmp_ptr, "\r\n");
        if (end_ptr == NULL)
        {
            esplog.error("http_parse_response - cannot find Content-Length value\n");
            return;
        }
        len = end_ptr - tmp_ptr;
        Heap_chunk tmp_str(len + 1);
        if (tmp_str.ref == NULL)
        {
            esplog.error("http_parse_response - not enough heap memory\n");
            return;
        }
        os_strncpy(tmp_str.ref, tmp_ptr, len);
        parsed_response->h_content_len = atoi(tmp_str.ref);
    }
    // now Content-Range (if any)
    tmp_ptr = response;
    tmp_ptr = (char *)os_strstr(tmp_ptr, "Content-Range: ");
    if (tmp_ptr == NULL)
    {
        esplog.trace("http_parse_response - didn't find any Content-Range\n");
    }
    else
    {
        tmp_ptr = (char *)os_strstr(tmp_ptr, "bytes");
        if (tmp_ptr == NULL)
        {
            esplog.error("http_parse_response - cannot find Content-Range value\n");
            return;
        }
        // range start
        tmp_ptr += os_strlen("bytes ");
        end_ptr = (char *)os_strstr(tmp_ptr, "-");
        if (end_ptr == NULL)
        {
            esplog.error("http_parse_response - cannot find range start\n");
            return;
        }
        len = end_ptr - tmp_ptr;
        {
            Heap_chunk tmp_str(len + 1);
            if (tmp_str.ref == NULL)
            {
                esplog.error("http_parse_response - not enough heap memory\n");
                return;
            }
            os_strncpy(tmp_str.ref, tmp_ptr, len);
            parsed_response->content_range_start = atoi(tmp_str.ref);
        }
        // range end
        tmp_ptr++;
        end_ptr = (char *)os_strstr(tmp_ptr, "/");
        if (end_ptr == NULL)
        {
            esplog.error("http_parse_response - cannot find range end\n");
            return;
        }
        len = end_ptr - tmp_ptr;
        {
            Heap_chunk tmp_str(len + 1);
            if (tmp_str.ref == NULL)
            {
                esplog.error("http_parse_response - not enough heap memory\n");
                return;
            }
            os_strncpy(tmp_str.ref, tmp_ptr, len);
            parsed_response->content_range_start = atoi(tmp_str.ref);
        }
        // range size
        tmp_ptr++;
        end_ptr = (char *)os_strstr(tmp_ptr, "\r\n");
        if (end_ptr == NULL)
        {
            esplog.error("http_parse_response - cannot find Content-Range size\n");
            return;
        }
        len = end_ptr - tmp_ptr;
        {
            Heap_chunk tmp_str(len + 1);
            if (tmp_str.ref == NULL)
            {
                esplog.error("http_parse_response - not enough heap memory\n");
                return;
            }
            os_strncpy(tmp_str.ref, tmp_ptr, len);
            parsed_response->content_range_size = atoi(tmp_str.ref);
        }
    }
    // finally the body
    tmp_ptr = response;
    tmp_ptr = (char *)os_strstr(tmp_ptr, "\r\n\r\n");
    if (tmp_ptr == NULL)
    {
        esplog.error("http_parse_response - cannot find Content start\n");
        return;
    }
    tmp_ptr += 4;
    len = os_strlen(tmp_ptr);
    parsed_response->content_len = len;
    parsed_response->body = new char[len + 1];
    if (parsed_response->body == NULL)
    {
        esplog.error("http_parse_response - not enough heap memory\n");
        return;
    }
    os_strncpy(parsed_response->body, tmp_ptr, len);
}