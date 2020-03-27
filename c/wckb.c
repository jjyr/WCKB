/* WCKB type script
 * WCKB is an extended UDT,
 * support transfer WCKB token while the native CKB is locked in NervosDAO,
 * WCKB owner can withdraw native CKB and interests from NervosDAO by destroy
 * corresponded WCKB.
 *
 * WCKB format:
 * tokens(16 bytes) | height(8 bytes)
 * > 16 bytes u128 number to store the TOKEN.
 * > 8 bytes u64 number to store the block number.
 *
 * Align block number:
 * Update the height of WCKB to a higher block number, and update the amount by
 * apply NervosDAO formula. All WCKB cell will align to the first input WCKB's
 * block number, so the first input WCKB must has higher block number than
 * others.
 *
 * Verification:
 * This type script make sure the equation between inputs and outputs(all coins
 * are aligned):
 * 1. inputs WCKB - withdraw NervosDAO == outputs WCKB
 * 2. uninited WCKB == deposited NervosDAO
 * 3. all outputs WCKB's block number must align to aligned block number
 *
 * Get WCKB:
 * 1. send a NervosDAO deposition request
 * 2. put a output in the same tx to create corresponded WCKB
 * 3. the height should set to 0
 *
 * Transfer WCKB:
 * 1. The first WCKB input must has highest block number compares to other
 * inputs.
 * 2. Outputs WCKB must aligned to this number.
 * 3. verify inputs amount is equals to outputs amount (after aligned).
 *
 * Withdraw WCKB:
 * 1. Perform NervosDAO withdraw phase 1.
 * 2. Prepare a WCKB input that has enough coins to cover the withdraw CKB
 * coins.
 * 3. Put a withdrawed output.
 *
 */

#include "blake2b.h"
#include "ckb_syscalls.h"
#include "common.h"
#include "dao_utils.h"
#include "defs.h"
#include "overflow_add.h"
#include "protocol.h"

#define BLAKE2B_BLOCK_SIZE 32
#define SCRIPT_SIZE 32768
#define BLOCK_NUM_LEN 8
#define CKB_LEN 8
#define UDT_LEN 16
#define MAX_HEADER_SIZE 32768
#define MAX_SWAPS 256

const uint8_t NERVOS_DAO_TYPE_HASH[] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

typedef struct {
  unsigned char lock_hash[BLAKE2B_BLOCK_SIZE];
  uint128_t amount;
} SwapInfo;

typedef struct {
  uint64_t block_number;
  uint128_t amount;
} TokenInfo;

int is_dao_deposit_cell(unsigned char *cell_type_hash, uint8_t *data,
                        uint64_t data_len) {
  static uint8_t empty[] = {0, 0, 0, 0, 0, 0, 0, 0};
  return (memcmp(cell_type_hash, NERVOS_DAO_TYPE_HASH, BLAKE2B_BLOCK_SIZE) ==
          0) &&
         (data_len == BLOCK_NUM_LEN) &&
         (memcmp(data, empty, BLOCK_NUM_LEN) == 0);
}

int is_dao_withdraw1_cell(unsigned char *cell_type_hash, uint8_t *data,
                          uint64_t data_len) {
  static uint8_t empty[] = {0, 0, 0, 0, 0, 0, 0, 0};
  return (memcmp(cell_type_hash, NERVOS_DAO_TYPE_HASH, BLAKE2B_BLOCK_SIZE) ==
          0) &&
         (data_len == BLOCK_NUM_LEN) &&
         (memcmp(data, empty, BLOCK_NUM_LEN) != 0);
}

/* check inputs, return input WCKB */
int fetch_inputs(unsigned char *type_hash, int *withdraw_dao_cnt,
                 TokenInfo withdraw_dao_infos[MAX_SWAPS], int *input_wckb_cnt,
                 TokenInfo input_wckb_infos[MAX_SWAPS]) {

  *withdraw_dao_cnt = 0;
  *input_wckb_cnt = 0;
  int i = 0;
  int ret;
  uint64_t len;
  while (1) {
    unsigned char input_type_hash[BLAKE2B_BLOCK_SIZE];
    len = BLAKE2B_BLOCK_SIZE;
    ret = ckb_checked_load_cell_by_field(input_type_hash, &len, 0, i,
                                         CKB_SOURCE_INPUT,
                                         CKB_CELL_FIELD_TYPE_HASH);
    if (ret == CKB_INDEX_OUT_OF_BOUND) {
      break;
    }
    if (ret == CKB_ITEM_MISSING) {
      i++;
      continue;
    }
    if (ret != CKB_SUCCESS || len != BLAKE2B_BLOCK_SIZE) {
      return ERROR_ENCODING;
    }
    uint8_t buf[UDT_LEN + BLOCK_NUM_LEN];
    len = UDT_LEN + BLOCK_NUM_LEN;
    ret = ckb_load_cell_data(buf, &len, 0, i, CKB_SOURCE_INPUT);
    if (ret == CKB_ITEM_MISSING) {
      i++;
      continue;
    }
    if (ret != CKB_SUCCESS || len > UDT_LEN + BLOCK_NUM_LEN) {
      return ERROR_ENCODING;
    }
    if (is_dao_withdraw1_cell(input_type_hash, buf, len)) {
      /* withdraw NervosDAO */
      uint64_t deposited_block_number = *(uint64_t *)buf;
      size_t deposit_index;
      ret = extract_deposit_header_index(i, &deposit_index);
      if (ret != CKB_SUCCESS) {
        return ret;
      }
      /* calculate withdraw amount */
      dao_header_data_t deposit_data;
      load_dao_header_data(deposit_index, CKB_SOURCE_HEADER_DEP, &deposit_data);
      dao_header_data_t withdraw_data;
      load_dao_header_data(i, CKB_SOURCE_INPUT, &withdraw_data);
      uint64_t occupied_capacity;
      len = CKB_LEN;
      ret = ckb_checked_load_cell_by_field((uint8_t *)&occupied_capacity, &len,
                                           0, i, CKB_SOURCE_INPUT,
                                           CKB_CELL_FIELD_OCCUPIED_CAPACITY);
      if (ret != CKB_SUCCESS || len != CKB_LEN) {
        return ERROR_ENCODING;
      }
      len = CKB_LEN;
      uint64_t original_capacity;
      ret = ckb_checked_load_cell_by_field((uint8_t *)&original_capacity, &len,
                                           0, i, CKB_SOURCE_INPUT,
                                           CKB_CELL_FIELD_CAPACITY);
      if (ret != CKB_SUCCESS || len != CKB_LEN) {
        return ERROR_ENCODING;
      }
      uint64_t calculated_capacity = 0;
      calculate_dao_input_capacity(occupied_capacity, deposit_data,
                                   withdraw_data, deposited_block_number,
                                   original_capacity, &calculated_capacity);
      /* record withdraw amount */
      int j = *withdraw_dao_cnt;
      *withdraw_dao_cnt += 1;
      withdraw_dao_infos[j].amount = calculated_capacity;
      withdraw_dao_infos[j].block_number = withdraw_data.block_number;
    } else if (memcmp(input_type_hash, type_hash, BLAKE2B_BLOCK_SIZE) == 0) {
      /* WCKB */
      uint128_t amount;
      uint64_t block_number;
      if (len != UDT_LEN) {
        return ERROR_ENCODING;
      }
      amount = *(uint128_t *)buf;
      block_number = *(uint64_t *)(buf + UDT_LEN);
      /* record input amount */
      int j = *input_wckb_cnt;
      *input_wckb_cnt += 1;
      input_wckb_infos[j].amount = amount;
      input_wckb_infos[j].block_number = block_number;
    }
    i++;
  }
  return CKB_SUCCESS;
}

/* return index of swap_infos, return -1 if not found */
int find_swap_by_lock_hash(SwapInfo *swap_infos, int swap_infos_cnt,
                           unsigned char *lock_hash) {
  for (int j = 0; j < swap_infos_cnt; j++) {
    int ret = memcmp(swap_infos[j].lock_hash, lock_hash, BLAKE2B_BLOCK_SIZE);
    if (ret == 0) {
      return j;
    }
  }
  return -1;
}

/* return index of token_infos, return -1 if not found */
int find_token_by_block_number(TokenInfo *token_infos, int token_infos_cnt,
                               uint64_t block_number) {
  for (int j = 0; j < token_infos_cnt; j++) {
    if (token_infos[j].block_number == block_number) {
      return j;
    }
  }
  return -1;
}

/* check outputs WCKB
 * 1. check uninitialized(height is 0) WCKB that mapping to DAO
 * 2. check initialized(height > 0) WCKB that equals to inputs
 */
int fetch_outputs(unsigned char *wckb_type_hash, uint64_t align_block_number,
                  int *deposited_dao_cnt, SwapInfo deposited_dao[MAX_SWAPS],
                  int *uninitialized_wckb_cnt,
                  SwapInfo uninitialized_wckb[MAX_SWAPS],
                  int *initialized_wckb_cnt,
                  TokenInfo initialized_wckb[MAX_SWAPS]) {
  *deposited_dao_cnt = 0;
  *uninitialized_wckb_cnt = 0;
  *initialized_wckb_cnt = 0;
  int ret;
  /* iterate all outputs */
  int i = 0;
  while (1) {
    unsigned char output_type_hash[BLAKE2B_BLOCK_SIZE];
    uint64_t len = BLAKE2B_BLOCK_SIZE;
    ret = ckb_checked_load_cell_by_field(output_type_hash, &len, 0, i,
                                         CKB_SOURCE_OUTPUT,
                                         CKB_CELL_FIELD_TYPE_HASH);
    if (ret == CKB_INDEX_OUT_OF_BOUND) {
      break;
    }
    if (ret == CKB_ITEM_MISSING) {
      i++;
      continue;
    }
    if (ret != CKB_SUCCESS || len != BLAKE2B_BLOCK_SIZE) {
      return ERROR_ENCODING;
    }
    len = BLOCK_NUM_LEN + UDT_LEN;
    uint8_t buf[BLOCK_NUM_LEN + UDT_LEN];
    ret = ckb_load_cell_data(buf, &len, 0, i, CKB_SOURCE_OUTPUT);
    if (ret == CKB_ITEM_MISSING) {
      i++;
      continue;
    }
    if (ret != CKB_SUCCESS || len > (UDT_LEN + BLOCK_NUM_LEN)) {
      return ERROR_ENCODING;
    }
    if (is_dao_deposit_cell(output_type_hash, buf, len)) {
      /* check deposited dao cell */
      uint64_t amount;
      unsigned char lock_hash[BLAKE2B_BLOCK_SIZE];
      len = BLAKE2B_BLOCK_SIZE;
      ret = ckb_checked_load_cell_by_field(
          lock_hash, &len, 0, i, CKB_SOURCE_OUTPUT, CKB_CELL_FIELD_LOCK_HASH);
      if (ret == CKB_INDEX_OUT_OF_BOUND) {
        break;
      }
      if (ret != CKB_SUCCESS || len != BLAKE2B_BLOCK_SIZE) {
        return ERROR_SYSCALL;
      }
      len = CKB_LEN;
      ret = ckb_checked_load_cell_by_field(
          &amount, &len, 0, i, CKB_SOURCE_OUTPUT, CKB_CELL_FIELD_CAPACITY);
      if (ret != CKB_SUCCESS || len != CKB_LEN) {
        return ERROR_SYSCALL;
      }
      /* record deposited dao amount */
      int found =
          find_swap_by_lock_hash(deposited_dao, *deposited_dao_cnt, lock_hash);
      if (found >= 0) {
        /* found it */
        deposited_dao[found].amount += amount;
      } else {
        /* initialize new instance */
        if (*deposited_dao_cnt >= MAX_SWAPS) {
          return ERROR_TOO_MANY_SWAPS;
        }
        int new_i = *deposited_dao_cnt;
        deposited_dao_cnt++;
        deposited_dao[new_i].amount = amount;
        memcpy(deposited_dao[new_i].lock_hash, lock_hash, BLAKE2B_BLOCK_SIZE);
      }
    } else if (memcmp(output_type_hash, wckb_type_hash, BLAKE2B_BLOCK_SIZE) ==
               0) {
      /* check wckb cell */
      /* read wckb info */
      uint128_t amount;
      uint64_t block_number;
      if (len != (UDT_LEN + BLOCK_NUM_LEN)) {
        return ERROR_ENCODING;
      }
      amount = *(uint128_t *)buf;
      block_number = *(uint64_t *)(buf + UDT_LEN);
      if (block_number == 0) {
        /* wckb is unitialized, record the amount */
        unsigned char lock_hash[BLAKE2B_BLOCK_SIZE];
        len = BLAKE2B_BLOCK_SIZE;
        ret = ckb_checked_load_cell_by_field(
            lock_hash, &len, 0, i, CKB_SOURCE_OUTPUT, CKB_CELL_FIELD_LOCK_HASH);
        if (ret == CKB_INDEX_OUT_OF_BOUND) {
          break;
        }
        if (ret != CKB_SUCCESS || len != BLAKE2B_BLOCK_SIZE) {
          return ERROR_SYSCALL;
        }
        int found = find_swap_by_lock_hash(uninitialized_wckb,
                                           *uninitialized_wckb_cnt, lock_hash);
        if (found >= 0) {
          /* found it */
          uninitialized_wckb[found].amount += amount;
        } else {
          /* initialize new instance */
          if (*uninitialized_wckb_cnt >= MAX_SWAPS) {
            return ERROR_TOO_MANY_SWAPS;
          }
          int new_i = *uninitialized_wckb_cnt;
          *uninitialized_wckb_cnt += 1;
          uninitialized_wckb[new_i].amount = amount;
          memcpy(uninitialized_wckb[new_i].lock_hash, lock_hash,
                 BLAKE2B_BLOCK_SIZE);
        }
      } else {
        /* wckb is initialized */
        if (block_number != align_block_number) {
          return ERROR_OUTPUT_ALIGN;
        }
        int found = find_token_by_block_number(
            initialized_wckb, *initialized_wckb_cnt, block_number);
        if (found >= 0) {
          /* found it */
          initialized_wckb[found].amount += amount;
        } else {
          /* initialize new instance */
          if (*initialized_wckb_cnt >= MAX_SWAPS) {
            return ERROR_TOO_MANY_SWAPS;
          }
          int new_i = *initialized_wckb_cnt;
          *initialized_wckb_cnt += 1;
          initialized_wckb[new_i].amount = amount;
          initialized_wckb[new_i].block_number = block_number;
        }
      }
    }
    i++;
  }
  return CKB_SUCCESS;
}

int align_dao(size_t i, size_t source, dao_header_data_t align_target_data,
              uint64_t deposited_block_number, uint64_t original_capacity,
              uint64_t *calculated_capacity) {
  if (align_target_data.block_number == deposited_block_number) {
    original_capacity = *calculated_capacity;
    return CKB_SUCCESS;
  }

  if (align_target_data.block_number < deposited_block_number) {
    return ERROR_ALIGN;
  }
  uint64_t occupied_capacity;
  uint64_t len = CKB_LEN;
  int ret =
      ckb_checked_load_cell_by_field((uint8_t *)&occupied_capacity, &len, 0, i,
                                     source, CKB_CELL_FIELD_OCCUPIED_CAPACITY);
  if (ret != CKB_SUCCESS || len != CKB_LEN) {
    return ERROR_ENCODING;
  }
  dao_header_data_t deposit_data;
  ret = load_dao_header_data(i, source, &deposit_data);
  if (ret != CKB_SUCCESS) {
    return ret;
  }
  /* uninitialized wckb */
  if (deposited_block_number == 0) {
    deposited_block_number = deposit_data.block_number;
  }
  return calculate_dao_input_capacity(occupied_capacity, deposit_data,
                                      align_target_data, deposited_block_number,
                                      original_capacity, calculated_capacity);
}

int main() {
  int ret;
  unsigned char type_hash[BLAKE2B_BLOCK_SIZE];
  uint64_t len = BLAKE2B_BLOCK_SIZE;
  /* load self type hash */
  ret = ckb_load_script_hash(type_hash, &len, 0);
  if (ret != CKB_SUCCESS || len != BLAKE2B_BLOCK_SIZE) {
    return ERROR_SYSCALL;
  }
  /* load aligned target header */
  dao_header_data_t align_target_data;
  ret = load_dao_header_data(0, CKB_SOURCE_GROUP_INPUT, &align_target_data);
  if (ret != CKB_SUCCESS) {
    return ERROR_ENCODING;
  }

  /* fetch inputs */
  TokenInfo withdraw_dao_infos[MAX_SWAPS];
  int withdraw_dao_cnt;
  TokenInfo input_wckb_infos[MAX_SWAPS];
  int input_wckb_cnt;
  ret = fetch_inputs(type_hash, &withdraw_dao_cnt, withdraw_dao_infos,
                     &input_wckb_cnt, input_wckb_infos);
  if (ret != CKB_SUCCESS) {
    return ret;
  }
  /* fetch outputs */
  int deposited_dao_cnt = 0;
  SwapInfo deposited_dao[MAX_SWAPS];
  int output_uninit_wckb_cnt = 0;
  SwapInfo output_uninit_wckb[MAX_SWAPS];
  int output_inited_wckb_cnt = 0;
  TokenInfo output_inited_wckb[MAX_SWAPS];
  ret = fetch_outputs(type_hash, align_target_data.block_number,
                      &deposited_dao_cnt, deposited_dao,
                      &output_uninit_wckb_cnt, output_uninit_wckb,
                      &output_inited_wckb_cnt, output_inited_wckb);
  if (ret != CKB_SUCCESS) {
    return ret;
  }
  /* check equations
   * 1. inputs WCKB - withdraw NervosDAO == outputs WCKB
   * 2. uninited WCKB == deposited NervosDAO
   */
  uint64_t calculated_capacity;
  uint64_t total_withdraw_dao = 0;
  for (int i = 0; i < withdraw_dao_cnt; i++) {
    ret = align_dao(i, CKB_SOURCE_INPUT, align_target_data,
                    withdraw_dao_infos[i].block_number,
                    withdraw_dao_infos[i].amount, &calculated_capacity);
    if (ret != CKB_SUCCESS) {
      return ret;
    }
    total_withdraw_dao += calculated_capacity;
  }

  uint64_t total_input_wckb = 0;
  for (int i = 0; i < input_wckb_cnt; i++) {
    ret = align_dao(i, CKB_SOURCE_INPUT, align_target_data,
                    input_wckb_infos[i].block_number,
                    input_wckb_infos[i].amount, &calculated_capacity);
    if (ret != CKB_SUCCESS) {
      return ret;
    }
    total_input_wckb += calculated_capacity;
  }

  uint64_t total_output_wckb = 0;
  for (int i = 0; i < output_inited_wckb_cnt; i++) {
    ret = align_dao(i, CKB_SOURCE_OUTPUT, align_target_data,
                    output_inited_wckb[i].block_number,
                    output_inited_wckb[i].amount, &calculated_capacity);
    if (ret != CKB_SUCCESS) {
      return ret;
    }
    total_output_wckb += calculated_capacity;
  }

  /* 1. inputs WCKB - withdraw NervosDAO == outputs WCKB */
  if (!(total_input_wckb - total_withdraw_dao == total_output_wckb)) {
    return ERROR_INCORRECT_OUTPUT_WCKB;
  }

  /* 2. uninited WCKB == deposited NervosDAO */

  for (int i = 0; i < output_uninit_wckb_cnt; i++) {
    int found = find_swap_by_lock_hash(deposited_dao, deposited_dao_cnt,
                                       output_uninit_wckb[i].lock_hash);
    if (found < 0) {
      return ERROR_INCORRECT_UNINIT_OUTPUT_WCKB;
    }
    if (output_uninit_wckb[i].amount != deposited_dao[found].amount) {
      return ERROR_INCORRECT_UNINIT_OUTPUT_WCKB;
    }
  }

  return CKB_SUCCESS;
}
