#include <bts/blockchain/chain_database.hpp>
#include <bts/blockchain/chain_database_impl.hpp>
#include <bts/blockchain/checkpoints.hpp>
#include <bts/blockchain/config.hpp>
#include <bts/blockchain/exceptions.hpp>
#include <bts/blockchain/genesis_state.hpp>
#include <bts/blockchain/genesis_json.hpp>
#include <bts/blockchain/market_engine.hpp>
#include <bts/blockchain/time.hpp>

#include <fc/io/fstream.hpp>
#include <fc/io/raw_variant.hpp>
#include <fc/thread/non_preemptable_scope_check.hpp>
#include <fc/thread/unique_lock.hpp>

#include <iomanip>
#include <iostream>

#include <bts/blockchain/fork_blocks.hpp>

namespace bts { namespace blockchain {

   namespace detail
   {
      void chain_database_impl::revalidate_pending()
      {
            _pending_fee_index.clear();

            vector<transaction_id_type> trx_to_discard;

            _pending_trx_state = std::make_shared<pending_chain_state>( self->shared_from_this() );
            unsigned num_pending_transaction_considered = 0;
            auto itr = _pending_transaction_db.begin();
            while( itr.valid() )
            {
                signed_transaction trx = itr.value();
                const transaction_id_type trx_id = itr.key();
                assert(trx_id == trx.id());
                try
                {
                  transaction_evaluation_state_ptr eval_state = self->evaluate_transaction( trx, _relay_fee );
                  share_type fees = eval_state->get_fees();
                  _pending_fee_index[ fee_index( fees, trx_id ) ] = eval_state;
                  ilog( "revalidated pending transaction id ${id}", ("id", trx_id) );
                }
                catch ( const fc::canceled_exception& )
                {
                  throw;
                }
                catch ( const fc::exception& e )
                {
                  trx_to_discard.push_back(trx_id);
                  wlog( "discarding invalid transaction: ${id} ${e}",
                        ("id",trx_id)("e",e.to_detail_string()) );
                }
                ++num_pending_transaction_considered;
                ++itr;
            }

            for( const auto& item : trx_to_discard )
                _pending_transaction_db.remove( item );
            ilog("revalidate_pending complete, there are now ${pending_count} evaluated transactions, ${num_pending_transaction_considered} raw transactions",
                 ("pending_count", _pending_fee_index.size())
                 ("num_pending_transaction_considered", num_pending_transaction_considered));
      }

      void chain_database_impl::open_database( const fc::path& data_dir )
      { try {
          bool rebuild_index = false;

          if( !fc::exists(data_dir / "index" ) )
          {
              ilog("Rebuilding database index...");
              fc::create_directories( data_dir / "index" );
              rebuild_index = true;
          }

          _property_db.open( data_dir / "index/property_db" );
          auto database_version = _property_db.fetch_optional( chain_property_enum::database_version );
          if( !database_version || database_version->as_int64() < BTS_BLOCKCHAIN_DATABASE_VERSION )
          {
              if ( !rebuild_index )
              {
                wlog( "old database version, upgrade and re-sync" );
                _property_db.close();
                fc::remove_all( data_dir / "index" );
                fc::create_directories( data_dir / "index" );
                _property_db.open( data_dir / "index/property_db" );
                rebuild_index = true;
              }
              self->set_property( chain_property_enum::database_version, BTS_BLOCKCHAIN_DATABASE_VERSION );
          }
          else if( database_version && !database_version->is_null() && database_version->as_int64() > BTS_BLOCKCHAIN_DATABASE_VERSION )
          {
             FC_CAPTURE_AND_THROW( new_database_version, (database_version)(BTS_BLOCKCHAIN_DATABASE_VERSION) );
          }

          _fork_number_db.open( data_dir / "index/fork_number_db" );
          _fork_db.open( data_dir / "index/fork_db" );

          _block_id_to_undo_state.open( data_dir / "index/block_id_to_undo_state" );

          _block_id_to_block_record_db.open( data_dir / "index/block_id_to_block_record_db" );
          _block_num_to_id_db.open( data_dir / "raw_chain/block_num_to_id_db" );
          _block_id_to_block_data_db.open( data_dir / "raw_chain/block_id_to_block_data_db" );

          _account_id_to_record.open( data_dir / "index/account_id_to_record" );
          _account_name_to_id.open( data_dir / "index/account_name_to_id" );
          _account_address_to_id.open( data_dir / "index/account_address_to_id" );

          _asset_id_to_record.open( data_dir / "index/asset_id_to_record" );
          _asset_symbol_to_id.open( data_dir / "index/asset_symbol_to_id" );

          _balance_id_to_record.open( data_dir / "index/balance_id_to_record" );
          _empty_balance_id_to_record.open( data_dir / "index/empty_balance_id_to_record" );

          _id_to_transaction_record_db.open( data_dir / "index/id_to_transaction_record_db" );

          _slate_db.open( data_dir / "index/slate_db" );
          _market_transactions_db.open( data_dir / "index/market_transactions_db" );

          _pending_transaction_db.open( data_dir / "index/pending_transaction_db" );

          _address_to_trx_index.open( data_dir / "index/address_to_trx_db" );
          _burn_db.open( data_dir / "index/burn_db" );

          _slot_record_db.open( data_dir / "index/slot_record_db" );

          _feed_index_to_record.open( data_dir / "index/feed_index_to_record" );

          _ask_db.open( data_dir / "index/ask_db" );
          _bid_db.open( data_dir / "index/bid_db" );
          _relative_ask_db.open( data_dir / "index/relative_ask_db" );
          _relative_bid_db.open( data_dir / "index/relative_bid_db" );
          _short_db.open( data_dir / "index/short_db" );
          _collateral_db.open( data_dir / "index/collateral_db" );

          _object_db.open( data_dir / "index/object_db" );
          _edge_index.open( data_dir / "index/edge_index" );
          _reverse_edge_index.open( data_dir / "index/reverse_edge_index" );

          _market_status_db.open( data_dir / "index/market_status_db" );
          _market_history_db.open( data_dir / "index/market_history_db" );

          _auth_db.open( data_dir / "index/auth_db" );
          _asset_proposal_db.open( data_dir / "index/asset_proposal_db" );

          _pending_trx_state = std::make_shared<pending_chain_state>( self->shared_from_this() );

          _revalidatable_future_blocks_db.open( data_dir / "index/future_blocks_db" );
          clear_invalidation_of_future_blocks();
      } FC_CAPTURE_AND_RETHROW( (data_dir) ) }

      void chain_database_impl::populate_indexes()
      { try {
          for( auto iter = _account_id_to_record.unordered_begin();
               iter != _account_id_to_record.unordered_end(); ++iter )
          {
              const account_record& record = iter->second;
              if( !record.is_retracted() && record.is_delegate() )
                  _delegate_votes.emplace( record.net_votes(), record.id );
          }

          for( auto iter = _id_to_transaction_record_db.begin(); iter.valid(); ++iter )
          {
              const transaction& trx = iter.value().trx;
              if( trx.expiration > self->now() )
                  _unique_transactions.emplace( trx, _chain_id );
          }

          for( auto iter = _collateral_db.begin(); iter.valid(); ++iter )
          {
              const market_index_key& key = iter.key();
              const collateral_record& record = iter.value();
              const expiration_index index{ key.order_price.quote_asset_id, record.expiration, key };
              _collateral_expiration_index.insert( index );
          }

      } FC_CAPTURE_AND_RETHROW() }

      void chain_database_impl::clear_invalidation_of_future_blocks()
      {
        for (auto block_id_itr = _revalidatable_future_blocks_db.begin(); block_id_itr.valid(); ++block_id_itr)
        {
          mark_as_unchecked( block_id_itr.key() );
        }
      }

      digest_type chain_database_impl::initialize_genesis( const optional<path>& genesis_file )
      { try {
         digest_type chain_id = self->chain_id();
         if( chain_id != digest_type() )
         {
            wlog( "Genesis state already initialized" );
            return chain_id;
         }

         genesis_state config;
         if( !genesis_file.valid() )
         {
           std::cout << "Initializing genesis state from built-in genesis file\n";
           config = get_builtin_genesis_block_config();
           chain_id = get_builtin_genesis_block_state_hash();
         }
         else
         {
           std::cout << "Initializing genesis state from "<< genesis_file->generic_string() << "\n";
           FC_ASSERT( fc::exists( *genesis_file ), "Genesis file '${file}' was not found.", ("file", *genesis_file) );

           if( genesis_file->extension() == ".json" )
           {
              config = fc::json::from_file(*genesis_file).as<genesis_state>();
           }
           else if( genesis_file->extension() == ".dat" )
           {
              fc::ifstream in( *genesis_file );
              fc::raw::unpack( in, config );
           }
           else
           {
              FC_ASSERT( false, "Invalid genesis format '${format}'", ("format",genesis_file->extension() ) );
           }
           fc::sha256::encoder enc;
           fc::raw::pack( enc, config );
           chain_id = enc.result();
         }

         if( chain_id == BTS_EXPECTED_CHAIN_ID )
             chain_id = BTS_DESIRED_CHAIN_ID;

         _chain_id = chain_id;
         self->set_property( bts::blockchain::chain_id, variant( _chain_id ) );

         // Check genesis state
         FC_ASSERT( config.delegates.size() >= BTS_BLOCKCHAIN_NUM_DELEGATES,
                    "genesis.json does not contain enough initial delegates!",
                    ("required",BTS_BLOCKCHAIN_NUM_DELEGATES)("provided",config.delegates.size()) );

         const fc::time_point_sec timestamp = config.timestamp;

         // Initialize god account
         int32_t account_id = 0;
         account_record god;
         god.id = account_id;
         god.name = "GOD";
         self->store_account_record( god );

         // Initialize delegates
         for( const genesis_delegate& delegate : config.delegates )
         {
             ++account_id;
             account_record rec;
             rec.id = account_id;
             rec.name = delegate.name;
             rec.owner_key = delegate.owner;
             rec.set_active_key( timestamp, delegate.owner );
             rec.registration_date = timestamp;
             rec.last_update = timestamp;
             rec.delegate_info = delegate_stats();
             rec.delegate_info->pay_rate = 100;
             rec.set_signing_key( 0, delegate.owner );
             self->store_account_record( rec );
         }

         // For loading balances originally snapshotted from other chains
         const auto convert_raw_address = []( const string& raw_address ) -> address
         {
             static const vector<string> bts_prefixes{ "BTS", "KEY", "DVS", "XTS" };
             try
             {
                 return address( pts_address( raw_address ) );
             }
             catch( const fc::exception& )
             {
                 for( const string& prefix : bts_prefixes )
                 {
                     if( raw_address.find( prefix ) == 0 )
                     {
                         return address( BTS_ADDRESS_PREFIX + raw_address.substr( prefix.size() ) );
                     }
                 }
             }
             FC_THROW_EXCEPTION( invalid_pts_address, "Invalid raw address format!", ("raw_address",raw_address) );
         };

         // Initialize signature balances
         share_type total_base_supply = 0;
         for( const auto& genesis_balance : config.initial_balances )
         {
            const auto addr = convert_raw_address( genesis_balance.raw_address );
            balance_record initial_balance( addr, asset( genesis_balance.balance, 0 ), 0 );

            /* In case of redundant balances */
            const auto cur = self->get_balance_record( initial_balance.id() );
            if( cur.valid() ) initial_balance.balance += cur->balance;

            initial_balance.snapshot_info = snapshot_record( genesis_balance.raw_address, genesis_balance.balance );
            initial_balance.last_update = config.timestamp;
            self->store_balance_record( initial_balance );

            total_base_supply += genesis_balance.balance;
         }

         // Initialize vesting balances
         for( const auto& genesis_balance : config.sharedrop_balances.vesting_balances )
         {
            withdraw_vesting vesting;
            vesting.owner = convert_raw_address( genesis_balance.raw_address );
            vesting.start_time = config.sharedrop_balances.start_time;
            vesting.duration = fc::days( config.sharedrop_balances.duration_days ).to_seconds();
            vesting.original_balance = genesis_balance.balance;

            withdraw_condition condition( vesting, 0, 0 );
            balance_record initial_balance( condition );
            initial_balance.balance = vesting.original_balance;

            /* In case of redundant balances */
            const auto cur = self->get_balance_record( initial_balance.id() );
            if( cur.valid() ) initial_balance.balance += cur->balance;

            initial_balance.snapshot_info = snapshot_record( genesis_balance.raw_address, genesis_balance.balance );
            initial_balance.last_update = vesting.start_time;
            self->store_balance_record( initial_balance );

            total_base_supply += genesis_balance.balance;
         }

         // Initialize base asset
         int32_t asset_id = 0;
         asset_record base_asset;
         base_asset.id = asset_id;
         base_asset.symbol = BTS_BLOCKCHAIN_SYMBOL;
         base_asset.name = BTS_BLOCKCHAIN_NAME;
         base_asset.description = BTS_BLOCKCHAIN_DESCRIPTION;
         base_asset.public_data = variant("");
         base_asset.issuer_account_id = god.id;
         base_asset.precision = BTS_BLOCKCHAIN_PRECISION;
         base_asset.registration_date = timestamp;
         base_asset.last_update = timestamp;
         base_asset.current_share_supply = total_base_supply;
         base_asset.maximum_share_supply = BTS_BLOCKCHAIN_MAX_SHARES;
         base_asset.collected_fees = 0;
         base_asset.flags = asset_permissions::none;
         base_asset.issuer_permissions = asset_permissions::none;
         self->store_asset_record( base_asset );

         // Initialize initial market assets
         for( const genesis_asset& asset : config.market_assets )
         {
             ++asset_id;
             asset_record rec;
             rec.id = asset_id;
             rec.symbol = asset.symbol;
             rec.name = asset.name;
             rec.description = asset.description;
             rec.public_data = variant("");
             rec.issuer_account_id = asset_record::market_issuer_id;
             rec.precision = asset.precision;
             rec.registration_date = timestamp;
             rec.last_update = timestamp;
             rec.current_share_supply = 0;
             rec.maximum_share_supply = BTS_BLOCKCHAIN_MAX_SHARES;
             rec.collected_fees = 0;
             self->store_asset_record( rec );
         }

         //add fork_data for the genesis block to the fork database
         block_fork_data gen_fork;
         gen_fork.is_valid = true;
         gen_fork.is_included = true;
         gen_fork.is_linked = true;
         gen_fork.is_known = true;
         _fork_db.store( block_id_type(), gen_fork );

         self->set_property( chain_property_enum::active_delegate_list_id, fc::variant( self->next_round_active_delegates() ) );
         self->set_property( chain_property_enum::last_asset_id, asset_id );
         self->set_property( chain_property_enum::last_account_id, uint64_t( config.delegates.size() ) );
         self->set_property( chain_property_enum::last_object_id, 0 );

         self->sanity_check();
         return _chain_id;
      } FC_RETHROW_EXCEPTIONS( warn, "" ) }

      std::vector<block_id_type> chain_database_impl::fetch_blocks_at_number( uint32_t block_num )
      {
         std::vector<block_id_type> current_blocks;
         auto itr = _fork_number_db.find( block_num );
         if( itr.valid() ) return itr.value();
         return current_blocks;
      }

      void chain_database_impl::clear_pending( const full_block& blk )
      {
         for( const signed_transaction& trx : blk.user_transactions )
            _pending_transaction_db.remove( trx.id() );

         _pending_fee_index.clear();

         // this schedules the revalidate-pending-transactions task to execute in this thread
         // as soon as this current task (probably pushing a block) gets around to yielding.
         // This was changed from waiting on the old _revalidate_pending to prevent yielding
         // during the middle of pushing a block.  If that happens, the database is in an
         // inconsistent state and it confuses the p2p network code.
         // We skip this step if we are dealing with blocks prior to the last checkpointed block
         uint32_t last_checkpoint_block_num = 0;
         if( !CHECKPOINT_BLOCKS.empty() )
             last_checkpoint_block_num = (--(CHECKPOINT_BLOCKS.end()))->first;
         if( (!_revalidate_pending.valid() || _revalidate_pending.ready()) &&
             _head_block_header.block_num >= last_checkpoint_block_num )
           _revalidate_pending = fc::async( [=](){ revalidate_pending(); }, "revalidate_pending" );

         _pending_trx_state = std::make_shared<pending_chain_state>( self->shared_from_this() );
      }

      std::pair<block_id_type, block_fork_data> chain_database_impl::recursive_mark_as_linked(const std::unordered_set<block_id_type>& ids)
      {
         block_fork_data longest_fork;
         uint32_t highest_block_num = 0;
         block_id_type last_block_id;

         std::unordered_set<block_id_type> next_ids = ids;
         //while there are any next blocks for current block number being processed
         while( next_ids.size() )
         {
            std::unordered_set<block_id_type> pending; //builds list of all next blocks for the current block number being processed
            //mark as linked all blocks at the current block number being processed
            for( const auto& item : next_ids )
            {
                block_fork_data record = _fork_db.fetch( item );
                record.is_linked = true;
                pending.insert( record.next_blocks.begin(), record.next_blocks.end() );
                //ilog( "store: ${id} => ${data}", ("id",item)("data",record) );
                _fork_db.store( item, record );

                //keep one of the block ids of the current block number being processed (simplify this code)
                auto block_record = _block_id_to_block_record_db.fetch(item);
                if( block_record.block_num > highest_block_num )
                {
                    highest_block_num = block_record.block_num;
                    last_block_id = item;
                    longest_fork = record;
                }
            }
            next_ids = pending; //conceptually this increments the current block number being processed
         }

         return std::make_pair(last_block_id, longest_fork);
      }
      void chain_database_impl::recursive_mark_as_invalid(const std::unordered_set<block_id_type>& ids , const fc::exception& reason)
      {
         std::unordered_set<block_id_type> next_ids = ids;
         while( next_ids.size() )
         {
            std::unordered_set<block_id_type> pending;
            for( const auto& item : next_ids )
            {
                block_fork_data record = _fork_db.fetch( item );
                assert(!record.valid()); //make sure we don't invalidate a previously validated record
                record.is_valid = false;
                record.invalid_reason = reason;
                pending.insert( record.next_blocks.begin(), record.next_blocks.end() );
                //ilog( "store: ${id} => ${data}", ("id",item)("data",record) );
                _fork_db.store( item, record );
            }
            next_ids = pending;
         }
      }

      /**
       *  Place the block in the block tree, the block tree contains all blocks
       *  and tracks whether they are valid, linked, and current.
       *
       *  There are several options for this block:
       *
       *  1) It extends an existing block
       *      - a valid chain
       *      - an invalid chain
       *      - an unlinked chain
       *  2) It is free floating and doesn't link to anything we have
       *      - create two entries into the database
       *          - one for this block
       *          - placeholder for previous
       *      - mark both as unlinked
       *  3) It provides the missing link between the genesis block and an existing chain
       *      - all next blocks need to be updated to change state to 'linked'
       *
       *  Returns the pair of the block id and block_fork_data of the block with the highest block number
       *  in the fork which contains the new block, in all of the above cases where the new block is linked;
       *  otherwise, returns the block id and fork data of the new block
       */
      std::pair<block_id_type, block_fork_data> chain_database_impl::store_and_index( const block_id_type& block_id,
                                                                                      const full_block& block_data )
      { try {
          //we should never try to store a block we've already seen (verify not in any of our databases)
          assert(!_block_id_to_block_data_db.fetch_optional(block_id));
          #ifndef NDEBUG
          {
            //check block id is not in fork_data, or if it is, make sure it's just a placeholder for block we are waiting for
            optional<block_fork_data> fork_data = _fork_db.fetch_optional(block_id);
            assert(!fork_data || !fork_data->is_known);
            //check block not in parallel_blocks database
            vector<block_id_type> parallel_blocks = fetch_blocks_at_number( block_data.block_num );
            assert( std::find(parallel_blocks.begin(), parallel_blocks.end(), block_id) == parallel_blocks.end());
          }
          #endif

          // first of all store this block at the given block number
          _block_id_to_block_data_db.store( block_id, block_data );

          if( !self->get_block_record( block_id ).valid() )
          {
              block_record record;
              digest_block& temp = record;
              temp = block_data;
              record.block_size = block_data.block_size();
              record.latency = blockchain::now() - block_data.timestamp;
              _block_id_to_block_record_db.store( block_id, record );
          }

          // update the parallel block list (fork_number_db):
          // get vector of all blocks with same block number, add this block to that list, then update the database
          vector<block_id_type> parallel_blocks = fetch_blocks_at_number( block_data.block_num );
          //if block not in parallel block list, add it
          if (std::find( parallel_blocks.begin(), parallel_blocks.end(), block_id ) == parallel_blocks.end())
          {
            // TODO: do we need to execute any of the rest of this function (or, for that matter, its caller) if the block is already there
            parallel_blocks.push_back( block_id );
            _fork_number_db.store( block_data.block_num, parallel_blocks );
          }

          // Tell our previous block that we are one of it's next blocks (update previous block's next_blocks set)
          block_fork_data prev_fork_data;
          auto prev_itr = _fork_db.find( block_data.previous );
          if( prev_itr.valid() ) // we already know about its previous (note: we always know about genesis block)
          {
             ilog( "           we already know about its previous: ${p}", ("p",block_data.previous) );
             prev_fork_data = prev_itr.value();
             prev_fork_data.next_blocks.insert(block_id);
             //ilog( "              ${id} = ${record}", ("id",prev_itr.key())("record",prev_fork_data) );
             _fork_db.store( prev_itr.key(), prev_fork_data );
          }
          else //if we don't know about the previous block even as a placeholder, create a placeholder for the previous block (placeholder block defaults as unlinked)
          {
             elog( "           we don't know about its previous: ${p}", ("p",block_data.previous) );
             prev_fork_data.next_blocks.insert(block_id); //tell placeholder block about new block
             prev_fork_data.is_linked = false; //this is only a placeholder, we don't know what its previous block is, so it can't be linked
             //ilog( "              ${id} = ${record}", ("id",block_data.previous)("record",prev_fork_data) );
             _fork_db.store( block_data.previous, prev_fork_data );
          }

          auto cur_itr = _fork_db.find( block_id );
          if( cur_itr.valid() ) //if placeholder was previously created for block
          {
            block_fork_data current_fork = cur_itr.value();
            current_fork.is_known = true; //was placeholder, now a known block
            ilog( "          current_fork: ${fork}", ("fork",current_fork) );
            ilog( "          prev_fork: ${prev_fork}", ("prev_fork",prev_fork_data) );
            // if new block is linked to genesis block, recursively mark all its next blocks as linked and return longest descendant block
            assert(!current_fork.is_linked);
            if( prev_fork_data.is_linked )
            {
              current_fork.is_linked = true;
              //if previous block is invalid, mark the new block as invalid too (block can't be valid if any previous block in its chain is invalid)
              bool prev_block_is_invalid = prev_fork_data.is_valid && !*prev_fork_data.is_valid;
              if (prev_block_is_invalid)
              {
                current_fork.is_valid = false;
                current_fork.invalid_reason = prev_fork_data.invalid_reason;
              }
              _fork_db.store( block_id, current_fork ); //update placeholder fork_block record with block data
              if (prev_block_is_invalid) //if previous block was invalid, mark all descendants as invalid and return current_block
              {
                recursive_mark_as_invalid(current_fork.next_blocks, *prev_fork_data.invalid_reason );
                return std::make_pair(block_id, current_fork);
              }
              else //we have a potentially viable alternate chain, mark the descendant blocks as linked and return the longest end block from descendant chains
              {
                std::pair<block_id_type,block_fork_data> longest_fork = recursive_mark_as_linked(current_fork.next_blocks);
                return longest_fork;
              }
            }
            else //this new block is not linked to genesis block, so no point in determining its longest descendant block, just return it and let it be skipped over
            {
              _fork_db.store( block_id, current_fork ); //update placeholder fork_block record with block data
              return std::make_pair(block_id, current_fork);
            }
          }
          else //no placeholder exists for this new block, just set its link flag
          {
            block_fork_data current_fork;
            current_fork.is_known = true;
            current_fork.is_linked = prev_fork_data.is_linked; //is linked if it's previous block is linked
            bool prev_block_is_invalid = prev_fork_data.is_valid && !*prev_fork_data.is_valid;
            if (prev_block_is_invalid)
            {
              current_fork.is_valid = false;
              current_fork.invalid_reason = prev_fork_data.invalid_reason;
            }
            //ilog( "          current_fork: ${id} = ${fork}", ("id",block_id)("fork",current_fork) );
            _fork_db.store( block_id, current_fork ); //add new fork_block record to database
            //this is first time we've seen this block mentioned, so we don't know about any linked descendants from it,
            //and therefore this is the last block in this chain that we know about, so just return that
            return std::make_pair(block_id, current_fork);
          }
      } FC_CAPTURE_AND_RETHROW( (block_id) ) }

      void chain_database_impl::mark_invalid(const block_id_type& block_id , const fc::exception& reason)
      {
         // fetch the fork data for block_id, mark it as invalid and
         // then mark every item after it as invalid as well.
         auto fork_data = _fork_db.fetch( block_id );
         assert(!fork_data.valid()); //make sure we're not invalidating a block that we previously have validated
         fork_data.is_valid = false;
         fork_data.invalid_reason = reason;
         _fork_db.store( block_id, fork_data );
         recursive_mark_as_invalid( fork_data.next_blocks, reason );
      }

      void chain_database_impl::mark_as_unchecked(const block_id_type& block_id)
      {
        // fetch the fork data for block_id, mark it as unchecked
        auto fork_data = _fork_db.fetch( block_id );
        assert(!fork_data.valid()); //make sure we're not unchecking a block that we previously have validated
        fork_data.is_valid.reset(); //mark as unchecked (i.e. we will check validity again later during switch_to_fork)
        fork_data.invalid_reason.reset();
        dlog( "store: ${id} => ${data}", ("id",block_id)("data",fork_data) );
        _fork_db.store( block_id, fork_data );
        // then mark every block after it as unchecked as well.
        std::unordered_set<block_id_type>& next_ids = fork_data.next_blocks;
        while( next_ids.size() )
        {
          std::unordered_set<block_id_type> pending_blocks_for_next_loop_iteration;
          for( const auto& next_block_id : next_ids )
          {
            block_fork_data record = _fork_db.fetch( next_block_id );
            record.is_valid.reset(); //mark as unchecked (i.e. we will check validity again later during switch_to_fork)
            record.invalid_reason.reset();
            pending_blocks_for_next_loop_iteration.insert( record.next_blocks.begin(), record.next_blocks.end() );
            dlog( "store: ${id} => ${data}", ("id",next_block_id)("data",record) );
            _fork_db.store( next_block_id, record );
          }
          next_ids = pending_blocks_for_next_loop_iteration;
        }
      }

      void chain_database_impl::mark_included( const block_id_type& block_id, bool included )
      { try {
         //ilog( "included: ${block_id} = ${state}", ("block_id",block_id)("state",included) );
         auto fork_data = _fork_db.fetch( block_id );
         //if( fork_data.is_included != included )
         {
            fork_data.is_included = included;
            if( included )
            {
               fork_data.is_valid  = true;
            }
            //ilog( "store: ${id} => ${data}", ("id",block_id)("data",fork_data) );
            _fork_db.store( block_id, fork_data );
         }
         // fetch the fork data for block_id, mark it as included and
      } FC_RETHROW_EXCEPTIONS( warn, "", ("block_id",block_id)("included",included) ) }

      void chain_database_impl::switch_to_fork( const block_id_type& block_id )
      { try {
         if (block_id == _head_block_id) //if block_id is current head block, do nothing
           return; //this is necessary to avoid unnecessarily popping the head block in this case

         ilog( "switch from fork ${id} to ${to_id}", ("id",_head_block_id)("to_id",block_id) );
         vector<block_id_type> history = get_fork_history( block_id );
         while( history.back() != _head_block_id )
         {
            ilog( "    pop ${id}", ("id",_head_block_id) );
            pop_block();
         }
         for( int32_t i = history.size()-2; i >= 0 ; --i )
         {
            ilog( "    extend ${i}", ("i",history[i]) );
            extend_chain( self->get_block( history[i] ) );
         }
      } FC_CAPTURE_AND_RETHROW( (block_id) ) }

      void chain_database_impl::apply_transactions( const full_block& block,
                                                    const pending_chain_state_ptr& pending_state )
      {
         ilog( "Applying transactions from block: ${n}", ("n",block.block_num) );
         uint32_t trx_num = 0;
         try
         {
            for( const auto& trx : block.user_transactions )
            {
               transaction_evaluation_state_ptr trx_eval_state = std::make_shared<transaction_evaluation_state>( pending_state.get() );
               trx_eval_state->evaluate( trx, _skip_signature_verification, false );

               // TODO:  capture the evaluation state with a callback for wallets...
               // summary.transaction_states.emplace_back( std::move(trx_eval_state) );

               transaction_location trx_loc( block.block_num, trx_num );
               transaction_record record( trx_loc, *trx_eval_state);
               pending_state->store_transaction( trx.id(), record );
               ++trx_num;
            }
         } FC_RETHROW_EXCEPTIONS( warn, "", ("trx_num",trx_num) )
      }

      void chain_database_impl::pay_delegate( const pending_chain_state_ptr& pending_state, const public_key_type& block_signee,
                                              const block_id_type& block_id, block_record& record )
      { try {
          if( pending_state->get_head_block_num() < BTS_V0_4_28_FORK_BLOCK_NUM )
              return pay_delegate_v2( pending_state, block_signee, block_id, record );

          oasset_record base_asset_record = pending_state->get_asset_record( asset_id_type( 0 ) );
          FC_ASSERT( base_asset_record.valid() );

          oaccount_record delegate_record = self->get_account_record( address( block_signee ) );
          FC_ASSERT( delegate_record.valid() );
          delegate_record = pending_state->get_account_record( delegate_record->id );
          FC_ASSERT( delegate_record.valid() && delegate_record->is_delegate() && delegate_record->delegate_info.valid() );

          const uint8_t pay_rate_percent = delegate_record->delegate_info->pay_rate;
          FC_ASSERT( pay_rate_percent >= 0 && pay_rate_percent <= 100 );

          const share_type max_new_shares = self->get_max_delegate_pay_issued_per_block();
          const share_type accepted_new_shares = (max_new_shares * pay_rate_percent) / 100;
          FC_ASSERT( max_new_shares >= 0 && accepted_new_shares >= 0 );
          base_asset_record->current_share_supply += accepted_new_shares;

          static const uint32_t blocks_per_two_weeks = 14 * BTS_BLOCKCHAIN_BLOCKS_PER_DAY;
          const share_type max_collected_fees = base_asset_record->collected_fees / blocks_per_two_weeks;
          const share_type accepted_collected_fees = (max_collected_fees * pay_rate_percent) / 100;
          const share_type destroyed_collected_fees = max_collected_fees - accepted_collected_fees;
          FC_ASSERT( max_collected_fees >= 0 && accepted_collected_fees >= 0 && destroyed_collected_fees >= 0 );
          base_asset_record->collected_fees -= max_collected_fees;
          base_asset_record->current_share_supply -= destroyed_collected_fees;

          const share_type accepted_paycheck = accepted_new_shares + accepted_collected_fees;
          FC_ASSERT( accepted_paycheck >= 0 );
          delegate_record->delegate_info->votes_for += accepted_paycheck;
          delegate_record->delegate_info->pay_balance += accepted_paycheck;
          delegate_record->delegate_info->total_paid += accepted_paycheck;

          pending_state->store_account_record( *delegate_record );
          pending_state->store_asset_record( *base_asset_record );

          record.signee_shares_issued = accepted_new_shares;
          record.signee_fees_collected = accepted_collected_fees;
          record.signee_fees_destroyed = destroyed_collected_fees;
      } FC_CAPTURE_AND_RETHROW( (block_signee)(block_id)(record) ) }

      void chain_database_impl::save_undo_state( const block_id_type& block_id,
                                                 const pending_chain_state_ptr& pending_state )
      { try {
          uint32_t last_checkpoint_block_num = 0;
          if( !CHECKPOINT_BLOCKS.empty() )
              last_checkpoint_block_num = (--(CHECKPOINT_BLOCKS.end()))->first;
          if( _head_block_header.block_num < last_checkpoint_block_num )
                return;  // don't bother saving it...

          pending_chain_state_ptr undo_state = std::make_shared<pending_chain_state>( nullptr );
          undo_state->set_chain_id( _chain_id );
          pending_state->get_undo_state( undo_state );

          const int32_t old_block_num = self->get_head_block_num() - BTS_BLOCKCHAIN_MAX_UNDO_HISTORY;
          if( old_block_num > 0 )
          {
             const auto old_block_id = self->get_block_id( old_block_num );
             _block_id_to_undo_state.remove( old_block_id );
          }

          _block_id_to_undo_state.store( block_id, *undo_state );
      } FC_CAPTURE_AND_RETHROW( (block_id) ) }

      void chain_database_impl::verify_header( const full_block& block_data, const public_key_type& block_signee )
      { try {
          // validate preliminaries:
          if( block_data.block_num > 1 && block_data.block_num != _head_block_header.block_num + 1 )
             FC_CAPTURE_AND_THROW( block_numbers_not_sequential, (block_data)(_head_block_header) );
          if( block_data.previous  != _head_block_id )
             FC_CAPTURE_AND_THROW( invalid_previous_block_id, (block_data)(_head_block_id) );
          if( block_data.timestamp.sec_since_epoch() % BTS_BLOCKCHAIN_BLOCK_INTERVAL_SEC != 0 )
             FC_CAPTURE_AND_THROW( invalid_block_time );
          if( block_data.block_num > 1 && block_data.timestamp <= _head_block_header.timestamp )
             FC_CAPTURE_AND_THROW( time_in_past, (block_data.timestamp)(_head_block_header.timestamp) );

          fc::time_point_sec now = bts::blockchain::now();
          auto delta_seconds = (block_data.timestamp - now).to_seconds();
          if( block_data.timestamp >  (now + BTS_BLOCKCHAIN_BLOCK_INTERVAL_SEC*2) )
              FC_CAPTURE_AND_THROW( time_in_future, (block_data.timestamp)(now)(delta_seconds) );

          digest_block digest_data(block_data);
          if( NOT digest_data.validate_digest() )
            FC_CAPTURE_AND_THROW( invalid_block_digest );

          FC_ASSERT( digest_data.validate_unique() );

          // signing delegate:
          auto expected_delegate = self->get_slot_signee( block_data.timestamp, self->get_active_delegates() );

          if( block_signee != expected_delegate.signing_key() )
             FC_CAPTURE_AND_THROW( invalid_delegate_signee, (expected_delegate.id) );
      } FC_CAPTURE_AND_RETHROW( (block_data) ) }

      void chain_database_impl::update_head_block( const full_block& block_data )
      { try {
          _head_block_header = block_data;
          _head_block_id = block_data.id();
      } FC_CAPTURE_AND_RETHROW() }

      /**
       *  A block should be produced every BTS_BLOCKCHAIN_BLOCK_INTERVAL_SEC. If we do not have a
       *  block for any multiple of this interval between produced_block and the current head block
       *  then we need to lookup the delegates that should have produced a block during that interval
       *  and increment their blocks_missed.
       *
       *  We also need to increment the blocks_produced for the delegate that actually produced the
       *  block.
       *
       *  Note that produced_block has already been verified by the caller and that updates are
       *  applied to pending_state.
       */
      void chain_database_impl::update_delegate_production_info( const full_block& produced_block,
                                                                 const pending_chain_state_ptr& pending_state,
                                                                 const public_key_type& block_signee )
      { try {
          /* Update production info for signing delegate */
          account_id_type delegate_id = self->get_delegate_record_for_signee( block_signee ).id;
          oaccount_record delegate_record = pending_state->get_account_record( delegate_id );
          FC_ASSERT( delegate_record.valid() && delegate_record->is_delegate() && delegate_record->delegate_info.valid() );
          delegate_stats& delegate_info = *delegate_record->delegate_info;

          /* Validate secret */
          if( delegate_info.next_secret_hash.valid() )
          {
              const secret_hash_type hash_of_previous_secret = fc::ripemd160::hash( produced_block.previous_secret );
              FC_ASSERT( hash_of_previous_secret == *delegate_info.next_secret_hash,
                         "",
                         ("previous_secret",produced_block.previous_secret)
                         ("hash_of_previous_secret",hash_of_previous_secret)
                         ("delegate_record",delegate_record) );
          }

          delegate_info.blocks_produced += 1;
          delegate_info.next_secret_hash = produced_block.next_secret_hash;
          delegate_info.last_block_num_produced = produced_block.block_num;
          pending_state->store_account_record( *delegate_record );

          if( _track_stats )
          {
             const slot_record slot( produced_block.timestamp, delegate_id, produced_block.id() );
             pending_state->store_slot_record( slot );
          }

          /* Update production info for missing delegates */

          uint64_t required_confirmations = self->get_required_confirmations();

          time_point_sec block_timestamp;
          auto head_block = self->get_head_block();
          if( head_block.block_num > 0 ) block_timestamp = head_block.timestamp + BTS_BLOCKCHAIN_BLOCK_INTERVAL_SEC;
          else block_timestamp = produced_block.timestamp;
          const auto& active_delegates = self->get_active_delegates();

          for( ; block_timestamp < produced_block.timestamp;
                 block_timestamp += BTS_BLOCKCHAIN_BLOCK_INTERVAL_SEC,
                 required_confirmations += 2 )
          {
              /* Note: Active delegate list has not been updated yet so we can use the timestamp */
              delegate_id = self->get_slot_signee( block_timestamp, active_delegates ).id;
              delegate_record = pending_state->get_account_record( delegate_id );
              FC_ASSERT( delegate_record.valid() && delegate_record->is_delegate() );

              delegate_record->delegate_info->blocks_missed += 1;
              pending_state->store_account_record( *delegate_record );

             if( _track_stats )
                 pending_state->store_slot_record( slot_record( block_timestamp, delegate_id )  );
          }

          /* Update required confirmation count */

          required_confirmations -= 1;
          if( required_confirmations < 1 ) required_confirmations = 1;
          if( required_confirmations > BTS_BLOCKCHAIN_NUM_DELEGATES*3 )
             required_confirmations = 3*BTS_BLOCKCHAIN_NUM_DELEGATES;

          pending_state->set_property( confirmation_requirement, required_confirmations );
      } FC_CAPTURE_AND_RETHROW( (block_signee) ) }

      void chain_database_impl::update_random_seed( const secret_hash_type& new_secret,
                                                    const pending_chain_state_ptr& pending_state,
                                                    block_record& record )
      { try {
         const auto current_seed = pending_state->get_current_random_seed();
         fc::sha512::encoder enc;
         fc::raw::pack( enc, new_secret );
         fc::raw::pack( enc, current_seed );
         const auto& new_seed = fc::ripemd160::hash( enc.result() );
         pending_state->set_property( last_random_seed_id, variant( new_seed ) );
         record.random_seed = new_seed;
      } FC_CAPTURE_AND_RETHROW( (new_secret) ) }

      void chain_database_impl::update_active_delegate_list( const full_block& block_data,
                                                             const pending_chain_state_ptr& pending_state )
      { try {
          if( block_data.block_num % BTS_BLOCKCHAIN_NUM_DELEGATES != 0 )
              return;

          auto active_del = self->next_round_active_delegates();
          const size_t num_del = active_del.size();

          // Perform a random shuffle of the sorted delegate list.
          fc::sha256 rand_seed = fc::sha256::hash( pending_state->get_current_random_seed() );
          for( uint32_t i = 0; i < num_del; ++i )
          {
             for( uint32_t x = 0; x < 4 && i < num_del; ++x, ++i )
                std::swap( active_del[ i ], active_del[ rand_seed._hash[ x ] % num_del ] );
             rand_seed = fc::sha256::hash( rand_seed );
          }

          pending_state->set_active_delegates( active_del );
      } FC_CAPTURE_AND_RETHROW() }

      void chain_database_impl::execute_markets( const fc::time_point_sec& timestamp, const pending_chain_state_ptr& pending_state )
      { try {
        if( pending_state->get_head_block_num() < BTS_V0_4_29_FORK_BLOCK_NUM )
           return execute_markets_v1( timestamp, pending_state );

        vector<market_transaction> market_transactions;

        const auto dirty_markets = self->get_dirty_markets();
        for( const auto& market_pair : dirty_markets )
        {
           FC_ASSERT( market_pair.first > market_pair.second );
           market_engine engine( pending_state, *this );
           if( engine.execute( market_pair.first, market_pair.second, timestamp ) )
           {
              if( _track_stats )
                 market_transactions.insert( market_transactions.end(), engine._market_transactions.begin(), engine._market_transactions.end() );
           }
        }
        if( _track_stats )
           pending_state->set_market_transactions( std::move( market_transactions ) );
      } FC_CAPTURE_AND_RETHROW( (timestamp) ) }

      /**
       *  Performs all of the block validation steps and throws if error.
       */
      void chain_database_impl::extend_chain( const full_block& block_data )
      { try {
         const time_point& start_time = time_point::now();
         const block_id_type& block_id = block_data.id();
         block_summary summary;
         try
         {
            public_key_type block_signee;
            if( CHECKPOINT_BLOCKS.size() > 0 && (--CHECKPOINT_BLOCKS.end())->first > block_data.block_num )
               //Skip signature validation
               block_signee = self->get_slot_signee( block_data.timestamp, self->get_active_delegates() ).signing_key();
            else
               /* We need the block_signee's key in several places and computing it is expensive, so compute it here and pass it down */
               block_signee = block_data.signee( false );

            auto checkpoint_itr = CHECKPOINT_BLOCKS.find(block_data.block_num);
            if( checkpoint_itr != CHECKPOINT_BLOCKS.end() && checkpoint_itr->second != block_id )
              FC_CAPTURE_AND_THROW( failed_checkpoint_verification, (block_id)(checkpoint_itr->second) );

            /* Note: Secret is validated later in update_delegate_production_info() */
            verify_header( block_data, block_signee );

            summary.block_data = block_data;

            /* Create a pending state to track changes that would apply as we evaluate the block */
            pending_chain_state_ptr pending_state = std::make_shared<pending_chain_state>( self->shared_from_this() );
            summary.applied_changes = pending_state;

            /** Increment the blocks produced or missed for all delegates. This must be done
             *  before applying transactions because it depends upon the current active delegate order.
             **/
            update_delegate_production_info( block_data, pending_state, block_signee );

            oblock_record block_record = self->get_block_record( block_id );
            FC_ASSERT( block_record.valid() );

            pay_delegate( pending_state, block_signee, block_id, *block_record );

            if( block_data.block_num < BTS_V0_4_9_FORK_BLOCK_NUM )
                apply_transactions( block_data, pending_state );

            execute_markets( block_data.timestamp, pending_state );

            if( block_data.block_num >= BTS_V0_4_9_FORK_BLOCK_NUM )
                apply_transactions( block_data, pending_state );

            update_active_delegate_list( block_data, pending_state );

            update_random_seed( block_data.previous_secret, pending_state, *block_record );

            save_undo_state( block_id, pending_state );

            // TODO: verify that apply changes can be called any number of
            // times without changing the database other than the first
            // attempt.
            pending_state->apply_changes();

            mark_included( block_id, true );

            update_head_block( block_data );

            clear_pending( block_data );

            _block_num_to_id_db.store( block_data.block_num, block_id );

            block_record->processing_time = time_point::now() - start_time;
            _block_id_to_block_record_db.store( block_id, *block_record );

            if( block_data.block_num == BTS_V0_4_16_FORK_BLOCK_NUM )
            {
                auto base_asset_record = self->get_asset_record( asset_id_type( 0 ) );
                FC_ASSERT( base_asset_record.valid() );
                base_asset_record->current_share_supply = self->calculate_supply( asset_id_type( 0 ) ).amount;
                self->store_asset_record( *base_asset_record );
            }
            else if( block_data.block_num == BTS_V0_4_17_FORK_BLOCK_NUM
                     || block_data.block_num == BTS_V0_4_21_FORK_BLOCK_NUM
                     || block_data.block_num == BTS_V0_4_24_FORK_BLOCK_NUM )
            {
                vector<asset_record> records;
                records.reserve( _asset_id_to_record.size() );

                const auto scan_asset = [ &records ]( const asset_record& record )
                {
                    records.push_back( record );
                };
                self->scan_unordered_assets( scan_asset );

                wlog( "Recalculating supply for ${x} assets", ("x",records.size()) );
                for( auto& record : records )
                {
                    asset supply = self->calculate_supply( record.id );
                    share_type fees = record.collected_fees;

                    if( record.is_market_issued() )
                    {
                        asset debt = self->calculate_debt( record.id );
                        if( supply != debt )
                        {
                            const share_type difference = debt.amount - supply.amount;
                            supply.amount += difference;
                            fees += difference;
                        }
                    }

                    record.current_share_supply = supply.amount;
                    record.collected_fees = fees;
                    self->store_asset_record( record );
                }
            }

            if( block_data.block_num == BTS_V0_4_24_FORK_BLOCK_NUM )
            {
                vector<account_record> records;
                records.reserve( 5343 );

                const auto scan_account = [ &records ]( const account_record& record )
                {
                    if( record.is_delegate() )
                        records.push_back( record );
                };
                self->scan_unordered_accounts( scan_account );

                wlog( "Resetting pay rates for ${x} delegates", ("x",records.size()) );
                for( auto& record : records )
                {
                    record.delegate_info->pay_rate = 3;
                    self->store_account_record( record );
                }
            }
         }
         catch ( const fc::exception& e )
         {
            wlog( "error applying block: ${e}", ("e",e.to_detail_string() ));
            mark_invalid( block_id, e );
            throw;
         }

         // Purge expired transactions from unique cache
         auto iter = _unique_transactions.begin();
         while( iter != _unique_transactions.end() && iter->expiration <= self->now() )
             iter = _unique_transactions.erase( iter );

         //Schedule the observer notifications for later; the chain is in a
         //non-premptable state right now, and observers may yield.
         if( (now() - block_data.timestamp).to_seconds() < BTS_BLOCKCHAIN_BLOCK_INTERVAL_SEC )
           for( chain_observer* o : _observers )
              fc::async([o,summary]{o->block_applied( summary );}, "call_block_applied_observer");
      } FC_CAPTURE_AND_RETHROW( (block_data) ) }

      /**
       * Traverse the previous links of all blocks in fork until we find one that is_included
       *
       * The last item in the result will be the only block id that is already included in
       * the blockchain.
       */
      std::vector<block_id_type> chain_database_impl::get_fork_history( const block_id_type& id )
      { try {
         ilog( "" );
         std::vector<block_id_type> history;
         history.push_back( id );

         block_id_type next_id = id;
         while( true )
         {
            auto header = self->get_block_header( next_id );
            //ilog( "header: ${h}", ("h",header) );
            history.push_back( header.previous );
            if( header.previous == block_id_type() )
            {
               ilog( "return: ${h}", ("h",history) );
               return history;
            }
            auto prev_fork_data = _fork_db.fetch( header.previous );

            /// this shouldn't happen if the database invariants are properly maintained
            FC_ASSERT( prev_fork_data.is_linked, "we hit a dead end, this fork isn't really linked!" );
            if( prev_fork_data.is_included )
            {
               ilog( "return: ${h}", ("h",history) );
               return history;
            }
            next_id = header.previous;
         }
         ilog( "${h}", ("h",history) );
         return history;
      } FC_CAPTURE_AND_RETHROW( (id) ) }

      void chain_database_impl::pop_block()
      { try {
         assert(_head_block_header.block_num != 0);
         if( _head_block_header.block_num == 0 )
         {
            wlog( "attempting to pop block 0" );
            return;
         }

         // update the is_included flag on the fork data
         mark_included( _head_block_id, false );

         // update the block_num_to_block_id index
         _block_num_to_id_db.remove( _head_block_header.block_num );

         auto previous_block_id = _head_block_header.previous;

         const auto undo_iter = _block_id_to_undo_state.unordered_find( _head_block_id );
         FC_ASSERT( undo_iter != _block_id_to_undo_state.unordered_end() );
         const auto& undo_state = undo_iter->second;

         bts::blockchain::pending_chain_state_ptr undo_state_ptr = std::make_shared<bts::blockchain::pending_chain_state>( undo_state );
         undo_state_ptr->set_prev_state( self->shared_from_this() );
         undo_state_ptr->apply_changes();

         _head_block_id = previous_block_id;
         _head_block_header = self->get_block_header( _head_block_id );

         //Schedule the observer notifications for later; the chain is in a
         //non-premptable state right now, and observers may yield.
         for( chain_observer* o : _observers )
            fc::async([o,undo_state_ptr]{ o->state_changed( undo_state_ptr ); }, "call_state_changed_observer");
      } FC_CAPTURE_AND_RETHROW() }

   } // namespace detail

   chain_database::chain_database()
   :my( new detail::chain_database_impl() )
   {
      my->self = this;
      my->_skip_signature_verification = true;
      my->_relay_fee = BTS_BLOCKCHAIN_DEFAULT_RELAY_FEE;

      init_account_db_interface();
      init_asset_db_interface();
      init_balance_db_interface();
      init_transaction_db_interface();
      init_feed_db_interface();
   }

   chain_database::~chain_database()
   {
      try
      {
         close();
      }
      catch ( const fc::exception& e )
      {
         wlog( "unexpected exception closing database\n ${e}", ("e",e.to_detail_string() ) );
      }
      catch ( ... )
      {
         wlog( "unexpected exception closing database\n" );
      }
   }

   std::vector<account_id_type> chain_database::next_round_active_delegates()const
   {
       return get_delegates_by_vote( 0, BTS_BLOCKCHAIN_NUM_DELEGATES );
   }

   std::vector<account_id_type> chain_database::get_delegates_by_vote( uint32_t first, uint32_t count )const
   { try {
      auto del_vote_itr = my->_delegate_votes.begin();
      std::vector<account_id_type> sorted_delegates;
      sorted_delegates.reserve( count );
      uint32_t pos = 0;
      while( sorted_delegates.size() < count && del_vote_itr != my->_delegate_votes.end() )
      {
         if( pos >= first )
            sorted_delegates.push_back( del_vote_itr->delegate_id );
         ++pos;
         ++del_vote_itr;
      }
      return sorted_delegates;
   } FC_RETHROW_EXCEPTIONS( warn, "" ) }

   void chain_database::open( const fc::path& data_dir, fc::optional<fc::path> genesis_file, std::function<void(float)> reindex_status_callback )
   { try {
      bool must_rebuild_index = !fc::exists( data_dir / "index" );
      std::exception_ptr error_opening_database;
      try
      {
          //This function will yield the first time it is called. Do that now, before calling push_block
          now();

          fc::create_directories( data_dir );

          my->open_database( data_dir );

          // TODO: check to see if we crashed during the last write
          //   if so, then apply the last undo operation stored.

          uint32_t       last_block_num = -1;
          block_id_type  last_block_id;
          my->_block_num_to_id_db.last( last_block_num, last_block_id );

          try {
            if( !must_rebuild_index && last_block_num != uint32_t(-1) )
            {
               my->_head_block_header = get_block_digest( last_block_id );
               my->_head_block_id = last_block_id;
            }
          } catch (...) {
            must_rebuild_index = true;
          }

          bool replay_blockchain = must_rebuild_index || last_block_num == uint32_t( -1 );
          if( replay_blockchain )
          {
             close();
             fc::remove_all( data_dir / "index" );
             fc::create_directories( data_dir / "index");
             if( !fc::is_directory(data_dir / "raw_chain/id_to_data_orig") )
                fc::rename( data_dir / "raw_chain/block_id_to_block_data_db", data_dir / "raw_chain/id_to_data_orig" );

             //During reindexing we implement stop-and-copy garbage collection on the raw chain
             decltype(my->_block_id_to_block_data_db) id_to_data_orig;
             id_to_data_orig.open( data_dir / "raw_chain/id_to_data_orig" );
             auto orig_chain_size = fc::directory_size( data_dir / "raw_chain/id_to_data_orig" );

             my->open_database( data_dir );

             const auto set_db_cache_write_through = [ this ]( bool write_through )
             {
                 my->_property_db.set_write_through( write_through );

                 my->_account_id_to_record.set_write_through( write_through );
                 my->_account_name_to_id.set_write_through( write_through );
                 my->_account_address_to_id.set_write_through( write_through );

                 my->_asset_id_to_record.set_write_through( write_through );
                 my->_asset_symbol_to_id.set_write_through( write_through );

                 my->_balance_id_to_record.set_write_through( write_through );
                 my->_empty_balance_id_to_record.set_write_through( write_through );

                 my->_slate_db.set_write_through( write_through );
                 my->_burn_db.set_write_through( write_through );

                 my->_feed_index_to_record.set_write_through( write_through );

                 my->_ask_db.set_write_through( write_through );
                 my->_bid_db.set_write_through( write_through );
                 my->_relative_ask_db.set_write_through( write_through );
                 my->_relative_bid_db.set_write_through( write_through );
                 my->_short_db.set_write_through( write_through );
                 my->_collateral_db.set_write_through( write_through );

                 my->_market_status_db.set_write_through( write_through );
                 my->_market_transactions_db.set_write_through( write_through );
                 my->_market_history_db.set_write_through( write_through );
             };

             // For the duration of reindexing, we allow certain databases to postpone flushing until we finish
             set_db_cache_write_through( false );

             my->initialize_genesis( genesis_file );

             // Load block num -> id db into memory and clear from disk for re-indexing
             map<uint32_t, block_id_type> num_to_id;
             {
                 for( auto itr = my->_block_num_to_id_db.begin(); itr.valid(); ++itr )
                     num_to_id.emplace_hint( num_to_id.end(), itr.key(), itr.value() );

                 my->_block_num_to_id_db.close();
                 fc::remove_all( data_dir / "raw_chain/block_num_to_id_db" );
                 my->_block_num_to_id_db.open( data_dir / "raw_chain/block_num_to_id_db" );
             }

             if( !reindex_status_callback )
                 std::cout << "Please be patient, this will take a few minutes...\r\nRe-indexing database..." << std::flush << std::fixed;
             else
                 reindex_status_callback(0);

             uint32_t blocks_indexed = 0;
             const float total_blocks = num_to_id.size();
             auto genesis_time = get_genesis_timestamp();
             auto start_time = blockchain::now();

             auto insert_block = [&](const full_block& block) {
                 if( blocks_indexed % 200 == 0 ) {
                     float progress;
                     if (total_blocks)
                         progress = blocks_indexed / total_blocks;
                     else
                         progress = float(blocks_indexed*BTS_BLOCKCHAIN_BLOCK_INTERVAL_SEC) / (start_time - genesis_time).to_seconds();
                     progress *= 100;

                     if( !reindex_status_callback )
                         std::cout << "\rRe-indexing database... "
                                      "Approximately " << std::setprecision(2) << progress << "% complete." << std::flush;
                     else
                         reindex_status_callback(progress);
                 }

                 push_block(block);
                 ++blocks_indexed;

                 if( blocks_indexed % 1000 == 0 )
                 {
                     set_db_cache_write_through( true );
                     set_db_cache_write_through( false );
                 }
             };

             if (num_to_id.empty())
             {
                 for( auto block_itr = id_to_data_orig.begin(); block_itr.valid(); ++block_itr )
                     insert_block(block_itr.value());
             }
             else
             {
                 for (const auto& num_id : num_to_id)
                 {
                     auto oblock = id_to_data_orig.fetch_optional(num_id.second);
                     if (oblock)
                         insert_block(*oblock);
                 }
             }

             // Re-enable flushing on all cached databases we disabled it on above
             set_db_cache_write_through( true );

             id_to_data_orig.close();
             fc::remove_all( data_dir / "raw_chain/id_to_data_orig" );
             auto final_chain_size = fc::directory_size( data_dir / "raw_chain/block_id_to_block_data_db" );

             std::cout << "\rSuccessfully re-indexed " << blocks_indexed << " blocks in "
                       << (blockchain::now() - start_time).to_seconds() << " seconds.                          "
                                                                           "\nBlockchain size changed from "
                       << orig_chain_size / 1024 / 1024 << "MiB to "
                       << final_chain_size / 1024 / 1024 << "MiB.\n" << std::flush;
          }
          else
          {
              const optional<variant> property = get_property( bts::blockchain::chain_id );
              FC_ASSERT( property.valid() );
              my->_chain_id = property->as<digest_type>();

              my->populate_indexes();
          }

          //  process the pending transactions to cache by fees
          for( auto pending_itr = my->_pending_transaction_db.begin(); pending_itr.valid(); ++pending_itr )
          {
             try
             {
                auto trx = pending_itr.value();
                ilog( " loading pending transaction ${trx}", ("trx",trx) );
                auto trx_id = trx.id();
                auto eval_state = evaluate_transaction( trx, my->_relay_fee );
                share_type fees = eval_state->get_fees();
                my->_pending_fee_index[ fee_index( fees, trx_id ) ] = eval_state;
                my->_pending_transaction_db.store( trx_id, trx );
             }
             catch ( const fc::exception& e )
             {
                wlog( "error processing pending transaction: ${e}", ("e",e.to_detail_string() ) );
             }
          }
      }
      catch (...)
      {
        error_opening_database = std::current_exception();
      }

      if (error_opening_database)
      {
        elog( "error opening database" );
        close();
        fc::remove_all( data_dir / "index" );
        std::rethrow_exception(error_opening_database);
      }
   } FC_CAPTURE_AND_RETHROW( (data_dir) ) }

   void chain_database::close()
   { try {
      my->_block_num_to_id_db.close();
      my->_block_id_to_block_record_db.close();
      my->_block_id_to_block_data_db.close();
      my->_revalidatable_future_blocks_db.close();

      my->_fork_number_db.close();
      my->_fork_db.close();
      my->_block_id_to_undo_state.close();

      my->_property_db.close();

      my->_account_id_to_record.close();
      my->_account_name_to_id.close();
      my->_account_address_to_id.close();

      my->_asset_id_to_record.close();
      my->_asset_symbol_to_id.close();

      my->_balance_id_to_record.close();
      my->_empty_balance_id_to_record.close();

      my->_pending_transaction_db.close();
      my->_id_to_transaction_record_db.close();
      my->_address_to_trx_index.close();

      my->_slate_db.close();
      my->_burn_db.close();

      my->_slot_record_db.close();

      my->_feed_index_to_record.close();

      my->_ask_db.close();
      my->_bid_db.close();
      my->_relative_ask_db.close();
      my->_relative_bid_db.close();
      my->_short_db.close();
      my->_collateral_db.close();

      my->_market_history_db.close();
      my->_market_status_db.close();
      my->_market_transactions_db.close();

      my->_object_db.close();
      my->_edge_index.close();
      my->_reverse_edge_index.close();

      my->_auth_db.close();
      my->_asset_proposal_db.close();
   } FC_CAPTURE_AND_RETHROW() }

   account_record chain_database::get_delegate_record_for_signee( const public_key_type& block_signee )const
   {
      auto delegate_record = get_account_record( address( block_signee ) );
      FC_ASSERT( delegate_record.valid() && delegate_record->is_delegate() );
      return *delegate_record;
   }

   account_record chain_database::get_block_signee( const block_id_type& block_id )const
   {
      auto block_header = get_block_header( block_id );
      auto delegate_record = get_account_record( address( block_header.signee( false ) ) );
      FC_ASSERT( delegate_record.valid() && delegate_record->is_delegate() );
      return *delegate_record;
   }

   account_record chain_database::get_block_signee( uint32_t block_num )const
   {
      return get_block_signee( get_block_id( block_num ) );
   }

   account_record chain_database::get_slot_signee( const time_point_sec& timestamp,
                                                   const std::vector<account_id_type>& ordered_delegates )const
   { try {
      auto slot_number = blockchain::get_slot_number( timestamp );
      auto delegate_pos = slot_number % BTS_BLOCKCHAIN_NUM_DELEGATES;
      FC_ASSERT( delegate_pos < ordered_delegates.size() );
      auto delegate_id = ordered_delegates[ delegate_pos ];
      auto delegate_record = get_account_record( delegate_id );
      FC_ASSERT( delegate_record.valid() );
      FC_ASSERT( delegate_record->is_delegate() );
      return *delegate_record;
   } FC_CAPTURE_AND_RETHROW( (timestamp)(ordered_delegates) ) }

   optional<time_point_sec> chain_database::get_next_producible_block_timestamp( const vector<account_id_type>& delegate_ids )const
   { try {
      auto next_block_time = blockchain::get_slot_start_time( blockchain::now() );
      if( next_block_time <= now() ) next_block_time += BTS_BLOCKCHAIN_BLOCK_INTERVAL_SEC;
      auto last_block_time = next_block_time + (BTS_BLOCKCHAIN_NUM_DELEGATES * BTS_BLOCKCHAIN_BLOCK_INTERVAL_SEC);

      auto active_delegates = get_active_delegates();
      for( ; next_block_time < last_block_time; next_block_time += BTS_BLOCKCHAIN_BLOCK_INTERVAL_SEC )
      {
          auto slot_number = blockchain::get_slot_number( next_block_time );
          auto delegate_pos = slot_number % BTS_BLOCKCHAIN_NUM_DELEGATES;
          FC_ASSERT( delegate_pos < active_delegates.size() );
          auto delegate_id = active_delegates[ delegate_pos ];

          if( std::find( delegate_ids.begin(), delegate_ids.end(), delegate_id ) != delegate_ids.end() )
              return next_block_time;
      }
      return optional<time_point_sec>();
   } FC_CAPTURE_AND_RETHROW( (delegate_ids) ) }

   transaction_evaluation_state_ptr chain_database::evaluate_transaction( const signed_transaction& trx,
                                                                          const share_type& required_fees )
   { try {
      if( !my->_pending_trx_state )
         my->_pending_trx_state = std::make_shared<pending_chain_state>( shared_from_this() );

      pending_chain_state_ptr          pend_state = std::make_shared<pending_chain_state>(my->_pending_trx_state);
      transaction_evaluation_state_ptr trx_eval_state = std::make_shared<transaction_evaluation_state>( pend_state.get() );

      trx_eval_state->evaluate( trx, false, false );
      auto fees = trx_eval_state->get_fees() + trx_eval_state->alt_fees_paid.amount;
      if( fees < required_fees )
      {
          wlog("Transaction ${id} needed relay fee ${required_fees} but only had ${fees}", ("id", trx.id())("required_fees",required_fees)("fees",fees));
          FC_CAPTURE_AND_THROW( insufficient_relay_fee, (fees)(required_fees) );
      }
      // apply changes from this transaction to _pending_trx_state
      pend_state->apply_changes();

      return trx_eval_state;
   } FC_CAPTURE_AND_RETHROW( (trx) ) }

   optional<fc::exception> chain_database::get_transaction_error( const signed_transaction& transaction, const share_type& min_fee )
   { try {
       try
       {
          auto pending_state = std::make_shared<pending_chain_state>( shared_from_this() );
          transaction_evaluation_state_ptr eval_state = std::make_shared<transaction_evaluation_state>( pending_state.get() );

          eval_state->evaluate( transaction, false, false );
          auto fees = eval_state->get_fees();
          if( fees < min_fee )
             FC_CAPTURE_AND_THROW( insufficient_relay_fee, (fees)(min_fee) );
       }
       catch (const fc::canceled_exception&)
       {
         throw;
       }
       catch( const fc::exception& e )
       {
           return e;
       }
       return optional<fc::exception>();
   } FC_CAPTURE_AND_RETHROW( (transaction) ) }

   signed_block_header chain_database::get_block_header( const block_id_type& block_id )const
   { try {
      const auto record = get_block_record( block_id );
      if( !record.valid() )
          FC_THROW_EXCEPTION( unknown_block, "Unknown block!", ("block_id",block_id) );
      return *record;
   } FC_CAPTURE_AND_RETHROW( (block_id) ) }

   signed_block_header  chain_database::get_block_header( uint32_t block_num )const
   { try {
      return *get_block_record( get_block_id( block_num ) );
   } FC_CAPTURE_AND_RETHROW( (block_num) ) }

   oblock_record chain_database::get_block_record( const block_id_type& block_id ) const
   { try {
      return my->_block_id_to_block_record_db.fetch_optional(block_id);
   } FC_CAPTURE_AND_RETHROW( (block_id) ) }

   oblock_record chain_database::get_block_record( uint32_t block_num ) const
   { try {
      return get_block_record( get_block_id( block_num ) );
   } FC_CAPTURE_AND_RETHROW( (block_num) ) }

   block_id_type chain_database::get_block_id( uint32_t block_num ) const
   { try {
      return my->_block_num_to_id_db.fetch( block_num );
   } FC_CAPTURE_AND_RETHROW( (block_num) ) }

   vector<transaction_record> chain_database::get_transactions_for_block( const block_id_type& block_id )const
   {
      auto block_record = my->_block_id_to_block_record_db.fetch(block_id);
      vector<transaction_record> result;
      result.reserve( block_record.user_transaction_ids.size() );

      for( const auto& trx_id : block_record.user_transaction_ids )
      {
         auto otrx_record = get_transaction( trx_id );
         if( !otrx_record ) FC_CAPTURE_AND_THROW( unknown_transaction, (trx_id) );
         result.emplace_back( *otrx_record );
      }
      return result;
   }

   digest_block chain_database::get_block_digest( const block_id_type& block_id )const
   {
      return my->_block_id_to_block_record_db.fetch( block_id );
   }

   digest_block chain_database::get_block_digest( uint32_t block_num )const
   {
      auto block_id = my->_block_num_to_id_db.fetch( block_num );
      return get_block_digest( block_id );
   }

   full_block chain_database::get_block( const block_id_type& block_id )const
   { try {
      return my->_block_id_to_block_data_db.fetch(block_id);
   } FC_CAPTURE_AND_RETHROW( (block_id) ) }

   full_block chain_database::get_block( uint32_t block_num )const
   { try {
      auto block_id = my->_block_num_to_id_db.fetch( block_num );
      return get_block( block_id );
   } FC_RETHROW_EXCEPTIONS( warn, "", ("block_num",block_num) ) }

   signed_block_header chain_database::get_head_block()const
   {
      return my->_head_block_header;
   }

   /**
    *  Adds the block to the database and manages any reorganizations as a result.
    *
    *  Returns the block_fork_data of the new block, not necessarily the head block
    */
   block_fork_data chain_database::push_block( const full_block& block_data )
   { try {
      uint32_t head_block_num = get_head_block_num();
      if( head_block_num > BTS_BLOCKCHAIN_MAX_UNDO_HISTORY &&
          block_data.block_num <= (head_block_num - BTS_BLOCKCHAIN_MAX_UNDO_HISTORY) )
      {
        elog( "block ${new_block_hash} (number ${new_block_num}) is on a fork older than "
               "our undo history would allow us to switch to (current head block is number ${head_block_num}, undo history is ${undo_history})",
                           ("new_block_hash", block_data.id())("new_block_num", block_data.block_num)
                           ("head_block_num", head_block_num)("undo_history", BTS_BLOCKCHAIN_MAX_UNDO_HISTORY));

        FC_THROW_EXCEPTION(block_older_than_undo_history,
                           "block ${new_block_hash} (number ${new_block_num}) is on a fork older than "
                           "our undo history would allow us to switch to (current head block is number ${head_block_num}, undo history is ${undo_history})",
                           ("new_block_hash", block_data.id())("new_block_num", block_data.block_num)
                           ("head_block_num", head_block_num)("undo_history", BTS_BLOCKCHAIN_MAX_UNDO_HISTORY));
      }

      // only allow a single fiber attempt to push blocks at any given time,
      // this method is not re-entrant.
      fc::unique_lock<fc::mutex> lock( my->_push_block_mutex );

      // The above check probably isn't enough.  We need to make certain that
      // no other code sees the chain_database in an inconsistent state.
      // The lock above prevents two push_blocks from happening at the same time,
      // but we also need to ensure the wallet, blockchain, delegate, &c. loops don't
      // see partially-applied blocks
      ASSERT_TASK_NOT_PREEMPTED();

      const block_id_type& block_id = block_data.id();
      std::pair<block_id_type, block_fork_data> longest_fork = my->store_and_index( block_id, block_data );
      assert(get_block_fork_data(block_id) && "can't get fork data for a block we just successfully pushed");

      /*
      store_and_index has returned the potential chain with the longest_fork (highest block number other than possible the current head block number)
      if (longest_fork is linked and not known to be invalid and is higher than the current head block number)
        highest_unchecked_block_number = longest_fork blocknumber;
        do
          foreach next_fork_to_try in all blocks at same block number
              if (next_fork_try is linked and not known to be invalid)
                try
                  switch_to_fork(next_fork_to_try) //this throws if block in fork is invalid, then we'll try another fork
                  return
                catch block from future and add to database for potential revalidation on startup or if we get from another peer later
                catch any other invalid block and do nothing
          --highest_unchecked_block_number
        while(highest_unchecked_block_number > 0)
      */
      if (longest_fork.second.can_link())
      {
        full_block longest_fork_block = my->_block_id_to_block_data_db.fetch(longest_fork.first);
        uint32_t highest_unchecked_block_number = longest_fork_block.block_num;
        if (highest_unchecked_block_number > head_block_num)
        {
          do
          {
            optional<vector<block_id_type>> parallel_blocks = my->_fork_number_db.fetch_optional(highest_unchecked_block_number);
            if (parallel_blocks)
              //for all blocks at same block number
              for (const block_id_type& next_fork_to_try_id : *parallel_blocks)
              {
                block_fork_data next_fork_to_try = my->_fork_db.fetch(next_fork_to_try_id);
                if (next_fork_to_try.can_link())
                  try
                  {
                    my->switch_to_fork(next_fork_to_try_id); //verify this works if next_fork_to_try is current head block
                    return *get_block_fork_data(block_id);
                  }
                  catch (const time_in_future& e)
                  {
                    // Blocks from the future can become valid later, so keep a list of these blocks that we can iterate over
                    // whenever we think our clock time has changed from it's standard flow
                    my->_revalidatable_future_blocks_db.store(block_id, 0);
                    wlog("fork rejected because it has block with time in future, storing block id for revalidation later");
                  }
                  catch (const fc::exception& e) //swallow any invalidation exceptions except for time_in_future invalidations
                  {
                    wlog("fork permanently rejected as it has permanently invalid block");
                  }
              }
            --highest_unchecked_block_number;
          } while(highest_unchecked_block_number > 0); // while condition should only fail if we've never received a valid block yet
        } //end if fork is longer than current chain (including possibly by extending chain)
      }
      else
      {
         elog( "unable to link longest fork ${f}", ("f", longest_fork) );
      }
      return *get_block_fork_data(block_id);
   } FC_CAPTURE_AND_RETHROW( (block_data) )  }

  std::vector<block_id_type> chain_database::get_fork_history( const block_id_type& id )
  {
    return my->get_fork_history(id);
  }

   /** return the timestamp from the head block */
   fc::time_point_sec chain_database::now()const
   {
      if( my->_head_block_header.block_num <= 0 ) /* Genesis */
      {
          auto slot_number = blockchain::get_slot_number( blockchain::now() );
          return blockchain::get_slot_start_time( slot_number - 1 );
      }

      return my->_head_block_header.timestamp;
   }

   oasset_record chain_database::get_asset_record( const asset_id_type id )const
   { try {
       return lookup<asset_record>( id );
   } FC_CAPTURE_AND_RETHROW( (id) ) }

   oaccount_record chain_database::get_account_record( const address& account_owner )const
   { try {
       return lookup<account_record>( account_owner );
   } FC_CAPTURE_AND_RETHROW( (account_owner) ) }

   obalance_record chain_database::get_balance_record( const balance_id_type& balance_id )const
   { try {
       return lookup<balance_record>( balance_id );
   } FC_CAPTURE_AND_RETHROW( (balance_id) ) }

   oaccount_record chain_database::get_account_record( const account_id_type account_id )const
   { try {
       return lookup<account_record>( account_id );
   } FC_CAPTURE_AND_RETHROW( (account_id) ) }

   asset_id_type chain_database::get_asset_id( const string& symbol )const
   { try {
       auto arec = get_asset_record( symbol );
       FC_ASSERT( arec.valid() );
       return arec->id;
   } FC_CAPTURE_AND_RETHROW( (symbol) ) }

   bool chain_database::is_valid_symbol( const string& symbol )const
   { try {
       return get_asset_record(symbol).valid();
   } FC_CAPTURE_AND_RETHROW( (symbol) ) }

   oasset_record chain_database::get_asset_record( const string& symbol )const
   { try {
       return lookup<asset_record>( symbol );
   } FC_CAPTURE_AND_RETHROW( (symbol) ) }

   oaccount_record chain_database::get_account_record( const string& account_name )const
   { try {
       return lookup<account_record>( account_name );
   } FC_CAPTURE_AND_RETHROW( (account_name) ) }

   void chain_database::store_asset_record( const asset_record& asset_to_store )
   { try {
       store( asset_to_store );
   } FC_CAPTURE_AND_RETHROW( (asset_to_store) ) }

   void chain_database::store_balance_record( const balance_record& r )
   { try {
       store( r );
   } FC_CAPTURE_AND_RETHROW( (r) ) }

   void chain_database::store_account_record( const account_record& record_to_store )
   { try {
       store( record_to_store );
   } FC_CAPTURE_AND_RETHROW( (record_to_store) ) }

   vector<operation> chain_database::get_recent_operations(operation_type_enum t)
   {
      const auto& recent_op_queue = my->_recent_operations[t];
      vector<operation> recent_ops(recent_op_queue.size());
      std::copy(recent_op_queue.begin(), recent_op_queue.end(), recent_ops.begin());
      return recent_ops;
   }

   void chain_database::store_recent_operation(const operation& o)
   {
      auto& recent_op_queue = my->_recent_operations[o.type];
      recent_op_queue.push_back(o);
      if( recent_op_queue.size() > MAX_RECENT_OPERATIONS )
        recent_op_queue.pop_front();
   }


    oobject_record             chain_database::get_object_record( const object_id_type& id )const
    {
       return my->_object_db.fetch_optional( id );
    }

    void                       chain_database::store_object_record( const object_record& obj )
    { try {
        switch( obj.type() )
        {
            case base_object:
            {
                ilog("@n storing a base_object record in chain DB");
                my->_object_db.store( obj._id, obj );
                auto o = my->_object_db.fetch_optional( obj._id );
                ilog("@n fetched it again as a sanity check: ${o}", ("o", o));
                break;
            }
            case edge_object:
            {
                store_edge_record( obj );
                break;
            }
            case account_object:
            case asset_object:
            case throttled_auction_object:
            case user_auction_object:
            case site_object:
            default:
                FC_ASSERT(false, "You cannot store these object types via object interface yet!");
                break;
        }
    } FC_CAPTURE_AND_RETHROW( (obj) ) }



    void                       chain_database::store_site_record( const site_record& site )
    {
        /*
        my->_site_index.store(site.site_name, site);
        my->_object_db.store(site._id, site);
        ilog("@n after storing site in chain DB:");
        ilog("@n      as an object: ${o}", ("o", object_record(site)));
        ilog("@n      as a site: ${s}", ("s", site));
        */
    }

   osite_record  chain_database::lookup_site( const string& site_name )const
   { try {
       auto site = my->_site_index.fetch_optional( site_name );
       if( site.valid() )
       {
           return site;
           /*
           auto obj = my->_object_db.fetch( *site_id );
           return obj.as<site_record>();
           */
       }
       return osite_record();
   } FC_CAPTURE_AND_RETHROW( (site_name) ) }




    void            chain_database::store_edge_record( const object_record& edge )
    { try {
        ilog("@n storing edge in chain DB: ${e}", ("e", edge));
        auto edge_data = edge.as<edge_record>();
        my->_edge_index.store( edge_data.index_key(), edge._id );
        my->_reverse_edge_index.store( edge_data.reverse_index_key(), edge._id );
        my->_object_db.store( edge._id, edge );
    } FC_CAPTURE_AND_RETHROW( (edge) ) }

    oobject_record  chain_database::get_edge( const object_id_type& from,
                                             const object_id_type& to,
                                             const string& name )const
    {
        ilog("@n getting edge with key: (${f}, ${t}, ${n})", ("f",from)("t",to)("n",name));
        edge_index_key key( from, to, name );
        auto object_id = my->_edge_index.fetch_optional( key );
        if( object_id.valid() )
           return get_object_record( *object_id );
        return oobject_record();
    }
    map<string, object_record>   chain_database::get_edges( const object_id_type& from,
                                                            const object_id_type& to )const
    {
        FC_ASSERT(false, "unimplemented");
        map<string, object_record> ret;
        return ret;
    }

    map<object_id_type, map<string, object_record>> chain_database::get_edges( const object_id_type& from )const
    {
        FC_ASSERT(false, "unimplemented");
        map<object_id_type, map<string, object_record>> ret;
        return ret;
    }

   otransaction_record chain_database::get_transaction( const transaction_id_type& trx_id, bool exact )const
   { try {
      FC_ASSERT( my->_track_stats );
      auto trx_rec = my->_id_to_transaction_record_db.fetch_optional( trx_id );
      if( trx_rec || exact )
      {
         //ilog( "trx_rec: ${id} => ${t}", ("id",trx_id)("t",trx_rec) );
         if( trx_rec )
            FC_ASSERT( trx_rec->trx.id() == trx_id,"", ("trx_rec->id",trx_rec->trx.id()) );
         return trx_rec;
      }

      auto itr = my->_id_to_transaction_record_db.lower_bound( trx_id );
      if( itr.valid() )
      {
         auto id = itr.key();

         if( memcmp( (char*)&id, (const char*)&trx_id, 4 ) != 0 )
            return otransaction_record();

         return itr.value();
      }
      return otransaction_record();
   } FC_CAPTURE_AND_RETHROW( (trx_id)(exact) ) }

   void chain_database::store_transaction( const transaction_id_type& record_id,
                                           const transaction_record& record_to_store )
   { try {
       store( record_to_store );
   } FC_CAPTURE_AND_RETHROW( (record_id)(record_to_store) ) }

   void chain_database::scan_balances( function<void( const balance_record& )> callback, bool include_empty )const
   { try {
       for( auto iter = my->_balance_id_to_record.unordered_begin();
            iter != my->_balance_id_to_record.unordered_end(); ++iter )
       {
           callback( iter->second );
       }
       if( !include_empty ) return;
       for( auto iter = my->_empty_balance_id_to_record.unordered_begin();
            iter != my->_empty_balance_id_to_record.unordered_end(); ++iter )
       {
           callback( iter->second );
       }
   } FC_CAPTURE_AND_RETHROW( (include_empty) ) }

   void chain_database::scan_unordered_accounts( function<void( const account_record& )> callback )const
   { try {
       for( auto iter = my->_account_id_to_record.unordered_begin();
            iter != my->_account_id_to_record.unordered_end(); ++iter )
       {
           callback( iter->second );
       }
   } FC_CAPTURE_AND_RETHROW() }

   void chain_database::scan_ordered_accounts( function<void( const account_record& )> callback )const
   { try {
       for( auto iter = my->_account_name_to_id.ordered_first(); iter.valid(); ++iter )
       {
           const oaccount_record& record = lookup<account_record>( iter.value() );
           if( record.valid() ) callback( *record );
       }
   } FC_CAPTURE_AND_RETHROW() }

   void chain_database::scan_unordered_assets( function<void( const asset_record& )> callback )const
   { try {
       for( auto iter = my->_asset_id_to_record.unordered_begin();
            iter != my->_asset_id_to_record.unordered_end(); ++iter )
       {
           callback( iter->second );
       }
   } FC_CAPTURE_AND_RETHROW() }

   void chain_database::scan_ordered_assets( function<void( const asset_record& )> callback )const
   { try {
       for( auto iter = my->_asset_symbol_to_id.ordered_first(); iter.valid(); ++iter )
       {
           const oasset_record& record = lookup<asset_record>( iter.value() );
           if( record.valid() ) callback( *record );
       }
   } FC_CAPTURE_AND_RETHROW() }

   void chain_database::scan_objects( function<void( const object_record& )> callback )const
   {
        ilog("@n starting object db scan");
        for( auto itr = my->_object_db.begin(); itr.valid(); ++itr )
        {
           ilog("@n scanning object: ${o}", ("o", itr.value()));
           callback( itr.value() );
        }
   }

   /** this should throw if the trx is invalid */
   transaction_evaluation_state_ptr chain_database::store_pending_transaction( const signed_transaction& trx, bool override_limits )
   { try {
      auto trx_id = trx.id();
      if (override_limits)
        wlog("storing new local transaction with id ${id}", ("id", trx_id));

      auto current_itr = my->_pending_transaction_db.find( trx_id );
      if( current_itr.valid() )
        return nullptr;

      share_type relay_fee = my->_relay_fee;
      if( !override_limits )
      {
         if( my->_pending_fee_index.size() > BTS_BLOCKCHAIN_MAX_PENDING_QUEUE_SIZE )
         {
             auto overage = my->_pending_fee_index.size() - BTS_BLOCKCHAIN_MAX_PENDING_QUEUE_SIZE;
             relay_fee = my->_relay_fee * overage * overage;
         }
      }

      transaction_evaluation_state_ptr eval_state = evaluate_transaction( trx, relay_fee );
      share_type fees = eval_state->get_fees();

      //if( fees < my->_relay_fee )
      //   FC_CAPTURE_AND_THROW( insufficient_relay_fee, (fees)(my->_relay_fee) );

      my->_pending_fee_index[ fee_index( fees, trx_id ) ] = eval_state;
      my->_pending_transaction_db.store( trx_id, trx );

      return eval_state;
   } FC_CAPTURE_AND_RETHROW( (trx)(override_limits) ) }

   /** returns all transactions that are valid (independent of each other) sorted by fee */
   std::vector<transaction_evaluation_state_ptr> chain_database::get_pending_transactions()const
   {
      std::vector<transaction_evaluation_state_ptr> trxs;
      for( const auto& item : my->_pending_fee_index )
      {
          trxs.push_back( item.second );
      }
      return trxs;
   }

   full_block chain_database::generate_block( const time_point_sec& block_timestamp, const delegate_config& config )
   { try {
      const time_point start_time = time_point::now();

      const pending_chain_state_ptr pending_state = std::make_shared<pending_chain_state>( shared_from_this() );
      if( pending_state->get_head_block_num() >= BTS_V0_4_9_FORK_BLOCK_NUM )
          my->execute_markets( block_timestamp, pending_state );

      // Initialize block
      full_block new_block;
      size_t block_size = new_block.block_size();
      if( config.block_max_transaction_count > 0 && config.block_max_size > block_size )
      {
          // Evaluate pending transactions
          const vector<transaction_evaluation_state_ptr> pending_trx = get_pending_transactions();
          for( const transaction_evaluation_state_ptr& item : pending_trx )
          {
              // Check block production time limit
              if( time_point::now() - start_time >= config.block_max_production_time )
                  break;

              const signed_transaction& new_transaction = item->trx;
              try
              {
                  // Check transaction size limit
                  const size_t transaction_size = new_transaction.data_size();
                  if( transaction_size > config.transaction_max_size )
                  {
                      wlog( "Excluding transaction ${id} of size ${size} because it exceeds transaction size limit ${limit}",
                            ("id",new_transaction.id())("size",transaction_size)("limit",config.transaction_max_size) );
                      continue;
                  }

                  // Check block size limit
                  if( block_size + transaction_size > config.block_max_size )
                  {
                      wlog( "Excluding transaction ${id} of size ${size} because block would exceed block size limit ${limit}",
                            ("id",new_transaction.id())("size",transaction_size)("limit",config.block_max_size) );
                      continue;
                  }

                  // Check transaction blacklist
                  if( !config.transaction_blacklist.empty() )
                  {
                      const transaction_id_type id = new_transaction.id();
                      if( config.transaction_blacklist.count( id ) > 0 )
                      {
                          wlog( "Excluding blacklisted transaction ${id}", ("id",id) );
                          continue;
                      }
                  }

                  // Check operation blacklist
                  if( !config.operation_blacklist.empty() )
                  {
                      optional<operation_type_enum> blacklisted_op;
                      for( const operation& op : new_transaction.operations )
                      {
                          if( config.operation_blacklist.count( op.type ) > 0 )
                          {
                              blacklisted_op = op.type;
                              break;
                          }
                      }
                      if( blacklisted_op.valid() )
                      {
                          wlog( "Excluding transaction ${id} because of blacklisted operation ${op}",
                                ("id",new_transaction.id())("op",*blacklisted_op) );
                          continue;
                      }
                  }

                  // Validate transaction
                  auto pending_trx_state = std::make_shared<pending_chain_state>( pending_state );
                  {
                      auto trx_eval_state = std::make_shared<transaction_evaluation_state>( pending_trx_state.get() );
                      trx_eval_state->evaluate( new_transaction, false, config.transaction_canonical_signatures_required );

                      // Check transaction fee limit
                      const share_type transaction_fee = trx_eval_state->get_fees( 0 ) + trx_eval_state->alt_fees_paid.amount;
                      if( transaction_fee < config.transaction_min_fee )
                      {
                          wlog( "Excluding transaction ${id} with fee ${fee} because it does not meet transaction fee limit ${limit}",
                                ("id",new_transaction.id())("fee",transaction_fee)("limit",config.transaction_min_fee) );
                          continue;
                      }
                  }

                  // Include transaction
                  pending_trx_state->apply_changes();
                  new_block.user_transactions.push_back( new_transaction );
                  block_size += transaction_size;

                  // Check block transaction count limit
                  if( new_block.user_transactions.size() >= config.block_max_transaction_count )
                      break;
              }
              catch( const fc::canceled_exception& )
              {
                  throw;
              }
              catch( const fc::exception& e )
              {
                  wlog( "Pending transaction was found to be invalid in context of block\n${trx}\n${e}",
                        ("trx",fc::json::to_pretty_string( new_transaction ))("e",e.to_detail_string()) );
              }
          }
      }

      const signed_block_header head_block = get_head_block();

      // Populate block header
      new_block.previous            = head_block.block_num > 0 ? head_block.id() : block_id_type();
      new_block.block_num           = head_block.block_num + 1;
      new_block.timestamp           = block_timestamp;
      new_block.transaction_digest  = digest_block( new_block ).calculate_transaction_digest();

      return new_block;
   } FC_CAPTURE_AND_RETHROW( (block_timestamp)(config) ) }

   void chain_database::add_observer( chain_observer* observer )
   {
      my->_observers.insert(observer);
   }

   void chain_database::remove_observer( chain_observer* observer )
   {
      my->_observers.erase(observer);
   }

   bool chain_database::is_known_block( const block_id_type& block_id )const
   {
      auto fork_data = get_block_fork_data( block_id );
      return fork_data && fork_data->is_known;
   }
   bool chain_database::is_included_block( const block_id_type& block_id )const
   {
      auto fork_data = get_block_fork_data( block_id );
      return fork_data && fork_data->is_included;
   }
   optional<block_fork_data> chain_database::get_block_fork_data( const block_id_type& id )const
   {
      return my->_fork_db.fetch_optional(id);
   }

   uint32_t chain_database::get_block_num( const block_id_type& block_id )const
   { try {
      if( block_id == block_id_type() )
         return 0;
      return my->_block_id_to_block_record_db.fetch( block_id ).block_num;
   } FC_RETHROW_EXCEPTIONS( warn, "Unable to find block ${block_id}", ("block_id", block_id) ) }

    uint32_t chain_database::get_head_block_num()const
    {
       return my->_head_block_header.block_num;
    }

    block_id_type chain_database::get_head_block_id()const
    {
       return my->_head_block_id;
    }

    map<balance_id_type, balance_record> chain_database::get_balances( const balance_id_type& first, uint32_t limit )const
    { try {
        map<balance_id_type, balance_record> records;
        for( auto iter = my->_balance_id_to_record.ordered_lower_bound( first );
             iter.valid() && records.size() <= limit; ++iter )
        {
            records[ iter.key() ] = iter.value();
        }
        return records;
    } FC_CAPTURE_AND_RETHROW( (first)(limit) ) }

   map<balance_id_type, balance_record> chain_database::get_balances_for_address( const address& addr )const
   { try {
        map<balance_id_type, balance_record> records;
        const auto scan_balance = [ &addr, &records ]( const balance_record& record )
        {
            if( record.is_owner( addr ) || record.id() == addr )
                records[ record.id() ] = record;
        };
        scan_balances( scan_balance, false );
        return records;
   } FC_CAPTURE_AND_RETHROW( (addr) ) }

   map<balance_id_type, balance_record> chain_database::get_balances_for_key( const public_key_type& key )const
   { try {
        map<balance_id_type, balance_record> records;
        const auto scan_balance = [ &key, &records ]( const balance_record& record )
        {
            if( record.is_owner( key ) )
                records[ record.id() ] = record;
        };
        scan_balances( scan_balance, false );
        return records;
   } FC_CAPTURE_AND_RETHROW( (key) ) }

    vector<account_record> chain_database::get_accounts( const string& first, uint32_t limit )const
    { try {
        vector<account_record> records;
        for( auto iter = my->_account_name_to_id.ordered_lower_bound( first );
             iter.valid() && records.size() <= limit; ++iter )
        {
            const oaccount_record& record = lookup<account_record>( iter.value() );
            if( record.valid() ) records.push_back( *record );
        }
        return records;
    } FC_CAPTURE_AND_RETHROW( (first)(limit) ) }

    vector<asset_record> chain_database::get_assets( const string& first, uint32_t limit )const
    { try {
        vector<asset_record> records;
        for( auto iter = my->_asset_symbol_to_id.ordered_lower_bound( first );
             iter.valid() && records.size() <= limit; ++iter )
        {
            const oasset_record& record = lookup<asset_record>( iter.value() );
            if( record.valid() ) records.push_back( *record );
        }
        return records;
    } FC_CAPTURE_AND_RETHROW( (first)(limit) ) }

    string chain_database::export_fork_graph( uint32_t start_block, uint32_t end_block, const fc::path& filename )const
    {
      FC_ASSERT( start_block >= 0 );
      FC_ASSERT( end_block >= start_block );
      std::stringstream out;
      out << "digraph G { \n";
      out << "rankdir=LR;\n";

      bool first = true;
      fc::time_point_sec start_time;
      std::map<uint32_t, std::vector<block_record> > nodes_by_rank;
      //std::set<uint32_t> ranks_in_use;
      for( auto block_itr = my->_block_id_to_block_record_db.begin(); block_itr.valid(); ++block_itr )
      {
        block_record block_record = block_itr.value();
        if (first)
        {
          first = false;
          start_time = block_record.timestamp;
        }
        std::cout << block_record.block_num << "  start " << start_block << "  end " << end_block << "\n";
        if ( block_record.block_num >= start_block && block_record.block_num <= end_block )
        {
          unsigned rank = (unsigned)((block_record.timestamp - start_time).to_seconds() / BTS_BLOCKCHAIN_BLOCK_INTERVAL_SEC);

          //ilog( "${id} => ${r}", ("id",fork_itr.key())("r",fork_data) );
          nodes_by_rank[rank].push_back(block_record);
        }
      }

      for( const auto& item : nodes_by_rank )
      {
        out << "{rank=same l" << item.first << "[style=invis, shape=point] ";
        for( const auto& record : item.second )
          out << "; \"" << std::string(record.id()) << "\"";
        out << ";}\n";
      }
      for( const auto& blocks_at_time : nodes_by_rank )
      {
        for( const auto& block : blocks_at_time.second )
        {
          auto delegate_record = get_block_signee( block.id() );

          out << '"' << std::string ( block.id() ) <<"\" "
              << "[label=<"
              << std::string ( block.id() ).substr(0,5)
              << "<br/>" << blocks_at_time.first
              << "<br/>" << block.block_num
              << "<br/>" << delegate_record.name
              << ">,style=filled,rank=" << blocks_at_time.first << "];\n";
          out << '"' << std::string ( block.id() ) <<"\" -> \"" << std::string( block.previous ) << "\";\n";
        }
      }
      out << "edge[style=invis];\n";

      bool first2 = true;
      for( const auto& item : nodes_by_rank )
      {
        if (first2)
          first2 = false;
        else
          out << "->";
        out << "l" << item.first;
      }
      out << ";\n";
      out << "}";

      if( filename == "" )
          return out.str();

      FC_ASSERT( !fc::exists( fc::path( filename ) ) );
      std::ofstream fileout( filename.generic_string().c_str() );
      fileout << out.str();

      return std::string();
    }

    std::map<uint32_t, std::vector<fork_record>> chain_database::get_forks_list()const
    {
        std::map<uint32_t, std::vector<fork_record>> fork_blocks;
        for( auto iter = my->_fork_db.begin(); iter.valid(); ++iter )
        {
            try
            {
                auto fork_iter = iter.value();
                if( fork_iter.next_blocks.size() > 1 )
                {
                    vector<fork_record> forks;

                    for( const auto& forked_block_id : fork_iter.next_blocks )
                    {
                        fork_record fork;
                        block_fork_data fork_data = my->_fork_db.fetch(forked_block_id);
                        block_record fork_block = my->_block_id_to_block_record_db.fetch(forked_block_id);

                        fork.block_id = forked_block_id;
                        fork.latency = fork_block.latency;
                        fork.signing_delegate = get_block_signee( forked_block_id ).id;
                        fork.transaction_count = fork_block.user_transaction_ids.size();
                        fork.size = (uint32_t)fork_block.block_size;
                        fork.timestamp = fork_block.timestamp;
                        fork.is_valid = fork_data.is_valid;
                        fork.invalid_reason = fork_data.invalid_reason;
                        fork.is_current_fork = fork_data.is_included;

                        forks.push_back(fork);
                    }

                    fork_blocks[get_block_num( iter.key() )] = forks;
                }
            }
            catch( ... )
            {
                wlog( "error fetching block num of block ${b} while building fork list", ("b",iter.key()));
                throw;
            }
        }

        return fork_blocks;
    }

    vector<slot_record> chain_database::get_delegate_slot_records( const account_id_type delegate_id,
                                                                   int64_t start_block_num, uint32_t count )const
    {
        FC_ASSERT( my->_track_stats, "index of slot records is disabled" );
        FC_ASSERT( count > 0 );
        if( start_block_num < 0 )
            start_block_num = int64_t( get_head_block_num() ) + start_block_num;
        FC_ASSERT( start_block_num >= 1 );

        const signed_block_header block_header = get_block_header( start_block_num );
        const time_point_sec& min_timestamp = block_header.timestamp;

        vector<slot_record> slot_records;
        slot_records.reserve( count );

        for( auto iter = my->_slot_record_db.begin(); iter.valid(); ++iter )
        {
            const auto slot_record = iter.value();
            if( slot_record.start_time < min_timestamp || slot_record.block_producer_id != delegate_id )
                continue;

            slot_records.push_back( slot_record );
            if( slot_records.size() >= count )
                break;
        }

        return slot_records;
    }

   optional<variant> chain_database::get_property( chain_property_enum property_id )const
   { try {
      return my->_property_db.fetch_optional( property_id );
   } FC_CAPTURE_AND_RETHROW( (property_id) ) }

   void chain_database::set_property( chain_property_enum property_id, const fc::variant& property_value )
   { try {
      if( property_value.is_null() )
         my->_property_db.remove( property_id );
      else
         my->_property_db.store( property_id, property_value );
   } FC_CAPTURE_AND_RETHROW( (property_id)(property_value) ) }

   digest_type chain_database::chain_id()const
   {
         return my->_chain_id;
   }

   fc::variant_object chain_database::find_delegate_vote_discrepancies() const
   {
      unordered_map<account_id_type, share_type> calculated_balances;

      for( auto iter = my->_balance_id_to_record.unordered_begin();
           iter != my->_balance_id_to_record.unordered_end(); ++iter )
      {
        const balance_record& balance = iter->second;
        if (balance.slate_id() == 0)
          continue;
        if (balance.asset_id() == 0)
        {
          odelegate_slate slate = get_delegate_slate(balance.slate_id());
          FC_ASSERT(slate.valid(), "Unknown slate ID found in balance.");

          for (account_id_type delegate : slate->supported_delegates)
            calculated_balances[delegate] += balance.balance;
        }
      }

      fc::mutable_variant_object discrepancies;

      for( const auto& vote_record : my->_delegate_votes )
      {
        oaccount_record delegate_record = get_account_record(vote_record.delegate_id);
        FC_ASSERT(delegate_record.valid(), "Unknown delegate ID in votes database.");

        calculated_balances[delegate_record->id] += delegate_record->delegate_pay_balance();

        if (vote_record.votes != delegate_record->net_votes() ||
            vote_record.votes != calculated_balances[vote_record.delegate_id])
        {
          fc::mutable_variant_object discrepancy_record;
          discrepancy_record["calculated_votes"] = calculated_balances[vote_record.delegate_id];
          discrepancy_record["indexed_votes"] = vote_record.votes;
          discrepancy_record["stored_votes"] = delegate_record->net_votes();
          discrepancies[delegate_record->name] = discrepancy_record;
        }
      }

      return discrepancies;
   }

   fc::ripemd160 chain_database::get_current_random_seed()const
   { try {
      const optional<variant> result = get_property( last_random_seed_id );
      if( result.valid() ) return result->as<fc::ripemd160>();
      return fc::ripemd160();
   } FC_CAPTURE_AND_RETHROW() }

   oorder_record chain_database::get_bid_record( const market_index_key&  key )const
   {
      return my->_bid_db.fetch_optional(key);
   }

   oorder_record chain_database::get_relative_bid_record( const market_index_key&  key )const
   {
      return my->_relative_bid_db.fetch_optional(key);
   }

   omarket_order chain_database::get_lowest_ask_record( const asset_id_type quote_id, const asset_id_type base_id )
   {
      omarket_order result;
      auto itr = my->_ask_db.lower_bound( market_index_key( price(0,quote_id,base_id) ) );
      if( itr.valid() )
      {
         auto market_index = itr.key();
         if( market_index.order_price.quote_asset_id == quote_id &&
             market_index.order_price.base_asset_id == base_id      )
            return market_order( ask_order, market_index, itr.value() );
      }
      return result;
   }

   oorder_record chain_database::get_ask_record( const market_index_key&  key )const
   {
      return my->_ask_db.fetch_optional(key);
   }
   oorder_record chain_database::get_relative_ask_record( const market_index_key&  key )const
   {
      return my->_relative_ask_db.fetch_optional(key);
   }


   oorder_record chain_database::get_short_record( const market_index_key& key )const
   {
      return my->_short_db.fetch_optional(key);
   }

   ocollateral_record chain_database::get_collateral_record( const market_index_key& key )const
   {
      return my->_collateral_db.fetch_optional(key);
   }

   void chain_database::store_bid_record( const market_index_key& key, const order_record& order )
   {
      if( order.is_null() )
         my->_bid_db.remove( key );
      else
         my->_bid_db.store( key, order );
   }
   void chain_database::store_relative_bid_record( const market_index_key& key, const order_record& order )
   {
      if( order.is_null() )
         my->_relative_bid_db.remove( key );
      else
         my->_relative_bid_db.store( key, order );
   }

   void chain_database::store_ask_record( const market_index_key& key, const order_record& order )
   {
      if( order.is_null() )
         my->_ask_db.remove( key );
      else
         my->_ask_db.store( key, order );
   }

   void chain_database::store_relative_ask_record( const market_index_key& key, const order_record& order )
   {
      if( order.is_null() )
         my->_relative_ask_db.remove( key );
      else
         my->_relative_ask_db.store( key, order );
   }

   void chain_database::store_short_record( const market_index_key& key, const order_record& order )
   {
      if( order.is_null() )
         my->_short_db.remove( key );
      else
         my->_short_db.store( key, order );
   }

   void chain_database::store_collateral_record( const market_index_key& key, const collateral_record& collateral )
   {
      if( collateral.is_null() )
      {
         auto old_record = my->_collateral_db.fetch_optional(key);
         if( old_record && old_record->expiration != collateral.expiration)
         {
            my->_collateral_expiration_index.erase( {key.order_price.quote_asset_id,  old_record->expiration, key } );
         }
         my->_collateral_db.remove( key );
      }
      else
      {
         auto old_record = my->_collateral_db.fetch_optional(key);
         if( old_record && old_record->expiration != collateral.expiration)
         {
            my->_collateral_expiration_index.erase( {key.order_price.quote_asset_id,  old_record->expiration, key } );
            my->_collateral_expiration_index.insert( {key.order_price.quote_asset_id, collateral.expiration, key } );
         }
         my->_collateral_db.store( key, collateral );
      }
   }

   string chain_database::get_asset_symbol( const asset_id_type asset_id )const
   { try {
      auto asset_rec = get_asset_record( asset_id );
      FC_ASSERT( asset_rec.valid(), "Unknown Asset ID: ${id}", ("asset_id",asset_id) );
      return asset_rec->symbol;
   } FC_RETHROW_EXCEPTIONS( warn, "", ("asset_id",asset_id) ) }

   time_point_sec chain_database::get_genesis_timestamp()const
   {
       return get_asset_record( asset_id_type() )->registration_date;
   }

   void chain_database::sanity_check()const
   { try {
#if 0
      asset total;
      auto itr = my->_balance_db.begin();
      while( itr.valid() )
      {
         const asset ind( itr.value().balance, itr.value().condition.asset_id );
         if( ind.asset_id == 0 )
         {
            FC_ASSERT( ind.amount >= 0, "", ("record",itr.value()) );
            total += ind;
         }
         ++itr;
      }
      int64_t total_votes = 0;
      auto aitr = my->_account_db.begin();
      while( aitr.valid() )
      {
         auto v = aitr.value();
         if( v.is_delegate() )
         {
            total += asset(v.delegate_info->pay_balance);
            total_votes += v.delegate_info->votes_for;
         }
         ++aitr;
      }

//      FC_ASSERT( total_votes == total.amount, "",
 //                ("total_votes",total_votes)
  //               ("total_shares",total) );

      auto ar = get_asset_record( asset_id_type(0) );
      FC_ASSERT( ar.valid() );
      FC_ASSERT( ar->current_share_supply == total.amount, "", ("ar",ar)("total",total)("delta",ar->current_share_supply-total.amount) );
      FC_ASSERT( ar->current_share_supply <= ar->maximum_share_supply );
      //std::cerr << "Total Balances: " << to_pretty_asset( total ) << "\n";
#endif
   } FC_CAPTURE_AND_RETHROW() }

   /**
    *   Calculates the percentage of blocks produced in the last 10 rounds as an average
    *   measure of the delegate participation rate.
    *
    *   @return a value betwee 0 and 100
    */
   double chain_database::get_average_delegate_participation()const
   { try {
      const auto head_num = get_head_block_num();
      const auto now = bts::blockchain::now();
      if( head_num < 1 )
      {
          return 0;
      }
      else if( head_num <= BTS_BLOCKCHAIN_NUM_DELEGATES )
      {
         // what percent of the maximum total blocks that could have been produced
         // have been produced.
         const auto expected_blocks = (now - get_block_header( 1 ).timestamp).to_seconds() / BTS_BLOCKCHAIN_BLOCK_INTERVAL_SEC;
         return 100*double( head_num ) / expected_blocks;
      }
      else
      {
         // if 10*N blocks ago is longer than 10*N*INTERVAL_SEC ago then we missed blocks, calculate
         // in terms of percentage time rather than percentage blocks.
         const auto starting_time = get_block_header( head_num - BTS_BLOCKCHAIN_NUM_DELEGATES ).timestamp;
         const auto expected_production = (now - starting_time).to_seconds() / BTS_BLOCKCHAIN_BLOCK_INTERVAL_SEC;
         return 100*double( BTS_BLOCKCHAIN_NUM_DELEGATES ) / expected_production;
      }
   } FC_RETHROW_EXCEPTIONS( warn, "" ) }

   optional<market_order> chain_database::get_market_bid( const market_index_key& key )const
   { try {
       { // absolute bids
          auto market_itr  = my->_bid_db.find(key);
          if( market_itr.valid() )
             return market_order { bid_order, market_itr.key(), market_itr.value() };
       }
       { // relative bids
          auto market_itr  = my->_relative_bid_db.find(key);
          if( market_itr.valid() )
             return market_order { relative_bid_order, market_itr.key(), market_itr.value() };
       }

       return optional<market_order>();
   } FC_CAPTURE_AND_RETHROW( (key) ) }

   vector<market_order> chain_database::get_market_bids( const string& quote_symbol,
                                                          const string& base_symbol,
                                                          uint32_t limit  )
   { try {
       auto quote_id = get_asset_id( quote_symbol );
       auto base_id  = get_asset_id( base_symbol );
       if( base_id >= quote_id )
          FC_CAPTURE_AND_THROW( invalid_market, (quote_id)(base_id) );

       vector<market_order> results;

       //We dance around like this because the _bid_db sorts the bids backwards, so we must iterate it backwards.
       { // absolute bids
          const price next_pair = (base_id+1 == quote_id) ? price( 0, quote_id+1, 0 ) : price( 0, quote_id, base_id+1 );
          auto market_itr = my->_bid_db.lower_bound( market_index_key( next_pair ) );
          if( market_itr.valid() )   --market_itr;
          else market_itr = my->_bid_db.last();

          while( market_itr.valid() )
          {
             auto key = market_itr.key();
             if( key.order_price.quote_asset_id == quote_id &&
                 key.order_price.base_asset_id == base_id  )
             {
                results.push_back( {bid_order, key, market_itr.value()} );
             }
             else break;


             if( results.size() == limit )
                return results;

             --market_itr;
          }
       }
       { // relative bids
          const price next_pair = (base_id+1 == quote_id) ? price( 0, quote_id+1, 0 ) : price( 0, quote_id, base_id+1 );
          auto market_itr = my->_relative_bid_db.lower_bound( market_index_key( next_pair ) );
          if( market_itr.valid() )   --market_itr;
          else market_itr = my->_relative_bid_db.last();

          while( market_itr.valid() )
          {
             auto key = market_itr.key();
             if( key.order_price.quote_asset_id == quote_id &&
                 key.order_price.base_asset_id == base_id  )
             {
                results.push_back( {relative_bid_order, key, market_itr.value()} );
             }
             else break;


             if( results.size() == limit )
                return results;

             --market_itr;
          }
       }


       return results;
   } FC_CAPTURE_AND_RETHROW( (quote_symbol)(base_symbol)(limit) ) }

   optional<market_order> chain_database::get_market_short( const market_index_key& key )const
   { try {
       auto market_itr  = my->_short_db.find(key);
       if( market_itr.valid() )
          return market_order { short_order, market_itr.key(), market_itr.value() };

       return optional<market_order>();
   } FC_CAPTURE_AND_RETHROW( (key) ) }

   vector<market_order> chain_database::get_market_shorts( const string& quote_symbol,
                                                          uint32_t limit  )
   { try {
       asset_id_type quote_id = get_asset_id( quote_symbol );
       asset_id_type base_id  = 0;
       if( base_id >= quote_id )
          FC_CAPTURE_AND_THROW( invalid_market, (quote_id)(base_id) );

       vector<market_order> results;
       //We dance around like this because the database sorts the shorts backwards, so we must iterate it backwards.
       const price next_pair = (base_id+1 == quote_id) ? price( 0, quote_id+1, 0 ) : price( 0, quote_id, base_id+1 );
       auto market_itr = my->_short_db.lower_bound( market_index_key( next_pair ) );
       if( market_itr.valid() )   --market_itr;
       else market_itr = my->_short_db.last();

       while( market_itr.valid() )
       {
          auto key = market_itr.key();
          if( key.order_price.quote_asset_id == quote_id &&
              key.order_price.base_asset_id == base_id  )
          {
             order_record value = market_itr.value();
             results.push_back( {short_order, key, value, value.balance, key.order_price} );
          }
          else
          {
             break;
          }

          if( results.size() == limit )
             return results;

          --market_itr;
       }
       return results;
   } FC_CAPTURE_AND_RETHROW( (quote_symbol)(limit) ) }

   vector<market_order> chain_database::get_market_covers( const string& quote_symbol, uint32_t limit )
   { try {
       asset_id_type quote_asset_id = get_asset_id( quote_symbol );
       asset_id_type base_asset_id  = 0;
       if( base_asset_id >= quote_asset_id )
          FC_CAPTURE_AND_THROW( invalid_market, (quote_asset_id)(base_asset_id) );

       vector<market_order> results;

       auto market_itr  = my->_collateral_db.lower_bound( market_index_key( price( 0, quote_asset_id, base_asset_id ) ) );
       while( market_itr.valid() )
       {
          auto key = market_itr.key();
          if( key.order_price.quote_asset_id == quote_asset_id &&
              key.order_price.base_asset_id == base_asset_id  )
          {
             auto collat_record = market_itr.value();
             results.push_back( {cover_order,
                                 key,
                                 order_record(collat_record.payoff_balance),
                                 collat_record.collateral_balance,
                                 collat_record.interest_rate,
                                 collat_record.expiration } );
          }
          else
          {
             break;
          }

          if( results.size() == limit )
             return results;

          ++market_itr;
       }
       return results;
   } FC_CAPTURE_AND_RETHROW( (quote_symbol)(limit) ) }

   optional<market_order> chain_database::get_market_ask( const market_index_key& key )const
   { try {
       { // abs asks
          auto market_itr  = my->_ask_db.find(key);
          if( market_itr.valid() )
             return market_order { ask_order, market_itr.key(), market_itr.value() };
       }
       { // relative asks
          auto market_itr  = my->_relative_ask_db.find(key);
          if( market_itr.valid() )
             return market_order { relative_ask_order, market_itr.key(), market_itr.value() };
       }

       return optional<market_order>();
   } FC_CAPTURE_AND_RETHROW( (key) ) }


   share_type           chain_database::get_asset_collateral( const string& symbol )
   { try {
       asset_id_type quote_asset_id = get_asset_id( symbol);
       asset_id_type base_asset_id = 0;
       auto total = share_type(0);

       auto market_itr = my->_collateral_db.lower_bound( market_index_key( price( 0, quote_asset_id, base_asset_id ) ) );
       while( market_itr.valid() )
       {
           auto key = market_itr.key();
           if( key.order_price.quote_asset_id == quote_asset_id
               &&  key.order_price.base_asset_id == base_asset_id )
           {
               total += market_itr.value().collateral_balance;
           }
           else
           {
               break;
           }

           market_itr++;
       }
       return total;

   } FC_CAPTURE_AND_RETHROW( (symbol) ) }

   vector<market_order> chain_database::get_market_asks( const string& quote_symbol,
                                                          const string& base_symbol,
                                                          uint32_t limit  )
   { try {
       auto quote_asset_id = get_asset_id( quote_symbol );
       auto base_asset_id  = get_asset_id( base_symbol );
       if( base_asset_id >= quote_asset_id )
          FC_CAPTURE_AND_THROW( invalid_market, (quote_asset_id)(base_asset_id) );

       vector<market_order> results;
       { // absolute asks
          auto market_itr  = my->_ask_db.lower_bound( market_index_key( price( 0, quote_asset_id, base_asset_id ) ) );
          while( market_itr.valid() )
          {
             auto key = market_itr.key();
             if( key.order_price.quote_asset_id == quote_asset_id &&
                 key.order_price.base_asset_id == base_asset_id  )
             {
                results.push_back( {ask_order, key, market_itr.value()} );
             }
             else
             {
                break;
             }

             if( results.size() == limit )
                return results;

             ++market_itr;
          }
       }
       { // relative asks
          auto market_itr  = my->_relative_ask_db.lower_bound( market_index_key( price( 0, quote_asset_id, base_asset_id ) ) );
          while( market_itr.valid() )
          {
             auto key = market_itr.key();
             if( key.order_price.quote_asset_id == quote_asset_id &&
                 key.order_price.base_asset_id == base_asset_id  )
             {
                results.push_back( {relative_ask_order, key, market_itr.value()} );
             }
             else
             {
                break;
             }

             if( results.size() == limit )
                return results;

             ++market_itr;
          }
       }
       return results;
   } FC_CAPTURE_AND_RETHROW( (quote_symbol)(base_symbol)(limit) ) }

   vector<market_order> chain_database::scan_market_orders( std::function<bool( const market_order& )> filter,
                                                            uint32_t limit, order_type_enum type )const
   { try {
       vector<market_order> orders;
       if( limit == 0 ) return orders;

       if( type == null_order || type == ask_order )
       {
           for( auto itr = my->_ask_db.begin(); itr.valid(); ++itr )
           {
               const auto order = market_order( ask_order, itr.key(), itr.value() );
               if( filter( order ) )
               {
                   orders.push_back( order );
                   if( orders.size() >= limit )
                       return orders;
               }
           }
       }

       if( type == null_order || type == bid_order )
       {
           for( auto itr = my->_bid_db.begin(); itr.valid(); ++itr )
           {
               const auto order = market_order( bid_order, itr.key(), itr.value() );
               if( filter( order ) )
               {
                   orders.push_back( order );
                   if( orders.size() >= limit )
                       return orders;
               }
           }
       }

       if( type == null_order || type == relative_ask_order )
       {
           for( auto itr = my->_relative_ask_db.begin(); itr.valid(); ++itr )
           {
               const auto order = market_order( relative_ask_order, itr.key(), itr.value() );
               if( filter( order ) )
               {
                   orders.push_back( order );
                   if( orders.size() >= limit )
                       return orders;
               }
           }
       }

       if( type == null_order || type == relative_bid_order )
       {
           for( auto itr = my->_relative_bid_db.begin(); itr.valid(); ++itr )
           {
               const auto order = market_order( relative_bid_order, itr.key(), itr.value() );
               if( filter( order ) )
               {
                   orders.push_back( order );
                   if( orders.size() >= limit )
                       return orders;
               }
           }
       }

       if( type == null_order || type == short_order )
       {
           for( auto itr = my->_short_db.begin(); itr.valid(); ++itr )
           {
               const market_index_key& key = itr.key();
               const order_record& record = itr.value();
               const auto order = market_order( short_order, key, record, record.balance, key.order_price );
               if( filter( order ) )
               {
                   orders.push_back( order );
                   if( orders.size() >= limit )
                       return orders;
               }
           }
       }

       if( type == null_order || type == cover_order ) {
           for( auto itr = my->_collateral_db.begin(); itr.valid(); ++itr )
           {
               const auto collateral_rec = itr.value();
               const auto order_rec = order_record( collateral_rec.payoff_balance );
               const auto order = market_order( cover_order,
                                                itr.key(),
                                                order_rec,
                                                collateral_rec.collateral_balance,
                                                collateral_rec.interest_rate,
                                                collateral_rec.expiration );
               if( filter( order ) )
               {
                   orders.push_back( order );
                   if( orders.size() >= limit )
                       return orders;
               }
           }
       }

       return orders;
   } FC_RETHROW_EXCEPTIONS( warn, "" ) }

   optional<market_order> chain_database::get_market_order( const order_id_type& order_id, order_type_enum type )const
   { try {
       const auto filter = [&]( const market_order& order ) -> bool
       {
           return order.get_id() == order_id;
       };

       const auto orders = scan_market_orders( filter, 1, type );
       if( !orders.empty() )
           return orders.front();

       return optional<market_order>();
   } FC_RETHROW_EXCEPTIONS( warn, "" ) }

   pending_chain_state_ptr chain_database::get_pending_state()const
   {
      return my->_pending_trx_state;
   }

   odelegate_slate chain_database::get_delegate_slate( slate_id_type id )const
   {
      return my->_slate_db.fetch_optional( id );
   }

   void chain_database::store_delegate_slate( slate_id_type id, const delegate_slate& slate )
   {
      if( slate.supported_delegates.size() == 0 )
         my->_slate_db.remove( id );
      else
         my->_slate_db.store( id, slate );
   }

   void chain_database::store_slot_record( const slot_record& r )
   {
     if( !my->_track_stats ) return;
     if( r.is_null() )
         my->_slot_record_db.remove( r.start_time );
     else
         my->_slot_record_db.store( r.start_time, r );
   }

   oslot_record chain_database::get_slot_record( const time_point_sec& start_time )const
   {
     FC_ASSERT( my->_track_stats );
     return my->_slot_record_db.fetch_optional( start_time );
   }

   void chain_database::store_market_history_record(const market_history_key& key, const market_history_record& record)
   {
     if( !my->_track_stats ) return;
     if( record.volume == 0 )
       my->_market_history_db.remove( key );
     else
       my->_market_history_db.store( key, record );
   }

   omarket_history_record chain_database::get_market_history_record(const market_history_key& key) const
   {
     FC_ASSERT( my->_track_stats );
     return my->_market_history_db.fetch_optional( key );
   }

   vector<pair<asset_id_type, asset_id_type>> chain_database::get_market_pairs()const
   {
       vector<pair<asset_id_type, asset_id_type>> pairs;
       for( auto iter = my->_market_status_db.begin(); iter.valid(); ++iter )
           pairs.push_back( iter.key() );
       return pairs;
   }

   omarket_status chain_database::get_market_status( const asset_id_type quote_id, const asset_id_type base_id )
   {
      return my->_market_status_db.fetch_optional( std::make_pair(quote_id,base_id) );
   }

   void chain_database::store_market_status( const market_status& s )
   {
      if( s.is_null() )
      {
         my->_market_status_db.remove( std::make_pair( s.quote_id, s.base_id ) );
      }
      else
      {
         my->_market_status_db.store( std::make_pair( s.quote_id, s.base_id ), s );
      }
   }

   market_history_points chain_database::get_market_price_history( const asset_id_type quote_id,
                                                                   const asset_id_type base_id,
                                                                   const fc::time_point& start_time,
                                                                   const fc::microseconds& duration,
                                                                   market_history_key::time_granularity_enum granularity)
   {
      time_point_sec end_time = start_time + duration;
      auto record_itr = my->_market_history_db.lower_bound( market_history_key(quote_id, base_id, granularity, start_time) );
      market_history_points history;
      auto base = get_asset_record(base_id);
      auto quote = get_asset_record(quote_id);

      FC_ASSERT( base && quote );

      while( record_itr.valid()
             && record_itr.key().quote_id == quote_id
             && record_itr.key().base_id == base_id
             && record_itr.key().granularity == granularity
             && record_itr.key().timestamp <= end_time )
      {
        history.push_back( {
                             record_itr.key().timestamp,
                             fc::variant(string(record_itr.value().highest_bid.ratio * base->precision / quote->precision)).as_double() / (BTS_BLOCKCHAIN_MAX_SHARES*1000),
                             fc::variant(string(record_itr.value().lowest_ask.ratio * base->precision / quote->precision)).as_double() / (BTS_BLOCKCHAIN_MAX_SHARES*1000),
                             fc::variant(string(record_itr.value().opening_price.ratio * base->precision / quote->precision)).as_double() / (BTS_BLOCKCHAIN_MAX_SHARES*1000),
                             fc::variant(string(record_itr.value().closing_price.ratio * base->precision / quote->precision)).as_double() / (BTS_BLOCKCHAIN_MAX_SHARES*1000),
                             record_itr.value().volume
                           } );
        ++record_itr;
      }

      return history;
   }

   bool chain_database::is_known_transaction( const transaction& trx )const
   { try {
       return my->_unique_transactions.count( unique_transaction_key( trx, chain_id() ) ) > 0;
   } FC_CAPTURE_AND_RETHROW( (trx) ) }

   void chain_database::skip_signature_verification( bool state )
   {
      my->_skip_signature_verification = state;
   }

   void chain_database::set_relay_fee( share_type shares )
   {
      my->_relay_fee = shares;
   }

   share_type chain_database::get_relay_fee()
   {
      return my->_relay_fee;
   }

   void chain_database::set_market_transactions( vector<market_transaction> trxs )
   {
      FC_ASSERT( my->_track_stats );
      if( trxs.size() == 0 )
      {
         my->_market_transactions_db.remove( get_head_block_num()+1 );
      }
      else
      {
         my->_market_transactions_db.store( get_head_block_num()+1, trxs );
      }
   }

   vector<market_transaction> chain_database::get_market_transactions( uint32_t block_num  )const
   {
      FC_ASSERT( my->_track_stats );
      auto tmp = my->_market_transactions_db.fetch_optional(block_num);
      if( tmp ) return *tmp;
      return vector<market_transaction>();
   }

   vector<order_history_record> chain_database::market_order_history(asset_id_type quote,
                                                                     asset_id_type base,
                                                                     uint32_t skip_count,
                                                                     uint32_t limit,
                                                                     const address& owner)
   {
      FC_ASSERT(limit <= 10000, "Limit must be at most 10000!");

      auto current_block_num = get_head_block_num();
      auto get_transactions_from_prior_block = [&]() -> vector<market_transaction> {
          auto itr = my->_market_transactions_db.lower_bound(current_block_num);
          if (current_block_num == get_head_block_num())
              itr = my->_market_transactions_db.last();

          if (itr.valid()) --itr;
          if (itr.valid())
          {
              current_block_num = itr.key();
              return itr.value();
          }
          current_block_num = 1;
          return vector<market_transaction>();
      };

      FC_ASSERT(current_block_num > 0, "No blocks have been created yet!");
      auto orders = get_transactions_from_prior_block();

      std::function<bool(const market_transaction&)> order_is_uninteresting =
          [&quote,&base,&owner,this](const market_transaction& order) -> bool
      {
          //If it's in the market pair we're interested in, it might be interesting or uninteresting
          if( order.ask_price.base_asset_id == base
              && order.ask_price.quote_asset_id == quote ) {
            //If we're not filtering for a specific owner, it's interesting (not uninteresting)
            if (owner == address())
              return false;
            //If neither the bidder nor the asker is the owner I'm looking for, it's uninteresting
            return owner != order.bid_owner && owner != order.ask_owner;
          }
          //If it's not the market pair we're interested in, it's definitely uninteresting
          return true;
      };
      //Filter out orders not in our current market of interest
      orders.erase(std::remove_if(orders.begin(), orders.end(), order_is_uninteresting), orders.end());

      //While the next entire block of orders should be skipped...
      while( skip_count > 0 && --current_block_num > 0 && orders.size() <= skip_count ) {
        ilog("Skipping ${num} block ${block} orders", ("num", orders.size())("block", current_block_num));
        skip_count -= orders.size();
        orders = get_transactions_from_prior_block();
        orders.erase(std::remove_if(orders.begin(), orders.end(), order_is_uninteresting), orders.end());
      }

      if( current_block_num == 0 && orders.size() <= skip_count )
        // Skip count is greater or equal to the total number of relevant orders on the blockchain.
        return vector<order_history_record>();

      //If there are still some orders from the last block inspected to skip, remove them
      if( skip_count > 0 )
        orders.erase(orders.begin(), orders.begin() + skip_count);
      ilog("Building up order history, got ${num} so far...", ("num", orders.size()));

      std::vector<order_history_record> results;
      results.reserve(limit);
      fc::time_point_sec stamp = get_block_header(current_block_num).timestamp;
      for( const auto& rec : orders )
        results.push_back(order_history_record(rec, stamp));

      //While we still need more orders to reach our limit...
      while( --current_block_num >= 1 && orders.size() < limit )
      {
        auto more_orders = get_transactions_from_prior_block();
        more_orders.erase(std::remove_if(more_orders.begin(), more_orders.end(), order_is_uninteresting), more_orders.end());
        ilog("Found ${num} more orders in block ${block}...", ("num", more_orders.size())("block", current_block_num));
        stamp = get_block_header(current_block_num).timestamp;
        for( const auto& rec : more_orders )
          if( results.size() < limit )
            results.push_back(order_history_record(rec, stamp));
          else
            return results;
      }

      return results;
   }

   void chain_database::set_feed( const feed_record& r )
   {
       store( r );
   }

   ofeed_record chain_database::get_feed( const feed_index i )const
   {
       return lookup<feed_record>( i );
   }

   // This ignores all balances that aren't claim by signature
   map<string, share_type> chain_database::generate_snapshot()const
   {
       auto snapshot = map<string, share_type>();

       // normal / unclaimed balances
       for( auto iter = my->_balance_id_to_record.unordered_begin();
            iter != my->_balance_id_to_record.unordered_end(); ++iter )
       {
           const balance_record& balance = iter->second;
           if( balance.asset_id() != 0 ) continue;
           if( balance.condition.type != withdraw_signature_type ) continue;
           string claimer;
           if( balance.snapshot_info.valid() )
           {
               claimer = balance.snapshot_info->original_address;
           }
           else
           {
               const auto owner = balance.owner();
               if( !owner.valid() ) continue;
               claimer = string( *owner );
           }
           snapshot[claimer] += balance.get_spendable_balance( now() ).amount;
       }

       // pay balances
       for( auto account_itr = my->_account_id_to_record.unordered_begin();
            account_itr != my->_account_id_to_record.unordered_end(); ++account_itr )
       {
           const account_record& account = account_itr->second;
           if( account.delegate_info.valid() && !account.is_retracted() )
           {
               auto address = string( account.active_address() );
               snapshot[address] += account.delegate_info->pay_balance;
           }
       }

       // ask balances
       for( auto ask_itr = my->_ask_db.begin(); ask_itr.valid(); ++ask_itr )
       {
           const market_index_key market_index = ask_itr.key();
           if( market_index.order_price.base_asset_id == 0 )
           {
               auto address = string( ask_itr.key().owner );
               auto balance = ask_itr.value().balance;
               snapshot[address] += balance;
           }
       }

       // relative ask balances
       for( auto ask_itr = my->_relative_ask_db.begin(); ask_itr.valid(); ++ask_itr )
       {
           const market_index_key market_index = ask_itr.key();
           if( market_index.order_price.base_asset_id == 0 )
           {
               auto address = string( ask_itr.key().owner );
               auto balance = ask_itr.value().balance;
               snapshot[address] += balance;
           }
       }

       // Add short balances
       for( auto short_itr = my->_short_db.begin(); short_itr.valid(); ++short_itr )
       {
           auto address = string( short_itr.key().owner );
           auto balance = short_itr.value().balance;
           snapshot[address] += balance;
       }

       // Add collateral balances
       for( auto collateral_itr = my->_collateral_db.begin(); collateral_itr.valid(); ++collateral_itr )
       {
           auto address = string( collateral_itr.key().owner );
           auto balance = collateral_itr.value().collateral_balance;
           snapshot[address] += balance;
       }

       return snapshot;
   }

   asset chain_database::calculate_supply( const asset_id_type asset_id )const
   {
       const auto record = get_asset_record( asset_id );
       FC_ASSERT( record.valid() );

       // Add fees
       asset total( record->collected_fees, asset_id );

       // Add balances
       for( auto iter = my->_balance_id_to_record.unordered_begin();
            iter != my->_balance_id_to_record.unordered_end(); ++iter )
       {
           const balance_record& balance = iter->second;
           if( balance.asset_id() == total.asset_id )
               total.amount += balance.balance;
       }

       // Add ask balances
       for( auto ask_itr = my->_ask_db.begin(); ask_itr.valid(); ++ask_itr )
       {
           const market_index_key market_index = ask_itr.key();
           if( market_index.order_price.base_asset_id == total.asset_id )
           {
               const order_record ask = ask_itr.value();
               total.amount += ask.balance;
           }
       }
       for( auto ask_itr = my->_relative_ask_db.begin(); ask_itr.valid(); ++ask_itr )
       {
           const market_index_key market_index = ask_itr.key();
           if( market_index.order_price.base_asset_id == total.asset_id )
           {
               const order_record ask = ask_itr.value();
               total.amount += ask.balance;
           }
       }

       // If base asset
       if( asset_id == asset_id_type( 0 ) )
       {
           // Add short balances
           for( auto short_itr = my->_short_db.begin(); short_itr.valid(); ++short_itr )
           {
               const order_record sh = short_itr.value();
               total.amount += sh.balance;
           }

           // Add collateral balances
           for( auto collateral_itr = my->_collateral_db.begin(); collateral_itr.valid(); ++collateral_itr )
           {
               const collateral_record collateral = collateral_itr.value();
               total.amount += collateral.collateral_balance;
           }

           // Add pay balances
           for( auto account_itr = my->_account_id_to_record.unordered_begin();
                account_itr != my->_account_id_to_record.unordered_end(); ++account_itr )
           {
               const account_record& account = account_itr->second;
               if( account.delegate_info.valid() )
                   total.amount += account.delegate_info->pay_balance;
           }
       }
       else // If non-base asset
       {
           // Add bid balances
           for( auto bid_itr = my->_bid_db.begin(); bid_itr.valid(); ++bid_itr )
           {
               const market_index_key market_index = bid_itr.key();
               if( market_index.order_price.quote_asset_id == total.asset_id )
               {
                   const order_record bid = bid_itr.value();
                   total.amount += bid.balance;
               }
           }
           for( auto bid_itr = my->_relative_bid_db.begin(); bid_itr.valid(); ++bid_itr )
           {
               const market_index_key market_index = bid_itr.key();
               if( market_index.order_price.quote_asset_id == total.asset_id )
               {
                   const order_record bid = bid_itr.value();
                   total.amount += bid.balance;
               }
           }
       }

       return total;
   }

   asset chain_database::calculate_debt( const asset_id_type asset_id, bool include_interest )const
   {
       const auto record = get_asset_record( asset_id );
       FC_ASSERT( record.valid() && record->is_market_issued() );

       asset total( 0, asset_id );

       for( auto itr = my->_collateral_db.begin(); itr.valid(); ++itr )
       {
           const market_index_key& market_index = itr.key();
           if( market_index.order_price.quote_asset_id != asset_id ) continue;
           FC_ASSERT( market_index.order_price.base_asset_id == asset_id_type( 0 ) );

           const collateral_record& record = itr.value();
           const asset principle( record.payoff_balance, asset_id );
           total += principle;
           if( !include_interest ) continue;

           const time_point_sec position_start_time = record.expiration - BTS_BLOCKCHAIN_MAX_SHORT_PERIOD_SEC;
           const uint32_t position_age = (now() - position_start_time).to_seconds();
           total += detail::market_engine::get_interest_owed( principle, record.interest_rate, position_age );
       }

       return total;
   }

   asset chain_database::unclaimed_genesis()
   { try {
        asset unclaimed_total;
        const auto genesis_date = get_genesis_timestamp();
        const auto scan_balance = [ &unclaimed_total, &genesis_date ]( const balance_record& record )
        {
            if( record.snapshot_info.valid() )
            {
                if( record.last_update <= genesis_date )
                    unclaimed_total.amount += record.balance;
            }
        };
        scan_balances( scan_balance, false );
        return unclaimed_total;
   } FC_CAPTURE_AND_RETHROW() }

   /**
    *  Given the list of active delegates and price feeds for asset_id return the median value.
    */
   oprice chain_database::get_median_delegate_price( const asset_id_type quote_id, const asset_id_type base_id )const
   { try {
      auto feed_itr = my->_feed_index_to_record.lower_bound( feed_index{quote_id} );
      vector<account_id_type> active_delegates = get_active_delegates();
      std::sort( active_delegates.begin(), active_delegates.end() );
      vector<price> prices;
      while( feed_itr.valid() && feed_itr.key().quote_id == quote_id )
      {
         const feed_index key = feed_itr.key();
         if( std::binary_search( active_delegates.begin(), active_delegates.end(), key.delegate_id ) )
         {
            try {
               const feed_record& val = feed_itr.value();
               // only consider feeds updated in the past day
               if( (fc::time_point(val.last_update) + fc::days(1)) > fc::time_point(this->now()) )
               {
                   const price& feed_price = val.value;
                   if( feed_price.quote_asset_id == quote_id && feed_price.base_asset_id == base_id )
                       prices.push_back( feed_price );
               }
            }
            catch ( ... )
            { // we want to catch any exceptions caused attempted to interpret value as a price and simply ignore
              // the data feed...
            }
         }
         ++feed_itr;
      }

      if( prices.size() >= BTS_BLOCKCHAIN_MIN_FEEDS )
      {
         std::nth_element( prices.begin(), prices.begin() + prices.size()/2, prices.end() );
         return prices[prices.size()/2];
      }

      return oprice();
   } FC_CAPTURE_AND_RETHROW( (quote_id)(base_id) ) }

   vector<feed_record> chain_database::get_feeds_for_asset( const asset_id_type quote_id, const asset_id_type base_id )const
   {  try {
      vector<feed_record> feeds;
      auto feed_itr = my->_feed_index_to_record.lower_bound(feed_index{quote_id});
      while( feed_itr.valid() && feed_itr.key().quote_id == quote_id )
      {
        const auto& val = feed_itr.value();
        if( val.value.base_asset_id == base_id )
           feeds.push_back(val);
        ++feed_itr;
      }
      return feeds;
   } FC_CAPTURE_AND_RETHROW( (quote_id)(base_id) ) }

   vector<feed_record> chain_database::get_feeds_from_delegate( const account_id_type delegate_id )const
   {  try {
      vector<feed_record> records;
      for( auto iter = my->_feed_index_to_record.begin(); iter.valid(); ++iter )
      {
          const feed_record& record = iter.value();
          if( record.index.delegate_id == delegate_id )
              records.push_back( record );
      }
      return records;
   } FC_CAPTURE_AND_RETHROW( (delegate_id) ) }

   void chain_database::store_burn_record( const burn_record& br )
   {
      if( !my->_track_stats )
         return;

      if( br.is_null() )
      {
         my->_burn_db.remove( br );
      }
      else
         my->_burn_db.store( br, br );
   }

   oburn_record chain_database::fetch_burn_record( const burn_record_key& key )const
   {
      FC_ASSERT( my->_track_stats );
      auto oval = my->_burn_db.fetch_optional( key );
      if( oval )
         return burn_record( key, *oval );
      return oburn_record();
   }
   vector<burn_record> chain_database::fetch_burn_records( const string& account_name )const
   { try {
      FC_ASSERT( my->_track_stats );
      vector<burn_record> results;
      auto opt_account_record = get_account_record( account_name );
      FC_ASSERT( opt_account_record.valid() );

      auto itr = my->_burn_db.lower_bound( {opt_account_record->id} );
      while( itr.valid() && itr.key().account_id == opt_account_record->id )
      {
         results.push_back( burn_record( itr.key(), itr.value() ) );
         ++itr;
      }

      itr = my->_burn_db.lower_bound( {-opt_account_record->id} );
      while( itr.valid() && abs(itr.key().account_id) == opt_account_record->id )
      {
         results.push_back( burn_record( itr.key(), itr.value() ) );
         ++itr;
      }
      return results;
   } FC_CAPTURE_AND_RETHROW( (account_name) ) }

   void chain_database::dump_state( const fc::path& path )const
   { try {
       const auto dir = fc::absolute( path );
       FC_ASSERT( !fc::exists( dir ) );
       fc::create_directories( dir );

       fc::path next_path;
       ulog( "This will take a while..." );

       next_path = dir / "_market_transactions_db.json";
       my->_market_transactions_db.export_to_json( next_path );
       ulog( "Dumped ${p}", ("p",next_path) );

       next_path = dir / "_slate_db.json";
       my->_slate_db.export_to_json( next_path );
       ulog( "Dumped ${p}", ("p",next_path) );

       next_path = dir / "_property_db.json";
       my->_property_db.export_to_json( next_path );
       ulog( "Dumped ${p}", ("p",next_path) );

       next_path = dir / "_block_num_to_id_db.json";
       my->_block_num_to_id_db.export_to_json( next_path );
       ulog( "Dumped ${p}", ("p",next_path) );

       next_path = dir / "_block_id_to_block_record_db.json";
       my->_block_id_to_block_record_db.export_to_json( next_path );
       ulog( "Dumped ${p}", ("p",next_path) );

       next_path = dir / "_block_id_to_block_data_db.json";
       my->_block_id_to_block_data_db.export_to_json( next_path );
       ulog( "Dumped ${p}", ("p",next_path) );

       next_path = dir / "_id_to_transaction_record_db.json";
       my->_id_to_transaction_record_db.export_to_json( next_path );
       ulog( "Dumped ${p}", ("p",next_path) );

       //next_path = dir / "_asset_db.json";
       //my->_asset_db.export_to_json( next_path );
       //ulog( "Dumped ${p}", ("p",next_path) );

       //next_path = dir / "_balance_db.json";
       //my->_balance_db.export_to_json( next_path );
       //ulog( "Dumped ${p}", ("p",next_path) );

       next_path = dir / "_burn_db.json";
       my->_burn_db.export_to_json( next_path );
       ulog( "Dumped ${p}", ("p",next_path) );

       //next_path = dir / "_account_db.json";
       //my->_account_db.export_to_json( next_path );
       //ulog( "Dumped ${p}", ("p",next_path) );

       //next_path = dir / "_symbol_index_db.json";
       //my->_symbol_index_db.export_to_json( next_path );
       //ulog( "Dumped ${p}", ("p",next_path) );

       next_path = dir / "_slot_record_db.json";
       my->_slot_record_db.export_to_json( next_path );
       ulog( "Dumped ${p}", ("p",next_path) );

       next_path = dir / "_ask_db.json";
       my->_ask_db.export_to_json( next_path );
       ulog( "Dumped ${p}", ("p",next_path) );

       next_path = dir / "_bid_db.json";
       my->_bid_db.export_to_json( next_path );
       ulog( "Dumped ${p}", ("p",next_path) );

       next_path = dir / "_relative_ask_db.json";
       my->_relative_ask_db.export_to_json( next_path );
       ulog( "Dumped ${p}", ("p",next_path) );

       next_path = dir / "_relative_bid_db.json";
       my->_relative_bid_db.export_to_json( next_path );
       ulog( "Dumped ${p}", ("p",next_path) );

       next_path = dir / "_short_db.json";
       my->_short_db.export_to_json( next_path );
       ulog( "Dumped ${p}", ("p",next_path) );

       next_path = dir / "_collateral_db.json";
       my->_collateral_db.export_to_json( next_path );
       ulog( "Dumped ${p}", ("p",next_path) );

       //next_path = dir / "_feed_db.json";
       //my->_feed_db.export_to_json( next_path );
       //ulog( "Dumped ${p}", ("p",next_path) );

       next_path = dir / "_object_db.json";
       my->_object_db.export_to_json( next_path );
       ulog( "Dumped ${p}", ("p",next_path) );

       next_path = dir / "_edge_index.json";
       my->_edge_index.export_to_json( next_path );
       ulog( "Dumped ${p}", ("p",next_path) );

       next_path = dir / "_reverse_edge_index.json";
       my->_reverse_edge_index.export_to_json( next_path );
       ulog( "Dumped ${p}", ("p",next_path) );

       next_path = dir / "_market_status_db.json";
       my->_market_status_db.export_to_json( next_path );
       ulog( "Dumped ${p}", ("p",next_path) );

       next_path = dir / "_market_history_db.json";
       my->_market_history_db.export_to_json( next_path );
       ulog( "Dumped ${p}", ("p",next_path) );
   } FC_CAPTURE_AND_RETHROW( (path) ) }

   fc::variant_object chain_database::get_stats() const
   {
     fc::mutable_variant_object stats;
#define CHAIN_DB_DATABASES (_block_num_to_id_db)(_block_id_to_block_record_db)(_block_id_to_block_data_db) \
                           (_fork_number_db)(_fork_db)(_block_id_to_undo_state) \
                           (_property_db) \
                           (_account_id_to_record)(_account_name_to_id)(_account_address_to_id) \
                           (_asset_id_to_record)(_asset_symbol_to_id) \
                           (_balance_id_to_record)(_empty_balance_id_to_record) \
                           (_id_to_transaction_record_db)(_pending_transaction_db)(_pending_fee_index) \
                           (_slate_db)(_burn_db)(_slot_record_db) \
                           (_feed_index_to_record) \
                           (_ask_db)(_bid_db)(_short_db)(_collateral_db) \
                           (_market_transactions_db)(_market_status_db)(_market_history_db) \
                           (_object_db)(_edge_index)(_reverse_edge_index) \
                           (_recent_operations)
#define GET_DATABASE_SIZE(r, data, elem) stats[BOOST_PP_STRINGIZE(elem)] = my->elem.size();
     BOOST_PP_SEQ_FOR_EACH(GET_DATABASE_SIZE, _, CHAIN_DB_DATABASES)
     return stats;
   }

   void                        chain_database::authorize( asset_id_type asset_id, const address& owner, object_id_type oid  )
   {
      if( oid != -1 )
         my->_auth_db.store( std::make_pair( asset_id, owner ), oid );
      else
         my->_auth_db.remove( std::make_pair( asset_id, owner ) );
   }

   optional<object_id_type>    chain_database::get_authorization( asset_id_type asset_id, const address& owner )const
   {
      return my->_auth_db.fetch_optional( std::make_pair( asset_id, owner ) );
   }
   void                       chain_database::store_asset_proposal( const proposal_record& r )
   {
      if( r.info == -1 )
      {
         my->_asset_proposal_db.remove( r.key() );
      }
      else
      {
         my->_asset_proposal_db.store( r.key(), r );
      }
   }

   optional<proposal_record> chain_database::fetch_asset_proposal( asset_id_type asset_id, proposal_id_type proposal_id )const
   {
      return my->_asset_proposal_db.fetch_optional( std::make_pair(asset_id,proposal_id) );
   }

   void chain_database::index_transaction( const address& addr, const transaction_id_type& trx_id )
   {
      if( my->_track_stats )
         my->_address_to_trx_index.store( std::make_pair(addr,trx_id), char(0) );
   }

   vector<transaction_record> chain_database::fetch_address_transactions( const address& addr )
   {
      FC_ASSERT( my->_track_stats );
      vector<transaction_record> results;
      auto itr = my->_address_to_trx_index.lower_bound( std::make_pair(addr, transaction_id_type()) );
      while( itr.valid() )
      {
         auto key = itr.key();
         if( key.first != addr )
            break;

         if( auto otrx = get_transaction( key.second ) )
            results.push_back( *otrx );

         ++itr;
      }
      return results;
   }

   void chain_database::track_chain_statistics( bool status )
   {
      my->_track_stats = status;
   }

   void chain_database::init_account_db_interface()
   {
       account_db_interface& interface = _account_db_interface;

       interface.lookup_by_id = [&]( const account_id_type id ) -> oaccount_record
       {
           const auto iter = my->_account_id_to_record.unordered_find( id );
           if( iter != my->_account_id_to_record.unordered_end() ) return iter->second;
           return oaccount_record();
       };

       interface.lookup_by_name = [&]( const string& name ) -> oaccount_record
       {
           const auto iter = my->_account_name_to_id.unordered_find( name );
           if( iter != my->_account_name_to_id.unordered_end() ) return interface.lookup_by_id( iter->second );
           return oaccount_record();
       };

       interface.lookup_by_address = [&]( const address& addr ) -> oaccount_record
       {
           const auto iter = my->_account_address_to_id.unordered_find( addr );
           if( iter != my->_account_address_to_id.unordered_end() ) return interface.lookup_by_id( iter->second );
           return oaccount_record();
       };

       interface.insert_into_id_map = [&]( const account_id_type id, const account_record& record )
       {
           my->_account_id_to_record.store( id, record );
       };

       interface.insert_into_name_map = [&]( const string& name, const account_id_type id )
       {
           my->_account_name_to_id.store( name, id );
       };

       interface.insert_into_address_map = [&]( const address& addr, const account_id_type id )
       {
           my->_account_address_to_id.store( addr, id );
       };

       interface.insert_into_vote_set = [&]( const vote_del& vote )
       {
           my->_delegate_votes.insert( vote );
       };

       interface.erase_from_id_map = [&]( const account_id_type id )
       {
           my->_account_id_to_record.remove( id );
       };

       interface.erase_from_name_map = [&]( const string& name )
       {
           my->_account_name_to_id.remove( name );
       };

       interface.erase_from_address_map = [&]( const address& addr )
       {
           my->_account_address_to_id.remove( addr );
       };

       interface.erase_from_vote_set = [&]( const vote_del& vote )
       {
           my->_delegate_votes.erase( vote );
       };
   }

   void chain_database::init_asset_db_interface()
   {
       asset_db_interface& interface = _asset_db_interface;

       interface.lookup_by_id = [&]( const asset_id_type id ) -> oasset_record
       {
           const auto iter = my->_asset_id_to_record.unordered_find( id );
           if( iter != my->_asset_id_to_record.unordered_end() ) return iter->second;
           return oasset_record();
       };

       interface.lookup_by_symbol = [&]( const string& symbol ) -> oasset_record
       {
           const auto iter = my->_asset_symbol_to_id.unordered_find( symbol );
           if( iter != my->_asset_symbol_to_id.unordered_end() ) return interface.lookup_by_id( iter->second );
           return oasset_record();
       };

       interface.insert_into_id_map = [&]( const asset_id_type id, const asset_record& record )
       {
           my->_asset_id_to_record.store( id, record );
       };

       interface.insert_into_symbol_map = [&]( const string& symbol, const asset_id_type id )
       {
           my->_asset_symbol_to_id.store( symbol, id );
       };

       interface.erase_from_id_map = [&]( const asset_id_type id )
       {
           my->_asset_id_to_record.remove( id );
       };

       interface.erase_from_symbol_map = [&]( const string& symbol )
       {
           my->_asset_symbol_to_id.remove( symbol );
       };
   }

   void chain_database::init_balance_db_interface()
   {
       balance_db_interface& interface = _balance_db_interface;

       interface.lookup_by_id = [&]( const balance_id_type& id ) -> obalance_record
       {
           auto iter = my->_balance_id_to_record.unordered_find( id );
           if( iter != my->_balance_id_to_record.unordered_end() ) return iter->second;
           iter = my->_empty_balance_id_to_record.unordered_find( id );
           if( iter != my->_empty_balance_id_to_record.unordered_end() ) return iter->second;
           return obalance_record();
       };

       interface.insert_into_id_map = [&]( const balance_id_type& id, const balance_record& record )
       {
           if( record.balance != 0 )
           {
               my->_empty_balance_id_to_record.remove( id );
               my->_balance_id_to_record.store( id, record );
           }
           else
           {
               my->_balance_id_to_record.remove( id );
               my->_empty_balance_id_to_record.store( id, record );
           }
       };

       interface.erase_from_id_map = [&]( const balance_id_type& id )
       {
           my->_balance_id_to_record.remove( id );
           my->_empty_balance_id_to_record.remove( id );
       };
   }

   void chain_database::init_transaction_db_interface()
   {
       transaction_db_interface& interface = _transaction_db_interface;

       interface.lookup_by_id = [&]( const transaction_id_type& id ) -> otransaction_record
       {
           return my->_id_to_transaction_record_db.fetch_optional( id );
       };

       interface.insert_into_id_map = [&]( const transaction_id_type& id, const transaction_record& record )
       {
           my->_id_to_transaction_record_db.store( id, record );
       };

       interface.insert_into_unique_set = [&]( const transaction& trx )
       {
           if( trx.expiration > this->now() )
               my->_unique_transactions.emplace( trx, chain_id() );
       };

       interface.erase_from_id_map = [&]( const transaction_id_type& id )
       {
           my->_id_to_transaction_record_db.remove( id );
       };

       interface.erase_from_unique_set = [&]( const transaction& trx )
       {
           my->_unique_transactions.erase( unique_transaction_key( trx, chain_id() ) );
       };
   }

   void chain_database::init_feed_db_interface()
   {
       feed_db_interface& interface = _feed_db_interface;

       interface.lookup_by_index = [&]( const feed_index index ) -> ofeed_record
       {
           return my->_feed_index_to_record.fetch_optional( index );
       };

       interface.insert_into_index_map = [&]( const feed_index index, const feed_record& record )
       {
           my->_feed_index_to_record.store( index, record );
       };

       interface.erase_from_index_map = [&]( const feed_index index )
       {
           my->_feed_index_to_record.remove( index );
       };
   }

} } // bts::blockchain
