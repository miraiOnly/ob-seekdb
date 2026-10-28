/* Copyright (c) 2026 OceanBase. Licensed under the Apache License v2.0. */

#ifndef OCEANBASE_SQL_ENGINE_TABLE_OB_FILE_SQL_DEF_H_
#define OCEANBASE_SQL_ENGINE_TABLE_OB_FILE_SQL_DEF_H_

#include "lib/allocator/ob_allocator.h"
#include "lib/string/ob_string.h"
#include "lib/container/ob_se_array.h"
#include "common/object/ob_object.h"
#include "common/datum/ob_datum.h"
#include "common/row/ob_row.h"

namespace oceanbase
{
namespace sql
{

enum class ObFileSqlFormat : int8_t
{
  INVALID = 0,
  CSV = 1,
  JSON = 2,
  PARQUET = 3,
  GLOB = 4,
};

struct ObFileSqlColumnDef
{
  ObString name_;
  ObObjType type_;
  ObCollationType cs_type_;
  int32_t length_;
  int16_t precision_;
  int16_t scale_;
  int16_t nullable_; // saw a NULL during inference
  TO_STRING_KV(K_(name), K_(type), K_(length));
};

class ObFileSqlDef final
{
public:
  ObFileSqlDef(common::ObIAllocator &allocator) : allocator_(allocator) { reset(); }
  ~ObFileSqlDef() {}

  void reset()
  {
    format_ = ObFileSqlFormat::INVALID;
    path_.reset();
    options_.reset();
    header_ = true;
    sample_size_ = -1;
    delim_ = ',';
    quote_ = '"';
    escape_ = '\0';
    null_str_.reset();
    columns_.reset();
    file_mtime_ = 0;
    file_size_ = 0;
  }

  int assign(const ObFileSqlDef &other);

  common::ObIAllocator &allocator_;
  ObFileSqlFormat format_;
  ObString path_;
  // Raw option strings kept for explain/identity; parsed options follow.
  ObString options_;
  bool header_;
  int64_t sample_size_;
  char delim_;
  char quote_;
  char escape_;
  ObString null_str_;
  common::ObSEArray<ObFileSqlColumnDef, 8> columns_;
  // File identity for change detection.
  int64_t file_mtime_;
  int64_t file_size_;

  TO_STRING_KV(K_(format), K_(path), K_(header), K_(sample_size));
  DISALLOW_COPY_AND_ASSIGN(ObFileSqlDef);
};

// Streaming file scanners. Each scanner opens the file once, keeps the
// read offset, fills batches of ObNewRow values, and supports rescan by
// rewinding to the start. Data is never fully materialized.
class ObIFileSqlScanner
{
public:
  virtual ~ObIFileSqlScanner() {}
  virtual int open() = 0;
  virtual void close() = 0;
  virtual int rewind() = 0;
  virtual int get_next_batch(common::ObIAllocator &allocator, int64_t capacity, int64_t &count,
                             ObNewRow *&rows) = 0;
};

// Inference helpers exposed for unit-test parity with the platform.
class ObFileSqlInferUtils final
{
public:
  // Deterministic per-column type inference. values_[i] holds the string
  // representation of column i for every scanned row; candidates_[i] is
  // the widest type observed so far. Returns the final types in columns.
  static int infer_column_types(common::ObIAllocator &allocator,
                                const common::ObIArray<ObString> &column_names,
                                common::ObIArray<common::ObSEArray<ObString, 16> > &values,
                                common::ObIArray<ObObjType> &types);
  static int infer_csv_type(common::ObIAllocator &allocator, const ObString &raw, ObObjType &type,
                            int32_t &length);
  // Read the whole file and infer the output schema (column names + types)
  // into def.columns_. Full scan (sample_size == -1). PARQUET is not
  // supported yet and returns OB_NOT_SUPPORTED.
  static int infer_schema(ObFileSqlDef &def);
  static int csv_to_obj(common::ObIAllocator &allocator, const ObString &raw, ObObjType type,
                        ObObj &obj);
  static int csv_to_datums(common::ObIAllocator &allocator, const ObString &raw, ObObjType type,
                           ObDatum &datum, ObObjMeta &meta);
  static bool check_file_changed(int64_t mtime, int64_t size, const ObString &path,
                                 int64_t &new_mtime, int64_t &new_size);
};

} // namespace sql
} // namespace oceanbase

#endif
