/** http_client_impl_v1.cc
    Wolfgang Sourdeau, January 2014
    This file is part of MLDB. Copyright 2014 mldb.ai inc. All rights reserved.

    V1 of the HTTP client, based on libcurl.
*/

#include <errno.h>
#include <poll.h>
#include <string.h>
#include "mldb/io/epoller.h"
#include "mldb/io/timerfd.h"
#include "mldb/arch/wakeup_fd.h"

#include "mldb/arch/exception.h"
#include "mldb/base/scope.h"
#include "mldb/utils/string_functions.h"

#include "mldb/http/http_header.h"
#include "mldb/http/http_client_callbacks.h"
#include "mldb/http/http_client_impl_v1.h"

using namespace std;
using namespace MLDB;


namespace {

HttpClientError
translateError(CURLcode curlError)
{
    HttpClientError error;

    switch (curlError) {
    case CURLE_OK:
        error = HttpClientError::None;
        break;
    case CURLE_OPERATION_TIMEDOUT:
        error = HttpClientError::Timeout;
        break;
    case CURLE_COULDNT_RESOLVE_HOST:
        error = HttpClientError::HostNotFound;
        break;
    case CURLE_COULDNT_CONNECT:
        error = HttpClientError::CouldNotConnect;
        break;
    case CURLE_SEND_ERROR:
        error = HttpClientError::SendError;
        break;
    case CURLE_RECV_ERROR:
        error = HttpClientError::RecvError;
        break;
    default:
        cerr << "returning 'unknown' for code " + to_string(curlError) + "\n";
        error = HttpClientError::Unknown;
    }

    return error;
}

} // file scope


/****************************************************************************/
/* HTTP CLIENT IMPL V1                                                      */
/****************************************************************************/

HttpClientImplV1::
HttpClientImplV1(const string & baseUrl, int numParallel, int queueSize)
    : HttpClientImpl(baseUrl, numParallel, queueSize),
      baseUrl_(baseUrl),
      multi_(curl_multi_init()),
      connectionStash_(numParallel),
      avlConnections_(numParallel),
      nextAvail_(0)
{
    if (queueSize > 0) {
        throw MLDB::Exception("'queueSize' semantics not implemented");
    }

    poller_.reset(new Epoller());
    poller_->handleEvent = [this] (const EpollEvent & event) { this->handleEvent(event); return Epoller::DONE; };
    poller_->init(512 /* size doesn't matter since linux 2.6.8 but may for other OSs */, 0 /* timeout */, true /* close_on_exec */);

    wakeup_.reset(new WakeupFD(WFD_NONBLOCK, WFD_CLOEXEC));
    poller_->addFd(wakeup_->fd(), EPOLL_INPUT);

    timerFd_.reset(new TimerFD(TIMER_MONOTONIC, TIMER_CLOSE_ON_EXEC));
    poller_->addFd(timerFd_->fd(), EPOLL_INPUT);

    /* multi */
    ::curl_multi_setopt(multi_.get(), CURLMOPT_SOCKETFUNCTION,
                        socketCallback);
    ::curl_multi_setopt(multi_.get(), CURLMOPT_SOCKETDATA, this);
    ::curl_multi_setopt(multi_.get(), CURLMOPT_TIMERFUNCTION,
                        timerCallback);
    ::curl_multi_setopt(multi_.get(), CURLMOPT_TIMERDATA, this);

    /* available connections */
    for (size_t i = 0; i < connectionStash_.size(); i++) {
        avlConnections_[i] = &connectionStash_[i];
    }

    /* kick start multi */
    int runningHandles;
    CURLMcode rc = ::curl_multi_socket_action(multi_.get(),
                                              CURL_SOCKET_TIMEOUT, 0,
                                              &runningHandles);
    if (rc != ::CURLM_OK) {
        throw MLDB::Exception("curl error " + to_string(rc));
    }
}

void
HttpClientImplV1::
CurlMultiCleanup::
operator () (::CURLM * c)
{
    curl_multi_cleanup(c);
}

HttpClientImplV1::
~HttpClientImplV1()
{
}

void
HttpClientImplV1::
enableDebug(bool value)
{
    debug(value);
}

void
HttpClientImplV1::
enableSSLChecks(bool value)
{
    noSSLChecks_ = !value;
}

void
HttpClientImplV1::
enableTcpNoDelay(bool value)
{
    tcpNoDelay_ = value;
}

void
HttpClientImplV1::
enablePipelining(bool value)
{
    ::curl_multi_setopt(multi_.get(), CURLMOPT_PIPELINING, value ? 1 : 0);
}

void
HttpClientImplV1::
addFd(int fd, bool isMod, bool input, bool output)
    const
{
    if (isMod)
      poller_->modifyFd(fd, (input ? EPOLL_INPUT : 0) | (output ? EPOLL_OUTPUT : 0), nullptr);
    else
      poller_->addFd(fd, (input ? EPOLL_INPUT : 0) | (output ? EPOLL_OUTPUT : 0), nullptr);
}

void
HttpClientImplV1::
removeFd(int fd)
    const
{
    poller_->removeFd(fd);
}

bool
HttpClientImplV1::
enqueueRequest(const string & verb, const string & resource,
               const shared_ptr<HttpClientCallbacks> & callbacks,
               const HttpRequestContent & content,
               const RestParams & queryParams, const RestParams & headers,
               int timeout)
{
    string url = baseUrl_ + resource + queryParams.uriEscaped();
    {
        Guard guard(queueLock_);
        queue_.emplace(std::make_shared<HttpRequest>(verb, url, callbacks, content, headers, timeout));
    }

    // Wakeup the message loop so it sees that there is something new to do
    wakeup_->signal();

    return true;
}

std::vector<std::shared_ptr<HttpRequest>>
HttpClientImplV1::
popRequests(size_t number)
{
    Guard guard(queueLock_);
    std::vector<std::shared_ptr<HttpRequest>> requests;
    number = min(number, queue_.size());
    requests.reserve(number);

    for (size_t i = 0; i < number; i++) {
        requests.emplace_back(move(queue_.front()));
        queue_.pop();
    }

    return requests;
}

size_t
HttpClientImplV1::
queuedRequests()
    const
{
    Guard guard(queueLock_);
    return queue_.size();
}

int
HttpClientImplV1::
selectFd()
    const
{
    return poller_->selectFd();
}

bool
HttpClientImplV1::
processOne()
{
    return poller_->processOne();
}

void
HttpClientImplV1::
handleEvent(const EpollEvent & event)
{
    if (getFd(event) == wakeup_->fd()) {
        handleWakeupEvent();
    }
    else if (getFd(event) == timerFd_->fd()) {
        handleTimerEvent();
    }
    else {
        handleMultiEvent(event);
    }
}

void
HttpClientImplV1::
handleWakeupEvent()
{
    /* Deduplication of wakeup events */
    while (wakeup_->tryRead());

    size_t numAvail = avlConnections_.size() - nextAvail_;
    if (numAvail > 0) {
        vector<shared_ptr<HttpRequest>> requests = popRequests(numAvail);
        for (auto & request: requests) {
            HttpConnection *conn = getConnection();
            conn->request_ = move(request);
            conn->perform(noSSLChecks_, tcpNoDelay_, debug_);

            CURLMcode code = ::curl_multi_add_handle(multi_.get(),
                                                     conn->easy_);
            if (code != CURLM_CALL_MULTI_PERFORM && code != CURLM_OK) {
                throw MLDB::Exception("failing to add handle to multi");
            }
        }
    }
}

void
HttpClientImplV1::
handleTimerEvent()
{
    // Clear the timer fd by reading the number of missed timers
    timerFd_->read();
    int runningHandles;
    CURLMcode rc = ::curl_multi_socket_action(multi_.get(),
                                              CURL_SOCKET_TIMEOUT, 0,
                                              &runningHandles);
    if (rc != ::CURLM_OK) {
        throw MLDB::Exception("curl error " + to_string(rc));
    }
    checkMultiInfos();
}

void
HttpClientImplV1::
handleMultiEvent(const EpollEvent & event)
{
    int actionFlags(0);
    if (hasInput(event)) {
        actionFlags |= CURL_CSELECT_IN;
    }
    if (hasOutput(event)) {
        actionFlags |= CURL_CSELECT_OUT;
    }
    
    int runningHandles;
    CURLMcode rc = ::curl_multi_socket_action(multi_.get(), getFd(event),
                                              actionFlags,
                                              &runningHandles);
    if (rc != ::CURLM_OK) {
        throw MLDB::Exception("curl error " + to_string(rc));
    }

    checkMultiInfos();
}

void
HttpClientImplV1::
checkMultiInfos()
{
    int remainingMsgs(0);
    CURLMsg * msg;
    while ((msg = ::curl_multi_info_read(multi_.get(), &remainingMsgs))) {
        if (msg->msg == CURLMSG_DONE) {
            HttpConnection * conn(nullptr);
            ::curl_easy_getinfo(msg->easy_handle,
                                CURLINFO_PRIVATE, &conn);

            const auto & cbs = conn->request_->callbacks();
            cbs->onDone(*conn->request_, translateError(msg->data.result));
            conn->clear();

            CURLMcode code = ::curl_multi_remove_handle(multi_.get(),
                                                        conn->easy_);
            if (code != CURLM_CALL_MULTI_PERFORM && code != CURLM_OK) {
                throw MLDB::Exception("failed to remove handle to multi");
            }
            releaseConnection(conn);
            wakeup_->signal();
        }
    }
}

int
HttpClientImplV1::
socketCallback(CURL *e, curl_socket_t s, int what, void *clientP, void *sockp)
{
    HttpClientImplV1 * this_ = static_cast<HttpClientImplV1 *>(clientP);

    return this_->onCurlSocketEvent(e, s, what, sockp);
}

int
HttpClientImplV1::
onCurlSocketEvent(CURL *e, curl_socket_t fd, int what, void *sockp)
{
    // cerr << "onCurlSocketEvent: " + to_string(fd) + " what: " + to_string(what) + "\n";

    if (what == CURL_POLL_REMOVE) {
        removeFd(fd);
    }
    else if (what != CURL_POLL_NONE) {
        bool hasInput = false, hasOutput = false;
        if ((what & CURL_POLL_IN)) {
            hasInput = true;
        }
        if ((what & CURL_POLL_OUT)) {
            hasOutput = true;
        }
        addFd(fd, (sockp != nullptr), hasInput, hasOutput);
        if (sockp == nullptr) {
            CURLMcode rc = ::curl_multi_assign(multi_.get(), fd, this);
            if (rc != ::CURLM_OK) {
                throw MLDB::Exception("curl error " + to_string(rc));
            }
        }
    }

    return 0;
}

int
HttpClientImplV1::
timerCallback(CURLM *multi, long timeoutMs, void *clientP)
{
    HttpClientImplV1 * this_ = static_cast<HttpClientImplV1 *>(clientP);

    return this_->onCurlTimerEvent(timeoutMs);
}

int
HttpClientImplV1::
onCurlTimerEvent(long timeoutMs)
{
    // cerr << "onCurlTimerEvent: timeout = " + to_string(timeoutMs) + "\n";

    if (timeoutMs < -1) {
        throw MLDB::Exception("unhandled timeout value: %ld", timeoutMs);
    }

    timerFd_->setTimeout(std::chrono::milliseconds(timeoutMs));

    if (timeoutMs == 0) {
        int runningHandles;
        CURLMcode rc = ::curl_multi_socket_action(multi_.get(),
                                                  CURL_SOCKET_TIMEOUT, 0,
                                                  &runningHandles);
        if (rc != ::CURLM_OK) {
            throw MLDB::Exception("curl error " + to_string(rc));
        }
        checkMultiInfos();
    }

    return 0;
}

HttpClientImplV1::
HttpConnection *
HttpClientImplV1::
getConnection()
{
    HttpConnection * conn;

    if (nextAvail_ < avlConnections_.size()) {
        conn = avlConnections_[nextAvail_];
        nextAvail_++;
    }
    else {
        conn = nullptr;
    }

    return conn;
}

void
HttpClientImplV1::
releaseConnection(HttpConnection * oldConnection)
{
    if (nextAvail_ > 0) {
        nextAvail_--;
        avlConnections_[nextAvail_] = oldConnection;
    }
}


/* HTTPCLIENT::HTTPCONNECTION */

HttpClientImplV1::
HttpConnection::
HttpConnection()
    : onHeader_([&] (const char * data, size_t ofs1, size_t ofs2) {
          return this->onCurlHeader(data, ofs1 * ofs2);
      }),
      onWrite_([&] (const char * data, size_t ofs1, size_t ofs2) {
          return this->onCurlWrite(data, ofs1 * ofs2);
      }),
      onRead_([&] (char * data, size_t ofs1, size_t ofs2) {
          return this->onCurlRead(data, ofs1 * ofs2);
      }),
      afterContinue_(false), uploadOffset_(0)
{
}

void
HttpClientImplV1::
HttpConnection::
perform(bool noSSLChecks, bool tcpNoDelay, bool debug)
{
    // cerr << "* performRequest\n";

    afterContinue_ = false;

    easy_.add_option(CURLOPT_URL, request_->url());

    RestParams headers = request_->headers();

    const string & verb = request_->verb();
    if (verb != "GET") {
        const auto & content = request_->content();
        const string & body = content.body();
        if (verb == "PUT") {
            easy_.add_option(CURLOPT_UPLOAD, true);
            easy_.add_option(CURLOPT_INFILESIZE, body.size());
        }
        else if (verb == "POST") {
            easy_.add_option(CURLOPT_POST, true);
            easy_.add_option(CURLOPT_POSTFIELDS, body);
            easy_.add_option(CURLOPT_POSTFIELDSIZE, body.size());
        }
        else if (verb == "HEAD") {
            easy_.add_option(CURLOPT_NOBODY, true);
        }
        headers.emplace_back(make_pair("Content-Length",
                                       to_string(body.size())));
        headers.emplace_back(make_pair("Transfer-Encoding", ""));
        headers.emplace_back(make_pair("Content-Type",
                                       content.contentType()));

        /* Disable "Expect: 100 Continue" header that curl sets automatically
           for uploads larger than 1 Kbyte */
        headers.emplace_back(make_pair("Expect", ""));
    }
    easy_.add_header_option(headers);

    easy_.add_option(CURLOPT_CUSTOMREQUEST, verb);
    easy_.add_data_option(CURLOPT_PRIVATE, this);

    easy_.add_callback_option(CURLOPT_HEADERFUNCTION, CURLOPT_HEADERDATA, onHeader_);
    easy_.add_callback_option(CURLOPT_WRITEFUNCTION, CURLOPT_WRITEDATA, onWrite_);
    easy_.add_callback_option(CURLOPT_READFUNCTION, CURLOPT_READDATA,  onRead_);
    
    easy_.add_option(CURLOPT_BUFFERSIZE, 65536);

    if (request_->timeout() != -1) {
        easy_.add_option(CURLOPT_TIMEOUT, request_->timeout());
    }
    easy_.add_option(CURLOPT_NOSIGNAL, true);
    easy_.add_option(CURLOPT_NOPROGRESS, true);
    if (noSSLChecks) {
        easy_.add_option(CURLOPT_SSL_VERIFYHOST, false);
        easy_.add_option(CURLOPT_SSL_VERIFYPEER, false);
    }
    if (debug) {
        easy_.add_option(CURLOPT_VERBOSE, 1L);
    }
    if (tcpNoDelay) {
        easy_.add_option(CURLOPT_TCP_NODELAY, true);
    }
}

size_t
HttpClientImplV1::
HttpConnection::
onCurlHeader(const char * data, size_t size)
{
    string headerLine(data, size);
    if (headerLine.find("HTTP/1.1 100") == 0) {
        afterContinue_ = true;
    }
    else if (afterContinue_) {
        if (headerLine == "\r\n")
            afterContinue_ = false;
    }
    else {
        const auto & cbs = request_->callbacks();
        if (headerLine.find("HTTP/") == 0) {
            size_t lineSize = headerLine.size();
            size_t oldTokenIdx(0);
            size_t tokenIdx = headerLine.find(" ");
            if (tokenIdx == string::npos || tokenIdx >= lineSize) {
                throw MLDB::Exception("malformed header");
            }
            string version = headerLine.substr(oldTokenIdx, tokenIdx);

            oldTokenIdx = tokenIdx + 1;
            tokenIdx = headerLine.find(" ", oldTokenIdx);
            if (tokenIdx == string::npos || tokenIdx >= lineSize) {
                throw MLDB::Exception("malformed header");
            }
            int code = stoi(headerLine.substr(oldTokenIdx, tokenIdx));

            cbs->onResponseStart(*request_, move(version), code);
        }
        else {
            cbs->onHeader(*request_, data, size);
        }
    }

    return size;
}

size_t
HttpClientImplV1::
HttpConnection::
onCurlWrite(const char * data, size_t size)
{
    request_->callbacks()->onData(*request_, data, size);
    return size;
}

size_t
HttpClientImplV1::
HttpConnection::
onCurlRead(char * buffer, size_t bufferSize)
{
    const string & body = request_->content().body();
    size_t chunkSize = body.size() - uploadOffset_;
    if (chunkSize > bufferSize) {
        chunkSize = bufferSize;
    }
    const char * chunkStart = body.c_str() + uploadOffset_;
    copy(chunkStart, chunkStart + chunkSize, buffer);
    uploadOffset_ += chunkSize;

    return chunkSize;
}
