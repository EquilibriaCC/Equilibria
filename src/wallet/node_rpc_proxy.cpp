// Copyright (c) 2017-2019, The Monero Project
// 
// All rights reserved.
// 
// Redistribution and use in source and binary forms, with or without modification, are
// permitted provided that the following conditions are met:
// 
// 1. Redistributions of source code must retain the above copyright notice, this list of
//    conditions and the following disclaimer.
// 
// 2. Redistributions in binary form must reproduce the above copyright notice, this list
//    of conditions and the following disclaimer in the documentation and/or other
//    materials provided with the distribution.
// 
// 3. Neither the name of the copyright holder nor the names of its contributors may be
//    used to endorse or promote products derived from this software without specific
//    prior written permission.
// 
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
// THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
// THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "node_rpc_proxy.h"
#include "storages/http_abstract_invoke.h"

#include <boost/thread.hpp>

#define RETURN_ON_RPC_RESPONSE_ERROR(r, error, res, method) \
  do { \
    CHECK_AND_ASSERT_MES(error.code == 0, error.message, error.message); \
    CHECK_AND_ASSERT_MES(r, std::string("Failed to connect to daemon"), "Failed to connect to daemon"); \
    /* empty string -> no connection */ \
    CHECK_AND_ASSERT_MES(!res.status.empty(), res.status, "No connection to daemon"); \
    CHECK_AND_ASSERT_MES(res.status != CORE_RPC_STATUS_BUSY, res.status, "Daemon busy"); \
    CHECK_AND_ASSERT_MES(res.status == CORE_RPC_STATUS_OK, res.status, "Error calling " + std::string(method) + " daemon RPC"); \
  } while(0)

using namespace epee;

namespace tools
{

static const std::chrono::seconds rpc_timeout = std::chrono::minutes(3) + std::chrono::seconds(30);

NodeRPCProxy::NodeRPCProxy(epee::net_utils::http::abstract_http_client &http_client, boost::recursive_mutex &mutex)
  : m_http_client(http_client)
  , m_daemon_rpc_mutex(mutex)
  , m_offline(false)
{
  invalidate();
}

void NodeRPCProxy::invalidate()
{
  m_all_service_nodes_cached_height = 0;
  m_all_service_nodes.clear();

  m_height = 0;
  for (size_t n = 0; n < 256; ++n)
    m_earliest_height[n] = 0;
  m_dynamic_base_fee_estimate = 0;
  m_dynamic_base_fee_estimate_cached_height = 0;
  m_dynamic_base_fee_estimate_grace_blocks = 0;
  m_fee_quantization_mask = 1;
  m_rpc_version = 0;
  m_target_height = 0;
  m_block_weight_limit = 0;
  m_get_info_time = 0;
  m_height_time = 0;
}

boost::optional<std::string> NodeRPCProxy::get_rpc_version(uint32_t &rpc_version) const
{
  if (m_offline)
    return boost::optional<std::string>("offline");
  if (m_rpc_version == 0)
  {
    cryptonote::COMMAND_RPC_GET_VERSION::request req_t{};
    cryptonote::COMMAND_RPC_GET_VERSION::response resp_t{};
    {
      const boost::lock_guard<boost::recursive_mutex> lock(m_daemon_rpc_mutex);
      bool r = net_utils::invoke_http_json_rpc("/json_rpc", "get_version", req_t, resp_t, m_http_client, rpc_timeout);
      RETURN_ON_RPC_RESPONSE_ERROR(r, epee::json_rpc::error{}, resp_t, "get_version");
    }
    m_rpc_version = resp_t.version;
  }
  rpc_version = m_rpc_version;
  return boost::optional<std::string>();
}

void NodeRPCProxy::set_height(uint64_t h)
{
  m_height = h;
  m_height_time = time(NULL);
}

boost::optional<std::string> NodeRPCProxy::get_info() const
{
  if (m_offline)
    return boost::optional<std::string>("offline");
  const time_t now = time(NULL);
  if (now >= m_get_info_time + 30) // re-cache every 30 seconds
  {
    cryptonote::COMMAND_RPC_GET_INFO::request req_t{};
    cryptonote::COMMAND_RPC_GET_INFO::response resp_t{};

    {
      const boost::lock_guard<boost::recursive_mutex> lock(m_daemon_rpc_mutex);
      bool r = net_utils::invoke_http_json_rpc("/json_rpc", "get_info", req_t, resp_t, m_http_client, rpc_timeout);
      RETURN_ON_RPC_RESPONSE_ERROR(r, epee::json_rpc::error{}, resp_t, "get_info");
    }

    m_height = resp_t.height;
    m_target_height = resp_t.target_height;
    m_block_weight_limit = resp_t.block_weight_limit ? resp_t.block_weight_limit : resp_t.block_size_limit;
    m_get_info_time = now;
    m_height_time = now;
  }
  return boost::optional<std::string>();
}

boost::optional<std::string> NodeRPCProxy::get_height(uint64_t &height) const
{
  const time_t now = time(NULL);
  if (now < m_height_time + 30) // re-cache every 30 seconds
  {
    height = m_height;
    return boost::optional<std::string>();
  }

  auto res = get_info();
  if (res)
    return res;
  height = m_height;
  return boost::optional<std::string>();
}

boost::optional<std::string> NodeRPCProxy::get_target_height(uint64_t &height) const
{
  auto res = get_info();
  if (res)
    return res;
  height = m_target_height;
  return boost::optional<std::string>();
}

boost::optional<std::string> NodeRPCProxy::get_block_weight_limit(uint64_t &block_weight_limit) const
{
  auto res = get_info();
  if (res)
    return res;
  block_weight_limit = m_block_weight_limit;
  return boost::optional<std::string>();
}

boost::optional<std::string> NodeRPCProxy::get_earliest_height(uint8_t version, uint64_t &earliest_height) const
{
  if (m_offline)
    return boost::optional<std::string>("offline");
  if (m_earliest_height[version] == 0)
  {
    cryptonote::COMMAND_RPC_HARD_FORK_INFO::request req_t{};
    cryptonote::COMMAND_RPC_HARD_FORK_INFO::response resp_t{};
    req_t.version = version;

    {
      const boost::lock_guard<boost::recursive_mutex> lock(m_daemon_rpc_mutex);
      bool r = net_utils::invoke_http_json_rpc("/json_rpc", "hard_fork_info", req_t, resp_t, m_http_client, rpc_timeout);
      RETURN_ON_RPC_RESPONSE_ERROR(r, epee::json_rpc::error{}, resp_t, "hard_fork_info");
    }

    m_earliest_height[version] = resp_t.earliest_height;
  }

  earliest_height = m_earliest_height[version];
  return boost::optional<std::string>();
}

boost::optional<uint8_t> NodeRPCProxy::get_hardfork_version() const
{
  cryptonote::COMMAND_RPC_HARD_FORK_INFO::request req{};
  cryptonote::COMMAND_RPC_HARD_FORK_INFO::response resp{};

  m_daemon_rpc_mutex.lock();
  bool r = net_utils::invoke_http_json_rpc("/json_rpc", "hard_fork_info", req, resp, m_http_client, rpc_timeout);
  m_daemon_rpc_mutex.unlock();
  CHECK_AND_ASSERT_MES(r, {}, "Failed to connect to daemon");
  CHECK_AND_ASSERT_MES(resp.status != CORE_RPC_STATUS_BUSY, {}, "Failed to connect to daemon");
  CHECK_AND_ASSERT_MES(resp.status == CORE_RPC_STATUS_OK, {}, "Failed to get hard fork status");

  return resp.version;
}

boost::optional<std::string> NodeRPCProxy::get_dynamic_base_fee_estimate(uint64_t grace_blocks, uint64_t &fee) const
{
  uint64_t height;

  boost::optional<std::string> result = get_height(height);
  if (result)
    return result;

  if (m_offline)
    return boost::optional<std::string>("offline");
  if (m_dynamic_base_fee_estimate_cached_height != height || m_dynamic_base_fee_estimate_grace_blocks != grace_blocks)
  {
    cryptonote::COMMAND_RPC_GET_BASE_FEE_ESTIMATE::request req_t{};
    cryptonote::COMMAND_RPC_GET_BASE_FEE_ESTIMATE::response resp_t{};
    req_t.grace_blocks = grace_blocks;

    {
      const boost::lock_guard<boost::recursive_mutex> lock(m_daemon_rpc_mutex);
      bool r = net_utils::invoke_http_json_rpc("/json_rpc", "get_fee_estimate", req_t, resp_t, m_http_client, rpc_timeout);
      RETURN_ON_RPC_RESPONSE_ERROR(r, epee::json_rpc::error{}, resp_t, "get_fee_estimate");
    }

    m_dynamic_base_fee_estimate = resp_t.fee;
    m_dynamic_base_fee_estimate_cached_height = height;
    m_dynamic_base_fee_estimate_grace_blocks = grace_blocks;
    m_fee_quantization_mask = resp_t.quantization_mask;
  }

  fee = m_dynamic_base_fee_estimate;
  return boost::optional<std::string>();
}

boost::optional<std::string> NodeRPCProxy::get_fee_quantization_mask(uint64_t &fee_quantization_mask) const
{
  uint64_t height;

  boost::optional<std::string> result = get_height(height);
  if (result)
    return result;

  if (m_offline)
    return boost::optional<std::string>("offline");
  if (m_dynamic_base_fee_estimate_cached_height != height)
  {
    cryptonote::COMMAND_RPC_GET_BASE_FEE_ESTIMATE::request req_t{};
    cryptonote::COMMAND_RPC_GET_BASE_FEE_ESTIMATE::response resp_t{};
    req_t.grace_blocks = m_dynamic_base_fee_estimate_grace_blocks;

    {
      const boost::lock_guard<boost::recursive_mutex> lock(m_daemon_rpc_mutex);
      bool r = net_utils::invoke_http_json_rpc("/json_rpc", "get_fee_estimate", req_t, resp_t, m_http_client, rpc_timeout);
      RETURN_ON_RPC_RESPONSE_ERROR(r, epee::json_rpc::error{}, resp_t, "get_fee_estimate");
    }

    m_dynamic_base_fee_estimate = resp_t.fee;
    m_dynamic_base_fee_estimate_cached_height = height;
    m_fee_quantization_mask = resp_t.quantization_mask;
  }

  fee_quantization_mask = m_fee_quantization_mask;
  if (fee_quantization_mask == 0)
  {
    MERROR("Fee quantization mask is 0, forcing to 1");
    fee_quantization_mask = 1;
  }
  return boost::optional<std::string>();
}

std::vector<cryptonote::COMMAND_RPC_GET_SERVICE_NODES::response::entry> NodeRPCProxy::get_service_nodes(std::vector<std::string> const &pubkeys, boost::optional<std::string> &failed) const
{
  std::vector<cryptonote::COMMAND_RPC_GET_SERVICE_NODES::response::entry> result;

  cryptonote::COMMAND_RPC_GET_SERVICE_NODES::request req = {};
  cryptonote::COMMAND_RPC_GET_SERVICE_NODES::response res = {};
  req.service_node_pubkeys = pubkeys;

  m_daemon_rpc_mutex.lock();
  bool r = net_utils::invoke_http_json_rpc("/json_rpc", "get_service_nodes", req, res, m_http_client, rpc_timeout);
  m_daemon_rpc_mutex.unlock();

  if (!r)
  {
    failed = std::string("Failed to connect to daemon");
    return result;
  }

  if (res.status == CORE_RPC_STATUS_BUSY)
  {
    failed = res.status;
    return result;
  }

  if (res.status != CORE_RPC_STATUS_OK)
  {
    failed = res.status;
    return result;
  }

  result = std::move(res.service_node_states);
  return result;
}

std::vector<cryptonote::COMMAND_RPC_GET_SERVICE_NODES::response::entry> NodeRPCProxy::get_all_service_nodes(boost::optional<std::string> &failed) const
{
  std::vector<cryptonote::COMMAND_RPC_GET_SERVICE_NODES::response::entry> result;

  uint64_t height;
  failed = get_height(height);
  if (failed)
    return result;

  {
    boost::lock_guard<boost::recursive_mutex> lock(m_daemon_rpc_mutex);
    if (m_all_service_nodes_cached_height != height)
    {
      cryptonote::COMMAND_RPC_GET_SERVICE_NODES::request req = {};
      cryptonote::COMMAND_RPC_GET_SERVICE_NODES::response res = {};

      bool r = epee::net_utils::invoke_http_json_rpc("/json_rpc", "get_all_service_nodes", req, res, m_http_client, rpc_timeout);

      if (!r)
      {
        failed = std::string("Failed to connect to daemon");
        return result;
      }

      if (res.status == CORE_RPC_STATUS_BUSY)
      {
        failed = res.status;
        return result;
      }

      if (res.status != CORE_RPC_STATUS_OK)
      {
        failed = res.status;
      }

      m_all_service_nodes_cached_height = height;
      m_all_service_nodes = std::move(res.service_node_states);
    }

    result = m_all_service_nodes;
  }

  return result;
}

}
