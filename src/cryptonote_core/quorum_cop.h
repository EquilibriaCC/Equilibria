// Copyright (c)      2018, The Loki Project
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

#pragma once

#include "blockchain.h"
#include "cryptonote_protocol/cryptonote_protocol_handler_common.h"

namespace triton
{
	class deregister_vote_pool;
};

namespace cryptonote
{
	class core;
};




namespace service_nodes
{

	struct ribbon_data {
		uint64_t height;
		uint64_t ribbon_blue;
	};

	class quorum_cop
		: public cryptonote::Blockchain::BlockAddedHook,
		public cryptonote::Blockchain::BlockchainDetachedHook,
		public cryptonote::Blockchain::InitHook
	{
	public:
		explicit quorum_cop(cryptonote::core& core);

		void init() override;
		void block_added(const cryptonote::block& block, const std::vector<cryptonote::transaction>& txs) override;
		void blockchain_detached(uint64_t height) override;

		bool handle_uptime_proof(const cryptonote::NOTIFY_UPTIME_PROOF::request &proof);
		bool handle_ribbon_data_received(const cryptonote::NOTIFY_RIBBON_DATA::request &data);

		static const uint64_t REORG_SAFETY_BUFFER_IN_BLOCKS = 20;
		static_assert(REORG_SAFETY_BUFFER_IN_BLOCKS < triton::service_node_deregister::VOTE_LIFETIME_BY_HEIGHT,
			"Safety buffer should always be less than the vote lifetime");
		bool prune_uptime_proof();

		uint64_t get_uptime_proof(const crypto::public_key &pubkey) const;
		uint64_t get_ribbon_data(const crypto::public_key &pubkey, uint64_t height);
		void clear_ribbon_data(uint64_t clear_height);
		bool send_out_ribbon();



		crypto::hash make_ribbon_key_hash(crypto::public_key pubkey, uint64_t height);
		bool generate_ribbon_data_request(const crypto::public_key& pubkey, const crypto::secret_key& seckey, cryptonote::NOTIFY_RIBBON_DATA::request& req);
		std::unordered_map<crypto::hash, ribbon_data> get_all_ribbon_data();
		
	private:

		cryptonote::core& m_core;
		uint64_t m_last_height;

		using timestamp = uint64_t;
		std::unordered_map<crypto::public_key, timestamp> m_uptime_proof_seen;
		std::unordered_map<crypto::hash, ribbon_data> m_ribbon_data_received; // use hash of pubkey + height as key
		mutable epee::critical_section m_lock;
	};
	void generate_uptime_proof_request(const crypto::public_key& pubkey, const crypto::secret_key& seckey, cryptonote::NOTIFY_UPTIME_PROOF::request& req);

}
