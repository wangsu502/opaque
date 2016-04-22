#include "Filter.h"

int ecall_filter_single_row(int op_code, uint8_t *row, uint32_t length) {

  // row is in format of [num cols][enc_attr1 size][enc_attr1][enc_attr2 size][enc_attr2] ...
  // enc_attr's format is [type][len of plaintext attr][plaintext attr]
  // num cols is 4 bytes; size is also 4 bytes
  int ret = 1;

  uint32_t num_cols = get_num_col(row);
  if (num_cols == 0) {
    printf("Number of columns: 0\n");    
    return 0;
  }

  // printf("Number of columns: %u\n", num_cols);

  uint8_t *row_ptr = row + 4;

  const size_t decrypted_data_len = 1024;
  uint8_t decrypted_data[decrypted_data_len];


  uint8_t *enc_value_ptr = NULL;
  uint32_t enc_value_len = 0;

  uint8_t attr_type = 0;
  uint32_t attr_len = 0;
  uint8_t *attr_ptr = NULL;

  if (op_code == 0) {
    // find the second attribute
		
    // row_ptr = get_enc_attr(&enc_attr_ptr, &enc_attr_len, row_ptr, row, length);
    // row_ptr = get_enc_attr(&enc_attr_ptr, &enc_attr_len, row_ptr, row, length);

	find_attribute(row_ptr, length, num_cols,
				   2,
				   &enc_value_ptr, &enc_value_len);

	decrypt(enc_value_ptr, enc_value_len, decrypted_data);

	get_attr(decrypted_data, &attr_type, &attr_len, &attr_ptr);
	

	// since op_code is 0, number should be "integer"
    int *value_ptr = (int *) attr_ptr;

    if (*value_ptr <= 3) {
      ret = 0;
    }

  } else if (op_code == 2) {
    // Filter out rows with a dummy attribute in the 4th column. Such rows represent partial aggregates
    find_attribute(row_ptr, length, num_cols, 4, &enc_value_ptr, &enc_value_len);
    decrypt(enc_value_ptr, enc_value_len, decrypted_data);
    get_attr(decrypted_data, &attr_type, &attr_len, &attr_ptr);

    if (attr_type == DUMMY) {
      ret = 0;
    }

  } else if (op_code == -1) {
    // this is for test only

	find_attribute(row_ptr, length, num_cols,
				   1,
				   &enc_value_ptr, &enc_value_len);
	decrypt(enc_value_ptr, enc_value_len, decrypted_data);
	get_attr(decrypted_data, &attr_type, &attr_len, &attr_ptr);
    
    int *value_ptr = (int *) attr_ptr;

    printf("Input value is %u\n", *value_ptr);
    printf("Attr len is  is %u\n", attr_len);
	
    ret = 0;
  } else {
    printf("Error in ecall_filter_single_row: unexpected op code %d\n", op_code);
    assert(false);
  }

  return ret;
}