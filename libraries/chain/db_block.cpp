/*
 * Copyright (c) 2015 Cryptonomex, Inc., and contributors.
 *
 * The MIT License
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <graphene/chain/database.hpp>
#include <graphene/chain/db_with.hpp>
#include <graphene/chain/hardfork.hpp>

#include <graphene/chain/block_summary_object.hpp>
#include <graphene/chain/global_property_object.hpp>
#include <graphene/chain/operation_history_object.hpp>

#include <graphene/chain/proposal_object.hpp>
#include <graphene/chain/transaction_object.hpp>
#include <graphene/chain/witness_object.hpp>
#include <graphene/chain/protocol/fee_schedule.hpp>
//#include <graphene/chain/protocol/contract.hpp>
#include <graphene/chain/exceptions.hpp>
#include <graphene/chain/evaluator.hpp>
#include <graphene/chain/crontab_object.hpp>
#include <graphene/chain/temporary_authority.hpp>

#include <fc/smart_ref_impl.hpp>

//#include <threadpool/threadpool.hpp>
//using namespace boost::threadpool;

namespace graphene
{
namespace chain
{

bool database::is_known_block(const block_id_type &id) const
{
  return _fork_db.is_known_block(id) || _block_id_to_block.contains(id);
}
/**
 * Only return true *if* the transaction has not expired or been invalidated. If this
 * method is called with a VERY old transaction we will return false, they should
 * query things by blocks if they are that old.
 */
bool database::is_known_transaction(const transaction_id_type &id) const
{
  const auto &trx_idx = get_index_type<transaction_index>().indices().get<by_trx_id>();
  return trx_idx.find(id) != trx_idx.end();
}

block_id_type database::get_block_id_for_num(uint32_t block_num) const
{
  try
  {
    return _block_id_to_block.fetch_block_id(block_num);
  }
  FC_CAPTURE_AND_RETHROW((block_num))
}

optional<signed_block> database::fetch_block_by_id(const block_id_type &id) const
{
  auto b = _fork_db.fetch_block(id);
  if (!b)
    return _block_id_to_block.fetch_optional(id);
  return b->data;
}

optional<signed_block> database::fetch_block_by_number(uint32_t num) const
{
  auto results = _fork_db.fetch_block_by_number(num);
  if (results.size() == 1)
    return results[0]->data;
  else
    return _block_id_to_block.fetch_by_number(num);
  return optional<signed_block>();
}

const signed_transaction &database::get_recent_transaction(const string &trx_id) const
{
  //wdump((trx_id));
  auto &index = get_index_type<transaction_index>().indices().get<by_trx_hash>();
  auto itr = index.find(tx_hash_type(trx_id));
  FC_ASSERT(itr != index.end(), "No specified transaction was found in transaction_index");
  return itr->trx;
}
const transaction_in_block_info &database::get_transaction_in_block_info(const string &trx_id) const
{
  //wdump((trx_id));
  auto &index = get_index_type<transaction_in_block_index>().indices().get<by_trx_hash>();
  auto itr = index.find(tx_hash_type(trx_id));
  FC_ASSERT(itr != index.end(), "No specified transaction was found in transaction_in_block_index");
  return *itr;
}

const transaction_in_block_info &database::get_transaction_in_block_info(const string &trx_id,int &ret) const
{
   //wdump((trx_id));
   auto &index = get_index_type<transaction_in_block_index>().indices().get<by_trx_hash>();
   auto itr = index.find(tx_hash_type(trx_id));
   if(itr != index.end())
     ret = 1;
   else
     ret = 0;
   return *itr;
 }

std::vector<block_id_type> database::get_block_ids_on_fork(block_id_type head_of_fork) const
{
  pair<fork_database::branch_type, fork_database::branch_type> branches = _fork_db.fetch_branch_from(head_block_id(), head_of_fork);
  if (!((branches.first.back()->previous_id() == branches.second.back()->previous_id())))
  {
    edump((head_of_fork)(head_block_id())(branches.first.size())(branches.second.size()));
    assert(branches.first.back()->previous_id() == branches.second.back()->previous_id());
  }
  std::vector<block_id_type> result;
  for (const item_ptr &fork_block : branches.second)
    result.emplace_back(fork_block->id);
  result.emplace_back(branches.first.back()->previous_id());
  return result;
}

/**
 * Push block "may fail" in which case every partial change is unwound.  After
 * push block is successful the block is appended to the chain database on disk.
 *
 * @return true if we switched forks as a result of this push.
 */
bool database::push_block(const signed_block &new_block, uint32_t skip)
{
  bool result;
  detail::with_skip_flags(*this, skip, [&]() {
    detail::without_pending_transactions(*this, _pending_tx,
                                         [&]() {
                                           result = _push_block(new_block);
                                         });
  });
  return result;
}

bool database::_push_block(const signed_block &new_block)
{
  try
  {
    FC_ASSERT(new_block.block_id == new_block.make_id());
    uint32_t skip = get_node_properties().skip_flags;
    if (!(skip & skip_fork_db))
    {
      /// TODO: if the block is greater than the head block and before the next maitenance interval
      // verify that the block signer is in the current set of active witnesses.

      shared_ptr<fork_item> new_head = _fork_db.push_block(new_block);
      //If the head block from the longest chain does not build off of the current head, we need to switch forks.
      if (new_head->data.previous != head_block_id()) //  ????????????????????????????????????????????????????????????????????????,??????????????????
      {
        //If the newly pushed block is the same height as head, we get head back in new_head
        //Only switch forks if new_head is actually higher than head
        if (new_head->data.block_num() > head_block_num()) // ????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????
        {
          wlog("Switching to fork: ${id}", ("id", new_head->data.block_id));
          auto branches = _fork_db.fetch_branch_from(new_head->data.block_id, head_block_id()); //???????????????

          // pop blocks until we hit the forked block
          while (head_block_id() != branches.second.back()->data.previous) //pop  ???????????????????????????????????????????????????
            pop_block();

          // push all blocks on the new fork                                    //push ??????????????????????????????????????????????????????
          for (auto ritr = branches.first.rbegin(); ritr != branches.first.rend(); ++ritr)
          {
            ilog("pushing blocks from fork ${n} ${id}", ("n", (*ritr)->data.block_num())("id", (*ritr)->data.block_id));
            optional<fc::exception> except;
            try
            {
              undo_database::session session = _undo_db.start_undo_session();
              apply_block((*ritr)->data, skip); // ????????????
              _block_id_to_block.store((*ritr)->id, (*ritr)->data);
              session.commit();
            }
            catch (const fc::exception &e)
            {
              except = e;
            }
            if (except)
            {
              wlog("exception thrown while switching forks ${e}", ("e", except->to_detail_string()));
              // remove the rest of branches.first from the fork_db, those blocks are invalid
              while (ritr != branches.first.rend())
              {
                _fork_db.remove((*ritr)->data.block_id);
                ++ritr;
              }
              _fork_db.set_head(branches.second.front());

              // pop all blocks from the bad fork
              while (head_block_id() != branches.second.back()->data.previous)
                pop_block();

              // restore all blocks from the good fork
              for (auto ritr = branches.second.rbegin(); ritr != branches.second.rend(); ++ritr)
              {
                auto session = _undo_db.start_undo_session();
                apply_block((*ritr)->data, skip);
                _block_id_to_block.store(new_block.block_id, (*ritr)->data);
                session.commit();
              }
              throw *except;
            }
          }
          return true;
        }
        else
          return false;
      }
    }

    try
    {
      auto session = _undo_db.start_undo_session();
      apply_block(new_block, skip);
      _block_id_to_block.store(new_block.block_id, new_block);
      session.commit();
    }
    catch (const fc::exception &e)
    {
      elog("Failed to push new block:\n${e}", ("e", e.to_detail_string()));
      _fork_db.remove(new_block.block_id);
      throw;
    }

    return false;
  }
  FC_CAPTURE_AND_RETHROW((new_block))
}

/**
 * Attempts to push the transaction into the pending queue
 *
 * When called to push a locally generated transaction, set the skip_block_size_check bit on the skip argument. This
 * will allow the transaction to be pushed even if it causes the pending block size to exceed the maximum block size.
 * Although the transaction will probably not propagate further now, as the peers are likely to have their pending
 * queues full as well, it will be kept in the queue to be propagated later when a new block flushes out the pending
 * queues.
 */
processed_transaction database::push_transaction(const signed_transaction &trx, uint32_t skip, transaction_push_state push_state)
{
  try
  {
    processed_transaction result;
    detail::with_skip_flags(*this, skip, [&]() {
      result = _push_transaction(trx, push_state);
    });
    return result;
  }
  FC_CAPTURE_AND_RETHROW((trx))
}
processed_transaction database::_push_transaction(const signed_transaction &trx, transaction_push_state push_state)
{
  // If this is the first transaction pushed after applying a block, start a new undo session.
  // This allows us to quickly rewind to the clean state of the head block, in case a new block arrives.
  if (!_pending_tx_session.valid())
    _pending_tx_session = _undo_db.start_undo_session();

  // Create a temporary undo session as a child of _pending_tx_session.
  // The temporary session will be discarded by the destructor if
  // _apply_transaction fails.  If we make it to merge(), we
  // apply the changes.

  auto temp_session = _undo_db.start_undo_session();

  processed_transaction processed_trx;
  transaction_apply_mode mode;
  if (push_state != transaction_push_state::re_push)
  {
    if (push_state == transaction_push_state::from_me)
    {
      //get_message_send_cache_size();
      _pending_size=std::max(_pending_size,(uint64_t)_pending_tx.size());
      if (_message_cache_size_limit)
        FC_ASSERT(_pending_size <= _message_cache_size_limit, "The number of messages cached by the current node has exceeded the maximum limit,size:${size}", ("size", _pending_size));
      mode = transaction_apply_mode::push_mode;
      processed_trx = _apply_transaction(trx, mode);
    }
    else
    {
      mode = transaction_apply_mode::validate_transaction_mode;
      processed_trx = _apply_transaction(trx, mode, !deduce_in_verification_mode);
    }
  }
  else
  {
    uint32_t skip = get_node_properties().skip_flags;
    auto share_flag = database::skip_transaction_signatures|database::skip_tapos_check;
    if((trx.operations[0].which() == operation::tag<contract_share_fee_operation>::value)&&(skip!=share_flag))
      skip = database::skip_transaction_signatures|database::skip_tapos_check;

    const chain_parameters &chain_parameters = get_global_properties().parameters;
    if (BOOST_LIKELY(head_block_num() > 0))
    {
      if (!(skip & skip_tapos_check) && !trx.agreed_task)
      {
        const auto &tapos_block_summary = block_summary_id_type(trx.ref_block_num)(*this);
        FC_ASSERT(trx.ref_block_prefix == tapos_block_summary.block_id._hash[1]);
      }
      fc::time_point_sec now = head_block_time();
      FC_ASSERT(trx.expiration <= now + chain_parameters.maximum_time_until_expiration, "",
                ("trx.expiration", trx.expiration)("now", now)("max_til_exp", chain_parameters.maximum_time_until_expiration));
      FC_ASSERT(now <= trx.expiration, "", ("now", now)("trx.exp", trx.expiration));
    }
    processed_trx = *(processed_transaction *)&trx;
  }
  //(processed_trx.operation_results.size() > 0, "in ${push_state} ", ("push_state", push_state));
  _pending_tx.push_back(processed_trx); //nico ??????pending???

  //notify_changed_objects();               //??????????????????,push_mode???validate_transaction_mode,???????????????????????????,\
                                                ????????????????????????????????????????????????????????????????????????????????????(?????????????????????)
  // The transaction applied successfully. Merge its changes into the pending block session.
  if (push_state == transaction_push_state::re_push || mode == transaction_apply_mode::invoke_mode) //nico chang:: ????????????????????????????????????invoke_mode???????????????????????????????????????????????????
  {
    temp_session.undo();
    this->create<transaction_object>([&](transaction_object &transaction) {
         transaction.trx_hash=processed_trx.hash();
         transaction.trx_id = processed_trx.id(transaction.trx_hash);
         transaction.trx = processed_trx; });
  }
  temp_session.merge();

  // notify anyone listening to pending transactions
  on_pending_transaction(processed_trx);
  return processed_trx;
}

processed_transaction database::validate_transaction(const signed_transaction &trx)
{
  auto session = _undo_db.start_undo_session();
  auto mode = transaction_apply_mode::just_try;
  return _apply_transaction(trx, mode);
}

processed_transaction database::push_proposal(const proposal_object &proposal)
{
  try
  {
    transaction_evaluation_state eval_state(this);
    eval_state.is_agreed_task = true;

    eval_state.operation_results.reserve(proposal.proposed_transaction.operations.size());
    processed_transaction ptrx(proposal.proposed_transaction);
    eval_state._trx = &ptrx;
    size_t old_applied_ops_size = _applied_ops.size();

    try
    {
      auto session = _undo_db.start_undo_session(true);
      for (auto &op : proposal.proposed_transaction.operations)
        eval_state.operation_results.emplace_back(apply_operation(eval_state, op));
      remove(proposal);
      session.merge();
    }
    catch (const fc::exception &e)
    {
      /*
      if (head_block_time() <= HARDFORK_483_TIME)
      {
        for (size_t i = old_applied_ops_size, n = _applied_ops.size(); i < n; i++)
        {
          ilog("removing failed operation from applied_ops: ${op}", ("op", *(_applied_ops[i])));
          _applied_ops[i].reset();
        }
      }
      else*/
      {
        _applied_ops.resize(old_applied_ops_size);
      }
      elog("e", ("e", e.to_detail_string()));
      throw;
    }

    ptrx.operation_results = std::move(eval_state.operation_results);
    return ptrx;
  }
  FC_CAPTURE_AND_RETHROW((proposal))
}

signed_block database::generate_block(
    fc::time_point_sec when,
    witness_id_type witness_id,
    const fc::ecc::private_key &block_signing_private_key,
    uint32_t skip /* = 0 */
)
{
  try
  {
    signed_block result;
    detail::with_skip_flags(*this, skip, [&]() {
      result = _generate_block(when, witness_id, block_signing_private_key);
    });
    return result;
  }
  FC_CAPTURE_AND_RETHROW()
}

signed_block database::_generate_block(
    fc::time_point_sec when,
    witness_id_type witness_id,
    const fc::ecc::private_key &block_signing_private_key)
{
  try
  {
    uint32_t skip = get_node_properties().skip_flags;
    uint32_t slot_num = get_slot_at_time(when);
    FC_ASSERT(slot_num > 0);
    witness_id_type scheduled_witness = get_scheduled_witness(slot_num);
    FC_ASSERT(scheduled_witness == witness_id);
    const auto &witness_obj = witness_id(*this);
    if (!(skip & skip_witness_signature))
      FC_ASSERT(witness_obj.signing_key == block_signing_private_key.get_public_key());
    static const size_t max_block_header_size = fc::raw::pack_size(signed_block_header()) + 4;
    const chain_parameters &chain_parameters = get_global_properties().parameters;
    auto maximum_block_size = chain_parameters.maximum_block_size;
    size_t total_block_size = max_block_header_size;
    signed_block pending_block;
    // The following code throws away existing pending_tx_session and
    // rebuilds it by re-applying pending transactions.
    //
    // This rebuild is necessary because pending transactions' validity
    // and semantics may have changed since they were received, because
    // time-based semantics are evaluated based on the current block
    // time.  These changes can only be reflected in the database when
    // the value of the "when" variable is known, which means we need to
    // re-apply pending transactions in this method.

    _pending_tx_session.reset();
    _pending_tx_session = _undo_db.start_undo_session();
    //uint64_t postponed_tx_count = 0;

    for (const processed_transaction &tx : _pending_tx) // ???pending??????????????????????????????
    {
      size_t new_total_size = total_block_size + fc::raw::pack_size(tx);
      // postpone transaction if it would make block too big
      if (new_total_size >= maximum_block_size)
        break; //nico change:?????????????????????????????????????????????tx??????
      try
      {
        //if (tx.operation_results.size() > 0)
        //{
          
        if((tx.operations[0].which() == operation::tag<contract_share_fee_operation>::value))
          skip = database::skip_transaction_signatures|database::skip_tapos_check;

        if (BOOST_LIKELY(head_block_num() > 0)&& !tx.agreed_task)
        {
          if (!(skip & skip_tapos_check) )
          {
            const auto &tapos_block_summary = block_summary_id_type(tx.ref_block_num)(*this);
            FC_ASSERT(tx.ref_block_prefix == tapos_block_summary.block_id._hash[1]);
          }
          fc::time_point_sec now = head_block_time();
          FC_ASSERT(tx.expiration <= now + chain_parameters.maximum_time_until_expiration, "",
                    ("trx.expiration", tx.expiration)("now", now)("max_til_exp", chain_parameters.maximum_time_until_expiration));
          FC_ASSERT(now <= tx.expiration, "", ("now", now)("trx.exp", tx.expiration));
        }
        total_block_size += fc::raw::pack_size(tx);
        pending_block.transactions.push_back(std::make_pair(tx.hash(), tx));
        //}
      }
      catch (const fc::exception &e)
      {
        // Do nothing, transaction will not be re-applied
        wlog("Transaction was not processed while generating block due to ${e}", ("e", e));
        wlog("The transaction was ${t}", ("t", tx));
      }
    }
    /*
    if (postponed_tx_count > 0)
    {
      wlog("Postponed ${n} transactions due to block size limit", ("n", postponed_tx_count));
      // ???????????????????????????n??????????????????
    }*/

    _pending_tx_session.reset();

    // We have temporarily broken the invariant that
    // _pending_tx_session is the result of applying _pending_tx, as
    // _pending_tx now consists of the set of postponed transactions.
    // However, the push_block() call below will re-create the
    // _pending_tx_session.

    pending_block.previous = head_block_id();
    if(pending_block.previous==block_id_type())
    {
      pending_block.extensions=vector<string>{"Ignition with Kevin , Nico , Major and Wililiam"};
    }
    pending_block.timestamp = when;
    //pending_block.transaction_merkle_root = pending_block.calculate_merkle_root();
    pending_block.witness = witness_id;

    // TODO:  Move this to _push_block() so session is restored.

    uint skip_authority=skip_authority_check;
    if(!deduce_in_verification_mode)
       skip_authority=0;      
    validate_block(pending_block, block_signing_private_key, skip | skip_authority | skip_merkle_check | skip_witness_signature); // push_transation , _apply_transaction , push_block 3???????????????
    return pending_block;
  }
  FC_CAPTURE_AND_RETHROW((witness_id))
}

/**
 * Removes the most recent block from the database and
 * undoes any changes it made.
 */
void database::pop_block()
{
  try
  {
    _pending_tx_session.reset();
    auto head_id = head_block_id();
    optional<signed_block> head_block = fetch_block_by_id(head_id);
    GRAPHENE_ASSERT(head_block.valid(), pop_empty_chain, "there are no blocks to pop");

    _fork_db.pop_block();
    pop_undo();
    _popped_tx.resize(head_block->transactions.size());
    std::transform(head_block->transactions.begin(), head_block->transactions.end(), _popped_tx.begin(), [&](std::pair<tx_hash_type, processed_transaction> trx_pair) { return trx_pair.second; });
    //_popped_tx.insert(_popped_tx.begin(), head_block->transactions.begin(), head_block->transactions.end());
  }
  FC_CAPTURE_AND_RETHROW()
}

void database::clear_pending()
{
  try
  {
    assert((_pending_tx.size() == 0) || _pending_tx_session.valid());
    _pending_tx.clear();
    _pending_tx_session.reset();
  }
  FC_CAPTURE_AND_RETHROW()
}

uint32_t database::push_applied_operation(const operation &op)
{
  _applied_ops.emplace_back(op);
  /*
   "emplace_back" ??? "push_back" ?????????:???????????????????????????????????????, ?????????push_back,emplace_back?????????????????????????????????????????????.
   */
  operation_history_object &oh = *(_applied_ops.back());
  oh.block_num = _current_block_num;
  oh.trx_in_block = _current_trx_in_block;
  oh.op_in_trx = _current_op_in_trx;
  oh.virtual_op = _current_virtual_op++;
  return _applied_ops.size() - 1;
}
void database::set_applied_operation_result(uint32_t op_id, const operation_result &result)
{
  assert(op_id < _applied_ops.size());
  if (_applied_ops[op_id])
    _applied_ops[op_id]->result = result;
  else
  {
    elog("Could not set operation result (head_block_num=${b})", ("b", head_block_num()));
  }
}

const vector<optional<operation_history_object>> &database::get_applied_operations() const
{
  return _applied_ops;
}

//////////////////// private methods ////////////////////

void database::apply_block(const signed_block &next_block, uint32_t skip)
{
  auto block_num = next_block.block_num();
  if (_checkpoints.size() && _checkpoints.rbegin()->second != block_id_type())
  {
    auto itr = _checkpoints.find(block_num);
    if (itr != _checkpoints.end())
      FC_ASSERT(next_block.block_id == itr->second, "Block did not match checkpoint", ("checkpoint", *itr)("block_id", next_block.block_id));

    if (_checkpoints.rbegin()->first >= block_num)
      skip = ~0; // WE CAN SKIP ALMOST EVERYTHING
  }

  detail::with_skip_flags(*this, skip, [&]() {
    _apply_block(next_block);
  });
  return;
}

void database::_apply_block(const signed_block &next_block)
{
  try
  {
    uint32_t next_block_num = next_block.block_num();
    uint32_t skip = get_node_properties().skip_flags;
    _applied_ops.clear();

    FC_ASSERT((skip & skip_merkle_check) || next_block.transaction_merkle_root == next_block.calculate_merkle_root() /*checking_transactions_hash()*/, "",
              ("next_block.transaction_merkle_root", next_block.transaction_merkle_root)("calc", next_block.calculate_merkle_root() /*checking_transactions_hash()*/)("next_block", next_block)("id", next_block.block_id));
    const witness_object &signing_witness = validate_block_header(skip, next_block);
    const auto &global_props = get_global_properties();
    const auto &dynamic_global_props = get<dynamic_global_property_object>(dynamic_global_property_id_type());
    bool maint_needed = (dynamic_global_props.next_maintenance_time <= next_block.timestamp);

    _current_block_num = next_block_num;
    _current_trx_in_block = 0;

    for (auto &trx : next_block.transactions) // ??????????????????tx
    {
      /* We do not need to push the undo state for each transaction
       * because they either all apply and are valid or the
       * entire block fails to apply.  We only need an "undo" state
       * for transactions when validating broadcast transactions or
       * when building a block.
       */
      FC_ASSERT(trx.second.operation_results.size() > 0, "trx_hash:${trx_hash}", ("trx_hash", trx.second.hash()));
      apply_transaction(trx.second, skip | skip_authority_check, transaction_apply_mode::apply_block_mode); // ????????????transaction , ?????????????????????????????????tx??????????????????
      ++_current_trx_in_block;
    }
    update_global_dynamic_data(next_block);
    update_signing_witness(signing_witness, next_block);
    update_last_irreversible_block();
    // Are we at the maintenance interval?
    if (maint_needed)
      perform_chain_maintenance(next_block, global_props);

    create_block_summary(next_block);
    clear_expired_transactions(); // hash??????????????????????????????database????????????
    clear_expired_nh_asset_orders();
    clear_expired_proposals();
    clear_expired_orders();
    clear_expired_timed_task();
    update_expired_feeds();
    clear_expired_active();

    // n.b., update_maintenance_flag() happens this late
    // because get_slot_time() / get_slot_at_time() is needed above
    // TODO:  figure out if we could collapse this function into
    // update_global_dynamic_data() as perhaps these methods only need
    // to be called for header validation?
    update_maintenance_flag(maint_needed);
    update_witness_schedule();
    if (!_node_property_object.debug_updates.empty())
      apply_debug_updates();

    // notify observers that the block has been applied
    applied_block(next_block); // applied_block????????????
    _applied_ops.clear();
    notify_changed_objects(); // ????????????????????????
  }
  FC_CAPTURE_AND_RETHROW((next_block.block_num()))
}

processed_transaction database::apply_transaction(const signed_transaction &trx, uint32_t skip, transaction_apply_mode run_mode)
{
  processed_transaction result;
  detail::with_skip_flags(*this, skip, [&]() {
    result = _apply_transaction(trx, run_mode);
  });
  return result;
}

processed_transaction database::_apply_transaction(const signed_transaction &trx, transaction_apply_mode &run_mode, bool only_try_permissions)
{
  //fc::microseconds start1 = fc::time_point::now().time_since_epoch();
  try
  { 
    uint32_t skip = get_node_properties().skip_flags;

    auto share_flag = database::skip_transaction_signatures|database::skip_tapos_check;
    if((trx.operations[0].which() == operation::tag<contract_share_fee_operation>::value)&&(skip!=share_flag))
    {
      skip = database::skip_transaction_signatures|database::skip_tapos_check;
    }
    auto &chain_parameters = get_global_properties().parameters;

    int op_maxsize_proportion_percent = 1; //default

    if (_options->count("op_maxsize_proportion_percent"))
    {
      auto percent = _options->at("op_maxsize_proportion_percent").as<uint32_t>(); 

      if(percent>=0 && percent<=100) //if percent out of range,just do nothing
        op_maxsize_proportion_percent = percent;
    }
    int size = chain_parameters.maximum_block_size*op_maxsize_proportion_percent/GRAPHENE_FULL_PROPOTION;
    FC_ASSERT(fc::raw::pack_size(trx) < size);//???????????????????????????????????????????????????????????????????????????????????????
    if (!(skip & skip_validate))                                                    /* issue #505 explains why this skip_flag is disabled */
      trx.validate();
    auto &trx_idx = get_mutable_index_type<transaction_index>();
    const chain_id_type &chain_id = get_chain_id();
    fc::time_point_sec now = head_block_time();
    auto trx_hash = trx.hash();
    auto trx_id = trx.id(trx_hash);
    if(trx.operations[0].which() != operation::tag<contract_share_fee_operation>::value)
      FC_ASSERT((skip & skip_transaction_dupe_check) || trx_idx.indices().get<by_trx_id>().find(trx_id) == trx_idx.indices().get<by_trx_id>().end());
    transaction_evaluation_state eval_state(this);
    eval_state._trx = &trx;
    eval_state.run_mode = run_mode;
    eval_state.skip=skip;
    const crontab_object *temp_crontab = nullptr; 
    if (!(skip & (skip_transaction_signatures | skip_authority_check)) || trx.agreed_task)
    {
      if (trx.agreed_task)
      {
        object_id_type id = trx.agreed_task->second;
        switch (id.type())
        {
        case proposal_object::type_id:
        {
          auto &proposal = ((proposal_id_type)id)(*this);
          FC_ASSERT(trx_hash == proposal.proposed_transaction.hash() && proposal.expiration_time <= now && proposal.allow_execution);
          modify(proposal,[](proposal_object &pr){pr.allow_execution=false;});
          break;
        }
        case crontab_object::type_id:
        {
          auto &crontab = ((crontab_id_type)id)(*this);
          temp_crontab = &crontab;
          FC_ASSERT(trx_hash == crontab.timed_transaction.hash() && crontab.next_execte_time <= now&&crontab.allow_execution);
          modify(crontab, [&](crontab_object &c) {
            c.last_execte_time = now;
            c.next_execte_time = c.last_execte_time + c.execute_interval;
            c.expiration_time = c.last_execte_time + (c.scheduled_execute_times - c.already_execute_times) * c.execute_interval;
            c.already_execute_times++;
            c.timed_transaction.expiration = c.next_execte_time + std::min<uint32_t>(chain_parameters.assigned_task_life_cycle, 7200);
          });
          break;
        }
        default:
          FC_THROW("Unexpected System Transactions");
        }
        eval_state.is_agreed_task=trx.agreed_task.valid()?true:false;
      }
      else
      {
        authority active;
        auto get_active = [&](account_id_type id) -> const authority * { 
                  active=id(*this).active;
                  /*************************nico ????????????????????????******************/
                  flat_map<public_key_type,weight_type>  temporary;
                  auto &index = this->get_index_type<temporary_active_index>().indices().get<by_account_id_type>(); 
                  auto itr = index.find(id);if(itr!=index.end())temporary=itr->temporary_active;
                  for(auto itr=temporary.begin();itr!=temporary.end();itr++)active.key_auths.insert(*itr);return &active; };
        auto get_owner = [&](account_id_type id) -> const authority * { return &id(*this).owner; };
        trx.verify_authority(chain_id, get_active, get_owner, eval_state.sigkeys, get_global_properties().parameters.max_authority_depth);
        // ??????_apply_transaction ??????????????????
      }
    }
    if (BOOST_LIKELY(head_block_num() > 0)&&!eval_state.is_agreed_task)
    {
      if (!(skip & skip_tapos_check))
      {
        const auto &tapos_block_summary = block_summary_id_type(trx.ref_block_num)(*this);
        FC_ASSERT(trx.ref_block_prefix == tapos_block_summary.block_id._hash[1]);
      }
      FC_ASSERT(trx.expiration <= now + chain_parameters.maximum_time_until_expiration, "",
                ("trx.expiration", trx.expiration)("now", now)("max_til_exp", chain_parameters.maximum_time_until_expiration));
      FC_ASSERT(now <= trx.expiration, "", ("now", now)("trx.exp", trx.expiration));
    }
    if (run_mode == transaction_apply_mode::apply_block_mode || run_mode == transaction_apply_mode::production_block_mode)
    {
      auto temp = this->create<transaction_in_block_info>([&](transaction_in_block_info &info) { //
        info.trx_hash = trx_hash;
        info.block_num = _current_block_num;
        info.trx_in_block = _current_trx_in_block;
      });
    }
    if (!(skip & skip_transaction_dupe_check)||trx.operations[0].which() == operation::tag<contract_share_fee_operation>::value)
    {
       try{
             this->create<transaction_object>([&](transaction_object &transaction) {
         transaction.trx_hash=trx_hash;
         transaction.trx_id = trx_id;
         transaction.trx = trx; });
       }catch(...)
       {
         ilog("+++error in apply_transactionwhen create tx_object"); 
       }
  
    }
    processed_transaction ptrx(trx);
    if (only_try_permissions)
    {
      //ptrx.operation_results.push_back(void_result());
      return ptrx;
    }
    eval_state.operation_results.reserve(trx.operations.size());
    _current_op_in_trx = 0;
    uint64_t real_run_time = 0;
    auto get_runtime = operation_result_visitor_get_runtime();
    bool result_contains_error = false;
    
    // add auto gas
    account_id_type last_from = account_id_type();
    for (const auto &op : ptrx.operations)
    {
      auto op_result = apply_operation(eval_state, op, eval_state.is_agreed_task);
      real_run_time += op_result.visit(get_runtime);
      if (run_mode != transaction_apply_mode::apply_block_mode)
        FC_ASSERT(real_run_time < block_interval() * 75000ll, "Total execution time exceeds block interval,tx:${tx}", ("tx", trx)); //block_interval*75%
      if (run_mode == transaction_apply_mode::apply_block_mode && eval_state.is_agreed_task)
        FC_ASSERT(op_result == ((processed_transaction *)eval_state._trx)->operation_results[_current_op_in_trx]);
      eval_state.operation_results.emplace_back(op_result);
      if (op_result.which() == operation_result::tag<contract_result>::value && op_result.get<contract_result>().existed_pv)
        run_mode = transaction_apply_mode::invoke_mode; // ????????????,?????????????????????
      ++_current_op_in_trx;
      if (op_result.which() == operation_result::tag<error_result>::value)
      {
        result_contains_error = true;
      }

      if( head_block_time() > AUTO_GAS_TIMEPOINT )
      {
        auto call_contract_condition = (op.which() == operation::tag<call_contract_function_operation>::value && op_result.which() == operation_result::tag<contract_result>::value);
        auto transfer_condition = (op.which() == operation::tag<transfer_operation>::value && op_result.which() == operation_result::tag<void_result>::value);
        if ( call_contract_condition || transfer_condition )
        {
          account_id_type op_from;
          if( call_contract_condition ){
            op_from = op.get<call_contract_function_operation>().caller;
          }
          if( transfer_condition ){
            op_from = op.get<transfer_operation>().from;
          }
          if(last_from != op_from){
            auto_gas(eval_state, op_from);
            last_from = op_from;
          }
        }
      }
    }

    /* If the task fails for 3 consecutive executions, it will be suspended and set to expire after 3 days, but if the 
     * number of task executions has reached the schedule execute times, it will be deleted directly instead of suspend.
     */
    if (temp_crontab != nullptr && temp_crontab->already_execute_times < temp_crontab->scheduled_execute_times)
    {
      if (result_contains_error)
      {
        modify(*temp_crontab, [&](crontab_object &c) {
          c.continuous_failure_times++;
          if (chain_parameters.crontab_suspend_threshold == c.continuous_failure_times) // the task execution fails consecutively 3 times, it will be suspended
          {
            c.next_execte_time = fc::time_point_sec::maximum();
            c.is_suspended = true;
            c.expiration_time = now + chain_parameters.crontab_suspend_expiration; // the task is suspended, modify its expiration time to be 3 days later
          }
        });
      }
      else if (0 != temp_crontab->continuous_failure_times) // reset crontab's continuous failure times
        modify(*temp_crontab, [&](crontab_object &c) { c.continuous_failure_times = 0; });
    }
    //Insert transaction into unique transactions database.
    ptrx.operation_results = std::move(eval_state.operation_results);
    return ptrx;
  }
  FC_CAPTURE_AND_RETHROW((trx))
}

void database::auto_gas(transaction_evaluation_state &eval_state, account_id_type from){
    vector<vesting_balance_object> vbos;
    auto vesting_range = get_index_type<vesting_balance_index>().indices().get<by_account>().equal_range(from);
    std::for_each(vesting_range.first, vesting_range.second,
                  [&vbos](const vesting_balance_object &balance) {
                      vbos.emplace_back(balance);
                  });

    vesting_balance_withdraw_operation vesting_balance_withdraw_op;
    if(!vbos.empty())
    {
      fc::optional<vesting_balance_id_type> vbid = maybe_id<vesting_balance_id_type>(string(vbos.begin()->id));
      if(vbid)
      {                        
        auto now = head_block_time();
        auto vbo1_tmp = find_object(*vbid);
        const vesting_balance_object *vbo1 = static_cast<const vesting_balance_object*>(vbo1_tmp);
        vesting_balance_withdraw_op.vesting_balance = *vbid;
        vesting_balance_withdraw_op.owner = vbo1->owner;
        vesting_balance_withdraw_op.amount = vbo1->get_allowed_withdraw(now);
        if( vesting_balance_withdraw_op.amount.asset_id == asset_id_type(1) && vesting_balance_withdraw_op.amount > asset(100000, asset_id_type(1)) )
        {
          try
          {
            auto op_result = apply_operation(eval_state, vesting_balance_withdraw_op);
            if (op_result.which() != operation_result::tag<error_result>::value)
            {
              eval_state.operation_results.emplace_back(op_result);
              return ;
            }
            wlog("auto gas failed...");
          }
          catch (...)
          {
            wlog("auto gas failed...");
          }
        }
      }
    }
}

operation_result database::apply_operation(transaction_evaluation_state &eval_state, const operation &op, bool is_agreed_task)
{
  try
  {
    operation_result result;
    bool _undo_db_state = _undo_db.enabled();
    _undo_db.enable();
    fc::microseconds start = fc::time_point::now().time_since_epoch();
    {
      auto op_session = _undo_db.start_undo_session();
      try
      {
        int i_which = op.which();
        uint64_t u_which = uint64_t(i_which);
        if (i_which < 0)
          assert("Negative operation tag" && false);
        if (u_which >= _operation_evaluators.size())
          assert("No registered evaluator for this operation" && false);
        unique_ptr<op_evaluator> &eval = _operation_evaluators[u_which]; //  ????????????????????????????????????
        if (!eval)
          assert("No registered evaluator for this operation" && false);
        result = eval->evaluate(eval_state, op, true);
      }
      catch (fc::exception &e)
      {
        if (is_agreed_task)
        {
          auto error_re = error_result(e.code(), e.to_string());
          error_re.real_running_time = fc::time_point::now().time_since_epoch().count() - start.count();
          result = error_re;
          op_session.undo();
        }
        else
          throw e;
      }
      auto op_id = push_applied_operation(op);
      set_applied_operation_result(op_id, result);
      op_session.merge();
    }
    _undo_db_state ? _undo_db.enable() : _undo_db.disable();
    return result;
  }
  FC_CAPTURE_AND_RETHROW((op))
}

const witness_object &database::validate_block_header(uint32_t skip, const signed_block &next_block) const
{
  FC_ASSERT(head_block_id() == next_block.previous, "", ("head_block_id", head_block_id())("next.prev", next_block.previous));
  FC_ASSERT(head_block_time() < next_block.timestamp, "", ("head_block_time", head_block_time())("next", next_block.timestamp)("blocknum", next_block.block_num()));
  const witness_object &witness = next_block.witness(*this);

  if (!(skip & skip_witness_signature))
    FC_ASSERT(next_block.validate_signee(witness.signing_key));

  if (!(skip & skip_witness_schedule_check))
  {
    uint32_t slot_num = get_slot_at_time(next_block.timestamp);
    FC_ASSERT(slot_num > 0);

    witness_id_type scheduled_witness = get_scheduled_witness(slot_num);

    FC_ASSERT(next_block.witness == scheduled_witness, "Witness produced block at wrong time",
              ("block witness", next_block.witness)("scheduled", scheduled_witness)("slot_num", slot_num));
  }

  return witness;
}

void database::create_block_summary(const signed_block &next_block)
{
  block_summary_id_type sid(next_block.block_num() & 0xffff);
  modify(sid(*this), [&](block_summary_object &p) {
    p.block_id = next_block.block_id;
  });
}
void database::set_message_cache_size_limit(uint16_t message_cache_size_limit)
{
  FC_ASSERT(message_cache_size_limit >= 3000 || message_cache_size_limit == 0);
  _message_cache_size_limit = message_cache_size_limit;
}

void database::add_checkpoints(const flat_map<uint32_t, block_id_type> &checkpts)
{
  for (const auto &i : checkpts)
    _checkpoints[i.first] = i.second;
}

bool database::before_last_checkpoint() const
{
  return (_checkpoints.size() > 0) && (_checkpoints.rbegin()->first >= head_block_num());
}
bool database::log_pending_size()
{
  _pending_size=_pending_tx.size();
  return true;
}
} // namespace chain
} // namespace graphene
