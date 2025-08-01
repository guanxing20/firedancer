#ifndef HEADER_fd_src_flamenco_runtime_program_fd_vote_program_h
#define HEADER_fd_src_flamenco_runtime_program_fd_vote_program_h

/* The vote program (native program) allows node operators to register
   their nodes and participate in consensus.  The vote program
   implements various Tower BFT logic like voting and lockouts.  The set
   of vote accounts is the 'source of truth' for Solana's consensus
   algorithm.

   Address: Vote111111111111111111111111111111111111111 */

#include "../context/fd_exec_instr_ctx.h"
#include "../fd_bank.h"

/* Vote program custom error codes */

#define FD_VOTE_ERROR_VOTE_TOO_OLD                  (  0 )
#define FD_VOTE_ERR_SLOTS_MISMATCH                  (  1 )
#define FD_VOTE_ERR_SLOTS_HASH_MISMATCH             (  2 )
#define FD_VOTE_ERR_EMPTY_SLOTS                     (  3 )
#define FD_VOTE_ERR_TIMESTAMP_TOO_OLD               (  4 )
#define FD_VOTE_ERR_TOO_SOON_TO_REAUTHORIZE         (  5 )
#define FD_VOTE_ERR_LOCKOUT_CONFLICT                (  6 )
#define FD_VOTE_ERR_NEW_VOTE_STATE_LOCKOUT_MISMATCH (  7 )
#define FD_VOTE_ERR_SLOTS_NOT_ORDERED               (  8 )
#define FD_VOTE_ERR_CONFIRMATIONS_NOT_ORDERED       (  9 )
#define FD_VOTE_ERR_ZERO_CONFIRMATIONS              ( 10 )
#define FD_VOTE_ERR_CONFIRMATION_TOO_LARGE          ( 11 )
#define FD_VOTE_ERR_ROOT_ROLL_BACK                  ( 12 )
#define FD_VOTE_ERR_CONFIRMATION_ROLL_BACK          ( 13 )
#define FD_VOTE_ERR_SLOT_SMALLER_THAN_ROOT          ( 14 )
#define FD_VOTE_ERR_TOO_MANY_VOTES                  ( 15 )
#define FD_VOTE_ERR_VOTES_TOO_OLD_ALL_FILTERED      ( 16 )
#define FD_VOTE_ERR_ROOT_ON_DIFFERENT_FORK          ( 17 )
#define FD_VOTE_ERR_ACTIVE_VOTE_ACCOUNT_CLOSE       ( 18 )
#define FD_VOTE_ERR_COMMISSION_UPDATE_TOO_LATE      ( 19 )

#define FD_VOTE_STATE_V2_SZ (3731UL)
#define FD_VOTE_STATE_V3_SZ (3762UL)

FD_PROTOTYPES_BEGIN

/* fd_vote_program_execute is the instruction processing entrypoint
   for the vote program.  On return, ctx.txn_ctx->dirty_vote_acc==1 if a
   vote account may have been modified. */

int
fd_vote_program_execute( fd_exec_instr_ctx_t * ctx );

/* https://github.com/anza-xyz/agave/blob/v2.0.1/sdk/program/src/vote/state/vote_state_versions.rs#L90 */
uint
fd_vote_state_versions_is_correct_and_initialized( fd_txn_account_t * vote_account );

/* Queries the delegated stake amount for the given vote account pubkey,
   given the vote accounts map. Returns 0 if nonexistent. */
ulong
fd_query_pubkey_stake( fd_pubkey_t const * pubkey, fd_vote_accounts_global_t const * vote_accounts );

/* An implementation of solana_sdk::transaction_context::BorrowedAccount::get_state
   for setting the vote state.

   https://github.com/anza-xyz/agave/blob/v2.1.14/sdk/src/transaction_context.rs#L965 */
int
fd_vote_get_state( fd_txn_account_t const *      self,
                   fd_spad_t *                   spad,
                   fd_vote_state_versioned_t * * versioned /* out */ );

void
fd_vote_convert_to_current( fd_vote_state_versioned_t * self,
                            fd_spad_t *                 spad );

void
fd_vote_record_timestamp_vote_with_slot( fd_pubkey_t const *  vote_acc,
                                         long                 timestamp,
                                         ulong                slot,
                                         fd_bank_t *          bank );

struct fd_commission_split {
  ulong voter_portion;
  ulong staker_portion;
  uint  is_split;
};
typedef struct fd_commission_split fd_commission_split_t;

void
fd_vote_commission_split( fd_vote_state_versioned_t * vote_state_versioned,
                          ulong                       on,
                          fd_commission_split_t *     result );

void
fd_vote_store_account( fd_txn_account_t *   vote_account,
                       fd_bank_t *          bank );

FD_PROTOTYPES_END

#endif /* HEADER_fd_src_flamenco_runtime_program_fd_vote_program_h */
