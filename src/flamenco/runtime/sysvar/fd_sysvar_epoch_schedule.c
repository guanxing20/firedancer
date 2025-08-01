#include "fd_sysvar_epoch_schedule.h"
#include "fd_sysvar.h"
#include "../fd_system_ids.h"
#include "../context/fd_exec_slot_ctx.h"
fd_epoch_schedule_t *
fd_epoch_schedule_derive( fd_epoch_schedule_t * schedule,
                          ulong                 epoch_len,
                          ulong                 leader_schedule_slot_offset,
                          int                   warmup ) {

  if( FD_UNLIKELY( epoch_len < FD_EPOCH_LEN_MIN ) ) {
    FD_LOG_WARNING(( "epoch_len too small" ));
    return NULL;
  }

  *schedule = (fd_epoch_schedule_t) {
    .slots_per_epoch             = epoch_len,
    .leader_schedule_slot_offset = leader_schedule_slot_offset,
    .warmup                      = !!warmup
  };

  if( warmup ) {
    ulong ceil_log2_epoch   = (ulong)fd_ulong_find_msb( epoch_len-1UL ) + 1UL;
    ulong ceil_log2_len_min = (ulong)fd_ulong_find_msb( FD_EPOCH_LEN_MIN );

    schedule->first_normal_epoch = fd_ulong_sat_sub( ceil_log2_epoch, ceil_log2_len_min );
    schedule->first_normal_slot  = (1UL << ceil_log2_epoch) - FD_EPOCH_LEN_MIN;
  }

  return schedule;
}

void
fd_sysvar_epoch_schedule_write( fd_exec_slot_ctx_t *        slot_ctx,
                                fd_epoch_schedule_t const * epoch_schedule ) {
  ulong sz = fd_epoch_schedule_size( epoch_schedule );
  /* TODO remove alloca */
  uchar enc[ sz ];
  memset( enc, 0, sz );
  fd_bincode_encode_ctx_t ctx = {
    .data    = enc,
    .dataend = enc + sz
  };
  if( fd_epoch_schedule_encode( epoch_schedule, &ctx ) ) {
    FD_LOG_ERR(("fd_epoch_schedule_encode failed"));
  }

  fd_sysvar_set( slot_ctx->bank, slot_ctx->funk, slot_ctx->funk_txn, &fd_sysvar_owner_id, &fd_sysvar_epoch_schedule_id, enc, sz, fd_bank_slot_get( slot_ctx->bank ) );
}

fd_epoch_schedule_t *
fd_sysvar_epoch_schedule_read( fd_funk_t *           funk,
                               fd_funk_txn_t *       funk_txn,
                               fd_epoch_schedule_t * out ) {

  FD_TXN_ACCOUNT_DECL( acc );
  int err = fd_txn_account_init_from_funk_readonly( acc, &fd_sysvar_epoch_schedule_id, funk, funk_txn );
  if( FD_UNLIKELY( err != FD_ACC_MGR_SUCCESS ) ) {
    return NULL;
  }

  /* This check is needed as a quirk of the fuzzer. If a sysvar account
     exists in the accounts database, but doesn't have any lamports,
     this means that the account does not exist. This wouldn't happen
     in a real execution environment. */
  if( FD_UNLIKELY( acc->vt->get_lamports( acc ) == 0UL ) ) {
    return NULL;
  }

  return fd_bincode_decode_static(
      epoch_schedule, out,
      acc->vt->get_data( acc ),
      acc->vt->get_data_len( acc ),
      &err );
}

void
fd_sysvar_epoch_schedule_init( fd_exec_slot_ctx_t * slot_ctx ) {
  fd_epoch_schedule_t const * epoch_schedule = fd_bank_epoch_schedule_query( slot_ctx->bank );
  fd_sysvar_epoch_schedule_write( slot_ctx, epoch_schedule );
}

/* https://github.com/solana-labs/solana/blob/88aeaa82a856fc807234e7da0b31b89f2dc0e091/sdk/program/src/epoch_schedule.rs#L105 */

ulong
fd_epoch_slot_cnt( fd_epoch_schedule_t const * schedule,
                   ulong                       epoch ) {

  if( FD_UNLIKELY( epoch < schedule->first_normal_epoch ) ) {
    ulong exp = fd_ulong_sat_add( epoch, (ulong)fd_ulong_find_lsb( FD_EPOCH_LEN_MIN ) );
    return ( exp<64UL ? 1UL<<exp : ULONG_MAX ); // saturating_pow
  }

  return schedule->slots_per_epoch;
}

/* https://github.com/solana-labs/solana/blob/88aeaa82a856fc807234e7da0b31b89f2dc0e091/sdk/program/src/epoch_schedule.rs#L170 */

ulong
fd_epoch_slot0( fd_epoch_schedule_t const * schedule,
                ulong                       epoch ) {
  if( FD_UNLIKELY( epoch <= schedule->first_normal_epoch ) ) {
    ulong power = fd_ulong_if( epoch<64UL, 1UL<<epoch, ULONG_MAX );
    return fd_ulong_sat_mul( power-1UL, FD_EPOCH_LEN_MIN );
  }

  return fd_ulong_sat_add(
          fd_ulong_sat_mul(
            fd_ulong_sat_sub(
              epoch,
              schedule->first_normal_epoch),
            schedule->slots_per_epoch),
          schedule->first_normal_slot);
}

/* https://github.com/solana-labs/solana/blob/88aeaa82a856fc807234e7da0b31b89f2dc0e091/sdk/program/src/epoch_schedule.rs#L140 */

ulong
fd_slot_to_epoch( fd_epoch_schedule_t const * schedule,
                  ulong                       slot,
                  ulong *                     out_offset_opt ) {

  if( FD_UNLIKELY( schedule->slots_per_epoch == 0UL ) ) {
    FD_LOG_WARNING(( "zero slots_per_epoch" ));
    return 0UL;
  }

  ulong epoch;
  ulong offset;

  if( FD_UNLIKELY( slot < schedule->first_normal_slot ) ) {
    /* step0 = ceil(log2(FD_EPOCH_LEN_MIN))
       step1 = ceil(log2(FD_EPOCH_LEN_MIN + slot + 1))
       epoch = step1 - step0 - 1 */

    ulong s0 = FD_EPOCH_LEN_MIN + slot + 1UL;
    /* Invariant: s0 > 1UL */
    /* Invariant: s0 > FD_EPOCH_LEN_MIN */

    /* Find lowest exp where (2^exp) >= s0
       (Only valid for s0 > 1UL and FD_EPOCH_LEN_MIN > 1UL) */
    int   exp       = fd_ulong_find_msb( s0-1UL ) + 1;
    int   min_exp   = fd_ulong_find_msb( FD_EPOCH_LEN_MIN );
          epoch     = (ulong)( exp - min_exp - 1 );
    ulong epoch_len = 1UL<<( epoch + (ulong)fd_uint_find_lsb( FD_EPOCH_LEN_MIN ) );
          offset    = slot - ( epoch_len - FD_EPOCH_LEN_MIN );
  } else {
    // FD_LOG_WARNING(("First %lu slots per epoch %lu", schedule->first_normal_slot, schedule->slots_per_epoch));
    ulong n_slot  = slot - schedule->first_normal_slot;
    ulong n_epoch = n_slot / schedule->slots_per_epoch;
          epoch   = schedule->first_normal_epoch + n_epoch;
          offset  = n_slot % schedule->slots_per_epoch;
  }

  ulong   dummy_out;
  ulong * out_offset = out_offset_opt ? out_offset_opt : &dummy_out;

  *out_offset = offset;
  return epoch;
}

/* https://github.com/firedancer-io/solana/blob/dab3da8e7b667d7527565bddbdbecf7ec1fb868e/sdk/program/src/epoch_schedule.rs#L114 */

ulong
fd_slot_to_leader_schedule_epoch( fd_epoch_schedule_t const * schedule,
                                  ulong                       slot ) {

  if( slot < schedule->first_normal_slot )
    return fd_slot_to_epoch( schedule, slot, NULL ) + 1UL;

  /* These variable names ... sigh */

  ulong new_slots_since_first_normal_slot =
    slot - schedule->first_normal_slot;
  ulong new_first_normal_leader_schedule_slot =
    new_slots_since_first_normal_slot + schedule->leader_schedule_slot_offset;
  ulong new_epochs_since_first_normal_leader_schedule =
    new_first_normal_leader_schedule_slot / schedule->slots_per_epoch;

  return schedule->first_normal_epoch + new_epochs_since_first_normal_leader_schedule;
}
