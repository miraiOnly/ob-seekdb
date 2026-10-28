/* Copyright (c) 2026 OceanBase. Licensed under the Apache License v2.0. */

#ifndef OCEANBASE_SQL_ENGINE_TABLE_OB_PARQUET_READER_H_
#define OCEANBASE_SQL_ENGINE_TABLE_OB_PARQUET_READER_H_

#include "lib/string/ob_string.h"

namespace oceanbase
{
namespace sql
{

// Minimal Apache Parquet reader for the competition test scope: a single
// column chunk, PLAIN encoding, no compression, no repetition/definition
// levels (all fields REQUIRED). The compact-Thrift footer is parsed with a
// small dedicated decoder; the data page values are read straight from the
// file at the reported offset.

struct ObParquetFileInfo
{
  bool valid_;
  common::ObString col_name_;
  int32_t physical_type_;     // thrift Type enum: 2=INT32, 3=INT64, 5=DOUBLE, 6=BYTE_ARRAY...
  int32_t codec_;             // 0 = UNCOMPRESSED
  int64_t num_values_;
  int64_t data_page_offset_;  // offset of the PageHeader of the data page
  int64_t total_compressed_size_;  // whole column chunk compressed size
  int64_t value_data_offset_; // offset of the first value inside the page
  common::ObString schema_key_value_;  // reserved

  ObParquetFileInfo() { reset(); }
  void reset()
  {
    valid_ = false;
    col_name_.reset();
    physical_type_ = 0;
    codec_ = 0;
    num_values_ = 0;
    data_page_offset_ = 0;
    total_compressed_size_ = 0;
    value_data_offset_ = 0;
    schema_key_value_.reset();
  }
};

// data: whole-file buffer (or at least the footer region); len: total length.
// Returns OB_SUCCESS and fills info on success, or an error code.
int ob_parse_parquet_footer(const char *data, int64_t len, ObParquetFileInfo &info);

} // namespace sql
} // namespace oceanbase

#endif
