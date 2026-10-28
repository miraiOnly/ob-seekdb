/* Copyright (c) 2026 OceanBase. Licensed under the Apache License v2.0. */

#pragma once

#include <cstdio>
#include "sql/engine/table/ob_file_sql_def.h"
#include <glob.h>
#include "lib/string/ob_string_buffer.h"

namespace oceanbase
{
namespace sql
{

struct ObFileSqlJsonField
{
  ObFileSqlJsonField() : name_(), value_(), is_null_(false), is_string_(false) {}
  ObString name_;
  ObString value_;
  bool is_null_;
  bool is_string_;
};

// Streaming CSV/JSONL scanner implementing ObIFileSqlScanner.
// The file is read once through a fixed-size buffer. Each get_next_batch()
// call parses at most %capacity records and materializes them into
// ObNewRow/ObObj arrays allocated from the caller-provided allocator (deep
// copies, so the batch stays valid until the allocator is reset).
// rewind() restarts the scan from the beginning of the file.
//
// CSV semantics: delimiter/quote/escape taken from ObFileSqlDef, quoted
// fields may span lines, doubled quotes embed a literal quote, CRLF records
// are terminated on \r\n, the first record is skipped when header_ is true.
// JSONL semantics: one flat JSON object per line; values are taken by column
// name (missing -> NULL); nested objects/arrays raise OB_ERR_UNEXPECTED.
// Per-field conversion is best-effort: a value that cannot be converted to
// the declared column type becomes NULL (never an error).
class ObFileSqlScanner : public ObIFileSqlScanner
{
public:
  explicit ObFileSqlScanner(const ObFileSqlDef &def);
  virtual ~ObFileSqlScanner();
  virtual int open() override;
  virtual void close() override;
  virtual int rewind() override;
  virtual int get_next_batch(common::ObIAllocator &allocator, int64_t capacity,
                             int64_t &count, ObNewRow *&rows) override;

private:
  static const int64_t IO_BUF_SIZE = 1 << 16;      // 64KB read buffer
  static const int64_t MAX_FILE_PATH_LEN = 4096;

  int fill_buf();
  int read_char(char &c);
  int push_field(common::ObSEArray<ObString, 32> &fields, const ObStringBuffer &field);
  void skip_csv_header();
  // CSV
  int read_csv_record(common::ObSEArray<ObString, 32> &fields);
  int csv_record_to_row(common::ObIAllocator &allocator,
                        const common::ObSEArray<ObString, 32> &fields,
                        ObObj *cells);
  // JSONL
  int read_json_line(ObString &line);
  int parse_json_object(const ObString &line,
                        common::ObSEArray<ObFileSqlJsonField, 32> &fields);
  int json_record_to_row(common::ObIAllocator &allocator,
                         const common::ObSEArray<ObFileSqlJsonField, 32> &fields,
                         ObObj *cells);
  // shared per-field conversion; conversion failure -> NULL, never an error
  int convert_field(common::ObIAllocator &allocator, const ObString &raw,
                    const ObFileSqlColumnDef &col_def, ObObj &obj);

private:
  const ObFileSqlDef &def_;
  FILE *fp_;
  char file_path_[MAX_FILE_PATH_LEN];
  glob_t glob_buf_;
  int64_t glob_idx_;
  bool glob_inited_;
  char *buf_;
  int64_t buf_len_;
  int64_t buf_pos_;
  bool eof_;
  bool has_push_back_;
  char push_back_;
  bool header_skipped_;
  // per-record scratch, reused across records
  common::ObArenaAllocator scratch_alloc_;
  common::ObSEArray<ObString, 32> scratch_fields_;
  common::ObSEArray<ObFileSqlJsonField, 32> json_fields_;

  DISALLOW_COPY_AND_ASSIGN(ObFileSqlScanner);
};

} // namespace sql
} // namespace oceanbase
