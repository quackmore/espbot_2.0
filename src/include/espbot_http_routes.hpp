/*
 * ----------------------------------------------------------------------------
 * "THE BEER-WARE LICENSE" (Revision 42):
 * <quackmore-ff@yahoo.com> wrote this file.  As long as you retain this notice
 * you can do whatever you want with this stuff. If we meet some day, and you 
 * think this stuff is worth it, you can buy me a beer in return. Quackmore
 * ----------------------------------------------------------------------------
 */

#ifndef __ESPBOT_HTTP_ROUTES_HPP__
#define __ESPBOT_HTTP_ROUTES_HPP__

#include "webserver.hpp"

void espbot_http_routes(struct espconn *ptr_espconn, Html_parsed_req *parsed_req);

#endif