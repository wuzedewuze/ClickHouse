#include <DB/IO/ReadWriteBufferFromHTTP.h>

#include <Poco/URI.h>
#include <Poco/Net/DNS.h>
#include <Poco/Net/HTTPResponse.h>
#include <Poco/Net/HTTPRequest.h>

#include <DB/IO/ReadBufferFromIStream.h>

#include <DB/Common/SimpleCache.h>

#include <common/logger_useful.h>


namespace DB
{

// copypaste from ReadBufferFromHTTP.cpp
namespace ErrorCodes
{
	extern const int RECEIVED_ERROR_FROM_REMOTE_IO_SERVER;
}

static Poco::Net::IPAddress resolveHostImpl(const String & host)
{
	return Poco::Net::DNS::resolveOne(host);
}

static Poco::Net::IPAddress resolveHost(const String & host)
{
	static SimpleCache<decltype(resolveHostImpl), &resolveHostImpl> cache;
	return cache(host);
}
// ==========


ReadWriteBufferFromHTTP::ReadWriteBufferFromHTTP(
		const Poco::URI & uri,
		const std::string & method,
		const std::string & post_body,
		size_t buffer_size_,
		const HTTPTimeouts & timeouts
	) :
	ReadBuffer(nullptr, 0),
	uri{uri},
	method{!method.empty() ? method : post_body.empty() ? Poco::Net::HTTPRequest::HTTP_GET : Poco::Net::HTTPRequest::HTTP_POST},
	timeouts{timeouts}
{
	session.setHost(resolveHost(uri.getHost()).toString());	/// Cache DNS forever (until server restart)
	session.setPort(uri.getPort());

	session.setTimeout(timeouts.connection_timeout, timeouts.send_timeout, timeouts.receive_timeout);

	Poco::Net::HTTPRequest request(method, uri.getPathAndQuery());
	Poco::Net::HTTPResponse response;

	LOG_TRACE((&Logger::get("ReadBufferFromHTTP")), "Sending request to " << uri.toString());

	if (!post_body.empty() || method == Poco::Net::HTTPRequest::HTTP_POST)
		request.setContentLength(post_body.size());

	auto & stream_out = session.sendRequest(request);

	if (!post_body.empty())
		stream_out << post_body;

	istr = &session.receiveResponse(response);

	auto status = response.getStatus();

	if (status != Poco::Net::HTTPResponse::HTTP_OK)
	{
		std::stringstream error_message;
		error_message << "Received error from remote server " << uri.toString() << ". HTTP status code: "
			<< status << ", body: " << istr->rdbuf();

		throw Exception(error_message.str(), ErrorCodes::RECEIVED_ERROR_FROM_REMOTE_IO_SERVER);
	}

	impl = std::make_unique<ReadBufferFromIStream>(*istr, buffer_size_);
}


bool ReadWriteBufferFromHTTP::nextImpl()
{
	if (!impl->next())
		return false;
	internal_buffer = impl->buffer();
	working_buffer = internal_buffer;
	return true;
}

}
