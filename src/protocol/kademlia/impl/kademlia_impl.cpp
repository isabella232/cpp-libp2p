/**
 * Copyright Soramitsu Co., Ltd. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <libp2p/common/types.hpp>
#include <libp2p/crypto/sha/sha256.hpp>
#include <libp2p/multi/content_identifier.hpp>
#include <libp2p/multi/content_identifier_codec.hpp>
#include <libp2p/network/connection_manager.hpp>
#include <libp2p/peer/address_repository.hpp>
#include <libp2p/protocol/kademlia/common.hpp>
#include <libp2p/protocol/kademlia/error.hpp>
#include <libp2p/protocol/kademlia/impl/add_provider_executor.hpp>
#include <libp2p/protocol/kademlia/impl/content_routing_table.hpp>
#include <libp2p/protocol/kademlia/impl/find_peer_executor.hpp>
#include <libp2p/protocol/kademlia/impl/find_providers_executor.hpp>
#include <libp2p/protocol/kademlia/impl/get_value_executor.hpp>
#include <libp2p/protocol/kademlia/impl/kademlia_impl.hpp>
#include <libp2p/protocol/kademlia/impl/put_value_executor.hpp>
#include <libp2p/protocol/kademlia/message.hpp>
#include <unordered_set>

namespace libp2p::protocol::kademlia {

  KademliaImpl::KademliaImpl(
      const Config &config, std::shared_ptr<Host> host,
      std::shared_ptr<Storage> storage,
      std::shared_ptr<ContentRoutingTable> content_routing_table,
      std::shared_ptr<PeerRoutingTable> peer_routing_table,
      std::shared_ptr<Validator> validator,
      std::shared_ptr<Scheduler> scheduler, std::shared_ptr<event::Bus> bus,
      std::shared_ptr<crypto::random::RandomGenerator> random_generator)
      : config_(config),
        host_(std::move(host)),
        storage_(std::move(storage)),
        content_routing_table_(std::move(content_routing_table)),
        peer_routing_table_(std::move(peer_routing_table)),
        validator_(std::move(validator)),
        scheduler_(std::move(scheduler)),
        bus_(std::move(bus)),
        random_generator_(std::move(random_generator)),
        protocol_(config_.protocolId),
        self_id_(host_->getId()),
        log_("kad", "Kademlia") {
    BOOST_ASSERT(host_ != nullptr);
    BOOST_ASSERT(storage_ != nullptr);
    BOOST_ASSERT(content_routing_table_ != nullptr);
    BOOST_ASSERT(peer_routing_table_ != nullptr);
    BOOST_ASSERT(validator_ != nullptr);
    BOOST_ASSERT(scheduler_ != nullptr);
    BOOST_ASSERT(bus_ != nullptr);
    BOOST_ASSERT(random_generator_ != nullptr);
  }

  void KademliaImpl::start() {
    BOOST_ASSERT(not started_);
    if (started_) {
      return;
    }
    started_ = true;

    // save himself into peer repo
    addPeer(host_->getPeerInfo(), true);

    // handle streams for observed protocol
    host_->setProtocolHandler(
        protocol_,
        [wp = weak_from_this()](protocol::BaseProtocol::StreamResult rstream) {
          if (auto self = wp.lock()) {
            self->handleProtocol(std::move(rstream));
          }
        });

    // subscribe to new connection
    new_connection_subscription_ =
        host_->getBus()
            .getChannel<network::event::OnNewConnectionChannel>()
            .subscribe([this](
                           // NOLINTNEXTLINE
                           std::weak_ptr<connection::CapableConnection> conn) {
              if (auto connection = conn.lock()) {
                // adding outbound connections only
                if (connection->isInitiator()) {
                  log_.debug("new outbound connection");
                  auto remote_peer_res = connection->remotePeer();
                  if (!remote_peer_res) {
                    return;
                  }
                  auto remote_peer_addr_res = connection->remoteMultiaddr();
                  if (!remote_peer_addr_res) {
                    return;
                  }
                  addPeer(
                      peer::PeerInfo{std::move(remote_peer_res.value()),
                                     {std::move(remote_peer_addr_res.value())}},
                      false);
                }
              }
            });

    // start random walking
    if (config_.randomWalk.enabled) {
      randomWalk();
    }
  }

  outcome::result<void> KademliaImpl::bootstrap() {
    return findRandomPeer();
  }

  outcome::result<void> KademliaImpl::putValue(Key key, Value value) {
    log_.debug("CALL: PutValue ({})", multi::detail::encodeBase58(key.data));

    if (auto res = storage_->putValue(key, std::move(value));
        not res.has_value()) {
      return res.as_failure();
    }

    return outcome::success();
  }

  outcome::result<void> KademliaImpl::getValue(const Key &key,
                                               FoundValueHandler handler) {
    log_.debug("CALL: GetValue ({})", multi::detail::encodeBase58(key.data));

    // Check if has actual value locally
    if (auto res = storage_->getValue(key); res.has_value()) {
      auto &[value, ts] = res.value();
      if (scheduler_->now() < ts) {
        if (handler) {
          handler(std::move(value));
          return outcome::success();
        }
      }
    }

    auto nearest_peer_infos = getNearestPeerInfos(NodeId(key));
    if (nearest_peer_infos.empty()) {
      log_.info("Can't do GetValue request: no peers to connect to");
      return Error::NO_PEERS;
    }

    auto get_value_executor =
        createGetValueExecutor(key, std::move(nearest_peer_infos), handler);

    return get_value_executor->start();
  }

  outcome::result<void> KademliaImpl::provide(const Key &key,
                                              bool need_notify) {
    log_.debug("CALL: Provide ({})", multi::detail::encodeBase58(key.data));

    content_routing_table_->addProvider(key, self_id_);

    if (not need_notify) {
      return outcome::success();
    }

    auto add_provider_executor = createAddProviderExecutor(key);

    return add_provider_executor->start();
  }

  outcome::result<void> KademliaImpl::findProviders(
      const Key &key, size_t limit, FoundProvidersHandler handler) {
    log_.debug("CALL: FindProviders ({})",
               multi::detail::encodeBase58(key.data));

    // Try to find locally
    auto providers = content_routing_table_->getProvidersFor(key, limit);
    if (not providers.empty()) {
      if (limit > 0 && providers.size() > limit) {
        std::vector<PeerInfo> result;
        result.reserve(limit);
        for (auto &provider : providers) {
          // Get peer info
          auto peer_info = host_->getPeerRepository().getPeerInfo(provider);
          if (peer_info.addresses.empty()) {
            continue;
          }

          // Check if connectable
          auto connectedness =
              host_->getNetwork().getConnectionManager().connectedness(
                  peer_info);
          if (connectedness == Message::Connectedness::CAN_NOT_CONNECT) {
            continue;
          }
          result.emplace_back(std::move(peer_info));
          if (result.size() >= limit) {
            break;
          }
        }
        if (result.size() >= limit) {
          scheduler_
              ->schedule([handler = std::move(handler),
                          result = std::move(result)] { handler(result); })
              .detach();

          log_.info("Found {} providers locally from host!", result.size());
          return outcome::success();
        }
      }
    }

    auto find_providers_executor =
        createGetProvidersExecutor(key, std::move(handler));

    return find_providers_executor->start();
  }

  void KademliaImpl::addPeer(const PeerInfo &peer_info, bool permanent) {
    log_.debug("CALL: AddPeer ({})", peer_info.id.toBase58());

    auto upsert_res =
        host_->getPeerRepository().getAddressRepository().upsertAddresses(
            peer_info.id,
            gsl::span(peer_info.addresses.data(), peer_info.addresses.size()),
            permanent ? peer::ttl::kPermanent : peer::ttl::kDay);
    if (not upsert_res) {
      log_.debug("{} was skipped at addind to peer routing table: {}",
                 peer_info.id.toBase58(), upsert_res.error().message());
      return;
    }

    auto update_res = peer_routing_table_->update(peer_info.id);
    if (not update_res) {
      log_.debug("{} was not added to peer routing table: {}",
                 peer_info.id.toBase58(), update_res.error().message());
      return;
    }
    if (update_res.value()) {
      log_.debug("{} was added to peer routing table; total {} peers",
                 peer_info.id.toBase58(), peer_routing_table_->size());
    } else {
      log_.trace("{} was updated to peer routing table",
                 peer_info.id.toBase58());
    }
  }

  outcome::result<void> KademliaImpl::findPeer(const peer::PeerId &peer_id,
                                               FoundPeerInfoHandler handler) {
    BOOST_ASSERT(handler);
    log_.debug("CALL: FindPeer ({})", peer_id.toBase58());

    // Try to find locally
    auto peer_info = host_->getPeerRepository().getPeerInfo(peer_id);
    if (not peer_info.addresses.empty()) {
      scheduler_
          ->schedule([handler = std::move(handler),
                      peer_info = std::move(peer_info)] { handler(peer_info); })
          .detach();

      log_.debug("{} found locally", peer_id.toBase58());
      return outcome::success();
    }

    auto find_peer_executor =
        createFindPeerExecutor(peer_id, std::move(handler));

    return find_peer_executor->start();
  }

  std::vector<PeerId> KademliaImpl::getNearestPeerIds(const NodeId &id) {
    return peer_routing_table_->getNearestPeers(id,
                                                config_.closerPeerCount * 2);
  }

  std::unordered_set<PeerInfo> KademliaImpl::getNearestPeerInfos(
      const NodeId &id) {
    // Get nearest peer
    auto nearest_peer_ids = getNearestPeerIds(id);

    std::unordered_set<PeerInfo> nearest_peer_infos;

    // Try to find over nearest peers
    for (const auto &nearest_peer_id : nearest_peer_ids) {
      // Exclude yoursef, because not found locally anyway
      if (nearest_peer_id == self_id_) {
        continue;
      }

      // Get peer info
      auto info = host_->getPeerRepository().getPeerInfo(nearest_peer_id);
      if (info.addresses.empty()) {
        continue;
      }

      // Check if connectable
      auto connectedness =
          host_->getNetwork().getConnectionManager().connectedness(info);
      if (connectedness == Message::Connectedness::CAN_NOT_CONNECT) {
        continue;
      }

      nearest_peer_infos.emplace(std::move(info));
    }

    return nearest_peer_infos;
  }

  void KademliaImpl::onMessage(const std::shared_ptr<Session> &session,
                               Message &&msg) {
    switch (msg.type) {
      case Message::Type::kPutValue:
        onPutValue(session, std::move(msg));
        break;
      case Message::Type::kGetValue:
        onGetValue(session, std::move(msg));
        break;
      case Message::Type::kAddProvider:
        onAddProvider(session, std::move(msg));
        break;
      case Message::Type::kGetProviders:
        onGetProviders(session, std::move(msg));
        break;
      case Message::Type::kFindNode:
        onFindNode(session, std::move(msg));
        break;
      case Message::Type::kPing:
        onPing(session, std::move(msg));
        break;
      default:
        session->close(Error::UNEXPECTED_MESSAGE_TYPE);
        return;
    }
  }

  void KademliaImpl::onPutValue(const std::shared_ptr<Session> &session,
                                Message &&msg) {
    if (not msg.record) {
      log_.warn("incoming PutValue failed: no record in message");
      return;
    }
    auto &[key, value, ts] = msg.record.value();

    log_.debug("MSG: PutValue ({})", multi::detail::encodeBase58(key.data));

    auto validation_res = validator_->validate(key, value);
    if (not validation_res) {
      log_.warn("incoming PutValue failed: {}",
                validation_res.error().message());
      return;
    }

    auto res = putValue(key, std::move(value));
    if (!res) {
      log_.warn("incoming PutValue failed: {}", res.error().message());
      return;
    }
  }

  void KademliaImpl::onGetValue(const std::shared_ptr<Session> &session,
                                Message &&msg) {
    if (msg.key.empty()) {
      log_.warn("incoming GetValue failed: empty key in message");
      return;
    }

    auto cid_res = ContentId::fromWire(msg.key);
    if (!cid_res) {
      log_.warn("incoming GetValue failed: invalid key in message");
      return;
    }
    auto &cid = cid_res.value();

    log_.debug("MSG: GetValue ({})", multi::detail::encodeBase58(cid.data));

    if (auto providers = content_routing_table_->getProvidersFor(cid);
        not providers.empty()) {
      std::vector<Message::Peer> peers;
      peers.reserve(config_.closerPeerCount);

      for (const auto &peer : providers) {
        auto info = host_->getPeerRepository().getPeerInfo(peer);
        if (info.addresses.empty()) {
          continue;
        }
        auto connectedness =
            host_->getNetwork().getConnectionManager().connectedness(info);
        peers.push_back({std::move(info), connectedness});
        if (peers.size() >= config_.closerPeerCount) {
          break;
        }
      }

      msg.provider_peers = std::move(peers);
    }

    auto res = storage_->getValue(cid);
    if (res) {
      auto &[value, expire] = res.value();
      msg.record = Message::Record{std::move(cid), std::move(value),
                                   std::to_string(expire)};
    }

    auto buffer = std::make_shared<std::vector<uint8_t>>();
    if (not msg.serialize(*buffer)) {
      session->close(Error::MESSAGE_SERIALIZE_ERROR);
      BOOST_UNREACHABLE_RETURN();
    }

    session->write(buffer, {});
  }

  void KademliaImpl::onAddProvider(const std::shared_ptr<Session> &session,
                                   Message &&msg) {
    if (not msg.provider_peers) {
      log_.warn("AddProvider failed: no provider_peers im message");
      return;
    }

    auto cid_res = ContentId::fromWire(msg.key);
    if (not cid_res) {
      log_.warn("AddProvider failed: invalid key in message");
      return;
    }
    auto &cid = cid_res.value();

    log_.debug("MSG: AddProvider ({})", multi::detail::encodeBase58(cid.data));

    auto &providers = msg.provider_peers.value();
    for (auto &provider : providers) {
      if (auto peer_id_res = session->stream()->remotePeerId()) {
        if (peer_id_res.value() == provider.info.id) {
          // Save providers who have provided themselves
          content_routing_table_->addProvider(cid, provider.info.id);
          addPeer(provider.info, false);
        }
      }
    }
  }

  void KademliaImpl::onGetProviders(const std::shared_ptr<Session> &session,
                                    Message &&msg) {
    if (msg.key.empty()) {
      log_.warn("GetProviders failed: empty key in message");
      return;
    }

    auto cid_res = ContentId::fromWire(msg.key);
    if (not cid_res) {
      log_.warn("GetProviders failed: invalid key in message");
      return;
    }
    auto &cid = cid_res.value();

    log_.debug("MSG: GetProviders ({})", multi::detail::encodeBase58(cid.data));

    auto peer_ids = content_routing_table_->getProvidersFor(
        cid, config_.closerPeerCount * 2);

    if (not peer_ids.empty()) {
      std::vector<Message::Peer> peers;
      peers.reserve(config_.closerPeerCount);

      for (const auto &p : peer_ids) {
        auto info = host_->getPeerRepository().getPeerInfo(p);
        if (info.addresses.empty()) {
          continue;
        }
        auto connectedness =
            host_->getNetwork().getConnectionManager().connectedness(info);
        peers.push_back({std::move(info), connectedness});
        if (peers.size() >= config_.closerPeerCount) {
          break;
        }
      }

      if (not peers.empty()) {
        msg.provider_peers = std::move(peers);
      }
    }

    peer_ids = peer_routing_table_->getNearestPeers(
        NodeId(cid), config_.closerPeerCount * 2);

    if (not peer_ids.empty()) {
      std::vector<Message::Peer> peers;
      peers.reserve(config_.closerPeerCount);

      for (const auto &p : peer_ids) {
        auto info = host_->getPeerRepository().getPeerInfo(p);
        if (info.addresses.empty()) {
          continue;
        }
        auto connectedness =
            host_->getNetwork().getConnectionManager().connectedness(info);
        peers.push_back({std::move(info), connectedness});
        if (peers.size() >= config_.closerPeerCount) {
          break;
        }
      }

      if (not peers.empty()) {
        msg.closer_peers = std::move(peers);
      }
    }

    auto buffer = std::make_shared<std::vector<uint8_t>>();
    if (not msg.serialize(*buffer)) {
      session->close(Error::MESSAGE_SERIALIZE_ERROR);
      BOOST_UNREACHABLE_RETURN();
    }

    session->write(buffer, {});
  }

  void KademliaImpl::onFindNode(const std::shared_ptr<Session> &session,
                                Message &&msg) {
    if (msg.closer_peers) {
      for (auto &peer : msg.closer_peers.value()) {
        if (peer.conn_status != Message::Connectedness::CAN_NOT_CONNECT) {
          // Add/Update peer info
          [[maybe_unused]] auto res =
              host_->getPeerRepository().getAddressRepository().upsertAddresses(
                  peer.info.id,
                  gsl::span(peer.info.addresses.data(),
                            peer.info.addresses.size()),
                  peer::ttl::kDay);
        }
      }
      msg.closer_peers.reset();
    }

    auto cid_res = ContentId::fromWire(msg.key);
    if (not cid_res) {
      log_.warn("FindNode failed: invalid key in message");
      return;
    }
    auto &cid = cid_res.value();

    log_.debug("MSG: FindNode ({})", multi::detail::encodeBase58(cid.data));

//  auto id_res =
//      peer::PeerId::fromBytes(gsl::span(msg.key.data(), msg.key.size()));
//  if (not id_res.has_value()) {
//    auto cid_res = multi::ContentIdentifierCodec::decode(
//        gsl::span(msg.key.data(), msg.key.size()));
//    if (cid_res.has_value()) {
//      auto &cid = cid_res.value();
//      if (cid.version == multi::ContentIdentifier::Version::V1
//          && cid.content_type == multi::MulticodecType::Code::RAW) {
//        id_res = peer::PeerId::fromBytes(cid.content_address.toBuffer());
//      }
//    }
//  }

    auto ids = getNearestPeerIds(NodeId(cid));

    std::vector<Message::Peer> peers;
    peers.reserve(config_.closerPeerCount);

    for (const auto &p : ids) {
      auto info = host_->getPeerRepository().getPeerInfo(p);
      if (info.addresses.empty()) {
        continue;
      }
      auto connectedness =
          host_->getNetwork().getConnectionManager().connectedness(info);
      peers.push_back({std::move(info), connectedness});
      if (peers.size() >= config_.closerPeerCount) {
        break;
      }
    }

    if (not peers.empty()) {
      msg.closer_peers = std::move(peers);
    }

    auto buffer = std::make_shared<std::vector<uint8_t>>();
    if (not msg.serialize(*buffer)) {
      session->close(Error::MESSAGE_SERIALIZE_ERROR);
      BOOST_UNREACHABLE_RETURN();
    }

    session->write(buffer, {});
  }

  void KademliaImpl::onPing(const std::shared_ptr<Session> &session,
                            Message &&msg) {
    msg.clear();

    auto buffer = std::make_shared<std::vector<uint8_t>>();
    if (not msg.serialize(*buffer)) {
      session->close(Error::MESSAGE_SERIALIZE_ERROR);
      BOOST_UNREACHABLE_RETURN();
    }

    session->write(buffer, {});
  }

  outcome::result<void> KademliaImpl::findRandomPeer() {
    BOOST_ASSERT(config_.randomWalk.enabled);

    common::Hash256 hash;
    random_generator_->fillRandomly(hash);

    auto peer_id =
        peer::PeerId::fromHash(
            multi::Multihash::create(multi::HashType::sha256, hash).value())
            .value();

    FoundPeerInfoHandler handler =
        [wp = weak_from_this()](outcome::result<PeerInfo> res) {
          if (auto self = wp.lock()) {
            if (res.has_value()) {
              self->addPeer(res.value(), false);
            }
          }
        };

    return findPeer(peer_id, handler);
  }

  void KademliaImpl::randomWalk() {
    BOOST_ASSERT(config_.randomWalk.enabled);

    // Doing walk
    [[maybe_unused]] auto result = findRandomPeer();

    auto iteration = random_walking_.iteration++;

    // if period end
    scheduler::Ticks delay =
        ((iteration % config_.randomWalk.queries_per_period) != 0)
        ? scheduler::toTicks(config_.randomWalk.delay)
        : scheduler::toTicks(config_.randomWalk.interval
                             - config_.randomWalk.delay
                                 * config_.randomWalk.queries_per_period);

    // Schedule next walking
    random_walking_.handle =
        scheduler_->schedule(delay, [this] { randomWalk(); });
  }

  std::shared_ptr<Session> KademliaImpl::openSession(
      std::shared_ptr<connection::Stream> stream) {
    auto [it, is_new_session] = sessions_.emplace(
        stream,
        std::make_shared<Session>(weak_from_this(), scheduler_, stream));
    assert(is_new_session);

    log_.debug("session opened, total sessions: {}", sessions_.size());

    return it->second;
  }

  void KademliaImpl::closeSession(std::shared_ptr<connection::Stream> stream) {
    auto it = sessions_.find(stream);
    if (it == sessions_.end()) {
      return;
    }

    auto &session = it->second;

    session->close();
    sessions_.erase(it);

    log_.debug("session completed, total sessions: {}", sessions_.size());
  }

  void KademliaImpl::handleProtocol(
      protocol::BaseProtocol::StreamResult stream_res) {
    if (!stream_res) {
      log_.info("incoming connection failed due to '{}'",
                stream_res.error().message());
      return;
    }

    auto &stream = stream_res.value();

    log_.debug("incoming connection from '{}'",
               stream->remoteMultiaddr().value().getStringAddress());

    addPeer(peer::PeerInfo{stream->remotePeerId().value(),
                           {stream->remoteMultiaddr().value()}},
            false);

    auto session = openSession(stream);

    if (!session->read()) {
      sessions_.erase(stream);
      stream->reset();
      return;
    }
  }

  std::shared_ptr<GetValueExecutor> KademliaImpl::createGetValueExecutor(
      ContentId sought_key, std::unordered_set<PeerInfo> nearest_peer_infos,
      FoundValueHandler handler) {
    return std::make_shared<GetValueExecutor>(
        config_, host_, scheduler_, shared_from_this(), shared_from_this(),
        content_routing_table_, validator_, shared_from_this(),
        std::move(sought_key), std::move(nearest_peer_infos),
        std::move(handler));
  }

  std::shared_ptr<PutValueExecutor> KademliaImpl::createPutValueExecutor(
      ContentId key, ContentValue value, std::vector<PeerId> addressees) {
    return std::make_shared<PutValueExecutor>(
        config_, host_, scheduler_, shared_from_this(), std::move(key),
        std::move(value), std::move(addressees));
  }

  std::shared_ptr<FindProvidersExecutor>
  KademliaImpl::createGetProvidersExecutor(ContentId sought_key,
                                           FoundProvidersHandler handler) {
    return std::make_shared<FindProvidersExecutor>(
        config_, host_, scheduler_, shared_from_this(), shared_from_this(),
        peer_routing_table_, std::move(sought_key), std::move(handler));
  }

  std::shared_ptr<AddProviderExecutor> KademliaImpl::createAddProviderExecutor(
      ContentId key) {
    return std::make_shared<AddProviderExecutor>(
        config_, host_, scheduler_, shared_from_this(), peer_routing_table_,
        std::move(key));
  }

  std::shared_ptr<FindPeerExecutor> KademliaImpl::createFindPeerExecutor(
      PeerId peer_id, FoundPeerInfoHandler handler) {
    return std::make_shared<FindPeerExecutor>(
        config_, host_, scheduler_, shared_from_this(), shared_from_this(),
        peer_routing_table_, std::move(peer_id), std::move(handler));
  }

}  // namespace libp2p::protocol::kademlia
