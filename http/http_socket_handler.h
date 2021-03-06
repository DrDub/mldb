/* http_socket_handler.h - This file is part of MLDB               -*- C++ -*-
   Wolfgang Sourdeau, September 2015
   Copyright (c) 2015 Datacratic.  All rights reserved.

   TCP handler classes for an HTTP service
*/

#pragma once

#include "mldb/ext/jsoncpp/value.h"
#include "mldb/jml/utils/string_functions.h"
#include "mldb/http/http_header.h"
#include "mldb/http/http_parsers.h"
#include "mldb/http/tcp_socket_handler.h"


namespace Datacratic {

/****************************************************************************/
/* HTTP CONNECTION HANDLER                                                  */
/****************************************************************************/

/* A base class for handling HTTP connections. */

struct HttpSocketHandler : public TcpSocketHandler {
    HttpSocketHandler(TcpSocket socket);

    /* Callback used when to report the request line. */
    virtual void onRequestStart(const char * methodData, size_t methodSize,
                                const char * urlData, size_t urlSize,
                                const char * versionData,
                                size_t versionSize) = 0;

    /* Callback used to report a header-line, including the header key and the
     * value. */
    virtual void onHeader(const char * data, size_t dataSize) = 0;

    /* Callback used to report a chunk of the response body. Only invoked
       when the body is larger than 0 byte. */
    virtual void onData(const char * data, size_t dataSize) = 0;

    /* Callback used to report the end of a response. */
    virtual void onDone(bool requireClose) = 0;

private:
    /* TcpSocketHandler interface */
    virtual void bootstrap();
    virtual void onReceivedData(const char * buffer, size_t bufferSize);
    virtual void onReceiveError(const boost::system::error_code & ec,
                                size_t bufferSize);

    HttpRequestParser parser_;
};


/*****************************************************************************/
/* HTTP RESPONSE                                                             */
/*****************************************************************************/

/** Structure used to return an HTTP response.
    TODO: make use of the the HttpHeader class
*/

struct HttpResponse {
    HttpResponse(int responseCode,
                 std::string contentType,
                 std::string body,
                 std::vector<std::pair<std::string, std::string> > extraHeaders
                     = std::vector<std::pair<std::string, std::string> >())
        : responseCode(responseCode),
          responseStatus(getResponseReasonPhrase(responseCode)),
          contentType(std::move(contentType)),
          body(std::move(body)),
          extraHeaders(std::move(extraHeaders)),
          sendBody(true)
    {
    }

    /** Construct an HTTP response header only, with no body.  No content-
        length will be inferred. */

    HttpResponse(int responseCode,
                 std::string contentType,
                 std::vector<std::pair<std::string, std::string> > extraHeaders
                     = std::vector<std::pair<std::string, std::string> >())
        : responseCode(responseCode),
          responseStatus(getResponseReasonPhrase(responseCode)),
          contentType(std::move(contentType)),
          extraHeaders(std::move(extraHeaders)),
          sendBody(false)
    {
    }

    HttpResponse(int responseCode,
                 Json::Value body,
                 std::vector<std::pair<std::string, std::string> > extraHeaders
                     = std::vector<std::pair<std::string, std::string> >())
        : responseCode(responseCode),
          responseStatus(getResponseReasonPhrase(responseCode)),
          contentType("application/json"),
          body(ML::trim(body.toString())),
          extraHeaders(std::move(extraHeaders)),
          sendBody(true)
    {
    }

    int responseCode;
    std::string responseStatus;
    std::string contentType;
    std::string body;
    std::vector<std::pair<std::string, std::string> > extraHeaders;
    bool sendBody;
};


/****************************************************************************/
/* HTTP LEGACY CONNECTION HANDLER                                           */
/****************************************************************************/

/* A drop-in replacement class for PassiveSocketHandler. So that old
 * handler code can easily plugged into the recent versions of the service
 * classes. */

struct HttpLegacySocketHandler : public HttpSocketHandler {
    /** Action to perform once we've finished sending. */
    enum NextAction {
        NEXT_CLOSE,
        NEXT_RECYCLE,
        NEXT_CONTINUE
    };

    /* Type of function called when a write operation has finished. */
    typedef std::function<void ()> OnWriteFinished;

    HttpLegacySocketHandler(TcpSocket && socket);

    virtual void handleHttpPayload(const HttpHeader & header,
                                   const std::string & payload) = 0;

    void putResponseOnWire(const HttpResponse & response,
                           std::function<void ()> onSendFinished
                           = std::function<void ()>(),
                           NextAction next = NEXT_CONTINUE);
    void send(std::string str,
              NextAction action = NEXT_CONTINUE,
              OnWriteFinished onWriteFinished = nullptr);

private:
    virtual void onRequestStart(const char * methodData, size_t methodSize,
                                const char * urlData, size_t urlSize,
                                const char * versionData,
                                size_t versionSize);
    virtual void onHeader(const char * data, size_t dataSize);
    virtual void onData(const char * data, size_t dataSize);
    virtual void onDone(bool requireClose);

    std::string headerPayload;
    std::string bodyPayload;
    bool bodyStarted_;

    std::string writeData_;
};

} // namespace Datacratic
