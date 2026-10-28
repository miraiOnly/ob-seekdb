/* Copyright (c) 2026 OceanBase. Licensed under the Apache License v2.0. */

#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/basic/ob_file_sql_op.h"

using namespace oceanbase::common;

namespace oceanbase
{
namespace sql
{

OB_DEF_SERIALIZE_SIZE(ObFileSqlSpec)
{
  int64_t len = 0;
  BASE_ADD_LEN((ObFileSqlSpec, ObOpSpec));
  OB_UNIS_ADD_LEN(column_exprs_);
  OB_UNIS_ADD_LEN(file_sql_id_);
  const bool has_def = (NULL != file_sql_def_);
  OB_UNIS_ADD_LEN(has_def);
  if (has_def) {
    const ObFileSqlDef &def = *file_sql_def_;
    OB_UNIS_ADD_LEN(def.path_);
    OB_UNIS_ADD_LEN(def.options_);
    OB_UNIS_ADD_LEN(def.null_str_);
    OB_UNIS_ADD_LEN(def.header_);
    OB_UNIS_ADD_LEN(def.sample_size_);
    OB_UNIS_ADD_LEN(def.delim_);
    OB_UNIS_ADD_LEN(def.quote_);
    OB_UNIS_ADD_LEN(def.escape_);
    OB_UNIS_ADD_LEN(def.file_mtime_);
    OB_UNIS_ADD_LEN(def.file_size_);
    const int8_t format = static_cast<int8_t>(def.format_);
    OB_UNIS_ADD_LEN(format);
    const int64_t col_cnt = def.columns_.count();
    OB_UNIS_ADD_LEN(col_cnt);
    for (int64_t i = 0; i < col_cnt; ++i) {
      const ObFileSqlColumnDef &col = def.columns_.at(i);
      OB_UNIS_ADD_LEN(col.name_);
      OB_UNIS_ADD_LEN(col.type_);
      OB_UNIS_ADD_LEN(col.cs_type_);
      OB_UNIS_ADD_LEN(col.length_);
      OB_UNIS_ADD_LEN(col.precision_);
      OB_UNIS_ADD_LEN(col.scale_);
    }
  }
  return len;
}

OB_DEF_SERIALIZE(ObFileSqlSpec)
{
  int ret = OB_SUCCESS;
  BASE_SER((ObFileSqlSpec, ObOpSpec));
  OB_UNIS_ENCODE(column_exprs_);
  OB_UNIS_ENCODE(file_sql_id_);
  const bool has_def = (NULL != file_sql_def_);
  OB_UNIS_ENCODE(has_def);
  if (OB_SUCC(ret) && has_def) {
    const ObFileSqlDef &def = *file_sql_def_;
    OB_UNIS_ENCODE(def.path_);
    OB_UNIS_ENCODE(def.options_);
    OB_UNIS_ENCODE(def.null_str_);
    OB_UNIS_ENCODE(def.header_);
    OB_UNIS_ENCODE(def.sample_size_);
    OB_UNIS_ENCODE(def.delim_);
    OB_UNIS_ENCODE(def.quote_);
    OB_UNIS_ENCODE(def.escape_);
    OB_UNIS_ENCODE(def.file_mtime_);
    OB_UNIS_ENCODE(def.file_size_);
    const int8_t format = static_cast<int8_t>(def.format_);
    OB_UNIS_ENCODE(format);
    const int64_t col_cnt = def.columns_.count();
    OB_UNIS_ENCODE(col_cnt);
    for (int64_t i = 0; OB_SUCC(ret) && i < col_cnt; ++i) {
      const ObFileSqlColumnDef &col = def.columns_.at(i);
      OB_UNIS_ENCODE(col.name_);
      OB_UNIS_ENCODE(col.type_);
      OB_UNIS_ENCODE(col.cs_type_);
      OB_UNIS_ENCODE(col.length_);
      OB_UNIS_ENCODE(col.precision_);
      OB_UNIS_ENCODE(col.scale_);
    }
  }
  return ret;
}

OB_DEF_DESERIALIZE(ObFileSqlSpec)
{
  int ret = OB_SUCCESS;
  BASE_DESER((ObFileSqlSpec, ObOpSpec));
  OB_UNIS_DECODE(column_exprs_);
  OB_UNIS_DECODE(file_sql_id_);
  bool has_def = false;
  OB_UNIS_DECODE(has_def);
  if (OB_SUCC(ret) && has_def) {
    if (OB_ISNULL(deserialize_alloc_)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("file sql spec deserialize allocator is null", K(ret));
    } else {
      void *buf = deserialize_alloc_->alloc(sizeof(ObFileSqlDef));
      if (OB_ISNULL(buf)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        ObFileSqlDef *def = new (buf) ObFileSqlDef(*deserialize_alloc_);
        file_sql_def_ = def;
        OB_UNIS_DECODE(def->path_);
        OB_UNIS_DECODE(def->options_);
        OB_UNIS_DECODE(def->null_str_);
        OB_UNIS_DECODE(def->header_);
        OB_UNIS_DECODE(def->sample_size_);
        OB_UNIS_DECODE(def->delim_);
        OB_UNIS_DECODE(def->quote_);
        OB_UNIS_DECODE(def->escape_);
        OB_UNIS_DECODE(def->file_mtime_);
        OB_UNIS_DECODE(def->file_size_);
        int8_t format = 0;
        OB_UNIS_DECODE(format);
        def->format_ = static_cast<ObFileSqlFormat>(format);
        int64_t col_cnt = 0;
        OB_UNIS_DECODE(col_cnt);
        if (OB_SUCC(ret) && OB_FAIL(def->columns_.prepare_allocate(col_cnt))) {
        }
        for (int64_t i = 0; OB_SUCC(ret) && i < col_cnt; ++i) {
          ObFileSqlColumnDef &col = def->columns_.at(i);
          OB_UNIS_DECODE(col.name_);
          OB_UNIS_DECODE(col.type_);
          OB_UNIS_DECODE(col.cs_type_);
          OB_UNIS_DECODE(col.length_);
          OB_UNIS_DECODE(col.precision_);
          OB_UNIS_DECODE(col.scale_);
        }
      }
    }
  }
  return ret;
}

// Copies one ObObj cell into a datum. String cells are shallow: the string
// storage lives in the query-lifetime exec ctx allocator (the scanner deep
// copies each field), so the datum stays valid until the query ends.
static int file_sql_obj_to_datum(const ObObj &obj, ObDatum &datum)
{
  int ret = OB_SUCCESS;
  if (obj.is_null()) {
    datum.set_null();
  } else {
    switch (obj.get_type()) {
      case ObIntType: {
        datum.set_int(obj.get_int());
        break;
      }
      case ObDoubleType: {
        datum.set_double(obj.get_double());
        break;
      }
      case ObTinyIntType: {
        datum.set_int(static_cast<int64_t>(obj.get_tinyint()));
        break;
      }
      case ObDateType: {
        datum.set_date(obj.get_date());
        break;
      }
      case ObDateTimeType: {
        datum.set_datetime(obj.get_datetime());
        break;
      }
      case ObVarcharType: {
        datum.set_string(obj.get_string());
        break;
      }
      default: {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unexpected file sql cell type", K(ret), K(obj.get_type()));
        break;
      }
    }
  }
  return ret;
}

ObFileSqlOp::ObFileSqlOp(ObExecContext &exec_ctx, const ObOpSpec &spec, ObOpInput *input)
    : ObOperator(exec_ctx, spec, input),
      allocator_(&exec_ctx.get_allocator()),
      scanner_(NULL),
      cur_rows_(NULL),
      cur_row_count_(0),
      cur_row_idx_(0),
      col_count_(0)
{}

int ObFileSqlOp::inner_open()
{
  int ret = OB_SUCCESS;
  ObFileSqlScanner *scanner = NULL;
  void *buf = NULL;
  if (OB_ISNULL(MY_SPEC.file_sql_def_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("file sql def is null", K(ret));
  } else if (MY_SPEC.column_exprs_.count() != MY_SPEC.file_sql_def_->columns_.count()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("file sql column count mismatch", K(ret),
             K(MY_SPEC.column_exprs_.count()), K(MY_SPEC.file_sql_def_->columns_.count()));
  } else if (OB_ISNULL(buf = allocator_->alloc(sizeof(ObFileSqlScanner)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_ISNULL(scanner = new (buf) ObFileSqlScanner(*MY_SPEC.file_sql_def_))) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(scanner->open())) {
    LOG_WARN("failed to open file sql scanner", K(ret), K(MY_SPEC.file_sql_def_->path_));
  } else {
    scanner_ = scanner;
    col_count_ = MY_SPEC.column_exprs_.count();
    cur_rows_ = NULL;
    cur_row_count_ = 0;
    cur_row_idx_ = 0;
  }
  return ret;
}

int ObFileSqlOp::inner_rescan()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOperator::inner_rescan())) {
  } else if (OB_ISNULL(scanner_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(scanner_->rewind())) {
    LOG_WARN("failed to rewind file sql scanner", K(ret));
  } else {
    cur_rows_ = NULL;
    cur_row_count_ = 0;
    cur_row_idx_ = 0;
  }
  return ret;
}

int ObFileSqlOp::inner_close()
{
  int ret = OB_SUCCESS;
  if (OB_NOT_NULL(scanner_)) {
    scanner_->close();
    allocator_->free(scanner_);
    scanner_ = NULL;
  }
  cur_rows_ = NULL;
  cur_row_count_ = 0;
  cur_row_idx_ = 0;
  return ret;
}

void ObFileSqlOp::destroy()
{
  ObOperator::destroy();
}

int ObFileSqlOp::fetch_next_batch()
{
  int ret = OB_SUCCESS;
  int64_t count = 0;
  ObNewRow *rows = NULL;
  cur_rows_ = NULL;
  cur_row_count_ = 0;
  cur_row_idx_ = 0;
  if (OB_FAIL(scanner_->get_next_batch(*allocator_, FETCH_BATCH_SIZE, count, rows))) {
    LOG_WARN("failed to fetch file sql batch", K(ret));
  } else {
    cur_rows_ = rows;
    cur_row_count_ = count;
  }
  return ret;
}

int ObFileSqlOp::output_row(const ObNewRow &row)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < col_count_; ++i) {
    ObExpr *expr = MY_SPEC.column_exprs_.at(i);
    ObDatum &datum = expr->locate_datum_for_write(eval_ctx_);
    if (OB_FAIL(file_sql_obj_to_datum(row.get_cell(i), datum))) {
      LOG_WARN("failed to fill file sql output datum", K(ret), K(i));
    } else {
      expr->get_eval_info(eval_ctx_).evaluated_ = true;
    }
  }
  return ret;
}

int ObFileSqlOp::inner_get_next_row()
{
  int ret = OB_SUCCESS;
  clear_evaluated_flag();
  if (OB_ISNULL(scanner_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("file sql scanner is null", K(ret));
  } else if (cur_row_idx_ >= cur_row_count_) {
    if (OB_FAIL(fetch_next_batch())) {
    } else if (0 == cur_row_count_) {
      ret = OB_ITER_END;
    }
  }
  if (OB_SUCC(ret)) {
    if (OB_FAIL(output_row(cur_rows_[cur_row_idx_]))) {
    } else {
      ++cur_row_idx_;
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
