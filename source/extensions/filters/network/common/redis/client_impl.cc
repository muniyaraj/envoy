#include "extensions/filters/network/common/redis/client_impl.h"

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace Common {
namespace Redis {
namespace Client {

ConfigImpl::ConfigImpl(
    const envoy::config::filter::network::redis_proxy::v2::RedisProxy::ConnPoolSettings& config)
    : op_timeout_(PROTOBUF_GET_MS_REQUIRED(config, op_timeout)),
      enable_hashtagging_(config.enable_hashtagging()) {}

ClientPtr ClientImpl::create(Upstream::HostConstSharedPtr host, Event::Dispatcher& dispatcher,
                             EncoderPtr&& encoder, DecoderFactory& decoder_factory,
                             const Config& config) {

  std::unique_ptr<ClientImpl> client(
      new ClientImpl(host, dispatcher, std::move(encoder), decoder_factory, config));
  client->connection_ = host->createConnection(dispatcher, nullptr, nullptr).connection_;
  client->connection_->addConnectionCallbacks(*client);
  client->connection_->addReadFilter(Network::ReadFilterSharedPtr{new UpstreamReadFilter(*client)});
  client->connection_->connect();
  client->connection_->noDelay(true);
  return std::move(client);
}

ClientImpl::ClientImpl(Upstream::HostConstSharedPtr host, Event::Dispatcher& dispatcher,
                       EncoderPtr&& encoder, DecoderFactory& decoder_factory, const Config& config)
    : host_(host), encoder_(std::move(encoder)), decoder_(decoder_factory.create(*this)),
      config_(config),
      connect_or_op_timer_(dispatcher.createTimer([this]() -> void { onConnectOrOpTimeout(); })) {
  host->cluster().stats().upstream_cx_total_.inc();
  host->stats().cx_total_.inc();
  host->cluster().stats().upstream_cx_active_.inc();
  host->stats().cx_active_.inc();
  connect_or_op_timer_->enableTimer(host->cluster().connectTimeout());
}

ClientImpl::~ClientImpl() {
  ASSERT(pending_requests_.empty());
  ASSERT(connection_->state() == Network::Connection::State::Closed);
  host_->cluster().stats().upstream_cx_active_.dec();
  host_->stats().cx_active_.dec();
}

void ClientImpl::close() { connection_->close(Network::ConnectionCloseType::NoFlush); }

PoolRequest* ClientImpl::makeRequest(const RespValue& request, PoolCallbacks& callbacks) {
  ASSERT(connection_->state() == Network::Connection::State::Open);

  pending_requests_.emplace_back(*this, callbacks);
  encoder_->encode(request, encoder_buffer_);
  connection_->write(encoder_buffer_, false);

  // Only boost the op timeout if:
  // - We are not already connected. Otherwise, we are governed by the connect timeout and the timer
  //   will be reset when/if connection occurs. This allows a relatively long connection spin up
  //   time for example if TLS is being used.
  // - This is the first request on the pipeline. Otherwise the timeout would effectively start on
  //   the last operation.
  if (connected_ && pending_requests_.size() == 1) {
    connect_or_op_timer_->enableTimer(config_.opTimeout());
  }

  return &pending_requests_.back();
}

void ClientImpl::onConnectOrOpTimeout() {
  putOutlierEvent(Upstream::Outlier::Result::TIMEOUT);
  if (connected_) {
    host_->cluster().stats().upstream_rq_timeout_.inc();
    host_->stats().rq_timeout_.inc();
  } else {
    host_->cluster().stats().upstream_cx_connect_timeout_.inc();
    host_->stats().cx_connect_fail_.inc();
  }

  connection_->close(Network::ConnectionCloseType::NoFlush);
}

void ClientImpl::onData(Buffer::Instance& data) {
  try {
    decoder_->decode(data);
  } catch (ProtocolError&) {
    putOutlierEvent(Upstream::Outlier::Result::REQUEST_FAILED);
    host_->cluster().stats().upstream_cx_protocol_error_.inc();
    host_->stats().rq_error_.inc();
    connection_->close(Network::ConnectionCloseType::NoFlush);
  }
}

void ClientImpl::putOutlierEvent(Upstream::Outlier::Result result) {
  if (!config_.disableOutlierEvents()) {
    host_->outlierDetector().putResult(result);
  }
}

void ClientImpl::onEvent(Network::ConnectionEvent event) {
  if (event == Network::ConnectionEvent::RemoteClose ||
      event == Network::ConnectionEvent::LocalClose) {
    if (!pending_requests_.empty()) {
      host_->cluster().stats().upstream_cx_destroy_with_active_rq_.inc();
      if (event == Network::ConnectionEvent::RemoteClose) {
        putOutlierEvent(Upstream::Outlier::Result::SERVER_FAILURE);
        host_->cluster().stats().upstream_cx_destroy_remote_with_active_rq_.inc();
      }
      if (event == Network::ConnectionEvent::LocalClose) {
        host_->cluster().stats().upstream_cx_destroy_local_with_active_rq_.inc();
      }
    }

    while (!pending_requests_.empty()) {
      PendingRequest& request = pending_requests_.front();
      if (!request.canceled_) {
        request.callbacks_.onFailure();
      } else {
        host_->cluster().stats().upstream_rq_cancelled_.inc();
      }
      pending_requests_.pop_front();
    }

    connect_or_op_timer_->disableTimer();
  } else if (event == Network::ConnectionEvent::Connected) {
    connected_ = true;
    ASSERT(!pending_requests_.empty());
    connect_or_op_timer_->enableTimer(config_.opTimeout());
  }

  if (event == Network::ConnectionEvent::RemoteClose && !connected_) {
    host_->cluster().stats().upstream_cx_connect_fail_.inc();
    host_->stats().cx_connect_fail_.inc();
  }
}

void ClientImpl::onRespValue(RespValuePtr&& value) {
  ASSERT(!pending_requests_.empty());
  PendingRequest& request = pending_requests_.front();
  if (!request.canceled_) {
    request.callbacks_.onResponse(std::move(value));
  } else {
    host_->cluster().stats().upstream_rq_cancelled_.inc();
  }
  pending_requests_.pop_front();

  // If there are no remaining ops in the pipeline we need to disable the timer.
  // Otherwise we boost the timer since we are receiving responses and there are more to flush out.
  if (pending_requests_.empty()) {
    connect_or_op_timer_->disableTimer();
  } else {
    connect_or_op_timer_->enableTimer(config_.opTimeout());
  }

  putOutlierEvent(Upstream::Outlier::Result::SUCCESS);
}

ClientImpl::PendingRequest::PendingRequest(ClientImpl& parent, PoolCallbacks& callbacks)
    : parent_(parent), callbacks_(callbacks) {
  parent.host_->cluster().stats().upstream_rq_total_.inc();
  parent.host_->stats().rq_total_.inc();
  parent.host_->cluster().stats().upstream_rq_active_.inc();
  parent.host_->stats().rq_active_.inc();
}

ClientImpl::PendingRequest::~PendingRequest() {
  parent_.host_->cluster().stats().upstream_rq_active_.dec();
  parent_.host_->stats().rq_active_.dec();
}

void ClientImpl::PendingRequest::cancel() {
  // If we get a cancellation, we just mark the pending request as cancelled, and then we drop
  // the response as it comes through. There is no reason to blow away the connection when the
  // remote is already responding as fast as possible.
  canceled_ = true;
}

ClientFactoryImpl ClientFactoryImpl::instance_;

ClientPtr ClientFactoryImpl::create(Upstream::HostConstSharedPtr host,
                                    Event::Dispatcher& dispatcher, const Config& config) {
  return ClientImpl::create(host, dispatcher, EncoderPtr{new EncoderImpl()}, decoder_factory_,
                            config);
}

} // namespace Client
} // namespace Redis
} // namespace Common
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
