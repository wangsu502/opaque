#include "Sort.h"

#include <algorithm>
#include <memory>
#include <queue>

template<typename RecordType>
class MergeItem {
 public:
  SortPointer<RecordType> v;
  uint32_t reader_idx;
};

template<typename RecordType>
void external_merge(int op_code,
                    std::vector<uint8_t *> &runs,
                    uint32_t run_start,
                    uint32_t num_runs,
                    SortPointer<RecordType> *sort_ptrs,
                    uint32_t sort_ptrs_len,
                    uint32_t row_upper_bound,
                    uint8_t *scratch,
                    uint32_t *num_comparisons,
                    uint32_t *num_deep_comparisons) {

  check(sort_ptrs_len >= num_runs,
        "external_merge: sort_ptrs is not large enough (%d vs %d)\n", sort_ptrs_len, num_runs);

  std::vector<StreamRowReader *> readers;
  for (uint32_t i = 0; i < num_runs; i++) {
    readers.push_back(new StreamRowReader(runs[run_start + i], runs[run_start + i + 1]));
  }

  auto compare = [op_code, num_comparisons, num_deep_comparisons](const MergeItem<RecordType> &a,
                                                                  const MergeItem<RecordType> &b) {
    (*num_comparisons)++;
    return b.v.less_than(&a.v, op_code, num_deep_comparisons);
  };
  std::priority_queue<MergeItem<RecordType>, std::vector<MergeItem<RecordType>>, decltype(compare)>
    queue(compare);
  for (uint32_t i = 0; i < num_runs; i++) {
    MergeItem<RecordType> item;
    item.v = sort_ptrs[i];
    readers[i]->read(&item.v, op_code);
    item.reader_idx = i;
    queue.push(item);
  }

  // Sort the runs into scratch
  RowWriter w(scratch, row_upper_bound);
  w.set_part_index(0);
  w.set_opcode(op_code);
  while (!queue.empty()) {
    MergeItem<RecordType> item = queue.top();
    queue.pop();
    w.write(&item.v);

    // Read another row from the same run that this one came from
    if (readers[item.reader_idx]->has_next()) {
      readers[item.reader_idx]->read(&item.v, op_code);
      queue.push(item);
    }
  }
  w.close();

  // Overwrite the runs with scratch, merging them into one big run
  memcpy(runs[run_start], scratch, w.bytes_written());

  for (uint32_t i = 0; i < num_runs; i++) {
    delete readers[i];
  }
}

template<typename RecordType>
void external_sort(int op_code,
		   uint32_t num_buffers,
		   uint8_t **buffer_list,
                   uint32_t *num_rows,
		   uint32_t row_upper_bound,
		   uint8_t *scratch) {

  // Maximum number of rows we will need to store in memory at a time: the contents of the largest
  // buffer
  
  uint32_t max_num_rows = 0;
  for (uint32_t i = 0; i < num_buffers; i++) {
    if (max_num_rows < num_rows[i]) {
      max_num_rows = num_rows[i];
    }
  }
  uint32_t max_list_length = std::max(max_num_rows, MAX_NUM_STREAMS);

  // Actual record data, in arbitrary and unchanging order
  RecordType *data = (RecordType *) malloc(max_list_length * sizeof(RecordType));
  for (uint32_t i = 0; i < max_list_length; i++) {
    new(&data[i]) RecordType(row_upper_bound);
  }

  // Pointers to the record data. Only the pointers will be sorted, not the records themselves
  SortPointer<RecordType> *sort_ptrs = new SortPointer<RecordType>[max_list_length];
  for (uint32_t i = 0; i < max_list_length; i++) {
    sort_ptrs[i].init(&data[i]);
  }

  uint32_t num_comparisons = 0, num_deep_comparisons = 0;

  printf("Sort single buffer starting\n");

  // Sort each buffer individually
  for (uint32_t i = 0; i < num_buffers; i++) {
    debug("Sorting buffer %d with %d rows, opcode %d\n", i, num_rows[i], op_code);
    sort_single_buffer(op_code, buffer_list[i], num_rows[i], sort_ptrs, max_list_length,
                       row_upper_bound, &num_comparisons, &num_deep_comparisons);
  }

  printf("Sort single buffer is done\n");

  // Each buffer now forms a sorted run. Keep a pointer to the beginning of each run, plus a
  // sentinel pointer to the end of the last run
  std::vector<uint8_t *> runs(buffer_list, buffer_list + num_buffers + 1);

  // Merge sorted runs, merging up to MAX_NUM_STREAMS runs at a time
  while (runs.size() - 1 > 1) {
    printf("run's size is %u\n", runs.size());
    perf("external_sort: Merging %d runs, up to %d at a time\n",
         runs.size() - 1, MAX_NUM_STREAMS);

    std::vector<uint8_t *> new_runs;
    for (uint32_t run_start = 0; run_start < runs.size() - 1; run_start += MAX_NUM_STREAMS) {
      uint32_t num_runs =
        std::min(MAX_NUM_STREAMS, static_cast<uint32_t>(runs.size() - 1) - run_start);
      debug("external_sort: Merging buffers %d-%d\n", run_start, run_start + num_runs - 1);

      external_merge<RecordType>(
        op_code, runs, run_start, num_runs, sort_ptrs, max_list_length, row_upper_bound, scratch,
        &num_comparisons, &num_deep_comparisons);

      new_runs.push_back(runs[run_start]);
    }
    new_runs.push_back(runs[runs.size() - 1]); // copy over the sentinel pointer

    runs = new_runs;
  }

  perf("external_sort: %d comparisons, %d deep comparisons\n",
       num_comparisons, num_deep_comparisons);

  delete[] sort_ptrs;
  for (uint32_t i = 0; i < max_list_length; i++) {
    data[i].~RecordType();
  }
  free(data);
}

template<typename RecordType>
void sample(uint8_t *input_rows,
            uint32_t input_rows_len,
            uint32_t num_rows,
			uint8_t *output_rows,
            uint32_t *output_rows_size,
            uint32_t *num_output_rows) {

  uint32_t row_upper_bound = 0;
  {
    BlockReader b(input_rows, input_rows_len);
    uint8_t *block;
    uint32_t len, num_rows, result;
    b.read(&block, &len, &num_rows, &result);
    if (block == NULL) {
      *output_rows_size = 0;
      return;
    } else {
      row_upper_bound = result;
    }
  }
  
  // sample ~5% of the rows
  unsigned char buf[2];
  uint16_t *buf_ptr = (uint16_t *) buf;

  RowReader r(input_rows);
  RowWriter w(output_rows, row_upper_bound);
  RecordType row;
  uint32_t num_output_rows_result = 0;
  for (uint32_t i = 0; i < num_rows; i++) {
    r.read(&row);
    sgx_read_rand(buf, 2);
    if (*buf_ptr <= 3276) {
      w.write(&row);
      num_output_rows_result++;
    }
  }

  w.close();
  *output_rows_size = w.bytes_written();
  *num_output_rows = num_output_rows_result;
}

template<typename RecordType>
void find_range_bounds(int op_code,
                       uint32_t num_partitions,
					   uint32_t num_buffers,
                       uint8_t **buffer_list,
                       uint32_t *num_rows,
                       uint32_t row_upper_bound,
                       uint8_t *output_rows,
                       uint32_t *output_rows_len,
                       uint8_t *scratch) {

  // Sort the input rows
  external_sort<RecordType>(op_code, num_buffers, buffer_list, num_rows, row_upper_bound, scratch);

  // Split them into one range per partition
  uint32_t total_num_rows = 0;
  for (uint32_t i = 0; i < num_buffers; i++) {
    total_num_rows += num_rows[i];
  }
  uint32_t num_rows_per_part = total_num_rows / num_partitions;

  RowReader r(buffer_list[0]);
  RowWriter w(output_rows, row_upper_bound);
  RecordType row;
  uint32_t current_rows_in_part = 0;
  for (uint32_t i = 0; i < total_num_rows; i++) {
    r.read(&row);
    if (current_rows_in_part == num_rows_per_part) {
      w.write(&row);
      current_rows_in_part = 0;
	} else {
	  ++current_rows_in_part;
	}
  }

  w.close();
  *output_rows_len = w.bytes_written();
}

template<typename RecordType>
void partition_for_sort(int op_code,
                        uint8_t num_partitions,
                        uint32_t num_buffers,
                        uint8_t **buffer_list,
                        uint32_t *num_rows,
                        uint32_t row_upper_bound,
                        uint8_t *boundary_rows,
                        uint8_t *output,
                        uint8_t **output_partition_ptrs,
                        uint32_t *output_partition_num_rows,
                        uint8_t *scratch) {

  // Sort the input rows
  external_sort<RecordType>(op_code, num_buffers, buffer_list, num_rows, row_upper_bound, scratch);

  uint32_t total_num_rows = 0;
  for (uint32_t i = 0; i < num_buffers; i++) {
    total_num_rows += num_rows[i];
  }

  // Scan through the sorted input rows and copy them to the output, marking the beginning of each
  // range with a pointer. A range contains all rows greater than or equal to one boundary row and
  // less than the next boundary row. The first range contains all rows less than the first boundary
  // row, and the last range contains all rows greater than or equal to the last boundary row.
  RowReader r(buffer_list[0]);
  RowReader b(boundary_rows);
  RowWriter w(output, row_upper_bound);
  RecordType row;
  RecordType boundary_row;
  uint32_t out_idx = 0;
  uint32_t cur_num_rows = 0;

  output_partition_ptrs[out_idx] = output;
  b.read(&boundary_row);

  for (uint32_t i = 0; i < total_num_rows; i++) {
    r.read(&row);

    // The new row falls outside the current range, so we start a new range
    if (!row.less_than(&boundary_row, op_code) && out_idx < num_partitions - 1) {
      // Record num_rows for the old range
      output_partition_num_rows[out_idx] = cur_num_rows; cur_num_rows = 0;

      out_idx++;

      // Record the beginning of the new range
      w.finish_block();
      output_partition_ptrs[out_idx] = output + w.bytes_written();

      if (out_idx < num_partitions - 1) {
        b.read(&boundary_row);
      }
	}
	
    w.write(&row);
    ++cur_num_rows;
  }

  // Record num_rows for the last range
  output_partition_num_rows[num_partitions - 1] = cur_num_rows;

  w.close();
  // Write the sentinel pointer to the end of the last range
  output_partition_ptrs[num_partitions] = output + w.bytes_written();
}

template void external_sort<NewRecord>(
  int op_code,
  uint32_t num_buffers,
  uint8_t **buffer_list,
  uint32_t *num_rows,
  uint32_t row_upper_bound,
  uint8_t *scratch);

template void external_sort<NewJoinRecord>(
  int op_code,
  uint32_t num_buffers,
  uint8_t **buffer_list,
  uint32_t *num_rows,
  uint32_t row_upper_bound,
  uint8_t *scratch);

template void sample<NewRecord>(
  uint8_t *input_rows,
  uint32_t input_rows_len,
  uint32_t num_rows,
  uint8_t *output_rows,
  uint32_t *output_rows_size,
  uint32_t *num_output_rows);

template void sample<NewJoinRecord>(
  uint8_t *input_rows,
  uint32_t input_rows_len,
  uint32_t num_rows,
  uint8_t *output_rows,
  uint32_t *output_rows_size,
  uint32_t *num_output_rows);

template void find_range_bounds<NewRecord>(
  int op_code,
  uint32_t num_partitions,
  uint32_t num_buffers,
  uint8_t **buffer_list,
  uint32_t *num_rows,
  uint32_t row_upper_bound,
  uint8_t *output_rows,
  uint32_t *output_rows_len,
  uint8_t *scratch);

template void find_range_bounds<NewJoinRecord>(
  int op_code,
  uint32_t num_partitions,
  uint32_t num_buffers,
  uint8_t **buffer_list,
  uint32_t *num_rows,
  uint32_t row_upper_bound,
  uint8_t *output_rows,
  uint32_t *output_rows_len,
  uint8_t *scratch);

template void partition_for_sort<NewRecord>(
  int op_code,
  uint8_t num_partitions,
  uint32_t num_buffers,
  uint8_t **buffer_list,
  uint32_t *num_rows,
  uint32_t row_upper_bound,
  uint8_t *boundary_rows,
  uint8_t *output,
  uint8_t **output_partition_ptrs,
  uint32_t *output_partition_num_rows,
  uint8_t *scratch);

template void partition_for_sort<NewJoinRecord>(
  int op_code,
  uint8_t num_partitions,
  uint32_t num_buffers,
  uint8_t **buffer_list,
  uint32_t *num_rows,
  uint32_t row_upper_bound,
  uint8_t *boundary_rows,
  uint8_t *output,
  uint8_t **output_partition_ptrs,
  uint32_t *output_partition_num_rows,
  uint8_t *scratch);
