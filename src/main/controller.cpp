#include "controller.h"
#include "timer.h"

#include "murmur/MurmurHash3.h"

#include <regex>
#include <boost/tokenizer.hpp>

Controller::Controller(Replicator* replicator,
                       TimerHandler* handler) :
                       _replicator(replicator),
                       _handler(handler)
{
  _cluster.push_back("localhost");
  _cluster.push_back("localhost");
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
  std::vector<std::string> replicas;
  if ((path == "/timers") || (path == "/timers/"))
  {
    if (method != EVHTTP_REQ_POST)
    {
      send_error(req, HTTP_BADMETHOD, NULL);
      return;
    }
    timer_id = Timer::generate_timer_id();
    // Leave replica list empty until we know the replication-factor.
  }
  else if (std::regex_match(path, matches, std::regex("/timers/([1-9][0-9]*)-(.*)")))
  {
    if ((method != EVHTTP_REQ_PUT) && (method != EVHTTP_REQ_DELETE))
    {
      send_error(req, HTTP_BADMETHOD, NULL);
      return;
    }
    timer_id = std::stoul(matches[1]);
    boost::char_separator<char> sep("-");
    boost::tokenizer<boost::char_separator<char>> tokens(std::string(matches[1]), sep);
    for (const auto& replica : tokens)
    {
      replicas.push_back(std::string(replica));
    }
  }
  else
  {
    send_error(req, HTTP_NOTFOUND, NULL);
    return;
  }

  // At this point, the ReqURI has been parsed and validated and we've got the
  // ID for the timer worked out.  Now, create the timer object from the body,
  // for a DELETE request, we'll create a tombstone record instead.
  Timer* timer = NULL;
  if (method == EVHTTP_REQ_DELETE)
  {
    timer = Timer::create_tombstone(timer_id);
  }
  else
  {
    std::string body = get_req_body(req);
    std::string error_str;
    timer = Timer::from_json(timer_id, replicas, body, error_str);
    if (!timer)
    {
      send_error(req, HTTP_BADREQUEST, error_str.c_str());
      return;
    }
  }

  // If the timer has no replicas set up yet, calculate them now.
  if (timer->replicas.empty())
  {
    calculate_replicas(timer);
  }

  // Now we have a valid timer object, reply to the HTTP request.
  evhttp_add_header(evhttp_request_get_output_headers(req),
                    "Location", timer->url("localhost").c_str());
  evhttp_send_reply(req, 200, "OK", NULL);

  // Replicate the timer to the other replicas if this is a client request
  // _replicator->replicate(timer);

  // If the timer belongs to the local node, store it.
  // TODO Use real local address.
  if (timer->is_local("localhost"))
  {
    _handler->add_timer(timer);
    timer = NULL;
  }
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

void Controller::calculate_replicas(Timer* timer)
{
  uint32_t hash;
  MurmurHash3_x86_32(&timer->id, sizeof(TimerID), 0x0, &hash);
  unsigned int primary_replica_idx = hash % _cluster.size();
  for (unsigned int ii = 0; ii < timer->replication_factor && ii < _cluster.size(); ii++)
  {
    timer->replicas.push_back(_cluster[primary_replica_idx + ii]);
  }
}
