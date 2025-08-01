#include "fd_bpf_loader_program.h"

/* For additional context see https://solana.com/docs/programs/deploying#state-accounts */

#include "../fd_pubkey_utils.h"
#include "../../../ballet/base58/fd_base58.h"
#include "../../../ballet/sbpf/fd_sbpf_loader.h"
#include "../sysvar/fd_sysvar_clock.h"
#include "../sysvar/fd_sysvar_rent.h"
#include "../../vm/syscall/fd_vm_syscall.h"
#include "../../vm/fd_vm.h"
#include "../fd_executor.h"
#include "fd_bpf_loader_serialization.h"
#include "fd_native_cpi.h"
#include "../fd_borrowed_account.h"

#include <stdlib.h>

static char * trace_buf;

static void __attribute__((constructor)) make_buf(void) {
  trace_buf = (char*)malloc(256*1024);
}

static void __attribute__((destructor)) free_buf(void) {
  free(trace_buf);
}

/* https://github.com/anza-xyz/agave/blob/ced98f1ebe73f7e9691308afa757323003ff744f/sdk/program/src/program_error.rs#L290-L335 */
static inline int
program_error_to_instr_error( ulong  err,
                              uint * custom_err ) {
  switch( err ) {
    case CUSTOM_ZERO:
      *custom_err = 0;
      return FD_EXECUTOR_INSTR_ERR_CUSTOM_ERR;
    case INVALID_ARGUMENT:
      return FD_EXECUTOR_INSTR_ERR_INVALID_ARG;
    case INVALID_INSTRUCTION_DATA:
      return FD_EXECUTOR_INSTR_ERR_INVALID_INSTR_DATA;
    case INVALID_ACCOUNT_DATA:
      return FD_EXECUTOR_INSTR_ERR_INVALID_ACC_DATA;
    case ACCOUNT_DATA_TOO_SMALL:
      return FD_EXECUTOR_INSTR_ERR_ACC_DATA_TOO_SMALL;
    case INSUFFICIENT_FUNDS:
      return FD_EXECUTOR_INSTR_ERR_INSUFFICIENT_FUNDS;
    case INCORRECT_PROGRAM_ID:
      return FD_EXECUTOR_INSTR_ERR_INCORRECT_PROGRAM_ID;
    case MISSING_REQUIRED_SIGNATURES:
      return FD_EXECUTOR_INSTR_ERR_MISSING_REQUIRED_SIGNATURE;
    case ACCOUNT_ALREADY_INITIALIZED:
      return FD_EXECUTOR_INSTR_ERR_ACC_ALREADY_INITIALIZED;
    case UNINITIALIZED_ACCOUNT:
      return FD_EXECUTOR_INSTR_ERR_UNINITIALIZED_ACCOUNT;
    case NOT_ENOUGH_ACCOUNT_KEYS:
      return FD_EXECUTOR_INSTR_ERR_NOT_ENOUGH_ACC_KEYS;
    case ACCOUNT_BORROW_FAILED:
      return FD_EXECUTOR_INSTR_ERR_ACC_BORROW_FAILED;
    case MAX_SEED_LENGTH_EXCEEDED:
      return FD_EXECUTOR_INSTR_ERR_MAX_SEED_LENGTH_EXCEEDED;
    case INVALID_SEEDS:
      return FD_EXECUTOR_INSTR_ERR_INVALID_SEEDS;
    case BORSH_IO_ERROR:
      return FD_EXECUTOR_INSTR_ERR_BORSH_IO_ERROR;
    case ACCOUNT_NOT_RENT_EXEMPT:
      return FD_EXECUTOR_INSTR_ERR_ACC_NOT_RENT_EXEMPT;
    case UNSUPPORTED_SYSVAR:
      return FD_EXECUTOR_INSTR_ERR_UNSUPPORTED_SYSVAR;
    case ILLEGAL_OWNER:
      return FD_EXECUTOR_INSTR_ERR_ILLEGAL_OWNER;
    case MAX_ACCOUNTS_DATA_ALLOCATIONS_EXCEEDED:
      return FD_EXECUTOR_INSTR_ERR_MAX_ACCS_DATA_ALLOCS_EXCEEDED;
    case INVALID_ACCOUNT_DATA_REALLOC:
      return FD_EXECUTOR_INSTR_ERR_INVALID_REALLOC;
    case MAX_INSTRUCTION_TRACE_LENGTH_EXCEEDED:
      return FD_EXECUTOR_INSTR_ERR_MAX_INSN_TRACE_LENS_EXCEEDED;
    case BUILTIN_PROGRAMS_MUST_CONSUME_COMPUTE_UNITS:
      return FD_EXECUTOR_INSTR_ERR_BUILTINS_MUST_CONSUME_CUS;
    case INVALID_ACCOUNT_OWNER:
      return FD_EXECUTOR_INSTR_ERR_INVALID_ACC_OWNER;
    case ARITHMETIC_OVERFLOW:
      return FD_EXECUTOR_INSTR_ERR_ARITHMETIC_OVERFLOW;
    case IMMUTABLE:
      return FD_EXECUTOR_INSTR_ERR_ACC_IMMUTABLE;
    case INCORRECT_AUTHORITY:
      return FD_EXECUTOR_INSTR_ERR_INCORRECT_AUTHORITY;
    default:
      if( err>>BUILTIN_BIT_SHIFT == 0 ) {
        *custom_err = (uint)err;
        return FD_EXECUTOR_INSTR_ERR_CUSTOM_ERR;
      }
      return FD_EXECUTOR_INSTR_ERR_INVALID_ERR;
  }
}

/* TODO: This can be combined with the other bpf loader state decode function */
fd_bpf_upgradeable_loader_state_t *
read_bpf_upgradeable_loader_state_for_program( fd_exec_txn_ctx_t * txn_ctx,
                                               ushort              program_id,
                                               int *               opt_err ) {
  fd_txn_account_t * rec = NULL;
  int err = fd_exec_txn_ctx_get_account_at_index( txn_ctx,
                                                  program_id,
                                                  &rec,
                                                  fd_txn_account_check_exists );
  if( FD_UNLIKELY( err ) ) {
    if( opt_err ) {
      *opt_err = err;
    }
    return NULL;
  }

  fd_bpf_upgradeable_loader_state_t * res = fd_bincode_decode_spad(
      bpf_upgradeable_loader_state,
      txn_ctx->spad,
      rec->vt->get_data( rec ),
      rec->vt->get_data_len( rec ),
      &err );
  if( FD_UNLIKELY( err ) ) {
    if( opt_err ) {
      *opt_err = FD_EXECUTOR_INSTR_ERR_INVALID_ACC_DATA;
    }
    return NULL;
  }
  return res;
}

/* https://github.com/anza-xyz/agave/blob/9b22f28104ec5fd606e4bb39442a7600b38bb671/programs/bpf_loader/src/lib.rs#L216-L229 */
static ulong
calculate_heap_cost( ulong heap_size, ulong heap_cost ) {
  #define KIBIBYTE_MUL_PAGES       (1024UL * 32UL)
  #define KIBIBYTE_MUL_PAGES_SUB_1 (KIBIBYTE_MUL_PAGES - 1UL)

  heap_size = fd_ulong_sat_add( heap_size, KIBIBYTE_MUL_PAGES_SUB_1 );

  heap_size = fd_ulong_sat_mul( fd_ulong_sat_sub( heap_size / KIBIBYTE_MUL_PAGES, 1UL ), heap_cost );
  return heap_size;

  #undef KIBIBYTE_MUL_PAGES
  #undef KIBIBYTE_MUL_PAGES_SUB_1
}

/* https://github.com/anza-xyz/agave/blob/574bae8fefc0ed256b55340b9d87b7689bcdf222/programs/bpf_loader/src/lib.rs#L105-L171

   Our arguments to deploy_program are different from the Agave version because
   we handle the caching of deployed programs differently. In Firedancer we
   lack the concept of ProgramCacheEntryType entirely.
   https://github.com/anza-xyz/agave/blob/114d94a25e9631f9bf6349c4b833d7900ef1fb1c/program-runtime/src/loaded_programs.rs#L158

   In Agave there is a separate caching structure that is used to store the
   deployed programs. In Firedancer the deployed, validated program is stored as
  metadata for the account in the funk record.

   See https://github.com/firedancer-io/firedancer/blob/9c1df680b3f38bebb0597e089766ec58f3b41e85/src/flamenco/runtime/program/fd_bpf_loader_v3_program.c#L1640
   for how we handle the concept of 'LoadedProgramType::DelayVisibility' in Firedancer.

   As a concrete example, our version of deploy_program does not have the
   'account_size' argument because we do not update the funk record here.

   The spad used for allocations can be either scoped to the executor or the
   runtime depending on where it is called from. If a program is deployed from
   the v3 contract, then the executor spad should be used. */
int
fd_deploy_program( fd_exec_instr_ctx_t * instr_ctx,
                   uchar const *         programdata,
                   ulong                 programdata_size,
                   fd_spad_t *           spad ) {
  int deploy_mode    = 1;
  int direct_mapping = FD_FEATURE_ACTIVE_BANK( instr_ctx->txn_ctx->bank, bpf_account_data_direct_mapping );
  fd_sbpf_syscalls_t * syscalls = fd_sbpf_syscalls_new( fd_spad_alloc( spad,
                                                                       fd_sbpf_syscalls_align(),
                                                                       fd_sbpf_syscalls_footprint() ) );
  if( FD_UNLIKELY( !syscalls ) ) {
    //TODO: full log including err
    fd_log_collector_msg_literal( instr_ctx, "Failed to register syscalls" );
    return FD_EXECUTOR_INSTR_ERR_PROGRAM_ENVIRONMENT_SETUP_FAILURE;
  }

  fd_vm_syscall_register_slot( syscalls,
                               instr_ctx->txn_ctx->slot,
                               &instr_ctx->txn_ctx->features,
                               1 );

  /* Load executable */
  fd_sbpf_elf_info_t  _elf_info[ 1UL ];
  uint min_sbpf_version, max_sbpf_version;
  fd_bpf_get_sbpf_versions( &min_sbpf_version,
                            &max_sbpf_version,
                            instr_ctx->txn_ctx->slot,
                            &instr_ctx->txn_ctx->features );
  fd_sbpf_elf_info_t * elf_info = fd_sbpf_elf_peek( _elf_info, programdata, programdata_size, deploy_mode, min_sbpf_version, max_sbpf_version );
  if( FD_UNLIKELY( !elf_info ) ) {
    //TODO: actual log, this is a custom Firedancer msg
    fd_log_collector_msg_literal( instr_ctx, "Failed to load or verify Elf" );
    return FD_EXECUTOR_INSTR_ERR_INVALID_ACC_DATA;
  }

  /* Allocate rodata segment */
  void * rodata = fd_spad_alloc( spad, FD_SBPF_PROG_RODATA_ALIGN, elf_info->rodata_footprint );
  if( FD_UNLIKELY( !rodata ) ) {
    return FD_EXECUTOR_INSTR_ERR_INVALID_ACC_DATA;
  }

  /* Allocate program buffer */
  ulong  prog_align        = fd_sbpf_program_align();
  ulong  prog_footprint    = fd_sbpf_program_footprint( elf_info );
  fd_sbpf_program_t * prog = fd_sbpf_program_new( fd_spad_alloc( spad, prog_align, prog_footprint ), elf_info, rodata );
  if( FD_UNLIKELY( !prog ) ) {
    FD_LOG_ERR(( "fd_sbpf_program_new() failed: %s", fd_sbpf_strerror() ));
  }

  /* Load program */
  int err = fd_sbpf_program_load( prog, programdata, programdata_size, syscalls, deploy_mode );
  if( FD_UNLIKELY( err ) ) {
    return FD_EXECUTOR_INSTR_ERR_INVALID_ACC_DATA;
  }

  /* Validate the program */
  fd_vm_t _vm[ 1UL ];
  fd_vm_t * vm = fd_vm_join( fd_vm_new( _vm ) );

  vm = fd_vm_init(
    /* vm                 */ vm,
    /* instr_ctx          */ instr_ctx,
    /* heap_max           */ instr_ctx->txn_ctx->compute_budget_details.heap_size,
    /* entry_cu           */ instr_ctx->txn_ctx->compute_budget_details.compute_meter,
    /* rodata             */ prog->rodata,
    /* rodata_sz          */ prog->rodata_sz,
    /* text               */ prog->text,
    /* text_cnt           */ prog->text_cnt,
    /* text_off           */ prog->text_off, /* FIXME: What if text_off is not multiple of 8 */
    /* text_sz            */ prog->text_sz,
    /* entry_pc           */ prog->entry_pc,
    /* calldests          */ prog->calldests,
    /* sbpf_version       */ elf_info->sbpf_version,
    /* syscalls           */ syscalls,
    /* trace              */ NULL,
    /* sha                */ NULL,
    /* mem_regions        */ NULL,
    /* mem_regions_cnt    */ 0,
    /* mem_region_accs    */ NULL,
    /* is_deprecated      */ 0,
    /* direct mapping     */ direct_mapping,
    /* dump_syscall_to_pb */ 0 );
  if ( FD_UNLIKELY( vm == NULL ) ) {
    FD_LOG_WARNING(( "NULL vm" ));
    return FD_EXECUTOR_INSTR_ERR_PROGRAM_ENVIRONMENT_SETUP_FAILURE;
  }

  int validate_result = fd_vm_validate( vm );
  if( FD_UNLIKELY( validate_result!=FD_VM_SUCCESS ) ) {
    return FD_EXECUTOR_INSTR_ERR_INVALID_ACC_DATA;
  }
  return FD_EXECUTOR_INSTR_SUCCESS;
}

/* https://github.com/anza-xyz/agave/blob/574bae8fefc0ed256b55340b9d87b7689bcdf222/programs/bpf_loader/src/lib.rs#L195-L218 */
static int
write_program_data( fd_exec_instr_ctx_t *   instr_ctx,
                    ushort                  instr_acc_idx,
                    ulong                   program_data_offset,
                    uchar *                 bytes,
                    ulong                   bytes_len ) {
  int err;

  /* https://github.com/anza-xyz/agave/blob/v2.1.4/programs/bpf_loader/src/lib.rs#L202 */
  fd_guarded_borrowed_account_t program;
  FD_TRY_BORROW_INSTR_ACCOUNT_DEFAULT_ERR_CHECK( instr_ctx, instr_acc_idx, &program );

  /* https://github.com/anza-xyz/agave/blob/v2.1.4/programs/bpf_loader/src/lib.rs#L203 */
  uchar * data = NULL;
  ulong   dlen = 0UL;
  err = fd_borrowed_account_get_data_mut( &program, &data, &dlen );
  if( FD_UNLIKELY( err ) ) {
    return err;
  }

  ulong write_offset = fd_ulong_sat_add( program_data_offset, bytes_len );
  if( FD_UNLIKELY( fd_borrowed_account_get_data_len( &program )<write_offset ) ) {
    /* Max msg_sz: 24 - 6 + 2*20 = 58 < 127 => we can use printf */
    fd_log_collector_printf_dangerous_max_127( instr_ctx,
      "Write overflow %lu < %lu", fd_borrowed_account_get_data_len( &program ), write_offset );
    return FD_EXECUTOR_INSTR_ERR_ACC_DATA_TOO_SMALL;
  }

  if( FD_UNLIKELY( program_data_offset>dlen ) ) {
    return FD_EXECUTOR_INSTR_ERR_ACC_DATA_TOO_SMALL;
  }

  if( FD_LIKELY( bytes_len ) ) {
    fd_memcpy( data+program_data_offset, bytes, bytes_len );
  }

  return FD_EXECUTOR_INSTR_SUCCESS;
}

fd_bpf_upgradeable_loader_state_t *
fd_bpf_loader_program_get_state( fd_txn_account_t const * acct,
                                 fd_spad_t *              spad,
                                 int *                    err ) {
  fd_bpf_upgradeable_loader_state_t * res = fd_bincode_decode_spad(
      bpf_upgradeable_loader_state,
      spad,
      acct->vt->get_data( acct ),
      acct->vt->get_data_len( acct ),
      err );
  if( FD_UNLIKELY( *err ) ) {
    *err = FD_EXECUTOR_INSTR_ERR_INVALID_ACC_DATA;
    return NULL;
  }
  return res;
}

/* Mirrors solana_sdk::transaction_context::BorrowedAccount::set_state()
   https://github.com/anza-xyz/agave/blob/v2.1.14/sdk/src/transaction_context.rs#L973 */
int
fd_bpf_loader_v3_program_set_state( fd_borrowed_account_t * borrowed_acct,
                                    fd_bpf_upgradeable_loader_state_t * state ) {
  ulong state_size = fd_bpf_upgradeable_loader_state_size( state );

  uchar * data = NULL;
  ulong   dlen = 0UL;

  int err = fd_borrowed_account_get_data_mut( borrowed_acct, &data, &dlen );
  if( FD_UNLIKELY( err ) ) {
    return err;
  }

  if( FD_UNLIKELY( state_size>dlen ) ) {
    return FD_EXECUTOR_INSTR_ERR_ACC_DATA_TOO_SMALL;
  }

  fd_bincode_encode_ctx_t ctx = {
    .data    = data,
    .dataend = data + state_size
  };

  err = fd_bpf_upgradeable_loader_state_encode( state, &ctx );
  if( FD_UNLIKELY( err!=FD_BINCODE_SUCCESS ) ) {
    return FD_EXECUTOR_INSTR_ERR_GENERIC_ERR;
  }

  return FD_BINCODE_SUCCESS;
}

/* https://github.com/anza-xyz/agave/blob/574bae8fefc0ed256b55340b9d87b7689bcdf222/programs/bpf_loader/src/lib.rs#L1299-L1331 */
static int
common_close_account( fd_pubkey_t * authority_address,
                      fd_exec_instr_ctx_t * instr_ctx,
                      fd_bpf_upgradeable_loader_state_t * state ) {
  int err;

  /* https://github.com/anza-xyz/agave/blob/v2.1.14/programs/bpf_loader/src/lib.rs#L1307 */
  if( FD_UNLIKELY( !authority_address ) ) {
    return FD_EXECUTOR_INSTR_ERR_ACC_IMMUTABLE;
  }

  /* https://github.com/anza-xyz/agave/blob/v2.1.14/programs/bpf_loader/src/lib.rs#L1312-L1313 */
  fd_pubkey_t const * acc_key = NULL;
  err = fd_exec_instr_ctx_get_key_of_account_at_index( instr_ctx, 2UL, &acc_key );
  if( FD_UNLIKELY( err ) ) {
    return err;
  }

  if( FD_UNLIKELY( memcmp( authority_address, acc_key, sizeof(fd_pubkey_t) ) ) ) {
    return FD_EXECUTOR_INSTR_ERR_INCORRECT_AUTHORITY;
  }

  /* https://github.com/anza-xyz/agave/blob/v2.1.14/programs/bpf_loader/src/lib.rs#L1319-L1322 */
  if( FD_UNLIKELY( !fd_instr_acc_is_signer_idx( instr_ctx->instr, 2UL ) ) ) {
    return FD_EXECUTOR_INSTR_ERR_MISSING_REQUIRED_SIGNATURE;
  }

  /* https://github.com/anza-xyz/agave/blob/v2.1.4/programs/bpf_loader/src/lib.rs#L1324 */
  fd_guarded_borrowed_account_t close_account;
  FD_TRY_BORROW_INSTR_ACCOUNT_DEFAULT_ERR_CHECK( instr_ctx, 0UL, &close_account );

  /* https://github.com/anza-xyz/agave/blob/v2.1.4/programs/bpf_loader/src/lib.rs#L1326 */
  fd_guarded_borrowed_account_t recipient_account;
  FD_TRY_BORROW_INSTR_ACCOUNT_DEFAULT_ERR_CHECK( instr_ctx, 1UL, &recipient_account );

  err = fd_borrowed_account_checked_add_lamports( &recipient_account,
                                                  fd_borrowed_account_get_lamports( &close_account ) );
  if( FD_UNLIKELY( err ) ) {
    return err;
  }

  err = fd_borrowed_account_set_lamports( &close_account, 0UL );
  if( FD_UNLIKELY( err ) ) {
    return err;
  }

  state->discriminant = fd_bpf_upgradeable_loader_state_enum_uninitialized;
  err = fd_bpf_loader_v3_program_set_state( &close_account, state );
  if( FD_UNLIKELY( err!=FD_BINCODE_SUCCESS ) ) {
    return err;
  }

  return FD_EXECUTOR_INSTR_SUCCESS;
}


/* Every loader-owned BPF program goes through this function, which goes into the VM.

   https://github.com/anza-xyz/agave/blob/574bae8fefc0ed256b55340b9d87b7689bcdf222/programs/bpf_loader/src/lib.rs#L1332-L1501 */
int
fd_bpf_execute( fd_exec_instr_ctx_t * instr_ctx, fd_sbpf_validated_program_t const * prog, uchar is_deprecated ) {

  int err                       = FD_EXECUTOR_INSTR_SUCCESS;
  fd_sbpf_syscalls_t * syscalls = fd_sbpf_syscalls_new( fd_spad_alloc( instr_ctx->txn_ctx->spad,
                                                                       fd_sbpf_syscalls_align(),
                                                                       fd_sbpf_syscalls_footprint() ) );
  FD_TEST( syscalls );

  /* TODO do we really need to re-do this on every instruction? */
  fd_vm_syscall_register_slot( syscalls,
                               instr_ctx->txn_ctx->slot,
                               &instr_ctx->txn_ctx->features,
                               0 );

  /* https://github.com/anza-xyz/agave/blob/574bae8fefc0ed256b55340b9d87b7689bcdf222/programs/bpf_loader/src/lib.rs#L1362-L1368 */
  ulong                   input_sz                                = 0UL;
  ulong                   pre_lens[256]                           = {0};
  fd_vm_input_region_t    input_mem_regions[1000]                 = {0}; /* We can have a max of (3 * num accounts + 1) regions */
  fd_vm_acc_region_meta_t acc_region_metas[256]                   = {0}; /* instr acc idx to idx */
  uint                    input_mem_regions_cnt                   = 0U;
  int                     direct_mapping                          = FD_FEATURE_ACTIVE_BANK( instr_ctx->txn_ctx->bank, bpf_account_data_direct_mapping );
  int                     mask_out_rent_epoch_in_vm_serialization = FD_FEATURE_ACTIVE_BANK( instr_ctx->txn_ctx->bank, mask_out_rent_epoch_in_vm_serialization );

  uchar * input = NULL;
  err = fd_bpf_loader_input_serialize_parameters( instr_ctx, &input_sz, pre_lens,
                                                  input_mem_regions, &input_mem_regions_cnt,
                                                  acc_region_metas, direct_mapping, mask_out_rent_epoch_in_vm_serialization,
                                                  is_deprecated, &input );
  if( FD_UNLIKELY( err ) ) {
    return err;
  }

  if( FD_UNLIKELY( input==NULL ) ) {
    return FD_EXECUTOR_INSTR_ERR_MISSING_ACC;
  }

  fd_sha256_t _sha[1];
  fd_sha256_t * sha = fd_sha256_join( fd_sha256_new( _sha ) );

  fd_vm_t _vm[1];
  fd_vm_t * vm = fd_vm_join( fd_vm_new( _vm ) );

  ulong pre_insn_cus = instr_ctx->txn_ctx->compute_budget_details.compute_meter;
  ulong heap_size    = instr_ctx->txn_ctx->compute_budget_details.heap_size;

  /* https://github.com/anza-xyz/agave/blob/v2.3.1/programs/bpf_loader/src/lib.rs#L275-L278 */
  ulong heap_cost = calculate_heap_cost( heap_size, FD_VM_HEAP_COST );
  int heap_cost_result = fd_exec_consume_cus( instr_ctx->txn_ctx, heap_cost );
  if( FD_UNLIKELY( heap_cost_result ) ) {
    return FD_EXECUTOR_INSTR_ERR_PROGRAM_ENVIRONMENT_SETUP_FAILURE;
  }

  /* For dumping syscalls for seed corpora */
  int dump_syscall_to_pb = instr_ctx->txn_ctx->capture_ctx &&
                           instr_ctx->txn_ctx->slot >= instr_ctx->txn_ctx->capture_ctx->dump_proto_start_slot &&
                           instr_ctx->txn_ctx->capture_ctx->dump_syscall_to_pb;

  /* TODO: (topointon): correctly set check_size in vm setup */
  vm = fd_vm_init(
    /* vm                    */ vm,
    /* instr_ctx             */ instr_ctx,
    /* heap_max              */ heap_size,
    /* entry_cu              */ instr_ctx->txn_ctx->compute_budget_details.compute_meter,
    /* rodata                */ prog->rodata,
    /* rodata_sz             */ prog->rodata_sz,
    /* text                  */ (ulong *)((ulong)prog->rodata + (ulong)prog->text_off), /* Note: text_off is byte offset */
    /* text_cnt              */ prog->text_cnt,
    /* text_off              */ prog->text_off,
    /* text_sz               */ prog->text_sz,
    /* entry_pc              */ prog->entry_pc,
    /* calldests             */ prog->calldests,
    /* sbpf_version          */ prog->sbpf_version,
    /* syscalls              */ syscalls,
    /* trace                 */ NULL,
    /* sha                   */ sha,
    /* input_mem_regions     */ input_mem_regions,
    /* input_mem_regions_cnt */ input_mem_regions_cnt,
    /* acc_region_metas      */ acc_region_metas,
    /* is_deprecated         */ is_deprecated,
    /* direct_mapping        */ direct_mapping,
    /* dump_syscall_to_pb    */ dump_syscall_to_pb );
  if( FD_UNLIKELY( !vm ) ) {
    /* We throw an error here because it could be the case that the given heap_size > HEAP_MAX.
       In this case, Agave fails the transaction but does not error out.

       https://github.com/anza-xyz/agave/blob/574bae8fefc0ed256b55340b9d87b7689bcdf222/programs/bpf_loader/src/lib.rs#L1396 */
    FD_LOG_WARNING(( "null vm" ));
    return FD_EXECUTOR_INSTR_ERR_PROGRAM_ENVIRONMENT_SETUP_FAILURE;
  }

#ifdef FD_DEBUG_SBPF_TRACES
  uchar * signature = (uchar*)vm->instr_ctx->txn_ctx->_txn_raw->raw + vm->instr_ctx->txn_ctx->txn_descriptor->signature_off;
  uchar sig[64];
  /* TODO (topointon): make this run-time configurable, no need for this ifdef */
  fd_base58_decode_64( "tkacc4VCh2z9cLsQowCnKqX14DmUUxpRyES755FhUzrFxSFvo8kVk444kNTL7kJxYnnANYwRWAdHCgBJupftZrz", sig );
  if( FD_UNLIKELY( !memcmp( signature, sig, 64UL ) ) ) {
    ulong event_max = FD_RUNTIME_VM_TRACE_EVENT_MAX;
    ulong event_data_max = FD_RUNTIME_VM_TRACE_EVENT_DATA_MAX;
    vm->trace = fd_vm_trace_join( fd_vm_trace_new( fd_spad_alloc(
    instr_ctx->txn_ctx->spad, fd_vm_trace_align(), fd_vm_trace_footprint( event_max, event_data_max ) ), event_max, event_data_max ) );
    if( FD_UNLIKELY( !vm->trace ) ) FD_LOG_ERR(( "unable to create trace; make sure you've compiled with sufficient spad size " ));
  }
#endif

  int exec_err = fd_vm_exec( vm );
  instr_ctx->txn_ctx->compute_budget_details.compute_meter = vm->cu;

  if( FD_UNLIKELY( vm->trace ) ) {
    err = fd_vm_trace_printf( vm->trace, vm->syscalls );
    if( FD_UNLIKELY( err ) ) {
      FD_LOG_WARNING(( "fd_vm_trace_printf failed (%i-%s)", err, fd_vm_strerror( err ) ));
    }
  }

  /* Log consumed compute units and return data.
     https://github.com/anza-xyz/agave/blob/v2.0.6/programs/bpf_loader/src/lib.rs#L1418-L1429 */
  fd_log_collector_program_consumed( instr_ctx, pre_insn_cus-vm->cu, pre_insn_cus );
  if( FD_UNLIKELY( instr_ctx->txn_ctx->return_data.len ) ) {
    fd_log_collector_program_return( instr_ctx );
  }

  /* We have a big error-matching arm here
     https://github.com/anza-xyz/agave/blob/v2.3.1/programs/bpf_loader/src/lib.rs#L1674-L1744 */

  /* Handle non-zero return status with successful VM execution. This is
     the Ok(status) case, hence exec_err must be 0 for this case to be hit.
     https://github.com/anza-xyz/agave/blob/v2.3.1/programs/bpf_loader/src/lib.rs#L1675-L1678 */
  if( FD_LIKELY( !exec_err ) ) {
    ulong status = vm->reg[0];
    if( FD_UNLIKELY( status ) ) {
      err = program_error_to_instr_error( status, &instr_ctx->txn_ctx->custom_err );
      FD_VM_PREPARE_ERR_OVERWRITE( vm );
      FD_VM_ERR_FOR_LOG_INSTR( vm, err );
      return err;
    }
  } else {
    /* https://github.com/anza-xyz/agave/blob/v2.1.13/programs/bpf_loader/src/lib.rs#L1434-L1439 */
    /* (SIMD-182) Consume ALL requested CUs on non-Syscall errors */
    if( FD_FEATURE_ACTIVE_BANK( instr_ctx->txn_ctx->bank, deplete_cu_meter_on_vm_failure ) &&
        exec_err!=FD_VM_ERR_EBPF_SYSCALL_ERROR ) {
      instr_ctx->txn_ctx->compute_budget_details.compute_meter = 0UL;
    }

    /* Direct mapping access violation case
       Edge case with error codes: if direct mapping is enabled, the EBPF error is an access violation,
       and the access type was a store, a different error code is returned to give developers more insight
       as to what caused the error.
       https://github.com/anza-xyz/agave/blob/v2.0.9/programs/bpf_loader/src/lib.rs#L1436-L1470 */
    if( FD_UNLIKELY( direct_mapping && exec_err==FD_VM_ERR_EBPF_ACCESS_VIOLATION &&
                     vm->segv_vaddr!=ULONG_MAX &&
                     vm->segv_access_type==FD_VM_ACCESS_TYPE_ST ) ) {
      /* vaddrs start at 0xFFFFFFFF + 1, so anything below it would not correspond to any account metadata. */
      if( FD_UNLIKELY( vm->segv_vaddr>>32UL==0UL ) ) {
        return FD_EXECUTOR_INSTR_ERR_PROGRAM_FAILED_TO_COMPLETE;
      }

      /* Find the account meta corresponding to the vaddr */
      ulong vaddr_offset = vm->segv_vaddr & FD_VM_OFFSET_MASK;
      ulong acc_region_addl_off = is_deprecated ? 0UL : MAX_PERMITTED_DATA_INCREASE;

      /* If the vaddr doesn't live in the input region, then we don't need to
         bother trying to iterate through all of the borrowed accounts. */
      if( FD_VADDR_TO_REGION( vm->segv_vaddr )!=FD_VM_INPUT_REGION ) {
        return FD_EXECUTOR_INSTR_ERR_PROGRAM_FAILED_TO_COMPLETE;
      }

      /* If the vaddr of the access violation falls within the bounds of a
         serialized account vaddr range, then try to retrieve a more specific
         vm error based on the account's accesss permissions. */
      for( ushort i=0UL; i<instr_ctx->instr->acct_cnt; i++ ) {
        /* https://github.com/anza-xyz/agave/blob/v2.1.4/programs/bpf_loader/src/lib.rs#L1455 */
        fd_guarded_borrowed_account_t instr_acc;
        FD_TRY_BORROW_INSTR_ACCOUNT_DEFAULT_ERR_CHECK( instr_ctx, i, &instr_acc );

        ulong idx = acc_region_metas[i].region_idx;
        if( input_mem_regions[idx].vaddr_offset<=vaddr_offset && vaddr_offset<input_mem_regions[idx].vaddr_offset+pre_lens[i]+acc_region_addl_off ) {

          /* Found an input mem region!
             https://github.com/anza-xyz/agave/blob/89872fdb074e6658646b2b57a299984f0059cc84/programs/bpf_loader/src/lib.rs#L1515-L1528 */
          if( !FD_FEATURE_ACTIVE_BANK( instr_ctx->txn_ctx->bank, remove_accounts_executable_flag_checks ) &&
              fd_borrowed_account_is_executable( &instr_acc ) ) {
            err = FD_EXECUTOR_INSTR_ERR_EXECUTABLE_DATA_MODIFIED;
          } else if( fd_borrowed_account_is_writable( &instr_acc ) ) {
            err = FD_EXECUTOR_INSTR_ERR_EXTERNAL_DATA_MODIFIED;
          } else {
            err = FD_EXECUTOR_INSTR_ERR_READONLY_DATA_MODIFIED;
          }
          return err;
        }
      }
    }

    /* The error kind should have been set in the VM. Match it and set
       the error code accordingly. There are no direct permalinks here -
       this is all a result of Agave's complex nested error-code handling
       and our design decisions for making our error codes match. */

    /* Instr error case. Set the error kind and return the instruction error */
    if( instr_ctx->txn_ctx->exec_err_kind==FD_EXECUTOR_ERR_KIND_INSTR ) {
      err = instr_ctx->txn_ctx->exec_err;
      FD_VM_PREPARE_ERR_OVERWRITE( vm );
      FD_VM_ERR_FOR_LOG_INSTR( vm, err );
      return err;
    }

    /* Syscall error case. The VM would have also set the syscall error
       code in the txn_ctx exec_err. */
    if( instr_ctx->txn_ctx->exec_err_kind==FD_EXECUTOR_ERR_KIND_SYSCALL ) {
      err = instr_ctx->txn_ctx->exec_err;
      FD_VM_PREPARE_ERR_OVERWRITE( vm );
      FD_VM_ERR_FOR_LOG_SYSCALL( vm, err );
      return FD_EXECUTOR_INSTR_ERR_PROGRAM_FAILED_TO_COMPLETE;
    }

    /* An access violation that takes place inside a syscall will
       cause `exec_res` to be set to EbpfError::SyscallError,
       but the `txn_ctx->exec_err_kind` will be set to EBPF and
       `txn_ctx->exec_err` will be set to the EBPF error. In this
       specific case, there is nothing to do since the error and error
       kind area already set correctly. Otherwise, we need to log the
       EBPF error. */
    if( exec_err!=FD_VM_ERR_EBPF_SYSCALL_ERROR ) {
      FD_VM_PREPARE_ERR_OVERWRITE( vm );
      FD_VM_ERR_FOR_LOG_EBPF( vm, exec_err );
    }

    return FD_EXECUTOR_INSTR_ERR_PROGRAM_FAILED_TO_COMPLETE;
  }

  err = fd_bpf_loader_input_deserialize_parameters( instr_ctx, pre_lens, input, input_sz, direct_mapping, is_deprecated );
  if( FD_UNLIKELY( err ) ) {
    return err;
  }

  return FD_EXECUTOR_INSTR_SUCCESS;
}

/* https://github.com/anza-xyz/agave/blob/v2.3.1/programs/bpf_loader/src/lib.rs#L1358-L1539 */
static int
common_extend_program( fd_exec_instr_ctx_t * instr_ctx,
                       uint                  additional_bytes,
                       uchar                 check_authority ) {
  int err;

  /* https://github.com/anza-xyz/agave/blob/v2.3.1/programs/bpf_loader/src/lib.rs#L1366 */
  fd_pubkey_t const * program_id = NULL;
  err = fd_exec_instr_ctx_get_last_program_key( instr_ctx, &program_id );
  if( FD_UNLIKELY( err ) ) {
    return err;
  }

  /* https://github.com/anza-xyz/agave/blob/v2.3.1/programs/bpf_loader/src/lib.rs#L1368-L1370 */
  #define PROGRAM_DATA_ACCOUNT_INDEX (0)
  #define PROGRAM_ACCOUNT_INDEX      (1)
  #define AUTHORITY_ACCOUNT_INDEX    (2)

  /* https://github.com/anza-xyz/agave/blob/v2.3.1/programs/bpf_loader/src/lib.rs#L1371-L1372 */
  uchar optional_payer_account_index = check_authority ? 4 : 3;

  /* https://github.com/anza-xyz/agave/blob/v2.3.1/programs/bpf_loader/src/lib.rs#L1374-L1377 */
  if( FD_UNLIKELY( additional_bytes==0U ) ) {
    fd_log_collector_msg_literal( instr_ctx, "Additional bytes must be greater than 0" );
    return FD_EXECUTOR_INSTR_ERR_INVALID_INSTR_DATA;
  }

  /* https://github.com/anza-xyz/agave/blob/v2.3.1/programs/bpf_loader/src/lib.rs#L1379-L1381 */
  fd_guarded_borrowed_account_t programdata_account;
  FD_TRY_BORROW_INSTR_ACCOUNT_DEFAULT_ERR_CHECK( instr_ctx, PROGRAM_DATA_ACCOUNT_INDEX, &programdata_account );
  fd_pubkey_t * programdata_key = programdata_account.acct->pubkey;

  /* https://github.com/anza-xyz/agave/blob/v2.3.1/programs/bpf_loader/src/lib.rs#L1383-L1386 */
  if( FD_UNLIKELY( memcmp( program_id, fd_borrowed_account_get_owner( &programdata_account ), sizeof(fd_pubkey_t) ) ) ) {
    fd_log_collector_msg_literal( instr_ctx, "ProgramData owner is invalid" );
    return FD_EXECUTOR_INSTR_ERR_INVALID_ACC_OWNER;
  }

  /* https://github.com/anza-xyz/agave/blob/v2.3.1/programs/bpf_loader/src/lib.rs#L1387-L1390 */
  if( FD_UNLIKELY( !fd_borrowed_account_is_writable( &programdata_account ) ) ) {
    fd_log_collector_msg_literal( instr_ctx, "ProgramData is not writable" );
    return FD_EXECUTOR_INSTR_ERR_INVALID_ARG;
  }

  /* https://github.com/anza-xyz/agave/blob/v2.3.1/programs/bpf_loader/src/lib.rs#L1392-L1393 */
  fd_guarded_borrowed_account_t program_account;
  FD_TRY_BORROW_INSTR_ACCOUNT_DEFAULT_ERR_CHECK( instr_ctx, PROGRAM_ACCOUNT_INDEX, &program_account );

  /* https://github.com/anza-xyz/agave/blob/v2.3.1/programs/bpf_loader/src/lib.rs#L1394-L1397 */
  if( FD_UNLIKELY( !fd_borrowed_account_is_writable( &program_account ) ) ) {
    fd_log_collector_msg_literal( instr_ctx, "Program account is not writable" );
    return FD_EXECUTOR_INSTR_ERR_INVALID_ARG;
  }

  /* https://github.com/anza-xyz/agave/blob/v2.3.1/programs/bpf_loader/src/lib.rs#L1398-L1401 */
  if( FD_UNLIKELY( memcmp( program_id, fd_borrowed_account_get_owner( &program_account ), sizeof(fd_pubkey_t) ) ) ) {
    fd_log_collector_msg_literal( instr_ctx, "Program account not owned by loader" );
    return FD_EXECUTOR_INSTR_ERR_INVALID_ACC_OWNER;
  }

  /* https://github.com/anza-xyz/agave/blob/v2.3.1/programs/bpf_loader/src/lib.rs#L1403-L1419 */
  fd_bpf_upgradeable_loader_state_t * program_state = fd_bpf_loader_program_get_state( program_account.acct, instr_ctx->txn_ctx->spad, &err );
  if( FD_UNLIKELY( err!=FD_BINCODE_SUCCESS ) ) {
    return err;
  }
  if( fd_bpf_upgradeable_loader_state_is_program( program_state ) ) {
    if( FD_UNLIKELY( memcmp( &program_state->inner.program.programdata_address, programdata_key, sizeof(fd_pubkey_t) ) ) ) {
      fd_log_collector_msg_literal( instr_ctx, "Program account does not match ProgramData account" );
      return FD_EXECUTOR_INSTR_ERR_INVALID_ARG;
    }
  } else {
    fd_log_collector_msg_literal( instr_ctx, "Invalid Program account" );
    return FD_EXECUTOR_INSTR_ERR_INVALID_ACC_DATA;
  }

  /* https://github.com/anza-xyz/agave/blob/v2.3.1/programs/bpf_loader/src/lib.rs#L1420 */
  fd_borrowed_account_drop( &program_account );

  /* https://github.com/anza-xyz/agave/blob/v2.3.1/programs/bpf_loader/src/lib.rs#L1422-L1432 */
  ulong old_len = fd_borrowed_account_get_data_len( &programdata_account );
  ulong new_len = fd_ulong_sat_add( old_len, additional_bytes );
  if( FD_UNLIKELY( new_len>MAX_PERMITTED_DATA_LENGTH ) ) {
    /* Max msg_sz: 85 - 6 + 2*20 = 119 < 127 => we can use printf */
    fd_log_collector_printf_dangerous_max_127( instr_ctx,
      "Extended ProgramData length of %lu bytes exceeds max account data length of %lu bytes", new_len, MAX_PERMITTED_DATA_LENGTH );
    return FD_EXECUTOR_INSTR_ERR_INVALID_REALLOC;
  }

  /* https://github.com/anza-xyz/agave/blob/v2.3.1/programs/bpf_loader/src/lib.rs#L1434-L1437 */
  fd_sol_sysvar_clock_t const * clock = fd_sysvar_clock_read( instr_ctx->txn_ctx->funk, instr_ctx->txn_ctx->funk_txn, instr_ctx->txn_ctx->spad );
  if( FD_UNLIKELY( !clock ) ) {
    return FD_EXECUTOR_INSTR_ERR_UNSUPPORTED_SYSVAR;
  }
  ulong clock_slot = clock->slot;

  /* https://github.com/anza-xyz/agave/blob/v2.3.1/programs/bpf_loader/src/lib.rs#L1439-L1478 */
  fd_pubkey_t * upgrade_authority_address = NULL;
  fd_bpf_upgradeable_loader_state_t * programdata_state = fd_bpf_loader_program_get_state( programdata_account.acct, instr_ctx->txn_ctx->spad, &err );
  if( FD_UNLIKELY( err!=FD_BINCODE_SUCCESS ) ) {
    return err;
  }
  if( fd_bpf_upgradeable_loader_state_is_program_data( programdata_state ) ) {
    /* https://github.com/anza-xyz/agave/blob/v2.3.1/programs/bpf_loader/src/lib.rs#L1444-L1447 */
    if( FD_UNLIKELY( clock_slot==programdata_state->inner.program_data.slot ) ) {
      fd_log_collector_msg_literal( instr_ctx, "Program was extended in this block already" );
      return FD_EXECUTOR_INSTR_ERR_INVALID_ARG;
    }

    /* https://github.com/anza-xyz/agave/blob/v2.3.1/programs/bpf_loader/src/lib.rs#L1449-L1455 */
    if( FD_UNLIKELY( !programdata_state->inner.program_data.upgrade_authority_address ) ) {
      fd_log_collector_msg_literal( instr_ctx, "Cannot extend ProgramData accounts that are not upgradeable" );
      return FD_EXECUTOR_INSTR_ERR_ACC_IMMUTABLE;
    }

    /* https://github.com/anza-xyz/agave/blob/v2.3.1/programs/bpf_loader/src/lib.rs#L1457-L1472 */
    if( check_authority ) {
      /* https://github.com/anza-xyz/agave/blob/v2.3.1/programs/bpf_loader/src/lib.rs#L1458-L1463 */
      fd_pubkey_t const * authority_key = NULL;
      err = fd_exec_instr_ctx_get_key_of_account_at_index( instr_ctx, AUTHORITY_ACCOUNT_INDEX, &authority_key );
      if( FD_UNLIKELY( err ) ) {
        return err;
      }

      /* https://github.com/anza-xyz/agave/blob/v2.3.1/programs/bpf_loader/src/lib.rs#L1464-L1467 */
      if( FD_UNLIKELY( memcmp( programdata_state->inner.program_data.upgrade_authority_address, authority_key, sizeof(fd_pubkey_t) ) ) ) {
        fd_log_collector_msg_literal( instr_ctx, "Incorrect upgrade authority provided" );
        return FD_EXECUTOR_INSTR_ERR_INCORRECT_AUTHORITY;
      }

      /* https://github.com/anza-xyz/agave/blob/v2.3.1/programs/bpf_loader/src/lib.rs#L1468-L1471 */
      if( FD_UNLIKELY( !fd_instr_acc_is_signer_idx( instr_ctx->instr, AUTHORITY_ACCOUNT_INDEX ) ) ) {
        fd_log_collector_msg_literal( instr_ctx, "Upgrade authority did not sign" );
        return FD_EXECUTOR_INSTR_ERR_MISSING_REQUIRED_SIGNATURE;
      }
    }

    /* https://github.com/anza-xyz/agave/blob/v2.3.1/programs/bpf_loader/src/lib.rs#L1474 */
    upgrade_authority_address = programdata_state->inner.program_data.upgrade_authority_address;
  } else {
    /* https://github.com/anza-xyz/agave/blob/v2.3.1/programs/bpf_loader/src/lib.rs#L1476-L1477 */
    fd_log_collector_msg_literal( instr_ctx, "ProgramData state is invalid" );
    return FD_EXECUTOR_INSTR_ERR_INVALID_ACC_DATA;
  }

  /* https://github.com/anza-xyz/agave/blob/v2.3.1/programs/bpf_loader/src/lib.rs#L1480-L1485 */
  fd_rent_t const * rent             = fd_bank_rent_query( instr_ctx->txn_ctx->bank );
  ulong             balance          = fd_borrowed_account_get_lamports( &programdata_account );
  ulong             min_balance      = fd_ulong_max( fd_rent_exempt_minimum_balance( rent, new_len ), 1UL );
  ulong             required_payment = fd_ulong_sat_sub( min_balance, balance );

  /* Borrowed accounts need to be dropped before native invocations. Note:
     the programdata account is manually released and acquired within the
     extend instruction to preserve the local variable scoping to maintain
     readability. The scoped macro still successfully handles the case of
     freeing a write lock in case of an early termination. */

  /* https://github.com/anza-xyz/agave/blob/v2.3.1/programs/bpf_loader/src/lib.rs#L1488 */
  fd_borrowed_account_drop( &programdata_account );

  /* https://github.com/anza-xyz/agave/blob/v2.3.1/programs/bpf_loader/src/lib.rs#L1492-L1502 */
  if( FD_UNLIKELY( required_payment>0UL ) ) {
    /* https://github.com/anza-xyz/agave/blob/v2.3.1/programs/bpf_loader/src/lib.rs#L1493-L1496 */
    fd_pubkey_t const * payer_key = NULL;
    err = fd_exec_instr_ctx_get_key_of_account_at_index( instr_ctx, optional_payer_account_index, &payer_key );
    if( FD_UNLIKELY( err ) ) {
      return err;
    }

    /* https://github.com/anza-xyz/agave/blob/v2.3.1/programs/bpf_loader/src/lib.rs#L1498-L1501 */
    uchar instr_data[FD_TXN_MTU];
    fd_system_program_instruction_t instr = {
      .discriminant = fd_system_program_instruction_enum_transfer,
      .inner = {
        .transfer = required_payment
      }
    };

    fd_bincode_encode_ctx_t encode_ctx = {
      .data    = instr_data,
      .dataend = instr_data + FD_TXN_MTU
    };

    // This should never fail.
    int err = fd_system_program_instruction_encode( &instr, &encode_ctx );
    if( FD_UNLIKELY( err ) ) {
      return FD_EXECUTOR_INSTR_ERR_FATAL;
    }

    fd_vm_rust_account_meta_t * acct_metas = (fd_vm_rust_account_meta_t *)
                                              fd_spad_alloc( instr_ctx->txn_ctx->spad,
                                                             FD_VM_RUST_ACCOUNT_META_ALIGN,
                                                             2UL * sizeof(fd_vm_rust_account_meta_t) );
    fd_native_cpi_create_account_meta( payer_key,       1UL, 1UL, &acct_metas[ 0UL ] );
    fd_native_cpi_create_account_meta( programdata_key, 0UL, 1UL, &acct_metas[ 1UL ] );

    err = fd_native_cpi_native_invoke( instr_ctx,
                                        &fd_solana_system_program_id,
                                        instr_data,
                                        FD_TXN_MTU,
                                        acct_metas,
                                        2UL,
                                        NULL,
                                        0UL );
    if( FD_UNLIKELY( err ) ) {
      return err;
    }
  }

  /* https://github.com/anza-xyz/agave/blob/v2.3.1/programs/bpf_loader/src/lib.rs#L1506-L1507 */
  FD_TRY_BORROW_INSTR_ACCOUNT_DEFAULT_ERR_CHECK( instr_ctx, PROGRAM_DATA_ACCOUNT_INDEX, &programdata_account );

  /* https://github.com/anza-xyz/agave/blob/v2.3.1/programs/bpf_loader/src/lib.rs#L1508 */
  err = fd_borrowed_account_set_data_length( &programdata_account, new_len );
  if( FD_UNLIKELY( err ) ) {
    return err;
  }

  /* https://github.com/anza-xyz/agave/blob/v2.3.1/programs/bpf_loader/src/lib.rs#L1510 */
  ulong programdata_data_offset = PROGRAMDATA_METADATA_SIZE;

  /* https://github.com/anza-xyz/agave/blob/v2.3.1/programs/bpf_loader/src/lib.rs#L1517-L1520 */
  if( FD_UNLIKELY( programdata_data_offset>fd_borrowed_account_get_data_len( &programdata_account ) ) ) {
    return FD_EXECUTOR_INSTR_ERR_ACC_DATA_TOO_SMALL;
  }
  uchar const * programdata_data = fd_borrowed_account_get_data( &programdata_account ) + programdata_data_offset;
  ulong         programdata_size = new_len - PROGRAMDATA_METADATA_SIZE;

  /* https://github.com/anza-xyz/agave/blob/v2.3.1/programs/bpf_loader/src/lib.rs#L1512-L1522 */
  err = fd_deploy_program( instr_ctx, programdata_data, programdata_size, instr_ctx->txn_ctx->spad );
  if( FD_UNLIKELY( err ) ) {
    return err;
  }

  /* https://github.com/anza-xyz/agave/blob/v2.3.1/programs/bpf_loader/src/lib.rs#L1523 */
  fd_borrowed_account_drop( &programdata_account );

  /* Setting the discriminant and upgrade authority address here can likely
     be a no-op because these values shouldn't change. These can probably be
     removed, but can help to mirror against Agave client's implementation.
     The set_state function also contains an ownership check. */

  /* https://github.com/anza-xyz/agave/blob/v2.3.1/programs/bpf_loader/src/lib.rs#L1525-L1526 */
  FD_TRY_BORROW_INSTR_ACCOUNT_DEFAULT_ERR_CHECK( instr_ctx, 0UL, &programdata_account );

  /* https://github.com/anza-xyz/agave/blob/v2.3.1/programs/bpf_loader/src/lib.rs#L1527-L1530 */
  programdata_state->discriminant                                 = fd_bpf_upgradeable_loader_state_enum_program_data;
  programdata_state->inner.program_data.slot                      = clock_slot;
  programdata_state->inner.program_data.upgrade_authority_address = upgrade_authority_address;

  err = fd_bpf_loader_v3_program_set_state( &programdata_account, programdata_state );
  if( FD_UNLIKELY( err!=FD_BINCODE_SUCCESS ) ) {
    return err;
  }

  /* Max msg_sz: 41 - 2 + 20 = 57 < 127 => we can use printf
     https://github.com/anza-xyz/agave/blob/v2.3.1/programs/bpf_loader/src/lib.rs#L1532-L1536 */
  fd_log_collector_printf_dangerous_max_127( instr_ctx,
    "Extended ProgramData account by %u bytes", additional_bytes );

  /* programdata account is dropped when it goes out of scope */

  return FD_EXECUTOR_INSTR_SUCCESS;

  #undef PROGRAM_DATA_ACCOUNT_INDEX
  #undef PROGRAM_ACCOUNT_INDEX
  #undef AUTHORITY_ACCOUNT_INDEX
}

/* https://github.com/anza-xyz/agave/blob/77daab497df191ef485a7ad36ed291c1874596e5/programs/bpf_loader/src/lib.rs#L566-L1444 */
static int
process_loader_upgradeable_instruction( fd_exec_instr_ctx_t * instr_ctx ) {
  uchar const * data = instr_ctx->instr->data;
  fd_spad_t *   spad = instr_ctx->txn_ctx->spad;

  int err;
  fd_bpf_upgradeable_loader_program_instruction_t * instruction =
    fd_bincode_decode_spad(
      bpf_upgradeable_loader_program_instruction, spad,
      data,
      instr_ctx->instr->data_sz>FD_TXN_MTU ? FD_TXN_MTU: instr_ctx->instr->data_sz,
      &err );
  if( FD_UNLIKELY( err!=FD_BINCODE_SUCCESS ) ) {
    return FD_EXECUTOR_INSTR_ERR_INVALID_INSTR_DATA;
  }

  /* https://github.com/anza-xyz/agave/blob/v2.2.0/programs/bpf_loader/src/lib.rs#L510 */
  fd_pubkey_t const * program_id = NULL;
  err = fd_exec_instr_ctx_get_last_program_key( instr_ctx, &program_id );
  if( FD_UNLIKELY( err ) ) {
    return err;
  }

  switch( instruction->discriminant ) {
    /* https://github.com/anza-xyz/agave/blob/574bae8fefc0ed256b55340b9d87b7689bcdf222/programs/bpf_loader/src/lib.rs#L476-L493 */
    case fd_bpf_upgradeable_loader_program_instruction_enum_initialize_buffer: {
      if( FD_UNLIKELY( fd_exec_instr_ctx_check_num_insn_accounts( instr_ctx, 2U ) ) ) {
        return FD_EXECUTOR_INSTR_ERR_NOT_ENOUGH_ACC_KEYS;
      }

      /* https://github.com/anza-xyz/agave/blob/v2.1.4/programs/bpf_loader/src/lib.rs#L479 */
      fd_guarded_borrowed_account_t buffer;
      FD_TRY_BORROW_INSTR_ACCOUNT_DEFAULT_ERR_CHECK( instr_ctx, 0UL, &buffer );
      fd_bpf_upgradeable_loader_state_t * buffer_state = fd_bpf_loader_program_get_state( buffer.acct,
                                                                                          spad,
                                                                                          &err );
      if( FD_UNLIKELY( err!=FD_BINCODE_SUCCESS ) ) {
        return err;
      }

      if( FD_UNLIKELY( !fd_bpf_upgradeable_loader_state_is_uninitialized( buffer_state ) ) ) {
        fd_log_collector_msg_literal( instr_ctx, "Buffer account is already initialized" );
        return FD_EXECUTOR_INSTR_ERR_ACC_ALREADY_INITIALIZED;
      }

      /* https://github.com/anza-xyz/agave/blob/v2.1.4/programs/bpf_loader/src/lib.rs#L487-L489 */
      fd_pubkey_t const * authority_key = NULL;
      err = fd_exec_instr_ctx_get_key_of_account_at_index( instr_ctx, 1UL, &authority_key );
      if( FD_UNLIKELY( err ) ) {
        return err;
      }

      buffer_state->discriminant                   = fd_bpf_upgradeable_loader_state_enum_buffer;
      buffer_state->inner.buffer.authority_address = (fd_pubkey_t*)authority_key;

      err = fd_bpf_loader_v3_program_set_state( &buffer, buffer_state );
      if( FD_UNLIKELY( err!=FD_BINCODE_SUCCESS ) ) {
        return err;
      }

      /* implicit drop of buffer account */

      break;
    }
    /* https://github.com/anza-xyz/agave/blob/574bae8fefc0ed256b55340b9d87b7689bcdf222/programs/bpf_loader/src/lib.rs#L494-L525 */
    case fd_bpf_upgradeable_loader_program_instruction_enum_write: {
      if( FD_UNLIKELY( fd_exec_instr_ctx_check_num_insn_accounts( instr_ctx, 2U ) ) ) {
        return FD_EXECUTOR_INSTR_ERR_NOT_ENOUGH_ACC_KEYS;
      }

      /* https://github.com/anza-xyz/agave/blob/v2.1.4/programs/bpf_loader/src/lib.rs#L497 */
      fd_guarded_borrowed_account_t buffer;
      FD_TRY_BORROW_INSTR_ACCOUNT_DEFAULT_ERR_CHECK( instr_ctx, 0UL, &buffer );

       fd_bpf_upgradeable_loader_state_t * loader_state = fd_bpf_loader_program_get_state( buffer.acct,
                                                                                           spad,
                                                                                           &err );
      if( FD_UNLIKELY( err!=FD_BINCODE_SUCCESS ) ) {
        return err;
      }

      if( fd_bpf_upgradeable_loader_state_is_buffer( loader_state ) ) {
        if( FD_UNLIKELY( !loader_state->inner.buffer.authority_address ) ) {
          fd_log_collector_msg_literal( instr_ctx, "Buffer is immutable" );
          return FD_EXECUTOR_INSTR_ERR_ACC_IMMUTABLE;
        }

        /* https://github.com/anza-xyz/agave/blob/v2.1.4/programs/bpf_loader/src/lib.rs#L505-L507 */
        fd_pubkey_t const * authority_key = NULL;
        err = fd_exec_instr_ctx_get_key_of_account_at_index( instr_ctx, 1UL, &authority_key );
        if( FD_UNLIKELY( err ) ) {
          return err;
        }

        if( FD_UNLIKELY( memcmp( loader_state->inner.buffer.authority_address, authority_key, sizeof(fd_pubkey_t) ) ) ) {
          fd_log_collector_msg_literal( instr_ctx, "Incorrect buffer authority provided" );
          return FD_EXECUTOR_INSTR_ERR_INCORRECT_AUTHORITY;
        }
        if( FD_UNLIKELY( !fd_instr_acc_is_signer_idx( instr_ctx->instr, 1UL ) ) ) {
          fd_log_collector_msg_literal( instr_ctx, "Buffer authority did not sign" );
          return FD_EXECUTOR_INSTR_ERR_MISSING_REQUIRED_SIGNATURE;
        }
      } else {
        fd_log_collector_msg_literal( instr_ctx, "Invalid Buffer account" );
        return FD_EXECUTOR_INSTR_ERR_INVALID_ACC_DATA;
      }

      /* https://github.com/anza-xyz/agave/blob/v2.1.4/programs/bpf_loader/src/lib.rs#L520 */
      fd_borrowed_account_drop( &buffer );

      ulong program_data_offset = fd_ulong_sat_add( BUFFER_METADATA_SIZE, instruction->inner.write.offset );
      err = write_program_data( instr_ctx,
                                0UL,
                                program_data_offset,
                                instruction->inner.write.bytes,
                                instruction->inner.write.bytes_len );
      if( FD_UNLIKELY( err ) ) {
        return err;
      }

      break;
    }
    /* https://github.com/anza-xyz/agave/blob/574bae8fefc0ed256b55340b9d87b7689bcdf222/programs/bpf_loader/src/lib.rs#L526-L702 */
    case fd_bpf_upgradeable_loader_program_instruction_enum_deploy_with_max_data_len: {
      /* https://github.com/anza-xyz/agave/blob/574bae8fefc0ed256b55340b9d87b7689bcdf222/programs/bpf_loader/src/lib.rs#L527-L541 */
      if( FD_UNLIKELY( fd_exec_instr_ctx_check_num_insn_accounts( instr_ctx, 4U ) ) ) {
        return FD_EXECUTOR_INSTR_ERR_NOT_ENOUGH_ACC_KEYS;
      }

      /* https://github.com/anza-xyz/agave/blob/v2.1.4/programs/bpf_loader/src/lib.rs#L529-L534 */
      fd_pubkey_t const * payer_key       = NULL;
      fd_pubkey_t const * programdata_key = NULL;

      err = fd_exec_instr_ctx_get_key_of_account_at_index( instr_ctx, 0UL, &payer_key );
      if( FD_UNLIKELY( err ) ) {
        return err;
      }

      err = fd_exec_instr_ctx_get_key_of_account_at_index( instr_ctx, 1UL, &programdata_key );
      if( FD_UNLIKELY( err ) ) {
        return err;
      }

      /* rent is accessed directly from the epoch bank and the clock from the
        slot context. However, a check must be done to make sure that the
        sysvars are correctly included in the set of transaction accounts. */
      err = fd_sysvar_instr_acct_check( instr_ctx, 4UL, &fd_sysvar_rent_id );
      if( FD_UNLIKELY( err ) ) {
        return err;
      }
      err = fd_sysvar_instr_acct_check( instr_ctx, 5UL, &fd_sysvar_clock_id );
      if( FD_UNLIKELY( err ) ) {
        return err;
      }

      fd_sol_sysvar_clock_t const * clock = fd_sysvar_clock_read( instr_ctx->txn_ctx->funk, instr_ctx->txn_ctx->funk_txn, instr_ctx->txn_ctx->spad );
      if( FD_UNLIKELY( !clock ) ) {
        return FD_EXECUTOR_INSTR_ERR_GENERIC_ERR;
      }

      /* https://github.com/anza-xyz/agave/blob/v2.1.14/programs/bpf_loader/src/lib.rs#L538 */
      if( fd_exec_instr_ctx_check_num_insn_accounts( instr_ctx, 8U ) ) {
        return FD_EXECUTOR_INSTR_ERR_NOT_ENOUGH_ACC_KEYS;
      }

      /* https://github.com/anza-xyz/agave/blob/v2.1.14/programs/bpf_loader/src/lib.rs#L539-L541 */
      fd_pubkey_t const * authority_key = NULL;
      err = fd_exec_instr_ctx_get_key_of_account_at_index( instr_ctx, 7UL, &authority_key );
      if( FD_UNLIKELY( err ) ) {
        return err;
      }

      /* https://github.com/anza-xyz/agave/blob/574bae8fefc0ed256b55340b9d87b7689bcdf222/programs/bpf_loader/src/lib.rs#L542-L560 */
      /* Verify Program account */

      fd_bpf_upgradeable_loader_state_t * loader_state   = NULL;
      fd_pubkey_t *                       new_program_id = NULL;
      fd_rent_t const *                   rent           = fd_bank_rent_query( instr_ctx->txn_ctx->bank );

      /* https://github.com/anza-xyz/agave/blob/v2.1.4/programs/bpf_loader/src/lib.rs#L545 */
      fd_guarded_borrowed_account_t program;
      FD_TRY_BORROW_INSTR_ACCOUNT_DEFAULT_ERR_CHECK( instr_ctx, 2UL, &program );

      loader_state = fd_bpf_loader_program_get_state( program.acct,
                                                      spad,
                                                      &err );

      if( FD_UNLIKELY( err!=FD_BINCODE_SUCCESS ) ) {
        return err;
      }
      if( FD_UNLIKELY( !fd_bpf_upgradeable_loader_state_is_uninitialized( loader_state ) ) ) {
        fd_log_collector_msg_literal( instr_ctx, "Program account already initialized" );
        return FD_EXECUTOR_INSTR_ERR_ACC_ALREADY_INITIALIZED;
      }
      if( FD_UNLIKELY( fd_borrowed_account_get_data_len( &program )<SIZE_OF_PROGRAM ) ) {
        fd_log_collector_msg_literal( instr_ctx, "Program account too small" );
        return FD_EXECUTOR_INSTR_ERR_ACC_DATA_TOO_SMALL;
      }
      if( FD_UNLIKELY( fd_borrowed_account_get_lamports( &program )<fd_rent_exempt_minimum_balance( rent,
                                                                                                    fd_borrowed_account_get_data_len( &program ) ) ) ) {
        fd_log_collector_msg_literal( instr_ctx, "Program account not rent-exempt" );
        return FD_EXECUTOR_INSTR_ERR_EXECUTABLE_ACCOUNT_NOT_RENT_EXEMPT;
      }
      new_program_id = program.acct->pubkey;

      /* https://github.com/anza-xyz/agave/blob/v2.1.4/programs/bpf_loader/src/lib.rs#L560 */
      fd_borrowed_account_drop( &program );

      /* https://github.com/anza-xyz/agave/blob/574bae8fefc0ed256b55340b9d87b7689bcdf222/programs/bpf_loader/src/lib.rs#L561-L600 */
      /* Verify Buffer account */

      fd_pubkey_t * buffer_key       = NULL;
      ulong buffer_data_offset       = 0UL;
      ulong buffer_data_len          = 0UL;
      ulong programdata_len          = 0UL;

      /* https://github.com/anza-xyz/agave/blob/v2.1.4/programs/bpf_loader/src/lib.rs#L564-L565 */
      fd_guarded_borrowed_account_t buffer;
      FD_TRY_BORROW_INSTR_ACCOUNT_DEFAULT_ERR_CHECK( instr_ctx, 3UL, &buffer );

      fd_bpf_upgradeable_loader_state_t * buffer_state = fd_bpf_loader_program_get_state( buffer.acct, spad, &err );
      if( FD_UNLIKELY( err!=FD_BINCODE_SUCCESS ) ) {
        return err;
      }

      if( fd_bpf_upgradeable_loader_state_is_buffer( buffer_state ) ) {
        if( FD_UNLIKELY( (authority_key==NULL) != (buffer_state->inner.buffer.authority_address == NULL) ||
            (authority_key!=NULL && memcmp( buffer_state->inner.buffer.authority_address, authority_key, sizeof(fd_pubkey_t) ) ) ) ) {
          fd_log_collector_msg_literal( instr_ctx, "Buffer and upgrade authority don't match" );
          return FD_EXECUTOR_INSTR_ERR_INCORRECT_AUTHORITY;
        }
        if( FD_UNLIKELY( !fd_instr_acc_is_signer_idx( instr_ctx->instr, 7UL ) ) ) {
          fd_log_collector_msg_literal( instr_ctx, "Upgrade authority did not sign" );
          return FD_EXECUTOR_INSTR_ERR_MISSING_REQUIRED_SIGNATURE;
        }
      } else {
        fd_log_collector_msg_literal( instr_ctx, "Invalid Buffer account" );
        return FD_EXECUTOR_INSTR_ERR_INVALID_ARG;
      }
      buffer_key         = buffer.acct->pubkey;
      buffer_data_offset = BUFFER_METADATA_SIZE;
      buffer_data_len    = fd_ulong_sat_sub( fd_borrowed_account_get_data_len( &buffer ), buffer_data_offset );
      /* UpgradeableLoaderState::size_of_program_data( max_data_len ) */
      programdata_len    = fd_ulong_sat_add( PROGRAMDATA_METADATA_SIZE,
                                             instruction->inner.deploy_with_max_data_len.max_data_len );

      if( FD_UNLIKELY( fd_borrowed_account_get_data_len( &buffer )<BUFFER_METADATA_SIZE || buffer_data_len==0UL ) ) {
        fd_log_collector_msg_literal( instr_ctx, "Buffer account too small" );
        return FD_EXECUTOR_INSTR_ERR_INVALID_ACC_DATA;
      }

      if( FD_UNLIKELY( instruction->inner.deploy_with_max_data_len.max_data_len<buffer_data_len ) ) {
        fd_log_collector_msg_literal( instr_ctx, "Max data length is too small to hold Buffer data" );
        return FD_EXECUTOR_INSTR_ERR_ACC_DATA_TOO_SMALL;
      }

      if( FD_UNLIKELY( programdata_len>MAX_PERMITTED_DATA_LENGTH ) ) {
        fd_log_collector_msg_literal( instr_ctx, "Max data length is too large" );
        return FD_EXECUTOR_INSTR_ERR_INVALID_ARG;
      }

      /* https://github.com/anza-xyz/agave/blob/v2.1.4/programs/bpf_loader/src/lib.rs#L590 */
      fd_borrowed_account_drop( &buffer );

      /* https://github.com/anza-xyz/agave/blob/574bae8fefc0ed256b55340b9d87b7689bcdf222/programs/bpf_loader/src/lib.rs#L602-L608 */
      /* Create ProgramData account */

      fd_pubkey_t derived_address[ 1UL ];
      uchar * seeds[ 1UL ];
      seeds[ 0UL ]    = (uchar *)new_program_id;
      ulong seed_sz   = sizeof(fd_pubkey_t);
      uchar bump_seed = 0;
      err = fd_pubkey_find_program_address( program_id, 1UL, seeds, &seed_sz, derived_address,
                                            &bump_seed, &instr_ctx->txn_ctx->custom_err );
      if( FD_UNLIKELY( err ) ) {
        /* TODO: We should handle these errors more gracefully instead of just killing the client (e.g. excluding the transaction
           from the block). */
        FD_LOG_ERR(( "Unable to find a viable program address bump seed" )); // Solana panics, error code is undefined
        return err;
      }
      if( FD_UNLIKELY( memcmp( derived_address, programdata_key, sizeof(fd_pubkey_t) ) ) ) {
        fd_log_collector_msg_literal( instr_ctx, "ProgramData address is not derived" );
        return FD_EXECUTOR_INSTR_ERR_INVALID_ARG;
      }

      /* Drain the Buffer account to payer before paying for programdata account in a local scope
         https://github.com/anza-xyz/agave/blob/v2.1.4/programs/bpf_loader/src/lib.rs#L612-L628 */

      do {
        /* https://github.com/anza-xyz/agave/blob/v2.1.4/programs/bpf_loader/src/lib.rs#L615 */
        fd_guarded_borrowed_account_t payer;
        FD_TRY_BORROW_INSTR_ACCOUNT_DEFAULT_ERR_CHECK( instr_ctx, 0UL, &payer );

        /* https://github.com/anza-xyz/agave/blob/v2.1.4/programs/bpf_loader/src/lib.rs#L613 */
        fd_guarded_borrowed_account_t buffer;
        FD_TRY_BORROW_INSTR_ACCOUNT_DEFAULT_ERR_CHECK( instr_ctx, 3UL, &buffer );

        err = fd_borrowed_account_checked_add_lamports( &payer, fd_borrowed_account_get_lamports( &buffer ) );
        if( FD_UNLIKELY( err ) ) {
          return err;
        }
        err = fd_borrowed_account_set_lamports( &buffer, 0UL );
        if( FD_UNLIKELY( err ) ) {
          return err;
        }
      } while (0);

      /* https://github.com/anza-xyz/agave/blob/574bae8fefc0ed256b55340b9d87b7689bcdf222/programs/bpf_loader/src/lib.rs#L628-L642 */
      /* Pass an extra account to avoid the overly strict unbalanced instruction error */
      /* Invoke the system program to create the new account */
      uchar instr_data[FD_TXN_MTU];
      fd_system_program_instruction_create_account_t create_acct = {
        .lamports = fd_rent_exempt_minimum_balance( rent, programdata_len ),
        .space    = programdata_len,
        .owner    = *program_id,
      };
      if( !create_acct.lamports ) {
        create_acct.lamports = 1UL;
      }

      fd_system_program_instruction_t instr = {
        .discriminant = fd_system_program_instruction_enum_create_account,
        .inner = {
          .create_account = create_acct,
        }
      };

      fd_bincode_encode_ctx_t encode_ctx = {
        .data    = instr_data,
        .dataend = instr_data + FD_TXN_MTU
      };

      // This should never fail.
      int err = fd_system_program_instruction_encode( &instr, &encode_ctx );
      if( FD_UNLIKELY( err ) ) {
        return FD_EXECUTOR_INSTR_ERR_FATAL;
      }

      fd_vm_rust_account_meta_t * acct_metas = (fd_vm_rust_account_meta_t*)
                                                fd_spad_alloc( instr_ctx->txn_ctx->spad,
                                                               FD_VM_RUST_ACCOUNT_META_ALIGN,
                                                               3UL * sizeof(fd_vm_rust_account_meta_t) );
      fd_native_cpi_create_account_meta( payer_key,       1U, 1U, &acct_metas[ 0UL ] );
      fd_native_cpi_create_account_meta( programdata_key, 1U, 1U, &acct_metas[ 1UL ] );
      fd_native_cpi_create_account_meta( buffer_key,      0U, 1U, &acct_metas[ 2UL ] );

      /* caller_program_id == program_id */
      fd_pubkey_t signers[ 1UL ];
      err = fd_pubkey_derive_pda( program_id, 1UL, seeds, &seed_sz, &bump_seed, signers, &instr_ctx->txn_ctx->custom_err );
      if( FD_UNLIKELY( err ) ) {
        return err;
      }
      err = fd_native_cpi_native_invoke( instr_ctx,
                                         &fd_solana_system_program_id,
                                         instr_data,
                                         FD_TXN_MTU,
                                         acct_metas,
                                         3UL,
                                         signers,
                                         1UL );
      if( FD_UNLIKELY( err ) ) {
        return err;
      }

      /* https://github.com/anza-xyz/agave/blob/574bae8fefc0ed256b55340b9d87b7689bcdf222/programs/bpf_loader/src/lib.rs#L644-L665 */
      /* Load and verify the program bits */

      /* https://github.com/anza-xyz/agave/blob/v2.1.4/programs/bpf_loader/src/lib.rs#L648-L649 */
      FD_TRY_BORROW_INSTR_ACCOUNT_DEFAULT_ERR_CHECK( instr_ctx, 3UL, &buffer );

      if( FD_UNLIKELY( buffer_data_offset>fd_borrowed_account_get_data_len( &buffer ) ) ) {
        return FD_EXECUTOR_INSTR_ERR_ACC_DATA_TOO_SMALL;
      }

      const uchar * buffer_data = fd_borrowed_account_get_data( &buffer ) + buffer_data_offset;

      err = fd_deploy_program( instr_ctx, buffer_data, buffer_data_len, instr_ctx->txn_ctx->spad );
      if( FD_UNLIKELY( err ) ) {
        return err;
      }

      /* https://github.com/anza-xyz/agave/blob/v2.1.4/programs/bpf_loader/src/lib.rs#L657 */
      fd_borrowed_account_drop( &buffer );

      /* Update the ProgramData account and record the program bits in a local scope
         https://github.com/anza-xyz/agave/blob/v2.1.4/programs/bpf_loader/src/lib.rs#L669-L691 */
      do {
        /* https://github.com/anza-xyz/agave/blob/v2.1.4/programs/bpf_loader/src/lib.rs#L670-L671 */
        fd_guarded_borrowed_account_t programdata;
        FD_TRY_BORROW_INSTR_ACCOUNT_DEFAULT_ERR_CHECK( instr_ctx, 1UL, &programdata );

        fd_bpf_upgradeable_loader_state_t programdata_loader_state = {
          .discriminant = fd_bpf_upgradeable_loader_state_enum_program_data,
          .inner.program_data = {
            .slot                      = clock->slot,
            .upgrade_authority_address = (fd_pubkey_t *)authority_key,
          },
        };
        err = fd_bpf_loader_v3_program_set_state( &programdata, &programdata_loader_state );
        if( FD_UNLIKELY( err!=FD_BINCODE_SUCCESS ) ) {
          return err;
        }

        /* https://github.com/anza-xyz/agave/blob/574bae8fefc0ed256b55340b9d87b7689bcdf222/programs/bpf_loader/src/lib.rs#L675-L689 */
        if( FD_UNLIKELY( PROGRAMDATA_METADATA_SIZE+buffer_data_len>fd_borrowed_account_get_data_len( &programdata ) ) ) {
          return FD_EXECUTOR_INSTR_ERR_ACC_DATA_TOO_SMALL;
        }
        if( FD_UNLIKELY( buffer_data_offset>fd_borrowed_account_get_data_len( &buffer ) ) ) {
          return FD_EXECUTOR_INSTR_ERR_ACC_DATA_TOO_SMALL;
        }

        uchar * programdata_data = NULL;
        ulong   programdata_dlen = 0UL;
        err = fd_borrowed_account_get_data_mut( &programdata, &programdata_data, &programdata_dlen );
        if( FD_UNLIKELY( err ) ) {
          return err;
        }

        uchar *   dst_slice = programdata_data + PROGRAMDATA_METADATA_SIZE;
        ulong dst_slice_len = buffer_data_len;

        /* https://github.com/anza-xyz/agave/blob/v2.1.4/programs/bpf_loader/src/lib.rs#L683-L684 */
        fd_guarded_borrowed_account_t buffer;
        FD_TRY_BORROW_INSTR_ACCOUNT_DEFAULT_ERR_CHECK( instr_ctx, 3UL, &buffer );

        if( FD_UNLIKELY( buffer_data_offset>fd_borrowed_account_get_data_len( &buffer ) ) ) {
          return FD_EXECUTOR_INSTR_ERR_ACC_DATA_TOO_SMALL;
        }
        const uchar * src_slice = fd_borrowed_account_get_data( &buffer ) + buffer_data_offset;
        fd_memcpy( dst_slice, src_slice, dst_slice_len );
        /* Update buffer data length.
          BUFFER_METADATA_SIZE == UpgradeableLoaderState::size_of_buffer(0) */
        err = fd_borrowed_account_set_data_length( &buffer, BUFFER_METADATA_SIZE );
        if( FD_UNLIKELY( err ) ) {
          return err;
        }
      } while(0);

      /* Max msg_sz: 19 - 2 + 45 = 62 < 127 => we can use printf */
      fd_log_collector_printf_dangerous_max_127( instr_ctx, "Deployed program %s", FD_BASE58_ENC_32_ALLOCA( program_id ) );

      /* https://github.com/anza-xyz/agave/blob/574bae8fefc0ed256b55340b9d87b7689bcdf222/programs/bpf_loader/src/lib.rs#L692-L699 */

      /* Update the Program account
         https://github.com/anza-xyz/agave/blob/v2.1.4/programs/bpf_loader/src/lib.rs#L694-L695 */
      FD_TRY_BORROW_INSTR_ACCOUNT_DEFAULT_ERR_CHECK( instr_ctx, 2UL, &program );

      loader_state->discriminant = fd_bpf_upgradeable_loader_state_enum_program;
      loader_state->inner.program.programdata_address =  *programdata_key;
      err = fd_bpf_loader_v3_program_set_state( &program, loader_state );
      if( FD_UNLIKELY( err!=FD_BINCODE_SUCCESS ) ) {
        return err;
      }
      err = fd_borrowed_account_set_executable( &program, 1 );
      if( FD_UNLIKELY( err ) ) {
        return err;
      }

      FD_LOG_INFO(( "Program deployed %s", FD_BASE58_ENC_32_ALLOCA( program.acct->pubkey ) ));

      /* https://github.com/anza-xyz/agave/blob/v2.1.4/programs/bpf_loader/src/lib.rs#L700 */
      fd_borrowed_account_drop( &program );

      break;
    }
    /* https://github.com/anza-xyz/agave/blob/574bae8fefc0ed256b55340b9d87b7689bcdf222/programs/bpf_loader/src/lib.rs#L703-L891 */
    case fd_bpf_upgradeable_loader_program_instruction_enum_upgrade: {
      /* https://github.com/anza-xyz/agave/blob/574bae8fefc0ed256b55340b9d87b7689bcdf222/programs/bpf_loader/src/lib.rs#L704-L714 */
      if( FD_UNLIKELY( fd_exec_instr_ctx_check_num_insn_accounts( instr_ctx, 3U ) ) ) {
        return FD_EXECUTOR_INSTR_ERR_NOT_ENOUGH_ACC_KEYS;
      }

      /* https://github.com/anza-xyz/agave/blob/v2.1.14/programs/bpf_loader/src/lib.rs#L706-L708 */
      fd_pubkey_t const * programdata_key = NULL;
      err = fd_exec_instr_ctx_get_key_of_account_at_index( instr_ctx, 0UL, &programdata_key );
      if( FD_UNLIKELY( err ) ) {
        return err;
      }

      /* rent is accessed directly from the epoch bank and the clock from the
        slot context. However, a check must be done to make sure that the
        sysvars are correctly included in the set of transaction accounts. */
      err = fd_sysvar_instr_acct_check( instr_ctx, 4UL, &fd_sysvar_rent_id );
      if( FD_UNLIKELY( err ) ) {
        return err;
      }
      err = fd_sysvar_instr_acct_check( instr_ctx, 5UL, &fd_sysvar_clock_id );
      if( FD_UNLIKELY( err ) ) {
        return err;
      }

      if( FD_UNLIKELY( fd_exec_instr_ctx_check_num_insn_accounts( instr_ctx, 7U ) ) ) {
        return FD_EXECUTOR_INSTR_ERR_NOT_ENOUGH_ACC_KEYS;
      }

      /* https://github.com/anza-xyz/agave/blob/v2.1.14/programs/bpf_loader/src/lib.rs#L713-L715 */
      fd_pubkey_t const * authority_key = NULL;
      err = fd_exec_instr_ctx_get_key_of_account_at_index( instr_ctx, 6UL, &authority_key );
      if( FD_UNLIKELY( err ) ) {
        return err;
      }

      /* https://github.com/anza-xyz/agave/blob/574bae8fefc0ed256b55340b9d87b7689bcdf222/programs/bpf_loader/src/lib.rs#L716-L745 */
      /* Verify Program account */

      /* https://github.com/anza-xyz/agave/blob/v2.1.4/programs/bpf_loader/src/lib.rs#L719-L720 */
      fd_guarded_borrowed_account_t program;
      FD_TRY_BORROW_INSTR_ACCOUNT_DEFAULT_ERR_CHECK( instr_ctx, 1UL, &program );

      /* https://github.com/anza-xyz/agave/blob/89872fdb074e6658646b2b57a299984f0059cc84/programs/bpf_loader/src/lib.rs#L758-L765 */
      if( FD_UNLIKELY( !FD_FEATURE_ACTIVE_BANK( instr_ctx->txn_ctx->bank, remove_accounts_executable_flag_checks ) &&
                       !fd_borrowed_account_is_executable( &program ) ) ) {
        fd_log_collector_msg_literal( instr_ctx, "Program account not executable" );
        return FD_EXECUTOR_INSTR_ERR_ACC_NOT_EXECUTABLE;
      }
      if( FD_UNLIKELY( !fd_borrowed_account_is_writable( &program ) ) ) {
        fd_log_collector_msg_literal( instr_ctx, "Program account not writeable" );
        return FD_EXECUTOR_INSTR_ERR_INVALID_ARG;
      }
      if( FD_UNLIKELY( memcmp( fd_borrowed_account_get_owner( &program ), program_id, sizeof(fd_pubkey_t) ) ) ) {
        fd_log_collector_msg_literal( instr_ctx, "Program account not owned by loader" );
        return FD_EXECUTOR_INSTR_ERR_INCORRECT_PROGRAM_ID;
      }
      fd_bpf_upgradeable_loader_state_t * program_state = fd_bpf_loader_program_get_state( program.acct, spad, &err );
      if( FD_UNLIKELY( err!=FD_BINCODE_SUCCESS ) ) {
        return err;
      }
      if( FD_UNLIKELY( fd_bpf_upgradeable_loader_state_is_program( program_state ) ) ) {
        if( FD_UNLIKELY( memcmp( &program_state->inner.program.programdata_address, programdata_key, sizeof(fd_pubkey_t) ) ) ) {
          fd_log_collector_msg_literal( instr_ctx, "Program and ProgramData account mismatch" );
          return FD_EXECUTOR_INSTR_ERR_INVALID_ARG;
        }
      } else {
        fd_log_collector_msg_literal( instr_ctx, "Invalid Program account" );
        return FD_EXECUTOR_INSTR_ERR_INVALID_ACC_DATA;
      }

      /* https://github.com/anza-xyz/agave/blob/v2.1.4/programs/bpf_loader/src/lib.rs#L746 */
      fd_borrowed_account_drop( &program );

      /* https://github.com/anza-xyz/agave/blob/574bae8fefc0ed256b55340b9d87b7689bcdf222/programs/bpf_loader/src/lib.rs#L747-L773 */
      /* Verify Buffer account */

      ulong buffer_lamports    = 0UL;
      ulong buffer_data_offset = 0UL;
      ulong buffer_data_len    = 0UL;

      /* https://github.com/anza-xyz/agave/blob/v2.1.4/programs/bpf_loader/src/lib.rs#L750-L751 */
      fd_guarded_borrowed_account_t buffer;
      FD_TRY_BORROW_INSTR_ACCOUNT_DEFAULT_ERR_CHECK( instr_ctx, 2UL, &buffer );

      fd_bpf_upgradeable_loader_state_t * buffer_state = fd_bpf_loader_program_get_state( buffer.acct, spad, &err );
      if( FD_UNLIKELY( err!=FD_BINCODE_SUCCESS ) ) {
        return err;
      }
      if( fd_bpf_upgradeable_loader_state_is_buffer( buffer_state ) ) {
        if( FD_UNLIKELY( (authority_key==NULL) != (buffer_state->inner.buffer.authority_address == NULL) ||
            (authority_key!=NULL && memcmp( buffer_state->inner.buffer.authority_address, authority_key, sizeof(fd_pubkey_t) ) ) ) ) {
          fd_log_collector_msg_literal( instr_ctx, "Buffer and upgrade authority don't match" );
          return FD_EXECUTOR_INSTR_ERR_INCORRECT_AUTHORITY;
        }
        if( FD_UNLIKELY( !fd_instr_acc_is_signer_idx( instr_ctx->instr, 6UL ) ) ) {
          fd_log_collector_msg_literal( instr_ctx, "Upgrade authority did not sign" );
          return FD_EXECUTOR_INSTR_ERR_MISSING_REQUIRED_SIGNATURE;
        }
      } else {
        fd_log_collector_msg_literal( instr_ctx, "Invalid Buffer account" );
        return FD_EXECUTOR_INSTR_ERR_INVALID_ARG;
      }
      buffer_lamports    = fd_borrowed_account_get_lamports( &buffer );
      buffer_data_offset = BUFFER_METADATA_SIZE;
      buffer_data_len    = fd_ulong_sat_sub( fd_borrowed_account_get_data_len( &buffer ), buffer_data_offset );
      if( FD_UNLIKELY( fd_borrowed_account_get_data_len( &buffer )<BUFFER_METADATA_SIZE || buffer_data_len==0UL ) ) {
        fd_log_collector_msg_literal( instr_ctx, "Buffer account too small" );
        return FD_EXECUTOR_INSTR_ERR_INVALID_ACC_DATA;
      }

      /* https://github.com/anza-xyz/agave/blob/v2.1.4/programs/bpf_loader/src/lib.rs#L774 */
      fd_borrowed_account_drop( &buffer );

      /* https://github.com/anza-xyz/agave/blob/574bae8fefc0ed256b55340b9d87b7689bcdf222/programs/bpf_loader/src/lib.rs#L775-L823 */
      /* Verify ProgramData account */

      ulong                               programdata_data_offset      = PROGRAMDATA_METADATA_SIZE;
      fd_bpf_upgradeable_loader_state_t * programdata_state            = NULL;
      ulong                               programdata_balance_required = 0UL;

      /* https://github.com/anza-xyz/agave/blob/v2.1.4/programs/bpf_loader/src/lib.rs#L778-L779 */
      fd_guarded_borrowed_account_t programdata;
      FD_TRY_BORROW_INSTR_ACCOUNT_DEFAULT_ERR_CHECK( instr_ctx, 0UL, &programdata );

      fd_rent_t const * rent = fd_bank_rent_query( instr_ctx->txn_ctx->bank );

      programdata_balance_required = fd_ulong_max( 1UL, fd_rent_exempt_minimum_balance( rent, fd_borrowed_account_get_data_len( &programdata ) ) );

      if( FD_UNLIKELY( fd_borrowed_account_get_data_len( &programdata )<fd_ulong_sat_add( PROGRAMDATA_METADATA_SIZE, buffer_data_len ) ) ) {
        fd_log_collector_msg_literal( instr_ctx, "ProgramData account not large enough" );
        return FD_EXECUTOR_INSTR_ERR_ACC_DATA_TOO_SMALL;
      }
      if( FD_UNLIKELY( fd_ulong_sat_add( fd_borrowed_account_get_lamports( &programdata ), buffer_lamports )<programdata_balance_required ) ) {
        fd_log_collector_msg_literal( instr_ctx, "Buffer account balance too low to fund upgrade" );
        return FD_EXECUTOR_INSTR_ERR_INSUFFICIENT_FUNDS;
      }
      programdata_state = fd_bpf_loader_program_get_state( programdata.acct, spad, &err );
      if( FD_UNLIKELY( err!=FD_BINCODE_SUCCESS ) ) {
        return err;
      }

      fd_sol_sysvar_clock_t const * clock = fd_sysvar_clock_read( instr_ctx->txn_ctx->funk, instr_ctx->txn_ctx->funk_txn, instr_ctx->txn_ctx->spad );
      if( FD_UNLIKELY( !clock ) ) {
        return FD_EXECUTOR_INSTR_ERR_GENERIC_ERR;
      }

      if( fd_bpf_upgradeable_loader_state_is_program_data( programdata_state ) ) {
        if( FD_UNLIKELY( clock->slot==programdata_state->inner.program_data.slot ) ) {
          fd_log_collector_msg_literal( instr_ctx, "Program was deployed in this block already" );
          return FD_EXECUTOR_INSTR_ERR_INVALID_ARG;
        }
        if( FD_UNLIKELY( !programdata_state->inner.program_data.upgrade_authority_address ) ) {
          fd_log_collector_msg_literal( instr_ctx, "Prrogram not upgradeable" );
          return FD_EXECUTOR_INSTR_ERR_ACC_IMMUTABLE;
        }
        if( FD_UNLIKELY( memcmp( programdata_state->inner.program_data.upgrade_authority_address, authority_key, sizeof(fd_pubkey_t) ) ) ) {
          fd_log_collector_msg_literal( instr_ctx, "Incorrect upgrade authority provided" );
          return FD_EXECUTOR_INSTR_ERR_INCORRECT_AUTHORITY;
        }
        if( FD_UNLIKELY( !fd_instr_acc_is_signer_idx( instr_ctx->instr, 6UL ) ) ) {
          fd_log_collector_msg_literal( instr_ctx, "Upgrade authority did not sign" );
          return FD_EXECUTOR_INSTR_ERR_MISSING_REQUIRED_SIGNATURE;
        }
      } else {
        fd_log_collector_msg_literal( instr_ctx, "Invalid ProgramData account" );
        return FD_EXECUTOR_INSTR_ERR_INVALID_ACC_DATA;
      }

      /* https://github.com/anza-xyz/agave/blob/v2.1.4/programs/bpf_loader/src/lib.rs#L824 */
      fd_borrowed_account_drop( &programdata );

      /* https://github.com/anza-xyz/agave/blob/574bae8fefc0ed256b55340b9d87b7689bcdf222/programs/bpf_loader/src/lib.rs#L825-L845 */
      /* Load and verify the program bits */

      /* https://github.com/anza-xyz/agave/blob/v2.1.4/programs/bpf_loader/src/lib.rs#L827-L828 */
      FD_TRY_BORROW_INSTR_ACCOUNT_DEFAULT_ERR_CHECK( instr_ctx, 2UL, &buffer );

      if( FD_UNLIKELY( buffer_data_offset>fd_borrowed_account_get_data_len( &buffer ) ) ) {
        return FD_EXECUTOR_INSTR_ERR_ACC_DATA_TOO_SMALL;
      }

      const uchar * buffer_data = fd_borrowed_account_get_data( &buffer ) + buffer_data_offset;
      err = fd_deploy_program( instr_ctx, buffer_data, buffer_data_len, instr_ctx->txn_ctx->spad );
      if( FD_UNLIKELY( err ) ) {
        return err;
      }

      /* https://github.com/anza-xyz/agave/blob/v2.1.4/programs/bpf_loader/src/lib.rs#L836 */
      fd_borrowed_account_drop( &buffer );

      /* https://github.com/anza-xyz/agave/blob/v2.1.4/programs/bpf_loader/src/lib.rs#L849-L850 */
      FD_TRY_BORROW_INSTR_ACCOUNT_DEFAULT_ERR_CHECK( instr_ctx, 0UL, &programdata );

      /* https://github.com/anza-xyz/agave/blob/574bae8fefc0ed256b55340b9d87b7689bcdf222/programs/bpf_loader/src/lib.rs#L846-L874 */
      /* Update the ProgramData account, record the upgraded data, and zero the rest in a local scope */
      do {
        programdata_state->discriminant                                 = fd_bpf_upgradeable_loader_state_enum_program_data;
        programdata_state->inner.program_data.slot                      = clock->slot;
        programdata_state->inner.program_data.upgrade_authority_address = (fd_pubkey_t *)authority_key;
        err = fd_bpf_loader_v3_program_set_state( &programdata, programdata_state );
        if( FD_UNLIKELY( err!=FD_BINCODE_SUCCESS ) ) {
          return err;
        }

        /* https://github.com/anza-xyz/agave/blob/574bae8fefc0ed256b55340b9d87b7689bcdf222/programs/bpf_loader/src/lib.rs#L846-L875 */
        /* We want to copy over the data and zero out the rest */
        if( FD_UNLIKELY( programdata_data_offset+buffer_data_len>fd_borrowed_account_get_data_len( &programdata ) ) ) {
          return FD_EXECUTOR_INSTR_ERR_ACC_DATA_TOO_SMALL;
        }

        uchar * programdata_data = NULL;
        ulong   programdata_dlen = 0UL;
        err = fd_borrowed_account_get_data_mut( &programdata, &programdata_data, &programdata_dlen );
        if( FD_UNLIKELY( err ) ) {
          return err;
        }
        uchar * dst_slice     = programdata_data + programdata_data_offset;
        ulong   dst_slice_len = buffer_data_len;

        /* https://github.com/anza-xyz/agave/blob/v2.1.4/programs/bpf_loader/src/lib.rs#L863-L864 */
        fd_guarded_borrowed_account_t buffer;
        FD_TRY_BORROW_INSTR_ACCOUNT_DEFAULT_ERR_CHECK( instr_ctx, 2UL, &buffer );

        if( FD_UNLIKELY( buffer_data_offset>fd_borrowed_account_get_data_len( &buffer ) ) ){
          return FD_EXECUTOR_INSTR_ERR_ACC_DATA_TOO_SMALL;
        }

        const uchar * src_slice = fd_borrowed_account_get_data( &buffer ) + buffer_data_offset;
        fd_memcpy( dst_slice, src_slice, dst_slice_len );
        fd_memset( dst_slice + dst_slice_len, 0, fd_borrowed_account_get_data_len( &programdata ) - programdata_data_offset - dst_slice_len );

        /* implicit drop of buffer */
      } while (0);

      /* https://github.com/anza-xyz/agave/blob/574bae8fefc0ed256b55340b9d87b7689bcdf222/programs/bpf_loader/src/lib.rs#L876-L891 */
      /* Fund ProgramData to rent-exemption, spill the rest */

      /* https://github.com/anza-xyz/agave/blob/v2.1.4/programs/bpf_loader/src/lib.rs#L878-L879 */
      FD_TRY_BORROW_INSTR_ACCOUNT_DEFAULT_ERR_CHECK( instr_ctx, 2UL, &buffer );

      /* https://github.com/anza-xyz/agave/blob/v2.1.4/programs/bpf_loader/src/lib.rs#L880-L881 */
      fd_guarded_borrowed_account_t spill;
      FD_TRY_BORROW_INSTR_ACCOUNT_DEFAULT_ERR_CHECK( instr_ctx, 3UL, &spill );

      ulong spill_addend = fd_ulong_sat_sub( fd_ulong_sat_add( fd_borrowed_account_get_lamports( &programdata ), buffer_lamports ),
                                            programdata_balance_required );
      err = fd_borrowed_account_checked_add_lamports( &spill, spill_addend );
      if( FD_UNLIKELY( err ) ) {
        return err;
      }
      err = fd_borrowed_account_set_lamports( &buffer, 0UL );
      if( FD_UNLIKELY( err ) ) {
        return err;
      }
      err = fd_borrowed_account_set_lamports( &programdata, programdata_balance_required );
      if( FD_UNLIKELY( err ) ) {
        return err;
      }

      /* Buffer account set_data_length */
      err = fd_borrowed_account_set_data_length( &buffer, BUFFER_METADATA_SIZE );
      if( FD_UNLIKELY( err ) ) {
        return err;
      }

      /* buffer is dropped when it goes out of scope */
      /* spill is dropped when it goes out of scope */
      /* programdata is dropped when it goes out of scope */

      /* Max msg_sz: 19 - 2 + 45 = 62 < 127 => we can use printf */
      //TODO: this is likely the incorrect program_id, do we have new_program_id?
      fd_log_collector_printf_dangerous_max_127( instr_ctx, "Upgraded program %s", FD_BASE58_ENC_32_ALLOCA( program_id ) );

      break;
    }
    /* https://github.com/anza-xyz/agave/blob/574bae8fefc0ed256b55340b9d87b7689bcdf222/programs/bpf_loader/src/lib.rs#L893-L957 */
    case fd_bpf_upgradeable_loader_program_instruction_enum_set_authority: {
      int err;
      if( FD_UNLIKELY( fd_exec_instr_ctx_check_num_insn_accounts( instr_ctx, 2U ) ) ) {
        return FD_EXECUTOR_INSTR_ERR_NOT_ENOUGH_ACC_KEYS;
      }

      /* https://github.com/anza-xyz/agave/blob/v2.1.4/programs/bpf_loader/src/lib.rs#L896-L897 */
      fd_guarded_borrowed_account_t account;
      FD_TRY_BORROW_INSTR_ACCOUNT_DEFAULT_ERR_CHECK( instr_ctx, 0UL, &account );

      /* https://github.com/anza-xyz/agave/blob/v2.1.14/programs/bpf_loader/src/lib.rs#L898-L900 */
      fd_pubkey_t const * present_authority_key = NULL;
      err = fd_exec_instr_ctx_get_key_of_account_at_index( instr_ctx, 1UL, &present_authority_key );
      if( FD_UNLIKELY( err ) ) {
        return err;
      }

      /* Don't check the error here because the new_authority key is allowed to be NULL until further checks.
         https://github.com/anza-xyz/agave/blob/v2.1.14/programs/bpf_loader/src/lib.rs#L901-L906 */
      fd_pubkey_t const * new_authority = NULL;
      fd_exec_instr_ctx_get_key_of_account_at_index( instr_ctx, 2UL, &new_authority );

      fd_bpf_upgradeable_loader_state_t * account_state = fd_bpf_loader_program_get_state( account.acct,
                                                                                           spad,
                                                                                           &err );
      if( FD_UNLIKELY( err!=FD_BINCODE_SUCCESS ) ) {
        return err;
      }

      if( fd_bpf_upgradeable_loader_state_is_buffer( account_state ) ) {
        if( FD_UNLIKELY( !new_authority ) ) {
          fd_log_collector_msg_literal( instr_ctx, "Buffer authority is not optional" );
          return FD_EXECUTOR_INSTR_ERR_INCORRECT_AUTHORITY;
        }
        if( FD_UNLIKELY( !account_state->inner.buffer.authority_address ) ) {
          fd_log_collector_msg_literal( instr_ctx, "Buffer is immutable" );
          return FD_EXECUTOR_INSTR_ERR_ACC_IMMUTABLE;
        }
        if( FD_UNLIKELY( memcmp( account_state->inner.buffer.authority_address, present_authority_key, sizeof(fd_pubkey_t) ) ) ) {
          fd_log_collector_msg_literal( instr_ctx, "Incorrect buffer authority provided" );
          return FD_EXECUTOR_INSTR_ERR_INCORRECT_AUTHORITY;
        }
        if( FD_UNLIKELY( !fd_instr_acc_is_signer_idx( instr_ctx->instr, 1UL ) ) ) {
          fd_log_collector_msg_literal( instr_ctx, "Buffer authority did not sign" );
          return FD_EXECUTOR_INSTR_ERR_MISSING_REQUIRED_SIGNATURE;
        }

        /* copy in the authority public key into the authority address.
           https://github.com/anza-xyz/agave/blob/v2.1.14/programs/bpf_loader/src/lib.rs#L926-L928 */
        *account_state->inner.buffer.authority_address = *new_authority;

        err = fd_bpf_loader_v3_program_set_state( &account, account_state );
        if( FD_UNLIKELY( err!=FD_BINCODE_SUCCESS ) ) {
          return err;
        }
      } else if( fd_bpf_upgradeable_loader_state_is_program_data( account_state ) ) {
        if( FD_UNLIKELY( !account_state->inner.program_data.upgrade_authority_address ) ) {
          fd_log_collector_msg_literal( instr_ctx, "Program not upgradeable" );
          return FD_EXECUTOR_INSTR_ERR_ACC_IMMUTABLE;
        }
        if( FD_UNLIKELY( memcmp( account_state->inner.program_data.upgrade_authority_address, present_authority_key, sizeof(fd_pubkey_t) ) ) ) {
          fd_log_collector_msg_literal( instr_ctx, "Incorrect upgrade authority provided" );
          return FD_EXECUTOR_INSTR_ERR_INCORRECT_AUTHORITY;
        }
        if( FD_UNLIKELY( !fd_instr_acc_is_signer_idx( instr_ctx->instr, 1UL ) ) ) {
          fd_log_collector_msg_literal( instr_ctx, "Upgrade authority did not sign" );
          return FD_EXECUTOR_INSTR_ERR_MISSING_REQUIRED_SIGNATURE;
        }

        /* copy in the authority public key into the upgrade authority address.
           https://github.com/anza-xyz/agave/blob/v2.1.14/programs/bpf_loader/src/lib.rs#L946-L949 */
        if( new_authority ) {
          *account_state->inner.program_data.upgrade_authority_address = *new_authority;
        } else {
          account_state->inner.program_data.upgrade_authority_address = NULL;
        }

        err = fd_bpf_loader_v3_program_set_state( &account, account_state );
        if( FD_UNLIKELY( err!=FD_BINCODE_SUCCESS ) ) {
          return err;
        }
      } else {
        fd_log_collector_msg_literal( instr_ctx, "Account does not support authorities" );
        return FD_EXECUTOR_INSTR_ERR_INVALID_ARG;
      }

      /* Max msg_sz: 16 - 2 + 45 = 59 < 127 => we can use printf */
      fd_log_collector_printf_dangerous_max_127( instr_ctx, "New authority %s", FD_BASE58_ENC_32_ALLOCA( new_authority ) );

      /* implicit drop of account */

      break;
    }
    /* https://github.com/anza-xyz/agave/blob/574bae8fefc0ed256b55340b9d87b7689bcdf222/programs/bpf_loader/src/lib.rs#L958-L1030 */
    case fd_bpf_upgradeable_loader_program_instruction_enum_set_authority_checked: {
      int err;
      if( !FD_FEATURE_ACTIVE_BANK( instr_ctx->txn_ctx->bank, enable_bpf_loader_set_authority_checked_ix ) ) {
        return FD_EXECUTOR_INSTR_ERR_INVALID_INSTR_DATA;
      }

      if( FD_UNLIKELY( fd_exec_instr_ctx_check_num_insn_accounts( instr_ctx, 3U ) ) ) {
        return FD_EXECUTOR_INSTR_ERR_NOT_ENOUGH_ACC_KEYS;
      }

      /* https://github.com/anza-xyz/agave/blob/v2.1.4/programs/bpf_loader/src/lib.rs#L968-L969 */
      fd_guarded_borrowed_account_t account;
      FD_TRY_BORROW_INSTR_ACCOUNT_DEFAULT_ERR_CHECK( instr_ctx, 0UL, &account );

      /* https://github.com/anza-xyz/agave/blob/v2.1.14/programs/bpf_loader/src/lib.rs#L970-L975 */
      fd_pubkey_t const * present_authority_key = NULL;
      fd_pubkey_t const * new_authority_key     = NULL;

      err = fd_exec_instr_ctx_get_key_of_account_at_index( instr_ctx, 1UL, &present_authority_key );
      if( FD_UNLIKELY( err ) ) {
        return err;
      }

      err = fd_exec_instr_ctx_get_key_of_account_at_index( instr_ctx, 2UL, &new_authority_key );
      if( FD_UNLIKELY( err ) ) {
        return err;
      }


      fd_bpf_upgradeable_loader_state_t * account_state = fd_bpf_loader_program_get_state( account.acct, spad, &err );
      if( FD_UNLIKELY( err!=FD_BINCODE_SUCCESS ) ) {
        return err;
      }

      if( fd_bpf_upgradeable_loader_state_is_buffer( account_state ) ) {
        if( FD_UNLIKELY( !account_state->inner.buffer.authority_address ) ) {
          fd_log_collector_msg_literal( instr_ctx, "Buffer is immutable" );
          return FD_EXECUTOR_INSTR_ERR_ACC_IMMUTABLE;
        }
        if( FD_UNLIKELY( memcmp( account_state->inner.buffer.authority_address, present_authority_key, sizeof(fd_pubkey_t) ) ) ) {
          fd_log_collector_msg_literal( instr_ctx, "Incorrect buffer authority provided" );
          return FD_EXECUTOR_INSTR_ERR_INCORRECT_AUTHORITY;
        }
        if( FD_UNLIKELY( !fd_instr_acc_is_signer_idx( instr_ctx->instr, 1UL ) ) ) {
          fd_log_collector_msg_literal( instr_ctx, "Buffer authority did not sign" );
          return FD_EXECUTOR_INSTR_ERR_MISSING_REQUIRED_SIGNATURE;
        }
        if( FD_UNLIKELY( !fd_instr_acc_is_signer_idx( instr_ctx->instr, 2UL ) ) ) {
          fd_log_collector_msg_literal( instr_ctx, "New authority did not sign" );
          return FD_EXECUTOR_INSTR_ERR_MISSING_REQUIRED_SIGNATURE;
        }
        account_state->inner.buffer.authority_address = (fd_pubkey_t*)new_authority_key;
        err = fd_bpf_loader_v3_program_set_state( &account, account_state );
        if( FD_UNLIKELY( err!=FD_BINCODE_SUCCESS ) ) {
          return err;
        }
      } else if( fd_bpf_upgradeable_loader_state_is_program_data( account_state ) ) {
        if( FD_UNLIKELY( !account_state->inner.program_data.upgrade_authority_address ) ) {
          fd_log_collector_msg_literal( instr_ctx, "Program not upgradeable" );
          return FD_EXECUTOR_INSTR_ERR_ACC_IMMUTABLE;
        }
        if( FD_UNLIKELY( memcmp( account_state->inner.program_data.upgrade_authority_address, present_authority_key, sizeof(fd_pubkey_t) ) ) ) {
          fd_log_collector_msg_literal( instr_ctx, "Incorrect upgrade authority provided" );
          return FD_EXECUTOR_INSTR_ERR_INCORRECT_AUTHORITY;
        }
        if( FD_UNLIKELY( !fd_instr_acc_is_signer_idx( instr_ctx->instr, 1UL ) ) ) {
          fd_log_collector_msg_literal( instr_ctx, "Upgrade authority did not sign" );
          return FD_EXECUTOR_INSTR_ERR_MISSING_REQUIRED_SIGNATURE;
        }
        if( FD_UNLIKELY( !fd_instr_acc_is_signer_idx( instr_ctx->instr, 2UL ) ) ) {
          fd_log_collector_msg_literal( instr_ctx, "New authority did not sign" );
          return FD_EXECUTOR_INSTR_ERR_MISSING_REQUIRED_SIGNATURE;
        }
        account_state->inner.program_data.upgrade_authority_address = (fd_pubkey_t*)new_authority_key;
        err = fd_bpf_loader_v3_program_set_state( &account, account_state );
        if( FD_UNLIKELY( err!=FD_BINCODE_SUCCESS ) ) {
          return err;
        }
      } else {
        fd_log_collector_msg_literal( instr_ctx, "Account does not support authorities" );
        return FD_EXECUTOR_INSTR_ERR_INVALID_ARG;
      }

      /* Max msg_sz: 16 - 2 + 45 = 59 < 127 => we can use printf */
      fd_log_collector_printf_dangerous_max_127( instr_ctx, "New authority %s", FD_BASE58_ENC_32_ALLOCA( new_authority_key ) );

      /* implicit drop of account */

      break;
    }
    /* https://github.com/anza-xyz/agave/blob/574bae8fefc0ed256b55340b9d87b7689bcdf222/programs/bpf_loader/src/lib.rs#L1031-L1134 */
    case fd_bpf_upgradeable_loader_program_instruction_enum_close: {
      int err;
      /* https://github.com/anza-xyz/agave/blob/574bae8fefc0ed256b55340b9d87b7689bcdf222/programs/bpf_loader/src/lib.rs#L1032-L1046 */
      if( FD_UNLIKELY( fd_exec_instr_ctx_check_num_insn_accounts( instr_ctx, 2U ) ) ) {
        return FD_EXECUTOR_INSTR_ERR_NOT_ENOUGH_ACC_KEYS;
      }

      /* It's safe to directly access the instruction accounts because we already checked for two
         instruction accounts previously.
         https://github.com/anza-xyz/agave/blob/v2.1.14/programs/bpf_loader/src/lib.rs#L1034-L1035 */
      if( FD_UNLIKELY( instr_ctx->instr->accounts[ 0UL ].index_in_transaction ==
                       instr_ctx->instr->accounts[ 1UL ].index_in_transaction ) ) {
        fd_log_collector_msg_literal( instr_ctx, "Recipient is the same as the account being closed" );
        return FD_EXECUTOR_INSTR_ERR_INVALID_ARG;
      }

      /* https://github.com/anza-xyz/agave/blob/v2.1.4/programs/bpf_loader/src/lib.rs#L1043-L1044 */
      fd_guarded_borrowed_account_t close_account;
      FD_TRY_BORROW_INSTR_ACCOUNT_DEFAULT_ERR_CHECK( instr_ctx, 0UL, &close_account );

      fd_pubkey_t * close_key = close_account.acct->pubkey;
      fd_bpf_upgradeable_loader_state_t * close_account_state = fd_bpf_loader_program_get_state( close_account.acct, spad, &err );
      if( FD_UNLIKELY( err!=FD_BINCODE_SUCCESS ) ) {
        return err;
      }
      /* Close account set data length */
      err = fd_borrowed_account_set_data_length( &close_account, SIZE_OF_UNINITIALIZED );
      if( FD_UNLIKELY( err ) ) {
        return err;
      }

      /* https://github.com/anza-xyz/agave/blob/574bae8fefc0ed256b55340b9d87b7689bcdf222/programs/bpf_loader/src/lib.rs#L1049-L1056 */
      if( fd_bpf_upgradeable_loader_state_is_uninitialized( close_account_state ) ) {

        /* https://github.com/anza-xyz/agave/blob/v2.1.4/programs/bpf_loader/src/lib.rs#L1050-L1051 */
        fd_guarded_borrowed_account_t recipient_account;
        FD_TRY_BORROW_INSTR_ACCOUNT_DEFAULT_ERR_CHECK( instr_ctx, 1UL, &recipient_account );

        err = fd_borrowed_account_checked_add_lamports( &recipient_account, fd_borrowed_account_get_lamports( &close_account ) );
        if( FD_UNLIKELY( err ) ) {
          return err;
        }
        err = fd_borrowed_account_set_lamports( &close_account, 0UL );
        if( FD_UNLIKELY( err ) ) {
          return err;
        }
        /* Max msg_sz: 23 - 2 + 45 = 66 < 127 => we can use printf */
        fd_log_collector_printf_dangerous_max_127( instr_ctx,
          "Closed Uninitialized %s", FD_BASE58_ENC_32_ALLOCA( close_key ) );

      /* https://github.com/anza-xyz/agave/blob/574bae8fefc0ed256b55340b9d87b7689bcdf222/programs/bpf_loader/src/lib.rs#L1057-L1068 */
      } else if( fd_bpf_upgradeable_loader_state_is_buffer( close_account_state ) ) {

        /* https://github.com/anza-xyz/agave/blob/v2.1.4/programs/bpf_loader/src/lib.rs#L1059 */
        fd_borrowed_account_drop( &close_account );

        if( FD_UNLIKELY( fd_exec_instr_ctx_check_num_insn_accounts( instr_ctx, 3U ) ) ) {
          return FD_EXECUTOR_INSTR_ERR_NOT_ENOUGH_ACC_KEYS;
        }

        err = common_close_account( close_account_state->inner.buffer.authority_address, instr_ctx, close_account_state );
        if( FD_UNLIKELY( err ) ) {
          return err;
        }
        /* Max msg_sz: 16 - 2 + 45 = 63 < 127 => we can use printf */
        fd_log_collector_printf_dangerous_max_127( instr_ctx,
          "Closed Buffer %s", FD_BASE58_ENC_32_ALLOCA( close_key ) );

      /* https://github.com/anza-xyz/agave/blob/574bae8fefc0ed256b55340b9d87b7689bcdf222/programs/bpf_loader/src/lib.rs#L1069-L1129 */
      } else if( fd_bpf_upgradeable_loader_state_is_program_data( close_account_state ) ) {
        int err;
        if( FD_UNLIKELY( fd_exec_instr_ctx_check_num_insn_accounts( instr_ctx, 4U ) ) ) {
          return FD_EXECUTOR_INSTR_ERR_NOT_ENOUGH_ACC_KEYS;
        }

        /* https://github.com/anza-xyz/agave/blob/v2.1.4/programs/bpf_loader/src/lib.rs#L1074 */
        fd_borrowed_account_drop( &close_account );

        /* https://github.com/anza-xyz/agave/blob/v2.1.4/programs/bpf_loader/src/lib.rs#L1075-L1076 */
        fd_guarded_borrowed_account_t program_account;
        FD_TRY_BORROW_INSTR_ACCOUNT_DEFAULT_ERR_CHECK(instr_ctx, 3UL, &program_account );

        if( FD_UNLIKELY( !fd_borrowed_account_is_writable( &program_account ) ) ) {
          fd_log_collector_msg_literal( instr_ctx, "Program account is not writable" );
          return FD_EXECUTOR_INSTR_ERR_INVALID_ARG;
        }
        if( FD_UNLIKELY( memcmp( fd_borrowed_account_get_owner( &program_account ), program_id, sizeof(fd_pubkey_t) ) ) ) {
          fd_log_collector_msg_literal( instr_ctx, "Program account not owned by loader" );
          return FD_EXECUTOR_INSTR_ERR_INCORRECT_PROGRAM_ID;
        }
        fd_sol_sysvar_clock_t const * clock = fd_sysvar_clock_read( instr_ctx->txn_ctx->funk, instr_ctx->txn_ctx->funk_txn, instr_ctx->txn_ctx->spad );
        if( FD_UNLIKELY( !clock ) ) {
          return FD_EXECUTOR_INSTR_ERR_UNSUPPORTED_SYSVAR;
        }
        if( FD_UNLIKELY( clock->slot==close_account_state->inner.program_data.slot ) ) {
          fd_log_collector_msg_literal( instr_ctx,"Program was deployed in this block already" );
          return FD_EXECUTOR_INSTR_ERR_INVALID_ARG;
        }

        fd_bpf_upgradeable_loader_state_t * program_state = fd_bpf_loader_program_get_state( program_account.acct, spad, &err );
        if( FD_UNLIKELY( err!=FD_BINCODE_SUCCESS ) ) {
          return err;
        }
        if( fd_bpf_upgradeable_loader_state_is_program( program_state ) ) {
          if( FD_UNLIKELY( memcmp( &program_state->inner.program.programdata_address, close_key, sizeof(fd_pubkey_t) ) ) ) {
            fd_log_collector_msg_literal( instr_ctx,"Program account does not match ProgramData account" );
            return FD_EXECUTOR_INSTR_ERR_INVALID_ARG;
          }

          /* https://github.com/anza-xyz/agave/blob/v2.1.4/programs/bpf_loader/src/lib.rs#L1105 */
          fd_borrowed_account_drop( &program_account );

          err = common_close_account( close_account_state->inner.program_data.upgrade_authority_address,
                                      instr_ctx,
                                      close_account_state );
          if( FD_UNLIKELY( err ) ) {
            return err;
          }

          /* The Agave client updates the account state upon closing an account
             in their loaded program cache. Checking for a program can be
             checked by checking to see if the programdata account's loader state
             is unitialized. The firedancer implementation also removes closed
             accounts from the loaded program cache at the end of a slot. Closed
             accounts are not checked from the cache, instead the account state
             is looked up. */

        } else {
          fd_log_collector_msg_literal( instr_ctx, "Invalid program account" );
          return FD_EXECUTOR_INSTR_ERR_INVALID_ARG;
        }

        /* Max msg_sz: 17 - 2 + 45 = 60 < 127 => we can use printf */
        fd_log_collector_printf_dangerous_max_127( instr_ctx,
          "Closed Program %s", FD_BASE58_ENC_32_ALLOCA( close_key ) );

        /* program account is dropped when it goes out of scope */
      } else {
        fd_log_collector_msg_literal( instr_ctx, "Account does not support closing" );
        return FD_EXECUTOR_INSTR_ERR_INVALID_ARG;
      }

      /* implicit drop of close account */
      break;
    }
    /* https://github.com/anza-xyz/agave/blob/v2.3.1/programs/bpf_loader/src/lib.rs#L1158-L1170 */
    case fd_bpf_upgradeable_loader_program_instruction_enum_extend_program: {
      if( FD_UNLIKELY( FD_FEATURE_ACTIVE_BANK( instr_ctx->txn_ctx->bank, enable_extend_program_checked ) ) ) {
        fd_log_collector_msg_literal( instr_ctx, "ExtendProgram was superseded by ExtendProgramChecked" );
        return FD_EXECUTOR_INSTR_ERR_INVALID_INSTR_DATA;
      }
      err = common_extend_program( instr_ctx, instruction->inner.extend_program.additional_bytes, 0 );
      if( FD_UNLIKELY( err ) ) {
        return err;
      }

      break;
    }
    /* https://github.com/anza-xyz/agave/blob/v2.3.1/programs/bpf_loader/src/lib.rs#L1171-L1179 */
    case fd_bpf_upgradeable_loader_program_instruction_enum_extend_program_checked: {
      if( FD_UNLIKELY( !FD_FEATURE_ACTIVE_BANK( instr_ctx->txn_ctx->bank, enable_extend_program_checked ) ) ) {
        return FD_EXECUTOR_INSTR_ERR_INVALID_INSTR_DATA;
      }
      err = common_extend_program( instr_ctx, instruction->inner.extend_program_checked.additional_bytes, 1 );
      if( FD_UNLIKELY( err ) ) {
        return err;
      }

      break;
    }
    /* https://github.com/anza-xyz/agave/blob/v2.2.6/programs/bpf_loader/src/lib.rs#L1338-L1508 */
    case fd_bpf_upgradeable_loader_program_instruction_enum_migrate: {
      /* https://github.com/anza-xyz/agave/blob/v2.2.6/programs/bpf_loader/src/lib.rs#L1339-L1344 */
      if( FD_UNLIKELY( !FD_FEATURE_ACTIVE_BANK( instr_ctx->txn_ctx->bank, enable_loader_v4 ) ) ) {
        return FD_EXECUTOR_INSTR_ERR_INVALID_INSTR_DATA;
      }

      /* https://github.com/anza-xyz/agave/blob/v2.2.6/programs/bpf_loader/src/lib.rs#L1346 */
      if( FD_UNLIKELY( fd_exec_instr_ctx_check_num_insn_accounts( instr_ctx, 3U ) ) ) {
        return FD_EXECUTOR_INSTR_ERR_NOT_ENOUGH_ACC_KEYS;
      }

      /* https://github.com/anza-xyz/agave/blob/v2.2.6/programs/bpf_loader/src/lib.rs#L1347-L1349 */
      fd_pubkey_t const * programdata_address = NULL;
      err = fd_exec_instr_ctx_get_key_of_account_at_index( instr_ctx, 0UL, &programdata_address );
      if( FD_UNLIKELY( err ) ) {
        return err;
      }

      /* https://github.com/anza-xyz/agave/blob/v2.2.6/programs/bpf_loader/src/lib.rs#L1350-L1352 */
      fd_pubkey_t const * program_address = NULL;
      err = fd_exec_instr_ctx_get_key_of_account_at_index( instr_ctx, 1UL, &program_address );
      if( FD_UNLIKELY( err ) ) {
        return err;
      }

      /* https://github.com/anza-xyz/agave/blob/v2.2.6/programs/bpf_loader/src/lib.rs#L1353-L1355 */
      fd_pubkey_t const * provided_authority_address = NULL;
      err = fd_exec_instr_ctx_get_key_of_account_at_index( instr_ctx, 2UL, &provided_authority_address );
      if( FD_UNLIKELY( err ) ) {
        return err;
      }

      /* https://github.com/anza-xyz/agave/blob/v2.2.6/programs/bpf_loader/src/lib.rs#L1356-L1359 */
      fd_sol_sysvar_clock_t const * clock = fd_sysvar_clock_read( instr_ctx->txn_ctx->funk, instr_ctx->txn_ctx->funk_txn, instr_ctx->txn_ctx->spad );
      if( FD_UNLIKELY( !clock ) ) {
        return FD_EXECUTOR_INSTR_ERR_UNSUPPORTED_SYSVAR;
      }
      ulong clock_slot = clock->slot;

      /* Verify ProgramData account
         https://github.com/anza-xyz/agave/blob/v2.2.6/programs/bpf_loader/src/lib.rs#L1362-L1363 */
      fd_guarded_borrowed_account_t programdata;
      FD_TRY_BORROW_INSTR_ACCOUNT_DEFAULT_ERR_CHECK( instr_ctx, 0UL, &programdata );

      /* https://github.com/anza-xyz/agave/blob/v2.2.6/programs/bpf_loader/src/lib.rs#L1364-L1367 */
      if( FD_UNLIKELY( !fd_borrowed_account_is_writable( &programdata ) ) ) {
        fd_log_collector_msg_literal( instr_ctx, "ProgramData account not writeable" );
        return FD_EXECUTOR_INSTR_ERR_INVALID_ARG;
      }

      /* https://github.com/anza-xyz/agave/blob/v2.2.6/programs/bpf_loader/src/lib.rs#L1368-L1387 */
      ulong         program_len               = 0UL;
      fd_pubkey_t * upgrade_authority_address = NULL;
      fd_bpf_upgradeable_loader_state_t * programdata_state = fd_bpf_loader_program_get_state( programdata.acct, spad, &err );
      if( FD_LIKELY( err==FD_BINCODE_SUCCESS && fd_bpf_upgradeable_loader_state_is_program_data( programdata_state ) ) ) {

        /* https://github.com/anza-xyz/agave/blob/v2.2.6/programs/bpf_loader/src/lib.rs#L1374-L1377 */
        if( FD_UNLIKELY( clock_slot==programdata_state->inner.program_data.slot ) ) {
          fd_log_collector_msg_literal( instr_ctx, "Program was deployed in this block already" );
          return FD_EXECUTOR_INSTR_ERR_INVALID_ARG;
        }

        /* https://github.com/anza-xyz/agave/blob/v2.2.6/programs/bpf_loader/src/lib.rs#L1378-L1384 */
        program_len = fd_ulong_sat_sub( fd_borrowed_account_get_data_len( &programdata ), PROGRAMDATA_METADATA_SIZE );
        upgrade_authority_address = programdata_state->inner.program_data.upgrade_authority_address;
      }

      /* https://github.com/anza-xyz/agave/blob/v2.2.6/programs/bpf_loader/src/lib.rs#L1388 */
      ulong programdata_funds = fd_borrowed_account_get_lamports( &programdata );

      /* https://github.com/anza-xyz/agave/blob/v2.2.6/programs/bpf_loader/src/lib.rs#L1389 */
      fd_borrowed_account_drop( &programdata );

      /* Verify authority signature
         https://github.com/anza-xyz/agave/blob/v2.2.6/programs/bpf_loader/src/lib.rs#L1391-L1398 */
      fd_pubkey_t const * authority_key_to_compare = upgrade_authority_address ? upgrade_authority_address : program_address;
      if( FD_UNLIKELY( memcmp( fd_solana_migration_authority.key, provided_authority_address->key, sizeof(fd_pubkey_t) ) &&
                       memcmp( authority_key_to_compare->key, provided_authority_address->key, sizeof(fd_pubkey_t) ) ) ) {
        fd_log_collector_msg_literal( instr_ctx, "Incorrect migration authority provided" );
        return FD_EXECUTOR_INSTR_ERR_INCORRECT_AUTHORITY;
      }

      /* https://github.com/anza-xyz/agave/blob/v2.2.6/programs/bpf_loader/src/lib.rs#L1399-L1402 */
      if( FD_UNLIKELY( !instr_ctx->instr->accounts[ 2UL ].is_signer ) ) {
        fd_log_collector_msg_literal( instr_ctx, "Migration authority did not sign" );
        return FD_EXECUTOR_INSTR_ERR_MISSING_REQUIRED_SIGNATURE;
      }

      /* Verify Program account
         https://github.com/anza-xyz/agave/blob/v2.2.6/programs/bpf_loader/src/lib.rs#L1404-L1406 */
      fd_guarded_borrowed_account_t program;
      FD_TRY_BORROW_INSTR_ACCOUNT_DEFAULT_ERR_CHECK( instr_ctx, 1UL, &program );

      /* https://github.com/anza-xyz/agave/blob/v2.2.6/programs/bpf_loader/src/lib.rs#L1407-L1410 */
      if( FD_UNLIKELY( !fd_borrowed_account_is_writable( &program ) ) ) {
        fd_log_collector_msg_literal( instr_ctx, "Program account not writeable" );
        return FD_EXECUTOR_INSTR_ERR_INVALID_ARG;
      }

      /* https://github.com/anza-xyz/agave/blob/v2.2.6/programs/bpf_loader/src/lib.rs#L1411-L1414 */
      if( FD_UNLIKELY( memcmp( fd_borrowed_account_get_owner( &program ), program_id, sizeof(fd_pubkey_t) ) ) ) {
        fd_log_collector_msg_literal( instr_ctx, "Program account not owned by loader" );
        return FD_EXECUTOR_INSTR_ERR_INCORRECT_PROGRAM_ID;
      }

      /* https://github.com/anza-xyz/agave/blob/v2.2.6/programs/bpf_loader/src/lib.rs#L1415-L1426 */
      fd_bpf_upgradeable_loader_state_t * program_state = fd_bpf_loader_program_get_state( program.acct, spad, &err );
      if( FD_UNLIKELY( err!=FD_BINCODE_SUCCESS ) ) {
        return err;
      }

      if( FD_LIKELY( fd_bpf_upgradeable_loader_state_is_program( program_state ) ) ) {
        if( FD_UNLIKELY( memcmp( programdata_address->key, program_state->inner.program.programdata_address.key, sizeof(fd_pubkey_t) ) ) ) {
          fd_log_collector_msg_literal( instr_ctx, "Program and ProgramData account mismatch" );
          return FD_EXECUTOR_INSTR_ERR_INVALID_ARG;
        }
      } else {
        fd_log_collector_msg_literal( instr_ctx, "Invalid Program account" );
        return FD_EXECUTOR_INSTR_ERR_INVALID_ACC_DATA;
      }

      /* https://github.com/anza-xyz/agave/blob/v2.2.6/programs/bpf_loader/src/lib.rs#L1427 */
      err = fd_borrowed_account_set_data_from_slice( &program, NULL, 0UL );
      if( FD_UNLIKELY( err ) ) {
        return err;
      }

      /* https://github.com/anza-xyz/agave/blob/v2.2.6/programs/bpf_loader/src/lib.rs#L1428 */
      err = fd_borrowed_account_checked_add_lamports( &program , programdata_funds );
      if( FD_UNLIKELY( err ) ) {
        return err;
      }

      /* https://github.com/anza-xyz/agave/blob/v2.3.1/programs/bpf_loader/src/lib.rs#L1268 */
      err = fd_borrowed_account_set_owner( &program, &fd_solana_bpf_loader_v4_program_id );
      if( FD_UNLIKELY( err ) ) {
        return err;
      }

      /* https://github.com/anza-xyz/agave/blob/v2.2.6/programs/bpf_loader/src/lib.rs#L1434 */
      fd_borrowed_account_drop( &program );

      /* https://github.com/anza-xyz/agave/blob/v2.2.6/programs/bpf_loader/src/lib.rs#L1436-L1437 */
      err = fd_exec_instr_ctx_try_borrow_instr_account( instr_ctx , 0U, &programdata );
      if( FD_UNLIKELY( err ) ) {
        return err;
      }

      /* https://github.com/anza-xyz/agave/blob/v2.2.6/programs/bpf_loader/src/lib.rs#L1438 */
      err = fd_borrowed_account_set_lamports( &programdata, 0UL );
      if( FD_UNLIKELY( err ) ) {
        return err;
      }

      /* https://github.com/anza-xyz/agave/blob/v2.2.6/programs/bpf_loader/src/lib.rs#L1439 */
      fd_borrowed_account_drop( &programdata );

      FD_SPAD_FRAME_BEGIN( instr_ctx->txn_ctx->spad ) {
        uchar                              instr_data[FD_TXN_MTU];
        fd_loader_v4_program_instruction_t instr      = {0};
        fd_bincode_encode_ctx_t            encode_ctx = {0};
        fd_vm_rust_account_meta_t *        acct_metas = fd_spad_alloc( instr_ctx->txn_ctx->spad,
                                                                       FD_VM_RUST_ACCOUNT_META_ALIGN,
                                                                       3UL * sizeof(fd_vm_rust_account_meta_t) );

        /* https://github.com/anza-xyz/agave/blob/v2.2.6/programs/bpf_loader/src/lib.rs#L1441-L1484 */
        if( FD_LIKELY( program_len>0UL ) ) {

          /* Set program length
             https://github.com/anza-xyz/agave/blob/v2.2.6/programs/bpf_loader/src/lib.rs#L1442-L1451 */
          fd_native_cpi_create_account_meta( program_address,            0, 1, &acct_metas[0] );
          fd_native_cpi_create_account_meta( provided_authority_address, 1, 0, &acct_metas[1] );
          fd_native_cpi_create_account_meta( program_address,            0, 1, &acct_metas[2] );

          instr = (fd_loader_v4_program_instruction_t) {
            .discriminant = fd_loader_v4_program_instruction_enum_set_program_length,
            .inner = {
              .set_program_length = {
                .new_size = (uint)program_len
              }
            }
          };

          encode_ctx = (fd_bincode_encode_ctx_t) {
            .data    = instr_data,
            .dataend = instr_data + FD_TXN_MTU
          };

          // This should never fail.
          err = fd_loader_v4_program_instruction_encode( &instr, &encode_ctx );
          if( FD_UNLIKELY( err ) ) {
            return FD_EXECUTOR_INSTR_ERR_FATAL;
          }

          err = fd_native_cpi_native_invoke( instr_ctx,
                                             &fd_solana_bpf_loader_v4_program_id,
                                             instr_data,
                                             FD_TXN_MTU,
                                             acct_metas,
                                             3UL,
                                             NULL,
                                             0UL );
          if( FD_UNLIKELY( err ) ) {
            return err;
          }

          /* Copy
             https://github.com/anza-xyz/agave/blob/v2.2.6/programs/bpf_loader/src/lib.rs#L1453-L1464 */
          fd_native_cpi_create_account_meta( program_address,            0, 1, &acct_metas[0] );
          fd_native_cpi_create_account_meta( provided_authority_address, 1, 0, &acct_metas[1] );
          fd_native_cpi_create_account_meta( programdata_address,        0, 0, &acct_metas[2] );

          instr = (fd_loader_v4_program_instruction_t) {
            .discriminant = fd_loader_v4_program_instruction_enum_copy,
            .inner = {
              .copy = {
                .destination_offset = 0U,
                .source_offset      = 0U,
                .length             = (uint)program_len
              }
            }
          };

          encode_ctx = (fd_bincode_encode_ctx_t) {
            .data    = instr_data,
            .dataend = instr_data + FD_TXN_MTU
          };

          // This should never fail.
          err = fd_loader_v4_program_instruction_encode( &instr, &encode_ctx );
          if( FD_UNLIKELY( err ) ) {
            return FD_EXECUTOR_INSTR_ERR_FATAL;
          }

          err = fd_native_cpi_native_invoke( instr_ctx,
                                             &fd_solana_bpf_loader_v4_program_id,
                                             instr_data,
                                             FD_TXN_MTU,
                                             acct_metas,
                                             3UL,
                                             NULL,
                                             0UL );
          if( FD_UNLIKELY( err ) ) {
            return err;
          }

          /* Deploy
             https://github.com/anza-xyz/agave/blob/v2.2.6/programs/bpf_loader/src/lib.rs#L1466-L1473 */
          fd_native_cpi_create_account_meta( program_address,            0, 1, &acct_metas[0] );
          fd_native_cpi_create_account_meta( provided_authority_address, 1, 0, &acct_metas[1] );

          instr = (fd_loader_v4_program_instruction_t) {
            .discriminant = fd_loader_v4_program_instruction_enum_deploy,
          };

          encode_ctx = (fd_bincode_encode_ctx_t) {
            .data    = instr_data,
            .dataend = instr_data + FD_TXN_MTU
          };

          // This should never fail.
          err = fd_loader_v4_program_instruction_encode( &instr, &encode_ctx );
          if( FD_UNLIKELY( err ) ) {
            return FD_EXECUTOR_INSTR_ERR_FATAL;
          }

          err = fd_native_cpi_native_invoke( instr_ctx,
                                            &fd_solana_bpf_loader_v4_program_id,
                                            instr_data,
                                            FD_TXN_MTU,
                                            acct_metas,
                                            2UL,
                                            NULL,
                                            0UL );
          if( FD_UNLIKELY( err ) ) {
            return err;
          }

          /* Finalize (if no upgrade authority address provided)
             https://github.com/anza-xyz/agave/blob/v2.2.6/programs/bpf_loader/src/lib.rs#L1475-L1484 */
          if( upgrade_authority_address==NULL ) {
            fd_native_cpi_create_account_meta( program_address,            0, 1, &acct_metas[0] );
            fd_native_cpi_create_account_meta( provided_authority_address, 1, 0, &acct_metas[1] );
            fd_native_cpi_create_account_meta( program_address,            0, 0, &acct_metas[2] );

            instr = (fd_loader_v4_program_instruction_t) {
              .discriminant = fd_loader_v4_program_instruction_enum_finalize,
            };

            encode_ctx = (fd_bincode_encode_ctx_t) {
              .data    = instr_data,
              .dataend = instr_data + FD_TXN_MTU
            };

            // This should never fail.
            err = fd_loader_v4_program_instruction_encode( &instr, &encode_ctx );
            if( FD_UNLIKELY( err ) ) {
              return FD_EXECUTOR_INSTR_ERR_FATAL;
            }

            err = fd_native_cpi_native_invoke( instr_ctx,
                                              &fd_solana_bpf_loader_v4_program_id,
                                              instr_data,
                                              FD_TXN_MTU,
                                              acct_metas,
                                              3UL,
                                              NULL,
                                              0UL );
            if( FD_UNLIKELY( err ) ) {
              return err;
            }
          } else if( !memcmp( fd_solana_migration_authority.key, provided_authority_address->key, sizeof(fd_pubkey_t) ) ) {

            /* Transfer authority
               https://github.com/anza-xyz/agave/blob/v2.2.6/programs/bpf_loader/src/lib.rs#L1486-L1494 */
            fd_native_cpi_create_account_meta( program_address,            0, 1, &acct_metas[0] );
            fd_native_cpi_create_account_meta( provided_authority_address, 1, 0, &acct_metas[1] );
            fd_native_cpi_create_account_meta( upgrade_authority_address,  1, 0, &acct_metas[2] );

            instr = (fd_loader_v4_program_instruction_t) {
              .discriminant = fd_loader_v4_program_instruction_enum_transfer_authority,
            };

            encode_ctx = (fd_bincode_encode_ctx_t) {
              .data    = instr_data,
              .dataend = instr_data + FD_TXN_MTU
            };

            // This should never fail.
            err = fd_loader_v4_program_instruction_encode( &instr, &encode_ctx );
            if( FD_UNLIKELY( err ) ) {
              return FD_EXECUTOR_INSTR_ERR_FATAL;
            }

            err = fd_native_cpi_native_invoke( instr_ctx,
                                              &fd_solana_bpf_loader_v4_program_id,
                                              instr_data,
                                              FD_TXN_MTU,
                                              acct_metas,
                                              3UL,
                                              NULL,
                                              0UL );
            if( FD_UNLIKELY( err ) ) {
              return err;
            }
          }
        }
      } FD_SPAD_FRAME_END;

      /* https://github.com/anza-xyz/agave/blob/v2.2.6/programs/bpf_loader/src/lib.rs#L1500-L1501 */
      err = fd_exec_instr_ctx_try_borrow_instr_account( instr_ctx , 0U, &programdata );
      if( FD_UNLIKELY( err ) ) {
        return err;
      }

      /* https://github.com/anza-xyz/agave/blob/v2.2.6/programs/bpf_loader/src/lib.rs#L1502 */
      err = fd_borrowed_account_set_data_from_slice( &programdata, NULL, 0UL );
      if( FD_UNLIKELY( err ) ) {
        return err;
      }

      /* https://github.com/anza-xyz/agave/blob/v2.2.6/programs/bpf_loader/src/lib.rs#L1504 */
      fd_borrowed_account_drop( &programdata );

      /* https://github.com/anza-xyz/agave/blob/v2.2.6/programs/bpf_loader/src/lib.rs#L1506 */
      fd_log_collector_printf_dangerous_max_127( instr_ctx, "Migrated program %s", FD_BASE58_ENC_32_ALLOCA( program_address ) );

      break;
    }
    default: {
      return FD_EXECUTOR_INSTR_ERR_INVALID_ACC_DATA;
    }
  }
  return FD_EXECUTOR_INSTR_SUCCESS;
}

/* process_instruction_inner() */
/* https://github.com/anza-xyz/agave/blob/77daab497df191ef485a7ad36ed291c1874596e5/programs/bpf_loader/src/lib.rs#L394-L564 */
int
fd_bpf_loader_program_execute( fd_exec_instr_ctx_t * ctx ) {
  FD_SPAD_FRAME_BEGIN( ctx->txn_ctx->spad ) {
    /* https://github.com/anza-xyz/agave/blob/77daab497df191ef485a7ad36ed291c1874596e5/programs/bpf_loader/src/lib.rs#L491-L529 */

    /* https://github.com/anza-xyz/agave/blob/v2.1.14/programs/bpf_loader/src/lib.rs#L403-L404 */
    fd_guarded_borrowed_account_t program_account;
    int err = fd_exec_instr_ctx_try_borrow_last_program_account( ctx, &program_account );
    if( FD_UNLIKELY( err ) ) {
      return err;
    }

    /* https://github.com/anza-xyz/agave/blob/v2.2.0/programs/bpf_loader/src/lib.rs#L409 */
    fd_pubkey_t const * program_id = NULL;
    err = fd_exec_instr_ctx_get_last_program_key( ctx, &program_id );
    if( FD_UNLIKELY( err ) ) {
      return err;
    }

    /* Program management instruction */
    if( FD_UNLIKELY( !memcmp( &fd_solana_native_loader_id, fd_borrowed_account_get_owner( &program_account ), sizeof(fd_pubkey_t) ) ) ) {
      /* https://github.com/anza-xyz/agave/blob/v2.2.3/programs/bpf_loader/src/lib.rs#L416 */
      fd_borrowed_account_drop( &program_account );

      if( FD_UNLIKELY( !memcmp( &fd_solana_bpf_loader_upgradeable_program_id, program_id, sizeof(fd_pubkey_t) ) ) ) {
        FD_EXEC_CU_UPDATE( ctx, UPGRADEABLE_LOADER_COMPUTE_UNITS );
        return process_loader_upgradeable_instruction( ctx );
      } else if( FD_UNLIKELY( !memcmp( &fd_solana_bpf_loader_program_id, program_id, sizeof(fd_pubkey_t) ) ) ) {
        FD_EXEC_CU_UPDATE( ctx, DEFAULT_LOADER_COMPUTE_UNITS );
        fd_log_collector_msg_literal( ctx, "BPF loader management instructions are no longer supported" );
        return FD_EXECUTOR_INSTR_ERR_UNSUPPORTED_PROGRAM_ID;
      } else if( FD_UNLIKELY( !memcmp( &fd_solana_bpf_loader_deprecated_program_id, program_id, sizeof(fd_pubkey_t) ) ) ) {
        FD_EXEC_CU_UPDATE( ctx, DEPRECATED_LOADER_COMPUTE_UNITS );
        fd_log_collector_msg_literal( ctx, "Deprecated loader is no longer supported" );
        return FD_EXECUTOR_INSTR_ERR_UNSUPPORTED_PROGRAM_ID;
      } else {
        fd_log_collector_msg_literal( ctx, "Invalid BPF loader id" );
        /* https://github.com/anza-xyz/agave/blob/89872fdb074e6658646b2b57a299984f0059cc84/programs/bpf_loader/src/lib.rs#L429-L436 */
        if( FD_FEATURE_ACTIVE_BANK( ctx->txn_ctx->bank, remove_accounts_executable_flag_checks ) ) {
          return FD_EXECUTOR_INSTR_ERR_UNSUPPORTED_PROGRAM_ID;
        }
        return FD_EXECUTOR_INSTR_ERR_INCORRECT_PROGRAM_ID;
      }
    }

    /* https://github.com/anza-xyz/agave/blob/89872fdb074e6658646b2b57a299984f0059cc84/programs/bpf_loader/src/lib.rs#L445-L452 */
    /* Program invocation. Any invalid programs will be caught here or at the program load. */
    if( FD_UNLIKELY( !FD_FEATURE_ACTIVE_BANK( ctx->txn_ctx->bank, remove_accounts_executable_flag_checks ) &&
                     !fd_borrowed_account_is_executable( &program_account ) ) ) {
      fd_log_collector_msg_literal( ctx, "Program is not executable" );
      return FD_EXECUTOR_INSTR_ERR_INCORRECT_PROGRAM_ID;
    }

    /* https://github.com/anza-xyz/agave/blob/77daab497df191ef485a7ad36ed291c1874596e5/programs/bpf_loader/src/lib.rs#L551-L563 */
    /* The Agave client stores a loaded program type state in its implementation
      of the loaded program cache. It checks to see if an account is able to be
      executed. It is possible for a program to be in the DelayVisibility state or
      Closed state but it won't be reflected in the Firedancer cache. Program
      accounts that are in this state should exit with an invalid account data
      error. For programs that are recently deployed or upgraded, they should not
      be allowed to be executed for the remainder of the slot. For closed
      accounts, they're uninitialized and shouldn't be executed as well.

      For the former case the slot that the
      program was last updated in is in the program data account.
      This means that if the slot in the program data account is greater than or
      equal to the current execution slot, then the account is in a
      'LoadedProgramType::DelayVisiblity' state.

      The latter case as described above is a tombstone account which is in a Closed
      state. This occurs when a program data account is closed. However, our cache
      does not track this. Instead, this can be checked for by seeing if the program
      account's respective program data account is uninitialized. This should only
      happen when the account is closed.

      Every error that comes out of this block is mapped to an InvalidAccountData instruction error in Agave. */

    fd_account_meta_t const * metadata = fd_borrowed_account_get_acc_meta( &program_account );
    uchar is_deprecated = !memcmp( metadata->info.owner, &fd_solana_bpf_loader_deprecated_program_id, sizeof(fd_pubkey_t) );

    if( !memcmp( metadata->info.owner, &fd_solana_bpf_loader_upgradeable_program_id, sizeof(fd_pubkey_t) ) ) {
      fd_bpf_upgradeable_loader_state_t * program_account_state = fd_bpf_loader_program_get_state( program_account.acct, ctx->txn_ctx->spad, &err );
      if( FD_UNLIKELY( err!=FD_BINCODE_SUCCESS ) ) {
        fd_log_collector_msg_literal( ctx, "Program is not deployed" );
        if( FD_FEATURE_ACTIVE_BANK( ctx->txn_ctx->bank, remove_accounts_executable_flag_checks ) ) {
          return FD_EXECUTOR_INSTR_ERR_UNSUPPORTED_PROGRAM_ID;
        }
        return FD_EXECUTOR_INSTR_ERR_INVALID_ACC_DATA;
      }

      /* https://github.com/anza-xyz/agave/blob/v2.0.9/svm/src/program_loader.rs#L96-L98
         Program account and program data account discriminants get checked when loading in program accounts
         into the program cache. If the discriminants are incorrect, the program is marked as closed. */
      if( FD_UNLIKELY( !fd_bpf_upgradeable_loader_state_is_program( program_account_state ) ) ) {
        fd_log_collector_msg_literal( ctx, "Program is not deployed" );
        if( FD_FEATURE_ACTIVE_BANK( ctx->txn_ctx->bank, remove_accounts_executable_flag_checks ) ) {
          return FD_EXECUTOR_INSTR_ERR_UNSUPPORTED_PROGRAM_ID;
        }
        return FD_EXECUTOR_INSTR_ERR_INVALID_ACC_DATA;
      }

      fd_txn_account_t * program_data_account = NULL;
      fd_pubkey_t *      programdata_pubkey   = (fd_pubkey_t *)&program_account_state->inner.program.programdata_address;
      err = fd_exec_txn_ctx_get_executable_account( ctx->txn_ctx,
                                                    programdata_pubkey,
                                                    &program_data_account,
                                                    fd_txn_account_check_exists );
      if( FD_UNLIKELY( err!=FD_ACC_MGR_SUCCESS ) ) {
        fd_log_collector_msg_literal( ctx, "Program is not deployed" );
        if( FD_FEATURE_ACTIVE_BANK( ctx->txn_ctx->bank, remove_accounts_executable_flag_checks ) ) {
          return FD_EXECUTOR_INSTR_ERR_UNSUPPORTED_PROGRAM_ID;
        }
        return FD_EXECUTOR_INSTR_ERR_INVALID_ACC_DATA;
      }

      if( FD_UNLIKELY( program_data_account->vt->get_data_len( program_data_account )<PROGRAMDATA_METADATA_SIZE ) ) {
        fd_log_collector_msg_literal( ctx, "Program is not deployed" );
        if( FD_FEATURE_ACTIVE_BANK( ctx->txn_ctx->bank, remove_accounts_executable_flag_checks ) ) {
          return FD_EXECUTOR_INSTR_ERR_UNSUPPORTED_PROGRAM_ID;
        }
        return FD_EXECUTOR_INSTR_ERR_INVALID_ACC_DATA;
      }

      fd_bpf_upgradeable_loader_state_t * program_data_account_state = fd_bpf_loader_program_get_state( program_data_account,
                                                                                                        ctx->txn_ctx->spad,
                                                                                                        &err );
      if( FD_UNLIKELY( err!=FD_BINCODE_SUCCESS ) ) {
        fd_log_collector_msg_literal( ctx, "Program is not deployed" );
        if( FD_FEATURE_ACTIVE_BANK( ctx->txn_ctx->bank, remove_accounts_executable_flag_checks ) ) {
          return FD_EXECUTOR_INSTR_ERR_UNSUPPORTED_PROGRAM_ID;
        }
        return FD_EXECUTOR_INSTR_ERR_INVALID_ACC_DATA;
      }

      /* https://github.com/anza-xyz/agave/blob/v2.0.9/svm/src/program_loader.rs#L100-L104
         Same as above comment. Program data discriminant must be set correctly. */
      if( FD_UNLIKELY( !fd_bpf_upgradeable_loader_state_is_program_data( program_data_account_state ) ) ) {
        /* The account is closed. */
        fd_log_collector_msg_literal( ctx, "Program is not deployed" );
        if( FD_FEATURE_ACTIVE_BANK( ctx->txn_ctx->bank, remove_accounts_executable_flag_checks ) ) {
          return FD_EXECUTOR_INSTR_ERR_UNSUPPORTED_PROGRAM_ID;
        }
        return FD_EXECUTOR_INSTR_ERR_INVALID_ACC_DATA;
      }

      ulong program_data_slot = program_data_account_state->inner.program_data.slot;
      if( FD_UNLIKELY( program_data_slot>=ctx->txn_ctx->slot ) ) {
        /* The account was likely just deployed or upgraded. Corresponds to
          'LoadedProgramType::DelayVisibility' */
        fd_log_collector_msg_literal( ctx, "Program is not deployed" );
        if( FD_FEATURE_ACTIVE_BANK( ctx->txn_ctx->bank, remove_accounts_executable_flag_checks ) ) {
          return FD_EXECUTOR_INSTR_ERR_UNSUPPORTED_PROGRAM_ID;
        }
        return FD_EXECUTOR_INSTR_ERR_INVALID_ACC_DATA;
      }
    }

    /* Sadly, we have to tie the cache in with consensus. We tried our best to avoid this,
       but Agave's program loading logic is too complex to solely rely on checks without
       significant redundancy.

       For example, devnet and testnet have older programs that were deployed before stricter ELF / VM validation
       checks were put in place, causing these older programs to fail newer validation checks and
       be unexecutable. `fd_bpf_scan_and_create_bpf_program_cache_entry()` will populate our BPF program
       cache correctly, but now, we have no way of checking if this validation passed or not here without
       querying our program cache, otherwise we would have to copy-paste our validation checks here.

       Any failures here would indicate an attempt to interact with a deployed programs that either failed
       to load or failed bytecode verification. This applies for v1, v2, and v3 programs. This could
       also theoretically cause some currently-deployed programs to fail in the future if ELF / VM checks
       are eventually made stricter.

       TLDR: A program is present in the BPF cache iff it is already deployed AND passes current SBPF and VM checks.
       Only then it is considered valid to interact with. */
    fd_sbpf_validated_program_t const * prog = NULL;
    if( FD_UNLIKELY( fd_bpf_load_cache_entry( ctx->txn_ctx->funk,
                                              ctx->txn_ctx->funk_txn,
                                              program_id,
                                              &prog )!=0 ) ) {
      fd_log_collector_msg_literal( ctx, "Program is not cached" );

      /* https://github.com/anza-xyz/agave/blob/89872fdb074e6658646b2b57a299984f0059cc84/programs/bpf_loader/src/lib.rs#L460-L467 */
      if( FD_FEATURE_ACTIVE_BANK( ctx->txn_ctx->bank, remove_accounts_executable_flag_checks ) ) {
        return FD_EXECUTOR_INSTR_ERR_UNSUPPORTED_PROGRAM_ID;
      }
      return FD_EXECUTOR_INSTR_ERR_INVALID_ACC_DATA;
    }

    /* The program may be in the cache but could have failed verification in the current epoch. */
    if( FD_UNLIKELY( prog->failed_verification ) ) {
      fd_log_collector_msg_literal( ctx, "Program is not deployed" );
      if( FD_FEATURE_ACTIVE_BANK( ctx->txn_ctx->bank, remove_accounts_executable_flag_checks ) ) {
        return FD_EXECUTOR_INSTR_ERR_UNSUPPORTED_PROGRAM_ID;
      }
      return FD_EXECUTOR_INSTR_ERR_INVALID_ACC_DATA;
    }

    /* https://github.com/anza-xyz/agave/blob/v2.1.14/programs/bpf_loader/src/lib.rs#L446 */
    fd_borrowed_account_drop( &program_account );

    return fd_bpf_execute( ctx, prog, is_deprecated );
  } FD_SPAD_FRAME_END;
}


/* Public APIs */

int
fd_directly_invoke_loader_v3_deploy( fd_exec_slot_ctx_t * slot_ctx,
                                     uchar const *        elf,
                                     ulong                elf_sz,
                                     fd_spad_t *          runtime_spad ) {
  /* Set up a dummy instr and txn context */
  fd_exec_txn_ctx_t * txn_ctx            = fd_exec_txn_ctx_join( fd_exec_txn_ctx_new( fd_spad_alloc( runtime_spad, FD_EXEC_TXN_CTX_ALIGN, FD_EXEC_TXN_CTX_FOOTPRINT ) ), runtime_spad, fd_wksp_containing( runtime_spad ) );
  fd_funk_t *         funk               = slot_ctx->funk;
  fd_wksp_t *         funk_wksp          = fd_funk_wksp( funk );
  fd_wksp_t *         runtime_wksp       = fd_wksp_containing( slot_ctx );
  ulong               funk_txn_gaddr     = fd_wksp_gaddr( funk_wksp, slot_ctx->funk_txn );
  ulong               funk_gaddr         = fd_wksp_gaddr( funk_wksp, funk->shmem );

  fd_exec_txn_ctx_from_exec_slot_ctx( slot_ctx,
                                      txn_ctx,
                                      funk_wksp,
                                      runtime_wksp,
                                      funk_txn_gaddr,
                                      funk_gaddr,
                                      NULL );

  fd_exec_txn_ctx_setup_basic( txn_ctx );
  txn_ctx->instr_stack_sz = 1;
  fd_exec_instr_ctx_t * instr_ctx = &txn_ctx->instr_stack[0];
  *instr_ctx = (fd_exec_instr_ctx_t) {
    .instr     = NULL,
    .txn_ctx   = txn_ctx,
  };

  return fd_deploy_program( instr_ctx, elf, elf_sz, runtime_spad );
}
