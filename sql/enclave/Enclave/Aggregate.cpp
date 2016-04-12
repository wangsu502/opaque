#include <stdarg.h>
#include <stdio.h>      /* vsnprintf */
#include <stdint.h>

#include "sgx_trts.h"
#include "math.h"

#include "Aggregate.h"

// scan algorithm
// 1. Global column sort

// 2. Oblivious count-distinct for padding, plus calculate the global offset for
//    each distinct element

//    For global count-distinct, compute local count-distinct, plus first and last
//    elements in each partition. The coordinator uses these to compensate for
//    runs of records that span partition boundaries.

//    Compact numbering can be calculated from this result.

//    Also track the partial sum for the last record of each partition

// 3. If the number of distinct elements is greater than a threshold (e.g., the
//    partition size), use high-cardinality algorithm.

// 4. Map-side aggregate + pad

//    Use the compact numbering from above to ensure the partial aggregrates are
//    output in sorted order and at the correct offset

// 5. Colocate matching partial aggregates using any group-by operator (not
//    necessarily oblivious)

// 6. Reduce-side final aggregate



// TODO: should we set all aggregate statistics to be the AGG_UPPER_BOUND?
// TODO: change the [in] pointers to [user_check]


// mode indicates whether we're in low cardinality or high cardinality situation
// 1: scan write (low)
// 2: global sort (high)
int mode = 1;

class aggregate_data {
 public:
  aggregate_data() {

  }
  virtual void reset() {}
  virtual void agg(uint8_t data_type, uint8_t *data, uint32_t data_len) {}
  virtual uint32_t ret_length() {}
  virtual void ret_result(uint8_t *result) {}
  virtual void ret_dummy_result(uint8_t *result) {}
  virtual void ret_result_print() {}

  virtual void copy_data(aggregate_data *data) {}
};

class aggregate_data_sum : public aggregate_data {

 public:
  aggregate_data_sum() {
	sum = 0;
  }

  void reset() {
	sum = 0;
  }

  void agg(uint8_t data_type, uint8_t *data, uint32_t data_len) {
	switch(data_type) {

 	case 1: // int
	  {
		uint32_t *int_data_ptr = NULL;
		assert(data_len == 4);
		int_data_ptr = (uint32_t *) data;
		sum += *int_data_ptr;
	  }
	  break;
	default: // cannot handle sum of other types!
	  {
		assert(false);
	  }
	  break;
	}
  }

  uint32_t ret_length() {
	// 1 for type, 4 for length, 4 for data
	return (1 + 4 + 4);
  }

  // serialize the result directly into the result buffer
  void ret_result(uint8_t *result) {
	uint8_t *result_ptr = result;
	*result_ptr = 1;
	result_ptr += 1;
	*( (uint32_t *) result_ptr) = 4;
	result_ptr += 4;
	uint32_t cur_value = *( (uint32_t *) result_ptr);
	*( (uint32_t *) result_ptr) = cur_value + sum - cur_value;
  }

  // TODO: is this oblivious?
  void ret_dummy_result(uint8_t *result) {
	uint8_t *result_ptr = result;
	*result_ptr = *result_ptr;
	result_ptr += 1;
	*( (uint32_t *) result_ptr) = 4;
	result_ptr += 4;
	uint32_t cur_value = *( (uint32_t *) result_ptr);
	*( (uint32_t *) result_ptr) = cur_value + (sum - sum);
  }

  void ret_result_print() {
	printf("Current result is %u\n", sum);
  }

  void copy_data(aggregate_data *data) {

	// only copy if the data is of the same type
	if (dynamic_cast<aggregate_data_sum *>(data) != NULL) {
	  this->sum = dynamic_cast<aggregate_data_sum *> (data)->sum;
	}

  }

  uint32_t sum;
};

class aggregate_data_count : public aggregate_data {

  aggregate_data_count() {
	count = 0;
  }
  
  void agg(uint8_t data_type, uint8_t *data, uint32_t data_len) {
	count += 1;
  }
  
  uint32_t ret_length() {
	return (1 + 4 + 4);
  }
  
  void ret_result(uint8_t *result) {
	uint8_t *result_ptr = result;
	*result_ptr = 1;
	result_ptr += 1;
	*( (uint32_t *) result_ptr) = 4;
	result_ptr += 4;
	*( (uint32_t *) result_ptr) = count;
  }

  uint32_t count;
};


class aggregate_data_avg : public aggregate_data {

  aggregate_data_avg() {
	count = 0;
  }
  
  void agg(uint8_t data_type, uint8_t *data, uint32_t data_len) {

	switch(data_type) {
	case 1: // int
	  {
		uint32_t *int_data_ptr = NULL;
		assert(data_len == 4);
		int_data_ptr = (uint32_t *) data;
		sum += *int_data_ptr;
	  }
	  break;
	default: // cannot handle sum of other types!
	  {
		assert(false);
	  }
	  break;
	}

	count += 1;
  }
  
  uint32_t ret_length() {
	return (1 + 4 + 8);
  }
  
  void ret_result(uint8_t *result) {
	uint8_t *result_ptr = result;
	*result_ptr = 1;
	result_ptr += 1;
	*( (uint32_t *) result_ptr) = 8;
	result_ptr += 4;

	double avg = ((double) sum) / ((double) count);
	*( (double *) result_ptr) = avg;
  }

  uint64_t sum;
  uint64_t count;
};

class agg_stats_data {

public:
  agg_stats_data(int op_code) {
	//buffer = (uint8_t *) malloc(AGG_UPPER_BOUND);
	
	distinct_entries = (uint32_t *) buffer;
	*distinct_entries = 0;
	offset_ptr = (uint32_t *) (buffer + 4);
	*offset_ptr = 0;
	
	sort_attr = buffer + 8;
	agg = sort_attr + ROW_UPPER_BOUND;

	assert(agg < buffer + AGG_UPPER_BOUND);
	
	if (op_code == 1) {
	  agg_data = new aggregate_data_sum;
	} else {
	  agg_data = NULL;
	  assert(false);
	}
  }

  ~agg_stats_data() {
	delete agg_data;
  }

  void inc_distinct() {
	*distinct_entries += 1;
  }

  uint32_t distinct() {
	return *distinct_entries;
  }

  void set_distinct(uint32_t distinct) {
	*distinct_entries = distinct;
  }


  uint32_t offset() {
	return *offset_ptr;
  }

  void set_offset(uint32_t offset_) {
	*offset_ptr = offset_;
  }

  void set_sort_attr(uint8_t *in_sort_attr, uint32_t in_sort_attr_len) {
	cpy(sort_attr, in_sort_attr, in_sort_attr_len);
  }

  uint32_t sort_attr_len() {
	uint32_t *ptr = (uint32_t *) (sort_attr + 1);
	return *ptr;
  }

  uint32_t agg_attr_len() {
	uint32_t *ptr = (uint32_t *) (agg + 1);
	return *ptr;
  }

  // this assumes that the appropriate information has been copied into agg
  void aggregate() {
	agg_data->agg(*agg, agg+1+this->agg_attr_len(), this->agg_attr_len());
  }

  // aggregate data from another 
  void aggregate(agg_stats_data *data) {
	uint32_t len = data->agg_attr_len();
	agg_data->agg(*(data->agg), (data->agg)+1+len, len);
  }

  // this flushes agg_data's information into agg
  void flush_agg() {
	agg_data->ret_result(this->agg);
  }

  uint32_t total_agg_len() {
	return agg_data->ret_length();
  }

  // copies 
  void copy_agg(agg_stats_data *data) {
	agg_data->copy_data(data->agg_data);
	cpy(this->sort_attr, data->sort_attr, ROW_UPPER_BOUND);
	cpy(this->agg, data->agg, PARTIAL_AGG_UPPER_BOUND);
	this->flush_all();
  }
  
  int cmp_sort_attr(agg_stats_data *data) {
	if (data->sort_attr_len() != this->sort_attr_len()) {
	  return -1;
	}
	return cmp(data->sort_attr, this->sort_attr, this->sort_attr_len() + 1 + 4);
  }

  int cmp_sort_attr(uint8_t *data) {
	if (this->sort_attr_len() != *( (uint32_t *) (data + 1))) {
	  return -1;
	}
	return cmp(this->sort_attr, data, *( (uint32_t *) (data + 1)) + 1 + 4);
  }

  // flush all paramters into buffer
  void flush_all() {
	this->flush_agg();
  }

  void clear() {
	this->agg_data->reset();
	write_dummy(this->buffer, AGG_UPPER_BOUND);
  }

  void print() {
	if (*sort_attr == 0) {
	  printf("==============\n");
	  printf("Distinct entries: %u\n", *distinct_entries);
	  printf("Offset: %u\n", *offset_ptr);
	  printf("DUMMY\n");
	  printf("==============\n");	  
	} else {
	  printf("==============\n");
	  printf("Distinct entries: %u\n", *distinct_entries);
	  printf("Offset: %u\n", *offset_ptr);
	  print_attribute("sort attr", sort_attr);
	  print_attribute("agg attr", agg);
	  printf("==============\n");
	}
  }

  uint32_t *distinct_entries;
  uint32_t *offset_ptr;
  // the sort attribute could consist of multiple attributes
  uint8_t *sort_attr;
  uint8_t *agg;

  // underlying buffer: [distinct entries][sort attr][agg]
  uint8_t buffer[AGG_UPPER_BOUND];
  
  aggregate_data *agg_data;
};


enum AGGTYPE {
  SUM,
  COUNT,
  AVG
};

uint32_t current_agg_size(uint8_t *current_agg) {
  // return the size of this buffer
  uint8_t *ptr = current_agg;
  uint32_t size = 0;
  uint32_t temp_size = 0;
  temp_size = *( (uint32_t *) ptr);
  size += 4 + temp_size;
  ptr += 4;
  ptr += temp_size;
  size += 4;
  ptr += 4;

  temp_size = *( (uint32_t *) ptr);
  size += 4 + temp_size;
  return size;
}

void set_agg_pointers(uint8_t *current_agg,
					  uint8_t **sort_attribute_type, uint32_t **sort_attribute_len, uint8_t **sort_attribute,
					  uint32_t **distinct_entries,
					  uint8_t **agg_attribute_type, uint32_t **agg_attribute_len, uint8_t **agg_attribute) {
  
  *sort_attribute_type = current_agg;
  *sort_attribute_len = (uint32_t *) (current_agg + 1);
  *sort_attribute = current_agg + 5;
  
  *distinct_entries = (uint32_t *) (*sort_attribute + **sort_attribute_len);
  
  *agg_attribute_type = ((uint8_t *) *distinct_entries) + 4;
  *agg_attribute_len = (uint32_t *) (*agg_attribute_type + 1);
  *agg_attribute = ((uint8_t *) *agg_attribute_len) + 4;
}

void set_agg_attribute_ptr(uint8_t *current_agg,
						   uint8_t **agg_attr_ptr) {
  *agg_attr_ptr = current_agg;
  *agg_attr_ptr += *((uint32_t *) *agg_attr_ptr);
  *agg_attr_ptr += 4 + 4 + 4;
}

// Scan a sequence of rows
// op_code: decides the aggregation function
// flag: is this the first scan? or the second scan? would need to use different algorithms
//
// First scan: the goal is to
//   1. get the first row of this partition
//   2. get the partial sum for the last sort attribute
//   3. count the number of distinct items in this partition
//
// Second scan:
//   1. compute final output
void scan_aggregation_count_distinct(int op_code,
									 uint8_t *input_rows, uint32_t input_rows_length,
									 uint32_t num_rows,
									 uint8_t *agg_row, uint32_t agg_row_buffer_length,
									 uint8_t *output_rows, uint32_t output_rows_length,
									 uint32_t *actual_output_rows_length,
									 int flag) {

  // agg_row is initially encrypted
  // [agg_row length]enc{agg_row}
  // after decryption, agg_row's should only contain
  // 1. 4 bytes for # of distinct entries so far
  // 2. current partial aggregate result ([type][len][attribute]); size should be fixed to PARTIAL_AGG_UPPER_BOUND
  // 3. 1. the sort attribute for the previous variable ([type][len][attribute])
  agg_stats_data current_agg(op_code);
  agg_stats_data prev_agg(op_code);
  
  uint8_t *prev_row = NULL;
  uint8_t *current_row = NULL;
  
  uint32_t agg_attribute_num = 1;
  uint32_t sort_attribute_num = 1;

  // this op_code decides the aggregation function
  // as well as the aggregation column
  switch(op_code) {
  case 1:
	sort_attribute_num = 2;
	agg_attribute_num = 3;
	break;

  default:
	break;
  }
  

  prev_agg.agg_data->reset();
  current_agg.agg_data->reset();

  uint32_t offset = 0;
  uint32_t distinct_items = 0;

  // aggregate attributes point to the columns being aggregated
  // sort attributes point to the GROUP BY attributes

  int dummy = -1;  

  uint32_t agg_row_length = *( (uint32_t *) agg_row);
  uint8_t *agg_row_ptr = agg_row + 4;

  printf("agg_row_length is %u, agg_row_buffer_length is %u\n", agg_row_length, agg_row_buffer_length);

  if (test_dummy(agg_row_ptr, agg_row_length) == 0) {
	// copy the entire agg_row_ptr to curreng_agg
	cpy(current_agg.agg, agg_row_ptr, agg_row_length);
	dummy = 0;
  } else {
	decrypt(agg_row_ptr, agg_row_length, current_agg.buffer);
	current_agg.aggregate();
	offset = current_agg.offset();
	distinct_items = current_agg.distinct();

	printf("offset is %u, distinct_items is %u\n", offset, distinct_items);

	if (mode == 1) {
	  dummy = 0;
	  current_agg.clear();
	}
  }

  // returned row's structure should be appended with
  // [enc agg len]enc{[agg type][agg len][aggregation]}
  uint8_t *input_ptr = input_rows;
  uint8_t *output_rows_ptr = output_rows;
  uint8_t *enc_row_ptr = NULL;
  uint32_t enc_row_len = 0;

  uint8_t *prev_row_ptr = NULL;
  uint8_t prev_row_len = 0;
  
  uint8_t *enc_value_ptr = NULL;
  uint32_t enc_value_len = 0;

  uint8_t value_type = 0;
  uint32_t value_len = 0;
  uint8_t *value_ptr = NULL;
  
  *actual_output_rows_length = 0;

  for (uint32_t r = 0; r < num_rows; r++) {
	get_next_row(&input_ptr, &enc_row_ptr, &enc_row_len);
	uint32_t num_cols = *( (uint32_t *) enc_row_ptr);

	printf("Record %u, num cols is %u\n", r, num_cols);
	enc_row_ptr += 4;

	if (r == 0 && dummy == 0) {
	  // if the dummy is empty, it is the very first row of the current partition
	  // put down marker for the previous row
	  prev_row_ptr = enc_row_ptr - 4; 
	  prev_row_len = enc_row_len;

	  current_agg.inc_distinct();
	  
	  // also copy the attribute information into current_agg
 	  find_attribute(enc_row_ptr, enc_row_len, num_cols,
					 sort_attribute_num,
					 &enc_value_ptr, &enc_value_len);
	  decrypt(enc_value_ptr, enc_value_len, current_agg.sort_attr);

	  find_attribute(enc_row_ptr, enc_row_len, num_cols,
					 agg_attribute_num,
					 &enc_value_ptr, &enc_value_len);
	  decrypt(enc_value_ptr, enc_value_len, current_agg.agg);

	  current_agg.aggregate();
	  
	  // cleanup
	  enc_row_ptr += enc_row_len;

	  continue;
	}

	// copy current_agg to prev_agg
	prev_agg.copy_agg(&current_agg);

	// should output the previous row with the partial aggregate information
	if ((flag == 1 && r == 1)) {
	  //*( (uint32_t *) output_rows_ptr) = prev_row_len;
	  //output_rows_ptr += 4;
	  cpy(output_rows_ptr, prev_row_ptr, prev_row_len);
	  output_rows_ptr += prev_row_len;
	  *actual_output_rows_length += prev_row_len;

	  printf("Write out to output: total bytes is %u\n", prev_row_len);
	}


	// now encrypt the partial aggregate and write back
	uint32_t partial_agg_len = prev_agg.agg_data->ret_length();
	prev_agg.flush_agg();
	prev_agg.agg_data->ret_result_print();

	if (flag == 2) {
	  // *( (uint32_t *) output_rows_ptr) = prev_agg.agg_data->ret_length();
	  // encrypt(prev_agg.agg, PARTIAL_AGG_UPPER_BOUND, output_rows_ptr);
	  // output_rows_ptr += enc_size(PARTIAL_AGG_UPPER_BOUND);

	  // update the output buffer with the partial sum
	  agg_final_result(&current_agg, offset, output_rows_ptr, distinct_items);
	}

	// integrate with the current row
 	find_attribute(enc_row_ptr, enc_row_len, num_cols,
				   sort_attribute_num,
				   &enc_value_ptr, &enc_value_len);
	decrypt(enc_value_ptr, enc_value_len, current_agg.sort_attr);

	find_attribute(enc_row_ptr, enc_row_len, num_cols,
				   agg_attribute_num,
				   &enc_value_ptr, &enc_value_len);
	decrypt(enc_value_ptr, enc_value_len, current_agg.agg);


	// update the partial aggregate
	if (current_agg.cmp_sort_attr(&prev_agg) == 0) {
	  current_agg.aggregate();
	} else {
	  current_agg.inc_distinct();
	  current_agg.agg_data->reset();
	  current_agg.aggregate();
	  ++offset;
	}

	// cleanup
	prev_row_ptr = enc_row_ptr - 4;
	prev_row_len = enc_row_len;
	
	enc_row_ptr += enc_row_len;
  }

  // final output: need to somehow zip together the output
  // for stage 1: 1 output row, and 1 agg row
  // for stage 2: output should be either
  //    a. D rows, where D = # of distinct records in the result set
  //    b. N rows (with extra agg data), where N is the number of input rows
  if (flag == 1) {
	current_agg.flush_all();
	uint32_t ca_size = enc_size(AGG_UPPER_BOUND);
	printf("ca_size is %u\n", ca_size);
	current_agg.print();
	*( (uint32_t *) output_rows_ptr) = ca_size;
	encrypt(current_agg.buffer, AGG_UPPER_BOUND, output_rows_ptr + 4);
	*actual_output_rows_length += 4 + ca_size;
  }
}


// given a list of boundary records from each partition, process these records
// rows: each machine sends [first row][agg_row]
void process_boundary_records(int op_code,
							  uint8_t *rows, uint32_t rows_size,
							  uint32_t num_rows,
							  uint8_t *out_agg_rows, uint32_t out_agg_row_size) {
  
  // rows should be structured as
  // [regular row info][enc agg len]enc{agg}[regular row]...

  uint32_t sort_attribute_num = 1;

  switch(op_code) {
  case 1:
	sort_attribute_num = 2;
	break;
  default:
	break;
  }

  uint8_t *input_ptr = rows;
  uint8_t *enc_row_ptr = NULL;
  uint32_t enc_row_len = 0;
  uint8_t *enc_value_ptr = NULL;
  uint32_t enc_value_len = 0;
  uint32_t num_cols = 0;
  
  uint8_t decrypted_row[ROW_UPPER_BOUND];

  uint8_t *enc_agg_ptr = NULL;
  uint32_t enc_agg_len = 0;
  
  // agg_row attribute
  agg_stats_data prev_agg(op_code);
  agg_stats_data current_agg(op_code);

  printf("Current agg's pointer: %p\n", current_agg.buffer);
  printf("Prev agg's pointer: %p\n", prev_agg.buffer);
  
  // in this scan, we must count the number of distinct items
  // a.k.a. the size of the aggregation result
  uint32_t distinct_items = 0;
  uint32_t offset = 0;
  uint32_t single_distinct_items = 0;

  // write back to out_agg_rows
  uint8_t *out_agg_rows_ptr = out_agg_rows;

  uint32_t agg_enc_size = AGG_UPPER_BOUND;
  int ret = 0;

  printf("Process called; buffer size is %u, %p\n", rows_size, out_agg_rows);

  for (int round = 0; round < 2; round++) {
	// round 1: collect information about num distinct items
	input_ptr = rows;
	current_agg.clear();
	prev_agg.clear();
	offset = 0;
	single_distinct_items = 0;
	
	for (uint32_t i = 0; i < num_rows; i++) {

	  get_next_row(&input_ptr, &enc_row_ptr, &enc_row_len);
	  uint32_t num_cols = *( (uint32_t *) enc_row_ptr);

	  //printf("Record %u, num cols is %u, enc_row_len is %u\n", i, num_cols, enc_row_len);
	  enc_row_ptr += 4;

	  enc_agg_ptr = input_ptr;
	  enc_agg_len = *( (uint32_t *) enc_agg_ptr);
	  enc_agg_ptr += 4;

	  if (i == 0) {
		
		if (round == 1) {
		  // in the second round, we need to also write a dummy agg row for the first machine
		  // so that it knows the number of distinct items
		  
		  *((uint32_t *) out_agg_rows_ptr) = enc_size(agg_enc_size);
		  out_agg_rows_ptr += 4;
		  prev_agg.clear();
		  prev_agg.flush_all();
		  prev_agg.set_distinct(distinct_items);
		  prev_agg.set_offset(0);
		  printf("Writing out %u, distinct items is %u, agg_enc_size: %u\n", i, distinct_items, agg_enc_size);
		  prev_agg.print();
		  encrypt(prev_agg.buffer, agg_enc_size, out_agg_rows_ptr);
		  out_agg_rows_ptr += enc_size(agg_enc_size);
		}


		find_attribute(enc_row_ptr, enc_row_len, num_cols,
					   sort_attribute_num,
					   &enc_value_ptr, &enc_value_len);
		decrypt(enc_value_ptr, enc_value_len, decrypted_row);

		decrypt(enc_agg_ptr, enc_agg_len, prev_agg.buffer);

		if (round == 0) {
		  distinct_items = prev_agg.distinct();
		}
		single_distinct_items = prev_agg.distinct();

		
		input_ptr = input_ptr + 4 + enc_agg_len;
		continue;
	  }

	  find_attribute(enc_row_ptr, enc_row_len, num_cols,
					 sort_attribute_num,
					 &enc_value_ptr, &enc_value_len);
	  decrypt(enc_value_ptr, enc_value_len, decrypted_row);


	  // decrypt into current_agg
	  decrypt(enc_agg_ptr, enc_agg_len, current_agg.buffer);
	  if (round == 0) {
		distinct_items += current_agg.distinct();
	  }
	  offset += single_distinct_items;

	  // To find the number of distinct records:
	  // compare the first row with prev_agg
	  ret = prev_agg.cmp_sort_attr(decrypted_row);

	  if (ret == 0) {
		if (round == 0) {
		  --distinct_items;
		}
		--offset;
	  } else {
		if (round == 0) {
		  distinct_items += 0;
		}
	  }

	  single_distinct_items = current_agg.distinct();

	  // compare sort attributes current and previous agg
	  ret = prev_agg.cmp_sort_attr(&current_agg);

	  if (ret == 0) {
		// these two attributes are the same -- we have something that spans multiple machines
		// must aggregate the partial aggregations
		current_agg.aggregate();
		prev_agg.aggregate();
		prev_agg.aggregate(&current_agg);
		prev_agg.flush_all();
		current_agg.copy_agg(&prev_agg);
	  }
	
	  if (ret == 0 || i == 1) {
		// clear prev_agg
		prev_agg.clear();
	  }
	  
	  if (round == 1) {
		// only write out for the second round
		// write prev_agg into out_agg_rows
		*((uint32_t *) out_agg_rows_ptr) = enc_size(agg_enc_size);
		out_agg_rows_ptr += 4;
		prev_agg.flush_all();
		prev_agg.set_distinct(distinct_items);
		prev_agg.set_offset(offset);
		printf("Writing out %u, distinct items is %u, size is %u\n", i, distinct_items, agg_enc_size);
		prev_agg.print();
		
		encrypt(prev_agg.buffer, agg_enc_size, out_agg_rows_ptr);
		out_agg_rows_ptr += enc_size(agg_enc_size);
	  }

	  // copy current_agg into prev_agg
	  prev_agg.copy_agg(&current_agg);

	  input_ptr = enc_agg_ptr + enc_agg_len;
	}
	if (round == 0) {
	  printf("Distinct items is %u\n", distinct_items);
	}
  }

  printf("Is in enclave? %u\n", sgx_is_within_enclave(out_agg_rows, (size_t ) (out_agg_rows_ptr - out_agg_rows)));
  
  printf("Return process_boundary, %p\n", out_agg_rows_ptr);
}


// produces the final result
// this assumes that the EPC won't be oblivious: the malicious OS could page out
void allocate_agg_final_result(uint8_t *enc_result_size,
							   uint32_t enc_result_size_length,
							   uint32_t *result_size,
							   uint8_t *result_set) {
  decrypt(enc_result_size, enc_result_size_length, (uint8_t *) result_size);
  // allocate result set
  uint32_t size = *result_size;
  result_set = (uint8_t *) malloc(size * AGG_UPPER_BOUND);
}


// This does not assume an oblivious EPC
void agg_final_result(agg_stats_data *data, uint32_t offset,
					  uint8_t *result_set, uint32_t result_size) {

  uint8_t *result_set_ptr = NULL;
  uint32_t size = (4 + enc_size(AGG_UPPER_BOUND));
  
  // need to do a scan over the entire result_set
  for (uint32_t i = 0; i < result_size; i++) {
	result_set_ptr = result_set + size * i;
	if (offset == i) {
	  // write back the real aggregate data
	  data->flush_all();
	  printf("[WRITE TO FINAL RESULT]\n");
	  data->print();
	  
	  *( (uint32_t *) result_set_ptr) = size;
	  uint32_t distinct = data->distinct();
	  data->set_distinct(distinct);
	  encrypt(data->buffer, AGG_UPPER_BOUND, result_set_ptr + 4);
	  data->set_distinct(distinct);
	} else {
	  *( (uint32_t *) result_set_ptr) = size;
	  uint32_t distinct = data->distinct();
	  __builtin_prefetch(result_set_ptr + 4, 1, 1);
	  
	  // data->set_distinct(0);
	  // encrypt(data->buffer, AGG_UPPER_BOUND, result_set_ptr + 4);
	  // data->set_distinct(distinct);
	}
  }
}

// This assumes an oblivious EPC
void agg_final_result_oblivious_epc(agg_stats_data *data, uint32_t offset,
									uint8_t *result_set, uint32_t result_size) {
  uint8_t *result_set_ptr = result_set + AGG_UPPER_BOUND * offset;
  uint32_t size = (4 + enc_size(AGG_UPPER_BOUND));

  data->flush_all();
  data->print();
  *( (uint32_t *) result_set_ptr) = size;
  uint32_t distinct = data->distinct();
  data->set_distinct(distinct);
  encrypt(data->buffer, AGG_UPPER_BOUND, result_set_ptr + 4);
  data->set_distinct(distinct);
}


// The final aggregation step does not need to be fully oblivious
void final_aggregation(int op_code,
					   uint8_t *agg_rows, uint32_t agg_rows_length,
					   uint32_t num_rows,
					   uint8_t *ret, uint32_t ret_length) {
  // iterate through all rows
  uint8_t *output_rows_ptr = ret;
  uint8_t *enc_row_ptr = agg_rows;
  uint32_t enc_row_len = 0;


  uint8_t value_type = 0;
  uint32_t value_len = 0;
  uint8_t *value_ptr = NULL;

  agg_stats_data current_agg(op_code);

  for (uint32_t i = 0; i < num_rows; i++) {

	enc_row_len = *( (uint32_t *) enc_row_ptr);
	enc_row_ptr += 4;
	decrypt(enc_row_ptr, enc_row_len, current_agg.buffer);

	current_agg.aggregate();
	enc_row_ptr += enc_row_len;
  }

  current_agg.flush_all();
  current_agg.print();
  
  *( (uint32_t *) output_rows_ptr) = enc_size(AGG_UPPER_BOUND);
  encrypt(current_agg.buffer, AGG_UPPER_BOUND, ret);
}

void agg_test() {
  agg_stats_data current_agg(1);
}

//void fake_write(int a) { asm volatile ("mov %0 %0" : =m(a)::); }