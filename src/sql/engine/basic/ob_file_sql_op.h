/* Copyright (c) 2026 OceanBase. Licensed under the Apache License v2.0. */

#pragma once

#include "sql/engine/ob_operator.h"
#include "sql/engine/table/ob_file_sql_def.h"
#include "sql/engine/table/ob_file_sql_scanner.h"
#include "lib/container/ob_fixed_array.h"

namespace oceanbase
{
namespace sql
{

// Physical spec for the FILE_SQL operator. The file sql definition is deep
// copied into the phy plan allocator by ObStaticEngineCG::generate_spec
// (same ownership model as ObJsonTableSpec::cols_def_). column_exprs_[i]
// receives the i-th column of file_sql_def_->columns_ for every scanned row.
class ObFileSqlSpec : public ObOpSpec
{
OB_UNIS_VERSION_V(1);
public:
  ObFileSqlSpec(common::ObIAllocator &alloc, const ObPhyOperatorType type)
      : ObOpSpec(alloc, type),
        column_exprs_(alloc),
        file_sql_def_(nullptr),
        file_sql_id_(OB_INVALID_ID),
        deserialize_alloc_(nullptr)
  {}
  virtual ~ObFileSqlSpec() {}
  virtual void set_deserialize_allocator(common::ObIAllocator *allocator) override
  {
    deserialize_alloc_ = allocator;
  }
  // column outputs, one per def column, in def.columns_ order
  common::ObFixedArray<ObExpr*, common::ObIAllocator> column_exprs_;
  ObFileSqlDef *file_sql_def_;
  int64_t file_sql_id_;  // table item id of the file sql table
private:
  common::ObIAllocator *deserialize_alloc_;
  DISALLOW_COPY_AND_ASSIGN(ObFileSqlSpec);
};

// Row-producing leaf operator: scans the configured file through
// ObFileSqlScanner and fills the output datums of column_exprs_.
class ObFileSqlOp : public ObOperator
{
public:
  ObFileSqlOp(ObExecContext &exec_ctx, const ObOpSpec &spec, ObOpInput *input);
  virtual int inner_open() override;
  virtual int inner_rescan() override;
  virtual int inner_get_next_row() override;
  virtual int inner_close() override;
  virtual void destroy() override;
private:
  static const int64_t FETCH_BATCH_SIZE = 256;
  int fetch_next_batch();
  int output_row(const ObNewRow &row);
private:
  common::ObIAllocator *allocator_;
  ObFileSqlScanner *scanner_;
  ObNewRow *cur_rows_;
  int64_t cur_row_count_;
  int64_t cur_row_idx_;
  int64_t col_count_;

  DISALLOW_COPY_AND_ASSIGN(ObFileSqlOp);
};

} // namespace sql
} // namespace oceanbase
