#include "controller.h"
#include "timer.h"

#include <regex>

Controller::Controller()
{
}

Controller::~Controller()
{
}

void Controller::handle_request(struct evhttp_request* req)
{
  // Do our own path parsing to check the request is to a known path:
  //
  // /timers
  // /timers/
  // /timers/<timerid>
  const char *uri = evhttp_request_get_uri(req);  
  struct evhttp_uri* decoded = evhttp_uri_parse(uri);
  if (!decoded)
  {
    send_error(req, HTTP_BADREQUEST, "Requested URI is unparseable");
    return;
  }

  const char* encoded_path = evhttp_uri_get_path(decoded);
  if (!encoded_path)
  {
    encoded_path = "/";
  }

  size_t path_len;
  const char* path_str = evhttp_uridecode(encoded_path, 0, &path_len);
  if (!path_str)
  {
    send_error(req, HTTP_BADREQUEST, "Requested path is unparseable");
    return;
  }

  std::string path(path_str, path_len);

  // Also need to check the user has supplied a valid method:
  //
  //  * POST to the collection
  //  * PUT to a specific ID
  evhttp_cmd_type method = evhttp_request_get_command(req);
  
  std::smatch matches;
  TimerID timer_id;
  if ((path == "/timers") || (path == "/timers/"))
  {
    if (method != EVHTTP_REQ_POST)
    {
      send_error(req, HTTP_BADMETHOD, NULL);
      return;
    }
    timer_id = Timer::generate_timer_id();
  }
  else if (std::regex_match(path, matches, std::regex("/timers/([1-9][0-9]*)")))
  {
    if (method != EVHTTP_REQ_PUT)
    {
      send_error(req, HTTP_BADMETHOD, NULL);
      return;
    }
    timer_id = std::stoul(matches[1]);
  }
  else
  {
    send_error(req, HTTP_NOTFOUND, NULL);
    return;
  }

  // At this point, the ReqURI has been parsed and validated and we've got the
  // ID for the timer worked out.  Now, validate the timer body.
  std::string body = get_req_body(req);
  std::string error_str;
  Timer* timer = Timer::from_json(timer_id, body, error_str);
  if (!timer)
  {
    send_error(req, HTTP_BADREQUEST, error_str.c_str());
    return;
  }

  // Now we have a valid timer object, maybe determine replicas if they weren't
  // specified in the request.
  evhttp_add_header(evhttp_request_get_output_headers(req),
                    "Location", timer->url("localhost").c_str());
  evhttp_send_reply(req, 200, "OK", NULL);

  printf("%s\n", timer->to_json().c_str());
  delete timer;
}

void Controller::controller_cb(struct evhttp_request* req, void* controller)
{
  ((Controller*)controller)->handle_request(req);
}

void Controller::controller_ping_cb(struct evhttp_request* req, void* controller)
{
  evhttp_send_reply(req, 200, "OK", NULL);
}

/*****************************************************************************/
/* PRIVATE FUNCTIONS                                                         */
/*****************************************************************************/

void Controller::send_error(struct evhttp_request* req, int error, const char* reason)
{
  // LOG_ERROR("Rejecting request with %d %s", error, reason);
  evhttp_send_error(req, error, reason);
}

std::string Controller::get_req_body(struct evhttp_request* req)
{
  struct evbuffer* evbuf;
  std::string rc;
  evbuf = evhttp_request_get_input_buffer(req);
  while (evbuffer_get_length(evbuf))
  {
    int nbytes;
    char* buf[1024];
    nbytes = evbuffer_remove(evbuf, buf, sizeof(buf));
    if (nbytes > 0)
    {
      rc.append((const char*)buf, (size_t)nbytes);
    }
  }
  return rc;
}
