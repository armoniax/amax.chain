#include <eosio/producer_plugin/producer_plugin.hpp>
#include <eosio/producer_plugin/subjective_billing.hpp>
#include <eosio/chain/plugin_interface.hpp>
#include <eosio/chain/global_property_object.hpp>
#include <eosio/chain/generated_transaction_object.hpp>
#include <eosio/chain/snapshot.hpp>
#include <eosio/chain/transaction_object.hpp>
#include <eosio/chain/thread_utils.hpp>
#include <eosio/chain/unapplied_transaction_queue.hpp>

#include <fc/io/json.hpp>
#include <fc/log/logger_config.hpp>
#include <fc/smart_ref_impl.hpp>
#include <fc/scoped_exit.hpp>

#include <boost/asio.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>

#include <iostream>
#include <algorithm>
#include <boost/algorithm/string.hpp>
#include <boost/range/adaptor/map.hpp>
#include <boost/function_output_iterator.hpp>
#include <boost/multi_index_container.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/signals2/connection.hpp>

namespace bmi = boost::multi_index;
using bmi::indexed_by;
using bmi::ordered_non_unique;
using bmi::member;
using bmi::tag;
using bmi::hashed_unique;

using boost::multi_index_container;

using std::string;
using std::vector;
using std::deque;
using boost::signals2::scoped_connection;

#undef FC_LOG_AND_DROP
#define ACCEPT_BLOCK_IN_PRODUCING // TODO: remove?
#define LOG_AND_DROP()  \
   catch ( const guard_exception& e ) { \
      chain_plugin::handle_guard_exception(e); \
   } catch ( const std::bad_alloc& ) { \
      chain_plugin::handle_bad_alloc(); \
   } catch ( boost::interprocess::bad_alloc& ) { \
      chain_plugin::handle_db_exhaustion(); \
   } catch( fc::exception& er ) { \
      wlog( "${details}", ("details",er.to_detail_string()) ); \
   } catch( const std::exception& e ) {  \
      fc::exception fce( \
                FC_LOG_MESSAGE( warn, "std::exception: ${what}: ",("what",e.what()) ), \
                fc::std_exception_code,\
                BOOST_CORE_TYPEID(e).name(), \
                e.what() ) ; \
      wlog( "${details}", ("details",fce.to_detail_string()) ); \
   } catch( ... ) {  \
      fc::unhandled_exception e( \
                FC_LOG_MESSAGE( warn, "unknown: ",  ), \
                std::current_exception() ); \
      wlog( "${details}", ("details",e.to_detail_string()) ); \
   }

const fc::string logger_name("producer_plugin");
fc::logger _log;

const fc::string trx_successful_trace_logger_name("transaction_tracing");
fc::logger       _trx_successful_trace_log;

const fc::string trx_failed_trace_logger_name("transaction_failure_tracing");
fc::logger       _trx_failed_trace_log;

namespace eosio {

static appbase::abstract_plugin& _producer_plugin = app().register_plugin<producer_plugin>();

using namespace eosio::chain;
using namespace eosio::chain::plugin_interface;

namespace {
   bool exception_is_exhausted(const fc::exception& e, bool deadline_is_subjective) {
      auto code = e.code();
      return (code == block_cpu_usage_exceeded::code_value) ||
             (code == block_net_usage_exceeded::code_value) ||
             (code == deadline_exception::code_value && deadline_is_subjective);
   }
}

struct transaction_id_with_expiry {
   transaction_id_type     trx_id;
   fc::time_point          expiry;
};

struct by_id;
struct by_expiry;

using transaction_id_with_expiry_index = multi_index_container<
   transaction_id_with_expiry,
   indexed_by<
      hashed_unique<tag<by_id>, BOOST_MULTI_INDEX_MEMBER(transaction_id_with_expiry, transaction_id_type, trx_id)>,
      ordered_non_unique<tag<by_expiry>, BOOST_MULTI_INDEX_MEMBER(transaction_id_with_expiry, fc::time_point, expiry)>
   >
>;

struct by_height;

class pending_snapshot {
public:
   using next_t = producer_plugin::next_function<producer_plugin::snapshot_information>;

   pending_snapshot(const block_id_type& block_id, next_t& next, std::string pending_path, std::string final_path)
   : block_id(block_id)
   , next(next)
   , pending_path(pending_path)
   , final_path(final_path)
   {}

   uint32_t get_height() const {
      return block_header::num_from_id(block_id);
   }

   static bfs::path get_final_path(const block_id_type& block_id, const bfs::path& snapshots_dir) {
      return snapshots_dir / fc::format_string("snapshot-${id}.bin", fc::mutable_variant_object()("id", block_id));
   }

   static bfs::path get_pending_path(const block_id_type& block_id, const bfs::path& snapshots_dir) {
      return snapshots_dir / fc::format_string(".pending-snapshot-${id}.bin", fc::mutable_variant_object()("id", block_id));
   }

   static bfs::path get_temp_path(const block_id_type& block_id, const bfs::path& snapshots_dir) {
      return snapshots_dir / fc::format_string(".incomplete-snapshot-${id}.bin", fc::mutable_variant_object()("id", block_id));
   }

   producer_plugin::snapshot_information finalize( const chain::controller& chain ) const {
      auto in_chain = (bool)chain.fetch_block_by_id( block_id );
      boost::system::error_code ec;

      if (!in_chain) {
         bfs::remove(bfs::path(pending_path), ec);
         EOS_THROW(snapshot_finalization_exception,
                   "Snapshotted block was forked out of the chain.  ID: ${block_id}",
                   ("block_id", block_id));
      }

      bfs::rename(bfs::path(pending_path), bfs::path(final_path), ec);
      EOS_ASSERT(!ec, snapshot_finalization_exception,
                 "Unable to finalize valid snapshot of block number ${bn}: [code: ${ec}] ${message}",
                 ("bn", get_height())
                 ("ec", ec.value())
                 ("message", ec.message()));

      return {block_id, final_path};
   }

   block_id_type     block_id;
   next_t            next;
   std::string       pending_path;
   std::string       final_path;
};

using pending_snapshot_index = multi_index_container<
   pending_snapshot,
   indexed_by<
      hashed_unique<tag<by_id>, BOOST_MULTI_INDEX_MEMBER(pending_snapshot, block_id_type, block_id)>,
      ordered_non_unique<tag<by_height>, BOOST_MULTI_INDEX_CONST_MEM_FUN( pending_snapshot, uint32_t, get_height)>
   >
>;

enum class pending_block_mode {
   producing,
   speculating
};


class producer_plugin_impl : public std::enable_shared_from_this<producer_plugin_impl> {
   public:
      producer_plugin_impl(boost::asio::io_service& io)
      :_timer(io)
      ,_transaction_ack_channel(app().get_channel<compat::channels::transaction_ack>())
      {
      }

      optional<fc::time_point> calculate_next_block_time(const account_name& producer_name, const block_timestamp_type& current_block_time) const;
      void schedule_production_loop();
      void schedule_maybe_produce_block( bool exhausted ,bool is_backup);
      void produce_block(bool is_backup);
      bool maybe_produce_block(bool is_backup);
      bool block_is_exhausted() const;
      bool remove_expired_persisted_trxs( const fc::time_point& deadline );
      bool remove_expired_blacklisted_trxs( const fc::time_point& deadline );
      bool process_unapplied_trxs( const fc::time_point& deadline );
      void process_scheduled_and_incoming_trxs( const fc::time_point& deadline, size_t& pending_incoming_process_limit );
      bool process_incoming_trxs( const fc::time_point& deadline, size_t& pending_incoming_process_limit );

      boost::program_options::variables_map _options;
      bool     _production_enabled                 = false;
      bool     _pause_production                   = false;

      using signature_provider_type = std::function<chain::signature_type(chain::digest_type)>;
      std::map<chain::public_key_type, signature_provider_type> _signature_providers;
      std::set<chain::account_name>                             _producers;
      boost::asio::deadline_timer                               _timer;
      using producer_watermark = std::pair<uint32_t, block_timestamp_type>;
      std::map<chain::account_name, producer_watermark>         _producer_watermarks;
      pending_block_mode                                        _pending_block_mode = pending_block_mode::speculating;
      unapplied_transaction_queue                               _unapplied_transactions;
      fc::optional<named_thread_pool>                           _thread_pool;

      std::atomic<int32_t>                                      _max_transaction_time_ms; // modified by app thread, read by net_plugin thread pool
      fc::microseconds                                          _max_irreversible_block_age_us;
      int32_t                                                   _produce_time_offset_us = 0;
      int32_t                                                   _last_block_time_offset_us = 0;
      uint32_t                                                  _max_block_cpu_usage_threshold_us = 0;
      uint32_t                                                  _max_block_net_usage_threshold_bytes = 0;
      int32_t                                                   _max_scheduled_transaction_time_per_block_ms = 0;
      bool                                                      _disable_persist_until_expired = false;
      bool                                                      _disable_subjective_p2p_billing = true;
      bool                                                      _disable_subjective_api_billing = true;
      fc::time_point                                            _irreversible_block_time;
      fc::microseconds                                          _wallet_provider_timeout_us;

      std::vector<chain::digest_type>                           _protocol_features_to_activate;
      bool                                                      _protocol_features_signaled = false; // to mark whether it has been signaled in start_block

      chain_plugin* chain_plug = nullptr;

      incoming::channels::block::channel_type::handle         _incoming_block_subscription;
      incoming::channels::transaction::channel_type::handle   _incoming_transaction_subscription;

      compat::channels::transaction_ack::channel_type&        _transaction_ack_channel;

      incoming::methods::block_sync::method_type::handle        _incoming_block_sync_provider;
      incoming::methods::transaction_async::method_type::handle _incoming_transaction_async_provider;

      transaction_id_with_expiry_index                         _blacklisted_transactions;
      pending_snapshot_index                                   _pending_snapshot_index;
      subjective_billing                                       _subjective_billing;

      fc::optional<scoped_connection>                          _accepted_block_connection;
      fc::optional<scoped_connection>                          _accepted_block_header_connection;
      fc::optional<scoped_connection>                          _irreversible_block_connection;

      /*
       * HACK ALERT
       * Boost timers can be in a state where a handler has not yet executed but is not abortable.
       * As this method needs to mutate state handlers depend on for proper functioning to maintain
       * invariants for other code (namely accepting incoming transactions in a nearly full block)
       * the handlers capture a corelation ID at the time they are set.  When they are executed
       * they must check that correlation_id against the global ordinal.  If it does not match that
       * implies that this method has been called with the handler in the state where it should be
       * cancelled but wasn't able to be.
       */
      uint32_t _timer_corelation_id = 0;

      // keep a expected ratio between defer txn and incoming txn
      double _incoming_defer_ratio = 1.0; // 1:1

      // path to write the snapshots to
      bfs::path _snapshots_dir;

      void consider_new_watermark( account_name producer, uint32_t block_num, block_timestamp_type timestamp) {
         auto itr = _producer_watermarks.find( producer );
         if( itr != _producer_watermarks.end() ) {
            itr->second.first = std::max( itr->second.first, block_num );
            itr->second.second = std::max( itr->second.second, timestamp );
         } else if( _producers.count( producer ) > 0 ) {
            _producer_watermarks.emplace( producer, std::make_pair(block_num, timestamp) );
         }
      }

      std::optional<producer_watermark> get_watermark( account_name producer ) const {
         auto itr = _producer_watermarks.find( producer );

         if( itr == _producer_watermarks.end() ) return {};

         return itr->second;
      }

      void on_block( const block_state_ptr& bsp ) {
         _unapplied_transactions.clear_applied( bsp );
         _subjective_billing.on_block( bsp, fc::time_point::now() );
      }

      void on_block_header( const block_state_ptr& bsp ) {
         consider_new_watermark( bsp->header.producer, bsp->block_num, bsp->block->timestamp );
      }

      void on_irreversible_block( const signed_block_ptr& lib ) {
         _irreversible_block_time = lib->timestamp.to_time_point();
         const chain::controller& chain = chain_plug->chain();

         // promote any pending snapshots
         auto& snapshots_by_height = _pending_snapshot_index.get<by_height>();
         uint32_t lib_height = lib->block_num();

         while (!snapshots_by_height.empty() && snapshots_by_height.begin()->get_height() <= lib_height) {
            const auto& pending = snapshots_by_height.begin();
            auto next = pending->next;

            try {
               next(pending->finalize(chain));
            } CATCH_AND_CALL(next);

            snapshots_by_height.erase(snapshots_by_height.begin());
         }
      }

      void abort_block() {
         auto& chain = chain_plug->chain();

         _unapplied_transactions.add_aborted( chain.abort_block() );
         _subjective_billing.abort_block();
      }

      bool on_incoming_block(const signed_block_ptr& block, const std::optional<block_id_type>& block_id) {
         auto& chain = chain_plug->chain();
         #ifndef ACCEPT_BLOCK_IN_PRODUCING
            if ( _pending_block_mode == pending_block_mode::producing ) {
               fc_wlog( _log, "dropped incoming block #${num} id: ${id}",
                     ("num", block->block_num())("id", block_id ? (*block_id).str() : "UNKNOWN") );
               return false;
            }
         #endif
         
         const auto& id = block_id ? *block_id : block->id();
         auto blk_num = block->block_num();

         fc_dlog(_log, "received incoming block ${n} ${id}", ("n", blk_num)("id", id));

         EOS_ASSERT( block->timestamp < (fc::time_point::now() + fc::seconds( 7 )), block_from_the_future,
                     "received a block from the future, ignoring it: ${id}", ("id", id) );

         /* de-dupe here... no point in aborting block if we already know the block */
         auto existing = chain.fetch_block_by_id( id );
         if( existing ) { return false; }

         // start processing of block
         auto bsf = chain.create_block_state_future( block );

         // abort the pending block
         abort_block();

         // exceptions throw out, make sure we restart our loop
         auto ensure = fc::make_scoped_exit([this](){
            schedule_production_loop();
         });

         // push the new block
         try {
            chain.push_block( bsf, [this]( const branch_type& forked_branch ) {
               _unapplied_transactions.add_forked( forked_branch );
            }, [this]( const transaction_id_type& id ) {
               return _unapplied_transactions.get_trx( id );
            } );
         } catch ( const guard_exception& e ) {
            chain_plugin::handle_guard_exception(e);
            return false;
         } catch( const fc::exception& e ) {
            elog((e.to_detail_string()));
            app().get_channel<channels::rejected_block>().publish( priority::medium, block );
            throw;
         } catch ( const std::bad_alloc& ) {
            chain_plugin::handle_bad_alloc();
         } catch ( boost::interprocess::bad_alloc& ) {
            chain_plugin::handle_db_exhaustion();
         }

         const auto& hbs = chain.head_block_state();
         if( hbs->header.timestamp.next().to_time_point() >= fc::time_point::now() ) {
            _production_enabled = true;
         }

         if( fc::time_point::now() - block->timestamp < fc::minutes(5) || (blk_num % 1000 == 0) ) {
            ilog("Received block ${id}... #${n} @ ${t} signed by ${p} [trxs: ${count}, lib: ${lib}, conf: ${confs}, latency: ${latency} ms]",
                 ("p",block->producer)("id",id.str().substr(8,16))("n",blk_num)("t",block->timestamp)
                 ("count",block->transactions.size())("lib",chain.last_irreversible_block_num())
                 ("confs", block->confirmed)("latency", (fc::time_point::now() - block->timestamp).count()/1000 ) );
            if( chain.get_read_mode() != db_read_mode::IRREVERSIBLE && hbs->id != id && hbs->block != nullptr ) { // not applied to head
               ilog("Block not applied to head ${id}... #${n} @ ${t} signed by ${p} [trxs: ${count}, dpos: ${dpos}, conf: ${confs}, latency: ${latency} ms]",
                    ("p",hbs->block->producer)("id",hbs->id.str().substr(8,16))("n",hbs->block_num)("t",hbs->block->timestamp)
                    ("count",hbs->block->transactions.size())("dpos", hbs->dpos_irreversible_blocknum)
                    ("confs", hbs->block->confirmed)("latency", (fc::time_point::now() - hbs->block->timestamp).count()/1000 ) );
            }
         }
         /** 
         *@Description: reset backup verify mode to main
         */
         if(chain.is_backup_verify()){
            chain.set_verify_mode(false);
         }
         return true;
      }

      class incoming_transaction_queue {
         uint64_t max_incoming_transaction_queue_size = 0;
         uint64_t size_in_bytes = 0;
         std::deque<std::tuple<transaction_metadata_ptr, bool, next_function<transaction_trace_ptr>>> _incoming_transactions;

      private:
         static uint64_t calc_size( const transaction_metadata_ptr& trx ) {
            return trx->packed_trx()->get_unprunable_size() + trx->packed_trx()->get_prunable_size() + sizeof( *trx );
         }

         void add_size( const transaction_metadata_ptr& trx ) {
            auto size = calc_size( trx );
            EOS_ASSERT( size_in_bytes + size < max_incoming_transaction_queue_size, tx_resource_exhaustion, "Transaction exceeded producer resource limit" );
            size_in_bytes += size;
         }

      public:
         void set_max_incoming_transaction_queue_size( uint64_t v ) { max_incoming_transaction_queue_size = v; }

         void add( const transaction_metadata_ptr& trx, bool persist_until_expired, next_function<transaction_trace_ptr> next ) {
            add_size( trx );
            _incoming_transactions.emplace_back( trx, persist_until_expired, std::move( next ) );
         }

         void add_front( const transaction_metadata_ptr& trx, bool persist_until_expired, next_function<transaction_trace_ptr> next ) {
            add_size( trx );
            _incoming_transactions.emplace_front( trx, persist_until_expired, std::move( next ) );
         }

         auto pop_front() {
            EOS_ASSERT( !_incoming_transactions.empty(), producer_exception, "logic error, front() called on empty incoming_transactions" );
            auto intrx = _incoming_transactions.front();
            _incoming_transactions.pop_front();
            const transaction_metadata_ptr& trx = std::get<0>( intrx );
            size_in_bytes -= calc_size( trx );
            return intrx;
         }

         bool empty()const { return _incoming_transactions.empty(); }
         size_t size()const { return _incoming_transactions.size(); }
      };

      incoming_transaction_queue _pending_incoming_transactions;

      void on_incoming_transaction_async(const packed_transaction_ptr& trx, bool persist_until_expired, next_function<transaction_trace_ptr> next) {
         chain::controller& chain = chain_plug->chain();
         const auto max_trx_time_ms = _max_transaction_time_ms.load();
         fc::microseconds max_trx_cpu_usage = max_trx_time_ms < 0 ? fc::microseconds::maximum() : fc::milliseconds( max_trx_time_ms );

         auto future = transaction_metadata::start_recover_keys( trx, _thread_pool->get_executor(),
                chain.get_chain_id(), fc::microseconds( max_trx_cpu_usage ), chain.configured_subjective_signature_length_limit() );

         boost::asio::post(_thread_pool->get_executor(), [self = this, future{std::move(future)}, persist_until_expired,
                                                          next{std::move(next)}, trx,&chain]() mutable {
            if( future.valid() ) {
               future.wait();
               app().post( priority::low, [self, future{std::move(future)}, persist_until_expired, next{std::move( next )}, trx{std::move(trx)},&chain]() mutable {
                  auto exception_handler = [&next, trx{std::move(trx)}](fc::exception_ptr ex) {
                     fc_dlog(_trx_successful_trace_log, "[TRX_TRACE] Speculative execution is REJECTING tx: ${txid}, auth: ${a} : ${why} ",
                            ("txid", trx->id())("a",trx->get_transaction().first_authorizer())("why",ex->what()));
                     fc_dlog(_trx_failed_trace_log, "[TRX_TRACE] Speculative execution is REJECTING tx: ${txid}, auth: ${a} : ${why} ",
                            ("txid", trx->id())("a",trx->get_transaction().first_authorizer())("why",ex->what()));
                     next(ex);
                  };
                  try {
                     auto result = future.get();
                     if( !self->process_incoming_transaction_async( result, persist_until_expired, next ) ) {
                        if( self->_pending_block_mode == pending_block_mode::producing ) {
                           //only main block ?
                           self->schedule_maybe_produce_block( true , chain.is_backup_produce());
                        }
                     }
                  } CATCH_AND_CALL(exception_handler);
               } );
            }
         });
      }

      bool process_incoming_transaction_async(const transaction_metadata_ptr& trx, bool persist_until_expired, next_function<transaction_trace_ptr> next) {
         bool exhausted = false;
         chain::controller& chain = chain_plug->chain();

         auto send_response = [this, &trx, &chain, &next](const fc::static_variant<fc::exception_ptr, transaction_trace_ptr>& response) {
            next(response);
            if (response.contains<fc::exception_ptr>()) {
               _transaction_ack_channel.publish(priority::low, std::pair<fc::exception_ptr, transaction_metadata_ptr>(response.get<fc::exception_ptr>(), trx));
               if (_pending_block_mode == pending_block_mode::producing) {
                  fc_dlog(_trx_successful_trace_log, "[TRX_TRACE] Block ${block_num} for producer ${prod} is REJECTING tx: ${txid}, auth: ${a} : ${why} ",
                        ("block_num", chain.head_block_num() + 1)
                        ("prod", get_pending_block_producer())
                        ("txid", trx->id())
                        ("a", trx->packed_trx()->get_transaction().first_authorizer())
                        ("why",response.get<fc::exception_ptr>()->what()));
                  fc_dlog(_trx_failed_trace_log, "[TRX_TRACE] Block ${block_num} for producer ${prod} is REJECTING tx: ${txid}, auth: ${a} : ${why} ",
                        ("block_num", chain.head_block_num() + 1)
                        ("prod", get_pending_block_producer())
                        ("txid", trx->id())
                        ("a", trx->packed_trx()->get_transaction().first_authorizer())
                        ("why",response.get<fc::exception_ptr>()->what()));
               } else {
                  fc_dlog(_trx_successful_trace_log, "[TRX_TRACE] Speculative execution is REJECTING tx: ${txid}, auth: ${a} : ${why} ",
                          ("txid", trx->id())
                          ("a", trx->packed_trx()->get_transaction().first_authorizer())
                          ("why",response.get<fc::exception_ptr>()->what()));
                  fc_dlog(_trx_failed_trace_log, "[TRX_TRACE] Speculative execution is REJECTING tx: ${txid}, auth: ${a} : ${why} ",
                          ("txid", trx->id())
                          ("a", trx->packed_trx()->get_transaction().first_authorizer())
                          ("why",response.get<fc::exception_ptr>()->what()));
               }
            } else {
               _transaction_ack_channel.publish(priority::low, std::pair<fc::exception_ptr, transaction_metadata_ptr>(nullptr, trx));
               if (_pending_block_mode == pending_block_mode::producing) {
                  fc_dlog(_trx_successful_trace_log, "[TRX_TRACE] Block ${block_num} for producer ${prod} is ACCEPTING tx: ${txid}, auth: ${a}",
                          ("block_num", chain.head_block_num() + 1)
                          ("prod", get_pending_block_producer())
                          ("txid", trx->id())
                          ("a", trx->packed_trx()->get_transaction().first_authorizer()));
               } else {
                  fc_dlog(_trx_successful_trace_log, "[TRX_TRACE] Speculative execution is ACCEPTING tx: ${txid}, auth: ${a}",
                          ("txid", trx->id())
                          ("a", trx->packed_trx()->get_transaction().first_authorizer()));
               }
            }
         };

         try {
            const auto& id = trx->id();

            fc::time_point bt = chain.is_building_block() ? chain.pending_block_time() : chain.head_block_time();
            const fc::time_point expire = trx->packed_trx()->expiration();
            if( expire < bt ) {
               send_response( std::static_pointer_cast<fc::exception>(
                     std::make_shared<expired_tx_exception>(
                           FC_LOG_MESSAGE( error, "expired transaction ${id}, expiration ${e}, block time ${bt}",
                                           ("id", id)("e", expire)( "bt", bt )))));
               return true;
            }

            if( chain.is_known_unexpired_transaction( id )) {
               send_response( std::static_pointer_cast<fc::exception>( std::make_shared<tx_duplicate>(
                     FC_LOG_MESSAGE( error, "duplicate transaction ${id}", ("id", id)))) );
               return true;
            }

            if( !chain.is_building_block()) {
               _pending_incoming_transactions.add( trx, persist_until_expired, next );
               return true;
            }

            auto deadline = fc::time_point::now() + fc::milliseconds( _max_transaction_time_ms );
            bool deadline_is_subjective = false;
            const auto block_deadline = calculate_block_deadline( chain.pending_block_time() );
            if( _max_transaction_time_ms < 0 ||
                (_pending_block_mode == pending_block_mode::producing && block_deadline < deadline)) {
               deadline_is_subjective = true;
               deadline = block_deadline;
            }

            bool disable_subjective_billing = ( _pending_block_mode == pending_block_mode::producing )
                                              || ( persist_until_expired && _disable_subjective_api_billing )
                                              || ( !persist_until_expired && _disable_subjective_p2p_billing );

            auto first_auth = trx->packed_trx()->get_transaction().first_authorizer();
            uint32_t sub_bill = 0;
            if( !disable_subjective_billing )
               sub_bill = _subjective_billing.get_subjective_bill( first_auth, fc::time_point::now() );

            auto trace = chain.push_transaction( trx, deadline, trx->billed_cpu_time_us, false, sub_bill );
            fc_dlog( _trx_failed_trace_log, "Subjective bill for ${a}: ${b} elapsed ${t}us", ("a",first_auth)("b",sub_bill)("t",trace->elapsed));
            if( trace->except ) {
               if( exception_is_exhausted( *trace->except, deadline_is_subjective )) {
                  _pending_incoming_transactions.add( trx, persist_until_expired, next );
                  if( _pending_block_mode == pending_block_mode::producing ) {
                     fc_dlog(_log, "[TRX_TRACE] Block ${block_num} for producer ${prod} COULD NOT FIT, tx: ${txid} RETRYING ",
                              ("block_num", chain.head_block_num() + 1)
                              ("prod", get_pending_block_producer())
                              ("txid", trx->id()));
                  } else {
                     fc_dlog(_log, "[TRX_TRACE] Speculative execution COULD NOT FIT tx: ${txid} RETRYING",
                              ("txid", trx->id()));
                  }
                  exhausted = block_is_exhausted();
               } else {
                  _subjective_billing.subjective_bill_failure( first_auth, trace->elapsed, fc::time_point::now() );
                  auto e_ptr = trace->except->dynamic_copy_exception();
                  send_response( e_ptr );
               }
            } else {
               if( persist_until_expired && !_disable_persist_until_expired ) {
                  // if this trx didnt fail/soft-fail and the persist flag is set, store its ID so that we can
                  // ensure its applied to all future speculative blocks as well.
                  // No need to subjective bill since it will be re-applied
                  _unapplied_transactions.add_persisted( trx );
               } else {
                  // if db_read_mode SPECULATIVE then trx is in the pending block and not immediately reverted
                  _subjective_billing.subjective_bill( trx->id(), expire, first_auth, trace->elapsed,
                                                       chain.get_read_mode() == chain::db_read_mode::SPECULATIVE );
               }
               send_response( trace );
            }

         } catch ( const guard_exception& e ) {
            chain_plugin::handle_guard_exception(e);
         } catch ( boost::interprocess::bad_alloc& ) {
            chain_plugin::handle_db_exhaustion();
         } catch ( std::bad_alloc& ) {
            chain_plugin::handle_bad_alloc();
         } CATCH_AND_CALL(send_response);

         return !exhausted;
      }


      fc::microseconds get_irreversible_block_age() {
         auto now = fc::time_point::now();
         if (now < _irreversible_block_time) {
            return fc::microseconds(0);
         } else {
            return now - _irreversible_block_time;
         }
      }

      account_name get_pending_block_producer() {
         auto& chain = chain_plug->chain();
         if (chain.is_building_block()) {
            return chain.pending_block_producer();
         } else {
            return {};
         }
      }

      bool production_disabled_by_policy() {
         return !_production_enabled || _pause_production || (_max_irreversible_block_age_us.count() >= 0 && get_irreversible_block_age() >= _max_irreversible_block_age_us);
      }

      enum class start_block_result {
         succeeded,
         failed,
         waiting_for_block,
         waiting_for_production,
         exhausted
      };

      start_block_result start_block(bool& is_backup);

      fc::time_point calculate_pending_block_time() const;
      fc::time_point calculate_block_deadline( const fc::time_point& ) const;
      void schedule_delayed_production_loop(const std::weak_ptr<producer_plugin_impl>& weak_this, optional<fc::time_point> wake_up_time);
      optional<fc::time_point> calculate_producer_wake_up_time( const block_timestamp_type& ref_block_time ) const;

};

void new_chain_banner(const eosio::chain::controller& db)
{
   std::cerr << "\n"
      "*******************************\n"
      "*                             *\n"
      "*   ------ NEW CHAIN ------   *\n"
      "*   -  Welcome to AMAX!  -   *\n"
      "*   -----------------------   *\n"
      "*                             *\n"
      "*******************************\n"
      "\n";

   if( db.head_block_state()->header.timestamp.to_time_point() < (fc::time_point::now() - fc::milliseconds(200 * config::block_interval_ms)))
   {
      std::cerr << "Your genesis seems to have an old timestamp\n"
         "Please consider using the --genesis-timestamp option to give your genesis a recent timestamp\n"
         "\n"
         ;
   }
   return;
}

producer_plugin::producer_plugin()
   : my(new producer_plugin_impl(app().get_io_service())){
   }

producer_plugin::~producer_plugin() {}

void producer_plugin::set_program_options(
   boost::program_options::options_description& command_line_options,
   boost::program_options::options_description& config_file_options)
{
   auto default_priv_key = private_key_type::regenerate<fc::ecc::private_key_shim>(fc::sha256::hash(std::string("nathan")));
   auto private_key_default = std::make_pair(default_priv_key.get_public_key(), default_priv_key );

   boost::program_options::options_description producer_options;

   producer_options.add_options()
         ("enable-stale-production,e", boost::program_options::bool_switch()->notifier([this](bool e){my->_production_enabled = e;}), "Enable block production, even if the chain is stale.")
         ("pause-on-startup,x", boost::program_options::bool_switch()->notifier([this](bool p){my->_pause_production = p;}), "Start this node in a state where production is paused")
         ("max-transaction-time", bpo::value<int32_t>()->default_value(30),
          "Limits the maximum time (in milliseconds) that is allowed a pushed transaction's code to execute before being considered invalid")
         ("max-irreversible-block-age", bpo::value<int32_t>()->default_value( -1 ),
          "Limits the maximum age (in seconds) of the DPOS Irreversible Block for a chain this node will produce blocks on (use negative value to indicate unlimited)")
         ("producer-name,p", boost::program_options::value<vector<string>>()->composing()->multitoken(),
          "ID of producer controlled by this node (e.g. inita; may specify multiple times)")
         ("private-key", boost::program_options::value<vector<string>>()->composing()->multitoken(),
          "(DEPRECATED - Use signature-provider instead) Tuple of [public key, WIF private key] (may specify multiple times)")
         ("signature-provider", boost::program_options::value<vector<string>>()->composing()->multitoken()->default_value(
               {default_priv_key.get_public_key().to_string() + "=KEY:" + default_priv_key.to_string()},
                default_priv_key.get_public_key().to_string() + "=KEY:" + default_priv_key.to_string()),
          "Key=Value pairs in the form <public-key>=<provider-spec>\n"
          "Where:\n"
          "   <public-key>    \tis a string form of a vaild AMAX public key\n\n"
          "   <provider-spec> \tis a string in the form <provider-type>:<data>\n\n"
          "   <provider-type> \tis KEY, or WALLET\n\n"
          "   KEY:<data>      \tis a string form of a valid AMAX private key which maps to the provided public key\n\n"
          "   WALLET:<data>    \tis the URL where wallet is available and the approptiate wallet(s) are unlocked")
         ("wallet-provider-timeout", boost::program_options::value<int32_t>()->default_value(5),
          "Limits the maximum time (in milliseconds) that is allowed for sending blocks to a wallet provider for signing")
         ("greylist-account", boost::program_options::value<vector<string>>()->composing()->multitoken(),
          "account that can not access to extended CPU/NET virtual resources")
         ("greylist-limit", boost::program_options::value<uint32_t>()->default_value(1000),
          "Limit (between 1 and 1000) on the multiple that CPU/NET virtual resources can extend during low usage (only enforced subjectively; use 1000 to not enforce any limit)")
         ("produce-time-offset-us", boost::program_options::value<int32_t>()->default_value(0),
          "Offset of non last block producing time in microseconds. Valid range 0 .. -block_time_interval.")
         ("last-block-time-offset-us", boost::program_options::value<int32_t>()->default_value(-200000),
          "Offset of last block producing time in microseconds. Valid range 0 .. -block_time_interval.")
         ("cpu-effort-percent", bpo::value<uint32_t>()->default_value(config::default_block_cpu_effort_pct / config::percent_1),
          "Percentage of cpu block production time used to produce block. Whole number percentages, e.g. 80 for 80%")
         ("last-block-cpu-effort-percent", bpo::value<uint32_t>()->default_value(config::default_block_cpu_effort_pct / config::percent_1),
          "Percentage of cpu block production time used to produce last block. Whole number percentages, e.g. 80 for 80%")
         ("max-block-cpu-usage-threshold-us", bpo::value<uint32_t>()->default_value( 5000 ),
          "Threshold of CPU block production to consider block full; when within threshold of max-block-cpu-usage block can be produced immediately")
         ("max-block-net-usage-threshold-bytes", bpo::value<uint32_t>()->default_value( 1024 ),
          "Threshold of NET block production to consider block full; when within threshold of max-block-net-usage block can be produced immediately")
         ("max-scheduled-transaction-time-per-block-ms", boost::program_options::value<int32_t>()->default_value(100),
          "Maximum wall-clock time, in milliseconds, spent retiring scheduled transactions in any block before returning to normal transaction processing.")
         ("subjective-cpu-leeway-us", boost::program_options::value<int32_t>()->default_value( config::default_subjective_cpu_leeway_us ),
          "Time in microseconds allowed for a transaction that starts with insufficient CPU quota to complete and cover its CPU usage.")
         ("incoming-defer-ratio", bpo::value<double>()->default_value(1.0),
          "ratio between incoming transactions and deferred transactions when both are queued for execution")
         ("incoming-transaction-queue-size-mb", bpo::value<uint16_t>()->default_value( 1024 ),
          "Maximum size (in MiB) of the incoming transaction queue. Exceeding this value will subjectively drop transaction with resource exhaustion.")
         ("disable-api-persisted-trx", bpo::bool_switch()->default_value(false),
          "Disable the re-apply of API transactions.")
         ("disable-subjective-billing", bpo::value<bool>()->default_value(true),
          "Disable subjective CPU billing for API/P2P transactions")
         ("disable-subjective-account-billing", boost::program_options::value<vector<string>>()->composing()->multitoken(),
          "Account which is excluded from subjective CPU billing")
         ("disable-subjective-p2p-billing", bpo::value<bool>()->default_value(true),
          "Disable subjective CPU billing for P2P transactions")
         ("disable-subjective-api-billing", bpo::value<bool>()->default_value(true),
          "Disable subjective CPU billing for API transactions")
         ("producer-threads", bpo::value<uint16_t>()->default_value(config::default_controller_thread_pool_size),
          "Number of worker threads in producer thread pool")
         ("snapshots-dir", bpo::value<bfs::path>()->default_value("snapshots"),
          "the location of the snapshots directory (absolute path or relative to application data dir)")
         ;
   config_file_options.add(producer_options);
}

bool producer_plugin::is_producer_key(const chain::public_key_type& key) const
{
  auto private_key_itr = my->_signature_providers.find(key);
  if(private_key_itr != my->_signature_providers.end())
    return true;
  return false;
}

uint32_t producer_plugin::get_subjective_bill( const account_name& first_auth, const fc::time_point& now ) const
{
   return my->_subjective_billing.get_subjective_bill( first_auth, now );
}

chain::signature_type producer_plugin::sign_compact(const chain::public_key_type& key, const fc::sha256& digest) const
{
  if(key != chain::public_key_type()) {
    auto private_key_itr = my->_signature_providers.find(key);
    EOS_ASSERT(private_key_itr != my->_signature_providers.end(), producer_priv_key_not_found, "Local producer has no private key in config.ini corresponding to public key ${key}", ("key", key));

    return private_key_itr->second(digest);
  }
  else {
    return chain::signature_type();
  }
}

template<typename T>
T dejsonify(const string& s) {
   return fc::json::from_string(s).as<T>();
}

#define LOAD_VALUE_SET(options, op_name, container) \
if( options.count(op_name) ) { \
   const std::vector<std::string>& ops = options[op_name].as<std::vector<std::string>>(); \
   for( const auto& v : ops ) { \
      container.emplace( eosio::chain::name( v ) ); \
   } \
}

static producer_plugin_impl::signature_provider_type
make_key_signature_provider(const private_key_type& key) {
   return [key]( const chain::digest_type& digest ) {
      return key.sign(digest);
   };
}

static producer_plugin_impl::signature_provider_type
make_wallet_signature_provider(const std::shared_ptr<producer_plugin_impl>& impl, const string& url_str, const public_key_type pubkey) {
   fc::url wallet_url;
   if(boost::algorithm::starts_with(url_str, "unix://"))
      //send the entire string after unix:// to http_plugin. It'll auto-detect which part
      // is the unix socket path, and which part is the url to hit on the server
      wallet_url = fc::url("unix", url_str.substr(7), ostring(), ostring(), ostring(), ostring(), ovariant_object(), fc::optional<uint16_t>());
   else
      wallet_url = fc::url(url_str);
   std::weak_ptr<producer_plugin_impl> weak_impl = impl;

   return [weak_impl, wallet_url, pubkey]( const chain::digest_type& digest ) {
      auto impl = weak_impl.lock();
      if (impl) {
         fc::variant params;
         fc::to_variant(std::make_pair(digest, pubkey), params);
         auto deadline = impl->_wallet_provider_timeout_us.count() >= 0 ? fc::time_point::now() + impl->_wallet_provider_timeout_us : fc::time_point::maximum();
         return app().get_plugin<http_client_plugin>().get_client().post_sync(wallet_url, params, deadline).as<chain::signature_type>();
      } else {
         return signature_type();
      }
   };
}

void producer_plugin::plugin_initialize(const boost::program_options::variables_map& options)
{ try {
   my->chain_plug = app().find_plugin<chain_plugin>();
   EOS_ASSERT( my->chain_plug, plugin_config_exception, "chain_plugin not found" );
   my->_options = &options;
   LOAD_VALUE_SET(options, "producer-name", my->_producers)

   chain::controller& chain = my->chain_plug->chain();
   unapplied_transaction_queue::process_mode unapplied_mode =
      (chain.get_read_mode() != chain::db_read_mode::SPECULATIVE) ? unapplied_transaction_queue::process_mode::non_speculative :
         my->_producers.empty() ? unapplied_transaction_queue::process_mode::speculative_non_producer :
            unapplied_transaction_queue::process_mode::speculative_producer;
   my->_unapplied_transactions.set_mode( unapplied_mode );

   if( options.count("private-key") )
   {
      const std::vector<std::string> key_id_to_wif_pair_strings = options["private-key"].as<std::vector<std::string>>();
      for (const std::string& key_id_to_wif_pair_string : key_id_to_wif_pair_strings)
      {
         try {
            auto key_id_to_wif_pair = dejsonify<std::pair<public_key_type, private_key_type>>(key_id_to_wif_pair_string);
            my->_signature_providers[key_id_to_wif_pair.first] = make_key_signature_provider(key_id_to_wif_pair.second);
            auto blanked_privkey = std::string(key_id_to_wif_pair.second.to_string().size(), '*' );
            wlog("\"private-key\" is DEPRECATED, use \"signature-provider=${pub}=KEY:${priv}\"", ("pub",key_id_to_wif_pair.first)("priv", blanked_privkey));
         } catch ( fc::exception& e ) {
            elog("Malformed private key pair");
         }
      }
   }

   if( options.count("signature-provider") ) {
      const std::vector<std::string> key_spec_pairs = options["signature-provider"].as<std::vector<std::string>>();
      for (const auto& key_spec_pair : key_spec_pairs) {
         try {
            auto delim = key_spec_pair.find("=");
            EOS_ASSERT(delim != std::string::npos, plugin_config_exception, "Missing \"=\" in the key spec pair");
            auto pub_key_str = key_spec_pair.substr(0, delim);
            auto spec_str = key_spec_pair.substr(delim + 1);

            auto spec_delim = spec_str.find(":");
            EOS_ASSERT(spec_delim != std::string::npos, plugin_config_exception, "Missing \":\" in the key spec pair");
            auto spec_type_str = spec_str.substr(0, spec_delim);
            auto spec_data = spec_str.substr(spec_delim + 1);

            auto pubkey = public_key_type(pub_key_str);

            if (spec_type_str == "KEY") {
               my->_signature_providers[pubkey] = make_key_signature_provider(private_key_type(spec_data));
            } else if (spec_type_str == "WALLET") {
               my->_signature_providers[pubkey] = make_wallet_signature_provider(my, spec_data, pubkey);
            }

         } catch (...) {
            elog("Malformed signature provider: \"${val}\", ignoring!", ("val", key_spec_pair));
         }
      }
   }

   my->_wallet_provider_timeout_us = fc::milliseconds(options.at("wallet-provider-timeout").as<int32_t>());

   my->_produce_time_offset_us = options.at("produce-time-offset-us").as<int32_t>();
   EOS_ASSERT( my->_produce_time_offset_us <= 0 && my->_produce_time_offset_us >= -config::block_interval_us, plugin_config_exception,
               "produce-time-offset-us ${o} must be 0 .. -${bi}", ("bi", config::block_interval_us)("o", my->_produce_time_offset_us) );

   my->_last_block_time_offset_us = options.at("last-block-time-offset-us").as<int32_t>();
   EOS_ASSERT( my->_last_block_time_offset_us <= 0 && my->_last_block_time_offset_us >= -config::block_interval_us, plugin_config_exception,
               "last-block-time-offset-us ${o} must be 0 .. -${bi}", ("bi", config::block_interval_us)("o", my->_last_block_time_offset_us) );

   uint32_t cpu_effort_pct = options.at("cpu-effort-percent").as<uint32_t>();
   EOS_ASSERT( cpu_effort_pct >= 0 && cpu_effort_pct <= 100, plugin_config_exception,
               "cpu-effort-percent ${pct} must be 0 - 100", ("pct", cpu_effort_pct) );
      cpu_effort_pct *= config::percent_1;
   int32_t cpu_effort_offset_us =
         -EOS_PERCENT( config::block_interval_us, chain::config::percent_100 - cpu_effort_pct );

   uint32_t last_block_cpu_effort_pct = options.at("last-block-cpu-effort-percent").as<uint32_t>();
   EOS_ASSERT( last_block_cpu_effort_pct >= 0 && last_block_cpu_effort_pct <= 100, plugin_config_exception,
               "last-block-cpu-effort-percent ${pct} must be 0 - 100", ("pct", last_block_cpu_effort_pct) );
      last_block_cpu_effort_pct *= config::percent_1;
   int32_t last_block_cpu_effort_offset_us =
         -EOS_PERCENT( config::block_interval_us, chain::config::percent_100 - last_block_cpu_effort_pct );

   my->_produce_time_offset_us = std::min( my->_produce_time_offset_us, cpu_effort_offset_us );
   my->_last_block_time_offset_us = std::min( my->_last_block_time_offset_us, last_block_cpu_effort_offset_us );

   my->_max_block_cpu_usage_threshold_us = options.at( "max-block-cpu-usage-threshold-us" ).as<uint32_t>();
   EOS_ASSERT( my->_max_block_cpu_usage_threshold_us < config::block_interval_us, plugin_config_exception,
               "max-block-cpu-usage-threshold-us ${t} must be 0 .. ${bi}", ("bi", config::block_interval_us)("t", my->_max_block_cpu_usage_threshold_us) );

   my->_max_block_net_usage_threshold_bytes = options.at( "max-block-net-usage-threshold-bytes" ).as<uint32_t>();

   my->_max_scheduled_transaction_time_per_block_ms = options.at("max-scheduled-transaction-time-per-block-ms").as<int32_t>();

   if( options.at( "subjective-cpu-leeway-us" ).as<int32_t>() != config::default_subjective_cpu_leeway_us ) {
      chain.set_subjective_cpu_leeway( fc::microseconds( options.at( "subjective-cpu-leeway-us" ).as<int32_t>() ) );
   }

   my->_max_transaction_time_ms = options.at("max-transaction-time").as<int32_t>();

   my->_max_irreversible_block_age_us = fc::seconds(options.at("max-irreversible-block-age").as<int32_t>());

   auto max_incoming_transaction_queue_size = options.at("incoming-transaction-queue-size-mb").as<uint16_t>() * 1024*1024;

   EOS_ASSERT( max_incoming_transaction_queue_size > 0, plugin_config_exception,
               "incoming-transaction-queue-size-mb ${mb} must be greater than 0", ("mb", max_incoming_transaction_queue_size) );

   my->_pending_incoming_transactions.set_max_incoming_transaction_queue_size( max_incoming_transaction_queue_size );

   my->_incoming_defer_ratio = options.at("incoming-defer-ratio").as<double>();

   my->_disable_persist_until_expired = options.at("disable-api-persisted-trx").as<bool>();
   bool disable_subjective_billing = options.at("disable-subjective-billing").as<bool>();
   my->_disable_subjective_p2p_billing = options.at("disable-subjective-p2p-billing").as<bool>();
   my->_disable_subjective_api_billing = options.at("disable-subjective-api-billing").as<bool>();
   dlog( "disable-subjective-billing: ${s}, disable-subjective-p2p-billing: ${p2p}, disable-subjective-api-billing: ${api}",
         ("s", disable_subjective_billing)("p2p", my->_disable_subjective_p2p_billing)("api", my->_disable_subjective_api_billing) );
   if( !disable_subjective_billing ) {
       my->_disable_subjective_p2p_billing = my->_disable_subjective_api_billing = false;
   } else if( !my->_disable_subjective_p2p_billing || !my->_disable_subjective_api_billing ) {
       disable_subjective_billing = false;
   }
   if( disable_subjective_billing ) {
       my->_subjective_billing.disable();
       ilog( "Subjective CPU billing disabled" );
   } else if( !my->_disable_subjective_p2p_billing && !my->_disable_subjective_api_billing ) {
       ilog( "Subjective CPU billing enabled" );
   } else {
       if( my->_disable_subjective_p2p_billing ) ilog( "Subjective CPU billing of P2P trxs disabled " );
       if( my->_disable_subjective_api_billing ) ilog( "Subjective CPU billing of API trxs disabled " );
   }

   auto thread_pool_size = options.at( "producer-threads" ).as<uint16_t>();
   EOS_ASSERT( thread_pool_size > 0, plugin_config_exception,
               "producer-threads ${num} must be greater than 0", ("num", thread_pool_size));
   my->_thread_pool.emplace( "prod", thread_pool_size );

   if( options.count( "snapshots-dir" )) {
      auto sd = options.at( "snapshots-dir" ).as<bfs::path>();
      if( sd.is_relative()) {
         my->_snapshots_dir = app().data_dir() / sd;
         if (!fc::exists(my->_snapshots_dir)) {
            fc::create_directories(my->_snapshots_dir);
         }
      } else {
         my->_snapshots_dir = sd;
      }

      EOS_ASSERT( fc::is_directory(my->_snapshots_dir), snapshot_directory_not_found_exception,
                  "No such directory '${dir}'", ("dir", my->_snapshots_dir.generic_string()) );
   }

   my->_incoming_block_subscription = app().get_channel<incoming::channels::block>().subscribe(
         [this](const signed_block_ptr& block) {
      try {
         my->on_incoming_block(block, {});
      } LOG_AND_DROP();
   });

   my->_incoming_transaction_subscription = app().get_channel<incoming::channels::transaction>().subscribe(
         [this](const packed_transaction_ptr& trx) {
      try {
         my->on_incoming_transaction_async(trx, false, [](const auto&){});
      } LOG_AND_DROP();
   });

   my->_incoming_block_sync_provider = app().get_method<incoming::methods::block_sync>().register_provider(
         [this](const signed_block_ptr& block, const std::optional<block_id_type>& block_id) {
      return my->on_incoming_block(block, block_id);
   });

   my->_incoming_transaction_async_provider = app().get_method<incoming::methods::transaction_async>().register_provider(
         [this](const packed_transaction_ptr& trx, bool persist_until_expired, next_function<transaction_trace_ptr> next) -> void {
      return my->on_incoming_transaction_async(trx, persist_until_expired, next );
   });

   if (options.count("greylist-account")) {
      std::vector<std::string> greylist = options["greylist-account"].as<std::vector<std::string>>();
      greylist_params param;
      for (auto &a : greylist) {
         param.accounts.push_back(account_name(a));
      }
      add_greylist_accounts(param);
   }

   {
      uint32_t greylist_limit = options.at("greylist-limit").as<uint32_t>();
      chain.set_greylist_limit( greylist_limit );
   }

   if( options.count("disable-subjective-account-billing") ) {
      std::vector<std::string> accounts = options["disable-subjective-account-billing"].as<std::vector<std::string>>();
      for( const auto& a : accounts ) {
         my->_subjective_billing.disable_account( account_name(a) );
      }
   }

} FC_LOG_AND_RETHROW() }

void producer_plugin::plugin_startup()
{ try {
   handle_sighup(); // Sets loggers

   try {
   ilog("producer plugin:  plugin_startup() begin");

   chain::controller& chain = my->chain_plug->chain();
   EOS_ASSERT( my->_producers.empty() || chain.get_read_mode() == chain::db_read_mode::SPECULATIVE, plugin_config_exception,
              "node cannot have any producer-name configured because block production is impossible when read_mode is not \"speculative\"" );

   EOS_ASSERT( my->_producers.empty() || chain.get_validation_mode() == chain::validation_mode::FULL, plugin_config_exception,
              "node cannot have any producer-name configured because block production is not safe when validation_mode is not \"full\"" );

   EOS_ASSERT( my->_producers.empty() || my->chain_plug->accept_transactions(), plugin_config_exception,
              "node cannot have any producer-name configured because no block production is possible with no [api|p2p]-accepted-transactions" );

   my->_accepted_block_connection.emplace(chain.accepted_block.connect( [this]( const auto& bsp ){ my->on_block( bsp ); } ));
   my->_accepted_block_header_connection.emplace(chain.accepted_block_header.connect( [this]( const auto& bsp ){ my->on_block_header( bsp ); } ));
   my->_irreversible_block_connection.emplace(chain.irreversible_block.connect( [this]( const auto& bsp ){ my->on_irreversible_block( bsp->block ); } ));

   const auto lib_num = chain.last_irreversible_block_num();
   const auto lib = chain.fetch_block_by_number(lib_num);
   if (lib) {
      my->on_irreversible_block(lib);
   } else {
      my->_irreversible_block_time = fc::time_point::maximum();
   }

   if (!my->_producers.empty()) {
      ilog("Launching block production for ${n} producers at ${time}.", ("n", my->_producers.size())("time",fc::time_point::now()));

      if (my->_production_enabled) {
         if (chain.head_block_num() == 0) {
            new_chain_banner(chain);
         }
      }
   }

   my->schedule_production_loop();

   ilog("producer plugin:  plugin_startup() end");
   } catch( ... ) {
      // always call plugin_shutdown, even on exception
      plugin_shutdown();
      throw;
   }
} FC_CAPTURE_AND_RETHROW() }

void producer_plugin::plugin_shutdown() {
   try {
      my->_timer.cancel();
   } catch(fc::exception& e) {
      edump((e.to_detail_string()));
   }

   if( my->_thread_pool ) {
      my->_thread_pool->stop();
   }

   app().post( 0, [me = my](){} ); // keep my pointer alive until queue is drained
}

void producer_plugin::handle_sighup() {
   fc::logger::update( logger_name, _log );
   fc::logger::update(trx_successful_trace_logger_name, _trx_successful_trace_log);
   fc::logger::update(trx_failed_trace_logger_name, _trx_failed_trace_log);
}

void producer_plugin::pause() {
   fc_ilog(_log, "Producer paused.");
   my->_pause_production = true;
}

void producer_plugin::resume() {
   my->_pause_production = false;
   // it is possible that we are only speculating because of this policy which we have now changed
   // re-evaluate that now
   //
   if (my->_pending_block_mode == pending_block_mode::speculating) {
      my->abort_block();
      fc_ilog(_log, "Producer resumed. Scheduling production.");
      my->schedule_production_loop();
   } else {
      fc_ilog(_log, "Producer resumed.");
   }
}

bool producer_plugin::paused() const {
   return my->_pause_production;
}

void producer_plugin::update_runtime_options(const runtime_options& options) {
   chain::controller& chain = my->chain_plug->chain();
   bool check_speculating = false;

   if (options.max_transaction_time) {
      my->_max_transaction_time_ms = *options.max_transaction_time;
   }

   if (options.max_irreversible_block_age) {
      my->_max_irreversible_block_age_us =  fc::seconds(*options.max_irreversible_block_age);
      check_speculating = true;
   }

   if (options.produce_time_offset_us) {
      my->_produce_time_offset_us = *options.produce_time_offset_us;
   }

   if (options.last_block_time_offset_us) {
      my->_last_block_time_offset_us = *options.last_block_time_offset_us;
   }

   if (options.max_scheduled_transaction_time_per_block_ms) {
      my->_max_scheduled_transaction_time_per_block_ms = *options.max_scheduled_transaction_time_per_block_ms;
   }

   if (options.incoming_defer_ratio) {
      my->_incoming_defer_ratio = *options.incoming_defer_ratio;
   }

   if (check_speculating && my->_pending_block_mode == pending_block_mode::speculating) {
      my->abort_block();
      my->schedule_production_loop();
   }

   if (options.subjective_cpu_leeway_us) {
      chain.set_subjective_cpu_leeway(fc::microseconds(*options.subjective_cpu_leeway_us));
   }

   if (options.greylist_limit) {
      chain.set_greylist_limit(*options.greylist_limit);
   }
}

producer_plugin::runtime_options producer_plugin::get_runtime_options() const {
   return {
      my->_max_transaction_time_ms,
      my->_max_irreversible_block_age_us.count() < 0 ? -1 : my->_max_irreversible_block_age_us.count() / 1'000'000,
      my->_produce_time_offset_us,
      my->_last_block_time_offset_us,
      my->_max_scheduled_transaction_time_per_block_ms,
      my->chain_plug->chain().get_subjective_cpu_leeway() ?
            my->chain_plug->chain().get_subjective_cpu_leeway()->count() :
            fc::optional<int32_t>(),
      my->_incoming_defer_ratio,
      my->chain_plug->chain().get_greylist_limit()
   };
}

void producer_plugin::add_greylist_accounts(const greylist_params& params) {
   chain::controller& chain = my->chain_plug->chain();
   for (auto &acc : params.accounts) {
      chain.add_resource_greylist(acc);
   }
}

void producer_plugin::remove_greylist_accounts(const greylist_params& params) {
   chain::controller& chain = my->chain_plug->chain();
   for (auto &acc : params.accounts) {
      chain.remove_resource_greylist(acc);
   }
}

producer_plugin::greylist_params producer_plugin::get_greylist() const {
   chain::controller& chain = my->chain_plug->chain();
   greylist_params result;
   const auto& list = chain.get_resource_greylist();
   result.accounts.reserve(list.size());
   for (auto &acc: list) {
      result.accounts.push_back(acc);
   }
   return result;
}

producer_plugin::whitelist_blacklist producer_plugin::get_whitelist_blacklist() const {
   chain::controller& chain = my->chain_plug->chain();
   return {
      chain.get_actor_whitelist(),
      chain.get_actor_blacklist(),
      chain.get_contract_whitelist(),
      chain.get_contract_blacklist(),
      chain.get_action_blacklist(),
      chain.get_key_blacklist()
   };
}

void producer_plugin::set_whitelist_blacklist(const producer_plugin::whitelist_blacklist& params) {
   chain::controller& chain = my->chain_plug->chain();
   if(params.actor_whitelist.valid()) chain.set_actor_whitelist(*params.actor_whitelist);
   if(params.actor_blacklist.valid()) chain.set_actor_blacklist(*params.actor_blacklist);
   if(params.contract_whitelist.valid()) chain.set_contract_whitelist(*params.contract_whitelist);
   if(params.contract_blacklist.valid()) chain.set_contract_blacklist(*params.contract_blacklist);
   if(params.action_blacklist.valid()) chain.set_action_blacklist(*params.action_blacklist);
   if(params.key_blacklist.valid()) chain.set_key_blacklist(*params.key_blacklist);
}

producer_plugin::integrity_hash_information producer_plugin::get_integrity_hash() const {
   chain::controller& chain = my->chain_plug->chain();

   auto reschedule = fc::make_scoped_exit([this](){
      my->schedule_production_loop();
   });

   if (chain.is_building_block()) {
      // abort the pending block
      my->abort_block();
   } else {
      reschedule.cancel();
   }

   return {chain.head_block_id(), chain.calculate_integrity_hash()};
}

void producer_plugin::create_snapshot(producer_plugin::next_function<producer_plugin::snapshot_information> next) {
   chain::controller& chain = my->chain_plug->chain();

   auto head_id = chain.head_block_id();
   const auto& snapshot_path = pending_snapshot::get_final_path(head_id, my->_snapshots_dir);
   const auto& temp_path     = pending_snapshot::get_temp_path(head_id, my->_snapshots_dir);

   // maintain legacy exception if the snapshot exists
   if( fc::is_regular_file(snapshot_path) ) {
      auto ex = snapshot_exists_exception( FC_LOG_MESSAGE( error, "snapshot named ${name} already exists", ("name", snapshot_path.generic_string()) ) );
      next(ex.dynamic_copy_exception());
      return;
   }

   auto write_snapshot = [&]( const bfs::path& p ) -> void {
      auto reschedule = fc::make_scoped_exit([this](){
         my->schedule_production_loop();
      });

      if (chain.is_building_block()) {
         // abort the pending block
         my->abort_block();
      } else {
         reschedule.cancel();
      }

      bfs::create_directory( p.parent_path() );

      // create the snapshot
      auto snap_out = std::ofstream(p.generic_string(), (std::ios::out | std::ios::binary));
      auto writer = std::make_shared<ostream_snapshot_writer>(snap_out);
      chain.write_snapshot(writer);
      writer->finalize();
      snap_out.flush();
      snap_out.close();
   };

   // If in irreversible mode, create snapshot and return path to snapshot immediately.
   if( chain.get_read_mode() == db_read_mode::IRREVERSIBLE ) {
      try {
         write_snapshot( temp_path );

         boost::system::error_code ec;
         bfs::rename(temp_path, snapshot_path, ec);
         EOS_ASSERT(!ec, snapshot_finalization_exception,
               "Unable to finalize valid snapshot of block number ${bn}: [code: ${ec}] ${message}",
               ("bn", chain.head_block_num())
               ("ec", ec.value())
               ("message", ec.message()));

         next( producer_plugin::snapshot_information{head_id, snapshot_path.generic_string()} );
      } CATCH_AND_CALL (next);
      return;
   }

   // Otherwise, the result will be returned when the snapshot becomes irreversible.

   // determine if this snapshot is already in-flight
   auto& pending_by_id = my->_pending_snapshot_index.get<by_id>();
   auto existing = pending_by_id.find(head_id);
   if( existing != pending_by_id.end() ) {
      // if a snapshot at this block is already pending, attach this requests handler to it
      pending_by_id.modify(existing, [&next]( auto& entry ){
         entry.next = [prev = entry.next, next](const fc::static_variant<fc::exception_ptr, producer_plugin::snapshot_information>& res){
            prev(res);
            next(res);
         };
      });
   } else {
      const auto& pending_path = pending_snapshot::get_pending_path(head_id, my->_snapshots_dir);

      try {
         write_snapshot( temp_path ); // create a new pending snapshot

         boost::system::error_code ec;
         bfs::rename(temp_path, pending_path, ec);
         EOS_ASSERT(!ec, snapshot_finalization_exception,
               "Unable to promote temp snapshot to pending for block number ${bn}: [code: ${ec}] ${message}",
               ("bn", chain.head_block_num())
               ("ec", ec.value())
               ("message", ec.message()));

         my->_pending_snapshot_index.emplace(head_id, next, pending_path.generic_string(), snapshot_path.generic_string());
      } CATCH_AND_CALL (next);
   }
}

producer_plugin::scheduled_protocol_feature_activations
producer_plugin::get_scheduled_protocol_feature_activations()const {
   return {my->_protocol_features_to_activate};
}

void producer_plugin::schedule_protocol_feature_activations( const scheduled_protocol_feature_activations& schedule ) {
   const chain::controller& chain = my->chain_plug->chain();
   std::set<digest_type> set_of_features_to_activate( schedule.protocol_features_to_activate.begin(),
                                                      schedule.protocol_features_to_activate.end() );
   EOS_ASSERT( set_of_features_to_activate.size() == schedule.protocol_features_to_activate.size(),
               invalid_protocol_features_to_activate, "duplicate digests" );
   chain.validate_protocol_features( schedule.protocol_features_to_activate );
   const auto& pfs = chain.get_protocol_feature_manager().get_protocol_feature_set();
   for (auto &feature_digest : set_of_features_to_activate) {
      const auto& pf = pfs.get_protocol_feature(feature_digest);
      EOS_ASSERT( !pf.preactivation_required, protocol_feature_exception,
                  "protocol feature requires preactivation: ${digest}",
                  ("digest", feature_digest));
   }
   my->_protocol_features_to_activate = schedule.protocol_features_to_activate;
   my->_protocol_features_signaled = false;
}

fc::variants producer_plugin::get_supported_protocol_features( const get_supported_protocol_features_params& params ) const {
   fc::variants results;
   const chain::controller& chain = my->chain_plug->chain();
   const auto& pfs = chain.get_protocol_feature_manager().get_protocol_feature_set();
   const auto next_block_time = chain.head_block_time() + fc::milliseconds(config::block_interval_ms);

   flat_map<digest_type, bool>  visited_protocol_features;
   visited_protocol_features.reserve( pfs.size() );

   std::function<bool(const protocol_feature&)> add_feature =
   [&results, &pfs, &params, next_block_time, &visited_protocol_features, &add_feature]
   ( const protocol_feature& pf ) -> bool {
      if( ( params.exclude_disabled || params.exclude_unactivatable ) && !pf.enabled ) return false;
      if( params.exclude_unactivatable && ( next_block_time < pf.earliest_allowed_activation_time  ) ) return false;

      auto res = visited_protocol_features.emplace( pf.feature_digest, false );
      if( !res.second ) return res.first->second;

      const auto original_size = results.size();
      for( const auto& dependency : pf.dependencies ) {
         if( !add_feature( pfs.get_protocol_feature( dependency ) ) ) {
            results.resize( original_size );
            return false;
         }
      }

      res.first->second = true;
      results.emplace_back( pf.to_variant(true) );
      return true;
   };

   for( const auto& pf : pfs ) {
      add_feature( pf );
   }

   return results;
}

producer_plugin::get_account_ram_corrections_result
producer_plugin::get_account_ram_corrections( const get_account_ram_corrections_params& params ) const {
   get_account_ram_corrections_result result;
   const auto& db = my->chain_plug->chain().db();

   const auto& idx = db.get_index<chain::account_ram_correction_index, chain::by_name>();
   account_name lower_bound_value{ std::numeric_limits<uint64_t>::lowest() };
   account_name upper_bound_value{ std::numeric_limits<uint64_t>::max() };

   if( params.lower_bound ) {
      lower_bound_value = *params.lower_bound;
   }

   if( params.upper_bound ) {
      upper_bound_value = *params.upper_bound;
   }

   if( upper_bound_value < lower_bound_value )
      return result;

   auto walk_range = [&]( auto itr, auto end_itr ) {
      for( unsigned int count = 0;
           count < params.limit && itr != end_itr;
           ++itr )
      {
         result.rows.push_back( fc::variant( *itr ) );
         ++count;
      }
      if( itr != end_itr ) {
         result.more = itr->name;
      }
   };

   auto lower = idx.lower_bound( lower_bound_value );
   auto upper = idx.upper_bound( upper_bound_value );
   if( params.reverse ) {
      walk_range( boost::make_reverse_iterator(upper), boost::make_reverse_iterator(lower) );
   } else {
      walk_range( lower, upper );
   }

   return result;
}

optional<fc::time_point> producer_plugin_impl::calculate_next_block_time(const account_name& producer_name, const block_timestamp_type& current_block_time) const {
   chain::controller& chain = chain_plug->chain();
   const auto& hbs = chain.head_block_state();
   if(!hbs){
      ilog("in special situation, head block state is null ptr.....");
   }
   auto active_schedule = hbs->active_schedule.producers;
   auto schedule = hbs->active_backup_schedule.get_schedule();
   flat_map<name, block_signing_authority>* backup_active_schedule = nullptr;
   if(schedule){
      backup_active_schedule = &(hbs->active_backup_schedule.get_schedule()->producers);
   }
   // determine if this producer is in the active schedule/backup active schedule and if so, where
   auto itr = std::find_if(active_schedule.begin(), active_schedule.end(), [&](const auto& asp){ return asp.producer_name == producer_name; });
   flat_map<name, block_signing_authority>::iterator itr_bak;

   if(backup_active_schedule){
      itr_bak = backup_active_schedule->find(producer_name);
   }
   if (itr == active_schedule.end() && backup_active_schedule && itr_bak == backup_active_schedule->end()) {
      // this producer is neither in the active producer set nor in backup active producer set
      return optional<fc::time_point>();
   }
   // else if(itr == active_schedule.end()){
   //    return optional<fc::time_point>();
   // }
   
   if(itr != active_schedule.end()){
      size_t producer_index = itr - active_schedule.begin();
      uint32_t minimum_offset = 1; // must at least be the "next" block

      // account for a watermark in the future which is disqualifying this producer for now
      // this is conservative assuming no blocks are dropped.  If blocks are dropped the watermark will
      // disqualify this producer for longer but it is assumed they will wake up, determine that they
      // are disqualified for longer due to skipped blocks and re-caculate their next block with better
      // information then
      auto current_watermark = get_watermark(producer_name);
      if (current_watermark) {
         const auto watermark = *current_watermark;
         auto block_num = chain.head_block_state()->block_num;
         if (chain.is_building_block()) {
            ++block_num;
         }
         if (watermark.first > block_num) {
            // if I have a watermark block number then I need to wait until after that watermark
            minimum_offset = watermark.first - block_num + 1;
         }
         if (watermark.second > current_block_time) {
            // if I have a watermark block timestamp then I need to wait until after that watermark timestamp
            minimum_offset = std::max(minimum_offset, watermark.second.slot - current_block_time.slot + 1);
         }
      }

      // this producers next opportuity to produce is the next time its slot arrives after or at the calculated minimum
      uint32_t minimum_slot = current_block_time.slot + minimum_offset;
      size_t minimum_slot_producer_index = (minimum_slot % (active_schedule.size() * config::producer_repetitions)) / config::producer_repetitions;
      if ( producer_index == minimum_slot_producer_index ) {
         // this is the producer for the minimum slot, go with that
         return block_timestamp_type(minimum_slot).to_time_point();
      } else {
         // calculate how many rounds are between the minimum producer and the producer in question
         size_t producer_distance = producer_index - minimum_slot_producer_index;
         // check for unsigned underflow
         if (producer_distance > producer_index) {
            producer_distance += active_schedule.size();
         }

         // align the minimum slot to the first of its set of reps
         uint32_t first_minimum_producer_slot = minimum_slot - (minimum_slot % config::producer_repetitions);

         // offset the aligned minimum to the *earliest* next set of slots for this producer
         uint32_t next_block_slot = first_minimum_producer_slot  + (producer_distance * config::producer_repetitions);
         return block_timestamp_type(next_block_slot).to_time_point();
      }
   }else if(backup_active_schedule && itr_bak != backup_active_schedule->end()){
      size_t producer_index = itr_bak - backup_active_schedule->begin();
      uint32_t minimum_offset = 1; // must at least be the "next" block

      auto current_watermark = get_watermark(producer_name);
      if (current_watermark) {
         dlog("current watermark is not null");
         const auto watermark = *current_watermark;
         auto block_num = chain.head_block_state()->block_num;
         if (chain.is_building_block()) {
            ++block_num;
         }
         if (watermark.first > block_num) {
            // if I have a watermark block number then I need to wait until after that watermark
            minimum_offset = watermark.first - block_num + 1;
         }
         if (watermark.second > current_block_time) {
            // if I have a watermark block timestamp then I need to wait until after that watermark timestamp
            minimum_offset = std::max(minimum_offset, watermark.second.slot - current_block_time.slot + 1);
         }
      }

      // this producers next opportuity to produce is the next time its slot arrives after or at the calculated minimum
      uint32_t minimum_slot = current_block_time.slot + minimum_offset;
      size_t minimum_slot_producer_index = (minimum_slot % (backup_active_schedule->size() * config::backup_producer_repetitions)) / config::backup_producer_repetitions;
      if ( producer_index == minimum_slot_producer_index ) {
         // this is the producer for the minimum slot, go with that
         return block_timestamp_type(minimum_slot).to_time_point();
      } else {
         // calculate how many rounds are between the minimum producer and the producer in question
         size_t producer_distance = producer_index - minimum_slot_producer_index;
         // check for unsigned underflow
         if (producer_distance > producer_index) {
            producer_distance += backup_active_schedule->size();
         }

         // align the minimum slot to the first of its set of reps
         uint32_t first_minimum_producer_slot = minimum_slot - (minimum_slot % config::backup_producer_repetitions);

         // offset the aligned minimum to the *earliest* next set of slots for this producer
         uint32_t next_block_slot = first_minimum_producer_slot  + (producer_distance * config::backup_producer_repetitions);
         return block_timestamp_type(next_block_slot).to_time_point();
      }
   }
   return optional<fc::time_point>();
}

fc::time_point producer_plugin_impl::calculate_pending_block_time() const {
   const chain::controller& chain = chain_plug->chain();
   const fc::time_point now = fc::time_point::now();
   const fc::time_point base = std::max<fc::time_point>(now, chain.head_block_time());
   const int64_t min_time_to_next_block = (config::block_interval_us) - (base.time_since_epoch().count() % (config::block_interval_us) );
   fc::time_point block_time = base + fc::microseconds(min_time_to_next_block);
   return block_time;
}

fc::time_point producer_plugin_impl::calculate_block_deadline( const fc::time_point& block_time ) const {
   bool last_block = ((block_timestamp_type(block_time).slot % config::producer_repetitions) == config::producer_repetitions - 1);
   return block_time + fc::microseconds(last_block ? _last_block_time_offset_us : _produce_time_offset_us);
}

producer_plugin_impl::start_block_result producer_plugin_impl::start_block(bool& is_backup_out) {
   chain::controller& chain = chain_plug->chain();

   if( !chain_plug->accept_transactions() )
      return start_block_result::waiting_for_block;

   const auto& hbs = chain.head_block_state();
   bool is_backup = false;

   const fc::time_point now = fc::time_point::now();
   const fc::time_point block_time = calculate_pending_block_time();

   const pending_block_mode previous_pending_mode = _pending_block_mode;
   _pending_block_mode = pending_block_mode::producing;

   // Not our turn
   const auto& scheduled_producer = hbs->get_scheduled_producer(block_time);
   const auto& backup_scheduled_producer = hbs->get_backup_scheduled_producer(block_time);
   const auto current_watermark = get_watermark(scheduled_producer.producer_name);

   size_t num_relevant_signatures = 0;
   dlog("num_relevant_signatures 1st: ${num_relevant_signatures}",("num_relevant_signatures",num_relevant_signatures));
   scheduled_producer.for_each_key([&](const public_key_type& key){
      const auto& iter = _signature_providers.find(key);
      if(iter != _signature_providers.end()) {
         num_relevant_signatures++;
      }
      /**
      *@Description: process producing mode switching
      */
      if(num_relevant_signatures > 0){
         dlog("producer start change from ${pmod} to ${cmod}",("pmod",chain.is_backup_produce()?"backup":"main")("cmod","main"));
         chain.set_produce_mode(false);
         is_backup = false;
      }
   });
    dlog("num_relevant_signatures 2nd: ${num_relevant_signatures}",("num_relevant_signatures",num_relevant_signatures));
    dlog("backup_scheduled_producer: ${backup_scheduled_producer}",("backup_scheduled_producer",backup_scheduled_producer));
    dlog("backup_scheduled_producer is valid ?: ${valid}",("valid",backup_scheduled_producer.valid()));
   // TODO: should not enter backup producing mode when main producer is valid
   if(num_relevant_signatures == 0 && backup_scheduled_producer.valid()){
      dlog("num_relevant_signatures 3rd: ${num_relevant_signatures}",("num_relevant_signatures",num_relevant_signatures));
      backup_scheduled_producer->for_each_key([&](const public_key_type& key){
         const auto& iter = _signature_providers.find(key);
         if(iter != _signature_providers.end()) {
            num_relevant_signatures++;
         }
         /** 
         *@Description: for test 2 producing mode switch
         */
         if(num_relevant_signatures > 0){
            dlog("producer start change from ${pmod} to ${cmod}",("pmod",chain.is_backup_produce()?"backup":"main")("cmod","backup"));
            chain.set_verify_mode(true);
            is_backup = true;
         }
      });
   }
   
   is_backup_out = is_backup;
   
   producer_authority check_producer;
   if(!is_backup){
      check_producer = scheduled_producer;
   }else{
      check_producer = *backup_scheduled_producer;
   }

   auto irreversible_block_age = get_irreversible_block_age();

   // If the next block production opportunity is in the present or future, we're synced.
   if( !_production_enabled ) {
      _pending_block_mode = pending_block_mode::speculating;
   } else if( _producers.find(check_producer.producer_name) == _producers.end()) {
      _pending_block_mode = pending_block_mode::speculating;
   } else if (num_relevant_signatures == 0) {
      elog("Not producing block because I don't have any private keys relevant to authority: ${authority}", ("authority", scheduled_producer.authority));
      _pending_block_mode = pending_block_mode::speculating;
   } else if ( _pause_production ) {
      elog("Not producing block because production is explicitly paused");
      _pending_block_mode = pending_block_mode::speculating;
   } else if ( _max_irreversible_block_age_us.count() >= 0 && irreversible_block_age >= _max_irreversible_block_age_us ) {
      elog("Not producing block because the irreversible block is too old [age:${age}s, max:${max}s]", ("age", irreversible_block_age.count() / 1'000'000)( "max", _max_irreversible_block_age_us.count() / 1'000'000 ));
      _pending_block_mode = pending_block_mode::speculating;
   }

   if (_pending_block_mode == pending_block_mode::producing) {
      // determine if our watermark excludes us from producing at this point
      if (current_watermark) {
         const block_timestamp_type block_timestamp{block_time};
         if (current_watermark->first > hbs->block_num && !is_backup) {
            elog("Not producing block because \"${producer}\" signed a block at a higher block number (${watermark}) than the current fork's head (${head_block_num})",
                 ("producer", scheduled_producer.producer_name)
                 ("watermark", current_watermark->first)
                 ("head_block_num", hbs->block_num));
            _pending_block_mode = pending_block_mode::speculating;
         } else if (current_watermark->second >= block_timestamp && !is_backup) {
            elog("Not producing block because \"${producer}\" signed a block at the next block time or later (${watermark}) than the pending block time (${block_timestamp})",
                 ("producer", scheduled_producer.producer_name)
                 ("watermark", current_watermark->second)
                 ("block_timestamp", block_timestamp));
            _pending_block_mode = pending_block_mode::speculating;
         }
      }
   }

   if (_pending_block_mode == pending_block_mode::speculating) {
      auto head_block_age = now - chain.head_block_time();
      if (head_block_age > fc::seconds(5))
         return start_block_result::waiting_for_block;
   }

   if (_pending_block_mode == pending_block_mode::producing) {
      const auto start_block_time = block_time - fc::microseconds( config::block_interval_us );
      if( now < start_block_time ) {
         fc_dlog(_log, "Not producing block waiting for production window ${n} ${bt}", ("n", hbs->block_num + 1)("bt", block_time) );
         // start_block_time instead of block_time because schedule_delayed_production_loop calculates next block time from given time
         schedule_delayed_production_loop(weak_from_this(), calculate_producer_wake_up_time(start_block_time));
         return start_block_result::waiting_for_production;
      }
   } else if (previous_pending_mode == pending_block_mode::producing) {
      // just produced our last block of our round
      const auto start_block_time = block_time - fc::microseconds( config::block_interval_us );
      fc_dlog(_log, "Not starting speculative block until ${bt}", ("bt", start_block_time) );
      schedule_delayed_production_loop( weak_from_this(), start_block_time);
      return start_block_result::waiting_for_production;
   }
   if(!is_backup){
       fc_dlog(_log, "Starting block #${n} at ${time} producer ${p}",
           ("n", hbs->block_num + 1)("time", now)("p", scheduled_producer.producer_name));
   }else{
      fc_dlog(_log, "Starting backup block #${n} at ${time} producer ${p}",
           ("n", hbs->block_num + 1)("time", now)("p", backup_scheduled_producer->producer_name));
   }
   
   try {
      //                      ||
      //setup watermark point \/
      uint16_t blocks_to_confirm = 0;

      if (_pending_block_mode == pending_block_mode::producing) {
         // determine how many blocks this producer can confirm
         // 1) if it is not a producer from this node, assume no confirmations (we will discard this block anyway)
         // 2) if it is a producer on this node that has never produced, the conservative approach is to assume no
         //    confirmations to make sure we don't double sign after a crash TODO: make these watermarks durable?
         // 3) if it is a producer on this node where this node knows the last block it produced, safely set it -UNLESS-
         // 4) the producer on this node's last watermark is higher (meaning on a different fork)
         if (current_watermark) {
            auto watermark_bn = current_watermark->first;
            if (watermark_bn < hbs->block_num) {
               blocks_to_confirm = (uint16_t)(std::min<uint32_t>(std::numeric_limits<uint16_t>::max(), (uint32_t)(hbs->block_num - watermark_bn)));
            }
         }

         // can not confirm irreversible blocks
         blocks_to_confirm = (uint16_t)(std::min<uint32_t>(blocks_to_confirm, (uint32_t)(hbs->block_num - hbs->dpos_irreversible_blocknum)));
      }

      if(is_backup){
         blocks_to_confirm = 0;
      }

      abort_block();

      auto features_to_activate = chain.get_preactivated_protocol_features();
      if( _pending_block_mode == pending_block_mode::producing && _protocol_features_to_activate.size() > 0 ) {
         bool drop_features_to_activate = false;
         try {
            chain.validate_protocol_features( _protocol_features_to_activate );
         } catch( const fc::exception& e ) {
            wlog( "protocol features to activate are no longer all valid: ${details}",
                  ("details",e.to_detail_string()) );
            drop_features_to_activate = true;
         }

         if( drop_features_to_activate ) {
            _protocol_features_to_activate.clear();
         } else {
            auto protocol_features_to_activate = _protocol_features_to_activate; // do a copy as pending_block might be aborted
            if( features_to_activate.size() > 0 ) {
               protocol_features_to_activate.reserve( protocol_features_to_activate.size()
                                                         + features_to_activate.size() );
               std::set<digest_type> set_of_features_to_activate( protocol_features_to_activate.begin(),
                                                                  protocol_features_to_activate.end() );
               for( const auto& f : features_to_activate ) {
                  auto res = set_of_features_to_activate.insert( f );
                  if( res.second ) {
                     protocol_features_to_activate.push_back( f );
                  }
               }
               features_to_activate.clear();
            }
            std::swap( features_to_activate, protocol_features_to_activate );
            _protocol_features_signaled = true;
            ilog( "signaling activation of the following protocol features in block ${num}: ${features_to_activate}",
                  ("num", hbs->block_num + 1)("features_to_activate", features_to_activate) );
         }
      }

      chain.start_block( block_time, blocks_to_confirm, features_to_activate ,is_backup);
   } LOG_AND_DROP();

   if( chain.is_building_block() ) {
      const auto& pending_block_signing_authority = chain.pending_block_signing_authority();
      const fc::time_point preprocess_deadline = calculate_block_deadline(block_time);

      // if (_pending_block_mode == pending_block_mode::producing && pending_block_signing_authority != scheduled_producer.authority) {
      //    elog("Unexpected block signing authority, reverting to speculative mode! [expected: \"${expected}\", actual: \"${actual\"", ("expected", scheduled_producer.authority)("actual", pending_block_signing_authority));
      //    _pending_block_mode = pending_block_mode::speculating;
      // }

      try {
         if( !remove_expired_persisted_trxs( preprocess_deadline ) )
            return start_block_result::exhausted;
         if( !remove_expired_blacklisted_trxs( preprocess_deadline ) )
            return start_block_result::exhausted;
         if( !_subjective_billing.remove_expired( _log, chain.pending_block_time(), fc::time_point::now(), preprocess_deadline ) )
            return start_block_result::exhausted;

         // limit execution of pending incoming to once per block
         size_t pending_incoming_process_limit = _pending_incoming_transactions.size();

         if( !process_unapplied_trxs( preprocess_deadline ) )
            return start_block_result::exhausted;

         if (_pending_block_mode == pending_block_mode::producing) {
            auto scheduled_trx_deadline = preprocess_deadline;
            if (_max_scheduled_transaction_time_per_block_ms >= 0) {
               scheduled_trx_deadline = std::min<fc::time_point>(
                     scheduled_trx_deadline,
                     fc::time_point::now() + fc::milliseconds(_max_scheduled_transaction_time_per_block_ms)
               );
            }
            // may exhaust scheduled_trx_deadline but not preprocess_deadline, exhausted preprocess_deadline checked below
            process_scheduled_and_incoming_trxs( scheduled_trx_deadline, pending_incoming_process_limit );
         }

         if( app().is_quiting() ) // db guard exception above in LOG_AND_DROP could have called app().quit()
            return start_block_result::failed;
         if (preprocess_deadline <= fc::time_point::now() || block_is_exhausted()) {
            return start_block_result::exhausted;
         } else {
            if( !process_incoming_trxs( preprocess_deadline, pending_incoming_process_limit ) )
               return start_block_result::exhausted;
            return start_block_result::succeeded;
         }

      } catch ( const guard_exception& e ) {
         chain_plugin::handle_guard_exception(e);
         return start_block_result::failed;
      } catch ( std::bad_alloc& ) {
         chain_plugin::handle_bad_alloc();
      } catch ( boost::interprocess::bad_alloc& ) {
         chain_plugin::handle_db_exhaustion();
      }

   }

   return start_block_result::failed;
}

bool producer_plugin_impl::remove_expired_persisted_trxs( const fc::time_point& deadline )
{
   chain::controller& chain = chain_plug->chain();
   auto pending_block_time = chain.pending_block_time();

   // remove all expired transactions
   size_t num_expired_persistent = 0;
   size_t num_expired_other = 0;
   size_t orig_count = _unapplied_transactions.size();
   bool exhausted = !_unapplied_transactions.clear_expired( pending_block_time, deadline,
                  [&num_expired_persistent, &num_expired_other, pbm = _pending_block_mode,
                   &chain, has_producers = !_producers.empty()]( const transaction_id_type& txid, trx_enum_type trx_type ) {
            if( trx_type == trx_enum_type::persisted ) {
               if( pbm == pending_block_mode::producing ) {
                  fc_dlog(_trx_successful_trace_log,
                           "[TRX_TRACE] Block ${block_num} for producer ${prod} is EXPIRING PERSISTED tx: ${txid}",
                           ("block_num", chain.head_block_num() + 1)("txid", txid)
                           ("prod", chain.is_building_block() ? chain.pending_block_producer() : name()) );
                  fc_dlog(_trx_failed_trace_log,
                           "[TRX_TRACE] Block ${block_num} for producer ${prod} is EXPIRING PERSISTED tx: ${txid}",
                           ("block_num", chain.head_block_num() + 1)("txid", txid)
                           ("prod", chain.is_building_block() ? chain.pending_block_producer() : name()) );
               } else {
                  fc_dlog(_trx_successful_trace_log, "[TRX_TRACE] Speculative execution is EXPIRING PERSISTED tx: ${txid}", ("txid", txid));
                  fc_dlog(_trx_failed_trace_log, "[TRX_TRACE] Speculative execution is EXPIRING PERSISTED tx: ${txid}", ("txid", txid));
               }
               ++num_expired_persistent;
            } else {
               if (has_producers) {
                  fc_dlog(_trx_successful_trace_log,
                        "[TRX_TRACE] Node with producers configured is dropping an EXPIRED transaction that was PREVIOUSLY ACCEPTED : ${txid}",
                        ("txid", txid));
                  fc_dlog(_trx_failed_trace_log,
                        "[TRX_TRACE] Node with producers configured is dropping an EXPIRED transaction that was PREVIOUSLY ACCEPTED : ${txid}",
                        ("txid", txid));
               }
               ++num_expired_other;
            }
   });

   if( exhausted ) {
      fc_wlog( _log, "Unable to process all expired transactions in unapplied queue before deadline, "
                     "Persistent expired ${persistent_expired}, Other expired ${other_expired}",
               ("persistent_expired", num_expired_persistent)("other_expired", num_expired_other) );
   } else {
      fc_dlog( _log, "Processed ${m} expired transactions of the ${n} transactions in the unapplied queue, "
                     "Persistent expired ${persistent_expired}, Other expired ${other_expired}",
               ("m", num_expired_persistent+num_expired_other)("n", orig_count)
               ("persistent_expired", num_expired_persistent)("other_expired", num_expired_other) );
   }

   return !exhausted;
}

bool producer_plugin_impl::remove_expired_blacklisted_trxs( const fc::time_point& deadline )
{
   bool exhausted = false;
   auto& blacklist_by_expiry = _blacklisted_transactions.get<by_expiry>();
   if(!blacklist_by_expiry.empty()) {
      const chain::controller& chain = chain_plug->chain();
      const auto lib_time = chain.last_irreversible_block_time();

      int num_expired = 0;
      int orig_count = _blacklisted_transactions.size();

      while (!blacklist_by_expiry.empty() && blacklist_by_expiry.begin()->expiry <= lib_time) {
         if (deadline <= fc::time_point::now()) {
            exhausted = true;
            break;
         }
         blacklist_by_expiry.erase(blacklist_by_expiry.begin());
         num_expired++;
      }

      fc_dlog(_log, "Processed ${n} blacklisted transactions, Expired ${expired}",
              ("n", orig_count)("expired", num_expired));
   }
   return !exhausted;
}

namespace {
// track multiple failures on unapplied transactions
class account_failures {
public:
   constexpr static uint32_t max_failures_per_account = 3;

   void add( const account_name& n, int64_t exception_code ) {
      auto& fa = failed_accounts[n];
      ++fa.num_failures;
      fa.add( n, exception_code );
   }

   // return true if exceeds max_failures_per_account and should be dropped
   bool failure_limit( const account_name& n ) {
      auto fitr = failed_accounts.find( n );
      if( fitr != failed_accounts.end() && fitr->second.num_failures >= max_failures_per_account ) {
         ++fitr->second.num_failures;
         return true;
      }
      return false;
   }

   void report() const {
      if( _log.is_enabled( fc::log_level::debug ) ) {
         for( const auto& e : failed_accounts ) {
            std::string reason;
            if( e.second.num_failures > max_failures_per_account ) {
               reason.clear();
               if( e.second.is_deadline() ) reason += "deadline";
               if( e.second.is_tx_cpu_usage() ) {
                  if( !reason.empty() ) reason += ", ";
                  reason += "tx_cpu_usage";
               }
               if( e.second.is_eosio_assert() ) {
                  if( !reason.empty() ) reason += ", ";
                  reason += "assert";
               }
               if( e.second.is_other() ) {
                  if( !reason.empty() ) reason += ", ";
                  reason += "other";
               }
               fc_dlog( _log, "Dropped ${n} trxs, account: ${a}, reason: ${r} exceeded",
                        ("n", e.second.num_failures - max_failures_per_account)("a", e.first)("r", reason) );
            }
         }
      }
   }

private:
   struct account_failure {
      enum class ex_fields : uint8_t {
         ex_deadline_exception = 1,
         ex_tx_cpu_usage_exceeded = 2,
         ex_eosio_assert_exception = 4,
         ex_other_exception = 8
      };

      void add( const account_name& n, int64_t exception_code ) {
         if( exception_code == tx_cpu_usage_exceeded::code_value ) {
            ex_flags = set_field( ex_flags, ex_fields::ex_tx_cpu_usage_exceeded );
         } else if( exception_code == deadline_exception::code_value ) {
            ex_flags = set_field( ex_flags, ex_fields::ex_deadline_exception );
         } else if( exception_code == eosio_assert_message_exception::code_value ||
                    exception_code == eosio_assert_code_exception::code_value ) {
            ex_flags = set_field( ex_flags, ex_fields::ex_eosio_assert_exception );
         } else {
            ex_flags = set_field( ex_flags, ex_fields::ex_other_exception );
            fc_dlog( _log, "Failed trx, account: ${a}, reason: ${r}",
                     ("a", n)("r", exception_code) );
         }
      }

      bool is_deadline() const { return has_field( ex_flags, ex_fields::ex_deadline_exception ); }
      bool is_tx_cpu_usage() const { return has_field( ex_flags, ex_fields::ex_tx_cpu_usage_exceeded ); }
      bool is_eosio_assert() const { return has_field( ex_flags, ex_fields::ex_eosio_assert_exception ); }
      bool is_other() const { return has_field( ex_flags, ex_fields::ex_other_exception ); }

      uint32_t num_failures = 0;
      uint8_t ex_flags = 0;
   };

   std::map<account_name, account_failure> failed_accounts;
};

} // anonymous namespace

bool producer_plugin_impl::process_unapplied_trxs( const fc::time_point& deadline )
{
   bool exhausted = false;
   if( !_unapplied_transactions.empty() ) {
      account_failures account_fails;
      chain::controller& chain = chain_plug->chain();
      const auto& rl = chain.get_resource_limits_manager();
      int num_applied = 0, num_failed = 0, num_processed = 0;
      auto unapplied_trxs_size = _unapplied_transactions.size();
      auto itr     = (_pending_block_mode == pending_block_mode::producing) ?
                     _unapplied_transactions.begin() : _unapplied_transactions.persisted_begin();
      auto end_itr = (_pending_block_mode == pending_block_mode::producing) ?
                     _unapplied_transactions.end()   : _unapplied_transactions.persisted_end();
      while( itr != end_itr ) {
         if( deadline <= fc::time_point::now() ) {
            exhausted = true;
            break;
         }

         const transaction_metadata_ptr trx = itr->trx_meta;
         ++num_processed;
         try {
            auto start = fc::time_point::now();
            auto trx_deadline = start + fc::milliseconds( _max_transaction_time_ms );

            auto first_auth = trx->packed_trx()->get_transaction().first_authorizer();
            if( account_fails.failure_limit( first_auth ) ) {
               ++num_failed;
               itr = _unapplied_transactions.erase( itr );
               continue;
            }

            auto prev_billed_cpu_time_us = trx->billed_cpu_time_us;
            if( prev_billed_cpu_time_us > 0 && !rl.is_unlimited_cpu( first_auth )) {
               auto prev_billed_plus100 = prev_billed_cpu_time_us + EOS_PERCENT( prev_billed_cpu_time_us, 100 * config::percent_1 );
               auto trx_dl = start + fc::microseconds( prev_billed_plus100 );
               if( trx_dl < trx_deadline ) trx_deadline = trx_dl;
            }
            bool deadline_is_subjective = false;
            if( _max_transaction_time_ms < 0 ||
                (_pending_block_mode == pending_block_mode::producing && deadline < trx_deadline) ) {
               deadline_is_subjective = true;
               trx_deadline = deadline;
            }
            // no subjective billing since we are producing or processing persisted trxs
            const uint32_t sub_bill = 0;

            auto trace = chain.push_transaction( trx, trx_deadline, prev_billed_cpu_time_us, false, sub_bill );
            fc_dlog( _trx_failed_trace_log, "Subjective unapplied bill for ${a}: ${b} prev ${t}us", ("a",first_auth)("b",prev_billed_cpu_time_us)("t",trace->elapsed));
            if( trace->except ) {
               if( exception_is_exhausted( *trace->except, deadline_is_subjective ) ) {
                  if( block_is_exhausted() ) {
                     exhausted = true;
                     // don't erase, subjective failure so try again next time
                     break;
                  }
               } else {
                  fc_dlog( _trx_failed_trace_log, "Subjective unapplied bill for failed ${a}: ${b} prev ${t}us", ("a",first_auth)("b",prev_billed_cpu_time_us)("t",trace->elapsed));
                  auto failure_code = trace->except->code();
                  if( failure_code != tx_duplicate::code_value ) {
                     // this failed our configured maximum transaction time, we don't want to replay it
                     fc_dlog( _log, "Failed ${c} trx, prev billed: ${p}us, ran: ${r}us, id: ${id}",
                              ("c", trace->except->code())("p", prev_billed_cpu_time_us)
                              ("r", fc::time_point::now() - start)("id", trx->id()) );
                     account_fails.add( first_auth, failure_code );
                     _subjective_billing.subjective_bill_failure( first_auth, trace->elapsed, fc::time_point::now() );
                  }
                  ++num_failed;
                  itr = _unapplied_transactions.erase( itr );
                  continue;
               }
            } else {
               fc_dlog( _trx_successful_trace_log, "Subjective unapplied bill for success ${a}: ${b} prev ${t}us", ("a",first_auth)("b",prev_billed_cpu_time_us)("t",trace->elapsed));
               // if db_read_mode SPECULATIVE then trx is in the pending block and not immediately reverted
               _subjective_billing.subjective_bill( trx->id(), trx->packed_trx()->expiration(), first_auth, trace->elapsed,
                                                    chain.get_read_mode() == chain::db_read_mode::SPECULATIVE );
               ++num_applied;
               itr = _unapplied_transactions.erase( itr );
               continue;
            }
         } LOG_AND_DROP();
         ++itr;
      }

      fc_dlog( _log, "Processed ${m} of ${n} previously applied transactions, Applied ${applied}, Failed/Dropped ${failed}",
               ("m", num_processed)( "n", unapplied_trxs_size )("applied", num_applied)("failed", num_failed) );
      account_fails.report();
   }
   return !exhausted;
}

void producer_plugin_impl::process_scheduled_and_incoming_trxs( const fc::time_point& deadline, size_t& pending_incoming_process_limit )
{
   // scheduled transactions
   int num_applied = 0;
   int num_failed = 0;
   int num_processed = 0;
   bool exhausted = false;
   double incoming_trx_weight = 0.0;

   auto& blacklist_by_id = _blacklisted_transactions.get<by_id>();
   chain::controller& chain = chain_plug->chain();
   time_point pending_block_time = chain.pending_block_time();
   const auto& sch_idx = chain.db().get_index<generated_transaction_multi_index,by_delay>();
   const auto scheduled_trxs_size = sch_idx.size();
   auto sch_itr = sch_idx.begin();
   while( sch_itr != sch_idx.end() ) {
      if( sch_itr->delay_until > pending_block_time) break;    // not scheduled yet
      if( exhausted || deadline <= fc::time_point::now() ) {
         exhausted = true;
         break;
      }
      if( sch_itr->published >= pending_block_time ) {
         ++sch_itr;
         continue; // do not allow schedule and execute in same block
      }

      if (blacklist_by_id.find(sch_itr->trx_id) != blacklist_by_id.end()) {
         ++sch_itr;
         continue;
      }

      const transaction_id_type trx_id = sch_itr->trx_id; // make copy since reference could be invalidated
      const auto sch_expiration = sch_itr->expiration;
      auto sch_itr_next = sch_itr; // save off next since sch_itr may be invalidated by loop
      ++sch_itr_next;
      const auto next_delay_until = sch_itr_next != sch_idx.end() ? sch_itr_next->delay_until : sch_itr->delay_until;
      const auto next_id = sch_itr_next != sch_idx.end() ? sch_itr_next->id : sch_itr->id;

      num_processed++;

      // configurable ratio of incoming txns vs deferred txns
      while (incoming_trx_weight >= 1.0 && pending_incoming_process_limit && _pending_incoming_transactions.size()) {
         if (deadline <= fc::time_point::now()) {
            exhausted = true;
            break;
         }

         auto e = _pending_incoming_transactions.pop_front();
         --pending_incoming_process_limit;
         incoming_trx_weight -= 1.0;
         if( !process_incoming_transaction_async(std::get<0>(e), std::get<1>(e), std::get<2>(e)) ) {
            exhausted = true;
            break;
         }
      }

      if (exhausted || deadline <= fc::time_point::now()) {
         exhausted = true;
         break;
      }

      try {
         auto trx_deadline = fc::time_point::now() + fc::milliseconds(_max_transaction_time_ms);
         bool deadline_is_subjective = false;
         if (_max_transaction_time_ms < 0 || (_pending_block_mode == pending_block_mode::producing && deadline < trx_deadline)) {
            deadline_is_subjective = true;
            trx_deadline = deadline;
         }

         auto trace = chain.push_scheduled_transaction(trx_id, trx_deadline, 0, false);
         if (trace->except) {
            if (exception_is_exhausted(*trace->except, deadline_is_subjective)) {
               if( block_is_exhausted() ) {
                  exhausted = true;
                  break;
               }
            } else {
               // this failed our configured maximum transaction time, we don't want to replay it add it to a blacklist
               _blacklisted_transactions.insert(transaction_id_with_expiry{trx_id, sch_expiration});
               num_failed++;
            }
         } else {
            num_applied++;
         }
      } LOG_AND_DROP();

      incoming_trx_weight += _incoming_defer_ratio;
      if (!pending_incoming_process_limit) incoming_trx_weight = 0.0;

      if( sch_itr_next == sch_idx.end() ) break;
      sch_itr = sch_idx.lower_bound( boost::make_tuple( next_delay_until, next_id ) );
   }

   if( scheduled_trxs_size > 0 ) {
      fc_dlog( _log,
               "Processed ${m} of ${n} scheduled transactions, Applied ${applied}, Failed/Dropped ${failed}",
               ( "m", num_processed )( "n", scheduled_trxs_size )( "applied", num_applied )( "failed", num_failed ) );
   }
}

bool producer_plugin_impl::process_incoming_trxs( const fc::time_point& deadline, size_t& pending_incoming_process_limit )
{
   bool exhausted = false;
   if (!_pending_incoming_transactions.empty()) {
      size_t processed = 0;
      fc_dlog(_log, "Processing ${n} pending transactions", ("n", pending_incoming_process_limit));
      while (pending_incoming_process_limit && _pending_incoming_transactions.size()) {
         if (deadline <= fc::time_point::now()) {
            exhausted = true;
            break;
         }
         auto e = _pending_incoming_transactions.pop_front();
         --pending_incoming_process_limit;
         ++processed;
         if( !process_incoming_transaction_async(std::get<0>(e), std::get<1>(e), std::get<2>(e)) ) {
            exhausted = true;
            break;
         }
      }
      fc_dlog(_log, "Processed ${n} pending transactions, ${p} left", ("n", processed)("p", _pending_incoming_transactions.size()));
   }
   return !exhausted;
}

bool producer_plugin_impl::block_is_exhausted() const {
   const chain::controller& chain = chain_plug->chain();
   const auto& rl = chain.get_resource_limits_manager();

   const uint64_t cpu_limit = rl.get_block_cpu_limit();
   if( cpu_limit < _max_block_cpu_usage_threshold_us ) return true;
   const uint64_t net_limit = rl.get_block_net_limit();
   if( net_limit < _max_block_net_usage_threshold_bytes ) return true;
   return false;
}

// Example:
// --> Start block A (block time x.500) at time x.000
// -> start_block()
// --> deadline, produce block x.500 at time x.400 (assuming 80% cpu block effort)
// -> Idle
// --> Start block B (block time y.000) at time x.500
void producer_plugin_impl::schedule_production_loop() {
   _timer.cancel();
   bool is_backup = false;
   auto result = start_block(is_backup);

   if (result == start_block_result::failed) {
      elog("Failed to start a pending block, will try again later");
      _timer.expires_from_now( boost::posix_time::microseconds( config::block_interval_us  / 10 ));
      if(is_backup){
         chain::controller& chain = chain_plug->chain();
         const auto& hbs = chain.head_block_state();
         hbs->next_is_backup = false;
      }
      // we failed to start a block, so try again later?
      _timer.async_wait( app().get_priority_queue().wrap( priority::high,
          [weak_this = weak_from_this(), cid = ++_timer_corelation_id]( const boost::system::error_code& ec ) {
             auto self = weak_this.lock();
             if( self && ec != boost::asio::error::operation_aborted && cid == self->_timer_corelation_id ) {
                self->schedule_production_loop();
             }
          } ) );
   } else if (result == start_block_result::waiting_for_block){
      if(is_backup){
         chain::controller& chain = chain_plug->chain();
         const auto& hbs = chain.head_block_state();
         hbs->next_is_backup = false;
      }

      if (!_producers.empty() && !production_disabled_by_policy()) {
         fc_dlog(_log, "Waiting till another block is received and scheduling Speculative/Production Change");
         schedule_delayed_production_loop(weak_from_this(), calculate_producer_wake_up_time(calculate_pending_block_time()));
      } else {
         fc_dlog(_log, "Waiting till another block is received");
         // nothing to do until more blocks arrive
      }

   } else if (result == start_block_result::waiting_for_production) {
      // scheduled in start_block()

   } else if (_pending_block_mode == pending_block_mode::producing) {
      schedule_maybe_produce_block( result == start_block_result::exhausted,is_backup);

   } else if (_pending_block_mode == pending_block_mode::speculating && !_producers.empty() && !production_disabled_by_policy()){
      chain::controller& chain = chain_plug->chain();
      if(is_backup){
         const auto& hbs = chain.head_block_state();
         hbs->next_is_backup = false;
      }
      fc_dlog(_log, "Speculative Block Created; Scheduling Speculative/Production Change");
      EOS_ASSERT( chain.is_building_block(), missing_pending_block_state, "speculating without pending_block_state" );
      schedule_delayed_production_loop(weak_from_this(), calculate_producer_wake_up_time(chain.pending_block_time()));
   } else {
      fc_dlog(_log, "Speculative Block Created");
   }
}

void producer_plugin_impl::schedule_maybe_produce_block( bool exhausted,bool is_backup) {
   chain::controller& chain = chain_plug->chain();

   // we succeeded but block may be exhausted
   static const boost::posix_time::ptime epoch( boost::gregorian::date( 1970, 1, 1 ) );
   auto deadline = calculate_block_deadline( chain.pending_block_time() );

   if( !exhausted && deadline > fc::time_point::now() ) {
      // ship this block off no later than its deadline
      EOS_ASSERT( chain.is_building_block(), missing_pending_block_state,
                  "producing without pending_block_state, start_block succeeded" );
      _timer.expires_at( epoch + boost::posix_time::microseconds( deadline.time_since_epoch().count() ) );
      fc_dlog( _log, "Scheduling Block Production on Normal Block #${num} for ${time}",
               ("num", chain.head_block_num() + 1)( "time", deadline ) );
   } else {
      EOS_ASSERT( chain.is_building_block(), missing_pending_block_state, "producing without pending_block_state" );
      _timer.expires_from_now( boost::posix_time::microseconds( 0 ) );
      fc_dlog( _log, "Scheduling Block Production on ${desc} Block #${num} immediately",
               ("num", chain.head_block_num() + 1)("desc", block_is_exhausted() ? "Exhausted" : "Deadline exceeded") );
   }

   _timer.async_wait( app().get_priority_queue().wrap( priority::high,
         [&chain, weak_this = weak_from_this(), cid=++_timer_corelation_id,is_backup](const boost::system::error_code& ec) {
            auto self = weak_this.lock();
            if( self && ec != boost::asio::error::operation_aborted && cid == self->_timer_corelation_id ) {
               // pending_block_state expected, but can't assert inside async_wait
               auto block_num = chain.is_building_block() ? chain.head_block_num() + 1 : 0;
               fc_dlog( _log, "Produce block timer for ${num} running at ${time}", ("num", block_num)("time", fc::time_point::now()) );
               fc_dlog( _log, "Produce backup block ? ${backup}....", ("backup",is_backup));
               auto res = self->maybe_produce_block(is_backup);
               fc_dlog( _log, "Producing Block #${num} returned: ${res}", ("num", block_num)( "res", res ) );
            }
         } ) );
}

optional<fc::time_point> producer_plugin_impl::calculate_producer_wake_up_time( const block_timestamp_type& ref_block_time ) const {
   // if we have any producers then we should at least set a timer for our next available slot
   optional<fc::time_point> wake_up_time;
   for (const auto& p : _producers) {
      auto next_producer_block_time = calculate_next_block_time(p, ref_block_time);
      ilog("next block time: ${nbt}, producer size: ${size}",("nbt",next_producer_block_time)("size",_producers.size()));
      if (next_producer_block_time) {
         auto producer_wake_up_time = *next_producer_block_time - fc::microseconds(config::block_interval_us);
         if (wake_up_time) {
            // wake up with a full block interval to the deadline
            if( producer_wake_up_time < *wake_up_time ) {
               wake_up_time = producer_wake_up_time;
            }
         } else {
            wake_up_time = producer_wake_up_time;
         }
      }
   }
   if( !wake_up_time ) {
      fc_dlog(_log, "Not Scheduling Speculative/Production, no local producers had valid wake up times");
   }

   return wake_up_time;
}

void producer_plugin_impl::schedule_delayed_production_loop(const std::weak_ptr<producer_plugin_impl>& weak_this, optional<fc::time_point> wake_up_time) {
   if (wake_up_time) {
      fc_dlog(_log, "Scheduling Speculative/Production Change at ${time}", ("time", wake_up_time));
      static const boost::posix_time::ptime epoch(boost::gregorian::date(1970, 1, 1));
      _timer.expires_at(epoch + boost::posix_time::microseconds(wake_up_time->time_since_epoch().count()));
      _timer.async_wait( app().get_priority_queue().wrap( priority::high,
         [weak_this,cid=++_timer_corelation_id](const boost::system::error_code& ec) {
            auto self = weak_this.lock();
            if( self && ec != boost::asio::error::operation_aborted && cid == self->_timer_corelation_id ) {
               self->schedule_production_loop();
            }
         } ) );
   }
}


bool producer_plugin_impl::maybe_produce_block(bool is_backup) {
   auto reschedule = fc::make_scoped_exit([this]{
      schedule_production_loop();
   });

   try {
      produce_block(is_backup);
      return true;
   } LOG_AND_DROP();

   fc_dlog(_log, "Aborting block due to produce_block error");
   abort_block();
   return false;
}

static auto make_debug_time_logger() {
   auto start = fc::time_point::now();
   return fc::make_scoped_exit([=](){
      fc_dlog(_log, "Signing took ${ms}us", ("ms", fc::time_point::now() - start) );
   });
}

static auto maybe_make_debug_time_logger() -> fc::optional<decltype(make_debug_time_logger())> {
   if (_log.is_enabled( fc::log_level::debug ) ){
      return make_debug_time_logger();
   } else {
      return {};
   }
}

void producer_plugin_impl::produce_block(bool is_backup) {
   //ilog("produce_block ${t}", ("t", fc::time_point::now())); // for testing _produce_time_offset_us
   EOS_ASSERT(_pending_block_mode == pending_block_mode::producing, producer_exception, "called produce_block while not actually producing");
   chain::controller& chain = chain_plug->chain();
   const auto& hbs = chain.head_block_state();
   EOS_ASSERT(chain.is_building_block(), missing_pending_block_state, "pending_block_state does not exist but it should, another plugin may have corrupted it");


   const auto& auth = chain.pending_block_signing_authority();
   std::vector<std::reference_wrapper<const signature_provider_type>> relevant_providers;

   relevant_providers.reserve(_signature_providers.size());

   producer_authority::for_each_key(auth, [&](const public_key_type& key){
      const auto& iter = _signature_providers.find(key);
      if (iter != _signature_providers.end()) {
         relevant_providers.emplace_back(iter->second);
      }
   });

   EOS_ASSERT(relevant_providers.size() > 0, producer_priv_key_not_found, "Attempting to produce a block for which we don't have any relevant private keys");

   if (_protocol_features_signaled) {
      _protocol_features_to_activate.clear(); // clear _protocol_features_to_activate as it is already set in pending_block
      _protocol_features_signaled = false;
   }

   //idump( (fc::time_point::now() - chain.pending_block_time()) );
   chain.finalize_block( [&]( const digest_type& d ) {
      auto debug_logger = maybe_make_debug_time_logger();
      vector<signature_type> sigs;
      sigs.reserve(relevant_providers.size());

      // sign with all relevant public keys
      for (const auto& p : relevant_providers) {
         sigs.emplace_back(p.get()(d));
      }
      return sigs;
   },is_backup);

   chain.commit_block(is_backup);

   block_state_ptr new_bs = chain.head_block_state();

   ilog("Produced block ${id}... #${n} @ ${t} signed by ${p} [trxs: ${count}, lib: ${lib}, confirmed: ${confs}]",
        ("p",new_bs->header.producer)("id",new_bs->id.str().substr(8,16))
        ("n",new_bs->block_num)("t",new_bs->header.timestamp)
        ("count",new_bs->block->transactions.size())("lib",chain.last_irreversible_block_num())("confs", new_bs->header.confirmed));

}

void producer_plugin::log_failed_transaction(const transaction_id_type& trx_id, const char* reason) const {
   fc_dlog(_trx_failed_trace_log, "[TRX_TRACE] Speculative execution is REJECTING tx: ${txid} : ${why}",
           ("txid", trx_id)("why", reason));
}

} // namespace eosio
